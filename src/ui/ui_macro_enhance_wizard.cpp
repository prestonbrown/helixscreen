// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_macro_enhance_wizard.h"

#include "ui_update_queue.h"

#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix::ui {

// Static member initialization
bool MacroEnhanceWizard::callbacks_registered_ = false;

// ============================================================================
// Construction / Destruction
// ============================================================================

MacroEnhanceWizard::MacroEnhanceWizard() {
    init_subjects();
    register_callbacks();
    spdlog::debug("[MacroEnhanceWizard] Constructed");
}

MacroEnhanceWizard::~MacroEnhanceWizard() {
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&step_title_subject_);
        lv_subject_deinit(&step_progress_subject_);
        lv_subject_deinit(&description_subject_);
        lv_subject_deinit(&diff_preview_subject_);
        lv_subject_deinit(&summary_subject_);
        lv_subject_deinit(&backup_text_subject_);
        lv_subject_deinit(&state_subject_);
        lv_subject_deinit(&show_operation_subject_);
        lv_subject_deinit(&show_summary_subject_);
        lv_subject_deinit(&show_applying_subject_);
        lv_subject_deinit(&show_success_subject_);
        lv_subject_deinit(&show_error_subject_);
        subjects_initialized_ = false;
    }
}

void MacroEnhanceWizard::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize all subjects with empty strings
    lv_subject_init_pointer(&step_title_subject_, step_title_buf_);
    lv_subject_init_pointer(&step_progress_subject_, step_progress_buf_);
    lv_subject_init_pointer(&description_subject_, description_buf_);
    lv_subject_init_pointer(&diff_preview_subject_, diff_preview_buf_);
    lv_subject_init_pointer(&summary_subject_, summary_buf_);
    lv_subject_init_pointer(&backup_text_subject_, backup_text_buf_);
    lv_subject_init_int(&state_subject_, static_cast<int>(MacroEnhanceState::OPERATION));

    // Boolean visibility subjects - initial state is OPERATION (0)
    // Using bind_flag_if_eq pattern: 1 = visible, 0 = hidden
    lv_subject_init_int(&show_operation_subject_, 1); // Start visible
    lv_subject_init_int(&show_summary_subject_, 0);
    lv_subject_init_int(&show_applying_subject_, 0);
    lv_subject_init_int(&show_success_subject_, 0);
    lv_subject_init_int(&show_error_subject_, 0);

    // Register subjects for XML binding [CRITICAL: Without this, XML bindings silently fail]
    lv_xml_register_subject(nullptr, "macro_enhance_step_title", &step_title_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_step_progress", &step_progress_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_description", &description_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_diff_preview", &diff_preview_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_summary", &summary_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_backup_text", &backup_text_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_state", &state_subject_);

    // Register boolean visibility subjects
    lv_xml_register_subject(nullptr, "macro_enhance_show_operation", &show_operation_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_show_summary", &show_summary_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_show_applying", &show_applying_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_show_success", &show_success_subject_);
    lv_xml_register_subject(nullptr, "macro_enhance_show_error", &show_error_subject_);

    subjects_initialized_ = true;
}

void MacroEnhanceWizard::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    lv_xml_register_event_cb(nullptr, "on_macro_enhance_skip", on_skip_cb);
    lv_xml_register_event_cb(nullptr, "on_macro_enhance_approve", on_approve_cb);
    lv_xml_register_event_cb(nullptr, "on_macro_enhance_cancel", on_cancel_cb);
    lv_xml_register_event_cb(nullptr, "on_macro_enhance_apply", on_apply_cb);
    lv_xml_register_event_cb(nullptr, "on_macro_enhance_close", on_close_cb);

    callbacks_registered_ = true;
}

// ============================================================================
// Setup
// ============================================================================

