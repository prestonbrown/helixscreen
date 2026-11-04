// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plr_prompt.h"
#include "ui_plr_wizard_overlay.h"

#include "app_globals.h" // is_wizard_active
#include "core/observer_factory.h"
#include "json_utils.h"
#include "moonraker_api.h"
#include "plr_backend.h"
#include "plr_offer.h"
#include "plr_offer_controller.h"
#include "plr_plugin_rpc.h"
#include "plr_profile.h"
#include "plr_wizard.h"
#include "print_lifecycle_state.h" // PrintState, job_holds_machine
#include "printer_detector.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

PlrOfferController::PlrOfferController() {
    auto& ps = get_printer_state();

    // Seed last_conn_state_ from the live subject BEFORE registering the conn
    // observer. observe<int> fires once at registration with the current
    // value, so seeding here means that first firing sees prev == next and does
    // not spuriously re-arm the latch.
    last_conn_state_ =
        lv_subject_get_int(ps.network_state().get_printer_connection_state_subject());

    // plr_snapshot_valid is the PRIMARY trigger of a firmware that validates its
    // own snapshot. observe<int> fires once at registration with the current
    // value (deferred via the update queue), so a flag that is ALREADY true
    // when we register still offers via this registration-fire; thereafter a
    // genuine 0->1 edge offers. See on_connection_state_changed for how
    // reconnect manufactures that edge.
    snapshot_valid_observer_ = observe<int>(
        ps.print_state().get_plr_snapshot_valid_subject(), this,
        [](PlrOfferController* self, int value) { self->on_snapshot_valid_changed(value); },
        ps.get_subjects_lifetime());

    // plr_interrupted_flag is the PRIMARY trigger of the macro-and-flag
    // mechanism, with the same shape: a value the status payload already
    // carried at registration still offers via the registration-fire, and
    // thereafter a 0->1 edge does. The capability half (the discovered resume
    // macro) comes from discovery and is read live in evaluate_offer, so this
    // observer alone is enough.
    interrupted_flag_observer_ = observe<int>(
        ps.print_state().get_plr_interrupted_flag_subject(), this,
        [](PlrOfferController* self, int value) { self->on_plr_interrupted_flag_changed(value); },
        ps.get_subjects_lifetime());

    // plr_power_loss_signal is the PRIMARY trigger of the active probe. Unlike
    // a validated-snapshot flag this only says the FIRMWARE supports recovery —
    // whether a snapshot exists takes a separate, side-effectful probe.
    probe_capable_observer_ = observe<int>(
        ps.print_state().get_plr_power_loss_signal_subject(), this,
        [](PlrOfferController* self, int value) { self->on_probe_capable_changed(value); },
        ps.get_subjects_lifetime());

    conn_observer_ = observe<int>(
        ps.network_state().get_printer_connection_state_subject(), this,
        [](PlrOfferController* self, int value) { self->on_connection_state_changed(value); },
        ps.get_subjects_lifetime());

    // Wizard-active edge: re-evaluate when the wizard closes so a
    // wizard-suppressed offer fires. See evaluate_offer for the full rationale.
    wizard_observer_ = observe<int>(
        &get_wizard_active_subject(), this,
        [](PlrOfferController* self, int value) { self->on_wizard_active_changed(value); },
        get_app_globals_subjects_lifetime());

    // A job that vanishes under the app: re-evaluate an offer the stale job
    // held back. See on_print_lifecycle_changed.
    lifecycle_observer_ = observe_print_lifecycle(
        ps.print_state().get_print_lifecycle_subject(), this,
        [](PlrOfferController* self, PrintState state) { self->on_print_lifecycle_changed(state); },
        ps.get_subjects_lifetime());

    // The plugin's status is asked once the `plr` feature is Ready and Klipper
    // is ready; whichever arrives last asks.
    plr_feature_observer_ = observe<int>(
        ps.plugin_status_state().get_plr_feature_state_subject(), this,
        [](PlrOfferController* self, int value) { self->on_plr_feature_changed(value); },
        ps.get_subjects_lifetime());
    plugin_probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](PlrOfferController* self, int seq) {
            if (seq > 0) {
                self->probe_answered_this_connect_ = true;
                self->publish_profile();
            }
        },
        ps.get_subjects_lifetime());
    printer_type_observer_ = observe<const char*>(
        ps.profile_state().get_printer_type_subject(), this,
        [](PlrOfferController* self, const char* /*type*/) { self->publish_profile(); },
        ps.get_subjects_lifetime());
    klippy_observer_ = observe<int>(
        ps.network_state().get_klippy_state_subject(), this,
        [](PlrOfferController* self, int /*value*/) { self->request_plugin_status(); },
        ps.get_subjects_lifetime());
}

