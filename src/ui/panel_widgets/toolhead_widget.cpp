// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolhead_widget.h"

#include "ui_drag_surface.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_panel_motion.h"
#include "ui_subject_registry.h"

#include "app_globals.h"
#include "bed_occupancy_state.h"
#include "bed_plate_context.h"
#include "bed_plate_draw.h"
#include "display_numbering.h"
#include "helix_psram_attr.h"
#include "home_macro_run.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "moonraker_error.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_capabilities_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/content_chrome.h"
#include "src/ui/panel_widgets/live_tile_owner.h"
#include "subject_managed_panel.h"
#include "text_measure.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "toolhead_homing.h"
#include "toolhead_preset_config_modal.h"
#include "unit_conversions.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using helix::ui::copy_string_if_changed;
using helix::ui::measure_text_px;
using helix::ui::set_int_if_changed;

namespace {

// The verdict and readings the widget publishes, read by panel_widget_toolhead.xml.
// Module-level because the widget is single-instance and registration rides
// SubjectInitializer's panel-subject phase, so the subjects exist before any XML that
// binds them is parsed.
HELIX_PSRAM_BSS lv_subject_t s_rung_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_letters_subject{};
HELIX_PSRAM_BSS lv_subject_t s_z_only_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_rows_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_plate_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_home_subject{};
HELIX_PSRAM_BSS lv_subject_t s_show_status_subject{};
HELIX_PSRAM_BSS lv_subject_t s_homed_subjects[3]{};
HELIX_PSRAM_BSS lv_subject_t s_x_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_y_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_z_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_tool_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_speed_text_subject{};
HELIX_PSRAM_BSS lv_subject_t s_plate_interactive_subject{};
HELIX_PSRAM_BSS lv_subject_t s_ghost_active_subject{};
HELIX_PSRAM_BSS lv_subject_t s_target_shown_subject{};
HELIX_PSRAM_BSS lv_subject_t s_ghost_text_subject{};
HELIX_PSRAM_BSS char s_x_text_buf[16];
HELIX_PSRAM_BSS char s_y_text_buf[16];
HELIX_PSRAM_BSS char s_z_text_buf[16];
HELIX_PSRAM_BSS char s_tool_text_buf[32];
HELIX_PSRAM_BSS char s_speed_text_buf[24];
HELIX_PSRAM_BSS char s_ghost_text_buf[256];
HELIX_PSRAM_BSS lv_subject_t s_show_presets_subject{};
HELIX_PSRAM_BSS lv_subject_t s_chip_icon_only_subject{};
HELIX_PSRAM_BSS lv_subject_t s_lift_text_subject{};
HELIX_PSRAM_BSS char s_lift_text_buf[160];
HELIX_PSRAM_BSS lv_subject_t s_preset_icon_subjects[helix::kToolheadPresetSlotCount]{};
HELIX_PSRAM_BSS lv_subject_t s_preset_label_subjects[helix::kToolheadPresetSlotCount]{};
HELIX_PSRAM_BSS lv_subject_t s_preset_state_subjects[helix::kToolheadPresetSlotCount]{};
HELIX_PSRAM_BSS char s_preset_icon_bufs[helix::kToolheadPresetSlotCount][32];
HELIX_PSRAM_BSS char s_preset_label_bufs[helix::kToolheadPresetSlotCount][64];
helix::WidgetSubjects s_subjects{"ToolheadWidgetSubjects"};
// The instance bound to the live tile. Only it publishes and acts: a transient
// instance (a fits_at probe, a rebuild's outgoing widget) must not overwrite the
// tile's verdict.
helix::LiveTileOwner<helix::ToolheadWidget> s_tile;

constexpr const char* kHomedSubjectNames[3] = {"toolhead_homed_x", "toolhead_homed_y",
                                               "toolhead_homed_z"};
constexpr const char* kPresetIconNames[helix::kToolheadPresetSlotCount] = {
    "toolhead_preset_0_icon", "toolhead_preset_1_icon", "toolhead_preset_2_icon"};
constexpr const char* kPresetLabelNames[helix::kToolheadPresetSlotCount] = {
    "toolhead_preset_0_label", "toolhead_preset_1_label", "toolhead_preset_2_label"};
/// 0 an empty slot (hidden), 1 inert (disabled), 2 live.
constexpr const char* kPresetStateNames[helix::kToolheadPresetSlotCount] = {
    "toolhead_preset_0_state", "toolhead_preset_1_state", "toolhead_preset_2_state"};
constexpr int kSlotEmpty = 0;
constexpr int kSlotInert = 1;
constexpr int kSlotLive = 2;

void toolhead_init_subjects() {
    s_subjects.ensure_init([] {
        UI_MANAGED_SUBJECT_INT(s_rung_subject, static_cast<int>(helix::SizeRung::Wide),
                               "toolhead_rung", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_letters_subject, 0, "toolhead_show_letters", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_z_only_subject, 0, "toolhead_z_only", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_rows_subject, 0, "toolhead_show_rows", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_plate_subject, 0, "toolhead_show_plate", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_home_subject, 0, "toolhead_show_home", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_status_subject, 0, "toolhead_show_status", s_subjects);
        for (int i = 0; i < 3; ++i) {
            UI_MANAGED_SUBJECT_INT(s_homed_subjects[i], 0, kHomedSubjectNames[i], s_subjects);
        }
        UI_MANAGED_SUBJECT_STRING(s_x_text_subject, s_x_text_buf, "--", "toolhead_x_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_y_text_subject, s_y_text_buf, "--", "toolhead_y_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_z_text_subject, s_z_text_buf, "--", "toolhead_z_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_tool_text_subject, s_tool_text_buf, "", "toolhead_tool_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_speed_text_subject, s_speed_text_buf, "", "toolhead_speed_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_INT(s_plate_interactive_subject, 0, "toolhead_plate_interactive",
                               s_subjects);
        UI_MANAGED_SUBJECT_INT(s_ghost_active_subject, 0, "toolhead_ghost_active", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_target_shown_subject, 0, "toolhead_target_shown", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_ghost_text_subject, s_ghost_text_buf, "", "toolhead_ghost_text",
                                  s_subjects);
        UI_MANAGED_SUBJECT_INT(s_show_presets_subject, 0, "toolhead_show_presets", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_chip_icon_only_subject, 0, "toolhead_chip_icon_only", s_subjects);
        UI_MANAGED_SUBJECT_STRING(s_lift_text_subject, s_lift_text_buf, "", "toolhead_lift_text",
                                  s_subjects);
        for (std::size_t i = 0; i < helix::kToolheadPresetSlotCount; ++i) {
            UI_MANAGED_SUBJECT_STRING(s_preset_icon_subjects[i], s_preset_icon_bufs[i], "target",
                                      kPresetIconNames[i], s_subjects);
            UI_MANAGED_SUBJECT_STRING(s_preset_label_subjects[i], s_preset_label_bufs[i], "",
                                      kPresetLabelNames[i], s_subjects);
            UI_MANAGED_SUBJECT_INT(s_preset_state_subjects[i], kSlotEmpty, kPresetStateNames[i],
                                   s_subjects);
        }
    });
}

struct Fonts {
    const lv_font_t* xs;
    const lv_font_t* small;
    const lv_font_t* body;
    const lv_font_t* icon;
};

Fonts load_fonts() {
    return {theme_manager_get_font("font_xs"), theme_manager_get_font("font_small"),
            theme_manager_get_font("font_body"), theme_manager_get_font("icon_font_xs")};
}

using helix::line_px;

/// The widest value any axis can show, so the rung does not change as the numbers do.
int widest_value_px(const lv_font_t* font, bool compact = false) {
    using helix::Axis;
    return std::max(
        {measure_text_px(helix::format_toolhead_axis(Axis::X, -88888, true, compact), font),
         measure_text_px(helix::format_toolhead_axis(Axis::Z, 88888, true, compact), font)});
}

/// The minimum content box each rung draws in. Mirrors panel_widget_toolhead.xml:
/// spacing is the XML's pad_row (space_xxs) and pad_column (space_xs). Idle and printing
/// share the boxes, and Full is measured for the tallest footer.
helix::RungNeeds measure_needs() {
    using helix::rung_box;
    const Fonts f = load_fonts();
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int col_gap = theme_manager_get_spacing("space_xs");
    // Measured and rendered widths disagree by a few pixels; a text rung is never
    // budgeted to the last one.
    const int comfort = theme_manager_get_spacing("space_xs");
    const int tight = std::max<int>(2, theme_manager_get_spacing("space_xxs") / 2);
    // A box measured exactly renders as an overlap by a pixel.
    constexpr int kSlackPx = 1;
    const int button_px = theme_manager_get_spacing("button_height_sm");
    const int dot_px = theme_manager_get_spacing("space_sm");

    const int icon_w = measure_text_px(helix::ui::icon::lookup_codepoint("axis_z_arrow"), f.icon);
    const int icon_h = line_px(f.icon);
    const int xs_h = line_px(f.xs);
    const int small_h = line_px(f.small);
    const int body_h = line_px(f.body);

    const int letters_w = measure_text_px("X", f.body) + measure_text_px("Y", f.body) +
                          measure_text_px("Z", f.body) + 2 * col_gap;
    const int z_xs_w =
        measure_text_px(helix::format_toolhead_axis(helix::Axis::Z, 88888, true), f.xs);

    helix::RungNeeds needs;
    // Glance holds either the letters or the Z height stacked under its glyph.
    needs.glance = rung_box(std::max({letters_w, icon_w, z_xs_w}) + tight,
                            std::max(body_h, icon_h + xs_h) + kSlackPx);
    needs.strip = rung_box(std::max(letters_w, icon_w + col_gap + z_xs_w) + comfort,
                           std::max({body_h, icon_h, xs_h}) + kSlackPx);

    const int letter_xs_w = measure_text_px("X", f.xs);
    needs.column = rung_box(letter_xs_w + row_gap + widest_value_px(f.xs, true) + tight,
                            3 * xs_h + 2 * row_gap + comfort + kSlackPx);

    const int letter_w = measure_text_px("X", f.body);
    needs.cell = rung_box(letter_w + row_gap + widest_value_px(f.body) + row_gap + dot_px + comfort,
                          3 * body_h + 2 * row_gap + comfort + kSlackPx);

    // The plate needs enough pixels for a crosshair to read against its Z bar.
    const int plate_min = 3 * theme_manager_get_spacing("space_xl");
    const int bar_w = theme_manager_get_spacing("space_xs");
    const int value_row_w = letter_w + row_gap + widest_value_px(f.body);
    needs.wide = rung_box(plate_min + bar_w + col_gap + value_row_w + comfort,
                          std::max(plate_min, 3 * body_h + 2 * row_gap) + comfort + kSlackPx);

    // Full lays the three axes in a row under the plate, in the small face.
    const int full_row_w = measure_text_px("X", f.small) + row_gap + widest_value_px(f.small);
    const int footer_h = small_h + row_gap + button_px;
    const int values_row_w = 3 * full_row_w + 2 * theme_manager_get_spacing("space_sm");
    needs.full = rung_box(std::max(values_row_w, plate_min + bar_w + col_gap) + comfort,
                          plate_min + row_gap + small_h + row_gap + footer_h + comfort + kSlackPx);
    return needs;
}

/// The height the preset row and its lift line add under the Full rung's content. Mirrors
/// panel_widget_toolhead.xml: each slot is an icon over a font_small label (label pad_top
/// space_xxs) in a button padded space_xxs top and bottom, then the lift line, each after
/// the map's pad_row.
int measure_presets_extra_h() {
    const int row_gap = theme_manager_get_spacing("space_xxs");
    const int pad = theme_manager_get_spacing("space_xxs");
    const int border = theme_manager_get_spacing("border_width");
    const int small_h = line_px(theme_manager_get_font("font_small"));
    const int icon_h = line_px(theme_manager_get_font("icon_font_sm"));
    constexpr int kSlackPx = 1;
    const int button_h = icon_h + pad + small_h + 2 * pad + 2 * border;
    return row_gap + button_h + row_gap + small_h + kSlackPx;
}

/// The Move-here chip with its label, measured off the built chip: the button's padding
/// and border, its icon and label with the gap between, then the chip's gap and the
/// dismiss button. 0 before the chip exists.
int measure_labelled_chip_w(lv_obj_t* root) {
    if (!root)
        return 0;
    lv_obj_t* chip = lv_obj_find_by_name(root, "toolhead_ghost_chip");
    lv_obj_t* button = lv_obj_find_by_name(root, "toolhead_move_here");
    lv_obj_t* dismiss = lv_obj_find_by_name(root, "toolhead_ghost_dismiss");
    if (!chip || !button || !dismiss || lv_obj_get_child_count(button) == 0)
        return 0;
    // ui_button puts the icon first and the label last.
    lv_obj_t* label = lv_obj_get_child(button, -1);
    const lv_font_t* label_font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int icon_w = measure_text_px(helix::ui::icon::lookup_codepoint("cursor_move"),
                                       theme_manager_get_font("icon_font_sm"));
    const int button_w = lv_obj_get_style_space_left(button, LV_PART_MAIN) + icon_w +
                         lv_obj_get_style_pad_column(button, LV_PART_MAIN) +
                         measure_text_px(lv_tr("Move here"), label_font) +
                         lv_obj_get_style_space_right(button, LV_PART_MAIN);
    return button_w + lv_obj_get_style_pad_column(chip, LV_PART_MAIN) +
           theme_manager_get_spacing("button_height_sm");
}

/// What the plate last drew, so an unchanged position costs no redraw.
using DrawKey = std::array<int, 10>;

DrawKey draw_key(const helix::ToolheadPlateGeometry& g) {
    return {g.show_crosshair ? 1 : 0,
            static_cast<int>(std::lround(g.cross_x)),
            static_cast<int>(std::lround(g.cross_y)),
            static_cast<int>(std::lround(g.z_fraction * g.z_bar.h)),
            g.fill_from_top ? 1 : 0,
            g.circular ? 1 : 0,
            static_cast<int>(std::lround(g.plate.w)),
            static_cast<int>(std::lround(g.plate.h)),
            g.has_room ? 1 : 0,
            static_cast<int>(std::lround(g.plate.x))};
}

/// The draw state owned by the plate object: freed with it, so the draw hook never
/// outlives what it reads.
struct PlateView {
    helix::ToolheadPlateInput input;
    std::optional<DrawKey> drawn;
    /// The plate in G-code space, for the grid, the outlines and the tapped target.
    std::optional<helix::AxisBounds> area;
    std::vector<helix::BedObjectOutline> outlines;
    std::optional<helix::AxisTarget> ghost;
};

/// Maps the plate's G-code area onto the plate rectangle the geometry drew.
helix::BedCoordMapper plate_mapper(const helix::AxisBounds& area,
                                   const helix::ToolheadPlateGeometry& g) {
    return helix::bed_map_mapper(area, static_cast<int>(std::lround(g.plate.w)),
                                 static_cast<int>(std::lround(g.plate.h)));
}

lv_area_t px_area(const lv_area_t& origin, float x, float y, float w, float h) {
    lv_area_t a;
    a.x1 = origin.x1 + static_cast<int32_t>(std::lround(x));
    a.y1 = origin.y1 + static_cast<int32_t>(std::lround(y));
    a.x2 = a.x1 + std::max<int32_t>(0, static_cast<int32_t>(std::lround(w)) - 1);
    a.y2 = a.y1 + std::max<int32_t>(0, static_cast<int32_t>(std::lround(h)) - 1);
    return a;
}

/// A dashed ring: lv_draw_arc has no dash, so the dashes are short arcs.
void draw_dashed_circle(lv_layer_t* layer, const lv_area_t& box, lv_color_t color, int width_px) {
    constexpr int kDashes = 24;
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width_px;
    dsc.opa = LV_OPA_COVER;
    dsc.center.x = (box.x1 + box.x2 + 1) / 2;
    dsc.center.y = (box.y1 + box.y2 + 1) / 2;
    dsc.radius = static_cast<uint16_t>((box.x2 - box.x1 + 1) / 2);
    for (int i = 0; i < kDashes; ++i) {
        dsc.start_angle = static_cast<lv_value_precise_t>(i * 360 / kDashes);
        dsc.end_angle = static_cast<lv_value_precise_t>(i * 360 / kDashes + 360 / kDashes / 2);
        lv_draw_arc(layer, &dsc);
    }
}

void draw_line(lv_layer_t* layer, float x1, float y1, float x2, float y2, lv_color_t color,
               int width_px, int dash_px = 0) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width_px;
    dsc.opa = LV_OPA_COVER;
    dsc.dash_width = dash_px;
    dsc.dash_gap = dash_px;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

void draw_plate(lv_event_t* e) {
    auto* view = static_cast<PlateView*>(lv_event_get_user_data(e));
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!view || !obj || !layer)
        return;

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    view->input.box_w = lv_area_get_width(&area);
    view->input.box_h = lv_area_get_height(&area);
    const auto g = helix::plan_toolhead_plate(view->input);
    if (!g.has_room)
        return;

