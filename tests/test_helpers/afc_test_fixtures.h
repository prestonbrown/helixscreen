// tests/test_helpers/afc_test_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What these test files share: their fixtures, helpers and includes.
// test_ams_backend_afc.cpp, test_ams_backend_afc_2.cpp, test_ams_backend_afc_3.cpp,
// test_ams_backend_afc_4.cpp, test_ams_backend_afc_5.cpp, test_ams_backend_afc_6.cpp. Generated
// from the first of those files by split_v3.py, which keeps every case's text and order and moves
// everything else here.
#pragma once

#include "ui_ams_lane_settings.h"
#include "ui_callback_helpers.h"
#include "ui_language_refresh.h"
#include "ui_panel_filament.h"
#include "ui_print_select_detail_view.h"
#include "ui_print_start_controller.h"
#include "ui_test_utils.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "afc_defaults.h"
#include "ams_backend_afc.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_step_model.h"
#include "ams_tool_topology.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "error_event.h"
#include "filament_mapper.h"
#include "filament_op_execute.h"
#include "filament_op_router.h"
#include "filament_slot_override_store.h"
#include "gcode_footer_summary.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "print_start_inputs.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/config_dir_guard.h"
#include "test_helpers/filament_accounting_cases.h"
#include "test_helpers/log_capture.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/print_select_detail_view_test_access.h"
#include "test_helpers/print_start_controller_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_cache_dir.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/update_queue_test_access.h"
#include "tools_used_cache.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::test {

using namespace helix;

} // namespace helix::test

namespace helix {

/**
 * @brief Test helper class providing access to AmsBackendAfc internals
 *
 * This class provides controlled access to private members for unit testing.
 * It does NOT start the backend (no Moonraker connection needed).
 */
class AmsBackendAfcTestHelper : public AmsBackendAfc {
  public:
    // A message frame queues its toast decision for the main thread.
    ~AmsBackendAfcTestHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    AmsBackendAfcTestHelper() : AmsBackendAfc(nullptr, nullptr) {}
    // With a client, for paths that watch the console while a macro runs.
    explicit AmsBackendAfcTestHelper(helix::IMoonrakerClient* client)
        : AmsBackendAfc(nullptr, client) {}

    // Version is display/diagnostics only — nothing gates on it (AFC stopped
    // writing the afc-install namespace in its #451). Kept so tests can assert
    // that behavior is INDEPENDENT of whatever version is reported.
    void set_afc_version(const std::string& version) {
        AfcTestAccess::afc_version(*this) = version;
    }

    // Sensor state setters for compute_filament_segment_unlocked testing
    void set_tool_end_sensor(bool state) {
        AfcTestAccess::tool_end_sensor(*this) = state;
    }
    void set_tool_start_sensor(bool state) {
        AfcTestAccess::tool_start_sensor(*this) = state;
    }
    void set_hub_sensor(const std::string& hub_name, bool state) {
        AfcTestAccess::hub_sensors(*this)[hub_name] = state;
    }

    // Convenience overload for single-hub backward compat in tests
    void set_hub_sensor(bool state) {
        // Set/clear on a default hub name for single-hub tests
        if (state) {
            AfcTestAccess::hub_sensors(*this)["default"] = true;
        } else {
            AfcTestAccess::hub_sensors(*this).clear();
        }
    }

    // Lane → hub routing, as parsed from AFC_stepper.hub ("Turtle_1" or "direct").
    void set_lane_hub_routing(const std::string& lane_name, const std::string& hub_name) {
        AfcTestAccess::lane_hub_routing(*this)[lane_name] = hub_name;
    }

    // Lane AFC currently names as active (AFC.current_load / AFC.current_lane),
    // as tracked by apply_afc_state() — used to attribute a shared hub sensor.
    void set_active_load_lane(const std::string& lane_name) {
        AfcTestAccess::active_load_lane(*this) = lane_name;
    }

    std::string get_active_load_lane() const {
        return AfcTestAccess::active_load_lane(*this);
    }