void MacroEnhanceWizard::set_analysis(const helix::PrintStartAnalysis& analysis) {
    analysis_ = analysis;
    operations_.clear();
    enhancements_.clear();
    current_op_index_ = 0;

    // Log what we're working with
    auto uncontrollable = analysis_.get_uncontrollable_operations();
    spdlog::debug("[MacroEnhanceWizard] Analysis: {} total ops, {} uncontrollable",
                  analysis_.operations.size(), uncontrollable.size());

    // Collect uncontrollable operations (excluding homing which shouldn't be skipped)
    for (const auto* op : uncontrollable) {
        spdlog::debug("[MacroEnhanceWizard] Uncontrollable op: {} (category={}, has_skip={})",
                      op->name, static_cast<int>(op->category), op->has_skip_param);
        if (op->category != helix::PrintStartOpCategory::HOMING) {
            operations_.push_back(op);
        } else {
            spdlog::debug("[MacroEnhanceWizard] Skipping homing operation");
        }
    }

    spdlog::debug("[MacroEnhanceWizard] Found {} operations to enhance", operations_.size());
}

// ============================================================================
// Show / Hide
// ============================================================================

bool MacroEnhanceWizard::show(lv_obj_t* parent) {
    spdlog::debug("[MacroEnhanceWizard] show() called: visible={}, api={}, operations={}",
                  is_visible(), static_cast<void*>(api_), operations_.size());

    if (is_visible()) {
        spdlog::warn("[MacroEnhanceWizard] Wizard already open");
        return false;
    }

    if (api_ == nullptr) {
        spdlog::error("[MacroEnhanceWizard] API not set");
        return false;
    }

    if (operations_.empty()) {
        spdlog::warn("[MacroEnhanceWizard] No operations to enhance - nothing for wizard to do");
        return false;
    }

    // Reset state
    state_ = MacroEnhanceState::OPERATION;
    current_op_index_ = 0;
    enhancements_.clear();

    // Initialize subjects BEFORE Modal::show() calls lv_xml_create() [L004]
    // XML bindings like bind_text="macro_enhance_step_title" require subjects to exist
    init_subjects();

    // CRITICAL: Set visibility subjects BEFORE Modal::show() creates XML
    // XML bindings evaluate during creation, so subjects must have correct values.
    // Using bind_flag_if_eq pattern: 1 = visible, 0 = hidden
    lv_subject_set_int(&state_subject_, static_cast<int>(state_));
    lv_subject_set_int(&show_operation_subject_, 1);
    lv_subject_set_int(&show_summary_subject_, 0);
    lv_subject_set_int(&show_applying_subject_, 0);
    lv_subject_set_int(&show_success_subject_, 0);
    lv_subject_set_int(&show_error_subject_, 0);

    // Set dynamic backup checkbox text using source file from analysis
    snprintf(backup_text_buf_, sizeof(backup_text_buf_),
             lv_tr("Create backup of %s before applying"),
             analysis_.source_file.empty() ? "printer.cfg" : analysis_.source_file.c_str());
    lv_subject_set_pointer(&backup_text_subject_, backup_text_buf_);

    // Use Modal base class to show
    if (!Modal::show(parent)) {
        spdlog::error("[MacroEnhanceWizard] Failed to show modal");
        return false;
    }

    spdlog::info("[MacroEnhanceWizard] Wizard opened with {} operations", operations_.size());
    return true;
}

size_t MacroEnhanceWizard::get_approved_count() const {
    return std::count_if(enhancements_.begin(), enhancements_.end(),
                         [](const helix::MacroEnhancement& e) { return e.user_approved; });
}

// ============================================================================
// Modal Hooks
// ============================================================================

void MacroEnhanceWizard::on_show() {
    // Store 'this' in modal's user_data for callback traversal
    lv_obj_set_user_data(dialog(), this);

    bind_subjects_to_widgets();
    show_current_operation();
}

void MacroEnhanceWizard::on_hide() {
    // Guard against cleanup during LVGL shutdown [SERIOUS-6]
    if (!lv_is_initialized()) {
        return;
    }

    // Deinitialize subjects to properly remove all attached observers.
    // We use lv_subject_deinit() instead of lv_observer_remove() because
    // widget-bound observers can be auto-removed by LVGL when widgets are
    // deleted, leaving dangling pointers. Working from the subject side is safe.
    lv_subject_deinit(&step_title_subject_);
    lv_subject_deinit(&step_progress_subject_);
    lv_subject_deinit(&description_subject_);
    lv_subject_deinit(&diff_preview_subject_);
    lv_subject_deinit(&summary_subject_);
    lv_subject_deinit(&backup_text_subject_);
    lv_subject_deinit(&state_subject_);
    lv_subject_deinit(&show_operation_subject_);
    lv_subject_deinit(&show_summary_subject_);
    lv_subject_deinit(&show_applying_subject_);
    lv_subject_deinit(&show_success_subject_);
    lv_subject_deinit(&show_error_subject_);
    subjects_initialized_ = false;
}