    const lv_color_t edge = theme_manager_get_color("text_muted");
    const lv_color_t fill = theme_manager_get_color("card_bg");
    const lv_color_t accent = theme_manager_get_color("primary");
    const int line_w = std::max<int>(1, theme_manager_get_spacing("border_width"));
    const int dash = theme_manager_get_spacing("space_xs");
    const bool homed = g.show_crosshair;

    const lv_area_t plate = px_area(area, g.plate.x, g.plate.y, g.plate.w, g.plate.h);
    if (g.circular) {
        if (homed) {
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color = fill;
            dsc.bg_opa = LV_OPA_COVER;
            dsc.border_color = edge;
            dsc.border_width = line_w;
            dsc.border_opa = LV_OPA_COVER;
            dsc.radius = LV_RADIUS_CIRCLE;
            lv_draw_rect(layer, &dsc, &plate);
        } else {
            draw_dashed_circle(layer, plate, edge, line_w);
        }
    } else if (homed) {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = fill;
        dsc.bg_opa = LV_OPA_COVER;
        dsc.border_color = edge;
        dsc.border_width = line_w;
        dsc.border_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &dsc, &plate);
    } else {
        const float l = static_cast<float>(plate.x1);
        const float t = static_cast<float>(plate.y1);
        const float r = static_cast<float>(plate.x2);
        const float b = static_cast<float>(plate.y2);
        draw_line(layer, l, t, r, t, edge, line_w, dash);
        draw_line(layer, r, t, r, b, edge, line_w, dash);
        draw_line(layer, r, b, l, b, edge, line_w, dash);
        draw_line(layer, l, b, l, t, edge, line_w, dash);
    }

    const lv_point_t plate_origin{area.x1 + static_cast<int32_t>(std::lround(g.plate.x)),
                                  area.y1 + static_cast<int32_t>(std::lround(g.plate.y))};
    if (view->area) {
        const auto& a = *view->area;
        helix::BedPlateLayersInput layers_in;
        layers_in.x_min = a.x_min;
        layers_in.x_max = a.x_max;
        layers_in.y_min = a.y_min;
        layers_in.y_max = a.y_max;
        layers_in.circular = g.circular;
        layers_in.outlines = view->outlines;
        helix::BedPlateStyle style;
        style.grid_color = edge;
        style.outline_color = theme_manager_get_color("warning");
        style.outline_width_px = line_w;
        helix::draw_bed_plate_layers(layer, plate_origin,
                                     helix::plan_bed_plate_layers(layers_in, plate_mapper(a, g)),
                                     style);
    }

    // The Z bar: an outline, filled from the bottom as the gantry rises or from the top
    // as the bed lowers.
    const lv_area_t bar = px_area(area, g.z_bar.x, g.z_bar.y, g.z_bar.w, g.z_bar.h);
    lv_draw_rect_dsc_t bar_dsc;
    lv_draw_rect_dsc_init(&bar_dsc);
    bar_dsc.bg_opa = LV_OPA_TRANSP;
    bar_dsc.border_color = edge;
    bar_dsc.border_width = line_w;
    bar_dsc.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &bar_dsc, &bar);
    if (homed && g.z_fraction > 0.0f) {
        const float fill_h = g.z_fraction * g.z_bar.h;
        const float fill_y = g.fill_from_top ? g.z_bar.y : g.z_bar.y + g.z_bar.h - fill_h;
        const lv_area_t fill_area = px_area(area, g.z_bar.x, fill_y, g.z_bar.w, fill_h);
        lv_draw_rect_dsc_t fill_dsc;
        lv_draw_rect_dsc_init(&fill_dsc);
        fill_dsc.bg_color = accent;
        fill_dsc.bg_opa = LV_OPA_COVER;
        lv_draw_rect(layer, &fill_dsc, &fill_area);
    }

    if (homed) {
        const float cx = static_cast<float>(area.x1) + g.cross_x;
        const float cy = static_cast<float>(area.y1) + g.cross_y;
        draw_line(layer, static_cast<float>(plate.x1), cy, static_cast<float>(plate.x2), cy, accent,
                  line_w);
        draw_line(layer, cx, static_cast<float>(plate.y1), cx, static_cast<float>(plate.y2), accent,
                  line_w);
        const int r = std::max<int>(2, theme_manager_get_spacing("space_xxs"));
        lv_area_t dot;
        dot.x1 = static_cast<int32_t>(std::lround(cx)) - r;
        dot.y1 = static_cast<int32_t>(std::lround(cy)) - r;
        dot.x2 = static_cast<int32_t>(std::lround(cx)) + r;
        dot.y2 = static_cast<int32_t>(std::lround(cy)) + r;
        lv_draw_rect_dsc_t dot_dsc;
        lv_draw_rect_dsc_init(&dot_dsc);
        dot_dsc.bg_color = accent;
        dot_dsc.bg_opa = LV_OPA_COVER;
        dot_dsc.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &dot_dsc, &dot);

        // A tapped target: a ring there and a dashed line to it from the head.
        if (view->ghost && view->ghost->x && view->ghost->y && view->area) {
            const auto [gx, gy] = plate_mapper(*view->area, g)
                                      .mm_to_px(static_cast<float>(*view->ghost->x),
                                                static_cast<float>(*view->ghost->y));
            const float tx = static_cast<float>(plate_origin.x) + gx;
            const float ty = static_cast<float>(plate_origin.y) + gy;
            draw_line(layer, cx, cy, tx, ty, accent, line_w, dash);
            lv_draw_arc_dsc_t ring;
            lv_draw_arc_dsc_init(&ring);
            ring.color = accent;
            ring.width = 2 * line_w;
            ring.opa = LV_OPA_COVER;
            ring.center.x = static_cast<int32_t>(std::lround(tx));
            ring.center.y = static_cast<int32_t>(std::lround(ty));
            ring.radius = static_cast<uint16_t>(theme_manager_get_spacing("space_sm"));
            ring.start_angle = 0;
            ring.end_angle = 360;
            lv_draw_arc(layer, &ring);
        }
    }
}

