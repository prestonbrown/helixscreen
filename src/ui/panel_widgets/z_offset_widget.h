// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/z_offset_layout.h"

#include <optional>

namespace helix {

class PrinterState;

/// Home widget for the live Z-offset. Measures what each rung would draw, decides the
/// rung with decide_z_offset_layout(), and publishes the verdict and the readings as
/// subjects that panel_widget_z_offset.xml binds. A tap opens the Tune overlay; the
/// Wide rung adds Save and the Full rung adds baby-step buttons.
class ZOffsetWidget : public PanelWidget {
  public:
    explicit ZOffsetWidget(PrinterState& printer_state);
    ~ZOffsetWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "z_offset";
    }

    static void clicked_cb(lv_event_t* e);

    /// One baby step of the persisted amount: +1 farther from the bed, -1 closer.
    /// Does nothing while the steppers are not on screen.
    void step(int direction);
    /// Advance the step amount to the next entry of the shared table.
    void cycle_step();
    /// Save every unsaved offset. Does nothing unless the Save button is on screen,
    /// there is something to save and no job holds the machine.
    void save();

  private:
    struct LiveState;
    LiveState read_live() const;
    ZOffsetVerdict decide(int width_px, int height_px, bool tool_changer) const;
    void publish(const LiveState& live);
    lv_obj_t* find_indicator() const;
    void on_live_changed();
    void open_tune_overlay();

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    ZOffsetVerdict verdict_;
    /// Whether the verdict was decided for a tool changer; a change re-decides,
    /// because the Full box then carries the tool label.
    bool decided_tool_changer_ = false;
    /// The offset a run of baby steps started from; travel is bounded from it.
    double session_base_mm_ = 0.0;
    /// What the indicator last drew, so an unchanged reading costs no redraw.
    std::optional<int> indicator_microns_;

    ObserverGuard offset_observer_;
    ObserverGuard persisted_observer_;
    ObserverGuard persisted_valid_observer_;
    ObserverGuard pending_observer_;
    ObserverGuard print_active_observer_;
    ObserverGuard save_available_observer_;
    ObserverGuard tools_version_observer_;
    ObserverGuard active_tool_observer_;
    ObserverGuard per_tool_observer_;

    // Declared last so it is torn down first, expiring every token the
    // observers captured before any of them is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_z_offset_widget();

} // namespace helix