void MacroEnhanceWizard::bind_subjects_to_widgets() {
    // Bind subjects to UI elements using Modal's find_widget() helper
    lv_obj_t* step_title = find_widget("step_title");
    if (step_title != nullptr) {
        step_title_observer_ = lv_label_bind_text(step_title, &step_title_subject_, "%s");
    }

    lv_obj_t* step_progress = find_widget("step_progress");
    if (step_progress != nullptr) {
        step_progress_observer_ = lv_label_bind_text(step_progress, &step_progress_subject_, "%s");
    }

    lv_obj_t* description = find_widget("operation_description");
    if (description != nullptr) {
        description_observer_ = lv_label_bind_text(description, &description_subject_, "%s");
    }

    lv_obj_t* diff_preview = find_widget("diff_preview");
    if (diff_preview != nullptr) {
        diff_preview_observer_ = lv_label_bind_text(diff_preview, &diff_preview_subject_, "%s");
    }

    lv_obj_t* summary_list = find_widget("summary_list");
    if (summary_list != nullptr) {
        summary_observer_ = lv_label_bind_text(summary_list, &summary_subject_, "%s");
    }

    // Bind shared description subject to multiple widgets [L020: Track all observers]
    lv_obj_t* applying_status = find_widget("applying_status");
    if (applying_status != nullptr) {
        applying_status_observer_ =
            lv_label_bind_text(applying_status, &description_subject_, "%s");
    }

    lv_obj_t* success_message = find_widget("success_message");
    if (success_message != nullptr) {
        success_message_observer_ =
            lv_label_bind_text(success_message, &description_subject_, "%s");
    }

    lv_obj_t* error_message = find_widget("error_message");
    if (error_message != nullptr) {
        error_message_observer_ = lv_label_bind_text(error_message, &description_subject_, "%s");
    }

    // Bind backup label for dynamic source_file text
    lv_obj_t* backup_label = find_widget("backup_label");
    if (backup_label != nullptr) {
        backup_label_observer_ = lv_label_bind_text(backup_label, &backup_text_subject_, "%s");
    }

    // Set initial state
    lv_subject_set_int(&state_subject_, static_cast<int>(state_));
}

// ============================================================================
// UI Updates
// ============================================================================

void MacroEnhanceWizard::update_ui() {
    if (!is_visible()) {
        return;
    }

    // Update state subject (kept for any direct bindings)
    lv_subject_set_int(&state_subject_, static_cast<int>(state_));

    // Update boolean visibility subjects based on current state
    // Using bind_flag_if_eq pattern: 1 = visible, 0 = hidden
    int s = static_cast<int>(state_);
    lv_subject_set_int(&show_operation_subject_, s == 0 ? 1 : 0);
    lv_subject_set_int(&show_summary_subject_, s == 1 ? 1 : 0);
    lv_subject_set_int(&show_applying_subject_, s == 2 ? 1 : 0);
    lv_subject_set_int(&show_success_subject_, s == 3 ? 1 : 0);
    lv_subject_set_int(&show_error_subject_, s == 4 ? 1 : 0);

    // Update close button visibility for terminal states
    update_close_button_visibility();
}

