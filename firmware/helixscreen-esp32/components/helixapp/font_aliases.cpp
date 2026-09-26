// SPDX-License-Identifier: GPL-3.0-or-later
//
// Font-face aliases. AssetManager::register_fonts() (src/application/
// asset_manager.cpp) references every breakpoint tier's faces by symbol, and
// panels render several of these by pointer or by token name (ui_jog_pad.cpp,
// ui_bed_mesh.cpp, ui_fatal_error.cpp, ui_keyboard_manager.cpp, the path
// canvases, memory_stats_overlay.xml, the theme font tokens). The medium
// tier's 11 real faces all load from frogfs .bin files at boot (see
// components/helixcore/moved_fonts_shim.c); the faces below have no .bin of
// their own, so each is aliased to keep the app core linking without shipping
// every tier's glyph data (a full tier set would blow the 5.8MB image budget).
// Real per-tier subsetting for the K-Touch is a later task.
//
// Two-step value: the alias definitions below copy-initialize from
// lv_font_montserrat_14 - LVGL's built-in LV_FONT_DEFAULT and the only font
// this build compiles - because the copy-initializers run at static-init
// time, before helix_fonts_register() populates the frogfs shims at boot; a
// shim anchor would bake zero-value structs into every alias. Then
// helix_font_aliases_refresh() re-copies the populated noto_sans_18 (real
// glyphs, or the montserrat fallback if its .bin failed) into every non-const
// alias, called from font_registration.c before the token registrations hand
// these pointers to LVGL - so text that renders after boot draws Noto Sans
// glyphs, and only pre-registration draws (a fatal-error screen during early
// boot) ever see montserrat. The refresh replaces the whole struct, so it
// must run before CjkFontManager writes ->fallback on these symbols (a
// language switch, always post-boot).
//
// This file must be .cpp, not .c: the alias definitions below
// copy-initialize each symbol from the anchor font, which is not a
// compile-time constant expression (plain C requires one for static storage
// duration; C++ dynamic initialization allows it).
//
// noto_sans_10/11/12/14/16/20/24/28/32/40, noto_sans_bold_*, and
// noto_sans_light_* MUST stay non-const to match their `extern lv_font_t`
// declarations in ui_fonts.h - CjkFontManager (src/system/cjk_font_manager.cpp)
// takes their address into a `lv_font_t*` table at HELIX_MAX_FONT_TIER=6 and
// writes `->fallback` at runtime. mdi_icons_14/80/96/128 and the
// source_code_pro_* aliases stay `const` on montserrat_14 permanently,
// matching their `LV_FONT_DECLARE` (icon/mono fonts, never touched by
// CjkFontManager).

#include "ui_fonts.h"

// A real, always-linked compiled font every alias below copies from. Must be a
// symbol with glyph data in the image, NOT one of the 11 frogfs shim faces
// (moved_fonts_shim.c): the copy-initializers below run at static-init time,
// before helix_fonts_register() populates the shims at boot, so a shim anchor
// would bake zero-value structs into every alias. lv_font_montserrat_14 is
// LVGL's built-in LV_FONT_DEFAULT and the only font this build compiles.
LV_FONT_DECLARE(lv_font_montserrat_14);

// --- mdi_icons: tiers beyond the 5 helixcore compiles (16/24/32/48/64) ------
const lv_font_t mdi_icons_14 = lv_font_montserrat_14;
const lv_font_t mdi_icons_80 = lv_font_montserrat_14;
const lv_font_t mdi_icons_96 = lv_font_montserrat_14;
const lv_font_t mdi_icons_128 = lv_font_montserrat_14;

