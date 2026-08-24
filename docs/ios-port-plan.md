# iOS Port Plan — Skate 3 Recomp

## Context

`skate3recomp` is an ahead-of-time static recompilation of the Xbox 360 build of
Skate 3, currently shipping Windows, Linux, and macOS-ARM64 builds. The goal is
to get it running on iOS (arm64, iPhone/iPad).

This is more tractable than it sounds, for two architectural reasons.

**Nothing is JIT-compiled.** ReXGlue translates the guest PowerPC image into C++
source at build time (`cmake/CodegenTargets.cmake` → `generated/`), which is then
compiled into the binary like any other code. `src/core/memory_posix.cpp:118-127`
explicitly serves guest execute requests *without* `PROT_EXEC` and documents the
process as W^X by construction. There is no `MAP_JIT`, no runtime code emission,
no trampoline patching — the single thing that kills most console ports on iOS is
simply not present. Likewise the Vulkan renderer ships **pre-compiled SPIR-V
committed to the repo** (`src/native/shaders/spirv/skate3_native_shaders_spirv.h`,
selected in `MakeShaderDesc()` at `src/skate3_native_scene_gpu.cpp:2694`), and
`d3dcompiler` is linked `if(WIN32)`-only, so a non-Windows binary contains no
shader compiler at all.

**The macOS ARM64 build is a bridgehead.** It already proves out arm64 codegen,
16 KB host pages, Darwin `mmap` view aliasing, SDL3 windowing, Metal surfaces via
`VK_EXT_metal_surface`, and Vulkan through MoltenVK. Most decisively, the arm64
Darwin signal-handling work that usually sinks ports like this is **already
written and in use** — `src/core/exception_handler_posix.cpp:113-127, 342-368`
decodes `mcontext->__ss` / `__ns` and classifies faults from the ESR register,
with an instruction-decode fallback. iOS is therefore a *narrowing* of an
existing Darwin port, not a new platform from scratch.

The decisive constraints are memory, dynamic linking, and app lifecycle.

## Decisions locked

| Decision | Choice |
|---|---|
| SDK access | Fork `mchughalex/rexglue-skate3` under our own account; repoint the submodule; all runtime-level iOS work lands there |
| Distribution | Personal dev cert / sideload (Xcode, AltStore/SideStore). Full entitlements, no App Review |
| Device floor | 6 GB devices — iPhone 13 Pro / A15 and newer |
| Milestone 1 | Boot-to-menu spike: build for `arm64-ios`, map guest memory, reach the main menu with a MoltenVK frame on screen |

## Where the work actually lives

The game repo is a thin front-end. `src/main.cpp` is 49 lines ending in
`REX_DEFINE_APP(skate3, Skate3PureApp::Create)` — `main()`, the window, the main
loop, the RHI, input, and guest memory mapping all live in
`third_party/rexglue-sdk`.

- **~85% of milestone-1 work is in the SDK fork** (platform macros, guest memory
  backing, static linking, fibers, entry point, surface creation, MoltenVK
  linkage, filesystem paths, shutdown).
- **~15% is in this repo** (bundle/CMake plumbing, path policy, fonts, the ISO
  picker, disabling desktop-only affordances).

Of the ~38.7k hand-written lines in `src/`, ~26k are the native renderer, which
is platform-agnostic and needs no changes. The real portability surface on the
game side is about 4,200 lines. Note also that most Windows-only affordances
(screenshot, exe icon, freecam, F8/F9/F10 capture) are already inert `#else`
stubs on non-Windows — no work needed.

## Blockers, and the fix for each

Ordered roughly by when you hit them.

### B1 — `REX_PLATFORM_MAC` is silently true on iOS

`include/rex/platform.h:31` does `#if defined(TARGET_OS_MAC) && TARGET_OS_MAC`.
`TargetConditionals.h` defines `TARGET_OS_MAC = 1` on *every* Apple platform,
iOS included. So an iOS build today compiles the macOS path everywhere — which
is convenient (all the good Darwin paths activate) but fatal in specific places:
`shm_open`, `ucontext` fibers, `_Exit()`, bundle-relative writes.

