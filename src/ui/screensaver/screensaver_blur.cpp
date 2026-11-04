// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_blur.h"

#include "screensaver_motion.h"

#include <algorithm>
#include <cstring>

namespace helix::ui {

void box_blur(float* a, int32_t w, int32_t h, int32_t r, bool wrap_x, bool wrap_y, float* tmp,
              int passes) {
    if (r <= 0 || w <= 0 || h <= 0) {
        return;
    }
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    const auto pass = [&](bool along_x) {
        const int32_t len = along_x ? w : h;
        const int32_t lines = along_x ? h : w;
        const bool wrap = along_x ? wrap_x : wrap_y;
        for (int32_t l = 0; l < lines; l++) {
            const auto at = [&](int32_t i) -> float {
                if (wrap) {
                    i = screensaver::wrap_index(i, len);
                } else {
                    i = std::clamp(i, 0, len - 1);
                }
                return along_x ? a[static_cast<size_t>(l) * w + i]
                               : a[static_cast<size_t>(i) * w + l];
            };
            float sum = 0.0f;
            for (int32_t k = -r; k <= r; k++) {
                sum += at(k);
            }
            for (int32_t i = 0; i < len; i++) {
                const size_t o =
                    along_x ? static_cast<size_t>(l) * w + i : static_cast<size_t>(i) * w + l;
                tmp[o] = sum * inv;
                sum += at(i + r + 1) - at(i - r);
            }
        }
        std::memcpy(a, tmp, sizeof(float) * static_cast<size_t>(w) * h);
    };
    for (int n = 0; n < passes; n++) {
        pass(true);
        if (h > 1) {
            pass(false);
        }
    }
}

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
