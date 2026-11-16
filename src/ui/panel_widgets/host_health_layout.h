// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure layout decision for the Host Health home widget.
//
// Free of LVGL so it is unit-testable without a display. HostHealthWidget measures
// the minimum box each rung draws in for the state being decided and feeds it here;
// this header picks the rung and the flags the XML binds, and composes the readings'
// texts so they never depend on a font.

#include "core/temperature_units.h"
#include "src/ui/panel_widgets/size_rung.h"
#include "value_status.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace helix {

struct HostHealthLayoutInput {
    int content_w = 0; ///< the box the content draws in, chrome already subtracted
    int content_h = 0;
    bool printing = false; ///< print_active: Wide swaps memory for the busiest MCU
    int mcu_count = 0;     ///< MCUs the printer reports
    int mcu_row_pitch = 0; ///< px one MCU row costs at Full, gap included
    /// Minimum box per rung for THIS state. The Full box holds the host block, the
    /// throttle line at its worst (shown) and ONE MCU row; more rows cost the pitch each.
    RungNeeds needs;
    /// Full is a size class as well as a content fit: a box a 2x1 tile fills holds the
    /// host block, but the rung is for 2x2 and up, so it needs at least this height.
    int full_min_h = 0;
};

/// The published verdict. `rung` is SizeRung's int; the flags are 0/1 subjects.
struct HostHealthVerdict {
    SizeRung rung = SizeRung::Glance;
    bool fits = false;
    /// Wide while printing: the busiest MCU takes the memory row's place.
    bool show_mcu_row = false;
    /// Full: how many MCU rows the box holds, never more than the printer has.
    int mcu_rows = 0;
};

[[nodiscard]] constexpr HostHealthVerdict
decide_host_health_layout(const HostHealthLayoutInput& in) {
    HostHealthVerdict v;
    RungNeeds needs = in.needs;
    needs.full.h = std::max(needs.full.h, in.full_min_h);
    v.rung = choose_size_rung(in.content_w, in.content_h, needs);
    v.fits = holds_any_rung(in.content_w, in.content_h, needs);
    v.show_mcu_row = v.rung == SizeRung::Wide && in.printing && in.mcu_count > 0;
    if (v.rung == SizeRung::Full && in.mcu_count > 0) {
        const int spare = std::max(0, in.content_h - in.needs.full.h);
        const int extra = in.mcu_row_pitch > 0 ? spare / in.mcu_row_pitch : 0;
        v.mcu_rows = std::min(in.mcu_count, 1 + extra);
    }
    return v;
}

/// What the Glance rung draws under the chip: the reading of the metric that put the
/// host at its status, as a number the tile can hold in any language. Metrics with no
/// short number of their own (throttle, retransmits) draw "!".
struct HostHealthReadings {
    int cpu_pct = -1;
    int cpu_temp_c10 = -1;
    int mcu_max_load_pct = -1;
    int disk_free_mb = -1;
};

[[nodiscard]] inline std::string format_glance_text(status::HealthCause cause,
                                                    const HostHealthReadings& r) {
    char buf[16];
    switch (cause) {
    case status::HealthCause::HostTemp:
        if (r.cpu_temp_c10 >= 0) {
            std::snprintf(buf, sizeof(buf), "%d\xc2\xb0", (r.cpu_temp_c10 + 5) / 10);
            return buf;
        }
        return "--";
    case status::HealthCause::McuLoad:
        if (r.mcu_max_load_pct >= 0) {
            std::snprintf(buf, sizeof(buf), "%d%%", r.mcu_max_load_pct);
            return buf;
        }
        return "--";
    case status::HealthCause::DiskFree:
        if (r.disk_free_mb >= 0) {
            if (r.disk_free_mb >= 1024)
                std::snprintf(buf, sizeof(buf), "%dG", r.disk_free_mb / 1024);
            else
                std::snprintf(buf, sizeof(buf), "%dM", r.disk_free_mb);
            return buf;
        }
        return "--";
    case status::HealthCause::Throttle:
    case status::HealthCause::McuRetransmit:
        return "!";
    case status::HealthCause::None:
    case status::HealthCause::HostCpu:
        break;
    }
    if (r.cpu_pct < 0)
        return "--";
    std::snprintf(buf, sizeof(buf), "%d%%", r.cpu_pct);
    return buf;
}

/// "61°C" at whole degrees, "61.4°C" with a decimal, in the display unit; "--" when
/// the host reports none.
[[nodiscard]] inline std::string format_cpu_temp(int c10, int decimals) {
    if (c10 < 0)
        return "--";
    char buf[24];
    const double shown = helix::units::to_display_temp(c10 / 10.0);
    if (decimals > 0)
        std::snprintf(buf, sizeof(buf), "%.1f%s", shown, helix::units::display_temp_symbol());
    else
        std::snprintf(buf, sizeof(buf), "%d%s", static_cast<int>(std::lround(shown)),
                      helix::units::display_temp_symbol());
    return buf;
}

[[nodiscard]] inline std::string format_percent(int pct) {
    if (pct < 0)
        return "--";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    return buf;
}

} // namespace helix
