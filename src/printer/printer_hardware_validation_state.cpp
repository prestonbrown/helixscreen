// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_hardware_validation_state.cpp
 * @brief Hardware validation state management extracted from PrinterState
 *
 * Manages hardware validation subjects for UI display including issue counts,
 * severity levels, and formatted status text for the Settings panel.
 *
 * Extracted from PrinterState as part of god class decomposition.
 */

#include "printer_hardware_validation_state.h"

#include "i18n_plural.h"
#include "lvgl.h"
#include "state/subject_macros.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>

namespace helix {

void PrinterHardwareValidationState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterHardwareValidationState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterHardwareValidationState] Initializing subjects (register_xml={})",
                  register_xml);

    // Initialize hardware validation subjects
    INIT_SUBJECT_INT(hardware_status_level, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(hardware_critical_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(hardware_warning_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(hardware_info_count, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(hardware_session_count, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(hardware_status_title, lv_tr("Healthy"), subjects_, register_xml);
    INIT_SUBJECT_STRING(hardware_status_detail, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(hardware_issues_label, lv_tr("No Hardware Issues"), subjects_,
                        register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterHardwareValidationState] Subjects initialized successfully");
}

void PrinterHardwareValidationState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterHardwareValidationState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterHardwareValidationState::set_hardware_validation_result(
    const HardwareValidationResult& result) {
    // Store the full result for UI access
    hardware_validation_result_ = result;
    has_result_ = true;

    // Update summary subjects
    lv_subject_set_int(&hardware_status_level_, static_cast<int>(result.headline_level()));

    // Update category counts
    lv_subject_set_int(&hardware_critical_count_, static_cast<int>(result.critical_missing.size()));
    lv_subject_set_int(&hardware_warning_count_, static_cast<int>(result.expected_missing.size()));
    lv_subject_set_int(&hardware_info_count_, static_cast<int>(result.newly_discovered.size()));
    lv_subject_set_int(&hardware_session_count_,
                       static_cast<int>(result.changed_from_last_session.size()));

    // Update status text
    if (!result.has_issues()) {
        snprintf(hardware_status_title_buf_, sizeof(hardware_status_title_buf_), "%s",
                 lv_tr("All Healthy"));
        snprintf(hardware_status_detail_buf_, sizeof(hardware_status_detail_buf_), "%s",
                 lv_tr("All configured hardware detected"));
    } else {
        size_t total = result.total_issue_count();
        snprintf(
            hardware_status_title_buf_, sizeof(hardware_status_title_buf_), "%s",
            fmt::format(
                fmt::runtime(ui::tr_plural("1 Issue Detected", "{} Issues Detected", total)), total)
                .c_str());

        // Build detail string
        std::string detail;
        if (!result.critical_missing.empty()) {
            detail += fmt::format(lv_tr("{} critical"), result.critical_missing.size());
        }
        if (!result.expected_missing.empty()) {
            if (!detail.empty())
                detail += ", ";
            detail += fmt::format(lv_tr("{} missing"), result.expected_missing.size());
        }
        if (!result.newly_discovered.empty()) {
            if (!detail.empty())
                detail += ", ";
            detail += fmt::format(lv_tr("{} new"), result.newly_discovered.size());
        }
        if (!result.changed_from_last_session.empty()) {
            if (!detail.empty())
                detail += ", ";
            detail += fmt::format(lv_tr("{} changed"), result.changed_from_last_session.size());
        }
        snprintf(hardware_status_detail_buf_, sizeof(hardware_status_detail_buf_), "%s",
                 detail.c_str());
    }
    lv_subject_copy_string(&hardware_status_title_, hardware_status_title_buf_);
    lv_subject_copy_string(&hardware_status_detail_, hardware_status_detail_buf_);

    // Update issues label for settings panel ("1 Hardware Issue" / "5 Hardware Issues")
    size_t total = result.total_issue_count();
    if (total == 0) {
        snprintf(hardware_issues_label_buf_, sizeof(hardware_issues_label_buf_), "%s",
                 lv_tr("No Hardware Issues"));
    } else {
        snprintf(
            hardware_issues_label_buf_, sizeof(hardware_issues_label_buf_), "%s",
            fmt::format(
                fmt::runtime(ui::tr_plural("1 Hardware Issue", "{} Hardware Issues", total)), total)
                .c_str());
    }
    lv_subject_copy_string(&hardware_issues_label_, hardware_issues_label_buf_);

    spdlog::debug("[PrinterHardwareValidationState] Hardware validation updated: {} issues, "
                  "status_level={}",
                  result.total_issue_count(), static_cast<int>(result.headline_level()));
}

void PrinterHardwareValidationState::refresh_texts() {
    if (subjects_initialized_ && has_result_) {
        set_hardware_validation_result(hardware_validation_result_);
    }
}

void PrinterHardwareValidationState::remove_hardware_issue(const std::string& hardware_name) {
    // Helper lambda to remove an issue from a vector by hardware_name
    auto remove_by_name = [&hardware_name](std::vector<HardwareIssue>& issues) {
        issues.erase(std::remove_if(issues.begin(), issues.end(),
                                    [&hardware_name](const HardwareIssue& issue) {
                                        return issue.hardware_name == hardware_name;
                                    }),
                     issues.end());
    };

    // Remove from all issue lists
    remove_by_name(hardware_validation_result_.critical_missing);
    remove_by_name(hardware_validation_result_.expected_missing);
    remove_by_name(hardware_validation_result_.newly_discovered);
    remove_by_name(hardware_validation_result_.changed_from_last_session);

    // Re-apply the updated result to refresh all subjects
    set_hardware_validation_result(hardware_validation_result_);

    spdlog::debug("[PrinterHardwareValidationState] Removed hardware issue: {}", hardware_name);
}

} // namespace helix
