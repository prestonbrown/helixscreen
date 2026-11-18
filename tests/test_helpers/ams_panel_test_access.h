// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"

namespace helix {

/// What a closed AMS panel keeps running, for the keep-alive tests.
class AmsPanelTestAccess {
  public:
    static bool is_open(const AmsPanel& p) {
        return p.open_;
    }
    static bool has_sidebar(const AmsPanel& p) {
        return p.sidebar_ != nullptr;
    }
    static helix::ui::AmsOperationSidebar* sidebar(AmsPanel& p) {
        return p.sidebar_.get();
    }
    static lv_obj_t* path_canvas(const AmsPanel& p) {
        return p.path_canvas_;
    }
    /// Re-read the endless-spool relation from the backend into the arrows widget.
    static void update_endless_arrows(AmsPanel& p) {
        p.update_endless_arrows_from_backend();
    }
    /// A tap on the path canvas's head for @p slot, as the canvas hit-test reports it.
    static void tap_toolhead(AmsPanel& p, int slot) {
        AmsPanel::on_path_toolhead_clicked(slot, &p);
    }
    /// A tap on the path canvas's tube or spool for @p slot.
    static void tap_lane(AmsPanel& p, int slot) {
        AmsPanel::on_path_slot_clicked(slot, &p);
    }
    static bool context_menu_open(const AmsPanel& p) {
        return p.context_menu_ && p.context_menu_->is_visible();
    }
    static bool is_open(const AmsOverviewPanel& p) {
        return p.open_;
    }
    static int units_refreshes(const AmsOverviewPanel& p) {
        return p.units_refreshes_;
    }
    static int pages_refreshes(const AmsOverviewPanel& p) {
        return p.pages_refreshes_;
    }
    static const std::vector<helix::ui::UnitPage>& pages(const AmsOverviewPanel& p) {
        return p.pages_;
    }
    static int shown_unit_position(const AmsOverviewPanel& p) {
        return p.shown_unit_pos_;
    }
    static lv_obj_t* path_canvas(const AmsOverviewPanel& p) {
        return p.detail_path_canvas_;
    }
    static lv_obj_t* prev_button(const AmsOverviewPanel& p) {
        return p.prev_button_;
    }
    static lv_obj_t* next_button(const AmsOverviewPanel& p) {
        return p.next_button_;
    }
    static lv_obj_t* unit_view(const AmsOverviewPanel& p) {
        return p.detail_container_;
    }
    static lv_obj_t* cards_row(const AmsOverviewPanel& p) {
        return p.cards_row_;
    }
    static lv_obj_t* system_path(const AmsOverviewPanel& p) {
        return p.system_path_;
    }
    static std::vector<lv_obj_t*> unit_cards(const AmsOverviewPanel& p) {
        std::vector<lv_obj_t*> out;
        for (const auto& uc : p.unit_cards_)
            out.push_back(uc.card);
        return out;
    }
    static std::vector<int> unit_card_units(const AmsOverviewPanel& p) {
        std::vector<int> out;
        for (const auto& uc : p.unit_cards_)
            out.push_back(uc.unit_index);
        return out;
    }
    static lv_obj_t* page_dots(const AmsOverviewPanel& p) {
        return p.page_dots_;
    }
    static void resync(AmsOverviewPanel& p) {
        p.sync_pages(/*reopen=*/false);
    }
    static bool in_unit_view(const AmsOverviewPanel& p) {
        return p.unit_view_active_;
    }
};

} // namespace helix
