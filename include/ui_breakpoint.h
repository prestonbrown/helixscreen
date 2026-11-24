// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_breakpoint.h
 * @brief The UI's name for the breakpoint tiers in core/breakpoint.h
 *
 * The tiers are plain data with no LVGL or theme dependency, so they live in
 * core where services below the UI can classify a screen too.
 */

#pragma once

#include "core/breakpoint.h"

using helix::as_breakpoint;
using helix::breakpoint_for;
using helix::breakpoint_from_name;
using helix::responsive_pick;
using helix::to_int;
using helix::UiBreakpoint;
