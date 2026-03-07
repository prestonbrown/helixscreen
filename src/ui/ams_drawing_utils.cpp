// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/ams_drawing_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace ams_draw {

// ============================================================================
// Color Utilities
// ============================================================================

lv_color_t lighten_color(lv_color_t c, uint8_t amount) {
    return lv_color_make(std::min(255, c.red + amount), std::min(255, c.green + amount),
                         std::min(255, c.blue + amount));
}

lv_color_t darken_color(lv_color_t c, uint8_t amount) {
    return lv_color_make(c.red > amount ? c.red - amount : 0,
                         c.green > amount ? c.green - amount : 0,
                         c.blue > amount ? c.blue - amount : 0);
}

lv_color_t blend_color(lv_color_t c1, lv_color_t c2, float factor) {
    factor = std::clamp(factor, 0.0f, 1.0f);
    return lv_color_make(static_cast<uint8_t>(c1.red + (c2.red - c1.red) * factor),
                         static_cast<uint8_t>(c1.green + (c2.green - c1.green) * factor),
                         static_cast<uint8_t>(c1.blue + (c2.blue - c1.blue) * factor));
}

// ============================================================================
// Severity & Error Helpers
// ============================================================================

lv_color_t severity_color(SlotError::Severity severity) {
    switch (severity) {
    case SlotError::ERROR:
        return theme_manager_get_color("danger");
    case SlotError::WARNING:
        return theme_manager_get_color("warning");
    default:
        return theme_manager_get_color("text_muted");
    }
}

SlotError::Severity worst_unit_severity(const AmsUnit& unit) {
    SlotError::Severity worst = SlotError::INFO;
    for (const auto& slot : unit.slots) {
        if (slot.error.has_value() && slot.error->severity > worst) {
            worst = slot.error->severity;
        }
    }
    return worst;
}

// ============================================================================
// Data Helpers
// ============================================================================

int fill_percent_from_slot(const SlotInfo& slot, int min_pct) {
    float pct = slot.get_remaining_percent();
    if (pct < 0) {
        return 100;
    }
    return std::clamp(static_cast<int>(pct), min_pct, 100);
}

int32_t calc_bar_width(int32_t container_width, int slot_count, int32_t gap, int32_t min_width,
                       int32_t max_width, int container_pct) {
    int32_t usable = (container_width * container_pct) / 100;
    int count = std::max(1, slot_count);
    int32_t total_gaps = (count > 1) ? (count - 1) * gap : 0;
    int32_t width = (usable - total_gaps) / count;
    return std::clamp(width, min_width, max_width);
}

// ============================================================================
// Presentation Helpers
// ============================================================================

std::string get_unit_display_name(const AmsUnit& unit, int unit_index) {
    // Prefer display_name (short, pretty) over internal name
    std::string raw;
    if (!unit.display_name.empty()) {
        raw = unit.display_name;
    } else if (!unit.name.empty()) {
        raw = unit.name;
    } else {
        return "Unit " + std::to_string(unit_index + 1);
    }

    // Replace underscores with spaces for readability
    std::replace(raw.begin(), raw.end(), '_', ' ');
    return raw;
}

// ============================================================================
// LVGL Widget Factories
// ============================================================================

lv_obj_t* create_transparent_container(lv_obj_t* parent) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
    return c;
}

// ============================================================================
// Pulse Animation
// ============================================================================

static void pulse_scale_anim_cb(void* var, int32_t value) {
    auto* obj = static_cast<lv_obj_t*>(var);
    lv_obj_set_style_transform_scale(obj, value, LV_PART_MAIN);
    int32_t range = PULSE_SCALE_MAX - PULSE_SCALE_MIN;
    int32_t progress = value - PULSE_SCALE_MIN;
    int32_t shadow = progress * 8 / range;
    lv_obj_set_style_shadow_width(obj, shadow, LV_PART_MAIN);
    lv_opa_t shadow_opa = static_cast<lv_opa_t>(progress * 180 / range);
    lv_obj_set_style_shadow_opa(obj, shadow_opa, LV_PART_MAIN);
}

