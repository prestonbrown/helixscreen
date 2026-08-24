// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "ui_gcode_viewer.h"

#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_constants.h"
#include "config.h"
#include "gcode_camera.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "gcode_render_mode_policy.h"
#include "gcode_ssao_policy.h"
#include "gcode_streaming_config.h"
#include "gcode_streaming_controller.h"
#include "gcode_viewer_watchdog.h"
#include "geometry_budget_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_utils.h"
#include "system/crash_handler.h"
#include "system/telemetry_manager.h"
#include "theme_manager.h"

#include <filesystem>

#ifdef ENABLE_GLES_3D
#include "gcode_gles_renderer.h"
#define ENABLE_3D_RENDERER
using GCode3DRenderer = helix::gcode::GCodeGLESRenderer;
#else
#include "gcode_renderer.h"
#endif

// FPS tracking constants (for diagnostic logging, not mode selection)
constexpr float MIN_ACTUAL_RENDER_MS = 2.0f;        // Minimum render time to count as actual render
constexpr float FPS_EMA_ALPHA = 0.1f;               // Exponential moving average smoothing factor
constexpr int FPS_LOG_INTERVAL_FRAMES = 30;         // Log FPS every N frames
constexpr float ROTATION_DEGREES_PER_PIXEL = 0.5f;  // Camera rotation sensitivity
constexpr uint32_t DRAG_THROTTLE_MIN_FRAME_MS = 33; // ~30fps throttle during drag
constexpr int CLICK_DISTANCE_THRESHOLD = 10;        // Pixels: distinguish click from drag

// Lines parsed between cancellation polls in the background load. Small enough
// that cancel_build()'s join returns promptly, large enough that the atomic load
// is noise next to parsing that many lines.
constexpr size_t CANCEL_POLL_LINES = 2048;

#include <spdlog/spdlog.h>

#include <helix-xml/src/xml/lv_xml_parser.h>
#include <helix-xml/src/xml/parsers/lv_xml_obj_parser.h>

using namespace helix;

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_set>

/**
 * @brief GCode Viewer widget state with proper RAII thread management
 *
 * Manages the lifecycle of async geometry building threads safely.
 * The destructor signals cancellation and waits for threads to complete,
 * preventing use-after-free crashes during shutdown.
 */
class GCodeViewerState {
  public:
    GCodeViewerState() {
        camera_ = std::make_unique<helix::gcode::GCodeCamera>();
#ifdef ENABLE_3D_RENDERER
        renderer_ = std::make_unique<GCode3DRenderer>();
        spdlog::debug("[GCode Viewer] 3D renderer available");
#else
        renderer_ = std::make_unique<helix::gcode::GCodeRenderer>();
        spdlog::debug("[GCode Viewer] Using LVGL 2D renderer (3D disabled)");
#endif

        // HELIX_GCODE_MODE handling lives in decide_render_mode() (pure, unit
        // tested); this only applies the result and logs why.
#ifdef ENABLE_3D_RENDERER
        constexpr bool HAVE_3D_RENDERER = true;
#else
        constexpr bool HAVE_3D_RENDERER = false;
#endif
        const char* mode_env = std::getenv("HELIX_GCODE_MODE");
        const auto rm = helix::gcode_viewer::decide_render_mode(mode_env, HAVE_3D_RENDERER);
        render_mode_ = rm.mode;
        switch (rm.reason) {
        case helix::gcode_viewer::RenderModeReason::EnvForced3D:
            spdlog::info("[GCode Viewer] HELIX_GCODE_MODE=3D: forcing 3D renderer");
            break;
        case helix::gcode_viewer::RenderModeReason::Env3DUnavailable:
            spdlog::warn("[GCode Viewer] HELIX_GCODE_MODE=3D ignored: 3D renderer not available");
            break;
        case helix::gcode_viewer::RenderModeReason::EnvForced2D:
            spdlog::info("[GCode Viewer] HELIX_GCODE_MODE=2D: using 2D layer renderer");
            break;
        case helix::gcode_viewer::RenderModeReason::EnvUnrecognized:
            spdlog::warn("[GCode Viewer] Unknown HELIX_GCODE_MODE='{}', using 2D", mode_env);
            break;
        case helix::gcode_viewer::RenderModeReason::DefaultAuto:
            // Auto: uses 3D if GLES available, 2D otherwise.
            spdlog::debug("[GCode Viewer] Default render mode: Auto");
            break;
        }

        // Layer 2 backstop: a prior session that hard-faulted inside the GPU
        // driver leaves /display/gpu_3d_blocked set (promoted from the surviving
        // crash-loop guard at startup). Honor it here so the viewer never
        // re-enters the crashing GPU path — an explicit user render-mode pick
        // clears the flag (display_settings_manager) to allow a retry.
        gpu_3d_blocked_ = Config::get_instance()->get<bool>("/display/gpu_3d_blocked", false);
        if (gpu_3d_blocked_) {
            spdlog::warn("[GCode Viewer] GPU 3D path blocked by /display/gpu_3d_blocked (prior "
                         "driver crash) — using 2D renderer");
        }

        // Enhanced shading tiering lives in decide_ssao_enabled() (pure, unit
        // tested); this only applies the result and logs why.
        const auto ssao = helix::gcode_viewer::decide_ssao_enabled(
            helix::get_system_memory_info().is_constrained_device(), std::getenv("HELIX_SSAO"));
        ssao_enabled_at_init_ = ssao.enabled;
        switch (ssao.reason) {
        case helix::gcode_viewer::SsaoReason::ConstrainedOff:
            spdlog::info("[GCode Viewer] Constrained device - enhanced shading off by default "
                         "(HELIX_SSAO=1 to force on)");
            break;
        case helix::gcode_viewer::SsaoReason::EnvForcedOff:
            spdlog::info("[GCode Viewer] HELIX_SSAO=0: enhanced shading disabled");
            break;
        case helix::gcode_viewer::SsaoReason::EnvForcedOn:
            spdlog::info("[GCode Viewer] HELIX_SSAO=1: enhanced shading forced on");
            break;
        case helix::gcode_viewer::SsaoReason::DefaultOn:
            break;
        }
    }

    ~GCodeViewerState() {
        // RAII cleanup: signal cancellation and wait for thread
        cancel_build();

        // Renderer holds a raw pointer to streaming_controller_ and may have a
        // background ghost thread running. Destroy renderer first to join that
        // thread before the controller is freed.
        crash_handler::breadcrumb::note("layer_renderer", "dtor_pre");
        layer_renderer_2d_.reset();
        crash_handler::breadcrumb::note("layer_renderer", "dtor_post");
        streaming_controller_.reset();

        // Clean up LVGL timer if pending
        // Guard against LVGL shutdown - timer may already be destroyed
        if (long_press_timer_ && lv_is_initialized()) {
            lv_timer_delete(long_press_timer_);
            long_press_timer_ = nullptr;
        }
    }

    // Non-copyable, non-movable (prevents accidental thread ownership issues)
    GCodeViewerState(const GCodeViewerState&) = delete;
    GCodeViewerState& operator=(const GCodeViewerState&) = delete;
    GCodeViewerState(GCodeViewerState&&) = delete;
    GCodeViewerState& operator=(GCodeViewerState&&) = delete;

    // ========================================================================
    // Async Build Management
    // ========================================================================

    /**
     * @brief Check if a build operation can be cancelled
     * @return true if cancellation was requested
     */
    bool is_cancelled() const {
        return cancel_flag_.load();
    }

    /**
     * @brief Start an async geometry build operation
     *
     * Cancels any existing build, then launches a new thread.
     *
     * @param build_func Function to execute in background thread
     */
    void start_build(std::function<void()> build_func) {
        // Cancel and wait for any existing build
        cancel_build();

        // Reset state for new build
        cancel_flag_.store(false);
        building_.store(true);

        // Launch new thread. Wrap — pthread_create EAGAIN under thread
        // exhaustion (AD5M/CC1) throws std::system_error which would
        // propagate through PrintStatusPanel::on_activate's event-cb
        // frame and abort via std::terminate ([L083]). This is the exact
        // hot path for L081-family crashes (RPHAV9T7).
        try {
            build_thread_ = std::thread([this, func = std::move(build_func)]() {
                func();
                building_.store(false);
            });
        } catch (const std::system_error& e) {
            spdlog::error("[GcodeViewer] Failed to spawn build thread: {}", e.what());
            building_.store(false);
        }
    }

    /**
     * @brief Cancel any in-progress build and wait for completion
     *
     * Safe to call multiple times. Blocks until thread exits.
     */
    void cancel_build() {
        cancel_flag_.store(true);
        if (build_thread_.joinable()) {
            build_thread_.join();
        }
    }

    bool is_building() const {
        return building_.load();
    }

    // ========================================================================
    // Public State (accessed by static callbacks)
    // ========================================================================

