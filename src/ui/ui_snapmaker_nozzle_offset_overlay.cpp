// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_snapmaker_nozzle_offset_overlay.cpp
 * @brief Guided multi-toolhead offset calibration for the Snapmaker U1
 */

#if HELIX_HAS_SNAPMAKER

#include "ui_snapmaker_nozzle_offset_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_nav.h"

#include "app_globals.h"
#include "display_numbering.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace helix::ui {

namespace nozzle_offset = snapmaker::nozzle_offset;
using Phase = nozzle_offset::Runner::Phase;

// Static like the chute calibration's: XML binds them by name from the global
// scope, and SubjectManager deinit on the singleton keeps observers clean.
static lv_subject_t s_state;
static lv_subject_t s_title;
static char s_title_buf[128];
static lv_subject_t s_progress;
static char s_progress_buf[64];
static lv_subject_t s_prompt;
static char s_prompt_buf[256];
static lv_subject_t s_error;
static char s_error_buf[256];
static lv_subject_t s_results;
static char s_results_buf[384];

namespace {

std::string head_label(int tool) {
    return active_tool_label(tool_label(tool), tool, ToolLabelForm::Full);
}

/// What the Working screen says the printer is doing now.
std::string step_title(Phase phase, const nozzle_offset::Step* step) {
    if (phase == Phase::Finishing || !step) {
        return lv_tr("Saving and finishing...");
    }
    const std::string head = step->tool >= 0 ? head_label(step->tool) : std::string{};
    switch (step->kind) {
    case nozzle_offset::StepKind::Prestart:
        return lv_tr("Preheating and homing...");
    case nozzle_offset::StepKind::Heat:
        return fmt::format(lv_tr("Heating {}..."), head);
    case nozzle_offset::StepKind::AutoClean:
        return fmt::format(lv_tr("Wiping {}..."), head);
    case nozzle_offset::StepKind::ManualClean:
        return fmt::format(lv_tr("Moving {} to the front..."), head);
    case nozzle_offset::StepKind::WaitCool:
        return fmt::format(lv_tr("Cooling {}..."), head);
    case nozzle_offset::StepKind::DetectPlate:
        return lv_tr("Checking the PEI sheet is off...");
    case nozzle_offset::StepKind::Probe:
        return fmt::format(lv_tr("Measuring {}..."), head);
    }
    return {};
}

} // namespace

SnapmakerNozzleOffsetOverlay::Screen nozzle_offset_screen(Phase phase,
                                                          nozzle_offset::Prompt prompt) {
    using Screen = SnapmakerNozzleOffsetOverlay::Screen;
    switch (phase) {
    case Phase::Idle:
    case Phase::Cancelled:
        return Screen::Intro;
    case Phase::Running:
    case Phase::Finishing:
        return Screen::Working;
    case Phase::AwaitingUser:
        return prompt == nozzle_offset::Prompt::RemovePlate ? Screen::RemovePlate
                                                            : Screen::CleanNozzle;
    case Phase::Done:
        return Screen::Done;
    case Phase::Failed:
        return Screen::Failed;
    }
    return Screen::Intro;
}

SnapmakerNozzleOffsetOverlay::SnapmakerNozzleOffsetOverlay() : runner_(make_hooks()) {}

SnapmakerNozzleOffsetOverlay::~SnapmakerNozzleOffsetOverlay() {
    if (subjects_initialized_) {
        subjects_.deinit_all();
    }
}

nozzle_offset::Runner::Hooks SnapmakerNozzleOffsetOverlay::make_hooks() {
    return {[this](const std::string& gcode, uint32_t timeout_ms,
                   nozzle_offset::Runner::Completion done) {
                send(gcode, timeout_ms, std::move(done));
            },
            [this]() { abort_probe(); }, [this]() { refresh(); },
            []() -> std::string {
                const auto lifecycle = get_printer_state().print_state().get_print_lifecycle();
                return job_holds_machine(lifecycle) ? lv_tr("Printer is busy with a print")
                                                    : std::string();
            }};
}

// ============================================================================
// SUBJECTS / CALLBACKS
// ============================================================================

void SnapmakerNozzleOffsetOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(s_state, 0, "nozzle_offset_cal_state", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_title, s_title_buf, "", "nozzle_offset_cal_title", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_progress, s_progress_buf, "", "nozzle_offset_cal_progress",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(s_prompt, s_prompt_buf, "", "nozzle_offset_cal_prompt",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(s_error, s_error_buf, "", "nozzle_offset_cal_error", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_results, s_results_buf, "", "nozzle_offset_cal_results",
                                  subjects_);
    });
}

void SnapmakerNozzleOffsetOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_nozzle_offset_start",
         [](lv_event_t*) { get_snapmaker_nozzle_offset_overlay().runner().start(); }},
        {"on_nozzle_offset_next",
         [](lv_event_t*) { get_snapmaker_nozzle_offset_overlay().runner().confirm(); }},
        // The exit gcode rides on_deactivating(), so leaving is all the buttons do.
        {"on_nozzle_offset_close", [](lv_event_t*) { helix::nav::go_back(); }},
    });
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void SnapmakerNozzleOffsetOverlay::open(lv_obj_t* parent) {
    runner_.reset();
    show(parent);
}

void SnapmakerNozzleOffsetOverlay::on_activate() {
    OverlayBase::on_activate();
    refresh();
}

void SnapmakerNozzleOffsetOverlay::on_deactivating(DeactivateReason reason) {
    // A blanked screen is not the user leaving: the run outlives the
    // screensaver. Closing the overlay ends it, and the exit turns the heaters
    // off. Shutdown arrives after the client is gone; the Snapmaker backend's
    // before_disconnect() ends the run while it can still send.
    if (reason == DeactivateReason::NavigateAway) {
        runner_.cancel();
    }
}

