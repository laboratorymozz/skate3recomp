# Brief for a Claude Code session on macOS

You are picking up an in-progress iOS port of this project. Everything so far
was written on a Linux container with no Xcode and no iOS SDK, so **nothing has
been compiled for iOS or run on a device.** Your job is to be the first thing
that actually compiles it.

Read `docs/ios/STATUS.md` for what landed and `docs/ios-port-plan.md` for the
full blocker analysis (referenced below as B1–B15). This file is the running
order.

## Ground rules

- **Pull first.** Another session has been pushing to
  `claude/ios-rebuild-plan-mi0yu0`. Once you start, you own that branch — say so
  before pushing so the other session stops.
- **Every SDK change must be a provable no-op for macOS and Linux.** If the
  macOS build breaks after applying a patch, the patch is wrong. Do not "fix"
  macOS to accommodate it.
- **Report failures verbatim.** Exact target, exact error text. A guessed
  paraphrase is worse than nothing here.

## Prerequisites

```sh
brew install llvm cmake ninja molten-vk
git submodule update --init --recursive
```

The `macos-*` presets expect Homebrew LLVM at `/opt/homebrew/opt/llvm`.
ReXGlue requires Clang 18+; the SDK's compiler check rejects AppleClang except
on iOS, where a patch now lets it through.

Steps 2 onward additionally need **game data you must supply yourself**: an
extracted dump in `game/` containing `default.xex` and
`data/webkit/EAWebkit.xex`, plus the Title Update 3 package
(`TU_12K2276_000000C000000.00000000000O3`) in the repo root. The README is
explicit that building without the title update is no longer regularly tested,
so include it.

## Running order

Ordered so that everything possible *without* game data comes first.

### 1. Run the test harnesses — no game data, no device

```sh
./tests/ios/run_tests.sh
```

Two suites. The aliasing one has been passing on Linux and should pass here
unchanged. **The fiber one has never executed anywhere.**

It exercises the hand-written arm64 context switch that replaces `ucontext` on
iOS (B5), built with `REX_PLATFORM_IOS=1` forced on — macOS on Apple Silicon
shares the AAPCS64 ABI, so it runs natively without a device.

This is the highest-value step in the whole brief. Distinguish carefully
between:

- **passes** — the switch is probably sound;
- **fails an assertion** — the logic is wrong but the machinery works;
- **crashes / hangs** — the assembly itself is wrong, most likely the frame
  layout or the fabricated initial frame in `fiber_ios.cpp`.

The frame offsets in `src/core/fiber_arm64_apple.S` and the `kSlotX29` /
`kSlotX30` constants in `src/core/fiber_ios.cpp` must agree; that pairing is
the first thing to check on a crash.

### 2. Establish the macOS bridgehead — needs game data

```sh
cmake --preset macos-release -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset macos-release --target generate-all --parallel
cmake --preset macos-release -DSKATE3_GAME_DATA_ROOT="$PWD/game"
cmake --build --preset macos-release --parallel
```

Note the deliberate reconfigure between generate and build — codegen writes
`generated/sources.cmake`, which the configure step has to pick up.

This matters more than it looks. The SDK has **no macOS CI runner and no macOS
preset of its own**; the macOS support everything else rests on is
community-maintained and unverified upstream. If this is already broken before
any iOS patch is applied, stop and report that — it changes the plan.

### 3. Apply the SDK patches

See `docs/ios/sdk-patches/README.md`. Six commits off base `7eb0faf`,
covering B1, B2, B4, B5, B9 and B14. Per the plan we fork the SDK rather than
pushing to `mchughalex/rexglue-skate3`:

```sh
cd third_party/rexglue-sdk
git remote add fork git@github.com:<you>/rexglue-skate3.git
git checkout -b ios-port 7eb0faf
git am ../../docs/ios/sdk-patches/*.patch
```

Then **rebuild macOS and confirm it is still green.** Every one of these
patches is meant to be inert off iOS.

### 4. First iOS configure

```sh
cmake --preset ios-release
```

Expect this to fail. The point is *where* it fails. In rough order of
likelihood:

1. **Third-party dependencies.** FFmpeg is the prime suspect — the vendored
   fork needs an iOS cross-compile pass, though its aarch64 assembly is already
   wired up. SDL3, volk, VMA, imgui and the small header-only libraries should
   be fine.
2. **AppleClang and C++23.** The check now admits AppleClang on iOS, but
   whether the SDK's C++23 actually compiles under it is untested. If it does
   not, cross-compile with Homebrew Clang and `-isysroot` instead — see
   `cmake/ios.toolchain.cmake`.
3. **Codegen ordering.** An iOS configure fails early and deliberately if
   `generated/` is absent; codegen only runs host-native. Step 2 produces it.

A Ninja iOS build produces an **unsigned** bundle — the `XCODE_ATTRIBUTE_*`
properties only apply under the Xcode generator. `cmake/ios.toolchain.cmake`
has the `codesign` command.

## What to report back

- Whether the fiber test passed, failed, or crashed — and the output.
- Whether macOS was green before and after the patches.
- For the iOS configure: the failing target and the verbatim error.
- `clang --version` for both `/opt/homebrew/opt/llvm/bin/clang` and the Xcode
  one.

## What is deliberately not built yet

- **B12/B7, entry point and lifecycle.** SDL3's iOS backend is already the
  right shape — the Apple window path is SDL3, the surface abstraction has
  `kTypeIndex_SDLMetalView`, and `VK_EXT_metal_surface` is already requested —
  but the entry point still assumes a `main()` that owns the loop, and there is
  no background/foreground handling anywhere.
- **B13, MoltenVK.** iOS has no loader/ICD arrangement; MoltenVK must be linked
  as an xcframework with volk initialised from its `vkGetInstanceProcAddr`.
  Also note the emulated renderer needs `geometryShader`, which Metal lacks, so
  iOS is native-renderer-only with no fallback.
- **B15, game data import.** Milestone 1 side-loads a pre-extracted `game/`
  folder over file sharing; the document picker flow is milestone 2.
