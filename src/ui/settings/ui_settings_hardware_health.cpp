// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_hardware_health.cpp
 * @brief Implementation of HardwareHealthOverlay
 */

#include "ui_settings_hardware_health.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_row_text.h"
#include "ui_severity_card.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "config.h"
#include "core/update_queue.h"
#include "hardware_validator.h"
#include "lvgl.h"
#include "printer_state.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::settings {

// ============================================================================
// LIFECYCLE HOOKS
// ============================================================================

void HardwareHealthOverlay::on_activate() {
    OverlayBase::on_activate();

    // Populate issues from validation result
    populate_hardware_issues();
}

void HardwareHealthOverlay::on_deactivating(DeactivateReason) {
    // Clean up any open modal dialog
    if (hardware_save_dialog_) {
        helix::ui::modal_hide(hardware_save_dialog_);
        hardware_save_dialog_ = nullptr;
    }
    pending_hardware_save_.clear();
}

// ============================================================================
// INTERNAL METHODS
// ============================================================================

void HardwareHealthOverlay::populate_hardware_issues() {
    if (!overlay_root_) {
        return;
    }

    if (!printer_state_) {
        spdlog::error("[{}] Cannot populate - printer_state_ not set", get_name());
        return;
    }

    const auto& result =
        printer_state_->hardware_validation_state().get_hardware_validation_result();

    // Helper to convert severity enum to string for XML attribute
    auto severity_to_string = [](HardwareIssueSeverity sev) -> const char* {
        switch (sev) {
        case HardwareIssueSeverity::CRITICAL:
            return "error";
        case HardwareIssueSeverity::WARNING:
            return "warning";
        case HardwareIssueSeverity::INFO:
        default:
            return "info";
        }
    };

    // Helper to populate a list with issues
    auto populate_list = [&](const char* list_name, const std::vector<HardwareIssue>& issues) {
        lv_obj_t* list = lv_obj_find_by_name(overlay_root_, list_name);
        if (!list) {
            spdlog::warn("[{}] Could not find list: {}", get_name(), list_name);
            return;
        }

        // Clear existing children
        helix::ui::safe_clean_children(list);

        // Add issue rows
        for (const auto& issue : issues) {
            // Create row with severity attribute for colored left border
            const std::string index = std::to_string(issue_names_.size());
            const char* attrs[] = {"severity", severity_to_string(issue.severity),
                                   "message",  issue.message.c_str(),
                                   "index",    index.c_str(),
                                   nullptr};
            lv_obj_t* row =
                static_cast<lv_obj_t*>(lv_xml_create(list, "hardware_issue_row", attrs));
            if (!row) {
                continue;
            }

            // Printer object names are printer data.
            helix::ui::set_row_label_text(row, "hardware_name", issue.hardware_name.c_str());
            issue_names_.push_back(issue.hardware_name);

            // Finalize severity_card to show correct icon
            ui_severity_card_finalize(row);
        }
    };

    // Rows in all four lists index this, so it restarts with them.
    issue_names_.clear();

    // Populate each section
    populate_list("critical_issues_list", result.critical_missing);
    populate_list("warning_issues_list", result.expected_missing);
    populate_list("info_issues_list", result.newly_discovered);
    populate_list("session_issues_list", result.changed_from_last_session);

    spdlog::debug("[{}] Populated hardware issues: {} critical, {} warning, {} info, {} session",
                  get_name(), result.critical_missing.size(), result.expected_missing.size(),
                  result.newly_discovered.size(), result.changed_from_last_session.size());
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void HardwareHealthOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_hardware_issue_ignore", [](lv_event_t* e) { on_issue_action(e, true); }},
        {"on_hardware_issue_save", [](lv_event_t* e) { on_issue_action(e, false); }},
    });
}

void HardwareHealthOverlay::on_issue_action(lv_event_t* e, bool is_ignore) {
    auto& self = get_hardware_health_overlay();
    const auto index = helix::ui::event_user_int(e);
    if (!index || *index < 0 || static_cast<size_t>(*index) >= self.issue_names_.size())
        return;
    // Copied: acting on it rebuilds the rows and the names with them.
    const std::string name = self.issue_names_[static_cast<size_t>(*index)];
    self.handle_hardware_action(name.c_str(), is_ignore);
}

