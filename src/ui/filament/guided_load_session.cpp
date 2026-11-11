// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "guided_load_session.h"

#include "ui_error_reporting.h"

#include "ams_state.h"
#include "app_globals.h"
#include "cold_pull_runner.h"
#include "core/observer_factory.h"
#include "filament_op_execute.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "printer_state.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix::ui {

namespace {

constexpr const char* kLogTag = "[GuidedLoad]";

} // namespace

ManualLoadRefusal manual_load_refusal(PrintState lifecycle, bool printer_reachable,
                                      bool preheat_pending, bool cold_pull_running,
                                      const std::string& slot_extruder,
                                      const std::string& active_extruder) {
    if (job_holds_machine(lifecycle)) {
        return ManualLoadRefusal::JobHoldsMachine;
    }
    if (!printer_reachable) {
        return ManualLoadRefusal::NotReady;
    }
    if (preheat_pending) {
        return ManualLoadRefusal::PreheatPending;
    }
    if (cold_pull_running) {
        return ManualLoadRefusal::ColdPullRunning;
    }
    if (slot_extruder.empty()) {
        return ManualLoadRefusal::ToolUnknown;
    }
    if (slot_extruder != active_extruder) {
        return ManualLoadRefusal::ToolNotActive;
    }
    return ManualLoadRefusal::None;
}

std::string manual_load_extruder(AmsBackend* backend, int slot, PrinterState& state) {
    auto& temps = state.temperature_state();
    if (!backend || slot < 0) {
        return temps.active_extruder_name();
    }
    // resolve_op_nozzle() falls back to the active extruder here, which is right for
    // heating and wrong for pushing: G1 E would move whichever nozzle that is.
    const std::string extruder =
        ToolState::instance().extruder_name_for_tool(backend->get_slot_info(slot).mapped_tool);
    if (extruder.empty() || !temps.get_extruder_target_subject(extruder)) {
        return {};
    }
    return extruder;
}

ManualLoadRefusal manual_load_refusal_now(PrinterState& state, int slot) {
    const FilamentOpHeater* heater = get_filament_op_heater();
    const ColdPullRunner* cold_pull = get_cold_pull_runner();
    return manual_load_refusal(
        state.print_state().get_print_lifecycle(), printer_reachable(state),
        heater && heater->pending(), cold_pull && cold_pull->running(),
        manual_load_extruder(AmsState::instance().get_backend(), slot, state),
        state.temperature_state().active_extruder_name());
}

std::string manual_load_refusal_text(ManualLoadRefusal refusal) {
    switch (refusal) {
    case ManualLoadRefusal::None:
        return {};
    case ManualLoadRefusal::JobHoldsMachine:
        return lv_tr("A print holds the printer.");
    case ManualLoadRefusal::NotReady:
        return lv_tr("The printer is not ready.");
    case ManualLoadRefusal::PreheatPending:
        return lv_tr("Wait for the current filament operation to finish");
    case ManualLoadRefusal::ColdPullRunning:
        return lv_tr("A cold pull is already running.");
    case ManualLoadRefusal::ToolUnknown:
        return lv_tr("This slot feeds no tool");
    case ManualLoadRefusal::ToolNotActive:
        return lv_tr("Select this tool first, then load it by hand.");
    }
    return {};
}

GuidedLoadSession::GuidedLoadSession(PrinterState& state, filament_macros::GuidedLoad steps,
                                     bool waits_for_push)
    : state_(state), flow_(std::move(steps), waits_for_push) {}

GuidedLoadSession::GuidedLoadSession(PrinterState& state, int slot)
    : state_(state), flow_(GuidedLoadFlow::after_preheat(hand_feed_steps())), manual_(true),
      slot_(slot),
      extruder_(manual_load_extruder(AmsState::instance().get_backend(), slot, state)) {}

GuidedLoadSession::~GuidedLoadSession() {
    on_changed_ = nullptr;
    cancel();
    lifecycle_observer_.reset();
    connection_observer_.reset();
    klippy_observer_.reset();
    active_extruder_observer_.reset();
    lifetime_.invalidate();
}

