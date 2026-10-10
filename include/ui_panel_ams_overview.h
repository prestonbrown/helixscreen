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
#include "ams_unit_pages.h"
#include "async_lifetime_guard.h"
#include "display_numbering.h"

#include <memory>
#include <string>
#include <vector>

namespace helix::ui {

/// The inputs a unit card's lane bars are built from.
struct LaneBarsGeometry {
    int first_slot = -1;
    int slot_count = 0;
    int32_t width = 0;
};

/// Whether bars built for @p built must be rebuilt for @p now. Each bar binds
/// a global slot index, so a unit whose start shifts with an unchanged lane
/// count still needs new bars; color, fill and state repaint in place.
inline bool lane_bars_stale(const LaneBarsGeometry& built, const LaneBarsGeometry& now) {
    return built.first_slot != now.first_slot || built.slot_count != now.slot_count ||
           built.width != now.width;
}

} // namespace helix::ui

/**
 * @file ui_panel_ams_overview.h
 * @brief Multi-unit AMS system overview panel with a paging unit view
 *
 * Overview mode shows a zoomed-out view of all AMS units as compact cards above the system
 * path canvas. Each card displays slot color bars (reusing ams_mini_status visual pattern).
 *
 * Tapping a unit card zooms into the unit view: one unit per page, its spool box above the
 * filament path drawn the whole way from its lanes through the hub and buffer to the
 * toolhead. Paging (arrows, a horizontal swipe, page dots) swaps the unit and its lanes and
 * leaves everything downstream where it is. Units that feed the same hub are consecutive
 * pages, and the hub shows the ones on other pages as dashed stubs (see ams_unit_pages.h for
 * the grouping). Back returns to the overview; Back from the overview leaves the panel.
 *
 * Only shown for multi-unit setups (2+ units). Single-unit setups
 * skip this and go directly to the AMS detail panel.
 */
namespace helix {
class AmsPanelTestAccess;
}

class AmsOverviewPanel : public PanelBase {
    friend class helix::AmsPanelTestAccess;

