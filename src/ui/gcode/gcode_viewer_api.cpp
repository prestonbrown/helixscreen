// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "ui_gcode_viewer.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_constants.h"
#include "color_utils.h"
#include "config.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "gcode_camera.h"
#include "gcode_color_metadata.h"
#include "gcode_gl_fallback.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "gcode_pause_scan.h"
#include "gcode_render_mode_policy.h"
#include "gcode_render_schedule.h"
#include "gcode_ssao_policy.h"
#include "gcode_streaming_config.h"
#include "gcode_streaming_controller.h"
#include "gcode_viewer_state.h"
#include "gcode_viewer_watchdog.h"
#include "geometry_budget_manager.h"
#include "lvgl.h"
#include "memory_utils.h"
#include "print_status_preview_decision.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "theme_manager.h"
#include "view_gestures.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cmath>
#include <helix-xml/src/xml/lv_xml_parser.h>
#include <helix-xml/src/xml/parsers/lv_xml_obj_parser.h>

using namespace helix;
using namespace helix::gcode_viewer;

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_set>

// ==============================================
// Rendering Pause Control
// ==============================================

void helix::ui::gcode_viewer_set_paused(lv_obj_t* obj, bool paused) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    if (st->rendering_paused_ != paused) {
        st->rendering_paused_ = paused;

        // Include cache state in the log so a frozen-render bundle shows
        // whether resume actually got the cache moving again.
        int cached = st->layer_renderer_2d_ ? st->layer_renderer_2d_->get_cached_up_to_layer() : -1;
        int target = st->layer_renderer_2d_ ? st->layer_renderer_2d_->get_current_layer() : -1;
        spdlog::debug("[GCode Viewer] Rendering {} (cached={} target={} progress_layer={})",
                      paused ? "PAUSED" : "RESUMED", cached, target, st->print_progress_layer_);

        // If resuming, trigger a redraw to show current state
        if (!paused) {
            lv_obj_invalidate(obj);

            // The cache did not move while paused, which is not a stall.
            gcode_viewer_watchdog_restart(st);
        }
    }
}

bool helix::ui::gcode_viewer_is_paused(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    return st ? st->rendering_paused_ : true;
}

void helix::ui::gcode_viewer_force_redraw(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

        // 3D path: the renderer's cached-blit fast path skips re-rendering when
        // state is unchanged and draw_buf_ exists. Drop the draw_buf so the next
        // DRAW_POST takes the full render path (run_slice -> blit_to_lvgl).
#ifdef ENABLE_3D_RENDERER
    if (st->renderer_) {
        st->renderer_->clear_cached_frame();
    }
#endif

    // 2D path doesn't expose an invalidate hook, but the invalidate below
    // forces DRAW_POST which re-runs the renderer; without a stale draw_buf
    // to short-circuit on, that's sufficient for the 2D case.

    lv_obj_invalidate(obj);
    spdlog::debug("[GCode Viewer] force_redraw issued");
}

// ==============================================
// Render Mode Control
// ==============================================

void helix::ui::gcode_viewer_set_render_mode(lv_obj_t* obj, GcodeViewerRenderMode mode) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->render_mode_ = mode;

    const char* mode_names[] = {"AUTO", "3D", "2D_LAYER"};
    spdlog::debug("[GCode Viewer] Render mode set to {}", mode_names[static_cast<int>(mode)]);

    // If using 2D mode (AUTO or 2D_LAYER), ensure the 2D renderer is initialized
    if (st->is_using_2d_mode() && st->gcode_file && !st->layer_renderer_2d_) {
        lv_area_t coords;
        lv_obj_get_coords(obj, &coords);
        create_2d_renderer_for_file(st, lv_area_get_width(&coords), lv_area_get_height(&coords));
    }

