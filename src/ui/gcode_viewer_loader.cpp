// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The gcode viewer's file loading: the streaming and full-load paths, the
// background parse/geometry build, and the on-demand 3D build. One loader thread
// per viewer (GCodeViewerState::start_build); it touches only the state's atomics
// and data it allocated, never LVGL or the renderers. Results return through
// queue_update, guarded by the load generation.

#if HELIX_HAS_GCODE_VIEWER

#include "ui_gcode_viewer.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "color_utils.h"
#include "gcode_color_metadata.h"
#include "gcode_gl_fallback.h"
#include "gcode_parser.h"
#include "gcode_pause_scan.h"
#include "gcode_render_schedule.h"
#include "gcode_streaming_config.h"
#include "gcode_streaming_controller.h"
#include "gcode_viewer_state.h"
#include "geometry_budget_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "print_status_preview_decision.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>

using namespace helix;
using namespace helix::gcode_viewer;

// Lines parsed between cancellation polls in the background load. Small enough
// that cancel_build()'s join returns promptly, large enough that the atomic load
// is noise next to parsing that many lines.
constexpr size_t CANCEL_POLL_LINES = 2048;

#ifdef ENABLE_3D_RENDERER
/// Hand freshly built geometry for st->gcode_file to the 3D renderer. The
/// renderer writes per-tool overrides into a mesh's palette, so the AMS colors
/// the viewer holds have to be written into each new mesh.
static void install_3d_geometry(gcode_viewer_state_t* st,
                                std::unique_ptr<helix::gcode::RibbonGeometry> geometry) {
    st->renderer_->set_prebuilt_geometry(std::move(geometry), st->gcode_file->filename);
    st->applied_3d.tool_colors.clear();
    apply_view_options(st);
}
#endif

/// The 2D renderer's default extrusion color as 0xRRGGBB. Reads the theme, so
/// call it on the main thread and hand the value to the build thread.
static uint32_t default_3d_extrusion_rgb() {
    return lv_color_to_int(helix::gcode::GCodeLayerRenderer::default_extrusion_color());
}

#ifdef ENABLE_3D_RENDERER
/// Segments a banded build keeps: shells are strided by the band depth;
/// surfaces (skins, and the whole first layer so the model keeps its bottom)
/// are kept on every layer when they fit. Counted the way GeometryBuilder
/// selects them, so plan_bands() sizes the band the builder will make.
namespace helix {
struct BandSegmentCounts {
    size_t shell = 0;
    size_t surface = 0;
};
} // namespace helix

static BandSegmentCounts count_band_segments(const helix::gcode::ParsedGCodeFile& file) {
    BandSegmentCounts counts;
    const size_t first_layer = helix::gcode::first_print_layer(file);
    for (size_t li = first_layer; li < file.layers.size(); ++li) {
        for (const auto& seg : file.layers[li].segments) {
            if (!seg.is_extrusion || helix::gcode::is_auxiliary_geometry(seg.feature_type)) {
                continue;
            }
            if (helix::gcode::is_band_shell_feature(seg.feature_type)) {
                ++counts.shell;
            } else if (li == first_layer || helix::gcode::is_surface_feature(seg.feature_type)) {
                ++counts.surface;
            }
        }
    }
    return counts;
}

