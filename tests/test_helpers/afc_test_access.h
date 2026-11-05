// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/afc_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_backend_afc.h"

// Friend-class shim for AmsBackendAfc -- declared as friend in the backend
// header (`friend class AfcTestAccess;`), alongside the per-fixture friend
// declarations already there, which reach private state directly. Gives a
// test a named accessor for the override map (overrides_) instead of
// adding another per-fixture friend.
//
// The accessor templates on the backend reference type so one definition
// serves both a const and a non-const caller.
class AfcTestAccess {
  public:
    template <class B> static auto& overrides(B& b) {
        return b.overrides_;
    }
    // The re-layer a late read of the override store triggers.
    static void relayer_after_read(AmsBackendAfc& b) {
        b.relayer_overrides_after_read_locked();
    }
    template <class B> static auto& toolchange_hold_since(B& b) {
        return b.toolchange_hold_since_;
    }
    template <class B> static auto& timed_out_state(B& b) {
        return b.timed_out_state_;
    }
    template <class B> static auto& system_info(B& b) {
        return b.system_info_;
    }
};
