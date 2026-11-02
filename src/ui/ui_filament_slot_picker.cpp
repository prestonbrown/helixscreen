// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filament_slot_picker.h"

#include "ui_utils.h"

#include "filament_chip_model.h"
#include "filament_variants.h"
#include "helix/xml/indexed_subject_pool.h"
#include "helix_psram_attr.h"
#include "lvgl.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace helix::ui {

namespace {

constexpr lv_opa_t kSwatchBorderOpa = 30;

using helix::xml::IndexedSubjectPool;

HELIX_PSRAM_BSS lv_subject_t s_title{};
HELIX_PSRAM_BSS char s_title_buf[128];
HELIX_PSRAM_BSS lv_subject_t s_has_title{};
HELIX_PSRAM_BSS lv_subject_t s_row_count{};
HELIX_PSRAM_BSS lv_subject_t s_has_reasons{};
HELIX_PSRAM_BSS lv_subject_t s_offer_auto{};
HELIX_PSRAM_BSS lv_subject_t s_auto_selected{};
HELIX_PSRAM_BSS lv_subject_t s_auto_text{};
HELIX_PSRAM_BSS char s_auto_text_buf[48];
HELIX_PSRAM_BSS lv_subject_t s_has_extra{};
HELIX_PSRAM_BSS lv_subject_t s_extra_text{};
HELIX_PSRAM_BSS char s_extra_text_buf[96];
HELIX_PSRAM_BSS lv_subject_t s_has_cancel{};
HELIX_PSRAM_BSS lv_subject_t s_cancel_text{};
HELIX_PSRAM_BSS char s_cancel_text_buf[64];
helix::WidgetSubjects s_subjects{"FilamentSlotPickerSubjects"};

// Per slot row, in display order: filament_picker_row_<i>_label and so on.
IndexedSubjectPool s_row_label{"filament_picker_row", IndexedSubjectPool::Type::String, 64,
                               "_label"};
IndexedSubjectPool s_row_material{"filament_picker_row", IndexedSubjectPool::Type::String, 32,
                                  "_material"};
IndexedSubjectPool s_row_reason{"filament_picker_row", IndexedSubjectPool::Type::String, 64,
                                "_reason"};
IndexedSubjectPool s_row_color{"filament_picker_row", IndexedSubjectPool::Type::Color, 0, "_color"};
IndexedSubjectPool s_row_colors{"filament_picker_row", IndexedSubjectPool::Type::String, 96,
                                "_colors"};
IndexedSubjectPool s_row_finish{"filament_picker_row", IndexedSubjectPool::Type::String, 32,
                                "_finish"};
IndexedSubjectPool s_row_selected{"filament_picker_row", IndexedSubjectPool::Type::Int, 0,
                                  "_selected"};
IndexedSubjectPool s_row_empty{"filament_picker_row", IndexedSubjectPool::Type::Int, 0, "_empty"};
IndexedSubjectPool s_row_warn{"filament_picker_row", IndexedSubjectPool::Type::Int, 0, "_warn"};

IndexedSubjectPool* const s_pools[] = {&s_row_label,    &s_row_material, &s_row_reason,
                                       &s_row_color,    &s_row_colors,   &s_row_finish,
                                       &s_row_selected, &s_row_empty,    &s_row_warn};

/// The picker on screen; the XML callbacks are global, so they reach it here.
FilamentSlotPicker* s_shown = nullptr;

void init_subjects() {
    s_subjects.ensure_init(
        [] {
            UI_MANAGED_SUBJECT_STRING(s_title, s_title_buf, "", "filament_picker_title",
                                      s_subjects);
            UI_MANAGED_SUBJECT_INT(s_has_title, 0, "filament_picker_has_title", s_subjects);
            UI_MANAGED_SUBJECT_INT(s_row_count, 0, "filament_picker_row_count", s_subjects);
            UI_MANAGED_SUBJECT_INT(s_has_reasons, 0, "filament_picker_has_reasons", s_subjects);
            UI_MANAGED_SUBJECT_INT(s_offer_auto, 0, "filament_picker_offer_auto", s_subjects);
            UI_MANAGED_SUBJECT_INT(s_auto_selected, 0, "filament_picker_auto_selected", s_subjects);
            UI_MANAGED_SUBJECT_STRING(s_auto_text, s_auto_text_buf, "", "filament_picker_auto_text",
                                      s_subjects);
            UI_MANAGED_SUBJECT_INT(s_has_extra, 0, "filament_picker_has_extra", s_subjects);
            UI_MANAGED_SUBJECT_STRING(s_extra_text, s_extra_text_buf, "",
                                      "filament_picker_extra_text", s_subjects);
            UI_MANAGED_SUBJECT_INT(s_has_cancel, 0, "filament_picker_has_cancel", s_subjects);
            UI_MANAGED_SUBJECT_STRING(s_cancel_text, s_cancel_text_buf, "",
                                      "filament_picker_cancel_text", s_subjects);
            for (IndexedSubjectPool* pool : s_pools) {
                pool->ensure_size(FilamentSlotPicker::kMaxPickerRows);
            }
        },
        [] {
            if (s_shown) {
                s_shown->hide();
            }
            for (IndexedSubjectPool* pool : s_pools) {
                pool->reclaim();
            }
        });
    // Last write wins, so registering on every show is harmless and survives an
    // XML registry rebuilt between tests.
    lv_xml_register_event_cb(nullptr, "filament_picker_row_cb", FilamentSlotPicker::row_cb);
    lv_xml_register_event_cb(nullptr, "filament_picker_backdrop_cb",
                             FilamentSlotPicker::backdrop_cb);
}

bool same_slot(const helix::AvailableSlot& slot, const FilamentSlotPicker::Selection& sel) {
    return !sel.is_auto && slot.slot_index == sel.slot_index &&
           slot.backend_index == sel.backend_index;
}

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
    (void)tool_index; // kept in API for caller compatibility; the card has no heading
    show_impl(parent, trigger, slots, current, std::move(on_select), Options{}, expected_material);
}