/// Build a 3D RibbonGeometry from a parsed gcode file using the memory budget
/// system. Returns nullptr if the budget tier forces 2D or the build exceeds
/// the budget. Shared between the initial async-load path and the on-demand
/// path that fires when the user switches to 3D mode after starting in 2D.
///
/// @p default_rgb colors every segment the file's tool palette does not cover.
/// Pass default_3d_extrusion_rgb(), resolved on the main thread.
static std::unique_ptr<helix::gcode::RibbonGeometry>
build_3d_geometry_in_budget(const helix::gcode::ParsedGCodeFile& file, const char* context_tag,
                            uint32_t default_rgb, const std::function<bool()>& should_cancel = {}) {
    helix::gcode::GeometryBudgetManager budget_mgr;
    size_t available_kb = budget_mgr.read_system_available_kb();
    size_t budget = budget_mgr.calculate_budget(available_kb);
    std::string render_driver = budget_mgr.read_render_driver_name();
    size_t max_tris = helix::gcode::gpu_triangle_budget(render_driver.c_str());
    // Size the tier on what the builder will actually build. GeometryBuilder
    // drops auxiliary (purge / prime tower) segments, so estimating from
    // total_segments charged the budget for mass that never becomes geometry
    // and could downgrade tubes - or refuse 3D outright - on a tower-heavy
    // multi-color file.
    auto budget_config = budget_mgr.select_tier(file.drawable_segments, budget, max_tris);

    spdlog::info(
        "[GCode Viewer] {}: {}MB available, {}MB budget, driver '{}' tri cap {}, {} drawable of "
        "{} segments -> tier {}",
        context_tag, available_kb / 1024, budget / (1024 * 1024), render_driver, max_tris,
        file.drawable_segments, file.total_segments, budget_config.tier);

    // A file the triangle cap alone pushed to 2D gets a banded still instead:
    // shells on every n-th layer, n layers tall, skins and the first layer whole.
    helix::gcode::render_schedule::BandPlan still_bands{1, true};
    if (budget_config.tier == 4 && budget_config.triangle_capped) {
        const BandSegmentCounts counts = count_band_segments(file);
        still_bands =
            helix::gcode::render_schedule::plan_still_bands(counts.shell, counts.surface, max_tris);
        if (still_bands.band_layers > 1) {
            budget_config = {.tier = 3,
                             .tube_sides = 4,
                             .simplification_tolerance = 0.01f,
                             .include_travels = false,
                             .budget_bytes = budget_config.budget_bytes,
                             .triangle_capped = true};
            spdlog::info("[GCode Viewer] {}: over the triangle cap, banded still with {}-layer "
                         "bands",
                         context_tag, still_bands.band_layers);
        }
    }

    if (budget_config.tier > 3) {
        spdlog::info("[GCode Viewer] {}: tier {} — skipping 3D geometry build", context_tag,
                     budget_config.tier);
        return nullptr;
    }

    helix::gcode::GeometryBuilder builder;
    // Palette, width and layer height describe the file, so the moving mesh
    // builds with the same values as the main geometry.
    auto configure = [&file, default_rgb](helix::gcode::GeometryBuilder& b) {
        b.set_filament_rgb(default_rgb);
        if (!file.tool_color_palette.empty()) {
            b.set_tool_color_palette(file.tool_color_palette);
        }
        if (file.perimeter_extrusion_width_mm > 0.0f) {
            b.set_extrusion_width(file.perimeter_extrusion_width_mm);
        } else if (file.extrusion_width_mm > 0.0f) {
            b.set_extrusion_width(file.extrusion_width_mm);
        }
        b.set_layer_height(file.layer_height_mm);
    };
    configure(builder);
    builder.set_budget_tube_sides(budget_config.tube_sides);
    builder.set_budget_limit(budget_config.budget_bytes);
    builder.set_band_layers(still_bands.band_layers, still_bands.surfaces_every_layer);

    helix::gcode::SimplificationOptions opts{.tolerance_mm = budget_config.simplification_tolerance,
                                             .min_segment_length_mm = 0.05f,
                                             .max_direction_change_deg =
                                                 budget_config.triangle_capped ? 15.0f
                                                 : budget_config.tier >= 3     ? 45.0f
                                                 : budget_config.tier == 2     ? 30.0f
                                                                               : 15.0f};

    auto geometry =
        std::make_unique<helix::gcode::RibbonGeometry>(builder.build(file, opts, should_cancel));

    if (should_cancel && should_cancel()) {
        spdlog::info("[GCode Viewer] {}: build cancelled - discarding geometry", context_tag);
        return nullptr;
    }
    if (builder.was_budget_exceeded()) {
        spdlog::warn("[GCode Viewer] {}: budget exceeded — falling back to 2D", context_tag);
        return nullptr;
    }

    spdlog::info("[GCode Viewer] {}: built geometry: {} vertices, {} triangles (tier {})",
                 context_tag, geometry->vertices.size(),
                 geometry->extrusion_triangle_count + geometry->travel_triangle_count,
                 budget_config.tier);

    // Moving mesh: what a finger-down frame draws on a GPU too slow for the
    // strided view to read as the model. The gate is the seed-rate budget of
    // the weakest GPU class the app plans for, so a fast desktop builds it
    // too and simply never draws it.
    const size_t moving_budget =
        static_cast<size_t>(helix::gcode::render_schedule::kSeedRateTrisPerMs *
                            helix::gcode::render_schedule::kMovingBudgetMs);
    const size_t main_triangles =
        geometry->extrusion_triangle_count + geometry->travel_triangle_count;
    if (main_triangles > moving_budget) {
        const BandSegmentCounts counts = count_band_segments(file);
        if (counts.shell + counts.surface > 0) {
            const auto bands = helix::gcode::render_schedule::plan_bands(
                counts.shell, counts.surface, moving_budget);
            const int band_layers = bands.band_layers;
            helix::gcode::GeometryBuilder mesh_builder;
            configure(mesh_builder);
            mesh_builder.set_band_layers(band_layers, bands.surfaces_every_layer);
            mesh_builder.set_budget_tube_sides(4);
            helix::gcode::SimplificationOptions mesh_opts{.tolerance_mm = 0.05f,
                                                          .min_segment_length_mm = 0.05f,
                                                          .max_direction_change_deg = 30.0f};
            auto mesh = std::make_unique<helix::gcode::RibbonGeometry>(
                mesh_builder.build(file, mesh_opts, should_cancel));
            const size_t mesh_triangles =
                mesh->extrusion_triangle_count + mesh->travel_triangle_count;
            if (should_cancel && should_cancel()) {
                spdlog::info("[GCode Viewer] {}: moving mesh cancelled", context_tag);
            } else if (mesh->strips.empty() || mesh_triangles == 0) {
                spdlog::info("[GCode Viewer] {}: moving mesh built empty, keeping stride fallback",
                             context_tag);
            } else {
                mesh->prepare_interleaved_buffers();
                geometry->moving_mesh = std::move(mesh);
                spdlog::info("[GCode Viewer] Moving mesh: {}-layer bands, skins on {} layer, {} "
                             "triangles",
                             band_layers, bands.surfaces_every_layer ? "every" : "band",
                             mesh_triangles);
            }
        }
    }

    geometry->prepare_interleaved_buffers();
    return geometry;
}
#endif

