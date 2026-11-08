// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_guided_device_flow.h"

#include "widget_subjects.h"

#if HELIX_HAS_CFS

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_cfs_chute_calibration_overlay.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "ams_backend.h"
#include "ams_backend_cfs.h"
#include "ams_cfs_calibration.h"
#include "ams_state.h"
#include "core/static_panel_registry.h"
#include "lvgl.h"

#include <spdlog/fmt/fmt.h>

#include <cctype>

namespace helix::ui {

namespace {

// The cutter sweep and the K2 belt tensioning each run a minute or more with no
// other visible progress, so one bound status line under the calibration
// buttons carries whichever is running: running, then the result or the
// failure. One line serves both because the backend runs one calibration at a
// time. A toast result would expire, and one queued behind a "started" toast
// stays hidden for its whole lifetime. The buttons' labels never change;
// ui_xml/components/cfs_cutter_status.xml renders these subjects (phase: 0
// nothing to report, 1 running, 2 result).
lv_subject_t s_cfs_cutter_phase{};
lv_subject_t s_cfs_cutter_status{};
char s_cfs_cutter_status_buf[128] = {};
helix::WidgetSubjects s_cfs_cutter_subjects{"CfsCutterSubjects",
                                            StaticSubjectRegistry::Lifetime::Process};

/// Subjects initialize once until exit (a second init would memzero a subject whose observers
/// are live) and always register in the global scope, which is where the status line's XML binds
/// them. A global registry that lost them is repopulated without re-initializing.
void ensure_cutter_subjects() {
    s_cfs_cutter_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_cfs_cutter_phase, 0, "cfs_cutter_phase", s_cfs_cutter_subjects);
        UI_MANAGED_SUBJECT_STRING(s_cfs_cutter_status, s_cfs_cutter_status_buf, "",
                                  "cfs_cutter_status", s_cfs_cutter_subjects);
    });
    if (lv_xml_get_subject(nullptr, "cfs_cutter_phase") != &s_cfs_cutter_phase) {
        lv_xml_register_subject(nullptr, "cfs_cutter_phase", &s_cfs_cutter_phase);
        lv_xml_register_subject(nullptr, "cfs_cutter_status", &s_cfs_cutter_status);
    }
}

void set_cutter_row_state(const char* status_text, bool busy) {
    ensure_cutter_subjects();
    lv_subject_copy_string(&s_cfs_cutter_status, status_text);
    // 0 = nothing to report (line hidden), 1 = running, 2 = result.
    lv_subject_set_int(&s_cfs_cutter_phase, busy ? 1 : (status_text[0] != '\0' ? 2 : 0));
    // The backend greys every calibration action while one runs; the section
    // shows that once rebuilt from them.
    // Never builds the overlay: a completion can land during teardown.
    if (auto* overlay = lazy_global_if_exists<AmsDeviceSectionDetailOverlay>()) {
        overlay->refresh();
    }
}

/// Cutter calibration confirm: the printer homes before it finds the cutter,
/// so the click asks before moving it. Only the K1 sweep has a measured duration.
void confirm_cutter_calibration() {
    AmsBackend* current = AmsState::instance().get_backend();
    auto* current_calibration = current ? current->as<ICfsCalibration>() : nullptr;
    const bool timed =
        current_calibration && current_calibration->cutter_calibration_takes_a_minute();
    helix::ui::ConfirmOptions opts;
    helix::ui::modal_confirm(
        lv_tr("Calibrate Cutter"),
        timed ? lv_tr("The printer homes and sweeps the cutter (about 1 minute). Keep the filament "
                      "path clear.")
              : lv_tr("The printer homes if needed, then finds the cutter's position. Keep the "
                      "filament path clear."),
        ModalSeverity::Warning, lv_tr("Start"),
        [timed]() {
            AmsBackend* backend = AmsState::instance().get_backend();
            auto* calibration = backend ? backend->as<ICfsCalibration>() : nullptr;
            if (!calibration) {
                return;
            }
            AmsError result = calibration->calibrate_cutter([](bool ok, const std::string& line) {
                char axis = '\0';
                double value_mm = 0.0;
                std::string text;
                if (ok &&
                    helix::printer::AmsBackendCfs::parse_cut_found_line(line, axis, value_mm)) {
                    text = fmt::format(
                        lv_tr("Cut position saved: {}"),
                        fmt::format("{} {:.1f}", static_cast<char>(std::toupper(axis)), value_mm));
                } else if (ok) {
                    text = lv_tr("Cutter calibration completed");
                } else {
                    text = fmt::format(lv_tr("Cutter calibration failed: {}"), line);
                }
                set_cutter_row_state(text.c_str(), false);
            });
            if (result.success()) {
                set_cutter_row_state(timed ? lv_tr("Calibrating... (about 1 minute)")
                                           : lv_tr("Calibrating..."),
                                     true);
            } else {
                set_cutter_row_state("", false);
                helix::ui::notify_ams_error(result);
            }
        },
        opts);
}

/// The section's one calibration status line, whichever of its flows attaches first.
void attach_calibration_status(lv_obj_t* actions_container) {
    ensure_cutter_subjects();
    if (lv_obj_find_by_name(actions_container, "cfs_cutter_status_line") == nullptr) {
        lv_xml_create(actions_container, "cfs_cutter_status", nullptr);
    }
}

/// Belt tensioning confirm: the printer's BELT_TENSION macro may home X/Y and
/// park the toolhead before the tensioners run, so the click asks first.
void confirm_belt_tension() {
    helix::ui::ConfirmOptions opts;
    helix::ui::modal_confirm(
        lv_tr("Tension Belts"),
        lv_tr("The printer may home and move the toolhead, then the motorized tensioners set the "
              "X and Y belts. Keep the bed and gantry clear."),
        ModalSeverity::Warning, lv_tr("Start"),
        []() {
            AmsBackend* backend = AmsState::instance().get_backend();
            auto* calibration = backend ? backend->as<ICfsCalibration>() : nullptr;
            if (!calibration) {
                return;
            }
            AmsError result = calibration->tension_belts([](bool ok, const std::string& detail) {
                const std::string text =
                    ok ? std::string(lv_tr("Belt tensioning completed"))
                       : fmt::format(lv_tr("Belt tensioning failed: {}"), detail);
                set_cutter_row_state(text.c_str(), false);
            });
            if (result.success()) {
                set_cutter_row_state(lv_tr("Tensioning belts..."), true);
            } else {
                set_cutter_row_state("", false);
                helix::ui::notify_ams_error(result);
            }
        },
        opts);
}

} // namespace

void register_cfs_device_flows() {
    // The chute flow is a guided overlay with its own step machine.
    register_guided_device_flow("cfs_purge_chute",
                                {nullptr, [](const std::string&) {
                                     get_cfs_chute_calibration_overlay().show(lv_screen_active());
                                 }});

    // The status line sits below the button rows; it is pure bound appearance and comes from
    // XML, driven by the file-static subjects.
    register_guided_device_flow("cfs_cutter", {attach_calibration_status, [](const std::string&) {
                                                   confirm_cutter_calibration();
                                               }});
    register_guided_device_flow(
        "cfs_belt_tension",
        {attach_calibration_status, [](const std::string&) { confirm_belt_tension(); }});
}

} // namespace helix::ui

#else

namespace helix::ui {
void register_cfs_device_flows() {}
} // namespace helix::ui

#endif
