// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ipv4_config.h"

#include <functional>
#include <string>

namespace helix::settings {

/**
 * @brief Keep trying to restore @p link's pending IPv4 record until it ends
 *
 * Tries on helix::net::Ipv4RevertRetry's schedule, never while a job holds the
 * machine, and ends in one of three ways: restored (a notice says so), nothing
 * left to restore, or given up (the record is marked failed and kept, and a
 * notice names why). @p include_own also takes a record this process wrote,
 * for a change whose screen went away or whose revert failed. LVGL thread.
 */
void retry_ipv4_revert(helix::net::Ipv4Link link, bool include_own);

/// Mark @p link's record failed and say why, once. LVGL thread.
void give_up_ipv4_revert(helix::net::Ipv4Link link, const std::string& why);

/// How a revert the retry owned ended: the previous config is back, or the
/// record is marked failed on disk and a new change may replace it.
enum class Ipv4RevertEnd { Restored, GaveUp };

/// Called on the LVGL thread each time a revert ends, after the record says
/// so. One listener; an empty function clears it. LVGL thread.
void set_ipv4_revert_listener(std::function<void(helix::net::Ipv4Link, Ipv4RevertEnd)> listener);

/// At app start: retry a record another run left. LVGL thread.
void start_ipv4_startup_revert();

} // namespace helix::settings