// Result structure for async geometry building
struct AsyncBuildResult {
    std::unique_ptr<helix::gcode::ParsedGCodeFile> gcode_file;
#ifdef ENABLE_3D_RENDERER
    std::unique_ptr<helix::gcode::RibbonGeometry> geometry; ///< Full detail geometry
#endif
    /// Scheduled pauses + their axis, from the same parse pass (full-load mode).
    std::vector<helix::gcode::ScheduledPause> scheduled_pauses;
    helix::gcode::ProgressAxis scheduled_pauses_axis{helix::gcode::ProgressAxis::BytePosition};
    std::string error_msg;
    bool success{true};
    bool force_2d = false; ///< Budget system forced 2D fallback
};

/**
 * @brief Asynchronously load and build G-code geometry in background thread
 *
 * Shows loading spinner while parsing and building geometry. Uses background
 * thread to avoid blocking the UI thread. Geometry building is thread-safe
 * (no OpenGL calls, pure CPU work).
 */
static void ui_gcode_viewer_load_file_async(lv_obj_t* obj, const char* file_path) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !file_path) {
        return;
    }

    spdlog::info("[GCode Viewer] Loading file async: {}", file_path);
    st->viewer_state = GcodeViewerState::Loading;
    st->first_render = true;        // Reset for new file
    st->first_frame_fired_ = false; // Reset first-frame callback for new file
    st->budget_forced_2d_ = false;  // Reset budget 2D override for new file
    gcode_viewer_watchdog_restart(st);

    // Bump generation so any in-flight async callbacks from a prior load are rejected
    const uint64_t gen = st->bump_generation();

    // Clear any existing data sources (mutually exclusive: streaming XOR full-file)
    // Destroy renderer FIRST — its background ghost thread holds a raw pointer to
    // the streaming controller; joining that thread before destroying the controller
    // prevents use-after-free crashes.
    crash_handler::breadcrumb::note("layer_renderer", "load_reset_pre");
    st->layer_renderer_2d_.reset();
    crash_handler::breadcrumb::note("layer_renderer", "load_reset_post");
    crash_handler::breadcrumb::note("layer_renderer", "stream_reset_pre");
    st->streaming_controller_.reset();
    crash_handler::breadcrumb::note("layer_renderer", "stream_reset_post");
    crash_handler::breadcrumb::note("layer_renderer", "file_reset_pre");
    // An on-demand 3D build reads the file in place; join it before freeing.
    st->cancel_build();
    st->gcode_file.reset();
    crash_handler::breadcrumb::note("layer_renderer", "file_reset_post");