// --- noto_sans (regular weight): tiers beyond helixcore's 18/26 -------------
lv_font_t noto_sans_8 = lv_font_montserrat_14;
lv_font_t noto_sans_10 = lv_font_montserrat_14;
lv_font_t noto_sans_11 = lv_font_montserrat_14;
lv_font_t noto_sans_12 = lv_font_montserrat_14;
lv_font_t noto_sans_14 = lv_font_montserrat_14;
lv_font_t noto_sans_16 = lv_font_montserrat_14;
lv_font_t noto_sans_20 = lv_font_montserrat_14;
lv_font_t noto_sans_24 = lv_font_montserrat_14;
lv_font_t noto_sans_28 = lv_font_montserrat_14;
lv_font_t noto_sans_32 = lv_font_montserrat_14;
lv_font_t noto_sans_40 = lv_font_montserrat_14;

// --- noto_sans_bold: tiers beyond helixcore's bold_28 -----------------------
lv_font_t noto_sans_bold_14 = lv_font_montserrat_14;
lv_font_t noto_sans_bold_16 = lv_font_montserrat_14;
lv_font_t noto_sans_bold_18 = lv_font_montserrat_14;
lv_font_t noto_sans_bold_20 = lv_font_montserrat_14;
lv_font_t noto_sans_bold_24 = lv_font_montserrat_14;
lv_font_t noto_sans_bold_32 = lv_font_montserrat_14;
lv_font_t noto_sans_bold_40 = lv_font_montserrat_14;

// --- noto_sans_light: tiers beyond helixcore's light_12/light_16 ------------
lv_font_t noto_sans_light_10 = lv_font_montserrat_14;
lv_font_t noto_sans_light_11 = lv_font_montserrat_14;
lv_font_t noto_sans_light_14 = lv_font_montserrat_14;
lv_font_t noto_sans_light_18 = lv_font_montserrat_14;
lv_font_t noto_sans_light_20 = lv_font_montserrat_14;
lv_font_t noto_sans_light_26 = lv_font_montserrat_14;

// --- source_code_pro: the whole mono family except helixcore's real _14 ----
const lv_font_t source_code_pro_8 = lv_font_montserrat_14;
const lv_font_t source_code_pro_10 = lv_font_montserrat_14;
const lv_font_t source_code_pro_12 = lv_font_montserrat_14;
const lv_font_t source_code_pro_16 = lv_font_montserrat_14;
const lv_font_t source_code_pro_18 = lv_font_montserrat_14;
const lv_font_t source_code_pro_20 = lv_font_montserrat_14;
const lv_font_t source_code_pro_24 = lv_font_montserrat_14;

// Re-copy the boot-populated noto_sans_18 into every NON-const alias above
// (the const mdi_icons_14/80/96/128 and source_code_pro_* stay on
// montserrat_14). Called from helix_fonts_register()
// (main/font_registration.c) once noto_sans_18 is populated - see the header
// comment for the ordering contract. noto_sans_18's own declaration comes
// from ui_fonts.h.
extern "C" void helix_font_aliases_refresh(void) {
    noto_sans_8 = noto_sans_18;
    noto_sans_10 = noto_sans_18;
    noto_sans_11 = noto_sans_18;
    noto_sans_12 = noto_sans_18;
    noto_sans_14 = noto_sans_18;
    noto_sans_16 = noto_sans_18;
    noto_sans_20 = noto_sans_18;
    noto_sans_24 = noto_sans_18;
    noto_sans_28 = noto_sans_18;
    noto_sans_32 = noto_sans_18;
    noto_sans_40 = noto_sans_18;
    noto_sans_bold_14 = noto_sans_18;
    noto_sans_bold_16 = noto_sans_18;
    noto_sans_bold_18 = noto_sans_18;
    noto_sans_bold_20 = noto_sans_18;
    noto_sans_bold_24 = noto_sans_18;
    noto_sans_bold_32 = noto_sans_18;
    noto_sans_bold_40 = noto_sans_18;
    noto_sans_light_10 = noto_sans_18;
    noto_sans_light_11 = noto_sans_18;
    noto_sans_light_14 = noto_sans_18;
    noto_sans_light_18 = noto_sans_18;
    noto_sans_light_20 = noto_sans_18;
    noto_sans_light_26 = noto_sans_18;
}
