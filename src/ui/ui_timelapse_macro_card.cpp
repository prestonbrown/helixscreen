// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_timelapse_macro_card.h"

#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "timelapse_local_installer.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {
bool job_holds_now() {
    lv_subject_t* job = get_printer_state().print_state().get_job_holds_machine_subject();
    return job && lv_subject_get_int(job) != 0;
}
} // namespace

TimelapseMacroCard& timelapse_macro_card() {
    return helix::lazy_global<TimelapseMacroCard>("TimelapseMacroCard");
}

TimelapseMacroCard::~TimelapseMacroCard() {
    // The MacroManager's own guard drops its pending replies.
    macro_manager_.reset();
    subjects_.deinit_all();
}

void TimelapseMacroCard::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(card_subject_, static_cast<int>(Card::None),
                           "timelapse_macro_card_state", subjects_);
    UI_MANAGED_SUBJECT_STRING(include_line_subject_, include_line_buf_, kTimelapseIncludeLine,
                              "timelapse_macro_include_line", subjects_);
    subjects_initialized_ = true;
}

void TimelapseMacroCard::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }
    register_xml_callbacks({{"on_timelapse_macro_restart_klipper",
                             [](lv_event_t*) { timelapse_macro_card().restart_klipper(); }}});
    callbacks_registered_ = true;
}

TimelapseMacroCard::Card TimelapseMacroCard::card() const {
    if (!subjects_initialized_) {
        return Card::None;
    }
    return static_cast<Card>(lv_subject_get_int(const_cast<lv_subject_t*>(&card_subject_)));
}

void TimelapseMacroCard::show(Card card) {
    if (subjects_initialized_) {
        lv_subject_set_int(&card_subject_, static_cast<int>(card));
    }
    if (card != Card::None && on_shown_) {
        on_shown_();
    }
}

void TimelapseMacroCard::clear() {
    ++generation_;
    macro_manager_.reset();
    on_shown_ = nullptr;
    show(Card::None);
}

void TimelapseMacroCard::refresh(IMoonrakerAPI* api, const PrinterDiscovery& discovery,
                                 bool moonraker_local, std::function<void()> on_shown) {
    clear();
    on_shown_ = std::move(on_shown);
    const unsigned gen = generation_;

    const MacroInstallStatus helix_status = MacroManager::evaluate_status(discovery);
    const std::string repo = timelapse_macro_repo_on_host(moonraker_local);
    TimelapseMacroFacts facts;
    facts.objects_reported = discovery.objects_reported();
    facts.has_frame_macro = discovery.has_macro(kTimelapseFrameMacro);
    facts.helix_macros_loaded = helix_status == MacroInstallStatus::INSTALLED ||
                                helix_status == MacroInstallStatus::OUTDATED;
    facts.macro_file_known = !repo.empty();
    const TimelapseMacroSetup setup = decide_timelapse_macro_setup(facts);
    spdlog::info("[Timelapse Macros] objects={} {}={} helix_macros={} macro_file={} -> {}",
                 facts.objects_reported, kTimelapseFrameMacro, facts.has_frame_macro,
                 facts.helix_macros_loaded, repo.empty() ? "unknown" : repo,
                 static_cast<int>(setup));

    switch (setup) {
    case TimelapseMacroSetup::NotYetKnown:
    case TimelapseMacroSetup::Loaded:
        return;
    case TimelapseMacroSetup::ShowIncludeLine:
        show(Card::IncludeLine);
        return;
    case TimelapseMacroSetup::IncludeFromHelixMacros:
        break;
    }

    if (!api) {
        show(Card::IncludeLine);
        return;
    }
    macro_manager_ = std::make_unique<MacroManager>(*api, discovery);
    // MacroManager lands both callbacks on the main thread.
    macro_manager_->include_timelapse_macros(
        repo,
        [this, gen]() {
            if (gen != generation_)
                return;
            spdlog::info("[Timelapse Macros] {} includes the timelapse macros; they load at the "
                         "next Klipper restart",
                         kHelixMacrosFilename);
            show(Card::RestartKlipper);
        },
        [this, gen](const MoonrakerError& err) {
            if (gen != generation_)
                return;
            spdlog::warn("[Timelapse Macros] Could not add the timelapse macros to {} ({}); "
                         "showing the line to add",
                         kHelixMacrosFilename, err.message);
            show(Card::IncludeLine);
        });
}

void TimelapseMacroCard::restart_klipper() {
    // The button greys while a job holds the machine (moves_machine); this is the backstop.
    if (job_holds_now()) {
        ToastManager::instance().show(
            ToastSeverity::INFO, lv_tr("A print is in progress - restart still pending"), 3000);
        return;
    }
    if (!macro_manager_ || card() != Card::RestartKlipper) {
        return;
    }
    spdlog::info("[Timelapse Macros] Restarting Klipper to load the timelapse macros");
    show(Card::None);
    // The restart passes through SHUTDOWN on its way back to READY.
    EmergencyStopOverlay::instance().suppress_recovery_dialog(recovery_suppression::NORMAL);
    const unsigned gen = generation_;
    // The same restart the helper macros activate with; both callbacks land on main.
    macro_manager_->request_restart(
        []() {
            ToastManager::instance().show(
                ToastSeverity::INFO,
                lv_tr("Klipper is restarting. Timelapse frames are captured from the next print."),
                4000);
        },
        [this, gen](const MoonrakerError& err) {
            spdlog::error("[Timelapse Macros] Klipper restart request failed: {}", err.message);
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to restart Klipper"),
                                          4000);
            if (gen == generation_)
                show(Card::RestartKlipper);
        });
}

} // namespace helix::ui
