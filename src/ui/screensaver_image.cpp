// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_image.h"

#include "printer_image_manager.h"

#include <spdlog/spdlog.h>

#include <image/lv_image_decoder_private.h>

namespace helix::ui {

lv_draw_buf_t* saver_decode_image(const char* path, const char* what,
                                  spdlog::level::level_enum failure_level) {
    lv_image_decoder_dsc_t dsc;
    const lv_result_t res = lv_image_decoder_open(&dsc, path, nullptr);
    if (res != LV_RESULT_OK || dsc.decoded == nullptr) {
        spdlog::log(failure_level, "[Screensaver] Failed to decode {} '{}'", what, path);
        if (res == LV_RESULT_OK) {
            lv_image_decoder_close(&dsc);
        }
        return nullptr;
    }
    lv_draw_buf_t* copy = lv_draw_buf_dup(dsc.decoded);
    lv_image_decoder_close(&dsc);
    if (copy == nullptr) {
        spdlog::log(failure_level, "[Screensaver] Failed to copy decoded {} '{}'", what, path);
        return nullptr;
    }
    spdlog::debug("[Screensaver] Decoded {} '{}' at {}x{}", what, path,
                  static_cast<uint32_t>(copy->header.w), static_cast<uint32_t>(copy->header.h));
    return copy;
}

bool saver_image_info(const char* path, lv_image_header_t& out) {
    out = lv_image_header_t{};
    return lv_image_decoder_get_info(path, &out) == LV_RESULT_OK;
}

std::string saver_printer_image_path(int32_t screen_w) {
    return helix::PrinterImageManager::instance().get_displayed_image_path(screen_w);
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
