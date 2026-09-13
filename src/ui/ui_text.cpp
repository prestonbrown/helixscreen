// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_text.h"

#include "ui_animations_pref.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_label_parser.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/misc/lv_text_private.h" // lv_text_get_width, lv_text_encoded_next/prev
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

/**
 * Enum for text style types used by semantic text widgets
 */
enum class TextStyleType {
    PRIMARY, // Primary text color (text_body, text_button)
    MUTED    // Muted text color (text_heading, text_small, text_xs)
};

/**
 * One shared style per compiled font face
 *
 * Semantic text widgets carry their font through these styles instead of a
 * local style property. A local property outranks every ADDED style, and the
 * XML engine applies bind_style / bind_style_if_* through lv_obj_add_style —
 * so a font written locally here would make any style bound from XML silently
 * unable to override it. As a shared style added at create time, the semantic
 * font sits below the nested bind elements the parser applies afterwards
 * (same-precedence styles resolve in addition order) while an inline
 * style_text_font attribute — still local, still applied after create — keeps
 * outranking both.
 */
static lv_style_t* shared_font_style(const lv_font_t* font) {
    // Append-only: one entry per compiled face a semantic constant has ever
    // resolved to (at most one per tier per role), so a fixed table is enough
    // and nothing here needs the heap.
    constexpr size_t MAX_FACES = 32;
    static const lv_font_t* faces[MAX_FACES] = {};
    static lv_style_t styles[MAX_FACES];
    for (size_t i = 0; i < MAX_FACES; ++i) {
        if (faces[i] == font)
            return &styles[i];
        if (faces[i] == nullptr) {
            faces[i] = font;
            lv_style_init(&styles[i]);
            lv_style_set_text_font(&styles[i], font);
            return &styles[i];
        }
    }
    spdlog::critical("[ui_text] FATAL: shared font style table full ({} faces)", MAX_FACES);
    std::exit(EXIT_FAILURE);
}

/**
 * Helper function to apply semantic font to a label
 *
 * IMPORTANT: This function will CRASH the application if a font is not found.
 * This is intentional - silent font fallbacks cause visual bugs that are
 * extremely hard to debug. If a font is missing, fix lv_conf.h immediately.
 *
 * @param label Label widget to style
 * @param font_const_name Name of font constant in globals.xml (e.g., "font_heading")
 */
static void apply_semantic_font(lv_obj_t* label, const char* font_const_name) {
    // Apply font - FAIL FAST if font is not available
    const char* font_name = lv_xml_get_const(nullptr, font_const_name);
    if (!font_name) {
        spdlog::critical("[ui_text] FATAL: Font constant '{}' not found in globals.xml",
                         font_const_name);
        spdlog::critical("[ui_text] Check that globals.xml defines this constant");
        std::exit(EXIT_FAILURE);
    }

    const lv_font_t* font = lv_xml_get_font(nullptr, font_name);
    if (!font) {
        // Extract font size from name like "montserrat_26" -> "26"
        std::string font_str(font_name);
        std::string size_hint;
        size_t underscore = font_str.rfind('_');
        if (underscore != std::string::npos) {
            size_hint = font_str.substr(underscore + 1);
        }

        spdlog::critical("[ui_text] FATAL: Font '{}' (from constant '{}') is not compiled!",
                         font_name, font_const_name);
        if (!size_hint.empty()) {
            spdlog::critical("[ui_text] FIX: Enable LV_FONT_MONTSERRAT_{} in lv_conf.h", size_hint);
        }
        std::exit(EXIT_FAILURE);
    }

    lv_obj_add_style(label, shared_font_style(font), LV_PART_MAIN);

    // Debug: log the actual font being applied
    spdlog::trace("[ui_text] Applied font '{}' (from '{}') - line_height={}px", font_name,
                  font_const_name, lv_font_get_line_height(font));
}

