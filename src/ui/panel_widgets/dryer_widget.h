// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "dryer_summary.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/dryer_layout.h"

#include <functional>

namespace helix {

/// Home widget for filament drying. dryer_model summarises the three drying sources;
/// this widget measures what each rung would draw, decides the rung with
/// decide_dryer_layout() and publishes the verdict and the texts that
/// panel_widget_dryer.xml binds. A tap opens the environment overlay, or the chamber or bed
/// modal when no box has a dryer; at Full a tap on a row opens that row's source. The tile
/// never starts or stops a cycle.
class DryerWidget : public PanelWidget {
  public:
    DryerWidget() = default;
    ~DryerWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "dryer";
    }

    static void clicked_cb(lv_event_t* e);

  private:
    struct SummaryState;
    static SummaryState read_state();
    DryerVerdict decide(int width_px, int height_px, const SummaryState& state) const;
    void publish_verdict();
    void publish_texts();
    void on_summary_changed();

    DryerVerdict verdict_;
    /// The state the verdict was decided for; a change re-decides, because the content
    /// being measured changed.
    bool decided_drying_ = false;
    bool decided_multi_ = false;
    bool decided_progress_known_ = false;
    int decided_sources_ = 0;

    ObserverGuard version_observer_;
    ObserverGuard language_observer_;
    ObserverGuard temp_unit_observer_;

    // Declared last so it is torn down first, expiring every token the observer
    // captured before it is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_dryer_widget();

/// Replaces what a tap opens, so a test can see which source a tap routed to. Pass an empty
/// function to restore the real overlays and modals.
void set_dryer_open_hook_for_test(std::function<void(const DryerTap&)> hook);

} // namespace helix