    // Lane AFC reports as gripped by the extruder (AFC.current_load only), as
    // tracked by apply_afc_state() — distinct from active_load_lane_, which
    // prefers the transient AFC.current_lane.
    void set_toolhead_lane(const std::string& lane_name) {
        AfcTestAccess::toolhead_lane(*this) = lane_name;
    }

    std::string get_toolhead_lane() const {
        return AfcTestAccess::toolhead_lane(*this);
    }

    void set_current_lane(const std::string& lane_name) {
        AfcTestAccess::current_lane_name(*this) = lane_name;
    }

    void initialize_test_lanes(int count) {
        std::vector<std::string> names;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i + 1));
        }
        AfcTestAccess::initialize_slots(*this, names);
    }

    // 0-based lane naming: lane0, lane1, ... lane{N-1} (matches real AFC hardware)
    void initialize_test_lanes_zero_based(int count) {
        std::vector<std::string> names;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i));
        }
        AfcTestAccess::initialize_slots(*this, names);
    }

    void set_lane_prep_sensor(int lane_index, bool state) {
        if (AfcTestAccess::slots(*this).is_valid_index(lane_index)) {
            AfcTestAccess::lane_sensors(*this)[AfcTestAccess::slots(*this).name_of(lane_index)]
                .prep = state;
        }
    }

    void set_lane_load_sensor(int lane_index, bool state) {
        if (AfcTestAccess::slots(*this).is_valid_index(lane_index)) {
            AfcTestAccess::lane_sensors(*this)[AfcTestAccess::slots(*this).name_of(lane_index)]
                .load = state;
        }
    }

    void set_lane_loaded_to_hub(int lane_index, bool state) {
        if (AfcTestAccess::slots(*this).is_valid_index(lane_index)) {
            AfcTestAccess::lane_sensors(*this)[AfcTestAccess::slots(*this).name_of(lane_index)]
                .loaded_to_hub = state;
        }
    }

    // AFC_stepper.extruder — which extruder this lane feeds. Present whether or
    // not the lane is seated, unlike the lane_loaded back-reference.
    void set_lane_extruder(int lane_index, const std::string& extruder_name) {
        auto* entry = AfcTestAccess::slots(*this).get_mut(lane_index);
        if (entry)
            entry->info.extruder_name = extruder_name;
    }

    // Put a whole Spoolman link on the live slot — the shape a linked slot
    // carries. The persisted override record has no filament-id field, so
    // the live slot is the only half of a link a test can seed whole.
    void set_lane_spoolman_link(int lane_index, int spool_id, int filament_id, int vendor_id) {
        auto* entry = AfcTestAccess::slots(*this).get_mut(lane_index);
        if (entry) {
            entry->info.spoolman_id = spool_id;
            entry->info.spoolman_filament_id = filament_id;
            entry->info.spoolman_vendor_id = vendor_id;
            entry->info.spool_name = "Linked Spool";
        }
    }

    // AFC_extruder.lane_loaded — the lane this extruder currently holds.
    void set_extruder_lane_loaded(const std::string& extruder_name, const std::string& lane_name) {
        AfcTestAccess::extruder_sensors(*this)[extruder_name].lane_loaded = lane_name;
    }

    // AFC_extruder.is_standalone (v1.2.0+ only publishes it).
    void report_extruder_standalone(const std::string& extruder_name, bool standalone) {
        AfcTestAccess::tool_states(*this)[extruder_name].is_standalone = standalone;
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
        AfcTestAccess::message_drain_deadline(*this) = std::chrono::steady_clock::now() + offset;
    }

    PathSegment test_compute_filament_segment() const {
        return AfcTestAccess::compute_filament_segment_unlocked(*this);
    }

    void test_parse_afc_state(const nlohmann::json& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string deferred_error_event;
        bool current_slot_set_by_afc_state = false;
        bool afc_stated_unloaded = false;
        AfcTestAccess::apply_afc_state(*this, afc::parse_state(data), deferred_error_event,
                                       current_slot_set_by_afc_state, afc_stated_unloaded);
    }

    // Discovery testing helpers
    int get_slot_count() const {
        return AfcTestAccess::slots(*this).slot_count();
    }

    std::string get_slot_name(int index) const {
        return AfcTestAccess::slots(*this).name_of(index);
    }

    const std::vector<std::string>& get_hub_names() const {
        return AfcTestAccess::hub_names(*this);
    }

    void initialize_slots_from_discovery() {
        // The lane-seeding step of AmsBackendAfc::on_started() (start() itself is
        // final in AmsSubscriptionBackend and cannot be reached without a live
        // subscription). Scaffolding, not a second implementation: the work is
        // done by the production initialize_slots(), and the rest of on_started()
        // is network — an override-store load_blocking(), detect_afc_version()
        // and the lane_data query — none of which a unit test can run.
        // TEST_MIRROR_OK: reproduces on_started()'s two-line reachability guard
        //                 so the shipped initialize_slots() can be called; the
        //                 logic under test is initialize_slots(), which IS
        //                 production.
        if (!AfcTestAccess::discovered_lane_names(*this).empty() &&
            !AfcTestAccess::slots(*this).is_initialized()) {
            AfcTestAccess::initialize_slots(*this, AfcTestAccess::discovered_lane_names(*this));
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
            slot.color_rgb = kAmsDefaultSlotColor;
            unit.slots.push_back(slot);
        }

        system_info_.units.push_back(unit);
        system_info_.total_slots = count;
        AfcTestAccess::slots(*this).initialize("Box Turtle 1", names);

        // Set mapped_tool on registry entries to match unit slot info
        for (int i = 0; i < count; ++i) {
            auto* s = get_mutable_slot(i);
            if (s)
                s->mapped_tool = i;
        }
    }

    SlotInfo* get_mutable_slot(int slot_index) {
        auto* entry = AfcTestAccess::slots(*this).get_mut(slot_index);
        return entry ? &entry->info : nullptr;
    }

    // Initialize endless spool configs for reset testing
    void initialize_endless_spool_configs(int count) {
        for (int i = 0; i < count; ++i) {
            AfcTestAccess::slots(*this).set_backup(i, -1);
        }
    }

    // Set a specific endless spool backup for testing
    void set_endless_spool_config(int slot, int backup) {
        AfcTestAccess::slots(*this).set_backup(slot, backup);
    }

    // Set up multi-unit configuration and trigger reorganize
    void
    setup_multi_unit(const std::unordered_map<std::string, std::vector<std::string>>& unit_map) {
        AfcTestAccess::unit_lane_map(*this) = unit_map;
        AfcTestAccess::reorganize_slots(*this);
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
    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        captured_gcodes.push_back(gcode);
        pending_macro_ack = std::move(on_complete);
        return AmsErrorHelper::success();
    }

    helix::SharedCallback<void()> pending_macro_ack;

    // SET_COLOR/SET_MATERIAL dispatch through the error-callback form so a
    // refused macro can cancel its echo guard. Capture it like the others.
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/ = true) override {
        captured_gcodes.push_back(gcode);
        pending_gcode_complete = std::move(on_complete);
        pending_gcode_error = std::move(on_error);
        return AmsErrorHelper::success();
    }

    helix::SharedCallback<void()> pending_gcode_complete;
    helix::SharedCallback<void(const MoonrakerError&)> pending_gcode_error;

    // Override execute_gcode_notify to capture commands (avoids real API call)
    AmsError execute_gcode_notify(const std::string& gcode, const std::string& /*success_msg*/,
                                  const std::string& /*error_prefix*/,
                                  helix::SharedCallback<void()> on_success = nullptr) override {
        captured_gcodes.push_back(gcode);
        pending_notify_success = std::move(on_success);
        return AmsErrorHelper::success();
    }

    // The last execute_gcode_notify() success continuation; a test fires it to
    // stand for the firmware accepting the command.
    helix::SharedCallback<void()> pending_notify_success;
    void accept_notify_gcode() {
        REQUIRE(pending_notify_success);
        pending_notify_success();
        helix::ui::UpdateQueue::instance().drain();
    }

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
        std::lock_guard<std::mutex> lock(AfcTestAccess::eject_queue_mutex(*this));
        AfcTestAccess::pending_eject_lanes(*this).push_back(lane_name);
    }

    int test_pending_eject_count() {
        std::lock_guard<std::mutex> lock(AfcTestAccess::eject_queue_mutex(*this));
        return static_cast<int>(AfcTestAccess::pending_eject_lanes(*this).size());
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

    static constexpr int kMessageDrainMaxClears = AfcTestAccess::message_drain_max_clears();

    void test_maybe_drain_message_queue() {
        AfcTestAccess::maybe_drain_message_queue(*this);
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
        const auto* entry = AfcTestAccess::slots(*this).get(slot_index);
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
    helix::AfcLaneSensors get_lane_sensors(int index) const {
        const auto& map = AfcTestAccess::lane_sensors(*this);
        auto it = map.find(AfcTestAccess::slots(*this).name_of(index));
        return it != map.end() ? it->second : helix::AfcLaneSensors{};
    }
    bool get_hub_sensor() const {
        // Returns true if any hub sensor is triggered (backward compat)
        for (const auto& [name, triggered] : AfcTestAccess::hub_sensors(*this)) {
            if (triggered)
                return true;
        }
        return false;
    }

    bool get_hub_sensor(const std::string& hub_name) const {
        auto it = AfcTestAccess::hub_sensors(*this).find(hub_name);
        return it != AfcTestAccess::hub_sensors(*this).end() && it->second;
    }

    const std::unordered_map<std::string, bool>& get_hub_sensors() const {
        return AfcTestAccess::hub_sensors(*this);
    }
    bool get_tool_start_sensor() const {
        return AfcTestAccess::tool_start_sensor(*this);
    }
    bool get_tool_end_sensor() const {
        return AfcTestAccess::tool_end_sensor(*this);
    }
    bool get_quiet_mode() const {
        return AfcTestAccess::afc_quiet_mode(*this);
    }
    // AFC.maps — the T-commands AFC registered with Klipper (v1.2.0+)
    const std::vector<std::string>& get_afc_tool_cmds() const {
        return AfcTestAccess::afc_tool_cmds(*this);
    }
    // Per-lane AFC_stepper.remember_spool. nullopt = never reported for this lane.
    std::optional<bool> get_lane_remember_spool(const std::string& lane_name) const {
        auto it = AfcTestAccess::lane_remember_spool(*this).find(lane_name);
        if (it == AfcTestAccess::lane_remember_spool(*this).end()) {
            return std::nullopt;
        }
        return it->second;
    }
    bool get_led_state() const {
        return AfcTestAccess::afc_led_state(*this);
    }
    float get_bowden_length() const {
        return AfcTestAccess::bowden_length(*this);
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

    // Mixed topology accessors
    const std::vector<AfcUnitInfo>& get_unit_infos() const {
        return AfcTestAccess::unit_infos(*this);
    }

    const std::vector<std::string>& get_extruder_names() const {
        return AfcTestAccess::extruder_names(*this);
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
        AfcTestAccess::extruder_klipper_names(*this) = section_to_klipper;
        AfcTestAccess::configfile_answered(*this) = true; // stands in for the query having landed
        AfcTestAccess::extruder_tool_index_warned(*this).clear();
    }

    int tool_index_for(const std::string& ext_name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return AfcTestAccess::tool_index_for_extruder_unlocked(*this, ext_name);
    }

    /// N extruders, as AFC.extruders reports them on a multi-head machine.
    void setup_toolchanger(int num_extruders) {
        AfcTestAccess::num_extruders(*this) = num_extruders;
        AfcTestAccess::extruders(*this).clear();
        AfcTestAccess::extruder_names(*this).clear();
        for (int i = 0; i < num_extruders; ++i) {
            std::string name = (i == 0) ? "extruder" : "extruder" + std::to_string(i);
            AfcExtruderInfo ext;
            ext.name = name;
            AfcTestAccess::extruders(*this).push_back(std::move(ext));
            AfcTestAccess::extruder_names(*this).push_back(std::move(name));
        }
    }
};
} // namespace helix

namespace helix::test {

struct AfcEditedLaneFixture : HelixTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::optional<MoonrakerAPIMock> api;
    AmsBackendAfcTestHelper helper;

    AfcEditedLaneFixture() {
        state.init_subjects(false);
        api.emplace(client, state);
        helper.initialize_test_lanes_with_slots(4);
        AfcTestAccess::override_store(helper) =
            std::make_unique<helix::ams::FilamentSlotOverrideStore>(&*api, "afc");

        SlotInfo edit = helper.get_slot_info(0);
        edit.brand = "Polymaker";
        edit.material = "PETG";
        edit.color_rgb = 0x1E5AA8;
        REQUIRE(helix::test::apply_edit(helper, 0, edit).success());

        helper.clear_captured_gcodes();
    }

    [[nodiscard]] helix::ams::FilamentSlotOverride record() {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(helper));
        const auto& overrides = AfcTestAccess::overrides(helper);
        const auto kept = overrides.find(0);
        REQUIRE(kept != overrides.end());
        return kept->second;
    }
};

