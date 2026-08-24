# iOS toolchain for the Skate 3 recomp.
#
# Deliberately thin: CMake has had native iOS support since 3.14, and it already
# knows how to find the SDK, pass -arch and -isysroot, and drive the bundle and
# signing machinery. Anything this file adds beyond the four settings below
# tends to fight it.
#
# IMPORTANT - the codegen tool cannot be cross-compiled.
#
# The build runs `rexglue codegen` to translate the guest PowerPC image into
# C++, and that tool is built from the SDK sources in the same tree. Under this
# toolchain it would be built for the device and be unrunnable. Generate on the
# host first, then build for iOS against the result:
#
#   # 1. host tree - produces <source>/generated, which is gitignored
#   cmake --preset macos-release
#   cmake --build --preset macos-release --target generate-all
#
#   # 2. device tree - compiles what step 1 produced
#   cmake --preset ios-release
#   cmake --build --preset ios-release
#
# Signing: the XCODE_ATTRIBUTE_* properties in CMakeLists.txt only apply under
# the Xcode generator. The presets use Ninja, so the bundle comes out unsigned
# and needs an explicit pass before it will install:
#
#   codesign --force --sign <identity> \
#     --entitlements src/skate3_ios.entitlements \
#     out/build/ios-release/skate3recomp.app
#
# Set SKATE3_IOS_DEVELOPMENT_TEAM and SKATE3_IOS_BUNDLE_IDENTIFIER to match the
# provisioning profile.

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "iOS is ARM64-only")

# iphoneos targets a device; pass -DCMAKE_OSX_SYSROOT=iphonesimulator to build
# for the simulator instead. Note the simulator has no MoltenVK-capable GPU
# path worth trusting for this workload - it is only useful for smoke tests.
set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "iOS SDK to build against")

# 16.0 covers every device with the memory headroom this needs (A15 and newer).
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "Minimum supported iOS")

# Look for headers and libraries in the iOS SDK, but run host tools from the
# host. Without this, find_package can pick up Homebrew's macOS libraries.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
