// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_bypass_spool_widget.h"

#include "ui_ams_lane_spool.h"
#include "ui_spool_canvas.h"

#include "ams_backend.h"
#include "ams_bypass_policy.h"
#include "ams_state.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <cstring>

namespace helix::ui {

bool bypass_node_visible_for(const AmsBackend* backend) {
    if (backend == nullptr) {
        return false;
    }
    return bypass_node_visible_for(backend, backend->get_system_info());
}

bool bypass_node_visible_for(const AmsBackend* backend, const AmsSystemInfo& info) {
    if (backend == nullptr) {
        return false;
    }
    return bypass_node_visible(helix::bypass_available_for(info.supports_bypass),
                               backend->is_bypass_active(), backend->bypass_is_virtual(),
                               SettingsManager::instance().get_ams_always_show_bypass_spool());
}

void bypass_spool_sync_from_state(BypassSpoolWidgets& w) {
    if (!w.valid()) {
        return;
    }
    bypass_spool_sync_from_state(w, bypass_node_visible_for(AmsState::instance().get_backend()));
}

void bypass_spool_sync_from_state(BypassSpoolWidgets& w, bool show) {
    if (!w.valid()) {
        return;
    }
    auto& ams = AmsState::instance();
    // On AFC the node is removed entirely while bypass is disengaged (#1229).
    bypass_spool_set_visible(w, show);
    if (!show) {
        return;
    }
    if (auto ext = ams.get_external_spool_info()) {
        bypass_spool_set_filament(w, *ext);
        bypass_spool_set_has_spool(w, true);
        bypass_spool_set_material(w, ext->material.c_str());
    } else {
        bypass_spool_set_color(w, 0x505050);
        bypass_spool_set_has_spool(w, false);
        bypass_spool_set_material(w, "");
    }
    // Same ring a lane slot wears when it is the active node - bypass IS a node
    // on the path, and while it is engaged it is the one feeding the toolhead.
    bypass_spool_set_active(w, lv_subject_get_int(ams.get_bypass_active_subject()) != 0);
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
        if (part == nullptr) {
            continue;
        }
        if (visible) {
            lv_obj_set_hidden(part, false);
        } else {
            lv_obj_set_hidden(part, true);
        }
    }

    if (!visible && w.material_label != nullptr) {
        lv_obj_set_hidden(w.material_label, true);
    }
}

namespace {
constexpr int32_t kBoxPad = 4;
constexpr int32_t kLabelGap = 4;
constexpr int32_t kLabelHeight = 16;
// The room the tube's turn takes between the spool and the merge line, and between the
// line and the "Bypass" label under it.
constexpr int32_t kTubeDrop = 10;
constexpr int32_t kTubeClear = 8;
// What bypass_spool_drawn_area() finds the overlay by among its parent's children.
constexpr const char* kBoxName = "bypass_spool_box";
constexpr const char* kMaterialLabelName = "bypass_material_label";
} // namespace

int32_t bypass_spool_bottom_above_merge() {
    return kTubeDrop + kBoxPad;
}

int32_t bypass_spool_label_room() {
    return kLabelHeight + kLabelGap;
}

int32_t bypass_spool_box_size() {
    return bypass_spool_box_size(ams_slot_spool_size());
}

int32_t bypass_spool_box_size(int32_t spool_size) {
    return spool_size + kBoxPad * 2;
}

BypassSpoolSpot bypass_spool_spot(int32_t lane_spool, int32_t min_spool,
                                  const BypassSpoolRoom& room,
                                  const std::function<int32_t(int32_t top)>& unit_right) {
    for (int32_t spool = lane_spool; spool > min_spool; --spool) {
        const int32_t box = bypass_spool_box_size(spool);
        const int32_t label_top = room.box_bottom - box - bypass_spool_label_room();
        if (label_top < room.top)
            continue;
        const int32_t unit = unit_right(label_top);
        const int32_t clear_left = unit == INT32_MIN ? INT32_MIN / 2 : unit + room.gap;
        if (room.right - clear_left < box)
            continue;
        const int32_t cx =
            std::clamp(room.home_cx, clear_left + box / 2, room.right - (box - box / 2));
        return {spool, box, cx};
    }
    return {min_spool, bypass_spool_box_size(min_spool), room.home_cx};
}

namespace {
/// The top of a spool box @p box_size tall standing on the merge line at @p cy.
int32_t box_top_on(int32_t cy, int32_t box_size) {
    return cy - kTubeDrop - box_size;
}
} // namespace

bool bypass_spool_drawn_area(lv_obj_t* parent, lv_area_t* out) {
    if (!parent || !out)
        return false;
    lv_obj_t* box = nullptr;
    lv_obj_t* material = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t* child = lv_obj_get_child(parent, (int32_t)i);
        const char* name = lv_obj_get_name(child);
        if (!name)
            continue;
        if (std::strcmp(name, kBoxName) == 0)
            box = child;
        else if (std::strcmp(name, kMaterialLabelName) == 0)
            material = child;
    }
    if (!box || lv_obj_is_hidden(box))
        return false;
    lv_obj_update_layout(parent);
    lv_obj_get_coords(box, out);
    if (material && !lv_obj_is_hidden(material)) {
        lv_area_t m;
        lv_obj_get_coords(material, &m);
        out->x1 = LV_MIN(out->x1, m.x1);
        out->y1 = LV_MIN(out->y1, m.y1);
        out->x2 = LV_MAX(out->x2, m.x2);
        out->y2 = LV_MAX(out->y2, m.y2);
    }
    return true;
}

