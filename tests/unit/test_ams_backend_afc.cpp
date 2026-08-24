// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "ams_types.h"
#include "config.h"
#include "error_event.h"
#include "filament_op_router.h"
#include "moonraker_api.h"
#include "settings_manager.h"
#include "test_helpers/scoped_home_confirm_prompter.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
/**
 * @brief Test helper class providing access to AmsBackendAfc internals
 *
 * This class provides controlled access to private members for unit testing.
 * It does NOT start the backend (no Moonraker connection needed).
 */
class AmsBackendAfcTestHelper : public AmsBackendAfc {
  public:
    AmsBackendAfcTestHelper() : AmsBackendAfc(nullptr, nullptr) {}

    // Version is display/diagnostics only — nothing gates on it (AFC stopped
    // writing the afc-install namespace in its #451). Kept so tests can assert
    // that behavior is INDEPENDENT of whatever version is reported.
    void set_afc_version(const std::string& version) {
        afc_version_ = version;
    }

    // Sensor state setters for compute_filament_segment_unlocked testing
    void set_tool_end_sensor(bool state) {
        tool_end_sensor_ = state;
    }
    void set_tool_start_sensor(bool state) {
        tool_start_sensor_ = state;
    }
    void set_hub_sensor(const std::string& hub_name, bool state) {
        hub_sensors_[hub_name] = state;
    }

    // Convenience overload for single-hub backward compat in tests
    void set_hub_sensor(bool state) {
        // Set/clear on a default hub name for single-hub tests
        if (state) {
            hub_sensors_["default"] = true;
        } else {
            hub_sensors_.clear();
        }
    }

    // Lane → hub routing, as parsed from AFC_stepper.hub ("Turtle_1" or "direct").
    void set_lane_hub_routing(const std::string& lane_name, const std::string& hub_name) {
        lane_hub_routing_[lane_name] = hub_name;
    }

    // Lane AFC currently names as active (AFC.current_load / AFC.current_lane),
    // as tracked by parse_afc_state() — used to attribute a shared hub sensor.
    void set_active_load_lane(const std::string& lane_name) {
        active_load_lane_ = lane_name;
    }

    std::string get_active_load_lane() const {
        return active_load_lane_;
    }

    // Lane AFC reports as gripped by the extruder (AFC.current_load only), as
    // tracked by parse_afc_state() — distinct from active_load_lane_, which
    // prefers the transient AFC.current_lane.
    void set_toolhead_lane(const std::string& lane_name) {
        toolhead_lane_ = lane_name;
    }

    std::string get_toolhead_lane() const {
        return toolhead_lane_;
    }

    void set_current_lane(const std::string& lane_name) {
        current_lane_name_ = lane_name;
    }

    void initialize_test_lanes(int count) {
        std::vector<std::string> names;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i + 1));
        }
        initialize_slots(names);
    }

    // 0-based lane naming: lane0, lane1, ... lane{N-1} (matches real AFC hardware)
    void initialize_test_lanes_zero_based(int count) {
        std::vector<std::string> names;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i));
        }
        initialize_slots(names);
    }

    void set_lane_prep_sensor(int lane_index, bool state) {
        auto* entry = slots_.get_mut(lane_index);
        if (entry)
            entry->sensors.prep = state;
    }

    void set_lane_load_sensor(int lane_index, bool state) {
        auto* entry = slots_.get_mut(lane_index);
        if (entry)
            entry->sensors.load = state;
    }

    void set_lane_loaded_to_hub(int lane_index, bool state) {
        auto* entry = slots_.get_mut(lane_index);
        if (entry)
            entry->sensors.loaded_to_hub = state;
    }

    // AFC_stepper.extruder — which extruder this lane feeds. Present whether or
    // not the lane is seated, unlike the lane_loaded back-reference.
    void set_lane_extruder(int lane_index, const std::string& extruder_name) {
        auto* entry = slots_.get_mut(lane_index);
        if (entry)
            entry->info.extruder_name = extruder_name;
    }

    // AFC_extruder.lane_loaded — the lane this extruder currently holds.
    void set_extruder_lane_loaded(const std::string& extruder_name, const std::string& lane_name) {
        extruder_sensors_[extruder_name].lane_loaded = lane_name;
    }

    // AFC_extruder.is_standalone (v1.2.0+ only publishes it).
    void report_extruder_standalone(const std::string& extruder_name, bool standalone) {
        tool_states_[extruder_name].is_standalone = standalone;
    }

    void set_running(bool state) {
        running_ = state;
    }

    void set_filament_loaded(bool state) {
        system_info_.filament_loaded = state;
    }

    void set_current_slot(int slot) {
        system_info_.current_slot = slot;
    }

    void set_supports_bypass(bool state) {
        system_info_.supports_bypass = state;
    }

    // Backdate the drain arm's deadline so tests can simulate "the window has
    // expired" without a real sleep. Pass a negative offset to expire it.
    void set_message_drain_deadline_offset(std::chrono::seconds offset) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_drain_deadline_ = std::chrono::steady_clock::now() + offset;
    }

    PathSegment test_compute_filament_segment() const {
        return compute_filament_segment_unlocked();
    }

    void test_parse_afc_state(const nlohmann::json& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string deferred_error_event;
        bool current_slot_set_by_afc_state = false;
        bool afc_stated_unloaded = false;
        parse_afc_state(data, deferred_error_event, current_slot_set_by_afc_state,
                        afc_stated_unloaded);
    }

    // Discovery testing helpers
    int get_slot_count() const {
        return slots_.slot_count();
    }

    std::string get_slot_name(int index) const {
        return slots_.name_of(index);
    }

    const std::vector<std::string>& get_hub_names() const {
        return hub_names_;
    }

    void initialize_slots_from_discovery() {
        // Simulates what start() does when lanes are pre-set via set_discovered_lanes()
        if (!discovered_lane_names_.empty() && !slots_.is_initialized()) {
            initialize_slots(discovered_lane_names_);
        }
    }

    // Persistence testing helpers
    void initialize_test_lanes_with_slots(int count) {
        system_info_.units.clear();
        std::vector<std::string> names;

        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Box Turtle 1";
        unit.slot_count = count;
        unit.first_slot_global_index = 0;

        for (int i = 0; i < count; ++i) {
            std::string name = "lane" + std::to_string(i + 1);
            names.push_back(name);

            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            slot.status = SlotStatus::AVAILABLE;
            slot.mapped_tool = i;
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            unit.slots.push_back(slot);
        }

        system_info_.units.push_back(unit);
        system_info_.total_slots = count;
        slots_.initialize("Box Turtle 1", names);

        // Set mapped_tool on registry entries to match unit slot info
        for (int i = 0; i < count; ++i) {
            auto* s = get_mutable_slot(i);
            if (s)
                s->mapped_tool = i;
        }
    }

    SlotInfo* get_mutable_slot(int slot_index) {
        auto* entry = slots_.get_mut(slot_index);
        return entry ? &entry->info : nullptr;
    }

    // Initialize endless spool configs for reset testing
    void initialize_endless_spool_configs(int count) {
        for (int i = 0; i < count; ++i) {
            slots_.set_backup(i, -1);
        }
    }

    // Set a specific endless spool backup for testing
    void set_endless_spool_config(int slot, int backup) {
        slots_.set_backup(slot, backup);
    }

    // Set up multi-unit configuration and trigger reorganize
    void
    setup_multi_unit(const std::unordered_map<std::string, std::vector<std::string>>& unit_map) {
        unit_lane_map_ = unit_map;
        reorganize_slots();
    }

    // For persistence tests: capture G-code commands
    std::vector<std::string> captured_gcodes;

    // Override execute_gcode to capture commands for testing
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // Filament operations dispatch through the completion-callback form so the
    // macro's gcode ack can resolve the optimistic action (#1183). Capture it
    // too, and hold the callback so a test can fire the ack when it wants one.
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        captured_gcodes.push_back(gcode);
        pending_macro_ack = std::move(on_complete);
        return AmsErrorHelper::success();
    }

    std::function<void()> pending_macro_ack;

    // Override execute_gcode_notify to capture commands (avoids real API call)
    AmsError execute_gcode_notify(const std::string& gcode, const std::string& /*success_msg*/,
                                  const std::string& /*error_prefix*/) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // ------------------------------------------------------------------
    // LANE_UNLOAD dispatch capture
    //
    // The production dispatch_lane_unload() goes through api_->execute_gcode()
    // with success/error callbacks (the queue drains when Moonraker reports
    // completion). In tests, api_ is null. Override to capture the gcode AND
    // immediately signal completion so the queue advances synchronously —
    // letting tests assert on order/contents without async plumbing.
    //
    // Tests that need to inspect serialization should set
    // `defer_lane_unload_complete = true` and call `complete_pending_unload()`
    // manually to step through the queue one entry at a time.
    bool defer_lane_unload_complete = false;
    int pending_unload_completions = 0;

    void dispatch_lane_unload(const std::string& lane_name) override {
        captured_gcodes.push_back("LANE_UNLOAD LANE=" + lane_name);
        if (defer_lane_unload_complete) {
            ++pending_unload_completions;
        } else {
            on_lane_unload_done();
        }
    }

    void complete_pending_unload() {
        if (pending_unload_completions > 0) {
            --pending_unload_completions;
        }
        on_lane_unload_done();
    }

    // Directly seed pending_eject_lanes_ for clear_fault()'s discard test, taking
    // eject_queue_mutex_ the same way production code does.
    void test_queue_pending_eject(const std::string& lane_name) {
        std::lock_guard<std::mutex> lock(eject_queue_mutex_);
        pending_eject_lanes_.push_back(lane_name);
    }

    int test_pending_eject_count() {
        std::lock_guard<std::mutex> lock(eject_queue_mutex_);
        return static_cast<int>(pending_eject_lanes_.size());
    }

    void clear_captured_gcodes() {
        captured_gcodes.clear();
    }

    void clear_slot_override(int slot_index) {
        AmsBackendAfc::clear_slot_override(slot_index);
    }

    bool can_recover_lane_position(int slot_index) const {
        return AmsBackendAfc::can_recover_lane_position(slot_index);
    }

    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }

    bool has_gcode_starting_with(const std::string& prefix) const {
        for (const auto& gcode : captured_gcodes) {
            if (gcode.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    // How many recorded gcodes equal `expected` exactly. Used to verify the
    // message-queue drain sends AFC_CLEAR_MESSAGE once per queued entry.
    int gcode_count(const std::string& expected) const {
        return static_cast<int>(
            std::count(captured_gcodes.begin(), captured_gcodes.end(), expected));
    }

    static constexpr int MESSAGE_DRAIN_MAX_CLEARS = AmsBackendAfc::MESSAGE_DRAIN_MAX_CLEARS;

    void test_maybe_drain_message_queue() {
        maybe_drain_message_queue();
    }

    // Position of the first gcode starting with `prefix`, or -1 if absent.
    // Emission order matters on AFC: SET_SPOOL_ID with an empty value runs
    // AFC's clear_values(), which wipes material/color/weight/temps.
    int gcode_index_of(const std::string& prefix) const {
        for (size_t i = 0; i < captured_gcodes.size(); ++i) {
            if (captured_gcodes[i].rfind(prefix, 0) == 0)
                return static_cast<int>(i);
        }
        return -1;
    }

    // Feed a Moonraker notify_status_update notification through the backend
    void feed_status_update(const nlohmann::json& params_inner) {
        // Build the full notification format: { "params": [ { ... }, timestamp ] }
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    // Feed AFC global state update
    void feed_afc_state(const nlohmann::json& afc_data) {
        nlohmann::json params;
        params["AFC"] = afc_data;
        feed_status_update(params);
    }

    // Feed AFC_stepper lane update
    void feed_afc_stepper(const std::string& lane_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane_name] = data;
        feed_status_update(params);
    }

    // State accessors for test assertions
    AmsAction get_action() const {
        return system_info_.action;
    }
    std::string get_operation_detail() const {
        return system_info_.operation_detail;
    }
    std::vector<int> get_tool_to_slot_map() const {
        return get_system_info().tool_to_slot_map;
    }

    /// Per-slot backup edges, via the one shared group-to-edge projection.
    std::vector<int> get_endless_spool_edges() const {
        return helix::printer::endless_spool_backup_edges(get_endless_spool_config(),
                                                          get_system_info().total_slots);
    }

    // Get mapped_tool from a slot
    int get_slot_mapped_tool(int slot_index) const {
        const auto* entry = slots_.get(slot_index);
        return entry ? entry->info.mapped_tool : -1;
    }

    // Event tracking
    std::vector<std::pair<std::string, std::string>> emitted_events;

    void install_event_tracker() {
        set_event_callback([this](const std::string& event, const std::string& data) {
            emitted_events.emplace_back(event, data);
        });
    }

    bool has_event(const std::string& event) const {
        for (const auto& [ev, _] : emitted_events) {
            if (ev == event)
                return true;
        }
        return false;
    }

    std::string get_event_data(const std::string& event) const {
        for (const auto& [ev, data] : emitted_events) {
            if (ev == event)
                return data;
        }
        return "";
    }

    // Access to extended parsing state (reads from registry)
    helix::printer::SlotSensors get_lane_sensors(int index) const {
        const auto* entry = slots_.get(index);
        if (entry) {
            return entry->sensors;
        }
        return {};
    }
    bool get_hub_sensor() const {
        // Returns true if any hub sensor is triggered (backward compat)
        for (const auto& [name, triggered] : hub_sensors_) {
            if (triggered)
                return true;
        }
        return false;
    }

    bool get_hub_sensor(const std::string& hub_name) const {
        auto it = hub_sensors_.find(hub_name);
        return it != hub_sensors_.end() && it->second;
    }

    const std::unordered_map<std::string, bool>& get_hub_sensors() const {
        return hub_sensors_;
    }
    bool get_tool_start_sensor() const {
        return tool_start_sensor_;
    }
    bool get_tool_end_sensor() const {
        return tool_end_sensor_;
    }
    bool get_quiet_mode() const {
        return afc_quiet_mode_;
    }
    // AFC.maps — the T-commands AFC registered with Klipper (v1.2.0+)
    const std::vector<std::string>& get_afc_tool_cmds() const {
        return afc_tool_cmds_;
    }
    // Per-lane AFC_stepper.remember_spool. nullopt = never reported for this lane.
    std::optional<bool> get_lane_remember_spool(const std::string& lane_name) const {
        auto it = lane_remember_spool_.find(lane_name);
        if (it == lane_remember_spool_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    bool get_led_state() const {
        return afc_led_state_;
    }
    float get_bowden_length() const {
        return bowden_length_;
    }

    // Feed AFC_hub update
    void feed_afc_hub(const std::string& hub_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_hub " + hub_name] = data;
        feed_status_update(params);
    }

    // Feed AFC_extruder update
    void feed_afc_extruder(const std::string& ext_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_extruder " + ext_name] = data;
        feed_status_update(params);
    }

    // Feed AFC_buffer update
    void feed_afc_buffer(const std::string& buf_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_buffer " + buf_name] = data;
        feed_status_update(params);
    }

    // Phase 2 mixed topology accessors
    const std::vector<AfcUnitInfo>& get_unit_infos() const {
        return unit_infos_;
    }

    const std::vector<std::string>& get_extruder_names() const {
        return extruder_names_;
    }

    AmsSystemInfo& get_system_info_mutable() {
        return system_info_;
    }

    // Stand in for the configfile.settings response, which the test backend has
    // no client to fetch. Keys are the LOWERCASED AFC_extruder section names,
    // exactly as query_afc_configfile_topology() stores them.
    void seed_extruder_klipper_names(
        const std::unordered_map<std::string, std::string>& section_to_klipper) {
        std::lock_guard<std::mutex> lock(mutex_);
        extruder_klipper_names_ = section_to_klipper;
        configfile_answered_ = true; // stands in for the query having landed
        extruder_tool_index_warned_.clear();
    }

    int tool_index_for(const std::string& ext_name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tool_index_for_extruder_unlocked(ext_name);
    }

    /// Stand in for an `[AFC_Toolchanger …]` section in configfile.settings.
    void seed_configfile_toolchanger(bool present) {
        std::lock_guard<std::mutex> lock(mutex_);
        configfile_has_toolchanger_ = present;
    }

    bool test_has_toolchanger() const {
        return has_toolchanger();
    }

    /// N extruders and NO toolchanger — IDEX, or standalone toolheads each
    /// driven by their own [AFC_extruder] section. This is a real machine, and
    /// `AFC_SELECT_TOOL` does not exist on it.
    void setup_multi_extruder_no_toolchanger(int num_extruders) {
        num_extruders_ = num_extruders;
        extruders_.clear();
        extruder_names_.clear();
        for (int i = 0; i < num_extruders; ++i) {
            std::string name = (i == 0) ? "extruder" : "extruder" + std::to_string(i);
            AfcExtruderInfo ext;
            ext.name = name;
            extruders_.push_back(std::move(ext));
            extruder_names_.push_back(std::move(name));
        }
    }

    /// A real toolchanger: N extruders AND the `[AFC_Toolchanger …]` section
    /// that registers AFC_SELECT_TOOL. Extruder COUNT alone does not make a
    /// toolchanger — see setup_multi_extruder_no_toolchanger() — so anything
    /// asserting on AFC_SELECT_TOOL has to establish the toolchanger itself.
    ///
    /// Uses the configfile signal rather than pushing a unit: `unit_infos_` is
    /// rebuilt wholesale by any later frame carrying `units`, and several
    /// topology tests below assert on its exact contents.
    void setup_toolchanger(int num_extruders) {
        setup_multi_extruder_no_toolchanger(num_extruders);
        configfile_has_toolchanger_ = true;
    }
};

// ============================================================================
// AFC version is informational only
// ============================================================================
//
// version_at_least() is gone. AFC removed the code that writes the afc-install
// Moonraker namespace in its commit 7d20db7 (#451, 2025-06-16), so the version
// string is either absent or frozen at whatever it was before that date. A live
// BoxTurtle reported "1.0.0" on 2026-07-26 while its payload proved 1.0.32-era.
// Capabilities are feature-detected from the data; these tests pin that no
// behavior keys off the reported version.

TEST_CASE("AFC persistence is independent of the reported version", "[ams][afc][version]") {
    for (const char* version : {"1.0.0", "1.0.19", "unknown", "", "9.9.9"}) {
        AmsBackendAfcTestHelper helper;
        CAPTURE(version);
        helper.set_afc_version(version);
        helper.initialize_test_lanes_with_slots(4);

        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        info.remaining_weight_g = 850;
        info.spoolman_id = 42;
        helper.set_slot_info(0, info);

        // Every version, including the ones that used to be gated out, must
        // persist through G-code. Skipping this was issue #644.
        REQUIRE_FALSE(helper.captured_gcodes.empty());
        bool saw_spool_id = false;
        for (const auto& g : helper.captured_gcodes) {
            if (g.find("SET_SPOOL_ID") != std::string::npos)
                saw_spool_id = true;
        }
        REQUIRE(saw_spool_id);
    }
}

// ============================================================================
// compute_filament_segment_unlocked() - Sensor-to-Segment Mapping Tests
// ============================================================================

TEST_CASE("AFC segment: no sensors triggered returns NONE", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);

    // No sensors triggered, no filament loaded, no current slot
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::NONE);
}

TEST_CASE("AFC segment: filament loaded flag returns SPOOL when no sensors",
          "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_filament_loaded(true);

    // Filament is "loaded" but no sensors triggered - implies at spool
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::SPOOL);
}

TEST_CASE("AFC segment: current slot set returns SPOOL when no sensors", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_slot(0);

    // A slot is selected but no sensors - filament at spool area
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::SPOOL);
}

TEST_CASE("AFC segment: prep sensor triggered returns PREP", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_prep_sensor(0, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::PREP);
}

TEST_CASE("AFC segment: prep and load sensors return LANE", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_prep_sensor(0, true);
    helper.set_lane_load_sensor(0, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

TEST_CASE("AFC segment: hub_sensor returns OUTPUT", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_hub_sensor(true);

    // Hub sensor indicates filament past the hub merger, heading to toolhead
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::OUTPUT);
}

TEST_CASE("AFC segment: tool_start_sensor returns TOOLHEAD", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_hub_sensor(true);
    helper.set_tool_start_sensor(true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::TOOLHEAD);
}

TEST_CASE("AFC segment: tool_end_sensor returns NOZZLE", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_hub_sensor(true);
    helper.set_tool_start_sensor(true);
    helper.set_tool_end_sensor(true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::NOZZLE);
}

TEST_CASE("AFC segment: tool_end_sensor alone returns NOZZLE (overrides all)",
          "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // Only end sensor, no others - still returns NOZZLE as it's furthest
    helper.set_tool_end_sensor(true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::NOZZLE);
}

TEST_CASE("AFC segment: fallback scans all lanes for prep sensor", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // No current lane set, but lane3 has prep sensor triggered
    helper.set_lane_prep_sensor(2, true); // lane3 is index 2

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::PREP);
}

TEST_CASE("AFC segment: fallback scans all lanes for load sensor", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // No current lane set, but lane2 has load sensor triggered
    helper.set_lane_load_sensor(1, true); // lane2 is index 1

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

TEST_CASE("AFC segment: fallback scans all lanes for load sensor on the last lane",
          "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // No current lane set, but lane4 (the last lane scanned) has load triggered
    helper.set_lane_load_sensor(3, true); // lane4 is index 3

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

TEST_CASE("AFC segment: hub sensor takes priority over lane sensors", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_current_lane("lane1");
    helper.set_lane_prep_sensor(0, true);
    helper.set_lane_load_sensor(0, true);
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_hub_sensor(true);

    // Hub sensor should return OUTPUT even with all lane sensors triggered
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::OUTPUT);
}

TEST_CASE("AFC segment: toolhead sensors take priority over hub", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.set_hub_sensor(true);
    helper.set_tool_start_sensor(true);

    // tool_start_sensor should return TOOLHEAD even with hub sensor triggered
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::TOOLHEAD);
}

