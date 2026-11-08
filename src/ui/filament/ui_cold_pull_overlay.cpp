// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_cold_pull_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_guided_device_flow.h"
#include "ui_nav.h"
#include "ui_progress_arc.h"

#include "cold_pull_runner.h"
#include "core/static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace helix::ui {

namespace {

/// The integer an XML event carries in user_data, or 0.
int user_int(lv_event_t* e) {
    const char* v = static_cast<const char*>(lv_event_get_user_data(e));
    return v ? std::atoi(v) : 0;
}

template <typename F> void with_runner(F&& fn) {
    if (auto* r = get_cold_pull_runner()) {
        fn(*r);
    }
}

} // namespace

ColdPullOverlay& get_cold_pull_overlay() {
    return lazy_global<ColdPullOverlay>("ColdPullOverlay");
}

void ColdPullOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_cold_pull_material",
         [](lv_event_t* e) {
             const int m = user_int(e);
             if (cold_pull_material_valid(m)) {
                 with_runner([m](ColdPullRunner& r) {
                     r.select_material(static_cast<ColdPullMaterial>(m));
                 });
             }
         }},
        {"on_cold_pull_soak",
         [](lv_event_t* e) {
             const int d = user_int(e);
             with_runner([d](ColdPullRunner& r) { r.adjust_soak(d); });
         }},
        {"on_cold_pull_pull",
         [](lv_event_t* e) {
             const int d = user_int(e);
             with_runner([d](ColdPullRunner& r) { r.adjust_pull(d); });
         }},
        {"on_cold_pull_start",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { (void)r.start_from_setup(); }); }},
        {"on_cold_pull_home_park",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.home_and_park(); }); }},
        {"on_cold_pull_unload",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.unload_lane(); }); }},
        {"on_cold_pull_continue",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.toolhead_clear(); }); }},
        {"on_cold_pull_extrude",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.extrude(); }); }},
        {"on_cold_pull_fed", [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.fed(); }); }},
        {"on_cold_pull_pulled",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.pulled(); }); }},
        {"on_cold_pull_repeat",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.repeat(); }); }},
        {"on_cold_pull_done",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.done(); }); }},
        {"on_cold_pull_cancel",
         [](lv_event_t*) { with_runner([](ColdPullRunner& r) { r.cancel(); }); }},
        // Closing mid-run cancels it in on_dismissed().
        {"on_cold_pull_close", [](lv_event_t*) { helix::nav::go_back(); }},
    });
}

lv_obj_t* ColdPullOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    if (lv_obj_t* ring = lv_obj_find_by_name(overlay_root_, "cold_pull_ring")) {
        (void)attach_progress_arc_owned(ring, nullptr);
    }
    return overlay_root_;
}

void ColdPullOverlay::on_deactivating(DeactivateReason reason) {
    // The runner owns the run: a screensaver or a view pushed on top must not
    // stop a nozzle the user is waiting on. Leaving is on_dismissed().
    (void)reason;
}

void ColdPullOverlay::on_dismissed() {
    with_runner([](ColdPullRunner& r) {
        if (r.running()) {
            spdlog::info("[ColdPull] Overlay closed mid-run: cancelling");
            r.cancel();
        }
    });
}

void open_cold_pull_overlay() {
    auto* runner = get_cold_pull_runner();
    if (!runner) {
        spdlog::warn("[ColdPull] open: no runner");
        return;
    }
    runner->prepare_setup();
    get_cold_pull_overlay().show(lv_screen_active());
}

void register_cold_pull_flow() {
    GuidedDeviceFlow flow;
    flow.start = [](const std::string&) { open_cold_pull_overlay(); };
    register_guided_device_flow("cold_pull", std::move(flow));
}

} // namespace helix::ui