**Fix (revised during implementation):** add `REX_PLATFORM_IOS` (from
`TARGET_OS_IPHONE`), `REX_PLATFORM_MACOS`, and `REX_PLATFORM_APPLE`, but leave
`REX_PLATFORM_MAC` meaning "Darwin" rather than narrowing it to desktop macOS.
The original plan was to reclassify all 86 occurrences across 33 files; that is
both a large diff and the wrong default, because the large majority of them —
arm64 `mcontext` decoding, Metal surfaces, 16 KiB page handling, Darwin thread
naming — are correct on iOS, and narrowing the macro would switch them all off
at once. Only the handful that genuinely differ get an explicit
`REX_PLATFORM_IOS` guard. Also add an `iOS` branch to the
root `CMakeLists.txt` platform detection (`:127-165`), which today would label an
iOS build `macos-arm64`. There is `REX_PLATFORM_ANDROID` precedent in the same
header, and the vestigial Android scaffolding (a non-desktop surface type, a
per-platform shm backend hook) pre-carves roughly the right shape.

Do this first — it is the seam everything else hangs off, and it should be a
provable no-op for macOS.

### B2 — Guest memory backing uses `shm_open`

`Memory::Initialize()` (`src/system/xmemory.cpp:157`) creates one `0x120001000`
(4 GiB + 512 MiB + 4 KiB) shared-memory object, then `MapViews()` aliases it into
nine overlapping views so the 512 MB physical heap appears simultaneously at
guest `0x7F000000`, `0xA0000000`, `0xC0000000`, `0xE0000000`, and
`physical_membase_` — all mapped onto backing offset `0x100000000`
(`src/system/xmemory.cpp:303-340`). **The aliasing is load-bearing**; it cannot
be replaced with `MAP_ANON`.

On POSIX the backing is `shm_open` + `ftruncate` + `mmap`
(`src/core/memory_posix.cpp:392-420`), with the name
`"xenia_memory_<ticks>"`. POSIX shared memory is not usable in the iOS sandbox
for non-app-group names, and Darwin's `SHM_NAME_MAX` of 31 is already tight.

**Fix — a feature, not a workaround:** back the mapping with a regular file in
the app container, `open` + `ftruncate` + `mmap(MAP_SHARED)`. Aliasing semantics
are preserved exactly, and `MapFileView`
(`src/core/memory_posix.cpp:432-460`) already uses `MAP_FIXED`. The upside is
that dirty guest pages become file-backed and therefore *evictable*, which is
what you want against jetsam — anonymous memory is not reclaimable and counts
against you in full.

Refined during implementation: open the file under `TMPDIR` and `unlink` it
immediately. The descriptor keeps the inode alive, so mapping is unaffected,
while the space is reclaimed automatically on close *or on crash*, and there is
no directory entry — which means the backup exclusion this plan originally
called for is unnecessary. **Implemented and verified** (SDK patch 0002); see
`tests/ios/guest_alias_test.cpp`.

Fallback if file-backing proves too slow: `mach_make_memory_entry_64` +
`mach_vm_map`, which aliases anonymous memory and is sandbox-legal, but gives up
the eviction benefit.

The address-space reservation itself is fine. There is no fixed base — the code
probes `1ull << n` for n in 32..63 (`src/system/xmemory.cpp:176`) and translation
is a plain base+offset add, so only contiguity matters. macOS already has both a
`PROT_NONE` pre-reservation path (`:352-372`, because Darwin lacks
`MAP_FIXED_NOREPLACE`) and a `MapViews(nullptr)` "let the kernel pick" retry
(`:184-188`). iOS inherits both as-is. 4.5 GiB of *reserved* 64-bit VA is not a
problem; only committed pages count.

### B3 — Memory limit / jetsam

Concrete startup cost, before the title allocates anything: the **512 MiB
physical heap is pre-committed RW** (`src/system/xmemory.cpp:245-248`, so the GPU
can touch it without faulting), plus a 16 MiB `memset` at `0x7F000000` on macOS
(`:252-256`), plus the low-page commit — call it **~520 MiB dirty at launch**.
Add the native renderer's targets, streamed textures, and the recompiled binary's
own footprint. On a 6 GB device the default per-app limit is well below this.

**Fix:** `com.apple.developer.kernel.increased-memory-limit` entitlement (free
with a dev cert, no review), combined with B2's evictable backing. Budget tuning
is a milestone-2 concern, but instrument from day one: log
`os_proc_available_memory()` every frame so we know real headroom before touching
render scale, MSAA, or draw distance.

