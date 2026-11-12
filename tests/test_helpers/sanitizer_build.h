// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::test {

// Sanitizer builds instrument every load and store, so work runs tens of times
// slower there; a time bound sized for a plain build needs scaling in one.
// Both spellings: GCC defines __SANITIZE_*__, clang answers only __has_feature.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) ||                               \
    (defined(__has_feature) &&                                                                     \
     (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
constexpr int kSanitizerSlowdown = 20;
#else
constexpr int kSanitizerSlowdown = 1;
#endif

} // namespace helix::test