inline void feed_lane4_stands_in_for_lane2(AmsBackendAfcTestHelper& helper,
                                           const std::string& lane4_tool = "T1") {
    helper.initialize_test_lanes_with_slots(4);
    helper.feed_afc_stepper("lane1", {{"map", "T0"}, {"prep", true}});
    helper.feed_afc_stepper("lane2", {{"map", "T3"}, {"prep", false}, {"load", false}});
    helper.feed_afc_stepper("lane3", {{"map", "T2"}, {"prep", true}});
    helper.feed_afc_stepper("lane4", {{"map", lane4_tool}, {"prep", true}});
    helix::ui::UpdateQueue::instance().drain();
}

inline helix::GcodeToolInfo tool_of_unknown_color(int tool_index) {
    helix::GcodeToolInfo t{};
    t.tool_index = tool_index;
    t.color_rgb = 0x808080;
    t.color_known = false;
    return t;
}

inline std::vector<std::string> set_map_commands(const std::vector<std::string>& gcodes) {
    std::vector<std::string> out;
    std::copy_if(gcodes.begin(), gcodes.end(), std::back_inserter(out),
                 [](const std::string& g) { return g.rfind("SET_MAP", 0) == 0; });
    return out;
}

// A PrintStartController wired to a registered AFC backend, with the detail
// view holding the mapping the print select panel would seed for @p tools.
struct AfcPrintStartHarness {
    LVGLTestFixture lvgl;
    ConfigDirGuard config_dir{"afc_print_start_remap"};
    helix::test::RegisteredBackend<AmsBackendAfcTestHelper> reg;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;
    MoonrakerAPIMock api{client, ps};
    helix::ui::PrintStartController controller{ps, &api};
    helix::ui::PrintSelectDetailView view;
    std::vector<helix::GcodeToolInfo> tools;