void FilamentSlotPicker::show(lv_obj_t* parent, lv_obj_t* trigger,
                              const std::vector<helix::AvailableSlot>& slots,
                              const Selection& current, SelectCallback on_select,
                              const Options& options) {
    show_impl(parent, trigger, slots, current, std::move(on_select), options, {});
}

void FilamentSlotPicker::show_impl(lv_obj_t* parent, lv_obj_t* trigger,
                                   const std::vector<helix::AvailableSlot>& slots,
                                   const Selection& current, SelectCallback on_select,
                                   const Options& options, const std::string& expected_material) {
    hide();
    if (s_shown) {
        s_shown->hide();
    }

    if (!parent || !trigger) {
        return;
    }

    init_subjects();

    trigger_ = trigger;
    trigger_highlighted_ = options.highlight_trigger;
    slots_ = slots;
    expected_material_ = expected_material;
    current_selection_ = current;
    on_select_ = std::move(on_select);

    rows_.clear();
    std::vector<std::string> reasons;
    if (options.order.empty()) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            rows_.push_back(i);
        }
    } else {
        for (std::size_t k = 0; k < options.order.size(); ++k) {
            if (options.order[k] < slots_.size()) {
                rows_.push_back(options.order[k]);
                reasons.push_back(k < options.reasons.size() ? options.reasons[k] : std::string{});
            }
        }
    }
    if (rows_.size() > kMaxPickerRows) {
        spdlog::warn("[FilamentSlotPicker] {} slots, listing the first {}", rows_.size(),
                     kMaxPickerRows);
        rows_.resize(kMaxPickerRows);
    }

    publish(options, reasons);

    backdrop_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "filament_slot_picker", nullptr));
    if (!backdrop_) {
        spdlog::error("[FilamentSlotPicker] filament_slot_picker component failed to create");
        return;
    }
    s_shown = this;

    if (lv_obj_t* card = lv_obj_find_by_name(backdrop_, "filament_picker_card")) {
        fit_card_width(card);
        position_card(card);
    }

    // Outline the trigger while the picker is open.
    if (trigger_highlighted_) {
        lv_obj_set_style_border_color(trigger_, theme_manager_get_color("primary"), 0);
        lv_obj_set_style_border_opa(trigger_, LV_OPA_50, 0);
    }

    spdlog::debug("[FilamentSlotPicker] Shown with {} of {} slots", rows_.size(), slots_.size());
}

