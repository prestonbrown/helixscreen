// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state_test_access.h"

/// The process-wide PrinterState reset to a fresh session, its subjects initialized.
struct FreshPrinterState {
    helix::PrinterState& state;
    FreshPrinterState() : state((lv_init_safe(), get_printer_state())) {
        helix::PrinterStateTestAccess::reset(state);
        state.init_subjects(false);
    }
};
