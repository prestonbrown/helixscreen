// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/afc_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_afc.h"

#include <string>
#include <utility>

namespace helix {

// Friend-class shim for AmsBackendAfc -- declared as friend in the backend
// header (`friend class AfcTestAccess;`). One shim per backend is the shape the
// CFS, QIDI, ACE and AD5X IFS backends already use.
//
// A single shim keeps the friend list a contract a reader can check: the
// backend grants access to exactly one test-layer name, and every widening of
// what tests can reach happens here rather than in the backend header. Test
// fixtures still subclass AmsBackendAfc for their own setup, but they reach
// private state through these accessors instead of being befriended one by one.
//
// Nothing here widens the production API: each accessor is a named view of a
// member that stays private, which is what the shim buys over adding protected
// members for tests to inherit.
//
// The accessors template on the backend reference type so one definition serves
// both a const and a non-const caller; they instantiate only for a type that has
// the member, so the narrowness is in the member each one names.
class AfcTestAccess {
  public:
    // --- private data -------------------------------------------------------
    // `auto&` deduces each member's own type, so adding an accessor never
    // restates a declaration that would then have to track the backend's.
    template <class B> static auto& mutex(B& b) {
        return b.mutex_;
    }
    // Protected on AmsSubscriptionBackend, but [class.protected] lets a derived
    // class reach it only through its own type; the shim is a friend, so it can
    // take the backend by base reference the way these tests already do.
    template <class B> static auto& system_info(B& b) {
        return b.system_info_;
    }
    template <class B> static auto& slots(B& b) {
        return b.slots_;
    }
    template <class B> static auto& hub_sensors(B& b) {
        return b.hub_sensors_;
    }
    template <class B> static auto& hub_names(B& b) {
        return b.hub_names_;
    }
    template <class B> static auto& afc_config(B& b) {
        return b.afc_config_;
    }
    template <class B> static auto& afc_version(B& b) {
        return b.afc_version_;
    }
    template <class B> static auto& afc_led_state(B& b) {
        return b.afc_led_state_;
    }
    template <class B> static auto& afc_quiet_mode(B& b) {
        return b.afc_quiet_mode_;
    }
    template <class B> static auto& afc_tool_cmds(B& b) {
        return b.afc_tool_cmds_;
    }
    template <class B> static auto& overrides(B& b) {
        return b.overrides_;
    }
    /// Namespace the override store was pointed at, or empty when no store was
    /// built. Lets a test assert the PRIVATE namespace without reaching for the
    /// store itself, which stays private.
    template <class B> static std::string store_namespace(const B& b) {
        return b.override_store_ ? b.override_store_->namespace_for_test() : std::string();
    }
    template <class B> static auto& extruders(B& b) {
        return b.extruders_;
    }
    template <class B> static auto& extruder_names(B& b) {
        return b.extruder_names_;
    }
    template <class B> static auto& extruder_klipper_names(B& b) {
        return b.extruder_klipper_names_;
    }
    template <class B> static auto& extruder_sensors(B& b) {
        return b.extruder_sensors_;
    }
    template <class B> static auto& extruder_tool_index_warned(B& b) {
        return b.extruder_tool_index_warned_;
    }
    template <class B> static auto& num_extruders(B& b) {
        return b.num_extruders_;
    }
    template <class B> static auto& discovered_lane_names(B& b) {
        return b.discovered_lane_names_;
    }
    template <class B> static auto& configfile_answered(B& b) {
        return b.configfile_answered_;
    }
    template <class B> static auto& configfile_has_toolchanger(B& b) {
        return b.configfile_has_toolchanger_;
    }
    template <class B> static auto& configs_loaded(B& b) {
        return b.configs_loaded_;
    }
    template <class B> static auto& current_lane_name(B& b) {
        return b.current_lane_name_;
    }
    template <class B> static auto& active_load_lane(B& b) {
        return b.active_load_lane_;
    }
    template <class B> static auto& toolhead_lane(B& b) {
        return b.toolhead_lane_;
    }
    template <class B> static auto& tool_start_sensor(B& b) {
        return b.tool_start_sensor_;
    }
    template <class B> static auto& tool_end_sensor(B& b) {
        return b.tool_end_sensor_;
    }
    template <class B> static auto& tool_states(B& b) {
        return b.tool_states_;
    }
    template <class B> static auto& eject_queue_mutex(B& b) {
        return b.eject_queue_mutex_;
    }
    template <class B> static auto& pending_eject_lanes(B& b) {
        return b.pending_eject_lanes_;
    }
    template <class B> static auto& pending_dispatch_action(B& b) {
        return b.pending_dispatch_action_;
    }
    template <class B> static auto& action_start_time(B& b) {
        return b.action_start_time_;
    }
    template <class B> static auto& timed_out_state(B& b) {
        return b.timed_out_state_;
    }
    template <class B> static auto& error_state(B& b) {
        return b.error_state_;
    }
    template <class B> static auto& last_seen_message(B& b) {
        return b.last_seen_message_;
    }
    template <class B> static auto& last_error_msg(B& b) {
        return b.last_error_msg_;
    }
    template <class B> static auto& message_drain_deadline(B& b) {
        return b.message_drain_deadline_;
    }
    template <class B> static auto& macro_vars_config(B& b) {
        return b.macro_vars_config_;
    }
    template <class B> static auto& lane_hub_routing(B& b) {
        return b.lane_hub_routing_;
    }
    template <class B> static auto& lane_object_prefix(B& b) {
        return b.lane_object_prefix_;
    }
    template <class B> static auto& lane_remember_spool(B& b) {
        return b.lane_remember_spool_;
    }
    template <class B> static auto& buffer_names(B& b) {
        return b.buffer_names_;
    }
    template <class B> static auto& bowden_length(B& b) {
        return b.bowden_length_;
    }
    template <class B> static auto& unit_infos(B& b) {
        return b.unit_infos_;
    }
    template <class B> static auto& unit_lane_map(B& b) {
        return b.unit_lane_map_;
    }

