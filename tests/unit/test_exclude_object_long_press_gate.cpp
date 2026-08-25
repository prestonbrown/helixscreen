// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_exclude_object_long_press_gate.cpp
 * @brief PrintExcludeObjectManager::handle_object_long_press() entry guards
 *
 * Run with: ./build/bin/helix-tests "[exclude_object][long_press]"
 *
 * The four rejections in front of the confirmation modal, driven against the
 * real manager (src/ui/ui_print_exclude_object_manager.cpp:139-199):
 *
 *   1. empty / null object name          — long-press landed on bare bed
 *   2. defined_objects is empty          — [exclude_object] unconfigured, or the
 *                                          slicer emitted no EXCLUDE_OBJECT_DEFINE
 *                                          markers, so the gcode would be a silent
 *                                          no-op on the printer
 *   3. object is already excluded
 *   4. another exclusion is already pending
 *
 * Guard 2 is the one with teeth: without it the user gets a confirmation dialog,
 * a 5s undo toast, and a red object in the viewer for an EXCLUDE_OBJECT Klipper
 * will ignore.
 *
 * The confirmation/undo/RPC half of the flow — awaiting-confirmation bookkeeping,
 * status-subscription promotion, the print-state watchdog — is covered in
 * test_print_exclude_object_manager.cpp.
 */

#include "../../include/moonraker_api.h"
#include "../../include/printer_state.h"
#include "../../include/ui_print_exclude_object_manager.h"
#include "../../include/ui_update_queue.h"
#include "../lvgl_ui_test_fixture.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

/// Real manager over the fixture's PrinterState/API. gcode_viewer is nullptr —
/// every viewer-touching branch is guarded by `if (gcode_viewer_)`, which keeps
/// the test out of the widget graph without changing any decision under test.
struct LongPressHarness {
    LVGLUITestFixture& fx;
    std::unique_ptr<PrintExcludeObjectManager> manager;

    explicit LongPressHarness(LVGLUITestFixture& f) : fx(f) {
        manager = std::make_unique<PrintExcludeObjectManager>(fx.api(), fx.state(), nullptr);
        manager->init();
        // observe_int_sync defers its initial-value callback through UpdateQueue;
        // drain it so the first real drain isn't carrying a STANDBY fire-on-subscribe.
        UpdateQueue::instance().drain();
    }

    ~LongPressHarness() {
        manager->clear_excluded_objects();
        manager->deinit();
        manager.reset();
        // state() is the GLOBAL PrinterState, and the defined/excluded object sets
        // are plain members that init_subjects() does not touch — so anything left
        // here survives into the next test (and into unrelated ones), where a
        // manager syncs it on init and silently rejects a long-press.
        fx.state().set_excluded_objects({});
        fx.state().get_excluded_objects_state()->set_defined_objects({});
        UpdateQueue::instance().drain();
    }

    /// Populate what the slicer's EXCLUDE_OBJECT_DEFINE markers would have set.
    void define_objects(const std::vector<std::string>& names) {
        fx.state().get_excluded_objects_state()->set_defined_objects(names);
        UpdateQueue::instance().drain();
    }

    /// Push Klipper's exclude_object.excluded_objects the way a status update does.
    void klipper_excluded(const std::unordered_set<std::string>& names) {
        fx.state().set_excluded_objects(names);
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "long-press with no defined objects never opens the dialog",
                 "[exclude_object][long_press]") {
    LongPressHarness h(*this);
    // No EXCLUDE_OBJECT_DEFINE markers: Klipper would ignore the gcode entirely.
    h.define_objects({});

    h.manager->handle_object_long_press("Part_1");

    CHECK(h.manager->get_pending_object().empty());
    CHECK_FALSE(h.manager->has_pending_timer());
}

TEST_CASE_METHOD(LVGLUITestFixture, "long-press on a defined object opens the dialog",
                 "[exclude_object][long_press]") {
    LongPressHarness h(*this);
    h.define_objects({"Part_1", "Part_2"});

    h.manager->handle_object_long_press("Part_1");
    process_lvgl(20);

    // Pending survives only if the modal actually came up — handle_object_long_press
    // clears it when show() fails, so this also proves the dialog opened.
    CHECK(h.manager->get_pending_object() == "Part_1");
    // Still awaiting the user: nothing is excluded and no undo window is running.
    CHECK(h.manager->get_excluded_objects().empty());
    CHECK_FALSE(h.manager->has_pending_timer());
}

TEST_CASE_METHOD(LVGLUITestFixture, "long-press on empty ground is ignored",
                 "[exclude_object][long_press]") {
    LongPressHarness h(*this);
    h.define_objects({"Part_1"});

    SECTION("empty name") {
        h.manager->handle_object_long_press("");
        CHECK(h.manager->get_pending_object().empty());
    }

    SECTION("null name") {
        h.manager->handle_object_long_press(nullptr);
        CHECK(h.manager->get_pending_object().empty());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "long-press on an already-excluded object is ignored",
                 "[exclude_object][long_press]") {
    LongPressHarness h(*this);
    h.define_objects({"Part_1", "Part_2"});
    // Klipper confirms Part_1 is already out of the print.
    h.klipper_excluded({"Part_1"});
    REQUIRE(h.manager->get_excluded_objects().count("Part_1") == 1);

    h.manager->handle_object_long_press("Part_1");
    process_lvgl(20);

    CHECK(h.manager->get_pending_object().empty());

    // The neighbouring object is still excludable — the guard is per-object, not
    // a blanket "something is excluded" latch.
    h.manager->handle_object_long_press("Part_2");
    process_lvgl(20);
    CHECK(h.manager->get_pending_object() == "Part_2");
}

TEST_CASE_METHOD(LVGLUITestFixture, "a second long-press does not displace the pending one",
                 "[exclude_object][long_press]") {
    LongPressHarness h(*this);
    h.define_objects({"Part_1", "Part_2"});

    h.manager->handle_object_long_press("Part_1");
    process_lvgl(20);
    REQUIRE(h.manager->get_pending_object() == "Part_1");

    // Second press while the first dialog is still up. Overwriting here would
    // leave the visible dialog naming Part_1 while the confirm excludes Part_2.
    h.manager->handle_object_long_press("Part_2");
    process_lvgl(20);

    CHECK(h.manager->get_pending_object() == "Part_1");
}