void GuidedLoadSession::begin() {
    if (begun_) {
        return;
    }
    begun_ = true;
    const SubjectLifetime life = state_.get_subjects_lifetime();
    lifecycle_observer_ = observe<int>(
        state_.print_state().get_print_lifecycle_subject(), this,
        [](GuidedLoadSession* self, int) { self->on_printer_changed(); }, life);
    connection_observer_ = observe<int>(
        state_.network_state().get_printer_connection_state_subject(), this,
        [](GuidedLoadSession* self, int) { self->on_printer_changed(); }, life);
    klippy_observer_ = observe<int>(
        state_.network_state().get_klippy_state_subject(), this,
        [](GuidedLoadSession* self, int) { self->on_printer_changed(); }, life);
    if (!manual_ || ended()) {
        return;
    }
    // A tool change re-publishes the active extruder's reading, so this sees every switch.
    active_extruder_observer_ = observe<int>(
        state_.temperature_state().get_active_extruder_temp_subject(), this,
        [](GuidedLoadSession* self, int) { self->on_printer_changed(); }, life);
    if (ended()) {
        return;
    }

    FilamentOpHeater* heater = get_filament_op_heater();
    if (!heater) {
        lose("no heater service");
        return;
    }
    PreheatRequest req;
    req.op = FilamentHeatOp::Load;
    req.log_tag = kLogTag;
    req.skip = preheat_skip_reason_unplanned();
    req.owner = this;
    req.slot = slot_;
    req.announce = false; // the dialog shows the heat itself
    auto tok = lifetime_.token();
    // Both run on the main thread, from the heater.
    req.run = [tok, this](FilamentOpTicket ticket) {
        if (tok.expired()) {
            if (auto* h = get_filament_op_heater()) {
                h->op_cancelled(ticket);
            }
            return;
        }
        on_heated(ticket);
    };
    req.on_progress = [tok, this](const PreheatProgress& progress) {
        if (!tok.expired()) {
            on_heat_progress(progress);
        }
    };
    heat_requested_ = true;
    spdlog::info("{} Manual load on slot {}: heating first", kLogTag, slot_);
    if (heater->run_after_preheat(std::move(req)) == FilamentOpHeater::Outcome::Cancelled &&
        !ended()) {
        lose("the heater refused the preheat");
    }
}

void GuidedLoadSession::on_heat_progress(const PreheatProgress& progress) {
    heat_current_c_ = progress.current_c;
    heat_target_c_ = progress.target_c;
    if (progress.phase == PreheatPhase::Cancelled || progress.phase == PreheatPhase::Abandoned) {
        // The heater already put its heat back; nothing of ours is on the nozzle.
        if (!ended()) {
            spdlog::info("{} The preheat ended without reaching temperature", kLogTag);
            (void)flow_.cancel();
            heat_requested_ = false;
            finish();
        }
    }
    changed();
}

void GuidedLoadSession::on_heated(FilamentOpTicket ticket) {
    FilamentOpHeater* heater = get_filament_op_heater();
    if (ended()) {
        if (heater) {
            heater->op_cancelled(ticket);
        }
        return;
    }
    heat_requested_ = false;
    ticket_ = ticket;
    if (heater) {
        auto tok = lifetime_.token();
        heater->hold_heat(ticket, [tok, this]() {
            if (!tok.expired()) {
                lose("the printer is disconnecting");
            }
        });
    }
    spdlog::info("{} Nozzle hot: waiting for the hand push", kLogTag);
    flow_.heated();
    changed();
}

void GuidedLoadSession::on_printer_changed() {
    if (ended()) {
        return;
    }
    if (job_holds_machine(state_.print_state().get_print_lifecycle())) {
        lose("a print took the machine");
    } else if (!printer_reachable(state_)) {
        lose("the printer went away");
    } else if (!on_heated_extruder()) {
        lose("the active tool changed", lv_tr("Load canceled: the active tool changed"));
    }
}

bool GuidedLoadSession::on_heated_extruder() const {
    return !manual_ ||
           (!extruder_.empty() && extruder_ == state_.temperature_state().active_extruder_name());
}

