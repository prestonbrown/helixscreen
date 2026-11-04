// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/snapmaker_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_snapmaker.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "json_fwd.h"
#include "print_start_inputs.h"
#include "test_helpers/seeded_override.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace helix {

// Friend-class shim for AmsBackendSnapmaker -- declared as friend in the
// backend header. Provides narrow, purpose-built accessors for the private
// override and hardware-event-detection state so tests don't have to reach
// into the backend via public APIs (which layer apply_overrides on top and
// obscure what the internal maps actually hold).
class SnapmakerTestAccess {
  public:
    /// Takes a notify frame or a bare status object (a query result).
    static void handle_status(AmsBackendSnapmaker& b, const nlohmann::json& n) {
        if (n.contains("params")) {
            b.handle_status_update(n);
        } else {
            b.handle_status(n);
        }
    }
    /// Drive the boot path a live backend runs: construct the override store
    /// from the DB, load its records, and publish them. A unit test skips
    /// discovery, so without this the store an injected fixture installed is
    /// all a backend ever has.
    static void call_on_started(AmsBackendSnapmaker& b) {
        b.on_started();
    }
    /// Whether Resume is holding @p head for a load nothing has confirmed.
    static bool load_unconfirmed(AmsBackendSnapmaker& b, int head) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.load_unconfirmed_[static_cast<size_t>(head)];
    }
    static void seed_override(AmsBackendSnapmaker& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        {
            std::lock_guard<std::mutex> lock(b.mutex_);
            b.overrides_[slot_index] = ovr;
        }
        // The backend's own init files both stores together; a fixture that
        // wrote only this map would leave the lane reading empty.
        helix::test::file_override_as_lane_records(b, slot_index, ovr);
    }
    /// Replace the stored record alone, leaving the lane as it was filed: the
    /// shape of a record that moved on after the lane loaded it.
    static void replace_override_record(AmsBackendSnapmaker& b, int slot_index,
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
    /// Put a whole Spoolman link on the live slot — the shape a linked slot
    /// carries. The persisted override record has no filament-id field, so
    /// the live slot is the only half of a link a test can seed whole.
    static bool seed_live_spoolman_link(AmsBackendSnapmaker& b, int slot_index, int spool_id,
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
    static std::optional<std::string> last_rfid_uid(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.rfid_tracker_.baseline(slot_index);
    }
    /// End a refeed's wait for its head's frame, as its timer does.
    static void expire_refeed_wait(AmsBackendSnapmaker& b) {
        b.finish_refeed_wait();
    }
    /// Forget the print state, as a reconnect's on_started() does.
    static void forget_print_state(AmsBackendSnapmaker& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.print_state_.reset();
    }
    /// What start() sets once the subscription is up; filament ops refuse
    /// until it is.
    static void set_running(AmsBackendSnapmaker& b, bool running) {
        b.running_ = running;
    }
    /// Answer the print-file tools read without reading a file.
    static void set_file_tools_probe(
        AmsBackendSnapmaker& b,
        std::function<void(const std::string&, std::function<void(std::set<int>)>)> probe) {
        b.file_tools_probe_ = std::move(probe);
    }
    static void set_sensor_present(AmsBackendSnapmaker& b, int slot_index, bool present) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.sensor_filament_present_[slot_index] = present;
    }
    static void set_port_sensor_present(AmsBackendSnapmaker& b, int slot_index, bool present) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.port_sensor_filament_present_[slot_index] = present;
    }
    /// Force the AUTO_FEEDING_BATCH capability cache: a unit test has no
    /// discovery phase, so on_started() never populates it.
    static void set_use_batch_macro(AmsBackendSnapmaker& b, bool enabled) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.use_batch_macro_ = enabled;
    }
    /// Same reason as set_use_batch_macro: the status-object key the doing
    /// parse matches against comes from discovery, which a unit test skips.
    static void set_batch_macro_object(AmsBackendSnapmaker& b, std::string object) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.batch_macro_object_ = std::move(object);
    }
    /// Arm a batch plan exactly as do_filament_batch would, so the
    /// cursor-advance parse can be driven frame by frame; the mock client
    /// delivers a whole batch's frames in one drain.
    static void set_batch_plan(AmsBackendSnapmaker& b, std::vector<int> heads, bool load,
                               std::string direction_label, std::string of_label,
                               uint64_t dispatch_id = 0) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.batch_ = AmsBackendSnapmaker::BatchPlan{std::move(heads),    load,
                                                  /*cursor=*/0,
                                                  /*active=*/true,     std::move(direction_label),
                                                  std::move(of_label), dispatch_id};
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
    static ExtruderToolState extruder_state(const AmsBackendSnapmaker& b, int tool) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.extruder_states_[tool];
    }
    static int pending_insert_passes(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.pending_insert_passes_[slot_index];
    }
    static int last_published_port_present(const AmsBackendSnapmaker& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.last_published_port_present_;
    }
    static void set_current_tool(AmsBackendSnapmaker& b, int tool) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_tool = tool;
    }
};
} // namespace helix
