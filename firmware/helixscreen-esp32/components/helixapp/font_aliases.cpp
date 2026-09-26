// SPDX-License-Identifier: GPL-3.0-or-later
//
// Font-face aliases. AssetManager::register_fonts() (src/application/
// asset_manager.cpp) references every breakpoint tier's faces by symbol, but
// the medium tier's 11 faces the 800x480 K-Touch renders all load from frogfs
// .bin files at boot (see components/helixcore/moved_fonts_shim.c). The faces
// below are NOT in that set; each is aliased to a real compiled font so the
// app core links without shipping every tier's glyph data (a full tier set
// would blow the 5.8MB image budget). The medium globals.xml only references
// the 11 real faces, so at runtime these aliases are dead weight referenced
// solely by register_fonts()'s exhaustive list - never rendered. Real per-tier
// subsetting for the K-Touch is a later task.
//
// Same technique as the Plan 2 native-audit (audit_stubs.cpp font block). This
// file must be .cpp, not .c: the alias definitions below copy-initialize each
// symbol from the anchor font, which is not a compile-time constant expression
// (plain C requires one for static storage duration; C++ dynamic initialization
// allows it).
//
// noto_sans_10/11/12/14/16/20/24/28/32/40, noto_sans_bold_*, and
// noto_sans_light_* MUST stay non-const to match their `extern lv_font_t`
// declarations in ui_fonts.h — CjkFontManager (src/system/cjk_font_manager.cpp)
// takes their address into a `lv_font_t*` table at HELIX_MAX_FONT_TIER=6 and
// writes `->fallback` at runtime. mdi_icons_14 and the source_code_pro_*
// aliases stay `const`, matching their `LV_FONT_DECLARE` (icon/mono fonts,
// never touched by CjkFontManager).

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