#ifdef ENABLE_3D_RENDERER
    // A file loaded in 2D skipped the 3D build. A full load still running builds
    // it itself when it reaches that point, so only start one when idle.
    if (!st->is_using_2d_mode() && st->gcode_file && st->renderer_ &&
        !st->renderer_->has_geometry() && !st->is_building()) {
        start_on_demand_3d_build(st, obj);
    }
#endif

    lv_obj_invalidate(obj);
}

bool helix::ui::gcode_viewer_is_using_2d_mode(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    return st ? st->is_using_2d_mode() : false;
}

bool helix::ui::gcode_viewer_load_skipped_for_cost(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    return st && st->load_skipped_for_cost;
}

void helix::ui::gcode_viewer_disable_streaming(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (st) {
        st->streaming_disabled_ = true;
    }
}

// ==============================================
// Camera Controls
// ==============================================

void helix::ui::gcode_viewer_reset_camera(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera_->reset();

    // Re-fit to model if loaded
    if (st->gcode_file) {
        st->camera_->fit_to_bounds(st->gcode_file->global_bounding_box);
    }

    lv_obj_invalidate(obj);
}

// ==============================================
// Rendering Options
// ==============================================

void helix::ui::gcode_viewer_set_highlighted_objects(
    lv_obj_t* obj, const std::unordered_set<std::string>& object_names) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->view_options.highlighted = object_names;
    apply_view_options(st);
    lv_obj_invalidate(obj);
}

void helix::ui::gcode_viewer_set_excluded_objects(
    lv_obj_t* obj, const std::unordered_set<std::string>& object_names) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    // Skip if excluded set hasn't changed (avoids expensive cache invalidation)
    if (object_names == st->view_options.excluded) {
        return;
    }

    st->view_options.excluded = object_names;
    // An excluded object is no longer a selection: drop its brackets and rim,
    // and the tap toggle state that would otherwise re-select it.
    for (const auto& name : object_names) {
        st->view_options.highlighted.erase(name);
        st->selected_objects.erase(name);
    }
    apply_view_options(st);
    lv_obj_invalidate(obj);

    spdlog::debug("[GCode Viewer] Excluded objects updated ({} objects)", object_names.size());
}

void helix::ui::gcode_viewer_set_object_badges(lv_obj_t* obj,
                                               std::vector<helix::ui::ObjectBadge> badges) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || badges == st->object_badges)
        return;
    st->object_badges = std::move(badges);
    st->badge_look = helix::ui::resolve_badge_look(st->object_badges);
    // Drawn indices name the old list; the next frame records the new one.
    st->drawn_badge_index.clear();
    st->drawn_badge_centers.clear();
    // Only the overlay changed: the renderers repaint from their caches.
    lv_obj_invalidate(obj);
}

void helix::ui::gcode_viewer_set_excluded_badges_pickable(lv_obj_t* obj, bool pickable) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return;
    }
    st->excluded_badges_pickable = pickable;
}

void helix::ui::gcode_viewer_set_object_tap_callback(
    lv_obj_t* obj, helix::ui::gcode_viewer_object_tap_callback_t callback, void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->object_tap_callback = callback;
    st->object_tap_user_data = user_data;
}

void helix::ui::gcode_viewer_set_object_long_press_callback(
    lv_obj_t* obj, helix::ui::gcode_viewer_object_long_press_callback_t callback, void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->object_long_press_callback = callback;
    st->object_long_press_user_data = user_data;

    spdlog::debug("[GCode Viewer] Long-press callback {}", callback ? "registered" : "cleared");
}

// ==============================================
// Color & Rendering Control
// ==============================================

