// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// UpdateChecker's XML subjects, split from update_checker.cpp so builds without
// the network checker (the ESP32 firmware) register the same names and the
// update rows bind to an idle state instead of to nothing.

#include "helix_version.h"
#include "lvgl.h"
#include "subject_managed_panel.h"
#include "system/update_checker.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

void UpdateChecker::init_subjects() {
    if (subjects_initialized_)
        return;

    UI_MANAGED_SUBJECT_INT(status_subject_, static_cast<int>(Status::Idle), "update_status",
                           subjects_);
    // The Check for Updates row binds its status line straight to this subject, so an
    // empty default renders as a blank second line until the first check completes.
    // Seed it with the same "Version {}" idle text the settings root shows, reusing
    // that key rather than a printf-style duplicate.
    shown_ = {};
    UI_MANAGED_SUBJECT_STRING(version_text_subject_, version_text_buf_,
                              fmt::format(lv_tr("Version {}"), HELIX_VERSION).c_str(),
                              "update_version_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(new_version_subject_, new_version_buf_, "", "update_new_version",
                              subjects_);

    // Download subjects
    UI_MANAGED_SUBJECT_INT(download_status_subject_, static_cast<int>(DownloadStatus::Idle),
                           "download_status", subjects_);
    UI_MANAGED_SUBJECT_INT(download_progress_subject_, 0, "download_progress", subjects_);
    UI_MANAGED_SUBJECT_STRING(download_text_subject_, download_text_buf_, "", "download_text",
                              subjects_);

    // Notification subjects
    UI_MANAGED_SUBJECT_STRING(release_notes_subject_, release_notes_buf_, "",
                              "update_release_notes", subjects_);
    UI_MANAGED_SUBJECT_INT(changelog_visible_subject_, 0, "update_changelog_visible", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[UpdateChecker] LVGL subjects initialized");
}
