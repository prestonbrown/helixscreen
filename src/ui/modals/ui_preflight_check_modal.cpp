// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_preflight_check_modal.h"

#include "ui_icon.h"
#include "ui_swatch.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <vector>

namespace helix::ui {

namespace {

// Map a ToolCheck severity to the severity icon's glyph source + variant.
// The <icon> widget owns glyph + color, so a single setter pair covers both.
struct SeverityVisual {
    const char* icon_src;
    const char* variant;
};

SeverityVisual severity_visual(helix::ToolCheck::Severity sev) {
    switch (sev) {
    case helix::ToolCheck::Severity::Ok:
        return {"check", "success"};
    case helix::ToolCheck::Severity::ColorMismatch:
    case helix::ToolCheck::Severity::MaterialMismatch:
        return {"alert", "warning"};
    case helix::ToolCheck::Severity::EmptySlot:
        return {"close", "danger"};
    }
    return {"check", "success"};
}

// Look up the actually-seated slot for a ToolCheck by its mapped_slot index.
// Returns nullptr when the tool maps to no slot or the slot isn't present.
const helix::AvailableSlot* find_seated_slot(const std::vector<helix::AvailableSlot>& slots,
                                             const helix::ToolCheck& check) {
    if (check.mapped_slot < 0) {
        return nullptr;
    }
    for (const auto& slot : slots) {
        if (slot.slot_index == check.mapped_slot) {
            return &slot;
        }
    }
    return nullptr;
}

} // namespace

void PreflightCheckModal::on_show() {
    // Wire the three action buttons programmatically (mirrors the spaghetti /
    // runout-guidance modals). No XML callbacks on these buttons, so there's
    // no double-wiring.
    //   btn_primary   → on_ok()       → Print Anyway
    //   btn_secondary → on_cancel()   → Cancel
    //   btn_tertiary  → on_tertiary() → Remap…
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
    wire_tertiary_button("btn_tertiary");

    // Remap is only offered when the active backend can actually remap — which
    // is the declared route AND its readiness, not the route alone. A backend
    // built to remap through a firmware object it has not discovered yet (AD5X
    // IFS before `_IFS_VARS`) would otherwise be offered a write that is
    // silently dropped at print start.
    bool remap_supported = false;
    if (auto* backend = AmsState::instance().get_backend()) {
        remap_supported = helix::printer::can_remap(*backend);
    }
    if (auto* remap_btn = find_widget("btn_tertiary")) {
        if (remap_supported) {
            lv_obj_remove_flag(remap_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(remap_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_t* list = find_widget("preflight_tool_list");
    if (!list) {
        spdlog::warn("[PreflightCheckModal] preflight_tool_list widget not found");
        return;
    }
    build_rows(list);

    // Explanation line: describe the first blocking (empty-slot) check.
    if (auto* explain = find_widget("preflight_explanation")) {
        std::string text;
        for (const auto& check : result_.checks) {
            if (check.severity == helix::ToolCheck::Severity::EmptySlot) {
                char buf[160];
                if (check.mapped_slot < 0) {
                    snprintf(buf, sizeof(buf),
                             lv_tr("T%d has no filament loaded — this print will run "
                                   "out."),
                             check.tool_index);
                } else {
                    snprintf(buf, sizeof(buf),
                             lv_tr("T%d needs filament in slot %d, which is empty — "
                                   "this print will run out."),
                             check.tool_index, check.mapped_slot + 1);
                }
                text = buf;
                break;
            }
        }
        if (text.empty()) {
            text = lv_tr("Some filaments don't match the slicer's intent.");
        }
        lv_label_set_text(explain, text.c_str());
    }

    spdlog::debug("[PreflightCheckModal] Shown with {} checks, remap_supported={}",
                  result_.checks.size(), remap_supported);
}

void PreflightCheckModal::build_rows(lv_obj_t* list) {
    helix::ui::safe_clean_children(list);

    // Seated color/material is not on ToolCheck — fetch the live slots once and
    // pass them down so each row indexes by mapped_slot.
    const auto slots = AmsState::instance().collect_available_slots();

    for (const auto& check : result_.checks) {
        create_tool_row(list, check, slots);
    }
}

lv_obj_t* PreflightCheckModal::create_tool_row(lv_obj_t* list, const helix::ToolCheck& check,
                                               const std::vector<helix::AvailableSlot>& slots) {
    // Component is registered under its filename (preflight_check_tool_row), not
    // its <view name>; lv_xml_create resolves by the registered name.
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(list, "preflight_check_tool_row", nullptr));
    if (!row) {
        return nullptr;
    }

    // Tool label "Tx".
    if (auto* tool_label = lv_obj_find_by_name(row, "tool_label")) {
        char buf[8];
        snprintf(buf, sizeof(buf), "T%d", check.tool_index);
        lv_label_set_text(tool_label, buf);
    }

    // Intended (slicer) color swatch.
    if (auto* intended = lv_obj_find_by_name(row, "intended_swatch")) {
        lv_obj_set_style_bg_color(intended, lv_color_hex(check.intended_color), 0);
    }

    // Seated swatch / EMPTY label. Look up the live slot by mapped_slot.
    const auto* seated = find_seated_slot(slots, check);
    auto* seated_swatch = lv_obj_find_by_name(row, "seated_swatch");
    auto* empty_label = lv_obj_find_by_name(row, "empty_label");

    if (check.slot_present && seated && !seated->is_empty) {
        if (seated_swatch) {
            helix::ui::apply_swatch_color(seated_swatch, seated->color_rgb,
                                          seated->multi_color_hexes);
            lv_obj_remove_flag(seated_swatch, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // Empty / absent slot — show the EMPTY label instead of a swatch.
        if (empty_label) {
            lv_obj_remove_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Severity glyph + color.
    if (auto* sev_icon = lv_obj_find_by_name(row, "severity_icon")) {
        const auto vis = severity_visual(check.severity);
        ui_icon_set_source(sev_icon, vis.icon_src);
        ui_icon_set_variant(sev_icon, vis.variant);
    }

    return row;
}

void PreflightCheckModal::on_hide() {
    // Self-delete the heap object once hidden. ModalStack/LVGL cleanup never
    // calls Modal::~Modal, so the C++ object must free itself here or it leaks
    // on every show. Deferred via async_call so we never delete `this`
    // mid-event. Mirrors SpaghettiDetectionModal::on_hide().
    auto* self = this;
    helix::ui::async_call([](void* data) { delete static_cast<PreflightCheckModal*>(data); }, self);
}

} // namespace helix::ui
