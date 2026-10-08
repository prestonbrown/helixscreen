// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_printer_manager_overlay.h"

struct PrinterManagerOverlayTestAccess {
    static void set_name_input(PrinterManagerOverlay& o, lv_obj_t* input) {
        o.name_input_ = input;
    }
    static void refresh(PrinterManagerOverlay& o) {
        o.refresh_printer_info();
    }
    static void start_name_edit(PrinterManagerOverlay& o) {
        o.start_name_edit();
    }
    static void finish_name_edit(PrinterManagerOverlay& o) {
        o.finish_name_edit();
    }
};
