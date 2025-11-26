// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <memory>
#include <string>

/**
 * @file ui_wizard_connection.h
 * @brief Wizard Moonraker connection step - WebSocket configuration and testing
 *
 * Handles Moonraker WebSocket configuration during first-run wizard:
 * - IP address or hostname entry
 * - Port number configuration (default: 7125)
 * - Connection testing with async feedback
 * - Auto-discovery trigger on success
 * - Configuration persistence
 *
 * ## Class-Based Architecture (Phase 6)
 *
 * This step has been migrated from function-based to class-based design:
 * - Instance members instead of static globals
 * - Async WebSocket callbacks with captured instance reference
 * - Static trampolines for LVGL event callbacks
 * - Global singleton getter for backwards compatibility
 *
 * ## Subject Bindings (5 total):
 *
 * - connection_ip (string) - IP address or hostname
 * - connection_port (string) - Port number (default "7125")
 * - connection_status_icon (string) - FontAwesome icon (check/xmark/empty)
 * - connection_status_text (string) - Status message text
 * - connection_testing (int) - 0=idle, 1=testing (controls spinner)
 *
 * ## External Subject:
 *
 * - connection_test_passed (extern) - Controls wizard Next button globally
 *
 * Initialization Order (CRITICAL):
 *   1. Register XML component (wizard_connection.xml)
 *   2. init_subjects()
 *   3. register_callbacks()
 *   4. create(parent)
 */

/**
 * @class WizardConnectionStep
 * @brief Moonraker WebSocket connection step for the first-run wizard
 *
 * Allows user to enter Moonraker IP/port and test the connection.
 * On success, triggers hardware discovery for subsequent wizard steps.
 */
class WizardConnectionStep {
  public:
    WizardConnectionStep();
    ~WizardConnectionStep();

    // Non-copyable
    WizardConnectionStep(const WizardConnectionStep&) = delete;
    WizardConnectionStep& operator=(const WizardConnectionStep&) = delete;

    // Movable
    WizardConnectionStep(WizardConnectionStep&& other) noexcept;
    WizardConnectionStep& operator=(WizardConnectionStep&& other) noexcept;

    /**
     * @brief Initialize reactive subjects
     *
     * Creates and registers 5 subjects. Loads existing values from config.
     * Sets connection_test_passed to 0 (disabled) for this step.
     */
    void init_subjects();

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks:
     * - on_test_connection_clicked
     * - on_ip_input_changed
     * - on_port_input_changed
     */
    void register_callbacks();

    /**
     * @brief Create the connection UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent);

    /**
     * @brief Cleanup resources
     *
     * Cancels any ongoing connection test and resets UI references.
     */
    void cleanup();

    /**
     * @brief Get the configured Moonraker URL
     *
     * @param buffer Output buffer for the URL
     * @param size Size of the output buffer
     * @return true if URL was successfully constructed
     */
    bool get_url(char* buffer, size_t size) const;

    /**
     * @brief Check if connection has been successfully tested
     *
     * @return true if a successful connection test has been performed
     */
    bool is_validated() const;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const { return "Wizard Connection"; }

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Subjects (5 total)
    lv_subject_t connection_ip_;
    lv_subject_t connection_port_;
    lv_subject_t connection_status_icon_;
    lv_subject_t connection_status_text_;
    lv_subject_t connection_testing_;

    // String buffers (must be persistent)
    char connection_ip_buffer_[128];
    char connection_port_buffer_[8];
    char connection_status_icon_buffer_[8];
    char connection_status_text_buffer_[256];

    // State tracking
    bool connection_validated_ = false;
    bool subjects_initialized_ = false;

    // Saved values for async callback (connection test result)
    std::string saved_ip_;
    std::string saved_port_;

    // Event handler implementations
    void handle_test_connection_clicked();
    void handle_ip_input_changed();
    void handle_port_input_changed();

    // Async callback handlers (called from WebSocket callbacks)
    void on_connection_success();
    void on_connection_failure();

    // Static trampolines for LVGL callbacks
    static void on_test_connection_clicked_static(lv_event_t* e);
    static void on_ip_input_changed_static(lv_event_t* e);
    static void on_port_input_changed_static(lv_event_t* e);
};

// ============================================================================
// Global Instance Access
// ============================================================================

/**
 * @brief Get the global WizardConnectionStep instance
 *
 * Creates the instance on first call. Used by wizard framework.
 */
WizardConnectionStep* get_wizard_connection_step();

/**
 * @brief Destroy the global WizardConnectionStep instance
 *
 * Call during application shutdown.
 */
void destroy_wizard_connection_step();

// ============================================================================
// Deprecated Legacy API
// ============================================================================

/**
 * @deprecated Use get_wizard_connection_step()->init_subjects()
 */
[[deprecated("Use get_wizard_connection_step()->init_subjects()")]]
void ui_wizard_connection_init_subjects();

/**
 * @deprecated Use get_wizard_connection_step()->register_callbacks()
 */
[[deprecated("Use get_wizard_connection_step()->register_callbacks()")]]
void ui_wizard_connection_register_callbacks();

/**
 * @deprecated Use get_wizard_connection_step()->create()
 */
[[deprecated("Use get_wizard_connection_step()->create()")]]
lv_obj_t* ui_wizard_connection_create(lv_obj_t* parent);

/**
 * @deprecated Use get_wizard_connection_step()->cleanup()
 */
[[deprecated("Use get_wizard_connection_step()->cleanup()")]]
void ui_wizard_connection_cleanup();

/**
 * @deprecated Use get_wizard_connection_step()->get_url()
 */
[[deprecated("Use get_wizard_connection_step()->get_url()")]]
bool ui_wizard_connection_get_url(char* buffer, size_t size);

/**
 * @deprecated Use get_wizard_connection_step()->is_validated()
 */
[[deprecated("Use get_wizard_connection_step()->is_validated()")]]
bool ui_wizard_connection_is_validated();