### B4 — The runtime is a shared library (guest modules already static)

`src/system/CMakeLists.txt:48` builds `rexruntime` as `SHARED` — the SDK's only
shared target — which is why `CMakeLists.txt` has a
`skate3_stage_runtime_library` target copying `$<TARGET_FILE:rexruntime>` next
to the executable. On iOS, dynamic libraries must be embedded, signed
`.framework`s, and the directory the loader would search is the read-only app
bundle, so staging a loose dylib is not available.

The SDK *also* has a path that builds recompiled guest modules as `SHARED` and
`dlopen`s them by name (`resources/templates/codegen/dll_targets_cmake.inja`,
`src/system/kernel_state.cpp:684-730`), with `src/system/shared_library.cpp:83`
hardcoding a `.so` extension on all POSIX. That path is **not used by this
project** and has never worked on Apple — see the revised fix below.

**Fix (revised during implementation — this was substantially overstated).**
The guest-module half of this blocker does not exist for this project. Neither
`manifests/skate3.toml.in` nor `manifests/eawebkit.toml.in` declares a
`[[modules]]` array, so codegen never emits `dll_targets.cmake` and never takes
the `SHARED` path; the game compiles both generated images straight into the
executable via `include(generated/sources.cmake)`, and registers EAWebkit's
functions by hand from `func_mappings` in `src/skate3_app_common.cpp:816-828` —
already the compiled-in registry this section asks for. The
`SharedLibrary::Load` branch in `src/system/kernel_state.cpp:684-730` is
therefore dead code here, and was never reachable on Apple anyway given the
hardcoded `.so`.

That leaves `rexruntime` itself, the SDK's only `SHARED` target. **Implemented**
(SDK patch 0006): the library type is gated on iOS, keeping `SHARED` elsewhere
because `librexruntime.dylib`/`.so` is a documented desktop release artifact.
The OBJECT libraries linked in by `src/kernel/CMakeLists.txt:69` land in the
static archive exactly as they land in the shared object, so no source-list or
link-visibility change was needed — verified with a minimal CMake reproduction
of the real target shape built both ways.

The same patch stops building `rexcodegen` on iOS alongside the CLI that patch
0005 already skipped; only the CLI and unit tests link it.

One constraint to preserve if guest modules are ever made shared again: a
static `rexruntime` combined with `SHARED` guest modules is the broken
combination, because each dylib would get its own `active_memory_`,
`shared_kernel_state_`, PPC function registry and cvar storage. That cannot
arise here, but it is the reason not to mix the two.

Also set `REXGLUE_ENABLE_TRACY=OFF` — Tracy is built as a `SHARED` library. The
game's `CMakeLists.txt` already forces this off.

### B5 — `ucontext` fibers do not exist on iOS

`src/core/fiber_posix.cpp` implements `rex::thread::Fiber` on
`makecontext`/`swapcontext` (`:76`, `:89`). Those are deprecated on macOS and
**unavailable in the iOS SDK**. They back the guest's `CreateFiber`/
`SwitchToFiber` (`src/kernel/crt/threading.cpp:66-300`) and are used by
`src/system/xthread.cpp` and `src/system/kernel_state.cpp`, so this is not
optional.

**Fix:** a hand-written arm64 context switch — save/restore x19–x28, d8–d15, fp,
lr, sp (the AAPCS64 callee-saved set), roughly 40 lines of `.s`. This is plain
data movement, not code generation, so it raises no signing concerns.
Boost.Context's `fcontext` arm64 Apple assembly is the reference shape.

### B6 — Two conflicting signal frameworks, one of which throws from a handler

The SDK installs handlers twice:

- `rex::arch::ExceptionHandler` (`src/core/exception_handler_posix.cpp:317-345`)
  — `sigaction` for `SIGILL`/`SIGSEGV`/`SIGBUS`, builds a `HostThreadContext`,
  lets handlers mutate registers, writes them back to the `mcontext`. **The
  arm64-Darwin implementation is complete and correct.**
- `seh_posix.cpp` (`:103-116`) — installs its *own* `sigaction` for the same
  signals with `SA_NODEFER`, and **throws a C++ exception from the signal
  handler** (`:73`). Unwinding out of `_sigtramp` on arm64 Darwin is not
  something to rely on, and whichever framework initialises last wins.

