// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "ui_gcode_viewer.h"

#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_constants.h"
#include "color_utils.h"
#include "config.h"
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
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_utils.h"
#include "print_status_preview_decision.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "text_io.h"
#include "theme_manager.h"
#include "view_gestures.h"

#include <cerrno>
#include <cmath>

// FPS tracking constants (for diagnostic logging, not mode selection)
constexpr float MIN_ACTUAL_RENDER_MS = 2.0f; // Minimum render time to count as actual render
constexpr float FPS_EMA_ALPHA = 0.1f;        // Exponential moving average smoothing factor
constexpr int FPS_LOG_INTERVAL_FRAMES = 30;  // Log FPS every N frames

#include <spdlog/spdlog.h>

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

/// Registry of live gcode viewer widgets. Populated on create, drained on
/// delete. Used by ui_gcode_viewer_clear_all_active() for the memory-pressure
/// fallback (print_status + print_select_detail) so we can release every
/// ParsedGCodeFile + GPU buffer system-wide in one call without having to
/// know which panels currently hold a viewer. Main-thread-only access (LVGL
/// widget lifecycle is main-thread by contract); no mutex needed.
static std::vector<lv_obj_t*>& active_viewers() {
    static std::vector<lv_obj_t*> instances;
    return instances;
}

static void gcode_viewer_refresh_content_offset(gcode_viewer_state_t* st, lv_obj_t* obj,
                                                int canvas_width, int canvas_height);

/// Registered on the occluder, keyed to the viewer. Declared here so the
/// viewer's own delete handler can detach it before this object is freed.
static void gcode_viewer_occluder_delete_cb(lv_event_t* e);

