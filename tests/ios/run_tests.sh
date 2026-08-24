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
#                     replaces ucontext on iOS. Needs an Apple Silicon host,
#                     which shares the ABI, and forces REX_PLATFORM_IOS on so
#                     the iOS path is what gets built.

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
  if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
    echo "skipped: needs an Apple Silicon host (found $(uname -s)/$(uname -m))"
    return 0
  fi
  if [[ ! -d "$sdk/include" ]]; then
    echo "skipped: rexglue SDK submodule is not checked out at $sdk"
    return 0
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

run "guest address-space aliasing" build_and_run_alias
run "arm64 fiber context switch" build_and_run_fiber

echo
if [[ $failures -eq 0 ]]; then
  echo "all suites passed"
else
  echo "$failures suite(s) failed"
fi
exit $failures