    // G-code data
    std::unique_ptr<helix::gcode::ParsedGCodeFile> gcode_file;
    GcodeViewerState viewer_state{GcodeViewerState::Empty};

    // Rendering components (exposed for callbacks)
    std::unique_ptr<helix::gcode::GCodeCamera> camera_;
#ifdef ENABLE_3D_RENDERER
    std::unique_ptr<GCode3DRenderer> renderer_;
#else
    std::unique_ptr<helix::gcode::GCodeRenderer> renderer_;
#endif

    // Gesture state
    bool is_dragging{false};
    lv_point_t drag_start{0, 0};
    lv_point_t last_drag_pos{0, 0};
    bool gesture_moved{false}; ///< True once movement exceeded threshold anywhere in this touch
#if LV_USE_GESTURE_RECOGNITION
    float last_pinch_scale{0.0f}; ///< Previous cumulative pinch scale (0 = no reference yet)
    bool is_pinching{false};      ///< True during active pinch gesture (suppresses drag rotation)
    bool pinch_occurred{false};   ///< True if a pinch engaged at any point this touch sequence
#endif

    // Selection and exclusion state
    std::unordered_set<std::string> selected_objects;
    std::unordered_set<std::string> excluded_objects;

    // Callbacks
    gcode_viewer_object_tap_callback_t object_tap_callback{nullptr};
    void* object_tap_user_data{nullptr};
    gcode_viewer_object_long_press_callback_t object_long_press_callback{nullptr};
    void* object_long_press_user_data{nullptr};
    gcode_viewer_load_callback_t load_callback{nullptr};
    void* load_callback_user_data{nullptr};
    gcode_viewer_load_callback_t first_frame_callback{nullptr};
    void* first_frame_callback_user_data{nullptr};
    bool first_frame_fired_{false};
    ui_gcode_viewer_clear_cb_t clear_callback{nullptr};
    void* clear_callback_user_data{nullptr};

    // Long-press state
    lv_timer_t* long_press_timer_{nullptr};
    bool long_press_fired{false};
    std::string long_press_object_name;

    // Rendering settings
    bool use_filament_color{true};
    bool has_external_color_override{false};    ///< True when external color (AMS/Spoolman) is set
    lv_color_t external_color_override{};       ///< Stored override color for lazy-init renderers
    std::vector<uint32_t> tool_color_overrides; ///< Per-tool AMS colors for lazy-init renderers
    bool first_render{true};
    bool needs_3d_refresh_{false}; ///< Force one extra frame after first GPU render
    bool rendering_paused_{
        false}; ///< When true, draw_cb skips rendering (for visibility optimization)

    // Loading UI elements (managed by async load function)
    lv_obj_t* loading_container{nullptr};
    lv_obj_t* loading_spinner{nullptr};
    lv_obj_t* loading_label{nullptr};

    // Ghost build progress label (streaming mode only)
    lv_obj_t* ghost_progress_label_{nullptr};

    // ========================================================================
    // Render Mode (Phase 5: 2D Layer View)
    // ========================================================================

    /// 2D orthographic layer renderer (default for all platforms)
    std::unique_ptr<helix::gcode::GCodeLayerRenderer> layer_renderer_2d_;

    /// Streaming controller for large files (Phase 6)
    /// When set, renderer uses this instead of gcode_file for layer data.
    /// Mutually exclusive with gcode_file - exactly one should hold data.
    std::unique_ptr<helix::gcode::GCodeStreamingController> streaming_controller_;

    /// Print progress layer (set via ui_gcode_viewer_set_print_progress)
    /// -1 means "show all layers" (preview mode), >= 0 means "show up to this layer"
    int print_progress_layer_{-1};

    /// Wall-clock ms when print_progress_layer_ last changed. Sampled by the
    /// renderer-stall watchdog to detect "Klipper advanced but cache stalled"
    /// — see watchdog_timer_ below.
    uint32_t print_progress_last_change_ms_{0};

    // ========================================================================
    // Renderer-stall watchdog
    //
    // Self-heals the failure mode where a continuation lv_obj_invalidate from
    // needs_more_frames() (gcode_viewer_draw_cb at LV_EVENT_DRAW_POST) was
    // dropped or coalesced inside UpdateQueue back-pressure (CLAUDE.md L081 —
    // helix::ui::async_call routes through queue_update, no escape from a
    // batch). When that happens, cached_up_to_layer_ < target_layer is stuck
    // even though print_progress_layer_ is advancing on every Moonraker layer
    // event, and the user sees a visually-frozen 2D render despite numeric
    // progress text updating correctly.
    //
    // Tick: WATCHDOG_INTERVAL_MS (default 2s). On each tick, if the 2D
    // renderer reports needs_more_frames() AND its cached_up_to_layer_ has
    // not advanced since the previous tick, force one lv_obj_invalidate(obj).
    // The kick is idempotent: when the renderer is healthy each tick simply
    // observes a moving cached_up_to_layer_ and does nothing.
    // ========================================================================
    static constexpr uint32_t WATCHDOG_INTERVAL_MS = 2000;
    // Consecutive confirmed-stall ticks tolerated before the watchdog gives up
    // and surfaces an error. At WATCHDOG_INTERVAL_MS this is ~60s — generous vs.
    // the 1-2 ticks a real dropped-invalidate needs to recover, but it stops the
    // multi-hour thrash seen when an EXTERNAL failure (disk full) wedges the
    // render permanently (bundle YZQ47HQ6: 4000+ kicks over ~2h).
    static constexpr int WATCHDOG_MAX_STALL_KICKS = 30;
    lv_timer_t* watchdog_timer_{nullptr};
    int watchdog_last_cached_layer_{-2}; ///< -2 sentinel = never sampled
    int watchdog_last_target_layer_{-2}; ///< -2 sentinel = never sampled
    int watchdog_stall_streak_{0};       ///< Consecutive confirmed-stall ticks (resets on progress)
    uint32_t watchdog_kicks_{0};         ///< Diagnostic counter (cumulative)
    uint32_t watchdog_last_kick_log_ms_{0}; ///< Rate-limit kick warns to ~one per print phase

    /// Content offset (stored to apply when 2D renderer is lazily created).
    /// Derived — recomputed each draw from bottom_occluder_ and the active
    /// renderer's fitted content height; never set directly by callers.
    float content_offset_y_percent_{0.0f};

    /// Widget covering the bottom of this viewer (the translucent metadata
    /// strip), or null. Measured live rather than stored as a fraction so the
    /// offset tracks breakpoints, orientation, and the strip growing at runtime.
    /// Cleared by the occluder's own LV_EVENT_DELETE, so it can never dangle.
    lv_obj_t* bottom_occluder_{nullptr};

    /// SSAO enabled at init (from HELIX_SSAO env var, applied when 2D renderer is created)
    bool ssao_enabled_at_init_{false};

    /// Render mode setting - set by constructor based on HELIX_GCODE_MODE env var
    /// Render mode setting - configurable via HELIX_GCODE_MODE env var
    GcodeViewerRenderMode render_mode_{GcodeViewerRenderMode::Layer2D};

    /// Budget system forced 2D for current file (reset on each new load)
    bool budget_forced_2d_{false};

    /// GPU 3D path persistently blocked after a driver crash-loop (issues
    /// #966 / #1084 / #1085). Read once at construction from
    /// /display/gpu_3d_blocked; when set, is_using_2d_mode() always returns 2D.
    bool gpu_3d_blocked_{false};

    /// Disable streaming mode (detail panel uses full-load + budget instead)
    bool streaming_disabled_{false};

    /// Helper to check if currently using 2D layer renderer
    bool is_using_2d_mode() const {
#ifdef ENABLE_3D_RENDERER
        // Streaming mode provides layer data via streaming_controller_, not
        // ParsedGCodeFile. The 3D GLES renderer requires ParsedGCodeFile, so
        // fall back to 2D when streaming is active.
        if (streaming_controller_ && streaming_controller_->is_open()) {
            return true;
        }
        // With GPU-accelerated GLES: Auto defaults to 3D, only Layer2D forces 2D
        return render_mode_ == GcodeViewerRenderMode::Layer2D || budget_forced_2d_ ||
               gpu_3d_blocked_;
#else
        // Without 3D renderer: only explicit Render3D would use 3D (but it's not available)
        return render_mode_ != GcodeViewerRenderMode::Render3D;
#endif
    }

    // Per-widget FPS logging state (avoid static variables that would be shared
    // between multiple gcode_viewer instances)
    int fps_log_frame_count_{0};
    int fps_actual_render_count_{0};
    float fps_render_time_avg_ms_{0.0f};

    /**
     * @brief Generation counter for async callback staleness detection.
     *
     * Incremented each time a new file load begins. Async callbacks capture
     * the generation at dispatch time and compare on arrival — if they don't
     * match, the callback is from an earlier (stale) load and is skipped.
     * This prevents a completed-but-superseded load from deleting widgets
     * that belong to the current load.
     */
    uint64_t load_generation() const {
        return load_generation_.load();
    }

