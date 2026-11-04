// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "calibration_types.h"
#include "helix_shared_callback.h"
#include "i_moonraker_api.h"
#include "json_fwd.h"

#include <functional>
#include <vector>

/**
 * @file screws_tilt_dialect.h
 * @brief Capability questions for the firmware screws-tilt dialects
 *
 * Generic code (discovery, the advanced API, the screws-tilt panel) asks
 * these and never names the firmware that answers; which dialect a printer
 * speaks is PrinterDiscovery::screws_tilt_dialect(). Adding a firmware with
 * its own screws-tilt module touches this file's implementation only.
 */

namespace helix {

class IMoonrakerClient;
class PrinterDiscovery;

namespace screws_tilt {

/// Connect-time cleanup of a calibration state a previous session left held.
/// No-op for dialects whose firmware holds no such state.
void reconcile_on_connect(IMoonrakerClient& client, const PrinterDiscovery& hw,
                          const nlohmann::json& initial_status);

/// Leave the firmware's calibration state when a run is cancelled. No-op for
/// dialects whose firmware holds no such state. @p client must outlive the
/// round trip.
void request_exit(IMoonrakerClient& client, const PrinterDiscovery& hw);

/// Starts the dialect's own calibration sequence and returns true, or returns
/// false when the printer speaks upstream SCREWS_TILT_CALCULATE and the
/// caller runs the standard path.
bool start_dialect_sequence(IMoonrakerClient& client, IMoonrakerAPI& api,
                            const PrinterDiscovery& hw, ScrewTiltCallback on_success,
                            helix::SharedCallback<void(const MoonrakerError&)> on_error);

/// configfile.settings sections that can carry screw_thread, upstream first.
[[nodiscard]] const std::vector<const char*>& config_section_names();

} // namespace screws_tilt
} // namespace helix