void FilamentSlotPicker::publish(const Options& options, const std::vector<std::string>& reasons) {
    lv_subject_copy_string(&s_title, options.title.c_str());
    lv_subject_set_int(&s_has_title, options.title.empty() ? 0 : 1);
    lv_subject_set_int(&s_has_reasons, reasons.empty() ? 0 : 1);
    lv_subject_set_int(&s_offer_auto, options.offer_auto ? 1 : 0);
    lv_subject_set_int(&s_auto_selected, options.offer_auto && current_selection_.is_auto ? 1 : 0);
    lv_subject_copy_string(&s_auto_text, lv_tr("Auto"));
    lv_subject_set_int(&s_has_extra, options.extra_choice.empty() ? 0 : 1);
    lv_subject_copy_string(&s_extra_text, options.extra_choice.c_str());
    lv_subject_set_int(&s_has_cancel, options.cancel_choice.empty() ? 0 : 1);
    lv_subject_copy_string(&s_cancel_text, options.cancel_choice.c_str());

    for (std::size_t k = 0; k < rows_.size(); ++k) {
        const helix::AvailableSlot& slot = slots_[rows_[k]];
        s_row_label.set_string(k, helix::slot_row_label(slot, slots_));
        s_row_material.set_string(k, slot.is_empty ? std::string(lv_tr("Empty")) : slot.material);
        s_row_reason.set_string(k, k < reasons.size() ? reasons[k] : std::string{});
        s_row_color.set_color(k, slot.color_rgb);
        s_row_colors.set_string(k, slot.multi_color_hexes);
        s_row_finish.set_string(k, helix::filament_finish_name(slot.finish));
        s_row_selected.set_int(k, same_slot(slot, current_selection_) ? 1 : 0);
        s_row_empty.set_int(k, slot.is_empty ? 1 : 0);
        s_row_warn.set_int(
            k, !slot.is_empty && !expected_material_.empty() && !slot.material.empty() &&
                       !helix::FilamentMapper::materials_match(expected_material_, slot.material)
                   ? 1
                   : 0);
    }
    // Last: the count rebuilds the rows, which then bind the values above.
    lv_subject_set_int(&s_row_count, static_cast<int>(rows_.size()));
}

void FilamentSlotPicker::hide() {
    if (s_shown == this) {
        s_shown = nullptr;
    }
    if (!backdrop_) {
        return;
    }

    // Restore trigger border before clearing reference
    if (trigger_ && trigger_highlighted_) {
        lv_obj_set_style_border_color(trigger_, theme_manager_get_color("text_muted"), 0);
        lv_obj_set_style_border_opa(trigger_, kSwatchBorderOpa, 0);
    }

    helix::ui::safe_delete_deferred(backdrop_);
    trigger_ = nullptr;
    spdlog::debug("[FilamentSlotPicker] Hidden");
}

// ============================================================================
// Taps
// ============================================================================

void FilamentSlotPicker::row_cb(lv_event_t* e) {
    // The row's choice: its index, or "auto", "extra", "cancel".
    const char* choice = static_cast<const char*>(lv_event_get_user_data(e));
    if (s_shown && choice) {
        s_shown->on_row(choice);
    }
}

void FilamentSlotPicker::backdrop_cb(lv_event_t* /*e*/) {
    if (s_shown) {
        s_shown->hide();
    }
}

void FilamentSlotPicker::on_row(const char* choice) {
    Selection sel;
    if (std::strcmp(choice, "cancel") == 0) {
        hide();
        return;
    }
    if (std::strcmp(choice, "auto") == 0) {
        sel.is_auto = true;
    } else if (std::strcmp(choice, "extra") == 0) {
        sel.is_extra = true;
    } else {
        const long k = std::strtol(choice, nullptr, 10);
        if (k < 0 || static_cast<std::size_t>(k) >= rows_.size()) {
            return;
        }
        const helix::AvailableSlot& slot = slots_[rows_[static_cast<std::size_t>(k)]];
        sel.slot_index = slot.slot_index;
        sel.backend_index = slot.backend_index;
    }
    select(sel);
}

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

