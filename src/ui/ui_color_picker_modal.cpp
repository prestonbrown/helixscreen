// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_color_picker_modal.h"

#include "ui_callback_helpers.h"

#include "color_recents.h"
#include "color_utils.h"
#include "lane_translation.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {

ColorPickerModal* s_active_modal = nullptr;

void register_modal_callbacks() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    register_xml_callbacks({
        {"color_picker_modal_cancel_cb",
         [](lv_event_t* e) {
             if (auto* self = ColorPickerModal::active()) {
                 self->on_cancel();
             }
         }},
        {"color_picker_modal_done_cb",
         [](lv_event_t* e) {
             if (auto* self = ColorPickerModal::active()) {
                 self->done();
             }
         }},
    });
}

} // namespace

ColorPickerModal* ColorPickerModal::active() {
    return s_active_modal;
}

ColorPickerModal::~ColorPickerModal() {
    if (s_active_modal == this) {
        s_active_modal = nullptr;
    }
    view_.detach();
}

bool ColorPickerModal::show_with_color(lv_obj_t* parent, uint32_t initial_rgb) {
    register_modal_callbacks();
    ensure_color_picker_view_subjects();
    original_ = initial_rgb & 0xFFFFFF;
    if (!Modal::show(parent)) {
        return false;
    }
    spdlog::info("[ColorPickerModal] shown with #{:06X}", original_);
    return true;
}

void ColorPickerModal::on_show() {
    s_active_modal = this;
    lv_obj_t* body = find_widget("cpm_body");
    if (!body) {
        spdlog::error("[ColorPickerModal] cpm_body missing");
        return;
    }
    ColorPickerConfig cfg = config_;
    cfg.tabs_in_body = false;
    previewed_ = false;
    view_.attach(body, original_, cfg);
    view_.on_change([this](uint32_t rgb) {
        previewed_ = true;
        if (preview_callback_) {
            preview_callback_(rgb);
        }
    });
}

void ColorPickerModal::done() {
    const uint32_t rgb = helix::ams::picked_color(view_.color());
    if (remember_on_done_ && config_.strip != ColorStrip::Custom) {
        ColorRecents::remember(config_.strip == ColorStrip::LedRecents
                                   ? ColorRecents::Kind::Led
                                   : ColorRecents::Kind::Filament,
                               rgb);
    }
    spdlog::info("[ColorPickerModal] done: #{:06X}", rgb);
    // Detach first: the callback may show another modal, which would take the
    // shared picker subjects over.
    view_.detach();
    if (color_callback_) {
        color_callback_(rgb, helix::get_color_name_from_hex(rgb));
    }
    hide(ModalCloseReason::ButtonPress);
}

void ColorPickerModal::restore_original() {
    if (!previewed_) {
        return;
    }
    previewed_ = false;
    if (preview_callback_ && view_.color() != original_) {
        preview_callback_(original_);
    }
    if (restore_callback_) {
        restore_callback_();
    }
}

void ColorPickerModal::on_cancel() {
    restore_original();
    Modal::on_cancel();
}

void ColorPickerModal::on_dismissed() {
    restore_original();
}

void ColorPickerModal::on_hide() {
    if (s_active_modal == this) {
        s_active_modal = nullptr;
    }
    view_.detach();
    if (dismiss_callback_) {
        dismiss_callback_();
    }
}

} // namespace helix::ui
