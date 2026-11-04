// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_factory.h"
#include "filament_insert_policy.h"
#include "i_moonraker_api.h"

#include <optional>
#include <vector>

namespace helix {

class AmsBackend;

/// Opens the per-slot editor when a person newly inserts filament (#1335).
///
/// Two sources, one decision (should_open_editor_on_insert):
///  - AMS lanes with a prep sensor going empty to present, unless the lane's
///    tag reader names the spool (lane_insert_action);
///  - the ENTRY/TOOLHEAD sensors going empty to present, for a manual load on a
///    printer with no AMS or with its bypass engaged. The target is the external
///    spool.
/// Baselines are re-seeded on every printer (re)connect, Klipper restart and sensor
/// reconfiguration, so a snapshot of what is already loaded never reads as an insertion.
class FilamentInsertWatcher {
  public:
    explicit FilamentInsertWatcher(IMoonrakerAPI* api);
    void start(); ///< One-shot; installs the observers

    /// One judging pass over the lanes: updates each lane's baseline and held
    /// insert, claims the insert prompt for a lane it asks or waits on, and
    /// returns the lane whose editor to open, or -1. @p ctx carries the guards
    /// (setting, print, operation, editor); its presence fields are per lane.
    int judge_lanes(const AmsBackend& backend, FilamentInsertContext ctx);

    /// Forget every baseline, held insert and prompt claim: a reconnect, Klipper
    /// restart or sensor reconfiguration moves readings with no insert.
    void reseed();

  private:
    void on_ams_changed();
    void on_sensor_changed();
    [[nodiscard]] bool operation_busy() const;
    void open_lane(int slot_index);
    void open_external();

    IMoonrakerAPI* api_;
    ObserverGuard ams_observer_;
    ObserverGuard entry_observer_;
    ObserverGuard toolhead_observer_;
    ObserverGuard connection_observer_;
    ObserverGuard klippy_observer_;
    ObserverGuard config_observer_;
    /// Last stated presence per lane; empty means "seed on the next tick".
    std::vector<std::optional<bool>> lane_prev_;
    /// Lanes whose insert waits on the tag reader's verdict (slot_insert_identity).
    std::vector<bool> lane_waiting_;
    std::optional<bool> sensor_prev_;
};

} // namespace helix
