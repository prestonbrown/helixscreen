// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_runout_guidance_modal.h"

void RunoutGuidanceModal::on_show() {
    // RunoutGuidanceModal has 6 buttons for runout handling:
    // - btn_load_filament → on_ok() (primary action)
    // - btn_unload_filament → on_quaternary() (unload before loading new)
    // - btn_purge → on_quinary() (purge after loading)
    // - btn_resume → on_cancel() (resume paused print)
    // - btn_cancel_print → on_tertiary() (cancel print)
    // - btn_ok → on_senary() (dismiss when idle)
    wire_ok_button("btn_load_filament");
    wire_quaternary_button("btn_unload_filament");
    wire_quinary_button("btn_purge");
    wire_cancel_button("btn_resume");
    wire_tertiary_button("btn_cancel_print");
    wire_senary_button("btn_ok");
}