void on_plate_delete(lv_event_t* e) {
    delete static_cast<PlateView*>(lv_event_get_user_data(e));
}

void plate_touch_cb(lv_event_t* e, helix::BedTouch phase);

} // namespace

namespace helix {

struct ToolheadWidget::LiveState {
    bool printing = false;
    bool homed_all = false;
    bool homed[3] = {false, false, false};
    bool moving = false;
    int centimm[3] = {0, 0, 0};
    int velocity_mm_s = 0;
    std::string tool_label;
};

ToolheadWidget::ToolheadWidget(PrinterState& printer_state)
    : printer_state_(printer_state),
      move_sender_(lifetime_, "ToolheadWidget",
                   {[this]() {
                        // An ack moves where the next target starts, and with it the lift
                        // the readout announces.
                        publish_target_text();
                        publish_lift_text();
                    },
                    [this](const MoonrakerError&) { gesture_.mark_failed(); },
                    [this](const JogCoalescer::CoalescedMove&) { return plate_interactive(); }}),
      gesture_({[this](int x, int y) { return target_at(x, y); },
                [this]() { return plate_interactive(); },
                [this]() {
                    return current_plate_lift(printer_state_,
                                              move_sender_.target_start_z(current_z_mm()));
                },
                [this](const AxisTarget& target) { send_plate_target(target); },
                []() { MotionPanel::notify_part_too_tall(); }}) {}

ToolheadWidget::~ToolheadWidget() {
    detach();
}

ToolheadWidget::LiveState ToolheadWidget::read_live() const {
    auto& motion = printer_state_.motion_state();
    LiveState s;
    s.printing = lv_subject_get_int(printer_state_.print_state().get_print_active_subject()) != 0;
    for (const Axis axis : kAllAxes)
        s.homed[axis_index(axis)] = axis_is_homed(printer_state_, axis);
    s.homed_all = s.homed[0] && s.homed[1] && s.homed[2];
    s.velocity_mm_s = lv_subject_get_int(motion.get_live_velocity_subject());
    s.moving = s.velocity_mm_s > 0;
    lv_subject_t* live[3] = {motion.get_live_position_x_subject(),
                             motion.get_live_position_y_subject(),
                             motion.get_live_position_z_subject()};
    lv_subject_t* resting[3] = {motion.get_position_x_subject(), motion.get_position_y_subject(),
                                motion.get_position_z_subject()};
    for (int i = 0; i < 3; ++i) {
        s.centimm[i] = toolhead_position_centimm(s.moving, lv_subject_get_int(live[i]),
                                                 lv_subject_get_int(resting[i]));
    }
    auto& tools = ToolState::instance();
    // Several toolheads name the active one; AMS slots feeding one hotend need no label.
    if (tools.has_multiple_toolheads()) {
        const auto* active = tools.active_tool();
        s.tool_label = active ? active->display_label
                              : ui::lane_label(ui::active_tool_noun(),
                                               lv_subject_get_int(tools.get_active_tool_subject()));
    }
    return s;
}

ToolheadVerdict ToolheadWidget::decide(int width_px, int height_px, bool printing,
                                       bool homed) const {
    // The tree's own padding where it exists; before it does (the load path asks
    // fits_at then) the root's padding on each side.
    const ContentChrome chrome = content_chrome_or_estimate(root(), "space_xxs", "space_xxs");
    ToolheadInput in;
    in.content_w = width_px - chrome.w;
    in.content_h = height_px - chrome.h;
    in.printing = printing;
    in.homed = homed;
    in.needs = measure_needs();
    in.presets_extra_h = measure_presets_extra_h();
    in.labelled_chip_w = measure_labelled_chip_w(root());
    in.wide_gap_w = theme_manager_get_spacing("space_xs");
    const auto& n = in.needs;
    spdlog::trace("[ToolheadWidget] box {}x{} chrome {}x{} needs g{}x{} s{}x{} c{}x{} cell{}x{} "
                  "w{}x{} f{}x{} presets+{} chip{}",
                  width_px, height_px, chrome.w, chrome.h, n.glance.w, n.glance.h, n.strip.w,
                  n.strip.h, n.column.w, n.column.h, n.cell.w, n.cell.h, n.wide.w, n.wide.h,
                  n.full.w, n.full.h, in.presets_extra_h, in.labelled_chip_w);
    return decide_toolhead_layout(in);
}

void ToolheadWidget::resolve_bed() {
    IMoonrakerAPI* api = get_moonraker_api();
    // The plate maps the area the Bed tab maps, so a point means the same on both.
    if (const auto plate = current_bed_plate(printer_state_, api)) {
        const AxisBounds& a = plate->area;
        plate_area_ = a;
        bed_ = BedDimensions{a.x_max - a.x_min, a.y_max - a.y_min, a.x_min, a.y_min};
        circular_ = plate->circular;
    } else {
        plate_area_.reset();
        bed_ = bed_dimensions(api, &printer_state_);
        circular_ = api && circular_bed_kinematics(api->hardware().kinematics());
    }
    if (PlateView* view = plate_ ? static_cast<PlateView*>(lv_obj_get_user_data(plate_)) : nullptr)
        view->area = plate_area_;
}

void ToolheadWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_tile.claim(this);
    install_delete_hook(widget_obj);

