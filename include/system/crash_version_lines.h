// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file crash_version_lines.h
 * @brief The crash record's version lines, as one literal the signal handler can write
 *
 * lv_event_code_t numbering changes between LVGL releases, so the record names the LVGL
 * the crashing binary was built against and the crash worker decodes event_code with it.
 */

#pragma once

#include "helix_version.h"
#include "lvgl.h"

#define HELIX_CRASH_STR_(x) #x
#define HELIX_CRASH_STR(x) HELIX_CRASH_STR_(x)

static constexpr const char kCrashVersionLines[] =
    "version:" HELIX_VERSION "\n"
    "lvgl_version:" HELIX_CRASH_STR(LVGL_VERSION_MAJOR) "." HELIX_CRASH_STR(
        LVGL_VERSION_MINOR) "\n";
