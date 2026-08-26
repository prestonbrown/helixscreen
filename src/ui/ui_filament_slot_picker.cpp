// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_slot_picker.h"

#include "ui_fonts.h"
#include "ui_swatch.h"
#include "ui_utils.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Layout constants for picker rows.
// Rows are built in C++ (not XML components) because the number varies
// per AMS configuration. lv_obj_add_event_cb and lv_label_set_text are used
// here as allowed exceptions for dynamic content.
namespace {
constexpr lv_opa_t SWATCH_BORDER_OPA = 30;
constexpr lv_opa_t SELECTED_BG_OPA = 40;
/// Empty bays stay selectable (mapping a tool to a bay you are about to load is
/// legitimate) but must read as clearly not-ready at a glance. 60% was too close
/// to a filled row to carry that on a glossy panel.
constexpr lv_opa_t EMPTY_ROW_OPA = LV_OPA_30;
constexpr lv_opa_t BACKDROP_OPA = 128;
constexpr int32_t CARD_MIN_WIDTH = 480;
} // namespace

FilamentSlotPicker::~FilamentSlotPicker() {
    hide();
}

// ============================================================================
// Show / Hide
// ============================================================================

void FilamentSlotPicker::show(lv_obj_t* parent, lv_obj_t* trigger, int tool_index,
                              const std::string& expected_material,
                              const std::vector<helix::AvailableSlot>& slots,
                              const Selection& current, SelectCallback on_select) {
    hide();

    if (!parent || !trigger) {
        return;
    }

    trigger_ = trigger;
    (void)tool_index; // kept in API for caller compatibility; header removed in this redesign
    expected_material_ = expected_material;
    slots_ = slots;
    current_selection_ = current;
    on_select_ = std::move(on_select);

    // Full-screen semi-transparent backdrop (click to dismiss)
    backdrop_ = lv_obj_create(parent);
    lv_obj_remove_style_all(backdrop_);
    lv_obj_set_size(backdrop_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop_, BACKDROP_OPA, 0);
    lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(
        backdrop_,
        [](lv_event_t* e) {
            auto* self = static_cast<FilamentSlotPicker*>(lv_event_get_user_data(e));
            self->hide();
        },
        LV_EVENT_CLICKED, this);

    // Card container with scrollable slot list. Gap between rows is visible
    // — don't set it tighter than space_xs or rows appear crammed.
    int32_t card_pad = theme_manager_get_spacing("space_md");
    int32_t card_gap = theme_manager_get_spacing("space_xs");
    int32_t card_radius = theme_manager_get_spacing("border_radius");

    lv_obj_t* card = lv_obj_create(backdrop_);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, LV_PCT(60), 0);
    lv_obj_set_style_min_width(card, CARD_MIN_WIDTH, 0);
    lv_obj_set_style_bg_color(card, theme_manager_get_color("elevated_bg"), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, card_radius, 0);
    lv_obj_set_style_pad_all(card, card_pad, 0);
    lv_obj_set_style_pad_gap(card, card_gap, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    // Slot rows (no Auto option — user is manually selecting)
    for (size_t i = 0; i < slots_.size(); ++i) {
        create_slot_row(card, static_cast<int>(i), slots_[i]);
    }

    // Position anchored to trigger widget
    position_card(card);

    // Highlight the trigger to show it's active
    lv_obj_set_style_border_color(trigger_, theme_manager_get_color("primary"), 0);
    lv_obj_set_style_border_opa(trigger_, LV_OPA_50, 0);

    spdlog::debug("[FilamentSlotPicker] Shown with {} slots", slots_.size());
}

void FilamentSlotPicker::hide() {
    if (!backdrop_) {
        return;
    }

    // Restore trigger border before clearing reference
    if (trigger_) {
        lv_obj_set_style_border_color(trigger_, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_border_opa(trigger_, SWATCH_BORDER_OPA, 0);
    }

    helix::ui::safe_delete_deferred(backdrop_);
    trigger_ = nullptr;
    spdlog::debug("[FilamentSlotPicker] Hidden");
}

// ============================================================================
// Row creation
// ============================================================================

void FilamentSlotPicker::create_slot_row(lv_obj_t* list, int index,
                                         const helix::AvailableSlot& slot) {
    // Row layout/padding/fonts live in
    // ui_xml/components/filament_slot_picker_row.xml — tune visuals there
    // without rebuilding. C++ fills in colors, label text, warn visibility,
    // and the selected-state highlight.
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(list, "filament_slot_picker_row", nullptr));
    if (!row) {
        return;
    }

    if (slot.is_empty) {
        lv_obj_set_style_opa(row, EMPTY_ROW_OPA, 0);
    }

    bool is_selected = !current_selection_.is_auto &&
                       slot.slot_index == current_selection_.slot_index &&
                       slot.backend_index == current_selection_.backend_index;
    if (is_selected) {
        lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), 0);
        lv_obj_set_style_bg_opa(row, SELECTED_BG_OPA, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, theme_manager_get_color("primary"), 0);
    }

    if (auto* swatch = lv_obj_find_by_name(row, "swatch")) {
        if (slot.is_empty) {
            lv_obj_set_style_bg_opa(swatch, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(swatch, 2, 0);
            lv_obj_set_style_border_color(swatch, theme_manager_get_color("warning"), 0);
            lv_obj_set_style_border_opa(swatch, LV_OPA_COVER, 0);
        } else {
            helix::ui::apply_swatch_color(swatch, slot.color_rgb, slot.multi_color_hexes);
        }
    }

    if (auto* label = lv_obj_find_by_name(row, "slot_label")) {
        lv_label_set_text(label, helix::FilamentMapper::format_slot_label(slot).c_str());
    }

    if (!slot.is_empty && !expected_material_.empty() && !slot.material.empty() &&
        !helix::FilamentMapper::materials_match(expected_material_, slot.material)) {
        if (auto* warn = lv_obj_find_by_name(row, "warn_icon")) {
            lv_obj_remove_flag(warn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Store slot index for click lookup
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));

    // Click → instant select and dismiss
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* self = static_cast<FilamentSlotPicker*>(lv_event_get_user_data(e));
            lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
            if (idx >= 0 && idx < static_cast<int>(self->slots_.size())) {
                const auto& s = self->slots_[static_cast<size_t>(idx)];
                self->select({s.slot_index, s.backend_index, false});
            }
        },
        LV_EVENT_CLICKED, this);
}