void helix::ui::gcode_viewer_set_tool_colors(lv_obj_t* obj, const std::vector<uint32_t>& colors) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || colors.empty())
        return;

    // Same vector as last time is already applied — drop it instead of redoing
    // the work. Not a micro-optimisation: each apply joins the 2D renderer's
    // background ghost-render worker and invalidates its layer cache, and the
    // observers that drive recoloring (active-lane color, tool map, slot data)
    // all fire together during a toolchange, which is exactly when the preview
    // is on screen. The comparison lives here rather than in a caller because
    // view_options.tool_colors is already the viewer's record of what is applied,
    // and it is cleared by helix::ui::gcode_viewer_clear() — so a reload correctly
    // re-applies even when the colors are unchanged.
    if (!st->view_options.tool_colors.empty() && st->view_options.tool_colors == colors) {
        return;
    }

    st->view_options.tool_colors = colors;
    apply_view_options(st);

    lv_obj_invalidate(obj);
    spdlog::debug("[GCode Viewer] Applied {} per-tool AMS color overrides", colors.size());
}

void helix::gcode_viewer::ui_gcode_viewer_clear_tool_colors(lv_obj_t* obj) {
    if (!obj) {
        return;
    }
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || st->view_options.tool_colors.empty()) {
        return;
    }

    // Retraction needs its own entry point rather than set_tool_colors(obj, {}):
    // an empty vector already means "no information" everywhere below, and every
    // layer correctly refuses to act on it so that a FIRST apply with no AMS data
    // leaves the slicer palette alone. The two meanings cannot share a call.
    st->view_options.tool_colors.clear();
    apply_view_options(st);

    lv_obj_invalidate(obj);
    spdlog::debug("[GCode Viewer] Retracted per-tool AMS color overrides");
}

bool helix::ui::gcode_viewer_apply_ams_tool_colors(lv_obj_t* obj) {
    if (!obj) {
        return false;
    }
    // Pure adapter over the ONE color rule: color(tool N) = the color of the lane
    // that actually prints N. AmsSystemInfo::tool_to_slot_map is not that
    // answer on a tool changer, where it is physical attachment rather than
    // print routing. Empty means "nothing knowable": leave the
    // renderer's slicer palette alone rather than painting over it.
    const auto colors = AmsState::instance().routed_tool_colors();
    if (colors.empty()) {
        // "Nothing knowable" must also RETRACT: a previous non-degenerate
        // answer may still be applied as overrides, and returning false alone
        // would leave those lane colors frozen on the renderer for the rest
        // of the file instead of falling back to the slicer palette.
        ui_gcode_viewer_clear_tool_colors(obj);
        return false;
    }
    helix::ui::gcode_viewer_set_tool_colors(obj, colors);
    spdlog::debug("[GCode Viewer] Applied {} lane-derived tool colors", colors.size());
    return true;
}

// ==============================================
// Print Progress / Ghost Layer Visualization
// ==============================================

void helix::ui::gcode_viewer_set_print_progress(lv_obj_t* obj, int current_layer) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    // Skip if layer hasn't changed (avoids unnecessary invalidation)
    if (current_layer == st->print_progress_layer_) {
        return;
    }

    int prev_layer = st->print_progress_layer_;

    // Store the print progress layer for use by render callback
    st->print_progress_layer_ = current_layer;
    st->print_progress_last_change_ms_ = lv_tick_get();

    // Trace-level: this fires on every Moonraker layer event during a print,
    // which is multiple times per second on a fast print — too noisy for
    // default-bundled debug logs. The watchdog warn carries the values that
    // actually matter when something is wrong.
    spdlog::trace("[GCode Viewer] set_print_progress {} -> {} (paused={})", prev_layer,
                  current_layer, st->rendering_paused_);

    // Skip renderer updates and invalidation when paused —
    // the stored value above will be picked up on resume.
    if (st->rendering_paused_) {
        return;
    }

#ifdef ENABLE_3D_RENDERER
    st->renderer_->set_print_progress_layer(current_layer);
#endif

    // Note: 2D renderer's current_layer is set in the render callback
    // using print_progress_layer_, so we just need to invalidate.
    lv_obj_invalidate(obj);
}