void MacroEnhanceWizard::show_current_operation() {
    if (current_op_index_ >= operations_.size()) {
        // No more operations, show summary
        show_summary();
        return;
    }

    state_ = MacroEnhanceState::OPERATION;
    const auto* op = operations_[current_op_index_];

    // Get friendly name for category
    std::string friendly_name;
    switch (op->category) {
    case helix::PrintStartOpCategory::BED_MESH:
        friendly_name = "Bed Mesh"; // i18n: do not translate, universal 3D printing term
        break;
    case helix::PrintStartOpCategory::QGL:
        friendly_name =
            "Quad Gantry Leveling"; // i18n: do not translate, universal 3D printing term
        break;
    case helix::PrintStartOpCategory::Z_TILT:
        friendly_name = "Z-Tilt Adjustment"; // i18n: do not translate, universal 3D printing term
        break;
    case helix::PrintStartOpCategory::NOZZLE_CLEAN:
        friendly_name = "Nozzle Cleaning"; // i18n: do not translate, universal 3D printing term
        break;
    default:
        friendly_name = op->name;
        break;
    }

    // Update title
    snprintf(step_title_buf_, sizeof(step_title_buf_), lv_tr("Make %s Optional?"),
             friendly_name.c_str());
    lv_subject_set_pointer(&step_title_subject_, step_title_buf_);

    // Update progress
    snprintf(step_progress_buf_, sizeof(step_progress_buf_), "%zu of %zu", current_op_index_ + 1,
             operations_.size());
    lv_subject_set_pointer(&step_progress_subject_, step_progress_buf_);

    // Update description with user-friendly text
    snprintf(description_buf_, sizeof(description_buf_),
             lv_tr("When starting a print, you'll be able to choose whether to run %s. "
                   "This saves time when you've already done it recently or want more "
                   "control over your print preparation."),
             friendly_name.c_str());
    lv_subject_set_pointer(&description_subject_, description_buf_);

    update_ui();
}

void MacroEnhanceWizard::show_summary() {
    state_ = MacroEnhanceState::SUMMARY;

    size_t approved_count = get_approved_count();

    // Update title
    snprintf(step_title_buf_, sizeof(step_title_buf_), "%s", lv_tr("Ready to Apply"));
    lv_subject_set_pointer(&step_title_subject_, step_title_buf_);

    // Update progress
    snprintf(step_progress_buf_, sizeof(step_progress_buf_), lv_tr("%zu changes"), approved_count);
    lv_subject_set_pointer(&step_progress_subject_, step_progress_buf_);

    // Build summary list with friendly copy
    std::string summary;
    if (approved_count == 0) {
        summary = lv_tr("No changes selected.\n\nClick Cancel to close.");
    } else {
        summary = lv_tr("Your PRINT_START macro will be updated to give you control over:\n\n");
        for (const auto& e : enhancements_) {
            if (e.user_approved) {
                // Use friendly names in the summary too
                std::string friendly = e.operation_name;
                if (e.category == helix::PrintStartOpCategory::BED_MESH) {
                    friendly = "Bed Mesh"; // i18n: do not translate
                } else if (e.category == helix::PrintStartOpCategory::QGL) {
                    friendly = "Quad Gantry Leveling"; // i18n: do not translate
                } else if (e.category == helix::PrintStartOpCategory::Z_TILT) {
                    friendly = "Z-Tilt Adjustment"; // i18n: do not translate
                } else if (e.category == helix::PrintStartOpCategory::NOZZLE_CLEAN) {
                    friendly = "Nozzle Cleaning"; // i18n: do not translate
                }
                summary += "  \xE2\x80\xA2 " + friendly + "\n"; // UTF-8 bullet
            }
        }
        summary += "\n";
        summary += lv_tr("Changes can be reversed anytime using the Macro Viewer.");
    }

    snprintf(summary_buf_, sizeof(summary_buf_), "%s", summary.c_str());
    lv_subject_set_pointer(&summary_subject_, summary_buf_);

    update_ui();
}

void MacroEnhanceWizard::show_applying(const std::string& status) {
    state_ = MacroEnhanceState::APPLYING;

    snprintf(step_title_buf_, sizeof(step_title_buf_), "%s", lv_tr("Applying Changes"));
    lv_subject_set_pointer(&step_title_subject_, step_title_buf_);

    step_progress_buf_[0] = '\0'; // Clear progress text
    lv_subject_set_pointer(&step_progress_subject_, step_progress_buf_);

    snprintf(description_buf_, sizeof(description_buf_), "%s", status.c_str());
    lv_subject_set_pointer(&description_subject_, description_buf_);

    update_ui();
}