Layered on top, the game's own `src/native/skate3_native_guest_read.cpp` calls
`mprotect` *inside* a SIGSEGV handler (line 177) and `siglongjmp`s out of
faulting `memcpy`s, and `src/system/mmio_handler.cpp` + `PhysicalHeap::
EnableAccessCallbacks` (`src/system/xmemory.cpp:1880-2130`) use write-protect
faults to drive GPU cache invalidation.

**Fix:** resolve the double-install and eliminate the throw-from-handler path on
iOS before debugging anything else — otherwise every memory bug will look like a
different memory bug. The page-fault machinery itself is fine on iOS (`SIGBUS` on
file-backed mappings is already handled), just fault-heavy at 16 KiB granularity.
Note there are **no Mach exception ports anywhere** in the codebase; if
signal-based handling proves unreliable under the debugger, that would be new
work.

### B7 — Shutdown is `std::_Exit()` and deliberate leaks

`src/ui/windowed_app_main_sdl.cpp:52-61` ends with `rex::FlushLogging();
std::_Exit(result);`, and teardown deliberately leaks
(`src/system/runtime.cpp:248-263` releases `function_dispatcher_` and `memory_`;
`src/system/kernel_state.cpp:850-862` similarly). The reason is real:
`pthread_cancel` on Darwin only lands at cancellation points, never inside
CPU-bound recompiled code, so guest threads cannot be stopped cleanly.

On iOS an app must not terminate itself, and backgrounding *requires* a real
quiesce. Related: `RestartGame()` in `src/skate3_app_common.cpp:959-1028`
`posix_spawn`s a fresh process — impossible on iOS — which the settings overlay
wires to "Apply & Restart" for `kRequiresRestart` settings (native renderer
toggle, ultrawide). And `QuitFromUIThread()` on install-cancel
(`src/skate3_app_common.cpp:635, 661`) must not kill the app.

**Fix:** a cooperative quiesce — a check-in point in the guest thread loop, drain
in-flight command buffers, park threads on a condvar. Replace the restart flow
with "apply on next launch". Milestone 1 can get away with a partial version
(quiesce on background, no clean exit), but it cannot be skipped entirely.

### B8 — Toolchain: the SDK hard-rejects Apple Clang

`third_party/rexglue-sdk/CMakeLists.txt:78` is
`if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")` → `FATAL_ERROR`, plus a
`VERSION_LESS "18.0"` gate. Apple Clang reports `AppleClang` with its own version
numbering, so it fails immediately. The game's presets pin Homebrew LLVM at
`/opt/homebrew/opt/llvm`; the SDK has **no macOS preset at all**.

**Fix (recommended):** keep Homebrew LLVM and cross-compile —
`clang++ -target arm64-apple-ios16.0 -isysroot $(xcrun --sdk iphoneos --show-sdk-path)`.
This sidesteps the C++23/libc++ question entirely (Apple's headers come in via
`-isysroot`) and keeps us on the compiler the project is validated against.
Signing is a separate `codesign` step, so no Xcode toolchain dependency.

**Alternative, try second:** relax the check to `MATCHES "Clang"` and see whether
Apple Clang 17 (Xcode 16+) builds it. Nicer Xcode integration if it works, but
it is an unknown and must not gate milestone 1.

Keep `REXGLUE_BUILD_TESTS=OFF`: the PPC assembly test pipeline
(`cmake/ppc_test_pipeline.cmake`) shells out to prebuilt binutils in
`tools/binutils/` that ship only as x86-64 ELF and Windows `.exe` — they cannot
run from an Apple Silicon host at all.

### B9 — Cross-compilation breaks the codegen host tool

`cmake/CodegenTargets.cmake:2` invokes `$<TARGET_FILE:rex::rexglue>`, the
recompiler, which the SDK builds from source in the same tree and which links
the entire runtime stack. Under `CMAKE_SYSTEM_NAME=iOS` it would be built *for
iOS* and be unrunnable.

**Fix:** two build trees. Run `generate-all` in a host-native (macOS arm64)
build; it writes to `<source>/generated/`, a source-tree directory
(`manifests/skate3.toml.in` → `out_directory_path`) consumed via
`include(generated/sources.cmake)`. The iOS tree then compiles what is already
there. No SDK change needed — just a documented two-step build, and excluding
the `rexglue` target from the device configuration.