PlrCapabilitySignals PlrOfferController::read_signals() const {
    auto& ps = get_printer_state();

    // Every recovery mechanism reduces to the same capability markers;
    // plr_backend owns which firmware publishes which. See
    // docs/devel/POWER_LOSS_RECOVERY.md.
    PlrCapabilitySignals caps;
    caps.snapshot_valid =
        lv_subject_get_int(ps.print_state().get_plr_snapshot_valid_subject()) != 0;
    caps.resume_macro = lv_subject_get_int(ps.print_state().get_plr_resume_macro_subject()) != 0;
    caps.interrupted_flag =
        lv_subject_get_int(ps.print_state().get_plr_interrupted_flag_subject()) != 0;
    caps.probe_capable =
        lv_subject_get_int(ps.print_state().get_plr_power_loss_signal_subject()) != 0;
    caps.status_recovery_file = ps.print_state().pl_recovery_file();
    caps.plr_feature =
        ps.plugin_status_state().plugin_feature(kPlrFeatureName, kPlrFeatureMinVersion);
    if (plugin_status_) {
        caps.plugin_backend = plugin_status_->backend;
        caps.plugin_offer_available = plugin_status_->offer_available;
        if (plugin_status_->snapshot) {
            caps.plugin_recovery_file = plugin_status_->snapshot->filename;
        }
    }
    return caps;
}

void PlrOfferController::evaluate_offer() {
    auto& ps = get_printer_state();
    // Normalize the firmware mechanisms into ONE availability signal so the
    // pure decision below (and its latch/re-arm/wizard rules) stays
    // backend-agnostic.
    const PlrOfferAvailability offer = plr_offer_availability(read_signals(), probe_);
    const PlrBackendType backend = offer.backend;
    const bool recovery_available = offer.available;
    const std::string& recovery_file = offer.recovery_file;

    // The lifecycle, not the wire: during a host-side pre-print block
    // print_stats still reads standby, and offering "Resume interrupted print?"
    // on top of a start the user has already committed to is a modal ambush —
    // its Resume button would start a different file than the one they chose.
    bool idle = !job_holds_machine(ps.print_state().get_print_lifecycle());

    PlrOfferSignals signals;
    signals.recovery_available = recovery_available;
    signals.printer_idle = idle;
    signals.already_prompted = prompted_this_connect_;
    signals.wizard_active = is_wizard_active();
    signals.resume_in_progress = plr_wizard_open();

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
    //   - Not idle (plugin backend): a host-stack restart can hand over the
    //     plugin's offer before print_stats reports standby, while the
    //     lifecycle still reads the dead job. Its held -> Idle edge routes back
    //     here (on_print_lifecycle_changed).
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
    // is also the safety gate: for the probed backend, plr_build_plan refuses to
    // produce a resume gcode unless the probe completed and confirmed both
    // states — the probe is what set print_stats.power_loss=1, without which the
    // stock sensorless-homing macro lifts 0.1mm and homes X/Y through the part.
    PlrRecoveryPlan plan = plr_build_plan(backend, recovery_file, probe_.result);
    if (!plan.resume_allowed()) {
        // Deliberately do NOT latch: a prompt whose Resume cannot work is worse
        // than no prompt, but the situation may still resolve this connection
        // (e.g. the probe response has not landed yet).
        spdlog::debug("[PLR] Recovery snapshot present but resume not authorized yet "
                      "(backend={}) — not offering",
                      plr_backend_name(backend));
        return;
    }

    prompted_this_connect_ = true;
    ++offers_shown_;
    spdlog::info("[PLR] Offering power-loss recovery (backend={}, idle={}, file='{}')",
                 plr_backend_name(backend), idle, plan.recovery_file);
    show_plr_recovery_prompt(get_moonraker_api(), plan);
}