void MacroEnhanceWizard::show_success(const std::string& /* message */) {
    state_ = MacroEnhanceState::SUCCESS;

    snprintf(step_title_buf_, sizeof(step_title_buf_), "%s", lv_tr("Setup Complete!"));
    lv_subject_set_pointer(&step_title_subject_, step_title_buf_);

    step_progress_buf_[0] = '\0'; // Clear progress text
    lv_subject_set_pointer(&step_progress_subject_, step_progress_buf_);

    // Use friendly success message instead of technical details
    snprintf(description_buf_, sizeof(description_buf_), "%s",
             lv_tr("You can now skip these operations when starting prints.\n\n"
                   "Look for the new options in the print details before starting each print.\n\n"
                   "A backup of your config was saved automatically."));
    lv_subject_set_pointer(&description_subject_, description_buf_);

    update_ui();
}

void MacroEnhanceWizard::show_error(const std::string& message) {
    state_ = MacroEnhanceState::ERROR;

    snprintf(step_title_buf_, sizeof(step_title_buf_), "%s", lv_tr("Error"));
    lv_subject_set_pointer(&step_title_subject_, step_title_buf_);

    step_progress_buf_[0] = '\0'; // Clear progress text
    lv_subject_set_pointer(&step_progress_subject_, step_progress_buf_);

    snprintf(description_buf_, sizeof(description_buf_), "%s", message.c_str());
    lv_subject_set_pointer(&description_subject_, description_buf_);

    update_ui();
}

