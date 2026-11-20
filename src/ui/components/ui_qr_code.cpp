// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_qr_code.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/libs/qrcode/qrcodegen.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

lv_obj_t* create_qr_code(lv_obj_t* component, std::string_view text, int32_t size_px,
                         const char* log_tag) {
#if LV_USE_QRCODE
    lv_obj_t* frame = component ? lv_xml_component_get_slot(component, "qr") : nullptr;
    if (!frame) {
        spdlog::warn("[{}] no qr slot to hold the QR code", log_tag);
        return nullptr;
    }
    lv_obj_t* qr = lv_qrcode_create(frame);
    if (!qr) {
        spdlog::warn("[{}] lv_qrcode_create failed", log_tag);
        return nullptr;
    }
    lv_qrcode_set_size(qr, size_px);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, false);
    lv_qrcode_update(qr, text.data(), static_cast<uint32_t>(text.size()));
    lv_obj_center(qr);
    spdlog::debug("[{}] QR code created: {} chars, {}px", log_tag, text.size(), size_px);
    return qr;
#else
    (void)component;
    (void)text;
    (void)size_px;
    spdlog::warn("[{}] QR code support not compiled (LV_USE_QRCODE=0)", log_tag);
    return nullptr;
#endif
}

int32_t qr_size_snapped_to_modules(std::string_view text, int32_t target_px) {
    const int version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, text.size());
    if (version <= 0) {
        return target_px;
    }
    const int32_t modules = qrcodegen_version2size(version);
    return modules * std::max<int32_t>(1, target_px / modules);
}

} // namespace helix::ui
