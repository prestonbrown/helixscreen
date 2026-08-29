// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_hardware_health.h
 * @brief Hardware Health overlay - displays hardware validation issues
 *
 * This overlay shows hardware validation results categorized by severity:
 * - Critical: Missing required hardware (extruder, heater_bed)
 * - Warning: Missing configured hardware
 * - Info: Newly discovered hardware
 * - Session: Hardware changed since last session
 *
 * Users can:
 * - Ignore: Mark hardware as optional (suppress future warnings)
 * - Save: Add hardware to expected list (INFO severity only)
 *
 * @pattern Overlay (lazy init, dynamic row creation)
 * @threading Main thread only
 *
 * @see HardwareValidator for validation logic
 * @see PrinterState for validation result caching
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

#include <memory>
#include <string>

// Forward declarations
namespace helix {
class PrinterState;
}

namespace helix::settings {

/**
 * @class HardwareHealthOverlay
 * @brief Overlay for displaying and managing hardware validation issues
 *
 * This overlay provides:
 * - Status card with severity-based icon
 * - Four sections for issues by category
 * - Per-issue action buttons (Ignore/Save)
 * - Save confirmation dialog for adding hardware to config
 *
 * ## State Management:
 *
 * Validation results come from PrinterState::get_hardware_validation_result().
 * Configuration changes use HardwareValidator static methods.
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_hardware_health_overlay();
 * overlay.set_printer_state(&printer_state);
 * overlay.show(parent_screen);  // Creates overlay if needed, populates issues, shows
 * @endcode
 *
 * ## Note on Dynamic Row Creation:
 *
 * Issue rows are created dynamically using lv_xml_create(). This requires
 * using lv_obj_add_event_cb() for button callbacks and DELETE cleanup,
 * which is an acceptable exception to the declarative UI rule.
 */
class HardwareHealthOverlay : public OverlayBase {
  public:
    /**
     * @brief Default constructor
     */
    HardwareHealthOverlay();

    /**
     * @brief Destructor
     */
    ~HardwareHealthOverlay() override;

    // Non-copyable (inherited from OverlayBase)

    //
    // === OverlayBase Interface ===
    //

    /**
     * @brief Initialize subjects (empty - no subjects needed)
     */
    void init_subjects() override {}

    /**
     * @brief Create the overlay UI (called lazily)
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Hardware Health"
     */
    const char* get_name() const override {
        return "Hardware Health";
    }

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks for:
     * - on_hardware_health_clicked (entry point from SettingsPanel)
     */
    void register_callbacks() override;

    /**
     * @brief Called when overlay becomes visible
     *
     * Populates hardware issues from validation result.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     *
     * Cleans up any open modal dialogs.
     */
    void on_deactivate() override;

    //
    // === Initialization ===
    //

    /**
     * @brief Set PrinterState dependency (required for validation results)
     * @param printer_state Pointer to PrinterState instance
     */
    void set_printer_state(PrinterState* printer_state) {
        printer_state_ = printer_state;
    }

    /**
     * @brief Show the overlay (populates issues first)
     *
     * This method:
     * 1. Ensures overlay is created
     * 2. Populates issue lists from validation result
     * 3. Pushes overlay onto navigation stack
     *
     * @param parent_screen The parent screen for overlay creation
     */
    void show(lv_obj_t* parent_screen);

    //
    // === Accessors ===
    //

    /**
     * @brief Check if overlay has been created
     * @return true if create() was called successfully
     */
    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    //
    // === Event Handlers (public for static callbacks) ===
    //

    /**
     * @brief Handle hardware action button click
     *
     * @param hardware_name Name of hardware to act on
     * @param is_ignore true for "Ignore" (mark optional), false for "Save" (add to config)
     */
    void handle_hardware_action(const char* hardware_name, bool is_ignore);

    /**
     * @brief Handle save confirmation (user clicked "Save" in dialog)
     */
    void handle_hardware_save_confirm();

    /**
     * @brief Handle save cancellation (user clicked cancel in dialog)
     */
    void handle_hardware_save_cancel();

  private:
    //
    // === Internal Methods ===
    //

    /**
     * @brief Populate issue lists from validation result
     *
     * Creates a hardware_issue_row component for each issue.
     * Clears existing rows first to allow refresh.
     */
    void populate_hardware_issues();

    //
    // === State ===
    //

    PrinterState* printer_state_{nullptr};

    /// Hardware name pending save confirmation
    std::string pending_hardware_save_;

    /// Save confirmation dialog
    lv_obj_t* hardware_save_dialog_{nullptr};

    //
    // === Static Callbacks ===
    //

    static void on_hardware_save_confirm(lv_event_t* e);
    static void on_hardware_save_cancel(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton HardwareHealthOverlay
 */
HardwareHealthOverlay& get_hardware_health_overlay();

} // namespace helix::settings