/**
 * Helper function to apply shared text style for reactive theming
 *
 * Adds the appropriate shared text style from theme_core so text color
 * updates automatically when the theme changes.
 *
 * @param label Label widget to style
 * @param style_type Which text style to apply (PRIMARY or MUTED)
 */
static void apply_shared_text_style(lv_obj_t* label, TextStyleType style_type) {
    lv_style_t* text_style = nullptr;

    switch (style_type) {
    case TextStyleType::PRIMARY:
        text_style = ThemeManager::instance().get_style(StyleRole::TextPrimary);
        break;
    case TextStyleType::MUTED:
        text_style = ThemeManager::instance().get_style(StyleRole::TextMuted);
        break;
    }

    if (text_style) {
        lv_obj_add_style(label, text_style, LV_PART_MAIN);
        spdlog::trace("[ui_text] Applied shared {} text style",
                      style_type == TextStyleType::PRIMARY ? "primary" : "muted");
    } else {
        spdlog::warn("[ui_text] Shared text style not available - theme not initialized?");
    }
}

/**
 * Helper function to apply text stroke attributes from XML
 *
 * Parses and applies stroke_width, stroke_color, and stroke_opa attributes
 * to enable text outline effects on labels.
 *
 * @param label Label widget to apply stroke styling to
 * @param attrs XML attribute array (name/value pairs, NULL terminated)
 *
 * Usage in XML:
 *   <text_heading text="Title" stroke_width="2" stroke_color="0x000000" stroke_opa="255"/>
 *   <text_body text="Body" stroke_width="1" stroke_color="#000000"/>
 */
static void apply_stroke_attrs(lv_obj_t* label, const char** attrs) {
    if (!attrs)
        return;

    const char* stroke_width = lv_xml_get_value_of(attrs, "stroke_width");
    const char* stroke_color = lv_xml_get_value_of(attrs, "stroke_color");
    const char* stroke_opa = lv_xml_get_value_of(attrs, "stroke_opa");

    // Apply stroke width (required for stroke to be visible)
    if (stroke_width) {
        int32_t width = lv_xml_atoi(stroke_width);
        lv_obj_set_style_text_outline_stroke_width(label, width, 0);

        // Default to full opacity if width is set but opacity is not
        if (!stroke_opa) {
            lv_obj_set_style_text_outline_stroke_opa(label, LV_OPA_COVER, 0);
        }

        // Default to black stroke if width is set but color is not
        if (!stroke_color) {
            lv_obj_set_style_text_outline_stroke_color(label, lv_color_black(), 0);
        }

        spdlog::trace("[ui_text] Applied text stroke: width={}", width);
    }

    // Apply stroke color
    if (stroke_color) {
        lv_color_t color = lv_xml_to_color(stroke_color);
        lv_obj_set_style_text_outline_stroke_color(label, color, 0);
    }

    // Apply stroke opacity
    if (stroke_opa) {
        lv_opa_t opa = lv_xml_to_opa(stroke_opa);
        lv_obj_set_style_text_outline_stroke_opa(label, opa, 0);
    }
}

/**
 * Whether a long mode drives an offset animation while the text overflows
 *
 * LVGL runs these as LV_ANIM_REPEAT_INFINITE and invalidates the label on every
 * step, so a single overflowing label repaints its area at the display refresh
 * rate for as long as it is on screen.
 */
static bool long_mode_animates(lv_label_long_mode_t mode) {
    return mode == LV_LABEL_LONG_MODE_SCROLL || mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR;
}

/**
 * Observer: hold a scrolling label still while animations are switched off
 *
 * The long mode the XML declared is carried in the observer's user data, so the
 * label goes back to scrolling if the preference is switched on again.
 *
 * CLIP is the still counterpart of a scrolling mode, and the only one: it sets
 * the same LV_TEXT_FLAG_EXPAND, so the text measures unwrapped and the label
 * stays exactly one line, which is the height every layout around a scrolling
 * label is built for. DOTS clears that flag - the label wraps to two or three
 * lines and pushes whatever sits below it out of its container - and it also
 * rewrites the label's own text buffer with the ellipsized string, so
 * lv_label_get_text() would start returning truncated text app-wide for as long
 * as the preference is off. The ellipsis a still label shows instead comes from
 * middle_ellipsis_draw_cb, which swaps the string at draw time and leaves both
 * the buffer and the one-line geometry alone.
 */