static void pulse_color_anim_cb(void* var, int32_t value) {
    auto* obj = static_cast<lv_obj_t*>(var);
    lv_color_t base = lv_obj_get_style_border_color(obj, LV_PART_MAIN);
    uint8_t gray = static_cast<uint8_t>((base.red * 77 + base.green * 150 + base.blue * 29) >> 8);
    lv_color_t gray_color = lv_color_make(gray, gray, gray);
    lv_color_t result = lv_color_mix(base, gray_color, static_cast<lv_opa_t>(value));
    lv_obj_set_style_bg_color(obj, result, LV_PART_MAIN);
}

void start_pulse(lv_obj_t* dot, lv_color_t base_color) {
    lv_obj_set_style_border_color(dot, base_color, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(dot, base_color, LV_PART_MAIN);

    int32_t w = lv_obj_get_width(dot);
    int32_t h = lv_obj_get_height(dot);
    lv_obj_set_style_transform_pivot_x(dot, w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(dot, h / 2, LV_PART_MAIN);

    lv_anim_t sa;
    lv_anim_init(&sa);
    lv_anim_set_var(&sa, dot);
    lv_anim_set_values(&sa, PULSE_SCALE_MAX, PULSE_SCALE_MIN);
    lv_anim_set_time(&sa, PULSE_DURATION_MS);
    lv_anim_set_playback_time(&sa, PULSE_DURATION_MS);
    lv_anim_set_repeat_count(&sa, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&sa, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&sa, pulse_scale_anim_cb);
    lv_anim_start(&sa);

    lv_anim_t ca;
    lv_anim_init(&ca);
    lv_anim_set_var(&ca, dot);
    lv_anim_set_values(&ca, PULSE_SAT_MAX, PULSE_SAT_MIN);
    lv_anim_set_time(&ca, PULSE_DURATION_MS);
    lv_anim_set_playback_time(&ca, PULSE_DURATION_MS);
    lv_anim_set_repeat_count(&ca, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&ca, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&ca, pulse_color_anim_cb);
    lv_anim_start(&ca);
}

void stop_pulse(lv_obj_t* dot) {
    lv_anim_delete(dot, pulse_scale_anim_cb);
    lv_anim_delete(dot, pulse_color_anim_cb);
    lv_obj_set_style_transform_scale(dot, 256, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(dot, LV_OPA_TRANSP, LV_PART_MAIN);
}

// ============================================================================
// Error Badge
// ============================================================================

lv_obj_t* create_error_badge(lv_obj_t* parent, int32_t size) {
    lv_obj_t* badge = lv_obj_create(parent);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 0, LV_PART_MAIN);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
    return badge;
}

void update_error_badge(lv_obj_t* badge, bool has_error, SlotError::Severity severity,
                        bool animate) {
    if (!badge) {
        return;
    }

    if (has_error) {
        lv_color_t color = severity_color(severity);
        lv_obj_set_style_bg_color(badge, color, LV_PART_MAIN);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_HIDDEN);
        if (animate) {
            start_pulse(badge, color);
        } else {
            stop_pulse(badge);
        }
    } else {
        stop_pulse(badge);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// Slot Bar Column
// ============================================================================

SlotColumn create_slot_column(lv_obj_t* parent, int32_t bar_width, int32_t bar_height,
                              int32_t bar_radius) {
    SlotColumn col;

    // Column container (bar + status line)
    col.container = create_transparent_container(parent);
    lv_obj_set_size(col.container, bar_width,
                    bar_height + STATUS_LINE_HEIGHT_PX + STATUS_LINE_GAP_PX);
    lv_obj_set_flex_flow(col.container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col.container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col.container, STATUS_LINE_GAP_PX, LV_PART_MAIN);

    // Bar background (outline container)
    col.bar_bg = create_transparent_container(col.container);
    lv_obj_set_size(col.bar_bg, bar_width, bar_height);
    lv_obj_set_style_radius(col.bar_bg, bar_radius, LV_PART_MAIN);

    // Fill inside bar_bg (anchored to bottom, grows upward)
    col.bar_fill = create_transparent_container(col.bar_bg);
    lv_obj_set_width(col.bar_fill, LV_PCT(100));
    lv_obj_set_style_radius(col.bar_fill, bar_radius, LV_PART_MAIN);

    // Status line below bar
    col.status_line = create_transparent_container(col.container);
    lv_obj_set_size(col.status_line, bar_width, STATUS_LINE_HEIGHT_PX);
    lv_obj_set_style_radius(col.status_line, bar_radius / 2, LV_PART_MAIN);

    return col;
}

void style_slot_bar(const SlotColumn& col, const BarStyleParams& params, int32_t bar_radius) {
    (void)bar_radius; // Reserved for future dynamic radius changes
    if (!col.bar_bg || !col.bar_fill) {
        return;
    }

    // --- Bar background border ---
    if (params.is_loaded && !params.has_error) {
        // Loaded: wider, brighter border
        lv_obj_set_style_border_width(col.bar_bg, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(col.bar_bg, theme_manager_get_color("text"), LV_PART_MAIN);
        lv_obj_set_style_border_opa(col.bar_bg, LV_OPA_80, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_width(col.bar_bg, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(col.bar_bg, theme_manager_get_color("text_muted"),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_opa(col.bar_bg, params.is_present ? LV_OPA_50 : LV_OPA_20,
                                    LV_PART_MAIN);
    }

    // --- Fill gradient ---
    if (params.is_present && params.fill_pct > 0) {
        lv_color_t base_color = lv_color_hex(params.color_rgb);
        lv_color_t light_color = lighten_color(base_color, 50);

        lv_obj_set_style_bg_color(col.bar_fill, light_color, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(col.bar_fill, base_color, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(col.bar_fill, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(col.bar_fill, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_set_height(col.bar_fill, LV_PCT(params.fill_pct));
        lv_obj_align(col.bar_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_remove_flag(col.bar_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(col.bar_fill, LV_OBJ_FLAG_HIDDEN);
    }

    // --- Status line ---
    if (col.status_line) {
        if (params.has_error) {
            lv_color_t error_color = severity_color(params.severity);
            lv_obj_set_style_bg_color(col.status_line, error_color, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(col.status_line, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_remove_flag(col.status_line, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(col.status_line, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// Logo Helpers
// ============================================================================

void apply_logo(lv_obj_t* image, const AmsUnit& unit, const AmsSystemInfo& info) {
    if (!image) {
        return;
    }

    const char* path = AmsState::get_logo_path(unit.name);
    if (!path || !path[0]) {
        path = AmsState::get_logo_path(info.type_name);
    }

    if (path && path[0]) {
        lv_image_set_src(image, path);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
}

void apply_logo(lv_obj_t* image, const std::string& type_name) {
    if (!image) {
        return;
    }

    const char* path = AmsState::get_logo_path(type_name);
    if (path && path[0]) {
        lv_image_set_src(image, path);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// System Tool Layout
// ============================================================================

SystemToolLayout compute_system_tool_layout(const AmsSystemInfo& info, const AmsBackend* backend) {
    SystemToolLayout result;
    int total_physical = 0;

    for (int i = 0; i < static_cast<int>(info.units.size()); ++i) {
        const auto& unit = info.units[i];

        // Determine topology — prefer backend query, fall back to unit struct
        PathTopology topo = unit.topology;
        if (backend) {
            topo = backend->get_unit_topology(i);
        }

        // Find min/max mapped_tool across slots in this unit
        int min_tool = -1;
        int max_tool = -1;
        for (const auto& slot : unit.slots) {
            if (slot.mapped_tool >= 0) {
                if (min_tool < 0 || slot.mapped_tool < min_tool) {
                    min_tool = slot.mapped_tool;
                }
                if (max_tool < 0 || slot.mapped_tool > max_tool) {
                    max_tool = slot.mapped_tool;
                }
            }
        }

        UnitToolLayout utl;
        utl.min_virtual_tool = min_tool;
        utl.hub_tool_label = unit.hub_tool_label;

        if (topo != PathTopology::PARALLEL) {
            // HUB/LINEAR: all lanes converge to a single physical nozzle.
            // Multiple HUB units sharing the same hub_tool_label (e.g., all feeding
            // into T0 on a single-toolhead printer) share one physical nozzle.
            int shared_phys = -1;
            if (unit.hub_tool_label >= 0) {
                // Check if a previous unit already claimed this physical nozzle
                for (const auto& prev : result.units) {
                    if (prev.hub_tool_label == unit.hub_tool_label && prev.tool_count == 1) {
                        shared_phys = prev.first_physical_tool;
                        break;
                    }
                }
            }

            if (shared_phys >= 0) {
                // Reuse existing physical nozzle
                utl.first_physical_tool = shared_phys;
            } else {
                utl.first_physical_tool = total_physical;
                total_physical += 1;
            }
            utl.tool_count = 1;

            // Map all virtual tool numbers from this unit to this physical nozzle
            int phys = utl.first_physical_tool;
            for (const auto& slot : unit.slots) {
                if (slot.mapped_tool >= 0) {
                    result.virtual_to_physical[slot.mapped_tool] = phys;
                }
            }
        } else if (min_tool >= 0) {
            // PARALLEL: each lane maps to its own physical nozzle.
            // Count actual mapped slots rather than using max-min+1 range,
            // because tool remapping (SET_MAP) can create non-contiguous tool
            // numbers that span across units (e.g., a 6-lane unit with tools
            // T0,T1,T6,T7,T8,T11 would compute 12 instead of 6).
            int physical_first = total_physical;

            // Collect and sort the mapped tool numbers for stable ranking
            std::vector<int> mapped_tools;
            for (const auto& slot : unit.slots) {
                if (slot.mapped_tool >= 0) {
                    mapped_tools.push_back(slot.mapped_tool);
                }
            }
            std::sort(mapped_tools.begin(), mapped_tools.end());

            int lane_count = static_cast<int>(mapped_tools.size());
            utl.first_physical_tool = physical_first;
            utl.tool_count = lane_count;

            // Map each virtual tool to sequential physical position by sorted rank
            for (const auto& slot : unit.slots) {
                if (slot.mapped_tool >= 0) {
                    auto it = std::lower_bound(mapped_tools.begin(), mapped_tools.end(),
                                               slot.mapped_tool);
                    int rank = static_cast<int>(it - mapped_tools.begin());
                    result.virtual_to_physical[slot.mapped_tool] = physical_first + rank;
                }
            }

            total_physical += lane_count;
        } else if (!unit.slots.empty()) {
            // PARALLEL fallback: no mapped_tool data, use slot count
            utl.first_physical_tool = total_physical;
            utl.tool_count = static_cast<int>(unit.slots.size());
            total_physical += utl.tool_count;
        }

        result.units.push_back(utl);
    }

    result.total_physical_tools = total_physical;

    // Build physical→virtual label map
    result.physical_to_virtual_label.resize(total_physical, -1);
    for (const auto& utl : result.units) {
        // For each physical nozzle this unit owns, set the label.
        // HUB units with hub_tool_label: use that (derived from extruder name).
        // Otherwise: use min_virtual_tool (+ offset for PARALLEL).
        for (int t = 0; t < utl.tool_count; ++t) {
            int phys = utl.first_physical_tool + t;
            if (phys < total_physical) {
                if (utl.hub_tool_label >= 0 && utl.tool_count == 1) {
                    // HUB unit with explicit label from extruder name
                    result.physical_to_virtual_label[phys] = utl.hub_tool_label;
                } else if (utl.min_virtual_tool >= 0) {
                    result.physical_to_virtual_label[phys] = utl.min_virtual_tool + t;
                } else {
                    result.physical_to_virtual_label[phys] = phys;
                }
            }
        }
    }

    return result;
}

} // namespace ams_draw