/// Drop the reference when the strip is destroyed, so a later draw cannot
/// measure freed coordinates. Registered on the occluder, keyed to the viewer.
void helix::gcode_viewer::gcode_viewer_occluder_delete_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    gcode_viewer_state_t* st = get_state(obj);
    if (st) {
        st->bottom_occluder_ = nullptr;
    }
}

void helix::ui::gcode_viewer_set_bottom_occluder(lv_obj_t* obj, lv_obj_t* occluder) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    if (st->bottom_occluder_ == occluder) {
        return;
    }

    // Stop listening to the widget we are letting go of, or its delete would
    // clear a reference that now belongs to a different strip.
    if (st->bottom_occluder_) {
        lv_obj_remove_event_cb_with_user_data(st->bottom_occluder_, gcode_viewer_occluder_delete_cb,
                                              obj);
    }

    st->bottom_occluder_ = occluder;

    if (occluder) {
        lv_obj_add_event_cb(occluder, gcode_viewer_occluder_delete_cb, LV_EVENT_DELETE, obj);
    }

    // The offset is recomputed on the next draw from live geometry; nothing to
    // apply here, and the widgets may not be laid out yet.
    lv_obj_invalidate(obj);
    spdlog::debug("[GCode Viewer] Bottom occluder {}", occluder ? "set" : "cleared");
}

/// Fraction of the viewer's height that the occluder covers, 0 when they do not
/// overlap (the strip is a sibling BELOW the preview in some layouts) or when
/// either widget is hidden.
static float measure_bottom_occlusion(gcode_viewer_state_t* st, lv_obj_t* obj) {
    if (!st->bottom_occluder_ || lv_obj_is_hidden(st->bottom_occluder_)) {
        return 0.0f;
    }

    lv_area_t viewer_area;
    lv_area_t occluder_area;
    lv_obj_get_coords(obj, &viewer_area);
    lv_obj_get_coords(st->bottom_occluder_, &occluder_area);

    const int32_t viewer_h = lv_area_get_height(&viewer_area);
    if (viewer_h <= 0) {
        return 0.0f;
    }

    // Only the part of the strip that reaches into the viewer counts.
    const int32_t overlap = viewer_area.y2 - std::max(occluder_area.y1, viewer_area.y1) + 1;
    if (overlap <= 0) {
        return 0.0f;
    }

    return std::min(1.0f, static_cast<float>(overlap) / static_cast<float>(viewer_h));
}

/// Push the live occlusion and framing mode down to whichever renderer is
/// active. Both the fit and the vertical shift derive from them, so the
/// renderer owns that computation and re-fits when a number moves; this only
/// has to keep them current. Cheap enough to run per draw, which is what
/// keeps the framing right across relayout and lazy renderer creation without
/// the panel having to repush anything.
void helix::gcode_viewer::gcode_viewer_refresh_content_offset(gcode_viewer_state_t* st,
                                                              lv_obj_t* obj, int canvas_width,
                                                              int canvas_height) {
    const float occlusion = measure_bottom_occlusion(st, obj);
    const auto framing = st->framing_;

    if (st->layer_renderer_2d_) {
        st->layer_renderer_2d_->set_framing(framing);
        st->layer_renderer_2d_->set_bottom_occlusion(occlusion);
    }
#ifdef ENABLE_3D_RENDERER
    if (st->camera_) {
        st->camera_->set_framing(framing);
        st->camera_->set_bottom_occlusion(occlusion);
    }
    if (st->renderer_) {
        // The GLES path applies the shift in build_mvp(); the camera has already
        // absorbed the occlusion (or, under parity, the square) into its zoom.
        // Parity pins the model centre in the lifted square and ignores both
        // the content height and the occlusion.
        const float shift = framing == helix::gcode::FitFraming::THUMBNAIL_PARITY
                                ? helix::gcode::parity_content_offset_y(canvas_width, canvas_height)
                                : helix::gcode::compute_content_offset_y(
                                      st->camera_ ? st->camera_->get_content_height_fraction() *
                                                        static_cast<float>(canvas_height)
                                                  : 0.0f,
                                      canvas_height, occlusion);
        st->renderer_->set_content_offset_y(shift);
    }
#endif
}