    /// Static constant, so no object to take.
    static constexpr auto message_drain_max_clears() {
        return AmsBackendAfc::MESSAGE_DRAIN_MAX_CLEARS;
    }

    // --- private methods ----------------------------------------------------
    template <class B, class... A> static decltype(auto) handle_status_update(B& b, A&&... a) {
        return b.handle_status_update(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) initialize_slots(B& b, A&&... a) {
        return b.initialize_slots(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) execute_gcode(B& b, A&&... a) {
        return b.execute_gcode(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) execute_gcode_notify(B& b, A&&... a) {
        return b.execute_gcode_notify(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) emit_event(B& b, A&&... a) {
        return b.emit_event(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) check_preconditions(B& b, A&&... a) {
        return b.check_preconditions(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) on_started(B& b, A&&... a) {
        return b.on_started(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) parse_afc_state(B& b, A&&... a) {
        return b.parse_afc_state(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) parse_lane_data(B& b, A&&... a) {
        return b.parse_lane_data(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) apply_lane_data_response(B& b, A&&... a) {
        return b.apply_lane_data_response(std::forward<A>(a)...);
    }
    template <class B, class... A>
    static decltype(auto) apply_afc_version_response(B& b, A&&... a) {
        return b.apply_afc_version_response(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) detect_afc_version(B& b, A&&... a) {
        return b.detect_afc_version(std::forward<A>(a)...);
    }
    template <class B, class... A>
    static decltype(auto) query_afc_configfile_topology(B& b, A&&... a) {
        return b.query_afc_configfile_topology(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) reorganize_slots(B& b, A&&... a) {
        return b.reorganize_slots(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) dispatch_lane_unload(B& b, A&&... a) {
        return b.dispatch_lane_unload(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) on_lane_unload_done(B& b, A&&... a) {
        return b.on_lane_unload_done(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) maybe_drain_message_queue(B& b, A&&... a) {
        return b.maybe_drain_message_queue(std::forward<A>(a)...);
    }
    template <class B, class... A>
    static decltype(auto) compute_filament_segment_unlocked(B& b, A&&... a) {
        return b.compute_filament_segment_unlocked(std::forward<A>(a)...);
    }
    template <class B, class... A>
    static decltype(auto) tool_index_for_extruder_unlocked(B& b, A&&... a) {
        return b.tool_index_for_extruder_unlocked(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) record_own_spool_write(B& b, A&&... a) {
        return b.record_own_spool_write(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) ensure_homed_then(B& b, A&&... a) {
        return b.ensure_homed_then(std::forward<A>(a)...);
    }
    /// Static private method, so it takes no backend object.
    template <class... A> static decltype(auto) database_item_value(A&&... a) {
        return AmsBackendAfc::database_item_value(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) has_toolchanger(B& b, A&&... a) {
        return b.has_toolchanger(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) clear_values(B& b, A&&... a) {
        return b.clear_values(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) sync_from_backend(B& b, A&&... a) {
        return b.sync_from_backend(std::forward<A>(a)...);
    }
};

} // namespace helix
