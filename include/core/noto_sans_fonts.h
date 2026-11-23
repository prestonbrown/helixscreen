// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file noto_sans_fonts.h
 * @brief The regular-weight Noto Sans faces, for any layer that draws text.
 *
 * The faces are generated assets (assets/fonts), not UI code, so a renderer
 * can label what it draws without depending on ui_fonts.h, which declares the
 * rest of the UI's faces and includes this.
 */

#include "lvgl/lvgl.h"

// Noto Sans - Regular weight with extended Unicode (©®™€£¥°±•…)
// Declared mutable (not const) so CjkFontManager can set fallback pointers at runtime
extern lv_font_t noto_sans_8;
extern lv_font_t noto_sans_10;
extern lv_font_t noto_sans_11;
extern lv_font_t noto_sans_12;
extern lv_font_t noto_sans_14;
extern lv_font_t noto_sans_16;
extern lv_font_t noto_sans_18;
extern lv_font_t noto_sans_20;
extern lv_font_t noto_sans_24;
extern lv_font_t noto_sans_26;
extern lv_font_t noto_sans_28;
extern lv_font_t noto_sans_32;
extern lv_font_t noto_sans_40;
// 48/64 exist only for the high-DPI UI scale factor, which steps the XXLarge
// tier's face up rather than moving tier. Linked on the xxlarge tier only, so
// no printer build carries them (see FONTS_XXLARGE in mk/fonts.mk).
extern lv_font_t noto_sans_48;
extern lv_font_t noto_sans_64;
