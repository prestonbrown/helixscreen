// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_info_qr_modal.h"

#include "ui_qr_code.h"

#include <lvgl.h>
#include <memory>

namespace helix::ui {

InfoQrModal::InfoQrModal(Config config) : config_(std::move(config)) {}

bool InfoQrModal::show_modal(lv_obj_t* parent) {
    const char* attrs[] = {
        "icon_name", config_.icon.c_str(),    "title",    config_.title.c_str(),
        "message",   config_.message.c_str(), "url_text", config_.url_text.c_str(),
        nullptr,
    };
    return show(parent, attrs);
}

bool InfoQrModal::show_owned(Config config) {
    auto modal = std::make_unique<InfoQrModal>(std::move(config));
    if (!modal->show_modal(lv_screen_active())) {
        return false; // the unique_ptr frees the never-shown instance
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

void InfoQrModal::on_show() {
    wire_ok_button("btn_ok");
    create_qr_code();
}

void InfoQrModal::create_qr_code() {
    // A proportion of the dialog width, clamped to a scannable range.
    lv_obj_update_layout(dialog());
    const int32_t qr_size = LV_CLAMP(120, lv_obj_get_content_width(dialog()) * 2 / 5, 200);
    helix::ui::create_qr_code(dialog(), config_.url, qr_size, "InfoQrModal");
}

} // namespace helix::ui
