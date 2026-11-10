// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/host_health_layout.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace helix {

class PrinterState;

/// Home widget for host and MCU health. Measures what each rung would draw, decides the
/// rung with decide_host_health_layout(), and publishes the verdict, the worst status
/// with its cause and the readings as subjects that panel_widget_host_health.xml binds.
/// Sparklines and per-MCU rows read the performance state directly. A tap opens the
/// Performance overlay.
class HostHealthWidget : public PanelWidget {
  public:
    explicit HostHealthWidget(PrinterState& printer_state);
    ~HostHealthWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool fits_at(int width_px, int height_px) const override;
    const char* id() const override {
        return "host_health";
    }

    static void clicked_cb(lv_event_t* e);

  private:
    struct Sample;
    void refresh_mcu_names();
    Sample read_sample();
    HostHealthVerdict decide(int width_px, int height_px, bool printing, int mcu_count) const;
    void on_sample();
    void publish_status(const Sample& sample);
    void publish_texts(const Sample& sample);
    void sync_mcu_rows();
    bool is_printing() const;

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;

    HostHealthVerdict verdict_;
    /// The state the verdict was decided for; a change re-decides, because the
    /// content being measured changed.
    bool decided_printing_ = false;
    int decided_mcu_count_ = 0;

    /// perf_mcu_names as last parsed, and the MCUs it names.
    std::string names_key_;
    std::vector<std::string> mcu_names_;
    /// Each MCU's cumulative retransmit count at the previous sample.
    std::unordered_map<std::string, int> prev_retransmits_;
    /// The perf_history_tick the retransmit baseline was last advanced for, and whether that
    /// sample saw a rise. Only a new tick advances the baseline, so a re-decide for a size
    /// or print_active change between ticks reads the same answer instead of consuming it.
    int baseline_tick_ = -2;
    bool retransmits_rose_ = false;
    /// Whether this instance is counted in PerformanceState as showing the disk.
    bool disk_consumer_ = false;
    /// The MCU rows currently built into the Full rung's list, by names joined.
    std::string built_rows_key_;

    ObserverGuard sample_observer_;
    ObserverGuard print_active_observer_;

    // Declared last so it is torn down first, expiring every token the
    // observers captured before any of them is destroyed.
    helix::AsyncLifetimeGuard lifetime_;
};

void register_host_health_widget();

} // namespace helix
