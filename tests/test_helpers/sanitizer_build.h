// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::test {

// Sanitizer builds instrument every load and store, so work runs tens of times
// slower there; a time bound sized for a plain build needs scaling in one.
// Both spellings: GCC defines __SANITIZE_*__, clang answers only __has_feature.
// __has_feature is tested in its own #if: GCC before 14 does not define it, and
// `__has_feature(x)` on one line with `defined(__has_feature)` does not parse there.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define HELIX_TEST_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define HELIX_TEST_SANITIZER_BUILD 1
#endif
#endif

#ifdef HELIX_TEST_SANITIZER_BUILD
constexpr int kSanitizerSlowdown = 20;
#else
constexpr int kSanitizerSlowdown = 1;
#endif

} // namespace helix::test