    /// Bump generation counter -- call at the start of each new file load
    uint64_t bump_generation() {
        return load_generation_.fetch_add(1) + 1;
    }

  private:
    std::thread build_thread_;
    std::atomic<bool> building_{false};
    std::atomic<bool> cancel_flag_{false};
    std::atomic<uint64_t> load_generation_{0};
};

// Type alias for compatibility with existing code
using gcode_viewer_state_t = GCodeViewerState;

// Helper: Get widget state from object
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

static gcode_viewer_state_t* get_state(lv_obj_t* obj) {
    return static_cast<gcode_viewer_state_t*>(lv_obj_get_user_data(obj));
}

static void gcode_viewer_refresh_content_offset(gcode_viewer_state_t* st, lv_obj_t* obj,
                                                int canvas_height);

// Helper: Check if viewer has any G-code data (full file or streaming)
static bool has_gcode_data(const gcode_viewer_state_t* st) {
    return st->gcode_file || (st->streaming_controller_ && st->streaming_controller_->is_open());
}

#ifdef ENABLE_3D_RENDERER
/// Build a 3D RibbonGeometry from a parsed gcode file using the memory budget
/// system. Returns nullptr if the budget tier forces 2D or the build exceeds
/// the budget. Shared between the initial async-load path and the on-demand
/// path that fires when the user switches to 3D mode after starting in 2D.
static std::unique_ptr<helix::gcode::RibbonGeometry>
build_3d_geometry_in_budget(const helix::gcode::ParsedGCodeFile& file, const char* context_tag) {
    helix::gcode::GeometryBudgetManager budget_mgr;
    size_t available_kb = budget_mgr.read_system_available_kb();
    size_t budget = budget_mgr.calculate_budget(available_kb);
    auto budget_config = budget_mgr.select_tier(file.total_segments, budget);

    spdlog::info("[GCode Viewer] {}: {}MB available, {}MB budget, {} segments -> tier {}",
                 context_tag, available_kb / 1024, budget / (1024 * 1024), file.total_segments,
                 budget_config.tier);

    if (budget_config.tier > 3) {
        spdlog::info("[GCode Viewer] {}: tier {} — skipping 3D geometry build", context_tag,
                     budget_config.tier);
        return nullptr;
    }

    helix::gcode::GeometryBuilder builder;
    if (!file.tool_color_palette.empty()) {
        builder.set_tool_color_palette(file.tool_color_palette);
    }
    if (file.perimeter_extrusion_width_mm > 0.0f) {
        builder.set_extrusion_width(file.perimeter_extrusion_width_mm);
    } else if (file.extrusion_width_mm > 0.0f) {
        builder.set_extrusion_width(file.extrusion_width_mm);
    }
    builder.set_layer_height(file.layer_height_mm);
    builder.set_budget_tube_sides(budget_config.tube_sides);
    builder.set_budget_limit(budget_config.budget_bytes);

    helix::gcode::SimplificationOptions opts{.tolerance_mm = budget_config.simplification_tolerance,
                                             .min_segment_length_mm = 0.05f,
                                             .max_direction_change_deg =
                                                 budget_config.tier >= 3   ? 45.0f
                                                 : budget_config.tier == 2 ? 30.0f
                                                                           : 15.0f};

    auto geometry = std::make_unique<helix::gcode::RibbonGeometry>(builder.build(file, opts));

    if (builder.was_budget_exceeded()) {
        spdlog::warn("[GCode Viewer] {}: budget exceeded — falling back to 2D", context_tag);
        return nullptr;
    }

    spdlog::info("[GCode Viewer] {}: built geometry: {} vertices, {} triangles (tier {})",
                 context_tag, geometry->vertices.size(),
                 geometry->extrusion_triangle_count + geometry->travel_triangle_count,
                 budget_config.tier);
    geometry->prepare_interleaved_buffers();
    return geometry;
}
#endif

// ==============================================
// Event Callbacks
// ==============================================

/**
 * @brief Main draw callback - renders G-code using custom renderer
 *
 * Dispatches to either the 3D GLES renderer or the 2D layer renderer
 * based on current render mode and AUTO fallback state.
 */
// Apply the colour priority chain to the 2D renderer: per-tool AMS overrides, then a
// single external (AMS/Spoolman) override, then the colour the file was sliced for.
//
// Must run every time the 2D renderer is created, not just on load. Switching render
// mode lazily constructs it and used to apply only the G-code's own tool palette, so
// flipping 3D -> 2D silently reverted the view from the loaded filament colour to the
// sliced-for colour.
static void apply_2d_renderer_colors(gcode_viewer_state_t* st) {
    if (!st || !st->layer_renderer_2d_ || !st->gcode_file) {
        return;
    }

    if (!st->gcode_file->tool_color_palette.empty()) {
        st->layer_renderer_2d_->set_tool_color_palette(st->gcode_file->tool_color_palette);
    }

    if (!st->tool_color_overrides.empty()) {
        st->layer_renderer_2d_->set_tool_color_overrides(st->tool_color_overrides);
        spdlog::debug("[GCode Viewer] 2D renderer using {} tool color overrides",
                      st->tool_color_overrides.size());
    } else if (st->has_external_color_override) {
        st->layer_renderer_2d_->set_extrusion_color(st->external_color_override);
        spdlog::debug("[GCode Viewer] 2D renderer using external color override");
    } else if (st->use_filament_color && st->gcode_file->filament_color_hex.length() >= 2) {
        lv_color_t color = lv_color_hex(static_cast<uint32_t>(
            std::strtol(st->gcode_file->filament_color_hex.c_str() + 1, nullptr, 16)));
        st->layer_renderer_2d_->set_extrusion_color(color);
        spdlog::debug("[GCode Viewer] 2D renderer using filament color: {}",
                      st->gcode_file->filament_color_hex);
    }
}