TEST_CASE("AFC segment ignores the latched loaded_to_hub field", "[ams][afc][segment]") {
    // loaded_to_hub reads true on every prepped lane forever, so deriving HUB from
    // it reported filament at the hub for lanes holding nothing in the bowden.
    // On AFC there is no observable "at hub" state distinct from OUTPUT: the hub
    // sensor is the transition, and below it the lane load sensor is authoritative.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_hub_sensor("Turtle_1", false);
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_lane_load_sensor(0, true);
    helper.set_lane_prep_sensor(0, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

TEST_CASE("AFC segment reports OUTPUT when the hub sensor is triggered", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_lane_loaded_to_hub(0, false);
    helper.set_hub_sensor("Turtle_1", true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::OUTPUT);
}

TEST_CASE("AFC get_slot_filament_segment ignores the latched loaded_to_hub field "
          "for a non-active slot",
          "[ams][afc][segment]") {
    // Same defect as compute_filament_segment_unlocked(), in the per-slot accessor
    // that drives per-lane path rendering on the AMS panel: loaded_to_hub is
    // latched at prep and never updated, so a non-active lane that was ever
    // prepped reads "at hub" forever, regardless of where its filament actually
    // is. There is no per-slot hub sensor for a non-active slot, so load/prep
    // remain the furthest this can honestly report.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_hub_sensor("Turtle_1", false);
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_lane_load_sensor(0, true);

    // Slot 0 is not the active slot (current_slot defaults to -1), so this
    // exercises the non-active-slot branch of get_slot_filament_segment().
    REQUIRE(helper.get_slot_filament_segment(0) == PathSegment::LANE);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("AFC segment: no lanes initialized returns NONE", "[ams][afc][segment][edge]") {
    AmsBackendAfcTestHelper helper;
    // Don't call initialize_test_lanes - no lanes configured

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::NONE);
}

TEST_CASE("AFC segment: current lane not in map uses fallback scan", "[ams][afc][segment][edge]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // Set a lane name that doesn't exist in the map
    helper.set_current_lane("nonexistent");
    helper.set_lane_prep_sensor(0, true);

    // Should fall back to scanning all lanes
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::PREP);
}

TEST_CASE("AFC segment: multiple lanes with sensors uses first match in order",
          "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // Multiple lanes have sensors triggered, but no current lane set.
    // The algorithm iterates through lanes in index order and returns on the
    // first sensor found, not the furthest segment across all lanes.
    helper.set_lane_prep_sensor(0, true);
    helper.set_lane_load_sensor(1, true);

    // Lane 0: load=false, prep=true -> returns PREP immediately, even though
    // lane 1 holds LANE, a segment further along the path.
    REQUIRE(helper.test_compute_filament_segment() == PathSegment::PREP);
}

TEST_CASE("AFC segment: fallback prioritizes load over prep per-lane", "[ams][afc][segment]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    // Lane 0 has both load and prep triggered. load is checked before prep for
    // each individual lane, so it must win even though prep is also true.
    helper.set_lane_load_sensor(0, true);
    helper.set_lane_prep_sensor(0, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

// ============================================================================
// set_discovered_lanes() - Lane Discovery from PrinterCapabilities Tests
// ============================================================================

TEST_CASE("AFC set_discovered_lanes: sets lane names correctly", "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};

    helper.set_discovered_lanes(lanes, hubs);

    // After setting lanes and initializing, they should be accessible via registry
    helper.initialize_slots_from_discovery();
    REQUIRE(helper.get_slot_count() == 4);
    REQUIRE(helper.get_slot_name(0) == "lane1");
    REQUIRE(helper.get_slot_name(3) == "lane4");
}

TEST_CASE("AFC set_discovered_lanes: sets hub names correctly", "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    std::vector<std::string> lanes = {"lane1", "lane2"};
    std::vector<std::string> hubs = {"Turtle_1", "Turtle_2"};

    helper.set_discovered_lanes(lanes, hubs);

    REQUIRE(helper.get_hub_names().size() == 2);
    REQUIRE(helper.get_hub_names()[0] == "Turtle_1");
}

TEST_CASE("AFC set_discovered_lanes: empty lanes doesn't overwrite existing",
          "[ams][afc][discovery]") {
    AmsBackendAfcTestHelper helper;

    // First set some lanes
    std::vector<std::string> lanes = {"lane1", "lane2"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    // Then call with empty lanes - should not overwrite
    std::vector<std::string> empty_lanes;
    std::vector<std::string> new_hubs = {"NewHub"};
    helper.set_discovered_lanes(empty_lanes, new_hubs);

    // Lanes should remain unchanged (check via discovery init)
    helper.initialize_slots_from_discovery();
    REQUIRE(helper.get_slot_count() == 2);
    // But hubs should be updated
    REQUIRE(helper.get_hub_names().size() == 1);
    REQUIRE(helper.get_hub_names()[0] == "NewHub");
}

TEST_CASE("AFC segment: works with discovered lanes", "[ams][afc][discovery][segment]") {
    AmsBackendAfcTestHelper helper;

    // Set lanes via discovery (like PrinterCapabilities would)
    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    // Initialize the lanes (like start() would do)
    helper.initialize_slots_from_discovery();

    // Now test that sensors work correctly
    helper.set_current_lane("lane2");
    helper.set_lane_prep_sensor(1, true);
    helper.set_lane_load_sensor(1, true);

    REQUIRE(helper.test_compute_filament_segment() == PathSegment::LANE);
}

// ============================================================================
// set_slot_info() Persistence Tests - AFC >= 1.0.20
// ============================================================================
//
// These tests verify that set_slot_info() sends the appropriate G-code commands
// to persist filament properties when AFC version >= 1.0.20.
//
// Commands expected:
// - SET_COLOR LANE=<name> COLOR=<RRGGBB>
// - SET_MATERIAL LANE=<name> MATERIAL=<type>
// - SET_WEIGHT LANE=<name> WEIGHT=<grams>
// - SET_SPOOL_ID LANE=<name> SPOOL_ID=<id>
//
// NOTE: These tests are designed to FAIL initially. The set_slot_info() method
// currently only updates local state and does NOT send G-code commands.
// Implementation must be added to make these tests pass.
//
// Testing approach: Since MoonrakerAPI::execute_gcode() is not virtual,
// the test helper captures G-code via the protected execute_gcode() method
// that AmsBackendAfc exposes. The implementation must call execute_gcode()
// for these tests to pass.
// ============================================================================

// Inverted deliberately. This used to assert that an "old" version suppressed
// persistence; the version is not a usable signal (AFC stopped writing it), and
// suppressing G-code on an unrecognized version was issue #644 — spool
// assignment silently bypassed AFC. persist=false remains the only way to skip.
TEST_CASE("AFC persistence: persist=false is the only thing that skips G-code",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.19"); // Formerly below the 1.0.20 gate
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000; // Red
    info.material = "PLA";
    info.remaining_weight_g = 850;
    info.spoolman_id = 42;

    helper.set_slot_info(0, info, /*persist=*/false);
    REQUIRE(helper.captured_gcodes.empty());

    helper.set_slot_info(0, info);
    REQUIRE_FALSE(helper.captured_gcodes.empty());
    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=FF0000"));
    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=PLA"));
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=850"));
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
}

TEST_CASE("AFC persistence: SET_COLOR command format", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000; // Red

    helper.set_slot_info(0, info);

    // Should send: SET_COLOR LANE=lane1 COLOR=FF0000
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=FF0000"));
}

TEST_CASE("AFC persistence: SET_COLOR uppercase hex no prefix", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0x00FF00; // Green

    helper.set_slot_info(1, info);

    // Should send: SET_COLOR LANE=lane2 COLOR=00FF00 (uppercase, no #)
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane2 COLOR=00FF00"));
}

TEST_CASE("AFC persistence: SET_MATERIAL command format", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = "PLA";

    helper.set_slot_info(1, info);

    // Should send: SET_MATERIAL LANE=lane2 MATERIAL=PLA
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane2 MATERIAL=PLA"));
}

TEST_CASE("AFC persistence: SET_WEIGHT command format", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.remaining_weight_g = 850.5f; // Should be sent as integer

    helper.set_slot_info(0, info);

    // Should send: SET_WEIGHT LANE=lane1 WEIGHT=850 (no decimals)
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=850"));
}

TEST_CASE("AFC persistence: SET_SPOOL_ID command format", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.spoolman_id = 42;

    helper.set_slot_info(0, info);

    // Should send: SET_SPOOL_ID LANE=lane1 SPOOL_ID=42
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
}

TEST_CASE("AFC persistence: SET_SPOOL_ID clear with empty string", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Pre-set existing spoolman_id on slot
    SlotInfo* existing_slot = helper.get_mutable_slot(0);
    REQUIRE(existing_slot != nullptr);
    existing_slot->spoolman_id = 123;

    // Now clear it by setting spoolman_id = 0
    SlotInfo new_info;
    new_info.spoolman_id = 0;

    helper.set_slot_info(0, new_info);

    // Should send: SET_SPOOL_ID LANE=lane1 SPOOL_ID= (empty to clear)
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID="));
}

// Persistence must NOT be gated on the afc-install database version.
//
// Nothing in the AFC source writes the Moonraker `afc-install` namespace, so it
// is never updated on upgrade. A BoxTurtle running v1.1.0-4-g2921371 still
// reports {"version": "1.0.0"} there. The old gate was
// version_at_least("1.0.20"), which on that reading silently skipped EVERY
// SET_COLOR / SET_MATERIAL / SET_WEIGHT / SET_SPOOL_ID and logged only an
// "upgrade for persistence" info line. Saves survived purely because the DB
// query often lost the race and fell into the "unknown" escape hatch (#644).
// A stale-but-successful read is the dangerous case: total silent data loss.
TEST_CASE("AFC persistence: a stale afc-install version does not suppress gcode",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    // Exactly what the .112 BoxTurtle's afc-install namespace reports today,
    // while actually running v1.1.0-4.
    helper.set_afc_version("1.0.0");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = "PLA";
    info.color_rgb = 0xE53935;
    info.remaining_weight_g = 500.0f;

    helper.set_slot_info(0, info);

    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=PLA"));
    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=E53935"));
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=500"));
}

// On AFC, SET_SPOOL_ID with an empty value is not a narrow unlink: AFC_spool.py's
// set_spoolID() routes an empty/None id into clear_values(), which wipes material,
// color, weight and both temps (and calls clear_lane_data()). Emitting it LAST
// therefore destroys the SET_COLOR / SET_MATERIAL / SET_WEIGHT sent earlier in the
// same save. Observed on the .112 BoxTurtle: a save emitted COLOR/MATERIAL/WEIGHT
// then SPOOL_ID=, and the editor reopened 3s later with material empty.
TEST_CASE("AFC persistence: spool-link clear is emitted before the data writes",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Lane starts linked to a Spoolman spool.
    SlotInfo* existing_slot = helper.get_mutable_slot(0);
    REQUIRE(existing_slot != nullptr);
    existing_slot->spoolman_id = 86;

    // Unlink and set fresh identity in the SAME save — the exact shape of the
    // real-world failure.
    SlotInfo info;
    info.spoolman_id = 0;
    info.material = "PLA";
    info.color_rgb = 0xE53935;
    info.remaining_weight_g = 500.0f;

    helper.set_slot_info(0, info);

    const int clear_idx = helper.gcode_index_of("SET_SPOOL_ID LANE=lane1 SPOOL_ID=");
    const int color_idx = helper.gcode_index_of("SET_COLOR LANE=lane1");
    const int material_idx = helper.gcode_index_of("SET_MATERIAL LANE=lane1");
    const int weight_idx = helper.gcode_index_of("SET_WEIGHT LANE=lane1");

    REQUIRE(clear_idx >= 0);
    REQUIRE(color_idx >= 0);
    REQUIRE(material_idx >= 0);
    REQUIRE(weight_idx >= 0);

    // The clear must precede every data write, or AFC wipes what we just set.
    REQUIRE(clear_idx < color_idx);
    REQUIRE(clear_idx < material_idx);
    REQUIRE(clear_idx < weight_idx);
}

// The link branch is destructive in the opposite direction: AFC_spool.py's
// set_spoolID() with a valid id fetches the spool from Spoolman and overwrites
// material, color, weight, both temps, density, diameter and empty_spool_weight
// from that record. Emitting it after our own writes replaces them with
// Spoolman's values, so it must precede them too.
TEST_CASE("AFC persistence: spool-link set is emitted before the data writes",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.spoolman_id = 86;
    info.material = "PLA";
    info.color_rgb = 0xE53935;
    info.remaining_weight_g = 500.0f;

    helper.set_slot_info(0, info);

    const int link_idx = helper.gcode_index_of("SET_SPOOL_ID LANE=lane1 SPOOL_ID=86");
    const int color_idx = helper.gcode_index_of("SET_COLOR LANE=lane1");
    const int material_idx = helper.gcode_index_of("SET_MATERIAL LANE=lane1");
    const int weight_idx = helper.gcode_index_of("SET_WEIGHT LANE=lane1");

    REQUIRE(link_idx >= 0);
    REQUIRE(color_idx >= 0);
    REQUIRE(material_idx >= 0);
    REQUIRE(weight_idx >= 0);

    REQUIRE(link_idx < color_idx);
    REQUIRE(link_idx < material_idx);
    REQUIRE(link_idx < weight_idx);
}

TEST_CASE("AFC persistence: SET_MAP fires when mapped_tool changes via set_slot_info",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Helper seeds slot 0 → T0 by default. Remap it to T2 through the slot edit path.
    SlotInfo info;
    info.mapped_tool = 2;

    helper.set_slot_info(0, info);

    REQUIRE(helper.has_gcode("SET_MAP LANE=lane1 MAP=T2"));
}

TEST_CASE("AFC persistence: SET_MAP not fired when mapped_tool unchanged",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Default mapping for slot 0 is T0. Setting same value should not emit SET_MAP.
    SlotInfo info;
    info.mapped_tool = 0;
    info.material = "PLA";

    helper.set_slot_info(0, info);

    for (const auto& gcode : helper.captured_gcodes) {
        REQUIRE(gcode.rfind("SET_MAP ", 0) != 0);
    }
}

TEST_CASE("AFC persistence: SET_MAP not fired when caller leaves mapped_tool default (-1)",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Spoolman polling and consumption_sink build a SlotInfo from the existing slot,
    // but a misuse (default-constructed SlotInfo) must NOT clobber the live mapping.
    // The guard is `info.mapped_tool >= 0` — so mapped_tool=-1 is a no-op.
    SlotInfo info; // mapped_tool defaults to -1
    info.material = "PLA";

    helper.set_slot_info(2, info);

    for (const auto& gcode : helper.captured_gcodes) {
        REQUIRE(gcode.rfind("SET_MAP ", 0) != 0);
    }
    // Live registry mapping for slot 2 must still be T2 (the seeded default).
    REQUIRE(helper.get_slot_mapped_tool(2) == 2);
}

TEST_CASE("AFC persistence: mapped_tool change updates registry reverse map",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Remap slot 0 from T0 → T2 via the slot edit path. Registry should reflect it
    // so future tool-changes (T2 → which slot?) resolve correctly.
    SlotInfo info;
    info.mapped_tool = 2;
    helper.set_slot_info(0, info);

    REQUIRE(helper.get_slot_mapped_tool(0) == 2);
}

TEST_CASE("AFC persistence: sends gcode even with unknown version", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("unknown");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.spoolman_id = 42;
    info.material = "PLA";
    info.color_rgb = 0xFF0000;
    info.remaining_weight_g = 800.0f;

    helper.set_slot_info(0, info);

    // Unknown version should still attempt gcode (SET_SPOOL_ID existed before 1.0.20)
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
}

TEST_CASE("AFC persistence: sends gcode with empty version", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.spoolman_id = 42;

    helper.set_slot_info(0, info);

    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=42"));
}

TEST_CASE("AFC persistence: skips SET_COLOR for default grey", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0x808080; // Default grey - should NOT send

    helper.set_slot_info(0, info);

    // Should NOT send SET_COLOR for grey default
    // PASSES: no G-code sent at all currently
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_COLOR"));
}

TEST_CASE("AFC persistence: skips SET_COLOR for zero", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0; // Zero color - should NOT send

    helper.set_slot_info(0, info);

    // Should NOT send SET_COLOR for zero
    // PASSES: no G-code sent at all currently
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_COLOR"));
}

TEST_CASE("AFC persistence: SET_MATERIAL carries material names with punctuation",
          "[ams][afc][persistence]") {
    // Bundle XGVDYEB5: "Skipping SET_MATERIAL - unsafe characters in: PLA+".
    // The gate was is_safe_gcode_param() -> is_safe_identifier(), charset
    // [A-Za-z0-9_ ] — so `PLA+`, which HelixScreen offers from its own
    // filament_database.h, could never be persisted. The other four G-codes went
    // out and set_slot_info() still returned success: a silent partial write.
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = "PLA+";

    AmsError err = helper.set_slot_info(0, info);

    REQUIRE(helper.gcode_index_of("SET_MATERIAL LANE=lane1") >= 0);
    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=PLA+"));
    REQUIRE(err.success());
}

TEST_CASE("AFC persistence: SET_MATERIAL quotes multi-word material names",
          "[ams][afc][persistence]") {
    // `Silk PLA` and `Matte PLA` ship in filament_database.h too. They passed the
    // old identifier check (which allows space) and were sent raw — Klipper
    // tokenizes extended-command args on whitespace, so `MATERIAL=Silk PLA`
    // arrives as two tokens and the command is rejected as malformed. Quote it.
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = "Silk PLA";

    AmsError err = helper.set_slot_info(0, info);

    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=\"Silk PLA\""));
    REQUIRE(err.success());
}

TEST_CASE("AFC persistence: an unsendable material is reported, not swallowed",
          "[ams][afc][persistence]") {
    // A material that genuinely cannot be expressed as a G-code parameter still
    // must not be dropped in silence: the rest of the save goes out, and the
    // caller is told which part did not.
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = "PLA;G28";
    info.remaining_weight_g = 750.0f;

    AmsError err = helper.set_slot_info(0, info);

    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_MATERIAL"));
    // The rest of the write is unaffected.
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=750"));
    REQUIRE_FALSE(err.success());
    REQUIRE(err.result == AmsResult::COMMAND_FAILED);
}

TEST_CASE("AFC persistence: skips SET_MATERIAL for empty string", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.material = ""; // Empty - should NOT send

    helper.set_slot_info(0, info);

    // Should NOT send SET_MATERIAL for empty
    // PASSES: no G-code sent at all currently
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_MATERIAL"));
}

TEST_CASE("AFC persistence: skips SET_WEIGHT for zero or negative", "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SECTION("zero weight") {
        SlotInfo info;
        info.remaining_weight_g = 0;
        helper.set_slot_info(0, info);
        // PASSES: no G-code sent at all currently
        REQUIRE_FALSE(helper.has_gcode_starting_with("SET_WEIGHT"));
    }

    SECTION("negative weight (unknown)") {
        helper.clear_captured_gcodes();
        SlotInfo info;
        info.remaining_weight_g = -1;
        helper.set_slot_info(0, info);
        // PASSES: no G-code sent at all currently
        REQUIRE_FALSE(helper.has_gcode_starting_with("SET_WEIGHT"));
    }
}

TEST_CASE("AFC persistence: skips SET_SPOOL_ID when both old and new are zero",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    // Slot starts with spoolman_id = 0 (default)
    SlotInfo info;
    info.spoolman_id = 0;

    helper.set_slot_info(0, info);

    // Should NOT send SET_SPOOL_ID when both old and new are 0
    // PASSES: no G-code sent at all currently
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_SPOOL_ID"));
}

TEST_CASE("AFC persistence: sends multiple commands for full slot info",
          "[ams][afc][persistence]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0x0000FF; // Blue
    info.material = "PETG";
    info.remaining_weight_g = 750;
    info.spoolman_id = 99;

    helper.set_slot_info(0, info);

    // Should send all four commands
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=0000FF"));
    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=PETG"));
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=750"));
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=99"));
}

// ============================================================================
// set_slot_info() persist=false Tests
// ============================================================================
//
// When persist=false, set_slot_info() should update in-memory slot state but
// NOT send any G-code commands to firmware. This is critical for preventing an
// infinite feedback loop when Spoolman weight polling updates slot data:
//
//   set_slot_info(persist=true) → G-code to firmware → firmware status_update
//   via WebSocket → sync_from_backend → refresh_spoolman_weights →
//   set_slot_info again → ∞
//
// With persist=false, the cycle breaks because no G-code is sent, so firmware
// doesn't emit a status_update, and the loop terminates.
// ============================================================================

TEST_CASE("AFC persist=false: updates local state without G-code",
          "[ams][afc][persistence][persist_flag]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";
    info.remaining_weight_g = 850;
    info.spoolman_id = 42;

    // persist=false should NOT send any G-code
    helper.set_slot_info(0, info, /*persist=*/false);

    REQUIRE(helper.captured_gcodes.empty());

    // But local state SHOULD be updated
    SlotInfo stored = helper.get_slot_info(0);
    REQUIRE(stored.color_rgb == 0xFF0000);
    REQUIRE(stored.material == "PLA");
    REQUIRE(stored.remaining_weight_g == Catch::Approx(850.0f));
    REQUIRE(stored.spoolman_id == 42);
}

TEST_CASE("AFC persist=true: sends G-code (default behavior unchanged)",
          "[ams][afc][persistence][persist_flag]") {
    AmsBackendAfcTestHelper helper;

    helper.set_afc_version("1.0.20");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0x00FF00;
    info.material = "ABS";
    info.remaining_weight_g = 500;
    info.spoolman_id = 7;

    // Default persist=true should send G-code
    helper.set_slot_info(0, info);

    REQUIRE(helper.has_gcode("SET_COLOR LANE=lane1 COLOR=00FF00"));
    REQUIRE(helper.has_gcode("SET_MATERIAL LANE=lane1 MATERIAL=ABS"));
    REQUIRE(helper.has_gcode("SET_WEIGHT LANE=lane1 WEIGHT=500"));
    REQUIRE(helper.has_gcode("SET_SPOOL_ID LANE=lane1 SPOOL_ID=7"));
}

TEST_CASE("AFC persist=false: version warning not emitted",
          "[ams][afc][persistence][persist_flag]") {
    AmsBackendAfcTestHelper helper;

    // Old version + persist=false should NOT log the upgrade warning
    helper.set_afc_version("1.0.19");
    helper.initialize_test_lanes_with_slots(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";

    // Should succeed without errors and without persistence
    auto result = helper.set_slot_info(0, info, /*persist=*/false);
    REQUIRE(result.success());
    REQUIRE(helper.captured_gcodes.empty());
}

// ============================================================================
// reset_tool_mappings() Tests
// ============================================================================

TEST_CASE("AFC reset_tool_mappings sends RESET_AFC_MAPPING RUNOUT=no",
          "[ams][afc][tool_mapping][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("RESET_AFC_MAPPING RUNOUT=no"));
}

TEST_CASE("AFC reset_tool_mappings sends single command regardless of lane count",
          "[ams][afc][tool_mapping][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(8);

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    // Should send exactly one command, not one per lane
    REQUIRE(helper.captured_gcodes.size() == 1);
    REQUIRE(helper.has_gcode("RESET_AFC_MAPPING RUNOUT=no"));
}

// AFC dev/1.3 (Klipper-Add-On #832) deregistered RESET_AFC_MAPPING in favour of
// AFC_RESET_MAPPING. The new firmware is recognisable by the multiple_tool_mapping
// flag its get_status publishes alongside the rename — the flag's VALUE is the
// opt-in to virtual tools and defaults false, so key presence is the version
// signal, never the value.
TEST_CASE("AFC reset_tool_mappings uses AFC_RESET_MAPPING once multiple_tool_mapping is reported",
          "[ams][afc][tool_mapping][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Flag present but FALSE — virtual tools disabled, renamed firmware.
    helper.feed_afc_state({{"multiple_tool_mapping", false}});

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_RESET_MAPPING RUNOUT=no"));
    REQUIRE(helper.captured_gcodes.size() == 1);
}

TEST_CASE("AFC reset_tool_mappings keeps the old name until the firmware reports the flag",
          "[ams][afc][tool_mapping][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // A status frame from firmware predating the rename carries no flag; the
    // old macro name must survive — the new name is an unknown command there.
    helper.feed_afc_state({{"led_state", true}});

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("RESET_AFC_MAPPING RUNOUT=no"));
    REQUIRE(helper.captured_gcodes.size() == 1);
}

// ============================================================================
// reset_endless_spool() Tests
// ============================================================================

TEST_CASE("AFC reset_endless_spool clears all slots", "[ams][afc][endless_spool][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // Set some backups first
    helper.set_endless_spool_config(0, 1);
    helper.set_endless_spool_config(2, 3);

    auto result = helper.reset_endless_spool();

    REQUIRE(result.success());
    // Should have sent 4 SET_RUNOUT commands (one per slot)
    REQUIRE(helper.captured_gcodes.size() == 4);

    // Each should be setting RUNOUT=NONE to disable
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane1 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane2 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane3 RUNOUT=NONE"));
    REQUIRE(helper.has_gcode("SET_RUNOUT LANE=lane4 RUNOUT=NONE"));
}

TEST_CASE("AFC reset_endless_spool with no lanes yet refuses instead of silently succeeding",
          "[ams][afc][endless_spool][reset]") {
    AmsBackendAfcTestHelper helper;
    // Don't initialize any lanes or configs — AFC always advertises the mapping as
    // editable, so without a slot-count guard the loop is skipped and the caller is
    // told the wipe succeeded. The UI confirms a destructive warning before calling
    // this, so a silent no-op is worse than a refusal.

    auto result = helper.reset_endless_spool();

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::NOT_SUPPORTED);
    REQUIRE_FALSE(result.user_msg.empty());
    REQUIRE(helper.captured_gcodes.empty());
}

TEST_CASE("AFC reset_endless_spool continues on partial failure",
          "[ams][afc][endless_spool][reset]") {
    // This test verifies that if one slot fails, we still attempt the remaining slots
    // The implementation should return the first error but continue processing
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    auto result = helper.reset_endless_spool();

    // Should still have attempted all 4 slots even if one hypothetically failed
    REQUIRE(helper.captured_gcodes.size() == 4);
}

// ============================================================================
// Phase 1: Bug Fixes & Critical Data Sync Tests
// ============================================================================
//
// These tests verify parsing of fields that the real AFC device exposes
// (captured from 192.168.1.112). Tests use fixture data to validate that
// state updates flow through correctly to internal state.
// ============================================================================

TEST_CASE("AFC action comes from current_state; a stray AFC.status is ignored",
          "[ams][afc][state][deadfields]") {
    // AFC.get_status() publishes current_state and no "status" key at all
    // (AFC.py v1.2.0:2531-2564; same shape at v1.1.0). "status" exists on
    // AFC_lane and AFC_extruder, which have their own parsers — reading it off
    // the AFC object was a second, contradictory action writer that could never
    // fire on real firmware.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    SECTION("current_state drives the action") {
        helper.feed_afc_state({{"current_state", "Loading"}});
        REQUIRE(helper.get_action() == AmsAction::LOADING);
    }

    SECTION("a status key never overrides current_state") {
        helper.feed_afc_state({{"current_state", "Idle"}, {"status", "Loading"}});
        REQUIRE(helper.get_action() == AmsAction::IDLE);
    }

    SECTION("a status key on its own moves nothing") {
        helper.feed_afc_state({{"current_state", "Idle"}});
        REQUIRE(helper.get_action() == AmsAction::IDLE);

        helper.feed_afc_state({{"status", "Loading"}});
        REQUIRE(helper.get_action() == AmsAction::IDLE);
    }
}

TEST_CASE("AFC tool mapping from stepper map field", "[ams][afc][tool_mapping][phase1]") {
    // Real device: AFC_stepper lane1 has "map": "T0", lane2 has "map": "T1", etc.
    // We never parse this field today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed stepper data with map field
    helper.feed_afc_stepper("lane1", {{"map", "T0"}, {"prep", true}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"}, {"prep", true}});
    helper.feed_afc_stepper("lane3", {{"map", "T2"}, {"prep", false}});
    helper.feed_afc_stepper("lane4", {{"map", "T3"}, {"prep", false}});

    // tool_to_slot_map should reflect the mapping from stepper "map" fields
    auto mapping = helper.get_tool_mapping();
    REQUIRE(mapping.size() == 4);
    REQUIRE(mapping[0] == 0); // T0 → lane1 (slot 0)
    REQUIRE(mapping[1] == 1); // T1 → lane2 (slot 1)
    REQUIRE(mapping[2] == 2); // T2 → lane3 (slot 2)
    REQUIRE(mapping[3] == 3); // T3 → lane4 (slot 3)
}

TEST_CASE("AFC tool mapping swap updates correctly", "[ams][afc][tool_mapping][phase1]") {
    // When lanes swap tools (e.g., T0 moves from lane1 to lane3), the mapping
    // should update accordingly
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Initial mapping: T0→lane1, T1→lane2, T2→lane3, T3→lane4
    helper.feed_afc_stepper("lane1", {{"map", "T0"}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"}});
    helper.feed_afc_stepper("lane3", {{"map", "T2"}});
    helper.feed_afc_stepper("lane4", {{"map", "T3"}});

    // Now swap: lane1 gets T2, lane3 gets T0
    helper.feed_afc_stepper("lane1", {{"map", "T2"}});
    helper.feed_afc_stepper("lane3", {{"map", "T0"}});

    // After swap, mapping should reflect new tool assignments
    auto mapping = helper.get_tool_mapping();
    REQUIRE(mapping.size() == 4);
    REQUIRE(mapping[0] == 2); // T0 → lane3 (slot 2)
    REQUIRE(mapping[1] == 1); // T1 → lane2 (slot 1)
    REQUIRE(mapping[2] == 0); // T2 → lane1 (slot 0)
    REQUIRE(mapping[3] == 3); // T3 → lane4 (slot 3)

    // Slot mapped_tool should also be updated
    REQUIRE(helper.get_slot_mapped_tool(0) == 2); // lane1 now maps to T2
    REQUIRE(helper.get_slot_mapped_tool(2) == 0); // lane3 now maps to T0
}

TEST_CASE("AFC tool mapping resets when map transitions string to null",
          "[ams][afc][tool_mapping]") {
    // CORE REGRESSION: when a lane is explicitly unmapped, AFC sends "map" as JSON
    // null in the delta. A PRESENT null is authoritative — the lane must drop its
    // stale tool mapping instead of keeping the previous value.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"map", "T2"}, {"prep", true}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);

    // Same lane, map now null → mapping must reset to unmapped (-1)
    helper.feed_afc_stepper("lane1", {{"map", nullptr}, {"prep", true}});
    REQUIRE(helper.get_slot_mapped_tool(0) == -1);
    // Reverse map must no longer point T2 at this slot
    REQUIRE(helper.get_tool_mapping()[2] == -1);
}

TEST_CASE("AFC tool mapping survives an update with no map field", "[ams][afc][tool_mapping]") {
    // parse_afc_stepper receives Moonraker notify_status_update DELTAS: a partial
    // update (e.g. weight-only) that omits "map" means "unchanged", NOT "unmapped".
    // Clearing on absent would wipe a live tool mapping mid-print — the mapping must
    // survive. Only a PRESENT map value is authoritative (see the null test above).
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"map", "T2"}, {"prep", true}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);

    // Partial delta with no "map" key → mapping preserved.
    helper.feed_afc_stepper("lane1", {{"weight", 931.7}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);
}

TEST_CASE("AFC tool mapping valid string still maps", "[ams][afc][tool_mapping]") {
    // Guard against over-correction: the valid-string path is unchanged.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"map", "T0"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 0);

    helper.feed_afc_stepper("lane1", {{"map", "T3"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 3);
}

TEST_CASE("AFC tool mapping single-element list maps like a string", "[ams][afc][tool_mapping]") {
    // AFC virtual tools (#605) change `map` to a list UNCONDITIONALLY — a plain
    // 4-lane BoxTurtle with virtual tools disabled sends ["T0"], not "T0". Treating
    // the list as unsupported would unmap every lane on every AFC install the day
    // that version ships, so the one-tool list must behave exactly like the string.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array({"T0"})}});
    helper.feed_afc_stepper("lane2", {{"map", nlohmann::json::array({"T3"})}});

    REQUIRE(helper.get_slot_mapped_tool(0) == 0);
    REQUIRE(helper.get_slot_mapped_tool(1) == 3);
    // Forward map too — this is what change_tool() resolves through
    REQUIRE(helper.get_tool_mapping()[0] == 0);
    REQUIRE(helper.get_tool_mapping()[3] == 1);
}

TEST_CASE("AFC tool mapping multi-tool list without current_map keeps the lowest tool",
          "[ams][afc][tool_mapping]") {
    // FALLBACK ONLY. When AFC does not tell us which tool is active — pre-#605
    // firmware, or a delta that omits current_map before we have ever seen one —
    // SlotRegistry still holds exactly one tool per lane and we must pick something.
    // The lowest is an arbitrary but stable choice, NOT a claim about AFC's
    // semantics: its order is not sorted, so feed it unsorted to prove we do not
    // just take the first element.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    REQUIRE_NOTHROW(
        helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array({"T14", "T13", "T1"})}}));
    REQUIRE(helper.get_slot_mapped_tool(0) == 1);
    REQUIRE(helper.get_tool_mapping()[1] == 0);

    // Removing the virtual tools leaves the lane on the same tool it already had.
    helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array({"T1"})}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 1);
}

TEST_CASE("AFC current_map picks the active tool over the lowest", "[ams][afc][tool_mapping]") {
    // CORE REGRESSION for the lowest-wins heuristic. AFC #605 added current_map,
    // which names the tool a multi-tool lane is ACTUALLY on. This is the shape
    // upstream published: map is unsorted and current_map is not its minimum, so
    // picking the lowest would put the lane on T10 while AFC drives T11.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper(
        "lane1", {{"map", nlohmann::json::array({"T11", "T10"})}, {"current_map", "T11"}});

    REQUIRE(helper.get_slot_mapped_tool(0) == 11);
    // Forward map too — this is what change_tool() resolves through
    REQUIRE(helper.get_tool_mapping()[11] == 0);
    REQUIRE(helper.get_tool_mapping()[10] == -1);
}

TEST_CASE("AFC current_map alone retargets the lane", "[ams][afc][tool_mapping]") {
    // current_map is the field that moves while map stays put — that is its whole
    // purpose. Moonraker sends DELTAS, so a tool change inside a multi-tool lane
    // arrives as current_map with NO map key. Handling the pick only under
    // `if (data.contains("map"))` would drop it silently.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper(
        "lane1", {{"map", nlohmann::json::array({"T11", "T10"})}, {"current_map", "T11"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 11);

    helper.feed_afc_stepper("lane1", {{"current_map", "T10"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 10);
    REQUIRE(helper.get_tool_mapping()[10] == 0);
    REQUIRE(helper.get_tool_mapping()[11] == -1);
}

TEST_CASE("AFC current_map survives a later map-only delta", "[ams][afc][tool_mapping]") {
    // The mirror of the case above: once AFC has told us the lane is on T11, a
    // subsequent delta carrying only map must NOT fall back to the lowest and yank
    // the lane onto a tool AFC is not driving. Growing the lane's tool list is
    // exactly when this happens — AFC_ADD_MAPPING sends map without current_map.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper(
        "lane1", {{"map", nlohmann::json::array({"T11", "T10"})}, {"current_map", "T11"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 11);

    helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array({"T11", "T10", "T5"})}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 11); // not T5
}

TEST_CASE("AFC current_map dropped from map falls back to the lowest", "[ams][afc][tool_mapping]") {
    // Self-healing: AFC_REMOVE_MAPPING can strip the very tool current_map named.
    // The remembered pick must not outlive its membership in map, or the lane stays
    // pinned to a tool the firmware no longer routes to it.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper(
        "lane1", {{"map", nlohmann::json::array({"T11", "T10"})}, {"current_map", "T11"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 11);

    helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array({"T10"})}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 10);
    REQUIRE(helper.get_tool_mapping()[11] == -1);
}

TEST_CASE("AFC current_map outside map is ignored", "[ams][afc][tool_mapping]") {
    // map is the authority on which tools a lane owns; current_map only SELECTS
    // among them. A current_map naming a tool absent from a present map is drift we
    // do not understand, so fall back rather than route a tool AFC never listed.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper(
        "lane1", {{"map", nlohmann::json::array({"T11", "T10"})}, {"current_map", "T7"}});

    REQUIRE(helper.get_slot_mapped_tool(0) == 10); // lowest of map, not T7
    REQUIRE(helper.get_tool_mapping()[7] == -1);
}

TEST_CASE("AFC empty current_map does not unmap a mapped lane", "[ams][afc][tool_mapping]") {
    // Upstream describes current_map as holding the active tool "when more than one
    // T(n) is mapped to that lane", so a single-tool lane may well send it null or
    // empty. Treating a present-but-empty current_map as authoritative would unmap
    // every ordinary lane — the same tripwire that made the array shape unsafe.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1",
                            {{"map", nlohmann::json::array({"T2"})}, {"current_map", nullptr}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);

    helper.feed_afc_stepper("lane2", {{"map", nlohmann::json::array({"T3"})}, {"current_map", ""}});
    REQUIRE(helper.get_slot_mapped_tool(1) == 3);

    // And alone in a delta it means "no news", not "unmap"
    helper.feed_afc_stepper("lane1", {{"current_map", nullptr}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);
}

TEST_CASE("AFC unmapping a lane forgets its current_map", "[ams][afc][tool_mapping]") {
    // The remembered pick is per-lane state. An authoritative unmap must clear it,
    // or a lane later remapped to an unrelated tool list could resurrect a stale
    // tool that happens to reappear in it.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper(
        "lane1", {{"map", nlohmann::json::array({"T11", "T10"})}, {"current_map", "T11"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 11);

    helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array()}});
    REQUIRE(helper.get_slot_mapped_tool(0) == -1);

    // Remapped with T11 present again, but AFC never re-stated current_map →
    // lowest, not the stale T11.
    helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array({"T11", "T4"})}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 4);
}

TEST_CASE("AFC tool mapping empty list unmaps the lane", "[ams][afc][tool_mapping]") {
    // AFC_REMOVE_MAPPING can strip a lane back to no tools. An empty PRESENT list is
    // authoritative and must unmap, exactly like a present null.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array({"T2"})}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);

    helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::array()}});
    REQUIRE(helper.get_slot_mapped_tool(0) == -1);
    REQUIRE(helper.get_tool_mapping()[2] == -1);
}

TEST_CASE("AFC tool mapping list skips junk entries without losing good ones",
          "[ams][afc][tool_mapping]") {
    // One unparseable element must not discard the lane's real tools. In particular
    // "T14,T13" must NOT parse as 14 — std::stoi stops at the comma and returns a
    // confident wrong answer, which is exactly the silent failure to avoid.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    REQUIRE_NOTHROW(helper.feed_afc_stepper(
        "lane1", {{"map", nlohmann::json::array({"T14,T13", "garbage", "T7", 5, nullptr})}}));
    REQUIRE(helper.get_slot_mapped_tool(0) == 7);

    // A list with nothing salvageable unmaps rather than guessing.
    helper.feed_afc_stepper("lane2", {{"map", nlohmann::json::array({"T1"})}});
    REQUIRE(helper.get_slot_mapped_tool(1) == 1);
    helper.feed_afc_stepper("lane2", {{"map", nlohmann::json::array({"T14,T13", "nope"})}});
    REQUIRE(helper.get_slot_mapped_tool(1) == -1);
}

TEST_CASE("AFC tool mapping object-shaped map does not crash", "[ams][afc][tool_mapping]") {
    // Not a shape AFC is known to send; assert it degrades to unmapped, not a crash.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"map", "T2"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);

    REQUIRE_NOTHROW(
        helper.feed_afc_stepper("lane1", {{"map", nlohmann::json::object({{"T0", "lane1"}})}}));
    REQUIRE(helper.get_slot_mapped_tool(0) == -1);
}

TEST_CASE("AFC tool mapping empty or malformed map resets", "[ams][afc][tool_mapping]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"map", "T2"}});
    REQUIRE(helper.get_slot_mapped_tool(0) == 2);
    helper.feed_afc_stepper("lane1", {{"map", ""}});
    REQUIRE(helper.get_slot_mapped_tool(0) == -1);

    helper.feed_afc_stepper("lane2", {{"map", "T2"}});
    REQUIRE(helper.get_slot_mapped_tool(1) == 2);
    helper.feed_afc_stepper("lane2", {{"map", "garbage"}});
    REQUIRE(helper.get_slot_mapped_tool(1) == -1);
}

TEST_CASE("AFC endless spool from runout_lane field", "[ams][afc][endless_spool][phase1]") {
    // Real device: AFC_stepper lane1 has "runout_lane": "lane2"
    // meaning if lane1 runs out, switch to lane2.
    // We never parse this field today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // Feed stepper data with runout_lane
    helper.feed_afc_stepper("lane1", {{"runout_lane", "lane2"}});

    // runout_lane should update endless spool backup config
    auto edges = helper.get_endless_spool_edges();
    REQUIRE(edges.size() == 4);
    REQUIRE(edges[0] == 1); // lane1's backup is lane2 (slot 1)
}

TEST_CASE("AFC endless spool null runout_lane clears backup", "[ams][afc][endless_spool][phase1]") {
    // When runout_lane is null, the backup should be cleared (-1)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.initialize_endless_spool_configs(4);

    // First set a backup
    helper.set_endless_spool_config(0, 1); // lane1 backup = lane2

    // Now feed a null runout_lane
    nlohmann::json stepper_data;
    stepper_data["runout_lane"] = nullptr; // JSON null
    helper.feed_afc_stepper("lane1", stepper_data);

    // null runout_lane should clear the backup
    REQUIRE(helper.get_endless_spool_edges()[0] == -1); // Cleared
}

TEST_CASE("AFC message sets operation detail", "[ams][afc][message][phase1]") {
    // Real device: AFC global state has "message": {"message": "Loading T1", "type": "info"}
    // We never parse this field today, but it should set operation_detail.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {{"message", {{"message", "Loading T1"}, {"type", "info"}}}};
    helper.feed_afc_state(afc_data);

    // message.message should flow through to operation_detail
    REQUIRE(helper.get_operation_detail().find("Loading T1") != std::string::npos);
}

TEST_CASE("AFC error message emits EVENT_ERROR", "[ams][afc][message][phase1]") {
    // When message.type == "error", we should emit EVENT_ERROR with the message text
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.install_event_tracker();

    nlohmann::json afc_data = {
        {"message", {{"message", "AFC Error: lane1 failed to load"}, {"type", "error"}}}};
    helper.feed_afc_state(afc_data);

    // error type messages should emit EVENT_ERROR
    REQUIRE(helper.has_event(AmsBackend::EVENT_ERROR));
    // Error data should contain the message text
    std::string error_data = helper.get_event_data(AmsBackend::EVENT_ERROR);
    REQUIRE(error_data.find("lane1 failed to load") != std::string::npos);
}

TEST_CASE("AFC current_load and next_lane tracked", "[ams][afc][state][phase1]") {
    // Real device: AFC global state has "current_load": "lane2", "next_lane": "lane3"
    // These tell us which lane is actively loading and which is queued next.
    // We never parse these fields today.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"current_load", "lane2"}, {"next_lane", "lane3"}, {"current_state", "Loading"}};
    helper.feed_afc_state(afc_data);

    // current_load should update current_slot (lane2 = slot 1)
    REQUIRE(helper.get_current_slot() == 1);
    // operation_detail should mention the loading context
    // At minimum, the action should be LOADING from current_state
    REQUIRE(helper.get_action() == AmsAction::LOADING);
}

// ============================================================================
// Phase 2: Full Data Parsing Tests
// ============================================================================
//
// These tests verify parsing of extended hub, extruder, stepper, and buffer
// fields from real AFC device data. Tests use fixture structures captured
// from a real Box Turtle at 192.168.1.112.
// ============================================================================

