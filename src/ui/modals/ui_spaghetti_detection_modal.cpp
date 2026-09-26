// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spaghetti_detection_modal.h"

#include "ui_toast_manager.h"

#include "abort_manager.h"
#include "app_globals.h"
#include "i_moonraker_api.h"
#include "settings_manager.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <memory>

lv_subject_t SpaghettiDetectionModal::tune_available_subject_;
char SpaghettiDetectionModal::message_buf_[256];
lv_subject_t SpaghettiDetectionModal::message_subject_;
SubjectManager SpaghettiDetectionModal::subjects_;
bool SpaghettiDetectionModal::subjects_initialized_ = false;

void SpaghettiDetectionModal::init_subjects() {
    if (subjects_initialized_)
        return;
    UI_MANAGED_SUBJECT_INT(tune_available_subject_, 0, "spaghetti_tune_available", subjects_);
    UI_MANAGED_SUBJECT_STRING(message_subject_, message_buf_, "", "spaghetti_message", subjects_);
    subjects_initialized_ = true;

    // The subjects must die before lv_deinit(), and a re-initialised LVGL
    // must see them registered again.
    StaticSubjectRegistry::instance().register_deinit("SpaghettiDetectionModal", [] {
        if (!subjects_initialized_)
            return;
        subjects_.deinit_all();
        subjects_initialized_ = false;
    });
}

void SpaghettiDetectionModal::on_show() {
    // Wire the four action buttons programmatically (mirrors the runout-guidance
    // modal). No XML callbacks on these buttons, so there's no double-wiring.
    //   btn_primary    → on_ok()        → Resume
    //   btn_secondary  → on_cancel()    → Abort
    //   btn_tertiary   → on_tertiary()  → Tune (hidden via subject when the source can't tune)
    //   btn_quaternary → on_quaternary()→ Turn off detection
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    wire_tertiary_button("btn_tertiary");
    wire_quaternary_button("btn_quaternary");

    // Publish what the XML binds: a dead Tune button is worse than none, so
    // its flag (with its divider's) rides spaghetti_tune_available, and the
    // message text rides spaghetti_message.
    lv_subject_set_int(&tune_available_subject_, on_tune_ ? 1 : 0);
    lv_subject_copy_string(&message_subject_, message_.c_str());

    // Optional camera frame preview. Hide the image entirely when no frame is
    // available so it doesn't reserve empty space.
    lv_obj_t* preview = find_widget("detection_preview");
    if (preview) {
        if (frame_) {
            lv_image_set_src(preview, frame_);
            lv_obj_remove_flag(preview, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        spdlog::warn("[SpaghettiDetectionModal] detection_preview widget not found");
    }
}

namespace helix::detection {

void present_detection(const DetectionEvent& e, DetectionPolicy p) {
    using DetectionResponse = helix::detection::DetectionResponse;
    const auto response = DetectionManager::instance().response_for(p);
    if (response == DetectionResponse::Suppressed)
        return;
    // Warn-only still owes a self-paused print the Resume/Abort decision: a
    // print the firmware already paused cannot be left on a vanishing toast
    // with no path forward, so it escalates to the modal below.
    if (response == DetectionResponse::WarnOnly && !e.already_paused) {
        ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Spaghetti detected"), 8000);
        return;
    }
    // A print the source did not already pause pauses here; sources only
    // report. already_paused covers both pause-on-detect on (a firmware
    // pause makes ours redundant) and the warn-only escalation above.
    if (!e.already_paused) {
        get_moonraker_api()->job().pause_print([] { spdlog::info("[Detection] print paused"); },
                                               [](const MoonrakerError& err) {
                                                   spdlog::warn("[Detection] pause failed: {}",
                                                                err.message);
                                               });
    }
    // Stack-owned via Modal::show_owned() (#1382): ModalStack frees the
    // instance when its entry goes, on every teardown path.
    auto modal = std::make_unique<SpaghettiDetectionModal>();
    // TODO(#1506): no frame yet; the modal shows the detector's text only.
    // A confidence-bearing source (K2) gets the translated line; a source
    // whose message is the printer's own string (U1) shows that verbatim.
    char display[128];
    if (e.confidence) {
        const int pct = static_cast<int>(*e.confidence * 100.0f + 0.5f);
        snprintf(display, sizeof(display), lv_tr("Spaghetti detected (%d%%)"), pct);
    } else {
        snprintf(display, sizeof(display), "%s", e.message.c_str());
    }
    modal->set_detection(display, nullptr);
    modal->set_on_resume(
        [] { get_moonraker_api()->job().resume_print([] {}, [](const MoonrakerError&) {}); });
    modal->set_on_abort([] { helix::AbortManager::instance().start_abort(); });
    modal->set_on_disable([] {
        SettingsManager::instance().set_detection_enabled(false);
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Detection turned off"), 4000);
    });
    // The source owns its tuning command; can_tune() sources that run their
    // own model (the K2 polls it directly) decline the button.
    if (DetectionManager::instance().source_can_tune(e.source_id))
        modal->set_on_tune([id = e.source_id] { DetectionManager::instance().tune_source(id); });
    Modal::show_owned(std::move(modal), lv_screen_active());
}

} // namespace helix::detection
