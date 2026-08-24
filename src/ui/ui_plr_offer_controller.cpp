// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plr_prompt.h"

#include "app_globals.h" // is_wizard_active
#include "moonraker_api.h"
#include "observer_factory.h"
#include "plr_backend.h"
#include "plr_offer.h"
#include "plr_offer_controller.h"
#include "print_lifecycle_state.h" // PrintState, job_holds_machine
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

PlrOfferController::PlrOfferController() {
    auto& ps = get_printer_state();

    // Seed last_conn_state_ from the live subject BEFORE registering the conn
    // observer. observe_int_sync fires once at registration with the current
    // value, so seeding here means that first firing sees prev == next and does
    // not spuriously re-arm the latch.
    last_conn_state_ = lv_subject_get_int(ps.get_printer_connection_state_subject());

    // pl_env_valid is the PRIMARY Snapmaker trigger. observe_int_sync fires once
    // at registration with the current value (deferred via the update queue), so
    // a pl_env_valid that is ALREADY true when we register still offers via this
    // registration-fire; thereafter a genuine 0->1 edge offers. See
    // on_connection_state_changed for how reconnect manufactures that edge.
    pl_valid_observer_ = observe_int_sync(
        ps.get_pl_env_valid_subject(), this,
        [](PlrOfferController* self, int value) { self->on_pl_env_valid_changed(value); });

    // creality_plr_capable is the PRIMARY Creality trigger. Unlike Snapmaker's
    // flag this only says the FIRMWARE supports recovery — whether a snapshot
    // exists takes a separate, side-effectful probe.
    creality_capable_observer_ = observe_int_sync(
        ps.get_creality_plr_capable_subject(), this,
        [](PlrOfferController* self, int value) { self->on_creality_capable_changed(value); });

    conn_observer_ = observe_int_sync(
        ps.get_printer_connection_state_subject(), this,
        [](PlrOfferController* self, int value) { self->on_connection_state_changed(value); });

    // Wizard-active edge: re-evaluate when the wizard closes so a
    // wizard-suppressed offer fires. See evaluate_offer for the full rationale.
    wizard_observer_ = observe_int_sync(
        &get_wizard_active_subject(), this,
        [](PlrOfferController* self, int value) { self->on_wizard_active_changed(value); });
}

void PlrOfferController::evaluate_offer() {
    auto& ps = get_printer_state();

    // Normalize the two firmware mechanisms into ONE availability signal so the
    // pure decision below (and its latch/re-arm/wizard rules) stays
    // backend-agnostic. See docs/devel/POWER_LOSS_RECOVERY.md.
    PlrCapabilitySignals caps;
    caps.snapmaker_pl_env_valid = lv_subject_get_int(ps.get_pl_env_valid_subject()) != 0;
    caps.creality_power_loss_field = lv_subject_get_int(ps.get_creality_plr_capable_subject()) != 0;
    PlrBackendType backend = plr_select_backend(caps);

    bool recovery_available = false;
    std::string recovery_file;
    switch (backend) {
    case PlrBackendType::SNAPMAKER:
        // Passive: the firmware already validated the snapshot against MCU
        // flash on boot, so pl_env_valid IS availability.
        recovery_available = true;
        recovery_file = ps.pl_recovery_file();
        break;
    case PlrBackendType::CREALITY:
        // Active: only a completed probe reporting both states counts.
        recovery_available = plr_creality_recovery_available(creality_detect_);
        recovery_file = creality_recovery_file_;
        break;
    case PlrBackendType::NONE:
        break;
    }

    // The lifecycle, not the wire: during a host-side pre-print block
    // print_stats still reads standby, and offering "Resume interrupted print?"
    // on top of a start the user has already committed to is a modal ambush —
    // its Resume button would start a different file than the one they chose.
    bool idle = !job_holds_machine(ps.get_print_lifecycle());

    PlrOfferSignals signals;
    signals.recovery_available = recovery_available;
    signals.printer_idle = idle;
    signals.already_prompted = prompted_this_connect_;
    signals.wizard_active = is_wizard_active();

    // AUTHORITATIVE explanation of the offer's one-shot + re-fire behavior
    // (referenced from plr_offer.h, plr_offer_controller.h, and the observers):
    //
    // plr_should_offer is a pure self-guard: false when there is no valid
    // recovery snapshot, mid-print, already-prompted, or while the setup wizard
    // owns the screen. Two suppression cases resolve on their own because the
    // latch below is set ONLY on the success path:
    //   - Wizard active: prompted_this_connect_ stays unset, and the
    //     wizard-active subject's 1->0 edge routes back here the moment the
    //     wizard closes (on_wizard_active_changed), so the deferred offer fires.
    //   - Reconnect: on_connection_state_changed re-arms the latch AND forces
    //     the capability subjects to 0, so the reconnect's status re-dispatch
    //     drives real 0->1 edges back into the observers -> here. (Without the
    //     forced 0, the values stay 1 across the reconnect and
    //     lv_subject_set_int's changed-guard swallows the same-value write, so
    //     no edge would ever arrive.)
    if (!helix::plr_should_offer(signals)) {
        return;
    }

    // Resolve the concrete actions ONCE, here, and hand them to the modal. This
    // is also the safety gate: for CREALITY, plr_build_plan refuses to produce a
    // resume gcode unless the probe completed and confirmed both states — the
    // probe is what set print_stats.power_loss=1, without which the stock
    // sensorless-homing macro lifts 0.1mm and homes X/Y through the part.
    PlrRecoveryPlan plan = plr_build_plan(backend, recovery_file, creality_detect_);
    if (!plan.resume_allowed()) {
        // Deliberately do NOT latch: a prompt whose Resume cannot work is worse
        // than no prompt, but the situation may still resolve this connection
        // (e.g. the probe response has not landed yet).
        spdlog::debug("[PLR] Recovery snapshot present but resume not authorized yet "
                      "(backend={}) — not offering",
                      static_cast<int>(backend));
        return;
    }

    prompted_this_connect_ = true;
    spdlog::info("[PLR] Offering power-loss recovery (backend={}, idle={}, file='{}')",
                 static_cast<int>(backend), idle, plan.recovery_file);
    show_plr_recovery_prompt(get_moonraker_api(), plan);
}

