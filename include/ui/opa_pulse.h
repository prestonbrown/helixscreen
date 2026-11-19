// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file opa_pulse.h
 * @brief A slow, endless breathing of an image's opacity.
 *
 * @threading Main thread only (LVGL animation API)
 */

#include "lvgl.h"

#include <cstdint>

namespace helix::ui {

struct OpaPulse {
    lv_opa_t low;
    lv_opa_t high;
    uint32_t half_period_ms;
};

/// Start or stop @p pulse on @p image. Repeating the current state changes nothing, so a
/// running pulse keeps its phase; a stopped one rests at the pulse's high opacity. The
/// animation runs on the image, so LVGL deletes it with the image.
void set_image_opa_pulse(lv_obj_t* image, bool on, const OpaPulse& pulse);

} // namespace helix::ui
