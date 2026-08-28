// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_mapping_modal.h"

#include "ui_fonts.h"
#include "ui_swatch.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Layout constants for dynamically created rows.
// Rows are built in C++ (not XML components) because the number of rows
// varies per file. lv_obj_add_event_cb and lv_label_set_text are used here
// as allowed exceptions for dynamic content.
namespace {
constexpr int TOOL_LABEL_MIN_W = 32;
constexpr lv_opa_t SWATCH_BORDER_OPA = 30;
} // namespace

// ============================================================================
// Configuration
// ============================================================================

void FilamentMappingModal::set_tool_info(const std::vector<helix::GcodeToolInfo>& tools) {
    tool_info_ = tools;
}

void FilamentMappingModal::set_available_slots(const std::vector<helix::AvailableSlot>& slots) {
    available_slots_ = slots;
}

void FilamentMappingModal::set_mappings(std::vector<helix::ToolMapping> mappings) {
    mappings_ = std::move(mappings);
}

void FilamentMappingModal::set_on_mappings_updated(MappingsUpdatedCallback cb) {
    on_updated_cb_ = std::move(cb);
}

// ============================================================================
// Modal lifecycle
// ============================================================================

void FilamentMappingModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    auto_color_map_ = SettingsManager::instance().get_auto_color_map();

    // Snapshot so Cancel can revert. BOTH halves: the toggle writes the
    // preference straight through to SettingsManager (it has to — the rows
    // rebuild in the new mode immediately), which makes changing it a side
    // effect of merely LOOKING at what the other mode would do.
    original_mappings_ = mappings_;
    original_auto_color_map_ = auto_color_map_;

    tool_list_ = find_widget("mapping_tool_list");
    if (!tool_list_) {
        spdlog::warn("[FilamentMappingModal] Could not find mapping_tool_list widget");
        return;
    }

    rebuild_rows();

    spdlog::debug("[FilamentMappingModal] Shown with {} tools, {} slots, auto_color_map={}",
                  tool_info_.size(), available_slots_.size(), auto_color_map_);
}

void FilamentMappingModal::on_ok() {
    if (on_updated_cb_) {
        on_updated_cb_(mappings_);
    }
    hide();
}

void FilamentMappingModal::on_cancel() {
    mappings_ = original_mappings_;

    // The persisted preference is part of what Cancel undoes. Without this the
    // toggle was a one-way door: flip it to see the other mode, press Cancel,
    // and the printer kept the new preference for every future print — on every
    // backend that can reach this picker. Written only when it actually changed
    // so a plain Cancel does not churn the settings file.
    if (auto_color_map_ != original_auto_color_map_) {
        SettingsManager::instance().set_auto_color_map(original_auto_color_map_);
        auto_color_map_ = original_auto_color_map_;
    }

    hide();
}

// ============================================================================
// Row building
// ============================================================================

void FilamentMappingModal::rebuild_rows() {
    if (!tool_list_) {
        return;
    }

    // Hide picker before destroying trigger widgets it may reference
    slot_picker_.hide();

    helix::ui::safe_clean_children(tool_list_);
    toggle_switch_ = nullptr;

    trigger_widgets_.clear();
    trigger_widgets_.resize(mappings_.size(), nullptr);

    // Toggle row first, then tool rows
    create_toggle_row();

    for (size_t i = 0; i < mappings_.size(); ++i) {
        create_tool_row(static_cast<int>(i));
    }
}

