// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/toolchanger_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_toolchanger.h"
#include "ams_error.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "json_fwd.h"
#include "test_helpers/seeded_override.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace helix {

// Friend-class shim for AmsBackendToolChanger -- declared as friend in the
// backend header (`friend class ToolChangerTestAccess;`). Gives tests direct
// access to the private optimistic-dispatch surface (dispatch_operation) that
// AmsBackendToolChanger::change_tool() normally guards behind
// check_preconditions()/validate_slot_index(), which need a fully configured
// tool topology this shim lets a bare backend skip. Same pattern as
// CfsTestAccess (tests/test_helpers/cfs_test_access.h).
class ToolChangerTestAccess {
  public:
    /// Call the REAL dispatch_operation implementation directly: sets the
    /// AmsAction optimistically (begin_dispatch_locked), routes @p gcode
    /// through ensure_homed_then(), and resolves on the macro's ack -- or
    /// undoes the optimistic set if the gcode never left (dispatch_operation's
    /// own `if (!result) abandon_dispatch(...)` net).
    static AmsError call_dispatch_operation(AmsBackendToolChanger& b, std::string gcode,
                                            AmsAction action) {
        return b.dispatch_operation(std::move(gcode), action);
    }

    /// Point the backend at a lane-record store without a live Moonraker,
    /// which is what a resync re-reads.
    static void inject_override_store(AmsBackendToolChanger& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }

    /// Whether an optimistic dispatch is still armed and awaiting resolution.
    /// A cancelled home confirmation must clear this -- otherwise the next
    /// macro ack (or a superseding dispatch) resolves against a generation
    /// that no longer describes anything in flight.
    /// Set the optimistic action the way a dispatch does, without sending.
    static void arm_dispatch(AmsBackendToolChanger& b, AmsAction action) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.begin_dispatch_locked(action);
    }

    static bool has_pending_dispatch(const AmsBackendToolChanger& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.pending_dispatch_action_.has_value();
    }

    /// Deliver the operator's prompt reply to the mount-to-feed run, the
    /// main-thread half its notify_gcode_response listener drives on a real
    /// client.
    static void feed_operator_reply(AmsBackendToolChanger& b, helix::mount_feed::Reply reply) {
        b.on_feed_reply(reply);
    }

    /// Deliver what the console watcher parsed: a reply, or the dismissal echo
    /// of the prompt ending without an answer.
    static void feed_console_signal(AmsBackendToolChanger& b,
                                    helix::mount_feed::ConsoleSignal signal) {
        b.on_feed_signal(signal);
    }

    /// Advance the operator wait by @p ms, the way the watchdog timer does
    /// once a second in the app.
    static void feed_wait_elapsed(AmsBackendToolChanger& b, uint32_t ms) {
        b.handle_wait_elapsed(ms);
    }

    /// Replace the operator-wait watchdog's clock.
    static void set_feed_clock(AmsBackendToolChanger& b,
                               std::function<std::chrono::steady_clock::time_point()> clock) {
        b.feed_clock_ = std::move(clock);
    }

    /// One firing of the operator-wait watchdog timer, as LVGL runs it.
    static void feed_wait_tick(AmsBackendToolChanger& b) {
        b.feed_wait_tick();
    }

    /// The backend stopping under a live run, through stop() itself so the
    /// unwind runs under the same locking it meets in the app.
    static void feed_disconnect_stopping(AmsBackendToolChanger& b) {
        b.stop();
    }

    /// The wording of the last toast a feed run raised.
    static const std::string& feed_last_toast(const AmsBackendToolChanger& b) {
        return b.feed_last_toast_;
    }

    /// Forget the last toast, so a case can tell a repeat from no toast at all.
    static void clear_feed_last_toast(AmsBackendToolChanger& b) {
        b.feed_last_toast_.clear();
    }

    /// The grace for the dock sensors to confirm an acked swap ran out.
    static void feed_swap_check_elapsed(AmsBackendToolChanger& b) {
        b.handle_swap_check_elapsed();
    }

    /// A sensor fault flagged under no live run, as a path that never consumes
    /// it would leave it.
    static void set_feed_sensor_fault(AmsBackendToolChanger& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.feed_sensor_fault_ = true;
    }

    /// The severity of the last toast a feed run raised.
    static ToastSeverity feed_last_toast_severity(const AmsBackendToolChanger& b) {
        return b.feed_last_toast_severity_;
    }

    /// Deliver one gcode-response line to the Z-Mod marker handler, the
    /// main-thread half the notify_gcode_response listener drives on a real
    /// client. The listener itself needs a client, which the unit-test helper
    /// does not wire.
    static void feed_zmod_marker(AmsBackendToolChanger& b, const std::string& line) {
        b.on_zmod_marker_line(line);
    }

    /// The controller's latched phase word.
    static std::string last_operation_word(const AmsBackendToolChanger& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.last_operation_word_;
    }

    /// Run the REAL on_started(), which is where the backend builds its
    /// FilamentSlotOverrideStore and does the blocking load. start() is final on
    /// AmsSubscriptionBackend and drags in subscription setup a store
    /// round-trip has no use for, so this reaches the one hook that matters.
    ///
    /// on_started() and NOT additional_start_checks(): start() calls the latter
    /// with mutex_ held, and load_blocking() needs mutex_ to publish - doing it
    /// there self-deadlocks.
    static void call_on_started(AmsBackendToolChanger& b) {
        b.on_started();
    }

    /// Drive the REAL status path. handle_status_update() is protected, and it
    /// is where every dock and carriage reading lands, so a case that wants a
    /// later frame's effect has to go through it rather than poking members.
    static void handle_status(AmsBackendToolChanger& b, const nlohmann::json& n) {
        b.handle_status_update(n);
    }

    /// Put a user override in the map without a store behind it. On this
    /// backend the override IS the only source of filament identity, so this is
    /// how a case builds a slot whose every identity field is the user's.
    static void seed_override(AmsBackendToolChanger& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        {
            std::lock_guard<std::mutex> lock(b.mutex_);
            b.overrides_[slot_index] = ovr;
        }
        // The backend's own init files both stores together; a fixture that
        // wrote only this map would leave the lane reading empty.
        helix::test::file_override_as_lane_records(b, slot_index, ovr);
    }

    /// Run the lane_data re-read a firmware change scheduled, now rather than
    /// after its debounce.
    static void fire_lane_record_reread(AmsBackendToolChanger& b) {
        b.lane_reread_pending_.store(false);
        b.reread_lane_records();
    }

    /// Whether a lane_data re-read is scheduled and has not run yet.
    static bool lane_reread_pending(const AmsBackendToolChanger& b) {
        return b.lane_reread_pending_.load();
    }

    /// The override standing for @p slot_index, or nullopt when it has none.
    static std::optional<helix::ams::FilamentSlotOverride>
    get_override(const AmsBackendToolChanger& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// True once any slot holds a stored override.
    static bool has_overrides(const AmsBackendToolChanger& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return !b.overrides_.empty();
    }

    /// Name of the Moonraker DB namespace the store was pointed at, so a test
    /// can assert it is the SHARED "lane_data" and not a private one. Empty
    /// when no store was built (null API).
    static std::string store_namespace(const AmsBackendToolChanger& b) {
        return b.override_store_ ? b.override_store_->namespace_for_test() : std::string();
    }
};
} // namespace helix
