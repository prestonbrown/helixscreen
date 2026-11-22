// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

/**
 * @file remote_client.h
 * @brief Entry point for the folded remote-control client.
 *
 * The helixctl client is compiled into helix-screen and reached via the `ctl`
 * and `repl` subcommands (main.cpp dispatches before the app initializes).
 * There is no separate helixctl binary. Dev/test-only — compiled out of release
 * builds (see HELIX_ENABLE_REMOTE_CONTROL).
 *
 * @param argc/argv Forwarded from main with the subcommand as argv[0]
 *                  (e.g. {"ctl", "navigate", "controls"} or {"repl"}).
 * @return process exit code
 */

namespace helix {
int remote_client_main(int argc, char** argv);

/**
 * @brief True for a describe_screen locator written without the '@' prefix.
 *
 * An "s" (active screen) or "t" (top layer) root followed by one or more
 * non-empty segments — the exact shape `ls` prints. Widget names never contain
 * '/', so the root prefix alone makes the form unambiguous, and a path pasted
 * out of a listing works directly as a click/set_value/scroll target.
 *
 * Segments are names, "name[k]" ordinals, or child indices (see path_of() in
 * widget_resolution.h), so the contents are not checked beyond rejecting
 * characters no locator contains: whitespace, and the '*'/'?' that would make
 * it a glob instead.
 *
 * Inline because the test binary does not link remote_client.o (the ctl client
 * is app-only); keeping it header-side lets the tests exercise it directly.
 */
inline bool is_bare_path(const std::string& target) {
    if (target.size() < 3 || (target[0] != 's' && target[0] != 't') || target[1] != '/') {
        return false;
    }
    bool segment_has_content = false;
    for (size_t i = 2; i < target.size(); ++i) {
        const char c = target[i];
        if (c == '/') {
            if (!segment_has_content) {
                return false; // empty segment: "s//1" or a trailing slash
            }
            segment_has_content = false;
            continue;
        }
        const bool addressable = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '[' ||
                                 c == ']';
        if (!addressable) {
            return false;
        }
        segment_has_content = true;
    }
    return segment_has_content;
}

/**
 * @brief Seconds the client waits for a command's reply.
 *
 * `wait_for` and `wait_idle` block on the server for the caller's own
 * --timeout, so the read window covers that plus a margin, and never drops
 * below the 120 s those commands default to. Every other command answers at
 * once and gets 30 s. A non-positive timeout means none was given.
 */
inline int reply_window_secs(const std::string& command, int requested_timeout_secs) {
    constexpr int kQuickWindowSecs = 30;
    constexpr int kBlockingFloorSecs = 120;
    constexpr int kBlockingMarginSecs = 10;
    if (command != "wait_for" && command != "wait_idle") {
        return kQuickWindowSecs;
    }
    const int wanted = requested_timeout_secs + kBlockingMarginSecs;
    return wanted > kBlockingFloorSecs ? wanted : kBlockingFloorSecs;
}
} // namespace helix