BypassSpoolWidgets bypass_spool_create(lv_obj_t* parent, lv_event_cb_t on_click, void* user_data) {
    BypassSpoolWidgets w;
    if (!parent) {
        return w;
    }

    const int32_t spool_size = ams_slot_spool_size();
    w.spool_size = spool_size;
    w.box_size = bypass_spool_box_size(spool_size);
    w.box = lv_obj_create(parent);
    lv_obj_set_name(w.box, kBoxName);
    lv_obj_set_size(w.box, w.box_size, w.box_size);
    lv_obj_set_style_pad_all(w.box, 0, 0);
    lv_obj_set_style_bg_opa(w.box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(w.box, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_radius(w.box, theme_manager_get_spacing("border_radius"), 0);
    lv_obj_set_style_border_width(w.box, 0, 0);
    lv_obj_set_scrollable(w.box, false);
    lv_obj_set_floating(w.box, true);

    w.spool_canvas = ui_spool_canvas_create(w.box, spool_size);
    if (!w.spool_canvas) {
        lv_obj_delete(w.box);
        w = {};
        return w;
    }
    lv_obj_set_pos(w.spool_canvas, kBoxPad, kBoxPad);

    if (on_click) {
        lv_obj_set_clickable(w.box, true);
        lv_obj_add_event_cb(w.box, on_click, LV_EVENT_CLICKED, user_data);
    }

    w.bypass_label = lv_label_create(parent);
    lv_label_set_text(w.bypass_label, lv_tr("Bypass"));
    lv_obj_set_style_text_color(w.bypass_label, theme_manager_get_color("text"), 0);
    lv_obj_set_floating(w.bypass_label, true);
    lv_obj_update_layout(w.bypass_label);
    w.cached_bypass_label_w = lv_obj_get_width(w.bypass_label);

    w.material_label = lv_label_create(parent);
    lv_obj_set_name(w.material_label, kMaterialLabelName);
    lv_label_set_text(w.material_label, "");
    lv_obj_set_style_text_color(w.material_label, theme_manager_get_color("text"), 0);
    lv_obj_set_floating(w.material_label, true);
    lv_obj_set_hidden(w.material_label, true);

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

void bypass_spool_set_size(BypassSpoolWidgets& w, int32_t spool_size) {
    if (!w.box || !w.spool_canvas || spool_size <= 0 || spool_size == w.spool_size) {
        return;
    }
    w.spool_size = spool_size;
    w.box_size = bypass_spool_box_size(spool_size);
    lv_obj_set_size(w.box, w.box_size, w.box_size);
    ui_spool_canvas_set_size(w.spool_canvas, spool_size);
}

void bypass_spool_set_color(BypassSpoolWidgets& w, uint32_t color_rgb) {
    if (!w.spool_canvas || (w.color_painted && w.cached_color_rgb == color_rgb)) {
        return;
    }
    w.color_painted = true;
    w.cached_color_rgb = color_rgb;
    ui_spool_canvas_set_color(w.spool_canvas, lv_color_hex(color_rgb));
    ui_spool_canvas_redraw(w.spool_canvas);
}

void bypass_spool_set_filament(BypassSpoolWidgets& w, const SlotInfo& slot) {
    if (!w.spool_canvas) {
        return;
    }
    // The canvas dedups on its own render key. The plain-colour cache records
    // the colour but not as painted, so a later plain colour always repaints
    // over the split and finish.
    w.color_painted = false;
    w.cached_color_rgb = slot.color_rgb;
    spool_canvas_set_filament(w.spool_canvas, slot);
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

void bypass_spool_set_active(BypassSpoolWidgets& w, bool active) {
    if (!w.box || (w.active_applied && w.cached_active == active)) {
        return;
    }
    w.active_applied = true;
    w.cached_active = active;
    ams_draw::set_active_ring(w.box, active);
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
        lv_obj_set_hidden(w.material_label, true);
    } else {
        lv_obj_set_hidden(w.material_label, false);
    }
}

void bypass_spool_set_position(BypassSpoolWidgets& w, int32_t cx, int32_t cy) {
    if (!w.valid()) {
        return;
    }

    int32_t box_left = cx - w.box_size / 2;
    int32_t box_top = box_top_on(cy, w.box_size);
    lv_obj_set_pos(w.box, box_left, box_top);

    if (w.bypass_label) {
        lv_obj_set_pos(w.bypass_label, cx - w.cached_bypass_label_w / 2, cy + kTubeClear);
    }
    if (w.material_label) {
        // Material text changes — must measure each call.
        lv_obj_update_layout(w.material_label);
        int32_t mat_w = lv_obj_get_width(w.material_label);
        lv_obj_set_pos(w.material_label, cx - mat_w / 2, box_top - kLabelHeight - kLabelGap);
    }
}

} // namespace helix::ui