static void gcode_viewer_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !layer) {
        return;
    }

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
        // 2D Layer Renderer (orthographic top-down view)
        if (!st->layer_renderer_2d_) {
            // Lazy initialization of 2D renderer (non-streaming mode only)
            // In streaming mode, layer_renderer_2d_ is already initialized in open_file_async
            // callback
            if (!st->gcode_file) {
                spdlog::error(
                    "[GCode Viewer] 2D lazy init but no gcode_file - streaming init failed?");
                return;
            }
            st->layer_renderer_2d_ = std::make_unique<helix::gcode::GCodeLayerRenderer>();
            st->layer_renderer_2d_->set_gcode(st->gcode_file.get());
            int width = lv_area_get_width(&widget_coords);
            int height = lv_area_get_height(&widget_coords);
            st->layer_renderer_2d_->set_canvas_size(width, height);
            st->layer_renderer_2d_->auto_fit();

            apply_2d_renderer_colors(st);

            // Apply SSAO setting from env var or prior API call
            if (st->ssao_enabled_at_init_) {
                st->layer_renderer_2d_->set_ssao_enabled(true);
            }

            spdlog::debug("[GCode Viewer] Initialized 2D layer renderer ({}x{})", width, height);
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
        gcode_viewer_refresh_content_offset(st, obj, lv_area_get_height(&widget_coords));

        // Render 2D layer view
        st->layer_renderer_2d_->render(layer, &widget_coords);

        // Check if progressive rendering needs more frames
        // This drives ghost cache and solid cache completion
        if (st->layer_renderer_2d_->needs_more_frames()) {
            // IMPORTANT: Cannot call lv_obj_invalidate() during draw callback!
            // LVGL asserts if we invalidate while rendering_in_progress is true.
            // Use widget-safe async_call to schedule invalidation after render completes.
            helix::ui::async_call(
                obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
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
                    static char text[32];
                    lv_snprintf(text, sizeof(text), "Building preview: %d%%", u->percent);
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
    } else {
        // 3D GLES Renderer (isometric ribbon view)
        if (!st->gcode_file) {
            return; // No ParsedGCodeFile (streaming mode) — 3D renderer needs full geometry
        }
        gcode_viewer_refresh_content_offset(st, obj, lv_area_get_height(&widget_coords));
        st->renderer_->render(layer, *st->gcode_file, *st->camera_, &widget_coords);

#ifdef ENABLE_3D_RENDERER
        // A fatal GL draw error (out-of-memory / invalid-operation) means the
        // GPU path is unsafe on this device — degrade to the pure-CPU 2D
        // renderer for the rest of the session rather than risk a driver crash.
        // Reuse the existing budget_forced_2d_ sticky fallback so subsequent
        // is_using_2d_mode() queries route to the 2D path. We run on the main
        // LVGL thread here (draw callback), so no cross-thread marshaling is
        // needed — same context in which budget_forced_2d_ is normally set.
        if (st->renderer_->render_failed() && !st->budget_forced_2d_) {
            spdlog::warn("[GCode Viewer] GLES renderer reported a fatal GL error — switching to "
                         "2D for this session");
            st->budget_forced_2d_ = true;
            // Seed the 2D renderer now so the next frame renders immediately.
            // (Lazy init in the 2D branch also covers this, but doing it here
            // keeps colors/palette consistent with the loaded file.)
            if (!st->layer_renderer_2d_ && st->gcode_file) {
                st->layer_renderer_2d_ = std::make_unique<helix::gcode::GCodeLayerRenderer>();
                st->layer_renderer_2d_->set_gcode(st->gcode_file.get());
                if (!st->gcode_file->tool_color_palette.empty()) {
                    st->layer_renderer_2d_->set_tool_color_palette(
                        st->gcode_file->tool_color_palette);
                }
                st->layer_renderer_2d_->auto_fit();
            }
            // Repaint on the next tick now that the mode has flipped. Cannot
            // invalidate synchronously inside the draw callback.
            helix::ui::async_call(
                obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
            return;
        }

        // During chunked VBO upload, renderer returns early without drawing.
        // After the first real GPU render, force one extra frame so the
        // cached-buffer path (no GL context switch) blits cleanly.
        if (st->renderer_->is_uploading() || st->needs_3d_refresh_) {
            if (!st->renderer_->is_uploading()) {
                st->needs_3d_refresh_ = false;
            }
            helix::ui::async_call(
                obj, [](void* data) { lv_obj_invalidate(static_cast<lv_obj_t*>(data)); }, obj);
        }
#endif
    }

    // Fire the one-shot first-frame callback once the viewer has produced real
    // pixels (not during VBO upload, not on a skipped/failed frame). Callers
    // (e.g. PrintSelectDetailView) use this to defer hiding the thumbnail until
    // the viewer actually has something to show, avoiding a gray flash.
    if (!st->first_frame_fired_ && st->first_frame_callback) {
        bool frame_complete = true;
        if (st->is_using_2d_mode()) {
            // The 2D renderer paints progressively — the ghost and solid caches
            // finish over several frames, which is exactly when the "Building
            // preview: N%" label is up. Reporting completion here drops the
            // thumbnail onto a half-drawn view, the gray gap this callback
            // exists to prevent. This is every non-GLES device, plus GLES once
            // budget_forced_2d_ flips.
            if (st->layer_renderer_2d_ && (st->layer_renderer_2d_->needs_more_frames() ||
                                           st->layer_renderer_2d_->is_ghost_build_running())) {
                frame_complete = false;
            }
        } else {
#ifdef ENABLE_3D_RENDERER
            // is_uploading() (VBO upload in progress) exists only on GCode3DRenderer;
            // the non-GLES base GCodeRenderer has no such concept.
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

// Long-press threshold in milliseconds. Deliberately longer than the app-wide
// gesture timeout (AppConstants::Input::LONG_PRESS_MS, 500ms): a hold here fires
// EXCLUDE_OBJECT and cancels printing the object under the finger, so the gesture
// is tuned to demand a deliberate hold and resist accidental cancels.
constexpr uint32_t LONG_PRESS_THRESHOLD_MS = 1000;

/**
 * @brief Timer callback for long-press detection
 *
 * Fires after LONG_PRESS_THRESHOLD_MS if user hasn't moved the finger.
 * Picks the object under the initial press position and invokes the long-press callback.
 */
static void long_press_timer_cb(lv_timer_t* timer) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !has_gcode_data(st))
        return;

    // Timer fired - this is a long-press
    st->long_press_fired = true;

    // Delete the timer (one-shot)
    lv_timer_delete(timer);
    st->long_press_timer_ = nullptr;

    // Pick object at the original press position
    const char* picked = ui_gcode_viewer_pick_object(obj, st->drag_start.x, st->drag_start.y);

    if (picked && picked[0] != '\0') {
        st->long_press_object_name = picked;

        // Highlight the object to provide visual feedback
        st->selected_objects.clear();
        st->selected_objects.insert(picked);
        ui_gcode_viewer_set_highlighted_objects(obj, st->selected_objects);

        spdlog::info("[GCode Viewer] Long-press on object '{}'", picked);

        // Invoke long-press callback
        if (st->object_long_press_callback) {
            st->object_long_press_callback(obj, picked, st->object_long_press_user_data);
        }
    } else {
        st->long_press_object_name.clear();
        spdlog::debug("[GCode Viewer] Long-press at ({}, {}) - no object found", st->drag_start.x,
                      st->drag_start.y);

        // Invoke callback with empty string to indicate long-press on empty space
        if (st->object_long_press_callback) {
            st->object_long_press_callback(obj, "", st->object_long_press_user_data);
        }
    }
}

/**
 * @brief Touch press callback - start drag gesture and long-press timer
 */
static void gcode_viewer_press_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Fresh interaction (all fingers were up): clear per-sequence gesture state.
    // The pinch latch is otherwise cleared only by the recognizer's ENDED/CANCELED
    // event, which is not reliably delivered when both fingers lift in one input
    // poll (finger_cnt 2->0). Resetting here guarantees a stuck latch can't
    // permanently suppress single-finger rotation.
    if (!st->is_dragging) {
        st->gesture_moved = false;
#if LV_USE_GESTURE_RECOGNITION
        st->is_pinching = false;
        st->last_pinch_scale = 0.0f;
        st->pinch_occurred = false;
#endif
    }

    st->is_dragging = true;
    st->drag_start = point;
    st->last_drag_pos = point;
    st->long_press_fired = false;
    st->long_press_object_name.clear();

    spdlog::trace("[GCode Viewer] PRESSED at ({}, {}), is_dragging={}", point.x, point.y,
                  st->is_dragging);

    // Enter interaction mode for reduced resolution during drag
    if (st->renderer_) {
        st->renderer_->set_interaction_mode(true);
    }

    // Start long-press timer if callback is registered
    if (st->object_long_press_callback && has_gcode_data(st)) {
        // Cancel any existing timer
        if (st->long_press_timer_) {
            lv_timer_delete(st->long_press_timer_);
            st->long_press_timer_ = nullptr;
        }
        // Start new timer for long-press detection
        st->long_press_timer_ = lv_timer_create(long_press_timer_cb, LONG_PRESS_THRESHOLD_MS, obj);
        if (st->long_press_timer_) {
            lv_timer_set_repeat_count(st->long_press_timer_, 1); // One-shot timer
        }
    }

    spdlog::trace("[GCode Viewer] Press at ({}, {})", point.x, point.y);
}

// Movement threshold to cancel long-press (same as click threshold)
constexpr int LONG_PRESS_MOVE_THRESHOLD = 10;

/**
 * @brief Touch pressing callback - handle drag for camera rotation
 *
 * Also cancels long-press timer if user moves beyond threshold.
 */
static void gcode_viewer_pressing_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !st->is_dragging)
        return;

    // In 2D mode, no camera rotation - skip drag handling entirely.
    // This also prevents mouse micro-jitter from cancelling the long-press timer.
    if (st->is_using_2d_mode())
        return;

#if LV_USE_GESTURE_RECOGNITION
    // Suppress drag rotation during pinch-to-zoom to prevent fighting
    if (st->is_pinching) {
        spdlog::debug("[GCode Viewer] PRESSING suppressed (pinching)");
        return;
    }
#endif

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Check if movement exceeds threshold - cancel long-press timer
    int total_dx = abs(point.x - st->drag_start.x);
    int total_dy = abs(point.y - st->drag_start.y);

    // Latch "moved" once movement exceeds the tap threshold anywhere in the
    // sequence. Unlike the net displacement checked at release, this catches
    // rotate-and-return motions that settle back near the start point and must
    // NOT be treated as an object tap.
    if (total_dx >= CLICK_DISTANCE_THRESHOLD || total_dy >= CLICK_DISTANCE_THRESHOLD) {
        st->gesture_moved = true;
    }

    if ((total_dx >= LONG_PRESS_MOVE_THRESHOLD || total_dy >= LONG_PRESS_MOVE_THRESHOLD) &&
        st->long_press_timer_) {
        // User started dragging - cancel long-press
        lv_timer_delete(st->long_press_timer_);
        st->long_press_timer_ = nullptr;
        spdlog::trace("[GCode Viewer] Long-press cancelled due to movement");
    }

    // Calculate delta from last position
    int dx = point.x - st->last_drag_pos.x;
    int dy = point.y - st->last_drag_pos.y;

    if (dx != 0 || dy != 0) {
        // Convert pixel movement to rotation angles (~0.5 degrees per pixel)
        // Azimuth: drag right = orbit right
        // Elevation: drag up = tilt up (screen Y is inverted, so positive dy = down)
        float delta_azimuth = dx * ROTATION_DEGREES_PER_PIXEL;
        float delta_elevation = dy * ROTATION_DEGREES_PER_PIXEL;

        st->camera_->rotate(delta_azimuth, delta_elevation);

        // Throttled invalidation - limit to ~30fps during drag to reduce CPU load
        // Final frame is always rendered on RELEASED event
        static uint32_t last_invalidate_ms = 0;
        uint32_t now_ms = lv_tick_get();
        if (now_ms - last_invalidate_ms >= DRAG_THROTTLE_MIN_FRAME_MS) {
            lv_obj_invalidate(obj);
            last_invalidate_ms = now_ms;
        }

        st->last_drag_pos = point;

        spdlog::trace("[GCode Viewer] Drag ({}, {}) -> rotate({:.1f}, {:.1f})", dx, dy,
                      delta_azimuth, delta_elevation);
    }
}