/// The centered spinner card shown while the viewer has nothing to draw yet.
void helix::gcode_viewer::create_loading_ui(gcode_viewer_state_t* st, lv_obj_t* obj,
                                            const char* text) {
    st->loading_container = lv_obj_create(obj);
    lv_obj_set_size(st->loading_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(st->loading_container);
    lv_obj_set_flex_flow(st->loading_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st->loading_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(st->loading_container, theme_manager_get_color("card_bg"),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st->loading_container, 220, LV_PART_MAIN);
    lv_obj_set_style_border_width(st->loading_container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(st->loading_container, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(st->loading_container, theme_manager_get_spacing("space_xl"),
                             LV_PART_MAIN);
    lv_obj_set_style_pad_gap(st->loading_container, theme_manager_get_spacing("space_md"),
                             LV_PART_MAIN);

    st->loading_spinner = lv_spinner_create(st->loading_container);
    int32_t spinner_size = theme_manager_get_spacing("spinner_lg");
    if (spinner_size <= 0)
        spinner_size = 48;
    int32_t spinner_arc = theme_manager_get_spacing("spinner_arc_lg");
    if (spinner_arc <= 0)
        spinner_arc = 4;
    lv_obj_set_size(st->loading_spinner, spinner_size, spinner_size);
    lv_color_t primary = theme_manager_get_color("primary");
    lv_obj_set_style_arc_color(st->loading_spinner, primary, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(st->loading_spinner, spinner_arc, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(st->loading_spinner, LV_OPA_0, LV_PART_MAIN);

    st->loading_label = lv_label_create(st->loading_container);
    lv_label_set_text(st->loading_label, text);
    lv_obj_set_style_text_color(st->loading_label, theme_manager_get_color("text"), LV_PART_MAIN);
}

/// Take down the loading spinner, deferred: callers run inside queued
/// callbacks, where a synchronous delete corrupts LVGL's event list.
void helix::gcode_viewer::remove_loading_ui(gcode_viewer_state_t* st) {
    if (st->loading_container) {
        st->loading_spinner = nullptr;
        st->loading_label = nullptr;
        helix::ui::safe_delete_deferred(st->loading_container);
    }
}

// ==============================================
// Event Callbacks
// ==============================================

/**
 * @brief Main draw callback - renders G-code using custom renderer
 *
 * Dispatches to either the 3D GLES renderer or the 2D layer renderer
 * based on current render mode and AUTO fallback state.
 */
// Apply the colour priority chain to the 2D renderer: per-tool AMS overrides, then the
// colour the file was sliced for.
//
// Must run every time the 2D renderer is created, not just on load: a render-mode
// switch constructs it lazily, and a renderer given only the file's own palette shows
// the sliced-for colour instead of the loaded filament.
void helix::gcode_viewer::apply_2d_renderer_colors(gcode_viewer_state_t* st) {
    if (!st || !st->layer_renderer_2d_ || !st->gcode_file) {
        return;
    }

    // The file's color answer, classified once: a palette of any size layers
    // per-tool, and the single color acts as the per-segment fallback for tools
    // the palette does not cover.
    const auto file_colors = helix::gcode::classify_file_colors(st->gcode_file->tool_color_palette,
                                                                st->gcode_file->filament_color_hex);

    // Unconditional, including an EMPTY palette. This function is the one place
    // that rebuilds the 2D renderer's colors from the file, so it is also the
    // retraction path (ui_gcode_viewer_clear_tool_colors) and the re-load path.
    // Skipping the call when the file names no palette left whatever was there
    // before - a previous file's palette, or AMS overrides applied over it -
    // still resolving per tool. set_tool_color_palette() no-ops cheaply when
    // there is genuinely nothing to install and nothing to clear.
    st->layer_renderer_2d_->set_tool_color_palette(file_colors.palette);

    const auto& overrides = st->view_options.tool_colors;
    st->applied_2d.tool_colors = overrides;
    if (!overrides.empty()) {
        st->layer_renderer_2d_->set_tool_color_overrides(overrides);
        spdlog::debug("[GCode Viewer] 2D renderer using {} tool color overrides", overrides.size());
    } else if (file_colors.has_single_color()) {
        uint32_t rgb = 0;
        if (helix::parse_hex_color(file_colors.single_color.c_str(), rgb)) {
            st->layer_renderer_2d_->set_extrusion_color(lv_color_hex(rgb));
            spdlog::debug("[GCode Viewer] 2D renderer using filament color: {}",
                          file_colors.single_color);
        } else {
            spdlog::warn("[GCode Viewer] 2D renderer: unusable filament color '{}' - "
                         "keeping current extrusion color",
                         file_colors.single_color);
        }
    }
}

template <typename Renderer>
static void push_selection(const ViewOptions& want, ViewOptions& have, Renderer& renderer) {
    if (want.highlighted != have.highlighted) {
        renderer.set_highlighted_objects(want.highlighted);
        have.highlighted = want.highlighted;
    }
    if (want.excluded != have.excluded) {
        renderer.set_excluded_objects(want.excluded);
        have.excluded = want.excluded;
    }
}

void helix::gcode_viewer::apply_view_options(gcode_viewer_state_t* st) {
    const ViewOptions& want = st->view_options;

#ifdef ENABLE_3D_RENDERER
    if (st->renderer_) {
        auto& renderer = *st->renderer_;
        auto& have = st->applied_3d;
        push_selection(want, have, renderer);
        if (want.tool_colors != have.tool_colors) {
            if (want.tool_colors.empty()) {
                // The overrides were written into the baked palette in place, so
                // only the renderer's own snapshot puts the slicer colors back.
                renderer.clear_tool_color_overrides();
                have.tool_colors.clear();
            } else if (renderer.has_geometry()) {
                renderer.set_tool_color_overrides(want.tool_colors);
                have.tool_colors = want.tool_colors;
            }
            // No geometry yet: install_3d_geometry() applies them to the new mesh.
        }
    }
#endif

    if (st->layer_renderer_2d_) {
        auto& have = st->applied_2d;
        push_selection(want, have, *st->layer_renderer_2d_);
        if (want.tool_colors != have.tool_colors) {
            if (want.tool_colors.empty()) {
                // Empty means retract: rebuild palette-then-fallback from the file,
                // the same order a fresh load takes.
                apply_2d_renderer_colors(st);
            } else {
                st->layer_renderer_2d_->set_tool_color_overrides(want.tool_colors);
            }
            have.tool_colors = want.tool_colors;
        }
    }
}

// Everything a freshly created 2D renderer needs that does not come from the
// data source: the canvas it draws into, the fit shape, and the shading tier.
//
// The renderer defaults SSAO and antialiasing ON, so the decision has to be
// pushed either way: a renderer that skips this pays the SSAO pass, its
// full-canvas buffer, and antialiased rasterization (~6x the aliased cost) on
// exactly the constrained devices the tier exists to spare
// (prestonbrown/helixscreen#1555). auto_fit() runs last because it consumes
// both the canvas size and the framing.
void helix::gcode_viewer::seed_2d_renderer_view(gcode_viewer_state_t* st, int width, int height) {
    auto& renderer = *st->layer_renderer_2d_;
    renderer.set_canvas_size(width, height);
    renderer.set_framing(st->framing_);
    renderer.set_ssao_enabled(st->ssao_enabled_at_init_);
    renderer.set_antialias_enabled(st->antialias_enabled_at_init_);
    renderer.auto_fit();
}

// The one sequence that creates a 2D renderer for a fully parsed file: data
// source, colour chain, canvas, framing, shading tier. Every non-streaming
// creation site calls this, so a site cannot acquire a partial copy of the
// sequence and drift from the rest.
//
// Streaming is the exception and creates its own renderer: its data source is
// the controller rather than a ParsedGCodeFile, and its colours come from the
// index stats, which apply_2d_renderer_colors() cannot read.
static void create_2d_renderer_for_file(gcode_viewer_state_t* st, int width, int height) {
    st->layer_renderer_2d_ = std::make_unique<helix::gcode::GCodeLayerRenderer>();
    st->applied_2d = {};
    st->layer_renderer_2d_->set_gcode(st->gcode_file.get());
    apply_2d_renderer_colors(st);
    apply_view_options(st);
    seed_2d_renderer_view(st, width, height);
    spdlog::debug("[GCode Viewer] Initialized 2D layer renderer ({}x{})", width, height);
}

// Route this file to the 2D renderer because the memory budget refused to build
// its 3D geometry. Sticky for the file, not the session: each load re-asks the
// budget.
void helix::gcode_viewer::apply_budget_forced_2d(gcode_viewer_state_t* st, lv_obj_t* obj) {
    spdlog::info("[GCode Viewer] Using 2D renderer (budget fallback)");
    st->budget_forced_2d_ = true;

    if (st->layer_renderer_2d_) {
        // Already created and seeded for this viewer; the caller has just
        // re-pointed it at the new file and recoloured it, so only the fit is
        // outstanding.
        st->layer_renderer_2d_->auto_fit();
    } else {
        lv_area_t coords;
        lv_obj_get_coords(obj, &coords);
        create_2d_renderer_for_file(st, lv_area_get_width(&coords), lv_area_get_height(&coords));
    }

    lv_obj_invalidate(obj);
}

// Exclude-mode badges, drawn on top of whichever renderer painted this frame.
// Each anchor is projected through the transform of the image actually on
// screen, so the badges follow pan, zoom and rotation and never run ahead of a
// frame that is still refining; ones landing outside the widget are skipped.
// Overlapping badges are drawn in defined order, the later one on top.
static void draw_object_badges(gcode_viewer_state_t* st, lv_layer_t* layer,
                               const lv_area_t& widget_coords) {
    if (st->object_badges.empty()) {
        return;
    }
    // A theme or size-class switch redraws the screen with the same badge list.
    if (!helix::ui::badge_look_current(st->badge_look)) {
        st->badge_look = helix::ui::resolve_badge_look(st->object_badges);
    }

    // The top of what is on screen: badges sit on the current layer while an
    // object is still printing, and on the object's own top once it is done.
    const bool two_d = st->is_using_2d_mode();
    const helix::gcode::GCodeLayerRenderer* r2d = nullptr;
    float drawn_top = 0.0f;
    if (two_d) {
        r2d = st->layer_renderer_2d_.get();
        if (!r2d) {
            return;
        }
        drawn_top = r2d->current_layer_z();
    }
#ifdef ENABLE_3D_RENDERER
    else {
        if (!st->renderer_ || !st->gcode_file || st->gcode_file->layers.empty()) {
            return;
        }
        const auto& layers = st->gcode_file->layers;
        const int last = static_cast<int>(layers.size()) - 1;
        const int top_layer =
            st->print_progress_layer_ >= 0 ? std::min(st->print_progress_layer_, last) : last;
        drawn_top = layers[static_cast<size_t>(top_layer)].z_height;
    }
#else
    else {
        return;
    }
#endif

    const float w = static_cast<float>(lv_area_get_width(&widget_coords));
    const float h = static_cast<float>(lv_area_get_height(&widget_coords));
    for (size_t i = 0; i < st->object_badges.size(); ++i) {
        const auto& badge = st->object_badges[i];
        if (!badge.has_anchor) {
            continue;
        }
        const float z = std::min(badge.top_z.value_or(drawn_top), drawn_top);
        std::optional<glm::vec2> p;
        if (two_d) {
            p = glm::vec2(r2d->project_to_screen(badge.anchor.x, badge.anchor.y, z));
        }
#ifdef ENABLE_3D_RENDERER
        else {
            p = st->renderer_->project_to_shown_image(glm::vec3(badge.anchor, z));
        }
#endif
        if (!p || p->x < 0.0f || p->y < 0.0f || p->x >= w || p->y >= h) {
            continue;
        }
        helix::ui::draw_object_badge(layer, st->badge_look, i, badge,
                                     widget_coords.x1 + static_cast<int32_t>(std::lround(p->x)),
                                     widget_coords.y1 + static_cast<int32_t>(std::lround(p->y)));
        st->drawn_badge_index.push_back(static_cast<int>(i));
        st->drawn_badge_centers.push_back(*p);
    }
}

static void gcode_viewer_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !layer) {
        return;
    }

    // Pick targets describe this frame only; every early return below draws none.
    st->drawn_badge_index.clear();
    st->drawn_badge_centers.clear();

    // Check if rendering is paused (visibility optimization)
    if (st->rendering_paused_) {
        spdlog::trace("[GCode Viewer] draw_cb skipped (rendering paused)");
        return;
    }

    // If no G-code loaded, draw placeholder message
    // In streaming mode, gcode_file is null but streaming_controller_ is set
    bool has_gcode =
        st->gcode_file || (st->streaming_controller_ && st->streaming_controller_->is_open());
    if (st->viewer_state != GcodeViewerState::Loaded || !has_gcode) {
        return;
    }

    // On first render after async load, skip rendering to avoid blocking
    if (st->first_render) {
        spdlog::debug(
            "[GCode Viewer] First draw after async load - skipping render, will render on timer");
        return;
    }

    // Get widget's absolute screen coordinates for drawing
    lv_area_t widget_coords;
    lv_obj_get_coords(obj, &widget_coords);

    // Measure actual render time for FPS calculation
    auto render_start = std::chrono::high_resolution_clock::now();

    // Dispatch to appropriate renderer based on mode
    if (st->is_using_2d_mode()) {
        // 2D layer renderer (orthographic FRONT corner view)
        if (!st->layer_renderer_2d_) {
            // Lazy initialization of 2D renderer (non-streaming mode only)
            // In streaming mode, layer_renderer_2d_ is already initialized in open_file_async
            // callback
            if (!st->gcode_file) {
                spdlog::error(
                    "[GCode Viewer] 2D lazy init but no gcode_file - streaming init failed?");
                return;
            }
            create_2d_renderer_for_file(st, lv_area_get_width(&widget_coords),
                                        lv_area_get_height(&widget_coords));
        }

        // Use stored print progress layer (set via ui_gcode_viewer_set_print_progress)
        // Consistent with 3D renderer:
        //   - >= 0: Show layers 0 to current_layer (print progress mode)
        //   - < 0:  Show all layers (preview mode)
        int current_layer = st->print_progress_layer_;
        if (current_layer < 0) {
            // Preview mode: show all layers
            int max_layer = st->layer_renderer_2d_->get_layer_count() - 1;
            current_layer = std::max(0, max_layer);
        }
        st->layer_renderer_2d_->set_current_layer(current_layer);

        // Re-derive the vertical shift from the live metadata-strip overlap and
        // the fit this renderer settled on. Cheap, and doing it here is what
        // keeps the framing right across relayout without the panel repushing.
        gcode_viewer_refresh_content_offset(st, obj, lv_area_get_width(&widget_coords),
                                            lv_area_get_height(&widget_coords));

        // Render 2D layer view
        st->layer_renderer_2d_->render(layer, &widget_coords);

        // Check if progressive rendering needs more frames
        // This drives ghost cache and solid cache completion
        if (st->layer_renderer_2d_->needs_more_frames()) {
            // IMPORTANT: Cannot call lv_obj_invalidate() during draw callback!
            // LVGL asserts if we invalidate while rendering_in_progress is true.
            // Schedule the widget-safe invalidation for after the render completes.
            helix::ui::queue_invalidate(obj);
        }

        // Update ghost build progress label (streaming mode)
        // IMPORTANT: Cannot create/delete/modify objects during draw callback!
        // Use helix::ui::queue_update() to defer all label operations to after render completes.
        if (st->layer_renderer_2d_->is_ghost_build_running()) {
            int percent =
                static_cast<int>(st->layer_renderer_2d_->get_ghost_build_progress() * 100.0f);
            // Capture needed data for deferred update
            struct GhostProgressUpdate {
                int percent;
            };
            auto update = std::make_unique<GhostProgressUpdate>(GhostProgressUpdate{percent});
            helix::ui::queue_update<GhostProgressUpdate>(
                obj, std::move(update), [](lv_obj_t* viewer, GhostProgressUpdate* u) {
                    auto* state = static_cast<GCodeViewerState*>(lv_obj_get_user_data(viewer));
                    if (!state) {
                        return;
                    }
                    // Create label if needed
                    if (!state->ghost_progress_label_) {
                        state->ghost_progress_label_ = lv_label_create(viewer);
                        lv_obj_set_style_text_color(state->ghost_progress_label_,
                                                    theme_manager_get_color("text_muted"),
                                                    LV_PART_MAIN);
                        lv_obj_set_style_text_font(state->ghost_progress_label_,
                                                   theme_manager_get_font("font_small"),
                                                   LV_PART_MAIN);
                        lv_obj_align(state->ghost_progress_label_, LV_ALIGN_BOTTOM_LEFT, 8, -8);
                    }
                    char text[96];
                    snprintf(text, sizeof(text), lv_tr("Building preview: %d%%"), u->percent);
                    lv_label_set_text(state->ghost_progress_label_, text);
                });
        } else if (st->ghost_progress_label_) {
            // Defer label deletion to after render.
            // IMPORTANT: Do NOT capture the raw lv_obj_t* pointer — if the gcode
            // viewer is destroyed before process_pending() runs, the label is
            // already freed as a child and the captured pointer is dangling.
            // Instead, resolve from state at callback time. (fixes #290)
            helix::ui::queue_widget_update(obj, [](lv_obj_t* viewer) {
                auto* state = get_state(viewer);
                if (!state || !state->ghost_progress_label_)
                    return;
                // Hide immediately, defer deletion to next tick to avoid
                // corrupting LVGL's event list during UpdateQueue batch (crash #356)
                // Use lv_obj_delete_async() — LVGL cancels it automatically if the
                // object is deleted first, unlike custom lv_async_call lambdas.
                lv_obj_add_flag(state->ghost_progress_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_delete_async(state->ghost_progress_label_);
                state->ghost_progress_label_ = nullptr;
            });
        }
    }
#ifdef ENABLE_3D_RENDERER
    else {
        // 3D GLES Renderer (isometric ribbon view)
        if (!st->gcode_file) {
            return; // No ParsedGCodeFile (streaming mode) — 3D renderer needs full geometry
        }
        gcode_viewer_refresh_content_offset(st, obj, lv_area_get_width(&widget_coords),
                                            lv_area_get_height(&widget_coords));
        st->renderer_->render(layer, *st->gcode_file, *st->camera_, &widget_coords);

        // The GPU path is unusable on this device — either GL never came up, or
        // a draw batch returned a fatal error (out-of-memory /
        // invalid-operation) and continuing risks a driver crash. Degrade to the
        // pure-CPU 2D renderer. Reuse the budget_forced_2d_ sticky fallback so
        // subsequent is_using_2d_mode() queries route to the 2D path; it is
        // cleared on the next load, so this covers the current file. We run on
        // the main LVGL thread here (draw callback), so no cross-thread
        // marshaling is needed — same context in which budget_forced_2d_ is
        // normally set.
        if (st->renderer_->render_failed() && !st->budget_forced_2d_) {
            spdlog::warn("[GCode Viewer] GLES renderer unusable ({}) — rendering this file in 2D",
                         st->renderer_->init_failed() ? "GL init failed" : "fatal GL draw error");
            st->budget_forced_2d_ = true;
            // Seed the 2D renderer now so the next frame renders immediately.
            // The lazy init in the 2D branch cannot: a renderer already exists
            // by the time it runs, so its colour chain never fires.
            if (!st->layer_renderer_2d_ && st->gcode_file) {
                create_2d_renderer_for_file(st, lv_area_get_width(&widget_coords),
                                            lv_area_get_height(&widget_coords));
            }
            // Repaint on the next tick now that the mode has flipped. Cannot
            // invalidate synchronously inside the draw callback.
            helix::ui::queue_invalidate(obj);
            return;
        }

        // During chunked VBO upload, renderer returns early without drawing.
        // After the first real GPU render, force one extra frame so the
        // cached-buffer path (no GL context switch) blits cleanly.
        if (st->renderer_->is_uploading() || st->renderer_->is_refining() ||
            st->needs_3d_refresh_) {
            if (!st->renderer_->is_uploading()) {
                st->needs_3d_refresh_ = false;
            }
            helix::ui::queue_invalidate(obj);
        }
    }
#endif

    draw_object_badges(st, layer, widget_coords);

    // Fire the one-shot first-frame callback once the viewer has real content
    // on its canvas (not during VBO upload, not on a skipped/failed frame).
    // Callers (e.g. PrintSelectDetailView) use this to hide the thumbnail they
    // stack on top of the viewer.
    if (!st->first_frame_fired_ && st->first_frame_callback) {
        bool frame_complete = true;
        if (st->is_using_2d_mode()) {
            // The 2D renderer paints progressively, and the ghost copy is the
            // first frame with real content: the solid cache keeps building
            // visibly on top of it afterwards. has_first_output() fires there
            // rather than at full build completion — waiting for the complete
            // build kept the render drawing behind the thumbnail for the whole
            // build window, and slicer thumbnails (Orca's especially) are
            // largely transparent, so the half-built render and its
            // "Building preview: N%" label showed through them at a mismatched
            // scale. Revealing at the ghost copy is also safe for opaque
            // thumbnails: real content is already on the canvas by then. This
            // is every non-GLES device, plus GLES once budget_forced_2d_
            // flips.
            if (st->layer_renderer_2d_ && !st->layer_renderer_2d_->has_first_output()) {
                frame_complete = false;
            }
        } else {
#ifdef ENABLE_3D_RENDERER
            // is_uploading(): VBO upload in progress, no frame on the canvas yet.
            if (st->renderer_ && st->renderer_->is_uploading())
                frame_complete = false;
#endif
        }
        if (frame_complete) {
            st->first_frame_fired_ = true;
            // Defer the callback out of the draw pass. It drives subject writes
            // that hide widgets (bind_flag_if_eq → lv_obj_invalidate +
            // mark_layout_as_dirty), and LVGL rejects invalidation while a
            // render is in progress: lv_refr.c asserts and lv_inv_area returns
            // without marking the area, so stale thumbnail pixels stay painted
            // over the viewer. Resolve state at callback time so a viewer torn
            // down in between (which clears first_frame_callback) is a no-op.
            helix::ui::queue_widget_update(obj, [](lv_obj_t* viewer) {
                auto* state = get_state(viewer);
                if (!state || !state->first_frame_callback) {
                    return;
                }
                state->first_frame_callback(viewer, state->first_frame_callback_user_data, true);
            });
        }
    }

    auto render_end = std::chrono::high_resolution_clock::now();
    auto render_duration_us =
        std::chrono::duration_cast<std::chrono::microseconds>(render_end - render_start).count();

    float render_time_ms = render_duration_us / 1000.0f;

    // Periodic FPS logging (every 30 frames) - use per-widget state to avoid
    // corruption when multiple gcode_viewer widgets exist
    if (render_time_ms > MIN_ACTUAL_RENDER_MS) {
        st->fps_render_time_avg_ms_ = (st->fps_render_time_avg_ms_ == 0.0f)
                                          ? render_time_ms
                                          : (FPS_EMA_ALPHA * render_time_ms +
                                             (1.0f - FPS_EMA_ALPHA) * st->fps_render_time_avg_ms_);
        st->fps_actual_render_count_++;
    }

    if (++st->fps_log_frame_count_ >= FPS_LOG_INTERVAL_FRAMES) {
        if (st->fps_actual_render_count_ > 0 &&
            st->fps_render_time_avg_ms_ > MIN_ACTUAL_RENDER_MS) {
            float avg_fps = 1000.0f / st->fps_render_time_avg_ms_;
            const char* mode_str = st->is_using_2d_mode() ? "2D" : "3D";
            spdlog::debug("[GCode Viewer] {} mode: {:.1f}ms ({:.1f}fps) over {} frames", mode_str,
                          st->fps_render_time_avg_ms_, avg_fps, st->fps_actual_render_count_);
        }
        st->fps_log_frame_count_ = 0;
        st->fps_actual_render_count_ = 0;
    }
}

/**
 * @brief Size changed callback - update camera aspect ratio on resize
 */
static void gcode_viewer_size_changed_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    // Get new widget dimensions
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int width = lv_area_get_width(&coords);
    int height = lv_area_get_height(&coords);

    // Update camera and renderer viewport to match new size
    st->camera_->set_viewport_size(width, height);
#ifdef ENABLE_3D_RENDERER
    st->renderer_->set_viewport_size(width, height);
#endif

    // Also update 2D renderer if initialized
    if (st->layer_renderer_2d_) {
        st->layer_renderer_2d_->set_canvas_size(width, height);
        st->layer_renderer_2d_->auto_fit();
    }

    // Trigger redraw with new aspect ratio
    lv_obj_invalidate(obj);

    spdlog::trace("[GCode Viewer] SIZE_CHANGED: {}x{}, aspect={:.3f}", width, height,
                  (float)width / (float)height);
}

/**
 * @brief Renderer-stall watchdog timer callback.
 *
 * Detects the failure mode where the 2D renderer's progressive cache fails
 * to catch up to the active print's current layer because a continuation
 * lv_obj_invalidate() was dropped (UpdateQueue back-pressure / coalescing
 * with a sync deletion in the same batch — see CLAUDE.md L081).
 *
 * Symptom in user reports: numerical layer text advances correctly, but the
 * 2D render is visually frozen. Navigating away and back doesn't recover
 * because pause/resume only invalidates once and that single frame may not
 * complete the cache either.
 *
 * Self-heal logic: every WATCHDOG_INTERVAL_MS, observe the 2D renderer's
 * cached_up_to_layer_. If the renderer reports needs_more_frames() AND the
 * cached layer has not advanced since the previous tick, force one
 * lv_obj_invalidate(obj). Idempotent — when the renderer is healthy, each
 * tick simply observes a moving cached_up_to_layer_ and does nothing.
 */
/// Forget what the stall watchdog has observed. The cache it compares against
/// restarts on a resume or a new load, so the next tick samples a fresh baseline
/// instead of counting an old stall against new content.
void helix::gcode_viewer::gcode_viewer_watchdog_restart(gcode_viewer_state_t* st) {
    st->watchdog_last_cached_layer_ = -2;
    st->watchdog_last_target_layer_ = -2;
    st->watchdog_stall_streak_ = 0;
}

static void gcode_viewer_watchdog_cb(lv_timer_t* timer) {
    auto* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    if (!obj)
        return;
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    // Pre-conditions for the stall check. None of these indicate a bug — they
    // just mean the watchdog has nothing useful to do this tick.
    if (st->viewer_state != GcodeViewerState::Loaded)
        return;
    if (st->rendering_paused_)
        return;
    if (st->print_progress_layer_ < 0)
        return; // Preview mode — not tracking a print
    if (!st->is_using_2d_mode())
        return; // 3D path has its own continuation chain via needs_3d_refresh_
    if (!st->layer_renderer_2d_)
        return;

    int cached = st->layer_renderer_2d_->get_cached_up_to_layer();
    int target = st->layer_renderer_2d_->get_current_layer();

    // Decide whether to kick (self-heal a dropped invalidate) or give up (the
    // render is wedged on a persistent external failure — e.g. disk full — that
    // re-invalidating can't fix). Direct cached<target (vs. needs_more_frames())
    // avoids the ghost-build false-positive: ghost thread running with solid
    // cache complete is a healthy waiting state, not a stall. See
    // gcode_viewer_watchdog.h; logic is unit-tested in test_gcode_viewer_watchdog.
    const helix::gcode_viewer::WatchdogObservation obs{cached,
                                                       target,
                                                       st->watchdog_last_cached_layer_,
                                                       st->watchdog_last_target_layer_,
                                                       st->watchdog_stall_streak_,
                                                       lv_obj_is_visible(obj)};
    const auto decision =
        helix::gcode_viewer::watchdog_evaluate(obs, gcode_viewer_state_t::WATCHDOG_MAX_STALL_KICKS);
    st->watchdog_stall_streak_ = decision.stall_streak;

    if (decision.kick) {
        st->watchdog_kicks_++;

        // Rate-limit the warn so we don't fill the bundle on a wedged renderer.
        // First kick logs immediately; subsequent kicks log at most every 30s.
        uint32_t now_ms = lv_tick_get();
        constexpr uint32_t KICK_LOG_INTERVAL_MS = 30000;
        bool should_log = (st->watchdog_last_kick_log_ms_ == 0) ||
                          (now_ms - st->watchdog_last_kick_log_ms_ >= KICK_LOG_INTERVAL_MS);
        if (should_log) {
            uint32_t age_ms = st->print_progress_last_change_ms_ == 0
                                  ? 0
                                  : (now_ms - st->print_progress_last_change_ms_);
            spdlog::warn("[GCode Viewer] watchdog: cache stalled (cached={} target={} "
                         "progress_layer={} progress_age_ms={} kicks={}), forcing invalidate",
                         cached, target, st->print_progress_layer_, age_ms, st->watchdog_kicks_);
            st->watchdog_last_kick_log_ms_ = now_ms;
        }

        lv_obj_invalidate(obj);
    } else if (decision.give_up) {
        // Self-heal exhausted: stop thrashing and surface the failure. Mirrors
        // the streaming load-failure path (toast + telemetry + Error state); the
        // early-return at the top of this callback for non-Loaded state means we
        // stop kicking on the next tick.
        spdlog::warn("[GCode Viewer] watchdog: giving up after {} consecutive stalls "
                     "(cached={} target={} progress_layer={}) — surfacing error",
                     decision.stall_streak, cached, target, st->print_progress_layer_);
        st->viewer_state = GcodeViewerState::Error;
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to load G-code preview"));
        TelemetryManager::instance().record_error("gcode_viewer", "render_stall_giveup", "");
        lv_obj_invalidate(obj);
    }

    st->watchdog_last_cached_layer_ = cached;
    st->watchdog_last_target_layer_ = target;
}

/**
 * @brief Cleanup callback - free resources on widget deletion
 */
static void gcode_viewer_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto* state = static_cast<gcode_viewer_state_t*>(lv_obj_get_user_data(obj));
    lv_obj_set_user_data(obj, nullptr);

    // Drain from active-viewers registry (mirror of the push in _create).
    auto& reg = active_viewers();
    reg.erase(std::remove(reg.begin(), reg.end(), obj), reg.end());

    if (state) {
        // The occluder carries an LV_EVENT_DELETE handler that reaches back
        // into this viewer through its user_data. obj_delete_core clears only
        // the deleted object's OWN event list, so that handler survives us and
        // fires later in the same recursion if the occluder is a younger
        // sibling -- which it is in both layouts that set one. Detach it while
        // this object is still allocated.
        //
        // A non-null bottom_occluder_ here proves the occluder has not been
        // freed: its own delete handler is what clears this field, and
        // LV_EVENT_DELETE is sent before lv_free.
        if (state->bottom_occluder_) {
            lv_obj_remove_event_cb_with_user_data(state->bottom_occluder_,
                                                  gcode_viewer_occluder_delete_cb, obj);
            state->bottom_occluder_ = nullptr;
        }

        // Delete timers now while LVGL is guaranteed alive (the destructor's
        // lv_is_initialized() guard might skip this during shutdown)
        if (state->long_press_timer_) {
            lv_timer_delete(state->long_press_timer_);
            state->long_press_timer_ = nullptr;
        }
        if (state->watchdog_timer_) {
            lv_timer_delete(state->watchdog_timer_);
            state->watchdog_timer_ = nullptr;
        }

        // Stop build thread before state destruction
        state->cancel_build();

        spdlog::trace("[GCode Viewer] Widget destroyed");

        // RAII destruction of remaining members
        delete state;
    }
}

// ==============================================
// Public API Implementation
// ==============================================

lv_obj_t* ui_gcode_viewer_create(lv_obj_t* parent) {
    // Create base object
    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj) {
        return nullptr;
    }

    // Set default size (will be overridden by XML attrs or manual sizing)
    // This prevents 0x0 at init time since lv_obj now defaults to content sizing
    lv_obj_set_size(obj, 200, 200);

    // Allocate state (C++ object) using RAII
    auto state_ptr = std::make_unique<gcode_viewer_state_t>();
    if (!state_ptr) {
        helix::ui::safe_delete(obj);
        return nullptr;
    }

    // Get raw pointer for subsequent initialization before transferring ownership
    gcode_viewer_state_t* st = state_ptr.get();
    lv_obj_set_user_data(obj, state_ptr.release());

    // Configure object appearance
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, gcode_viewer_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_size_changed_cb, LV_EVENT_SIZE_CHANGED, nullptr);
    install_input_handlers(obj);
    lv_obj_add_event_cb(obj, gcode_viewer_delete_cb, LV_EVENT_DELETE, nullptr);

    // Register in active-viewers list (drained in gcode_viewer_delete_cb).
    active_viewers().push_back(obj);

    // Initialize viewport size based on current widget dimensions
    // This ensures correct aspect ratio from the start
    lv_obj_update_layout(obj); // Force layout calculation
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int width = lv_area_get_width(&coords);
    int height = lv_area_get_height(&coords);

    if (width > 0 && height > 0) {
        st->camera_->set_viewport_size(width, height);
#ifdef ENABLE_3D_RENDERER
        st->renderer_->set_viewport_size(width, height);
#endif
        spdlog::debug("[GCode Viewer] INIT: viewport={}x{}, aspect={:.3f}", width, height,
                      (float)width / (float)height);
    } else {
        spdlog::error("[GCode Viewer] INIT: Invalid size {}x{}, using defaults", width, height);
    }

    // Renderer-stall watchdog — see gcode_viewer_watchdog_cb for rationale.
    // Always-on timer; the callback gates on viewer_state / paused / progress
    // mode so it's a no-op when there's nothing to watch.
    st->watchdog_timer_ =
        lv_timer_create(gcode_viewer_watchdog_cb, gcode_viewer_state_t::WATCHDOG_INTERVAL_MS, obj);

    spdlog::debug("[GCode Viewer] Widget created");
    return obj;
}

void ui_gcode_viewer_set_load_callback(lv_obj_t* obj, gcode_viewer_load_callback_t callback,
                                       void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return;
    }

    st->load_callback = callback;
    st->load_callback_user_data = user_data;
    spdlog::debug("[GCode Viewer] Load callback registered");
}

