// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_info_qr_modal.h"

#include "ui_update_queue.h"

#include <lvgl.h>
#include <spdlog/spdlog.h>

namespace helix::ui {

InfoQrModal::InfoQrModal(Config config) : config_(std::move(config)) {}

bool InfoQrModal::show_modal(lv_obj_t* parent) {
    const char* attrs[] = {
        "icon_name", config_.icon.c_str(),
        "title",     config_.title.c_str(),
        "message",   config_.message.c_str(),
        "url_text",  config_.url_text.c_str(),
        nullptr,
    };
    return show(parent, attrs);
}

void InfoQrModal::on_show() {
    wire_ok_button("btn_ok");
    create_qr_code();
}

void InfoQrModal::on_hide() {
    auto* self = this;
    helix::ui::async_call(
        [](void* data) { delete static_cast<InfoQrModal*>(data); }, self);
}

void InfoQrModal::create_qr_code() {
#if LV_USE_QRCODE
    auto* container = find_widget("qr_container");
    if (!container) {
        spdlog::warn("[InfoQrModal] qr_container not found");
        return;
    }

    // Adaptive QR size: proportion of dialog width, clamped to reasonable bounds
    lv_obj_update_layout(dialog());
    int32_t dialog_w = lv_obj_get_content_width(dialog());
    int32_t qr_size = LV_CLAMP(120, dialog_w * 2 / 5, 200);

    lv_obj_set_size(container, qr_size, qr_size);

    lv_obj_t* qr = lv_qrcode_create(container);
    if (qr) {
        lv_qrcode_set_size(qr, qr_size);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_update(qr, config_.url.c_str(),
                         static_cast<uint32_t>(config_.url.size()));
        lv_obj_center(qr);
        spdlog::debug("[InfoQrModal] QR code created: {}px for '{}'", qr_size,
                      config_.url);
    }
#else
    spdlog::warn("[InfoQrModal] QR code support not compiled (LV_USE_QRCODE=0)");
#endif
}

} // namespace helix::ui
