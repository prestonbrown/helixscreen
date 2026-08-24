// SPDX-License-Identifier: GPL-3.0-or-later
//
// Forces the helix_lvgl_abi.h tripwire to compile in the helixapp component
// (theme/UI code). Its lv_conf.h view MUST match helixcore's, or LVGL struct
// layouts diverge across the boundary. See helix_lvgl_abi.h for the rationale.
#include "helix_lvgl_abi.h"