#ifdef ENABLE_3D_RENDERER
    // The previous file's mesh would otherwise satisfy has_geometry() and
    // stand in for this file on a later switch to 3D.
    if (st->renderer_) {
        st->renderer_->release_geometry();
    }
#endif
    st->scheduled_pauses.clear();
    st->scheduled_pauses_axis = helix::gcode::ProgressAxis::BytePosition;
    st->has_pause_scan = false;

    // =========================================================================
    // PHASE 0: Streaming Mode Detection (Phase 6)
    // Determine whether to use streaming (layer-by-layer) or full-load mode
    // based on file size and available memory.
    // =========================================================================
    auto file_size_or = helix::text_io::file_size(file_path);
    if (!file_size_or) {
        spdlog::warn("[GCode Viewer] Cannot get file size for {}: {}", file_path,
                     std::strerror(errno));
    }
    const auto file_size = file_size_or.value_or(0); // stat failure falls through to full-load

#ifdef ENABLE_3D_RENDERER
    constexpr bool kBuildHas3D = true;
#else
    constexpr bool kBuildHas3D = false;
#endif
    // A screen's opt-out only counts when 3D is actually available to fall back
    // on; see gcode_viewer_should_stream() for the K2 OOM this guards.
    const bool use_streaming = helix::gcode_viewer_should_stream(
        st->streaming_disabled_, kBuildHas3D, helix::should_use_gcode_streaming(file_size));
    spdlog::info("[GCode Viewer] File size: {}KB, streaming mode: {}", file_size / 1024,
                 use_streaming ? "ON" : "OFF");

    // Clean up previous loading UI if it exists — freeze queue to prevent
    // background thread from enqueueing spinner animation callbacks mid-delete
    if (st->loading_container) {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::safe_delete(st->loading_container);
        st->loading_container = nullptr;
        st->loading_spinner = nullptr;
        st->loading_label = nullptr;
    }

    // =========================================================================
    // STREAMING MODE PATH
    // Uses GCodeStreamingController for on-demand layer loading.
    // Ideal for large files on memory-constrained devices.
    // =========================================================================
    if (use_streaming) {
        create_loading_ui(st, obj, lv_tr("Indexing G-code..."));

        // Create streaming controller
        st->streaming_controller_ = std::make_unique<helix::gcode::GCodeStreamingController>();

        // Launch async index building with completion callback
        // The callback can run on a background thread; queue_update marshals it to the UI thread
        std::string path_copy = file_path;
        st->streaming_controller_->open_file_async(path_copy, [obj, path_copy, gen](bool success) {
            // Marshal completion to UI thread
            struct StreamingResult {
                bool success;
                std::string path;
            };
            auto result = std::make_unique<StreamingResult>();
            result->success = success;
            result->path = path_copy;

            helix::ui::queue_update<StreamingResult>(
                obj, std::move(result), [gen](lv_obj_t* obj, StreamingResult* r) {
                    gcode_viewer_state_t* st = get_state(obj);
                    if (!st) {
                        return;
                    }

                    // Reject stale callbacks from superseded loads
                    if (st->load_generation() != gen) {
                        spdlog::debug(
                            "[GCode Viewer] Stale streaming callback (gen {} vs current {}), "
                            "skipping",
                            gen, st->load_generation());
                        return;
                    }

                    // Clean up loading UI — deferred to next frame to avoid deleting
                    // the spinner while its animation timer events may be in-flight
                    remove_loading_ui(st);

                    if (r->success && st->streaming_controller_ &&
                        st->streaming_controller_->is_open()) {
                        spdlog::info("[GCode Viewer] Streaming mode: indexed {} layers",
                                     st->streaming_controller_->get_layer_count());

                        // The layer scan collected this file's scheduled pauses on
                        // the same pass; remember them for the load callback.
                        const auto& stats = st->streaming_controller_->get_index_stats();
                        st->scheduled_pauses = stats.scheduled_pauses;
                        st->scheduled_pauses_axis = stats.has_m73
                                                        ? helix::gcode::ProgressAxis::SlicerTime
                                                        : helix::gcode::ProgressAxis::BytePosition;
                        st->has_pause_scan = true;

                        // Initialize 2D renderer with streaming controller
                        st->layer_renderer_2d_ =
                            std::make_unique<helix::gcode::GCodeLayerRenderer>();
                        st->layer_renderer_2d_->set_streaming_controller(
                            st->streaming_controller_.get());

                        st->applied_2d = {};

                        // The file's color answer, classified once. A per-tool palette goes to
                        // the renderer whole so each tool's segments render in its own color;
                        // collapsing it to a single set_extrusion_color() would paint
                        // everything in palette[initial_tool], which on a dark filament (e.g.
                        // #080A0D, a near-black PLA) looks like a uniformly black model.
                        const auto file_colors = helix::gcode::classify_file_colors(
                            stats.filament_palette, stats.filament_color, stats.initial_tool_index);
                        if (file_colors.has_palette()) {
                            st->layer_renderer_2d_->set_tool_color_palette(file_colors.palette);
                            spdlog::info("[GCode Viewer] Streaming 2D using tool palette "
                                         "(size={}, initial_tool={})",
                                         file_colors.palette.size(), file_colors.initial_tool);
                        } else if (file_colors.has_single_color()) {
                            uint32_t rgb = 0;
                            if (helix::parse_hex_color(file_colors.single_color.c_str(), rgb)) {
                                st->layer_renderer_2d_->set_extrusion_color(lv_color_hex(rgb));
                                spdlog::info("[GCode Viewer] Using filament color from metadata: "
                                             "{} (tool={}, palette={})",
                                             file_colors.single_color, file_colors.initial_tool,
                                             stats.filament_palette.size());
                            } else {
                                spdlog::warn("[GCode Viewer] Unusable filament color '{}' in "
                                             "metadata - keeping current extrusion color",
                                             file_colors.single_color);
                            }
                        }

                        // AMS-known slot colors layer over the metadata palette: they are
                        // typically more accurate than the slicer's, which can lag
                        // firmware-side filament swaps.
                        apply_view_options(st);

                        // Canvas, framing and shading tier — the half of the seed
                        // that does not depend on where the layer data comes from.
                        lv_area_t coords;
                        lv_obj_get_coords(obj, &coords);
                        seed_2d_renderer_view(st, lv_area_get_width(&coords),
                                              lv_area_get_height(&coords));

                        st->viewer_state = GcodeViewerState::Loaded;
                        st->first_render = false;
                        helix::telemetry_context::gcode_renderer_loaded.store(
                            true, std::memory_order_relaxed);

                        // Trigger initial render
                        lv_obj_invalidate(obj);

                        // Invoke load callback
                        if (st->load_callback) {
                            st->load_callback(obj, st->load_callback_user_data, true);
                        }
                    } else {
                        spdlog::error("[GCode Viewer] Streaming mode: failed to index {}", r->path);
                        st->viewer_state = GcodeViewerState::Error;
                        st->streaming_controller_.reset();

                        ToastManager::instance().show(ToastSeverity::ERROR,
                                                      lv_tr("Failed to load G-code preview"));
                        TelemetryManager::instance().record_error("gcode_viewer",
                                                                  "streaming_load_failed", r->path);

                        if (st->load_callback) {
                            st->load_callback(obj, st->load_callback_user_data, false);
                        }
                    }
                });
        });

        return; // Streaming path handles everything asynchronously
    }

    // =========================================================================
    // FULL-LOAD MODE PATH (existing implementation)
    // Parses entire file into memory. Used for smaller files.
    // =========================================================================

    // Create loading UI only when the widget is visible. When the parent hides
    // the viewer (e.g., detail panel uses XML-based loading overlay), creating an
    // LVGL spinner child causes crashes during deletion — the spinner's animation
    // timer events corrupt the event list during safe_delete in the async callback.
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        create_loading_ui(st, obj, lv_tr("Loading G-code..."));
    }

    // Launch worker thread via RAII-managed start_build()
    // Automatically cancels any existing build and joins the thread
    const uint32_t default_rgb = default_3d_extrusion_rgb();
    st->start_build([st, obj, path = std::string(file_path), gen, default_rgb]() {
        auto result = std::make_unique<AsyncBuildResult>();

        try {
            // PHASE 1: Parse G-code file (fast, ~100ms)
            helix::text_io::LineReader file(path);
            if (!file) {
                result->success = false;
                result->error_msg = "Failed to open file: " + path;
            } else {
                helix::gcode::GCodeParser parser;
                std::string line;

                // Poll cancellation while parsing, not just after it.
                // cancel_build() joins this thread FROM THE MAIN THREAD, so with
                // the check only at the end, switching files blocked the LVGL
                // loop for an entire parse — seconds for a multi-megabyte file
                // on a 2-core board. Checked every CANCEL_POLL_LINES lines so the
                // atomic load costs nothing next to the parse itself.
                bool cancelled_mid_parse = false;
                size_t lines_since_cancel_check = 0;

                // Same pass, second collector: scheduled pauses and the M73
                // running state (see gcode_pause_scan.h). Offsets follow
                // getline framing (line length + 1). current_layer() wraps to
                // SIZE_MAX before the first layer; narrowing that to int32_t
                // lands on -1, the scan's prologue sentinel.
                helix::gcode::PauseScan pause_scan;
                pause_scan.begin(static_cast<size_t>(helix::text_io::file_size(path).value_or(0)));
                uint64_t line_offset = 0;

                while (file.next(line)) {
                    parser.parse_line(line);
                    pause_scan.feed_line(line, line_offset,
                                         static_cast<int32_t>(parser.current_layer()));
                    line_offset += line.length() + 1;

                    if (++lines_since_cancel_check >= CANCEL_POLL_LINES) {
                        lines_since_cancel_check = 0;
                        if (st->is_cancelled()) {
                            cancelled_mid_parse = true;
                            break;
                        }
                    }
                }

                result->scheduled_pauses = pause_scan.pauses();
                result->scheduled_pauses_axis = pause_scan.axis();

                if (cancelled_mid_parse) {
                    spdlog::debug("[GCode Viewer] Build cancelled mid-parse, discarding");
                    return;
                }

                result->gcode_file = std::make_unique<helix::gcode::ParsedGCodeFile>(
                    parser.finalize(/*whole_file=*/true));
                result->gcode_file->filename = path;

                spdlog::debug("[GCode Viewer] Parsed {} layers, {} segments",
                              result->gcode_file->layers.size(),
                              result->gcode_file->total_segments);

#ifdef ENABLE_3D_RENDERER
                // PHASE 2: Budget-aware 3D geometry build.
                // Built at load time only when 3D is the active render mode. The 2D path
                // skips this (saves CPU + memory); if the user later switches to 3D the
                // build runs on demand from ui_gcode_viewer_set_render_mode().
                // Segments are deliberately retained even after the build so the 2D
                // renderer can walk them when the user switches back.
                if (!st->is_using_2d_mode()) {
                    result->geometry = build_3d_geometry_in_budget(
                        *result->gcode_file, "Initial load", default_rgb,
                        [st]() { return st->is_cancelled(); });
                    if (!result->geometry) {
                        result->force_2d = true;
                    }
                } else {
                    spdlog::debug("[GCode Viewer] 2D mode - skipping 3D geometry build");
                }
#else
                (void)default_rgb;
                spdlog::debug("[GCode Viewer] 2D renderer - skipping geometry build");
#endif
            }
        } catch (const std::exception& ex) {
            result->success = false;
            result->error_msg = std::string("Exception: ") + ex.what();
        }

        // Check cancellation before dispatching to UI - if cancelled, widget may be destroyed
        if (st->is_cancelled()) {
            spdlog::debug("[GCode Viewer] Build cancelled, discarding result");
            return;
        }

        // PHASE 3: Marshal result back to UI thread (SAFE)
        // Capture generation so the callback can detect if a newer load superseded us
        helix::ui::queue_update<AsyncBuildResult>(
            obj, std::move(result), [gen](lv_obj_t* obj, AsyncBuildResult* r) {
                gcode_viewer_state_t* st = get_state(obj);
                if (!st) {
                    return;
                }

                // Reject stale callbacks from superseded builds — a newer
                // load_file_async() has already set up its own loading UI
                if (st->load_generation() != gen) {
                    spdlog::debug("[GCode Viewer] Stale async callback (gen {} vs current {}), "
                                  "skipping",
                                  gen, st->load_generation());
                    return;
                }

                // Clean up loading UI — deferred to next frame to avoid deleting
                // the spinner while its animation timer events may be in-flight
                remove_loading_ui(st);

                if (r->success) {
                    spdlog::debug("[GCode Viewer] Async callback - setting up geometry");

                    // Store G-code data
                    st->gcode_file = std::move(r->gcode_file);
                    st->scheduled_pauses = std::move(r->scheduled_pauses);
                    st->scheduled_pauses_axis = r->scheduled_pauses_axis;
                    st->has_pause_scan = true;

                    // Update 2D renderer if it exists (prevents dangling pointer).
                    // The whole colour chain runs here, not just the palette:
                    // set_gcode() does not reset colours, so a renderer that
                    // survived a mode flip would otherwise keep the PREVIOUS
                    // file's single color.
                    if (st->layer_renderer_2d_) {
                        st->layer_renderer_2d_->set_gcode(st->gcode_file.get());
                        apply_2d_renderer_colors(st);
                        st->layer_renderer_2d_->auto_fit();
                    }

                    if (r->force_2d) {
                        apply_budget_forced_2d(st, obj);
                    }

                // Set pre-built geometry on renderer
#ifdef ENABLE_3D_RENDERER
                    if (r->geometry) {
                        install_3d_geometry(st, std::move(r->geometry));
                    } else if (!st->is_using_2d_mode()) {
                        // The mode went to 3D after this parse passed its own
                        // build step.
                        start_on_demand_3d_build(st, obj);
                    }
#endif

                    // Fit camera to model bounds
                    st->camera_->fit_to_bounds(st->gcode_file->global_bounding_box);

                    st->viewer_state = GcodeViewerState::Loaded;
                    spdlog::debug("[GCode Viewer] State set to LOADED");

                    // Auto-apply filament color from gcode metadata.
                    // Both renderers, not just the 3D one. The 2D renderer draws on
                    // every build without GLES and on any device the user has put in
                    // 2D mode, and until this it kept the theme default for
                    // color_extrusion_ - the single-color fallback a file without a
                    // parsed tool palette lands on.
                    const auto file_colors = helix::gcode::classify_file_colors(
                        st->gcode_file->tool_color_palette, st->gcode_file->filament_color_hex);
                    if (file_colors.has_single_color()) {
                        uint32_t rgb = 0;
                        if (helix::parse_hex_color(file_colors.single_color.c_str(), rgb)) {
                            const lv_color_t color = lv_color_hex(rgb);
#ifdef ENABLE_3D_RENDERER
                            st->renderer_->set_extrusion_color(color);
#endif
                            if (st->layer_renderer_2d_) {
                                st->layer_renderer_2d_->set_extrusion_color(color);
                            }
                            spdlog::debug("[GCode Viewer] Applied filament color: {}",
                                          file_colors.single_color);
                        } else {
                            spdlog::warn("[GCode Viewer] Unusable filament color '{}' - "
                                         "keeping current extrusion color",
                                         file_colors.single_color);
                        }
                    }

                    // Clear first_render flag to allow actual rendering on next draw
                    st->first_render = false;
                    st->needs_3d_refresh_ = true;

                    // Trigger redraw (will render geometry now that first_render is false)
                    lv_obj_invalidate(obj);

                    spdlog::info("[GCode Viewer] Async load completed successfully");

                    // Invoke load callback if registered
                    if (st->load_callback) {
                        spdlog::debug("[GCode Viewer] Invoking load callback");
                        st->load_callback(obj, st->load_callback_user_data, true);
                    }

                    // Re-invalidate after load callback — the callback may have
                    // changed visibility (e.g. show_gcode_viewer), and the earlier
                    // invalidate (above) would have been ignored while hidden.
                    lv_obj_invalidate(obj);
                } else {
                    spdlog::error("[GCode Viewer] Async load failed: {}", r->error_msg);
                    st->viewer_state = GcodeViewerState::Error;
                    st->gcode_file.reset();

                    // Invoke load callback with error status if registered
                    if (st->load_callback) {
                        spdlog::debug("[GCode Viewer] Invoking load callback (error)");
                        st->load_callback(obj, st->load_callback_user_data, false);
                    }
                }
            });
    });
}

