// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The gcode viewer widget's per-instance state, shared by the shell
// (ui_gcode_viewer.cpp), the loader and the input handlers. Private to src/ui/.

#pragma once

#if HELIX_HAS_GCODE_VIEWER

#include "ui_gcode_viewer.h"

#include "config.h"
#include "gcode_camera.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "gcode_pause_scan.h"
#include "gcode_render_mode_policy.h"
#include "gcode_ssao_policy.h"
#include "gcode_streaming_controller.h"
#include "memory_utils.h"
#include "system/crash_handler.h"
#include "view_gestures.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef ENABLE_GLES_3D
#include "gcode_gles_renderer.h"
#define ENABLE_3D_RENDERER
using GCode3DRenderer = helix::gcode::GCodeGLESRenderer;
#endif

namespace helix::gcode_viewer {

/// A fresh range of load generations for one viewer. A queued result names its
/// widget by address, and LVGL reuses a deleted widget's address, so a generation
/// that restarted at zero would match the result queued for the viewer that held
/// the address before.
inline uint64_t next_generation_base() {
    static std::atomic<uint64_t> epoch{0};
    return (epoch.fetch_add(1) + 1) << 32;
}

/// What the viewer was told to show, held apart from whichever renderers exist.
struct ViewOptions {
    std::unordered_set<std::string> highlighted;
    std::unordered_set<std::string> excluded;
    std::vector<uint32_t> tool_colors; ///< Per-tool AMS colors (0xRRGGBB); empty = the file's own
};

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
        const bool constrained = helix::get_system_memory_info().is_constrained_device();
        const char* ssao_env = std::getenv("HELIX_SSAO");
        const auto ssao = helix::gcode_viewer::decide_ssao_enabled(constrained, ssao_env);
        ssao_enabled_at_init_ = ssao.enabled;
        antialias_enabled_at_init_ =
            helix::gcode_viewer::decide_antialias_enabled(constrained, ssao_env);
        switch (ssao.reason) {
        case helix::gcode_viewer::SsaoReason::ConstrainedReduced:
            spdlog::info("[GCode Viewer] Constrained device - outline shading on, antialiasing off "
                         "(HELIX_SSAO=0 to disable both, =1 to force both on)");
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

    /// Join the build thread without cancelling it (test seam).
    void wait_for_build() {
        if (build_thread_.joinable()) {
            build_thread_.join();
        }
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
#endif

    // Gesture state
    bool is_dragging{false};
    lv_point_t drag_start{0, 0};
    lv_point_t last_drag_pos{0, 0};
    bool gesture_moved{false}; ///< True once movement exceeded threshold anywhere in this touch
#if LV_USE_GESTURE_RECOGNITION
    helix::ui::TwoFingerState two_finger; ///< Pan/zoom totals for the gesture in progress
    bool two_finger_occurred{
        false}; ///< Sticky until all fingers lift: gates rotate, tap, long-press
#endif

    // Touch selection: what the viewer's own taps and long presses picked
    std::unordered_set<std::string> selected_objects;

    /// What every renderer of this viewer should show, and what each was last
    /// given, so applying it again pushes only what changed.
    ViewOptions view_options;
    ViewOptions applied_3d;
    ViewOptions applied_2d;

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
    helix::gcode::FitFraming framing_{
        helix::gcode::FitFraming::STANDARD}; ///< Fit shape every renderer of this viewer uses
    ui_gcode_viewer_clear_cb_t clear_callback{nullptr};
    void* clear_callback_user_data{nullptr};

    // Long-press state
    lv_timer_t* long_press_timer_{nullptr};
    bool long_press_fired{false};
    std::string long_press_object_name;

    // Rendering settings
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

    /// Scheduled pauses of the loaded file + the axis its progress bar fills
    /// on, collected by whichever path read it (streaming layer-index scan or
    /// full-load parse loop). The panel's load callback publishes them into
    /// PrinterPrintState, which knows which print the file belongs to.
    std::vector<helix::gcode::ScheduledPause> scheduled_pauses;
    helix::gcode::ProgressAxis scheduled_pauses_axis{helix::gcode::ProgressAxis::BytePosition};
    bool has_pause_scan{false};

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
    // queue_invalidate routes through queue_update, no escape from a
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
    /// Cleared by the occluder's own LV_EVENT_DELETE if the occluder dies
    /// first; the viewer's delete handler detaches that callback if the viewer
    /// dies first. Both directions are needed -- the two are siblings in one
    /// subtree, so either order happens during teardown.
    lv_obj_t* bottom_occluder_{nullptr};

    /// SSAO enabled at init (from HELIX_SSAO env var, applied when 2D renderer is created)
    bool ssao_enabled_at_init_{false};
    bool antialias_enabled_at_init_{false};

    /// Render mode setting, seeded from HELIX_GCODE_MODE by the constructor.
    /// Atomic, like budget_forced_2d_: the load worker reads both through
    /// is_using_2d_mode() while the main thread may be setting them.
    std::atomic<GcodeViewerRenderMode> render_mode_{GcodeViewerRenderMode::Layer2D};

    /// Budget system forced 2D for current file (reset on each new load)
    std::atomic<bool> budget_forced_2d_{false};

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
        // Without a 3D renderer there is no 3D mode, full stop. A stored
        // display/gcode_render_mode of 3D (copied from a GLES device, or
        // hand-edited) must still land here: ui_gcode_viewer_set_render_mode()
        // has no availability guard.
        return true;
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
    std::atomic<uint64_t> load_generation_{next_generation_base()};
};

using gcode_viewer_state_t = GCodeViewerState;

/// The widget's state, or null for an object that is not a viewer.
inline gcode_viewer_state_t* get_state(lv_obj_t* obj) {
    return static_cast<gcode_viewer_state_t*>(lv_obj_get_user_data(obj));
}

/// The loading spinner card, shown while the viewer has nothing to draw yet.
void create_loading_ui(gcode_viewer_state_t* st, lv_obj_t* obj, const char* text);
/// Take down the loading spinner, deferred (callers run inside queued callbacks).
void remove_loading_ui(gcode_viewer_state_t* st);
/// Apply the color priority chain to the 2D renderer from the loaded file.
void apply_2d_renderer_colors(gcode_viewer_state_t* st);
/// Canvas, framing and shading tier for a freshly created 2D renderer.
void seed_2d_renderer_view(gcode_viewer_state_t* st, int width, int height);
/// Route the current file to the 2D renderer because the memory budget refused 3D.
void apply_budget_forced_2d(gcode_viewer_state_t* st, lv_obj_t* obj);
/// Give each renderer the parts of view_options it has not been given yet. Call after
/// changing view_options and after creating a renderer; an unchanged option costs the
/// 3D renderer nothing (a changed color list re-uploads its VBOs).
void apply_view_options(gcode_viewer_state_t* st);
/// Forget what the stall watchdog has observed.
void gcode_viewer_watchdog_restart(gcode_viewer_state_t* st);

/// Register the touch handlers (press, drag, release, two-finger gesture) on a viewer.
void install_input_handlers(lv_obj_t* obj);

#ifdef ENABLE_3D_RENDERER
/// Build 3D geometry for the file already loaded, on the viewer's build thread.
void start_on_demand_3d_build(gcode_viewer_state_t* st, lv_obj_t* obj);
#endif

} // namespace helix::gcode_viewer

#endif // HELIX_HAS_GCODE_VIEWER
