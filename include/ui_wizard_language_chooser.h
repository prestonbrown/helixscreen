// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"
#include "ui_timer_guard.h"

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"
#include "wizard_step.h"

#include <atomic>
#include <memory>

/**
 * @file ui_wizard_language_chooser.h
 * @brief Wizard language selection step - first-run language choice with cycling welcome text
 *
 * Displays a language selection screen with animated "Welcome!" text that cycles
 * through supported languages. Users select their preferred language by clicking
 * a flag button.
 *
 * ## Skip Logic:
 *
 * - Language already explicitly set in config: Skip step
 * - No language preference saved: Show step for selection
 *
 * ## Subject Bindings:
 *
 * - wizard_welcome_text (string) - Cycling "Welcome!" in different languages
 *
 * ## Validation:
 *
 * Step is validated when user selects a language.
 */

/**
 * @class WizardLanguageChooserStep
 * @brief Language selection step for the first-run wizard
 */
class WizardLanguageChooserStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::Language;
    }
    const char* component_name() const override {
        return "wizard_language_chooser";
    }
    const char* log_name() const override {
        return "Wizard Language Chooser";
    }
    bool should_skip([[maybe_unused]] const helix::wizard::StepContext& ctx) const override {
        return should_skip();
    }

    WizardLanguageChooserStep();
    ~WizardLanguageChooserStep();

    // Non-copyable, non-movable (singleton with lv_subject_t members that
    // contain internal linked lists — moving corrupts observer pointers)
    WizardLanguageChooserStep(const WizardLanguageChooserStep&) = delete;
    WizardLanguageChooserStep& operator=(const WizardLanguageChooserStep&) = delete;
    WizardLanguageChooserStep(WizardLanguageChooserStep&&) = delete;
    WizardLanguageChooserStep& operator=(WizardLanguageChooserStep&&) = delete;

    /**
     * @brief Initialize reactive subjects
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks
     */
    void register_callbacks() override;

    /**
     * @brief Create the language chooser UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Cleanup resources
     */
    void cleanup() override;

    /**
     * @brief Check if step is validated
     *
     * @return true if a language has been selected
     */
    bool is_validated() const override;

    /**
     * @brief Check if this step should be skipped
     *
     * Skips if a language has already been explicitly set in config.
     *
     * @return true if step should be skipped, false otherwise
     */
    bool should_skip() const;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "Wizard Language Chooser";
    }

    // ========================================================================
    // State accessors for testing and wizard flow
    // ========================================================================

    /**
     * @brief Get the screen root object
     *
     * @return Pointer to the screen root object, or nullptr if not created
     */
    lv_obj_t* get_screen_root() const {
        return screen_root_;
    }

    /**
     * @brief Get welcome text subject for XML binding
     */
    lv_subject_t* get_welcome_text_subject() {
        return &welcome_text_;
    }

    /**
     * @brief Check if a language has been selected
     */
    bool is_language_selected() const {
        return language_selected_;
    }

    /**
     * @brief Set language selected flag (called from callback)
     */
    void set_language_selected(bool selected) {
        language_selected_ = selected;
    }

    /**
     * @brief Stop the welcome text cycling timer
     */
    void stop_cycle_timer() {
        cycle_timer_.reset();
    }

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // SubjectManager, declared ahead of the subject it owns so it tears down
    // after it (the name withdraws before the storage dies).
    SubjectManager subjects_;

    // Subjects
    lv_subject_t welcome_text_;

    // String buffer for welcome text subject
    char welcome_buffer_[64] = "Welcome!";

    // Timer for cycling welcome text
    helix::ui::LvglTimerGuard cycle_timer_;
    int current_welcome_index_ = 0;

    // State tracking
    bool subjects_initialized_ = false;
    bool language_selected_ = false;

    // Helper methods
    void cycle_welcome_text();
    void animate_crossfade(const char* new_text);

    // Static timer callback
    static void cycle_timer_cb(lv_timer_t* timer);
};

// ============================================================================
// Global Instance Access
// ============================================================================

WizardLanguageChooserStep* get_wizard_language_chooser_step();

/**
 * @brief Force-show the language chooser step (for testing)
 *
 * When set to true, should_skip() will return false regardless
 * of whether a language has been configured.
 *
 * @param force true to force-show the step, false for normal behavior
 */
void force_language_chooser_step(bool force);

namespace helix {

/**
 * @brief Display-size face for the cycling Welcome header, per breakpoint
 *
 * The header is the first thing a user ever sees and sits a size class above
 * a section heading (prestonbrown/helixscreen#1599). The 48/64 faces are
 * xxlarge-tier (mk/fonts.mk) and the XML name map only registers them on
 * xxlarge displays, so XML cannot name them at lower breakpoints; this is
 * the computed-font exception, and builds without those faces step down to
 * the largest face they link.
 *
 * @param bp Current UI breakpoint
 * @return Font for the welcome header at that tier
 */
const lv_font_t* wizard_welcome_header_font(UiBreakpoint bp);

} // namespace helix
