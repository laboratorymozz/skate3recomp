#!/usr/bin/env bash
# Standalone checks for the iOS port's two riskiest pieces of plumbing.
#
# Neither needs a device, a signing identity, or a configured build tree - both
# compile in one clang invocation and run on the host. They are deliberately
# outside CMake so they can be run before the iOS build works end to end.
#
#   ./tests/ios/run_tests.sh
#
# guest_alias_test  - maps the real nine-view guest layout over both the current
#                     shm_open backing and the container-file backing iOS has to
#                     use, and asserts every aliasing relationship. Runs on any
#                     POSIX host; mmap MAP_SHARED semantics are the same on
#                     Linux and Darwin.
#
# fiber_switch_test - exercises the hand-written arm64 context switch that
#                     replaces ucontext on iOS. Prefers an Apple Silicon host,
#                     which shares the ABI, and forces REX_PLATFORM_IOS on so
#                     the iOS path is what gets built. Off Darwin it falls back
#                     to an aarch64-linux cross build run under qemu - see
#                     run_fiber_qemu below for what that does and does not
#                     prove.
#
# The fiber test needs the SDK patch series from docs/ios/sdk-patches applied to
# third_party/rexglue-sdk; it is skipped with a note if the sources are absent.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
sdk="$repo/third_party/rexglue-sdk"
out="$here/.out"
mkdir -p "$out"

CXX="${CXX:-clang++}"
failures=0

run() {
  local name="$1"
  echo
  echo "=== $name ==="
  shift
  if ! "$@"; then
    echo "*** $name FAILED"
    failures=$((failures + 1))
  fi
}

build_and_run_alias() {
  local rt=()
  # librt is where shm_open lives on glibc; on Darwin it is in libc.
  [[ "$(uname -s)" == "Linux" ]] && rt=(-lrt)
  "$CXX" -std=c++17 -O1 -Wall "$here/guest_alias_test.cpp" -o "$out/guest_alias_test" "${rt[@]}" \
    || return 1
  "$out/guest_alias_test"
}

build_and_run_fiber() {
  if [[ ! -d "$sdk/include" ]]; then
    echo "skipped: rexglue SDK submodule is not checked out at $sdk"
    return 0
  fi
  if [[ ! -f "$sdk/src/core/fiber_ios.cpp" ]]; then
    echo "skipped: the SDK is unpatched - apply docs/ios/sdk-patches first"
    return 0
  fi
  if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
    run_fiber_qemu
    return $?
  fi
  # REX_PLATFORM_IOS is forced on so the iOS fiber backend is what compiles;
  # macOS arm64 runs the same instructions.
  "$CXX" -std=c++23 -O1 -Wall \
    -DREX_PLATFORM_IOS=1 \
    -I"$sdk/include" \
    "$here/fiber_switch_test.cpp" \
    "$sdk/src/core/fiber_ios.cpp" \
    "$sdk/src/core/fiber_arm64_apple.S" \
    -o "$out/fiber_switch_test" || return 1
  "$out/fiber_switch_test"
}

# Fallback for hosts that are not an Apple Silicon Mac: cross-compile for
# aarch64-linux and run under qemu's user-mode emulation.
#
# What this does prove: the frame layout in fiber_arm64_apple.S agrees with the
# frame fabricated by fiber_ios.cpp, the callee-saved integer and floating point
# state survives a switch, and a fiber resumes mid-function on its own stack.
# AAPCS64 and Apple's arm64 ABI agree on all of that, and the assembly is
# assembled from the shipping source with only the symbol prefix renamed.
#
# What it does not prove: anything Darwin-specific. Apple's thread-local model,
# its reservation of x18, its unwind tables and its signal and stack handling
# are all out of scope here, as is arm64e pointer authentication - the routine
# does not sign the return address, so it must not be assembled for arm64e. A
# native run on an Apple Silicon Mac is still the check that counts.
run_fiber_qemu() {
  local cxx="${IOS_TEST_CROSS_CXX:-aarch64-linux-gnu-g++}"
  local qemu="${IOS_TEST_QEMU:-}"
  if [[ -z "$qemu" ]]; then
    for candidate in qemu-aarch64-static qemu-aarch64; do
      if command -v "$candidate" >/dev/null 2>&1; then
        qemu="$candidate"
        break
      fi
    done
  fi
  if ! command -v "$cxx" >/dev/null 2>&1 || [[ -z "$qemu" ]]; then
    echo "skipped: needs an Apple Silicon host, or $cxx plus qemu-aarch64"
    echo "         (found $(uname -s)/$(uname -m); on Debian or Ubuntu:"
    echo "          apt-get install g++-aarch64-linux-gnu qemu-user-static)"
    return 0
  fi

  echo "no Apple Silicon host: cross-building for aarch64-linux and running"
  echo "under $qemu - see the comment in run_tests.sh for what this proves."

  # TARGET_OS_MAC/TARGET_OS_IPHONE drive the real rex/platform.h down its iOS
  # branch. Defining REX_PLATFORM_IOS directly is not enough on Linux: platform.h
  # would still set REX_PLATFORM_LINUX, and fiber.h tests that first, so the
  # ucontext members would be selected and fiber_ios.cpp would not compile.
  # qemu/libkern/ stubs the one Darwin header platform.h pulls in.
  "$cxx" -std=c++23 -O1 -Wall \
    -DTARGET_OS_MAC=1 -DTARGET_OS_IPHONE=1 \
    -I"$here/qemu" \
    -I"$sdk/src/core" \
    -I"$sdk/include" \
    "$here/fiber_switch_test.cpp" \
    "$sdk/src/core/fiber_ios.cpp" \
    "$here/qemu/fiber_elf_shim.S" \
    -o "$out/fiber_switch_test_aarch64" || return 1

  local sysroot="${IOS_TEST_QEMU_SYSROOT:-/usr/aarch64-linux-gnu}"
  local qemu_args=()
  [[ -d "$sysroot" ]] && qemu_args=(-L "$sysroot")
  "$qemu" "${qemu_args[@]}" "$out/fiber_switch_test_aarch64"
}

run "guest address-space aliasing" build_and_run_alias
run "arm64 fiber context switch" build_and_run_fiber

echo
if [[ $failures -eq 0 ]]; then
  echo "all suites passed"
else
  echo "$failures suite(s) failed"
fi
exit $failures