    plate_ = lv_obj_find_by_name(widget_obj, "toolhead_plate");
    if (plate_ && !lv_obj_get_user_data(plate_)) {
        auto* view = new PlateView();
        lv_obj_set_user_data(plate_, view);
        // DRAW_MAIN_END overlays the plate on the object's own pass. DRAW_TASK_ADDED
        // fires after the draw tasks are queued and cannot add any.
        lv_obj_add_event_cb(plate_, draw_plate, LV_EVENT_DRAW_MAIN_END, view);
        lv_obj_add_event_cb(plate_, on_plate_delete, LV_EVENT_DELETE, view);
        ui::mark_drag_surface(plate_);
        // mark_drag_surface makes the plate clickable; toolhead_plate_interactive decides
        // that, so its binding is applied again.
        lv_subject_notify(&s_plate_interactive_subject);
    }

    auto& motion = printer_state_.motion_state();
    const auto lifetime = printer_state_.get_subjects_lifetime();
    const auto token = lifetime_.token();
    const auto on_position = [token](ToolheadWidget* self, int) {
        if (token.expired() || !self->uses_positions())
            return;
        self->schedule_refresh();
    };
    lv_subject_t* position_subjects[6] = {
        motion.get_position_x_subject(),      motion.get_position_y_subject(),
        motion.get_position_z_subject(),      motion.get_live_position_x_subject(),
        motion.get_live_position_y_subject(), motion.get_live_position_z_subject()};
    for (int i = 0; i < 6; ++i) {
        position_observers_[i] =
            ui::observe<int>(position_subjects[i], this, on_position, lifetime);
    }
    homed_observer_ = ui::observe<const char*>(
        motion.get_homed_axes_subject(), this,
        [token](ToolheadWidget* self, const char*) {
            if (token.expired())
                return;
            self->on_state_changed();
        },
        lifetime);
    print_active_observer_ = ui::observe<int>(
        printer_state_.print_state().get_print_active_subject(), this,
        [token](ToolheadWidget* self, int) {
            if (token.expired())
                return;
            self->on_state_changed();
        },
        lifetime);
    // Velocity ticks arrive several times a second. They matter only when the nozzle
    // starts or stops moving (the position source flips) or while the speed is on screen.
    velocity_observer_ = ui::observe<int>(
        motion.get_live_velocity_subject(), this,
        [token](ToolheadWidget* self, int velocity) {
            if (token.expired())
                return;
            const bool moving = velocity > 0;
            const bool flipped = moving != self->moving_;
            self->moving_ = moving;
            if ((flipped && self->uses_positions()) || self->verdict_.show_status_row)
                self->schedule_refresh();
        },
        lifetime);
    const auto on_slow_change = [token](ToolheadWidget* self, int) {
        if (token.expired())
            return;
        self->resolve_bed();
        self->schedule_refresh();
    };
    bed_moves_observer_ =
        ui::observe<int>(printer_state_.capabilities_state().subject(Capability::BedMoves), this,
                         on_slow_change, lifetime);
    if (IMoonrakerAPI* api = get_moonraker_api()) {
        // The version subject lives as long as the API, which outlives every tile.
        build_volume_observer_ = ui::observe<int>(api->get_build_volume_version_subject(), this,
                                                  on_slow_change, subject_never_freed());
    }
    auto& tools = ToolState::instance();
    const auto tools_lifetime = tools.get_subjects_lifetime();
    const auto on_tool = [token](ToolheadWidget* self, int) {
        if (token.expired() || !self->verdict_.show_status_row)
            return;
        self->schedule_refresh();
    };
    tools_version_observer_ =
        ui::observe<int>(tools.get_tools_version_subject(), this, on_tool, tools_lifetime);
    active_tool_observer_ =
        ui::observe<int>(tools.get_active_tool_subject(), this, on_tool, tools_lifetime);
    // The rest of the plate's motion gate: plate_moves_allowed() reads these too.
    const auto on_gate = [token](ToolheadWidget* self, int) {
        if (token.expired())
            return;
        self->publish_interaction();
    };
    nav_enabled_observer_ = ui::observe<int>(
        printer_state_.network_state().get_nav_buttons_enabled_subject(), this, on_gate, lifetime);
    motion_blocked_observer_ = ui::observe<int>(
        printer_state_.print_state().get_machine_motion_blocked_subject(), this, on_gate, lifetime);
    if (BedOccupancyState* occupancy = get_bed_occupancy_state()) {
        occupancy_observer_ = ui::observe<int>(
            occupancy->get_version_subject(), this,
            [token](ToolheadWidget* self, int) {
                if (token.expired())
                    return;
                self->refresh_outlines();
                self->publish_lift_text();
            },
            occupancy->get_subjects_lifetime());
    }
    language_observer_ = ui::observe_language_change(this, [token](ToolheadWidget* self) {
        if (token.expired())
            return;
        // The Move-here label's width decides whether the chip keeps it.
        self->relayout_for_granted_size();
        self->publish_target_text();
        self->publish_presets();
        self->publish_lift_text();
    });

