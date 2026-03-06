// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_dryer_card.h"
#include "ui_ams_edit_modal.h"
#include "ui_ams_sidebar.h"
#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "ams_state.h"
#include "ams_types.h"

#include <memory>
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
    AmsOverviewPanel(helix::PrinterState& printer_state, MoonrakerAPI* api);
    ~AmsOverviewPanel() override = default;

    // === PanelBase Interface ===
    void init_subjects() override;
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    void on_activate() override;
    void on_deactivate() override;

    [[nodiscard]] const char* get_name() const override {
        return "AMS Overview";
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
    };

    std::vector<UnitCard> unit_cards_;
    lv_obj_t* cards_row_ = nullptr;
    lv_obj_t* system_path_ = nullptr;
    lv_obj_t* system_path_area_ = nullptr;

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

    // === Setup Helpers ===
    void create_unit_cards(const AmsSystemInfo& info);
    void update_unit_card(UnitCard& card, const AmsUnit& unit, int current_slot);
    void create_mini_bars(UnitCard& card, const AmsUnit& unit, int current_slot);
    void refresh_system_path(const AmsSystemInfo& info, int current_slot);

    // === Detail View Helpers ===
    void refresh_detail_if_needed(); ///< Lightweight refresh — only rebuilds on structural change
    void create_detail_slots(const AmsUnit& unit);
    void destroy_detail_slots();
    void setup_detail_path_canvas(const AmsUnit& unit, const AmsSystemInfo& info);
    void update_detail_header(const AmsUnit& unit, const AmsSystemInfo& info);

    // === Slot Interaction ===
    std::unique_ptr<helix::ui::AmsContextMenu> context_menu_; ///< Slot context menu (lazy init)
    std::unique_ptr<helix::ui::AmsEditModal> edit_modal_;     ///< Edit modal (lazy init)

    void handle_detail_slot_tap(int global_slot_index, lv_point_t click_pt);
    void show_detail_context_menu(int slot_index, lv_obj_t* near_widget, lv_point_t click_pt);

    // === Bypass Spool Interaction ===
    void handle_bypass_click();
    void refresh_bypass_display();
    void show_edit_modal(int slot_index);
    static void on_bypass_spool_clicked(void* user_data);

    // === Sidebar ===
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;

    // === Dryer Card (info bar) ===
    std::unique_ptr<helix::ui::AmsDryerCard> dryer_card_;

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
