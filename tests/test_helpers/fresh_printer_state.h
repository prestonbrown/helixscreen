// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state_test_access.h"

/// The process-wide PrinterState reset to a fresh session, its subjects
/// initialized, and registered with the XML engine when @p register_xml.
struct FreshPrinterStateBase {
    helix::PrinterState& state;
    explicit FreshPrinterStateBase(bool register_xml)
        : state((lv_init_safe(), get_printer_state())) {
        helix::PrinterStateTestAccess::reset(state);
        state.init_subjects(register_xml);
    }
};

struct FreshPrinterState : FreshPrinterStateBase {
    FreshPrinterState() : FreshPrinterStateBase(false) {}
};

/// For cases that look subjects up by name.
struct FreshRegisteredPrinterState : FreshPrinterStateBase {
    FreshRegisteredPrinterState() : FreshPrinterStateBase(true) {}
};
