// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_device_ops_bypass_guard.cpp
 * @brief The Device Operations bypass switch runs the shared toggle policy.
 *
 * Run with: ./build/bin/helix-tests "[ams][bypass-device-ops]"
 *
 * Three surfaces flip bypass: the AMS sidebar toggle, the home Bypass tile, and
 * the switch on this overlay. The first two delegate to BypassToggleController;
 * this one called enable_bypass()/disable_bypass() straight from its event
 * handler, so it had neither the print guard nor the unload-first chain. On a
 * backend with no filament-loaded refusal of its own (AD5X IFS) that meant a tap
 * mid-print reached the firmware, and on every backend it meant enabling bypass
 * with a lane loaded stranded that filament behind the external feed.
 *
 * Each case sends LV_EVENT_VALUE_CHANGED at the switch rather than clicking it:
 * that is what a tap produces once lv_switch has already flipped its own CHECKED
 * state, and it is also what `helix-screen ctl click` produces — ctl reaches the
 * handler even on a disabled widget, so the handler-side guard has to hold on
 * its own, not just the binding.
 */

#include "ui_ams_device_operations_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::get_ams_device_operations_overlay;

namespace {

/// Build the overlay through show() (the production path: create, refresh from
/// the backend, register with NavigationManager, push) and hand back the switch.
///
/// The overlay is a process-lifetime singleton whose widgets belong to whichever
/// test screen built it, so every case drops the previous instance first — same
/// discipline as test_ams_env_overlay_unit_binding.cpp.
class DeviceOpsBypassFixture : public LVGLUITestFixture {
  public:
    AmsBackendMock* backend = nullptr;

    DeviceOpsBypassFixture() {
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::UpdateQueue::instance().drain();

        auto& ps = state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        helix::test::set_wire_state(ps, PrintJobState::STANDBY);
        ps.set_print_start_state(PrintStartPhase::IDLE, "", 0);

        // deinit first: init_subjects() early-returns when a previous case left
        // the singleton initialized, and the ams_* names would never reach the
        // XML scope this overlay binds against. deinit_subjects() also destroys
        // the installed backend, so the mock goes in between the two calls.
        auto& ams = AmsState::instance();
        ams.deinit_subjects();

        auto owned = std::make_unique<AmsBackendMock>(4);
        backend = owned.get();
        backend->set_operation_delay(0);
        REQUIRE(backend->start().success());
        ams.set_backend(std::move(owned));

        ams.init_subjects(true);
        ams.sync_from_backend();

        get_ams_device_operations_overlay().init_subjects();
        settle();
    }

    ~DeviceOpsBypassFixture() override {
        NavigationManager::instance().go_back();
        settle();
        if (backend) {
            backend->wait_for_operation_thread();
        }
        settle();
        StaticPanelRegistry::instance().destroy_all();
        settle();
        AmsState::instance().set_backend(nullptr);
    }

    void settle() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
        process_lvgl(10);
    }

    /// Open the overlay and return its bypass switch.
    lv_obj_t* show_and_find_toggle() {
        auto& overlay = get_ams_device_operations_overlay();
        overlay.show(test_screen());
        settle();
        lv_obj_t* root = overlay.get_root();
        REQUIRE(root != nullptr);
        lv_obj_t* toggle = lv_obj_find_by_name(root, "bypass_toggle");
        REQUIRE(toggle != nullptr);
        return toggle;
    }

    /// What a finger produces: lv_switch flips its own CHECKED state, then the
    /// handler runs off the resulting value_changed.
    void tap(lv_obj_t* toggle) {
        if (lv_obj_has_state(toggle, LV_STATE_CHECKED)) {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        }
        lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
        settle();
    }

    /// Drain until `pred` holds, bounded. The unload->enable chain crosses the
    /// mock's operation thread, an AmsState event sync and a deferred observer,
    /// so the number of drains it needs is an implementation detail, not
    /// something a test should hard-code.
    template <typename Pred> bool settle_until(Pred pred) {
        for (int i = 0; i < 20 && !pred(); ++i) {
            settle();
        }
        return pred();
    }

    void set_printing() {
        helix::test::set_wire_state(state(), PrintJobState::PRINTING);
        settle();
    }
};

} // namespace

TEST_CASE_METHOD(DeviceOpsBypassFixture, "Device Operations bypass switch dims while printing",
                 "[ui_integration][ams][bypass-device-ops]") {
    lv_obj_t* toggle = show_and_find_toggle();
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_DISABLED));

    set_printing();
    // The binding half of the guard, the same one panel_widget_bypass.xml
    // carries. A job owns the toolhead: the switch must refuse the finger.
    CHECK(lv_obj_has_state(toggle, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(DeviceOpsBypassFixture,
                 "Device Operations bypass switch refuses to reach the backend mid-print",
                 "[ui_integration][ams][bypass-device-ops]") {
    lv_obj_t* toggle = show_and_find_toggle();
    REQUIRE_FALSE(backend->is_bypass_active());

    set_printing();
    tap(toggle);

    // THE REGRESSION ASSERTION: the handler ran its own print guard, so nothing
    // reached the firmware. Pre-fix this handler called enable_bypass() directly.
    CHECK_FALSE(backend->is_bypass_active());
    // ...and the switch does not sit there claiming bypass is on.
    CHECK_FALSE(lv_obj_has_state(toggle, LV_STATE_CHECKED));
}

TEST_CASE_METHOD(DeviceOpsBypassFixture,
                 "Device Operations bypass switch unloads the lane before enabling",
                 "[ui_integration][ams][bypass-device-ops]") {
    // The mock boots with slot 0 loaded, and its enable_bypass() has no
    // filament-loaded refusal of its own — the same shape as AD5X IFS. Pre-fix
    // the tap below enabled bypass on the spot and left this lane's filament
    // stranded behind the external feed, which is what the #1229 chaining
    // discipline exists to prevent.
    REQUIRE(backend->get_system_info().filament_loaded);
    REQUIRE(backend->get_slot_info(0).status == SlotStatus::LOADED);

    lv_obj_t* toggle = show_and_find_toggle();
    tap(toggle);

    backend->wait_for_operation_thread();
    settle();

    // THE REGRESSION ASSERTION: the lane was unloaded on the way in. A direct
    // enable_bypass() leaves slot 0 LOADED while current_slot flips to -2 —
    // filament in a lane the firmware no longer feeds from.
    CHECK(backend->get_slot_info(0).status == SlotStatus::AVAILABLE);

    // ...and the chain still finishes the job the user asked for, driven by the
    // controller's own ams_action observer off the real backend events.
    CHECK(settle_until([this] { return backend->is_bypass_active(); }));
}
