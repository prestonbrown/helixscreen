// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_dryer_presentation.h
 * @brief The words a dryer page shows for a cycle, as pure functions of DryerInfo.
 */

#pragma once

#include "ams_drying_types.h"

#include <optional>
#include <string>

namespace helix::ui {

/// The status line under a running cycle's readout: what the dryer is doing and the
/// time left, "Heating to 55°C: 3:25 left" or "Drying: 3:25 left". A backend that does
/// not report a phase reads as drying. Empty when no cycle is running.
[[nodiscard]] std::string drying_status_text(const DryerInfo& dryer);

/// @p minutes as the hours a duration field shows: whole hours bare ("4"), anything
/// else to two places with trailing zeros dropped ("4.5", "4.33").
[[nodiscard]] std::string format_duration_hours(int minutes);

/// The minutes a duration field's hours stand for, rounded to the nearest minute.
/// nullopt for a blank, unreadable, zero or negative entry.
[[nodiscard]] std::optional<int> parse_duration_hours(const char* text);

} // namespace helix::ui