    explicit AfcPrintStartHarness(std::vector<helix::GcodeToolInfo> used,
                                  const std::string& lane4_tool = "T1")
        : tools(std::move(used)) {
        ps.init_subjects(false);
        feed_lane4_stands_in_for_lane2(*reg, lane4_tool);
        controller.set_detail_view(&view);
        auto& ams = AmsState::instance();
        view.set_filament_mappings(
            ams.seed_tool_mappings(tools, ams.collect_available_slots(), false));
        reg->captured_gcodes.clear();
    }

    ~AfcPrintStartHarness() {
        controller.set_detail_view(nullptr);
    }

    helix::PreflightResult preflight() const {
        return helix::PreflightValidator::validate(
            tools, AmsState::instance().collect_available_slots(), view.get_filament_mappings(),
            AmsState::instance().collect_firmware_routing(), false, false);
    }
};

struct AfcOpenDetailFixture : LVGLUITestFixture {
    helix::test::ScopedCacheDir cache_dir;
    ConfigDirGuard config_dir{"afc_open_detail"};
    bool prev_auto_color = false;
    std::optional<helix::test::RegisteredBackend<AmsBackendAfcTestHelper>> reg;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;
    MoonrakerAPIMock api{client, ps};
    std::optional<helix::ui::PrintStartController> controller;
    helix::ui::PrintSelectDetailView view;

