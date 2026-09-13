// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/snapmaker_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_snapmaker.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "hv/json.hpp"

namespace helix {

// Friend-class shim for AmsBackendSnapmaker -- declared as friend in the
// backend header. Provides narrow, purpose-built accessors for the private
// override and hardware-event-detection state so tests don't have to reach
// into the backend via public APIs (which layer apply_overrides on top and
// obscure what the internal maps actually hold).
class SnapmakerTestAccess {
  public:
    static void handle_status(AmsBackendSnapmaker& b, const nlohmann::json& n) {
        b.handle_status_update(n);
    }
    static void seed_override(AmsBackendSnapmaker& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }
    static std::optional<helix::ams::FilamentSlotOverride>
    get_override(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    static void inject_override_store(AmsBackendSnapmaker& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    static std::optional<std::string> last_rfid_uid(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.rfid_tracker_.baseline(slot_index);
    }
    static void set_sensor_present(AmsBackendSnapmaker& b, int slot_index, bool present) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.sensor_filament_present_[slot_index] = present;
    }
    static void set_port_sensor_present(AmsBackendSnapmaker& b, int slot_index, bool present) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.port_sensor_filament_present_[slot_index] = present;
    }
    static void set_current_slot(AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_slot = slot_index;
    }
    // The channel_state-driven "loaded at toolhead" latch. Read directly so
    // tests can assert the latch independently of the query methods that
    // consume it.
    static bool loaded_at_toolhead(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.loaded_at_toolhead_[slot_index];
    }
    static void set_loaded_at_toolhead(AmsBackendSnapmaker& b, int slot_index, bool loaded) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.loaded_at_toolhead_[slot_index] = loaded;
    }
    static void set_current_tool(AmsBackendSnapmaker& b, int tool) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_tool = tool;
    }
};
} // namespace helix