static void animations_pref_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    lv_obj_t* label = lv_observer_get_target_obj(observer);
    if (!label) {
        return;
    }
    auto declared = static_cast<lv_label_long_mode_t>(
        reinterpret_cast<intptr_t>(lv_observer_get_user_data(observer)));
    const bool still = !helix::ui::animations_enabled(subject);
    lv_label_set_long_mode(label, still ? LV_LABEL_LONG_MODE_CLIP : declared);

    // The middle-ellipsis hook below only hears about the label's draw task
    // while this flag is set. It is set only while the label is held still:
    // with the flag on, LVGL re-measures the text on every draw instead of
    // reading the cached size, and a scrolling label draws at the refresh rate.
    if (still) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    } else {
        lv_obj_remove_flag(label, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }
}

namespace {

/**
 * The display string a still, overflowing label draws in place of its text
 *
 * Owned by the label (freed on LV_EVENT_DELETE). `shown` is what the draw task
 * renders; it stays empty while the stored text fits. The remaining fields are
 * the inputs `shown` was derived from, so the string is rebuilt only when the
 * text, the width or the font changes, never on every draw.
 */
struct MiddleEllipsis {
    std::string source;
    std::string shown;
    int32_t width = -1;
    const lv_font_t* font = nullptr;
    int32_t letter_space = 0;
};

} // namespace

/**
 * Draw hook: render "<head>…<tail>" for a still label whose text overflows
 *
 * Runs on LV_EVENT_DRAW_TASK_ADDED, between LVGL building the label's draw
 * task and a draw unit taking it, so the text pointer in the task's own copy of
 * the descriptor can be swapped for the fitted string. The label's text buffer
 * and geometry are untouched: lv_label_get_text() keeps returning what was
 * set, and CLIP keeps the label one line tall. LVGL measures the descriptor's
 * text (not the label's cached size) when this flag is set, so a centred label
 * centres the fitted string.
 *
 * Content width, font and letter spacing are read on every draw, so a resize
 * or a font-tier change - both of which invalidate the label - recomputes the
 * string on the next frame. While the label scrolls (animations on) LVGL draws
 * the text whole and this does nothing.
 */
static void middle_ellipsis_draw_cb(lv_event_t* e) {
    auto* label = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* state = static_cast<MiddleEllipsis*>(lv_event_get_user_data(e));
    lv_draw_task_t* task = lv_event_get_draw_task(e);
    if (!label || !state || !task || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_LABEL) {
        return;
    }
    if (lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_CLIP) {
        return;
    }
    lv_draw_label_dsc_t* dsc = lv_draw_task_get_label_dsc(task);
    if (!dsc || !dsc->text || !dsc->font) {
        return;
    }

    const int32_t width = lv_obj_get_content_width(label);
    if (state->width != width || state->font != dsc->font ||
        state->letter_space != dsc->letter_space || state->source != dsc->text) {
        state->width = width;
        state->font = dsc->font;
        state->letter_space = dsc->letter_space;
        state->source = dsc->text;
        std::string fitted =
            helix::ui::middle_ellipsize(state->source.c_str(), width, dsc->font, dsc->letter_space);
        state->shown = fitted == state->source ? std::string() : std::move(fitted);
    }
    if (state->shown.empty()) {
        return;
    }

    dsc->text = state->shown.c_str();
    // The hint indexes lines of the stored text, which is not what is drawn now.
    dsc->hint = nullptr;
}

static void middle_ellipsis_delete_cb(lv_event_t* e) {
    delete static_cast<MiddleEllipsis*>(lv_event_get_user_data(e));
}

