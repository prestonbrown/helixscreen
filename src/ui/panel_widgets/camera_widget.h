// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "async_lifetime_guard.h"
#include "camera_stream.h"
#include "observer_factory.h"
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
    void apply_transform();   // Push rotation/flip config to stream
    void update_stream_fps(); // Re-evaluate and set max_fps based on current state
    void set_status_text(const char* text);
    void destroy_fullscreen(); // Synchronous cleanup of fullscreen overlay

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

    std::unique_ptr<CameraStream> stream_;
    bool active_ = false;              // true when on_activate() has been called
    bool compact_ = false;             // true at 1x1 — icon only, no live stream
    bool sleep_cb_registered_ = false; // true after display sleep callback registered

    // Observer for webcam availability — starts stream when URLs arrive
    ObserverGuard webcam_observer_;
    // Observer for home edit mode — throttles camera fps during editing
    ObserverGuard edit_mode_observer_;

    int target_fps_ = 15;                     // From Moonraker webcam config
    lv_timer_t* fps_recheck_timer_ = nullptr; // Periodic re-eval when paused

    // Lifetime guard — prevents use-after-free in queued callbacks.
    // stream lifetime is invalidated on each stop_stream() cycle;
    // widget lifetime lives for the entire CameraWidget object.
    helix::AsyncLifetimeGuard lifetime_;
    helix::AsyncLifetimeGuard widget_lifetime_;

    // Config modal (owned, destroyed when modal closes)
    std::unique_ptr<CameraConfigModal> config_modal_;

    // Persisted transform config
    nlohmann::json config_;

    // Test-observable call counts for the stream lifecycle. start_stream()/
    // stop_stream() sit behind their own guards (active_, fullscreen_overlay_)
    // strictly narrower than the on_size_changed() edge-trigger conditions
    // that invoke them, and with no camera configured stream_ always ends up
    // null again after either call — so "stream_ non-null" cannot distinguish
    // "the call happened and no-op'd" from "the call never happened". A raw
    // count of invocations can, without needing a real camera. See
    // tests/test_helpers/camera_widget_test_access.h.
    int start_stream_calls_ = 0;
    int stop_stream_calls_ = 0;

    friend class CameraWidgetTestAccess;
};

/**
 * @brief Open the camera fullscreen overlay independently of any CameraWidget.
 *
 * Creates a standalone CameraStream + camera_fullscreen overlay and pushes it
 * onto NavigationManager. Used by entry points outside the home panel (e.g.
 * Settings → Hardware & Devices → Camera). No-ops if no webcam is configured
 * or if a fullscreen view is already open.
 *
 * @param parent_screen Active screen the overlay is attached to.
 */
void open_standalone_camera_fullscreen(lv_obj_t* parent_screen);

} // namespace helix

#endif // HELIX_HAS_CAMERA
