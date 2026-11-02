// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_badges.h"

#include "ui_text.h"

#include "display_numbering.h"
#include "gcode_parser.h"
#include "lvgl.h"
#include "printer_excluded_objects_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "text_io.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>

namespace helix::ui {

std::vector<ObjectBadge> compute_object_badges(const PrinterExcludedObjectsState& state,
                                               const gcode::ParsedGCodeFile* parsed) {
    const auto& defined = state.get_defined_objects();
    const auto& excluded = state.get_excluded_objects();
    const auto& current = state.get_current_object();

    std::vector<ObjectBadge> badges;
    badges.reserve(defined.size());
    for (int i = 0; i < static_cast<int>(defined.size()); ++i) {
        ObjectBadge b;
        b.defined_index = i;
        b.name = defined[static_cast<size_t>(i)];
        b.number = lane_number_text(i);
        b.excluded = excluded.count(b.name) > 0;
        b.current = !current.empty() && b.name == current;

        const auto info = state.get_object_geometry(b.name);
        const gcode::GCodeObject* pobj = nullptr;
        if (parsed) {
            auto it = parsed->objects.find(b.name);
            if (it != parsed->objects.end()) {
                pobj = &it->second;
            }
        }
        const bool parsed_bbox = pobj && !pobj->bounding_box.is_empty();
        if (parsed_bbox) {
            b.top_z = pobj->bounding_box.max.z;
        }

        // The parser has no "CENTER present" flag; an unset CENTER reads (0,0),
        // so the origin falls through to the bbox (which sits there too if real).
        const bool parsed_center = pobj && pobj->center != glm::vec2(0.0f);

        if (info && info->has_center) {
            b.anchor = info->center;
        } else if (parsed_center) {
            b.anchor = pobj->center;
        } else if (info && info->has_bbox) {
            b.anchor = (info->bbox_min + info->bbox_max) * 0.5f;
        } else if (parsed_bbox) {
            const glm::vec3 c = pobj->bounding_box.center();
            b.anchor = {c.x, c.y};
        } else {
            badges.push_back(std::move(b));
            continue;
        }
        b.has_anchor = true;
        badges.push_back(std::move(b));
    }
    return badges;
}

std::string object_display_name(const std::string& name, int defined_index, const lv_font_t* font) {
    if (!name.empty() && font_renders_text(font, name.c_str())) {
        return name;
    }
    if (defined_index < 0) {
        return lv_tr("Object");
    }
    return fmt::format(fmt::runtime(lv_tr("Object {}")), lane_number_text(defined_index));
}

int defined_object_index(const std::vector<std::string>& defined, const std::string& name) {
    const std::string wanted = helix::text_io::to_upper(name);
    for (size_t i = 0; i < defined.size(); ++i) {
        if (helix::text_io::to_upper(defined[i]) == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string object_display_name(const std::vector<std::string>& defined, const std::string& name,
                                const lv_font_t* font) {
    return object_display_name(name, defined_object_index(defined, name), font);
}

std::string object_message_name(const std::vector<std::string>& defined, const std::string& name) {
    return object_display_name(defined, name, theme_manager_get_font("font_body"));
}

lv_color_t object_badge_color(int defined_index) {
    return theme_manager_get_object_palette_color(defined_index);
}

lv_color_t object_badge_text_color(lv_color_t fill) {
    return theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"), fill);
}

const lv_font_t* object_badge_font() {
    return theme_manager_get_font("font_small");
}

int32_t object_badge_diameter() {
    const lv_font_t* font = object_badge_font();
    return helix::line_px(font);
}

static int look_subject_int(lv_subject_t* subject) {
    return subject ? lv_subject_get_int(subject) : -1;
}

bool badge_look_current(const BadgeLook& look) {
    return look.theme_generation == look_subject_int(theme_manager_get_changed_subject()) &&
           look.breakpoint == look_subject_int(theme_manager_get_breakpoint_subject());
}

BadgeLook resolve_badge_look(const std::vector<ObjectBadge>& badges) {
    BadgeLook look;
    look.theme_generation = look_subject_int(theme_manager_get_changed_subject());
    look.breakpoint = look_subject_int(theme_manager_get_breakpoint_subject());
    look.font = object_badge_font();
    look.diameter = object_badge_diameter();
    look.ring_color = theme_manager_get_color("success");
    look.ring_width = theme_manager_get_spacing("space_xxs");
    look.fill.reserve(badges.size());
    look.text.reserve(badges.size());
    for (const auto& b : badges) {
        const lv_color_t fill = object_badge_color(b.defined_index);
        look.fill.push_back(fill);
        look.text.push_back(object_badge_text_color(fill));
    }
    return look;
}

void draw_object_badge(lv_layer_t* layer, const BadgeLook& look, size_t i, const ObjectBadge& badge,
                       int32_t cx, int32_t cy) {
    const int32_t d = look.diameter;
    const int32_t r = d / 2;
    const lv_area_t area = {cx - r, cy - r, cx - r + d - 1, cy - r + d - 1};
    const lv_opa_t opa = object_badge_opa(badge.excluded);

    lv_draw_rect_dsc_t disc;
    lv_draw_rect_dsc_init(&disc);
    disc.bg_color = look.fill[i];
    disc.bg_opa = opa;
    disc.radius = LV_RADIUS_CIRCLE;
    if (badge.current && !badge.excluded) {
        disc.outline_color = look.ring_color;
        disc.outline_width = look.ring_width;
        disc.outline_opa = opa;
    }
    lv_draw_rect(layer, &disc, &area);

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = look.font;
    label.color = look.text[i];
    label.opa = opa;
    label.align = LV_TEXT_ALIGN_CENTER;
    label.text = badge.number.c_str();
    label.text_local = 1;
    // The disc is one font line tall, so the label box is the disc itself.
    lv_draw_label(layer, &label, &area);
}

} // namespace helix::ui
