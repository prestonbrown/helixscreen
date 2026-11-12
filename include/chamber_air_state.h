// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Live chamber air readings for the connected printer, and the subjects the UI
// binds. helix::chamber_air decides what a status frame says; this class keeps
// the last answer for each reading (a frame that says nothing keeps it), and
// publishes them. Main thread only: PrinterState feeds it from the status path,
// which already runs on the main thread.
//
// Subjects (ints, registered as XML globals):
//   chamber_air_available        1 while the printer offers chamber air control
//                                and any detachable accessory it needs is fitted
//   chamber_air_mode             -1 unknown or not reported, else int(AirMode)
//   chamber_air_exhaust_pct      owned exhaust fan speed, 0-100
//   chamber_air_recirculate_pct  owned recirculation fan speed, 0-100
//   chamber_air_has_targets      1 while available and the modes carry targets
//   chamber_air_target_c, chamber_air_exhaust_target_pct,
//   chamber_air_recirculate_target_pct
//                                the stored mode targets (chamber_air_settings.h),
//                                which the settings sliders bind two-way

#include "chamber_air_modes.h"
#include "core/observer_guard.h"
#include "subject_managed_panel.h"

#include <functional>
#include <lvgl.h>
#include <optional>

namespace helix {

class PrinterDiscovery;

class ChamberAirState {
  public:
    /// Called on every change of availability, and once for the first reading after
    /// set_hardware() with @p at_startup true. Only for printers whose presence is a
    /// live reading.
    using PresenceEdge = std::function<void(bool present, bool at_startup)>;

    ChamberAirState() = default;
    ChamberAirState(const ChamberAirState&) = delete;
    ChamberAirState& operator=(const ChamberAirState&) = delete;

    void init_subjects(bool register_xml = true);
    void deinit_subjects();

    /// A new printer: forget every reading and take its provider's shape.
    void set_hardware(const PrinterDiscovery& hw);

    /// Merge one status frame's news; nullopt fields keep the last value.
    void apply(const chamber_air::Reading& r);

    void set_presence_edge_handler(PresenceEdge handler);

    [[nodiscard]] bool available() const;
    [[nodiscard]] std::optional<chamber_air::AirMode> mode() const;
    [[nodiscard]] std::optional<double> filter_seconds() const;
    [[nodiscard]] std::optional<double> critical_temp_c() const;
    [[nodiscard]] int speed_pct(chamber_air::FanRole role) const;
    [[nodiscard]] std::optional<int> threshold_pct(chamber_air::FanRole role) const;

    /// The targets the settings sliders hold right now.
    [[nodiscard]] chamber_air::ModeTargets targets() const;
    /// Stores the sliders' targets for the active printer.
    void commit_targets();

    lv_subject_t* available_subject() {
        return &chamber_air_available_;
    }
    lv_subject_t* mode_subject() {
        return &chamber_air_mode_;
    }
    /// The firmware filter counter in whole minutes, -1 when none has been reported.
    /// C++ only: it has no XML name.
    lv_subject_t* filter_minutes_subject() {
        return &chamber_air_filter_minutes_;
    }
    lv_subject_t* exhaust_pct_subject() {
        return &chamber_air_exhaust_pct_;
    }
    lv_subject_t* recirculate_pct_subject() {
        return &chamber_air_recirculate_pct_;
    }

  private:
    void publish();

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    bool supported_ = false;
    bool live_presence_ = false;
    bool takes_targets_ = false;
    bool startup_pending_ = false;
    std::optional<bool> present_;
    chamber_air::Reading last_{};
    PresenceEdge on_edge_;

    lv_subject_t chamber_air_available_{};
    lv_subject_t chamber_air_mode_{};
    lv_subject_t chamber_air_exhaust_pct_{};
    lv_subject_t chamber_air_recirculate_pct_{};
    lv_subject_t chamber_air_has_targets_{};
    lv_subject_t chamber_air_filter_minutes_{};
    lv_subject_t chamber_air_target_c_{};
    lv_subject_t chamber_air_exhaust_target_pct_{};
    lv_subject_t chamber_air_recirculate_target_pct_{};
    /// chamber_air_target_c as the user reads it ("50°C", "122°F").
    lv_subject_t chamber_air_target_text_{};
    char chamber_air_target_text_buf_[16] = {};
    ObserverGuard target_text_observer_;
};

} // namespace helix
