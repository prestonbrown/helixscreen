// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/ams_drawing_utils.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_spool_canvas.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "config.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <set>

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
    // Canonical fill semantics (SlotInfo::display_fill_pct): real ratio when
    // both weights are known, 100% when only metadata is present, 0 for an
    // absent/ghost lane, and -1 when there is no data at all. -1 is propagated
    // so callers can skip/keep-previous instead of rendering a phantom bar; it
    // is what keeps a lane we know nothing about from being painted, which is a
    // different case from a known lane whose weight nobody tracks.
    int pct = slot.display_fill_pct();
    if (pct < 0) {
        return -1;
    }
    return std::clamp(pct, min_pct, 100);
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

    // For extruder-name-grouped PARALLEL units, the physical position order
    // follows std::map iteration (alphabetical by extruder name), which may
    // not match sorted virtual tool numbers. Record the correct label per
    // physical position here and apply after the default label-building loop.
    std::map<int, int> physical_label_overrides;

    // Physical nozzle -> Klipper extruder name, recorded wherever the branch that
    // assigns the position actually knows which extruder it belongs to. Positions
    // never recorded here stay unknown (empty) — see physical_to_extruder_name.
    std::map<int, std::string> physical_extruder_names;

    // Record the extruder that owns a physical nozzle, ignoring empty names and
    // refusing to overwrite a differing earlier answer (shared HUB nozzles).
    auto record_extruder = [&physical_extruder_names](int phys, const std::string& name) {
        if (name.empty()) {
            return;
        }
        auto [it, inserted] = physical_extruder_names.emplace(phys, name);
        if (!inserted && it->second != name) {
            it->second.clear(); // conflicting claims -> unknown, never a guess
        }
    };

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

        if (topo == PathTopology::MIXED) {
            // MIXED: direct lanes each get their own nozzle position,
            // hub lanes share one nozzle position regardless of mapped_tool.
            // Count = number of direct lanes + 1 per hub group.
            int direct_count = 0;
            bool has_hub_group = false;
            int hub_tool = -1; // lowest mapped_tool among hub lanes (for label)
            // mapped_tool + owning extruder name for each direct lane. The name
            // rides along so the physical position it lands on can be labelled by
            // extruder identity, not by the lane's AFC map alias.
            std::vector<std::pair<int, std::string>> direct_tools;
            std::vector<std::string> hub_extruders;

            for (int s = 0; s < unit.slot_count; s++) {
                bool is_hub = (s < static_cast<int>(unit.lane_is_hub_routed.size()))
                                  ? unit.lane_is_hub_routed[s]
                                  : false;
                int gi = unit.first_slot_global_index + s;
                auto slot = backend ? backend->get_slot_info(gi) : SlotInfo{};
                int tool = (slot.mapped_tool >= 0) ? slot.mapped_tool : gi;

                if (is_hub) {
                    has_hub_group = true;
                    if (hub_tool < 0 || tool < hub_tool)
                        hub_tool = tool;
                    hub_extruders.push_back(slot.extruder_name);
                } else {
                    direct_count++;
                    direct_tools.emplace_back(tool, slot.extruder_name);
                }
            }
            utl.tool_count = direct_count + (has_hub_group ? 1 : 0);
            utl.first_physical_tool = total_physical;

            // Map direct lane tools to physical positions first (sorted)
            std::sort(direct_tools.begin(), direct_tools.end());
            int idx = 0;
            for (const auto& [t, ext_name] : direct_tools) {
                result.virtual_to_physical[t] = total_physical + idx;
                physical_label_overrides[total_physical + idx] = t;
                record_extruder(total_physical + idx, ext_name);
                ++idx;
            }
            // Hub group gets last physical position
            if (has_hub_group && hub_tool >= 0) {
                result.virtual_to_physical[hub_tool] = total_physical + idx;
                physical_label_overrides[total_physical + idx] = hub_tool;
                for (const auto& ext_name : hub_extruders) {
                    record_extruder(total_physical + idx, ext_name);
                }
            }

            total_physical += utl.tool_count;
        } else if (topo != PathTopology::PARALLEL) {
            // HUB/LINEAR: all lanes converge to a single physical nozzle.
            // Multiple HUB units feeding one extruder (e.g. a BoxTurtle and a
            // Claymore both on e0) share one physical nozzle.
            //
            // Two independent ways to recognise that. Extruder NAME identity is
            // the primary one: it is a string compare, so it works on names no
            // numbering scheme can parse and needs nothing beyond the status
            // frame. hub_tool_label stays as the fallback for backends that
            // publish a label but no per-lane extruder name (the mock, and any
            // future backend that only knows tool numbers).
            std::string unit_extruder;
            for (const auto& slot : unit.slots) {
                if (slot.extruder_name.empty()) {
                    continue;
                }
                if (unit_extruder.empty()) {
                    unit_extruder = slot.extruder_name;
                } else if (unit_extruder != slot.extruder_name) {
                    unit_extruder.clear(); // lanes disagree -> not one nozzle, do not guess
                    break;
                }
            }
            utl.extruder_identity = unit_extruder;

            int shared_phys = -1;
            for (const auto& prev : result.units) {
                if (prev.tool_count != 1) {
                    continue;
                }
                const bool same_extruder =
                    !unit_extruder.empty() && prev.extruder_identity == unit_extruder;
                const bool same_label =
                    unit.hub_tool_label >= 0 && prev.hub_tool_label == unit.hub_tool_label;
                if (same_extruder || same_label) {
                    shared_phys = prev.first_physical_tool;
                    break;
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
                // Every lane on a HUB unit converges on the same extruder, so any
                // lane names the nozzle. record_extruder() clears the entry if two
                // lanes disagree, which would mean this is not really one nozzle.
                record_extruder(phys, slot.extruder_name);
            }
        } else if (min_tool >= 0) {
            // PARALLEL: each lane maps to its own physical nozzle.
            // Use distinct tool count (not max-min+1) to handle cross-unit remapping
            // where mapped tool numbers may span beyond this unit's normal range.
            int physical_first = total_physical;

            // Collect unique physical extruders (or fall back to unique mapped tools)
            // For HTLF setups, multiple lanes with different T-numbers may share
            // one physical extruder (e.g., lane3->T1/extruder2, lane4->T3/extruder2)
            bool have_extruder_names = false;
            for (const auto& slot : unit.slots) {
                if (!slot.extruder_name.empty()) {
                    have_extruder_names = true;
                    break;
                }
            }

            if (have_extruder_names) {
                // Group by extruder name: map extruder -> sorted set of mapped_tools
                std::map<std::string, std::vector<int>> extruder_tools;
                for (const auto& slot : unit.slots) {
                    if (slot.mapped_tool >= 0) {
                        std::string ext = slot.extruder_name.empty()
                                              ? ("__tool_" + std::to_string(slot.mapped_tool))
                                              : slot.extruder_name;
                        extruder_tools[ext].push_back(slot.mapped_tool);
                    }
                }

                int lane_count = static_cast<int>(extruder_tools.size());
                utl.first_physical_tool = physical_first;
                utl.tool_count = lane_count;

                // Assign each extruder group a physical position and record
                // the correct label (min tool in group) for each physical slot
                int idx = 0;
                for (auto& [ext_name, tools] : extruder_tools) {
                    int min_in_group = *std::min_element(tools.begin(), tools.end());
                    for (int t : tools) {
                        result.virtual_to_physical[t] = physical_first + idx;
                    }
                    physical_label_overrides[physical_first + idx] = min_in_group;
                    // The group key IS the extruder identity for this nozzle. The
                    // synthetic "__tool_N" key used for nameless lanes is not an
                    // extruder name and is deliberately not recorded.
                    if (ext_name.rfind("__tool_", 0) != 0) {
                        record_extruder(physical_first + idx, ext_name);
                    }
                    ++idx;
                }

                total_physical += lane_count;
            } else {
                // Original logic: group by unique mapped_tool
                std::vector<int> mapped_tools;
                for (const auto& slot : unit.slots) {
                    if (slot.mapped_tool >= 0) {
                        mapped_tools.push_back(slot.mapped_tool);
                    }
                }
                std::sort(mapped_tools.begin(), mapped_tools.end());
                mapped_tools.erase(std::unique(mapped_tools.begin(), mapped_tools.end()),
                                   mapped_tools.end());

                int lane_count = static_cast<int>(mapped_tools.size());
                utl.first_physical_tool = physical_first;
                utl.tool_count = lane_count;

                for (int j = 0; j < static_cast<int>(mapped_tools.size()); ++j) {
                    result.virtual_to_physical[mapped_tools[j]] = physical_first + j;
                }

                total_physical += lane_count;
            }
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

    // Apply extruder-name-based label overrides (physical order follows
    // alphabetical extruder names, not sorted virtual tool numbers)
    for (auto& [phys, label] : physical_label_overrides) {
        if (phys < static_cast<int>(result.physical_to_virtual_label.size())) {
            result.physical_to_virtual_label[phys] = label;
        }
    }

    // Physical→extruder-name map. Positions no branch could identify stay empty.
    result.physical_to_extruder_name.assign(total_physical, std::string());
    for (const auto& [phys, name] : physical_extruder_names) {
        if (phys >= 0 && phys < total_physical) {
            result.physical_to_extruder_name[phys] = name;
        }
    }

    return result;
}

bool layout_has_extruder_identity(const SystemToolLayout& layout) {
    if (layout.total_physical_tools <= 0 ||
        static_cast<int>(layout.physical_to_extruder_name.size()) != layout.total_physical_tools) {
        return false;
    }
    return std::all_of(
        layout.physical_to_extruder_name.begin(), layout.physical_to_extruder_name.end(),
        [](const std::string& name) { return helix::tool_number_for_extruder(name).has_value(); });
}

ToolBadgeLabels compute_tool_badge_labels(const SystemToolLayout& layout, const AmsSystemInfo& info,
                                          int current_slot, int active_physical_tool) {
    ToolBadgeLabels out;

    if (layout_has_extruder_identity(layout)) {
        out.prefix = 'E';
        out.numbers.reserve(layout.physical_to_extruder_name.size());
        for (const auto& name : layout.physical_to_extruder_name) {
            out.numbers.push_back(*helix::tool_number_for_extruder(name));
        }
        return out;
    }

    if (layout.physical_to_virtual_label.empty()) {
        return out;
    }

    out.prefix = 'T';
    out.numbers = layout.physical_to_virtual_label;

    // Legacy behaviour: for HUB units the static hub label is replaced by the
    // loaded lane's own virtual tool number (show "T6" when AMS_1 lane 3 is
    // loaded, not "T4").
    if (active_physical_tool >= 0 && active_physical_tool < static_cast<int>(out.numbers.size()) &&
        current_slot >= 0) {
        const SlotInfo* active_slot_info = info.get_slot_global(current_slot);
        if (active_slot_info && active_slot_info->mapped_tool >= 0) {
            out.numbers[active_physical_tool] = active_slot_info->mapped_tool;
        }
    }
    return out;
}

// ============================================================================
// Spool Visualization
// ============================================================================

// Draw a dashed circle using segmented arcs (LVGL 9.5 has no dashed border API)
void draw_dashed_circle_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* layer = static_cast<lv_layer_t*>(lv_event_get_layer(e));

    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int32_t cx = coords.x1 + w / 2;
    int32_t cy = coords.y1 + h / 2;
    int32_t radius = LV_MIN(w, h) / 2 - 1;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.center.x = cx;
    arc_dsc.center.y = cy;
    arc_dsc.radius = static_cast<uint16_t>(radius);
    arc_dsc.width = 2;
    arc_dsc.color = theme_manager_get_color("text_muted");
    arc_dsc.opa = LV_OPA_20;

    // Draw 16 dashes of 15 degrees each with 7.5 degree gaps
    constexpr int DASH_COUNT = 16;
    constexpr int DASH_ANGLE = 15;
    constexpr int GAP_ANGLE = 7; // 16 * (15 + 7) = 352 ≈ 360
    for (int d = 0; d < DASH_COUNT; d++) {
        arc_dsc.start_angle = static_cast<uint16_t>(d * (DASH_ANGLE + GAP_ANGLE));
        arc_dsc.end_angle = static_cast<uint16_t>(arc_dsc.start_angle + DASH_ANGLE);
        lv_draw_arc(layer, &arc_dsc);
    }
}

