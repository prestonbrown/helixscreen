// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_overlay_open_trace.h"

#include "json_fwd.h"
#include "print_health_log.h"

#include <vector>

namespace helix {

/// What the UI has cost since the process started: drawn frames and their render time,
/// memory, and each overlay's open time and build count. Read by `ctl perf` and carried
/// into the debug bundle.
struct UiPerfSnapshot {
    bool captured = false;
    PrintHealthSample sample;
    std::vector<nav::OverlayOpenRecord> overlays;
};

/// Main thread: reads the display counters and the overlay tracker.
UiPerfSnapshot capture_ui_perf(lv_display_t* display);

/// Pure. Overlays come out slowest last open first.
nlohmann::json ui_perf_to_json(const UiPerfSnapshot& snapshot);

/// Gives the debug bundle its `ui_perf` section, read from the default display.
void install_ui_perf_bundle_provider();

} // namespace helix
