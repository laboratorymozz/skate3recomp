# rexglue SDK patches for the iOS port

The runtime work for the iOS port lives in the `rexglue-sdk` submodule, not in
this repository. This session had no write access to it, so the commits are
exported here as a patch series instead.

Six commits. Base: `7eb0faf7787f5e01333c228b8e3f03c32f7295ea`
("ui: guarantee progress when shrinking a wizard info row value") on
`skate3-sdk-clean` of `mchughalex/rexglue-skate3`.

## Applying

Per the plan, fork the SDK first, then:

```sh
cd third_party/rexglue-sdk
git remote add fork git@github.com:<you>/rexglue-skate3.git
git checkout -b ios-port 7eb0faf
git am ../../docs/ios/sdk-patches/*.patch
```

If the base has moved on, `git am -3` will three-way merge; the four patches
touch small, well-separated regions and should rebase cleanly.

## What each one does

| Patch | Blocker | Summary |
|---|---|---|
| 0001 | B1 | `REX_PLATFORM_IOS` / `REX_PLATFORM_MACOS` / `REX_PLATFORM_APPLE`. `REX_PLATFORM_MAC` keeps meaning "Darwin" so its 86 existing uses are untouched. |
| 0002 | B2 | Guest space backed by an unlinked container file instead of `shm_open`, which the iOS sandbox does not allow. |
| 0003 | B5 | Hand-written arm64 context switch replacing the `ucontext` fibers absent from the iOS SDK. |
| 0004 | B14 | App and user roots resolved inside the container, since the bundle is read-only. |
| 0005 | B9 | The `rexglue` CLI is a host tool and is no longer built when cross-compiling to iOS. |
| 0006 | B4 | `rexruntime` built as a static archive on iOS, where a loose dylib has nowhere to live. Also drops `rexcodegen` from the device build. |

Patch 0005 pairs with a guard in this repo's `cmake/CodegenTargets.cmake`, which
now fails early with an explanation if an iOS tree is configured before codegen
has been run on the host.

Patch 0001 also adds the `ios-arm64` branch to the SDK's platform detection,
enables the `ASM` language for iOS, and lets AppleClang through the compiler
check on iOS only.

## What was verified, and what was not

Everything here was written and checked on a Linux x86-64 container. No part of
it has been compiled for iOS or run on a device.

Verified:

- The platform macros resolve correctly for macOS, iOS and Linux
  (`static_assert`s against a stubbed `TargetConditionals.h`).
- The container-file backing reproduces every aliasing relationship of the real
  nine-view guest layout, at both 4 KiB and 16 KiB page granularity, and behaves
  identically to `shm_open`. 36 assertions — see `tests/ios/guest_alias_test.cpp`.
- Both branches of the changed mapping functions compile warning-clean and
  round-trip at runtime; the iOS path leaves no file behind.
- The arm64 switch assembles for `arm64-apple-ios` and disassembles to the
  intended frame layout, with the offsets the C++ side assumes.
- `fiber_ios.cpp` compiles clean for iOS and emits no symbols off it;
  `fiber_posix.cpp` still compiles for macOS.
- The iOS path resolution returns, creates and can write the expected
  directories; with iOS off, the macOS `.app` unwrapping and XDG fallback are
  unchanged.
- A static library built from OBJECT libraries linked `PRIVATE` still yields a
  linkable, runnable executable — checked with a minimal CMake reproduction of
  the real target shape built both ways, so patch 0006 needs no link-visibility
  changes.

Verified later, on Linux, with the series applied:

- **The series applies cleanly** to `7eb0faf` with a plain `git am`, all six
  commits, no fuzz.
- **The fiber switch runs.** Cross-built for aarch64-linux and executed under
  qemu — all 10 checks in `tests/ios/fiber_switch_test.cpp` pass, with GCC 13
  and Clang 18, at -O0 through -Os. See the caveats in `run_tests.sh`: this
  covers the ABI-shared part, not anything Darwin-specific, and not arm64e.
- **Patches 0002 and 0004 are inert on Linux**, shown rather than argued:
  `-E` output for `fiber_posix.cpp`, `filesystem_posix.cpp` and
  `memory_posix.cpp` is byte-identical before and after, once the one
  unconditional change — an added `#include <cstdlib>` in `memory_posix.cpp`,
  for `getenv` on the iOS path — is accounted for. The equivalent check for
  macOS still needs a Mac.
- **The CMake changes are `IOS`-gated** except for the compiler check in patch
  0001, which is restructured from two `if` blocks into an `if`/`elseif`. Off
  iOS it is equivalent: non-Clang still fails on the first arm, Clang still
  reaches the 18.0 floor, and AppleClang on macOS is still rejected.

Not verified — assume these are wrong until a Mac says otherwise:

- **Nothing here has been compiled by AppleClang, for Darwin, or for iOS.**
- Whether the SDK's third-party dependencies build for iOS at all. FFmpeg is
  the likely problem, though its aarch64 assembly is already wired up.
- Whether AppleClang can compile the SDK's C++23. If it cannot, cross-compile
  with Homebrew Clang and `-isysroot` instead.
- Whether `HOME` is a dependable way to find the container. The Foundation
  equivalent is `NSSearchPathForDirectoriesInDomains`.

## Not started

The remaining milestone-1 blockers need either a Mac or design decisions that
should not be made blind:

- **B4, static linking.** `rexruntime` and the generated guest modules are
  `SHARED` and `dlopen`'d by path with a hardcoded `.so` extension. Converting
  them to static with a compiled-in module registry is the largest single change
  in the milestone and touches the codegen templates. Doing it unverified would
  produce more review burden than value.
- **B7/B12, entry point and lifecycle.** Needs SDL3's iOS backend in front of
  you.
- **B13, MoltenVK.** Needs the xcframework to link against.