    AfcOpenDetailFixture() {
        const auto noop = [](lv_event_t*) {};
        register_xml_callbacks({
            {"on_print_select_detail_backdrop", noop},
            {"on_print_select_print_button", noop},
            {"on_print_select_delete_button", noop},
            {"on_print_select_more_button", noop},
            {"on_print_detail_back_clicked", noop},
            {"on_toggle_sliced_colors", noop},
            {"on_print_select_detail_objects", noop},
        });
        helix::SettingsManager::instance().init_subjects();
        prev_auto_color = helix::SettingsManager::instance().get_auto_color_map();
        AmsState::instance().init_subjects(true);
        ps.init_subjects(false);
        reg.emplace();
        (*reg)->initialize_test_lanes_with_slots(4);
        controller.emplace(ps, &api);
    }

    ~AfcOpenDetailFixture() {
        controller->set_detail_view(nullptr);
        view.hide();
        drain();
        lv_timer_handler();
        helix::SettingsManager::instance().set_auto_color_map(prev_auto_color);
        drain();
    }

    AmsBackendAfcTestHelper& afc() {
        return **reg;
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Lane N runs T N; @p colors, when given, loads lane N with colors[N].
    void feed_lanes(const std::vector<std::string>& colors = {}) {
        for (int i = 0; i < 4; ++i) {
            nlohmann::json lane = {{"map", "T" + std::to_string(i)}, {"prep", true}};
            if (static_cast<size_t>(i) < colors.size()) {
                lane["color"] = colors[static_cast<size_t>(i)];
            }
            afc().feed_afc_stepper("lane" + std::to_string(i + 1), lane);
        }
        drain();
    }

    /// The user puts T1 on lane4 in the remap modal.
    void pick_t1_on_lane4() {
        auto mappings = view.get_filament_mappings();
        REQUIRE(mappings.size() == 2);
        mappings[1].mapped_slot = 3;
        mappings[1].user_picked = true;
        view.set_filament_mappings(mappings);
    }

    void check_t1_pick_kept() {
        const auto mappings = view.get_filament_mappings();
        REQUIRE(mappings.size() == 2);
        CHECK(mappings[1].mapped_slot == 3);
        CHECK(mappings[1].user_picked);
    }

    /// Open a file using T0 and T1, sliced in @p palette.
    void open(const std::vector<std::string>& palette) {
        view.init_subjects();
        REQUIRE(view.create(test_screen()) != nullptr);
        constexpr size_t kSize = 4242;
        constexpr time_t kMtime = 4242;
        helix::tools_used_cache().store("remap/follow.gcode", {kSize, kMtime}, {0, 1});
        view.show("follow.gcode", "remap", "PLA", palette, {"PLA", "PLA"}, kSize, kMtime);
        controller->set_detail_view(&view);
        drain();
    }

    void set_auto_color(bool on) {
        helix::SettingsManager::instance().set_auto_color_map(on);
        drain();
    }
};

inline bool has_action(const std::vector<helix::printer::DeviceAction>& actions,
                       const std::string& id) {
    for (const auto& a : actions) {
        if (a.id == id)
            return true;
    }
    return false;
}

struct AfcFourLanes {
    AmsBackendAfcTestHelper helper;
    AfcFourLanes() {
        helper.initialize_test_lanes(4);
        helper.initialize_slots_from_discovery();
    }
};

struct AfcFourZeroBasedLanes {
    AmsBackendAfcTestHelper helper;
    AfcFourZeroBasedLanes() {
        helper.initialize_test_lanes_zero_based(4);
        helper.initialize_slots_from_discovery();
    }
};

struct AfcFourRunningLanes {
    AmsBackendAfcTestHelper helper;
    AfcFourRunningLanes() {
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
    }
};

inline void discover_afc(AmsBackendAfcTestHelper& helper, bool afclite) {
    nlohmann::json objects = nlohmann::json::array({"AFC", "AFC_unit IFS", "extruder"});
    if (afclite) {
        objects.push_back("zmod_afclite");
    }
    helix::PrinterDiscovery hw;
    hw.parse_objects(objects);
    helper.set_discovery(hw);
}

struct StaleFaultRig {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAfcTestHelper helper{&client};