    resolve_bed();
    refresh_outlines();
    // Instances are recycled across rebuilds: the verdict is republished here too.
    relayout_for_granted_size();
    publish_state(read_live());
}

void ToolheadWidget::detach() {
    // Ends the gesture and the target while this instance still owns the subjects, so
    // the tile it leaves shows neither.
    gesture_.lost();
    press_taken_ = false;
    clear_ghost();
    lifetime_.invalidate();
    move_sender_.reset();
    // Its pick callback saves into this instance, which a recycled tile no longer shows.
    config_modal_.reset();
    refresh_timer_.cancel();
    ghost_timer_.cancel();
    nav_enabled_observer_.reset();
    motion_blocked_observer_.reset();
    occupancy_observer_.reset();
    language_observer_.reset();
    homed_observer_.reset();
    print_active_observer_.reset();
    for (auto& guard : position_observers_)
        guard.reset();
    velocity_observer_.reset();
    bed_moves_observer_.reset();
    build_volume_observer_.reset();
    tools_version_observer_.reset();
    active_tool_observer_.reset();
    if (ToolheadWidget* displaced = s_tile.release(this)) {
        displaced->relayout_for_granted_size();
        displaced->publish_state(displaced->read_live());
        if (PlateView* view = displaced->plate_
                                  ? static_cast<PlateView*>(lv_obj_get_user_data(displaced->plate_))
                                  : nullptr) {
            view->drawn.reset();
        }
    }
    plate_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ToolheadWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                     int height_px) {
    const LiveState live = read_live();
    verdict_ = decide(width_px, height_px, live.printing, live.homed_all);
    decided_printing_ = live.printing;
    decided_homed_ = live.homed_all;
    if (!s_tile.is_owner(this))
        return;
    publish_state(live);
    // The canvas draws from state captured at refresh time; a new rung or size needs it.
    if (verdict_.show_plate)
        schedule_refresh();

    spdlog::debug("[ToolheadWidget] on_size_changed {}x{} rung={} letters={} z_only={} rows={} "
                  "plate={} home={} status={} presets={} chip_icon_only={} fits={} printing={} "
                  "homed={}",
                  width_px, height_px, static_cast<int>(verdict_.rung), verdict_.show_letters,
                  verdict_.show_z_only, verdict_.show_rows, verdict_.show_plate,
                  verdict_.show_home_button, verdict_.show_status_row, verdict_.show_presets,
                  verdict_.chip_icon_only, verdict_.fits, live.printing, live.homed_all);
}

bool ToolheadWidget::fits_at(int width_px, int height_px) const {
    // Refuse a size that cannot draw the Glance content. The boxes do not depend on
    // the printer's state, so one probe decides.
    return decide(width_px, height_px, false, false).fits;
}