/**
 * Helper to make a scrolling label obey the "Animations" preference
 *
 * A scrolling long mode repaints the label's area at the display refresh rate
 * for as long as the text overflows, and the repaint costs the render and blend
 * threads on every step. Turning animations off has to reach that, so the label
 * holds still and clips instead for as long as the preference is off. Labels
 * that do not scroll are left alone, and so is a label built before the
 * preference subject exists.
 *
 * The observer is registered against the label, so LVGL drops it when the label
 * is deleted.
 *
 * @param state XML parser state (supplies the component scope for the lookup)
 * @param label Label whose long mode was just applied from XML
 */
static void bind_long_mode_to_animations_pref(lv_xml_parser_state_t* state, lv_obj_t* label) {
    if (!label) {
        return;
    }

    lv_label_long_mode_t declared = lv_label_get_long_mode(label);
    if (!long_mode_animates(declared)) {
        return;
    }

    lv_subject_t* subject = helix::ui::animations_pref_subject(&state->scope);
    if (!subject) {
        return;
    }

    // While held still, an overflowing text is drawn as "<head>…<tail>" rather
    // than cut mid-glyph at the label's edge (prestonbrown/helixscreen#1441).
    auto* ellipsis = new MiddleEllipsis();
    lv_obj_add_event_cb(label, middle_ellipsis_draw_cb, LV_EVENT_DRAW_TASK_ADDED, ellipsis);
    lv_obj_add_event_cb(label, middle_ellipsis_delete_cb, LV_EVENT_DELETE, ellipsis);

    // Fires immediately, so the label is already in the right mode when it is
    // first drawn.
    lv_subject_add_observer_obj(subject, animations_pref_observer_cb, label,
                                reinterpret_cast<void*>(static_cast<intptr_t>(declared)));
}

/**
 * Shared XML apply callback for all text_* widgets
 *
 * Applies standard label properties plus custom stroke attributes.
 * All semantic text widgets use this same apply function.
 *
 * NOTE: Translation auto-tagging is handled in lv_xml_label_parser.c -
 * any text= attribute automatically becomes a translation lookup key.
 */
static void ui_text_apply(lv_xml_parser_state_t* state, const char** attrs) {
    // Apply label properties (text, long_mode, etc.) and base object properties
    lv_xml_label_apply(state, attrs);

    lv_obj_t* label = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));

    bind_long_mode_to_animations_pref(state, label);

    // Apply stroke attributes (stroke_width, stroke_color, stroke_opa)
    apply_stroke_attrs(label, attrs);

    // Apply text transform (uppercase) — works with bind_text and translations
    const char* transform = lv_xml_get_value_of(attrs, "text_transform");
    if (transform) {
        ui_text_apply_transform(label, transform);
    }
}

/**
 * Helper to create a semantic text label with specified font and shared text style
 *
 * @param state XML parser state
 * @param attrs XML attributes (unused)
 * @param font_const Font constant name in globals.xml
 * @param style_type Which shared text style to apply (PRIMARY or MUTED)
 */
static lv_obj_t* create_semantic_label(lv_xml_parser_state_t* state, const char** attrs,
                                       const char* font_const, TextStyleType style_type) {
    LV_UNUSED(attrs);
    lv_obj_t* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* label = lv_label_create(parent);
    apply_semantic_font(label, font_const);
    apply_shared_text_style(label, style_type);
    return label;
}

// XML create callbacks - each variant specifies font constant and style type

static void* ui_text_heading_create(lv_xml_parser_state_t* state, const char** attrs) {
    return create_semantic_label(state, attrs, "font_heading", TextStyleType::MUTED);
}

static void* ui_text_body_create(lv_xml_parser_state_t* state, const char** attrs) {
    return create_semantic_label(state, attrs, "font_body", TextStyleType::PRIMARY);
}