void FilamentSlotPicker::fit_card_width(lv_obj_t* card) {
    // Rows fill the card (width 100%), and a percentage child gives a
    // content-sized parent nothing to measure. Lay the children out at their
    // natural width once, take the card's width from that, then let them fill
    // it again. The card never runs past the screen less a margin each side;
    // a row wider than that ellipsizes its label.
    // A growing label claims only leftover space, so it adds nothing to a
    // content-sized row; it stops growing for the measure.
    std::vector<lv_obj_t*> filled;
    const auto collect = [&filled](lv_obj_t* parent) {
        const uint32_t count = lv_obj_get_child_count(parent);
        for (uint32_t i = 0; i < count; ++i) {
            filled.push_back(lv_obj_get_child(parent, static_cast<int32_t>(i)));
        }
    };
    collect(card);
    if (lv_obj_t* rows = lv_obj_find_by_name(card, "filament_picker_rows")) {
        collect(rows);
    }
    std::vector<std::pair<lv_obj_t*, uint8_t>> grown;
    for (lv_obj_t* child : filled) {
        lv_obj_set_width(child, LV_SIZE_CONTENT);
        if (lv_obj_t* label = lv_obj_find_by_name(child, "slot_label")) {
            grown.emplace_back(label, lv_obj_get_style_flex_grow(label, LV_PART_MAIN));
            lv_obj_set_flex_grow(label, 0);
        }
    }
    lv_obj_update_layout(card);
    const int32_t margin = theme_manager_get_spacing("space_md");
    const int32_t max_w = LV_HOR_RES - 2 * margin;
    const int32_t natural_w = lv_obj_get_width(card);
    for (lv_obj_t* child : filled) {
        lv_obj_set_width(child, LV_PCT(100));
    }
    for (const auto& [label, grow] : grown) {
        lv_obj_set_flex_grow(label, grow);
    }
    lv_obj_set_width(card, std::min(natural_w, max_w));
}

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

    // Prefer below the trigger, then above it, as lv_dropdown does. A card
    // taller than either side keeps its full height and slides along the
    // screen instead, covering the trigger: every row visible beats staying
    // clear of the button that opened it. Only a card taller than the screen
    // scrolls.
    int32_t space_below = screen_h - trigger_area.y2 - 1;
    int32_t space_above = trigger_area.y1;

    int32_t list_h = card_h > screen_h ? screen_h : card_h;
    lv_obj_set_height(card, list_h);

    int32_t card_y;
    if (list_h <= space_below) {
        card_y = trigger_area.y2 + 1;
    } else if (list_h <= space_above) {
        card_y = trigger_area.y1 - list_h;
    } else {
        card_y = std::clamp(trigger_area.y1, int32_t{0}, screen_h - list_h);
    }
    const bool open_above = card_y < trigger_area.y1;

    // Position: align to trigger edges
    int32_t card_x = trigger_area.x1;

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

    scroll_to_selected(card);

    spdlog::debug("[FilamentSlotPicker] Trigger({},{} {}x{}) -> Card({},{}) {}x{} {}",
                  trigger_area.x1, trigger_area.y1, trigger_w, lv_area_get_height(&trigger_area),
                  card_x, card_y, card_w, list_h, open_above ? "above" : "below");
}

void FilamentSlotPicker::scroll_to_selected(lv_obj_t* card) {
    if (!card || current_selection_.is_auto) {
        return;
    }
    for (std::size_t k = 0; k < rows_.size(); ++k) {
        if (same_slot(slots_[rows_[k]], current_selection_)) {
            const std::string name = "filament_picker_row_" + std::to_string(k);
            if (lv_obj_t* row = lv_obj_find_by_name(card, name.c_str())) {
                lv_obj_scroll_to_view(row, LV_ANIM_OFF);
            }
            return;
        }
    }
}

} // namespace helix::ui
