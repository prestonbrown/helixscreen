// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_perf_report.h"

#include "system/debug_bundle_collector.h"

#include <algorithm>
#include <cmath>

#include "hv/json.hpp"

namespace helix {

namespace {

double round1(double v) {
    return std::round(v * 10.0) / 10.0;
}

} // namespace

UiPerfSnapshot capture_ui_perf(lv_display_t* display) {
    UiPerfSnapshot snapshot;
    snapshot.captured = true;
    snapshot.sample = read_print_health_sample(display);
    snapshot.overlays = nav::overlay_open_records();
    return snapshot;
}

nlohmann::json ui_perf_to_json(const UiPerfSnapshot& snapshot) {
    if (!snapshot.captured) {
        return nlohmann::json::object();
    }
    const PrintHealthSample& s = snapshot.sample;
    nlohmann::json frames = {
        {"drawn", s.frames},
        {"render_ms_mean",
         s.frames > 0 ? round1(static_cast<double>(s.render_ns) / 1e6 / s.frames) : 0.0},
        {"slow", s.slow_frames},
        {"slow_over_ms", FrameCostCounter::kSlowRenderNs / 1'000'000ULL},
        {"flushed_screens",
         s.screen_px > 0 ? round1(static_cast<double>(s.flushed_px) / s.screen_px) : 0.0},
    };

    std::vector<const nav::OverlayOpenRecord*> order;
    for (const auto& r : snapshot.overlays) {
        order.push_back(&r);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto* a, const auto* b) { return a->last_ms > b->last_ms; });
    nlohmann::json overlays = nlohmann::json::array();
    for (const auto* r : order) {
        overlays.push_back({
            {"name", r->name},
            {"opens", r->opens},
            {"builds", r->builds},
            {"last_ms", round1(r->last_ms)},
            {"max_ms", round1(r->max_ms)},
            {"mean_ms", r->opens > 0 ? round1(r->total_ms / r->opens) : 0.0},
        });
    }

    return {
        {"frames", frames},
        {"rss_kb", s.rss_kb},
        {"peak_rss_kb", s.peak_rss_kb},
        {"overlays", overlays},
    };
}

void install_ui_perf_bundle_provider() {
    DebugBundleCollector::set_ui_perf_provider(
        [] { return ui_perf_to_json(capture_ui_perf(lv_display_get_default())).dump(); });
}

} // namespace helix
