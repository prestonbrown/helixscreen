// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "panel_widget.h"
#include "print_history_manager.h"
#include "src/ui/panel_widgets/print_stats_tile_layout.h"

#include <lvgl.h>
#include <string>

namespace helix {

/// Home tile for print activity. Sums the cached history into this week's print hours
/// and seven local-day bars, pairs them with the server's lifetime totals when those
/// have arrived, measures what each rung would draw, decides the rung with
/// decide_print_stats_tile_layout(), and publishes the verdict and the texts as
/// subjects that panel_widget_print_stats.xml binds. A tap opens the history dashboard.
class PrintStatsWidget : public PanelWidget {
  public:
    PrintStatsWidget();
    ~PrintStatsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "print_stats";
    }

    static void print_stats_clicked_cb(lv_event_t* e);

  protected:
    /// Drop the cached tile pointers and expire the in-flight totals fetch when
    /// the tile tree dies without a detach() (screen teardown).
    void on_hooked_root_deleted() override;

  private:
    /// Ask Moonraker for its server-computed lifetime totals.
    ///
    /// The cached job list is capped (PrintHistoryManager::fetch(limit)), so it
    /// cannot answer "all time" on a printer with a longer history.
    void fetch_lifetime_totals();

    /// Recompute the content, verdict and texts, and publish them when this
    /// instance owns the live tile. Direct subject writes only, so it is safe
    /// inside a ScopedFreeze.
    void refresh();

    /// @p hero and @p label are the texts the Strip would draw, for choosing its fonts.
    [[nodiscard]] PrintStatsTileVerdict decide(int width_px, int height_px,
                                               PrintStatsContent content, bool lifetime_known,
                                               const std::string& hero = {},
                                               const std::string& label = {}) const;
    void handle_clicked();

    /// The support line's two levels, from the lifetime totals.
    [[nodiscard]] std::string count_line() const;
    [[nodiscard]] std::string full_line() const;

    friend class PrintStatsTestAccess;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    int granted_w_ = 0;
    int granted_h_ = 0;

    /// Server-computed lifetime totals; only meaningful once valid_ is set.
    PrintHistoryTotals lifetime_totals_{};
    bool lifetime_totals_valid_ = false;
    bool lifetime_totals_in_flight_ = false;

    helix::HistoryChangedCallback history_observer_;
    ObserverGuard language_observer_;
    helix::AsyncLifetimeGuard lifetime_;
};

void register_print_stats_widget();

} // namespace helix