int helix::ui::gcode_viewer_get_max_layer(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return -1;

    // In streaming mode, get layer count from streaming controller
    if (st->streaming_controller_ && st->streaming_controller_->is_open()) {
        return static_cast<int>(st->streaming_controller_->get_layer_count()) - 1;
    }

    // In 2D mode with parsed gcode, get from 2D renderer
    if (st->layer_renderer_2d_) {
        return st->layer_renderer_2d_->get_layer_count() - 1;
    }

#ifdef ENABLE_3D_RENDERER
    return st->renderer_->get_max_layer_index();
#else
    // The 2D renderer is made on its first draw; until then the parsed file it
    // will read answers, so a caller asking at load time gets the real count.
    return st->gcode_file ? static_cast<int>(st->gcode_file->layers.size()) - 1 : -1;
#endif
}

// ==============================================
// Parsed Data Access
// ==============================================

const helix::gcode::ParsedGCodeFile* helix::ui::gcode_viewer_get_parsed_file(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file)
        return nullptr;

    return st->gcode_file.get();
}

static std::vector<std::string> ui_gcode_viewer_get_tool_palette(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return {};
    }
    // gcode_file and streaming_controller_ are mutually exclusive by
    // construction (see the state struct), so this is a choice of which ONE
    // holds data, not a precedence question.
    if (st->gcode_file && !st->gcode_file->tool_color_palette.empty()) {
        return st->gcode_file->tool_color_palette;
    }
    if (st->streaming_controller_ && st->streaming_controller_->is_open()) {
        return st->streaming_controller_->get_index_stats().filament_palette;
    }
    return {};
}

std::set<int> helix::ui::gcode_viewer_get_tools_used(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return {};
    }
    // Same "which ONE holds data" choice as get_tool_palette above.
    if (st->gcode_file) {
        return st->gcode_file->tools_used_indices;
    }
    if (st->streaming_controller_ && st->streaming_controller_->is_open()) {
        const auto& stats = st->streaming_controller_->get_index_stats();
        std::set<int> tools = stats.tools_used;
        // Apply ParsedGCodeFile's single-extruder convention (see
        // GCodeParser::finalize: tools_used_indices gets {0} when the file names
        // no tool but does carry a colour palette). The index scan deliberately
        // does not inject it — it has no opinion about palettes — so it is
        // applied here, where both halves must agree for the same file.
        if (tools.empty() && !stats.filament_palette.empty()) {
            tools.insert(0);
        }
        return tools;
    }
    return {};
}

std::optional<helix::FitBox> helix::ui::gcode_viewer_get_model_bounds(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return std::nullopt;
    }
    // Same "which ONE holds data" choice as get_tool_palette above.
    if (st->gcode_file) {
        const auto& bb = st->gcode_file->global_bounding_box;
        if (!(bb.min.x <= bb.max.x && bb.min.y <= bb.max.y)) {
            return std::nullopt;
        }
        helix::FitBox box;
        box.min = {bb.min.x, bb.min.y, bb.min.z};
        box.max = {bb.max.x, bb.max.y, bb.max.z};
        return box;
    }
    if (st->streaming_controller_ && st->streaming_controller_->is_open()) {
        const auto& stats = st->streaming_controller_->get_index_stats();
        if (!stats.has_xy_bounds()) {
            return std::nullopt;
        }
        helix::FitBox box;
        box.min = {stats.min_x, stats.min_y, std::nullopt};
        box.max = {stats.max_x, stats.max_y, std::nullopt};
        if (stats.has_z_bounds()) {
            box.min[2] = stats.min_z;
            box.max[2] = stats.max_z;
        }
        return box;
    }
    return std::nullopt;
}

