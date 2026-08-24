// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file filament_consumption_tracker_test_access.h
 * @brief Shared friend struct for peeking at FilamentConsumptionTracker privates.
 *
 * Test-only access helper per CLAUDE L065: tracker has no production-facing
 * test setters; instead, tests that need private access declare friendship via
 * this struct. Kept in tests/unit/ so production headers only `friend struct`
 * the type by name, not by including this header.
 */

#include "consumption_sink.h"
#include "filament_consumption_tracker.h"
#include "printer_state.h"

#include <cstdint>

namespace helix {

struct FilamentConsumptionTrackerTestAccess {
    /// Poke the ExternalSpoolSink's persist-interval throttle in the registry.
    /// Used by tests that want writes on every tick instead of every 60s.
    ///
    /// The sink is installed by the first start() in the process, so this is a
    /// no-op when called before then. Returns false in that case so callers can
    /// REQUIRE the override actually landed instead of silently testing the
    /// production 60s interval.
    [[nodiscard]] static bool set_persist_interval(FilamentConsumptionTracker& t, uint32_t ms) {
        for (auto& s : t.sinks_) {
            if (s->kind() == SinkKind::ExternalSpool) {
                static_cast<ExternalSpoolSink*>(s.get())->set_persist_interval_ms_override(ms);
                return true;
            }
        }
        return false;
    }

    /// Drive the tracker's print-state observer directly, bypassing the
    /// print_state_enum_ subject (useful when the test sets up fixtures before
    /// the aggregate observer fires).
    static void force_print_state(PrintJobState s) {
        FilamentConsumptionTracker::instance().on_print_state_changed(s);
    }

    /// Invoke the per-extruder handler directly — lets routing tests verify
    /// the dispatch logic without needing the full observer plumbing to fire.
    static void on_extruder_filament_used(int extruder_idx, int mm) {
        FilamentConsumptionTracker::instance().on_extruder_filament_used_changed(extruder_idx, mm);
    }

    /// Count of registered sinks (any kind).
    static std::size_t sink_count() {
        return FilamentConsumptionTracker::instance().sinks_.size();
    }

    /// Count of AmsSlotSink instances (ignores ExternalSpoolSink).
    static std::size_t ams_sink_count() {
        std::size_t count = 0;
        for (const auto& s : FilamentConsumptionTracker::instance().sinks_) {
            if (s->kind() == SinkKind::AmsSlot) {
                ++count;
            }
        }
        return count;
    }
};

} // namespace helix