void SnapmakerNozzleOffsetOverlay::prompt_end_held_calibration() {
    if (held_prompt_open_) {
        return;
    }
    held_prompt_open_ = true;
    ConfirmOptions options;
    options.cancel_text = lv_tr("Leave it");
    options.on_cancel = [this]() {
        held_prompt_open_ = false;
        spdlog::info("[NozzleOffsetCal] Leaving the held calibration state to its owner");
    };
    options.on_dismiss = [this]() { held_prompt_open_ = false; };
    options.owner_token = object_lifetime_.token();
    modal_confirm(
        lv_tr("Toolhead Calibration Running"),
        lv_tr("A toolhead offset calibration is in progress, possibly from another "
              "screen. End it now?"),
        ModalSeverity::Warning, lv_tr("End calibration"),
        [this]() {
            held_prompt_open_ = false;
            // A run started here while the prompt was up owns the state now.
            if (nozzle_offset::run_live()) {
                return;
            }
            if (IMoonrakerClient* client = get_moonraker_client()) {
                nozzle_offset::send_exit(*client);
            }
        },
        options);
}

// ============================================================================
// TRANSPORT
// ============================================================================

void SnapmakerNozzleOffsetOverlay::send(const std::string& gcode, uint32_t timeout_ms,
                                        nozzle_offset::Runner::Completion done) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        done(false, lv_tr("Not connected to the printer"));
        return;
    }
    // The run belongs to the printer, not the screen: its completions must land
    // after the overlay closes too, so the cancel's EXIT is still tracked.
    api->execute_gcode(
        gcode, object_lifetime_.bg_cb("NozzleOffsetCal::done", [done]() { done(true, {}); }),
        object_lifetime_.bg_cb("NozzleOffsetCal::error",
                               [done](const MoonrakerError& err) { done(false, err.message); }),
        timeout_ms);
}

void SnapmakerNozzleOffsetOverlay::abort_probe() {
    if (IMoonrakerClient* client = get_moonraker_client()) {
        nozzle_offset::send_probe_abort(*client);
    }
}

void SnapmakerNozzleOffsetOverlay::read_offsets() {
    lv_subject_copy_string(&s_results, "");
    IMoonrakerClient* client = get_moonraker_client();
    if (!client) {
        return;
    }
    client->send_jsonrpc(
        "printer.objects.query", nozzle_offset::offsets_query_params(),
        object_lifetime_.bg_cb("NozzleOffsetCal::offsets",
                               [](const nlohmann::json& response) {
                                   const auto offsets = nozzle_offset::offsets_from_query(response);
                                   if (!offsets) {
                                       spdlog::warn("[NozzleOffsetCal] No offsets in the "
                                                    "query response");
                                       return;
                                   }
                                   std::string text;
                                   for (size_t t = 0; t < offsets->size(); ++t) {
                                       const auto& o = (*offsets)[t];
                                       text += fmt::format("{}{}:  X {:+.3f}  Y {:+.3f}  Z {:+.3f}",
                                                           text.empty() ? "" : "\n",
                                                           head_label(static_cast<int>(t)), o[0],
                                                           o[1], o[2]);
                                   }
                                   lv_subject_copy_string(&s_results, text.c_str());
                               }),
        [](const MoonrakerError& err) {
            spdlog::warn("[NozzleOffsetCal] Reading the saved offsets failed: {}", err.message);
        },
        0, true);
}

// ============================================================================
// RENDER
// ============================================================================

void SnapmakerNozzleOffsetOverlay::refresh() {
    if (!subjects_initialized_) {
        init_subjects();
    }
    const Phase phase = runner_.phase();
    const nozzle_offset::Step* step = runner_.current_step();
    const auto screen = nozzle_offset_screen(phase, runner_.prompt());

    if (screen == Screen::Working) {
        lv_subject_copy_string(&s_title, step_title(phase, step).c_str());
        const size_t total = runner_.steps().size();
        const size_t at = phase == Phase::Finishing ? total : runner_.step_index() + 1;
        lv_subject_copy_string(&s_progress, fmt::format(lv_tr("Step {} of {}"), at, total).c_str());
    } else if (screen == Screen::CleanNozzle && step) {
        const std::string head = head_label(step->tool);
        lv_subject_copy_string(&s_title, fmt::format(lv_tr("Clean {}"), head).c_str());
        lv_subject_copy_string(
            &s_prompt, lv_tr("The nozzle is hot. Brush it clean with the wire brush that came "
                             "with the printer and keep the docking position clean, then tap "
                             "Next."));
    } else if (screen == Screen::RemovePlate) {
        lv_subject_copy_string(&s_title, lv_tr("Remove the PEI Sheet"));
        lv_subject_copy_string(
            &s_prompt, runner_.error().empty()
                           ? lv_tr("Take the PEI sheet off the heated bed, then tap Next. The "
                                   "toolheads probe the bare bed.")
                           : lv_tr("The PEI sheet is still on the bed. Take it off, then tap "
                                   "Next."));
    }

    if (screen == Screen::Failed) {
        lv_subject_copy_string(&s_error, runner_.error().c_str());
    }
    const int previous = lv_subject_get_int(&s_state);
    lv_subject_set_int(&s_state, static_cast<int>(screen));
    if (screen == Screen::Done && previous != static_cast<int>(Screen::Done)) {
        read_offsets();
    }
}

} // namespace helix::ui

#endif // HELIX_HAS_SNAPMAKER
