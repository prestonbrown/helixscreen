// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_text.h
 * @brief Semantic text widgets (text_heading, text_body, text_small, text_xs)
 *
 * @pattern Semantic typography instead of direct font specification
 * @threading Main thread only
 * @gotchas Must call ui_text_init() before creating UI; stroke effect via ui_text_set_stroke()
 */

#pragma once

#include "lvgl/lvgl.h"

#include <string>

/**
 * Semantic text widgets for theme-aware typography
 *
 * These widgets create labels with automatic font and color styling based on
 * semantic meaning, reading from globals.xml theme constants.
 *
 * Available widgets:
 * - <text_heading>: Section headings, large text (20/26/28 + text_muted)
 * - <text_body>:    Standard body text (14/18/20 + text_primary)
 * - <text_muted>:   Muted body text (14/18/20 + text_muted) for secondary content
 * - <text_small>:   Small/helper text (12/16/18 + text_muted)
 * - <text_xs>:      Extra-small text (10/12/14 + text_muted) for compact metadata
 *
 * All widgets inherit standard lv_label attributes (text, width, align, etc.)
 *
 * Text stroke/outline attributes (supported on all text_* widgets):
 * - stroke_width:  Outline thickness in pixels (e.g., "2")
 * - stroke_color:  Outline color as hex (e.g., "0x000000" or "#000000")
 * - stroke_opa:    Outline opacity 0-255 or percentage (e.g., "255" or "50%")
 *
 * Example:
 *   <text_heading text="Title" stroke_width="2" stroke_color="#000000"/>
 */

/**
 * Initialize semantic text widgets for XML usage
 * Registers custom widgets: text_heading, text_body, text_muted, text_small, text_xs
 *
 * Call this after globals.xml is registered but before creating UI.
 */
void ui_text_init();

/**
 * Apply text stroke/outline effect to a label
 *
 * Creates an outline around text glyphs for improved readability on
 * complex backgrounds.
 *
 * @param label  Label widget to apply stroke to
 * @param width  Stroke width in pixels (0 to disable)
 * @param color  Stroke color
 * @param opa    Stroke opacity (0-255, use LV_OPA_COVER for full opacity)
 */
void ui_text_set_stroke(lv_obj_t* label, int32_t width, lv_color_t color, lv_opa_t opa);

/**
 * Apply a text transform to a label
 *
 * Supports "uppercase", which converts text to uppercase at render time.
 * Works with bind_text subject bindings and translations.
 *
 * @param label     Label widget
 * @param transform Transform name: "uppercase" (or nullptr/empty to remove)
 */
void ui_text_apply_transform(lv_obj_t* label, const char* transform);

namespace helix::ui {

/**
 * Whether @p font, through its fallback chain, has a real glyph for every
 * character of @p text
 *
 * A character no font in the chain carries draws as a placeholder box, so text
 * that fails this check shows up as tofu, and so does malformed UTF-8, which
 * fails it too. Control characters (below U+0020) are layout, not glyphs, and
 * always pass. Empty text passes; a null font cannot be asked, so it passes too.
 *
 * Pure over its arguments; it neither reads nor writes a widget.
 *
 * @param font Font the text is drawn with
 * @param text UTF-8 text to check
 */
bool font_renders_text(const lv_font_t* font, const char* text);

/**
 * Fit text into a width by replacing its middle with an ellipsis
 *
 * Text that measures within @p max_width comes back unchanged. Text that
 * overflows keeps its head and its tail, each given half of the width left
 * beside the ellipsis, so a filename such as
 * "Delta filament barrel base_Hyper PLA_49m" keeps both the name that
 * identifies the job and the material/duration suffix that tells it apart from
 * its neighbours. Width is measured with @p font, never counted in bytes or
 * characters, so a Cyrillic name is cut where it is drawn, not where a Latin
 * one would be.
 *
 * Pure over its arguments; it neither reads nor writes a widget.
 *
 * @param text         UTF-8 text to fit
 * @param max_width    Width the result must fit in, in pixels
 * @param font         Font the text is drawn with
 * @param letter_space Letter spacing the text is drawn with
 * @return The text, or "<head>…<tail>" when it does not fit
 */
std::string middle_ellipsize(const char* text, int32_t max_width, const lv_font_t* font,
                             int32_t letter_space = 0);

} // namespace helix::ui
