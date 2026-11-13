// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// InfoQrModal open/close stress with the SW render thread live and a
// synchronous refresh between each cycle - the shape of the #1673 K1 crash,
// where the swdraw thread blended a transitional layer source the modal
// teardown had already freed. Host test builds run the same single render
// thread as the device (LV_USE_OS=PTHREAD, LV_DRAW_SW_DRAW_UNIT_CNT=1), so an
// unbalanced free surfaces here as an ASAN use-after-free instead of only on
// slow dual-core hardware.
//
// Run under ASAN on the test host:
//   scripts/test-host-run.sh asan '[info_qr_modal]'
// Soak locally:
//   INFO_QR_STRESS_ITERATIONS=500 ./build/bin/helix-tests '[info_qr_modal]'
//
// Tagged [.ui_integration] (hidden by default) - needs the XML component
// tree on disk. Mirrors test_action_prompt_modal_stress.cpp's tagging.

#include "ui_info_qr_modal.h"
#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

int env_iterations(int default_count) {
    if (const char* v = std::getenv("INFO_QR_STRESS_ITERATIONS")) {
        try {
            int n = std::stoi(v);
            if (n > 0)
                return n;
        } catch (...) {
        }
    }
    return default_count;
}

helix::ui::InfoQrModal::Config qr_config() {
    helix::ui::InfoQrModal::Config cfg;
    cfg.icon = "mdi-help-circle";
    cfg.title = "QR stress";
    cfg.message = "render-thread race stress";
    cfg.url = "https://helixscreen.org/help";
    cfg.url_text = "helixscreen.org/help";
    return cfg;
}

// Counts every widget on the screen and both layers. Modal teardown reparents
// a dying dialog to lv_layer_top() before its async delete, so a census of the
// screen alone stays green through a dropped delete; walking all three roots
// catches both the stranded-on-screen and stranded-on-layer shapes.
size_t widget_census(lv_obj_t* obj) {
    size_t n = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        n += widget_census(lv_obj_get_child(obj, i));
    return n;
}

size_t ui_census() {
    return widget_census(lv_screen_active()) + widget_census(lv_layer_top()) +
           widget_census(lv_layer_sys());
}

lv_obj_t* show_one() {
    if (!helix::ui::InfoQrModal::show_owned(qr_config())) {
        SKIP("info_qr_modal component not available in test fixture");
    }
    auto& stack = ModalStack::instance();
    lv_obj_t* dialog = stack.top_dialog();
    REQUIRE(dialog != nullptr);
    // The QR canvas is the container's only child; asserting it keeps a
    // registration regression from turning the whole case into a no-op.
    lv_obj_t* container = lv_obj_find_by_name(dialog, "qr_container");
    REQUIRE(container != nullptr);
    REQUIRE(lv_obj_get_child_count(container) == 1);
    return dialog;
}

// One device-shaped cycle: build, force the synchronous refresh that hands
// draw tasks (including the qr_container's transitional layer) to the swdraw
// thread, then hide so the async delete races whatever the thread still holds.
void cycle_modal(LVGLUITestFixture& fx, lv_obj_t* dialog, int ticks) {
    lv_obj_update_layout(dialog);
    lv_refr_now(nullptr);
    Modal::hide(dialog);
    fx.process_lvgl(ticks);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "InfoQrModal stress: show-refresh-hide cycles",
                 "[info_qr_modal][stress][.ui_integration]") {
    const size_t baseline = ui_census();
    const int iterations = env_iterations(150);
    spdlog::info("[info_qr-stress/cycles] {} iterations", iterations);

    for (int i = 0; i < iterations; ++i) {
        lv_obj_t* dialog = show_one();
        cycle_modal(*this, dialog, 3 + (i % 8));
    }

    process_lvgl(120);
    REQUIRE(ModalStack::instance().stack_empty());
    REQUIRE(ui_census() == baseline);
}

TEST_CASE_METHOD(LVGLUITestFixture, "InfoQrModal stress: hide before the render drain",
                 "[info_qr_modal][stress][.ui_integration]") {
    const size_t baseline = ui_census();
    const int iterations = env_iterations(200);
    spdlog::info("[info_qr-stress/minimal-ticks] {} iterations", iterations);

    for (int i = 0; i < iterations; ++i) {
        lv_obj_t* dialog = show_one();
        cycle_modal(*this, dialog, 1);
    }

    process_lvgl(120);
    REQUIRE(ModalStack::instance().stack_empty());
    REQUIRE(ui_census() == baseline);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "InfoQrModal stress: reshow while the previous delete is in flight",
                 "[info_qr_modal][stress][.ui_integration]") {
    const size_t baseline = ui_census();
    const int iterations = env_iterations(200);
    spdlog::info("[info_qr-stress/reshow] {} iterations", iterations);

    for (int i = 0; i < iterations; ++i) {
        lv_obj_t* previous = ModalStack::instance().top_dialog();
        if (previous != nullptr) {
            lv_obj_update_layout(previous);
            lv_refr_now(nullptr);
            Modal::hide(previous);
        }
        show_one(); // built while the old dialog's async delete is still queued
        process_lvgl(1);
    }

    Modal::hide(ModalStack::instance().top_dialog());
    process_lvgl(120);
    REQUIRE(ModalStack::instance().stack_empty());
    REQUIRE(ui_census() == baseline);
}

TEST_CASE_METHOD(LVGLUITestFixture, "InfoQrModal stress: burst queued before a single drain",
                 "[info_qr_modal][stress][.ui_integration]") {
    const size_t baseline = ui_census();
    const int bursts = env_iterations(40);
    const int pairs_per_burst = 8;
    spdlog::info("[info_qr-stress/burst] {} bursts x {} pairs", bursts, pairs_per_burst);

    for (int b = 0; b < bursts; ++b) {
        for (int p = 0; p < pairs_per_burst; ++p) {
            lv_obj_t* dialog = show_one();
            lv_obj_update_layout(dialog);
            lv_refr_now(nullptr);
            Modal::hide(dialog); // no ticks: deletes pile up on the
                                 // async list like they do on a
                                 // slow board's long tick
        }
        process_lvgl(1);
    }

    process_lvgl(120);
    REQUIRE(ModalStack::instance().stack_empty());
    REQUIRE(ui_census() == baseline);
}

TEST_CASE_METHOD(LVGLUITestFixture, "InfoQrModal stress: click-driven hide via btn_ok",
                 "[info_qr_modal][stress][.ui_integration]") {
    const size_t baseline = ui_census();
    const int iterations = env_iterations(150);
    spdlog::info("[info_qr-stress/click] {} iterations", iterations);

    for (int i = 0; i < iterations; ++i) {
        lv_obj_t* dialog = show_one();
        lv_obj_t* btn_ok = lv_obj_find_by_name(dialog, "btn_ok");
        REQUIRE(btn_ok != nullptr);
        lv_obj_update_layout(dialog);
        lv_refr_now(nullptr);
        lv_obj_send_event(btn_ok, LV_EVENT_CLICKED, nullptr);
        process_lvgl(4 + (i % 5));
    }

    process_lvgl(120);
    REQUIRE(ModalStack::instance().stack_empty());
    REQUIRE(ui_census() == baseline);
}
