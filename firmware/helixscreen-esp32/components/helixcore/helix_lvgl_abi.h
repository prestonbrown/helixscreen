// SPDX-License-Identifier: GPL-3.0-or-later
//
// Build-time ABI tripwire for the ESP32 firmware.
//
// Every component that touches LVGL structs (helixcore where LVGL compiles,
// helixapp where the theme/UI code lives) MUST see an identical lv_conf.h.
// When two components disagree on a layout-affecting flag, sizeof() of the
// shared types diverges and memory is corrupted across the call boundary —
// which cost a multi-day PC=0 boot crash: LV_USE_ASSERT_STYLE was gated on
// HELIX_RELEASE_BUILD (defined only for helixapp), so LVGL's lv_style_t carried
// a 4-byte sentinel that helixapp's copy did not, and lv_style_reset() overran
// into the adjacent StyleEntry.configure fn-ptr.
//
// Include this from one TU in EACH LVGL-touching component (see
// helix_lvgl_abi_check.c). If any assert fires, that component is compiling
// against the wrong lv_conf.h (or a flag drifted) — fix the config, not this
// file. The checked values are the two known divergence axes plus the type
// whose size actually crashed us.
#pragma once

#include "lvgl.h"

#if defined(__cplusplus)
#define HELIX_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define HELIX_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

// RGB565 panel. The repo-root desktop lv_conf.h is 32bpp; catching a leak of
// it into a firmware TU is the whole point. (Note: in LVGL 9 lv_color_t itself
// is a fixed 3-byte RGB888 struct regardless of depth, so this MACRO check --
// not sizeof(lv_color_t) -- is what actually distinguishes the two configs.)
HELIX_ABI_ASSERT(LV_COLOR_DEPTH == 16, "LV_COLOR_DEPTH mismatch: this TU is compiling against the "
                                       "wrong lv_conf.h (desktop repo-root copy is 32bpp).");

// The sentinel field this flag adds is lv_style_t's first member; it must be
// off (and identical) everywhere or lv_style_reset() overruns.
HELIX_ABI_ASSERT(LV_USE_ASSERT_STYLE == 0,
                 "LV_USE_ASSERT_STYLE must be 0 firmware-wide: a non-zero "
                 "value adds the lv_style_t sentinel and splits sizeof across "
                 "components (the theme-init PC=0 crash).");
HELIX_ABI_ASSERT(sizeof(lv_style_t) == 12,
                 "sizeof(lv_style_t) != 12: a layout-affecting lv_conf.h flag "
                 "diverged between components; the theme StyleEntry ABI is no "
                 "longer safe.");