static void* ui_text_muted_create(lv_xml_parser_state_t* state, const char** attrs) {
    return create_semantic_label(state, attrs, "font_body", TextStyleType::MUTED);
}

static void* ui_text_small_create(lv_xml_parser_state_t* state, const char** attrs) {
    return create_semantic_label(state, attrs, "font_small", TextStyleType::MUTED);
}

static void* ui_text_xs_create(lv_xml_parser_state_t* state, const char** attrs) {
    return create_semantic_label(state, attrs, "font_xs", TextStyleType::MUTED);
}

/**
 * Create callback for text_button widget
 *
 * Creates a centered label with body font. Text color is determined later
 * in ui_text_button_apply() after parent's bg_color is available.
 */
static void* ui_text_button_create(lv_xml_parser_state_t* state, const char** attrs) {
    lv_obj_t* label = create_semantic_label(state, attrs, "font_body", TextStyleType::PRIMARY);
    if (label) {
        // Center the label within its parent (the button)
        lv_obj_set_align(label, LV_ALIGN_CENTER);
        // Also set text alignment for multi-line button labels
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    // Default text color - will be overridden in apply if parent has colored bg
    const char* color_str = lv_xml_get_const(nullptr, "text");
    if (color_str && color_str[0] == '#') {
        uint32_t hex = static_cast<uint32_t>(strtoul(color_str + 1, nullptr, 16));
        lv_obj_set_style_text_color(label, lv_color_hex(hex), 0);
    }

    // Center the label within its parent (the button)
    lv_obj_set_align(label, LV_ALIGN_CENTER);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    return label;
}

/**
 * Apply callback for text_button - recalculates contrast AFTER parent is styled
 *
 * This handles the legacy pattern of `<lv_button><text_button text="..."/></lv_button>`.
 * For new code, prefer using `<ui_button text="..."/>` which handles contrast internally.
 *
 * Uses the same contrast logic as ui_button for consistency.
 */
static void ui_text_button_apply(lv_xml_parser_state_t* state, const char** attrs) {
    // First apply standard label properties
    lv_xml_label_apply(state, attrs);

    lv_obj_t* label = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));

    bind_long_mode_to_animations_pref(state, label);

    lv_obj_t* parent = lv_obj_get_parent(label);

    if (!parent) {
        return;
    }

    // Get parent's background color (now that XML attrs have been applied)
    lv_color_t bg_color = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
    lv_opa_t bg_opa = lv_obj_get_style_bg_opa(parent, LV_PART_MAIN);

    // Only apply auto-contrast if parent has a visible background
    if (bg_opa > LV_OPA_50) {
        // The parent is a filled button, so its colour is an accent (matches ui_button)
        lv_color_t text_color = theme_manager_get_readable_on(bg_color);
        lv_obj_set_style_text_color(label, text_color, LV_PART_MAIN);
    }
}

void ui_text_init() {
    // Register custom text widgets for XML usage
    // All widgets share the same apply function (ui_text_apply) which handles
    // standard label attributes plus custom stroke_* attributes
    lv_xml_register_widget("text_heading", ui_text_heading_create, ui_text_apply);
    lv_xml_register_widget("text_body", ui_text_body_create, ui_text_apply);
    lv_xml_register_widget("text_muted", ui_text_muted_create, ui_text_apply);
    lv_xml_register_widget("text_small", ui_text_small_create, ui_text_apply);
    lv_xml_register_widget("text_xs", ui_text_xs_create, ui_text_apply);
    // text_tiny is an alias for text_xs (same size, just a more intuitive name)
    lv_xml_register_widget("text_tiny", ui_text_xs_create, ui_text_apply);
    // text_button: centered body text with AUTO-CONTRAST based on parent bg color
    lv_xml_register_widget("text_button", ui_text_button_create, ui_text_button_apply);

    spdlog::trace(
        "[ui_text] Registered semantic text widgets: text_heading, text_body, text_muted, "
        "text_small, text_xs, text_tiny, text_button");
}