/**
 * @brief Touch release callback - handle click vs drag gesture
 *
 * Skips tap handling if long-press already fired (user held for 500ms+).
 */
static void gcode_viewer_release_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    // Cancel long-press timer if still pending
    if (st->long_press_timer_) {
        lv_timer_delete(st->long_press_timer_);
        st->long_press_timer_ = nullptr;
    }

    // Get release position
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        st->is_dragging = false;
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Calculate total drag distance from initial press
    int dx = abs(point.x - st->drag_start.x);
    int dy = abs(point.y - st->drag_start.y);

    const int CLICK_THRESHOLD = CLICK_DISTANCE_THRESHOLD;

    // Skip tap handling if long-press already fired
    if (st->long_press_fired) {
        spdlog::trace("[GCode Viewer] Release after long-press - skipping tap handling");
        st->is_dragging = false;
        st->long_press_fired = false;
        return;
    }

#if LV_USE_GESTURE_RECOGNITION
    // Skip tap handling if a pinch occurred at any point this sequence - not just
    // if one is still active. A pinch that ends as a single-finger lift would
    // otherwise land as a low-movement release and be misread as an object tap.
    if (st->is_pinching || st->pinch_occurred) {
        spdlog::trace("[GCode Viewer] Release after pinch - skipping tap handling");
        st->is_dragging = false;
        return;
    }
#endif

    // If movement was minimal, treat as click and try to pick object.
    // gesture_moved guards against rotate-and-return motions whose net
    // displacement is small but which clearly manipulated the view.
    if (!st->gesture_moved && dx < CLICK_THRESHOLD && dy < CLICK_THRESHOLD && has_gcode_data(st)) {
        spdlog::debug("[GCode Viewer] Click detected at ({}, {})", point.x, point.y);
        const char* picked = ui_gcode_viewer_pick_object(obj, point.x, point.y);

        if (picked && picked[0] != '\0') {
            // Object clicked - toggle selection (single-select)
            std::string picked_name(picked);

            if (st->selected_objects.count(picked_name) > 0) {
                // Already selected - deselect
                st->selected_objects.clear();
                spdlog::info("[GCode Viewer] Deselected object '{}'", picked_name);
            } else {
                // Select this object (replacing any previous selection)
                st->selected_objects.clear();
                st->selected_objects.insert(picked_name);
                spdlog::info("[GCode Viewer] Selected object '{}'", picked_name);
            }

            // Update highlighting to show all selected objects
            ui_gcode_viewer_set_highlighted_objects(obj, st->selected_objects);

            // Invoke tap callback if registered (for exclude object UI)
            if (st->object_tap_callback) {
                st->object_tap_callback(obj, picked, st->object_tap_user_data);
            }
        } else {
            spdlog::debug("[GCode Viewer] Click at ({}, {}) - no object found (G-code may lack "
                          "EXCLUDE_OBJECT metadata)",
                          point.x, point.y);
            // Still invoke callback with empty string to indicate click on empty space
            if (st->object_tap_callback) {
                st->object_tap_callback(obj, "", st->object_tap_user_data);
            }
        }
        // Note: If no object picked, keep current selection (per user requirements)
    }

    st->is_dragging = false;

    // Exit interaction mode to restore full resolution for final frame
    if (st->renderer_) {
        st->renderer_->set_interaction_mode(false);
    }

    // Always render final frame on release to ensure camera settles at correct position
    // (throttling during drag may have skipped the last frame)
    lv_obj_invalidate(obj);

    spdlog::trace("[GCode Viewer] Release at ({}, {}), drag=({}, {})", point.x, point.y, dx, dy);
}

#if LV_USE_GESTURE_RECOGNITION
/**
 * @brief Gesture callback - handle pinch-to-zoom (3D mode only)
 *
 * ROTATE is disabled at the input-device level (threshold set to ~180°)
 * so PINCH always wins the recognizer race.  We compute a per-frame
 * delta from the cumulative scale to drive smooth, incremental zoom.
 */
static void gcode_viewer_gesture_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || st->is_using_2d_mode())
        return;

    if (lv_event_get_gesture_type(e) != LV_INDEV_GESTURE_PINCH)
        return;

    auto state = lv_event_get_gesture_state(e, LV_INDEV_GESTURE_PINCH);

    if (state == LV_INDEV_GESTURE_STATE_ONGOING || state == LV_INDEV_GESTURE_STATE_RECOGNIZED) {
        st->is_pinching = true;
        st->pinch_occurred = true; // sticky for this touch sequence; gates tap at release
    }

    if (state == LV_INDEV_GESTURE_STATE_RECOGNIZED) {
        float scale = lv_event_get_pinch_scale(e);
        if (scale > 0.0f && st->last_pinch_scale > 0.0f) {
            float delta = scale / st->last_pinch_scale;
            // Normal per-frame deltas are 0.85–1.15. Anything outside
            // that range is a gesture restart (cumulative scale reset).
            if (delta > 0.7f && delta < 1.4f) {
                st->camera_->zoom(delta);
                lv_obj_invalidate(obj);
            } else {
                spdlog::debug("[GCode Viewer] Pinch delta filtered: {:.4f}", delta);
            }
        }
        if (scale > 0.0f)
            st->last_pinch_scale = scale;
    } else if (state == LV_INDEV_GESTURE_STATE_ENDED || state == LV_INDEV_GESTURE_STATE_CANCELED) {
        spdlog::trace("[GCode Viewer] Pinch gesture ended (zoom={:.2f})",
                      st->camera_->get_zoom_level());
        st->last_pinch_scale = 0.0f;
        st->is_pinching = false;
    }
}
#endif

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
    st->renderer_->set_viewport_size(width, height);

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
    const helix::gcode_viewer::WatchdogObservation obs{
        cached, target, st->watchdog_last_cached_layer_, st->watchdog_last_target_layer_,
        st->watchdog_stall_streak_};
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
    lv_obj_add_event_cb(obj, gcode_viewer_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_pressing_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_release_cb, LV_EVENT_RELEASED, nullptr);
#if LV_USE_GESTURE_RECOGNITION
    lv_obj_add_event_cb(obj, gcode_viewer_gesture_cb, LV_EVENT_GESTURE, nullptr);
#endif
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
        st->renderer_->set_viewport_size(width, height);
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

// Result structure for async geometry building
struct AsyncBuildResult {
    std::unique_ptr<helix::gcode::ParsedGCodeFile> gcode_file;
#ifdef ENABLE_3D_RENDERER
    std::unique_ptr<helix::gcode::RibbonGeometry> geometry; ///< Full detail geometry
#endif
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

    // Bump generation so any in-flight async callbacks from a prior load are rejected
    const uint64_t gen = st->bump_generation();

    // Clear any existing data sources (mutually exclusive: streaming XOR full-file)
    // Destroy renderer FIRST — its background ghost thread holds a raw pointer to
    // the streaming controller; joining that thread before destroying the controller
    // prevents use-after-free crashes (prestonbrown/helixscreen#XXX).
    crash_handler::breadcrumb::note("layer_renderer", "load_reset_pre");
    st->layer_renderer_2d_.reset();
    crash_handler::breadcrumb::note("layer_renderer", "load_reset_post");
    crash_handler::breadcrumb::note("layer_renderer", "stream_reset_pre");
    st->streaming_controller_.reset();
    crash_handler::breadcrumb::note("layer_renderer", "stream_reset_post");
    crash_handler::breadcrumb::note("layer_renderer", "file_reset_pre");
    st->gcode_file.reset();
    crash_handler::breadcrumb::note("layer_renderer", "file_reset_post");

    // =========================================================================
    // PHASE 0: Streaming Mode Detection (Phase 6)
    // Determine whether to use streaming (layer-by-layer) or full-load mode
    // based on file size and available memory.
    // =========================================================================
    std::error_code ec;
    auto file_size = std::filesystem::file_size(file_path, ec);
    if (ec) {
        spdlog::warn("[GCode Viewer] Cannot get file size for {}: {}", file_path, ec.message());
        file_size = 0; // Fall through to full-load mode
    }

    bool use_streaming = !st->streaming_disabled_ && helix::should_use_gcode_streaming(file_size);
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
        // Create loading UI
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
        lv_label_set_text(st->loading_label, lv_tr("Indexing G-code..."));
        lv_obj_set_style_text_color(st->loading_label, theme_manager_get_color("text"),
                                    LV_PART_MAIN);