Be aware `cmake/ApplySkate3CodegenPatches.cmake` does regex surgery on the
generated C++ and `FATAL_ERROR`s if its anchors move. It runs in the host tree so
it is unaffected, but it is a known-fragile step.

### B10 — No bundle, no Info.plist, no signing infrastructure

`add_executable(skate3 ...)` without `MACOSX_BUNDLE`; there is no `Info.plist`,
no entitlements file, and no `codesign` step anywhere in the repo — the macOS
`.app` is assembled by out-of-tree CI packaging. All of it must be written.

**Fix:** `MACOSX_BUNDLE` + a hand-written `Info.plist.in` (`UIDeviceFamily`,
`UIRequiredDeviceCapabilities=arm64/metal`, `UILaunchStoryboardName`,
`UIFileSharingEnabled=YES`, `LSSupportsOpeningDocumentsInPlace=YES`,
`UIApplicationSupportsIndirectInputEvents`), an `.entitlements` carrying
`com.apple.developer.kernel.increased-memory-limit`, and a post-build `codesign`
custom command. `-framework Cocoa` → `-framework UIKit`.

### B11 — Main thread stack is 1 MB and `-Wl,-stack_size` is ignored

`CMakeLists.txt` sets `-Wl,-stack_size,0x4000000` (64 MB) on APPLE because
recompiled PPC code uses deep host stacks. On iOS that linker flag does not apply
to the main thread.

**Fix:** run the guest on a `pthread` created with an explicit
`pthread_attr_setstacksize(64 MB)`. Guest threads already go through
`pthread_attr_setstacksize` (`src/core/threading_posix.cpp:629`), so this is
about the *initial* thread only, and it dovetails with B12.

### B12 — Entry point and lifecycle

iOS is `UIApplicationMain`-driven; `src/ui/windowed_app_main_sdl.cpp` assumes a
`main()` that owns the loop. The good news is that the Apple window backend is
**already SDL3** (`src/ui/CMakeLists.txt` selects `window_sdl.cpp` /
`surface_sdl.cpp` / `windowed_app_context_sdl.cpp` on Apple), the surface
abstraction already has `kTypeIndex_SDLMetalView`
(`include/rex/ui/surface.h:37,43`), `src/ui/window_sdl.cpp:440-452` already calls
`SDL_Metal_CreateView` / `SDL_Metal_GetLayer`, and `vulkan_instance.cpp:149-152`
already requests `VK_EXT_metal_surface`. **That is precisely the iOS shape** —
SDL3 is built static with `SDL_VIDEO`/`SDL_METAL` on for Apple and `SDL_VULKAN`
off, which is exactly right.

**Fix:** an iOS entry path on SDL3's `SDL_APP_*` callback lifecycle, plus
`SDL_EVENT_WILL_ENTER_BACKGROUND` / `DID_ENTER_FOREGROUND` handling that quiesces
the render thread, waits on in-flight command buffers, and parks the guest
threads (see B7). There is currently **no suspend/resume handling anywhere** —
guest threads, the prewarm worker pool (`src/skate3_native_scene_gpu.cpp:4663`),
and the render thread all assume they run forever. Review
`SDL_UNIX_CONSOLE_BUILD=ON` in `thirdparty/CMakeLists.txt` for iOS.

### B13 — MoltenVK on iOS, and no emulated-renderer fallback

macOS ships `libMoltenVK.dylib` next to the binary with a generated
`MoltenVK_icd.json` (`cmake/write_moltenvk_icd.cmake`) and lets the Vulkan loader
find it — `vulkan_instance.cpp:68-72` already tries `libvulkan.1.dylib`, then
`libvulkan.dylib`, then `libMoltenVK.dylib`. **iOS has no loader/ICD
arrangement**; the `VK_ICD_FILENAMES` logic at `vulkan_instance.cpp:36-56` does
not apply.

**Fix:** link `MoltenVK.xcframework` statically and initialise volk from
MoltenVK's `vkGetInstanceProcAddr` rather than from a loader. volk is already
built with `VK_USE_PLATFORM_METAL_EXT` on Apple, and
`VK_KHR_portability_enumeration` / `VK_KHR_portability_subset` are already
handled (`vulkan_instance.cpp:128`, `include/rex/ui/vulkan/device.h:130-137`).

