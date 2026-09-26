// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_bypass_toggle_controller.h"

#include "ui_error_reporting.h"

#include "ams_state.h"
#include "app_globals.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

BypassToggleController::~BypassToggleController() {
    cancel_pending();
}

void BypassToggleController::toggle() {
    spdlog::info("[BypassToggle] Toggle requested");

    // Print guard — fully disabled while a job owns the toolhead. Asked of the
    // lifecycle, not print_stats.state: Preparing counts (a paused print still
    // has filament staged mid-path, and a host-side pre-start block is actively
    // homing and probing). The tile's own binding in panel_widget_bypass.xml
    // greys it on the same subject; this is the handler half of the same guard.
    const PrintState state = get_printer_state().get_print_lifecycle();
    if (job_holds_machine(state)) {
        NOTIFY_WARNING(lv_tr("Bypass cannot be changed while printing"));
        spdlog::info("[BypassToggle] Refused — print active ({})", static_cast<int>(state));
        return;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return;
    }

    AmsSystemInfo info = backend->get_system_info();
    if (info.has_hardware_bypass_sensor) {
        NOTIFY_WARNING(lv_tr("Bypass controlled by sensor"));
        spdlog::warn("[BypassToggle] Blocked — hardware sensor controls bypass");
        return;
    }

    if (backend->is_bypass_active()) {
        AmsError error = backend->disable_bypass();
        if (error.result == AmsResult::SUCCESS) {
            NOTIFY_INFO(lv_tr("Bypass disabled"));
        }
        if (error.result != AmsResult::SUCCESS) {
            helix::ui::notify_ams_error(error, lv_tr("Bypass toggle failed"));
        }
        return;
    }

    // Enable path: #1229 chaining discipline — unload first when the backend
    // allows implicit chaining, then enable once poll_pending_engage() sees
    // the lane clear.
    if (should_unload_before_bypass(info, backend->allows_implicit_chaining())) {
        spdlog::info("[BypassToggle] Unloading slot {} before enabling bypass", info.current_slot);
        pending_bypass_enable_ = true;
        // Subscribe BEFORE starting the unload: a backend that finishes inside
        // the dispatch emits its events from there, and the sync those queue is
        // the revision bump that settles the chain.
        arm_backend_observer();
        AmsError error = backend->unload_active_filament();
        if (error.result == AmsResult::SUCCESS) {
            NOTIFY_INFO(lv_tr("Unloading before bypass..."));
        } else {
            cancel_pending();
            helix::ui::notify_ams_error(error);
        }
        return;
    }
    enable_now(backend);
}

void BypassToggleController::enable_now(AmsBackend* backend) {
    AmsError error = backend->enable_bypass();
    if (error.result == AmsResult::SUCCESS) {
        NOTIFY_INFO(lv_tr("Bypass enabled"));
    } else {
        helix::ui::notify_ams_error(error, lv_tr("Bypass failed"));
    }
}

bool BypassToggleController::poll_pending_engage() {
    if (!pending_bypass_enable_) {
        return false;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        // The backend the chain was armed against is gone; nothing can settle it.
        cancel_pending();
        return true;
    }
    const AmsSystemInfo info = backend->get_system_info();
    if (info.action == AmsAction::ERROR) {
        // The chain is armed by the unload we started, so whichever way that
        // unload ends has to disarm it: a chain left armed settles on the next
        // unrelated unload and enables bypass nobody asked for.
        spdlog::warn("[BypassToggle] Unload failed - cancelling pending bypass enable");
        cancel_pending();
        return true;
    }
    // The same question toggle() asked to arm the chain. Still true means
    // the lane the unload has to clear is still loaded - including the window
    // before the dispatched op has started, which is why a bare "backend is
    // idle" test cannot settle the chain on its own.
    if (info.is_busy() || should_unload_before_bypass(info, backend->allows_implicit_chaining())) {
        return false;
    }
    // Cleared before the enable: the sync queued off the enable's own events
    // would settle a still-armed chain a second time.
    pending_bypass_enable_ = false;
    disarm_backend_observer();
    spdlog::info("[BypassToggle] Lane clear - enabling bypass");
    enable_now(backend);
    return true;
}

void BypassToggleController::cancel_pending() {
    pending_bypass_enable_ = false;
    disarm_backend_observer();
}

void BypassToggleController::arm_backend_observer() {
    if (backend_observer_) {
        return;
    }
    auto& ams = AmsState::instance();
    lv_subject_t* subject = ams.get_ams_data_revision_subject();
    if (!subject) {
        return;
    }
    // ams_data_revision rather than ams_action: it is bumped after every
    // backend-event sync, so it notifies even when the state the sync read is
    // identical to the last one. The value itself is ignored - the handler
    // re-reads the backend.
    //
    // observe_int_sync defers the handler through ui_queue_update(), so the
    // guard mutation on settle never runs inside lv_subject_notify (issue #82
    // discipline). AmsState subjects fire on the main thread.
    backend_observer_ = observe_int_sync<BypassToggleController>(
        subject, this, [](BypassToggleController* self, int) { self->poll_pending_engage(); },
        ams.get_subjects_lifetime());
}

void BypassToggleController::disarm_backend_observer() {
    // [L085] reset(), never release(): the observer must come off the
    // subject so a settled controller is not pinged forever.
    backend_observer_.reset();
}

} // namespace helix::ui