void ToolheadWidget::publish_state(const LiveState& live) {
    if (!s_tile.is_owner(this))
        return;
    set_int_if_changed(&s_rung_subject, static_cast<int>(verdict_.rung));
    set_int_if_changed(&s_show_letters_subject, verdict_.show_letters ? 1 : 0);
    set_int_if_changed(&s_z_only_subject, verdict_.show_z_only ? 1 : 0);
    set_int_if_changed(&s_show_rows_subject, verdict_.show_rows ? 1 : 0);
    set_int_if_changed(&s_show_plate_subject, verdict_.show_plate ? 1 : 0);
    set_int_if_changed(&s_show_home_subject, verdict_.show_home_button ? 1 : 0);
    set_int_if_changed(&s_show_status_subject, verdict_.show_status_row ? 1 : 0);
    set_int_if_changed(&s_show_presets_subject, verdict_.show_presets ? 1 : 0);
    set_int_if_changed(&s_chip_icon_only_subject, verdict_.chip_icon_only ? 1 : 0);
    for (int i = 0; i < 3; ++i)
        set_int_if_changed(&s_homed_subjects[i], live.homed[i] ? 1 : 0);
    publish_values(live);
    publish_interaction();
}

void ToolheadWidget::publish_values(const LiveState& live) {
    if (verdict_.show_rows || verdict_.show_plate) {
        copy_string_if_changed(
            &s_x_text_subject,
            std::string(format_toolhead_axis(Axis::X, live.centimm[0], live.homed[0],
                                             verdict_.compact_values))
                .c_str());
        copy_string_if_changed(
            &s_y_text_subject,
            std::string(format_toolhead_axis(Axis::Y, live.centimm[1], live.homed[1],
                                             verdict_.compact_values))
                .c_str());
    }
    if (verdict_.show_rows || verdict_.show_plate || verdict_.show_z_only) {
        copy_string_if_changed(
            &s_z_text_subject,
            std::string(format_toolhead_axis(Axis::Z, live.centimm[2], live.homed[2],
                                             verdict_.compact_values))
                .c_str());
    }
    if (verdict_.show_status_row) {
        copy_string_if_changed(&s_tool_text_subject, live.tool_label.c_str());
        char speed[24];
        std::snprintf(speed, sizeof(speed), lv_tr("%d mm/s"), live.velocity_mm_s);
        copy_string_if_changed(&s_speed_text_subject, speed);
    }
}

bool ToolheadWidget::uses_positions() const {
    return verdict_.show_rows || verdict_.show_plate || verdict_.show_z_only;
}

bool ToolheadWidget::plate_visible() const {
    return plate_ && verdict_.show_plate && lv_obj_get_user_data(plate_) != nullptr;
}

void ToolheadWidget::on_state_changed() {
    const LiveState live = read_live();
    if (live.printing != decided_printing_ || live.homed_all != decided_homed_) {
        relayout_for_granted_size();
    }
    if (s_tile.is_owner(this)) {
        publish_state(live);
        schedule_refresh();
    }
}

void ToolheadWidget::schedule_refresh() {
    if (!s_tile.is_owner(this) || panel_hidden_)
        return;
    refresh_timer_.schedule_once([this]() { refresh(); });
}

void ToolheadWidget::refresh() {
    if (!s_tile.is_owner(this) || panel_hidden_)
        return;
    const LiveState live = read_live();
    publish_values(live);
    if (!plate_visible())
        return;
    // The bounds the plate maps arrive with the printer's status, not on an event of
    // their own.
    resolve_bed();
    if (gesture_.dragging() || ghost_)
        publish_target_text();
    publish_lift_text();
    auto* view = static_cast<PlateView*>(lv_obj_get_user_data(plate_));
    const int w = lv_obj_get_width(plate_);
    const int h = lv_obj_get_height(plate_);
    view->input = plate_input(live, w, h);
    const auto key = draw_key(plan_toolhead_plate(view->input));
    if (view->drawn != key) {
        view->drawn = key;
        lv_obj_invalidate(plate_);
    }
}

ToolheadPlateInput ToolheadWidget::plate_input(const LiveState& live, int box_w, int box_h) const {
    ToolheadPlateInput in;
    in.box_w = box_w;
    in.box_h = box_h;
    in.inset_px = theme_manager_get_spacing("space_xxs");
    in.bar_w_px = theme_manager_get_spacing("space_xs");
    in.gap_px = theme_manager_get_spacing("space_xs");
    in.bed = bed_;
    in.circular = circular_;
    in.bed_moves =
        lv_subject_get_int(printer_state_.capabilities_state().subject(Capability::BedMoves)) != 0;
    in.homed = live.homed_all;
    in.align_start = verdict_.rung == SizeRung::Wide;
    in.x_mm = static_cast<float>(units::from_centimm(live.centimm[0]));
    in.y_mm = static_cast<float>(units::from_centimm(live.centimm[1]));
    in.z_mm = static_cast<float>(units::from_centimm(live.centimm[2]));
    const AxisBounds bounds = printer_state_.motion_state().get_axis_bounds();
    in.has_z_range = bounds.has_z;
    in.z_min_mm = bounds.z_min;
    in.z_max_mm = bounds.z_max;
    return in;
}

std::optional<ToolheadPlateInput> ToolheadWidget::drawn_plate_input() const {
    if (!plate_)
        return std::nullopt;
    const auto* view = static_cast<const PlateView*>(lv_obj_get_user_data(plate_));
    if (!view)
        return std::nullopt;
    return view->input;
}

void ToolheadWidget::on_activate() {
    panel_hidden_ = false;
    schedule_refresh();
}

void ToolheadWidget::on_deactivate() {
    panel_hidden_ = true;
    if (plate_) {
        if (auto* view = static_cast<PlateView*>(lv_obj_get_user_data(plate_)))
            view->drawn.reset();
    }
    refresh_timer_.cancel();
}

std::optional<ToolheadPlateGeometry> ToolheadWidget::plate_geometry() const {
    if (!plate_visible())
        return std::nullopt;
    return plan_toolhead_plate(
        plate_input(read_live(), lv_obj_get_width(plate_), lv_obj_get_height(plate_)));
}

void ToolheadWidget::home_all() {
    if (!s_tile.is_owner(this) || !verdict_.show_home_button)
        return;
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        NOTIFY_WARNING(lv_tr("Printer connection unavailable"));
        return;
    }
    NOTIFY_INFO(lv_tr("Homing all axes..."));
    api->motion().home_axes(
        "",
        lifetime_.bg_cb("ToolheadWidget::home_ok",
                        []() { NOTIFY_SUCCESS(lv_tr("Homing complete")); }),
        lifetime_.bg_cb("ToolheadWidget::home_error", [](const MoonrakerError& err) {
            NOTIFY_ERROR(lv_tr("Homing failed: {}"), err.localized_message());
        }));
}

bool ToolheadWidget::plate_interactive() const {
    return s_tile.is_owner(this) && plate_visible() && plate_area_ &&
           get_moonraker_api() != nullptr &&
           lv_subject_get_int(printer_state_.print_state().get_print_active_subject()) == 0 &&
           plate_moves_allowed(printer_state_);
}

