// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "theme_manager.h"

namespace helix {

/// The theme fonts a sized home widget measures its rungs with.
struct PanelWidgetFonts {
    const lv_font_t* xs;
    const lv_font_t* small;
    const lv_font_t* body;
    const lv_font_t* heading;
    const lv_font_t* icon; ///< icon_font_xs
    const lv_font_t* icon_lg;
};

inline PanelWidgetFonts load_panel_widget_fonts() {
    return {theme_manager_get_font("font_xs"),      theme_manager_get_font("font_small"),
            theme_manager_get_font("font_body"),    theme_manager_get_font("font_heading"),
            theme_manager_get_font("icon_font_xs"), theme_manager_get_font("icon_font_lg")};
}

} // namespace helix
