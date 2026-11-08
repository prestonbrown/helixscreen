// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_dryer_presentation.h
 * @brief The words a dryer page shows for a cycle, as pure functions of DryerInfo.
 */

#pragma once

#include "ams_drying_types.h"

#include <string>

namespace helix::ui {

/// The status line under a running cycle's readout: what the dryer is doing and the
/// time left, "Heating to 55°C: 3:25 left" or "Drying: 3:25 left". A backend that does
/// not report a phase reads as drying. Empty when no cycle is running.
[[nodiscard]] std::string drying_status_text(const DryerInfo& dryer);

} // namespace helix::ui
