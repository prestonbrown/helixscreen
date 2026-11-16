// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "filament_database.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace helix {

/// Concrete sink type tag. Used instead of `dynamic_cast` for branch logic
/// (Phase 5 per-extruder routing filters out AMS sinks from external sinks
/// via this tag; the tracker test helper uses it to count AMS sinks).
enum class SinkKind : uint8_t {
    ExternalSpool,
    AmsSlot,
};

/// Target for per-stream filament consumption updates. Owns its own baseline
/// (snapshot_mm + snapshot_weight_g), material density, and persistence throttle.
/// Tracker calls these hooks in response to print lifecycle + filament_used
/// subject events.
class IConsumptionSink {
  public:
    virtual ~IConsumptionSink() = default;

    /// Concrete sink type tag. Prefer this over `dynamic_cast` for branch logic.
    [[nodiscard]] virtual SinkKind kind() const = 0;

    /// Stable identifier for logging ("external", "ams:0:2", ...).
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// True when this sink has a valid baseline + density. Tracker skips
    /// apply_delta() when false; sink may become trackable later (e.g. user
    /// sets weight mid-print) and rebaseline() fires on the next delta.
    [[nodiscard]] virtual bool is_trackable() const = 0;

    /// Capture the current remaining_weight_g + filament_used_mm baseline.
    /// Called on print start and when a sink is registered mid-print with a
    /// valid state.
    virtual void snapshot(float filament_used_mm) = 0;

    /// Apply the cumulative filament_used_mm delta since snapshot. Sink
    /// converts mm→g and writes remaining_weight_g back to its store. Must be
    /// monotonically non-decreasing between snapshots; the sink handles resets
    /// internally by rebaselining.
    virtual void apply_delta(float filament_used_mm) = 0;

    /// Flush any in-memory changes to persistent store. Called on print end,
    /// pause, and unregister.
    virtual void flush() = 0;

    /// Reset baseline to the currently-observed state. Used when an external
    /// writer (Spoolman poll, user edit) has updated remaining_weight_g
    /// between ticks.
    virtual void rebaseline(float filament_used_mm) = 0;
};

/// Sink for the external (bypass) spool. Each tick files the counted weight as
/// the bypass lane's Metered source; the bypass record itself is rewritten on
/// a throttle (AmsState::set_external_spool_info()) and at flush().
class ExternalSpoolSink : public IConsumptionSink {
  public:
    [[nodiscard]] SinkKind kind() const override {
        return SinkKind::ExternalSpool;
    }
    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] bool is_trackable() const override;
    void snapshot(float filament_used_mm) override;
    void apply_delta(float filament_used_mm) override;
    void flush() override;
    void rebaseline(float filament_used_mm) override;

    /// Test hook: override persist throttle (ms). 0 = default 60s. Plain-class
    /// sink (not a singleton) makes this setter acceptable for testing
    /// per-sink throttle behavior (see CLAUDE L065).
    void set_persist_interval_ms_override(uint32_t ms) {
        persist_interval_override_ms_ = ms;
    }

  private:
    bool active_ = false;
    float snapshot_mm_ = 0.0f;
    float snapshot_weight_g_ = 0.0f;
    float density_g_cm3_ = 0.0f;
    float diameter_mm_ = filament::kDefaultDiameterMm;
    float last_written_weight_g_ = 0.0f;
    /// The weight the bypass record was last written with (or read at); a
    /// record that moves away from it was written by someone else.
    float last_persisted_weight_g_ = 0.0f;
    uint32_t last_persist_tick_ms_ = 0;
    uint32_t persist_interval_override_ms_ = 0;

    [[nodiscard]] uint32_t persist_interval_ms() const;
    /// Write @p info with the metered weight as the bypass record.
    void persist(SlotInfo info);
};

/// Sink for a single AMS slot. Reads current SlotInfo via
/// AmsBackend::get_slot_info() and writes updates via
/// AmsBackend::update_slot_weight(), which routes persistence through the backend's
/// override store (Moonraker `lane_data/<backend>/lane<N>`).
///
/// Gating (per spec 4): a slot sink is trackable only when all of:
///   - backend exists and `tracks_consumption_natively()` is false
///   - `remaining_weight_g >= 0`
///   - `spoolman_id == 0` (Spoolman path handles writeback itself)
///   - material density resolvable via `filament::find_material`
///
/// Gating is re-evaluated on each apply_delta tick so that mid-print changes
/// (user links Spoolman, user edits weight) are respected immediately.
class AmsSlotSink : public IConsumptionSink {
  public:
    AmsSlotSink(int backend_index, int slot_index);

    [[nodiscard]] SinkKind kind() const override {
        return SinkKind::AmsSlot;
    }
    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] bool is_trackable() const override;
    void snapshot(float filament_used_mm) override;
    void apply_delta(float filament_used_mm) override;
    void flush() override;
    void rebaseline(float filament_used_mm) override;

    [[nodiscard]] int backend_index() const {
        return backend_index_;
    }
    [[nodiscard]] int slot_index() const {
        return slot_index_;
    }

    /// Test hook: override persist throttle (ms). 0 = default 60s.
    void set_persist_interval_ms_override(uint32_t ms) {
        persist_interval_override_ms_ = ms;
    }

  private:
    const int backend_index_;
    const int slot_index_;
    std::string name_; // "ams:<backend>:<slot>"

    bool active_ = false;
    float snapshot_mm_ = 0.0f;
    float snapshot_weight_g_ = 0.0f;
    float density_g_cm3_ = 0.0f;
    float diameter_mm_ = filament::kDefaultDiameterMm;
    float last_written_weight_g_ = 0.0f;
    uint32_t last_persist_tick_ms_ = 0;
    uint32_t persist_interval_override_ms_ = 0;

    [[nodiscard]] uint32_t persist_interval_ms() const;
    /// Fetch the current SlotInfo from the backend, or nullopt if the backend
    /// is no longer registered / the slot went away.
    [[nodiscard]] std::optional<SlotInfo> current_info() const;
};

} // namespace helix