void PlrOfferController::on_snapshot_valid_changed(int /*snapshot_valid*/) {
    // evaluate_offer reads the flag straight from the subject, so the notified
    // value is not needed here — kept for the observer signature.
    evaluate_offer();
}

void PlrOfferController::on_plr_interrupted_flag_changed(int /*was_interrupted*/) {
    // Same shape as on_snapshot_valid_changed: evaluate_offer reads the flag
    // straight from the subject, so the notified value is not needed here.
    evaluate_offer();
}

void PlrOfferController::on_probe_capable_changed(int capable) {
    if (capable == 0) {
        return;
    }
    // Capability alone offers nothing — ask the firmware whether a snapshot
    // actually exists. evaluate_offer runs from the probe response.
    probe_once();
}

void PlrOfferController::probe_once() {
    if (probe_.fired) {
        return;
    }
    auto& ps = get_printer_state();

    // STANDBY, not merely "not active": the Klipper handler itself only arms
    // power_loss when print_stats.state == "standby", and probing out of a
    // terminal state would clear exclude_object_info for no benefit.
    // RAW_PRINT_STATE_OK: mirrors a Klipper condition on print_stats.state
    // itself — the handler only arms power_loss from "standby".
    auto state = ps.print_state().get_print_job_state();
    if (state != PrintJobState::STANDBY) {
        spdlog::debug("[PLR] Probe-capable but print state is {} (not standby) — skipping probe",
                      static_cast<int>(state));
        return;
    }

    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[PLR] Recovery probe skipped: no IMoonrakerAPI");
        return;
    }

    // Latch BEFORE dispatch. The call is side-effectful (on a JSON parse failure
    // Klipper deletes the recovery sidecar), so an error path must not leave the
    // door open for a retry storm — one attempt per connection, full stop.
    probe_.fired = true;

    plr_rpc::request_recovery_probe(
        *api,
        lifetime_.bg_cb("PlrOfferController::recovery_probe",
                        [this](const PlrDetectResult& r) { on_probe_result(r); }),
        [](const MoonrakerError& err) {
            // Silent to the user: a printer that advertises power_loss but has
            // no endpoint is a firmware-variant mismatch, not something the
            // operator can act on. The absence of an offer is the outcome.
            spdlog::warn("[PLR] Recovery probe failed: {}", err.message);
        });
}

void PlrOfferController::on_probe_result(const PlrDetectResult& result) {
    probe_.result = result;
    if (!plr_probe_recovery_available(probe_.result)) {
        spdlog::info("[PLR] Probe reports no resumable print (completed={} file_state={} "
                     "eeprom_state={})",
                     result.completed, result.file_state, result.eeprom_state);
        return;
    }
    // Best-effort, read-only: HelixScreen runs on the printer, so the sidecar is
    // normally readable. Without a filename there is no SDCARD_PRINT_FILE to
    // send, and plr_build_plan will withhold the offer rather than show a
    // Resume button that cannot work.
    probe_.recovery_file = plr_read_probe_recovery_filename();
    if (probe_.recovery_file.empty()) {
        // Fall back to whatever recovery file the status payload carried.
        probe_.recovery_file = get_printer_state().print_state().pl_recovery_file();
    }
    evaluate_offer();
}

void PlrOfferController::listen_for_plugin_status() {
    auto* api = get_moonraker_api();
    if (!api || api == status_notify_api_) {
        return;
    }
    status_notify_api_ = api;
    api->register_method_callback(
        kPlrStatusNotify, kStatusNotifyHandler,
        lifetime_.bg_cb("PlrOfferController::plr_status_notify",
                        [this](const nlohmann::json& msg) { on_plugin_status_notify(msg); }));
}

