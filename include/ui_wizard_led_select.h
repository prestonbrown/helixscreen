// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"
#include "wizard_step.h"

#include <memory>
#include <string>
#include <vector>

/**
 * @file ui_wizard_led_select.h
 * @brief Wizard LED selection step - configures LED strips
 *
 * Uses hardware discovery from MoonrakerClient to populate dropdowns.
 *
 * ## Class-Based Architecture (Phase 6)
 *
 * Migrated from function-based to class-based design with:
 * - Instance members instead of static globals
 * - Static trampolines for LVGL callbacks
 * - Global singleton getter for backwards compatibility
 *
 * ## Subject Bindings (1 total):
 *
 * - led_strip_selected (int) - Selected LED strip index
 */

/**
 * @class WizardLedSelectStep
 * @brief LED configuration step for the first-run wizard
 */
class WizardLedSelectStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::LedSelect;
    }
    const char* component_name() const override {
        return "wizard_led_select";
    }
    const char* log_name() const override {
        return "Wizard LED";
    }
    bool should_skip(const helix::wizard::StepContext& ctx) const override {
        return ctx.preset.skip_hardware || should_skip();
    }

    WizardLedSelectStep();
    ~WizardLedSelectStep();

    // Non-copyable, non-movable (singleton with lv_subject_t members that
    // contain internal linked lists — moving corrupts observer pointers)
    WizardLedSelectStep(const WizardLedSelectStep&) = delete;
    WizardLedSelectStep& operator=(const WizardLedSelectStep&) = delete;
    WizardLedSelectStep(WizardLedSelectStep&&) = delete;
    WizardLedSelectStep& operator=(WizardLedSelectStep&&) = delete;

    /**
     * @brief Initialize reactive subjects
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks
     */
    void register_callbacks() override;

    /**
     * @brief Create the LED selection UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Cleanup resources and save selections to config
     */
    void cleanup() override;

    /**
     * @brief Check if step is validated
     *
     * @return true (always validated for baseline)
     */
    bool is_validated() const override;

    /**
     * @brief Check if this step should be skipped
     *
     * Skips if no LEDs are discovered from the printer.
     *
     * @return true if step should be skipped, false otherwise
     */
    bool should_skip() const;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "Wizard LED";
    }

    // Public access to subjects for helper functions
    lv_subject_t* get_led_strip_subject() {
        return &led_strip_selected_;
    }

    std::vector<std::string>& get_led_strip_items() {
        return led_strip_items_;
    }

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Subjects
    SubjectManager subjects_;
    lv_subject_t led_strip_selected_{};

    // Dynamic options storage
    std::vector<std::string> led_strip_items_;

    // Track initialization
    bool subjects_initialized_ = false;
};

// ============================================================================
// Global Instance Access
// ============================================================================

WizardLedSelectStep* get_wizard_led_select_step();
