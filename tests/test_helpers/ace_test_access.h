// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/ace_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_ace.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "hv/json.hpp"

namespace helix {

// Friend-class shim for AmsBackendAce -- declared as friend in the backend
// header. Gives tests narrow accessors for override state without going
// through public get_slot_info (which layers apply_overrides on top).
class AceTestAccess {
  public:
    static void seed_override(AmsBackendAce& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }

    /// Seed a running dry cycle so an update has elapsed time to preserve.
    static void set_dryer_run(AmsBackendAce& b, float target_c, int duration_min,
                              int remaining_min) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.dryer_info_.active = true;
        b.dryer_info_.target_temp_c = target_c;
        b.dryer_info_.duration_min = duration_min;
        b.dryer_info_.remaining_min = remaining_min;
    }
    static std::optional<helix::ams::FilamentSlotOverride> get_override(const AmsBackendAce& b,
                                                                        int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    static void inject_override_store(AmsBackendAce& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    // Drive parse_ace_object directly (public-ish parse entry point on the
    // production class) -- saves every test from rebuilding a full status
    // notify envelope.
    static void parse_ace(AmsBackendAce& b, const nlohmann::json& data) {
        b.parse_ace_object(data);
    }

    // Drive the production subscription entry point with a whole
    // notify_status_update envelope, which is what decides the hub object key
    // and the REST-fallback bail before any parsing happens.
    static void handle_status_update(AmsBackendAce& b, const nlohmann::json& notification) {
        b.handle_status_update(notification);
    }

    // Drive the REST bridge's /server/ace/slots parse. It is a second parser
    // with its own key ladder, and no seeder reaches it: poll_slots() needs a
    // live Moonraker.
    static bool parse_slots(AmsBackendAce& b, const nlohmann::json& data) {
        return b.parse_slots_response(data);
    }

    // Expose the on_started() subscription-vs-REST decision (#1069). Returns
    // the slot-bearing object, or nullptr when the REST fallback should run.
    static const nlohmann::json* select_slot_bearing_object(const nlohmann::json& status,
                                                            std::string* key) {
        return AmsBackendAce::select_slot_bearing_object(status, key);
    }

    // Drive the individual REST poll methods (private; async-defer their work
    // through the UpdateQueue, so tests must drain the queue afterward).
    static void poll_info(AmsBackendAce& b) {
        b.poll_info();
    }
    static void poll_status(AmsBackendAce& b) {
        b.poll_status();
    }
    static void poll_slots(AmsBackendAce& b) {
        b.poll_slots();
    }
};
} // namespace helix
