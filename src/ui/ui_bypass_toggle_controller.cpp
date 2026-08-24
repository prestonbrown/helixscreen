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
    // allows implicit chaining, enable on UNLOADING->IDLE, disarm on ERROR.
    if (should_unload_before_bypass(info, backend->allows_implicit_chaining())) {
        spdlog::info("[BypassToggle] Unloading slot {} before enabling bypass", info.current_slot);
        pending_bypass_enable_ = true;
        // Subscribe BEFORE starting the unload: the backend flips the action
        // to UNLOADING as the op dispatches, and a later subscribe would miss
        // that edge and never see prev==UNLOADING.
        arm_action_observer();
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

bool BypassToggleController::on_ams_action_changed(AmsAction prev, AmsAction next) {
    // The pending flag is armed by the unload we started, so it must be
    // disarmed by whichever way that unload ends. Clearing only on IDLE left
    // a failed unload's flag set, and the next unrelated unload completion
    // then enabled bypass out of nowhere. Only IDLE actually chains.
    if (!pending_bypass_enable_ || prev != AmsAction::UNLOADING ||
        (next != AmsAction::IDLE && next != AmsAction::ERROR)) {
        return false;
    }
    pending_bypass_enable_ = false;
    disarm_action_observer();
    if (next == AmsAction::ERROR) {
        spdlog::warn("[BypassToggle] Unload failed — cancelling pending bypass enable");
        return true;
    }
    spdlog::info("[BypassToggle] Unload complete — enabling bypass");
    if (AmsBackend* backend = AmsState::instance().get_backend()) {
        enable_now(backend);
    }
    return true;
}

void BypassToggleController::cancel_pending() {
    pending_bypass_enable_ = false;
    disarm_action_observer();
}

void BypassToggleController::arm_action_observer() {
    if (action_observer_) {
        return;
    }
    auto& ams = AmsState::instance();
    lv_subject_t* subject = ams.get_ams_action_subject();
    if (!subject) {
        return;
    }
    // Seed prev from the live subject so the first observed edge is computed
    // against what the subject actually holds right now (IDLE before our
    // unload dispatches, or already UNLOADING if the flip raced us).
    prev_action_ = static_cast<AmsAction>(lv_subject_get_int(subject));
    // observe_int_sync defers the handler through ui_queue_update(), so the
    // guard mutation on settle below never runs inside lv_subject_notify
    // (issue #82 discipline). AmsState subjects fire on the main thread.
    action_observer_ = observe_int_sync<BypassToggleController>(
        subject, this,
        [](BypassToggleController* self, int action_int) {
            const AmsAction next = static_cast<AmsAction>(action_int);
            self->on_ams_action_changed(self->prev_action_, next);
            self->prev_action_ = next;
        },
        ams.get_subjects_lifetime());
}

void BypassToggleController::disarm_action_observer() {
    // [L085] reset(), never release(): the observer must come off the
    // subject so a settled controller is not pinged forever.
    action_observer_.reset();
}

} // namespace helix::ui