void ToolheadWidget::publish_interaction() {
    if (!s_tile.is_owner(this))
        return;
    const bool interactive = plate_interactive();
    if (!interactive) {
        if (gesture_.active())
            gesture_.lost();
        clear_ghost();
        // The move in flight finishes; nothing queued behind it may follow.
        move_sender_.reset();
    }
    set_int_if_changed(&s_plate_interactive_subject, interactive ? 1 : 0);
    publish_target_text();
    publish_presets();
    publish_lift_text();
}

void ToolheadWidget::publish_presets() {
    if (!s_tile.is_owner(this))
        return;
    const bool moves_ok = plate_interactive();
    const bool api_ok = get_moonraker_api() != nullptr;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const ToolheadPresetSlot& slot = slots_[i];
        int state = kSlotEmpty;
        if (slot.kind == ToolheadPresetSlot::Kind::Macro)
            state = api_ok ? kSlotLive : kSlotInert;
        else if (toolhead_slot_moves_toolhead(slot))
            state = moves_ok ? kSlotLive : kSlotInert;
        if (state != kSlotEmpty) {
            copy_string_if_changed(&s_preset_icon_subjects[i], toolhead_preset_slot_icon(slot));
            copy_string_if_changed(&s_preset_label_subjects[i],
                                   toolhead_preset_slot_label(slot).c_str());
        }
        set_int_if_changed(&s_preset_state_subjects[i], state);
    }
}

void ToolheadWidget::publish_lift_text() {
    if (!s_tile.is_owner(this))
        return;
    std::string text;
    if (verdict_.show_presets && plate_interactive()) {
        const double z = current_z_mm();
        text = plate_lift_readout(
            z, current_plate_lift(printer_state_, move_sender_.target_start_z(z)));
    }
    copy_string_if_changed(&s_lift_text_subject, text.c_str());
}

void ToolheadWidget::set_config(const nlohmann::json& config) {
    config_ = config.is_object() ? config : nlohmann::json::object();
    slots_ = toolhead_preset_slots_from_config(config_);
    publish_presets();
}

bool ToolheadWidget::on_edit_configure() {
    spdlog::info("[ToolheadWidget] Configure requested - showing the preset picker");
    config_modal_ = std::make_unique<ToolheadPresetConfigModal>(
        slots_, [this](const ToolheadPresetSlots& slots) { save_slots(slots); });
    config_modal_->show(lv_screen_active());
    return false;
}

void ToolheadWidget::save_slots(const ToolheadPresetSlots& slots) {
    slots_ = slots;
    toolhead_preset_slots_to_config(slots_, config_);
    save_widget_config(config_);
    publish_presets();
}

void ToolheadWidget::run_preset(std::size_t index) {
    if (!s_tile.is_owner(this) || !verdict_.show_presets || index >= slots_.size())
        return;
    const ToolheadPresetSlot slot = slots_[index];
    if (slot.kind == ToolheadPresetSlot::Kind::Empty)
        return;
    IMoonrakerAPI* api = get_moonraker_api();
    if (slot.kind == ToolheadPresetSlot::Kind::Macro) {
        record_interaction();
        run_home_macro(api, {slot.macro, true, parent_screen_, "ToolheadWidget"});
        return;
    }
    if (!plate_interactive())
        return;
    record_interaction();
    switch (slot.kind) {
    case ToolheadPresetSlot::Kind::Position: {
        // Computed at tap time: the bounds can change between the tile drawing and the tap.
        const auto plate = current_bed_plate(printer_state_, api);
        const auto target = plate
                                ? motion_preset_target(slot.position, plate->area, plate->circular)
                                : std::nullopt;
        if (!target) {
            NOTIFY_INFO(lv_tr("Axis limits unknown"));
            return;
        }
        gesture_.send_once(*target);
        return;
    }
    case ToolheadPresetSlot::Kind::Park:
        park();
        return;
    case ToolheadPresetSlot::Kind::ZUp:
        step_z(kToolheadZStepMm);
        return;
    case ToolheadPresetSlot::Kind::ZDown:
        step_z(-kToolheadZStepMm);
        return;
    case ToolheadPresetSlot::Kind::Empty:
    case ToolheadPresetSlot::Kind::Macro:
        return;
    }
}

void ToolheadWidget::park() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (run_park_toolhead_macro(api, lifetime_, "ToolheadWidget::park_failed"))
        return;
    // Park's own lift rule, not the plate's: the target already carries its Z.
    const auto park =
        park_over_plate_target(printer_state_, api, move_sender_.target_start_z(current_z_mm()));
    switch (park.kind) {
    case ParkOverPlate::Kind::Unknown:
        NOTIFY_INFO(lv_tr("Axis limits unknown"));
        return;
    case ParkOverPlate::Kind::Refuse:
        MotionPanel::notify_part_too_tall();
        return;
    case ParkOverPlate::Kind::Move:
        send_plate_target(park.target);
        return;
    }
}

void ToolheadWidget::step_z(double delta_mm) {
    const double current = current_z_mm();
    double z = move_sender_.target_start_z(current) + delta_mm;
    if (delta_mm < 0.0)
        z = clamp_z_down_to_current_floor(z);
    const AxisBounds bounds = move_clamp_bounds(printer_state_);
    if (bounds.has_z)
        z = std::clamp(z, static_cast<double>(bounds.z_min), static_cast<double>(bounds.z_max));
    AxisTarget target;
    target.z = z;
    spdlog::info("[ToolheadWidget] Z step {:+.1f} to Z{:.2f}", delta_mm, z);
    move_sender_.send_target(get_moonraker_api(), target, bounds, current);
}

void ToolheadWidget::publish_target_text() {
    if (!s_tile.is_owner(this))
        return;
    std::optional<AxisTarget> target;
    if (gesture_.dragging() && gesture_.drag_target())
        target = gesture_.drag_target();
    else if (ghost_)
        target = ghost_;
    set_int_if_changed(&s_ghost_active_subject, ghost_ ? 1 : 0);
    set_int_if_changed(&s_target_shown_subject, target ? 1 : 0);

    std::string text;
    if (target && target->x && target->y) {
        text = fmt::format("-> X{:.0f} Y{:.0f}", *target->x, *target->y);
        const double z = current_z_mm();
        const std::string lift_text = plate_lift_readout(
            z, current_plate_lift(printer_state_, move_sender_.target_start_z(z)));
        if (!lift_text.empty()) {
            text += "\n";
            text += lift_text;
        }
    }
    copy_string_if_changed(&s_ghost_text_subject, text.c_str());
}

double ToolheadWidget::current_z_mm() const {
    return units::from_centimm(
        lv_subject_get_int(printer_state_.motion_state().get_position_z_subject()));
}

bool ToolheadWidget::on_plate(int screen_x, int screen_y) const {
    const auto g = plate_geometry();
    if (!g || !g->has_room)
        return false;
    lv_area_t coords;
    lv_obj_get_coords(plate_, &coords);
    const float x = static_cast<float>(screen_x - coords.x1) - g->plate.x;
    const float y = static_cast<float>(screen_y - coords.y1) - g->plate.y;
    return x >= 0.0f && x <= g->plate.w && y >= 0.0f && y <= g->plate.h;
}

