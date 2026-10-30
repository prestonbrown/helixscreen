// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_bypass_toggle_controller.h"

#include "ui_error_reporting.h"

#include "ams_bypass_policy.h"
#include "ams_state.h"
#include "app_globals.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

BypassToggleController::~BypassToggleController() {
    cancel_pending();
}

namespace {

/// The static half of "may this machine's bypass be driven at all", toasting the
/// refusal. bypass_toggle_offered() is the rule; both the disable path in
/// toggle() and the enable path in begin_engage() ask it here so neither can
/// answer it differently from the surfaces that decide whether to draw a toggle.
bool bypass_toggle_allowed(const AmsSystemInfo& info) {
    if (helix::bypass_toggle_offered(helix::bypass_available_for(info.supports_bypass),
                                     info.has_hardware_bypass_sensor)) {
        return true;
    }
    // Only the sensor case can realistically be reached: a machine with no
    // bypass at all draws no toggle to press.
    NOTIFY_WARNING(lv_tr("Bypass controlled by sensor"));
    spdlog::warn("[BypassToggle] Blocked — bypass not drivable on this machine");
    return false;
}

} // namespace

void BypassToggleController::toggle() {
    spdlog::info("[BypassToggle] Toggle requested");

    // Print guard — fully disabled while a job owns the toolhead. Asked of the
    // lifecycle, not print_stats.state: Preparing counts (a paused print still
    // has filament staged mid-path, and a host-side pre-start block is actively
    // homing and probing). The tile's own binding in panel_widget_bypass.xml
    // greys it on the same subject; this is the handler half of the same guard.
    const PrintState state = get_printer_state().print_state().get_print_lifecycle();
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

    if (!bypass_toggle_allowed(backend->get_system_info())) {
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

    begin_engage();
}

void BypassToggleController::ensure_engaged_then(std::function<void()> on_ready) {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend && backend->is_bypass_active()) {
        // Already on the bypass path: nothing to arm, and nothing to wait for.
        if (on_ready) {
            on_ready();
        }
        return;
    }

    // Armed before begin_engage() because an enable that succeeds without an
    // unload settles inside that call and fires the continuation from there.
    on_ready_ = std::move(on_ready);
    if (!begin_engage()) {
        // Either a guard refused (already toasted) or the unload chain is now
        // running, and poll_pending_engage() owns the continuation from here.
        // Only the refusal case needs clearing, and it is the one where the
        // chain is not armed.
        if (!pending_bypass_enable_) {
            on_ready_ = nullptr;
        }
    }
}

bool BypassToggleController::begin_engage() {
    // Print guard — see toggle(). Repeated here rather than hoisted because
    // ensure_engaged_then() reaches this path without going through toggle().
    const PrintState state = get_printer_state().print_state().get_print_lifecycle();
    if (job_holds_machine(state)) {
        NOTIFY_WARNING(lv_tr("Bypass cannot be changed while printing"));
        spdlog::info("[BypassToggle] Refused — print active ({})", static_cast<int>(state));
        return false;
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        NOTIFY_WARNING(lv_tr("Multi-Filament System not available"));
        return false;
    }

    AmsSystemInfo info = backend->get_system_info();
    if (!bypass_toggle_allowed(info)) {
        return false;
    }

    // #1229 chaining discipline — unload first when the backend allows implicit
    // chaining, then enable once poll_pending_engage() sees the lane clear.
    if (should_unload_before_bypass(info, backend->allows_implicit_chaining())) {
        spdlog::info("[BypassToggle] Unloading slot {} before enabling bypass", info.current_slot);
        pending_bypass_enable_ = true;
        // Subscribe BEFORE starting the unload: a backend that finishes inside
        // the dispatch emits its events from there, and the sync those queue is
        // the revision bump that settles the chain.
        arm_backend_observer();
        AmsError error = backend->unload_active_filament();
        if (error.result != AmsResult::SUCCESS) {
            cancel_pending();
            helix::ui::notify_ams_error(error);
            return false;
        }
        NOTIFY_INFO(lv_tr("Unloading before bypass..."));
        return false;
    }
    return enable_now(backend);
}

bool BypassToggleController::enable_now(AmsBackend* backend) {
    AmsError error = backend->enable_bypass();
    if (error.result == AmsResult::SUCCESS) {
        NOTIFY_INFO(lv_tr("Bypass enabled"));
        fire_on_ready();
        return true;
    }
    helix::ui::notify_ams_error(error, lv_tr("Bypass failed"));
    on_ready_ = nullptr;
    return false;
}

void BypassToggleController::fire_on_ready() {
    // Moved out before the call: the continuation dispatches a filament op that
    // can re-enter this controller, and a still-armed slot would run twice.
    auto ready = std::move(on_ready_);
    on_ready_ = nullptr;
    if (ready) {
        ready();
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
        spdlog::warn("[BypassToggle] Unload failed — cancelling pending bypass enable");
        cancel_pending();
        return true;
    }
    // The same question begin_engage() asked to arm the chain. Still true means
    // the lane the unload has to clear is still loaded — including the window
    // before the dispatched op has started, which is why a bare "backend is
    // idle" test cannot settle the chain on its own.
    if (info.is_busy() || should_unload_before_bypass(info, backend->allows_implicit_chaining())) {
        return false;
    }
    // Cleared before the enable: enable_now() fires the continuation, which can
    // re-enter this controller, and a still-armed chain would settle twice.
    pending_bypass_enable_ = false;
    disarm_backend_observer();
    spdlog::info("[BypassToggle] Lane clear — enabling bypass");
    enable_now(backend);
    return true;
}

void BypassToggleController::cancel_pending() {
    pending_bypass_enable_ = false;
    // The continuation belongs to the chain being abandoned. Leaving it armed
    // would run it against whatever the NEXT engage settles on.
    on_ready_ = nullptr;
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
    // identical to the last one. The value itself is ignored — the handler
    // re-reads the backend.
    //
    // observe<int> defers the handler through ui_queue_update(), so the
    // guard mutation on settle never runs inside lv_subject_notify (issue #82
    // discipline). AmsState subjects fire on the main thread.
    backend_observer_ = observe<int>(
        subject, this, [](BypassToggleController* self, int) { self->poll_pending_engage(); },
        ams.get_subjects_lifetime());
}

void BypassToggleController::disarm_backend_observer() {
    // [L085] reset(), never release(): the observer must come off the
    // subject so a settled controller is not pinged forever.
    backend_observer_.reset();
}

} // namespace helix::ui