void ui_text_apply_transform(lv_obj_t* label, const char* transform) {
    if (!label || !transform)
        return;

    if (strcmp(transform, "uppercase") == 0) {
        lv_label_set_text_transform_upper(label, true);
    }
}

/// Drawn width of the first @p len bytes of @p txt, as lv_draw_label lays them out.
static int32_t text_width(const char* txt, uint32_t len, const lv_font_t* font,
                          int32_t letter_space) {
    if (len == 0) {
        return 0;
    }
    lv_text_attributes_t attributes = {};
    attributes.letter_space = letter_space;
    attributes.max_width = LV_COORD_MAX;
    attributes.text_flags = LV_TEXT_FLAG_NONE;
    return lv_text_get_width(txt, len, font, &attributes);
}

/// "…" when @p font has the glyph, else the three-dot spelling LVGL's own DOTS mode uses.
static const char* ellipsis_for(const lv_font_t* font) {
    static constexpr const char* ELLIPSIS = "\xE2\x80\xA6"; // U+2026
    lv_font_glyph_dsc_t glyph;
    if (lv_font_get_glyph_dsc(font, &glyph, 0x2026, 0) && !glyph.is_placeholder) {
        return ELLIPSIS;
    }
    return "...";
}

namespace helix::ui {

std::string middle_ellipsize(const char* text, int32_t max_width, const lv_font_t* font,
                             int32_t letter_space) {
    if (!text) {
        return std::string();
    }
    std::string source(text);
    if (!font || max_width <= 0 || source.empty()) {
        return source;
    }
    const auto len = static_cast<uint32_t>(source.size());
    if (text_width(text, len, font, letter_space) <= max_width) {
        return source;
    }

    const char* ellipsis = ellipsis_for(font);
    const int32_t budget = max_width - text_width(ellipsis, static_cast<uint32_t>(strlen(ellipsis)),
                                                  font, letter_space);
    if (budget <= 0) {
        return ellipsis;
    }

    // Head: whole glyphs from the start, up to half the budget (the odd pixel
    // goes to the head, which carries the name).
    const int32_t head_budget = (budget + 1) / 2;
    uint32_t head_end = 0;
    while (head_end < len) {
        uint32_t next = head_end;
        lv_text_encoded_next(text, &next);
        if (next > len || text_width(text, next, font, letter_space) > head_budget) {
            break;
        }
        head_end = next;
    }

    // Tail: whole glyphs from the end, into whatever the head left over.
    const int32_t tail_budget = budget - text_width(text, head_end, font, letter_space);
    uint32_t tail_start = len;
    while (tail_start > head_end) {
        uint32_t prev = tail_start;
        lv_text_encoded_prev(text, &prev);
        if (prev <= head_end ||
            text_width(text + prev, len - prev, font, letter_space) > tail_budget) {
            break;
        }
        tail_start = prev;
    }

    // Glyph widths are not additive across a join (kerning), so measure the
    // composed string and shave the tail while it still overflows.
    std::string fitted;
    for (;;) {
        fitted = source.substr(0, head_end) + ellipsis + source.substr(tail_start);
        if (tail_start >= len || text_width(fitted.c_str(), static_cast<uint32_t>(fitted.size()),
                                            font, letter_space) <= max_width) {
            break;
        }
        lv_text_encoded_next(text, &tail_start);
    }
    return fitted;
}

} // namespace helix::ui

void ui_text_set_stroke(lv_obj_t* label, int32_t width, lv_color_t color, lv_opa_t opa) {
    if (!label) {
        spdlog::warn("[ui_text] ui_text_set_stroke called with NULL label");
        return;
    }

    lv_obj_set_style_text_outline_stroke_width(label, width, 0);
    lv_obj_set_style_text_outline_stroke_color(label, color, 0);
    lv_obj_set_style_text_outline_stroke_opa(label, opa, 0);

    spdlog::trace("[ui_text] Applied text stroke: width={}, opa={}", width, opa);
}