void MacroEnhanceWizard::update_close_button_visibility() {
    if (!is_visible()) {
        return;
    }

    // The close button should be visible in SUCCESS or ERROR states
    lv_obj_t* close_buttons = find_widget("close_buttons");
    if (close_buttons != nullptr) {
        if (state_ == MacroEnhanceState::SUCCESS || state_ == MacroEnhanceState::ERROR) {
            lv_obj_remove_flag(close_buttons, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(close_buttons, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// Navigation
// ============================================================================

void MacroEnhanceWizard::advance_to_next() {
    current_op_index_++;
    show_current_operation();
}

// ============================================================================
// Apply Enhancements
// ============================================================================

void MacroEnhanceWizard::apply_enhancements() {
    if (api_ == nullptr) {
        show_error("API connection not available");
        return;
    }

    // Filter to only approved enhancements
    std::vector<helix::MacroEnhancement> approved;
    for (const auto& e : enhancements_) {
        if (e.user_approved) {
            approved.push_back(e);
        }
    }

    if (approved.empty()) {
        show_error("No changes to apply");
        return;
    }

    show_applying("Creating backup...");

    // Check if backup checkbox is checked
    lv_obj_t* checkbox = find_widget("backup_checkbox");
    bool create_backup = true;
    if (checkbox != nullptr) {
        create_backup = lv_obj_has_state(checkbox, LV_STATE_CHECKED);
    }
    // TODO: Pass create_backup to enhancer when API supports it
    (void)create_backup;

    auto token = lifetime_.token();

    // Apply enhancements using PrintStartEnhancer
    enhancer_.apply_enhancements(
        api_, analysis_.macro_name, analysis_.source_file, approved,
        // Progress callback
        [this, token](const std::string& step, int /*current*/, int /*total*/) {
            if (token.expired())
                return;
            token.defer("MacroEnhanceWizard::progress", [this, step]() {
                if (is_visible()) {
                    show_applying(step);
                }
            });
        },
        // Success callback
        [this, token, approved_count = approved.size()](const helix::EnhancementResult& result) {
            if (token.expired())
                return;
            auto backup = result.backup_filename;
            token.defer("MacroEnhanceWizard::success", [this, approved_count, backup]() {
                if (is_visible()) {
                    std::string msg = "Successfully enhanced " + std::to_string(approved_count) +
                                      " operation(s).\n\nBackup: " + backup +
                                      "\n\nKlipper is restarting...";
                    show_success(msg);
                }
            });
        },
        // Error callback
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            auto msg = err.user_message();
            token.defer("MacroEnhanceWizard::error", [this, msg]() {
                if (is_visible()) {
                    show_error(msg);
                }
            });
        });
}

// ============================================================================
// Event Handlers
// ============================================================================

void MacroEnhanceWizard::handle_skip() {
    if (current_op_index_ >= operations_.size()) {
        return;
    }

    spdlog::debug("[MacroEnhanceWizard] Skipped operation: {}",
                  operations_[current_op_index_]->name);

    // Create enhancement marked as not approved
    const auto* op = operations_[current_op_index_];
    std::string skip_param = helix::PrintStartEnhancer::get_skip_param_for_category(op->category);
    if (skip_param.empty()) {
        skip_param = "SKIP_" + op->name;
    }

    helix::MacroEnhancement enhancement;
    enhancement.operation_name = op->name;
    enhancement.category = op->category;
    enhancement.skip_param_name = skip_param;
    enhancement.user_approved = false;
    enhancements_.push_back(enhancement);

    advance_to_next();
}

void MacroEnhanceWizard::handle_approve() {
    if (current_op_index_ >= operations_.size()) {
        return;
    }

    const auto* op = operations_[current_op_index_];
    spdlog::debug("[MacroEnhanceWizard] Approved operation: {}", op->name);

    // Generate enhancement
    std::string skip_param = helix::PrintStartEnhancer::get_skip_param_for_category(op->category);
    if (skip_param.empty()) {
        skip_param = "SKIP_" + op->name;
    }

    // Create a temporary PrintStartOperation for generate_wrapper
    helix::PrintStartOperation temp_op;
    temp_op.name = op->name;
    temp_op.category = op->category;
    temp_op.line_number = op->line_number;

    helix::MacroEnhancement enhancement =
        helix::PrintStartEnhancer::generate_wrapper(temp_op, skip_param);
    enhancement.user_approved = true;
    enhancements_.push_back(enhancement);

    advance_to_next();
}

void MacroEnhanceWizard::handle_cancel() {
    spdlog::info("[MacroEnhanceWizard] Wizard cancelled");

    if (on_complete_) {
        on_complete_(false, 0);
    }

    hide();
}

void MacroEnhanceWizard::handle_apply() {
    spdlog::info("[MacroEnhanceWizard] Applying {} approved enhancements", get_approved_count());
    apply_enhancements();
}

void MacroEnhanceWizard::handle_close() {
    bool applied = (state_ == MacroEnhanceState::SUCCESS);
    size_t count = applied ? get_approved_count() : 0;

    spdlog::info("[MacroEnhanceWizard] Wizard closed (applied: {}, count: {})", applied, count);

    if (on_complete_) {
        on_complete_(applied, count);
    }

    hide();
}

// ============================================================================
// Static Callbacks
// ============================================================================

MacroEnhanceWizard* MacroEnhanceWizard::get_instance_from_event(lv_event_t* e) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));

    // Find modal root by name, then get instance from its user_data.
    // Cannot walk parents checking any user_data — ui_button and other widgets
    // set their own user_data, which would be miscast (prestonbrown/helixscreen#195).
    lv_obj_t* obj = target;
    while (obj) {
        const char* name = lv_obj_get_name(obj);
        if (name && strcmp(name, "macro_enhance_modal") == 0) {
            void* user_data = lv_obj_get_user_data(obj);
            if (user_data) {
                return static_cast<MacroEnhanceWizard*>(user_data);
            }
        }
        obj = lv_obj_get_parent(obj);
    }

    spdlog::warn("[MacroEnhanceWizard] Could not find instance from event target");
    return nullptr;
}

void MacroEnhanceWizard::on_skip_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self != nullptr) {
        self->handle_skip();
    }
}

void MacroEnhanceWizard::on_approve_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self != nullptr) {
        self->handle_approve();
    }
}

void MacroEnhanceWizard::on_cancel_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self != nullptr) {
        self->handle_cancel();
    }
}

void MacroEnhanceWizard::on_apply_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self != nullptr) {
        self->handle_apply();
    }
}

void MacroEnhanceWizard::on_close_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self != nullptr) {
        self->handle_close();
    }
}

} // namespace helix::ui
