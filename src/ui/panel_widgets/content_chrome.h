// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <algorithm>

namespace helix {

/// What sits between a widget's outer box and the box its content draws in.
struct ContentChrome {
    int w = 0;
    int h = 0;
};

/// One line of @p font, in pixels; 0 for a font that is not loaded.
inline int line_px(const lv_font_t* font) {
    return font ? static_cast<int>(lv_font_get_line_height(font)) : 0;
}

/// The width a text button adds around its label: its side padding token (button_padding
/// unless the XML sets another) and a border pixel on each side.
inline int button_label_chrome_w(const char* pad_token = "button_padding") {
    return 2 * (theme_manager_get_spacing(pad_token) + 1);
}

/// Which gap of the branching container measure_content_chrome() charges.
enum class BranchGap {
    /// The gap flex draws between the parts visible now. Visibility that follows the
    /// verdict makes the next verdict follow the last one.
    Visible,
    /// The gap whenever the container holds more than one part, visible or not, for
    /// needs that leave the gap between the parts to the chrome.
    Always,
    /// None, for needs that count every gap of the layout they measure.
    None,
};

/// The chrome of the single-child chain from `root` down to the first container
/// that holds more than one child: the padding of every container on the way, and
/// the flex gap of that branching container as @p branch_gap says. Flex leaves hidden
/// children out, so a container with at most one visible child draws no gap.
///
/// Measured from the tree because container padding is a theme value that varies by
/// widget and by breakpoint; a constant guess is either too small, and the content
/// spills out of its container, or too large, and the widget draws smaller than it
/// needs to. `reserve_px` is held back on each side on top of whatever is measured,
/// since a measured-exact fit renders as an overlap by a pixel or two.
/// `row_inset_px` is a side inset charged to row candidates alone, so it is not
/// chrome of a container whose flow is already a row.
inline ContentChrome measure_content_chrome(lv_obj_t* root, int reserve_px, int row_inset_px = 0,
                                            BranchGap branch_gap = BranchGap::Visible) {
    ContentChrome chrome{2 * reserve_px, 2 * reserve_px};
    for (lv_obj_t* o = root; o != nullptr; o = lv_obj_get_child(o, 0)) {
        int side_pads = static_cast<int>(lv_obj_get_style_pad_left(o, LV_PART_MAIN)) +
                        static_cast<int>(lv_obj_get_style_pad_right(o, LV_PART_MAIN));
        if (lv_obj_get_style_flex_flow(o, LV_PART_MAIN) == LV_FLEX_FLOW_ROW) {
            side_pads -= std::min(side_pads, row_inset_px);
        }
        chrome.w += side_pads;
        chrome.h += static_cast<int>(lv_obj_get_style_pad_top(o, LV_PART_MAIN)) +
                    static_cast<int>(lv_obj_get_style_pad_bottom(o, LV_PART_MAIN));
        const uint32_t children = lv_obj_get_child_count(o);
        if (children != 1) {
            if (branch_gap == BranchGap::None)
                break;
            uint32_t parts = children;
            if (branch_gap == BranchGap::Visible) {
                parts = 0;
                for (uint32_t i = 0; i < children; ++i) {
                    if (!lv_obj_is_hidden(lv_obj_get_child(o, static_cast<int32_t>(i)))) {
                        ++parts;
                    }
                }
            }
            if (parts > 1) {
                chrome.w += static_cast<int>(lv_obj_get_style_pad_column(o, LV_PART_MAIN));
                chrome.h += static_cast<int>(lv_obj_get_style_pad_row(o, LV_PART_MAIN));
            }
            break;
        }
    }
    return chrome;
}

/// measure_content_chrome() of @p root once its tree exists. Before it does (the load
/// path asks fits_at() then), the chrome of a root padded @p pad_hor_token on each side
/// and @p pad_ver_token above and below, with @p row_gap_token between its rows and
/// @p col_gap_token between its columns. A null gap token is no gap.
inline ContentChrome content_chrome_or_estimate(lv_obj_t* root, const char* pad_hor_token,
                                                const char* pad_ver_token,
                                                const char* row_gap_token = nullptr,
                                                const char* col_gap_token = nullptr,
                                                BranchGap branch_gap = BranchGap::Visible) {
    if (root) {
        return measure_content_chrome(root, 0, 0, branch_gap);
    }
    const auto px = [](const char* token) { return token ? theme_manager_get_spacing(token) : 0; };
    return {2 * px(pad_hor_token) + px(col_gap_token), 2 * px(pad_ver_token) + px(row_gap_token)};
}

/// content_chrome_or_estimate() with no gap (BranchGap::None), for needs that count
/// every gap of the layout they measure.
inline ContentChrome content_padding_or_estimate(lv_obj_t* root, const char* pad_hor_token,
                                                 const char* pad_ver_token) {
    return content_chrome_or_estimate(root, pad_hor_token, pad_ver_token, nullptr, nullptr,
                                      BranchGap::None);
}

} // namespace helix