bool helix::ui::gcode_viewer_adopt_palette_if_empty(lv_obj_t* obj,
                                                    std::vector<std::string>& colors) {
    if (!colors.empty()) {
        return false;
    }
    auto palette = ui_gcode_viewer_get_tool_palette(obj);
    if (palette.empty()) {
        return false;
    }
    spdlog::info("[GCode Viewer] Metadata lacked filament colors — recovered {} from the file",
                 palette.size());
    colors = std::move(palette);
    return true;
}

namespace helix {
helix::gcode::PauseScanPtr helix::ui::gcode_viewer_get_pause_scan(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    return st ? st->pause_scan : nullptr;
}
} // namespace helix

float helix::ui::gcode_viewer_get_load_progress(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->streaming_controller_) {
        // Full-load mode, or nothing loading. Neither has a checkpoint to
        // report; 0.0 is the caller's cue to stay indeterminate.
        return 0.0f;
    }
    return st->streaming_controller_->get_index_progress();
}

bool helix::ui::gcode_viewer_pump_offscreen_2d(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return false;
    }
    // Only the 2D renderer builds off the draw pass. A 3D viewer cannot be
    // pumped at all, and claiming readiness here would reveal one that has not
    // uploaded yet - it stays visible under the thumbnail and reveals from its
    // own first-frame callback instead.
    if (!st->is_using_2d_mode()) {
        return false;
    }
    lv_obj_t* parent = lv_obj_get_parent(obj);
    const auto [w, h] =
        helix::ui::preview_build_size(lv_obj_get_width(obj), lv_obj_get_height(obj),
                                      parent ? lv_obj_get_content_width(parent) : 0,
                                      parent ? lv_obj_get_content_height(parent) : 0);
    if (w <= 0 || h <= 0) {
        // Nothing is laid out yet; the next tick will have real dimensions.
        return false;
    }
    if (!st->layer_renderer_2d_) {
        if (!st->gcode_file) {
            // Streaming builds its own renderer when the index opens; a full
            // load has no parsed file yet, so there is nothing to build from.
            return false;
        }
        create_2d_renderer_for_file(st, w, h);
    }
    return st->layer_renderer_2d_->pump_offscreen_build(w, h);
}

// ==============================================
// Statistics
// ==============================================

const char* helix::ui::gcode_viewer_get_filename(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return nullptr;

    // Streaming mode: get filename from controller
    if (st->streaming_controller_ && st->streaming_controller_->is_open()) {
        static std::string streaming_name; // Thread-safe for single-threaded LVGL
        streaming_name = st->streaming_controller_->get_source_name();
        return streaming_name.empty() ? nullptr : streaming_name.c_str();
    }

    // Full-file mode
    if (st->gcode_file && !st->gcode_file->filename.empty()) {
        return st->gcode_file->filename.c_str();
    }

    return nullptr;
}

int helix::ui::gcode_viewer_get_layer_count(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return 0;

    // Streaming mode: get layer count from controller
    if (st->streaming_controller_ && st->streaming_controller_->is_open()) {
        return static_cast<int>(st->streaming_controller_->get_layer_count());
    }

    // Full-file mode: get layer count from parsed file
    if (st->gcode_file) {
        return static_cast<int>(st->gcode_file->layers.size());
    }

    return 0;
}

namespace helix::test_access {

const helix::gcode::GCodeLayerRenderer*
gcode_viewer_budget_force_2d(lv_obj_t* viewer,
                             std::unique_ptr<helix::gcode::ParsedGCodeFile> file) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (!st) {
        return nullptr;
    }
    st->gcode_file = std::move(file);
    apply_budget_forced_2d(st, viewer);
    return st->layer_renderer_2d_.get();
}

void gcode_viewer_install_loaded_file(lv_obj_t* viewer,
                                      std::unique_ptr<helix::gcode::ParsedGCodeFile> file) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (!st) {
        return;
    }
    st->gcode_file = std::move(file);
    st->viewer_state = GcodeViewerState::Loaded;
}

