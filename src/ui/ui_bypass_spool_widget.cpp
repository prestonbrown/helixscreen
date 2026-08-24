// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_bypass_spool_widget.h"

#include "ui_spool_canvas.h"

#include "ams_backend.h"
#include "ams_bypass_policy.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <cstring>

namespace helix::ui {

bool bypass_node_visible_for(const AmsBackend* backend) {
    if (backend == nullptr) {
        return false;
    }
    return bypass_node_visible(
        helix::bypass_available_for(backend->get_system_info().supports_bypass),
        backend->is_bypass_active(), backend->is_afc_system(),
        SettingsManager::instance().get_ams_always_show_bypass_spool());
}

void bypass_spool_set_visible(BypassSpoolWidgets& w, bool visible) {
    // The labels are siblings of the card, not children, so each has to be
    // hidden individually or a stray "Bypass" caption survives the card.
    //
    // Asymmetric on purpose: hiding covers material_label too, but showing does
    // NOT unhide it — bypass_spool_set_material() owns that one and keeps it
    // hidden while the material string is empty. Force-showing it here would
    // resurrect an empty label the moment the node came back.
    lv_obj_t* const always[] = {w.box, w.bypass_label};
    for (lv_obj_t* part : always) {
        if (part == nullptr || !lv_obj_is_valid(part)) {
            continue;
        }
        if (visible) {
            lv_obj_remove_flag(part, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!visible && w.material_label != nullptr && lv_obj_is_valid(w.material_label)) {
        lv_obj_add_flag(w.material_label, LV_OBJ_FLAG_HIDDEN);
    }
}

namespace {
constexpr int32_t BYPASS_SPOOL_SIZE = 48;
constexpr int32_t BOX_PAD = 4;
constexpr int32_t BOX_SIZE = BYPASS_SPOOL_SIZE + BOX_PAD * 2;
constexpr int32_t LABEL_GAP = 4;
constexpr int32_t LABEL_HEIGHT = 16;
} // namespace

BypassSpoolWidgets bypass_spool_create(lv_obj_t* parent, lv_event_cb_t on_click, void* user_data) {
    BypassSpoolWidgets w;
    if (!parent) {
        return w;
    }

    w.box = lv_obj_create(parent);
    lv_obj_set_size(w.box, BOX_SIZE, BOX_SIZE);
    lv_obj_set_style_pad_all(w.box, 0, 0);
    lv_obj_set_style_bg_opa(w.box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(w.box, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_radius(w.box, theme_manager_get_spacing("border_radius"), 0);
    lv_obj_set_style_border_width(w.box, 0, 0);
    lv_obj_remove_flag(w.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w.box, LV_OBJ_FLAG_FLOATING);

    w.spool_canvas = ui_spool_canvas_create(w.box, BYPASS_SPOOL_SIZE);
    if (!w.spool_canvas) {
        lv_obj_delete(w.box);
        w = {};
        return w;
    }
    lv_obj_set_pos(w.spool_canvas, BOX_PAD, BOX_PAD);

    if (on_click) {
        lv_obj_add_flag(w.box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(w.box, on_click, LV_EVENT_CLICKED, user_data);
    }

    w.bypass_label = lv_label_create(parent);
    lv_label_set_text(w.bypass_label, lv_tr("Bypass"));
    lv_obj_set_style_text_color(w.bypass_label, theme_manager_get_color("text"), 0);
    lv_obj_add_flag(w.bypass_label, LV_OBJ_FLAG_FLOATING);
    lv_obj_update_layout(w.bypass_label);
    w.cached_bypass_label_w = lv_obj_get_width(w.bypass_label);

    w.material_label = lv_label_create(parent);
    lv_label_set_text(w.material_label, "");
    lv_obj_set_style_text_color(w.material_label, theme_manager_get_color("text"), 0);
    lv_obj_add_flag(w.material_label, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(w.material_label, LV_OBJ_FLAG_HIDDEN);

    return w;
}

void bypass_spool_destroy(BypassSpoolWidgets& w) {
    if (w.box) {
        lv_obj_delete(w.box);
    }
    if (w.bypass_label) {
        lv_obj_delete(w.bypass_label);
    }
    if (w.material_label) {
        lv_obj_delete(w.material_label);
    }
    w = {};
}

void bypass_spool_set_color(BypassSpoolWidgets& w, uint32_t color_rgb) {
    if (!w.spool_canvas || w.cached_color_rgb == color_rgb) {
        return;
    }
    w.cached_color_rgb = color_rgb;
    ui_spool_canvas_set_color(w.spool_canvas, lv_color_hex(color_rgb));
    ui_spool_canvas_redraw(w.spool_canvas);
}

void bypass_spool_set_has_spool(BypassSpoolWidgets& w, bool has_spool) {
    if (!w.spool_canvas || w.cached_has_spool == has_spool) {
        return;
    }
    w.cached_has_spool = has_spool;
    // The bypass spool is hand-fed and has no weight behind it, so it draws full
    // whenever one is present — same answer as any other unweighed spool.
    ui_spool_canvas_set_fill_level(w.spool_canvas, has_spool ? 1.0f : 0.0f);
    ui_spool_canvas_redraw(w.spool_canvas);
}

void bypass_spool_set_material(BypassSpoolWidgets& w, const char* material) {
    if (!w.material_label) {
        return;
    }
    const char* m = material ? material : "";
    if (std::strncmp(w.cached_material, m, sizeof(w.cached_material)) == 0) {
        return;
    }
    std::strncpy(w.cached_material, m, sizeof(w.cached_material) - 1);
    w.cached_material[sizeof(w.cached_material) - 1] = '\0';

    lv_label_set_text(w.material_label, w.cached_material);
    if (w.cached_material[0] == '\0') {
        lv_obj_add_flag(w.material_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(w.material_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void bypass_spool_set_position(BypassSpoolWidgets& w, int32_t cx, int32_t cy) {
    if (!w.valid()) {
        return;
    }

    int32_t box_left = cx - BOX_SIZE / 2;
    int32_t box_top = cy - BOX_SIZE / 2;
    lv_obj_set_pos(w.box, box_left, box_top);

    if (w.bypass_label) {
        lv_obj_set_pos(w.bypass_label, cx - w.cached_bypass_label_w / 2,
                       box_top + BOX_SIZE + LABEL_GAP);
    }
    if (w.material_label) {
        // Material text changes — must measure each call.
        lv_obj_update_layout(w.material_label);
        int32_t mat_w = lv_obj_get_width(w.material_label);
        lv_obj_set_pos(w.material_label, cx - mat_w / 2, box_top - LABEL_HEIGHT - LABEL_GAP);
    }
}

} // namespace helix::ui
