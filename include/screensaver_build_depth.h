// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "screensaver_registry.h"

#include <cstdint>

namespace helix::ui {

/// The SaverDepth bit this build draws at, for registry queries such as
/// default_screensaver_type(). A 16 bpp build compiles only the savers carrying SAVER_DEPTH_16.
/// Kept out of screensaver_registry.h so the LVGL-free splash build can read the registry.
inline constexpr uint8_t kSaverBuildDepth = LV_COLOR_DEPTH == 16 ? SAVER_DEPTH_16 : SAVER_DEPTH_32;

} // namespace helix::ui
