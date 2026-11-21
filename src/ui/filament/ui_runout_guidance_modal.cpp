// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_runout_guidance_modal.h"

#include "ui_icon.h"

#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

std::string RunoutGuidanceModal::switching_text(const std::string& target_name) {
    return fmt::format(
        fmt::runtime(lv_tr("Switching to {}. The printer heats and purges it, then resumes the "
                           "print.")),
        target_name);
}

void RunoutGuidanceModal::on_show() {
    // The XML header icon is the runout warning; an advisory show (a tap on the
    // home Filament tile) takes the neutral filament glyph instead.
    if (lv_subject_get_int(&advisory_subject_) == 1) {
        if (lv_obj_t* icon = find_widget("dialog_icon")) {
            helix::ui::icon::set_source(icon, "filament");
            helix::ui::icon::set_variant(icon, "primary");
        }
    }

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
    wire_button("btn_other_head", "OtherHead", other_head_button_cb);
}