    StaleFaultRig() {
        helper.initialize_test_lanes_with_slots(4);
        helper.set_running(true);
        // As captured from the Voron after the reset: still Error, lane3 named.
        helper.feed_afc_state(
            {{"current_state", "Error"}, {"error_state", true}, {"current_lane", "lane3"}});
        helper.clear_captured_gcodes();
    }

    // Exactly what notify_gcode_response carries: respond_info lines arrive as
    // "// <text>", AFC errors as "!! <text>".
    void console(const std::string& line) {
        client.fire_notification(
            "notify_gcode_response",
            {{"method", "notify_gcode_response"}, {"params", nlohmann::json::array({line})}});
    }

    bool console_watched() const {
        auto it = client.method_callbacks.find("notify_gcode_response");
        return it != client.method_callbacks.end() && !it->second.empty();
    }
};

inline void setup_interleaved_toolchanger(AmsBackendAfcTestHelper& helper) {
    helper.set_discovered_lanes({"lane1", "lane2", "lane3", "lane4", "e0", "e1", "e2"}, {});
    helper.initialize_slots_from_discovery();
    helper.feed_afc_state({{"units", {"Toolchanger Tools", "ViViD Vivid_1"}},
                           {"extruders", {"e0", "e1", "e2", "e3"}}});
    const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> lanes{
        {"lane1", {"T0", "e3"}}, {"lane2", {"T4", "e3"}}, {"lane3", {"T2", "e3"}},
        {"lane4", {"T6", "e3"}}, {"e0", {"T1", "e0"}},    {"e1", {"T3", "e1"}},
        {"e2", {"T5", "e2"}}};
    for (const auto& [lane, map_ext] : lanes) {
        helper.feed_afc_stepper(
            lane,
            {{"map", map_ext.first}, {"extruder", map_ext.second}, {"prep", true}, {"load", true}});
    }
    helper.set_running(true);
}

struct AfcBypassOn : AfcFourRunningLanes {
    enum class Head { NoSensor, Empty, Full };

