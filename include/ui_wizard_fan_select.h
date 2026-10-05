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
 * @file ui_wizard_fan_select.h
 * @brief Wizard fan selection step - configures hotend, part, chamber, and exhaust fans
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
 * ## Subject Bindings (4 total):
 *
 * - hotend_fan_selected (int) - Selected hotend fan index
 * - part_fan_selected (int) - Selected part cooling fan index
 * - chamber_fan_selected (int) - Selected chamber fan index (optional)
 * - exhaust_fan_selected (int) - Selected exhaust fan index (optional)
 */

/**
 * @class WizardFanSelectStep
 * @brief Fan configuration step for the first-run wizard
 */
class WizardFanSelectStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::FanSelect;
    }
    const char* component_name() const override {
        return "wizard_fan_select";
    }
    const char* log_name() const override {
        return "Wizard Fan";
    }
    bool should_skip(const helix::wizard::StepContext& ctx) const override {
        return ctx.preset.skip_hardware;
    }

    WizardFanSelectStep();
    ~WizardFanSelectStep();

    // Non-copyable, non-movable (singleton with lv_subject_t members that
    // contain internal linked lists — moving corrupts observer pointers)
    WizardFanSelectStep(const WizardFanSelectStep&) = delete;
    WizardFanSelectStep& operator=(const WizardFanSelectStep&) = delete;
    WizardFanSelectStep(WizardFanSelectStep&&) = delete;
    WizardFanSelectStep& operator=(WizardFanSelectStep&&) = delete;

    /**
     * @brief Initialize reactive subjects
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks
     */
    void register_callbacks() override;

    /**
     * @brief Create the fan selection UI from XML
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
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "Wizard Fan";
    }

    // Public access to subjects for helper functions
    lv_subject_t* get_hotend_fan_subject() {
        return &hotend_fan_selected_;
    }
    lv_subject_t* get_part_fan_subject() {
        return &part_fan_selected_;
    }
    lv_subject_t* get_chamber_fan_subject() {
        return &chamber_fan_selected_;
    }
    lv_subject_t* get_exhaust_fan_subject() {
        return &exhaust_fan_selected_;
    }

    std::vector<std::string>& get_hotend_fan_items() {
        return hotend_fan_items_;
    }
    std::vector<std::string>& get_part_fan_items() {
        return part_fan_items_;
    }
    std::vector<std::string>& get_chamber_fan_items() {
        return chamber_fan_items_;
    }
    std::vector<std::string>& get_exhaust_fan_items() {
        return exhaust_fan_items_;
    }

    /**
     * @brief Get the screen root widget (for status text updates)
     */
    lv_obj_t* get_screen_root() const {
        return screen_root_;
    }

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Subjects
    SubjectManager subjects_;
    lv_subject_t hotend_fan_selected_{};
    lv_subject_t part_fan_selected_{};
    lv_subject_t chamber_fan_selected_{};
    lv_subject_t exhaust_fan_selected_{};

    // Dynamic options storage
    std::vector<std::string> hotend_fan_items_;
    std::vector<std::string> part_fan_items_;
    std::vector<std::string> chamber_fan_items_;
    std::vector<std::string> exhaust_fan_items_;

    // Track initialization
    bool subjects_initialized_ = false;
};

// ============================================================================
// Global Instance Access
// ============================================================================

WizardFanSelectStep* get_wizard_fan_select_step();