        // Create streaming controller
        st->streaming_controller_ = std::make_unique<helix::gcode::GCodeStreamingController>();

        // Launch async index building with completion callback
        // The callback runs on the background thread, so we use lv_async_call to marshal to UI
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

            helix::ui::queue_update<
                StreamingResult>(obj, std::move(result), [gen](lv_obj_t* obj, StreamingResult* r) {
                gcode_viewer_state_t* st = get_state(obj);
                if (!st) {
                    return;
                }

                // Reject stale callbacks from superseded loads
                if (st->load_generation() != gen) {
                    spdlog::debug("[GCode Viewer] Stale streaming callback (gen {} vs current {}), "
                                  "skipping",
                                  gen, st->load_generation());
                    return;
                }

                // Clean up loading UI — deferred to next frame to avoid deleting
                // the spinner while its animation timer events may be in-flight
                if (st->loading_container) {
                    st->loading_spinner = nullptr;
                    st->loading_label = nullptr;
                    helix::ui::safe_delete_deferred(st->loading_container);
                }

                if (r->success && st->streaming_controller_ &&
                    st->streaming_controller_->is_open()) {
                    spdlog::info("[GCode Viewer] Streaming mode: indexed {} layers",
                                 st->streaming_controller_->get_layer_count());

                    // Initialize 2D renderer with streaming controller
                    st->layer_renderer_2d_ = std::make_unique<helix::gcode::GCodeLayerRenderer>();
                    st->layer_renderer_2d_->set_streaming_controller(
                        st->streaming_controller_.get());

                    // Apply color: external override (AMS/Spoolman) takes priority
                    if (st->has_external_color_override) {
                        st->layer_renderer_2d_->set_extrusion_color(st->external_color_override);
                        spdlog::info("[GCode Viewer] Streaming 2D using external color override");
                    } else {
                        const auto& stats = st->streaming_controller_->get_index_stats();
                        // Multi-color metadata: hand the full per-tool palette to the renderer so
                        // each tool's segments render in its own color. Collapsing to a single
                        // color via set_extrusion_color() would paint everything in palette
                        // [initial_tool], which on a dark filament (e.g. #080A0D, a near-black
                        // PLA) looks like a uniformly black model.
                        if (stats.filament_palette.size() > 1) {
                            st->layer_renderer_2d_->set_tool_color_palette(stats.filament_palette);
                            spdlog::info("[GCode Viewer] Streaming 2D using tool palette "
                                         "(size={}, initial_tool={})",
                                         stats.filament_palette.size(), stats.initial_tool_index);
                        } else {
                            // Single-color print: prefer palette[initial_tool_index] when the
                            // slicer emitted a multi-color metadata line and the gcode actually
                            // starts on a non-T0 tool. Falls back to filament_color (palette[0])
                            // for single-color prints or when the palette doesn't cover the
                            // active tool.
                            std::string chosen = stats.filament_color;
                            if (stats.initial_tool_index >= 0 &&
                                stats.initial_tool_index <
                                    static_cast<int>(stats.filament_palette.size()) &&
                                !stats.filament_palette[stats.initial_tool_index].empty()) {
                                chosen = stats.filament_palette[stats.initial_tool_index];
                            }
                            if (!chosen.empty()) {
                                lv_color_t color =
                                    lv_color_hex(std::strtol(chosen.c_str() + 1, nullptr, 16));
                                st->layer_renderer_2d_->set_extrusion_color(color);
                                spdlog::info("[GCode Viewer] Using filament color from metadata: "
                                             "{} (tool={}, palette={})",
                                             chosen, stats.initial_tool_index,
                                             stats.filament_palette.size());
                            }
                        }
                    }

                    // Apply AMS tool color overrides on top of the metadata palette when
                    // available. AMS-known slot colors are typically more accurate than
                    // slicer-emitted palette (which can lag firmware-side filament swaps).
                    if (!st->tool_color_overrides.empty()) {
                        st->layer_renderer_2d_->set_tool_color_overrides(st->tool_color_overrides);
                        spdlog::debug("[GCode Viewer] Streaming 2D applied {} AMS color overrides",
                                      st->tool_color_overrides.size());
                    }

                    // Get canvas size from widget
                    lv_area_t coords;
                    lv_obj_get_coords(obj, &coords);
                    int width = lv_area_get_width(&coords);
                    int height = lv_area_get_height(&coords);
                    st->layer_renderer_2d_->set_canvas_size(width, height);
                    st->layer_renderer_2d_->auto_fit();

                    // Apply SSAO setting
                    if (st->ssao_enabled_at_init_) {
                        st->layer_renderer_2d_->set_ssao_enabled(true);
                    }

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
        lv_label_set_text(st->loading_label, lv_tr("Loading G-code..."));
        lv_obj_set_style_text_color(st->loading_label, theme_manager_get_color("text"),
                                    LV_PART_MAIN);
    }

    // Launch worker thread via RAII-managed start_build()
    // Automatically cancels any existing build and joins the thread
    st->start_build([st, obj, path = std::string(file_path), gen]() {
        auto result = std::make_unique<AsyncBuildResult>();

        try {
            // PHASE 1: Parse G-code file (fast, ~100ms)
            std::ifstream file(path);
            if (!file.is_open()) {
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

                while (std::getline(file, line)) {
                    parser.parse_line(line);

                    if (++lines_since_cancel_check >= CANCEL_POLL_LINES) {
                        lines_since_cancel_check = 0;
                        if (st->is_cancelled()) {
                            cancelled_mid_parse = true;
                            break;
                        }
                    }
                }

                file.close();

                if (cancelled_mid_parse) {
                    spdlog::debug("[GCode Viewer] Build cancelled mid-parse, discarding");
                    return;
                }

                result->gcode_file =
                    std::make_unique<helix::gcode::ParsedGCodeFile>(parser.finalize());
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
                    result->geometry =
                        build_3d_geometry_in_budget(*result->gcode_file, "Initial load");
                    if (!result->geometry) {
                        result->force_2d = true;
                    }
                } else {
                    spdlog::debug("[GCode Viewer] 2D mode - skipping 3D geometry build");
                }
#else
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
                if (st->loading_container) {
                    st->loading_spinner = nullptr;
                    st->loading_label = nullptr;
                    helix::ui::safe_delete_deferred(st->loading_container);
                }

                if (r->success) {
                    spdlog::debug("[GCode Viewer] Async callback - setting up geometry");

                    // Store G-code data
                    st->gcode_file = std::move(r->gcode_file);

                    // Update 2D renderer if it exists (prevents dangling pointer)
                    if (st->layer_renderer_2d_) {
                        st->layer_renderer_2d_->set_gcode(st->gcode_file.get());
                        if (!st->gcode_file->tool_color_palette.empty()) {
                            st->layer_renderer_2d_->set_tool_color_palette(
                                st->gcode_file->tool_color_palette);
                        }
                        st->layer_renderer_2d_->auto_fit();
                    }

                    if (r->force_2d) {
                        // Budget-forced 2D fallback for this file only
                        spdlog::info("[GCode Viewer] Using 2D renderer (budget fallback)");
                        st->budget_forced_2d_ = true;
                        if (!st->layer_renderer_2d_) {
                            st->layer_renderer_2d_ =
                                std::make_unique<helix::gcode::GCodeLayerRenderer>();
                        }
                        st->layer_renderer_2d_->set_gcode(st->gcode_file.get());
                        if (!st->gcode_file->tool_color_palette.empty()) {
                            st->layer_renderer_2d_->set_tool_color_palette(
                                st->gcode_file->tool_color_palette);
                        }

                        // Apply color: external override takes priority
                        if (st->has_external_color_override) {
                            st->layer_renderer_2d_->set_extrusion_color(
                                st->external_color_override);
                        } else if (!st->gcode_file->filament_color_hex.empty()) {
                            lv_color_t color = lv_color_hex(static_cast<uint32_t>(std::strtol(
                                st->gcode_file->filament_color_hex.c_str() + 1, nullptr, 16)));
                            st->layer_renderer_2d_->set_extrusion_color(color);
                        }

                        st->layer_renderer_2d_->auto_fit();
                        lv_obj_invalidate(obj);
                    }

                // Set pre-built geometry on renderer
#ifdef ENABLE_3D_RENDERER
                    if (r->geometry) {
                        st->renderer_->set_prebuilt_geometry(std::move(r->geometry),
                                                             st->gcode_file->filename);
                    }
#endif

                    // Fit camera to model bounds
                    st->camera_->fit_to_bounds(st->gcode_file->global_bounding_box);

                    st->viewer_state = GcodeViewerState::Loaded;
                    spdlog::debug("[GCode Viewer] State set to LOADED");

                    // Auto-apply filament color from gcode metadata (unless
                    // AMS/Spoolman has already set an external override)
                    if (st->has_external_color_override) {
                        st->renderer_->set_extrusion_color(st->external_color_override);
                        spdlog::debug(
                            "[GCode Viewer] Applied external color override (AMS/Spoolman)");
                    } else if (st->use_filament_color &&
                               st->gcode_file->filament_color_hex.length() >= 2) {
                        lv_color_t color = lv_color_hex(static_cast<uint32_t>(std::strtol(
                            st->gcode_file->filament_color_hex.c_str() + 1, nullptr, 16)));
                        st->renderer_->set_extrusion_color(color);
                        spdlog::debug("[GCode Viewer] Applied filament color: {}",
                                      st->gcode_file->filament_color_hex);
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
    st->gcode_file.reset();
    st->streaming_controller_.reset();
    st->has_external_color_override = false; // Clear external color override
    st->tool_color_overrides.clear();        // Clear per-tool AMS colors
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

            // Reset watchdog baseline on resume — the layer-stall comparison
            // should start fresh from the resumed state, not flag the post-pause
            // tick as "stalled" just because the cache didn't move while paused.
            st->watchdog_last_cached_layer_ = -2;
            st->watchdog_last_target_layer_ = -2;
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
        // DRAW_POST takes the full render path (render_to_fbo -> blit_to_lvgl).
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
        st->layer_renderer_2d_ = std::make_unique<helix::gcode::GCodeLayerRenderer>();
        st->layer_renderer_2d_->set_gcode(st->gcode_file.get());
        apply_2d_renderer_colors(st);

        lv_area_t coords;
        lv_obj_get_coords(obj, &coords);
        int width = lv_area_get_width(&coords);
        int height = lv_area_get_height(&coords);
        st->layer_renderer_2d_->set_canvas_size(width, height);
        st->layer_renderer_2d_->auto_fit();

        if (st->ssao_enabled_at_init_) {
            st->layer_renderer_2d_->set_ssao_enabled(true);
        }
    }

#ifdef ENABLE_3D_RENDERER
    // If switching to 3D and the GLES renderer has no geometry (file was loaded
    // in 2D mode so the build was skipped), build it on demand now. Without this
    // the 3D viewer paints an empty background after a live 2D→3D switch.
    if (!st->is_using_2d_mode() && st->gcode_file && st->renderer_ &&
        !st->renderer_->has_geometry()) {
        auto geometry = build_3d_geometry_in_budget(*st->gcode_file, "On-demand 3D switch");
        if (geometry) {
            st->renderer_->set_prebuilt_geometry(std::move(geometry), st->gcode_file->filename);
            if (st->camera_) {
                st->camera_->fit_to_bounds(st->gcode_file->global_bounding_box);
            }
            st->needs_3d_refresh_ = true;
        } else {
            // Budget refused — stay in 2D and revert the mode flag so future state
            // queries (is_using_2d_mode, draw_cb dispatch) keep using the 2D path.
            spdlog::warn("[GCode Viewer] 3D switch refused by memory budget; staying in 2D");
            st->render_mode_ = GcodeViewerRenderMode::Layer2D;
        }
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

void ui_gcode_viewer_zoom(lv_obj_t* obj, float factor) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera_->zoom(factor);
    lv_obj_invalidate(obj);
}

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

void ui_gcode_viewer_set_view(lv_obj_t* obj, GcodeViewerPresetView preset) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    switch (preset) {
    case GcodeViewerPresetView::Isometric:
        st->camera_->set_isometric_view();
        break;
    case GcodeViewerPresetView::Top:
        st->camera_->set_top_view();
        break;
    case GcodeViewerPresetView::Front:
        st->camera_->set_front_view();
        break;
    case GcodeViewerPresetView::Side:
        st->camera_->set_side_view();
        break;
    }

    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_camera_azimuth(lv_obj_t* obj, float azimuth) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera_->set_azimuth(azimuth);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_camera_elevation(lv_obj_t* obj, float elevation) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera_->set_elevation(elevation);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_camera_zoom(lv_obj_t* obj, float zoom) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera_->set_zoom_level(zoom);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_debug_colors(lv_obj_t* obj, bool enable) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

#ifdef ENABLE_3D_RENDERER
    st->renderer_->set_debug_face_colors(enable);
    lv_obj_invalidate(obj);
#else
    (void)enable;
#endif
}

// ==============================================
// Rendering Options
// ==============================================

void ui_gcode_viewer_set_show_travels(lv_obj_t* obj, bool show) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer_->set_show_travels(show);

    // Also update 2D renderer if initialized
    if (st->layer_renderer_2d_) {
        st->layer_renderer_2d_->set_show_travels(show);
    }

    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_highlighted_objects(lv_obj_t* obj,
                                             const std::unordered_set<std::string>& object_names) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer_->set_highlighted_objects(object_names);
    if (st->layer_renderer_2d_) {
        st->layer_renderer_2d_->set_highlighted_objects(object_names);
    }
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_excluded_objects(lv_obj_t* obj,
                                          const std::unordered_set<std::string>& object_names) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    // Skip if excluded set hasn't changed (avoids expensive cache invalidation)
    if (object_names == st->excluded_objects) {
        return;
    }

    st->excluded_objects = object_names;
    st->renderer_->set_excluded_objects(object_names);
    if (st->layer_renderer_2d_) {
        st->layer_renderer_2d_->set_excluded_objects(object_names);
    }
    lv_obj_invalidate(obj);

    spdlog::debug("[GCode Viewer] Excluded objects updated ({} objects)", object_names.size());
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

void ui_gcode_viewer_set_extrusion_color(lv_obj_t* obj, lv_color_t color) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    // Store override so lazy-initialized renderers pick it up
    st->has_external_color_override = true;
    st->external_color_override = color;

    st->renderer_->set_extrusion_color(color);
    if (st->layer_renderer_2d_) {
        st->layer_renderer_2d_->set_extrusion_color(color);
    }
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_tool_colors(lv_obj_t* obj, const std::vector<uint32_t>& colors) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || colors.empty())
        return;

    // Store for lazy-init paths
    st->tool_color_overrides = colors;

    // Per-tool overrides supersede the single-color external override
    st->has_external_color_override = false;

    // Apply to 3D renderer
#ifdef ENABLE_3D_RENDERER
    st->renderer_->set_tool_color_overrides(colors);
#endif

    // Apply to 2D renderer
    if (st->layer_renderer_2d_) {
        st->layer_renderer_2d_->set_tool_color_overrides(colors);
    }

    lv_obj_invalidate(obj);
    spdlog::debug("[GCode Viewer] Applied {} per-tool AMS color overrides", colors.size());
}

bool ui_gcode_viewer_apply_ams_tool_colors(lv_obj_t* obj) {
    if (!obj) {
        return false;
    }

    auto* backend = AmsState::instance().get_backend();
    if (!backend) {
        spdlog::debug("[GCode Viewer] apply_ams_tool_colors: no AMS backend");
        return false;
    }

    const auto& info = backend->get_system_info();
    const auto& tool_map = info.tool_to_slot_map;
    if (tool_map.empty()) {
        spdlog::debug("[GCode Viewer] apply_ams_tool_colors: tool_to_slot_map empty");
        return false;
    }

    std::vector<uint32_t> tool_colors;
    tool_colors.reserve(tool_map.size());
    bool all_default = true;

    for (size_t tool = 0; tool < tool_map.size(); ++tool) {
        int slot_index = tool_map[tool];
        const auto* slot = info.get_slot_global(slot_index);
        if (slot && slot->color_rgb != AMS_DEFAULT_SLOT_COLOR) {
            tool_colors.push_back(slot->color_rgb);
            all_default = false;
            spdlog::debug("[GCode Viewer] Tool {} -> slot {} -> color 0x{:06X}", tool, slot_index,
                          slot->color_rgb);
        } else {
            tool_colors.push_back(AMS_DEFAULT_SLOT_COLOR);
            spdlog::debug("[GCode Viewer] Tool {} -> slot {} -> default", tool, slot_index);
        }
    }

    if (all_default) {
        spdlog::debug("[GCode Viewer] apply_ams_tool_colors: all colors are default, skipping");
        return false;
    }

    ui_gcode_viewer_set_tool_colors(obj, tool_colors);
    spdlog::debug("[GCode Viewer] Applied {} AMS tool colors", tool_colors.size());
    return true;
}

// ==============================================
// Layer Control Extensions
// ==============================================

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

    // Update 3D renderer
    st->renderer_->set_print_progress_layer(current_layer);

    // Note: 2D renderer's current_layer is set in the render callback
    // using print_progress_layer_, so we just need to invalidate.
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_ghost_mode(lv_obj_t* obj, int mode) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    // Map int to enum (0=Dimmed, 1=Stipple)
    helix::gcode::GhostRenderMode render_mode = (mode == 1) ? helix::gcode::GhostRenderMode::Stipple
                                                            : helix::gcode::GhostRenderMode::Dimmed;

    st->renderer_->set_ghost_render_mode(render_mode);
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

/// Push the live occlusion down to whichever renderer is active. Both the fit
/// and the vertical shift derive from it, so the renderer owns that computation
/// and re-fits when the number moves; this only has to keep it current. Cheap
/// enough to run per draw, which is what keeps the framing right across
/// relayout without the panel having to repush anything.
static void gcode_viewer_refresh_content_offset(gcode_viewer_state_t* st, lv_obj_t* obj,
                                                int canvas_height) {
    (void)canvas_height;
    const float occlusion = measure_bottom_occlusion(st, obj);

    if (st->layer_renderer_2d_) {
        st->layer_renderer_2d_->set_bottom_occlusion(occlusion);
    }
#ifdef ENABLE_3D_RENDERER
    if (st->camera_) {
        st->camera_->set_bottom_occlusion(occlusion);
    }
    if (st->renderer_) {
        // The GLES path applies the shift in build_mvp(); the camera has already
        // absorbed the occlusion into its zoom.
        const float content_height = st->camera_ ? st->camera_->get_content_height_fraction() *
                                                       static_cast<float>(canvas_height)
                                                 : 0.0f;
        st->renderer_->set_content_offset_y(
            helix::gcode::compute_content_offset_y(content_height, canvas_height, occlusion));
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

    // Fallback to 3D renderer
    return st->renderer_->get_max_layer_index();
}

// ==============================================
// Metadata Access
// ==============================================

const char* ui_gcode_viewer_get_filament_type(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file || st->gcode_file->filament_type.empty())
        return nullptr;

    return st->gcode_file->filament_type.c_str();
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

// ==============================================
// Object Picking
// ==============================================

const char* ui_gcode_viewer_pick_object(lv_obj_t* obj, int x, int y) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !has_gcode_data(st))
        return nullptr;

    // Convert screen coordinates to widget-local coordinates
    lv_area_t widget_coords;
    lv_obj_get_coords(obj, &widget_coords);
    int local_x = x - widget_coords.x1;
    int local_y = y - widget_coords.y1;

    spdlog::debug("[GCode Viewer] pick_object screen=({}, {}), widget_pos=({}, {}), local=({}, {})",
                  x, y, widget_coords.x1, widget_coords.y1, local_x, local_y);

    std::optional<std::string> result;

    // Use 2D renderer's pick_object_at in 2D mode
    if (st->is_using_2d_mode() && st->layer_renderer_2d_) {
        result = st->layer_renderer_2d_->pick_object_at(local_x, local_y);
    } else if (st->renderer_ && st->gcode_file) {
        // 3D renderer path (requires full gcode file)
        result =
            st->renderer_->pick_object(glm::vec2(local_x, local_y), *st->gcode_file, *st->camera_);
    }

    if (result) {
        // Store in static buffer (safe for single-threaded LVGL)
        static std::string picked_name;
        picked_name = *result;
        return picked_name.c_str();
    }

    return nullptr;
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
// Material & Lighting Control
// ==============================================

void ui_gcode_viewer_set_specular(lv_obj_t* obj, float intensity, float shininess) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

#ifdef ENABLE_3D_RENDERER
    st->renderer_->set_specular(intensity, shininess);
    lv_obj_invalidate(obj); // Request redraw
#else
    (void)intensity;
    (void)shininess;
    spdlog::warn("[GCode Viewer] set_specular() ignored - 3D renderer not available");
#endif
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

void ui_gcode_viewer_set_gcode_data(lv_obj_t*, void*) {}

void ui_gcode_viewer_clear(lv_obj_t*) {}

void ui_gcode_viewer_clear_all_active(void) {}

void ui_gcode_viewer_set_clear_callback(lv_obj_t*, ui_gcode_viewer_clear_cb_t, void*) {}

helix::GcodeViewerState ui_gcode_viewer_get_state(lv_obj_t*) {
    return helix::GcodeViewerState::Empty;
}

bool ui_gcode_viewer_has_content(lv_obj_t*) {
    return false;
}

void ui_gcode_viewer_set_paused(lv_obj_t*, bool) {}

bool ui_gcode_viewer_is_paused(lv_obj_t*) {
    return false;
}

void ui_gcode_viewer_force_redraw(lv_obj_t*) {}

void ui_gcode_viewer_set_render_mode(lv_obj_t*, helix::GcodeViewerRenderMode) {}

helix::GcodeViewerRenderMode ui_gcode_viewer_get_render_mode(lv_obj_t*) {
    return helix::GcodeViewerRenderMode::Auto;
}

void ui_gcode_viewer_evaluate_render_mode(lv_obj_t*) {}

bool ui_gcode_viewer_is_using_2d_mode(lv_obj_t*) {
    return false;
}

void ui_gcode_viewer_disable_streaming(lv_obj_t*) {}

void ui_gcode_viewer_set_show_supports(lv_obj_t*, bool) {}

void ui_gcode_viewer_rotate(lv_obj_t*, float, float) {}

void ui_gcode_viewer_pan(lv_obj_t*, float, float) {}

void ui_gcode_viewer_zoom(lv_obj_t*, float) {}

void ui_gcode_viewer_reset_camera(lv_obj_t*) {}

void ui_gcode_viewer_set_view(lv_obj_t*, helix::GcodeViewerPresetView) {}

void ui_gcode_viewer_set_camera_azimuth(lv_obj_t*, float) {}

void ui_gcode_viewer_set_camera_elevation(lv_obj_t*, float) {}

void ui_gcode_viewer_set_camera_zoom(lv_obj_t*, float) {}

void ui_gcode_viewer_set_debug_colors(lv_obj_t*, bool) {}

void ui_gcode_viewer_set_show_travels(lv_obj_t*, bool) {}

void ui_gcode_viewer_set_show_extrusions(lv_obj_t*, bool) {}

void ui_gcode_viewer_set_layer_range(lv_obj_t*, int, int) {}

void ui_gcode_viewer_set_highlighted_object(lv_obj_t*, const char*) {}

const char* ui_gcode_viewer_pick_object(lv_obj_t*, int, int) {
    return nullptr;
}

void ui_gcode_viewer_set_extrusion_color(lv_obj_t*, lv_color_t) {}

void ui_gcode_viewer_set_travel_color(lv_obj_t*, lv_color_t) {}

void ui_gcode_viewer_use_filament_color(lv_obj_t*, bool) {}

void ui_gcode_viewer_set_opacity(lv_obj_t*, lv_opa_t) {}

void ui_gcode_viewer_set_brightness(lv_obj_t*, float) {}

void ui_gcode_viewer_set_specular(lv_obj_t*, float, float) {}

void ui_gcode_viewer_set_single_layer(lv_obj_t*, int) {}

int ui_gcode_viewer_get_current_layer_start(lv_obj_t*) {
    return 0;
}

int ui_gcode_viewer_get_current_layer_end(lv_obj_t*) {
    return -1;
}

void ui_gcode_viewer_set_print_progress(lv_obj_t*, int) {}

void ui_gcode_viewer_set_ghost_opacity(lv_obj_t*, lv_opa_t) {}

void ui_gcode_viewer_set_ghost_mode(lv_obj_t*, int) {}

void ui_gcode_viewer_set_ssao_enabled(lv_obj_t*, bool) {}

bool ui_gcode_viewer_get_ssao_enabled(lv_obj_t*) {
    return false;
}

void ui_gcode_viewer_set_bottom_occluder(lv_obj_t*, lv_obj_t*) {}

int ui_gcode_viewer_get_max_layer(lv_obj_t*) {
    return -1;
}

const char* ui_gcode_viewer_get_filament_color(lv_obj_t*) {
    return nullptr;
}

const char* ui_gcode_viewer_get_filament_type(lv_obj_t*) {
    return nullptr;
}

const char* ui_gcode_viewer_get_printer_model(lv_obj_t*) {
    return nullptr;
}

float ui_gcode_viewer_get_estimated_time_minutes(lv_obj_t*) {
    return 0.0f;
}

float ui_gcode_viewer_get_filament_weight_g(lv_obj_t*) {
    return 0.0f;
}

float ui_gcode_viewer_get_filament_length_mm(lv_obj_t*) {
    return 0.0f;
}

float ui_gcode_viewer_get_filament_cost(lv_obj_t*) {
    return 0.0f;
}

float ui_gcode_viewer_get_nozzle_diameter_mm(lv_obj_t*) {
    return 0.0f;
}

const char* ui_gcode_viewer_get_filename(lv_obj_t*) {
    return nullptr;
}

int ui_gcode_viewer_get_layer_count(lv_obj_t*) {
    return 0;
}

int ui_gcode_viewer_get_segments_rendered(lv_obj_t*) {
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

void ui_gcode_viewer_set_object_long_press_callback(lv_obj_t*,
                                                    gcode_viewer_object_long_press_callback_t,
                                                    void*) {}

const helix::gcode::ParsedGCodeFile* ui_gcode_viewer_get_parsed_file(lv_obj_t*) {
    return nullptr;
}

#endif // HELIX_HAS_GCODE_VIEWER