TEST_CASE("AFC hub bowden length parsed from afc_bowden_length", "[ams][afc][hub][phase2]") {
    // Real device: AFC_hub Turtle_1 has "afc_bowden_length": 1285.0
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Set hub names so the status update routes correctly
    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    helper.feed_afc_hub("Turtle_1", {{"state", false}, {"afc_bowden_length", 1285.0}});

    // bowden_length should be stored and accessible for device actions
    auto actions = helper.get_device_actions();
    bool found_bowden = false;
    for (const auto& action : actions) {
        if (action.id == "bowden_length") {
            found_bowden = true;
            // Value should use the real bowden length, not hardcoded 450
            auto val = std::any_cast<float>(action.current_value);
            REQUIRE(val == Catch::Approx(1285.0f));
            break;
        }
    }
    REQUIRE(found_bowden);
}

TEST_CASE("AFC hub cutter info parsed", "[ams][afc][hub][phase2]") {
    // Real device: AFC_hub has "cut": false, "cut_dist": 50.0, etc.
    // We should track whether the hub has a cutter for UI decisions
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    helper.feed_afc_hub(
        "Turtle_1",
        {{"state", false}, {"cut", false}, {"cut_dist", 50.0}, {"afc_bowden_length", 1285.0}});

    // Hub sensor state should be updated
    REQUIRE(helper.get_hub_sensor() == false);

    // System info should reflect cutter availability
    auto sys_info = helper.get_system_info();
    // AFC always advertises TipMethod::CUT - but we should parse cut field
    // to know if cutter is actually present/configured
    REQUIRE(sys_info.tip_method == TipMethod::CUT);
}

TEST_CASE("AFC extruder speeds parsed", "[ams][afc][extruder][phase2]") {
    // Real device: AFC_extruder has "tool_load_speed": 25.0, "tool_unload_speed": 25.0
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_extruder("extruder", {{"tool_start_status", false},
                                          {"tool_end_status", false},
                                          {"tool_load_speed", 25.0},
                                          {"tool_unload_speed", 30.0}});

    // Sensor state should be updated
    REQUIRE(helper.get_tool_start_sensor() == false);
    REQUIRE(helper.get_tool_end_sensor() == false);
}

TEST_CASE("AFC extruder distances parsed", "[ams][afc][extruder][phase2]") {
    // Real device: tool_stn=42.0, tool_stn_unload=90.0
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_extruder("extruder", {{"tool_start_status", true},
                                          {"tool_end_status", false},
                                          {"tool_stn", 42.0},
                                          {"tool_stn_unload", 90.0}});

    REQUIRE(helper.get_tool_start_sensor() == true);
}

TEST_CASE("AFC stepper buffer_status parsed", "[ams][afc][stepper][phase2]") {
    // Real device: AFC_stepper lane1 has "buffer_status": "Advancing"
    // LaneSensors struct only has prep, load, loaded_to_hub today
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1",
                            {{"prep", true}, {"load", true}, {"buffer_status", "Advancing"}});

    // buffer_status should be stored on lane sensors
    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.prep == true);
    REQUIRE(sensors.load == true);
    REQUIRE(sensors.buffer_status == "Advancing");
}

TEST_CASE("AFC stepper filament_status parsed", "[ams][afc][stepper][phase2]") {
    // Real device: "filament_status": "Ready" or "Not Ready"
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1",
                            {{"filament_status", "Ready"}, {"filament_status_led", "#00ff00"}});

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.filament_status == "Ready");
}

TEST_CASE("AFC stepper dist_hub parsed", "[ams][afc][stepper][phase2]") {
    // Real device: "dist_hub": 200.0 (distance to hub in mm)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", {{"dist_hub", 200.0}});

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.dist_hub == Catch::Approx(200.0f));
}

TEST_CASE("AFC buffer object parsed via status update", "[ams][afc][buffer][phase2]") {
    // Real device: AFC_buffer Turtle_1 has "state": "Advancing", "enabled": false
    // We don't subscribe to or parse AFC_buffer objects today
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    // Feed buffer names through AFC state
    helper.feed_afc_state({{"buffers", {"Turtle_1"}}});

    // Now feed a buffer update
    helper.feed_afc_buffer("Turtle_1", {{"state", "Advancing"}, {"enabled", false}});

    // Verify the feed_afc_buffer path doesn't crash
    REQUIRE_NOTHROW(helper.feed_afc_buffer("Turtle_1", {{"state", "Idle"}, {"enabled", true}}));
}

TEST_CASE("AFC global quiet_mode parsed from AFC state", "[ams][afc][global][phase2]") {
    // Real device: AFC has "quiet_mode": false
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"quiet_mode", false}});
    REQUIRE(helper.get_quiet_mode() == false);

    // Toggle it on
    helper.feed_afc_state({{"quiet_mode", true}});
    REQUIRE(helper.get_quiet_mode() == true);
}

TEST_CASE("AFC global led_state parsed from AFC state", "[ams][afc][global][phase2]") {
    // Real device: AFC has "led_state": true
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"led_state", true}});
    REQUIRE(helper.get_led_state() == true);

    // Toggle it off
    helper.feed_afc_state({{"led_state", false}});
    REQUIRE(helper.get_led_state() == false);
}

TEST_CASE("AFC bowden slider max accommodates real bowden length",
          "[ams][afc][device_actions][phase2]") {
    // The bowden slider max was hardcoded to 1000mm, but real bowden can be 1285mm
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);

    helper.feed_afc_hub("Turtle_1", {{"state", false}, {"afc_bowden_length", 1285.0}});

    auto actions = helper.get_device_actions();
    for (const auto& action : actions) {
        if (action.id == "bowden_length") {
            // Max should accommodate the real bowden length
            REQUIRE(action.max_value >= 1285.0f);
            break;
        }
    }
}

// ============================================================================
// Phase 3: New Device Actions & Commands Tests
// ============================================================================
//
// Tests for new maintenance section, LED/mode toggles, and maintenance commands.
// ============================================================================

TEST_CASE("AFC device sections include maintenance and led",
          "[ams][afc][device_sections][phase3]") {
    AmsBackendAfcTestHelper helper;

    auto sections = helper.get_device_sections();

    bool has_maintenance = false;
    bool has_setup = false;
    for (const auto& section : sections) {
        if (section.id == "maintenance")
            has_maintenance = true;
        if (section.id == "setup")
            has_setup = true;
    }
    REQUIRE(has_maintenance);
    REQUIRE(has_setup);
}

TEST_CASE("AFC device action test_lanes dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("test_lanes");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_TEST_LANES"));
}

TEST_CASE("AFC device action change_blade dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("change_blade");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_CHANGE_BLADE"));
}

TEST_CASE("AFC device action park dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("park");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_PARK"));
}

TEST_CASE("AFC device action brush dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("brush");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_BRUSH"));
}

TEST_CASE("AFC device action reset_motor dispatches gcode", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.execute_device_action("reset_motor");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode_starting_with("AFC_RESET_MOTOR_TIME"));
}

TEST_CASE("AFC device action led toggle on when off", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // LED is off, toggling should turn it on
    helper.feed_afc_state({{"led_state", false}});

    auto result = helper.execute_device_action("led_toggle");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("TURN_ON_AFC_LED"));
}

TEST_CASE("AFC device action led toggle off when on", "[ams][afc][device_actions][phase3]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // LED is on, toggling should turn it off
    helper.feed_afc_state({{"led_state", true}});

    auto result = helper.execute_device_action("led_toggle");

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("TURN_OFF_AFC_LED"));
}

TEST_CASE("AFC device action quiet_mode dispatches an explicit ENABLE",
          "[ams][afc][device_actions][quiet_mode][phase3]") {
    // AFC_QUIET_MODE's ENABLE parameter defaults to the CURRENT value —
    // `gcmd.get_int("ENABLE", self._get_quiet_mode(), minval=0, maxval=1)`
    // (AFC.py v1.2.0:934-953, identical at v1.1.0:736-756) — so a bare
    // `AFC_QUIET_MODE` sets quiet mode to whatever it already was and the
    // button does nothing at all.
    //
    // The `show_macros` wrapper supplies no parameter either: _create_options
    // emits `{%set dummy=params.ENABLE|default('0')|int%}` and then
    // `_AFC_QUIET_MODE {rawparams}` (AFC_functions.py:637-644). The `dummy`
    // assignment is discarded and rawparams is empty for a bare call.
    //
    // So the ENABLE VALUE is the whole test. A prefix or substring match would
    // re-admit the exact bug, because `AFC_QUIET_MODE` is a prefix of the
    // correct command.
    SECTION("off -> on") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        REQUIRE_FALSE(helper.get_quiet_mode()); // default

        REQUIRE(helper.execute_device_action("quiet_mode").success());

        REQUIRE(helper.has_gcode("AFC_QUIET_MODE ENABLE=1"));
        // The value must be the INVERSE of the tracked state, not a constant.
        REQUIRE_FALSE(helper.has_gcode("AFC_QUIET_MODE ENABLE=0"));
    }

    SECTION("on -> off") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.feed_afc_state({{"quiet_mode", true}});
        REQUIRE(helper.get_quiet_mode());

        REQUIRE(helper.execute_device_action("quiet_mode").success());

        REQUIRE(helper.has_gcode("AFC_QUIET_MODE ENABLE=0"));
        REQUIRE_FALSE(helper.has_gcode("AFC_QUIET_MODE ENABLE=1"));
    }

    SECTION("never the bare no-op form, whatever the state") {
        // Exact-match, so this fails if the parameter is ever dropped again.
        for (bool quiet : {false, true}) {
            CAPTURE(quiet);
            AmsBackendAfcTestHelper helper;
            helper.initialize_test_lanes_with_slots(4);
            helper.feed_afc_state({{"quiet_mode", quiet}});
            helper.clear_captured_gcodes();

            REQUIRE(helper.execute_device_action("quiet_mode").success());

            REQUIRE(helper.captured_gcodes.size() == 1);
            REQUIRE_FALSE(helper.has_gcode("AFC_QUIET_MODE"));
            REQUIRE(helper.captured_gcodes[0] ==
                    (quiet ? "AFC_QUIET_MODE ENABLE=0" : "AFC_QUIET_MODE ENABLE=1"));
        }
    }
}

// ============================================================================
// Phase 4: Error Recovery Improvements Tests
// ============================================================================
//
// Tests for differentiated reset (AFC_RESET vs AFC_HOME), per-lane reset,
// and error message surfacing.
// ============================================================================

TEST_CASE("AFC recover sends AFC_RESET", "[ams][afc][recovery][phase4]") {
    // Regression guard — recover() should continue using AFC_RESET
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true); // Bypass precondition for unit test

    auto result = helper.recover();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_RESET"));
    REQUIRE_FALSE(helper.has_gcode("AFC_HOME"));
}

TEST_CASE("AFC reset sends AFC_RESET command", "[ams][afc][recovery]") {
    // reset() sends AFC_RESET — the same gcode as recover(), since AFC only has one reset command.
    // Both operations use AFC_RESET; the distinction is in the UI notification text only.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true); // Bypass precondition for unit test

    auto result = helper.reset();

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_RESET"));
}

TEST_CASE("AFC recover_lane_position sends AFC_LANE_RESET", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.recover_lane_position(0);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_LANE_RESET LANE=lane1"));
}

TEST_CASE("AFC recover_lane_position second lane", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.recover_lane_position(2);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("AFC_LANE_RESET LANE=lane3"));
}

TEST_CASE("AFC recover_lane_position validates slot index", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.recover_lane_position(99);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
}

TEST_CASE("AFC recover_lane_position validates negative index", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.recover_lane_position(-1);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
}

TEST_CASE("AFC lane reset is offered only when that lane's hub sensor is triggered",
          "[ams][afc][recovery]") {
    // Measured on a live BoxTurtle 2026-07-27: loaded_to_hub is latched at prep and
    // never updates, reading true on all four lanes at once while the hub is clear.
    // AFC_hub.state is the only signal that tracks an actual hub transit.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "Turtle_1");

    // Satisfy the other gates so the hub sensor is the only discriminator.
    helper.set_active_load_lane("lane1");
    helper.set_lane_load_sensor(0, true);

    // The latched field says "at hub" on every lane. It must not be believed.
    helper.set_lane_loaded_to_hub(0, true);
    helper.set_hub_sensor("Turtle_1", false);
    REQUIRE_FALSE(helper.can_recover_lane_position(0));

    // Hub sensor triggered → the retract has somewhere to retract from.
    helper.set_hub_sensor("Turtle_1", true);
    REQUIRE(helper.can_recover_lane_position(0));
}

TEST_CASE("AFC lane reset is refused while the toolhead holds filament", "[ams][afc][recovery]") {
    // Upstream's own toolhead guard logs and then falls through — it is missing
    // its `return` (AFCProject/AFC-Klipper-Add-On#803), so cmd_AFC_LANE_RESET
    // retracts the lane while the extruder still grips the filament. Ours is the
    // only check that actually stops that, so each of the three signals it reads
    // has to hold on its own.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "Turtle_1");
    helper.set_hub_sensor("Turtle_1", true);
    helper.set_active_load_lane("lane1");
    helper.set_lane_load_sensor(0, true);

    // Baseline: toolhead genuinely free.
    REQUIRE(helper.can_recover_lane_position(0));

    SECTION("tool_start sensor triggered") {
        helper.set_tool_start_sensor(true);
        REQUIRE_FALSE(helper.can_recover_lane_position(0));
        helper.set_tool_start_sensor(false);
        REQUIRE(helper.can_recover_lane_position(0));
    }

    SECTION("tool_end sensor triggered") {
        helper.set_tool_end_sensor(true);
        REQUIRE_FALSE(helper.can_recover_lane_position(0));
        helper.set_tool_end_sensor(false);
        REQUIRE(helper.can_recover_lane_position(0));
    }

    SECTION("AFC.current_load names a lane") {
        // This is the exact condition upstream's guard tests.
        helper.set_toolhead_lane("lane1");
        REQUIRE_FALSE(helper.can_recover_lane_position(0));
        helper.set_toolhead_lane("");
        REQUIRE(helper.can_recover_lane_position(0));
    }

    SECTION("a lane reports tool_loaded even with AFC.current_load empty") {
        // The desync case: AFC persists per-lane tool_loaded through save_vars,
        // so it survives a restart that leaves AFC.current null.
        helper.set_toolhead_lane("");
        helper.get_mutable_slot(2)->status = SlotStatus::LOADED;
        REQUIRE_FALSE(helper.can_recover_lane_position(0));
        helper.get_mutable_slot(2)->status = SlotStatus::AVAILABLE;
        REQUIRE(helper.can_recover_lane_position(0));
    }
}

TEST_CASE("AFC attribution survives the filament_loaded derivation", "[ams][afc][recovery]") {
    // The bug this pins: parse_afc_state() derives filament_loaded from
    // `loaded_lane`, which PREFERS current_lane (= AFC.current_loading). No
    // shipped AFC build publishes an explicit filament_loaded key, so naming a
    // lane in current_lane sets filament_loaded true as a side effect.
    //
    // A toolhead guard reading filament_loaded is therefore mutually exclusive
    // with attribution: the attributed arm could never fire, and recovery was
    // only ever reachable through the unattributed all-lanes fallback #1182
    // removes. Removing that fallback while the guard still read filament_loaded
    // would have deleted per-lane recovery outright rather than narrowing it.
    //
    // Driven through feed_afc_state() on purpose — with set_active_load_lane()
    // the derivation never runs and this test cannot fail.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "Turtle_1");
    helper.set_hub_sensor("Turtle_1", true);
    helper.set_lane_load_sensor(0, true);

    // A failed TOOL_LOAD: AFC names the lane it was working, the toolhead never
    // received anything, so current_load stays null.
    helper.feed_afc_state({{"current_lane", "lane1"}, {"current_load", nullptr}});

    REQUIRE(helper.get_active_load_lane() == "lane1");
    REQUIRE(helper.get_toolhead_lane().empty());
    REQUIRE(helper.get_system_info().filament_loaded); // the derivation, unchanged
    REQUIRE(helper.can_recover_lane_position(0));      // and recovery still offered
}

TEST_CASE("AFC lane reset is refused for a lane routed direct (no hub)", "[ams][afc][recovery]") {
    // "direct" routing means the lane bypasses the hub entirely, so there is no
    // hub sensor to consult and no hub-retract to perform.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "direct");
    helper.set_hub_sensor("Turtle_1", true);

    // Force the latched field true so this case discriminates: the pre-fix body
    // returned it directly and would answer true here. Without this the sensor's
    // false default makes old and new code agree, and the test proves nothing.
    helper.set_lane_loaded_to_hub(0, true);

    // Every other gate satisfied, so "direct" routing is the sole reason for the
    // refusal — otherwise this passes for the wrong reason once more gates exist.
    helper.set_active_load_lane("lane1");
    helper.set_lane_load_sensor(0, true);

    REQUIRE_FALSE(helper.can_recover_lane_position(0));
}

TEST_CASE("AFC attributes a triggered hub to the lane AFC names as active",
          "[ams][afc][recovery]") {
    // AFC_hub is one sensor shared by every lane on the unit, so a triggered hub
    // alone would offer recovery on all of them at once (observed on a live
    // BoxTurtle 2026-07-27). AFC.current_lane names the lane it was working.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    for (const char* lane : {"lane1", "lane2", "lane3", "lane4"}) {
        helper.set_lane_hub_routing(lane, "Turtle_1");
    }
    helper.set_hub_sensor("Turtle_1", true);

    // All four lanes are seated identically — the live BoxTurtle reads
    // prep/load/loaded_to_hub true on every lane at once. Attribution must be
    // what separates them, not any per-lane sensor.
    for (int i = 0; i < 4; ++i) {
        helper.set_lane_load_sensor(i, true);
    }

    helper.set_active_load_lane("lane2");

    REQUIRE_FALSE(helper.can_recover_lane_position(0));
    REQUIRE(helper.can_recover_lane_position(1));
    REQUIRE_FALSE(helper.can_recover_lane_position(2));
    REQUIRE_FALSE(helper.can_recover_lane_position(3));
}

TEST_CASE("AFC offers recovery on no lane at all when it names none", "[ams][afc][recovery]") {
    // This inverts the previous all-lanes fallback (prestonbrown/helixscreen#1182).
    //
    // The fallback rested on "a wrong guess costs one harmless refusal from the
    // firmware". That is false. cmd_AFC_LANE_RESET opens with an unconditional
    // move_to_hub(DISTANCE, NEG) — DISTANCE defaults to 50 — before any per-lane
    // state check, so a wrong guess physically retracts a correctly-seated lane
    // past its own load switch. Observed on the live BoxTurtle 2026-07-27: a
    // guess at lane1 left it load=False, which then failed T0 with "LOAD TRIGGER
    // NOT TRIGGERED" and needed a manual forward move to restore.
    //
    // Offering nothing does not strand the user: the sidebar Reset dispatches
    // AFC_RESET, which is AFC's OWN lane picker (cmd_AFC_RESET lists every lane
    // with raw_load_state true and dispatches AFC_LANE_RESET for the chosen
    // one). The firmware's candidate list beats anything we would guess.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    for (const char* lane : {"lane1", "lane2", "lane3", "lane4"}) {
        helper.set_lane_hub_routing(lane, "Turtle_1");
    }
    helper.set_hub_sensor("Turtle_1", true);

    // Every lane fully seated and the hub triggered: the ONLY thing missing is
    // attribution. Without these the lanes would be refused for lacking a load
    // switch and the test would not exercise the attribution gate at all.
    for (int i = 0; i < 4; ++i) {
        helper.set_lane_load_sensor(i, true);
    }
    helper.set_active_load_lane("");

    REQUIRE_FALSE(helper.lane_recovery_is_attributed());
    for (int i = 0; i < 4; ++i) {
        REQUIRE_FALSE(helper.can_recover_lane_position(i));
    }
}

TEST_CASE("AFC lane reset is refused while the lane's own load switch is clear",
          "[ams][afc][recovery]") {
    // prestonbrown/helixscreen#1187. cmd_AFC_LANE_RESET does not guard on the
    // lane's load state up front — it opens with an unconditional
    //     move_to_hub(cur_lane, DISTANCE, MoveDirection.NEG, ...)   # DISTANCE=50
    // and only checks `if not cur_lane.raw_load_state` AFTER that move plus one
    // further short move inside the retract loop. So invoking it on a lane whose
    // filament already sits behind its load switch retracts that lane a further
    // ~50mm toward (and potentially out of) its prep sensor and drive gears
    // before erroring out.
    //
    // That is exactly the state a lane is left in by a previous wrong guess, so
    // without this gate the most natural follow-up interaction — tapping Recover
    // again on the lane that just failed — compounds the damage.
    //
    // It also matches cmd_AFC_RESET's own picker, which lists only lanes with
    // raw_load_state true. AFC.get_status publishes that as `load`.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_lane_hub_routing("lane1", "Turtle_1");
    helper.set_hub_sensor("Turtle_1", true);
    helper.set_active_load_lane("lane1");

    // Hub occupied, toolhead free, AFC names this exact lane — every other gate
    // passes, so the load switch is the sole discriminator.
    helper.set_lane_load_sensor(0, false);
    REQUIRE_FALSE(helper.can_recover_lane_position(0));

    helper.set_lane_load_sensor(0, true);
    REQUIRE(helper.can_recover_lane_position(0));
}

TEST_CASE("AFC treats a stale active_load_lane_ as unattributed rather than "
          "matching no lane",
          "[ams][afc][recovery]") {
    // initialize_slots() (re-)runs whenever the lane count changes but never
    // touches active_load_lane_. If a unit re-init or lane rename leaves it
    // naming a lane that no longer exists in the registry, `lane_name ==
    // active_load_lane_` is false for every lane at once. Both the gate and the
    // UI-facing attribution flag must read that the same way — a stale name is
    // NOT attribution, so neither may treat it as one.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    for (const char* lane : {"lane1", "lane2", "lane3", "lane4"}) {
        helper.set_lane_hub_routing(lane, "Turtle_1");
    }
    helper.set_hub_sensor("Turtle_1", true);
    for (int i = 0; i < 4; ++i) {
        helper.set_lane_load_sensor(i, true);
    }

    // Attribute to a real lane first, to prove the stale case is what changes
    // behavior here (not simply an always-empty active_load_lane_).
    //
    // Driven through parse_afc_state rather than poking active_load_lane_ alone:
    // the name only ever reaches us on a frame that also carries current_state,
    // and lane_recovery_is_attributed() now reads the action too, so setting the
    // name by itself models a state AFC cannot produce.
    helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Loading"}});
    REQUIRE(helper.get_active_load_lane() == "lane2");
    REQUIRE(helper.lane_recovery_is_attributed());
    REQUIRE(helper.can_recover_lane_position(1));

    // Re-init drops lane2..lane4 from the registry. active_load_lane_ still
    // says "lane2", but lane2 no longer exists.
    helper.initialize_test_lanes_with_slots(1);
    helper.set_lane_load_sensor(0, true);
    REQUIRE(helper.get_active_load_lane() == "lane2");

    // Both the gate and the UI-facing attribution flag must fall back to
    // unattributed rather than disagree with each other.
    REQUIRE_FALSE(helper.lane_recovery_is_attributed());
    REQUIRE_FALSE(helper.can_recover_lane_position(0));
}

TEST_CASE("AFC: a current_loading latched by a FAILED toolchange is not attribution",
          "[ams][afc][recovery][eject]") {
    // AFC.current_loading is set at the top of TOOL_LOAD (AFC.py v1.2.0:1523) and
    // TOOL_UNLOAD (:1948). It is cleared in exactly two places, set_tool_loaded()
    // and set_tool_unloaded(), and BOTH clears sit under `if normal_toolchange:`
    // (AFC_lane.py:1526, :1545) — the success path. A toolchange that fails
    // therefore pins current_loading to that lane until the next SUCCESSFUL one,
    // which a user with a jammed lane cannot perform.
    //
    // AFC publishes it as `current_lane`, which active_load_lane_ prefers over
    // current_load precisely because attribution wants "the lane being worked".
    // That is right while a toolchange is running and wrong once it has stopped:
    // a latched name is the residue of a failure, not a live diagnosis.
    //
    // It matters because can_recover && recovery_attributed OUTRANKS Eject in
    // decide_unload_mode(). Treating the latch as attribution means the failure
    // that makes a user want Eject is the very thing that takes Eject away,
    // replacing it with Recover — a lane reset that retracts toward the hub and
    // never returns filament to the spool.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    for (const char* lane : {"lane1", "lane2", "lane3", "lane4"}) {
        helper.set_lane_hub_routing(lane, "Turtle_1");
    }
    helper.set_hub_sensor("Turtle_1", true);
    for (int i = 0; i < 4; ++i) {
        helper.set_lane_load_sensor(i, true);
    }

    SECTION("mid-toolchange, the named lane IS the one being worked") {
        helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Loading"}});

        REQUIRE(helper.get_active_load_lane() == "lane2");
        CHECK(helper.lane_recovery_is_attributed());
        CHECK(helper.can_recover_lane_position(1));
    }

    SECTION("an AFC error state is still a live diagnosis") {
        // The lane genuinely needs recovery here and AFC is still saying so, so
        // the attribution stands. Narrowing must not cost the case recovery
        // ranking exists to serve.
        helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Error"}});

        CHECK(helper.lane_recovery_is_attributed());
        CHECK(helper.can_recover_lane_position(1));
    }

    SECTION("back at Idle with the name still latched, it is NOT attribution") {
        // The failure path: TOOL_LOAD set current_loading, failed, and nothing
        // ever cleared it. AFC is idle, so no lane is being worked — the name is
        // stale by construction.
        helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Loading"}});
        REQUIRE(helper.lane_recovery_is_attributed());

        helper.test_parse_afc_state({{"current_state", "Idle"}});

        // The name is still there; AFC never clears it on failure.
        REQUIRE(helper.get_active_load_lane() == "lane2");
        // It no longer outranks Eject...
        CHECK_FALSE(helper.lane_recovery_is_attributed());
        // ...but Recover itself must REMAIN offered. can_recover_lane_position()
        // reads the same name as a safety gate — "only retract a lane AFC itself
        // names", so a blind opening retract cannot land on the wrong one
        // (#1182). A latched name still identifies the right lane, and the lane
        // really may be stranded. Withdrawing the ranking must not withdraw the
        // recovery; that would strand exactly the user it exists for.
        CHECK(helper.can_recover_lane_position(1));
    }

    SECTION("the whole point: Recover stays available but stops displacing Eject") {
        // decide_unload_mode() ranks `can_recover && attributed` above Eject and
        // plain `can_recover` below it. With attribution withdrawn and the gate
        // still open, the jammed lane gets Eject back AND keeps Recover as the
        // last-resort arm — which is the outcome this whole change is for.
        helper.test_parse_afc_state({{"current_lane", "lane2"}, {"current_state", "Idle"}});

        CHECK_FALSE(helper.lane_recovery_is_attributed());
        CHECK(helper.can_recover_lane_position(1));
    }
}

TEST_CASE("AFC active_load_lane_ is populated from a current_load delta and "
          "cleared when it goes null",
          "[ams][afc][recovery]") {
    // A setter-only test would pass even if parse_afc_state() never assigned
    // active_load_lane_ at all — this drives it through the real status-update
    // path (feed_afc_state -> parse_afc_state) to prove the parse actually runs.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    REQUIRE(helper.get_active_load_lane().empty());

    helper.feed_afc_state({{"current_load", "lane2"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});
    REQUIRE(helper.get_active_load_lane() == "lane2");

    helper.feed_afc_state({{"current_load", nullptr},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});
    REQUIRE(helper.get_active_load_lane().empty());
}

TEST_CASE("AFC active_load_lane_ survives a delta that omits both lane keys",
          "[ams][afc][recovery]") {
    // parse_afc_state() is fed notify_status_update DELTAS, not snapshots — an
    // absent key means "unchanged," never "clear." AFC.current_toolchange,
    // message, and current_state each trigger a parse independently of
    // current_lane/current_load. An unconditional assignment would wipe the
    // attribution on the very next such delta, re-exposing Recover on every
    // lane on the hub within one update — the original bug, back immediately.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"current_lane", "lane1"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});
    REQUIRE(helper.get_active_load_lane() == "lane1");

    // A delta that mentions neither current_lane nor current_load — only a
    // toolchange counter changed. Attribution must be untouched.
    helper.feed_afc_state({{"current_toolchange", 3}});
    REQUIRE(helper.get_active_load_lane() == "lane1");

    // Same for a message-only delta.
    helper.feed_afc_state({{"message", {{"message", "some notice"}, {"type", "info"}}}});
    REQUIRE(helper.get_active_load_lane() == "lane1");
}

TEST_CASE("AFC recover_lane_position fails when not running", "[ams][afc][recovery][phase4]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    // running_ defaults to false

    auto result = helper.recover_lane_position(0);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::NOT_CONNECTED);
}

TEST_CASE("AFC error message surfaces in EVENT_ERROR data", "[ams][afc][recovery][phase4]") {
    // Verify that AFC error messages contain useful text in the event data
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.install_event_tracker();

    nlohmann::json afc_data = {
        {"message", {{"message", "Lane 1 failed: filament jam detected"}, {"type", "error"}}}};
    helper.feed_afc_state(afc_data);

    REQUIRE(helper.has_event(AmsBackend::EVENT_ERROR));
    std::string error_data = helper.get_event_data(AmsBackend::EVENT_ERROR);
    REQUIRE(error_data.find("filament jam detected") != std::string::npos);
}

TEST_CASE("AFC clear_fault sends RESET_FAILURE and AFC_CLEAR_MESSAGE", "[ams][afc][recovery]") {
    // Measured 2026-07-27: AFC_RESET leaves printer.AFC.message untouched. Only
    // AFC_CLEAR_MESSAGE pops it, and RESET_FAILURE clears the failure flag.
    // Both must fire, and unlike cancel() this must work from IDLE — that is
    // exactly the state a queued message outlives its operation in.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.clear_fault(0);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("RESET_FAILURE"));
    REQUIRE(helper.has_gcode("AFC_CLEAR_MESSAGE"));
}

TEST_CASE("AFC clear_fault is scope-independent of the slot argument", "[ams][afc][recovery]") {
    // AFC has no per-lane fault clear; both commands are system-scoped. Passing a
    // slot must neither fail nor change the gcode emitted.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    REQUIRE(helper.clear_fault(-1).success());
    REQUIRE(helper.has_gcode("AFC_CLEAR_MESSAGE"));
}

TEST_CASE("AFC drains the message queue until it empties", "[ams][afc][recovery]") {
    // printer.AFC.message is a FIFO head; one AFC_CLEAR_MESSAGE pops one entry.
    // A second queued error surfaces only after the first is popped, so the
    // backend must keep clearing while deltas still carry a message.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.clear_fault(0);
    const int after_first = helper.gcode_count("AFC_CLEAR_MESSAGE");
    REQUIRE(after_first == 1);

    // Delta still carries a message: the next queue entry surfaced. Drain again.
    helper.test_parse_afc_state(nlohmann::json{
        {"message",
         {{"message", "Hub is already clear while trying to reset 'lane2'"}, {"type", "error"}}}});
    helper.test_maybe_drain_message_queue();
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 2);

    // Queue now empty: stop. No further clears.
    helper.test_parse_afc_state(nlohmann::json{{"message", {{"message", ""}, {"type", ""}}}});
    helper.test_maybe_drain_message_queue();
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 2);
}

TEST_CASE("AFC drains a session's worth of accumulated messages", "[ams][afc][recovery]") {
    // AFC's message_queue only ever grows on its own: one entry per
    // AFC_logger.error()/.warning() call, and neither reset_failure() nor
    // AFC_RESUME pops anything. A session therefore accumulates warnings and
    // resolved errors ahead of the actionable one — the reported case was depth
    // 4 behind a per-print-start SET_AFC_TOOLCHANGES deprecation warning
    // (#1186). The stopping condition must be an empty queue, not a fixed
    // count: whatever a short budget truncates survives to become the next
    // session's stale error.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    // Entry 0 is the head clear_fault() pops; the rest surface one at a time.
    const std::vector<std::string> queued = {
        "SET_AFC_TOOLCHANGES is deprecated",
        "lane1 Current lane not loaded, LOAD TRIGGER NOT TRIGGERED\n"
        "||==>--||----||-----||\nTRG   LOAD   HUB   TOOL",
        "'lane1' failed to reset to hub, load switch became false during reset",
        "'lane2' failed to reset to hub, load switch became false during reset",
        "lane3 filament failed to trigger pre extruder gear toolhead sensor",
    };

    helper.clear_fault(0);
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 1); // popped queued[0]

    // Each pop exposes the next head. Four deltas, four more clears.
    for (size_t i = 1; i < queued.size(); ++i) {
        helper.test_parse_afc_state(
            nlohmann::json{{"message", {{"message", queued[i]}, {"type", "error"}}}});
        helper.test_maybe_drain_message_queue();
    }
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == static_cast<int>(queued.size()));

    // Queue empty: stop, and stay stopped.
    helper.test_parse_afc_state(nlohmann::json{{"message", {{"message", ""}, {"type", ""}}}});
    helper.test_maybe_drain_message_queue();
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == static_cast<int>(queued.size()));
}

