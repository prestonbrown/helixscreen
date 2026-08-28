// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_macro_buttons.h
 * @brief Macro Buttons overlay - configures quick action buttons and standard macro slots
 *
 * This overlay allows users to configure:
 * - Quick Buttons: Assign any standard macro slot to the two quick action buttons
 * - Standard Macros: Override auto-detected macros for each operation slot
 *
 * Standard Macro Slots:
 * - LoadFilament, UnloadFilament, Purge
 * - Pause, Resume, Cancel
 * - BedMesh, BedLevel, CleanNozzle, HeatSoak
 *
 * @pattern Overlay (two-phase init: init_subjects -> create -> callbacks)
 * @threading Main thread only
 *
 * @see StandardMacros for macro slot management
 * @see Config for persistence
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "standard_macros.h"
#include "subject_managed_panel.h"

#include <string>
#include <vector>

/// Test seam for quick_button_index_to_slot_name(); defined in
/// tests/test_helpers/macro_buttons_test_access.h. Forward-declared at global
/// scope so the friend below names ::MacroButtonsOverlayTestAccess and not
/// helix::settings::MacroButtonsOverlayTestAccess (cf. xml_hot_reloader.h:14).
class MacroButtonsOverlayTestAccess;

namespace helix::settings {

/**
 * @class MacroButtonsOverlay
 * @brief Overlay for configuring quick action buttons and standard macro slots
 *
 * This overlay provides dropdowns for:
 * - Quick Button 1 & 2: Select which StandardMacroSlot to trigger
 * - Standard Slots (10 total): Select which printer macro to use for each operation
 *
 * ## State Management:
 *
 * Quick buttons are stored in Config at /standard_macros/quick_button_1 and _2.
 * Standard macros are managed by the StandardMacros singleton.
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_macro_buttons_overlay();
 * overlay.show(parent_screen);  // Creates overlay if needed, populates dropdowns, shows
 * @endcode
 */
class MacroButtonsOverlay : public OverlayBase {
  public:
    /**
     * @brief Default constructor
     */
    MacroButtonsOverlay();

    /**
     * @brief Destructor - cleans up subjects
     */
    ~MacroButtonsOverlay() override;

    // Non-copyable
    MacroButtonsOverlay(const MacroButtonsOverlay&) = delete;
    MacroButtonsOverlay& operator=(const MacroButtonsOverlay&) = delete;

    //
    // === Initialization ===
    //

    /**
     * @brief Initialize LVGL subjects for XML data binding
     *
     * Must be called BEFORE create() to ensure bindings work.
     * Currently no subjects needed for this overlay.
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks for:
     * - on_quick_button_1_changed
     * - on_quick_button_2_changed
     * - on_load_filament_changed, on_unload_filament_changed, etc.
     */
    void register_callbacks() override;

    //
    // === UI Creation ===
    //

    /**
     * @brief Create the overlay UI (called lazily)
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Show the overlay (populates dropdowns first)
     *
     * This method:
     * 1. Ensures overlay is created
     * 2. Populates all dropdowns with current macros
     * 3. Pushes overlay onto navigation stack
     *
     * @param parent_screen The parent screen for overlay creation
     */
    void show(lv_obj_t* parent_screen);

    //
    // === Lifecycle ===
    //

    /**
     * @brief Get human-readable overlay name
     * @return "Macro Buttons"
     */
    const char* get_name() const override {
        return "Macro Buttons";
    }

    /**
     * @brief Called when overlay becomes visible
     *
     * Populates dropdowns with current macro data.
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is being hidden
     */
    void on_deactivate() override;

    //
    // === Event Handlers (public for static callbacks) ===
    //

    /**
     * @brief Handle quick button 1 dropdown change
     * @param index Selected dropdown index
     */
    void handle_quick_button_1_changed(int index);

    /**
     * @brief Handle quick button 2 dropdown change
     * @param index Selected dropdown index
     */
    void handle_quick_button_2_changed(int index);

    /**
     * @brief Handle quick button 3 dropdown change
     * @param index Selected dropdown index
     */
    void handle_quick_button_3_changed(int index);

    /**
     * @brief Handle quick button 4 dropdown change
     * @param index Selected dropdown index
     */
    void handle_quick_button_4_changed(int index);

    /**
     * @brief Handle standard macro slot dropdown change
     * @param slot The slot being changed
     * @param dropdown The dropdown widget (to read selected value)
     */
    void handle_standard_macro_changed(StandardMacroSlot slot, lv_obj_t* dropdown);

  private:
    //
    // === Internal Methods ===
    //

    /**
     * @brief Populate all dropdown options from current printer state
     */
    void populate_dropdowns();

    /**
     * @brief Convert dropdown index to slot name for quick buttons
     * @param index Dropdown index (0 = Empty, 1+ = slots)
     * @return Slot name or empty string
     */
    static std::string quick_button_index_to_slot_name(int index);

    friend class ::MacroButtonsOverlayTestAccess;

    /**
     * @brief Get selected macro name from standard macro dropdown
     * @param dropdown The dropdown widget
     * @return Macro name or empty string (for auto-detection)
     */
    static std::string get_selected_macro_from_dropdown(lv_obj_t* dropdown);

    /**
     * @brief Deinitialize subjects for clean shutdown
     */
    void deinit_subjects();

    //
    // === State ===
    //

    std::vector<std::string> printer_macros_; ///< Cached sorted list of printer macros

    //
    // === Subject Management ===
    //

    SubjectManager subjects_;

    //
    // === Static Callbacks ===
    //

    static void on_quick_button_1_changed(lv_event_t* e);
    static void on_quick_button_2_changed(lv_event_t* e);
    static void on_quick_button_3_changed(lv_event_t* e);
    static void on_quick_button_4_changed(lv_event_t* e);
    static void on_load_filament_changed(lv_event_t* e);
    static void on_unload_filament_changed(lv_event_t* e);
    static void on_purge_changed(lv_event_t* e);
    static void on_pause_changed(lv_event_t* e);
    static void on_resume_changed(lv_event_t* e);
    static void on_cancel_changed(lv_event_t* e);
    static void on_bed_mesh_changed(lv_event_t* e);
    static void on_bed_level_changed(lv_event_t* e);
    static void on_clean_nozzle_changed(lv_event_t* e);
    static void on_heat_soak_changed(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton MacroButtonsOverlay
 */
MacroButtonsOverlay& get_macro_buttons_overlay();

} // namespace helix::settings
