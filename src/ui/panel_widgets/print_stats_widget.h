// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"
#include "print_history_manager.h"

#include <lvgl.h>
#include <memory>

namespace helix {

class PrintStatsWidget : public PanelWidget {
  public:
    PrintStatsWidget();
    ~PrintStatsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "print_stats";
    }

    static void print_stats_clicked_cb(lv_event_t* e);

  private:
    void update_stats();
    void handle_clicked();

    /// Ask Moonraker for its server-computed lifetime totals.
    ///
    /// The cached job list is capped (PrintHistoryManager::fetch(limit)), so it
    /// cannot answer "all time" on a printer with a longer history. Same source
    /// HistoryDashboardPanel uses for its ALL_TIME filter.
    void fetch_lifetime_totals();

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    /// Server-computed lifetime totals; only meaningful once valid_ is set.
    PrintHistoryTotals lifetime_totals_{};
    bool lifetime_totals_valid_ = false;
    bool lifetime_totals_in_flight_ = false;

    helix::HistoryChangedCallback history_observer_;
    helix::AsyncLifetimeGuard lifetime_;
};

void register_print_stats_widget();

} // namespace helix