void PlrOfferController::on_plugin_status_notify(const nlohmann::json& msg) {
    const nlohmann::json* payload = json_util::notification_payload(msg);
    if (payload == nullptr) {
        return;
    }
    auto& ps = get_printer_state();
    if (lv_subject_get_int(ps.network_state().get_printer_connection_state_subject()) !=
            static_cast<int>(ConnectionState::CONNECTED) ||
        ps.plugin_status_state().plugin_feature(kPlrFeatureName, kPlrFeatureMinVersion) !=
            PluginFeatureState::Ready) {
        return;
    }
    // The payload is the status reply itself: it replaces any answer this
    // connection holds, and a reply still in flight is older than it.
    spdlog::info("[PLR] Plugin pushed its recovery status");
    ++connection_generation_;
    plugin_status_inflight_ = false;
    on_plugin_status(plr_parse_plugin_status(*payload));
}

void PlrOfferController::request_plugin_status() {
    listen_for_plugin_status();
    if (plugin_status_ || plugin_status_inflight_) {
        return;
    }
    auto& ps = get_printer_state();
    if (lv_subject_get_int(ps.network_state().get_printer_connection_state_subject()) !=
        static_cast<int>(ConnectionState::CONNECTED)) {
        return;
    }
    if (lv_subject_get_int(ps.network_state().get_klippy_state_subject()) !=
        static_cast<int>(KlippyState::READY)) {
        return;
    }
    if (ps.plugin_status_state().plugin_feature(kPlrFeatureName, kPlrFeatureMinVersion) !=
        PluginFeatureState::Ready) {
        return;
    }
    auto* api = get_moonraker_api();
    if (!api) {
        return;
    }
    plugin_status_inflight_ = true;
    const int generation = connection_generation_;
    plr_rpc::request_status(
        *api,
        lifetime_.bg_cb("PlrOfferController::plugin_status",
                        [this, generation](const PlrPluginStatus& status) {
                            if (generation != connection_generation_) {
                                return;
                            }
                            plugin_status_inflight_ = false;
                            on_plugin_status(status);
                        }),
        lifetime_.bg_cb("PlrOfferController::plugin_status_error",
                        [this, generation](const MoonrakerError& err) {
                            if (generation != connection_generation_) {
                                return;
                            }
                            // Silent: the next Klipper-ready or plugin edge asks again.
                            plugin_status_inflight_ = false;
                            spdlog::warn("[PLR] Plugin recovery status failed: {}", err.message);
                        }));
}

void PlrOfferController::on_plr_feature_changed(int state) {
    if (state != last_plr_feature_state_) {
        last_plr_feature_state_ = state;
        // Saving switched on or off changes the plugin's answer.
        plugin_status_.reset();
    }
    publish_profile();
    request_plugin_status();
}

void PlrOfferController::publish_profile() {
    auto& ps = get_printer_state();
    if (lv_subject_get_int(ps.network_state().get_printer_connection_state_subject()) !=
        static_cast<int>(ConnectionState::CONNECTED)) {
        return;
    }
    // An unavailable plr feature still takes the profile: the plugin reports
    // an unlisted printer unavailable until a profile of tier 1 or more lands.
    if (!ps.plugin_status_state().plugin_feature_reported(kPlrFeatureName, kPlrFeatureMinVersion)) {
        return;
    }
    // Until this connection's discovery has run, printer_type() can still be
    // the previous printer's, and its block must not land in this Moonraker.
    if (!probe_answered_this_connect_) {
        return;
    }
    const std::string type = ps.profile_state().printer_type();
    if (published_profile_type_ && *published_profile_type_ == type) {
        return;
    }
    auto* api = get_moonraker_api();
    if (!api) {
        return;
    }
    published_profile_type_ = type;
    // The app does not read homing_override, so homing is unknown here: no
    // user raise passes it. The plugin checks the database tier itself.
    const PlrProfile profile = plr_resolve_profile(PrinterDetector::get_plr_block(type),
                                                   PlrUserOverride{}, PlrHomingCheck{});
    spdlog::info("[PLR] Writing plr_profile for '{}' (tier {})", type, profile.tier);
    plr_rpc::write_profile(
        *api, plr_profile_json(profile),
        [api]() {
            // The plugin's availability and saving verdict follow the profile,
            // and a map read before it was stored still reads not_listed.
            plr_rpc::refresh_features(*api);
        },
        lifetime_.bg_cb("PlrOfferController::write_profile_error",
                        [this, type](const MoonrakerError& err) {
                            spdlog::warn("[PLR] Writing plr_profile failed: {}", err.message);
                            // The next probe answer or type change writes again.
                            if (published_profile_type_ == type) {
                                published_profile_type_.reset();
                            }
                        }));
}