TEST_CASE("AFC message drain is bounded", "[ams][afc][recovery]") {
    // A fault that re-enqueues as fast as we pop must not spin forever.
    // MESSAGE_DRAIN_MAX_CLEARS is the runaway guard, not the expected exit — the
    // preceding test covers the normal drain-until-empty path.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.clear_fault(0);
    for (int i = 0; i < 50; ++i) {
        helper.test_parse_afc_state(
            nlohmann::json{{"message", {{"message", "still failing"}, {"type", "error"}}}});
        helper.test_maybe_drain_message_queue();
    }

    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") ==
            AmsBackendAfcTestHelper::MESSAGE_DRAIN_MAX_CLEARS);
}

TEST_CASE("AFC clear_fault discards queued lane ejects", "[ams][afc][recovery]") {
    // cancel() flushes pending_eject_lanes_ before its IDLE check. clear_fault()
    // replaced cancel() at the error-modal dismiss site, so it must flush too —
    // otherwise dismissing an error leaves a LANE_UNLOAD chain queued to run.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.test_queue_pending_eject("lane2");
    helper.test_queue_pending_eject("lane3");
    REQUIRE(helper.test_pending_eject_count() == 2);

    helper.clear_fault(0);

    REQUIRE(helper.test_pending_eject_count() == 0);
}

TEST_CASE("AFC drain does not fire without a preceding clear_fault", "[ams][afc][recovery]") {
    // Messages arrive constantly in normal operation. Only an explicit
    // clear_fault() arms the drain; otherwise the UI just displays them.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.test_parse_afc_state(
        nlohmann::json{{"message", {{"message", "Lane 1 loaded"}, {"type", ""}}}});
    helper.test_maybe_drain_message_queue();

    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 0);
}

TEST_CASE("AFC drain arm expires so a stale budget cannot eat a later unrelated error",
          "[ams][afc][recovery]") {
    // Pressing Reset when AFC.message is already empty leaves message_drain_budget_
    // armed. printer.AFC.message is a delta field, so if the queue was already
    // empty at clear_fault() time, no later delta will ever carry a `message` key
    // at all — the empty-message disarm in parse_afc_state() never fires, and
    // without a wall-clock bound the budget would sit armed until a genuinely new,
    // unrelated error rolls in and gets silently acknowledged. The deadline is what
    // actually bounds the arm's lifetime.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.clear_fault(0);
    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 1);

    // Simulate the window having passed with no intervening deltas.
    helper.set_message_drain_deadline_offset(std::chrono::seconds(-1));

    // A brand-new, unrelated error arrives. It must surface to the user, not be
    // silently popped by the stale residual budget.
    helper.test_parse_afc_state(
        nlohmann::json{{"message", {{"message", "filament jam detected"}, {"type", "error"}}}});
    helper.test_maybe_drain_message_queue();

    REQUIRE(helper.gcode_count("AFC_CLEAR_MESSAGE") == 1);
}

// ============================================================================
// Phase 2: Mixed Topology — Flat String Units, AFC_lane, Unit Objects, Multi-Extruder
// ============================================================================

// --- 2a: Flat string units array ---

TEST_CASE("AFC backend handles flat string units array", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed AFC state with flat string units (real hardware format)
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"OpenAMS AMS_1", "Box_Turtle Turtle_1", "OpenAMS AMS_2"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane4", "lane5", "lane6", "lane7", "lane8", "lane9", "lane10",
                               "lane11", "lane0", "lane1", "lane2", "lane3"});
    afc_state["extruders"] = nlohmann::json::array(
        {"extruder", "extruder1", "extruder2", "extruder3", "extruder4", "extruder5"});
    helper.feed_afc_state(afc_state);

    // Verify unit_infos_ populated from string parsing
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);

    // Check that type/name were parsed from "Type Name" format
    bool found_openams_1 = false;
    bool found_bt_1 = false;
    bool found_openams_2 = false;
    for (const auto& ui : unit_infos) {
        if (ui.name == "AMS_1" && ui.type == "OpenAMS") {
            found_openams_1 = true;
            REQUIRE(ui.klipper_key == "AFC_OpenAMS AMS_1");
        }
        if (ui.name == "Turtle_1" && ui.type == "Box_Turtle") {
            found_bt_1 = true;
            REQUIRE(ui.klipper_key == "AFC_BoxTurtle Turtle_1");
        }
        if (ui.name == "AMS_2" && ui.type == "OpenAMS") {
            found_openams_2 = true;
            REQUIRE(ui.klipper_key == "AFC_OpenAMS AMS_2");
        }
    }
    REQUIRE(found_openams_1);
    REQUIRE(found_bt_1);
    REQUIRE(found_openams_2);

    // System type is still AFC
    auto info = helper.get_system_info();
    REQUIRE(info.type == AmsType::AFC);
}

TEST_CASE("AFC backend ViViD unit klipper_key uses lowercase", "[ams][afc][vivid]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // ViViD reports as "ViViD vivid_1" but Klipper object is AFC_vivid (lowercase)
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"ViViD vivid_1"});
    afc_state["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].type == "ViViD");
    REQUIRE(unit_infos[0].name == "vivid_1");
    REQUIRE(unit_infos[0].klipper_key == "AFC_vivid vivid_1"); // lowercase, not AFC_ViViD
}

TEST_CASE("AFC backend flat string units: single word name still parses", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Edge case: unit string with no space should not crash
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"NoSpaceUnit"});
    helper.feed_afc_state(afc_state);

    // Should not crash; unit_infos_ may be empty (no space = can't parse)
    const auto& unit_infos = helper.get_unit_infos();
    // Single word without space has no valid type/name split
    REQUIRE(unit_infos.empty());
}

// --- 2b: Unit-level object data ---

TEST_CASE("AFC backend unit-level object populates AfcUnitInfo", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // First, feed flat string units to populate unit_infos_
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1", "OpenAMS AMS_2"});
    helper.feed_afc_state(afc_state);

    // Then feed unit-level object data via status update
    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt_data["hubs"] = nlohmann::json::array();
    bt_data["buffers"] = nlohmann::json::array({"TN", "TN1", "TN2", "TN3"});

    nlohmann::json ams1_data;
    ams1_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
    ams1_data["extruders"] = nlohmann::json::array({"extruder4"});
    ams1_data["hubs"] = nlohmann::json::array({"Hub_1", "Hub_2", "Hub_3", "Hub_4"});
    ams1_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_OpenAMS AMS_1"] = ams1_data;
    helper.feed_status_update(params);

    // Verify unit_infos_ got populated with lane/extruder/hub/buffer data
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);

    // Find Turtle_1 and verify
    for (const auto& ui : unit_infos) {
        if (ui.name == "Turtle_1") {
            REQUIRE(ui.lanes.size() == 4);
            REQUIRE(ui.extruders.size() == 4);
            REQUIRE(ui.hubs.empty());
            REQUIRE(ui.buffers.size() == 4);
            // Box Turtle: empty hubs + multiple extruders → PARALLEL
            REQUIRE(ui.topology == PathTopology::PARALLEL);
        }
        if (ui.name == "AMS_1") {
            REQUIRE(ui.lanes.size() == 4);
            REQUIRE(ui.extruders.size() == 1);
            REQUIRE(ui.hubs.size() == 4);
            REQUIRE(ui.buffers.empty());
            // OpenAMS: hubs present + 1 extruder → HUB
            REQUIRE(ui.topology == PathTopology::HUB);
        }
    }
}

TEST_CASE("AFC HTLF mixed topology classification from per-lane hub routing",
          "[ams][afc][topology][mixed]") {
    // HTLF unit has 4 lanes: 2 direct to their own extruders, 2 through hub to shared extruder.
    // Per-lane hub field: "direct" vs hub name determines MIXED topology.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(3);

    // Feed stepper data WITH hub routing BEFORE unit object (stepper parsing happens first)
    helper.feed_afc_stepper("lane0", {{"map", "T0"},
                                      {"extruder", "extruder"},
                                      {"hub", "direct"},
                                      {"status", "Loaded"},
                                      {"color", "FF0000"}});
    helper.feed_afc_stepper("lane1", {{"map", "T2"},
                                      {"extruder", "extruder1"},
                                      {"hub", "direct"},
                                      {"status", "Loaded"},
                                      {"color", "00FF00"}});
    helper.feed_afc_stepper("lane2", {{"map", "T1"},
                                      {"extruder", "extruder2"},
                                      {"hub", "HTLF_1"},
                                      {"status", "Loaded"},
                                      {"color", "0000FF"}});
    helper.feed_afc_stepper("lane3", {{"map", "T3"},
                                      {"extruder", "extruder2"},
                                      {"hub", "HTLF_1"},
                                      {"status", "Loaded"},
                                      {"color", "FFFF00"}});

    // Feed AFC state with unit name
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"HTLF HTLF_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object (triggers topology classification)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder", "extruder1", "extruder2"});
    unit_data["hubs"] = nlohmann::json::array({"HTLF_1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_HTLF HTLF_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is MIXED (not PARALLEL or HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::MIXED);

    // Verify per-lane hub routing data
    REQUIRE(unit_infos[0].lane_is_hub_routed.size() == 4);
    REQUIRE(unit_infos[0].lane_is_hub_routed[0] == false); // lane0 direct
    REQUIRE(unit_infos[0].lane_is_hub_routed[1] == false); // lane1 direct
    REQUIRE(unit_infos[0].lane_is_hub_routed[2] == true);  // lane2 via hub
    REQUIRE(unit_infos[0].lane_is_hub_routed[3] == true);  // lane3 via hub

    // Verify propagation to AmsUnit via get_system_info
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() >= 1);
    bool found = false;
    for (const auto& unit : info.units) {
        if (unit.name == "HTLF HTLF_1") {
            found = true;
            REQUIRE(unit.topology == PathTopology::MIXED);
            REQUIRE(unit.lane_is_hub_routed.size() == 4);
            REQUIRE(unit.lane_is_hub_routed[0] == false);
            REQUIRE(unit.lane_is_hub_routed[1] == false);
            REQUIRE(unit.lane_is_hub_routed[2] == true);
            REQUIRE(unit.lane_is_hub_routed[3] == true);
        }
    }
    REQUIRE(found);
}

TEST_CASE("AFC MIXED unit: a direct-fed lane and a hub-routed lane disagree on unload-before-load",
          "[ams][afc][topology][mixed][dispatch]") {
    // needs_unload_before_load() is a PER-LANE question and this is the machine
    // that proves it: one HTLF unit, lanes 0/1 wired straight to their own
    // extruders and lanes 2/3 merged through HTLF_1 into a shared one. Feeding
    // lane 1 disturbs nothing that lane 2 owns; feeding lane 3 must first clear
    // the shared path. A backend-wide answer cannot say both.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(3);

    helper.feed_afc_stepper(
        "lane0",
        {{"map", "T0"}, {"extruder", "extruder"}, {"hub", "direct"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T2"}, {"extruder", "extruder1"}, {"hub", "direct"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"map", "T1"}, {"extruder", "extruder2"}, {"hub", "HTLF_1"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"map", "T3"}, {"extruder", "extruder2"}, {"hub", "HTLF_1"}, {"status", "Loaded"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"HTLF HTLF_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder", "extruder1", "extruder2"});
    unit_data["hubs"] = nlohmann::json::array({"HTLF_1"});
    unit_data["buffers"] = nlohmann::json::array();
    nlohmann::json params;
    params["AFC_HTLF HTLF_1"] = unit_data;
    helper.feed_status_update(params);

    // Seat lane 2 (hub-routed) so the serial rule has something to want cleared.
    helper.set_filament_loaded(true);
    helper.set_current_slot(2);

    AmsSystemInfo info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    const AmsUnit& unit = info.units[0];
    REQUIRE(unit.topology == PathTopology::MIXED);
    REQUIRE(unit.lane_is_hub_routed.size() == 4);
    REQUIRE_FALSE(unit.lane_is_hub_routed[1]); // direct
    REQUIRE(unit.lane_is_hub_routed[3]);       // through HTLF_1

    const int direct_slot = unit.first_slot_global_index + 1;
    const int hub_slot = unit.first_slot_global_index + 3;

    // The whole point: same backend, same snapshot, two different answers.
    CHECK_FALSE(helper.needs_unload_before_load(info, direct_slot));
    CHECK(helper.needs_unload_before_load(info, hub_slot));

    // Guard rail — the backend-wide topology says HUB, so a get_topology()-keyed
    // rule would have answered "swap" for the direct lane too. That is the bug.
    REQUIRE(helper.get_topology() == PathTopology::HUB);
}

TEST_CASE("AFC uniform HUB unit: an unparsed lane does not masquerade as direct-fed",
          "[ams][afc][topology][mixed][dispatch]") {
    // lane_is_hub_routed stores `false` for lanes whose routing has not arrived
    // yet — Moonraker sorts unit objects before AFC_lane ones, so a frame can
    // carry a unit while some of its lanes have never been parsed (#1229 defect
    // 4). On a uniform unit that placeholder is indistinguishable from "direct",
    // which is why the per-lane vector is consulted ONLY on a MIXED unit. Pin it:
    // every lane of a pure-hub unit still needs the shared path cleared.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // lane3's routing is deliberately never fed.
    for (const char* lane : {"lane0", "lane1", "lane2"}) {
        helper.feed_afc_stepper(
            lane,
            {{"map", "T0"}, {"extruder", "extruder"}, {"hub", "Turtle_1"}, {"status", "Loaded"}});
    }

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder"});
    unit_data["hubs"] = nlohmann::json::array({"Turtle_1"});
    unit_data["buffers"] = nlohmann::json::array();
    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = unit_data;
    helper.feed_status_update(params);

    helper.set_filament_loaded(true);
    helper.set_current_slot(0);

    AmsSystemInfo info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    const AmsUnit& unit = info.units[0];
    REQUIRE(unit.topology == PathTopology::HUB);
    REQUIRE(unit.lane_is_hub_routed.size() == 4);
    REQUIRE_FALSE(unit.lane_is_hub_routed[3]); // the placeholder, not a real "direct"

    CHECK(helper.needs_unload_before_load(info, unit.first_slot_global_index + 3));
}

TEST_CASE("AFC all-hub lanes classified as HUB topology", "[ams][afc][topology]") {
    // When all lanes route through a hub, topology should be HUB (not MIXED)
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Feed stepper data: all lanes through hub
    helper.feed_afc_stepper("lane0", {{"hub", "Hub_1"}, {"status", "Loaded"}, {"color", "FF0000"}});
    helper.feed_afc_stepper("lane1", {{"hub", "Hub_1"}, {"status", "Loaded"}, {"color", "00FF00"}});
    helper.feed_afc_stepper("lane2", {{"hub", "Hub_1"}, {"status", "Loaded"}, {"color", "0000FF"}});
    helper.feed_afc_stepper("lane3", {{"hub", "Hub_1"}, {"status", "Loaded"}, {"color", "FFFF00"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder"});
    unit_data["hubs"] = nlohmann::json::array({"Hub_1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_OpenAMS AMS_1"] = unit_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::HUB);
    // All lanes hub-routed
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == true);
    }
}

TEST_CASE("AFC all-direct lanes classified as PARALLEL topology", "[ams][afc][topology]") {
    // When all lanes are direct and multiple extruders exist, topology should be PARALLEL
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Feed stepper data: all lanes direct
    helper.feed_afc_stepper("lane0",
                            {{"hub", "direct"}, {"extruder", "extruder"}, {"status", "Loaded"}});
    helper.feed_afc_stepper("lane1",
                            {{"hub", "direct"}, {"extruder", "extruder1"}, {"status", "Loaded"}});
    helper.feed_afc_stepper("lane2",
                            {{"hub", "direct"}, {"extruder", "extruder2"}, {"status", "Loaded"}});
    helper.feed_afc_stepper("lane3",
                            {{"hub", "direct"}, {"extruder", "extruder3"}, {"status", "Loaded"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array();
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = unit_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    // All lanes direct
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == false);
    }
}

TEST_CASE("AFC direct_load hub field classified as direct (not hub-routed)",
          "[ams][afc][topology]") {
    // Box Turtle lanes may report hub:"direct_load" instead of hub:"direct".
    // Both must be treated as direct (not hub-routed). Regression: #392
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Feed stepper data: all lanes with hub:"direct_load"
    helper.feed_afc_stepper(
        "lane0", {{"hub", "direct_load"}, {"extruder", "extruder"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane1", {{"hub", "direct_load"}, {"extruder", "extruder1"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane2", {{"hub", "direct_load"}, {"extruder", "extruder2"}, {"status", "Loaded"}});
    helper.feed_afc_stepper(
        "lane3", {{"hub", "direct_load"}, {"extruder", "extruder3"}, {"status", "Loaded"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array();
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = unit_data;
    helper.feed_status_update(params);

    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == false);
    }
}

TEST_CASE("AFC unit with hubs AND multiple extruders gets PARALLEL topology",
          "[ams][afc][mixed][toolchanger]") {
    // Toolchanger scenario: HTLF unit has 4 lanes, 3 extruders, 1 hub.
    // Lanes 1,2 go direct to extruder/extruder1, lanes 3,4 go through hub to extruder2.
    // Must be PARALLEL so each unique mapped_tool gets its own physical nozzle.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(3);

    // Feed AFC state with unit name
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"HTLF HTLF_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object: hubs present + 3 extruders (mixed topology)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder", "extruder1", "extruder2"});
    unit_data["hubs"] = nlohmann::json::array({"hub1"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_HTLF HTLF_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is PARALLEL (not HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::PARALLEL);

    // Feed stepper map data: lanes 0,1 direct, lanes 2,3 shared via hub
    helper.feed_afc_stepper(
        "lane0",
        {{"map", "T0"}, {"extruder", "extruder"}, {"status", "Loaded"}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T2"}, {"extruder", "extruder1"}, {"status", "Loaded"}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"map", "T1"}, {"extruder", "extruder2"}, {"status", "Loaded"}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"map", "T3"}, {"extruder", "extruder2"}, {"status", "Loaded"}, {"color", "FFFF00"}});

    // Verify tool mappings
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    auto* slot1 = info.get_slot_global(1);
    auto* slot2 = info.get_slot_global(2);
    auto* slot3 = info.get_slot_global(3);
    REQUIRE(slot0->mapped_tool == 0);
    REQUIRE(slot1->mapped_tool == 2);
    REQUIRE(slot2->mapped_tool == 1);
    REQUIRE(slot3->mapped_tool == 3); // Shared extruder2 via hub

    // Verify extruder_name is populated from AFC stepper data
    CHECK(slot0->extruder_name == "extruder");
    CHECK(slot1->extruder_name == "extruder1");
    CHECK(slot2->extruder_name == "extruder2");
    CHECK(slot3->extruder_name == "extruder2");
}

TEST_CASE("AFC hub-routed lanes with per-lane extruders classified as PARALLEL",
          "[ams][afc][topology]") {
    // ACE Pro as lane loader through AFC: 4 lanes, each with its own hub AND its
    // own extruder, all under one unit. Each lane independently feeds a different
    // toolhead through a hub sensor — this is parallel topology, not hub merger.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Feed stepper data: each lane has its own hub and extruder
    helper.feed_afc_stepper("lane0", {{"hub", "ace_hub1"},
                                      {"extruder", "extruder"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"color", "89a84f"}});
    helper.feed_afc_stepper("lane1", {{"hub", "ace_hub2"},
                                      {"extruder", "extruder1"},
                                      {"map", "T1"},
                                      {"status", "Loaded"},
                                      {"color", "23ADE9"}});
    helper.feed_afc_stepper("lane2", {{"hub", "ace_hub3"},
                                      {"extruder", "extruder2"},
                                      {"map", "T2"},
                                      {"status", "Loaded"},
                                      {"color", "F20808"}});
    helper.feed_afc_stepper("lane3", {{"hub", "ace_hub4"},
                                      {"extruder", "extruder3"},
                                      {"map", "T3"},
                                      {"status", "Loaded"},
                                      {"color", "DE4343"}});

    // Feed AFC state with unit
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"ACE Ace_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit object: 4 hubs + 4 extruders (parallel with hubs)
    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    unit_data["hubs"] = nlohmann::json::array({"ace_hub1", "ace_hub2", "ace_hub3", "ace_hub4"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_ACE Ace_1"] = unit_data;
    helper.feed_status_update(params);

    // Verify topology is PARALLEL (not HUB)
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::PARALLEL);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::PARALLEL);

    // All lanes are hub-routed (each through its own hub)
    REQUIRE(unit_infos[0].lane_is_hub_routed.size() == 4);
    for (bool routed : unit_infos[0].lane_is_hub_routed) {
        REQUIRE(routed == true);
    }

    // Verify tool mappings are 1:1
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    auto* slot1 = info.get_slot_global(1);
    auto* slot2 = info.get_slot_global(2);
    auto* slot3 = info.get_slot_global(3);
    REQUIRE(slot0->mapped_tool == 0);
    REQUIRE(slot1->mapped_tool == 1);
    REQUIRE(slot2->mapped_tool == 2);
    REQUIRE(slot3->mapped_tool == 3);
}

TEST_CASE("AFC hub-routed lanes with shared extruder still classified as HUB",
          "[ams][afc][topology]") {
    // Standard Box Turtle / OpenAMS: 4 lanes through hubs but only 1 extruder.
    // Must remain HUB topology — the new parallel-with-hubs check should NOT
    // trigger when extruder count != lane count.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Feed stepper data: all lanes through per-lane hubs but same extruder
    helper.feed_afc_stepper(
        "lane0",
        {{"hub", "Hub_1"}, {"extruder", "extruder4"}, {"status", "Loaded"}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"hub", "Hub_2"}, {"extruder", "extruder4"}, {"status", "Loaded"}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"hub", "Hub_3"}, {"extruder", "extruder4"}, {"status", "Loaded"}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"hub", "Hub_4"}, {"extruder", "extruder4"}, {"status", "Loaded"}, {"color", "FFFF00"}});

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    nlohmann::json unit_data;
    unit_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    unit_data["extruders"] = nlohmann::json::array({"extruder4"});
    unit_data["hubs"] = nlohmann::json::array({"Hub_1", "Hub_2", "Hub_3", "Hub_4"});
    unit_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_OpenAMS AMS_1"] = unit_data;
    helper.feed_status_update(params);

    // Must still be HUB — 4 hubs but only 1 extruder
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].topology == PathTopology::HUB);
}

TEST_CASE("AFC backend unit object triggers lane reorganization", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed flat string units
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit-level data for both units
    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt_data["hubs"] = nlohmann::json::array();
    bt_data["buffers"] = nlohmann::json::array();

    nlohmann::json ams1_data;
    ams1_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
    ams1_data["extruders"] = nlohmann::json::array({"extruder4"});
    ams1_data["hubs"] = nlohmann::json::array({"Hub_1"});
    ams1_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_OpenAMS AMS_1"] = ams1_data;
    helper.feed_status_update(params);

    // After both units are parsed, units should be reorganized
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    // Units sorted alphabetically: AMS_1 before Turtle_1
    // (reorganize_slots sorts unit names)
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[1].slot_count == 4);
    REQUIRE(info.total_slots == 8);
}

// --- 2c: AFC_lane status updates ---

TEST_CASE("AFC backend handles AFC_lane status updates", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed an AFC_lane update (same schema as AFC_stepper)
    nlohmann::json lane_data;
    lane_data["name"] = "lane4";
    lane_data["unit"] = "AMS_1";
    lane_data["hub"] = "Hub_1";
    lane_data["extruder"] = "extruder4";
    lane_data["buffer"] = nullptr;
    lane_data["prep"] = true;
    lane_data["load"] = true;
    lane_data["tool_loaded"] = false;
    lane_data["loaded_to_hub"] = false;
    lane_data["material"] = "PLA";
    lane_data["spool_id"] = 13;
    lane_data["color"] = "#000000";
    lane_data["weight"] = 295.25;
    lane_data["map"] = "T4";
    lane_data["status"] = "Loaded";
    lane_data["filament_status"] = "Ready";
    lane_data["dist_hub"] = 60;

    // Feed as AFC_lane (not AFC_stepper)
    nlohmann::json params;
    params["AFC_lane lane4"] = lane_data;
    helper.feed_status_update(params);

    // Verify the lane was parsed using parse_afc_stepper (same JSON schema)
    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(4);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->material == "PLA");
    REQUIRE(slot->mapped_tool == 4);
    REQUIRE(slot->color_rgb == 0x000000);
    // AFC "Loaded" means hub-loaded, tool_loaded=false → AVAILABLE, not LOADED
    REQUIRE(slot->status == SlotStatus::AVAILABLE);
}

TEST_CASE("AFC backend handles mix of AFC_stepper and AFC_lane in same update",
          "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Feed both AFC_stepper and AFC_lane in same notification
    nlohmann::json stepper_data;
    stepper_data["prep"] = true;
    stepper_data["load"] = true;
    stepper_data["material"] = "PETG";
    stepper_data["color"] = "#FF0000";
    stepper_data["map"] = "T0";
    stepper_data["status"] = "Loaded";

    nlohmann::json lane_data;
    lane_data["prep"] = true;
    lane_data["load"] = true;
    lane_data["material"] = "ABS";
    lane_data["color"] = "#00FF00";
    lane_data["map"] = "T4";
    lane_data["status"] = "Loaded";

    nlohmann::json params;
    params["AFC_stepper lane0"] = stepper_data;
    params["AFC_lane lane4"] = lane_data;
    helper.feed_status_update(params);

    // Both should be parsed
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    REQUIRE(slot0 != nullptr);
    REQUIRE(slot0->material == "PETG");

    auto* slot4 = info.get_slot_global(4);
    REQUIRE(slot4 != nullptr);
    REQUIRE(slot4->material == "ABS");
}

// --- 2d: Multiple AFC_extruder objects ---

TEST_CASE("AFC backend handles multiple AFC_extruder objects", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Set extruder names from AFC state
    nlohmann::json afc_state;
    afc_state["extruders"] = nlohmann::json::array(
        {"extruder", "extruder1", "extruder2", "extruder3", "extruder4", "extruder5"});
    helper.feed_afc_state(afc_state);

    // Verify extruder_names_ populated
    const auto& ext_names = helper.get_extruder_names();
    REQUIRE(ext_names.size() == 6);

    // Feed multiple extruder updates
    nlohmann::json params;
    params["AFC_extruder extruder4"] = {
        {"tool_start_status", true}, {"tool_end_status", false}, {"lane_loaded", "lane4"}};
    params["AFC_extruder extruder5"] = {
        {"tool_start_status", false}, {"tool_end_status", false}, {"lane_loaded", nullptr}};
    helper.feed_status_update(params);

    // Verify current slot updated from extruder4's lane_loaded
    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 4);
}

TEST_CASE("AFC backend multi-extruder backward compat: single extruder still works",
          "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Do NOT set extruder_names_ (empty = backward compat)
    // Feed single AFC_extruder extruder (old format)
    nlohmann::json params;
    params["AFC_extruder extruder"] = {
        {"tool_start_status", true}, {"tool_end_status", true}, {"lane_loaded", "lane0"}};
    helper.feed_status_update(params);

    // Should still work via backward-compat fallback
    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 0);
    REQUIRE(helper.get_tool_start_sensor() == true);
    REQUIRE(helper.get_tool_end_sensor() == true);
}

TEST_CASE("AFC backend stores extruder names from AFC state extruders array", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);

    nlohmann::json afc_state;
    afc_state["extruders"] = nlohmann::json::array({"extruder", "extruder1"});
    helper.feed_afc_state(afc_state);

    const auto& names = helper.get_extruder_names();
    REQUIRE(names.size() == 2);
    REQUIRE(names[0] == "extruder");
    REQUIRE(names[1] == "extruder1");
}

// --- Backward compatibility ---

TEST_CASE("AFC backend backward compat: object-format units still works", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Old format: units as objects with name and lanes
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Box Turtle 1";
    unit_obj["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    unit_obj["connected"] = true;
    afc_state["units"] = nlohmann::json::array({unit_obj});
    helper.feed_afc_state(afc_state);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].name == "Box Turtle 1");
    // unit_infos_ should be empty (object format doesn't populate it)
    REQUIRE(helper.get_unit_infos().empty());
}

TEST_CASE("AFC backend backward compat: mixed string and object units", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(8);
    helper.initialize_slots_from_discovery();

    // Mix of string and object units (shouldn't happen in practice, but be robust)
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Old Turtle";
    unit_obj["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    afc_state["units"] = nlohmann::json::array({"OpenAMS AMS_1", unit_obj});
    helper.feed_afc_state(afc_state);

    // String unit creates unit_info, object unit goes through old path
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 1);
    REQUIRE(unit_infos[0].name == "AMS_1");
}

// ============================================================================
// Phase 6: Backward Compatibility Tests
// ============================================================================

TEST_CASE("AFC get_unit_topology falls back to get_topology when unit_infos empty",
          "[ams][afc][backward_compat]") {
    // Standard non-mixed AFC: unit_infos_ is empty, so get_unit_topology()
    // should fall back to get_topology() which returns HUB.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // No flat string units fed — unit_infos_ is empty
    REQUIRE(helper.get_unit_infos().empty());

    // get_unit_topology() for any index should fall back to get_topology() = HUB
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(1) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(-1) == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(99) == PathTopology::HUB);
}

TEST_CASE("AFC get_topology still returns HUB for standard AFC", "[ams][afc][backward_compat]") {
    // Regression guard: get_topology() must always return HUB for AFC backend
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.get_topology() == PathTopology::HUB);
}

TEST_CASE("AFC backend with only AFC_stepper lanes works correctly (no AFC_lane)",
          "[ams][afc][backward_compat]") {
    // Standard Box Turtle setup: only AFC_stepper objects, no AFC_lane objects.
    // The AFC_lane loop should simply skip when no AFC_lane objects exist.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed only AFC_stepper updates (standard non-mixed Box Turtle)
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", false},
                                      {"material", "PLA"},
                                      {"color", "#FF0000"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"weight", 850}});
    helper.feed_afc_stepper("lane2", {{"prep", true},
                                      {"load", false},
                                      {"material", "PETG"},
                                      {"color", "#00FF00"},
                                      {"map", "T1"},
                                      {"status", "Loaded"}});

    // Verify stepper data parsed correctly
    auto info = helper.get_system_info();
    auto* slot0 = info.get_slot_global(0);
    REQUIRE(slot0 != nullptr);
    REQUIRE(slot0->material == "PLA");
    REQUIRE(slot0->color_rgb == 0xFF0000);
    REQUIRE(slot0->mapped_tool == 0);
    // AFC "Loaded" with no tool_loaded → AVAILABLE (hub-loaded only)
    REQUIRE(slot0->status == SlotStatus::AVAILABLE);

    auto* slot1 = info.get_slot_global(1);
    REQUIRE(slot1 != nullptr);
    REQUIRE(slot1->material == "PETG");
    REQUIRE(slot1->color_rgb == 0x00FF00);
    REQUIRE(slot1->mapped_tool == 1);

    // Sensors should work via AFC_stepper path
    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.prep == true);
    REQUIRE(sensors.load == true);
    REQUIRE(sensors.loaded_to_hub == false);

    // Topology should still be HUB (standard AFC)
    REQUIRE(helper.get_topology() == PathTopology::HUB);
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
}

TEST_CASE("AFC standard single-unit system unchanged by mixed topology code",
          "[ams][afc][backward_compat]") {
    // Verify that feeding a standard single-unit AFC state (object format)
    // does not populate unit_infos_ and preserves the old unit structure
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Feed old-format AFC state with object-style unit
    nlohmann::json afc_state;
    nlohmann::json unit_obj;
    unit_obj["name"] = "Box Turtle 1";
    unit_obj["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    unit_obj["connected"] = true;
    afc_state["units"] = nlohmann::json::array({unit_obj});
    afc_state["current_state"] = "Idle";
    helper.feed_afc_state(afc_state);

    // unit_infos_ should remain empty (object format does not populate it)
    REQUIRE(helper.get_unit_infos().empty());

    // Standard unit structure should still be correct
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].name == "Box Turtle 1");
    REQUIRE(info.units[0].slot_count == 4);

    // Topology falls back to HUB
    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
    REQUIRE(helper.get_action() == AmsAction::IDLE);
}

// ============================================================================
// eject_lane() Tests
// ============================================================================

TEST_CASE("AFC eject_lane sends LANE_UNLOAD command", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.eject_lane(0);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
}

// ============================================================================
// eject_lane() sends LANE_UNLOAD unconditionally
//
// We used to transcribe cmd_LANE_UNLOAD's own if/elif chain here, forked by an
// inferred AFC version, so a refusal could be reported locally. AFC's maintainer
// asked us not to gate their macros (prestonbrown/helixscreen#1258), and the
// fork could not be made correct anyway: there is no reliable AFC version to
// read, so the era was guessed from whether is_standalone appeared in the
// status payload.
//
// These cases pin the absence of that gate. Every one is a state the old mirror
// refused on; all of them must now dispatch.
//
// Mutation check: re-add any refusal keyed on lane_loaded, hub routing, or
// is_standalone to eject_lane() and these fail.
// ============================================================================

