// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_tape.h"

#include "lvgl/lvgl.h"

#include <cmath>
#include <cstdlib>
#include <fmt/format.h>

namespace helix::ztape {

namespace {

/// Floor division for a negative numerator too.
int floor_div(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

double pixels_per_micron(Zoom zoom, int ruler_top, int ruler_bottom) {
    return static_cast<double>(ruler_bottom - ruler_top) / zoom.span_microns;
}

int row_for(int microns, int value_microns, Zoom zoom, int tip_y, int ruler_top, int ruler_bottom) {
    // Halves round up the screen, as the canvas does.
    return static_cast<int>(std::floor(
        tip_y + (value_microns - microns) * pixels_per_micron(zoom, ruler_top, ruler_bottom) +
        0.5));
}

std::string mm3(int microns) {
    return fmt::format("{:.3f}", std::abs(microns) / 1000.0);
}

} // namespace

int step_microns(int index) {
    if (index < 0 || index >= kStepCount) {
        index = kDefaultStepIndex;
    }
    return kStepMicrons[index];
}

Zoom zoom_for_step(int step) {
    int span = 200;
    if (step >= 1000) {
        span = 4000;
    } else if (step >= 100) {
        span = 1000;
    } else if (step >= 50) {
        span = 400;
    }
    return {span, span / 4};
}

std::vector<Tick> ticks(int value_microns, Zoom zoom, int tip_y, int ruler_top, int ruler_bottom) {
    std::vector<Tick> out;
    const int every = zoom.every_microns;
    // The rows above and below the tip reach at most one span either way.
    const int first = floor_div(value_microns - zoom.span_microns, every) * every;
    for (int v = first; v <= value_microns + zoom.span_microns; v += every) {
        const int y = row_for(v, value_microns, zoom, tip_y, ruler_top, ruler_bottom);
        if (y < ruler_top || y > ruler_bottom) {
            continue;
        }
        out.push_back({y, v});
    }
    return out;
}

std::string tick_label(int microns, Zoom zoom) {
    const double mm = microns / 1000.0;
    if (zoom.every_microns >= 1000 && microns % 1000 == 0) {
        return fmt::format("{}", microns / 1000);
    }
    if (microns % 100 == 0) {
        return fmt::format("{:.1f}", mm);
    }
    return fmt::format("{:.2f}", mm);
}

std::string range_text(Zoom zoom) {
    const int half = zoom.span_microns / 2;
    if (half % 1000 == 0) {
        return fmt::format("\xC2\xB1{} mm", half / 1000);
    }
    return fmt::format("\xC2\xB1{:g} mm", half / 1000.0);
}

BedPlacement place_bed(int value_microns, int estimate_microns, Zoom zoom, int tip_y, int ruler_top,
                       int ruler_bottom, int static_paper_top) {
    if (estimate_microns == kNoEstimate) {
        return {BedView::Unknown, static_paper_top};
    }
    const int to_go = value_microns - estimate_microns;
    // Past the estimate the nozzle rests on the bed: the paper stays at the tip.
    const int paper_top =
        to_go <= 0 ? tip_y
                   : row_for(estimate_microns, value_microns, zoom, tip_y, ruler_top, ruler_bottom);
    if (paper_top > static_paper_top) {
        return {BedView::FarBelow, static_paper_top};
    }
    return {to_go < 0 ? BedView::PastLastTime : BedView::Estimated, paper_top};
}

EstimateSource estimate_source_for(const std::string& calibrate_gcode) {
    if (calibrate_gcode == "PROBE_CALIBRATE") {
        return EstimateSource::ProbeCalibrate;
    }
    if (calibrate_gcode == "Z_ENDSTOP_CALIBRATE") {
        return EstimateSource::Endstop;
    }
    return EstimateSource::None;
}

int expected_drag_microns(EstimateSource source, std::optional<int> start_z_microns,
                          int saved_offset_microns) {
    switch (source) {
    case EstimateSource::ProbeCalibrate:
        if (!start_z_microns) {
            return kNoEstimate;
        }
        return *start_z_microns - 5000 - saved_offset_microns;
    case EstimateSource::Endstop:
        return 0;
    case EstimateSource::None:
        break;
    }
    return kNoEstimate;
}

Guide guide(int value_microns, int estimate_microns, int start_microns, bool compact) {
    if (estimate_microns == kNoEstimate) {
        const int delta = value_microns - start_microns;
        if (delta == 0) {
            return {lv_tr("start position"), GuideTone::Neutral};
        }
        const char* fmt_text =
            delta < 0 ? (compact ? lv_tr("down {} from start") : lv_tr("down {} mm from start"))
                      : (compact ? lv_tr("up {} from start") : lv_tr("up {} mm from start"));
        return {fmt::format(fmt::runtime(fmt_text), mm3(delta)), GuideTone::Neutral};
    }
    const int to_go = value_microns - estimate_microns;
    if (to_go == 0) {
        return {compact ? lv_tr("at last time’s drag point")
                        : lv_tr("right where the paper dragged last time"),
                GuideTone::Above};
    }
    const char* fmt_text =
        to_go > 0 ? (compact ? lv_tr("{} mm above last time")
                             : lv_tr("about {} mm above where the paper dragged last time"))
                  : (compact ? lv_tr("{} mm below last time")
                             : lv_tr("about {} mm below where the paper dragged last time"));
    return {fmt::format(fmt::runtime(fmt_text), mm3(to_go)),
            to_go > 0 ? GuideTone::Above : GuideTone::Past};
}

} // namespace helix::ztape
