// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_toolhead_view.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "nozzle_renderer_dispatch.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

namespace {

helix::ToolheadStyle configured_style() {
    return helix::SettingsManager::instance().get_effective_toolhead_style();
}

} // namespace

int32_t toolhead_drawn_scale(ToolheadStyle style, int32_t scale) {
    return style == ToolheadStyle::A4T ? scale * 6 / 5 : scale;
}

ToolheadFit fit_toolhead(ToolheadStyle style, const lv_area_t& box, int32_t max_scale) {
    const int32_t box_w = lv_area_get_width(&box);
    const int32_t box_h = lv_area_get_height(&box);
    auto bounds_at = [style](int32_t scale) {
        return toolhead_bounds(style, 0, 0, helix::ui::toolhead_drawn_scale(style, scale));
    };
    auto fits = [&](int32_t scale) {
        const GlyphBounds g = bounds_at(scale);
        return g.right - g.left + 1 <= box_w && g.bottom - g.top + 1 <= box_h;
    };
    // The glyph grows monotonically with scale: binary-search the largest that fits.
    int32_t lo = 1;
    int32_t hi = std::max<int32_t>(1, max_scale);
    while (lo < hi) {
        const int32_t mid = lo + (hi - lo + 1) / 2;
        if (fits(mid)) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    const GlyphBounds g = bounds_at(lo);
    const int32_t gw = g.right - g.left + 1;
    const int32_t gh = g.bottom - g.top + 1;
    return {box.x1 + (box_w - gw) / 2 - g.left, box.y1 + (box_h - gh) / 2 - g.top, lo};
}

void draw_toolhead(lv_layer_t* layer, int32_t cx, int32_t cy, std::optional<lv_color_t> filament,
                   int32_t scale, lv_opa_t opa) {
    draw_nozzle_for_style(layer, cx, cy, filament,
                          helix::ui::toolhead_drawn_scale(configured_style(), scale), opa);
}

GlyphBounds toolhead_glyph_bounds(int32_t cx, int32_t cy, int32_t scale) {
    const auto style = configured_style();
    return helix::ui::toolhead_bounds(style, cx, cy, helix::ui::toolhead_drawn_scale(style, scale));
}

void draw_toolhead_fitted(lv_layer_t* layer, const lv_area_t& box,
                          std::optional<lv_color_t> filament, int32_t max_scale, lv_opa_t opa) {
    const auto fit = helix::ui::fit_toolhead(configured_style(), box, max_scale);
    draw_toolhead(layer, fit.cx, fit.nozzle_y, filament, fit.scale, opa);
}

// ---------------------------------------------------------------------------
// <toolhead_view>: the file is the widget (declarative-ui structural exception)
// ---------------------------------------------------------------------------

namespace {

void toolhead_view_draw_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t box;
    lv_obj_get_content_coords(obj, &box);
    if (lv_area_get_width(&box) <= 0 || lv_area_get_height(&box) <= 0) {
        return;
    }
    draw_toolhead_fitted(layer, box, std::nullopt);
}

void toolhead_style_changed_cb(lv_observer_t* observer, lv_subject_t* /*subject*/) {
    lv_obj_invalidate(static_cast<lv_obj_t*>(lv_observer_get_target(observer)));
}

void* toolhead_view_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, toolhead_view_draw_cb, LV_EVENT_DRAW_MAIN,
                        nullptr); // DECLARATIVE_OK: draw hook
    if (lv_subject_t* style = helix::SettingsManager::instance().subject_toolhead_style()) {
        // Removed with the widget (observer bound to obj).
        lv_subject_add_observer_obj(style, toolhead_style_changed_cb, obj, nullptr);
    }
    return obj;
}

} // namespace

void ui_toolhead_view_register() {
    lv_xml_register_widget("toolhead_view", toolhead_view_xml_create, lv_xml_obj_apply);
    spdlog::trace("[ToolheadView] Registered <toolhead_view> widget");
}

} // namespace helix::ui
