// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control_buttons_widget.h"

#include "ui_button.h"

#include "grid_layout.h"
#include "helix/ui/text_metrics.h"
#include "lvgl.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "print_control_view.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

namespace {

/// A ui_button's text label: its label child that is not the icon glyph.
lv_obj_t* button_text_label(lv_obj_t* btn) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
        lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class) && child != ui_button_get_icon(btn)) {
            return child;
        }
    }
    return nullptr;
}

} // namespace

void register_control_buttons_widget() {
    register_widget_factory("control_buttons", [](const std::string&) {
        return std::make_unique<ControlButtonsWidget>();
    });
    // No init_subjects: the print_control_* subjects are owned and registered
    // by the helix::ui::PrintControlButtons singleton at startup.
}

ControlButtonsLayout decide_control_buttons_layout(int colspan, int rowspan, int width_px,
                                                   int height_px, int pad_px, int gap_px,
                                                   int button_need_px, bool tiny_breakpoint) {
    ControlButtonsLayout layout;
    layout.column = height_px > width_px;
    layout.fill = layout.column || rowspan >= 2 * GridLayout::TRACKS_PER_CELL;
    const int inner_w = width_px - 2 * pad_px;
    const int button_w = layout.column ? inner_w : (inner_w - gap_px) / 2;
    layout.labels = !tiny_breakpoint && colspan >= 2 * GridLayout::TRACKS_PER_CELL &&
                    button_w >= button_need_px;
    return layout;
}

ControlButtonsWidget::ControlButtonsWidget() {
    UI_MANAGED_SUBJECT_INT(labels_subject_, 1, labels_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(column_subject_, 0, column_name_.c_str(), subjects_);
    UI_MANAGED_SUBJECT_INT(fill_subject_, 0, fill_name_.c_str(), subjects_);
    attr_storage_ = {"labels_subject", labels_name_,   "column_subject",
                     column_name_,     "fill_subject", fill_name_};
    for (const auto& s : attr_storage_) {
        attrs_.push_back(s.c_str());
    }
    attrs_.push_back(nullptr);
}

ControlButtonsWidget::~ControlButtonsWidget() {
    detach();
    subjects_.deinit_all();
}

void ControlButtonsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    widget_obj_ = widget_obj;
    // A new language can change whether the labels fit without any resize, so
    // the verdict is re-taken against the size the grid last granted.
    language_observer_ = helix::ui::observe_language_change(
        this, [](ControlButtonsWidget* self) { self->relayout_for_granted_size(); });
    spdlog::debug("[ControlButtonsWidget] Attached");
}

void ControlButtonsWidget::detach() {
    language_observer_.reset();
    widget_obj_ = nullptr;
    spdlog::debug("[ControlButtonsWidget] Detached");
}

int ControlButtonsWidget::button_need_px() const {
    if (!widget_obj_) {
        return 0;
    }
    // The primary button's text arrives from a subject and switches between
    // every label print_control_view can give it, translated or not; the stop
    // button's is whatever its label says now.
    std::vector<const char*> primary_labels;
    for (const char* text : ui::kControlPrimaryLabels) {
        primary_labels.push_back(text);
        primary_labels.push_back(lv_tr(text));
    }

    lv_obj_t* primary = lv_obj_find_by_name(widget_obj_, "btn_primary");
    lv_obj_t* stop = lv_obj_find_by_name(widget_obj_, "btn_stop");
    int need = 0;
    for (lv_obj_t* btn : {primary, stop}) {
        lv_obj_t* label = btn ? button_text_label(btn) : nullptr;
        if (!label) {
            continue;
        }
        const lv_font_t* label_font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        const std::vector<const char*> texts =
            btn == primary ? primary_labels : std::vector<const char*>{lv_label_get_text(label)};
        int label_w = 0;
        for (const char* text : texts) {
            label_w = std::max(label_w, static_cast<int>(ui::text_width(text, label_font)));
        }
        int icon_w = 0;
        if (lv_obj_t* icon = ui_button_get_icon(btn)) {
            icon_w = static_cast<int>(ui::text_width(
                lv_label_get_text(icon), lv_obj_get_style_text_font(icon, LV_PART_MAIN)));
        }
        // The labelled buttons' side inset is #space_xxs (cb_btn_labelled), read
        // from the token rather than the button: the button carries whichever
        // inset the current decision applied, and measuring that would let the
        // answer flip on the next resize.
        const int chrome = 2 * theme_manager_get_spacing("space_xxs") +
                           static_cast<int>(lv_obj_get_style_pad_column(btn, LV_PART_MAIN));
        need = std::max(need, icon_w + label_w + chrome);
    }
    return need;
}

void ControlButtonsWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    int pad = 0;
    int gap = 0;
    if (widget_obj_) {
        pad = static_cast<int>(lv_obj_get_style_pad_left(widget_obj_, LV_PART_MAIN));
        gap = static_cast<int>(lv_obj_get_style_pad_column(widget_obj_, LV_PART_MAIN));
    }
    const int need = button_need_px();
    const auto layout =
        decide_control_buttons_layout(colspan, rowspan, width_px, height_px, pad, gap, need,
                                      widget_size::current_breakpoint() == UiBreakpoint::Tiny);
    spdlog::debug("[ControlButtonsWidget] {}x{}px span {}, labels need {}px -> {} {}", width_px,
                  height_px, colspan, need, layout.column ? "column" : "row",
                  layout.labels ? "labels" : "icons only");
    lv_subject_set_int(&column_subject_, layout.column ? 1 : 0);
    lv_subject_set_int(&fill_subject_, layout.fill ? 1 : 0);
    lv_subject_set_int(&labels_subject_, layout.labels ? 1 : 0);
}

} // namespace helix