**Feature audit against MoltenVK.** Hard requirements are all satisfied:
`independentBlend`, `fragmentStoresAndAtomics`, `vertexPipelineStoresAndAtomics`.
Unsupported-on-Metal features all have existing fallbacks or are optional:
`geometryShader` (cvar `vulkan_require_geometry_shader`, fallback primitive
expansion at `src/graphics/vulkan/primitive_processor.cpp:54`), sparse residency
(`vulkan_device.cpp:419-471`), `fillModeNonSolid`, `robustness2`/`nullDescriptor`,
`non_seamless_cube_map`. There are already two MoltenVK-aware notes in the code:
a low sampler ceiling (`texture_cache.cpp:4154` — "2048, 1024, or even 96") and
always-on primitive restart (`pipeline_cache.cpp:3218`).

**Important consequence:** the *emulated* GPU path needs `geometryShader` for
primitive emulation (`src/graphics/vulkan/vulkan_device.cpp:135`,
`command_processor.cpp:1252`). On iOS we run the **native renderer only**, with
`vulkan_require_geometry_shader=false` — and we lose the F5 emulated-renderer
fallback that other platforms use as a safety net. Every rendering bug on iOS is
a hard failure rather than a degraded one.

### B14 — Filesystem: the bundle is read-only

`GetAppRootFolder()` (`src/core/filesystem_posix.cpp:96-110`) already unwraps a
macOS `.app` bundle, and `ReXApp::SetupEnvironment()` (`src/ui/rex_app.cpp:314-420`)
writes the config TOML and `logs/` next to it. On iOS that directory is the
read-only signed bundle, so both writes fail. `GetUserFolder()` (`:113-134`) is
XDG-based and would land at `<container>/.local/share/<AppName>` — functional but
wrong.

Game-side, `portable.txt`, `saves/`, `dlc/`, `skate3.toml`, and the default
`game/` root are all resolved "next to the binary"
(`ResolveSkate3UserRoot()`, `src/skate3_app_common.cpp:341`).

**Fix:** iOS implementations of `GetAppRootFolder`/`GetUserFolder` returning
`Documents` / `Library/Application Support` — a small SDK change after which
`ResolveSkate3UserRoot` mostly falls into line. Force-disable `portable.txt`
detection on iOS. Roots are all cvar/TOML-overridable
(`rex_app.cpp:345-390`), so an iOS `OnConfigurePaths` override covers the rest.

Also replace the hard-coded macOS font list at
`src/skate3_app_common.cpp:547-552` (`/System/Library/Fonts/SFNS.ttf` etc.) —
those paths do not exist on iOS and it currently falls back silently to ImGui's
built-in bitmap font.

### B15 — Game data: ~7 GB, and no desktop filesystem

The ISO installer (`src/skate3_iso_installer.cpp`) picks a file with a native
dialog (`NSOpenPanel` on macOS, in the repo's only Objective-C++ file
`src/skate3_iso_installer_macos.mm`), then streams a full XDVDFS extract —
roughly 6.5–7 GB for a DVD9 title — through a 4 MiB buffer into `<exe_dir>/game`.
Worse, it blocks the UI thread in its own pump loop (`:463-497`) for the entire
extract, which on iOS is a guaranteed watchdog kill (0x8BADF00D).

**Milestone 1: sidestep entirely.** Side-load a pre-extracted `game/` folder into
the app container over Finder file sharing and point `game_data_root` at it via
cvar. Do not touch the installer.

**Milestone 2:** `UIDocumentPickerViewController` + security-scoped bookmarks
held for the whole extract (the code currently takes a plain
`std::filesystem::path` and `ifstream`s it), destination in Documents with
`NSURLIsExcludedFromBackupKey`, and the extract moved off the UI thread. The
async pattern **already exists** — the `__APPLE__` branch at
`src/skate3_app_common.cpp:608-630` returns `std::nullopt` plus a continuation
callback; iOS uses that unconditionally. Keep the existing ImGui
`InstallWizardDialog`; only the picker is platform-specific.

## Milestone 1 — boot to menu

Ordered so each step unblocks the next and produces a checkable signal.

1. **Fork the SDK.** Fork `mchughalex/rexglue-skate3`, branch `ios`, repoint
   `.gitmodules`. Record the upstream commit we branched from — we will be
   rebasing.
