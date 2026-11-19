// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pinned (compact) callout chip, shared by the home printer image widget
// and the image tagger's review so both draw and size it the same way. A
// compact chip is styles.activity_chip around its text in font_xs, with no
// icon; the light chip, which has no text, keeps its xs icon.

#include "ui_icon_codepoints.h"

#include "text_measure.h"
#include "theme_manager.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace helix::ui {

/// How a full-size callout chip styled like a live `activity_chip` measures:
/// chip_text is a text_small (font_small) and every chip icon is size="xs"
/// (icon_font_xs). Padding and border come from the live chip's own style, so
/// they follow the theme. Every printer picture with callouts sizes its chips
/// through this one measure.
struct CalloutChipMetrics {
    const lv_font_t* text_font = nullptr;
    const lv_font_t* icon_font = nullptr;
    int chrome_w = 0; ///< padding and border, both sides
    int col_gap = 0;  ///< between the icons and the label
    int comfort = 0;  ///< measured and rendered text disagree by a few pixels
    int chip_h = 0;   ///< a chip with one line of text
    int chrome_h = 0; ///< padding and border, top and bottom
    int text_line_h = 0;
    int icon_line_h = 0;

    [[nodiscard]] int icon_px(const char* name) const {
        return measure_text_px(icon::lookup_codepoint(name), icon_font);
    }

    /// A chip with text gets the comfort margin; an icon-only chip is exactly its glyphs.
    [[nodiscard]] int chip_w(int icons, const std::string& text) const {
        if (text.empty())
            return chrome_w + icons;
        return chrome_w + icons + col_gap + widest_line_px(text, text_font) + comfort;
    }

    /// A chip's height for its text: one text line, or as many as it breaks into.
    [[nodiscard]] int chip_h_for(const std::string& text) const {
        return chrome_h + std::max(text_line_count(text) * text_line_h, icon_line_h);
    }
};

inline CalloutChipMetrics measure_callout_chip(lv_obj_t* probe) {
    CalloutChipMetrics m;
    m.text_font = theme_manager_get_font("font_small");
    m.icon_font = theme_manager_get_font("icon_font_xs");
    const int border = lv_obj_get_style_border_width(probe, LV_PART_MAIN);
    m.chrome_w = lv_obj_get_style_pad_left(probe, LV_PART_MAIN) +
                 lv_obj_get_style_pad_right(probe, LV_PART_MAIN) + 2 * border;
    m.col_gap = lv_obj_get_style_pad_column(probe, LV_PART_MAIN);
    m.comfort = theme_manager_get_spacing("space_md");
    m.chrome_h = lv_obj_get_style_pad_top(probe, LV_PART_MAIN) +
                 lv_obj_get_style_pad_bottom(probe, LV_PART_MAIN) + 2 * border;
    m.text_line_h = lv_font_get_line_height(m.text_font);
    m.icon_line_h = lv_font_get_line_height(m.icon_font);
    m.chip_h = m.chip_h_for({});
    return m;
}

/// A pinned chip sits on the picture, so it drops the unit letter ("60 / 70°").
inline std::string compact_callout_text(std::string s) {
    static constexpr char kDegree[] = "°";
    for (const char* unit : {"°C", "°F"}) {
        for (size_t p; (p = s.find(unit)) != std::string::npos;)
            s.erase(p + sizeof(kDegree) - 1, 1);
    }
    return s;
}

/// The merged toolhead chip's text: the nozzle reading and the part-fan
/// percentage, two spaces apart.
[[nodiscard]] inline std::string toolhead_chip_text(std::string_view nozzle_text,
                                                    std::string_view fan_pct) {
    std::string s(nozzle_text);
    s += "  ";
    s += fan_pct;
    return s;
}

/// Width of a compact chip styled like `chip`: chrome plus its compact text in
/// font_xs and a comfort margin (measured and rendered text disagree by a few
/// pixels). An icon-only chip (empty text) is chrome plus `icon_px`.
inline int compact_callout_chip_w(lv_obj_t* chip, const std::string& text, int icon_px) {
    const int chrome = lv_obj_get_style_pad_left(chip, LV_PART_MAIN) +
                       lv_obj_get_style_pad_right(chip, LV_PART_MAIN) +
                       2 * lv_obj_get_style_border_width(chip, LV_PART_MAIN);
    if (text.empty())
        return chrome + icon_px;
    return chrome + widest_line_px(compact_callout_text(text), theme_manager_get_font("font_xs")) +
           theme_manager_get_spacing("space_md");
}

/// Height of a compact chip styled like `chip` with `lines` lines of text. At
/// some breakpoints the xs icon line is taller than a font_xs line, so a layout
/// with the light chip showing slots every chip at the icon's height.
inline int compact_callout_chip_h(lv_obj_t* chip, bool with_icon, int lines = 1) {
    int line = lines * lv_font_get_line_height(theme_manager_get_font("font_xs"));
    if (with_icon)
        line = std::max(line, int(lv_font_get_line_height(theme_manager_get_font("icon_font_xs"))));
    return line + lv_obj_get_style_pad_top(chip, LV_PART_MAIN) +
           lv_obj_get_style_pad_bottom(chip, LV_PART_MAIN) +
           2 * lv_obj_get_style_border_width(chip, LV_PART_MAIN);
}

} // namespace helix::ui