  public:
    AmsOverviewPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);
    ~AmsOverviewPanel() override;

    // === PanelBase Interface ===
    void init_subjects() override;
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

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

    /// The overlay was closed. Stops the sidebar, returns to the overview and frees the unit
    /// view's path canvas buffer; the widget tree stays for the next open.
    void on_closed();

    /// The close callback every open registers: runs on_closed() on the instance.
    static void run_close();

    /// A hidden cached tree is dropped instead of rebuilt; a shown one is left.
    bool rebuild() override;

    /**
     * @brief Zoom from the overview into the unit view, open on the page of unit @p unit_index
     * @param unit_index position in AmsSystemInfo::units
     */
    void show_unit_view(int unit_index);

    /**
     * @brief Return from the unit view to the overview cards
     */
    void show_overview();

    /**
     * @brief Check if the unit view (rather than the overview) is on screen
     */
    [[nodiscard]] bool is_in_unit_view() const {
        return unit_view_active_;
    }

    // === Paging (unit view) ===

    /// Step one page. False, and nothing changes, at the last (first) page.
    bool next_page();
    bool prev_page();

    /// The page on screen, or -1 with no pages.
    [[nodiscard]] int page() const {
        return page_;
    }
    [[nodiscard]] int page_count() const {
        return static_cast<int>(pages_.size());
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
        /// What the lane bars were built for; see lane_bars_stale().
        helix::ui::LaneBarsGeometry bars_built;
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

    // === Unit view state ===
    static constexpr int MAX_DETAIL_SLOTS = AMS_DETAIL_MAX_SLOTS;
    bool unit_view_active_ = false;        ///< The unit view is on screen, not the overview
    lv_obj_t* detail_container_ = nullptr; ///< Unit view root container (takes the swipe)
    lv_obj_t* path_container_ = nullptr;
    lv_obj_t* prev_button_ = nullptr;
    lv_obj_t* next_button_ = nullptr;
    lv_obj_t* page_dots_ = nullptr;
    AmsDetailWidgets detail_widgets_;        ///< Shared widget pointers for the spool box
    lv_obj_t* detail_path_canvas_ = nullptr; ///< Filament path visualization
    lv_obj_t* detail_slot_widgets_[MAX_DETAIL_SLOTS] = {nullptr};
    int detail_slot_count_ = 0;
    int shown_unit_pos_ = -1; ///< Position in AmsSystemInfo::units of the unit on screen

    // === Pages ===
    std::vector<helix::ui::UnitPage> pages_;
    /// Per page, the unit's identity: how a page is recognised again after the system
    /// changes shape (a unit's position in the backend's list is not stable).
    std::vector<std::string> page_units_;
    int page_ = -1;
    std::string shown_unit_key_; ///< identity of the unit on screen
    int pages_refreshes_ = 0;    ///< sync_pages() runs, for tests

    // === Observers ===
    ObserverGuard slots_version_observer_;
    ObserverGuard current_slot_observer_;   ///< Reactive highlight update when active slot changes
    ObserverGuard external_spool_observer_; ///< Reactive updates when external spool color changes
    ObserverGuard bypass_active_observer_;  ///< Active ring follows bypass engage/disengage
    ObserverGuard dryer_version_observer_;  ///< The stubs' drying glyph follows other units' dryers
    ObserverGuard page_count_observer_;     ///< The dots refit when pages come or go
    bool units_rebuild_pending_ = false; ///< Coalesces rapid slots_version observer notifications
    bool open_ = false;       ///< Pushed and not yet closed; a closed panel's card observers wait
    int units_refreshes_ = 0; ///< refresh_units() runs, for tests

    // === Setup Helpers ===
    /// @p noun is the backend's word for one position, so a card reads "4 lanes"
    /// on AFC and "4 gates" on Happy Hare rather than always "4 slots".
    void create_unit_cards(const helix::AmsSystemInfo& info, helix::ui::LaneNoun noun);
    // The mini bars take no current_slot: the active-lane outline comes from the
    // per-slot active-loaded subject, not from comparing against current_slot.
    void update_unit_card(UnitCard& card, const helix::AmsUnit& unit, helix::ui::LaneNoun noun);
    void create_mini_bars(UnitCard& card, const helix::AmsUnit& unit);
    void refresh_system_path(const helix::AmsSystemInfo& info, int current_slot);
    /// Re-sample every unit card's centre and push it to the path canvas. Must run
    /// on every card-row scroll, not just on refresh - see on_cards_row_scrolled().
    /// @param relayout flush pending layout first; false on the scroll path, where
    ///                 coordinates are already current and a relayout would re-enter
    ///                 LVGL's layout pass every scroll step.
    void push_unit_anchors(bool relayout);
    static void on_cards_row_scrolled(lv_event_t* e);
    static void on_system_path_size_changed(lv_event_t* e);
    void detach_widget_hooks();
    /// Publish ams_cards_compact from the measured narrowest card width.
    void publish_cards_compact(int32_t narrowest_card_w);

    /// A slot, current-slot or dryer change: resync the unit view's pages, or refresh the
    /// overview's cards on the next tick.
    void on_state_changed(const char* tag);

    // === Unit view helpers ===
    /// Rebuild the pages from the backend and show the chosen one. @p reopen lays the screen
    /// out afresh; @p focus_unit (a position in AmsSystemInfo::units) picks that unit's page,
    /// otherwise the unit on screen stays on screen while it still has a page, and the page
    /// number is clamped when it does not. Every call repoints the screen at the chosen page
    /// through show_current_page() and redraws it.
    void sync_pages(bool reopen, int focus_unit = -1);
    /// Point the screen at pages_[page_]: header, spool box, path canvas, viewed unit.
    /// @p relayout rebuilds the spool box even when the unit and its slot count are unchanged.
    void show_current_page(const helix::AmsSystemInfo& info, bool relayout);
    void step_page(int delta);
    /// Drop the unit view's state and show the overview cards, without animation.
    void reset_to_overview();
    void create_detail_slots(const helix::AmsSystemInfo& info, int unit_pos);
    void destroy_detail_slots();
    void update_path_canvas();
    /// Level the paging arrows with the hub box and tell the canvas how much edge to leave.
    void layout_paging_controls();
    /// Fit the page dots into their row: the nominal pitch, closing to a floor.
    void layout_page_dots();
    void scroll_current_dot_into_view();
    static void on_path_layout_changed(lv_event_t* e);
    static void on_page_dots_resized(lv_event_t* e);
    static void on_unit_view_gesture(lv_event_t* e);

    // === Slot Interaction ===
    std::unique_ptr<helix::ui::AmsContextMenu> context_menu_; ///< Slot context menu (lazy init)

    void handle_detail_slot_tap(int global_slot_index, lv_point_t click_pt);
    void show_detail_context_menu(int slot_index, lv_obj_t* near_widget, lv_point_t click_pt);

    // === Bypass Spool Interaction ===
    // The overview's overlay sits on the system path canvas, the unit view's on its own
    // path canvas; both follow the same AmsState subjects.
    helix::ui::BypassSpoolWidgets page_bypass_widgets_{};
    void update_page_bypass_widgets_position();
    void handle_bypass_click();
    void refresh_bypass_display();
    void refresh_system_bypass();
    void refresh_page_bypass();
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

/**
 * @brief Open the single-unit AMS panel: drop a hidden overview, build under the
 *        loading pill when there is no cached panel, register and push
 *
 * Every entry point that shows the AMS panel goes through this.
 */
namespace helix::ui {
void open_ams_detail_panel();
} // namespace helix::ui
