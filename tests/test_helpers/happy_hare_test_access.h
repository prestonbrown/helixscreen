// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/happy_hare_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_happy_hare.h"

#include <string>
#include <utility>

namespace helix {

// Friend-class shim for AmsBackendHappyHare -- declared as friend in the backend
// header (`friend class HappyHareTestAccess;`). One shim per backend is the
// shape the CFS, QIDI, ACE and AD5X IFS backends already use.
//
// A single shim keeps the friend list a contract a reader can check: the backend
// grants access to exactly one test-layer name, and every widening of what tests
// can reach happens here rather than in the backend header. Test fixtures still
// subclass AmsBackendHappyHare for their own setup, but they reach private state
// through these accessors instead of being befriended one by one.
//
// Nothing here widens the production API: each accessor is a named view of a
// member that stays private, which is what the shim buys over adding protected
// members for tests to inherit.
//
// The accessors template on the backend reference type so one definition serves
// both a const and a non-const caller; they instantiate only for a type that has
// the member, so the narrowness is in the member each one names.
class HappyHareTestAccess {
  public:
    // --- private data -------------------------------------------------------
    // `auto&` deduces each member's own type, so adding an accessor never
    // restates a declaration that would then have to track the backend's.
    template <class B> static auto& mutex(B& b) {
        return b.mutex_;
    }
    template <class B> static auto& slots(B& b) {
        return b.slots_;
    }
    template <class B> static auto& overrides(B& b) {
        return b.overrides_;
    }
    template <class B> static auto& gate_sensors(B& b) {
        return b.gate_sensors_;
    }
    template <class B> static auto& selector_type(B& b) {
        return b.selector_type_;
    }
    template <class B> static auto& config_defaults(B& b) {
        return b.config_defaults_;
    }
    template <class B> static auto& user_overrides(B& b) {
        return b.user_overrides_;
    }
    template <class B> static auto& now_fn(B& b) {
        return b.now_fn_;
    }
    template <class B> static auto& filament_heaters(B& b) {
        return b.filament_heaters_;
    }
    template <class B> static auto& environment_sensors(B& b) {
        return b.environment_sensors_;
    }
    template <class B> static auto& gate_drying_states(B& b) {
        return b.gate_drying_states_;
    }
    template <class B> static auto& heater_temp(B& b) {
        return b.heater_temp_;
    }
    template <class B> static auto& dryer_info(B& b) {
        return b.dryer_info_;
    }
    template <class B> static auto gate_sensor(B& b, int slot_index) {
        return b.gate_sensor(slot_index);
    }
    template <class B> static auto& filament_pos(B& b) {
        return b.filament_pos_;
    }
    template <class B> static auto& bowden_progress(B& b) {
        return b.bowden_progress_;
    }
    template <class B> static auto& error_segment(B& b) {
        return b.error_segment_;
    }
    template <class B> static auto& reason_for_pause(B& b) {
        return b.reason_for_pause_;
    }
    template <class B> static auto& num_units(B& b) {
        return b.num_units_;
    }
    template <class B> static auto& per_unit_gate_counts(B& b) {
        return b.per_unit_gate_counts_;
    }
    template <class B> static auto& active_unit(B& b) {
        return b.active_unit_;
    }
    template <class B> static auto& gate_status_raw(B& b) {
        return b.gate_status_raw_;
    }
    template <class B> static auto& led_exit_effect(B& b) {
        return b.led_exit_effect_;
    }
    template <class B> static auto& espooler_active(B& b) {
        return b.espooler_active_;
    }
    template <class B> static auto& flowguard_encoder_mode(B& b) {
        return b.flowguard_encoder_mode_;
    }

    /// Namespace the override store was pointed at, or empty when no store was
    /// built. Lets a test assert the PRIVATE namespace without reaching for the
    /// store itself, which stays private.
    template <class B> static std::string store_namespace(const B& b) {
        return b.override_store_ ? b.override_store_->namespace_for_test() : std::string();
    }

    // --- private methods ----------------------------------------------------
    template <class B, class... A> static decltype(auto) handle_status_update(B& b, A&&... a) {
        return b.handle_status_update(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) reapply_overrides(B& b, A&&... a) {
        return b.reapply_overrides(std::forward<A>(a)...);
    }
    /// Retire a gate's departed-spool identity with no frame behind it, so a
    /// test can see what the retire itself dropped rather than what the paint
    /// that follows one in production restates.
    template <class B, class... A> static decltype(auto) retire_departed_identity(B& b, A&&... a) {
        return b.retire_departed_identity_locked(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) update_unit_topologies(B& b, A&&... a) {
        return b.update_unit_topologies(std::forward<A>(a)...);
    }
    template <class B, class... A> static decltype(auto) is_type_b(B& b, A&&... a) {
        return b.is_type_b(std::forward<A>(a)...);
    }
    template <class B> static void on_started(B& b) {
        b.on_started();
    }
    template <class B, class... A> static decltype(auto) apply_heater_config(B& b, A&&... a) {
        return b.apply_heater_config(std::forward<A>(a)...);
    }
    template <class B, class... A>
    static decltype(auto) apply_filament_heater_status(B& b, A&&... a) {
        return b.apply_filament_heater_status(std::forward<A>(a)...);
    }
    template <class B, class... A>
    static decltype(auto) apply_environment_sensor_status(B& b, A&&... a) {
        return b.apply_environment_sensor_status(std::forward<A>(a)...);
    }
};

} // namespace helix
