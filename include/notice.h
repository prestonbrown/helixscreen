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

class IMoonrakerAPI;

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

/// Something only the UI can do, named here so the layer asking for it includes
/// no UI header. The file that owns the UI installs the handler.
enum class UiRequestKind {
    OfferClearAfterUnverifiedInsert,      ///< Main thread; @ref UiRequest::slot is the lane
    PromptEndHeldNozzleOffsetCalibration, ///< Main thread
    OpenHardwareHealth,                   ///< Main thread
    OpenPrintStatus,        ///< Main thread; a no-op while print status is on the stack
    ExpectKlippyDisconnect, ///< Any thread; in effect before publish returns
    ExpectKlippyRestart,    ///< Any thread; @ref UiRequest::api is the connection about to drop
    kCount
};

struct UiRequest {
    UiRequestKind kind;
    int slot = -1;
    IMoonrakerAPI* api = nullptr;
};

using UiRequestHandler = void (*)(const UiRequest&);

/**
 * @brief Install the one handler for @p kind and return the previous one; nullptr
 *        removes it.
 *
 * Not synchronized, for the reason set_notice_subscriber() is not.
 */
UiRequestHandler set_ui_request_handler(UiRequestKind kind, UiRequestHandler handler);

/// Run the handler for @p request on this thread, before returning. No result
/// comes back, so a caller cannot make its flow wait on the UI. The thread stated
/// on each kind is the caller's to honor. With no handler the request is logged
/// and dropped.
void publish_ui_request(const UiRequest& request);

/// Installs a handler from a namespace-scope object, so no boot path (desktop,
/// firmware, tests) has a step to forget. Its file must be linked by another
/// reference: the firmware's archive drops a translation unit nothing names.
struct UiRequestRegistration {
    UiRequestRegistration(UiRequestKind kind, UiRequestHandler handler) {
        set_ui_request_handler(kind, handler);
    }
};

} // namespace helix
