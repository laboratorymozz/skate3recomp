// ELF wrapper around src/core/fiber_arm64_apple.S, for running the iOS fiber
// switch on an aarch64-linux host under qemu when no Apple Silicon Mac is
// available. See ../run_tests.sh.
//
// The only difference between Apple's arm64 assembler and the ELF one that
// matters for that file is the leading-underscore symbol prefix, so rename it
// lexically and include the real source unmodified. The instruction stream
// under test is byte-for-byte the one that ships; nothing about the frame
// layout, the register set or the calling convention is altered.

#define _rex_fiber_switch rex_fiber_switch
#include "fiber_arm64_apple.S"
