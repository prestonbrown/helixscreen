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
#include "observer_factory.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>
#include <spdlog/fmt/fmt.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::get_ams_device_operations_overlay;

namespace {

/// AmsBackendMock whose operation thread parks at its first event until the
/// test opens the gate.
///
/// Every backend event becomes a queued sync that re-reads the backend's
/// CURRENT state, so at operation delay 0 whether the main thread's first sync
/// reads UNLOADING or already IDLE is a scheduling outcome. The gate pins it:
/// parking the operation thread makes the mid-unload sync a fact of the test
/// rather than of the box it runs on, so the arming-edge assertion below is
/// asserting something real. Events emitted on the owning thread pass straight
/// through: the unload dispatch and the enable both emit there.
class GatedBackendMock : public AmsBackendMock {
  public:
    explicit GatedBackendMock(int slot_count) : AmsBackendMock(slot_count) {}

    ~GatedBackendMock() override {
        // The wrapper below captures this object; nothing may still be parked
        // in it when the base destructor joins the thread.
        open_gate();
        wait_for_operation_thread();
    }

    void set_event_callback(EventCallback callback) override {
        AmsBackendMock::set_event_callback(
            [this, cb = std::move(callback)](const std::string& event, const std::string& data) {
                wait_at_gate();
                if (cb) {
                    cb(event, data);
                }
            });
    }

    void close_gate() {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        gate_open_ = false;
    }

    void open_gate() {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            gate_open_ = true;
        }
        gate_cv_.notify_all();
    }

  private:
    void wait_at_gate() {
        if (std::this_thread::get_id() == owner_thread_) {
            return;
        }
        std::unique_lock<std::mutex> lock(gate_mutex_);
        gate_cv_.wait(lock, [this] { return gate_open_; });
    }

    std::thread::id owner_thread_ = std::this_thread::get_id();
    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    bool gate_open_ = true;
};

/// Build the overlay through show() (the production path: create, refresh from
/// the backend, register with NavigationManager, push) and hand back the switch.
///
/// The overlay is a process-lifetime singleton whose widgets belong to whichever
/// test screen built it, so every case drops the previous instance first — same
/// discipline as test_ams_env_overlay_unit_binding.cpp.
class DeviceOpsBypassFixture : public LVGLUITestFixture {
  public:
    GatedBackendMock* backend = nullptr;

    /// Every value the ams_action subject published since the overlay came up,
    /// in order. When the enable never lands, this sequence says how far the
    /// operation actually got — an empty list at expiry means the backend
    /// published nothing and no amount of waiting helps.
    std::vector<int> published_actions;
    ObserverGuard action_recorder_;

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

        auto owned = std::make_unique<GatedBackendMock>(4);
        backend = owned.get();
        backend->set_operation_delay(0);
        REQUIRE(backend->start().success());
        ams.set_backend(std::move(owned));

        ams.init_subjects(true);
        ams.sync_from_backend();

        get_ams_device_operations_overlay().init_subjects();
        settle();

        action_recorder_ = helix::ui::observe_int_immediate<DeviceOpsBypassFixture>(
            ams.get_ams_action_subject(), this,
            [](DeviceOpsBypassFixture* self, int action) {
                self->published_actions.push_back(action);
            },
            ams.get_subjects_lifetime());
        // Subscribing publishes the current value once; only edges matter.
        published_actions.clear();
    }

    ~DeviceOpsBypassFixture() override {
        action_recorder_.reset();
        NavigationManager::instance().go_back();
        settle();
        if (backend) {
            backend->open_gate();
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

    /// Wait until `pred` holds. The unload->enable chain crosses the mock's
    /// operation thread, an AmsState event sync and a deferred observer, so the
    /// number of drains it needs is an implementation detail, not something a
    /// test should hard-code. wait_until() sleeps real time between passes so a
    /// background thread can run; settle() never does (process_lvgl only sleeps
    /// above 50ms), so it cannot wait for one.
    template <typename Pred> bool settle_until(Pred pred) {
        return wait_until([&] {
            helix::ui::UpdateQueue::instance().drain();
            return pred();
        });
    }

    /// What the unload->enable chain looks like right now, for the failure
    /// message when the enable never lands.
    std::string chain_state() const {
        const AmsSystemInfo info = backend->get_system_info();
        std::string edges;
        for (int a : published_actions) {
            edges += fmt::format("{}{}", edges.empty() ? "" : ",",
                                 ams_action_to_string(static_cast<AmsAction>(a)));
        }
        return fmt::format("bypass_active={} backend.action={} backend.current_slot={} "
                           "backend.filament_loaded={} ams_action_subject={} "
                           "published_actions=[{}]",
                           backend->is_bypass_active(), ams_action_to_string(info.action),
                           info.current_slot, info.filament_loaded,
                           ams_action_to_string(static_cast<AmsAction>(
                               lv_subject_get_int(AmsState::instance().get_ams_action_subject()))),
                           edges);
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
    backend->close_gate();
    tap(toggle);

    // The handler syncs AmsState right after dispatching the unload; with the
    // operation thread parked, that sync can only read UNLOADING. This pins the
    // gate itself: without it the assertion below is a coin toss.
    REQUIRE(lv_subject_get_int(AmsState::instance().get_ams_action_subject()) ==
            static_cast<int>(AmsAction::UNLOADING));

    backend->open_gate();
    backend->wait_for_operation_thread();
    settle();

    // THE REGRESSION ASSERTION: the lane was unloaded on the way in. A direct
    // enable_bypass() leaves slot 0 LOADED while current_slot flips to -2 —
    // filament in a lane the firmware no longer feeds from.
    CHECK(backend->get_slot_info(0).status == SlotStatus::AVAILABLE);

    // ...and the chain still finishes the job the user asked for, driven by the
    // controller's own observer off the real backend events.
    const bool enabled = settle_until([this] { return backend->is_bypass_active(); });
    INFO("chain at expiry: " << chain_state());
    CHECK(enabled);
}
