// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spaghetti_detection_modal.h"

#include <spdlog/spdlog.h>

void SpaghettiDetectionModal::on_show() {
    // Wire the three action buttons programmatically (mirrors the runout-guidance
    // modal). No XML callbacks on these buttons, so there's no double-wiring.
    //   btn_primary   → on_ok()       → Resume
    //   btn_secondary → on_cancel()   → Abort
    //   btn_tertiary  → on_tertiary() → Tune
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    wire_tertiary_button("btn_tertiary");

    // Message text.
    lv_obj_t* text = find_widget("detection_text");
    if (text) {
        lv_label_set_text(text, message_.c_str());
    } else {
        spdlog::warn("[SpaghettiDetectionModal] detection_text widget not found");
    }

    // Optional camera frame preview. Hide the image entirely when no frame is
    // available so it doesn't reserve empty space.
    lv_obj_t* preview = find_widget("detection_preview");
    if (preview) {
        if (frame_) {
            lv_image_set_src(preview, frame_);
            lv_obj_remove_flag(preview, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        spdlog::warn("[SpaghettiDetectionModal] detection_preview widget not found");
    }
}
