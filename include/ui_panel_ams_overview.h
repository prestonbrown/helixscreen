// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_edit_overlay.h"
#include "ui_ams_sidebar.h"
#include "ui_bypass_spool_widget.h"
#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "ams_state.h"
#include "ams_types.h"
#include "async_lifetime_guard.h"

#include <memory>
#include <string>
#include <vector>

/**
 * @file ui_panel_ams_overview.h
 * @brief Multi-unit AMS system overview panel with inline detail view
 *
 * Shows a zoomed-out view of all AMS units as compact cards.
 * Each card displays slot color bars (reusing ams_mini_status visual pattern).
 * Clicking a unit card swaps the left column to show that unit's slot detail
 * inline (no separate overlay panel needed).
 *
 * Only shown for multi-unit setups (2+ units). Single-unit setups
 * skip this and go directly to the AMS detail panel.
 */
class AmsOverviewPanel : public PanelBase {
  public:
    AmsOverviewPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);
    ~AmsOverviewPanel() override = default;

    // === PanelBase Interface ===
    void init_subjects() override;
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    void on_activate() override;
    void on_deactivate() override;

    [[nodiscard]] const char* get_name() const override {
        return "AMS Overview";
    }

    /// Multi-unit filament overview — same reasoning as AmsPanel: a place
    /// users park, so full width, and its drill-downs inherit that. #1178
    [[nodiscard]] bool is_destination() const override {
        return true;
    }
    [[nodiscard]] const char* get_xml_component_name() const override {
        return "ams_overview_panel";
    }

    [[nodiscard]] lv_obj_t* get_panel() const {
        return panel_;
    }

    /**
     * @brief Refresh unit cards from backend state
     */
    void refresh_units();

    /**
     * @brief Clear panel reference before UI destruction
     */
    void clear_panel_reference();

    /**
     * @brief Show detail view for a specific unit (inline, no overlay)
     */
    void show_unit_detail(int unit_index);

    /**
     * @brief Return from detail view to overview cards
     */
    void show_overview();

    /**
     * @brief Check if currently in detail (zoomed) mode
     */
    [[nodiscard]] bool is_in_detail_mode() const {
        return detail_unit_index_ >= 0;
    }

  private:
    // === Unit Card Management ===
    struct UnitCard {
        lv_obj_t* card = nullptr;             // Card container (clickable)
        lv_obj_t* logo_image = nullptr;       // AMS type logo
        lv_obj_t* name_label = nullptr;       // Unit name
        lv_obj_t* bars_container = nullptr;   // Mini status bars
        lv_obj_t* slot_count_label = nullptr; // "4 slots"
        lv_obj_t* error_badge = nullptr;      // Error badge dot (top-right)
        int unit_index = -1;
        /// Untruncated display name. name_label uses long_mode="dots", and LVGL
        /// rewrites that label's own buffer with the ellipsized text - so
        /// lv_label_get_text() cannot answer "how wide does this name want to
        /// be?", which is exactly what the compact rule has to measure.
        std::string display_name;
    };

    std::vector<UnitCard> unit_cards_;
    lv_obj_t* cards_row_ = nullptr;
    lv_obj_t* system_path_ = nullptr;
    lv_obj_t* system_path_area_ = nullptr;

    // Shared bypass spool overlay (see include/ui_bypass_spool_widget.h). Lives
    // on top of system_path_ — the canvas only draws the connecting lines.
    helix::ui::BypassSpoolWidgets bypass_widgets_{};
    void update_bypass_widgets_position();

    // === Detail View State ===
    static constexpr int MAX_DETAIL_SLOTS = 16;
    int detail_unit_index_ = -1;             ///< Currently shown unit (-1 = overview mode)
    lv_obj_t* detail_container_ = nullptr;   ///< Detail view root container
    AmsDetailWidgets detail_widgets_;        ///< Shared widget pointers for detail view
    lv_obj_t* detail_path_canvas_ = nullptr; ///< Filament path visualization
    lv_obj_t* detail_slot_widgets_[MAX_DETAIL_SLOTS] = {nullptr};
    int detail_slot_count_ = 0;

    // === Observers ===
    ObserverGuard slots_version_observer_;
    ObserverGuard current_slot_observer_;   ///< Reactive highlight update when active slot changes
    ObserverGuard external_spool_observer_; ///< Reactive updates when external spool color changes
    bool units_rebuild_pending_ = false; ///< Coalesces rapid slots_version observer notifications
    helix::AsyncLifetimeGuard
        lifetime_; ///< Guards deferred callbacks from accessing destroyed panel

    // === Setup Helpers ===
    void create_unit_cards(const AmsSystemInfo& info);
    // The mini bars take no current_slot: the active-lane outline comes from the
    // per-slot active-loaded subject, not from comparing against current_slot.
    void update_unit_card(UnitCard& card, const AmsUnit& unit);
    void create_mini_bars(UnitCard& card, const AmsUnit& unit);
    void refresh_system_path(const AmsSystemInfo& info, int current_slot);
    /// Re-sample every unit card's centre and push it to the path canvas. Must run
    /// on every card-row scroll, not just on refresh - see on_cards_row_scrolled().
    /// @param relayout flush pending layout first; false on the scroll path, where
    ///                 coordinates are already current and a relayout would re-enter
    ///                 LVGL's layout pass every scroll step.
    void push_unit_anchors(bool relayout);
    static void on_cards_row_scrolled(lv_event_t* e);
    /// Publish ams_cards_compact from the measured narrowest card width.
    void publish_cards_compact(int32_t narrowest_card_w);

    // === Detail View Helpers ===
    void refresh_detail_if_needed(); ///< Lightweight refresh — only rebuilds on structural change
    void create_detail_slots(const AmsUnit& unit);
    void destroy_detail_slots();
    void setup_detail_path_canvas(const AmsUnit& unit, const AmsSystemInfo& info);
    void update_detail_header(const AmsUnit& unit, const AmsSystemInfo& info);

    // === Slot Interaction ===
    std::unique_ptr<helix::ui::AmsContextMenu> context_menu_; ///< Slot context menu (lazy init)

    void handle_detail_slot_tap(int global_slot_index, lv_point_t click_pt);
    void show_detail_context_menu(int slot_index, lv_obj_t* near_widget, lv_point_t click_pt);

    // === Bypass Spool Interaction ===
    void handle_bypass_click();
    void refresh_bypass_display();
    void show_edit_modal(int slot_index, bool open_on_picker = false);
    static void on_bypass_spool_clicked(lv_event_t* e);

    // === Sidebar ===
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;

    // === Event Handling ===
    static void on_unit_card_clicked(lv_event_t* e);
    static void on_detail_slot_clicked(lv_event_t* e);
};

/**
 * @brief Get global AMS overview panel singleton
 */
AmsOverviewPanel& get_global_ams_overview_panel();

/**
 * @brief Destroy the AMS overview panel UI
 */
void destroy_ams_overview_panel_ui();

/**
 * @brief Navigate to AMS panel with multi-unit awareness
 *
 * If multi-unit: push overview panel
 * If single-unit: push detail panel directly (unchanged behavior)
 */
void navigate_to_ams_panel();
