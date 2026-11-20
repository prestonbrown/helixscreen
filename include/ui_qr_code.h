// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>
#include <string_view>

namespace helix::ui {

/// Creates a QR code of `text`, `size_px` square, in the `qr` slot of the
/// component instance `component` (a modal's dialog whose XML declares
/// `<slot name="qr" target="qr_frame"/>` over a `<qr_frame>`). Black on white,
/// edge to edge, centered in the frame. Returns the lv_qrcode, or nullptr when
/// the component declares no `qr` slot or the build has no QR support (both
/// logged under `log_tag`).
lv_obj_t* create_qr_code(lv_obj_t* component, std::string_view text, int32_t size_px,
                         const char* log_tag);

/// The largest whole multiple of `text`'s QR module count that fits
/// `target_px`, so every module is the same number of pixels and the code
/// leaves no light margin inside its square. `target_px` when the text does not
/// fit a QR code.
int32_t qr_size_snapped_to_modules(std::string_view text, int32_t target_px);

} // namespace helix::ui