TEST_CASE("AFC eject_lane dispatches regardless of upstream refusal conditions",
          "[ams][afc][eject]") {
    SECTION("lane seated at its own extruder") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.set_extruder_lane_loaded("extruder", "lane1");

        REQUIRE(helper.eject_lane(0).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
    }

    SECTION("lane routed direct rather than through a hub") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.set_lane_hub_routing("lane1", "direct");

        REQUIRE(helper.eject_lane(0).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
    }

    SECTION("standalone extruder holding nothing") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.report_extruder_standalone("extruder", true);
        helper.set_extruder_lane_loaded("extruder", "");

        REQUIRE(helper.eject_lane(0).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane1"));
    }

    SECTION("aggregate state says this slot is the loaded one") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.set_filament_loaded(true);
        helper.set_current_slot(1);

        REQUIRE(helper.eject_lane(1).success());
        CHECK(helper.has_gcode("LANE_UNLOAD LANE=lane2"));
    }
}

TEST_CASE("AFC eject_lane targets correct lane", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.eject_lane(2);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("LANE_UNLOAD LANE=lane3"));
}

TEST_CASE("AFC eject_lane allows eject of non-current slot even when filament loaded",
          "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.set_current_slot(0);

    // Eject slot 2 while slot 0 is loaded — should work
    auto result = helper.eject_lane(2);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("LANE_UNLOAD LANE=lane3"));
}

TEST_CASE("AFC eject_lane validates slot index", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.eject_lane(99);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
}

TEST_CASE("AFC eject_lane fails when not running", "[ams][afc][eject]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto result = helper.eject_lane(0);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::NOT_CONNECTED);
}

// LANE_UNLOAD serialization
//
// Rapid-fire LANE_UNLOAD commands overlap their lane LED animations and stepper
// work on AFC's MCU and were a contributor to Klippy "Timer too close"
// shutdowns when a user tapped Eject on multiple lanes in quick succession.
// eject_lane() now queues consecutive requests and dispatches them one at a
// time, with the next one fired only when the previous completes.

TEST_CASE("AFC eject_lane serializes consecutive ejects (only first dispatches)",
          "[ams][afc][eject][serialization]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());

    // Only the first eject should have dispatched — the rest are queued.
    REQUIRE(helper.captured_gcodes.size() == 1);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
}

TEST_CASE("AFC eject_lane drains queue in order on each completion",
          "[ams][afc][eject][serialization]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());

    // After each completion, the next queued eject should fire.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");

    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 3);
    REQUIRE(helper.captured_gcodes[2] == "LANE_UNLOAD LANE=lane3");

    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 4);
    REQUIRE(helper.captured_gcodes[3] == "LANE_UNLOAD LANE=lane4");

    // Queue empty — further completions do nothing.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 4);
}

TEST_CASE("AFC eject_lane deduplicates consecutive duplicate slot requests",
          "[ams][afc][eject][serialization]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    // Repeat-tap the same lane while the first is still running.
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(1).success()); // dup — should be coalesced
    REQUIRE(helper.eject_lane(1).success()); // dup — should be coalesced

    helper.complete_pending_unload();
    helper.complete_pending_unload();

    // Expected dispatches: lane1, lane2 (only one lane2, despite three taps).
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");
}

TEST_CASE("AFC eject_lane fires next eject after previous completes (synchronous mode)",
          "[ams][afc][eject][serialization]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    // Default: defer_lane_unload_complete = false → completion fires synchronously.

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());

    // Synchronous completion means by the time eject_lane returns, the queue
    // has fully drained and both gcodes have dispatched.
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[0] == "LANE_UNLOAD LANE=lane1");
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");
}

TEST_CASE("AFC eject_lane queue drains on failure (error path)",
          "[ams][afc][eject][serialization]") {
    // In production, dispatch_lane_unload registers success AND error callbacks
    // with api_->execute_gcode. Both call on_lane_unload_done() so the queue
    // drains either way. This test simulates the error path: the in-flight
    // gcode "fails", but on_lane_unload_done still advances to the next queued
    // entry. Without this guarantee a transient AFC error would strand the
    // queue and silently break subsequent eject taps.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());

    REQUIRE(helper.captured_gcodes.size() == 1);

    // Simulate the error callback path — completion still drains the queue.
    helper.complete_pending_unload(); // "lane1 errored" → fire lane2
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane2");

    helper.complete_pending_unload(); // "lane2 errored" → fire lane3
    REQUIRE(helper.captured_gcodes.size() == 3);
    REQUIRE(helper.captured_gcodes[2] == "LANE_UNLOAD LANE=lane3");

    helper.complete_pending_unload(); // "lane3 errored" → queue empty
    REQUIRE(helper.captured_gcodes.size() == 3);
    // Subsequent eject_lane should fire immediately (in-flight flag cleared).
    REQUIRE(helper.eject_lane(3).success());
    REQUIRE(helper.captured_gcodes.size() == 4);
    REQUIRE(helper.captured_gcodes[3] == "LANE_UNLOAD LANE=lane4");
}

TEST_CASE("AFC cancel drops queued ejects but lets in-flight complete",
          "[ams][afc][eject][serialization][cancel]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.defer_lane_unload_complete = true;

    REQUIRE(helper.eject_lane(0).success());
    REQUIRE(helper.eject_lane(1).success());
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.eject_lane(3).success());
    REQUIRE(helper.captured_gcodes.size() == 1); // only lane1 dispatched

    // Cancel should drop queued lane2/lane3/lane4 but not abort the in-flight
    // lane1 — its completion callback is still pending.
    helper.cancel();

    // The in-flight callback firing now should NOT dispatch a queued next
    // (queue was cleared). It should just clear eject_in_flight_.
    helper.complete_pending_unload();
    REQUIRE(helper.captured_gcodes.size() == 1); // no new dispatches

    // A fresh eject after cancel completes should fire immediately.
    REQUIRE(helper.eject_lane(2).success());
    REQUIRE(helper.captured_gcodes.size() == 2);
    REQUIRE(helper.captured_gcodes[1] == "LANE_UNLOAD LANE=lane3");
}

TEST_CASE("AFC supports_lane_eject returns true", "[ams][afc][capability]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.supports_lane_eject());
}

// ============================================================================
// Slot status mapping: AFC "Loaded" vs tool_loaded
// ============================================================================

TEST_CASE("AFC hub-loaded lane is AVAILABLE, not LOADED", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Exact production state: loaded_to_hub=true, tool_loaded=false, status="Loaded"
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", false},
                                      {"material", "ASA"},
                                      {"color", "#000000"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"weight", 570}});

    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(0);
    REQUIRE(slot != nullptr);
    // Hub-loaded filament should be AVAILABLE (ready to load to toolhead)
    REQUIRE(slot->status == SlotStatus::AVAILABLE);
    // Should NOT be the "current" loaded slot
    REQUIRE(info.current_slot == -1);
    REQUIRE_FALSE(info.filament_loaded);
}

TEST_CASE("AFC tool_loaded=true lane is LOADED", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Filament actually at the toolhead
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", true},
                                      {"material", "ASA"},
                                      {"color", "#000000"},
                                      {"map", "T0"},
                                      {"status", "Loaded"},
                                      {"weight", 570}});

    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(0);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->status == SlotStatus::LOADED);
}

TEST_CASE("AFC 'Tooled' status maps to LOADED even without tool_loaded flag",
          "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // OpenAMS uses "Tooled" status string
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", false},
                                      {"material", "PLA"},
                                      {"color", "#FF0000"},
                                      {"map", "T0"},
                                      {"status", "Tooled"}});

    auto info = helper.get_system_info();
    auto* slot = info.get_slot_global(0);
    REQUIRE(slot != nullptr);
    // "Tooled" is an explicit toolhead-loaded indicator
    REQUIRE(slot->status == SlotStatus::LOADED);
}

TEST_CASE("AFC context menu shows Eject for hub-loaded slot", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Two lanes loaded to hub, none to toolhead
    helper.feed_afc_stepper("lane1", {{"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"tool_loaded", false},
                                      {"material", "ASA"},
                                      {"map", "T0"},
                                      {"status", "Loaded"}});

    auto slot = helper.get_slot_info(0);
    // Slot should be present (has filament)
    REQUIRE(slot.is_present());
    // But NOT loaded to extruder
    REQUIRE(slot.status == SlotStatus::AVAILABLE);
    REQUIRE(slot.status != SlotStatus::LOADED);
}

TEST_CASE("AFC slot transitions from LOADED to AVAILABLE on unload", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // First: loaded to toolhead
    helper.feed_afc_stepper("lane1", {{"tool_loaded", true},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T0"},
                                      {"material", "ASA"}});

    auto info = helper.get_system_info();
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::LOADED);

    // Then: unloaded from toolhead, still at hub
    helper.feed_afc_stepper("lane1", {{"tool_loaded", false},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T0"},
                                      {"material", "ASA"}});

    info = helper.get_system_info();
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::AVAILABLE);
}

// ============================================================================
// filament_loaded derived from stepper tool_loaded (no top-level AFC field)
// ============================================================================