void gcode_viewer_wait_for_build(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (st) {
        st->wait_for_build();
    }
}

std::vector<uint32_t> gcode_viewer_tool_colors(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st ? st->view_options.tool_colors : std::vector<uint32_t>{};
}

void gcode_viewer_clear_tool_colors(lv_obj_t* viewer) {
    ui_gcode_viewer_clear_tool_colors(viewer);
}

uint64_t gcode_viewer_load_generation(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st ? st->load_generation() : 0;
}

const helix::gcode::GCodeLayerRenderer* gcode_viewer_2d_renderer(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st ? st->layer_renderer_2d_.get() : nullptr;
}

helix::gcode::GCodeGLESRenderer* gcode_viewer_3d_renderer(lv_obj_t* viewer) {
#ifdef ENABLE_3D_RENDERER
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st ? st->renderer_.get() : nullptr;
#else
    (void)viewer;
    return nullptr;
#endif
}

std::vector<uint32_t> gcode_viewer_3d_palette(lv_obj_t* viewer) {
#ifdef ENABLE_3D_RENDERER
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (st && st->renderer_) {
        return st->renderer_->get_geometry_color_palette();
    }
#else
    (void)viewer;
#endif
    return {};
}

helix::GcodeViewerRenderMode gcode_viewer_render_mode(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st ? st->render_mode_.load() : helix::GcodeViewerRenderMode::Auto;
}

GcodeViewerWatchdogTrack gcode_viewer_watchdog_track(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (!st) {
        return {};
    }
    return {st->watchdog_last_cached_layer_, st->watchdog_last_target_layer_,
            st->watchdog_stall_streak_};
}

void gcode_viewer_set_watchdog_track(lv_obj_t* viewer, const GcodeViewerWatchdogTrack& track) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (!st) {
        return;
    }
    st->watchdog_last_cached_layer_ = track.prev_cached;
    st->watchdog_last_target_layer_ = track.prev_target;
    st->watchdog_stall_streak_ = track.stall_streak;
}

helix::gcode::GCodeLayerRenderer*
gcode_viewer_show_2d(lv_obj_t* viewer, std::unique_ptr<helix::gcode::ParsedGCodeFile> file) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (!st) {
        return nullptr;
    }
    st->gcode_file = std::move(file);
    st->viewer_state = GcodeViewerState::Loaded;
    st->first_render = false;
    apply_budget_forced_2d(st, viewer);
    return st->layer_renderer_2d_.get();
}

std::vector<helix::ui::ObjectBadge> gcode_viewer_object_badges(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st ? st->object_badges : std::vector<helix::ui::ObjectBadge>{};
}

std::vector<lv_color_t> gcode_viewer_badge_fills(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st ? st->badge_look.fill : std::vector<lv_color_t>{};
}

std::vector<lv_color_t> gcode_viewer_badge_texts(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st ? st->badge_look.text : std::vector<lv_color_t>{};
}

std::vector<GcodeViewerDrawnBadge> gcode_viewer_drawn_badges(lv_obj_t* viewer) {
    std::vector<GcodeViewerDrawnBadge> out;
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (!st) {
        return out;
    }
    for (size_t i = 0; i < st->drawn_badge_index.size(); ++i) {
        out.push_back({st->object_badges[static_cast<size_t>(st->drawn_badge_index[i])].name,
                       st->drawn_badge_centers[i]});
    }
    return out;
}

void gcode_viewer_fire_object_tap(lv_obj_t* viewer, const char* name) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    if (st && st->object_tap_callback) {
        st->object_tap_callback(viewer, name, st->object_tap_user_data);
    }
}

bool gcode_viewer_excluded_badges_pickable(lv_obj_t* viewer) {
    gcode_viewer_state_t* st = viewer ? get_state(viewer) : nullptr;
    return st && st->excluded_badges_pickable;
}

} // namespace helix::test_access

#endif // HELIX_HAS_GCODE_VIEWER
