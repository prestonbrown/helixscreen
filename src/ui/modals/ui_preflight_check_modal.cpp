// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_preflight_check_modal.h"

#include "ui_icon.h"
#include "ui_print_preparation_manager.h"
#include "ui_swatch.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "ui/ui_widget_helpers.h"

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
    case helix::ToolCheck::Severity::UnknownMaterial:
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

    // Remap is only offered when a remap can actually be carried out, which is
    // more than the backend's declared route: a route it has not discovered yet
    // (AD5X IFS before `_IFS_VARS`), a job with no tools, or a rewrite that
    // cannot run (no HelixPrint plugin, or a transport that keeps no local copy)
    // all end in a refusal the opener would have to deliver.
    // One tool check per tool, so checks.size() is this job's tool count.
    auto block = helix::printer::RemapBlock::NoStrategy;
    if (auto* backend = AmsState::instance().get_backend()) {
        block = helix::printer::remap_block(*backend,
                                            PrintPreparationManager::gcode_rewrite_block_for(
                                                &get_printer_state(), get_moonraker_api()),
                                            static_cast<int>(result_.checks.size()));
    }
    const bool remap_supported = block == helix::printer::RemapBlock::None;
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
        const auto slots = AmsState::instance().collect_available_slots();
        std::string text;
        for (const auto& check : result_.checks) {
            if (check.severity == helix::ToolCheck::Severity::UnknownMaterial) {
                char buf[192];
                const auto* seated = find_seated_slot(slots, check);
                const std::string lane_text =
                    seated ? helix::ui::lane_label(seated->noun, seated->unit_display_name,
                                                   seated->local_slot_index)
                           : helix::ui::lane_label(helix::ui::LaneNoun::Slot, check.mapped_slot);
                snprintf(buf, sizeof(buf),
                         lv_tr("%s uses %s, which has filament but no material set — the "
                               "printer will not start until you set it."),
                         helix::ui::tool_label(check.tool_index).c_str(), lane_text.c_str());
                text = buf;
                break;
            }
            if (check.severity == helix::ToolCheck::Severity::EmptySlot) {
                char buf[160];
                if (check.mapped_slot < 0) {
                    snprintf(buf, sizeof(buf),
                             lv_tr("%s has no filament loaded — this print will run "
                                   "out."),
                             helix::ui::tool_label(check.tool_index).c_str());
                } else {
                    const auto* seated = find_seated_slot(slots, check);
                    // A seated AvailableSlot carries its own unit and
                    // unit-local number, which agree with the mapping chip
                    // and remap modal; the global mapped_slot does not on a
                    // multi-unit backend. No seated slot to name means no
                    // AvailableSlot at all, so the generic Slot label from
                    // the global index is the best answer left.
                    const std::string lane_text =
                        seated
                            ? helix::ui::lane_label(seated->noun, seated->unit_display_name,
                                                    seated->local_slot_index)
                            : helix::ui::lane_label(helix::ui::LaneNoun::Slot, check.mapped_slot);
                    snprintf(buf, sizeof(buf),
                             lv_tr("%s needs filament in %s, which is empty — "
                                   "this print will run out."),
                             helix::ui::tool_label(check.tool_index).c_str(), lane_text.c_str());
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

    spdlog::debug("[PreflightCheckModal] Shown with {} checks, remap={}", result_.checks.size(),
                  helix::printer::remap_block_name(block));
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
    if (auto* tool_label = helix::ui::find_required(row, "tool_label", get_name())) {
        lv_label_set_text(tool_label, helix::ui::tool_label(check.tool_index).c_str());
    }

    // Intended (slicer) color swatch.
    if (auto* intended = helix::ui::find_required(row, "intended_swatch", get_name())) {
        lv_obj_set_style_bg_color(intended, lv_color_hex(check.intended_color), 0);
    }

    // Seated swatch / EMPTY label. Look up the live slot by mapped_slot.
    const auto* seated = find_seated_slot(slots, check);
    auto* seated_swatch = helix::ui::find_required(row, "seated_swatch", get_name());
    auto* empty_label = helix::ui::find_required(row, "empty_label", get_name());

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
    if (auto* sev_icon = helix::ui::find_required(row, "severity_icon", get_name())) {
        const auto vis = severity_visual(check.severity);
        helix::ui::icon::set_source(sev_icon, vis.icon_src);
        helix::ui::icon::set_variant(sev_icon, vis.variant);
    }

    return row;
}

} // namespace helix::ui
