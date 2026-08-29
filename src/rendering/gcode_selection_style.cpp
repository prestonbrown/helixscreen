// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file gcode_selection_style.cpp
 * @brief Resolves the selection palette from ui_xml/gcode_tokens.xml.
 *
 * The rest of gcode_selection_style.h is header-only and dependency-free on
 * purpose: resolve(), bracket_arm_length() and the width helpers are pure and
 * are tested without a theme, a display, or LVGL. This one function is the
 * exception, because reading a token means touching LVGL's const registry, so
 * it is compiled separately rather than dragging theme_manager.h into every
 * translation unit that wants to ask how a selected object looks.
 */

#include "gcode_selection_style.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "theme_manager.h"

#include <lvgl/lvgl.h>

namespace helix::gcode::selection {

namespace {

/// Read one token, or leave `out` alone if the token is not registered.
///
/// theme_manager_get_color() answers black for an unknown name (and logs), so
/// probing the const first is what tells "token says black" apart from "no token".
/// The case that actually reaches this is the headless unit-test fixture, which
/// initializes LVGL without theme_manager; a shipped tree always has the tokens,
/// since no packaging rule omits ui_xml.
void read_token(const char* name, uint32_t& out) {
    if (lv_xml_get_const_silent(nullptr, name) == nullptr) {
        return;
    }
    out = lv_color_to_u32(theme_manager_get_color(name)) & 0x00FFFFFFu;
}

} // namespace

Palette palette_from_theme() {
    Palette p; // defaults are the values gcode_tokens.xml ships
    read_token("gcode_selection_outline", p.outline);
    read_token("gcode_selection_excluded", p.excluded);
    read_token("gcode_selection_bracket", p.bracket);
    return p;
}

} // namespace helix::gcode::selection
