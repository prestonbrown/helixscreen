// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_macro_enhance_wizard.h
 * @brief Step-by-step wizard for enhancing PRINT_START macros
 *
 * This wizard guides users through making PRINT_START operations skippable.
 * For each detected uncontrollable operation (bed mesh, QGL, etc.), it:
 * 1. Shows the operation and explains what will be added
 * 2. Displays the Jinja2 wrapper code
 * 3. Lets user approve or skip each operation
 * 4. Shows summary and applies changes with backup
 *
 * ## Usage:
 * ```cpp
 * MacroEnhanceWizard wizard;
 * wizard.set_api(api);
 * wizard.set_analysis(analysis);
 * wizard.set_complete_callback([](bool applied, size_t count) { ... });
 * wizard.show(parent);
 * ```
 */

#include "ui_modal.h"

#include "print_start_analyzer.h"
#include "print_start_enhancer.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
class IMoonrakerAPI;

namespace helix::ui {

/**
 * @brief Wizard state machine states
 */
enum class MacroEnhanceState {
    OPERATION = 0, ///< Showing individual operation for approval
    SUMMARY = 1,   ///< Showing summary of all approved changes
    APPLYING = 2,  ///< Applying changes (spinner)
    SUCCESS = 3,   ///< Changes applied successfully
    ERROR = 4      ///< Error occurred
};

/**
 * @brief Callback when wizard completes (success or cancel)
 *
 * @param applied true if changes were applied, false if cancelled
 * @param operations_enhanced Number of operations that were enhanced
 */
using WizardCompleteCallback = std::function<void(bool applied, size_t operations_enhanced)>;

/**
 * @brief Step-by-step wizard for enhancing PRINT_START macros
 *
 * Manages the UI flow for reviewing and approving macro enhancements.
 * Uses the PrintStartEnhancer for code generation and Moonraker operations.
 *
 * Extends Modal class for proper backdrop management and lifecycle.
 */
class MacroEnhanceWizard : public Modal {
  public:
    MacroEnhanceWizard();
    ~MacroEnhanceWizard() override;

    // Non-copyable, non-movable (contains LVGL subjects and observers)
    MacroEnhanceWizard(const MacroEnhanceWizard&) = delete;
    MacroEnhanceWizard& operator=(const MacroEnhanceWizard&) = delete;
    MacroEnhanceWizard(MacroEnhanceWizard&&) = delete;
    MacroEnhanceWizard& operator=(MacroEnhanceWizard&&) = delete;

    // =========================================================================
    // Modal Interface
    // =========================================================================

    [[nodiscard]] const char* get_name() const override {
        return "Macro Enhancement Wizard";
    }
    [[nodiscard]] const char* component_name() const override {
        return "macro_enhance_modal";
    }

    // =========================================================================
    // Setup
    // =========================================================================

    /**
     * @brief Set API dependency
     *
     * @param api IMoonrakerAPI instance (must remain valid while wizard is open)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Set the analysis result to enhance
     *
     * @param analysis Analysis result from PrintStartAnalyzer
     */
    void set_analysis(const helix::PrintStartAnalysis& analysis);

    /**
     * @brief Set completion callback
     *
     * @param callback Called when wizard closes (success, cancel, or error)
     */
    void set_complete_callback(WizardCompleteCallback callback) {
        on_complete_ = std::move(callback);
    }

    // =========================================================================
    // Show/Hide
    // =========================================================================

    /**
     * @brief Show the wizard modal
     *
     * Creates the modal UI and starts the wizard flow.
     * Requires set_api() and set_analysis() to be called first.
     *
     * @param parent Parent object for modal placement
     * @return true if wizard was shown, false if no operations to enhance
     */
    bool show(lv_obj_t* parent);

    // =========================================================================
    // State Access (for testing)
    // =========================================================================

    [[nodiscard]] MacroEnhanceState get_state() const {
        return state_;
    }
    [[nodiscard]] size_t get_current_operation_index() const {
        return current_op_index_;
    }
    [[nodiscard]] size_t get_total_operations() const {
        return operations_.size();
    }
    [[nodiscard]] size_t get_approved_count() const;

  protected:
    // =========================================================================
    // Modal Hooks
    // =========================================================================

    void on_show() override;
    void on_hide() override;

  private:
    // === Dependencies ===
    IMoonrakerAPI* api_ = nullptr;
    helix::PrintStartAnalysis analysis_;
    helix::PrintStartEnhancer enhancer_;

    // === State ===
    MacroEnhanceState state_ = MacroEnhanceState::OPERATION;
    std::vector<const helix::PrintStartOperation*> operations_; ///< Uncontrollable ops to process
    std::vector<helix::MacroEnhancement> enhancements_;         ///< Generated enhancements
    size_t current_op_index_ = 0;

    // === Subjects ===
    lv_subject_t step_title_subject_;
    lv_subject_t step_progress_subject_;
    lv_subject_t description_subject_;
    lv_subject_t diff_preview_subject_;
    lv_subject_t summary_subject_;
    lv_subject_t state_subject_;
    lv_subject_t backup_text_subject_; ///< Dynamic backup checkbox label text

    // Boolean visibility subjects for each state (bind_flag_if_eq pattern)
    lv_subject_t show_operation_subject_;
    lv_subject_t show_summary_subject_;
    lv_subject_t show_applying_subject_;
    lv_subject_t show_success_subject_;
    lv_subject_t show_error_subject_;

    bool subjects_initialized_ = false;

    // Subject text buffers (must persist for subject lifetime)
    char step_title_buf_[128] = {};
    char step_progress_buf_[32] = {};
    char description_buf_[512] = {};
    char diff_preview_buf_[2048] = {};
    char summary_buf_[2048] = {};
    char backup_text_buf_[128] = {}; ///< Buffer for dynamic backup checkbox label

    // === Observer tracking for cleanup [L020] ===
    lv_observer_t* step_title_observer_ = nullptr;
    lv_observer_t* step_progress_observer_ = nullptr;
    lv_observer_t* description_observer_ = nullptr;
    lv_observer_t* diff_preview_observer_ = nullptr;
    lv_observer_t* summary_observer_ = nullptr;
    lv_observer_t* applying_status_observer_ = nullptr;
    lv_observer_t* success_message_observer_ = nullptr;
    lv_observer_t* error_message_observer_ = nullptr;
    lv_observer_t* backup_label_observer_ = nullptr;

    // === Callbacks ===
    WizardCompleteCallback on_complete_;

    // === Internal Methods ===

    void init_subjects();
    void bind_subjects_to_widgets();
    void update_ui();
    void show_current_operation();
    void show_summary();
    void show_applying(const std::string& status);
    void show_success(const std::string& message);
    void show_error(const std::string& message);
    void advance_to_next();
    void apply_enhancements();
    void update_close_button_visibility();

    // === Event Handlers ===
    void handle_skip();
    void handle_approve();
    void handle_cancel();
    void handle_apply();
    void handle_close();

    // === Static Callback Registration ===
    static void register_callbacks();
    static bool callbacks_registered_;

    // === Static Callbacks ===
    static void on_skip_cb(lv_event_t* e);
    static void on_approve_cb(lv_event_t* e);
    static void on_cancel_cb(lv_event_t* e);
    static void on_apply_cb(lv_event_t* e);
    static void on_close_cb(lv_event_t* e);

    /**
     * @brief Find MacroEnhanceWizard instance from event target
     */
    static MacroEnhanceWizard* get_instance_from_event(lv_event_t* e);
};

} // namespace helix::ui