    void stage(bool hardware_switch, Head head) {
        helper.set_supports_bypass(true);
        helper.set_discovered_sensors({hardware_switch ? "filament_switch_sensor bypass"
                                                       : "filament_switch_sensor virtual_bypass"});
        if (head != Head::NoSensor) {
            AfcTestAccess::apply_configfile_topology(
                helper, AmsBackendAfc::parse_configfile_topology(
                            {{"result",
                              {{"status",
                                {{"configfile",
                                  {{"settings",
                                    {{"afc_extruder extruder",
                                      {{"pin_tool_start", "sht36:PA1"}}}}}}}}}}}}));
            helper.feed_afc_extruder("extruder", {{"tool_start_status", head == Head::Full}});
        }
        helper.feed_afc_state({{"bypass_state", true}});
        helper.captured_gcodes.clear();
    }

    /// A lane tap planned the way the Load button plans it.
    helix::ui::FilamentOpPlan plan_lane_tap(int slot) {
        AmsSystemInfo info;
        const helix::ui::BackendCaps caps = helix::ui::read_backend_caps(&helper, info, slot);
        return helix::ui::plan_load(info, caps, slot, /*macro_available=*/true,
                                    /*macro_user_configured=*/false);
    }

    /// What the planner's own BypassLoaded refusal tells the user to do.
    static std::string remove_spool_advice() {
        helix::ui::FilamentOpPlan bypass_loaded;
        bypass_loaded.refusal = helix::ui::FilamentRefusal::BypassLoaded;
        return helix::ui::panel_load_outcome(bypass_loaded).toast;
    }
};

static const char* kJamLine =
    "!! Toolhead runout detected by tool_end sensor, but upstream sensors still "
    "detect filament. Possible filament break or jam at the toolhead. Please clear "
    "the jam and reload filament manually, then resume the print.";

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

inline nlohmann::json two_unit_afc_state() {
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array({"Box_Turtle Turtle_1", "Box_Turtle Turtle_2"});
    afc_state["lanes"] = nlohmann::json::array(
        {"lane1", "lane2", "lane3", "lane4", "lane5", "lane6", "lane7", "lane8"});
    afc_state["extruders"] = nlohmann::json::array({"extruder"});
    afc_state["buffers"] = nlohmann::json::array({"TN1", "TN2"});
    return afc_state;
}

/// The two AFC_BoxTurtle unit objects, which are what drive reorganize_slots().
inline nlohmann::json two_unit_objects() {
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
inline nlohmann::json two_buffer_objects() {
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

inline void discover_two_units(AmsBackendAfcTestHelper& helper) {
    helper.set_discovered_lanes(
        {"lane1", "lane2", "lane3", "lane4", "lane5", "lane6", "lane7", "lane8"},
        {"Turtle_1", "Turtle_2"});
    helper.initialize_slots_from_discovery();
    helper.feed_afc_state(two_unit_afc_state());
}

} // namespace helix::test

namespace helix {

class AfcDispatchAckHelper : public AmsBackendAfc {
  public:
    AfcDispatchAckHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> lanes{"lane1", "lane2", "lane3", "lane4"};
        AfcTestAccess::initialize_slots(*this, lanes);
        for (int i = 0; i < 4; ++i) {
            auto* entry = AfcTestAccess::slots(*this).get_mut(i);
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
        if (fail_g28 && gcode == "G28") {
            return AmsErrorHelper::not_connected("homing failed");
        }
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        sent_.push_back(gcode);
        pending_acks_.push_back(std::move(on_complete));
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return homed;
    }

    bool homed = true;
    /// The G28 a dispatch sends ahead of its payload fails.
    bool fail_g28 = false;

    /// Whether an optimistic dispatch is still armed and awaiting resolution
    /// -- mirrors ToolChangerTestAccess::has_pending_dispatch(). Reachable
    /// directly here since AfcDispatchAckHelper is already a declared friend
    /// of AmsBackendAfc.
    [[nodiscard]] bool has_pending_dispatch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return AfcTestAccess::pending_dispatch_action(*this).has_value();
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
        AfcTestAccess::action_start_time(*this) = std::chrono::steady_clock::now() - elapsed;
    }

    void mark_filament_loaded(bool loaded) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.filament_loaded = loaded;
    }

    [[nodiscard]] AmsAction action() const {
        return get_current_action();
    }

    [[nodiscard]] bool latched() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return AfcTestAccess::timed_out_state(*this).has_value();
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
    std::vector<helix::SharedCallback<void()>> pending_acks_;
    std::vector<AmsAction> action_trace_;
};
} // namespace helix

namespace helix::test {

inline bool trace_contains(const std::vector<AmsAction>& trace, AmsAction a) {
    return std::find(trace.begin(), trace.end(), a) != trace.end();
}

} // namespace helix::test

namespace helix {
/// AFC against a real MoonrakerAPI over the mock client, with no execute_gcode
/// override, so the macro ack reaches dispatch_operation through the base
/// class's own single defer.
class AfcLiveDispatchHelper : public AmsBackendAfc {
  public:
    MockPrinter printer;

    AfcLiveDispatchHelper() : AmsBackendAfc(nullptr, nullptr) {
        api_ = &printer.api;
        std::vector<std::string> lanes{"lane1", "lane2", "lane3", "lane4"};
        AfcTestAccess::initialize_slots(*this, lanes);
        for (int i = 0; i < 4; ++i) {
            if (auto* entry = AfcTestAccess::slots(*this).get_mut(i))
                entry->info.status = SlotStatus::AVAILABLE;
        }
        running_ = true;
    }
    ~AfcLiveDispatchHelper() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    bool toolhead_homed() const override {
        return true;
    }
};
} // namespace helix
