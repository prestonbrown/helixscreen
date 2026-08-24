// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_power_panel_print_lock.cpp
 * @brief locked_while_printing must cover the pre-print window too.
 *
 * Run with: ./build/bin/helix-tests "[power][print_state]"
 *
 * A device flagged locked_while_printing is usually the printer's own PSU or a
 * chamber heater. PowerPanel decided the lock from print_stats.state, which
 * reads standby for the whole of a host-side pre-print block - so the toggle
 * was live while the toolhead was homing and probing, and cutting the PSU there
 * is exactly what the flag exists to prevent.
 *
 * The row's lock is a snapshot taken at build time (there is no observer on it),
 * so the assertion is on a freshly built row - which is also how a user reaches
 * it: they open the panel during the window.
 */

#include "ui_panel_power.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/power_panel_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "moonraker_types.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::PrintStartPhase;

namespace {

class PowerPanelLockFixture : public LVGLUITestFixture {
  public:
    PowerPanelLockFixture() {
        // LVGLUITestFixture registers every production XML component, which
        // power_panel needs: it extends overlay_panel and pulls in setting_group,
        // the section headers and power_device_row.
        auto& ps = state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
        ps.set_print_start_state(PrintStartPhase::IDLE, "", 0);
        settle();
    }

    ~PowerPanelLockFixture() override {
        auto& ps = state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        ps.set_print_start_state(PrintStartPhase::IDLE, "", 0);
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
        settle();
    }

    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// Build the panel with a null API so setup()'s device fetch is a no-op and
    /// the rows come from the test's own list.
    lv_obj_t* build(PowerPanel& panel) {
        // Subjects before lv_xml_create (L004): power_panel binds power_status.
        panel.init_subjects();
        auto* obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "power_panel", nullptr));
        REQUIRE(obj != nullptr);
        panel.setup(obj, test_screen());
        lv_obj_update_layout(test_screen());
        return obj;
    }

    /// Tear the widget tree down before the panel, so nothing observes a
    /// subject the panel's destructor is about to deinit.
    ///
    /// Drain first, belt-and-braces. This used to be load-bearing: the deferred
    /// chip rebuild read chip_container_ raw and nothing nulled it when the tree
    /// died, so draining after the delete dereferenced freed memory. Fixed on
    /// main by 13db7c92e (PowerPanel drops its cached widget pointers on tree
    /// delete), so the ordering is now belt-and-braces rather than a workaround.
    void teardown(PowerPanel& panel, lv_obj_t* obj) {
        settle();
        process_lvgl(20);
        lv_obj_delete(obj);
        settle();
        process_lvgl(20);
        panel.deinit_subjects();
    }

    /// Is the freshly built row's toggle refusing input?
    static bool toggle_disabled(lv_obj_t* panel_obj) {
        lv_obj_t* toggle = lv_obj_find_by_name(panel_obj, "device_toggle");
        REQUIRE(toggle != nullptr);
        return lv_obj_has_state(toggle, LV_STATE_DISABLED);
    }

    static std::vector<PowerDevice> one_locked_device() {
        PowerDevice d;
        d.device = "printer";
        d.type = "gpio";
        d.status = "on";
        d.locked_while_printing = true;
        return {d};
    }
};

} // namespace

TEST_CASE_METHOD(PowerPanelLockFixture, "Power device toggle is live when nothing is printing",
                 "[power][print_state]") {
    // Non-vacuity baseline: a test that only ever saw DISABLED would pass with
    // the lock hard-coded true.
    PowerPanel panel(state(), nullptr);
    lv_obj_t* obj = build(panel);

    helix::ui::PowerPanelTestAccess::populate_device_list(panel, one_locked_device());
    lv_obj_update_layout(test_screen());

    CHECK_FALSE(toggle_disabled(obj));

    teardown(panel, obj);
}

TEST_CASE_METHOD(PowerPanelLockFixture, "Power device toggle locks while a print runs",
                 "[power][print_state]") {
    auto& ps = state();

    SECTION("printing") {
        helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    }
    SECTION("paused") {
        helix::test::set_wire_state(ps, PrintJobState::PAUSED);
    }
    settle();

    PowerPanel panel(state(), nullptr);
    lv_obj_t* obj = build(panel);

    helix::ui::PowerPanelTestAccess::populate_device_list(panel, one_locked_device());
    lv_obj_update_layout(test_screen());

    CHECK(toggle_disabled(obj));

    teardown(panel, obj);
}

TEST_CASE_METHOD(PowerPanelLockFixture,
                 "Power device toggle locks during a host-side pre-print block",
                 "[power][print_state]") {
    // THE BUG. print_stats still says standby while the app runs the user's own
    // pre-start block, so the PSU toggle stayed live through homing and probing.
    auto& ps = state();
    ps.begin_preparing(helix::PrintJobRef{"chosen.gcode", "", ""});
    ps.set_print_start_state(PrintStartPhase::HOMING, "", 0);
    settle();
    REQUIRE(ps.get_print_job_state() == PrintJobState::STANDBY);
    REQUIRE(ps.get_print_lifecycle() == PrintState::Preparing);

    PowerPanel panel(state(), nullptr);
    lv_obj_t* obj = build(panel);

    helix::ui::PowerPanelTestAccess::populate_device_list(panel, one_locked_device());
    lv_obj_update_layout(test_screen());

    CHECK(toggle_disabled(obj));

    teardown(panel, obj);
}

TEST_CASE_METHOD(PowerPanelLockFixture, "An unlocked device stays togglable mid-print",
                 "[power][print_state]") {
    // The flag is what gates the lock, not the print state on its own.
    auto& ps = state();
    helix::test::set_wire_state(ps, PrintJobState::PRINTING);
    settle();

    PowerPanel panel(state(), nullptr);
    lv_obj_t* obj = build(panel);

    auto devices = one_locked_device();
    devices[0].locked_while_printing = false;
    helix::ui::PowerPanelTestAccess::populate_device_list(panel, devices);
    lv_obj_update_layout(test_screen());

    CHECK_FALSE(toggle_disabled(obj));

    teardown(panel, obj);
}