std::optional<AxisTarget> ToolheadWidget::target_at(int screen_x, int screen_y) const {
    const auto g = plate_geometry();
    if (!plate_area_ || !g || !g->has_room)
        return std::nullopt;
    lv_area_t coords;
    lv_obj_get_coords(plate_, &coords);
    return bed_map_target(static_cast<float>(screen_x - coords.x1) - g->plate.x,
                          static_cast<float>(screen_y - coords.y1) - g->plate.y,
                          plate_mapper(*plate_area_, *g), *plate_area_, circular_);
}

void ToolheadWidget::handle_plate_touch(BedTouch phase, lv_point_t screen_point) {
    if (!s_tile.is_owner(this))
        return;
    const int x = static_cast<int>(screen_point.x);
    const int y = static_cast<int>(screen_point.y);
    switch (phase) {
    case BedTouch::Pressed:
        press_taken_ = false;
        // The Z bar and the margin beside the plate are the tile's: a tap there opens
        // the Motion panel.
        if (!plate_interactive() || !on_plate(x, y))
            return;
        press_taken_ = gesture_.press(x, y);
        if (press_taken_)
            record_interaction();
        break;
    case BedTouch::Pressing: {
        if (!gesture_.active())
            return;
        const bool was_dragging = gesture_.dragging();
        const bool target_changed = gesture_.pressing(x, y);
        if (gesture_.dragging() && !was_dragging)
            clear_ghost();
        if (target_changed)
            publish_target_text();
        break;
    }
    case BedTouch::Released: {
        if (!gesture_.active())
            return;
        const auto release = gesture_.release(x, y);
        if (!release.was_drag && release.tap_target)
            set_ghost(*release.tap_target);
        publish_target_text();
        break;
    }
    case BedTouch::Lost:
        if (!gesture_.active())
            return;
        gesture_.lost();
        publish_target_text();
        break;
    }
}

void ToolheadWidget::plate_clicked(lv_event_t* e) {
    // A tap or drag the gesture took is the plate's; anything else is a tap on the tile.
    if (press_taken_)
        lv_event_stop_bubbling(e);
    press_taken_ = false;
}

void ToolheadWidget::set_ghost(const AxisTarget& target) {
    ghost_ = target;
    ghost_timer_.schedule([this]() { clear_ghost(); });
    publish_target_text();
    invalidate_plate();
}

void ToolheadWidget::clear_ghost() {
    ghost_timer_.cancel();
    if (!ghost_)
        return;
    ghost_.reset();
    publish_target_text();
    invalidate_plate();
}

void ToolheadWidget::move_here() {
    if (!s_tile.is_owner(this) || !ghost_)
        return;
    const AxisTarget target = *ghost_;
    clear_ghost();
    gesture_.send_once(target);
}

void ToolheadWidget::send_plate_target(const AxisTarget& target) {
    spdlog::info("[ToolheadWidget] Plate target X{:.2f} Y{:.2f}{}", target.x.value_or(0.0),
                 target.y.value_or(0.0),
                 target.z ? fmt::format(" (lift Z{:.2f})", *target.z) : std::string());
    if (!move_sender_.send_target(get_moonraker_api(), target, move_clamp_bounds(printer_state_),
                                  current_z_mm())) {
        gesture_.mark_failed();
    }
}

void ToolheadWidget::refresh_outlines() {
    if (!plate_)
        return;
    auto* view = static_cast<PlateView*>(lv_obj_get_user_data(plate_));
    if (!view)
        return;
    const BedOccupancyState* occupancy = get_bed_occupancy_state();
    view->outlines = occupancy ? occupancy->outlines() : std::vector<BedObjectOutline>{};
    invalidate_plate();
}

void ToolheadWidget::invalidate_plate() {
    if (!plate_)
        return;
    auto* view = static_cast<PlateView*>(lv_obj_get_user_data(plate_));
    if (!view)
        return;
    view->ghost = ghost_;
    if (plate_visible() && !panel_hidden_)
        lv_obj_invalidate(plate_);
}

void ToolheadWidget::open_motion_panel() {
    get_global_motion_panel().show(parent_screen_);
}

void ToolheadWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] clicked_cb");
    if (auto* self = panel_widget_from_event<ToolheadWidget>(e)) {
        self->record_interaction();
        self->open_motion_panel();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix

namespace {

/// Forward a pointer event on the plate, with the point it fired at, to the live tile.
void plate_touch_cb(lv_event_t* e, helix::BedTouch phase) {
    // A press on one of the plate's chips is the chip's.
    if (lv_event_get_target(e) != lv_event_get_current_target(e))
        return;
    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (auto* owner = s_tile.owner())
        owner->handle_plate_touch(phase, point);
}

/// A preset slot's CLICKED. A slot tap is not a tap on the tile, which opens Motion.
template <std::size_t Index> void preset_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] preset_cb");
    lv_event_stop_bubbling(e);
    if (auto* owner = s_tile.owner())
        owner->run_preset(Index);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

namespace helix {

void register_toolhead_widget() {
    register_widget_factory("toolhead", [](const std::string&) {
        return std::make_unique<ToolheadWidget>(get_printer_state());
    });
    register_widget_subjects("toolhead", toolhead_init_subjects);

    lv_xml_register_event_cb(nullptr, "toolhead_clicked_cb", ToolheadWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "toolhead_plate_pressed_cb",
                             [](lv_event_t* e) { plate_touch_cb(e, BedTouch::Pressed); });
    lv_xml_register_event_cb(nullptr, "toolhead_plate_pressing_cb",
                             [](lv_event_t* e) { plate_touch_cb(e, BedTouch::Pressing); });
    lv_xml_register_event_cb(nullptr, "toolhead_plate_released_cb",
                             [](lv_event_t* e) { plate_touch_cb(e, BedTouch::Released); });
    lv_xml_register_event_cb(nullptr, "toolhead_plate_press_lost_cb",
                             [](lv_event_t* e) { plate_touch_cb(e, BedTouch::Lost); });
    lv_xml_register_event_cb(nullptr, "toolhead_plate_clicked_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] plate_clicked_cb");
        if (auto* owner = s_tile.owner())
            owner->plate_clicked(e);
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "toolhead_move_here_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] move_here_cb");
        lv_event_stop_bubbling(e);
        if (auto* owner = s_tile.owner())
            owner->move_here();
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "toolhead_ghost_dismiss_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] ghost_dismiss_cb");
        lv_event_stop_bubbling(e);
        if (auto* owner = s_tile.owner())
            owner->clear_ghost();
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "toolhead_bed_cleared_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] bed_cleared_cb");
        lv_event_stop_bubbling(e);
        confirm_bed_cleared();
        LVGL_SAFE_EVENT_CB_END();
    });
    lv_xml_register_event_cb(nullptr, "toolhead_preset_0_cb", preset_cb<0>);
    lv_xml_register_event_cb(nullptr, "toolhead_preset_1_cb", preset_cb<1>);
    lv_xml_register_event_cb(nullptr, "toolhead_preset_2_cb", preset_cb<2>);
    register_toolhead_preset_config();
    lv_xml_register_event_cb(nullptr, "toolhead_home_all_cb", [](lv_event_t* e) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ToolheadWidget] home_all_cb");
        // A button tap is not a tap on the tile, which opens the Motion panel.
        lv_event_stop_bubbling(e);
        if (auto* owner = s_tile.owner())
            owner->home_all();
        LVGL_SAFE_EVENT_CB_END();
    });
}

} // namespace helix
