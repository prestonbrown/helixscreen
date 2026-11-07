// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ipv4_config.h"
#include "ipv4_config_system.h"

#include <lvgl/lvgl.h>

#include <functional>
#include <optional>
#include <string>

namespace helix::settings {

/**
 * @brief Keep trying to restore @p link's pending IPv4 record until it ends
 *
 * Tries on helix::net::Ipv4RevertRetry's schedule, never while a job holds the
 * machine, and ends in one of three ways: restored (a notice says so), nothing
 * left to restore, or given up (the record is kept and marked, and a notice
 * names why). A restore that is refused gives up for good; one that runs out of
 * time is tried again at the next start. @p include_own also takes a record this process wrote,
 * for a change whose screen went away or whose revert failed. LVGL thread.
 */
void retry_ipv4_revert(helix::net::Ipv4Link link, bool include_own);

/// Mark @p link's record given up and say why, unless the record already says
/// the same. nullopt is a record that names no link it can be read for: every
/// link is told. LVGL thread.
void give_up_ipv4_revert(std::optional<helix::net::Ipv4Link> link, const std::string& why,
                         helix::net::Ipv4GiveUp how);

/// How a revert the retry owned ended: the previous config is back, the
/// record is marked given up on disk and a new change may replace it, or the
/// record could not be marked and still refuses a new change.
enum class Ipv4RevertEnd { Restored, GaveUp, GaveUpUnrecorded };

/// Why @p link's revert was given up without its record saying so, while that
/// holds this run. LVGL thread.
std::optional<std::string> ipv4_revert_unrecorded(helix::net::Ipv4Link link);

/// Called on the LVGL thread each time a revert ends, after the record says
/// so. One listener; an empty function clears it. LVGL thread.
void set_ipv4_revert_listener(std::function<void(helix::net::Ipv4Link, Ipv4RevertEnd)> listener);

/// At app start: retry a record another run left. A record that fails to read
/// is read again on the retry's schedule, and a notice says so if it never
/// reads; its bytes are left as they are. LVGL thread.
void start_ipv4_startup_revert();

/// The retry's tick timer while a revert is being retried, else null.
lv_timer_t* ipv4_revert_retry_timer_for_test();

/// The startup read's tick timer while it reads again, else null.
lv_timer_t* ipv4_startup_read_timer_for_test();

} // namespace helix::settings