TEST_CASE("AFC filament_loaded derived from stepper tool_loaded", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Simulate AFC version without top-level "filament_loaded" field:
    // only lane stepper data drives loaded state
    helper.feed_afc_stepper("lane4", {{"tool_loaded", true},
                                      {"status", "Tooled"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);
    REQUIRE(info.current_slot == 3); // lane4 = slot index 3
    REQUIRE(info.get_slot_global(3)->status == SlotStatus::LOADED);
}

TEST_CASE("AFC filament_loaded clears when tool_loaded goes false", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Load lane4
    helper.feed_afc_stepper("lane4", {{"tool_loaded", true},
                                      {"status", "Tooled"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);

    // Unload — tool_loaded goes false
    helper.feed_afc_stepper("lane4", {{"tool_loaded", false},
                                      {"status", "Loaded"},
                                      {"prep", true},
                                      {"load", true},
                                      {"loaded_to_hub", true},
                                      {"map", "T3"},
                                      {"material", "ABS"}});

    info = helper.get_system_info();
    REQUIRE_FALSE(info.filament_loaded);
    REQUIRE(info.get_slot_global(3)->status == SlotStatus::AVAILABLE);
}

TEST_CASE("AFC current_load fallback sets current_slot and filament_loaded", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // AFC state with current_load (not current_lane) and no filament_loaded field
    helper.feed_afc_state({{"current_load", "lane1"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 0); // lane1 = slot index 0
    // filament_loaded derived from current_load
    REQUIRE(info.filament_loaded);
}

TEST_CASE("AFC current_tool derived from current_load via tool mapping",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4); // Sets slot N → tool N mapping

    // AFC reports current_load=lane3 but no current_tool field
    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2); // lane3 = slot index 2
    REQUIRE(info.current_tool == 2); // derived from slot 2 → T2 mapping
}

TEST_CASE("AFC ignores a current_tool key it never publishes",
          "[ams][afc][status][current_tool][deadfields]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4); // slot N → tool N

    // There is no "current_tool" in AFC.get_status() on any version (AFC.py
    // v1.2.0:2531-2564). The branch that let it override the slot→tool mapping
    // was unreachable, and honouring it would have let an invented key put the
    // UI on a tool that does not exist.
    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_tool", 5},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2);
    // The slot→tool mapping is the only authority: slot 2 → T2, not the 5 fed in.
    REQUIRE(info.current_tool == 2);
}

TEST_CASE("AFC current_tool cleared on unload (current_load null)",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // First: load lane2
    helper.feed_afc_state({{"current_load", "lane2"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_tool == 1); // lane2 → slot 1 → T1
    REQUIRE(info.filament_loaded);

    // Then: unload — current_load becomes null
    helper.feed_afc_state({{"current_load", nullptr},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    info = helper.get_system_info();
    REQUIRE(info.current_tool == -1); // cleared
    REQUIRE(info.current_slot == -1);
    REQUIRE_FALSE(info.filament_loaded);
}

TEST_CASE("AFC current_tool uses default 1:1 mapping from initialize_slots",
          "[ams][afc][status][current_tool]") {
    AmsBackendAfcTestHelper helper;
    // initialize_test_lanes + initialize_slots_from_discovery creates default 1:1 mapping
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_state({{"current_load", "lane3"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.current_slot == 2);
    // Default 1:1 mapping: slot 2 → T2
    REQUIRE(info.current_tool == 2);
}

TEST_CASE("AFC stepper post-scan owns filament_loaded; a stray AFC.filament_loaded is ignored",
          "[ams][afc][status][deadfields]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // No AFC version publishes filament_loaded on the AFC object (AFC.py
    // v1.2.0:2531-2564), so the per-lane scan is the only authority. The old
    // code suppressed the scan whenever the key was merely PRESENT — which,
    // had AFC ever added it, would have handed a single boolean veto over
    // every lane's tool_loaded.
    nlohmann::json params;
    params["AFC"] = {{"filament_loaded", false}, {"current_state", "Unloading"}};
    params["AFC_stepper lane1"] = {
        {"tool_loaded", true}, {"status", "Tooled"}, {"prep", true}, {"load", true}, {"map", "T0"}};
    helper.feed_status_update(params);

    auto info = helper.get_system_info();
    // The lane says it is at the toolhead, so the system is loaded.
    REQUIRE(info.filament_loaded);
    REQUIRE(info.get_slot_global(0)->status == SlotStatus::LOADED);
}

TEST_CASE("AFC current_load null clears filament state", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // First: loaded via current_load
    helper.feed_afc_state({{"current_load", "lane1"},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    auto info = helper.get_system_info();
    REQUIRE(info.filament_loaded);
    REQUIRE(info.current_slot == 0);

    // Then: unloaded — current_load becomes null
    helper.feed_afc_state({{"current_load", nullptr},
                           {"current_state", "Idle"},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}}});

    info = helper.get_system_info();
    REQUIRE_FALSE(info.filament_loaded);
    REQUIRE(info.current_slot == -1);
}

// ============================================================================
// manages_active_spool() Tests
//
// AFC manages Spoolman's active spool natively (calls spoolman_set_active_spool
// on tool load/unload). HelixScreen must NOT call set_active_spool when AFC is
// the active backend, to avoid racing with AFC's own calls.
// ============================================================================

TEST_CASE("AFC backend reports manages_active_spool=true", "[ams][afc][spoolman]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.manages_active_spool() == true);
}

TEST_CASE("AFC get_type returns AFC", "[ams][afc][spoolman]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.get_type() == AmsType::AFC);
}

// ============================================================================
// auto_unloads_after_print() driven by per-printer SettingsManager toggle
// ============================================================================
//
// Unlike CFS/IFS (which hardcode auto_unloads_after_print()==true), AFC's
// post-print unload depends on the user's macros. The behaviour is controlled
// by a per-printer setting: get_afc_unload_after_print() (default false). The
// pre-print runout warning is suppressed only when the setting is enabled.

TEST_CASE_METHOD(LVGLTestFixture, "AFC auto_unloads_after_print defaults to false (setting off)",
                 "[ams][afc][capability]") {
    helix::Config::get_instance();
    helix::SettingsManager::instance().init_subjects();

    // Ensure the setting starts disabled
    helix::SettingsManager::instance().set_afc_unload_after_print(false);

    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.auto_unloads_after_print() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC auto_unloads_after_print returns true when setting enabled",
                 "[ams][afc][capability]") {
    helix::Config::get_instance();
    helix::SettingsManager::instance().init_subjects();

    helix::SettingsManager::instance().set_afc_unload_after_print(true);

    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.auto_unloads_after_print() == true);

    // Toggling the setting back off must flip the capability back
    helix::SettingsManager::instance().set_afc_unload_after_print(false);
    REQUIRE(helper.auto_unloads_after_print() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "SettingsManager afc_unload_after_print round-trips and persists",
                 "[ams][afc][settings]") {
    helix::Config* config = helix::Config::get_instance();
    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    const std::string path = config->df() + "ams/afc_unload_after_print";

    SECTION("defaults to false") {
        settings.set_afc_unload_after_print(false);
        REQUIRE(settings.get_afc_unload_after_print() == false);
    }

    SECTION("set true -> get true -> persisted to per-printer config path") {
        settings.set_afc_unload_after_print(true);
        REQUIRE(settings.get_afc_unload_after_print() == true);
        REQUIRE(config->get<bool>(path, false) == true);

        // subject reflects the new value
        REQUIRE(lv_subject_get_int(settings.subject_afc_unload_after_print()) == 1);
    }

    SECTION("set false after true clears the value") {
        settings.set_afc_unload_after_print(true);
        settings.set_afc_unload_after_print(false);
        REQUIRE(settings.get_afc_unload_after_print() == false);
        REQUIRE(config->get<bool>(path, true) == false);
        REQUIRE(lv_subject_get_int(settings.subject_afc_unload_after_print()) == 0);
    }
}

TEST_CASE("AFC backend reports tracks_weight_locally=true", "[ams][afc][spoolman]") {
    AmsBackendAfcTestHelper helper;
    REQUIRE(helper.tracks_weight_locally() == true);
}

// ============================================================================
// Toolchanger mode: AFC_SELECT_TOOL / AFC_UNSELECT_TOOL
// ============================================================================

TEST_CASE("AFC change_tool sends AFC_SELECT_TOOL in toolchanger mode", "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.setup_toolchanger(2);

    auto result = helper.change_tool(1);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SELECT_TOOL TOOL=extruder1"));
}

TEST_CASE("AFC change_tool sends T{n} in single-extruder mode", "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    // num_extruders_ defaults to 1, no setup_toolchanger call

    auto result = helper.change_tool(1);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("T1"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_SELECT_TOOL"));
}

TEST_CASE("AFC change_tool sends AFC_SELECT_TOOL with first extruder name",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.setup_toolchanger(3);

    auto result = helper.change_tool(0);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SELECT_TOOL TOOL=extruder"));
}

TEST_CASE("AFC load_filament sends AFC_SELECT_TOOL in toolchanger mode",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.setup_toolchanger(2);

    auto result = helper.load_filament(1);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SELECT_TOOL TOOL=extruder1"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("CHANGE_TOOL"));
}

TEST_CASE("AFC load_filament sends CHANGE_TOOL in single-extruder mode",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    auto result = helper.load_filament(1);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("CHANGE_TOOL LANE=lane2"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_SELECT_TOOL"));
}

TEST_CASE("AFC multi-extruder WITHOUT a toolchanger never sends AFC_SELECT_TOOL",
          "[ams][afc][toolchanger][select_tool]") {
    // The machine the old `num_extruders_ > 1` gate got wrong: IDEX, or two
    // standalone toolheads, each with its own [AFC_extruder] section and no
    // [AFC_Toolchanger] anywhere. AFC_SELECT_TOOL is registered exclusively by
    // AfcToolchanger.__init__ (AFC_Toolchanger.py:47-49, v1.2.0 only), so
    // Klipper answers `// Unknown command:"AFC_SELECT_TOOL"` and the operation
    // silently does nothing.
    SECTION("load_filament falls back to CHANGE_TOOL") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.setup_multi_extruder_no_toolchanger(3);

        REQUIRE(helper.load_filament(1));
        REQUIRE(helper.has_gcode("CHANGE_TOOL LANE=lane2"));
        REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_SELECT_TOOL"));
    }

    SECTION("change_tool falls back to T{n}") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.setup_multi_extruder_no_toolchanger(3);

        REQUIRE(helper.change_tool(1));
        REQUIRE(helper.has_gcode("T1"));
        REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_SELECT_TOOL"));
    }

    SECTION("the SAME machine plus a toolchanger section does tool-select") {
        // Both halves of the branch off one fixture: the only difference is the
        // toolchanger, which is exactly what the gate is supposed to key on.
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        helper.setup_multi_extruder_no_toolchanger(3);
        helper.seed_configfile_toolchanger(true);

        REQUIRE(helper.change_tool(1));
        REQUIRE(helper.has_gcode("AFC_SELECT_TOOL TOOL=extruder1"));
        REQUIRE_FALSE(helper.has_gcode("T1"));
    }
}

TEST_CASE("AFC unload_filament sends bare TOOL_UNLOAD in toolchanger mode (no slot)",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.setup_toolchanger(2);

    // No slot specified (slot_index = -1): unload whatever is currently loaded.
    auto result = helper.unload_active_filament();

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("TOOL_UNLOAD LANE="));
    REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
}

// #999: selecting a non-active lane and pressing Unload must unload THAT lane,
// not whatever is loaded on the shuttle. AFC's TOOL_UNLOAD LANE=<lane> picks up
// the correct tool first, so the requested lane is honored even in toolchanger
// mode (previously this sent a bare AFC_UNSELECT_TOOL and ignored the slot).
TEST_CASE("AFC unload_filament sends TOOL_UNLOAD LANE=<lane> for specific slot (#999)",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);
    helper.setup_toolchanger(4);

    SECTION("slot 0 unloads lane1") {
        auto result = helper.unload_filament(0);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane1"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }

    SECTION("slot 1 unloads lane2") {
        auto result = helper.unload_filament(1);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane2"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }

    SECTION("slot 3 unloads lane4") {
        auto result = helper.unload_filament(3);
        REQUIRE(result);
        REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane4"));
        REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
    }
}

// Bypass: the external spool feeds the toolhead with no lane behind it, so
// slots_.name_of(-2) resolves to "" and AFC unloads whatever is at the head via
// its own user-configured macro. The backend has always done this correctly —
// the UI decision layer refused to dispatch, treating -2 as "nothing resolved"
// alongside -1. Pinned here so a future tightening of the lane lookup cannot
// start rejecting the sentinel outright.
TEST_CASE("AFC unload_filament under bypass sends bare TOOL_UNLOAD", "[ams][afc][bypass]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_supports_bypass(true);
    helper.set_filament_loaded(true);
    helper.set_current_slot(-2); // bypass sentinel

    auto result = helper.unload_filament(-2);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD"));
    // No lane may be named: LANE=lane1 would make AFC select and unload bay 1,
    // which is empty, while the external spool stayed threaded through the head.
    REQUIRE_FALSE(helper.has_gcode_starting_with("TOOL_UNLOAD LANE="));
}

TEST_CASE("AFC unload_filament sends bare TOOL_UNLOAD in single-extruder mode (no slot)",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);

    auto result = helper.unload_active_filament();

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("TOOL_UNLOAD LANE="));
    REQUIRE_FALSE(helper.has_gcode("AFC_UNSELECT_TOOL"));
}

TEST_CASE("AFC unload_filament honors slot_index in single-extruder mode (#999)",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);
    helper.set_filament_loaded(true);

    auto result = helper.unload_filament(3);

    REQUIRE(result);
    REQUIRE(helper.has_gcode("TOOL_UNLOAD LANE=lane4"));
    REQUIRE_FALSE(helper.has_gcode_starting_with("AFC_UNSELECT_TOOL"));
}

// ============================================================================
// LED device actions (multi-extruder toolchanger mode)
// ============================================================================

TEST_CASE("AFC get_device_actions includes per-extruder LED toggles in toolchanger mode",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto actions = helper.get_device_actions();

    // Should have per-extruder LED actions
    bool found_t0 = false, found_t1 = false;
    for (const auto& a : actions) {
        if (a.id == "led_extruder_T0")
            found_t0 = true;
        if (a.id == "led_extruder_T1")
            found_t1 = true;
    }
    REQUIRE(found_t0);
    REQUIRE(found_t1);

    // Should NOT have the generic led_extruder action
    bool found_generic = false;
    for (const auto& a : actions) {
        if (a.id == "led_extruder")
            found_generic = true;
    }
    REQUIRE_FALSE(found_generic);
}

TEST_CASE("AFC get_device_actions does not include LED toggles in single-extruder mode",
          "[ams][afc][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    auto actions = helper.get_device_actions();

    bool found_per_extruder = false;
    for (const auto& a : actions) {
        if (a.id.rfind("led_extruder_T", 0) == 0)
            found_per_extruder = true;
    }
    REQUIRE_FALSE(found_per_extruder);
}

TEST_CASE("AFC execute_device_action led_extruder_T0 sends AFC_SET_EXTRUDER_LED",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto result = helper.execute_device_action("led_extruder_T0");

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=1"));
}

TEST_CASE("AFC execute_device_action led_extruder_T1 sends AFC_SET_EXTRUDER_LED",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto result = helper.execute_device_action("led_extruder_T1");

    REQUIRE(result);
    REQUIRE(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder1 TURN_ON=1"));
}

TEST_CASE("AFC execute_device_action led_extruder toggles state", "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    // First call turns ON
    auto result1 = helper.execute_device_action("led_extruder_T0");
    REQUIRE(result1);
    REQUIRE(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=1"));

    helper.clear_captured_gcodes();

    // Second call turns OFF
    auto result2 = helper.execute_device_action("led_extruder_T0");
    REQUIRE(result2);
    REQUIRE(helper.has_gcode("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=0"));
}

TEST_CASE("AFC execute_device_action led_extruder rejects invalid tool index",
          "[ams][afc][toolchanger][led]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.setup_toolchanger(2);

    auto result = helper.execute_device_action("led_extruder_T5");

    REQUIRE_FALSE(result);
}

// ============================================================================
// Multi-unit AFC scenarios (mixed topologies)
// ============================================================================

TEST_CASE("Mixed BoxTurtle + ViViD units with string format", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;

    // Pre-initialize 12 lanes (non-contiguous: lane1-lane8 + lane13-lane16)
    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3",  "lane4",  "lane5",  "lane6",
                                          "lane7", "lane8", "lane13", "lane14", "lane15", "lane16"};
    helper.set_discovered_lanes(all_lanes, {"Turtle_2"});
    helper.initialize_slots_from_discovery();

    // Feed AFC state with 3 string-format units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "Box_Turtle Turtle_2", "ViViD vivid_1"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane1", "lane2", "lane3", "lane4", "lane5", "lane6", "lane7",
                               "lane8", "lane13", "lane14", "lane15", "lane16"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    // Verify unit_infos_ parsed correctly
    const auto& unit_infos = helper.get_unit_infos();
    REQUIRE(unit_infos.size() == 3);
    CHECK(unit_infos[0].type == "Box_Turtle");
    CHECK(unit_infos[0].name == "Turtle_1");
    CHECK(unit_infos[0].klipper_key == "AFC_BoxTurtle Turtle_1");
    CHECK(unit_infos[1].type == "Box_Turtle");
    CHECK(unit_infos[1].name == "Turtle_2");
    CHECK(unit_infos[1].klipper_key == "AFC_BoxTurtle Turtle_2");
    CHECK(unit_infos[2].type == "ViViD");
    CHECK(unit_infos[2].name == "vivid_1");
    CHECK(unit_infos[2].klipper_key == "AFC_vivid vivid_1");

    // Feed BoxTurtle 1 unit object (lanes 1-4, parallel topology)
    nlohmann::json bt1_data;
    bt1_data["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    bt1_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt1_data["hubs"] = nlohmann::json::array();
    bt1_data["buffers"] = nlohmann::json::array();

    // Feed BoxTurtle 2 unit object (lanes 5-8, parallel topology)
    nlohmann::json bt2_data;
    bt2_data["lanes"] = nlohmann::json::array({"lane5", "lane6", "lane7", "lane8"});
    bt2_data["extruders"] =
        nlohmann::json::array({"extruder4", "extruder5", "extruder6", "extruder7"});
    bt2_data["hubs"] = nlohmann::json::array();
    bt2_data["buffers"] = nlohmann::json::array();

    // Feed ViViD unit object (lanes 13-16, hub topology)
    nlohmann::json vivid_data;
    vivid_data["lanes"] = nlohmann::json::array({"lane13", "lane14", "lane15", "lane16"});
    vivid_data["extruders"] = nlohmann::json::array({"extruder"});
    vivid_data["hubs"] = nlohmann::json::array({"vivid_hub"});
    vivid_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt1_data;
    params["AFC_BoxTurtle Turtle_2"] = bt2_data;
    params["AFC_vivid vivid_1"] = vivid_data;
    helper.feed_status_update(params);

    // After all 3 units have lanes, reorganization should have happened
    auto& sys_info = helper.get_system_info_mutable();
    REQUIRE(sys_info.units.size() == 3);

    // Check per-unit and total slot counts
    int total_slots = 0;
    for (const auto& unit : sys_info.units) {
        CHECK(unit.slots.size() == 4);
        total_slots += static_cast<int>(unit.slots.size());
    }
    CHECK(total_slots == 12);

    // Verify topologies are correct per unit
    for (const auto& ui : unit_infos) {
        if (ui.type == "Box_Turtle") {
            // Box Turtle: empty hubs + multiple extruders = PARALLEL
            CHECK(ui.topology == PathTopology::PARALLEL);
        }
        if (ui.type == "ViViD") {
            // ViViD: hubs present + single extruder = HUB
            CHECK(ui.topology == PathTopology::HUB);
        }
    }
}

TEST_CASE("Partial unit data waits for all units before reorganization", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed 3 string units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "OpenAMS AMS_1", "OpenAMS AMS_2"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane0", "lane1", "lane2", "lane3", "lane4", "lane5", "lane6",
                               "lane7", "lane8", "lane9", "lane10", "lane11"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    REQUIRE(helper.get_unit_infos().size() == 3);

    // Feed unit objects for only 2 of 3 units
    nlohmann::json bt_data;
    bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
    bt_data["extruders"] =
        nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
    bt_data["hubs"] = nlohmann::json::array();
    bt_data["buffers"] = nlohmann::json::array();

    nlohmann::json ams1_data;
    ams1_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
    ams1_data["extruders"] = nlohmann::json::array({"extruder4"});
    ams1_data["hubs"] = nlohmann::json::array({"Hub_1"});
    ams1_data["buffers"] = nlohmann::json::array();

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt_data;
    params["AFC_OpenAMS AMS_1"] = ams1_data;
    helper.feed_status_update(params);

    // With only 2 of 3 units having data, reorganization should NOT happen yet.
    // Premature reorganization drops the last unit's lane data from the stash.
    auto info = helper.get_system_info();
    // Units from initial initialize_slots remain (1 flat unit), not reorganized
    CHECK(info.units.size() == 1);

    // Now feed 3rd unit → reorganization triggers with all units present
    nlohmann::json ams2_data;
    ams2_data["lanes"] = nlohmann::json::array({"lane8", "lane9", "lane10", "lane11"});
    ams2_data["extruders"] = nlohmann::json::array({"extruder5"});
    ams2_data["hubs"] = nlohmann::json::array({"Hub_2"});
    ams2_data["buffers"] = nlohmann::json::array();

    nlohmann::json params2;
    params2["AFC_OpenAMS AMS_2"] = ams2_data;
    helper.feed_status_update(params2);

    // Now all 3 units should be present with all lane data preserved
    info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);
    int total_slots = 0;
    for (const auto& unit : info.units) {
        total_slots += static_cast<int>(unit.slots.size());
    }
    CHECK(total_slots == 12);
}

TEST_CASE("Non-contiguous lane numbering assigns sequential global indices", "[ams][afc][mixed]") {
    AmsBackendAfcTestHelper helper;

    // Lanes 1-8 and 13-16 (gap at 9-12)
    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3",  "lane4",  "lane5",  "lane6",
                                          "lane7", "lane8", "lane13", "lane14", "lane15", "lane16"};
    helper.set_discovered_lanes(all_lanes, {});
    helper.initialize_slots_from_discovery();

    // Verify all 12 lanes are initialized with sequential indices
    REQUIRE(helper.get_slot_count() == 12);

    // The slot registry maps sequential indices 0-11 to the non-contiguous lane names
    CHECK(helper.get_slot_name(0) == "lane1");
    CHECK(helper.get_slot_name(7) == "lane8");
    CHECK(helper.get_slot_name(8) == "lane13"); // Index 8 → lane13 (no gap)
    CHECK(helper.get_slot_name(11) == "lane16");

    // Feed units to trigger reorganization
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"Box_Turtle Turtle_1", "Box_Turtle Turtle_2", "ViViD vivid_1"});
    helper.feed_afc_state(afc_state);

    // Feed unit objects with lane assignments
    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = {
        {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
        {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}},
        {"hubs", nlohmann::json::array()},
        {"buffers", nlohmann::json::array()}};
    params["AFC_BoxTurtle Turtle_2"] = {
        {"lanes", {"lane5", "lane6", "lane7", "lane8"}},
        {"extruders", {"extruder4", "extruder5", "extruder6", "extruder7"}},
        {"hubs", nlohmann::json::array()},
        {"buffers", nlohmann::json::array()}};
    params["AFC_vivid vivid_1"] = {{"lanes", {"lane13", "lane14", "lane15", "lane16"}},
                                   {"extruders", {"extruder"}},
                                   {"hubs", {"vivid_hub"}},
                                   {"buffers", nlohmann::json::array()}};
    helper.feed_status_update(params);

    // After reorganization, global indices should still be sequential 0-11
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);

    // Verify each unit's slots have correct global indices
    int expected_global = 0;
    for (const auto& unit : info.units) {
        for (const auto& slot : unit.slots) {
            CHECK(slot.global_index == expected_global);
            expected_global++;
        }
    }
    CHECK(expected_global == 12);
}

// ============================================================================
// Bug fix: Tool changer current_slot overwrite
// ============================================================================

TEST_CASE("AFC tool changer reconciliation preserves current_load slot",
          "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // In a tool changer, ALL 4 lanes are tool_loaded: true (direct-feed).
    // AFC reports current_load = "lane2" meaning lane2 is active at toolhead.
    // The reconciliation block must NOT overwrite current_slot with lane0.

    // Build a single status update containing BOTH AFC state and lane data
    nlohmann::json params;

    // AFC global state with current_load
    params["AFC"] = {{"current_load", "lane2"}};

    // All 4 lanes report as loaded (tool changer direct-feed)
    for (int i = 0; i < 4; ++i) {
        std::string key = "AFC_stepper lane" + std::to_string(i);
        params[key] = {{"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"},
                       {"material", "PLA"},  {"spool_id", 100 + i}, {"weight", 750}};
    }

    helper.feed_status_update(params);

    // current_slot should be 2 (lane2), not 0 (first loaded lane)
    auto info = helper.get_system_info();
    CHECK(info.current_slot == 2);
    CHECK(info.filament_loaded == true);
}

TEST_CASE("AFC parse_lane_data does not overwrite valid current_slot", "[ams][afc][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // First set current_slot via AFC state (current_load)
    helper.feed_afc_state({{"current_load", "lane2"}});
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Now feed lane data where all lanes are tool_loaded
    nlohmann::json lane_data;
    for (int i = 0; i < 4; ++i) {
        std::string name = "lane" + std::to_string(i);
        lane_data[name] = {{"tool_loaded", true},
                           {"color", "00FF00"},
                           {"material", "PETG"},
                           {"spool_id", 200 + i}};
    }
    helper.feed_afc_state({{"lanes", lane_data}});

    // current_slot should still be 2 (set by current_load), not overwritten
    auto info = helper.get_system_info();
    CHECK(info.current_slot == 2);
}

// ============================================================================
// Bug fix: 3-unit premature reorganize drops last unit's lanes
// ============================================================================

TEST_CASE("AFC 3-unit incremental arrival preserves all unit lanes",
          "[ams][afc][mixed][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(12);
    helper.initialize_slots_from_discovery();

    // Feed AFC state with 3 string-format units
    nlohmann::json afc_state;
    afc_state["units"] =
        nlohmann::json::array({"OpenAMS AMS_1", "OpenAMS AMS_2", "Box_Turtle Turtle_1"});
    afc_state["lanes"] =
        nlohmann::json::array({"lane0", "lane1", "lane2", "lane3", "lane4", "lane5", "lane6",
                               "lane7", "lane8", "lane9", "lane10", "lane11"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    REQUIRE(helper.get_unit_infos().size() == 3);

    // Feed stepper data with color/material for ALL 12 lanes BEFORE unit objects
    for (int i = 0; i < 12; ++i) {
        std::string lane_name = "lane" + std::to_string(i);
        nlohmann::json stepper_data = {
            {"color", "FF" + std::to_string(1000 + i).substr(1)},
            {"material", "PLA"},
            {"spool_id", 300 + i},
            {"weight", 800},
            {"tool_loaded", (i >= 8)} // Turtle lanes are direct-feed
        };
        helper.feed_afc_stepper(lane_name, stepper_data);
    }

    // Now feed unit objects ONE AT A TIME (simulating incremental status notifications)
    // Unit 1: OpenAMS AMS_1
    {
        nlohmann::json ams1_data;
        ams1_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
        ams1_data["extruders"] = nlohmann::json::array({"extruder0"});
        ams1_data["hubs"] = nlohmann::json::array({"Hub_1"});
        ams1_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_OpenAMS AMS_1"] = ams1_data;
        helper.feed_status_update(params);
    }

    // Unit 2: OpenAMS AMS_2
    {
        nlohmann::json ams2_data;
        ams2_data["lanes"] = nlohmann::json::array({"lane4", "lane5", "lane6", "lane7"});
        ams2_data["extruders"] = nlohmann::json::array({"extruder1"});
        ams2_data["hubs"] = nlohmann::json::array({"Hub_2"});
        ams2_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_OpenAMS AMS_2"] = ams2_data;
        helper.feed_status_update(params);
    }

    // Unit 3: Box_Turtle Turtle_1
    {
        nlohmann::json bt_data;
        bt_data["lanes"] = nlohmann::json::array({"lane8", "lane9", "lane10", "lane11"});
        bt_data["extruders"] =
            nlohmann::json::array({"extruder", "extruder2", "extruder3", "extruder4"});
        bt_data["hubs"] = nlohmann::json::array();
        bt_data["buffers"] = nlohmann::json::array();

        nlohmann::json params;
        params["AFC_BoxTurtle Turtle_1"] = bt_data;
        helper.feed_status_update(params);
    }

    // After all 3 units processed, ALL 12 slots should have valid data
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 3);

    int total_slots = 0;
    for (const auto& unit : info.units) {
        total_slots += static_cast<int>(unit.slots.size());
    }
    REQUIRE(total_slots == 12);

    // Specifically verify that Turtle_1's lanes (8-11) have color/material from stepper data
    // Find Turtle unit - after reorganize, units are sorted by min lane number
    bool found_turtle = false;
    for (const auto& unit : info.units) {
        if (unit.name.find("Turtle") != std::string::npos) {
            found_turtle = true;
            for (const auto& slot : unit.slots) {
                CHECK(slot.material != "");
                CHECK(slot.color_rgb != 0x808080); // Not default gray
            }
        }
    }
    CHECK(found_turtle);
}

// ============================================================================
// 4-unit ordering: units sorted by min lane number, not name (#554)
// ============================================================================

TEST_CASE("AFC 4-unit ordering by lane number not unit name", "[ams][afc][mixed][toolchanger]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(16);
    helper.initialize_slots_from_discovery();

    // Feed AFC state: 4 units in Moonraker JSON order (last unit first)
    // This reproduces #554 where unit discovery order caused wrong display
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array(
        {"Box_Turtle Turtle_2", "Box_Turtle Turtle_1", "ACE ace_1", "OpenAMS AMS_1"});
    afc_state["lanes"] = nlohmann::json::array({});
    for (int i = 0; i < 16; ++i)
        afc_state["lanes"].push_back("lane" + std::to_string(i));
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    helper.feed_afc_state(afc_state);

    // Feed stepper data for all 16 lanes
    for (int i = 0; i < 16; ++i) {
        std::string lane = "lane" + std::to_string(i);
        helper.feed_afc_stepper(
            lane, {{"color", "FF0000"}, {"material", "PLA"}, {"spool_id", i}, {"weight", 800}});
    }

    // Feed unit objects mapping lanes to units (deliberately out of physical order).
    // Key format uses Klipper convention: "AFC_" + type (underscores stripped) + " " + name
    auto feed_unit = [&](const std::string& klipper_key, int first_lane, int count) {
        nlohmann::json data;
        data["lanes"] = nlohmann::json::array();
        for (int i = first_lane; i < first_lane + count; ++i)
            data["lanes"].push_back("lane" + std::to_string(i));
        data["extruders"] = nlohmann::json::array({"extruder"});
        data["hubs"] = nlohmann::json::array();
        data["buffers"] = nlohmann::json::array();
        nlohmann::json params;
        params[klipper_key] = data;
        helper.feed_status_update(params);
    };

    // Unit with lanes 12-15 arrives FIRST (the bug scenario from #554)
    feed_unit("AFC_BoxTurtle Turtle_2", 12, 4);
    feed_unit("AFC_BoxTurtle Turtle_1", 0, 4);
    feed_unit("AFC_ACE ace_1", 8, 4);
    feed_unit("AFC_OpenAMS AMS_1", 4, 4);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 4);

    // Units must be ordered by min lane number, NOT alphabetically:
    // - Turtle_1 (lanes 0-3) first
    // - AMS_1 (lanes 4-7) second
    // - ace_1 (lanes 8-11) third
    // - Turtle_2 (lanes 12-15) fourth
    // With alphabetical sort, "ACE ace_1" would wrongly be first
    CHECK(info.units[0].name.find("Turtle_1") != std::string::npos);
    CHECK(info.units[1].name.find("AMS_1") != std::string::npos);
    CHECK(info.units[2].name.find("ace_1") != std::string::npos);
    CHECK(info.units[3].name.find("Turtle_2") != std::string::npos);

    // All 16 lanes should map correctly by global index
    REQUIRE(info.total_slots == 16);
    int total = 0;
    for (const auto& unit : info.units) {
        total += static_cast<int>(unit.slots.size());
    }
    CHECK(total == 16);
}

// ============================================================================
// Reactive lane highlighting & current_slot stability (PR #336 + improvements)
// ============================================================================

TEST_CASE("AFC stepper-only updates do not overwrite current_slot set by AFC state",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(10);
    helper.initialize_slots_from_discovery();

    // Step 1: AFC global state sets current_load = "lane9" (slot 9)
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane9"}};
        params["AFC_stepper lane9"] = {{"status", "Tooled"}, {"tool_loaded", true},
                                       {"color", "00AEFF"},  {"material", "ASA"},
                                       {"spool_id", 42},     {"weight", 800}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 9);

    // Step 2: Incremental update with ONLY AFC_stepper for lane0 — must NOT reset to 0
    {
        nlohmann::json params;
        params["AFC_stepper lane0"] = {
            {"prep", true}, {"load", true}, {"status", "Loaded"}, {"tool_loaded", false}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 9);
    CHECK(info.filament_loaded == true);

    // Step 3: Another stepper-only update for a different lane — still preserved
    {
        nlohmann::json params;
        params["AFC_stepper lane3"] = {
            {"prep", false}, {"load", false}, {"status", "None"}, {"tool_loaded", false}};
        helper.feed_status_update(params);
    }
    CHECK(helper.get_system_info().current_slot == 9);
}

TEST_CASE("AFC carriage authority does not invent filament AFC says is absent",
          "[ams][afc][reconciliation][1229]") {
    // Guard rails on the afc_stated_unloaded suppression. The carriage is the
    // single authority on WHICH tool is current (#1229) and that must not be
    // weakened — it may simply not manufacture a loaded slot on a frame where AFC
    // said the toolhead is empty.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);
    // "Tooled", not "Loaded": upstream's AFCLaneState separates the two
    // (AFC_lane.py:55-65) and only TOOLED / TOOL_LOADED / tool_loaded mean the
    // filament reached a melt zone — "Loaded" is loaded to the HUB
    // (AFC_lane.py:873). The sections below assert filament_loaded, which is a
    // toolhead claim, so the lanes have to actually be at their toolheads.
    // All four at once is legitimate here and not a contrived state: a
    // toolchanger has one melt zone PER toolhead, which is why reconcile's own
    // comment notes several lanes are loaded simultaneously on these machines.
    for (int i = 0; i < 4; ++i) {
        helper.feed_afc_stepper(
            "lane" + std::to_string(i),
            {{"map", "T" + std::to_string(i)}, {"status", "Tooled"}, {"tool_loaded", true}});
    }

    SECTION("a frame silent about current_load still lets the carriage elect") {
        // The authority itself is untouched: no AFC key, so nothing suppresses it.
        nlohmann::json params;
        params["toolchanger"] = {{"tool_number", 2}};
        helper.feed_status_update(params);

        auto info = helper.get_system_info();
        CHECK(info.current_slot == 2);
        CHECK(info.current_tool == 2);
    }

    SECTION("the suppression is frame-scoped and cannot latch") {
        // #1229 was a value latching and never moving again. Suppress on one
        // frame, then elect normally on the next.
        {
            nlohmann::json params;
            params["AFC"] = {{"current_load", nullptr}};
            params["toolchanger"] = {{"tool_number", 2}};
            helper.feed_status_update(params);
        }
        REQUIRE(helper.get_system_info().current_slot == -1);

        {
            nlohmann::json params;
            params["toolchanger"] = {{"tool_number", 2}};
            helper.feed_status_update(params);
        }
        auto info = helper.get_system_info();
        CHECK(info.current_slot == 2); // elects again — the flag did not stick
        CHECK(info.filament_loaded == true);
    }

    SECTION("a named current_load is not mistaken for the unloaded signal") {
        // afc_stated_unloaded must be narrower than current_slot_set_by_afc_state,
        // which a named lane also sets. Reusing that flag would suppress here too.
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane1"}};
        params["toolchanger"] = {{"tool_number", 1}};
        helper.feed_status_update(params);

        auto info = helper.get_system_info();
        CHECK(info.current_slot == 1);
        CHECK(info.filament_loaded == true);
    }
}

TEST_CASE("AFC reconciliation updates current_slot when active lane becomes unloaded",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Step 1: Set current_slot = 2 via AFC state
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane2"}};
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Step 2: Tool change — lane2 unloaded, lane3 loaded (AFC state included)
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane3"}};
        params["AFC_stepper lane2"] = {{"status", "Loaded"}, {"tool_loaded", false}};
        params["AFC_stepper lane3"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "00FF00"}, {"material", "PETG"}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 3);
}

TEST_CASE("AFC reconciliation via stepper-only updates current_slot when no authoritative source",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Step 1: Set current_slot via stepper LOADED status only (no AFC state)
    {
        nlohmann::json params;
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);

    // Step 2: Stepper-only tool change — should detect and update
    {
        nlohmann::json params;
        params["AFC_stepper lane2"] = {{"status", "Loaded"}, {"tool_loaded", false}};
        params["AFC_stepper lane3"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "00FF00"}, {"material", "PETG"}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == 3);
}

TEST_CASE("AFC non-active extruder does not overwrite current_slot", "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(2);

    // Set tool mapping: T0 -> lane0 (slot 0), T1 -> lane1 (slot 1)
    helper.feed_afc_stepper(
        "lane0", {{"map", "T0"}, {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T1"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "00FF00"}});

    // AFC state says current_load = lane0. The tool number is DERIVED from that
    // lane's mapping (lane0 -> slot 0 -> T0); AFC publishes no current_tool key.
    helper.feed_afc_state({{"current_load", "lane0"}});
    REQUIRE(helper.get_system_info().current_slot == 0);
    REQUIRE(helper.get_system_info().current_tool == 0);

    // Non-active extruder1 reports lane_loaded = lane1 — must NOT overwrite current_slot
    helper.feed_afc_extruder(
        "extruder1",
        {{"lane_loaded", "lane1"}, {"tool_start_status", false}, {"tool_end_status", false}});

    auto info2 = helper.get_system_info();
    CHECK(info2.current_slot == 0);
    CHECK(info2.current_tool == 0);
}

TEST_CASE("AFC filament unload (current_load=null) clears state", "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Load filament into lane2
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", "lane2"}};
        params["AFC_stepper lane2"] = {
            {"status", "Tooled"}, {"tool_loaded", true}, {"color", "FF0000"}, {"material", "PLA"}};
        helper.feed_status_update(params);
    }
    REQUIRE(helper.get_system_info().current_slot == 2);
    REQUIRE(helper.get_system_info().filament_loaded == true);

    // AFC reports current_load = null (unloaded)
    helper.feed_afc_state({{"current_load", nullptr}});

    auto info = helper.get_system_info();
    CHECK(info.current_slot == -1);
    CHECK(info.filament_loaded == false);
}

TEST_CASE("AFC tool changer reconciliation derives current_slot from active tool",
          "[ams][afc][reconciliation]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();
    helper.setup_toolchanger(4);

    // Set up tool mapping: T0->slot0, T1->slot1, T2->slot2, T3->slot3
    helper.feed_afc_stepper(
        "lane0",
        {{"map", "T0"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "FF0000"}});
    helper.feed_afc_stepper(
        "lane1",
        {{"map", "T1"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "00FF00"}});
    helper.feed_afc_stepper(
        "lane2",
        {{"map", "T2"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "0000FF"}});
    helper.feed_afc_stepper(
        "lane3",
        {{"map", "T3"}, {"status", "Loaded"}, {"tool_loaded", false}, {"color", "FFFF00"}});

    // Verify tool mapping is populated
    auto mapping = helper.get_tool_to_slot_map();
    REQUIRE(mapping.size() >= 4);

    // Set up PARALLEL topology via unit object
    {
        nlohmann::json params;
        nlohmann::json bt_data;
        bt_data["lanes"] = nlohmann::json::array({"lane0", "lane1", "lane2", "lane3"});
        bt_data["extruders"] =
            nlohmann::json::array({"extruder", "extruder1", "extruder2", "extruder3"});
        bt_data["hubs"] = nlohmann::json::array();
        bt_data["buffers"] = nlohmann::json::array();
        params["AFC_BoxTurtle Turtle_1"] = bt_data;
        helper.feed_status_update(params);
    }

    // Simulate: Klipper reports active tool = T2, current_load is null (mid
    // tool-swap). The tool number comes from Klipper's own toolchanger object,
    // which is where a mid-swap tool number REALLY comes from — AFC.get_status()
    // publishes no "current_tool" key on any version (AFC.py v1.2.0:2531-2564),
    // so the fixture used to prove this with a field the firmware never sends.
    //
    // Both keys land in one frame, the same way Moonraker batches them:
    // parse_afc_state() runs first and clears the tool on the null current_load,
    // then the toolchanger block re-establishes T2. current_slot stays -1
    // because the null current_load is authoritative "unloaded" and sets
    // current_slot_set_by_afc_state, which the reconciliation block honours.
    {
        nlohmann::json params;
        params["AFC"] = {{"current_load", nullptr}};
        params["toolchanger"] = {{"tool_number", 2}};
        helper.feed_status_update(params);
    }

    auto info = helper.get_system_info();
    CHECK(info.current_slot == -1);
    CHECK(info.filament_loaded == false);
    CHECK(info.current_tool == 2);
}

// ---- L1: classify_error ----
static const char* JAM_LINE =
    "!! Toolhead runout detected by tool_end sensor, but upstream sensors still "
    "detect filament. Possible filament break or jam at the toolhead. Please clear "
    "the jam and reload filament manually, then resume the print.";

TEST_CASE("AFC jam with toolhead loaded offers Unload not Eject", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_extruder("extruder", {{"tool_start_status", true}, {"lane_loaded", "lane2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error(JAM_LINE, ctx);

    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
    REQUIRE(e->detail.find("reload filament manually") != std::string::npos); // full text

    auto has = [&](const std::string& label) {
        return std::any_of(e->recovery_actions.begin(), e->recovery_actions.end(),
                           [&](const helix::RecoveryAction& a) { return a.label == label; });
    };
    REQUIRE(has("Resume"));
    REQUIRE(has("Unload")); // toolhead loaded -> Unload (closes R1)
    REQUIRE(has("Recover"));
    REQUIRE_FALSE(has("Eject"));
}

TEST_CASE("AFC jam with empty toolhead offers Eject not Unload", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_extruder("extruder", {{"tool_start_status", false}, {"lane_loaded", "lane2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error(JAM_LINE, ctx);

    REQUIRE(e.has_value());
    auto has = [&](const std::string& label) {
        return std::any_of(e->recovery_actions.begin(), e->recovery_actions.end(),
                           [&](const helix::RecoveryAction& a) { return a.label == label; });
    };
    REQUIRE(has("Resume"));
    REQUIRE(has("Eject")); // empty toolhead -> Eject
    REQUIRE_FALSE(has("Unload"));
    REQUIRE(has("Recover"));
}

// RecoveryModalPresenter refuses to send a needs_hot_nozzle action into a nozzle
// below min_extrude_temp, so the flag decides whether a tapped recovery runs now
// or after a preheat. Getting it wrong is silent in both directions: false on a
// filament-moving action re-creates the cold-extrude failure, true on a
// state-only action makes a recovery wait on heat it does not need.
TEST_CASE("AFC recovery actions flag only the ones that move filament through the nozzle",
          "[ams][afc][classify]") {
    auto flag_of = [](const std::optional<helix::ErrorEvent>& e, const std::string& label) {
        for (const auto& a : e->recovery_actions) {
            if (a.label == label)
                return a.needs_hot_nozzle ? 1 : 0;
        }
        return -1; // absent
    };

    SECTION("toolhead loaded: Resume and Unload need heat, Recover does not") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.feed_afc_extruder("extruder",
                                 {{"tool_start_status", true}, {"lane_loaded", "lane2"}});
        helix::ClassifyContext ctx;
        ctx.is_paused = true;
        auto e = helper.classify_error(JAM_LINE, ctx);
        REQUIRE(e.has_value());

        CHECK(flag_of(e, "Resume") == 1);  // resuming the print extrudes
        CHECK(flag_of(e, "Unload") == 1);  // pulls filament back out of the melt zone
        CHECK(flag_of(e, "Recover") == 0); // AFC_RESET re-preps lanes, no nozzle
    }

    SECTION("empty toolhead: Eject is lane-to-spool and needs no heat") {
        AmsBackendAfcTestHelper helper;
        helper.initialize_test_lanes_with_slots(4);
        helper.feed_afc_extruder("extruder",
                                 {{"tool_start_status", false}, {"lane_loaded", "lane2"}});
        helix::ClassifyContext ctx;
        ctx.is_paused = true;
        auto e = helper.classify_error(JAM_LINE, ctx);
        REQUIRE(e.has_value());

        CHECK(flag_of(e, "Eject") == 0);
        CHECK(flag_of(e, "Resume") == 1);
        CHECK(flag_of(e, "Recover") == 0);
    }
}

TEST_CASE("AFC catch-all: paused + error_state + unknown !! is CRITICAL AFC",
          "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_state({{"error_state", true}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = helper.classify_error("!! Some AFC fault we do not recognize", ctx);

    REQUIRE(e.has_value());
    REQUIRE(e->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(e->source == helix::ErrorSource::AFC);
    REQUIRE_FALSE(e->recovery_actions.empty()); // std actions attached
}

TEST_CASE("AFC defers when not paused and no error_state", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helix::ClassifyContext ctx; // idle, no error_state fed
    REQUIRE_FALSE(helper.classify_error("!! Some AFC fault", ctx).has_value());
}

TEST_CASE("AFC ignores non-error lines", "[ams][afc][classify]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    REQUIRE_FALSE(helper.classify_error("// AFC_Brush: Clean Nozzle", ctx).has_value());
    REQUIRE_FALSE(helper.classify_error("ok", ctx).has_value());
}

TEST_CASE("AFC narration folds purge wording into the poop phase", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    // AFC has exactly one purge in a toolchange: the poop macro. "purge" wording
    // comes out of that same macro ("AFC_Poop: Move To Purge Location"), so it
    // must land on `poop` rather than a phase of its own.
    REQUIRE(afc.match_narration_phase("Purge") == std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("Purging old filament") ==
            std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("AFC_Poop: Move To Purge Location") ==
            std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("Loading lane 2 to hub") ==
            std::optional<std::string>("feed"));
}
TEST_CASE("AFC narration recognizes brush/cut/poop/kick (S2)", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    // AFC_BRUSH is the only wipe. It announces itself as "Clean Nozzle" at the
    // default verbose=1 and "Move to Brush." only at verbose>1, so both spellings
    // have to resolve to the same phase or the step never lights on a stock install.
    REQUIRE(afc.match_narration_phase("AFC_Brush: Clean Nozzle") ==
            std::optional<std::string>("brush"));
    REQUIRE(afc.match_narration_phase("Move to Brush") == std::optional<std::string>("brush"));
    REQUIRE(afc.match_narration_phase("Cutting tip") == std::optional<std::string>("cut"));
    REQUIRE(afc.match_narration_phase("Poop") == std::optional<std::string>("poop"));
    REQUIRE(afc.match_narration_phase("Kick") == std::optional<std::string>("kick"));
    REQUIRE(afc.match_narration_phase("lane 2 is now loaded in toolhead") ==
            std::optional<std::string>("load"));
}
TEST_CASE("AFC narration recognizes the old-filament unload (#1046)",
          "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    // Both templates end/continue on an `unload` step; without a matcher case the
    // final unload step could never highlight (issue #1046 I-1).
    REQUIRE(afc.match_narration_phase("Retracting filament") ==
            std::optional<std::string>("unload"));
    REQUIRE(afc.match_narration_phase("Retract") == std::optional<std::string>("unload"));
    // AFC's actual console line when it pulls the old filament back to its lane.
    REQUIRE(afc.match_narration_phase("Unloading lane1") == std::optional<std::string>("unload"));
    REQUIRE(afc.match_narration_phase("Lane lane1 unload done") ==
            std::optional<std::string>("unload"));
    // Guard the substring trap: "unloading lane1" contains "loading lane", so a
    // naive ordering resolves the unload to `feed` and the bar jumps forward.
    REQUIRE(afc.match_narration_phase("Unloading lane1") != std::optional<std::string>("feed"));
    // ...while the cut macro's own retract wording stays on `cut`.
    REQUIRE(afc.match_narration_phase("AFC_Cut: Retract Filament for Cut") ==
            std::optional<std::string>("cut"));
}
TEST_CASE("AFC narration ignores unrelated lines", "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    REQUIRE_FALSE(afc.match_narration_phase("Klipper state: ready").has_value());
    REQUIRE_FALSE(afc.match_narration_phase("").has_value());
}
TEST_CASE("AFC LOAD_SWAP template mirrors AFC's real CHANGE_TOOL order",
          "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    auto tmpl = afc.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(tmpl.empty());
    auto idx = [&](const std::string& id) {
        for (size_t i = 0; i < tmpl.size(); ++i)
            if (tmpl[i].id == id)
                return static_cast<int>(i);
        return -1;
    };
    // AFC's CHANGE_TOOL is TOOL_UNLOAD(old) then TOOL_LOAD(new):
    //   heat -> cut/form tip -> retract old to lane      (TOOL_UNLOAD)
    //   -> feed new to toolhead -> poop -> wipe -> kick -> wipe (TOOL_LOAD)
    // do_poop_kick_wipe runs *after* load_sequence succeeds, and its wipe runs
    // twice, straddling the kick: AFC.py v1.2.0:1390-1413, and the same sequence
    // inline in TOOL_LOAD at v1.1.0:1417-1440.
    //
    // The template is therefore ordered by FIRST occurrence, which puts brush
    // BEFORE kick. Listing kick first made the published index run 4 -> 6 -> 5
    // on every stock toolchange.
    REQUIRE(idx("heat") == 0);
    REQUIRE(idx("cut") == 1);
    REQUIRE(idx("unload") == 2);
    REQUIRE(idx("feed") == 3);
    REQUIRE(idx("poop") == 4);
    REQUIRE(idx("brush") == 5);
    REQUIRE(idx("kick") == 6);
    REQUIRE(idx("load") == 7);
    // The load-bearing half: brush precedes kick because the FIRST wipe does.
    REQUIRE(idx("brush") < idx("kick"));

    // The purge-to-bucket and the kick happen AFTER the new filament is fed —
    // that is the entire point of the poop, it purges the old colour out through
    // the new filament. Listing them before `feed` was the bug this pins.
    REQUIRE(idx("poop") > idx("feed"));
    REQUIRE(idx("kick") > idx("feed"));

    // AFC has no purge separate from the poop and no nozzle clean separate from
    // the brush. Both phantom steps must stay gone.
    REQUIRE(idx("purge") == -1);
    REQUIRE(idx("clean") == -1);
}

TEST_CASE("AFC LOAD_FRESH and UNLOAD templates match the real sequences",
          "[unit][ams][afc][narration]") {
    AmsBackendAfcTestHelper afc;
    auto ids = [&](StepOperationType op) {
        std::vector<std::string> out;
        for (const auto& p : afc.toolchange_phase_template(op))
            out.push_back(p.id);
        return out;
    };
    // A fresh load is TOOL_LOAD alone: no cut, no unload of a previous lane, but
    // do_poop_kick_wipe still runs afterwards — same poop / wipe / kick / wipe
    // body, so the same first-occurrence order as LOAD_SWAP.
    REQUIRE(ids(StepOperationType::LOAD_FRESH) ==
            std::vector<std::string>{"heat", "feed", "poop", "brush", "kick", "load"});
    // TOOL_UNLOAD alone: heat, cut/form tip, then pull back to the lane.
    REQUIRE(ids(StepOperationType::UNLOAD) == std::vector<std::string>{"heat", "cut", "unload"});
}

TEST_CASE("AFC get_operation_step_model mirrors the narration phase template",
          "[unit][ams][afc][stepmodel]") {
    AmsBackendAfcTestHelper afc;
    // The base AmsBackend default builds the step model from the backend's
    // toolchange_phase_template — narration backends need no separate override.
    auto tmpl = afc.toolchange_phase_template(StepOperationType::LOAD_SWAP);
    auto model = afc.get_operation_step_model(StepOperationType::LOAD_SWAP);
    REQUIRE_FALSE(model.steps.empty());
    REQUIRE(model.steps.size() == tmpl.size());
    for (size_t i = 0; i < tmpl.size(); ++i) {
        CHECK(model.steps[i].label == tmpl[i].label);
        CHECK(model.steps[i].optional == tmpl[i].optional);
    }
    // No step is flagged for a live temperature readout on a narration model.
    for (const auto& s : model.steps) {
        CHECK_FALSE(s.live_temp);
    }
}

TEST_CASE("AFC get_operation_step_index_subject is the narration toolchange-step subject",
          "[unit][ams][afc][stepmodel]") {
    AmsBackendAfcTestHelper afc;
    // The GcodeNarrationRouter drives AmsState's toolchange_step subject; the
    // base default returns it for any operation with a non-empty template.
    CHECK(afc.get_operation_step_index_subject(StepOperationType::LOAD_SWAP) ==
          AmsState::instance().get_toolchange_step_subject());
}

// ============================================================================
// parse_afc_stepper must represent AFC's CLEARS, not just its values
// ============================================================================
//
// AFC clears a lane's identity itself. LANE_UNLOAD ends with
// set_spoolID(lane, None), which (when remember_spool is false, the default)
// runs clear_values(): spool_id=None, material='', color='', weight=0.
//
// Helix could not represent any of that. `spool_id: null` fails
// is_number_integer() and kept the old id; `color: ""` threw inside std::stoul
// and was caught as "keep existing colour"; AFC's SET_COLOR with an empty value
// stores the literal '#', which stripped to "" and threw the same way. Only
// `material: ""` actually cleared. So an ejected lane kept showing the previous
// spool's identity and, worse, kept its Spoolman link — which is what aimed a
// later edit at the wrong spool.
//
// The parser's job is firmware truth, including absence. Retention across an
// eject is a policy decision that belongs one layer up, in the override store.

TEST_CASE("AFC parse: null spool_id clears the Spoolman link", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_stepper("lane1", {{"spool_id", 86}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->spoolman_id == 86);

    // AFC's clear_values() emits spool_id: null
    helper.feed_afc_stepper("lane1", {{"spool_id", nullptr}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->spoolman_id == 0);
}

TEST_CASE("AFC parse: empty colour clears rather than sticking", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_stepper("lane1", {{"color", "#E53935"}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->color_rgb == 0xE53935);

    helper.feed_afc_stepper("lane1", {{"color", ""}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->color_rgb == AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE("AFC parse: bare '#' colour clears (AFC SET_COLOR with empty value)",
          "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_stepper("lane1", {{"color", "#E53935"}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->color_rgb == 0xE53935);

    // SET_COLOR LANE=x COLOR=  ->  cur_lane.color = '#'
    helper.feed_afc_stepper("lane1", {{"color", "#"}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->color_rgb == AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE("AFC parse: a malformed colour still keeps the previous value", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_stepper("lane1", {{"color", "#E53935"}});
    // Garbage is a parse failure, NOT a clear — only empty means cleared.
    helper.feed_afc_stepper("lane1", {{"color", "#zzzzzz"}});
    REQUIRE(helper.get_system_info().get_slot_global(0)->color_rgb == 0xE53935);
}

TEST_CASE("AFC parse: absent fields are retained (deltas, not snapshots)", "[ams][afc][status]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_stepper("lane1", {{"spool_id", 86}, {"material", "ASA"}, {"color", "#E53935"}});

    // A weight-only delta must not disturb identity.
    helper.feed_afc_stepper("lane1", {{"weight", 500.0}});

    // get_system_info() returns by value; hold the snapshot in a named local.
    // Binding the pointer to the temporary let it die at the end of the
    // statement, so every check below read freed memory (ASAN: heap-use-after-free).
    const AmsSystemInfo info = helper.get_system_info();
    const auto* slot = info.get_slot_global(0);
    REQUIRE(slot->spoolman_id == 86);
    REQUIRE(slot->material == "ASA");
    REQUIRE(slot->color_rgb == 0xE53935);
    REQUIRE(slot->remaining_weight_g == Catch::Approx(500.0));
}

// ============================================================================
// can_recover_lane_position — AFC_LANE_RESET has real preconditions
// ============================================================================
//
// Lane-position recovery has no static capability gate, so the "Reset" menu
// entry was once offered on every AFC lane regardless of state. But AFC_LANE_RESET means
// "retract filament from the bowden back to the hub" (AFC_functions.py), and it
// refuses unless the lane's filament is actually at the hub:
//     if not CUR_HUB.state: AFC_error("Hub is already clear while trying to
//                                      reset '<lane>'"); return
// It also refuses while the toolhead is loaded. Offering it on an ejected lane
// produced exactly that error, which then LATCHED in printer.AFC.message and
// kept re-firing error toasts for the rest of the session (seen on the .112
// BoxTurtle). Reported upstream as AFCProject/AFC-Klipper-Add-On#803.

TEST_CASE("AFC can_recover_lane_position requires filament at the hub", "[ams][afc][reset]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();
    helper.set_discovered_lanes({}, {"Turtle_1"}); // so AFC_hub status updates route
    helper.feed_afc_stepper("lane1", {{"hub", "Turtle_1"}, {"load", true}});

    // Attribution comes from AFC.current_lane, which get_status() populates from
    // AFC.current_loading — set at the top of TOOL_LOAD/TOOL_UNLOAD and cleared
    // only by set_loaded()/set_unloaded() on success. A toolchange that strands
    // filament past the hub therefore leaves this naming the guilty lane, which
    // is what makes the attributed case reachable with the toolhead free.
    // Driving it through feed_afc_state() (rather than the setter) proves
    // parse_afc_state() actually wires the field.
    helper.feed_afc_state({{"current_lane", "lane1"}});

    // loaded_to_hub is latched at prep and never updated (see the [recovery]
    // cases above), so this now drives the real signal: AFC_hub.state.
    SECTION("hub sensor triggered, toolhead free, lane attributed -> reset is possible") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        CHECK(helper.can_recover_lane_position(0));
    }

    SECTION("hub sensor clear -> reset refused") {
        helper.feed_afc_hub("Turtle_1", {{"state", false}});
        CHECK_FALSE(helper.can_recover_lane_position(0));
    }

    SECTION("toolhead loaded -> reset refused even with hub sensor triggered") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        helper.feed_afc_state({{"current_load", "lane1"}});
        CHECK_FALSE(helper.can_recover_lane_position(0));
    }

    SECTION("AFC names no lane -> reset refused despite a triggered hub") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        helper.feed_afc_state({{"current_lane", nullptr}});
        CHECK_FALSE(helper.can_recover_lane_position(0));
    }

    SECTION("lane's own load switch clear -> reset refused") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        helper.feed_afc_stepper("lane1", {{"load", false}});
        CHECK_FALSE(helper.can_recover_lane_position(0));
    }

    SECTION("out-of-range slot is never resettable") {
        helper.feed_afc_hub("Turtle_1", {{"state", true}});
        CHECK_FALSE(helper.can_recover_lane_position(99));
        CHECK_FALSE(helper.can_recover_lane_position(-1));
    }
}

// ============================================================================
// Override store — user identity survives AFC's own clears
// ============================================================================
//
// parse_afc_stepper now honours AFC's clears (spool_id null, empty colour), so
// firmware truth genuinely clears on eject. Retention lives one layer up: the
// override store re-supplies the identity the user attached. Without it, the
// parser change alone would LOSE metadata on eject, which is the opposite of
// what the maintainer asked for (a lane keeps its spool until told otherwise,
// because pulling a spool for maintenance and putting the same one back is the
// common case).
//
// AFC firmware also structurally cannot hold brand / spool_name /
// total_weight_g / colour name / filament+vendor ids — verified against a live
// lane payload and its lane_data record — so those only ever live here.

TEST_CASE("AFC override survives an eject that clears firmware fields", "[ams][afc][override]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // User attaches identity to lane 1.
    SlotInfo info;
    info.brand = "Likesilk";
    info.spool_name = "Black ASA";
    info.spoolman_id = 86;
    info.material = "ASA";
    info.color_rgb = 0x1A1A1A;
    info.total_weight_g = 1000.0f;
    helper.set_slot_info(0, info);

    // AFC ejects the lane: clear_values() nulls spool_id and empties
    // colour/material, and parse_afc_stepper now represents that faithfully.
    helper.feed_afc_stepper(
        "lane1", {{"spool_id", nullptr}, {"material", ""}, {"color", ""}, {"status", "None"}});

    const SlotInfo after = helper.get_slot_info(0);

    // Identity the user attached is re-supplied by the override layer.
    CHECK(after.brand == "Likesilk");
    CHECK(after.spool_name == "Black ASA");
    CHECK(after.spoolman_id == 86);
    CHECK(after.total_weight_g == Catch::Approx(1000.0f));
}

TEST_CASE("AFC clear_slot_override drops the retained identity", "[ams][afc][override]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    SlotInfo info;
    info.brand = "Likesilk";
    info.spoolman_id = 86;
    info.material = "ASA";
    helper.set_slot_info(0, info);

    helper.clear_slot_override(0);
    helper.feed_afc_stepper("lane1", {{"spool_id", nullptr}, {"material", ""}});

    const SlotInfo after = helper.get_slot_info(0);
    CHECK(after.brand.empty());
    CHECK(after.spoolman_id == 0);
}

// ============================================================================
// Upstream follow-ups: version in status (#807) and lane vendor_name (#808)
// ============================================================================

TEST_CASE("AFC version is read from the status object when upstream supplies it",
          "[ams][afc][version]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // AFC is moving the version signal out of the dead afc-install DB namespace
    // and into the status object (AFCProject/AFC-Klipper-Add-On PR #807 adds
    // AFC.version to get_status()). Read it where they are putting it.
    helper.feed_afc_state({{"version", "1.2.1"}});
    CHECK(helper.get_system_info().version == "1.2.1");

    // Absent on every release predating #807 — must not clobber what we already
    // have, or the display flips to empty on the next status tick.
    helper.feed_afc_state({{"current_load", "lane1"}});
    CHECK(helper.get_system_info().version == "1.2.1");

    // Equally, an empty string is not a version.
    helper.feed_afc_state({{"version", ""}});
    CHECK(helper.get_system_info().version == "1.2.1");
}

TEST_CASE("AFC version never gates behaviour", "[ams][afc][version]") {
    // AFC_VERSION is a hand-bumped literal that already drifted from the release
    // tag — it sat at 1.1.37 through the whole v1.2.0 release. Presence of the
    // field is the only trustworthy signal, so a comically old version must not
    // disable anything.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    helper.feed_afc_state({{"version", "0.0.1"}});

    // NOTE: parse_lane_data re-initializes the slot registry whenever the payload
    // lane count differs from the current slot count, so a lane_data payload must
    // carry EVERY lane. A single-lane payload silently collapses the registry to
    // one slot and any get_slot_info(i>0) then reads a default-constructed slot.
    nlohmann::json lanes;
    for (int i = 0; i < 4; ++i) {
        lanes["lane" + std::to_string(i)] = {{"color", "#FF5500"}, {"material", "PLA"}};
    }
    helper.feed_afc_state({{"lanes", lanes}});

    auto slot = helper.get_slot_info(0);
    CHECK(slot.color_rgb == 0xFF5500u);
    CHECK(slot.material == "PLA");
}

TEST_CASE("AFC lane_data reads vendor_name for the brand, with brand as fallback (#808)",
          "[ams][afc][lane_data]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Upstream is naming this vendor_name, NOT vendor, to match Happy Hare's
    // mmu_server.py so both backends share one spelling.
    //
    // All four lanes in ONE payload: parse_lane_data re-initializes the registry
    // when the lane count changes, so feeding lanes piecemeal would collapse it
    // to a single slot and make these assertions read default-constructed slots.
    nlohmann::json lanes;
    lanes["lane0"] = {{"brand", "Prusament"}}; // legacy spelling
    lanes["lane1"] = {{"vendor_name", "Polymaker"},
                      {"brand", "Prusament"}};       // both -> upstream wins
    lanes["lane2"] = {{"vendor_name", "Polymaker"}}; // upstream spelling
    lanes["lane3"] = {{"material", "PLA"}};          // neither
    helper.feed_afc_state({{"lanes", lanes}});

    CHECK(helper.get_slot_info(0).brand == "Prusament");
    CHECK(helper.get_slot_info(1).brand == "Polymaker");
    CHECK(helper.get_slot_info(2).brand == "Polymaker");
    CHECK(helper.get_slot_info(3).brand.empty());
}

TEST_CASE("AFC lane_data also reads the `vendor` spelling we write ourselves (#808)",
          "[ams][afc][lane_data]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // `vendor` is the name we originally proposed upstream AND the key our own
    // FilamentSlotOverrideStore::to_lane_data_record() emits alongside vendor_name.
    //
    // For AFC that record currently lands in a PRIVATE namespace (#1158), so this
    // reader does not meet it yet — but it will the moment #1158 migrates AFC's
    // overrides into lane_data proper, and a record written by any other producer
    // using the originally-proposed spelling reads correctly today.
    nlohmann::json lanes;
    lanes["lane0"] = {{"vendor", "Hatchbox"}};
    lanes["lane1"] = {{"vendor_name", "Polymaker"}, {"vendor", "Hatchbox"}}; // upstream wins
    lanes["lane2"] = {{"vendor", "Hatchbox"}, {"brand", "Prusament"}}; // vendor outranks brand
    lanes["lane3"] = {{"material", "PLA"}};
    helper.feed_afc_state({{"lanes", lanes}});

    CHECK(helper.get_slot_info(0).brand == "Hatchbox");
    CHECK(helper.get_slot_info(1).brand == "Polymaker");
    CHECK(helper.get_slot_info(2).brand == "Hatchbox");
    CHECK(helper.get_slot_info(3).brand.empty());
}

TEST_CASE("AFC an empty vendor never clears an existing brand (#808)", "[ams][afc][lane_data]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    nlohmann::json lanes;
    for (int i = 0; i < 4; ++i)
        lanes["lane" + std::to_string(i)] = {{"vendor_name", "Polymaker"}};
    helper.feed_afc_state({{"lanes", lanes}});
    REQUIRE(helper.get_slot_info(0).brand == "Polymaker");

    // #808 is unimplemented, so we do not know whether an unlinked lane will omit
    // the key or publish "". Treating "" as a clear would silently wipe a user's
    // brand override, and parse_lane_data does NOT call apply_overrides() to put it
    // back. Ignoring empties is the recoverable direction.
    for (int i = 0; i < 4; ++i)
        lanes["lane" + std::to_string(i)] = {{"vendor_name", ""}};
    helper.feed_afc_state({{"lanes", lanes}});
    CHECK(helper.get_slot_info(0).brand == "Polymaker");

    // Absent behaves the same as empty.
    for (int i = 0; i < 4; ++i)
        lanes["lane" + std::to_string(i)] = {{"material", "PLA"}};
    helper.feed_afc_state({{"lanes", lanes}});
    CHECK(helper.get_slot_info(0).brand == "Polymaker");
}

TEST_CASE("AFC reads vendor_name from the AFC_stepper status object (#808)", "[ams][afc][vendor]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // Upstream #808 asked for the vendor on BOTH lane_data and get_status(), and
    // jimmyjon711 accepted. The status surface is the one that matters: it is live
    // and present on every AFC version, where lane_data is a DB snapshot that only
    // refreshes when AFC happens to push.
    helper.feed_afc_stepper("lane0", {{"vendor_name", "Polymaker"}});
    CHECK(helper.get_slot_info(0).brand == "Polymaker");

    // Deltas: an update without the key must not clobber what we already have.
    helper.feed_afc_stepper("lane0", {{"material", "PLA"}});
    CHECK(helper.get_slot_info(0).brand == "Polymaker");
}

TEST_CASE("AFC takes no weight from lane_data on any version (#805)", "[ams][afc][lane_data]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_zero_based(4);
    helper.initialize_slots_from_discovery();

    // The AFC_stepper subscription is the sole weight authority — live since v1.1.0.
    helper.feed_afc_stepper("lane0", {{"weight", 750.0}});
    REQUIRE(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(750.0f));

    // Now deliver a lane_data payload the way a v1.2.0 box does: `weight` present
    // but STALE, because cmd_SET_WEIGHT updated the lane object without publishing
    // (AFCProject/AFC-Klipper-Add-On#805, fixed only after v1.2.0 in PR #812). The
    // stale and fixed payloads are byte-identical, so no feature detection can tell
    // them apart — the only safe rule is to never source weight here.
    //
    // `remaining_weight` / `total_weight` are included too: no AFC version ever
    // emitted either key, and readers for them used to sit in parse_lane_data.
    // Deleting those readers must not be undone by someone "restoring" them.
    nlohmann::json lanes;
    for (int i = 0; i < 4; ++i) {
        lanes["lane" + std::to_string(i)] = {
            {"weight", 12.0}, {"remaining_weight", 34.0}, {"total_weight", 56.0}};
    }
    helper.feed_afc_state({{"lanes", lanes}});

    CHECK(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(750.0f));
    CHECK(helper.get_slot_info(0).total_weight_g == Catch::Approx(-1.0f));

    // A cleared lane_data record (post-#812 writes weight: 0) must not zero a live
    // reading either.
    for (int i = 0; i < 4; ++i)
        lanes["lane" + std::to_string(i)] = {{"weight", 0}, {"spool_id", nullptr}};
    helper.feed_afc_state({{"lanes", lanes}});
    CHECK(helper.get_slot_info(0).remaining_weight_g == Catch::Approx(750.0f));
}

TEST_CASE("AFC clears the operation detail when its message empties", "[ams][afc][recovery]") {
    // operation_detail outranks the action- and print-state-derived strings in
    // AmsState::recompute_action_detail(), so a value left behind here pins the
    // AMS sidebar status label to a stale error for the rest of the session.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_running(true);

    helper.test_parse_afc_state(nlohmann::json{
        {"message",
         {{"message", "Hub is already clear while trying to reset 'lane1'"}, {"type", "error"}}}});
    REQUIRE(helper.get_system_info().operation_detail ==
            "Hub is already clear while trying to reset 'lane1'");

    // AFC_CLEAR_MESSAGE lands: the message object empties.
    helper.test_parse_afc_state(nlohmann::json{{"message", {{"message", ""}, {"type", ""}}}});
    REQUIRE(helper.get_system_info().operation_detail.empty());
}

// ============================================================================
// AFC v1.2.0 status fields (prestonbrown/helixscreen#1149)
//
// Payloads marked "observed" are verbatim captures from a live BoxTurtle
// running AFC v1.1.0 (the `spoolman` URL is redacted). That box predates
// v1.2.0, so payloads marked "source-derived" are built from
// AFC_lane.get_status() / AFC_buffer.get_status() / AFC.get_status() at tag
// v1.2.0 instead — same key names, same types, not observed on hardware.
// ============================================================================

// Observed: AFC_stepper lane1, AFC v1.1.0, unlinked lane.
static nlohmann::json observed_lane1_v110() {
    return nlohmann::json{
        {"name", "lane1"},
        {"unit", "Turtle_1"},
        {"hub", "Turtle_1"},
        {"extruder", "extruder"},
        {"buffer", "Turtle_1"},
        {"buffer_status", "Advancing"},
        {"lane", 1},
        {"map", "T0"},
        {"load", true},
        {"prep", true},
        {"tool_loaded", false},
        {"loaded_to_hub", true},
        {"material", "PLA"},
        {"remember_spool", false},
        {"spool_id", nullptr},
        {"color", "#E53935"},
        {"weight", 505.8077510382372},
        {"extruder_temp", nullptr},
        {"runout_lane", nullptr},
        {"filament_status", "Ready"},
        {"filament_status_led", "#00cc00"},
        {"status", "None"},
        {"dist_hub", 194.57},
        {"td1_td", ""},
        {"td1_color", ""},
        {"td1_scan_time", ""},
        {"endstops", "load,hub,tool_start,tool_end,buffer_advance,buffer_trailing"}};
}

TEST_CASE("AFC lane parses the v1.1.0 fields we used to drop", "[ams][afc][status_fields][1149]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_stepper("lane1", observed_lane1_v110());

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.filament_status == "Ready");
    REQUIRE(sensors.filament_status_led == "#00cc00");
    REQUIRE(sensors.endstops == "load,hub,tool_start,tool_end,buffer_advance,buffer_trailing");

    // A Box Turtle has no selector, so AFC omits the key entirely. That must
    // read as "no selector on this lane", not "selector clear".
    REQUIRE(sensors.has_selector == false);
    REQUIRE(sensors.selector == false);

    REQUIRE(helper.get_lane_remember_spool("lane1") == std::optional<bool>{false});
    REQUIRE(helper.get_lane_remember_spool("lane4") == std::nullopt);
}

TEST_CASE("AFC lane parses the v1.2.0-only spool fields", "[ams][afc][status_fields][1149]") {
    // Source-derived: AFC_lane.get_status() at v1.2.0 adds filament_name,
    // multi_color_hexes, initial_weight and bed_temp to the observed shape.
    // multi_color_hexes is a list of BARE hexes — AFC splits Spoolman's
    // comma-joined string and re-prefixes only the first into `color`.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json lane = observed_lane1_v110();
    lane["spool_id"] = 42;
    lane["filament_name"] = "Galaxy Black";
    lane["initial_weight"] = 1000.0;
    lane["bed_temp"] = 60.0;
    lane["multi_color_hexes"] = nlohmann::json::array({"D4AF37", "C0C0C0", "B87333"});
    lane["selector"] = true;

    helper.feed_afc_stepper("lane1", lane);

    auto info = helper.get_system_info();
    const auto& slot = info.units[0].slots[0];
    REQUIRE(slot.spool_name == "Galaxy Black");
    REQUIRE(slot.bed_temp == 60);
    REQUIRE(slot.multi_color_hexes == "#D4AF37,#C0C0C0,#B87333");
    REQUIRE(slot.is_multi_color());
    REQUIRE(slot.total_weight_g == Catch::Approx(1000.0f));

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.has_selector == true);
    REQUIRE(sensors.selector == true);
}

TEST_CASE("AFC initial_weight is ignored on a lane with no Spoolman link",
          "[ams][afc][status_fields][1149]") {
    // espooler_values.full_weight is a CONFIGURED unit constant (typically
    // 1000 g) that every lane reports, empty ones included; Spoolman only
    // overwrites it when a spool is linked. Adopting it ungated would render an
    // ejected lane as "0 / 1000 g" instead of unknown.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json lane = observed_lane1_v110(); // spool_id: null
    lane["initial_weight"] = 1000.0;
    lane["weight"] = 0.0;
    helper.feed_afc_stepper("lane1", lane);

    auto info = helper.get_system_info();
    REQUIRE(info.units[0].slots[0].total_weight_g == Catch::Approx(-1.0f));
    REQUIRE(info.units[0].slots[0].get_remaining_percent() == Catch::Approx(-1.0f));
}

TEST_CASE("AFC lane delta omitting the new fields leaves them intact",
          "[ams][afc][status_fields][1149]") {
    // Moonraker forwards only CHANGED keys, so a frame carrying nothing but a
    // sensor flip must not zero everything parsed from the previous frame.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json lane = observed_lane1_v110();
    lane["spool_id"] = 42;
    lane["filament_name"] = "Galaxy Black";
    lane["initial_weight"] = 750.0;
    lane["bed_temp"] = 60.0;
    lane["multi_color_hexes"] = nlohmann::json::array({"D4AF37", "C0C0C0"});
    lane["selector"] = true;
    helper.feed_afc_stepper("lane1", lane);

    // The delta a real buffer switch produces: one key.
    helper.feed_afc_stepper("lane1", nlohmann::json{{"buffer_status", "Trailing"}});

    auto info = helper.get_system_info();
    const auto& slot = info.units[0].slots[0];
    REQUIRE(slot.spool_name == "Galaxy Black");
    REQUIRE(slot.bed_temp == 60);
    REQUIRE(slot.multi_color_hexes == "#D4AF37,#C0C0C0");
    REQUIRE(slot.total_weight_g == Catch::Approx(750.0f));

    auto sensors = helper.get_lane_sensors(0);
    REQUIRE(sensors.buffer_status == "Trailing");
    REQUIRE(sensors.filament_status_led == "#00cc00");
    REQUIRE(sensors.endstops == "load,hub,tool_start,tool_end,buffer_advance,buffer_trailing");
    REQUIRE(sensors.has_selector == true);
    REQUIRE(sensors.selector == true);
    REQUIRE(helper.get_lane_remember_spool("lane1") == std::optional<bool>{false});
}

TEST_CASE("AFC lane clears filament identity when firmware clears it",
          "[ams][afc][status_fields][1149]") {
    // clear_values() on eject sets filament_name="", multi_color=[] and
    // bed_temp=None. Those are deliberate clears, not missing fields.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json lane = observed_lane1_v110();
    lane["spool_id"] = 42;
    lane["filament_name"] = "Galaxy Black";
    lane["bed_temp"] = 60.0;
    lane["multi_color_hexes"] = nlohmann::json::array({"D4AF37"});
    helper.feed_afc_stepper("lane1", lane);

    helper.feed_afc_stepper("lane1", nlohmann::json{{"filament_name", ""},
                                                    {"bed_temp", nullptr},
                                                    {"multi_color_hexes", nlohmann::json::array()},
                                                    {"spool_id", nullptr}});

    auto info = helper.get_system_info();
    const auto& slot = info.units[0].slots[0];
    REQUIRE(slot.spool_name.empty());
    REQUIRE(slot.bed_temp == 0);
    REQUIRE(slot.multi_color_hexes.empty());
    REQUIRE_FALSE(slot.is_multi_color());
}

TEST_CASE("AFC top-level next_lane resolves, clears and survives deltas",
          "[ams][afc][status_fields][1149]") {
    // Observed on the live box: "next_lane": "lane4" with no toolchange running.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"next_lane", "lane4"}});
    REQUIRE(helper.get_system_info().next_slot == 3);

    // An unrelated delta must not drop the staging.
    helper.feed_afc_state({{"current_toolchange", 12}});
    REQUIRE(helper.get_system_info().next_slot == 3);

    // AFC naming no lane is a real transition to "none".
    helper.feed_afc_state({{"next_lane", nullptr}});
    REQUIRE(helper.get_system_info().next_slot == -1);
}

TEST_CASE("AFC top-level position_saved, spoolman and maps are read",
          "[ams][afc][status_fields][1149]") {
    // Observed shape (spoolman URL redacted); `maps` is source-derived — it is
    // the one top-level key v1.1.0 does not emit.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"position_saved", true},
                           {"spoolman", "http://spoolman.invalid:7912"},
                           {"maps", nlohmann::json::array({"T0", "T1", "T2", "T3"})}});

    auto info = helper.get_system_info();
    REQUIRE(info.position_saved == true);
    REQUIRE(info.spoolman_url == "http://spoolman.invalid:7912");
    REQUIRE(helper.get_afc_tool_cmds() == std::vector<std::string>{"T0", "T1", "T2", "T3"});

    // A later delta that mentions none of them leaves all three alone.
    helper.feed_afc_state({{"led_state", true}});
    info = helper.get_system_info();
    REQUIRE(info.position_saved == true);
    REQUIRE(info.spoolman_url == "http://spoolman.invalid:7912");
    REQUIRE(helper.get_afc_tool_cmds().size() == 4);

    // AFC publishes `false`, not null, when Spoolman is unconfigured — a
    // non-string must not be coerced into the URL.
    helper.feed_afc_state({{"spoolman", false}, {"position_saved", false}});
    info = helper.get_system_info();
    REQUIRE(info.spoolman_url == "http://spoolman.invalid:7912");
    REQUIRE(info.position_saved == false);
}

TEST_CASE("AFC maps cross-check does not override the per-lane tool mapping",
          "[ams][afc][status_fields][1149]") {
    // maps is diagnostic: a lane claiming a T-command AFC never registered still
    // maps, because maps is absent entirely before v1.2.0 and cannot be trusted
    // as an authority.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"maps", nlohmann::json::array({"T0", "T1"})}});
    helper.feed_afc_stepper("lane3", nlohmann::json{{"map", "T7"}});

    REQUIRE(helper.get_slot_mapped_tool(2) == 7);
}

TEST_CASE("AFC buffer parses the observed v1.1.0 frame", "[ams][afc][status_fields][1149]") {
    // Observed: AFC_buffer Turtle_1 on the live box, buffer disabled.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    // AFC_buffer frames are only dispatched for buffers AFC has named.
    helper.feed_afc_state({{"buffers", {"Turtle_1"}}});

    helper.feed_afc_buffer("Turtle_1",
                           nlohmann::json{{"state", "Advancing"},
                                          {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                          {"enabled", false},
                                          {"rotation_distance", nullptr},
                                          {"fault_detection_enabled", false},
                                          {"error_sensitivity", 0},
                                          {"fault_timer", nullptr},
                                          {"distance_to_fault", nullptr}});

    auto info = helper.get_system_info();
    REQUIRE(info.units[0].buffer_health.has_value());
    const auto& bh = info.units[0].buffer_health.value();
    REQUIRE(bh.state == "Advancing");
    REQUIRE(bh.fault_detection_enabled == false);
    // AFC nulls both while the buffer is idle — that maps to the not-reported
    // sentinel, not to zero.
    REQUIRE(bh.rotation_distance == Catch::Approx(-1.0f));
    REQUIRE(bh.fault_timer == Catch::Approx(-1.0f));
    // v1.1.0 emits no multipliers at all.
    REQUIRE(bh.multiplier == Catch::Approx(-1.0f));
    REQUIRE(bh.active_lane.empty());
}

TEST_CASE("AFC buffer parses the v1.2.0 multiplier and active-lane fields",
          "[ams][afc][status_fields][1149]") {
    // Source-derived: AFC_buffer.get_status() at v1.2.0 adds active_lane and the
    // multiplier trio. `multiplier` is _last_multiplier, seeded as an int 1 in
    // the base trigger, so it can arrive as either an int or a float.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    // AFC_buffer frames are only dispatched for buffers AFC has named.
    helper.feed_afc_state({{"buffers", {"Turtle_1"}}});

    helper.feed_afc_buffer("Turtle_1",
                           nlohmann::json{{"state", "Trailing"},
                                          {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                          {"enabled", true},
                                          {"rotation_distance", 22.678},
                                          {"active_lane", "lane2"},
                                          {"multiplier_high", 1.1},
                                          {"multiplier_low", 0.9},
                                          {"multiplier", 1},
                                          {"fault_detection_enabled", true},
                                          {"error_sensitivity", 7},
                                          {"fault_timer", 1.5},
                                          {"distance_to_fault", 25.5}});

    auto info = helper.get_system_info();
    REQUIRE(info.units[0].buffer_health.has_value());
    const auto& bh = info.units[0].buffer_health.value();
    REQUIRE(bh.active_lane == "lane2");
    REQUIRE(bh.rotation_distance == Catch::Approx(22.678f));
    REQUIRE(bh.multiplier == Catch::Approx(1.0f));
    REQUIRE(bh.multiplier_high == Catch::Approx(1.1f));
    REQUIRE(bh.multiplier_low == Catch::Approx(0.9f));
    REQUIRE(bh.fault_timer == Catch::Approx(1.5f));
    REQUIRE(bh.distance_to_fault == Catch::Approx(25.5f));
    REQUIRE(bh.error_sensitivity == Catch::Approx(7.0f));
}

TEST_CASE("AFC buffer delta omitting fields leaves the prior health intact",
          "[ams][afc][status_fields][1149]") {
    // The buffer parser used to build a fresh BufferHealth per frame and assign
    // it wholesale, so a state-only delta wiped the configured multipliers,
    // sensitivity and fault distance. It also carries no `lanes`, so the routing
    // has to come from what an earlier frame said.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    // AFC_buffer frames are only dispatched for buffers AFC has named.
    helper.feed_afc_state({{"buffers", {"Turtle_1"}}});

    helper.feed_afc_buffer("Turtle_1",
                           nlohmann::json{{"state", "Trailing"},
                                          {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                          {"enabled", true},
                                          {"rotation_distance", 22.678},
                                          {"active_lane", "lane2"},
                                          {"multiplier_high", 1.1},
                                          {"multiplier_low", 0.9},
                                          {"multiplier", 1.1},
                                          {"fault_detection_enabled", true},
                                          {"error_sensitivity", 7},
                                          {"fault_timer", 1.5},
                                          {"distance_to_fault", 25.5}});

    helper.feed_afc_buffer("Turtle_1", nlohmann::json{{"state", "Advancing"}});

    auto info = helper.get_system_info();
    REQUIRE(info.units[0].buffer_health.has_value());
    const auto& bh = info.units[0].buffer_health.value();
    REQUIRE(bh.state == "Advancing");
    REQUIRE(bh.active_lane == "lane2");
    REQUIRE(bh.rotation_distance == Catch::Approx(22.678f));
    REQUIRE(bh.multiplier == Catch::Approx(1.1f));
    REQUIRE(bh.multiplier_high == Catch::Approx(1.1f));
    REQUIRE(bh.multiplier_low == Catch::Approx(0.9f));
    REQUIRE(bh.fault_timer == Catch::Approx(1.5f));
    REQUIRE(bh.distance_to_fault == Catch::Approx(25.5f));
    REQUIRE(bh.error_sensitivity == Catch::Approx(7.0f));
    REQUIRE(bh.fault_detection_enabled == true);
}

// ----------------------------------------------------------------------------
// Multi-unit buffer attribution (bundle XGVDYEB5)
//
// Every buffer assertion above is single-unit units[0], which is why two
// separate defects hid here on a five-unit rig:
//   (a) handle_status_update parsed AFC_buffer objects BEFORE the unit-level
//       objects that build the multi-unit layout, so every lane resolved against
//       the synthetic single unit initialize_slots() creates and all five buffers
//       landed on unit 0, overwriting each other;
//   (b) reorganize_slots() rebuilds every AmsUnit from scratch, and buffer_health
//       had no writer other than the AFC_buffer parser — which Moonraker will not
//       run again until a buffer field changes. The rig went blank for three
//       minutes until a FIRMWARE_RESTART forced a full state push.
// ----------------------------------------------------------------------------

namespace {

/// Two BoxTurtles, lane1-4 and lane5-8, one buffer each. Returns a helper with
/// the lanes discovered and AFC's flat state fed, but with the unit objects NOT
/// yet sent — so system_info_ still holds the synthetic single unit.
nlohmann::json two_unit_afc_state() {
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1", "Box_Turtle Turtle_2"});
    afc_state["lanes"] = nlohmann::json::array(
        {"lane1", "lane2", "lane3", "lane4", "lane5", "lane6", "lane7", "lane8"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    afc_state["buffers"] = nlohmann::json::array({"TN1", "TN2"});
    return afc_state;
}

/// The two AFC_BoxTurtle unit objects, which are what drive reorganize_slots().
nlohmann::json two_unit_objects() {
    nlohmann::json bt1;
    bt1["lanes"] = nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"});
    bt1["extruders"] = nlohmann::json::array({"extruder"});
    bt1["hubs"] = nlohmann::json::array({"Turtle_1"});
    bt1["buffers"] = nlohmann::json::array({"TN1"});

    nlohmann::json bt2;
    bt2["lanes"] = nlohmann::json::array({"lane5", "lane6", "lane7", "lane8"});
    bt2["extruders"] = nlohmann::json::array({"extruder"});
    bt2["hubs"] = nlohmann::json::array({"Turtle_2"});
    bt2["buffers"] = nlohmann::json::array({"TN2"});

    nlohmann::json params;
    params["AFC_BoxTurtle Turtle_1"] = bt1;
    params["AFC_BoxTurtle Turtle_2"] = bt2;
    return params;
}

/// The two AFC_buffer objects, distinguishable by state and sensitivity.
nlohmann::json two_buffer_objects() {
    nlohmann::json params;
    params["AFC_buffer TN1"] = nlohmann::json{{"state", "Advancing"},
                                              {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                              {"error_sensitivity", 3},
                                              {"distance_to_fault", 11.0}};
    params["AFC_buffer TN2"] = nlohmann::json{{"state", "Trailing"},
                                              {"lanes", {"lane5", "lane6", "lane7", "lane8"}},
                                              {"error_sensitivity", 7},
                                              {"distance_to_fault", 22.0}};
    return params;
}

void discover_two_units(AmsBackendAfcTestHelper& helper) {
    helper.set_discovered_lanes(
        {"lane1", "lane2", "lane3", "lane4", "lane5", "lane6", "lane7", "lane8"},
        {"Turtle_1", "Turtle_2"});
    helper.initialize_slots_from_discovery();
    helper.feed_afc_state(two_unit_afc_state());
}

} // namespace

TEST_CASE("AFC buffers in the layout-building frame land on their own units",
          "[ams][afc][status_fields][buffer][multiunit]") {
    // Defect (a). The connect frame carries the AFC_buffer objects and the
    // AFC_BoxTurtle objects together. Buffers must be resolved against the units
    // that frame builds, not against the synthetic pre-reorganize unit 0.
    AmsBackendAfcTestHelper helper;
    discover_two_units(helper);

    nlohmann::json params = two_unit_objects();
    const nlohmann::json buffers = two_buffer_objects();
    for (auto it = buffers.begin(); it != buffers.end(); ++it) {
        params[it.key()] = it.value();
    }
    helper.feed_status_update(params);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].buffer_health.has_value());
    REQUIRE(info.units[1].buffer_health.has_value());

    CHECK(info.units[0].buffer_health->state == "Advancing");
    CHECK(info.units[0].buffer_health->error_sensitivity == Catch::Approx(3.0f));
    CHECK(info.units[0].buffer_health->distance_to_fault == Catch::Approx(11.0f));

    CHECK(info.units[1].buffer_health->state == "Trailing");
    CHECK(info.units[1].buffer_health->error_sensitivity == Catch::Approx(7.0f));
    CHECK(info.units[1].buffer_health->distance_to_fault == Catch::Approx(22.0f));
}

TEST_CASE("AFC buffer health survives a later frame that rebuilds the units",
          "[ams][afc][status_fields][buffer][multiunit]") {
    // Defect (b), isolated: the units already exist when the buffers arrive, so
    // the attribution is correct no matter what order the dispatcher uses. What
    // this asserts is that a subsequent unit-object frame — which re-runs
    // reorganize_slots() and default-constructs every AmsUnit — does not blank it.
    AmsBackendAfcTestHelper helper;
    discover_two_units(helper);

    helper.feed_status_update(two_unit_objects());
    helper.feed_status_update(two_buffer_objects());

    auto before = helper.get_system_info();
    REQUIRE(before.units.size() == 2);
    REQUIRE(before.units[0].buffer_health.has_value());
    REQUIRE(before.units[1].buffer_health.has_value());

    // AFC re-sends the unit objects and nothing else. Moonraker forwards only
    // changed keys, so the buffer parser will not run again for as long as the
    // buffers hold still.
    helper.feed_status_update(two_unit_objects());

    auto after = helper.get_system_info();
    REQUIRE(after.units.size() == 2);
    REQUIRE(after.units[0].buffer_health.has_value());
    REQUIRE(after.units[1].buffer_health.has_value());
    CHECK(after.units[0].buffer_health->state == "Advancing");
    CHECK(after.units[0].buffer_health->error_sensitivity == Catch::Approx(3.0f));
    CHECK(after.units[1].buffer_health->state == "Trailing");
    CHECK(after.units[1].buffer_health->error_sensitivity == Catch::Approx(7.0f));
}

TEST_CASE("AFC one buffer's fields do not bleed into another buffer's health",
          "[ams][afc][status_fields][buffer][multiunit]") {
    // The old read-modify-write seeded each buffer's update from the OWNING UNIT's
    // current health, which is only the same thing as "this buffer's last reading"
    // when one buffer owns the unit. In the connect frame, before the layout
    // exists, both buffers resolve to the same unit — so TN2 inherited TN1's value
    // for every field TN2's own frame omitted.
    AmsBackendAfcTestHelper helper;
    discover_two_units(helper);

    nlohmann::json params = two_unit_objects();
    params["AFC_buffer TN1"] = nlohmann::json{{"state", "Advancing"},
                                              {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                              {"error_sensitivity", 3},
                                              {"distance_to_fault", 11.0}};
    // TN2 reports no distance_to_fault at all — fault detection is off on this one.
    params["AFC_buffer TN2"] = nlohmann::json{{"state", "Trailing"},
                                              {"lanes", {"lane5", "lane6", "lane7", "lane8"}},
                                              {"error_sensitivity", 7}};
    helper.feed_status_update(params);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[1].buffer_health.has_value());
    CHECK(info.units[1].buffer_health->state == "Trailing");
    CHECK(info.units[1].buffer_health->error_sensitivity == Catch::Approx(7.0f));
    // The "not reported" sentinel, NOT TN1's 11mm.
    CHECK(info.units[1].buffer_health->distance_to_fault == Catch::Approx(-1.0f));

    REQUIRE(info.units[0].buffer_health.has_value());
    CHECK(info.units[0].buffer_health->error_sensitivity == Catch::Approx(3.0f));
    CHECK(info.units[0].buffer_health->distance_to_fault == Catch::Approx(11.0f));
}

TEST_CASE("AFC full multi-lane v1.2.0 frame lands on every slot",
          "[ams][afc][status_fields][1149]") {
    // Observed four-lane BoxTurtle frame with the v1.2.0-only keys added from
    // AFC_lane.get_status() at tag v1.2.0. Colours, weights, maps, dist_hub and
    // the endstop list are the live values.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    helper.feed_afc_state({{"current_lane", nullptr},
                           {"next_lane", "lane4"},
                           {"current_state", "Idle"},
                           {"current_toolchange", 12},
                           {"number_of_toolchanges", 12},
                           {"spoolman", "http://spoolman.invalid:7912"},
                           {"position_saved", false},
                           {"error_state", false},
                           {"bypass_state", false},
                           {"quiet_mode", false},
                           {"maps", nlohmann::json::array({"T0", "T1", "T2", "T3"})},
                           {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                           {"extruders", {"extruder"}},
                           {"hubs", {"Turtle_1"}},
                           {"buffers", {"Turtle_1"}},
                           {"led_state", true}});

    struct LaneFixture {
        const char* name;
        const char* map;
        const char* color;
        double weight;
        double dist_hub;
        int spool_id;
        const char* filament_name;
        int bed_temp;
    };
    const LaneFixture lanes[] = {
        {"lane1", "T0", "#E53935", 505.8077510382372, 194.57, 42, "Galaxy Black", 60},
        {"lane2", "T1", "#536DFE", 497.0348845693303, 122.73, 43, "Sky Blue", 62},
        {"lane3", "T2", "#43A047", 812.5, 150.0, 44, "Leaf Green", 60},
        {"lane4", "T3", "#FDD835", 233.25, 175.5, 45, "Sunflower", 65},
    };

    for (const auto& lf : lanes) {
        nlohmann::json lane = observed_lane1_v110();
        lane["name"] = lf.name;
        lane["map"] = lf.map;
        lane["color"] = lf.color;
        lane["weight"] = lf.weight;
        lane["dist_hub"] = lf.dist_hub;
        lane["spool_id"] = lf.spool_id;
        lane["filament_name"] = lf.filament_name;
        lane["bed_temp"] = lf.bed_temp;
        lane["initial_weight"] = 1000.0;
        lane["remember_spool"] = true;
        // AFCLaneState.LOADED — "loaded to hub". There is no "Ready" in the
        // lane `status` vocabulary (AFC_lane.py:65-76); that word belongs to the
        // separate filament_status field.
        lane["status"] = "Loaded";
        helper.feed_afc_stepper(lf.name, lane);
    }

    helper.feed_afc_buffer("Turtle_1",
                           nlohmann::json{{"state", "Advancing"},
                                          {"lanes", {"lane1", "lane2", "lane3", "lane4"}},
                                          {"enabled", true},
                                          {"rotation_distance", 22.678},
                                          {"active_lane", nullptr},
                                          {"multiplier_high", 1.1},
                                          {"multiplier_low", 0.9},
                                          {"multiplier", 1.0},
                                          {"fault_detection_enabled", true},
                                          {"error_sensitivity", 7},
                                          {"fault_timer", 1.5},
                                          {"distance_to_fault", nullptr}});

    auto info = helper.get_system_info();
    REQUIRE(info.next_slot == 3);
    REQUIRE(info.spoolman_url == "http://spoolman.invalid:7912");
    REQUIRE(info.position_saved == false);
    REQUIRE(helper.get_afc_tool_cmds().size() == 4);

    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slots.size() == 4);
    for (int i = 0; i < 4; ++i) {
        const auto& slot = info.units[0].slots[i];
        INFO("slot " << i);
        REQUIRE(slot.spool_name == lanes[i].filament_name);
        REQUIRE(slot.bed_temp == lanes[i].bed_temp);
        REQUIRE(slot.spoolman_id == lanes[i].spool_id);
        REQUIRE(slot.total_weight_g == Catch::Approx(1000.0f));
        REQUIRE(slot.remaining_weight_g == Catch::Approx(lanes[i].weight));
        REQUIRE(slot.mapped_tool == i);
        REQUIRE(helper.get_lane_remember_spool(lanes[i].name) == std::optional<bool>{true});
        REQUIRE(helper.get_lane_sensors(i).endstops ==
                "load,hub,tool_start,tool_end,buffer_advance,buffer_trailing");
        REQUIRE(helper.get_lane_sensors(i).filament_status_led == "#00cc00");
    }

    REQUIRE(info.units[0].buffer_health.has_value());
    const auto& bh = info.units[0].buffer_health.value();
    REQUIRE(bh.active_lane.empty());
    REQUIRE(bh.multiplier == Catch::Approx(1.0f));
    REQUIRE(bh.fault_timer == Catch::Approx(1.5f));
    REQUIRE(bh.distance_to_fault == Catch::Approx(-1.0f));
}

// ============================================================================
// Optimistic dispatch + macro-ack resolution (#1183)
// ============================================================================
//
// AFC answers a command it has nothing to do about — "lane3 already loaded" —
// by acking in 4ms without ever entering a toolchange, so current_state never
// leaves "Idle". The UI's completion path keys entirely on an ams_action
// transition, so a no-op produced no notify at all and nothing could end the
// operation. AFC now sets the action optimistically at dispatch and resolves it
// on the macro's own gcode ack, but only while AFC has not taken the operation
// over — forcing IDLE underneath a live toolchange would truncate it.

class AfcDispatchAckHelper : public AmsBackendAfc {
  public:
    AfcDispatchAckHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> lanes{"lane1", "lane2", "lane3", "lane4"};
        initialize_slots(lanes);
        for (int i = 0; i < 4; ++i) {
            auto* entry = slots_.get_mut(i);
            if (entry)
                entry->info.status = SlotStatus::AVAILABLE;
        }
        // check_preconditions() refuses everything while the backend is stopped.
        running_ = true;
        set_event_callback([this](const std::string& event, const std::string&) {
            if (event == EVENT_STATE_CHANGED) {
                action_trace_.push_back(get_current_action());
            }
        });
    }

    // client_ is null, so ensure_homed_then() routes straight here. Capture the
    // completion callback instead of dispatching, so the test decides when the
    // macro "acks".
    AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        pending_acks_.emplace_back(nullptr);
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        sent_.push_back(gcode);
        pending_acks_.push_back(std::move(on_complete));
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return homed;
    }

    bool homed = true;

    /// Whether an optimistic dispatch is still armed and awaiting resolution
    /// -- mirrors ToolChangerTestAccess::has_pending_dispatch(). Reachable
    /// directly here since AfcDispatchAckHelper is already a declared friend
    /// of AmsBackendAfc.
    [[nodiscard]] bool has_pending_dispatch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_dispatch_action_.has_value();
    }

    [[nodiscard]] std::string operation_detail() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return system_info_.operation_detail;
    }

    /// Fire the ack for the Nth dispatched gcode, then drain the UpdateQueue —
    /// the production callback arrives on a background thread and hops to the
    /// main thread via LifetimeToken::defer, so the work lands in the queue, not
    /// inline.
    void ack(size_t index) {
        REQUIRE(index < pending_acks_.size());
        REQUIRE(pending_acks_[index] != nullptr);
        pending_acks_[index]();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Feed an arbitrary AFC-object frame through the real status handler.
    void feed_afc(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    void feed_state(const std::string& current_state) {
        feed_afc(nlohmann::json{{"current_state", current_state}});
    }

    void age_action(std::chrono::seconds elapsed) {
        std::lock_guard<std::mutex> lock(mutex_);
        action_start_time_ = std::chrono::steady_clock::now() - elapsed;
    }

    void mark_filament_loaded(bool loaded) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.filament_loaded = loaded;
    }

    /// Stand in for an `[AFC_Toolchanger …]` section in configfile.settings —
    /// the backend has no client here, so the real query never runs.
    void seed_configfile_toolchanger(bool present) {
        std::lock_guard<std::mutex> lock(mutex_);
        configfile_has_toolchanger_ = present;
    }

    [[nodiscard]] AmsAction action() const {
        return get_current_action();
    }

    [[nodiscard]] bool latched() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timed_out_state_.has_value();
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }

    /// Every action published via EVENT_STATE_CHANGED, in order. This is what
    /// AmsState::sync_from_backend() sees, and the transitions in it are the
    /// only thing the filament panel can complete an operation on.
    [[nodiscard]] const std::vector<AmsAction>& trace() const {
        return action_trace_;
    }

  private:
    std::vector<std::string> sent_;
    std::vector<std::function<void()>> pending_acks_;
    std::vector<AmsAction> action_trace_;
};

namespace {

bool trace_contains(const std::vector<AmsAction>& trace, AmsAction a) {
    return std::find(trace.begin(), trace.end(), a) != trace.end();
}

} // namespace

TEST_CASE("AFC no-op load resolves on the macro ack", "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(2).success());
    REQUIRE(h.sent().size() == 1);
    REQUIRE(h.sent()[0] == "CHANGE_TOOL LANE=lane3");

    // The optimistic set is the transition that makes an operation completable.
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC says "lane3 already loaded", never enters a toolchange and reports
    // nothing at all. The macro acks in 4ms.
    h.ack(0);

    REQUIRE(h.action() == AmsAction::IDLE);
    // Asserting the end value alone would pass without the fix — IDLE is where
    // this started. The busy leg is the load-bearing half.
    REQUIRE(trace_contains(h.trace(), AmsAction::LOADING));
    REQUIRE(h.trace().back() == AmsAction::IDLE);
}

TEST_CASE("AFC declining the pre-load home confirmation fully unwinds the optimistic dispatch "
          "(final-review I2)",
          "[ams][afc][dispatch][homing][confirm]") {
    // dispatch_operation() calls begin_dispatch_locked() (arming
    // pending_dispatch_action_ + operation_detail + the optimistic action)
    // BEFORE ensure_homed_then() ever runs. On decline,
    // AmsBackendAfc::on_home_confirmation_declined() must route through
    // abandon_dispatch() -- the SAME unwind dispatch_operation()'s own
    // `if (!result) abandon_dispatch()` net uses -- not just reset the
    // action to IDLE. A partial unwind leaves pending_dispatch_action_
    // armed, so the next macro ack (or a newer dispatch) resolves against a
    // generation nothing is tracking, and leaves operation_detail stale
    // (the sidebar keeps showing "Loading" under an IDLE action).
    AfcDispatchAckHelper h;
    h.homed = false;

    ScopedHomeConfirmPrompter guard(
        [](std::function<void()>, std::function<void()> cancel) { cancel(); });

    REQUIRE(h.load_filament(2).success());

    CHECK(h.sent().empty());
    CHECK(h.action() == AmsAction::IDLE);
    CHECK_FALSE(h.has_pending_dispatch());
    CHECK(h.operation_detail().empty());

    // Not wedged: a subsequent load still dispatches normally.
    h.homed = true;
    REQUIRE(h.load_filament(1).success());
    REQUIRE(h.sent().size() == 1);
    CHECK(h.sent()[0] == "CHANGE_TOOL LANE=lane2");
    CHECK(h.action() == AmsAction::LOADING);
}

TEST_CASE("AFC unload and tool change dispatch their own actions", "[ams][afc][dispatch][1183]") {
    SECTION("unload") {
        AfcDispatchAckHelper h;
        h.mark_filament_loaded(true);

        REQUIRE(h.unload_filament(1).success());
        REQUIRE(h.sent()[0] == "TOOL_UNLOAD LANE=lane2");
        REQUIRE(h.action() == AmsAction::UNLOADING);

        h.ack(0);
        REQUIRE(h.action() == AmsAction::IDLE);
        REQUIRE(trace_contains(h.trace(), AmsAction::UNLOADING));
    }

    SECTION("tool change") {
        AfcDispatchAckHelper h;

        REQUIRE(h.change_tool(1).success());
        REQUIRE(h.sent()[0] == "T1");
        // SELECTING, not LOADING: AFC's toolchanger states all map to SELECTING
        // and the operation carries the 300s toolchange budget.
        REQUIRE(h.action() == AmsAction::SELECTING);

        h.ack(0);
        REQUIRE(h.action() == AmsAction::IDLE);
        REQUIRE(trace_contains(h.trace(), AmsAction::SELECTING));
    }
}

TEST_CASE("AFC macro ack does not truncate a toolchange AFC took over",
          "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(0).success());
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC entered the toolchange for real and is driving its own state machine.
    h.feed_state("ToolSwap");
    REQUIRE(h.action() == AmsAction::SELECTING);

    // The macro's ack lands while AFC is still cutting. Forcing IDLE here would
    // end the operation in the UI 60 seconds early.
    h.ack(0);
    REQUIRE(h.action() == AmsAction::SELECTING);

    h.feed_state("Cutting");
    REQUIRE(h.action() == AmsAction::CUTTING);

    // AFC's own terminating frame is what resolves it.
    h.feed_state("Idle");
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC Idle re-echo between dispatch and ack still ends the operation",
          "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(2).success());
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC re-echoes the state it was already in. That is NOT AFC taking over —
    // it is the no-op — but the frame itself already produced the LOADING ->
    // IDLE transition the UI needs, so the later ack has nothing left to do.
    h.feed_state("Idle");
    REQUIRE(h.action() == AmsAction::IDLE);
    REQUIRE(trace_contains(h.trace(), AmsAction::LOADING));

    h.ack(0);
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC second dispatch invalidates the first dispatch's pending ack",
          "[ams][afc][dispatch][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(0).success());
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC re-echoes Idle, so the action is no longer busy and the next operation
    // passes check_preconditions() — but the first dispatch's ack is still out
    // there, unfired.
    h.feed_state("Idle");
    REQUIRE(h.action() == AmsAction::IDLE);

    h.mark_filament_loaded(true);
    REQUIRE(h.unload_filament(1).success());
    REQUIRE(h.action() == AmsAction::UNLOADING);

    // The stale ack must not resolve the unload that is now in flight.
    h.ack(0);
    REQUIRE(h.action() == AmsAction::UNLOADING);

    // The current dispatch's own ack still works.
    h.ack(1);
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC optimistic dispatch stamps the stuck-action clock",
          "[ams][afc][dispatch][timeout][1183]") {
    AfcDispatchAckHelper h;

    // Age the clock before dispatching. Without its own stamp the new action
    // would inherit this elapsed time and blow the 180s LOADING budget on its
    // very first poll.
    h.age_action(std::chrono::seconds(1000));
    REQUIRE(h.load_filament(0).success());

    h.age_action(std::chrono::seconds(170));
    (void)h.get_system_info(); // the sidebar's stall-watchdog poll
    REQUIRE(h.action() == AmsAction::LOADING);

    h.age_action(std::chrono::seconds(200));
    (void)h.get_system_info();
    REQUIRE(h.action() == AmsAction::ERROR);
    // A timeout on an action AFC never acknowledged must not latch: the latch
    // keys on last_raw_state_, which here is still AFC's idle token, and would
    // re-force ERROR on every subsequent Idle frame.
    REQUIRE_FALSE(h.latched());
    h.feed_state("Idle");
    REQUIRE(h.action() == AmsAction::IDLE);
}

TEST_CASE("AFC stuck-action timeout still fires for an op AFC took over",
          "[ams][afc][dispatch][timeout][1183]") {
    AfcDispatchAckHelper h;

    REQUIRE(h.load_filament(0).success());
    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::LOADING);

    // AFC reports the same busy state forever and no ack ever arrives.
    h.age_action(std::chrono::seconds(200));
    h.feed_state("Loading");

    REQUIRE(h.action() == AmsAction::ERROR);
    // AFC owns this action, so the latch DOES arm — otherwise the next frame's
    // "Loading" would map straight back to LOADING and flap.
    REQUIRE(h.latched());
    h.feed_state("Loading");
    REQUIRE(h.action() == AmsAction::ERROR);
}

// ============================================================================
// Bypass: AFC answers for itself once the sidebar stops chaining (#1229 defect 6)
// ============================================================================
//
// allows_implicit_chaining() == false means the sidebar sends exactly one
// command and lets AFC refuse. That only helps if AFC actually refuses:
// execute_gcode() is fire-and-forget (returns success before Klipper answers,
// silent=true), so without the precondition in enable_bypass() the pass-through
// would report success and change nothing — a silent lie in place of an
// unrequested unload. These pin the refusal that makes the policy safe.
// The policy half is covered in test_ams_bypass_no_chaining.cpp.

TEST_CASE("AFC enable_bypass refuses while filament is at the toolhead",
          "[ams][afc][bypass][1229]") {
    AmsBackendAfcTestHelper helper;
    helper.set_running(true);
    helper.initialize_test_lanes(4);
    helper.set_supports_bypass(true);
    helper.set_current_slot(1);
    helper.set_filament_loaded(true);
    helper.clear_captured_gcodes();

    AmsError err = helper.enable_bypass();

    REQUIRE(err.result == AmsResult::WRONG_STATE);
    REQUIRE_FALSE(err.user_msg.empty());

    // Nothing may be sent at all — a SET_FILAMENT_SENSOR that Klipper later
    // rejected would still have returned SUCCESS to the caller.
    REQUIRE_FALSE(helper.has_gcode_starting_with("SET_FILAMENT_SENSOR"));
}

TEST_CASE("AFC enable_bypass sends the sensor enable when nothing is loaded",
          "[ams][afc][bypass][1229]") {
    AmsBackendAfcTestHelper helper;
    helper.set_running(true);
    helper.initialize_test_lanes(4);
    helper.set_supports_bypass(true);
    helper.set_current_slot(-1);
    helper.set_filament_loaded(false);
    helper.clear_captured_gcodes();

    AmsError err = helper.enable_bypass();

    REQUIRE(err.result == AmsResult::SUCCESS);
    // The AFC ctor defaults has_hardware_bypass_sensor to true; discovery
    // downgrades it to "virtual_bypass" only when Klipper publishes that object.
    REQUIRE(helper.has_gcode("SET_FILAMENT_SENSOR SENSOR=bypass ENABLE=1"));
}

// ============================================================================
// AFC_SELECT_TOOL is gated on a toolchanger EXISTING, not on the extruder count
// ============================================================================
//
// AFC_SELECT_TOOL is registered by AfcToolchanger.__init__ and nowhere else
// (AFC_Toolchanger.py:47-49), a file that exists only from v1.2.0 and that
// Klipper loads only for an `[AFC_Toolchanger <name>]` section. An IDEX or
// standalone-toolhead machine has several [AFC_extruder] sections and no
// toolchanger: `num_extruders_ > 1` was true there, we emitted the command,
// Klipper answered `// Unknown command:"AFC_SELECT_TOOL"`, and the load simply
// never happened.
//
// Two independent sufficient signals, because neither covers every topology:
// the `[AFC_Toolchanger …]` section in configfile.settings (authoritative — it
// IS the registration condition), and a `Toolchanger` entry in AFC.units. The
// units array misses a toolchanger whose toolheads are all lane-fed, because
// AFC only emits a unit with `len(unit.lanes) > 0` (AFC.py v1.2.0:2554) and
// AFCExtruder.check_lanes() pops the synthetic per-toolhead lane back off the
// unit as soon as a real lane feeds that head (AFC_extruder.py:391-401).

TEST_CASE("AFC toolchanger detection", "[ams][afc][toolchanger][select_tool]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);

    SECTION("nothing reported at all") {
        REQUIRE_FALSE(helper.test_has_toolchanger());
    }

    SECTION("a BoxTurtle is not a toolchanger") {
        helper.feed_afc_state({{"units", {"Box_Turtle Turtle_1"}}});
        REQUIRE(helper.get_unit_infos().size() == 1);
        REQUIRE_FALSE(helper.test_has_toolchanger());
    }

    SECTION("several extruders without a toolchanger is still not a toolchanger") {
        // The IDEX / standalone-toolhead machine. This is the whole defect:
        // extruder COUNT says nothing about whether AFC_SELECT_TOOL exists.
        helper.feed_afc_state({{"units", {"Box_Turtle Turtle_1"}},
                               {"extruders", {"extruder", "extruder1", "extruder2"}}});
        REQUIRE(helper.get_extruder_names().size() == 3);
        REQUIRE_FALSE(helper.test_has_toolchanger());
    }

    SECTION("signal 2: a published Toolchanger unit") {
        helper.feed_afc_state({{"units", {"Toolchanger TC_1"}}});
        REQUIRE(helper.test_has_toolchanger());
    }

    SECTION("signal 2 alongside a lane unit") {
        helper.feed_afc_state({{"units", {"Box_Turtle Turtle_1", "Toolchanger TC_1"}}});
        REQUIRE(helper.test_has_toolchanger());
    }

    SECTION("signal 2: the type is user-overridable, so matching is case-insensitive") {
        helper.feed_afc_state({{"units", {"toolchanger tc"}}});
        REQUIRE(helper.test_has_toolchanger());
    }

    SECTION("signal 1 alone: lane-fed toolchanger publishes NO Toolchanger unit") {
        // A BoxTurtle feeding several heads on a shuttle. Every toolhead has a
        // real lane, so check_lanes() popped each synthetic lane off the
        // Toolchanger unit and AFC's `len(unit.lanes) > 0` filter drops it from
        // the units array entirely. Only configfile knows.
        helper.feed_afc_state(
            {{"units", {"Box_Turtle Turtle_1"}}, {"extruders", {"extruder", "extruder1"}}});
        REQUIRE_FALSE(helper.test_has_toolchanger()); // units alone: invisible
        helper.seed_configfile_toolchanger(true);
        REQUIRE(helper.test_has_toolchanger());
    }

    SECTION("signal 1 stays latched across later status frames") {
        helper.seed_configfile_toolchanger(true);
        helper.feed_afc_state({{"units", {"Box_Turtle Turtle_1"}}});
        REQUIRE(helper.test_has_toolchanger());
    }
}

TEST_CASE("AFC load_filament only uses AFC_SELECT_TOOL on a toolchanger",
          "[ams][afc][toolchanger][select_tool]") {
    SECTION("multi-extruder, NO toolchanger unit -> CHANGE_TOOL") {
        AfcDispatchAckHelper h;
        h.feed_afc({{"units", {"Box_Turtle Turtle_1"}},
                    {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}}});

        REQUIRE(h.load_filament(2).success());
        REQUIRE(h.sent().size() == 1);
        // The regression: this used to be `AFC_SELECT_TOOL TOOL=extruder2`,
        // which Klipper does not know, so nothing loaded.
        REQUIRE(h.sent()[0] == "CHANGE_TOOL LANE=lane3");
    }

    SECTION("toolchanger unit present -> AFC_SELECT_TOOL") {
        AfcDispatchAckHelper h;
        h.feed_afc({{"units", {"Toolchanger TC_1"}},
                    {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}}});

        REQUIRE(h.load_filament(2).success());
        REQUIRE(h.sent().size() == 1);
        REQUIRE(h.sent()[0] == "AFC_SELECT_TOOL TOOL=extruder2");
    }

    SECTION("single extruder, no units -> CHANGE_TOOL") {
        AfcDispatchAckHelper h;
        REQUIRE(h.load_filament(0).success());
        REQUIRE(h.sent()[0] == "CHANGE_TOOL LANE=lane1");
    }

    SECTION("lane-fed toolchanger: no Toolchanger unit, but configfile has the section") {
        // The topology the units array cannot see. Must still tool-select.
        AfcDispatchAckHelper h;
        h.seed_configfile_toolchanger(true);
        h.feed_afc({{"units", {"Box_Turtle Turtle_1"}},
                    {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}}});

        REQUIRE(h.load_filament(2).success());
        REQUIRE(h.sent()[0] == "AFC_SELECT_TOOL TOOL=extruder2");
    }
}

TEST_CASE("AFC change_tool only uses AFC_SELECT_TOOL on a toolchanger",
          "[ams][afc][toolchanger][select_tool]") {
    SECTION("multi-extruder, NO toolchanger unit -> T<n>") {
        AfcDispatchAckHelper h;
        h.feed_afc({{"units", {"Box_Turtle Turtle_1"}},
                    {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}}});

        REQUIRE(h.change_tool(2).success());
        REQUIRE(h.sent()[0] == "T2");
    }

    SECTION("toolchanger unit present -> AFC_SELECT_TOOL") {
        AfcDispatchAckHelper h;
        h.feed_afc({{"units", {"Toolchanger TC_1"}},
                    {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}}});

        REQUIRE(h.change_tool(2).success());
        REQUIRE(h.sent()[0] == "AFC_SELECT_TOOL TOOL=extruder2");
    }

    SECTION("lane-fed toolchanger: configfile section, no Toolchanger unit") {
        AfcDispatchAckHelper h;
        h.seed_configfile_toolchanger(true);
        h.feed_afc({{"units", {"Box_Turtle Turtle_1"}},
                    {"extruders", {"extruder", "extruder1", "extruder2", "extruder3"}}});

        REQUIRE(h.change_tool(2).success());
        REQUIRE(h.sent()[0] == "AFC_SELECT_TOOL TOOL=extruder2");
    }
}

// ============================================================================
// Tool number comes from th_extruder_name, not the AFC_extruder section name
// ============================================================================
//
// AFC derives a toolhead's tool index from `th_extruder_name`
// (AFC_extruder.py:222-223 = `config.get("extruder_name", <section name>)`,
// consumed at AFC_Toolchanger.py:231-232, both v1.2.0). That value is published
// in NO get_status() anywhere — configfile.settings is the only surface that
// carries it. The old code read the SECTION name, threw on anything that was
// not `extruder<N>`, caught the throw and mapped every toolhead to T0.

TEST_CASE("AFC extruder tool index prefers configfile extruder_name",
          "[ams][afc][extruder_tool_index]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);

    SECTION("section name that already carries the index") {
        REQUIRE(helper.tool_index_for("extruder") == 0);
        REQUIRE(helper.tool_index_for("extruder1") == 1);
        REQUIRE(helper.tool_index_for("extruder12") == 12);
    }

    SECTION("configfile resolves a section name that does not") {
        helper.seed_extruder_klipper_names({{"e0", "extruder"}, {"e1", "extruder1"}});
        REQUIRE(helper.tool_index_for("e0") == 0);
        REQUIRE(helper.tool_index_for("e1") == 1);
    }

    SECTION("configfile wins over a misleading section name") {
        // `[AFC_extruder extruder1]` with `extruder_name: extruder3` is legal:
        // AFC indexes on the option, so T3 is the right answer, not T1.
        helper.seed_extruder_klipper_names({{"extruder1", "extruder3"}});
        REQUIRE(helper.tool_index_for("extruder1") == 3);
    }

    SECTION("lookup is case-insensitive — Klipper lowercases section headers") {
        helper.seed_extruder_klipper_names({{"t1", "extruder1"}});
        REQUIRE(helper.tool_index_for("T1") == 1);
    }

    SECTION("unresolvable is -1, never a silent T0") {
        // This is the whole defect: `e1` used to throw inside substr(8), get
        // caught, and become T0 — as did e2, e3 and every other toolhead.
        REQUIRE(helper.tool_index_for("e1") == -1);
        REQUIRE(helper.tool_index_for("e2") == -1);
        REQUIRE(helper.tool_index_for("left_head") == -1);
    }

    SECTION("a nonsense extruder_name falls back to the section name") {
        helper.seed_extruder_klipper_names({{"extruder2", "not_a_tool"}});
        REQUIRE(helper.tool_index_for("extruder2") == 2);
    }
}

TEST_CASE("AFC v1.1.0 resolves tool indices with no configfile answer at all",
          "[ams][afc][extruder_tool_index]") {
    // v1.1.0 has no `extruder_name` option on [AFC_extruder] (th_extruder_name
    // was introduced with the toolchanger in v1.2.0), so Klipper never
    // access-tracks the option and configfile.settings carries no value for it.
    // The section-name fallback is then the ONLY path, and it has to work
    // unaided — this is the majority of installed AFC hardware.
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    SECTION("plain extruder<N> section names resolve without configfile") {
        REQUIRE(helper.tool_index_for("extruder") == 0);
        REQUIRE(helper.tool_index_for("extruder1") == 1);
        REQUIRE(helper.tool_index_for("extruder2") == 2);
    }

    SECTION("and lane attribution works end to end") {
        helper.feed_afc_state({{"extruders", {"extruder", "extruder1"}}});
        // T1 is the tool on the carriage; extruder1 is T1, so its lane wins and
        // extruder (T0) is ignored — the ordering-independence the old
        // substr(8)/catch path destroyed by mapping both to T0.
        helper.feed_status_update(nlohmann::json{{"toolchanger", {{"tool_number", 1}}}});
        REQUIRE(helper.get_system_info().current_tool == 1);

        nlohmann::json frame;
        frame["AFC_extruder extruder"] = {{"lane_loaded", "lane1"}};
        frame["AFC_extruder extruder1"] = {{"lane_loaded", "lane3"}};
        helper.feed_status_update(frame);

        REQUIRE(helper.get_system_info().current_slot == 2); // lane3, not lane1
    }
}

TEST_CASE("AFC unresolvable extruder makes no lane attribution claim",
          "[ams][afc][extruder_tool_index]") {
    AmsBackendAfcTestHelper helper;
    helper.initialize_test_lanes(4);
    helper.initialize_slots_from_discovery();

    // Two toolheads named so the section name carries no index. Before the fix
    // both resolved to T0 and each in turn claimed to be the active tool, so
    // current_slot tracked whichever AFC_extruder frame arrived last.
    // AFC.extruders is what routes an "AFC_extruder <name>" frame to a parser.
    helper.feed_afc_state({{"extruders", {"e0", "e1"}}});
    REQUIRE(helper.get_extruder_names() == std::vector<std::string>{"e0", "e1"});

    helper.feed_afc_extruder("e0", {{"lane_loaded", "lane1"}});
    helper.feed_afc_extruder("e1", {{"lane_loaded", "lane3"}});

    REQUIRE(helper.get_system_info().current_slot == -1);

    // Once configfile resolves the names, attribution works again: e1 is T1,
    // and with current_tool unset the first resolvable claim is honoured.
    helper.seed_extruder_klipper_names({{"e0", "extruder"}, {"e1", "extruder1"}});
    helper.feed_afc_extruder("e1", {{"lane_loaded", "lane3"}});
    REQUIRE(helper.get_system_info().current_slot == 2); // lane3
}
