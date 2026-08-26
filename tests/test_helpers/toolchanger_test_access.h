// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/toolchanger_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_toolchanger.h"
#include "ams_error.h"
#include "filament_slot_override_store.h"

#include <mutex>
#include <string>
#include <utility>

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

    /// Whether an optimistic dispatch is still armed and awaiting resolution.
    /// A cancelled home confirmation must clear this -- otherwise the next
    /// macro ack (or a superseding dispatch) resolves against a generation
    /// that no longer describes anything in flight.
    static bool has_pending_dispatch(const AmsBackendToolChanger& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.pending_dispatch_action_.has_value();
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

    /// Name of the Moonraker DB namespace the store was pointed at, so a test
    /// can assert it is the SHARED "lane_data" and not a private one. Empty
    /// when no store was built (null API).
    static std::string store_namespace(const AmsBackendToolChanger& b) {
        return b.override_store_ ? b.override_store_->namespace_for_test() : std::string();
    }
};