lv_obj_t* FilamentMappingModal::create_tool_row(int tool_index) {
    if (!tool_list_ || tool_index < 0 || tool_index >= static_cast<int>(mappings_.size()) ||
        tool_index >= static_cast<int>(tool_info_.size())) {
        return nullptr;
    }

    const auto& mapping = mappings_[static_cast<size_t>(tool_index)];
    const auto& tool = tool_info_[static_cast<size_t>(tool_index)];

    // Row layout lives in ui_xml/components/filament_mapping_tool_row.xml;
    // everything visual (sizes, padding, fonts, radii) is editable there
    // without rebuilding. C++ only sets colors, label text, and show/hide
    // on the conditional children (Tx label, material label, chosen swatch,
    // warning, chevron).
    auto* row =
        static_cast<lv_obj_t*>(lv_xml_create(tool_list_, "filament_mapping_tool_row", nullptr));
    if (!row) {
        return nullptr;
    }

    lv_color_t gcode_color = lv_color_hex(tool.color_rgb);
    if (auto* expected_swatch = lv_obj_find_by_name(row, "expected_swatch")) {
        lv_obj_set_style_bg_color(expected_swatch, gcode_color, 0);
    }

    if (auto* tool_label = lv_obj_find_by_name(row, "tool_label")) {
        if (tool_info_.size() > 1) {
            char tool_buf[8];
            snprintf(tool_buf, sizeof(tool_buf), "T%d", tool.tool_index);
            lv_label_set_text(tool_label, tool_buf);
            lv_obj_set_style_text_color(tool_label, theme_manager_get_contrast_color(gcode_color),
                                        0);
            lv_obj_remove_flag(tool_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (auto* mat_label = lv_obj_find_by_name(row, "material_label")) {
        if (!tool.material.empty()) {
            lv_label_set_text(mat_label, tool.material.c_str());
            lv_obj_remove_flag(mat_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const auto* mapped = find_mapped_slot(mapping);
    auto* trigger = lv_obj_find_by_name(row, "trigger");
    trigger_widgets_[static_cast<size_t>(tool_index)] = trigger;

    if (auto* chosen_swatch = lv_obj_find_by_name(row, "chosen_swatch")) {
        if (mapped && !mapped->is_empty) {
            helix::ui::apply_swatch_color(chosen_swatch, mapped->color_rgb,
                                          mapped->multi_color_hexes);
            lv_obj_remove_flag(chosen_swatch, LV_OBJ_FLAG_HIDDEN);
        } else if (mapped && mapped->is_empty) {
            lv_obj_set_style_bg_opa(chosen_swatch, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(chosen_swatch, 2, 0);
            lv_obj_set_style_border_color(chosen_swatch, theme_manager_get_color("warning"), 0);
            lv_obj_set_style_border_opa(chosen_swatch, LV_OPA_COVER, 0);
            lv_obj_remove_flag(chosen_swatch, LV_OBJ_FLAG_HIDDEN);
        }
        // Auto/Unmapped (mapped == nullptr) leaves the swatch hidden.
    }

    if (auto* slot_text = lv_obj_find_by_name(row, "slot_text")) {
        lv_label_set_text(slot_text, get_slot_display_text(mapping).c_str());
    }

    bool mapped_to_empty = mapped && mapped->is_empty;
    if (mapping.material_mismatch || mapped_to_empty) {
        if (auto* warn = lv_obj_find_by_name(row, "trigger_warn")) {
            lv_obj_remove_flag(warn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!auto_color_map_) {
        if (auto* chevron = lv_obj_find_by_name(row, "trigger_chevron")) {
            lv_obj_remove_flag(chevron, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (auto_color_map_) {
        lv_obj_set_style_opa(row, LV_OPA_50, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* self = static_cast<FilamentMappingModal*>(lv_event_get_user_data(e));
                lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                int idx =
                    static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                self->on_row_tapped(idx);
            },
            LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(tool_index)));
    }

    return row;
}

// ============================================================================
// Display text helpers
// ============================================================================

const helix::AvailableSlot*
FilamentMappingModal::find_mapped_slot(const helix::ToolMapping& mapping) const {
    if (mapping.is_auto || mapping.mapped_slot < 0) {
        return nullptr;
    }
    for (const auto& slot : available_slots_) {
        if (slot.slot_index == mapping.mapped_slot &&
            slot.backend_index == mapping.mapped_backend) {
            return &slot;
        }
    }
    return nullptr;
}

std::string FilamentMappingModal::get_slot_display_text(const helix::ToolMapping& mapping) const {
    if (mapping.is_auto) {
        return lv_tr("Auto");
    }

    if (mapping.mapped_slot < 0) {
        return lv_tr("Unmapped");
    }

    const auto* slot = find_mapped_slot(mapping);
    if (slot) {
        return helix::FilamentMapper::format_slot_label(*slot);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%s %d", lv_tr("Slot"), mapping.mapped_slot + 1);
    return buf;
}

// ============================================================================
// Row tap -> slot picker
// ============================================================================

void FilamentMappingModal::on_row_tapped(int tool_index) {
    if (tool_index < 0 || tool_index >= static_cast<int>(tool_info_.size())) {
        return;
    }

    const auto& tool = tool_info_[static_cast<size_t>(tool_index)];
    const auto& mapping = mappings_[static_cast<size_t>(tool_index)];

    spdlog::debug("[FilamentMappingModal] Row tapped: T{}", tool_index);

    FilamentSlotPicker::Selection current{mapping.mapped_slot, mapping.mapped_backend,
                                          mapping.is_auto};

    lv_obj_t* trigger = trigger_widgets_[static_cast<size_t>(tool_index)];

    slot_picker_.show(lv_screen_active(), trigger, tool_index, tool.material, available_slots_,
                      current, [this, tool_index](const FilamentSlotPicker::Selection& sel) {
                          on_slot_selected(tool_index, sel);
                      });
}

void FilamentMappingModal::on_slot_selected(int tool_index,
                                            const FilamentSlotPicker::Selection& selection) {
    if (tool_index < 0 || tool_index >= static_cast<int>(mappings_.size())) {
        return;
    }

    auto& mapping = mappings_[static_cast<size_t>(tool_index)];
    mapping.mapped_slot = selection.slot_index;
    mapping.mapped_backend = selection.backend_index;
    mapping.is_auto = selection.is_auto;

    mapping.material_mismatch = false;
    if (selection.is_auto) {
        mapping.reason = helix::ToolMapping::MatchReason::AUTO;
    } else {
        const auto& tool = tool_info_[static_cast<size_t>(tool_index)];
        const auto* slot = find_mapped_slot(mapping);
        if (slot && !tool.material.empty() && !slot->material.empty() &&
            !helix::FilamentMapper::materials_match(tool.material, slot->material)) {
            mapping.material_mismatch = true;
        }
    }

    spdlog::info("[FilamentMappingModal] T{} mapped to: auto={}, slot={}, backend={}", tool_index,
                 selection.is_auto, selection.slot_index, selection.backend_index);

    // Rebuild UI to reflect changes
    rebuild_rows();
}

// ============================================================================
// Toggle: keep current assignments
// ============================================================================

void FilamentMappingModal::create_toggle_row() {
    if (!tool_list_) {
        return;
    }

    int32_t row_pad_v = theme_manager_get_spacing("space_sm");

    // Row container
    lv_obj_t* row = lv_obj_create(tool_list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(row, row_pad_v, 0);
    lv_obj_set_style_pad_hor(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Label
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, lv_tr("Map to closest colors with matching material"));
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);

    // Switch
    toggle_switch_ = lv_switch_create(row);
    if (auto_color_map_) {
        lv_obj_add_state(toggle_switch_, LV_STATE_CHECKED);
    }

    lv_obj_add_event_cb(
        toggle_switch_,
        [](lv_event_t* e) {
            auto* self = static_cast<FilamentMappingModal*>(lv_event_get_user_data(e));
            bool checked = lv_obj_has_state(self->toggle_switch_, LV_STATE_CHECKED);
            self->on_toggle_changed(checked);
        },
        LV_EVENT_VALUE_CHANGED, this);
}

void FilamentMappingModal::on_toggle_changed(bool auto_color) {
    auto_color_map_ = auto_color;
    SettingsManager::instance().set_auto_color_map(auto_color);
    recalculate_mappings();
    rebuild_rows();
}

void FilamentMappingModal::recalculate_mappings() {
    if (auto_color_map_) {
        // Color matching: clear firmware mappings so they don't override color matches
        auto slots_for_matching = available_slots_;
        for (auto& s : slots_for_matching) {
            s.current_tool_mapping = -1;
        }
        mappings_ = helix::FilamentMapper::compute_defaults(tool_info_, slots_for_matching);
    } else {
        // Positional assignment (T0→slot 0, T1→slot 1, etc.)
        mappings_ = helix::FilamentMapper::use_current_assignments(
            tool_info_, available_slots_, AmsState::instance().collect_firmware_routing());
    }
}

} // namespace helix::ui