void PlrOfferController::on_plugin_status(const PlrPluginStatus& status) {
    // An answer given while Klipper was still starting says nothing about the
    // snapshot; keep asking until one is given with Klipper ready.
    const bool klippy_not_ready = std::find(status.blocked_by.begin(), status.blocked_by.end(),
                                            "klippy_not_ready") != status.blocked_by.end();
    spdlog::info("[PLR] Plugin recovery status: backend={} tier={} offer={} snapshot='{}'",
                 static_cast<int>(status.backend), status.tier, status.offer_available,
                 status.snapshot ? status.snapshot->filename : std::string());
    for (const std::string& code : status.blocked_by) {
        spdlog::info("[PLR] Plugin withholds the offer: {}", plr_blocked_reason(code));
    }
    if (klippy_not_ready) {
        return;
    }
    plugin_status_ = status;
    evaluate_offer();
}

void PlrOfferController::on_connection_state_changed(int new_conn_state) {
    if (helix::plr_should_rearm(last_conn_state_, new_conn_state)) {
        spdlog::debug("[PLR] Connection dropped — re-arming recovery offer + probe latches");
        prompted_this_connect_ = false;
        // Drop the probe latch and the resume authorization with the
        // connection. `completed` must never survive into a session whose
        // probe has not run.
        probe_ = PlrProbeSession{};
        plugin_status_.reset();
        plugin_status_inflight_ = false;
        published_profile_type_.reset();
        probe_answered_this_connect_ = false;
        ++connection_generation_;

        // Force the PLR capability and availability subjects back to 0 (and
        // drop the stale recovery file) so the reconnect's full status
        // re-dispatch produces genuine 0->1 edges that re-fire the observers.
        // The subjects dedup same-value writes, so without this forced 0 the
        // values would stay 1 across the reconnect and no fresh edge would
        // ever arrive. Safe on the main thread: observer callbacks are
        // queue-deferred.
        auto& ps = get_printer_state();
        lv_subject_set_int(ps.print_state().get_plr_snapshot_valid_subject(), 0);
        lv_subject_set_int(ps.print_state().get_plr_resume_macro_subject(), 0);
        lv_subject_set_int(ps.print_state().get_plr_interrupted_flag_subject(), 0);
        lv_subject_set_int(ps.print_state().get_plr_power_loss_signal_subject(), 0);
        ps.print_state().clear_pl_recovery_file();
    }
    last_conn_state_ = new_conn_state;
    request_plugin_status();
}

void PlrOfferController::on_wizard_active_changed(int wizard_active) {
    // Only the wizard-closed edge matters: an offer suppressed solely because
    // the wizard owned the screen can now fire. evaluate_offer re-applies every
    // other guard (already-prompted, idle, availability), so a spurious re-eval
    // is harmless. Note this does NOT re-fire the recovery probe — that latch is
    // separate and per-connection.
    if (wizard_active == 0) {
        evaluate_offer();
    }
}

void PlrOfferController::on_print_lifecycle_changed(PrintState next) {
    const PrintState prev = last_lifecycle_;
    last_lifecycle_ = next;
    // A job dropping straight to Idle is either Klipper restarting under it or
    // a normal end: some firmware's print end or restart-from-beginning resets
    // print_stats to standby without reporting Complete. Only the backends
    // whose availability a normal end cannot fake re-evaluate here
    // (plr_reoffers_after_job_drop).
    if (!job_holds_machine(prev) || next != PrintState::Idle) {
        return;
    }
    // The probe is refused outside STANDBY and only runs on a probe-capable
    // firmware; its response does the evaluating.
    if (lv_subject_get_int(get_printer_state().print_state().get_plr_power_loss_signal_subject()) !=
        0) {
        probe_once();
    }
    if (plr_reoffers_after_job_drop(plr_select_backend(read_signals()))) {
        evaluate_offer();
    }
}

} // namespace helix::ui
