// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

/**
 * @file notice.h
 * @brief What a lower layer tells the user, as data (prestonbrown/helixscreen#1952).
 *
 * Backends and services publish a Notice; the UI decides how to present it.
 * publish_notice() delivers on the calling thread and marshals nothing, so the
 * subscriber is what must be safe to call from any thread. Code that holds
 * main-thread state around a notice defers it itself.
 */

namespace helix {

enum class NoticeSeverity { INFO, SUCCESS, WARNING, ERROR };

/// Runs on the main thread when the user takes the notice's action.
using NoticeActionCallback = void (*)(void* user_data);

inline constexpr uint32_t kNoticeDefaultDurationMs = 4000;

struct Notice {
    NoticeSeverity severity = NoticeSeverity::INFO;
    std::string message;
    uint32_t duration_ms = kNoticeDefaultDurationMs;
    std::string action_text{}; ///< Label of the optional action; needs @ref action too
    NoticeActionCallback action = nullptr;
    void* action_user_data = nullptr;
};

using NoticeSubscriber = void (*)(const Notice&);

/**
 * @brief Install the one subscriber every notice goes to and return the previous
 *        one; nullptr removes it.
 *
 * Not synchronized: the UI installs it during static initialization, before any
 * thread that could publish exists.
 */
NoticeSubscriber set_notice_subscriber(NoticeSubscriber subscriber);

/// Hand @p notice to the subscriber on this thread. With none installed the
/// notice is logged and dropped.
void publish_notice(const Notice& notice);

} // namespace helix
