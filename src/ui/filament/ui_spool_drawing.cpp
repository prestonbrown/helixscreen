// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spool_drawing.h"

lv_color_t ui_color_darken(lv_color_t c, uint8_t amt) {
    return lv_color_make(c.red > amt ? c.red - amt : 0, c.green > amt ? c.green - amt : 0,
                         c.blue > amt ? c.blue - amt : 0);
}

lv_color_t ui_color_lighten(lv_color_t c, uint8_t amt) {
    return lv_color_make((c.red + amt > 255) ? 255 : c.red + amt,
                         (c.green + amt > 255) ? 255 : c.green + amt,
                         (c.blue + amt > 255) ? 255 : c.blue + amt);
}

void ui_draw_spool_box(lv_layer_t* layer, int32_t cx, int32_t cy, lv_color_t color, bool has_spool,
                       int32_t sensor_r) {
    int32_t box_w = sensor_r * 3;
    int32_t box_h = sensor_r * 4;
    int32_t radius = LV_MAX(2, sensor_r / 2);

    lv_area_t box_area = {cx - box_w / 2, cy - box_h / 2, cx + box_w / 2, cy + box_h / 2};

    if (has_spool) {
        // Shadow (darker, slightly offset)
        lv_draw_rect_dsc_t shadow_dsc;
        lv_draw_rect_dsc_init(&shadow_dsc);
        shadow_dsc.radius = radius;
        shadow_dsc.bg_color = ui_color_darken(color, 40);
        shadow_dsc.bg_opa = LV_OPA_COVER;
        lv_area_t shadow_area = box_area;
        shadow_area.x1 += 1;
        shadow_area.y1 += 1;
        shadow_area.x2 += 1;
        shadow_area.y2 += 1;
        lv_draw_rect(layer, &shadow_dsc, &shadow_area);

        // Main body in filament color
        lv_draw_rect_dsc_t body_dsc;
        lv_draw_rect_dsc_init(&body_dsc);
        body_dsc.radius = radius;
        body_dsc.bg_color = color;
        body_dsc.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &body_dsc, &box_area);

        // Highlight border (top + left edges)
        lv_draw_rect_dsc_t hl_dsc;
        lv_draw_rect_dsc_init(&hl_dsc);
        hl_dsc.radius = radius;
        hl_dsc.bg_opa = LV_OPA_TRANSP;
        hl_dsc.border_color = ui_color_lighten(color, 40);
        hl_dsc.border_opa = LV_OPA_50;
        hl_dsc.border_width = 1;
        hl_dsc.border_side =
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
        lv_draw_rect(layer, &hl_dsc, &box_area);
    } else {
        // Empty box: hollow outline
        lv_draw_rect_dsc_t outline_dsc;
        lv_draw_rect_dsc_init(&outline_dsc);
        outline_dsc.radius = radius;
        outline_dsc.bg_opa = LV_OPA_TRANSP;
        outline_dsc.border_color = color;
        outline_dsc.border_opa = LV_OPA_40;
        outline_dsc.border_width = 1;
        lv_draw_rect(layer, &outline_dsc, &box_area);

        // Draw "+" indicator in center
        int32_t plus_size = LV_MAX(3, sensor_r);
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = color;
        line_dsc.opa = LV_OPA_40;
        line_dsc.width = 1;
        // Horizontal bar
        line_dsc.p1.x = cx - plus_size / 2;
        line_dsc.p1.y = cy;
        line_dsc.p2.x = cx + plus_size / 2;
        line_dsc.p2.y = cy;
        lv_draw_line(layer, &line_dsc);
        // Vertical bar
        line_dsc.p1.x = cx;
        line_dsc.p1.y = cy - plus_size / 2;
        line_dsc.p2.x = cx;
        line_dsc.p2.y = cy + plus_size / 2;
        lv_draw_line(layer, &line_dsc);
    }
}
