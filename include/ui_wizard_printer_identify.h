// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <memory>
#include <string>

/**
 * @file ui_wizard_printer_identify.h
 * @brief Wizard printer identification step - name and type configuration
 *
 * Handles printer identification during first-run wizard:
 * - User-entered printer name
 * - Printer type selection from roller
 * - Auto-detection via hardware fingerprinting
 * - Configuration persistence
 *
 * ## Class-Based Architecture (Phase 6)
 *
 * This step has been migrated from function-based to class-based design:
 * - Instance members instead of static globals
 * - Static trampolines for LVGL event callbacks
 * - Global singleton getter for backwards compatibility
 *
 * ## Subject Bindings (3 total):
 *
 * - printer_name (string) - User-entered printer name
 * - printer_type_selected (int) - Selected index in roller
 * - printer_detection_status (string) - Auto-detection status message
 *
 * ## External Subject:
 *
 * - connection_test_passed (extern) - Controls wizard Next button globally
 *
 * Initialization Order (CRITICAL):
 *   1. Register XML component (wizard_printer_identify.xml)
 *   2. init_subjects()
 *   3. register_callbacks()
 *   4. create(parent)
 */

/**
 * @class WizardPrinterIdentifyStep
 * @brief Printer identification step for the first-run wizard
 *
 * Allows user to enter printer name and select printer type.
 * Supports auto-detection via hardware fingerprinting.
 */
class WizardPrinterIdentifyStep {
  public:
    WizardPrinterIdentifyStep();
    ~WizardPrinterIdentifyStep();

    // Non-copyable
    WizardPrinterIdentifyStep(const WizardPrinterIdentifyStep&) = delete;
    WizardPrinterIdentifyStep& operator=(const WizardPrinterIdentifyStep&) = delete;

    // Movable
    WizardPrinterIdentifyStep(WizardPrinterIdentifyStep&& other) noexcept;
    WizardPrinterIdentifyStep& operator=(WizardPrinterIdentifyStep&& other) noexcept;

    /**
     * @brief Initialize reactive subjects
     *
     * Creates and registers 3 subjects. Loads existing values from config.
     * Runs auto-detection if no saved type.
     */
    void init_subjects();

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks:
     * - on_printer_name_changed
     * - on_printer_type_changed
     */
    void register_callbacks();

    /**
     * @brief Create the printer identification UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent);

    /**
     * @brief Cleanup resources
     *
     * Saves current values to config and resets UI references.
     */
    void cleanup();

    /**
     * @brief Check if printer identification is complete
     *
     * @return true if printer name is entered
     */
    bool is_validated() const;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const { return "Wizard Printer"; }

    /**
     * @brief Find printer type index by name
     *
     * @param printer_name Name to search for in PRINTER_TYPES_ROLLER
     * @return Index in the roller, or DEFAULT_PRINTER_TYPE_INDEX if not found
     */
    static int find_printer_type_index(const std::string& printer_name);

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Subjects (3 total)
    lv_subject_t printer_name_;
    lv_subject_t printer_type_selected_;
    lv_subject_t printer_detection_status_;

    // String buffers (must be persistent)
    char printer_name_buffer_[128];
    char printer_detection_status_buffer_[256];

    // State tracking
    bool printer_identify_validated_ = false;
    bool subjects_initialized_ = false;

    // Event handler implementations
    void handle_printer_name_changed(lv_event_t* e);
    void handle_printer_type_changed(lv_event_t* e);

    // Static trampolines for LVGL callbacks
    static void on_printer_name_changed_static(lv_event_t* e);
    static void on_printer_type_changed_static(lv_event_t* e);
};

// ============================================================================
// Auto-Detection Infrastructure
// ============================================================================

/**
 * @brief Printer auto-detection hint (confidence + reasoning)
 */
struct PrinterDetectionHint {
    int type_index;        // Index into PrinterTypes::PRINTER_TYPES_ROLLER
    int confidence;        // 0-100 (>=70 = auto-select, <70 = suggest)
    std::string type_name; // Detected printer type name
};

// ============================================================================
// Global Instance Access
// ============================================================================

/**
 * @brief Get the global WizardPrinterIdentifyStep instance
 *
 * Creates the instance on first call. Used by wizard framework.
 */
WizardPrinterIdentifyStep* get_wizard_printer_identify_step();

/**
 * @brief Destroy the global WizardPrinterIdentifyStep instance
 *
 * Call during application shutdown.
 */
void destroy_wizard_printer_identify_step();

// ============================================================================
// Deprecated Legacy API
// ============================================================================

/**
 * @deprecated Use get_wizard_printer_identify_step()->init_subjects()
 */
[[deprecated("Use get_wizard_printer_identify_step()->init_subjects()")]]
void ui_wizard_printer_identify_init_subjects();

/**
 * @deprecated Use get_wizard_printer_identify_step()->register_callbacks()
 */
[[deprecated("Use get_wizard_printer_identify_step()->register_callbacks()")]]
void ui_wizard_printer_identify_register_callbacks();

/**
 * @deprecated Use get_wizard_printer_identify_step()->create()
 */
[[deprecated("Use get_wizard_printer_identify_step()->create()")]]
lv_obj_t* ui_wizard_printer_identify_create(lv_obj_t* parent);

/**
 * @deprecated Use get_wizard_printer_identify_step()->cleanup()
 */
[[deprecated("Use get_wizard_printer_identify_step()->cleanup()")]]
void ui_wizard_printer_identify_cleanup();

/**
 * @deprecated Use get_wizard_printer_identify_step()->is_validated()
 */
[[deprecated("Use get_wizard_printer_identify_step()->is_validated()")]]
bool ui_wizard_printer_identify_is_validated();
