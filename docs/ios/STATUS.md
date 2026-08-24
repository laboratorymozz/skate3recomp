# iOS port — where things stand

Written in one unattended session on a Linux x86-64 container. **No part of this
has been compiled for iOS or run on a device.** Read the "verify first" section
before trusting anything here.

## Start here in the morning

```sh
# 1. Run what can be run without a device. On Apple Silicon this executes the
#    arm64 fiber switch for the first time - the highest-risk unverified item.
./tests/ios/run_tests.sh

# 2. Confirm the macOS build still works. Every SDK change should be a no-op
#    there, and the plan's first risk item is that this bridgehead is itself
#    unverified upstream - the SDK has no macOS CI.
cmake --preset macos-release && cmake --build --preset macos-release

# 3. Fork the SDK and apply the runtime work.
#    See docs/ios/sdk-patches/README.md.
```

## What landed

### In this repo

| Area | What |
|---|---|
| Platform seam | `src/skate3_platform.h` — `SKATE3_PLATFORM_IOS` vs `SKATE3_PLATFORM_MACOS`, since `TARGET_OS_MAC` is 1 on both |
| Installers | `NSOpenPanel` pickers narrowed to macOS; iOS returns an empty path, the existing cancelled-dialog contract |
| Paths | Portable mode disabled on iOS (bundle is read-only); iOS font candidates ahead of the macOS ones |
| Build | `ios-arm64` platform branch, `MACOSX_BUNDLE`, `Info.plist` template, entitlements with `increased-memory-limit`, UIKit instead of Cocoa, runtime-library staging skipped |
| Toolchain | `cmake/ios.toolchain.cmake` and `ios-release` / `ios-relwithdebinfo` presets |
| Tests | `tests/ios/` — two standalone harnesses, no CMake or device needed |

### In the SDK (as patches, not applied)

`docs/ios/sdk-patches/`, five commits off `7eb0faf`. Blockers B1, B2, B5, B9 and
B14 from the plan. See that directory's README for detail.

## Verify first

Ordered by how likely they are to be wrong.

1. **The arm64 fiber switch has never executed.** It assembles and disassembles
   correctly, and the frame offsets match what `fiber_ios.cpp` fabricates, but
   correct-looking context-switch assembly that has never run is exactly the kind
   of thing that is subtly wrong. `tests/ios/run_tests.sh` exercises it natively
   on an Apple Silicon Mac.
2. **Do the SDK's dependencies build for iOS at all?** Unknown. FFmpeg is the
   likely problem, though its aarch64 assembly is already wired up. This gates
   everything and nothing here tests it.
3. **Can AppleClang compile the SDK's C++23?** The compiler check now lets it
   through on iOS, but whether the code actually compiles is untested. Fallback
   is Homebrew Clang with `-isysroot`.
4. **Is `HOME` a dependable way to find the container?** Used by the path
   resolution to stay free of a Foundation dependency. The idiomatic equivalent
   is `NSSearchPathForDirectoriesInDomains`.
5. **iOS system font paths.** Guesses. They fall back to ImGui's built-in face,
   so a wrong guess is cosmetic, not fatal. Embedding a face in the bundle is the
   durable fix — note the SDK already embeds Inter (`src/ui/fonts_inter.cpp`).
6. **`XCODE_ATTRIBUTE_*` properties are inert under Ninja**, which is what the
   presets use. An iOS build comes out unsigned and needs an explicit `codesign`
   pass; the toolchain file has the command.

## Deliberately not started

Not blocked on a Mac so much as on decisions that shouldn't be made blind:

- **B4, static linking.** `rexruntime` and the generated guest modules are
  `SHARED` and `dlopen`'d by path with a hardcoded `.so`. Converting to static
  with a compiled-in module registry touches the codegen templates and is the
  largest single change in milestone 1. Written unverified it would cost more in
  review than it saves.
- **B7/B12, entry point and lifecycle.** Needs SDL3's iOS backend in front of you.
- **B13, MoltenVK.** Needs the xcframework to link against.

## Corrections to the plan

Both folded into `docs/ios-port-plan.md`:

- **B1** originally called for reclassifying all 86 `REX_PLATFORM_MAC` sites.
  That is the wrong default — most are correct on iOS, and narrowing the macro
  would switch them all off at once. `REX_PLATFORM_MAC` now keeps meaning
  "Darwin" and only the few genuine differences get an iOS guard.
- **B2** called for marking the backing file `NSURLIsExcludedFromBackupKey`.
  Unnecessary: the file is unlinked immediately after opening, so it has no
  directory entry to back up, and its space is reclaimed even on a crash.
