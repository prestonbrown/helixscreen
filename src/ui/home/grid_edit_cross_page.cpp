// SPDX-License-Identifier: GPL-3.0-or-later

#include "grid_edit_cross_page.h"

#include <algorithm>

namespace helix {

namespace {

/// Column track width of the 800x480 panel (12 tracks across 710px, 5px
/// gutters), where the zone and push speed were tuned. Stands in for the track
/// before a grid exists.
constexpr float kReferenceTrackPx = 54.58f;

/// Three quarters of a track: 40px on the reference track. Narrower than a
/// track, so a finger carrying a widget into the outer column is not
/// automatically in the zone.
constexpr float kEdgeZoneTrackFraction = 0.75f;

/// Floor for the zone: about a fingertip's contact patch on the smallest
/// panels, whose 34px tracks (480x272) would otherwise give 25px.
constexpr int kEdgeZoneMinPx = 24;

/// One track of push every 300ms: about 180px/s on the reference track.
constexpr int kPushMsPerTrack = 300;

constexpr int64_t kMpxPerPx = 1000;
constexpr float kMsPerS = 1000.0f;

/// The track the tuning reads: the live one, or the reference before a grid exists.
float tuning_track(float cell_w) {
    return cell_w > 0.0f ? cell_w : kReferenceTrackPx;
}

} // namespace

int cross_page_edge_zone_px(float cell_w) {
    const int zone = static_cast<int>(tuning_track(cell_w) * kEdgeZoneTrackFraction);
    return std::max(zone, kEdgeZoneMinPx);
}

int cross_page_push_px_per_s(float cell_w) {
    return static_cast<int>(tuning_track(cell_w) * kMsPerS / kPushMsPerTrack);
}

bool cross_page_past_right_border(int frame_x2, int widget_left, int widget_width) {
    return widget_left > frame_x2 - widget_width / 2;
}

bool cross_page_past_left_border(int frame_x1, int widget_left, int widget_width) {
    return widget_left + widget_width < frame_x1 + widget_width / 2;
}

bool cross_page_drop_creates_page(int page_index, int page_count, bool has_next_page_slot,
                                  bool past_right_border, bool past_left_border) {
    // A release on the page past the last one is a drop into it, wherever the
    // widget is; from the last page only a widget already majority past the
    // right border is dropped there. From the first page a majority past the
    // left border creates a page before it, the same cap permitting.
    const bool on_next_page = page_index == page_count;
    const bool past_last_border = page_index == page_count - 1 && past_right_border;
    const bool past_first_border = page_index == 0 && past_left_border;
    return has_next_page_slot && (on_next_page || past_last_border || past_first_border);
}

CrossPageStep cross_page_step(CrossPageState& state, const CrossPageInput& in) {
    CrossPageStep out;

    const int zone_px = cross_page_edge_zone_px(in.cell_w);
    int zone = 0;
    if (in.pointer_x >= in.frame_x2 - zone_px) {
        zone = 1;
    } else if (in.pointer_x <= in.frame_x1 + zone_px) {
        zone = -1;
    }

    // The push measures time spent in the current zone, so entering, leaving or
    // switching zones starts it over, together with the zone stay.
    const bool zone_changed = zone != state.zone_dir;
    if (zone_changed) {
        state.zone_dir = zone;
        state.push_mpx = 0;
        state.zone_spent = false;
    }

    const int half = in.widget_width / 2;
    if (zone != 0) {
        int64_t push = state.push_mpx;
        if (!zone_changed) {
            push += static_cast<int64_t>(zone) * cross_page_push_px_per_s(in.cell_w) *
                    static_cast<int64_t>(in.elapsed_ms);
        }
        // The caps bound the widget's left edge, so a pointer that already puts
        // the widget past one pulls it back to the cap.
        if (zone > 0) {
            const int max_left = in.frame_x2 - half + kCrossPagePushCapSlopPx;
            push = std::min(push, static_cast<int64_t>(max_left - in.widget_left) * kMpxPerPx);
        } else {
            const int min_left = in.frame_x1 - half - kCrossPagePushCapSlopPx;
            push = std::max(push, static_cast<int64_t>(min_left - in.widget_left) * kMpxPerPx);
        }
        state.push_mpx = static_cast<int>(push);
    }
    out.widget_left = in.widget_left + static_cast<int>(state.push_mpx / kMpxPerPx);

    const bool majority_left = out.widget_left + in.widget_width < in.frame_x1 + half;
    const bool majority_right =
        cross_page_past_right_border(in.frame_x2, out.widget_left, in.widget_width);

    if (!majority_left && !majority_right) {
        state.crossing_armed = true;
    } else if (state.crossing_armed) {
        state.crossing_armed = false;
        const int cross_dir = majority_right ? 1 : -1;
        if (!(state.zone_spent && zone == cross_dir)) {
            out.flip_dir = cross_dir;
            if (zone != 0) {
                state.zone_spent = true;
            }
        }
    }

    // The dwell waits on a crossing that has not happened yet: after a crossing
    // the widget is already past the border, and after any flip the zone is spent.
    const int dwell = (zone != 0 && !state.zone_spent && state.crossing_armed) ? zone : 0;
    out.dwell_changed = dwell != state.dwell_dir;
    out.dwell_dir = dwell;
    state.dwell_dir = dwell;
    return out;
}

void cross_page_note_dwell_flip(CrossPageState& state) {
    state.zone_spent = true;
    state.dwell_dir = 0;
}

} // namespace helix
