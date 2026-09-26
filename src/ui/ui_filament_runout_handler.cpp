// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_runout_handler.h"

#include "ui_nav_manager.h"
#include "ui_resume_dispatch.h"
#include "ui_update_queue.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_api.h"
#include "observer_factory.h"
#include "print_control_buttons.h"
#include "print_lifecycle_state.h" // For PrintState enum
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix::ui {

// ============================================================================
// FilamentRunoutHandler Implementation
// ============================================================================

FilamentRunoutHandler::FilamentRunoutHandler(IMoonrakerAPI* api) : api_(api) {
    spdlog::debug("[FilamentRunoutHandler] Constructed");
}

FilamentRunoutHandler::~FilamentRunoutHandler() {
    // lifetime_ destructor calls invalidate() automatically
    spdlog::trace("[FilamentRunoutHandler] Destroyed");
}

// ============================================================================
// State Transition Handler
// ============================================================================

void FilamentRunoutHandler::on_print_state_changed(::PrintState old_state, ::PrintState new_state) {
    (void)old_state;

    // Check for runout condition when entering Paused state
    if (new_state == ::PrintState::Paused) {
        check_and_show_runout_guidance();
    }

    // Reset runout modal flag and hide modal on print resume or end
    if (new_state == ::PrintState::Printing || new_state == ::PrintState::Idle ||
        new_state == ::PrintState::Complete || new_state == ::PrintState::Cancelled ||
        new_state == ::PrintState::Error) {
        runout_modal_shown_for_pause_ = false;
        user_took_manual_action_ = false;
        hide_runout_guidance_modal();
    }
}

// ============================================================================
// Runout Detection and Modal Display
// ============================================================================

void FilamentRunoutHandler::check_and_show_runout_guidance() {
    // Only show once per pause event
    if (runout_modal_shown_for_pause_) {
        return;
    }

    // Skip if AMS/MMU present and not forced (runout during swaps is normal)
    if (!get_runtime_config()->should_show_runout_modal()) {
        return;
    }

    auto& sensor_mgr = helix::FilamentSensorManager::instance();

    // Check if a runout sensor on a LOADED lane shows no filament. Use
    // has_real_runout() so an intentionally-empty AMS lane (e.g. head 1 left
    // unloaded for a multi-color print) doesn't pop guidance when the print is
    // paused for an unrelated reason. A loaded lane that lost filament still
    // counts. (Snapmaker U1 false-alarm fix.)
    if (sensor_mgr.has_real_runout()) {
        // Auto-recover-on-pause was previously gated on `motion=false AND
        // port=true` but field testing exposed two failure modes: (a) the
        // "port=true" signal alone doesn't prove filament reached the
        // extruder gear (e.g., Snapmaker assist motor pre-loads to ~4
        // inches short of the toolhead and stops); (b) firmware load
        // macros (AUTO_FEEDING/MANUAL_FEEDING) silently no-op outside an
        // active print, so the recovery chain can't actually move filament
        // either. Net result: silent air-prints. Pulled until we have a
        // verified detection signal AND a recovery path that observably
        // moves filament. Modal-driven Resume (user-initiated) still uses
        // backend->prepare_for_resume.
        spdlog::info(
            "[FilamentRunoutHandler] Runout detected during pause - showing guidance modal");
        show_runout_guidance_modal();
        runout_modal_shown_for_pause_ = true;
    }
}

void FilamentRunoutHandler::show_runout_guidance_modal() {
    if (runout_modal_.is_visible()) {
        // Already showing
        return;
    }

    // Fresh modal: re-arm the sensor-driven auto-close. Any in-dialog
    // Load/Unload/Purge will set this true to suppress auto-close.
    user_took_manual_action_ = false;
    // Re-arm the auto-close latch (#991): the observer must observe a confirmed
    // runout (value==1) on THIS modal before a clear (value==0) can auto-close,
    // so the observer's initial read / startup-grace transient cannot close it.
    runout_confirmed_active_ = false;

    spdlog::info("[FilamentRunoutHandler] Showing runout guidance modal");

    // This one IS a warning — say so rather than inheriting whatever the last
    // surface to show this dialog left behind (the home tile's advisory tap
    // otherwise latches the neutral icon on for the rest of the session).
    runout_modal_.set_advisory(false);

    // Capability-aware layout: backends that feed filament to the nozzle as part
    // of resume (e.g. Snapmaker U1's AUTO_FEEDING) present Resume as the primary
    // action and demote manual Load/Unload/Purge. Set on the main thread (show
    // happens on the main thread), so a direct subject set is safe here.
    {
        AmsBackend* backend = AmsState::instance().get_backend();
        bool autofeed = backend && backend->recovers_filament_on_resume();
        autofeed_context_ = autofeed;
        runout_modal_.set_autofeed_capable(autofeed);
        spdlog::debug("[FilamentRunoutHandler] Runout dialog autofeed_capable={}", autofeed);

        // Gate Resume on first-gate (port) filament presence (#991). On auto-feed
        // backends Resume is disabled until filament is present at the active
        // tool's PORT sensor (AmsState::active_tool_port_present, fed from
        // port_sensor_filament_present_ — NOT the toolhead motion sensor, which
        // stays "runout" until extrusion). On non-auto-feed backends the gate is
        // never applied: clear the block and skip the observer.
        if (autofeed) {
            // Seed the block from the current port value so a stale spool gates
            // immediately on show, then keep it in sync via the observer.
            int present =
                lv_subject_get_int(AmsState::instance().get_active_tool_port_present_subject());
            runout_modal_.set_resume_blocked(present == 0);
            // AmsState frees the subject in deinit_subjects(); hold its lifetime.
            port_present_observer_ = helix::ui::observe_int_sync<FilamentRunoutHandler>(
                AmsState::instance().get_active_tool_port_present_subject(), this,
                [](FilamentRunoutHandler* self, int port_present) {
                    // Only gate while still on an auto-feed runout modal.
                    if (!self->autofeed_context_)
                        return;
                    if (!self->runout_modal_.is_visible())
                        return;
                    self->runout_modal_.set_resume_blocked(port_present == 0);
                },
                AmsState::instance().get_subjects_lifetime());
        } else {
            runout_modal_.set_resume_blocked(false);
        }
    }

    // Capture token for async callback safety
    auto token = lifetime_.token();

    // Configure callbacks for the six options
    runout_modal_.set_on_load_filament([this, token]() {
        // #991 diagnostic: the Load press was observed to close the modal but
        // perform no action. Log entry + token state BEFORE the guard so the
        // next on-device repro disambiguates "callback never ran" vs
        // "token expired and swallowed the action".
        spdlog::info("[FilamentRunoutHandler] Load callback entered (token expired={})",
                     token.expired());
        if (token.expired())
            return;
        user_took_manual_action_ = true; // keep dialog open; suppress auto-close
        dispatch_load();
    });

    runout_modal_.set_on_resume([this, token]() {
        if (token.expired())
            return;

        // No client-side filament-present gate here. The previous
        // has_any_runout() check used the encoder-based motion sensor, which
        // only flips back to "present" after actual extrusion happens — so on
        // a tool-changer like the Snapmaker U1, where the user reloads a spool
        // at the buffer/port and the buffer auto-feeds to within a few inches
        // of the toolhead, the sensor stays in runout state until Klipper's
        // RESUME chain extrudes. Gating on the sensor refused legitimate user
        // intent ("Insert filament before resuming" while a fresh spool was
        // sitting in the buffer). Trust Klipper to enforce — INNER_RESUME has
        // its own CHECK_FILAMENT_RUNOUT, and any rejection now surfaces as a
        // single contextual error toast via the suppress_auto_toast path.

        spdlog::info("[FilamentRunoutHandler] User chose to resume print after runout");

        // Route through the SAME path as the panel's primary Resume button so
        // both buttons produce identical behavior (#991): pending-action UI +
        // start_pending_action(Resuming) + the shared prepare_for_resume →
        // Resume dispatch with clear-on-failure. request_resume() does the
        // macro-empty check and the dispatch itself, so we don't duplicate it
        // here. The AMS backend still gets its prepare_for_resume recovery
        // chance via dispatch_prepared_resume.
        PrintControlButtons::instance().request_resume();
    });

    runout_modal_.set_on_cancel_print([this, token]() {
        if (token.expired())
            return;

        spdlog::info("[FilamentRunoutHandler] User chose to cancel print after runout");
        // Shared with the home Filament tile's paused modal, which offers the
        // same button. Raises the same confirmation the Stop button does and
        // sends nothing until the user accepts — this dialog used to cancel on
        // the first tap, alone among the printer's three cancel affordances.
        //
        // This dialog stays up behind the confirmation so declining returns to
        // it; closing it is the confirmed path's job. Without that the decline
        // stranded the user: check_and_show_runout_guidance() will not re-show
        // until runout_modal_shown_for_pause_ clears on a print-state change.
        helix::ui::dispatch_cancel_print(api_, "[FilamentRunoutHandler]", [this, token]() {
            if (token.expired())
                return;
            hide_runout_guidance_modal();
        });
    });

    runout_modal_.set_on_unload_filament([this, token]() {
        if (token.expired())
            return;
        user_took_manual_action_ = true; // in-dialog action suppresses auto-close
        dispatch_unload();
    });

    runout_modal_.set_on_purge([this, token]() {
        if (token.expired())
            return;
        user_took_manual_action_ = true; // in-dialog action suppresses auto-close
        dispatch_purge();
    });

    runout_modal_.set_on_ok_dismiss([token]() {
        if (token.expired())
            return;
        spdlog::info("[FilamentRunoutHandler] User dismissed runout modal (idle mode)");
        // Just hide the modal - no action needed
    });

    if (!runout_modal_.show(lv_screen_active())) {
        spdlog::error("[FilamentRunoutHandler] Failed to create runout guidance modal");
        return;
    }

    // Auto-close when the runout resolves EXTERNALLY. get_any_runout_subject() is
    // int: 1=runout, 0=clear. observe_int_sync fires its INITIAL read the moment
    // it's installed and again on every change — and the sensor can momentarily
    // read 0 during its startup-grace window (e.g. right after a UI restart),
    // which previously closed the modal immediately (#991). Guards, in order:
    //   - value==1: latch a confirmed active runout for THIS modal (never closes)
    //   - startup-grace window: ignore (sensor not yet stabilized)
    //   - require runout_confirmed_active_: only a genuine confirmed runout→clear
    //     transition observed while this modal is up may auto-close
    //   - !user_took_manual_action_: user managing it in-dialog suppresses close
    // observe_int_sync defers to the main thread; hiding here mirrors the
    // existing on_print_state_changed close path (precedent-safe).
    runout_cleared_observer_ = helix::ui::observe_int_sync<FilamentRunoutHandler>(
        helix::FilamentSensorManager::instance().get_any_runout_subject(), this,
        [](FilamentRunoutHandler* self, int any_runout) {
            if (any_runout != 0) {
                // Confirmed active runout while the modal is up — arm auto-close.
                self->runout_confirmed_active_ = true;
                return;
            }
            // value == 0 (clear): only close on a genuine confirmed runout→clear.
            if (helix::FilamentSensorManager::instance().is_in_startup_grace_period()) {
                // Transient 0 during sensor stabilization — not a real resolution.
                return;
            }
            if (!self->runout_confirmed_active_)
                return; // never saw a confirmed runout
            if (self->user_took_manual_action_)
                return; // user is managing it in-dialog
            if (!self->runout_modal_.is_visible())
                return;
            spdlog::info(
                "[FilamentRunoutHandler] Runout cleared externally — auto-closing guidance modal");
            self->hide_runout_guidance_modal();
        },
        helix::FilamentSensorManager::instance().get_subjects_lifetime());
}

// ============================================================================
// Load Dispatch
// ============================================================================

void FilamentRunoutHandler::dispatch_load() {
    spdlog::info("[FilamentRunoutHandler] User chose to load filament after runout");

    // Shared three-tier ladder (AMS backend, then the configured LOAD_FILAMENT
    // macro, then raw gcode) via execute_filament_load() — the same execution
    // this dialog and PrintStatusWidget now share. FilamentPanel and
    // AmsOperationSidebar still answer the same plan_load() decision but each
    // carry their own independent execution ladder — unconverted follow-up.
    // Before this the runout dialog only ever reached the backend, and with no
    // backend it navigated the user to the Filament panel — out from under the
    // very dialog they were working in.
    AmsBackend* backend = AmsState::instance().get_backend();
    // The runout is on whatever lane is currently feeding, so that lane is the
    // target — there is no slot picker under a runout dialog.
    const int slot = backend ? backend->get_current_slot() : -1;
    helix::ui::execute_filament_load(backend, slot, "[FilamentRunoutHandler]");
}

// ============================================================================
// Unload Dispatch
// ============================================================================

void FilamentRunoutHandler::dispatch_unload() {
    spdlog::info("[FilamentRunoutHandler] User chose to unload filament after runout");

    AmsBackend* backend = AmsState::instance().get_backend();

    // The runout is on whatever lane is currently feeding, so that lane is the
    // target — there is no slot picker under a runout dialog.
    const int slot = backend ? backend->get_current_slot() : -1;

    // unload_target_is_loaded()'s is_current_slot arm is the whole reason this
    // button works at all here: a runout clears the lane's own sensor while
    // filament is still at the head, and #1199 deliberately keeps Unload
    // reachable in exactly that state (#995).
    bool loaded = false;
    if (backend) {
        loaded = helix::ui::unload_target_is_loaded(slot, backend->slot_is_actively_loaded(slot),
                                                    backend->slot_has_filament_at_toolhead(slot),
                                                    /*is_current_slot=*/true,
                                                    backend->get_system_info().filament_loaded);
    }

    helix::ui::execute_filament_unload(backend, slot, loaded, "[FilamentRunoutHandler]");
}

// ============================================================================
// Purge Dispatch
// ============================================================================

void FilamentRunoutHandler::dispatch_purge() {
    spdlog::info("[FilamentRunoutHandler] User chose to purge after runout");
    // Same shared two-tier ladder (configured PURGE macro, then raw gcode) the
    // home Filament tile uses — execute_filament_purge() was extracted from the
    // body that used to live here.
    helix::ui::execute_filament_purge("[FilamentRunoutHandler]");
}

void FilamentRunoutHandler::hide_modal() {
    hide_runout_guidance_modal();
}

void FilamentRunoutHandler::hide_runout_guidance_modal() {
    if (!runout_modal_.is_visible()) {
        return;
    }

    spdlog::debug("[FilamentRunoutHandler] Hiding runout guidance modal");
    runout_modal_.hide();
    // Stop observing the runout-cleared subject so it doesn't linger across pauses.
    runout_cleared_observer_.reset();
    // Stop gating Resume; clear the block so a future non-auto-feed modal starts
    // ungated (the subject is component-scoped and persists across show/hide).
    port_present_observer_.reset();
    autofeed_context_ = false;
    runout_modal_.set_resume_blocked(false);
}

} // namespace helix::ui
