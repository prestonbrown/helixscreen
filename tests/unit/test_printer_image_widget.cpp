// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "misc/lv_timer_private.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/printer_image_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Drain LVGL's one-shot timer queue (lv_async_call + our deferral timers) by
/// calling each ready one-shot timer's callback once, repeating until none
/// fire. Mirrors process_async_calls() in test_panel_widget_manager.cpp — a
/// fixed process_lvgl(ms) elapse does not reliably fire period-0 one-shot
/// timers created mid-tick, so we pump them explicitly.
void process_async_calls() {
    for (int safety = 0; safety < 50; ++safety) {
        bool fired = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && t->timer_cb) {
                t->timer_cb(t);
                fired = true;
                break;
            }
            t = next;
        }
        if (!fired)
            break;
    }
}

} // namespace

// Regression test for #1025 (grid walk-off crash family, #983). PrinterImageWidget
// forced a SYNCHRONOUS LVGL layout from inside attach() -> reload_from_config() ->
// refresh_printer_image() -> lv_image_set_inner_align()/update_align ->
// lv_obj_update_layout, which cascades into the parent grid's grid_update. When
// that runs against a mid-rebuild grid, count_tracks walks the freed descriptor off
// the heap end (SIGSEGV on v0.99.75). The v0.99.76 LV_LAYOUT_NONE window in
// panel_widget_manager.cpp prevents the crash, but the widget forcing synchronous
// layout from the rebuild path is the underlying fragility. PrintStatusWidget
// already established the safe idiom: defer image/layout work to a later tick.
//
// Invariant: the image src must NOT be set synchronously inside attach(); the
// refresh is deferred to a one-shot timer. This FAILS pre-fix (src set
// synchronously in attach) and PASSES post-fix (src still null right after
// attach, set only after a tick). Attached into a PLAIN lv_obj (not a grid) so
// the pre-fix synchronous path fails on the assertion, not a SIGSEGV.
TEST_CASE_METHOD(XMLTestFixture, "PrinterImageWidget defers image refresh out of attach() (#1025)",
                 "[panel_widget][printer_image][regression]") {
    // PrinterImageWidget's subjects (printer_type_text, printer_host_text,
    // printer_info_visible) must exist before reload_from_config touches them.
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    // Build the widget tree by hand. attach() looks up the child image by name
    // ("printer_image"), so no XML component is needed. A plain lv_obj container
    // (NOT a grid) means the pre-fix synchronous layout cannot SIGSEGV — it just
    // sets the src early, tripping the assertion below.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 200, 200);
    lv_obj_t* widget_obj = lv_obj_create(container);
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    process_lvgl(2);

    // Baseline: no source before attach.
    REQUIRE(lv_image_get_src(img) == nullptr);

    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());

    // KEY REGRESSION ASSERTION: the refresh must be DEFERRED. Immediately after
    // attach() the image source is still null — refresh_printer_image() (which
    // calls lv_image_set_src + lv_image_set_inner_align, forcing a synchronous
    // layout) has been scheduled on a one-shot timer, not run inline. Pre-fix this
    // FAILS (src set synchronously inside attach); post-fix it PASSES.
    INFO("attach() must not set the image src synchronously — refresh_printer_image() "
         "forces lv_obj_update_layout and must be deferred off the rebuild path (#1025/#983)");
    REQUIRE(lv_image_get_src(img) == nullptr);

    // Fire the deferred refresh timer and let layout settle.
    process_async_calls();
    process_lvgl(5);

    // After a tick the deferred refresh ran and applied a source image.
    REQUIRE(lv_image_get_src(img) != nullptr);

    SECTION("on_activate() also defers the refresh") {
        // After the first refresh resolved, on_activate() re-runs reload_from_config().
        // It must not crash and must (re)apply the source via the deferred timer.
        w.on_activate();
        // Drain any pending deferred refresh + cache timers from on_activate().
        process_async_calls();
        process_lvgl(5);
        REQUIRE(lv_image_get_src(img) != nullptr);
    }

    w.detach();
}

// A printer added by address has no name or type until it connects; Home names it by its host.
TEST_CASE_METHOD(XMLTestFixture,
                 "PrinterImageWidget labels a printer not yet identified by its host",
                 "[panel_widget][printer_image][multi-printer]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    helix::Config* cfg = helix::Config::get_instance();
    REQUIRE(cfg != nullptr);
    const nlohmann::json saved = helix::ConfigTestAccess::data(*cfg);
    const std::string saved_active = helix::ConfigTestAccess::active_printer_id(*cfg);
    nlohmann::json data = saved;
    data["printers"]["printer-2"] = {{"moonraker_host", "10.0.0.9"}};
    helix::ConfigTestAccess::data(*cfg) = data;
    helix::ConfigTestAccess::active_printer_id(*cfg) = "printer-2";

    lv_obj_t* widget_obj = lv_obj_create(test_screen());
    lv_obj_t* img = lv_image_create(widget_obj);
    lv_obj_set_name(img, "printer_image");
    helix::PrinterImageWidget w;
    w.attach(widget_obj, test_screen());

    lv_subject_t* label = lv_xml_get_subject(nullptr, "printer_type_text");
    REQUIRE(label != nullptr);
    CHECK(std::string(lv_subject_get_string(label)) == "10.0.0.9");

    w.detach();
    process_async_calls();
    helix::ConfigTestAccess::data(*cfg) = saved;
    helix::ConfigTestAccess::active_printer_id(*cfg) = saved_active;
}