void ui_gcode_viewer_set_first_frame_callback(lv_obj_t* obj, gcode_viewer_load_callback_t callback,
                                              void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return;
    }

    st->first_frame_callback = callback;
    st->first_frame_callback_user_data = user_data;
    st->first_frame_fired_ = false;
}

void ui_gcode_viewer_set_thumbnail_parity(lv_obj_t* obj, bool enabled) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return;
    }

    // Renderers pick the mode up from gcode_viewer_refresh_content_offset()
    // (every draw, before render()) and from set_framing() at each creation
    // site, so storing it here is all this setter does.
    st->framing_ =
        enabled ? helix::gcode::FitFraming::THUMBNAIL_PARITY : helix::gcode::FitFraming::STANDARD;
    spdlog::debug("[GCode Viewer] Thumbnail parity {}", enabled ? "on" : "off");
}

void ui_gcode_viewer_clear(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    // Destroy renderer FIRST — its background ghost thread holds a raw pointer to
    // the streaming controller; must join that thread before destroying the controller
    // to prevent use-after-free crashes.
    crash_handler::breadcrumb::note("layer_renderer", "clear_reset_pre");
    st->layer_renderer_2d_.reset();
    crash_handler::breadcrumb::note("layer_renderer", "clear_reset_post");
    // An on-demand 3D build reads the file in place; join it before freeing.
    st->cancel_build();
    // A result queued before the cancel would otherwise pass the generation
    // check and reinstall the file this call is clearing.
    st->bump_generation();
    // Nothing will deliver a result now, so nothing else takes the spinner down.
    remove_loading_ui(st);
    st->gcode_file.reset();
    st->streaming_controller_.reset();
    st->view_options.tool_colors.clear(); // Per-tool AMS colors belong to one load
    st->applied_3d.tool_colors.clear();
    st->viewer_state = GcodeViewerState::Empty;
    helix::telemetry_context::gcode_renderer_loaded.store(false, std::memory_order_relaxed);

    // Release all GPU and CPU geometry resources
#ifdef ENABLE_3D_RENDERER
    if (st->renderer_) {
        st->renderer_->release_geometry();
        st->renderer_->clear_cached_frame();
    }
#endif

    lv_obj_invalidate(obj);
    spdlog::debug("[GCode Viewer] Cleared");

    // Fire owner-installed clear callback (panels use this to flip mode
    // subject back to thumbnail so the user doesn't see a transparent
    // rectangle where the rendered model used to be).
    if (st->clear_callback) {
        st->clear_callback(obj, st->clear_callback_user_data);
    }
}

