// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim definitions for the 11 "cold" font faces moved out of the compiled app
// image into runtime .bin files in the frogfs storage partition (Stage B
// fonts->frogfs enabler). Each symbol is a zero-init, writable lv_font_t that
// main/font_registration.c populates at boot via lv_binfont_create + struct
// copy (fallback: a copy of LVGL's built-in lv_font_montserrat_14, the only
// font compiled into the image). Every existing &symbol reference
// (ui_icon.cpp, theme_manager.cpp, AssetManager::register_fonts, etc.) and
// token registration keeps working against these populated shims — so no call
// site changes. These MUST be writable (.bss/.data, never .rodata): the
// runtime struct-copy writes into them. The mdi_icons_* and source_code_pro_14
// faces are de-const'd in ui_fonts.h + lv_conf.h for exactly this reason
// (the noto_sans_* faces were already non-const).
#include "lvgl.h"

lv_font_t noto_sans_18;
lv_font_t mdi_icons_48;
lv_font_t mdi_icons_64;
lv_font_t noto_sans_bold_28;
lv_font_t noto_sans_light_16;
lv_font_t noto_sans_light_12;
lv_font_t noto_sans_26;
lv_font_t source_code_pro_14;
lv_font_t mdi_icons_16;
lv_font_t mdi_icons_24;
lv_font_t mdi_icons_32;
