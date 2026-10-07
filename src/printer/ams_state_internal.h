// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Helpers shared by the files AmsState's definitions are split across. Not a
// public header: nothing outside src/printer/ams_state*.cpp includes it.

#pragma once

#include "ams_types.h"
#include "lvgl/lvgl.h"

#include <cstring>

namespace helix::ams_state_detail {

/// Everything AmsState holds outside the registry, RunoutGrace and its atomics
/// is main-thread state with no lock; an off-main caller is a bug. Under strict
/// UI checks (unit tests, --test) it aborts. Otherwise the first hit logs at
/// error and files an "ams_off_main" anomaly, and the call carries on: a
/// shipped printer keeps running on a race rather than crashing on one.
void assert_main_thread(const char* caller = __builtin_FUNCTION());

/// The off-main report itself, for assert_main_thread() and its tests.
void report_off_main(const char* caller);

/// True once the singleton is being destroyed. Work queued to the main thread
/// checks it before touching AmsState.
bool shutting_down();

/// Write @p text into a string subject only when it differs, so observers are
/// not notified of a value they already have.
inline void copy_string_if_changed(lv_subject_t* subject, const char* text) {
    if (std::strcmp(lv_subject_get_string(subject), text) != 0) {
        lv_subject_copy_string(subject, text);
    }
}

/// The error state a lane bar's status line draws from: the same derivation
/// both current consumers (AMS overview mini bars, mini status) compute from
/// SlotInfo. has_error covers a carried SlotError AND a BLOCKED lane;
/// severity falls back to INFO when no error object is carried.
inline void slot_error_state(const SlotInfo& slot, bool& has_error, int& severity) {
    has_error = (slot.status == SlotStatus::BLOCKED || slot.error.has_value());
    severity =
        static_cast<int>(slot.error.has_value() ? slot.error->severity : SlotError::Severity::INFO);
}

} // namespace helix::ams_state_detail