2. ~~**Platform seam (B1).**~~ **Done** — SDK patch 0001. `REX_PLATFORM_IOS` /
   `REX_PLATFORM_MACOS` / `REX_PLATFORM_APPLE`, iOS branch in the SDK's platform
   detection, no-op for macOS.
3. **Toolchain + CMake (B8, B9, B10).** *Partly done* — toolchain file
   (`cmake/ios.toolchain.cmake`), `ios-*` presets, relaxed compiler-ID check,
   tests off, and the bundle/plist/entitlements are in. Still to do: exclude
   `rexglue` from the device configuration and confirm the two-tree build works.
   Target: SDK core library *compiles* for `arm64-apple-ios` (not linking yet).
4. **Third-party sweep.** SDL3, volk, Vulkan-Headers, VMA, imgui+FreeType,
   fmt/spdlog, tomlplusplus, snappy, xxHash, libmspack, simde, glslang,
   SPIRV-Tools, FFmpeg. FFmpeg already has its aarch64 NEON `.S` sources wired,
   so it should be less painful than feared. Tracy off (B4).
5. ~~**Static linking (B4).**~~ **Done** — SDK patch 0006, and far smaller than
   this step assumed: only `rexruntime` needed converting, gated on iOS. The
   generated guest code was already compiled into the executable.
6. ~~**Guest memory (B2).**~~ **Done** - SDK patch 0002. Unlinked container-file
   backing replacing `shm_open`. All nine views verified to alias correctly at
   both page granularities by `tests/ios/guest_alias_test.cpp`; still to be run
   on a device.
7. **Fibers (B5).** *Written, not executed* - SDK patch 0003. arm64
   context-switch assembly plus `fiber_ios.cpp`. Assembles and disassembles as
   intended. Run `tests/ios/run_tests.sh` on an Apple Silicon Mac to exercise it
   for the first time - highest-risk unverified item.
8. **Signal cleanup (B6).** Resolve the `seh_posix` / `ExceptionHandler` double
   install; no throwing from handlers on iOS.
9. **Entry point + surface (B11, B12).** SDL3 UIKit entry, guest on a 64 MB-stack
   pthread, `SDL_Metal_CreateView` → `VK_EXT_metal_surface`. Target: a cleared
   frame on device.
10. **MoltenVK (B13).** `MoltenVK.xcframework` linked statically, volk from its
    `vkGetInstanceProcAddr`, `vulkan_require_geometry_shader=false`, native
    renderer forced on. Target: the SDK's ImGui overlay drawing on device.
11. ~~**Path policy + fonts (B14).**~~ **Done** - SDK patch 0004 plus the
    game-side commit. Container-relative roots, portable mode force-disabled,
    iOS font candidates.
12. **Boot the game (B15).** Side-loaded pre-extracted `game/` folder, installer
    bypassed. Target: main menu rendered and navigable with a connected
    MFi/Xbox/DualSense controller — SDL3's iOS GameController backend gives us
    this for free, so **no input work is needed for milestone 1**.
13. **Instrument memory (B3).** `os_proc_available_memory()` per frame, so
    milestone 2 starts from real numbers.
14. **Minimum lifecycle (B7, B12).** Quiesce on background, resume on
    foreground. Enough not to crash on a home-button press.

**Definition of done:** on a physical iPhone 13 Pro or newer, the app launches,
reaches the Skate 3 main menu, renders through MoltenVK at a stable frame rate,
responds to a paired controller, survives a background/foreground cycle, and has
its memory headroom measured and logged.

## Beyond milestone 1 (sketch, not committed)

- **M2 — Content pipeline:** `UIDocumentPickerViewController` import with
  security-scoped bookmarks, async off-UI-thread extraction, Documents
  destination, backup exclusion, storage preflight. `NSURLSession` for the
  title-update download (currently WinHTTP on Windows, forked `curl` elsewhere —
  neither works on iOS).
- **M3 — Lifecycle hardening:** full cooperative teardown (B7), thermal
  handling, `AVAudioSession` category and interruption handling (only
  `SDL_HINT_AUDIO_CATEGORY` is set today, at
  `src/audio/sdl/sdl_audio_driver.cpp:76`), QoS classes for the prewarm workers
  (`src/skate3_native_scene_gpu.cpp:4663` uses Windows-only `SetThreadPriority`),
  replacing the `posix_spawn` restart flow.