void GuidedLoadSession::lose(const char* why, const char* toast) {
    if (ended()) {
        return;
    }
    spdlog::info("{} Ending: {}; sending nothing more", kLogTag, why);
    if (toast) {
        NOTIFY_WARNING("{}", toast);
    }
    (void)flow_.cancel();
    // A preheat a job abandoned is the heater's to settle, not ours to cancel.
    if (job_holds_machine(state_.print_state().get_print_lifecycle())) {
        heat_requested_ = false;
    }
    finish();
    changed();
}

void GuidedLoadSession::extrude() {
    send(flow_.extrude(), /*report_outcome=*/true);
    changed();
}

void GuidedLoadSession::done() {
    const std::string script = flow_.done();
    if (flow_.ended()) {
        finish();
    }
    send(script, /*report_outcome=*/true);
    changed();
}

void GuidedLoadSession::cancel() {
    if (ended()) {
        return;
    }
    send(flow_.cancel(), /*report_outcome=*/false);
    finish();
    changed();
}

void GuidedLoadSession::filament_detected() {
    send(flow_.filament_detected(), /*report_outcome=*/true);
    changed();
}

void GuidedLoadSession::send(const std::string& script, bool report_outcome) {
    if (script.empty()) {
        return;
    }
    // The extruder and the heater are the job's once one holds the machine.
    if (job_holds_machine(state_.print_state().get_print_lifecycle())) {
        spdlog::warn("{} A job holds the machine; not sending: {}", kLogTag, script);
        if (report_outcome) {
            lose("a print took the machine");
        }
        return;
    }
    // G1 E moves the active extruder: never push a nozzle this load did not heat.
    if (report_outcome && !on_heated_extruder()) {
        spdlog::warn("{} The active extruder is not {}; not sending: {}", kLogTag, extruder_,
                     script);
        lose("the active tool changed", lv_tr("Load canceled: the active tool changed"));
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("{} No API; not sending: {}", kLogTag, script);
        if (report_outcome) {
            on_command_finished(false);
        }
        return;
    }
    spdlog::info("{} Sending: {}", kLogTag, script);
    if (!report_outcome) {
        // Cleanup outlives this session: nothing is left to tell about it.
        api->execute_gcode(
            script, []() {}, [](const MoonrakerError&) {}, IMoonrakerAPI::kExtrusionTimeoutMs,
            /*silent=*/false, /*on_queued=*/nullptr, /*caller_surfaces_errors=*/false);
        return;
    }
    auto tok = lifetime_.token();
    api->execute_gcode(
        script,
        [tok, this]() {
            tok.defer("GuidedLoadSession::sent", [this]() { on_command_finished(true); });
        },
        [tok, this](const MoonrakerError&) {
            tok.defer("GuidedLoadSession::failed", [this]() { on_command_finished(false); });
        },
        IMoonrakerAPI::kExtrusionTimeoutMs, /*silent=*/false, /*on_queued=*/nullptr,
        /*caller_surfaces_errors=*/false);
}

void GuidedLoadSession::on_command_finished(bool ok) {
    const std::string follow_up = flow_.command_finished(ok);
    send(follow_up, /*report_outcome=*/false);
    if (flow_.ended()) {
        finish();
    }
    changed();
}

void GuidedLoadSession::finish() {
    if (finished_ || !flow_.ended()) {
        return;
    }
    finished_ = true;
    FilamentOpHeater* heater = get_filament_op_heater();
    if (!heater) {
        return;
    }
    if (ticket_ != 0) {
        const FilamentOpTicket ticket = ticket_;
        ticket_ = 0;
        if (flow_.stage() == GuidedLoadStage::Finished) {
            heater->op_succeeded(ticket); // the post-op cooldown
        } else {
            heater->op_cancelled(ticket);
        }
    } else if (heat_requested_) {
        heat_requested_ = false;
        heater->cancel_owned(this);
    }
}

void GuidedLoadSession::changed() {
    if (on_changed_) {
        // Copied: the callback may close the dialog that owns this session.
        auto cb = on_changed_;
        cb();
    }
}

} // namespace helix::ui
