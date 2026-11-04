// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_switch_trace.h"

#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <string>

namespace helix::nav {

namespace {
using SwitchClock = std::chrono::steady_clock;
constexpr size_t kSwitchTraceMaxMarks = 12;
struct SwitchTraceMark {
    const char* name;
    double ms;
};
struct SwitchTraceFrames {
    double layout_ms = 0;
    double render_ms = 0;
    int renders = 0;
};
struct SwitchTraceState {
    bool in_switch = false;
    bool awaiting_frame = false;
    int panel_id = -1;
    bool hooked = false;
    bool after_rendered = false;
    SwitchClock::time_point t0{}, last{}, sync_end{}, refr_start{}, render_start{};
    bool rendering = false;
    SwitchTraceMark marks[kSwitchTraceMaxMarks]{};
    size_t n = 0;
    SwitchTraceFrames during;
    SwitchTraceFrames after; // the first rendered refresh after the switch returned
    double to_after_ms = 0;
};

SwitchTraceState g_switch_trace;

template <typename D> static double ms(D d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

void finish() {
    g_switch_trace.awaiting_frame = false;
    std::string phases;
    for (size_t i = 0; i < g_switch_trace.n; i++) {
        phases +=
            fmt::format(" {}={:.1f}", g_switch_trace.marks[i].name, g_switch_trace.marks[i].ms);
    }
    const auto& t = g_switch_trace;
    const double sync_ms = ms(t.sync_end - t.t0);
    // start-to-frame runs from the switch's start: the tap's release for an
    // inline switch, the UpdateQueue drain after it for a queued one.
    const std::string next = t.after_rendered
                                 ? fmt::format("next frame +{:.1f}ms layout={:.1f} render={:.1f} | "
                                               "start-to-frame {:.1f}ms",
                                               t.to_after_ms, t.after.layout_ms, t.after.render_ms,
                                               sync_ms + t.to_after_ms)
                                 : std::string("no frame rendered after the switch");
    spdlog::info("[NavigationManager] Panel switch to {} took {:.1f}ms:{} | in-switch "
                 "frames={} layout={:.1f} render={:.1f} | {}",
                 t.panel_id, sync_ms, phases, t.during.renders, t.during.layout_ms,
                 t.during.render_ms, next);
}

void on_display_event(lv_event_t* e) {
    if (!g_switch_trace.in_switch && !g_switch_trace.awaiting_frame) {
        return;
    }
    const auto now = SwitchClock::now();
    SwitchTraceFrames& f = g_switch_trace.in_switch ? g_switch_trace.during : g_switch_trace.after;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        g_switch_trace.refr_start = now;
        g_switch_trace.rendering = false;
        break;
    case LV_EVENT_RENDER_START:
        f.layout_ms += ms(now - g_switch_trace.refr_start);
        g_switch_trace.render_start = now;
        g_switch_trace.rendering = true;
        break;
    case LV_EVENT_REFR_READY:
        if (!g_switch_trace.rendering) {
            f.layout_ms += ms(now - g_switch_trace.refr_start);
            break;
        }
        g_switch_trace.rendering = false;
        f.render_ms += ms(now - g_switch_trace.render_start);
        f.renders++;
        if (g_switch_trace.awaiting_frame && !g_switch_trace.in_switch) {
            g_switch_trace.after_rendered = true;
            g_switch_trace.to_after_ms = ms(now - g_switch_trace.sync_end);
            finish();
        }
        break;
    default:
        break;
    }
}
} // namespace

SwitchTrace::SwitchTrace(int panel_id) : owns_(!g_switch_trace.in_switch) {
    if (!owns_) {
        return;
    }
    if (g_switch_trace.awaiting_frame) {
        finish(); // a switch before the previous one's first frame
    }
    g_switch_trace = SwitchTraceState{};
    g_switch_trace.in_switch = true;
    g_switch_trace.panel_id = panel_id;
    g_switch_trace.t0 = g_switch_trace.last = SwitchClock::now();
    // Re-hooked on every switch, so a display created since the last one
    // (even at the same address) is the one traced, and the hooks never
    // stack. The callbacks stay in place between switches and return at
    // once: removing one from inside its own dispatch, where finish() runs,
    // would edit the list being walked.
    if (lv_display_t* disp = lv_display_get_default()) {
        lv_display_remove_event_cb_with_user_data(disp, on_display_event, nullptr);
        for (lv_event_code_t code :
             {LV_EVENT_REFR_START, LV_EVENT_RENDER_START, LV_EVENT_REFR_READY}) {
            lv_display_add_event_cb(disp, on_display_event, code, nullptr);
        }
        g_switch_trace.hooked = true;
    }
}

SwitchTrace::~SwitchTrace() {
    if (!owns_) {
        return;
    }
    g_switch_trace.in_switch = false;
    g_switch_trace.sync_end = SwitchClock::now();
    g_switch_trace.awaiting_frame = true;
    if (!g_switch_trace.hooked) {
        finish();
    }
}

void SwitchTrace::mark(const char* phase) {
    if (!g_switch_trace.in_switch || g_switch_trace.n >= kSwitchTraceMaxMarks) {
        return;
    }
    auto now = SwitchClock::now();
    g_switch_trace.marks[g_switch_trace.n++] = {phase, ms(now - g_switch_trace.last)};
    g_switch_trace.last = now;
}

} // namespace helix::nav
