// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_widget.h"

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "camera_config_modal.h"
#include "display_manager.h"
#include "http_executor.h"
#include "i_moonraker_api.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"
#include "system/telemetry_manager.h"
#include "ui/ui_cleanup_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <vector>

// Module-level subject for status text binding (static — shared across instances)
static lv_subject_t s_camera_status_subject;
static char s_camera_status_buffer[64];
static bool s_subjects_initialized = false;

static void camera_widget_init_subjects() {
    if (s_subjects_initialized) {
        return;
    }

    lv_subject_init_string(&s_camera_status_subject, s_camera_status_buffer, nullptr,
                           sizeof(s_camera_status_buffer), lv_tr("No Camera"));
    lv_xml_register_subject(nullptr, "camera_status_text", &s_camera_status_subject);
    SubjectDebugRegistry::instance().register_subject(
        &s_camera_status_subject, "camera_status_text", LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("CameraWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            lv_subject_deinit(&s_camera_status_subject);
            s_subjects_initialized = false;
            spdlog::trace("[CameraWidget] Subjects deinitialized");
        }
    });

    spdlog::debug("[CameraWidget] Subjects initialized");
}

// Only one fullscreen camera overlay can be open at a time
static helix::CameraWidget* s_fullscreen_owner = nullptr;

// All currently-attached CameraWidget instances. Used by the standalone
// fullscreen viewer to delegate to an existing widget's stream rather than
// open a second concurrent connection (which most MJPEG streamers reject).
static std::vector<helix::CameraWidget*> s_attached_widgets;

static void on_camera_fullscreen_close(lv_event_t* /*e*/) {
    if (s_fullscreen_owner) {
        s_fullscreen_owner->close_fullscreen();
    } else {
        // Standalone fullscreen viewer (no owning CameraWidget) — the
        // NavigationManager-registered close callback handles cleanup.
        NavigationManager::instance().go_back();
    }
}

namespace helix {
void register_camera_widget() {
    register_widget_factory("camera",
                            [](const std::string&) { return std::make_unique<CameraWidget>(); });
    register_widget_subjects("camera", camera_widget_init_subjects);
    lv_xml_register_event_cb(nullptr, "on_camera_clicked", CameraWidget::on_camera_clicked);
    lv_xml_register_event_cb(nullptr, "on_camera_fullscreen_close", on_camera_fullscreen_close);

    // Camera config modal callbacks (registered once, not per-modal-instance)
    lv_xml_register_event_cb(nullptr, "on_cam_rotate_0", CameraConfigModal::on_rotate_0);
    lv_xml_register_event_cb(nullptr, "on_cam_rotate_90", CameraConfigModal::on_rotate_90);
    lv_xml_register_event_cb(nullptr, "on_cam_rotate_180", CameraConfigModal::on_rotate_180);
    lv_xml_register_event_cb(nullptr, "on_cam_rotate_270", CameraConfigModal::on_rotate_270);
    lv_xml_register_event_cb(nullptr, "on_cam_flip_h_toggled",
                             CameraConfigModal::on_flip_h_toggled);
    lv_xml_register_event_cb(nullptr, "on_cam_flip_v_toggled",
                             CameraConfigModal::on_flip_v_toggled);
}

void CameraWidget::on_camera_clicked(lv_event_t* e) {
    auto* self = panel_widget_from_event<CameraWidget>(e);
    if (!self)
        return;
    self->record_interaction();
    self->open_fullscreen();
}

CameraWidget::CameraWidget() {}

CameraWidget::~CameraWidget() {
    // detach() handles LVGL pointer cleanup, observer release, fullscreen close
    detach();
    // Full cleanup beyond detach: invalidate lifetime guard and stop stream
    lifetime_.invalidate();
    stop_stream();
}

void CameraWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    camera_image_ = lv_obj_find_by_name(widget_obj_, "camera_image");
    camera_overlay_ = lv_obj_find_by_name(widget_obj_, "camera_overlay");
    camera_status_ = lv_obj_find_by_name(widget_obj_, "camera_status");

    lv_obj_set_user_data(widget_obj_, this);

