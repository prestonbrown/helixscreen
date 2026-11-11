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

/// Which long modes a line hold applies in.
enum class LineHold {
    Dotted,  ///< Only while the label is in DOTS; out of it the owner sets the height
    AnyMode, ///< In every long mode: a one-line slot that clips, scrolls or dots
};

/**
 * Hold a label to exactly one line of its own font
 *
 * LVGL's DOTS mode ellipsizes only when the wrapped text is taller than the
 * label, so a label at content height wraps instead of ellipsizing. This sets
 * the height to one line of the label's computed font plus its top and bottom
 * padding and borders, and keeps it there when the font or padding changes
 * later (on LV_EVENT_STYLE_CHANGED, writing only when the value differs).
 * Calling it again on the same label is a no-op beyond re-measuring.
 *
 * With LineHold::Dotted the label must be in DOTS mode for the height to be
 * applied. While it stays in DOTS, the hook rewrites any height a caller sets
 * later, so code that wants a different height switches the long mode first
 * or calls label_release_lines(); out of DOTS the hook leaves the height alone.
 * LineHold::AnyMode holds the height whatever the long mode.
 *
 * @param label Label to hold; nullptr is ignored
 * @param when  Which long modes the hold applies in
 */
void label_hold_one_line(lv_obj_t* label, LineHold when = LineHold::Dotted);

/**
 * Let a dotted label wrap to at most @p lines lines of its own font, then ellipsize
 *
 * label_hold_one_line() generalised: one line is held as a fixed height, more
 * as a max_height, so text that fits in fewer lines keeps its own height. The
 * cap follows font and padding changes the same way. XML reaches it through
 * `max_lines="N"` on any text_* widget with `long_mode="dots"`.
 *
 * @param label Label to hold; nullptr is ignored
 * @param lines Line ceiling; below 1 is ignored
 * @param when  Which long modes the hold applies in
 */
void label_hold_lines(lv_obj_t* label, int32_t lines, LineHold when = LineHold::Dotted);

/**
 * Stop holding a label: its height and max_height stay as they are until the
 * caller sets them. Does nothing to a label that is not held.
 *
 * @param label Label to release; nullptr is ignored
 */
void label_release_lines(lv_obj_t* label);

/**
 * Hold a dotted, content-height label to one line
 *
 * The rule text_* apply after their attributes: a label whose
 * long mode is DOTS, whose height is LV_SIZE_CONTENT and which sets no
 * max_height is held with label_hold_one_line(). An explicit height or a
 * max_height leaves it alone, as does any other long mode.
 *
 * @param label Label to check; nullptr is ignored
 */
void label_hold_one_line_if_dotted(lv_obj_t* label);

/**
 * label_hold_one_line_if_dotted() with a ceiling of @p lines lines
 *
 * The same opt-outs apply: an explicit height or a max_height leaves the label
 * alone. text_* apply it with their `max_lines` attribute (1 when absent or
 * below 1); text_button only when `max_lines` is given.
 */
void label_hold_lines_if_dotted(lv_obj_t* label, int32_t lines);

} // namespace helix::ui
