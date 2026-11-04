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
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see StandardMacros for macro slot management
 * @see Config for persistence
 */

#pragma once

#include "core/static_panel_registry.h"
#include "overlay_base.h"
#include "standard_macros.h"

#include <string>
#include <vector>

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
    const char* get_name() const override {
        return "Macro Buttons";
    }
    const char* xml_component() const override {
        return "macro_buttons_overlay";
    }
    /// Freed on close; the next open rebuilds it.
    bool destroy_on_close() const override {
        return true;
    }

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks for:
     * - on_quick_button_1_changed through on_quick_button_4_changed
     * - on_load_filament_changed, on_unload_filament_changed, etc.
     */
    void register_callbacks() override;

    /**
     * @brief Called when overlay becomes visible
     *
     * Populates dropdowns with current macro data.
     */
    void on_activate() override;

    //
    // === Event Handlers ===
    //

    /**
     * @brief Convert dropdown index to slot name for quick buttons
     * @param index Dropdown index (0 = Empty, 1+ = slots)
     * @return Slot name or empty string
     */
    static std::string quick_button_index_to_slot_name(int index);

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
     * @brief Get selected macro name from standard macro dropdown
     * @param dropdown The dropdown widget
     * @return Macro name or empty string (for auto-detection)
     */
    static std::string get_selected_macro_from_dropdown(lv_obj_t* dropdown);

    //
    // === State ===
    //

    std::vector<std::string> printer_macros_; ///< Cached sorted list of printer macros
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton MacroButtonsOverlay
 */
inline MacroButtonsOverlay& get_macro_buttons_overlay() {
    return lazy_global<MacroButtonsOverlay>("MacroButtonsOverlay");
}

} // namespace helix::settings