void ui_gcode_viewer_set_clear_callback(lv_obj_t* obj, ui_gcode_viewer_clear_cb_t cb,
                                        void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return;
    }
    st->clear_callback = cb;
    st->clear_callback_user_data = user_data;
}

void ui_gcode_viewer_clear_all_active() {
    // Copy the list first — ui_gcode_viewer_clear() doesn't mutate it (only
    // _delete_cb does), so a direct iteration would also be safe, but a copy
    // future-proofs against subtle changes to the clear path.
    auto snapshot = active_viewers();
    if (snapshot.empty()) {
        return;
    }
    spdlog::warn("[GCode Viewer] Pressure response: clearing {} active viewer(s)", snapshot.size());
    for (lv_obj_t* obj : snapshot) {
        if (obj && lv_obj_is_valid(obj)) {
            ui_gcode_viewer_clear(obj);
        }
    }
}

bool ui_gcode_viewer_has_content(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    // Loaded is the authoritative state for "holds renderable geometry": it is
    // set on a successful full-file or streaming load and cleared back to Empty
    // by ui_gcode_viewer_clear(). Loading/Error/Empty all mean no content.
    return st && st->viewer_state == GcodeViewerState::Loaded;
}

// ==============================================
// Rendering Pause Control
// ==============================================

void ui_gcode_viewer_set_paused(lv_obj_t* obj, bool paused) {
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

bool ui_gcode_viewer_is_paused(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    return st ? st->rendering_paused_ : true;
}

void ui_gcode_viewer_force_redraw(lv_obj_t* obj) {
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

void ui_gcode_viewer_set_render_mode(lv_obj_t* obj, GcodeViewerRenderMode mode) {
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

bool ui_gcode_viewer_is_using_2d_mode(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    return st ? st->is_using_2d_mode() : false;
}

void ui_gcode_viewer_disable_streaming(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (st) {
        st->streaming_disabled_ = true;
    }
}

// ==============================================
// Camera Controls
// ==============================================

void ui_gcode_viewer_reset_camera(lv_obj_t* obj) {
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

void ui_gcode_viewer_set_highlighted_objects(lv_obj_t* obj,
                                             const std::unordered_set<std::string>& object_names) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->view_options.highlighted = object_names;
    apply_view_options(st);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_excluded_objects(lv_obj_t* obj,
                                          const std::unordered_set<std::string>& object_names) {
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

// NAMESPACE_OK: joins this file's global ui_gcode_viewer_* API
void ui_gcode_viewer_set_object_badges(lv_obj_t* obj, std::vector<helix::ui::ObjectBadge> badges) {
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

// NAMESPACE_OK: joins this file's global ui_gcode_viewer_* API
void ui_gcode_viewer_set_excluded_badges_pickable(lv_obj_t* obj, bool pickable) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return;
    }
    st->excluded_badges_pickable = pickable;
}

void ui_gcode_viewer_set_object_tap_callback(lv_obj_t* obj,
                                             gcode_viewer_object_tap_callback_t callback,
                                             void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->object_tap_callback = callback;
    st->object_tap_user_data = user_data;
}

void ui_gcode_viewer_set_object_long_press_callback(
    lv_obj_t* obj, gcode_viewer_object_long_press_callback_t callback, void* user_data) {
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

void ui_gcode_viewer_set_tool_colors(lv_obj_t* obj, const std::vector<uint32_t>& colors) {
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
    // and it is cleared by ui_gcode_viewer_clear() — so a reload correctly
    // re-applies even when the colors are unchanged.
    if (!st->view_options.tool_colors.empty() && st->view_options.tool_colors == colors) {
        return;
    }

    st->view_options.tool_colors = colors;
    apply_view_options(st);

    lv_obj_invalidate(obj);
    spdlog::debug("[GCode Viewer] Applied {} per-tool AMS color overrides", colors.size());
}

static void ui_gcode_viewer_clear_tool_colors(lv_obj_t* obj) {
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

bool ui_gcode_viewer_apply_ams_tool_colors(lv_obj_t* obj) {
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
    ui_gcode_viewer_set_tool_colors(obj, colors);
    spdlog::debug("[GCode Viewer] Applied {} lane-derived tool colors", colors.size());
    return true;
}

// ==============================================
// Print Progress / Ghost Layer Visualization
// ==============================================

void ui_gcode_viewer_set_print_progress(lv_obj_t* obj, int current_layer) {
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
static void gcode_viewer_occluder_delete_cb(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    gcode_viewer_state_t* st = get_state(obj);
    if (st) {
        st->bottom_occluder_ = nullptr;
    }
}

void ui_gcode_viewer_set_bottom_occluder(lv_obj_t* obj, lv_obj_t* occluder) {
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
    if (!st->bottom_occluder_ || lv_obj_has_flag(st->bottom_occluder_, LV_OBJ_FLAG_HIDDEN)) {
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
static void gcode_viewer_refresh_content_offset(gcode_viewer_state_t* st, lv_obj_t* obj,
                                                int canvas_width, int canvas_height) {
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

int ui_gcode_viewer_get_max_layer(lv_obj_t* obj) {
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
    return -1;
#endif
}

// ==============================================
// Parsed Data Access
// ==============================================

const helix::gcode::ParsedGCodeFile* ui_gcode_viewer_get_parsed_file(lv_obj_t* obj) {
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

std::set<int> ui_gcode_viewer_get_tools_used(lv_obj_t* obj) {
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

bool ui_gcode_viewer_adopt_palette_if_empty(lv_obj_t* obj, std::vector<std::string>& colors) {
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
bool ui_gcode_viewer_get_scheduled_pauses(lv_obj_t* obj,
                                          std::vector<helix::gcode::ScheduledPause>& out_pauses,
                                          helix::gcode::ProgressAxis& out_axis) {
    out_pauses.clear();
    out_axis = helix::gcode::ProgressAxis::BytePosition;
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->has_pause_scan) {
        return false;
    }
    out_pauses = st->scheduled_pauses;
    out_axis = st->scheduled_pauses_axis;
    return true;
}
} // namespace helix

float ui_gcode_viewer_get_load_progress(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->streaming_controller_) {
        // Full-load mode, or nothing loading. Neither has a checkpoint to
        // report; 0.0 is the caller's cue to stay indeterminate.
        return 0.0f;
    }
    return st->streaming_controller_->get_index_progress();
}

bool ui_gcode_viewer_pump_offscreen_2d(lv_obj_t* obj) {
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

const char* ui_gcode_viewer_get_filename(lv_obj_t* obj) {
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

int ui_gcode_viewer_get_layer_count(lv_obj_t* obj) {
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

// ==============================================
// LVGL XML Component Registration
// ==============================================

/**
 * @brief XML create handler for gcode_viewer widget
 */
static void* gcode_viewer_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs; // Required by callback signature, but widget has no XML attributes
    void* parent = lv_xml_state_get_parent(state);
    if (!parent) {
        spdlog::error("[GCode Viewer] XML create: no parent object");
        return nullptr;
    }

    lv_obj_t* obj = ui_gcode_viewer_create((lv_obj_t*)parent);
    if (!obj) {
        spdlog::error("[GCode Viewer] XML create: failed to create widget");
        return nullptr;
    }

    spdlog::trace("[GCode Viewer] XML created widget");
    return (void*)obj;
}

/**
 * @brief XML apply handler for gcode_viewer widget
 * Applies XML attributes to the widget
 */
static void gcode_viewer_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = (lv_obj_t*)item;

    if (!obj) {
        spdlog::error("[GCode Viewer] NULL object in xml_apply");
        return;
    }

    // Apply standard lv_obj properties from XML (size, style, align, name, etc.)
    lv_xml_obj_apply(state, attrs);

    spdlog::trace("[GCode Viewer] Applied XML attributes");
}

/**
 * @brief Register gcode_viewer widget with LVGL XML system
 *
 * Call this during application initialization before loading any XML.
 * Typically called from main() or ui_init().
 */
extern "C" void ui_gcode_viewer_register(void) {
    lv_xml_register_widget("gcode_viewer", gcode_viewer_xml_create, gcode_viewer_xml_apply);
    spdlog::trace("[GCode Viewer] Registered <gcode_viewer> widget with LVGL XML system");
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

#else // !HELIX_HAS_GCODE_VIEWER

// Compiled-out build (HELIX_HAS_GCODE_VIEWER=0): stub widget keeps XML layouts
// parsing (unregistered tags corrupt sibling parenting); API is no-op.

#include "ui_gcode_viewer.h"

#include <spdlog/spdlog.h>

#include <helix-xml/src/xml/lv_xml_parser.h>
#include <helix-xml/src/xml/parsers/lv_xml_obj_parser.h>

static void* gcode_viewer_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs;
    return (void*)lv_obj_create((lv_obj_t*)lv_xml_state_get_parent(state));
}

static void gcode_viewer_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_xml_obj_apply(state, attrs);
}

extern "C" void ui_gcode_viewer_register(void) {
    lv_xml_register_widget("gcode_viewer", gcode_viewer_xml_create, gcode_viewer_xml_apply);
    spdlog::debug("[GCode Viewer] Compiled out (HELIX_HAS_GCODE_VIEWER=0); registered stub "
                  "<gcode_viewer> widget");
}

// ---- C API stubs ----

lv_obj_t* ui_gcode_viewer_create(lv_obj_t* parent) {
    return parent ? lv_obj_create(parent) : nullptr;
}

void ui_gcode_viewer_load_file(lv_obj_t*, const char*) {}

void ui_gcode_viewer_set_load_callback(lv_obj_t*, gcode_viewer_load_callback_t, void*) {}

void ui_gcode_viewer_set_first_frame_callback(lv_obj_t*, gcode_viewer_load_callback_t, void*) {}

void ui_gcode_viewer_set_thumbnail_parity(lv_obj_t*, bool) {}

void ui_gcode_viewer_clear(lv_obj_t*) {}

void ui_gcode_viewer_clear_all_active(void) {}

void ui_gcode_viewer_set_clear_callback(lv_obj_t*, ui_gcode_viewer_clear_cb_t, void*) {}

bool ui_gcode_viewer_has_content(lv_obj_t*) {
    return false;
}

void ui_gcode_viewer_set_paused(lv_obj_t*, bool) {}

bool ui_gcode_viewer_is_paused(lv_obj_t*) {
    return false;
}

void ui_gcode_viewer_force_redraw(lv_obj_t*) {}

void ui_gcode_viewer_set_render_mode(lv_obj_t*, helix::GcodeViewerRenderMode) {}

bool ui_gcode_viewer_is_using_2d_mode(lv_obj_t*) {
    return false;
}

void ui_gcode_viewer_disable_streaming(lv_obj_t*) {}

void ui_gcode_viewer_reset_camera(lv_obj_t*) {}

void ui_gcode_viewer_set_print_progress(lv_obj_t*, int) {}

void ui_gcode_viewer_set_bottom_occluder(lv_obj_t*, lv_obj_t*) {}

int ui_gcode_viewer_get_max_layer(lv_obj_t*) {
    return -1;
}

const char* ui_gcode_viewer_get_filename(lv_obj_t*) {
    return nullptr;
}

int ui_gcode_viewer_get_layer_count(lv_obj_t*) {
    return 0;
}

// ---- C++ API stubs ----

void ui_gcode_viewer_set_tool_colors(lv_obj_t*, const std::vector<uint32_t>&) {}

bool ui_gcode_viewer_apply_ams_tool_colors(lv_obj_t*) {
    return false;
}

void ui_gcode_viewer_set_highlighted_objects(lv_obj_t*, const std::unordered_set<std::string>&) {}

void ui_gcode_viewer_set_excluded_objects(lv_obj_t*, const std::unordered_set<std::string>&) {}

void ui_gcode_viewer_set_object_tap_callback(lv_obj_t*, gcode_viewer_object_tap_callback_t, void*) {
}

// NAMESPACE_OK: joins this file's global ui_gcode_viewer_* API
void ui_gcode_viewer_set_object_badges(lv_obj_t*, std::vector<helix::ui::ObjectBadge>) {}

// NAMESPACE_OK: joins this file's global ui_gcode_viewer_* API
void ui_gcode_viewer_set_excluded_badges_pickable(lv_obj_t*, bool) {}

void ui_gcode_viewer_set_object_long_press_callback(lv_obj_t*,
                                                    gcode_viewer_object_long_press_callback_t,
                                                    void*) {}

const helix::gcode::ParsedGCodeFile* ui_gcode_viewer_get_parsed_file(lv_obj_t*) {
    return nullptr;
}

std::set<int> ui_gcode_viewer_get_tools_used(lv_obj_t*) {
    return {};
}

bool ui_gcode_viewer_adopt_palette_if_empty(lv_obj_t*, std::vector<std::string>&) {
    return false;
}

namespace helix {
bool ui_gcode_viewer_get_scheduled_pauses(lv_obj_t*,
                                          std::vector<helix::gcode::ScheduledPause>& out_pauses,
                                          helix::gcode::ProgressAxis& out_axis) {
    out_pauses.clear();
    out_axis = helix::gcode::ProgressAxis::BytePosition;
    return false;
}
} // namespace helix
bool ui_gcode_viewer_pump_offscreen_2d(lv_obj_t*) {
    return false;
}

float ui_gcode_viewer_get_load_progress(lv_obj_t*) {
    return 0.0f;
}

namespace helix::test_access {
const helix::gcode::GCodeLayerRenderer*
gcode_viewer_budget_force_2d(lv_obj_t*, std::unique_ptr<helix::gcode::ParsedGCodeFile>) {
    return nullptr;
}
GcodeViewerWatchdogTrack gcode_viewer_watchdog_track(lv_obj_t*) {
    return {};
}
void gcode_viewer_set_watchdog_track(lv_obj_t*, const GcodeViewerWatchdogTrack&) {}
} // namespace helix::test_access

#endif // HELIX_HAS_GCODE_VIEWER