- **M4 — Touch controls:** the choke point is `rex::input::InputSystem`
  producing `rex::input::X_INPUT_GAMEPAD`; game-side code needs no changes.
  Touch plumbing already exists in the SDK (`include/rex/ui/ui_event.h`
  `TouchEvent`, `src/ui/window.cpp:699`, `src/ui/imgui_drawer.cpp:824-860`) but
  is fed only by Win32. Either a fourth `InputSystem` backend, or drive it via
  the `xam::input_injection` path `src/skate3_demo_path.cpp` already uses. Skate
  3's flick-it controls map badly to touch — this deserves real design work, not
  a virtual stick. The ImGui overlays (install wizard, settings) are also
  keyboard/mouse-shaped and need touch affordances.
- **M5 — Performance:** render scale, MSAA, draw distance, and streaming budgets
  tuned to the measured ceiling. Optionally eliminate MoltenVK's runtime
  SPIR-V→MSL step with a build-time `.metallib` variant alongside `sd.spirv` —
  `MakeShaderDesc`'s `(file, entry, variant)` lookup table at
  `src/skate3_native_scene_gpu.cpp:2694-2714` makes it a mechanical addition.

## Verification

- **Host tree stays green.** `cmake --preset macos-release && cmake --build` must
  keep working after every SDK change; the platform-seam commit especially must
  be a provable no-op for macOS.
- **Memory aliasing test.** A standalone on-device binary that runs
  `Memory::Initialize()` and asserts a write at `physical_membase_ + N` is
  visible at `virtual_membase_ + 0xA0000000 + N`, `+ 0xC0000000`, and
  `+ 0xE0000000`. This is the single most likely thing to fail silently.
- **Fiber test.** A switch-count round-trip against the `Fiber` API, run on
  device.
- **On-device run.** Deploy via Xcode / `ios-deploy`; watch the console for the
  SDK's startup build-title line, then the guest boot trace, then first present.
- **Frame capture.** Xcode's Metal frame debugger against the MoltenVK surface,
  to confirm the native renderer's passes actually execute.
- **Memory ceiling.** `os_proc_available_memory()` per frame; confirm we are not
  within ~200 MB of the limit at the menu, before any gameplay.
- **Lifecycle.** Background/foreground cycle, incoming call, and low-memory
  warning, without a crash.

## Risks and open questions

- **Our bridgehead is untested.** The SDK has **no macOS CI runner and no macOS
  preset** — `.github/workflows/` covers only `win-amd64`, `linux-amd64`,
  `linux-arm64`. The macOS/arm64 support we are building on is
  community-maintained and unverified upstream. Establish a known-good macOS
  build first; do not assume it works.
- **Binary size.** Recompiled game + EAWebKit is on the order of 50–200 MB of
  generated C++ across many translation units (the SDK's own notes mention 35 MB+
  binaries and use `-mcmodel=large` on Linux x86_64 for exactly this reason).
  arm64 gets branch islands from `ld64`, so linking should work, but codesigning
  a binary this large is slow and there are historical per-slice `__TEXT` limits.
  Measure early — it could force `-Os` on the generated sources.
- **Signal-handler debugging.** `mprotect`-from-handler and `siglongjmp` work on
  macOS today, but under a debugger iOS routes `EXC_BAD_ACCESS` through Mach
  exception ports first. Expect it to look broken under Xcode and be fine
  untethered. Budget time for that confusion.
- **16 KB page granularity.** Guest protection granularity is 4 KB; the host page
  is 16 KB. The SDK already compensates (`ShouldSkipHostCommit()` at
  `src/system/xmemory.cpp:95-103`, `PhysicalHeap`'s `host_address_offset` at
  `:1678-1694`), and macOS arm64 exercises it — but the write-watch path will
  fault more often than on a 4 KB host.
- **Thermals.** Sustained load on a phone is a different problem from a desktop
  GPU. Expect throttling within minutes even at good frame rates. A product
  question for M5, not a technical blocker.
- **No emulated-renderer safety net** (B13).
- **Rebase burden.** We are forking an actively developed SDK. Keep every iOS
  change narrow and behind `REX_PLATFORM_IOS` so upstream merges stay cheap.
- **Distribution reality.** Sideloading means a 7-day resign cycle on a free
  account (1 year paid), users must supply their own ISO *and* get ~7 GB of
  extracted data onto the device. This is a hobbyist-audience build, not a
  shippable product — worth being explicit about up front.
