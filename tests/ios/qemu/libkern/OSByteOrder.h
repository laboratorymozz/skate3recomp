/*
 * Stub for the Darwin header that <rex/platform.h> includes on REX_PLATFORM_MAC,
 * so the real platform.h can be used unmodified when the fiber test is built for
 * aarch64-linux under qemu. See ../run_tests.sh.
 *
 * Nothing on the path under test (fiber.h, fiber_ios.cpp, fiber_arm64_apple.S)
 * uses the OSSwap* family, so an empty header is enough. If a future test needs
 * one of them, define it here rather than reaching for the real thing.
 */

#pragma once