void ui_gcode_viewer_load_file(lv_obj_t* obj, const char* file_path) {
    // Use async version by default
    ui_gcode_viewer_load_file_async(obj, file_path);
}

#ifdef ENABLE_3D_RENDERER
/// Build 3D geometry for the file already loaded, on the viewer's build thread.
/// The result lands through the UpdateQueue: the geometry, or, when the budget
/// refuses, the same per-file 2D fallback a refused initial load takes.
void helix::gcode_viewer::start_on_demand_3d_build(gcode_viewer_state_t* st, lv_obj_t* obj) {
    const uint64_t gen = st->load_generation();
    const helix::gcode::ParsedGCodeFile* file = st->gcode_file.get();
    const uint32_t default_rgb = default_3d_extrusion_rgb();

    struct OnDemandBuild {
        std::unique_ptr<helix::gcode::RibbonGeometry> geometry;
    };

    // The 3D view has no mesh to draw until the result lands.
    if (!st->loading_container && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        create_loading_ui(st, obj, lv_tr("Loading G-code..."));
    }

    // Every path that frees st->gcode_file joins this thread first
    // (cancel_build), so `file` outlives the build.
    st->start_build([st, obj, file, gen, default_rgb]() {
        auto result = std::make_unique<OnDemandBuild>();
        result->geometry = build_3d_geometry_in_budget(*file, "On-demand 3D switch", default_rgb,
                                                       [st]() { return st->is_cancelled(); });
        if (st->is_cancelled()) {
            return;
        }
        helix::ui::queue_update<OnDemandBuild>(
            obj, std::move(result), [gen, file](lv_obj_t* viewer, OnDemandBuild* r) {
                gcode_viewer_state_t* state = get_state(viewer);
                if (!state || state->load_generation() != gen || state->gcode_file.get() != file) {
                    return;
                }
                remove_loading_ui(state);
                if (r->geometry) {
                    install_3d_geometry(state, std::move(r->geometry));
                    if (state->camera_) {
                        state->camera_->fit_to_bounds(file->global_bounding_box);
                    }
                    state->needs_3d_refresh_ = true;
                    lv_obj_invalidate(viewer);
                } else {
                    spdlog::warn("[GCode Viewer] 3D switch refused by memory budget; this file "
                                 "renders in 2D");
                    apply_budget_forced_2d(state, viewer);
                }
            });
    });
}
#endif

#endif // HELIX_HAS_GCODE_VIEWER