void PlrOfferController::on_pl_env_valid_changed(int /*pl_env_valid*/) {
    // evaluate_offer reads pl_env_valid straight from the subject, so the
    // notified value is not needed here — kept for the observer signature.
    evaluate_offer();
}

void PlrOfferController::on_creality_capable_changed(int capable) {
    if (capable == 0) {
        return;
    }
    // Capability alone offers nothing — ask the firmware whether a snapshot
    // actually exists. evaluate_offer runs from the probe response.
    probe_creality_once();
}

void PlrOfferController::probe_creality_once() {
    if (creality_probed_this_connect_) {
        return;
    }
    auto& ps = get_printer_state();

    // STANDBY, not merely "not active": the Klipper handler itself only arms
    // power_loss when print_stats.state == "standby", and probing out of a
    // terminal state would clear exclude_object_info for no benefit.
    // RAW_PRINT_STATE_OK: mirrors a Klipper condition on print_stats.state
    // itself — the handler only arms power_loss from "standby".
    auto state = ps.get_print_job_state();
    if (state != PrintJobState::STANDBY) {
        spdlog::debug("[PLR] Creality capable but print state is {} (not standby) — skipping probe",
                      static_cast<int>(state));
        return;
    }

    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[PLR] Creality probe skipped: no IMoonrakerAPI");
        return;
    }

    // Latch BEFORE dispatch. The call is side-effectful (on a JSON parse failure
    // Klipper deletes the recovery sidecar), so an error path must not leave the
    // door open for a retry storm — one attempt per connection, full stop.
    creality_probed_this_connect_ = true;

    api->check_continue_print_state(
        lifetime_.bg_cb("PlrOfferController::creality_detect",
                        [this](const PlrDetectResult& r) { on_creality_detect_result(r); }),
        [](const MoonrakerError& err) {
            // Silent to the user: a printer that advertises power_loss but has
            // no endpoint is a firmware-variant mismatch, not something the
            // operator can act on. The absence of an offer is the outcome.
            spdlog::warn("[PLR] Creality probe failed: {}", err.message);
        });
}

void PlrOfferController::on_creality_detect_result(const PlrDetectResult& result) {
    creality_detect_ = result;
    if (!plr_creality_recovery_available(creality_detect_)) {
        spdlog::info("[PLR] Creality reports no resumable print (completed={} file_state={} "
                     "eeprom_state={})",
                     result.completed, result.file_state, result.eeprom_state);
        return;
    }
    // Best-effort, read-only: HelixScreen runs on the printer, so the sidecar is
    // normally readable. Without a filename there is no SDCARD_PRINT_FILE to
    // send, and plr_build_plan will withhold the offer rather than show a
    // Resume button that cannot work.
    creality_recovery_file_ = plr_read_creality_recovery_filename();
    if (creality_recovery_file_.empty()) {
        // Fall back to whatever the status payload carried; virtual_sdcard's
        // file_path is parsed unconditionally, so it may be populated.
        creality_recovery_file_ = get_printer_state().pl_recovery_file();
    }
    evaluate_offer();
}

void PlrOfferController::on_connection_state_changed(int new_conn_state) {
    if (helix::plr_should_rearm(last_conn_state_, new_conn_state)) {
        spdlog::debug("[PLR] Connection dropped — re-arming recovery offer + probe latches");
        prompted_this_connect_ = false;
        creality_probed_this_connect_ = false;
        // Drop the resume authorization with the connection. `completed` must
        // never survive into a session whose probe has not run.
        creality_detect_ = PlrDetectResult{};
        creality_recovery_file_.clear();

        // Force both capability subjects back to 0 (and drop the stale recovery
        // file) so the reconnect's full status re-dispatch produces genuine 0->1
        // edges that re-fire the observers. The subjects dedup same-value
        // writes, so without this forced 0 the values would stay 1 across the
        // reconnect and no fresh edge would ever arrive. Safe on the main thread
        // — observer callbacks are queue-deferred.
        auto& ps = get_printer_state();
        lv_subject_set_int(ps.get_pl_env_valid_subject(), 0);
        lv_subject_set_int(ps.get_creality_plr_capable_subject(), 0);
        ps.clear_pl_recovery_file();
    }
    last_conn_state_ = new_conn_state;
}

void PlrOfferController::on_wizard_active_changed(int wizard_active) {
    // Only the wizard-closed edge matters: an offer suppressed solely because
    // the wizard owned the screen can now fire. evaluate_offer re-applies every
    // other guard (already-prompted, idle, availability), so a spurious re-eval
    // is harmless. Note this does NOT re-fire the Creality probe — that latch is
    // separate and per-connection.
    if (wizard_active == 0) {
        evaluate_offer();
    }
}

} // namespace helix::ui
