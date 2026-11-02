// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_guided_device_flow.h"

#if HELIX_HAS_CFS

#include "ui_ams_device_section_detail_overlay.h"
#include "ui_cfs_chute_calibration_overlay.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"

#include "ams_backend.h"
#include "ams_backend_cfs.h"
#include "ams_state.h"
#include "lvgl.h"
#include "static_panel_registry.h"

#include <spdlog/fmt/fmt.h>

#include <cctype>

namespace helix::ui {

namespace {

// The cutter sweep runs ~60s with no other visible progress, so a bound
// status line under the calibration buttons carries the flow's state: running,
// then the found position or the failure. A toast result would expire, and one
// queued behind a "started" toast stays hidden for its whole lifetime. The
// buttons' labels never change; ui_xml/components/cfs_cutter_status.xml
// renders these subjects (phase: 0 nothing to report, 1 running, 2 result).
lv_subject_t s_cfs_cutter_phase{};
lv_subject_t s_cfs_cutter_status{};
char s_cfs_cutter_status_buf[128] = {};
bool s_cfs_cutter_subjects_ready = false;

/// Subjects initialize once per process (a second init would memzero a subject whose observers
/// are live) and always register in the global scope, which is where the status line's XML binds
/// them. A global registry that lost them is repopulated without re-initializing.
void ensure_cutter_subjects() {
    if (!s_cfs_cutter_subjects_ready) {
        s_cfs_cutter_subjects_ready = true;
        lv_subject_init_int(&s_cfs_cutter_phase, 0);
        lv_subject_init_string(&s_cfs_cutter_status, s_cfs_cutter_status_buf, nullptr,
                               sizeof(s_cfs_cutter_status_buf), "");
    }
    if (lv_xml_get_subject(nullptr, "cfs_cutter_phase") != &s_cfs_cutter_phase) {
        lv_xml_register_subject(nullptr, "cfs_cutter_phase", &s_cfs_cutter_phase);
        lv_xml_register_subject(nullptr, "cfs_cutter_status", &s_cfs_cutter_status);
    }
}

void set_cutter_row_state(const char* status_text, bool busy) {
    ensure_cutter_subjects();
    lv_subject_copy_string(&s_cfs_cutter_status, status_text);
    // 0 = nothing to report (line hidden), 1 = sweep running, 2 = result.
    lv_subject_set_int(&s_cfs_cutter_phase, busy ? 1 : (status_text[0] != '\0' ? 2 : 0));
    // The backend greys both calibration actions while one runs; the section
    // shows that once rebuilt from them.
    // Never builds the overlay: a completion can land during teardown.
    if (auto* overlay = lazy_global_if_exists<AmsDeviceSectionDetailOverlay>()) {
        overlay->refresh();
    }
}

/// Cutter calibration confirm: BOX_FIND_CUT_POS homes X/Y and takes about a
/// minute, so the click asks before moving the printer.
void confirm_cutter_calibration() {
    helix::ui::ConfirmOptions opts;
    helix::ui::modal_confirm(
        lv_tr("Calibrate Cutter"),
        lv_tr("The printer homes and sweeps the cutter (about 1 minute). Keep the filament "
              "path clear."),
        ModalSeverity::Warning, lv_tr("Start"),
        []() {
            AmsBackend* backend = AmsState::instance().get_backend();
            if (!backend) {
                return;
            }
            AmsError result = backend->calibrate_cutter([](bool ok, const std::string& line) {
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
                set_cutter_row_state(lv_tr("Calibrating... (about 1 minute)"), true);
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
    register_guided_device_flow("cfs_cutter",
                                {[](lv_obj_t* actions_container) {
                                     ensure_cutter_subjects();
                                     lv_xml_create(actions_container, "cfs_cutter_status", nullptr);
                                 },
                                 [](const std::string&) { confirm_cutter_calibration(); }});
}

} // namespace helix::ui

#else

namespace helix::ui {
void register_cfs_device_flows() {}
} // namespace helix::ui

#endif
