// skate3 - ReXGlue Recompiled Project
//
// Apple platform discrimination.
//
// TargetConditionals.h defines TARGET_OS_MAC as 1 on *every* Apple platform,
// iOS included, so `#if defined(__APPLE__)` and the SDK's REX_PLATFORM_MAC both
// select the macOS path when building for a phone. Most of the Darwin code is
// genuinely shared; the places that are not need to tell the two apart.
//
// SKATE3_PLATFORM_APPLE - any Apple platform (macOS, iOS, simulator)
// SKATE3_PLATFORM_MACOS - desktop macOS only
// SKATE3_PLATFORM_IOS   - iOS and iPadOS, device or simulator

#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__)
#define SKATE3_PLATFORM_APPLE 1
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#define SKATE3_PLATFORM_IOS 1
#else
#define SKATE3_PLATFORM_MACOS 1
#endif
#endif

#ifndef SKATE3_PLATFORM_APPLE
#define SKATE3_PLATFORM_APPLE 0
#endif
#ifndef SKATE3_PLATFORM_MACOS
#define SKATE3_PLATFORM_MACOS 0
#endif
#ifndef SKATE3_PLATFORM_IOS
#define SKATE3_PLATFORM_IOS 0
#endif