// ============================================================================
// Selection
// ============================================================================

void FilamentSlotPicker::select(const Selection& sel) {
    // Copy callback and hide before invoking — hide() clears on_select_
    SelectCallback cb = std::move(on_select_);
    hide();
    if (cb) {
        cb(sel);
    }
}

// ============================================================================
// Positioning
// ============================================================================

void FilamentSlotPicker::position_card(lv_obj_t* card) {
    lv_obj_update_layout(card);

    int32_t card_h = lv_obj_get_height(card);
    int32_t card_w = lv_obj_get_width(card);

    // Get trigger position in screen coordinates
    lv_area_t trigger_area;
    lv_obj_get_coords(trigger_, &trigger_area);

    int32_t screen_h = LV_VER_RES;
    int32_t trigger_w = lv_area_get_width(&trigger_area);

    // Width: match trigger, expand if content is wider
    if (card_w < trigger_w) {
        lv_obj_set_width(card, trigger_w);
        lv_obj_update_layout(card);
        card_w = trigger_w;
        card_h = lv_obj_get_height(card);
    }

    // lv_dropdown positioning logic: prefer below, flip if more space above
    int32_t space_below = screen_h - trigger_area.y2 - 1;
    int32_t space_above = trigger_area.y1;

    bool open_above = false;
    int32_t list_h = card_h;

    if (list_h > space_below) {
        if (space_above > space_below) {
            // More space above — flip
            open_above = true;
            if (list_h > space_above) {
                list_h = space_above;
            }
        } else {
            // Stay below, clamp
            list_h = space_below;
        }
    }

    // Never exceed content height
    if (list_h > card_h) {
        list_h = card_h;
    }

    lv_obj_set_height(card, list_h);

    // Position: align to trigger edges
    int32_t card_x = trigger_area.x1;
    int32_t card_y;

    if (open_above) {
        card_y = trigger_area.y1 - list_h;
    } else {
        card_y = trigger_area.y2 + 1;
    }

    // Clamp horizontal position to screen bounds
    int32_t screen_w = LV_HOR_RES;
    if (card_x + card_w > screen_w) {
        card_x = screen_w - card_w;
    }
    if (card_x < 0) {
        card_x = 0;
    }

    // Convert to backdrop-local coordinates
    lv_area_t backdrop_area;
    lv_obj_get_coords(backdrop_, &backdrop_area);
    card_x -= backdrop_area.x1;
    card_y -= backdrop_area.y1;

    lv_obj_set_pos(card, card_x, card_y);

    // Corner radius
    int32_t radius = theme_manager_get_spacing("border_radius");
    lv_obj_set_style_radius(card, radius, 0);

    // Scroll to selected item
    scroll_to_selected(card);

    spdlog::debug("[FilamentSlotPicker] Trigger({},{} {}x{}) -> Card({},{}) {}x{} {}",
                  trigger_area.x1, trigger_area.y1, trigger_w, lv_area_get_height(&trigger_area),
                  card_x, card_y, card_w, list_h, open_above ? "above" : "below");
}

void FilamentSlotPicker::scroll_to_selected(lv_obj_t* card) {
    if (!card || current_selection_.is_auto) {
        return;
    }

    // Find the selected row and scroll it into view
    uint32_t child_count = lv_obj_get_child_count(card);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* child = lv_obj_get_child(card, static_cast<int32_t>(i));
        int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(child)));
        if (idx >= 0 && idx < static_cast<int>(slots_.size())) {
            const auto& slot = slots_[static_cast<size_t>(idx)];
            if (slot.slot_index == current_selection_.slot_index &&
                slot.backend_index == current_selection_.backend_index) {
                lv_obj_scroll_to_view(child, LV_ANIM_OFF);
                break;
            }
        }
    }
}

} // namespace helix::ui