static bool resolve_3d_spool_style() {
    helix::Config* cfg = helix::Config::get_instance();
    return cfg->get<std::string>("/ams/spool_style", "3d") == "3d";
}

SpoolVisual create_spool_visual(lv_obj_t* container, int32_t spool_size) {
    SpoolVisual sv;
    if (!container)
        return sv;
    sv.container = container;
    sv.use_3d = resolve_3d_spool_style();

    // Spool size is a dedicated responsive token (see ams_panel.xml consts).
    if (spool_size <= 0) {
        spool_size = theme_manager_get_spacing("ams_slot_spool_size");
        if (spool_size <= 0)
            spool_size = theme_manager_get_spacing("space_lg") * 4;
    }
    sv.spool_size = spool_size;

    int32_t container_size = spool_size + SPOOL_VISUAL_BADGE_MARGIN_PX; // Extra room for badge
    lv_obj_set_size(container, container_size, container_size);

    if (sv.use_3d) {
        // ====================================================================
        // 3D SPOOL CANVAS (Bambu-style pseudo-3D with gradients + AA)
        // ====================================================================
        lv_obj_t* canvas = ui_spool_canvas_create(container, spool_size);
        if (canvas) {
            lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
            // Prevent flex layout from resizing the canvas
            lv_obj_set_style_min_width(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_min_height(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_max_width(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_max_height(canvas, spool_size, LV_PART_MAIN);
            ui_spool_canvas_set_color(canvas, lv_color_hex(AMS_DEFAULT_SLOT_COLOR));
            ui_spool_canvas_set_fill_level(canvas, 1.0f);
            lv_obj_add_flag(canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
            sv.canvas = canvas;
        }
    } else {
        // ====================================================================
        // FLAT STYLE (skeuomorphic concentric rings)
        // ====================================================================
        int32_t filament_ring_size = spool_size - 8;
        int32_t hub_size = spool_size / 3;

        lv_obj_set_style_radius(container, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(container, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(container, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(container, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(container, lv_color_black(), LV_PART_MAIN);

        // Layer 1: Outer ring (flange - darker shade of filament color)
        lv_obj_t* outer_ring = lv_obj_create(container);
        lv_obj_set_size(outer_ring, spool_size, spool_size);
        lv_obj_align(outer_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(outer_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(outer_ring,
                                  ams_draw::darken_color(lv_color_hex(AMS_DEFAULT_SLOT_COLOR), 50),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(outer_ring, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(outer_ring, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(outer_ring, theme_manager_get_color("ams_hub"), LV_PART_MAIN);
        lv_obj_set_style_border_opa(outer_ring, LV_OPA_50, LV_PART_MAIN);
        lv_obj_remove_flag(outer_ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(outer_ring, LV_OBJ_FLAG_EVENT_BUBBLE);
        sv.spool_outer = outer_ring;

        // Layer 2: Main filament color ring
        lv_obj_t* filament_ring = lv_obj_create(container);
        lv_obj_set_size(filament_ring, filament_ring_size, filament_ring_size);
        lv_obj_align(filament_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(filament_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(filament_ring, lv_color_hex(AMS_DEFAULT_SLOT_COLOR),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(filament_ring, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(filament_ring, 0, LV_PART_MAIN);
        lv_obj_remove_flag(filament_ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(filament_ring, LV_OBJ_FLAG_EVENT_BUBBLE);
        sv.color_swatch = filament_ring;

        // Layer 3: Center hub
        lv_obj_t* hub = lv_obj_create(container);
        lv_obj_set_size(hub, hub_size, hub_size);
        lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(hub, theme_manager_get_color("ams_hub"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(hub, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(hub, theme_manager_get_color("ams_hub_border"), LV_PART_MAIN);
        lv_obj_remove_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hub, LV_OBJ_FLAG_EVENT_BUBBLE);
        sv.spool_hub = hub;
    }

    // Create empty slot placeholder (circle outline with plus icon, initially hidden)
    {
        lv_obj_t* ph = lv_obj_create(container);
        lv_obj_set_size(ph, spool_size - 4, spool_size - 4);
        lv_obj_align(ph, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ph, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ph, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(ph, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(ph, ams_draw::draw_dashed_circle_cb, LV_EVENT_DRAW_MAIN, nullptr);
        lv_obj_remove_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ph, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(ph, LV_OBJ_FLAG_HIDDEN);

        // Plus icon centered in circle to communicate "empty, add filament"
        const char* plus_glyph = ui_icon::lookup_codepoint("plus");
        if (plus_glyph) {
            lv_obj_t* plus = lv_label_create(ph);
            lv_label_set_text(plus, plus_glyph);
            lv_obj_set_style_text_font(plus, &mdi_icons_24, LV_PART_MAIN);
            lv_obj_set_style_text_color(plus, theme_manager_get_color("text_muted"), LV_PART_MAIN);
            lv_obj_set_style_text_opa(plus, LV_OPA_20, LV_PART_MAIN);
            lv_obj_align(plus, LV_ALIGN_CENTER, 0, 0);
            lv_obj_add_flag(plus, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        sv.empty_placeholder = ph;
    }

    // Create error indicator dot (top-right of container, initially hidden)
    {
        lv_obj_t* err = lv_obj_create(container);
        lv_obj_set_size(err, 14, 14);
        lv_obj_set_style_radius(err, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(err, theme_manager_get_color("danger"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(err, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(err, 0, LV_PART_MAIN);
        lv_obj_set_align(err, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_style_translate_x(err, -2, LV_PART_MAIN);
        lv_obj_set_style_translate_y(err, 2, LV_PART_MAIN);
        lv_obj_remove_flag(err, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(err, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(err, LV_OBJ_FLAG_HIDDEN);
        sv.error_indicator = err;
    }

    return sv;
}

// Ported verbatim from ui_ams_slot.cpp apply_slot_color()
void spool_visual_set_color(const SpoolVisual& sv, lv_color_t color) {
    if (sv.use_3d) {
        if (sv.canvas)
            ui_spool_canvas_set_color(sv.canvas, color);
    } else if (sv.color_swatch) {
        lv_obj_set_style_bg_color(sv.color_swatch, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sv.color_swatch, LV_OPA_COVER, LV_PART_MAIN);
        if (sv.spool_outer)
            lv_obj_set_style_bg_color(sv.spool_outer, ams_draw::darken_color(color, 50),
                                      LV_PART_MAIN);
    }
}

// Ported verbatim from ui_ams_slot.cpp update_filament_ring_size()
void spool_visual_set_fill(const SpoolVisual& sv, float fill) {
    if (fill < 0.0f)
        fill = 0.0f;
    if (fill > 1.0f)
        fill = 1.0f;
    if (sv.use_3d) {
        if (sv.canvas)
            ui_spool_canvas_set_fill_level(sv.canvas, fill);
    } else if (sv.color_swatch && sv.container && sv.spool_hub) {
        lv_obj_update_layout(sv.container);
        int32_t spool_w = lv_obj_get_width(sv.container);
        int32_t hub_w = lv_obj_get_width(sv.spool_hub);
        int32_t min_ring = hub_w + 4;
        int32_t max_ring = spool_w - 8;
        int32_t ring_size = min_ring + static_cast<int32_t>((max_ring - min_ring) * fill);
        lv_obj_set_size(sv.color_swatch, ring_size, ring_size);
        lv_obj_align(sv.color_swatch, LV_ALIGN_CENTER, 0, 0);
    }
}

void spool_visual_set_empty(const SpoolVisual& sv, bool empty) {
    auto show = [&](lv_obj_t* o, bool visible) {
        if (!o)
            return;
        if (visible)
            lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    show(sv.empty_placeholder, empty);
    show(sv.canvas, !empty);
    show(sv.spool_outer, !empty);
    show(sv.color_swatch, !empty);
    show(sv.spool_hub, !empty);
}

void spool_visual_set_error(const SpoolVisual& sv, bool has_error) {
    if (sv.error_indicator) {
        if (has_error)
            lv_obj_remove_flag(sv.error_indicator, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(sv.error_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t* create_lane_badge(lv_obj_t* parent, int lane_number, int32_t size, bool active) {
    if (!parent)
        return nullptr;
    lv_obj_t* badge = lv_obj_create(parent);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_color_t bg =
        active ? theme_manager_get_color("success") : theme_manager_get_color("ams_badge_bg");
    lv_obj_set_style_bg_color(badge, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(badge, theme_manager_get_color("card_bg"), LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
    lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t* lbl = lv_label_create(badge);
    char txt[8];
    snprintf(txt, sizeof(txt), "%d", lane_number); // 1-based; lane numbers not translated
    lv_label_set_text(lbl, txt);
    const lv_font_t* f = theme_manager_get_font("font_xs");
    if (f)
        lv_obj_set_style_text_font(lbl, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, theme_manager_get_color("text"), LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    return badge;
}

} // namespace ams_draw