void HardwareHealthOverlay::handle_hardware_action(const char* hardware_name, bool is_ignore) {
    if (!hardware_name) {
        return;
    }

    Config* config = Config::get_instance();
    std::string hw_name(hardware_name);

    if (is_ignore) {
        // "Ignore" - Mark hardware as optional (no confirmation needed)
        HardwareValidator::set_hardware_optional(config, hw_name, true);
        ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Hardware marked as optional"),
                                      2000);
        spdlog::info("[{}] Marked hardware '{}' as optional", get_name(), hw_name);

        // Remove from cached validation result and refresh overlay
        if (printer_state_) {
            printer_state_->hardware_validation_state().remove_hardware_issue(hw_name);
        }
        // The Ignore button that fired this event is a child of the list cleaned by
        // populate_hardware_issues(). Defer the rebuild off the click stack; the actual
        // batch-escape comes from safe_clean_children() inside populate_hardware_issues
        // which routes per-child deletes through lv_obj_delete_async [L081].
        lifetime_.defer("HardwareHealthOverlay::rebuild", [this]() {
            if (is_created() && is_visible())
                populate_hardware_issues();
        });
    } else {
        // "Save" - Add to expected hardware (with confirmation)
        // Re-entry guard: two rows tapped in quick succession must not stack
        // two dialogs over the same pending name.
        if (hardware_save_dialog_) {
            helix::ui::modal_hide(hardware_save_dialog_);
            hardware_save_dialog_ = nullptr;
        }

        // Store name for confirmation callback
        pending_hardware_save_ = hw_name;

        char message_buf[256];
        snprintf(message_buf, sizeof(message_buf),
                 "Add '%s' to expected hardware?\n\nYou'll be notified if it's removed later.",
                 hw_name.c_str());

        helix::ui::ConfirmOptions opts;
        opts.on_cancel = [this] { handle_hardware_save_cancel(); };
        opts.on_dismiss = [this] {
            // Backdrop tap / ESC strands the same pending name cancel clears
            hardware_save_dialog_ = nullptr;
            pending_hardware_save_.clear();
        };
        opts.owner_token = lifetime_.token();

        // The dialog closes itself on either button; the stored handle only
        // feeds the close-existing guard above.
        hardware_save_dialog_ = helix::ui::modal_confirm(
            lv_tr("Save Hardware"), message_buf, ModalSeverity::Info, lv_tr("Save"),
            [this] { handle_hardware_save_confirm(); }, opts);
    }
}

void HardwareHealthOverlay::handle_hardware_save_confirm() {
    // Runs from the dialog's confirm callback, BEFORE it self-closes; drop the
    // stale handle now so nothing re-hides an exiting/reused dialog address.
    hardware_save_dialog_ = nullptr;

    Config* cfg = Config::get_instance();

    // Add to expected hardware list
    HardwareValidator::add_expected_hardware(cfg, pending_hardware_save_);
    ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Hardware saved to config"), 2000);
    spdlog::info("[{}] Added hardware '{}' to expected list", get_name(), pending_hardware_save_);

    // Remove from cached validation result and refresh overlay
    if (printer_state_) {
        printer_state_->hardware_validation_state().remove_hardware_issue(pending_hardware_save_);
    }
    // Defer rebuild for consistency with the Ignore path. The actual batch-escape
    // comes from safe_clean_children() inside populate_hardware_issues [L081].
    {
        lifetime_.defer("HardwareHealthOverlay::rebuild", [this]() {
            if (is_created() && is_visible())
                populate_hardware_issues();
        });
    }
    pending_hardware_save_.clear();
}

void HardwareHealthOverlay::handle_hardware_save_cancel() {
    // Runs from the dialog's cancel callback, BEFORE it self-closes; drop the
    // stale handle now so nothing re-hides an exiting/reused dialog address.
    hardware_save_dialog_ = nullptr;

    pending_hardware_save_.clear();
}

} // namespace helix::settings
