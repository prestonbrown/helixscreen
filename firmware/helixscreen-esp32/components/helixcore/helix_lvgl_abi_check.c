// SPDX-License-Identifier: GPL-3.0-or-later
//
// Forces the helix_lvgl_abi.h tripwire to compile in the helixcore component
// (where LVGL itself is built). helixapp compiles its own copy of this check —
// both must agree. See helix_lvgl_abi.h for the rationale.
#include "helix_lvgl_abi.h"
