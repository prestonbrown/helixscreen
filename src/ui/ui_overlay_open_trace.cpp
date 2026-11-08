// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_open_trace.h"

#include "lvgl.h"

namespace helix::nav {

std::size_t OverlayOpenTracker::record_for(const char* name) {
    const char* key = name ? name : "?";
    for (std::size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].name == key) {
            return i;
        }
    }
    if (records_.size() >= kMaxOverlays) {
        return kMaxOverlays;
    }
    records_.push_back(OverlayOpenRecord{});
    records_.back().name = key;
    return records_.size() - 1;
}

void OverlayOpenTracker::drop_pending(std::size_t index) {
    for (std::size_t i = index + 1; i < pending_count_; ++i) {
        pending_[i - 1] = pending_[i];
    }
    --pending_count_;
}

void OverlayOpenTracker::requested(const void* root, const char* name, bool built, double t0_ms) {
    const std::size_t record = record_for(name);
    if (record >= kMaxOverlays) {
        return;
    }
    if (built) {
        ++records_[record].builds;
    }
    for (std::size_t i = 0; i < pending_count_; ++i) {
        if (pending_[i].root == root) {
            drop_pending(i);
            break;
        }
    }
    if (pending_count_ == kMaxPending) {
        drop_pending(0);
    }
    pending_[pending_count_++] = Pending{root, record, t0_ms, false};
}

void OverlayOpenTracker::pushed(const void* root) {
    for (std::size_t i = 0; i < pending_count_; ++i) {
        if (pending_[i].root == root) {
            pending_[i].pushed = true;
        }
    }
}

void OverlayOpenTracker::frame_drawn(double now_ms) {
    std::size_t i = 0;
    while (i < pending_count_) {
        const Pending& p = pending_[i];
        if (now_ms - p.t0_ms > kStaleMs) {
            drop_pending(i);
        } else if (p.pushed) {
            OverlayOpenRecord& r = records_[p.record];
            const double ms = now_ms > p.t0_ms ? now_ms - p.t0_ms : 0.0;
            ++r.opens;
            r.last_ms = ms;
            r.total_ms += ms;
            if (ms > r.max_ms) {
                r.max_ms = ms;
            }
            drop_pending(i);
        } else {
            ++i;
        }
    }
}

namespace {

OverlayOpenTracker g_tracker;

double to_ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
}

// Returns at once unless an open is waiting for its frame, so a drawn frame with no
// overlay opening reads no clock.
void on_render_ready(lv_event_t*) {
    if (g_tracker.awaiting_frame()) {
        g_tracker.frame_drawn(to_ms(std::chrono::steady_clock::now()));
    }
}

} // namespace

void overlay_open_requested(lv_obj_t* root, const char* name, bool built,
                            std::chrono::steady_clock::time_point t0) {
    // Re-hooked on every request so a display created since the last one is the one
    // watched, and the hook never stacks.
    if (lv_display_t* disp = lv_display_get_default()) {
        lv_display_remove_event_cb_with_user_data(disp, on_render_ready, nullptr);
        lv_display_add_event_cb(disp, on_render_ready, LV_EVENT_RENDER_READY, nullptr);
    }
    g_tracker.requested(root, name, built, to_ms(t0));
}

void overlay_open_pushed(lv_obj_t* root) {
    g_tracker.pushed(root);
}

std::vector<OverlayOpenRecord> overlay_open_records() {
    return g_tracker.records();
}

} // namespace helix::nav