    if (std::find(s_attached_widgets.begin(), s_attached_widgets.end(), this) ==
        s_attached_widgets.end()) {
        s_attached_widgets.push_back(this);
    }

    // Observe printer_has_webcam — URLs may not be available yet at attach
    // time (Moonraker sends webcam list asynchronously). When the subject
    // transitions to 1, try starting the stream if we're active.
    lv_subject_t* gate = lv_xml_get_subject(nullptr, "printer_has_webcam");
    if (gate) {
        webcam_observer_ =
            helix::ui::observe_int_sync<CameraWidget>(gate, this, [](CameraWidget* self, int val) {
                if (val > 0) {
                    if (self->compact_) {
                        // Compact mode: icon only, status text already hidden
                    } else {
                        self->set_status_text(lv_tr("Connecting Camera..."));
                        if (self->active_) {
                            self->start_stream();
                        }
                    }
                } else {
                    // Only stop if the stream isn't being actively displayed.
                    // observe_int_sync defers via queue_update(), so this callback can
                    // fire AFTER on_activate() already restarted the stream — killing it
                    // and leaving a gray rectangle. Also skip if fullscreen overlay is
                    // open — the stream must keep running for the fullscreen view.
                    if (!self->active_ && !self->fullscreen_overlay_) {
                        self->stop_stream();
                        self->set_status_text(lv_tr("No Camera"));
                    }
                }
            });
    }

    spdlog::debug("[CameraWidget] Attached");
}

void CameraWidget::detach() {
    if (!widget_obj_) {
        return; // Already detached or never attached
    }

    // Synchronously destroy fullscreen overlay. Cannot use close_fullscreen()
    // because go_back() is deferred and 'this' may be destroyed before it fires.
    destroy_fullscreen();

    // Lightweight detach: release observers and LVGL pointers but preserve
    // the camera stream. lifetime_ is intentionally NOT invalidated here
    // (unlike other widgets) — the stream keeps running during the
    // detach→reattach gap. Frame callbacks check camera_image_ (null
    // after detach) and safely no-op until re-attach.
    webcam_observer_.reset();
    edit_mode_observer_.reset();
    if (fps_recheck_timer_) {
        lv_timer_delete(fps_recheck_timer_);
        fps_recheck_timer_ = nullptr;
    }

    lv_obj_set_user_data(widget_obj_, nullptr);
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    camera_image_ = nullptr;
    camera_overlay_ = nullptr;
    camera_status_ = nullptr;

    auto it = std::find(s_attached_widgets.begin(), s_attached_widgets.end(), this);
    if (it != s_attached_widgets.end()) {
        s_attached_widgets.erase(it);
    }

    spdlog::debug("[CameraWidget] Detached (stream preserved)");
}

