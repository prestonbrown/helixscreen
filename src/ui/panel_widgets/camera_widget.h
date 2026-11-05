// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

#include "hv/json.hpp"

#if HELIX_HAS_CAMERA

#include "async_lifetime_guard.h"
#include "camera_view.h"
#include "core/observer_factory.h"
#include "moonraker_types.h"
#include "panel_widget.h"

#include <memory>

namespace helix {

class CameraConfigModal;

class CameraWidget : public PanelWidget {
  public:
    CameraWidget();
    ~CameraWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "camera";
    }
    void set_config(const nlohmann::json& config) override;
    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

    void on_activate() override;
    void on_deactivate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool has_overlay_open() const override {
        return fullscreen_overlay_ != nullptr;
    }

    /// Open/close fullscreen camera overlay
    void open_fullscreen();
    void close_fullscreen();

    /// Event callback for camera click (static to access protected record_interaction)
    static void on_camera_clicked(lv_event_t* e);

  private:
    void start_stream();
    void stop_stream();
    void apply_transform();                // Push rotation/flip config to stream
    std::string configured_source() const; // `source` key of config_ (webcam name), or ""
    /// Restart the stream when the feed configured_source() resolves to is no
    /// longer the one running — after a config save, or when the webcam list
    /// changes under a running stream (the named camera gone, or back).
    void resync_source();
    void update_stream_fps(); // Re-evaluate and set max_fps based on current state
    void set_status_text(const char* text);
    /// Show the translation of @p key, a string literal, and keep it translated.
    void set_status_key(const char* key);
    void destroy_fullscreen(); // Synchronous cleanup of fullscreen overlay
    /// Create the fullscreen overlay on the active screen and push it, unless
    /// another widget already owns one. The caller has already started the stream.
    void show_fullscreen_overlay();
    /// LV_EVENT_DELETE on the fullscreen overlay. A screen teardown frees it
    /// without running the NavigationManager close callback (#1430).
    static void on_fullscreen_overlay_deleted(lv_event_t* e);

    /// Null the tile-tree pointers (root, screen, image, overlay, status
    /// label) without touching the stream, the observers, or the fullscreen
    /// overlay. Shared by detach() and the raw-delete hook.
    void forget_tile_widgets();

    /// Drop the cached tile pointers when the tile tree dies without a
    /// detach() - a raw lv_obj_delete() of the home page container.
    void on_hooked_root_deleted() override;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* camera_image_ = nullptr;
    lv_obj_t* camera_overlay_ = nullptr; // Spinner overlay, hidden on first frame
    lv_obj_t* camera_status_ = nullptr;  // Status text label inside overlay

    // Fullscreen overlay state
    lv_obj_t* fullscreen_overlay_ = nullptr;
    lv_obj_t* fullscreen_image_ = nullptr;
    lv_obj_t* fullscreen_spinner_ =
        nullptr; // "Connecting…" overlay in fullscreen, hidden on first frame

    CameraView view_;
    bool active_ = false;  // true when on_activate() has been called
    bool compact_ = false; // true at 1x1 — icon only, no live stream

    // Observer for webcam availability — starts stream when URLs arrive
    ObserverGuard webcam_observer_;
    // Observer for the webcam list — re-resolves a named source when it changes
    ObserverGuard webcam_list_observer_;
    // Observer for home edit mode — throttles camera fps during editing
    ObserverGuard edit_mode_observer_;
    ObserverGuard language_observer_;

    lv_timer_t* fps_recheck_timer_ = nullptr; // Periodic re-eval when paused

    // Config modal (owned, destroyed when modal closes)
    std::unique_ptr<CameraConfigModal> config_modal_;

    // Persisted transform config
    nlohmann::json config_;

    // Test-observable call counts for the stream lifecycle. start_stream()/
    // stop_stream() sit behind their own guards (active_, fullscreen_overlay_)
    // strictly narrower than the on_size_changed() edge-trigger conditions
    // that invoke them, and with no camera configured the view never holds a stream
    // after either call — so "has a stream" cannot distinguish
    // "the call happened and no-op'd" from "the call never happened". A raw
    // count of invocations can, without needing a real camera. See
    // tests/test_helpers/camera_widget_test_access.h.
    int start_stream_calls_ = 0;
    int stop_stream_calls_ = 0;

    friend class CameraWidgetTestAccess;
};

/**
 * @brief The camera fullscreen overlay with no owning CameraWidget.
 *
 * Opened from entry points outside the home panel (Settings -> Devices ->
 * Camera, Print Status -> More -> Camera). Its stream is a CameraView's, so it
 * starts, preempts other views of the same camera, and tears down exactly as
 * the widget and the Print Status card do. One fullscreen view at a time.
 */
class StandaloneCameraViewer {
  public:
    /// Open the viewer on the active screen, or hand off to an attached
    /// CameraWidget's fullscreen when one exists. No-ops when no webcam is
    /// configured or a fullscreen view is already open.
    /// @return true when a fullscreen view opened, its own or a widget's.
    static bool open(lv_obj_t* parent_screen);

    StandaloneCameraViewer(const StandaloneCameraViewer&) = delete;
    StandaloneCameraViewer& operator=(const StandaloneCameraViewer&) = delete;

  private:
    StandaloneCameraViewer();
    ~StandaloneCameraViewer() = default;

    /// Stop the stream, delete the overlay and free the open viewer.
    static void close();
    /// A screen teardown frees the overlay without running the
    /// NavigationManager close callback (#1430).
    static void on_overlay_deleted(lv_event_t* e);

    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_obj_t* spinner_ = nullptr; // "Connecting..." indicator, hidden on first frame
    CameraView view_;

    static StandaloneCameraViewer* s_open_;

    friend class CameraWidgetTestAccess;
};

/// StandaloneCameraViewer::open(), for callers that only forward-declare it.
bool open_standalone_camera_fullscreen(lv_obj_t* parent_screen);

} // namespace helix

#endif // HELIX_HAS_CAMERA
