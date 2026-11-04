// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/ace_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_ace.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "json_fwd.h"
#include "test_helpers/seeded_override.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace helix {

// Friend-class shim for AmsBackendAce -- declared as friend in the backend
// header. Gives tests narrow accessors for override state without going
// through public get_slot_info (which layers apply_overrides on top).
class AceTestAccess {
  public:
    static void seed_override(AmsBackendAce& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        {
            std::lock_guard<std::mutex> lock(b.mutex_);
            b.overrides_[slot_index] = ovr;
        }
        // The backend's own init files both stores together; a fixture that
        // wrote only this map would leave the lane reading empty.
        helix::test::file_override_as_lane_records(b, slot_index, ovr);
    }

    /// Put a whole Spoolman link on the live slot — the shape a linked slot
    /// carries. The persisted override record has no filament-id field, so
    /// the live slot is the only half of a link a test can seed whole.
    static bool seed_live_spoolman_link(AmsBackendAce& b, int slot_index, int spool_id,
                                        int filament_id, int vendor_id) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        if (b.system_info_.units.empty())
            return false;
        SlotInfo* slot = b.system_info_.units[0].get_slot(slot_index);
        if (!slot)
            return false;
        slot->spoolman_id = spool_id;
        slot->spoolman_filament_id = filament_id;
        slot->spoolman_vendor_id = vendor_id;
        slot->spool_name = "Linked Spool";
        return true;
    }

    /// State the driver's seat and publish it, without a frame.
    static void set_seat(AmsBackendAce& b, int slot, bool loaded) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_slot = slot;
        b.seat_loaded_ = loaded;
        b.system_info_.filament_loaded = loaded;
    }

    /// Append a unit of @p slot_count slots after the existing ones, numbered
    /// as the next unit_index.
    static void append_unit(AmsBackendAce& b, int slot_count) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        AmsUnit unit;
        unit.unit_index = static_cast<int>(b.system_info_.units.size());
        unit.first_slot_global_index = b.system_info_.total_slots;
        unit.slot_count = slot_count;
        unit.slots.resize(static_cast<size_t>(slot_count));
        b.system_info_.units.push_back(unit);
        b.system_info_.total_slots += slot_count;
    }

    /// Seed a running dry cycle so an update has elapsed time to preserve.
    static void set_dryer_run(AmsBackendAce& b, float target_c, int duration_min,
                              int remaining_min) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        DryerInfo& dryer = b.dryer_locked(0);
        dryer.active = true;
        dryer.target_temp_c = target_c;
        dryer.duration_min = duration_min;
        dryer.remaining_min = remaining_min;
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
    static const nlohmann::json* select_ace_object(const nlohmann::json& status, std::string* key) {
        return AmsBackendAce::select_ace_object(status, key);
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

    // Runs only the startup store load: a null client makes on_started return
    // right after it.
    static void start_load(AmsBackendAce& b) {
        b.on_started();
    }

    // --- HelixPrint plugin vendor push ---
    /// Take the REST bridge path without starting its polling thread.
    static void set_rest_fallback(AmsBackendAce& b, bool on) {
        b.use_rest_fallback_ = on;
    }
    static void on_vendor_state(AmsBackendAce& b, const nlohmann::json& payload) {
        b.on_vendor_state(payload);
    }
    static void handle_status(AmsBackendAce& b, const nlohmann::json& status) {
        b.handle_status(status);
    }
    static bool vendor_pushing(const AmsBackendAce& b) {
        return b.vendor_push_.pushing();
    }
    static int data_fetch_failures(const AmsBackendAce& b) {
        return b.data_fetch_failures_.load();
    }
    static bool rest_data_poll_due(bool pushing, std::chrono::steady_clock::duration since_last) {
        return AmsBackendAce::rest_data_poll_due(pushing, since_last);
    }
    static int rest_data_polls(const AmsBackendAce& b) {
        return b.rest_data_polls_.load();
    }
    static void on_stopping(AmsBackendAce& b) {
        b.on_stopping();
    }
};
} // namespace helix