void CameraWidget::on_activate() {
    spdlog::debug("[CameraWidget] on_activate (was active={})", active_);
    active_ = true;

    // Register for display sleep/wake notifications to suspend the camera
    // stream while the screen is off (saves ~20% CPU on Pi).
    // Uses LifetimeToken so the callback safely no-ops after destruction.
    if (!sleep_cb_registered_) {
        if (auto* dm = DisplayManager::instance()) {
            auto token = widget_lifetime_.token();
            dm->register_sleep_callback([this, token](bool sleeping) {
                if (token.expired())
                    return;
                if (sleeping) {
                    // Don't stop the stream if fullscreen overlay is open — the
                    // user is actively viewing the camera feed.
                    if (!fullscreen_overlay_) {
                        spdlog::debug("[CameraWidget] Display sleeping — stopping camera stream");
                        stop_stream();
                    }
                } else if (fullscreen_overlay_ || (active_ && !compact_)) {
                    spdlog::debug("[CameraWidget] Display waking — restarting camera stream");
                    start_stream();
                }
            });
            sleep_cb_registered_ = true;
        }
    }

    // Observe edit mode changes to throttle camera fps
    if (!edit_mode_observer_) {
        lv_subject_t* edit_subj = &get_home_edit_mode_subject();
        edit_mode_observer_ = helix::ui::observe_int_sync<CameraWidget>(
            edit_subj, this, [](CameraWidget* self, int /*val*/) { self->update_stream_fps(); });
    }

    if (!compact_) {
        start_stream();
    } else {
        // Compact mode: just show the icon overlay, no stream
        if (camera_overlay_) {
            lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
        if (camera_status_) {
            lv_obj_add_flag(camera_status_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void CameraWidget::on_deactivate() {
    spdlog::debug("[CameraWidget] on_deactivate (was active={}, fullscreen={})", active_,
                  fullscreen_overlay_ != nullptr);
    active_ = false;
    // Don't stop the stream if fullscreen is open — we're still showing frames
    if (!fullscreen_overlay_) {
        stop_stream();
    }
}

void CameraWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                   int /*height_px*/) {
    bool was_compact = compact_;
    // Width-only: the live view fills its cell edge-to-edge either way
    // (LV_IMAGE_ALIGN_COVER), so extra height alone never earns the widget
    // a stream it wouldn't otherwise get — only a second column does, same
    // as fan_stack's row layout.
    compact_ = (width_px < widget_size::W_NORMAL);

    // Ensure scale-to-cover after any resize
    if (camera_image_) {
        lv_image_set_inner_align(camera_image_, LV_IMAGE_ALIGN_COVER);
    }

    if (compact_ && !was_compact) {
        // Transitioning to compact: stop stream, show icon-only overlay
        if (!fullscreen_overlay_) {
            stop_stream();
        }
        if (camera_image_) {
            lv_image_set_src(camera_image_, nullptr);
        }
        if (camera_overlay_) {
            lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
        if (camera_status_) {
            lv_obj_add_flag(camera_status_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (!compact_ && was_compact) {
        // Transitioning from compact to larger: show status text, start streaming
        if (camera_status_) {
            lv_obj_remove_flag(camera_status_, LV_OBJ_FLAG_HIDDEN);
        }
        if (active_) {
            start_stream();
        }
    }
}

void CameraWidget::start_stream() {
    ++start_stream_calls_;
    if (stream_ && stream_->is_running()) {
        spdlog::debug("[CameraWidget] start_stream: already running, skipping");
        return;
    }

    // No action needed — lifetime_ is always valid (invalidate() just bumps
    // the generation counter; new tokens from token() are automatically valid).

    stream_ = std::make_unique<CameraStream>();
    std::string stream_url, snapshot_url;
    if (!stream_->configure_from_printer(stream_url, snapshot_url)) {
        spdlog::debug("[CameraWidget] start_stream: no URLs available yet, waiting for observer");
        stream_.reset();
        return;
    }

    // Apply user rotation/flip config (XOR'd with Moonraker values)
    apply_transform();

    // Decode at display resolution instead of full camera resolution.
    // Eliminates expensive LVGL software scaling during rendering.
    auto* disp = lv_display_get_default();
    if (disp) {
        stream_->set_target_size(lv_display_get_horizontal_resolution(disp),
                                 lv_display_get_vertical_resolution(disp));
    }

    // Read target_fps from printer state and apply initial fps gate
    target_fps_ = get_printer_state().get_webcam_target_fps();
    if (target_fps_ <= 0)
        target_fps_ = 15;
    update_stream_fps();

    set_status_text(lv_tr("Connecting Camera..."));

    // Unhide the overlay so the user sees "Connecting Camera..." instead of
    // a bare gray rectangle. The overlay was hidden when the previous stream's
    // first frame arrived — we need to re-show it for the new connection attempt.
    if (camera_overlay_) {
        lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
    }

    // Capture lifetime token by value for safe callback
    auto token = lifetime_.token();

    stream_->start(
        stream_url, snapshot_url,
        [this, token](lv_draw_buf_t* frame) {
            if (token.expired())
                return;

            token.defer("CameraWidget::frame", [this, frame]() {
                // Re-evaluate fps on the UI thread (overlay/edit state is UI-thread only)
                update_stream_fps();

                // Deliver frame to fullscreen image if open, otherwise widget image
                lv_obj_t* target = fullscreen_image_ ? fullscreen_image_ : camera_image_;
                if (target) {
                    lv_image_set_src(target, frame);
                    set_status_text("");
                    // Hide the spinner overlay matching the active view on first frame.
                    // In fullscreen the frame goes to fullscreen_image_, so the fullscreen
                    // overlay's own spinner must be hidden — not the inline widget's.
                    lv_obj_t* spinner = fullscreen_image_ ? fullscreen_spinner_ : camera_overlay_;
                    if (spinner && !lv_obj_has_flag(spinner, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            });
        },
        [this, token](const char* msg) {
            if (token.expired())
                return;

            std::string status(msg);
            token.defer("CameraWidget::status",
                        [this, status]() { set_status_text(status.c_str()); });
        });

    spdlog::info("[CameraWidget] Stream started (stream={}, snapshot={})", stream_url,
                 snapshot_url);
}

void CameraWidget::stop_stream() {
    ++stop_stream_calls_;
    if (!stream_) {
        spdlog::trace("[CameraWidget] stop_stream: no stream to stop");
        return;
    }
    spdlog::debug("[CameraWidget] stop_stream: stopping active stream (active={})", active_);

    // Invalidate lifetime guard FIRST — queued UI callbacks check this and
    // become no-ops, preventing use-after-free on freed draw buffers
    lifetime_.invalidate();

    // Clear image sources before stopping — stop() frees the draw buffers
    // that LVGL may still reference for rendering
    if (lv_is_initialized()) {
        if (fullscreen_image_) {
            lv_image_set_src(fullscreen_image_, nullptr);
        }
        if (camera_image_) {
            lv_image_set_src(camera_image_, nullptr);
        }
        // Wait for any in-flight render to complete. The render thread may
        // still be reading from the old draw buffer even after we cleared the
        // image source — it captured the buffer pointer before our set_src(nullptr).
        // Without this fence, stop() → free_buffers() frees memory the render
        // thread is actively blending from → SIGSEGV in argb8888_image_blend (#749).
        lv_draw_wait_for_finish();
    }

    stream_->stop();

    if (stream_->was_detached()) {
        // Thread still running with a captured `this` — destroying the object
        // would be use-after-free (#624).  Intentionally leak; the detached
        // thread will wind down on its own once the HTTP request completes.
        spdlog::warn(
            "[CameraWidget] Stream thread was detached, leaking CameraStream to avoid UAF");
        (void)stream_.release();
    } else {
        stream_.reset();
    }

    // No action needed — lifetime_ auto-recovers (new tokens from token() are valid)

    spdlog::debug("[CameraWidget] Stream stopped");
}

void CameraWidget::set_config(const nlohmann::json& config) {
    config_ = config;
}

bool CameraWidget::on_edit_configure() {
    spdlog::info("[CameraWidget] Configure requested - showing config modal");
    config_modal_ =
        std::make_unique<CameraConfigModal>(id(), panel_id(), [this](const nlohmann::json& config) {
            config_ = config;
            apply_transform();
        });
    config_modal_->show(lv_screen_active());
    return false;
}

void CameraWidget::apply_transform() {
    if (!stream_)
        return;

    int rotation = 0;
    bool user_flip_h = false;
    bool user_flip_v = false;

    if (config_.contains("rotation") && config_["rotation"].is_number_integer())
        rotation = config_["rotation"].get<int>();
    if (config_.contains("flip_h") && config_["flip_h"].is_boolean())
        user_flip_h = config_["flip_h"].get<bool>();
    if (config_.contains("flip_v") && config_["flip_v"].is_boolean())
        user_flip_v = config_["flip_v"].get<bool>();

    auto cam_rotation = CameraRotation::None;
    switch (rotation) {
    case 90:
        cam_rotation = CameraRotation::Rotate90;
        break;
    case 180:
        cam_rotation = CameraRotation::Rotate180;
        break;
    case 270:
        cam_rotation = CameraRotation::Rotate270;
        break;
    default:
        break;
    }
    stream_->set_rotation(cam_rotation);

    // XOR user flip with Moonraker flip — toggling when Moonraker already flips = undo
    auto& state = get_printer_state();
    bool moonraker_flip_h = state.get_webcam_flip_horizontal();
    bool moonraker_flip_v = state.get_webcam_flip_vertical();
    stream_->set_flip(moonraker_flip_h != user_flip_h, moonraker_flip_v != user_flip_v);

    spdlog::debug("[CameraWidget] Transform: rotation={}, flip_h={} (moon={} ^ user={}), "
                  "flip_v={} (moon={} ^ user={})",
                  rotation, moonraker_flip_h != user_flip_h, moonraker_flip_h, user_flip_h,
                  moonraker_flip_v != user_flip_v, moonraker_flip_v, user_flip_v);
}

void CameraWidget::update_stream_fps() {
    if (!stream_)
        return;

    // Check if overlays are covering us (but NOT our own fullscreen overlay)
    if (!fullscreen_overlay_ && NavigationManager::instance().has_open_overlays()) {
        stream_->set_max_fps(0); // Paused — overlay covering us
        // Schedule periodic re-check so we resume when overlay closes.
        // When paused, no frames deliver → no callback → no re-evaluation.
        if (!fps_recheck_timer_) {
            fps_recheck_timer_ = lv_timer_create(
                [](lv_timer_t* t) {
                    static_cast<CameraWidget*>(lv_timer_get_user_data(t))->update_stream_fps();
                },
                500, this);
        }
        return;
    }

    // Not paused — cancel recheck timer if running
    if (fps_recheck_timer_) {
        lv_timer_delete(fps_recheck_timer_);
        fps_recheck_timer_ = nullptr;
    }

    // Check edit mode
    lv_subject_t* edit_subj = &get_home_edit_mode_subject();
    if (lv_subject_get_int(edit_subj) != 0) {
        stream_->set_max_fps(2); // Reduced — edit mode
        return;
    }

    stream_->set_max_fps(target_fps_); // Normal — capped at configured fps
}

void CameraWidget::set_status_text(const char* text) {
    if (s_subjects_initialized) {
        lv_subject_copy_string(&s_camera_status_subject, text);
    }
}

void CameraWidget::open_fullscreen() {
    if (fullscreen_overlay_ || !parent_screen_) {
        return;
    }

    // In compact mode, the stream isn't running yet — start it now
    if (!stream_) {
        start_stream();
    }
    if (!stream_) {
        return; // No URLs available
    }
    // Only one fullscreen camera at a time across all instances
    if (s_fullscreen_owner) {
        return;
    }

    // Create overlay as child of active screen (standard overlay pattern)
    lv_obj_t* screen = lv_display_get_screen_active(nullptr);
    if (!screen) {
        return;
    }

    auto* overlay = static_cast<lv_obj_t*>(lv_xml_create(screen, "camera_fullscreen", nullptr));
    if (!overlay) {
        spdlog::warn("[CameraWidget] Failed to create camera_fullscreen component");
        return;
    }

    // Start hidden — push_overlay handles showing it
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    fullscreen_overlay_ = overlay;
    fullscreen_image_ = lv_obj_find_by_name(overlay, "fullscreen_camera_image");
    fullscreen_spinner_ = lv_obj_find_by_name(overlay, "fullscreen_spinner");
    s_fullscreen_owner = this;

    if (fullscreen_image_) {
        lv_image_set_inner_align(fullscreen_image_, LV_IMAGE_ALIGN_COVER);

        // Copy the current frame to the fullscreen image immediately
        if (camera_image_) {
            const void* src = lv_image_get_src(camera_image_);
            if (src) {
                lv_image_set_src(fullscreen_image_, src);
            }
        }
    }

    // Register with NavigationManager for lifecycle and cleanup.
    // The close callback handles both explicit close (close button) and
    // NavigationManager-initiated close (backdrop click, back gesture).
    NavigationManager::instance().register_overlay_instance(overlay, nullptr);
    NavigationManager::instance().register_overlay_close_callback(overlay, [this]() {
        // Clear image source before deletion to prevent dangling draw buf reference
        if (fullscreen_image_) {
            lv_image_set_src(fullscreen_image_, nullptr);
        }
        fullscreen_image_ = nullptr;
        fullscreen_spinner_ = nullptr;
        s_fullscreen_owner = nullptr;

        // Delete the overlay widget tree
        helix::ui::safe_delete_obj(fullscreen_overlay_);

        // In compact mode, stop the stream — the widget only shows an icon
        if (compact_) {
            stop_stream();
            if (camera_overlay_) {
                lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
            }
            if (camera_status_) {
                lv_obj_add_flag(camera_status_, LV_OBJ_FLAG_HIDDEN);
            }
        }

        spdlog::debug("[CameraWidget] Fullscreen overlay closed");
    });

    // Fullscreen camera is width-unmanaged: it fills the entire screen via
    // its own XML (width="100%" height="100%") and must NOT go through the
    // standard overlay geometry calculator, which would give it a partial
    // transient width and offset it to one side.
    NavigationManager::instance().set_overlay_width_unmanaged(overlay);
    NavigationManager::instance().push_overlay(overlay);

    spdlog::info("[CameraWidget] Opened fullscreen camera");
}

void CameraWidget::close_fullscreen() {
    if (!fullscreen_overlay_) {
        return;
    }

    // go_back() fires the registered close callback which handles all cleanup
    NavigationManager::instance().go_back();
}

namespace {

// Standalone fullscreen camera viewer — used when there's no owning
// CameraWidget (e.g. Settings → Hardware & Devices → Camera). Owns its own
// CameraStream and is destroyed by the NavigationManager close callback.
struct StandaloneFullscreen {
    lv_obj_t* overlay = nullptr;
    lv_obj_t* image = nullptr;
    lv_obj_t* spinner = nullptr; // "Connecting…" indicator, hidden on first frame
    std::unique_ptr<CameraStream> stream;
    AsyncLifetimeGuard lifetime;
};

static StandaloneFullscreen* s_standalone = nullptr;

} // namespace

void open_standalone_camera_fullscreen(lv_obj_t* parent_screen) {
    if (!parent_screen) {
        return;
    }

    // Single fullscreen at a time across all entry points
    if (s_fullscreen_owner || s_standalone) {
        spdlog::debug(
            "[CameraWidget] Standalone fullscreen open requested, but one is already open");
        return;
    }

    if (!get_printer_state().has_webcam()) {
        spdlog::debug("[CameraWidget] Standalone fullscreen requested, but no webcam configured");
        return;
    }

    // Prefer an existing attached CameraWidget — it already owns a running
    // stream. Opening a second concurrent connection to the same MJPEG
    // endpoint causes one (or both) clients to receive incomplete frames on
    // many printer-side streamers (mjpg-streamer accepts a single client).
    if (!s_attached_widgets.empty()) {
        spdlog::debug("[CameraWidget] Delegating standalone fullscreen to attached widget");
        s_attached_widgets.front()->open_fullscreen();
        return;
    }

    auto state = std::make_unique<StandaloneFullscreen>();
    state->stream = std::make_unique<CameraStream>();

    std::string stream_url, snapshot_url;
    if (!state->stream->configure_from_printer(stream_url, snapshot_url)) {
        spdlog::warn("[CameraWidget] Standalone fullscreen: no webcam URLs available");
        return;
    }

    // Apply Moonraker flips (no per-user transform persisted for the standalone view)
    auto& ps = get_printer_state();
    state->stream->set_flip(ps.get_webcam_flip_horizontal(), ps.get_webcam_flip_vertical());

    if (auto* disp = lv_display_get_default()) {
        state->stream->set_target_size(lv_display_get_horizontal_resolution(disp),
                                       lv_display_get_vertical_resolution(disp));
    }

    int target_fps = ps.get_webcam_target_fps();
    if (target_fps <= 0)
        target_fps = 15;
    state->stream->set_max_fps(target_fps);

    lv_obj_t* screen = lv_display_get_screen_active(nullptr);
    if (!screen) {
        return;
    }

    auto* overlay = static_cast<lv_obj_t*>(lv_xml_create(screen, "camera_fullscreen", nullptr));
    if (!overlay) {
        spdlog::warn("[CameraWidget] Standalone: failed to create camera_fullscreen component");
        return;
    }
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    state->overlay = overlay;
    state->image = lv_obj_find_by_name(overlay, "fullscreen_camera_image");
    state->spinner = lv_obj_find_by_name(overlay, "fullscreen_spinner");
    if (state->image) {
        lv_image_set_inner_align(state->image, LV_IMAGE_ALIGN_COVER);
    }

    s_standalone = state.release();

    auto token = s_standalone->lifetime.token();
    s_standalone->stream->start(
        stream_url, snapshot_url,
        [token](lv_draw_buf_t* frame) {
            if (token.expired())
                return;
            token.defer("StandaloneCameraFullscreen::frame", [frame]() {
                if (!s_standalone || !s_standalone->image)
                    return;
                lv_image_set_src(s_standalone->image, frame);
                // First frame: hide the "Connecting…" spinner
                if (s_standalone->spinner &&
                    !lv_obj_has_flag(s_standalone->spinner, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(s_standalone->spinner, LV_OBJ_FLAG_HIDDEN);
                }
            });
        },
        [token](const char* msg) {
            if (token.expired())
                return;
            std::string status(msg);
            token.defer("StandaloneCameraFullscreen::status", [status]() {
                spdlog::debug("[CameraWidget] Standalone stream status: {}", status);
            });
        });

    NavigationManager::instance().register_overlay_instance(overlay, nullptr);
    NavigationManager::instance().register_overlay_close_callback(overlay, []() {
        if (!s_standalone)
            return;

        // Invalidate lifetime BEFORE clearing image src — queued frame deferrals
        // become no-ops and won't reference freed draw buffers.
        s_standalone->lifetime.invalidate();

        // Clear image src and wait for any in-flight render before stop() frees
        // the draw buffers the render thread may still be blending from (#749).
        if (lv_is_initialized()) {
            if (s_standalone->image) {
                lv_image_set_src(s_standalone->image, nullptr);
            }
            lv_draw_wait_for_finish();
        }

        // Move the stream onto a background worker. CameraStream::stop() joins
        // the stream thread with a 5s timeout — running it inline on the main
        // thread freezes UI input (the close button looked unresponsive #957-ish).
        std::unique_ptr<CameraStream> stream = std::move(s_standalone->stream);
        if (stream) {
            helix::http::HttpExecutor::fast().submit([stream = std::shared_ptr<CameraStream>(
                                                          std::move(stream))]() mutable {
                stream->stop();
                if (stream->was_detached()) {
                    spdlog::warn(
                        "[CameraWidget] Standalone stream thread detached, leaking to avoid UAF");
                    // Intentionally leak: thread still holds raw pointers
                    (void)new std::shared_ptr<CameraStream>(stream);
                }
            });
        }

        helix::ui::safe_delete_obj(s_standalone->overlay);

        delete s_standalone;
        s_standalone = nullptr;
        spdlog::debug("[CameraWidget] Standalone fullscreen closed");
    });

    // Same width-unmanaged marking as CameraWidget::open_fullscreen(): this is
    // the identical camera_fullscreen component, and without this it goes
    // through the overlay geometry calculator and renders at a partial
    // transient width offset to one side.
    NavigationManager::instance().set_overlay_width_unmanaged(overlay);
    NavigationManager::instance().push_overlay(overlay);
    spdlog::info("[CameraWidget] Standalone fullscreen opened (stream={}, snapshot={})", stream_url,
                 snapshot_url);
}

void CameraWidget::destroy_fullscreen() {
    if (!fullscreen_overlay_) {
        return;
    }

    // Synchronous cleanup — used by detach() when 'this' may be destroyed
    // before deferred go_back() callbacks fire.
    if (fullscreen_image_) {
        lv_image_set_src(fullscreen_image_, nullptr);
    }
    fullscreen_image_ = nullptr;
    s_fullscreen_owner = nullptr;

    NavigationManager::instance().unregister_overlay_close_callback(fullscreen_overlay_);
    NavigationManager::instance().unregister_overlay_instance(fullscreen_overlay_);
    helix::ui::safe_delete_obj(fullscreen_overlay_);

    spdlog::debug("[CameraWidget] Fullscreen overlay destroyed (synchronous)");
}

} // namespace helix

#endif // HELIX_HAS_CAMERA
