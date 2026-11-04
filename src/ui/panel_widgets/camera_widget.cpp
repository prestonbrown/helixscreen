// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_widget.h"

#include "ui_panel_common.h"

#include "lvgl.h"
#include "observe_language.h"
#include "ui/ui_widget_helpers.h"

#if HELIX_HAS_CAMERA

#include "ui_nav_manager.h"

#include "app_globals.h"
#include "camera_config_modal.h"
#include "core/update_queue.h"
#include "display_manager.h"
#include "i_moonraker_api.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "printer_state.h"
#include "subject_debug_registry.h"
#include "subject_managed_panel.h"
#include "system/telemetry_manager.h"
#include "translation_loader.h"
#include "ui/ui_cleanup_helpers.h"
#include "widget_subjects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

// Module-level subject for status text binding (static — shared across instances)
static lv_subject_t s_camera_status_subject;
static char s_camera_status_buffer[64];
static helix::WidgetSubjects s_subjects{"CameraWidgetSubjects"};
static const char* const kDefaultCameraStatusKey = TR_NOOP("No Camera");
// The translation key the status text was produced from, so a language switch
// can render it again; nullptr while it shows text that is not a key.
static const char* s_camera_status_key = kDefaultCameraStatusKey;

static void camera_widget_init_subjects() {
    s_subjects.ensure_init([] {
        // The key belongs to the subject's lifetime: a printer switch re-inits
        // the subjects after a frame or stream status has left it nullptr.
        s_camera_status_key = kDefaultCameraStatusKey;
        lv_subject_init_string(&s_camera_status_subject, s_camera_status_buffer, nullptr,
                               sizeof(s_camera_status_buffer), lv_tr(s_camera_status_key));
        s_subjects.publish("camera_status_text", &s_camera_status_subject);
        HELIX_SUBJECT_DEBUG_REGISTER(&s_camera_status_subject, "camera_status_text",
                                     LV_SUBJECT_TYPE_STRING);

        spdlog::debug("[CameraWidget] Subjects initialized");
    });
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
        helix::nav::go_back();
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
    lv_xml_register_event_cb(nullptr, "on_cam_source_clicked",
                             CameraConfigModal::on_source_clicked);
}

void CameraWidget::on_camera_clicked(lv_event_t* e) {
    auto* self = panel_widget_from_event<CameraWidget>(e);
    if (!self)
        return;
    self->record_interaction();
    self->open_fullscreen();
}

CameraWidget::CameraWidget() {
    // Another view taking this camera over; on_activate() starts it again.
    view_.on_preempted([this]() { stop_stream(); });
}

CameraWidget::~CameraWidget() {
    // detach() handles LVGL pointer cleanup, observer release, fullscreen close
    detach();
    // Full cleanup beyond detach: the stream detach() keeps running
    stop_stream();
}

void CameraWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // A raw lv_obj_delete() of the home page container frees this tile without
    // any detach() call; without the hook the cached child pointers below
    // dangle while the running MJPEG stream keeps deferring frames into them.
    install_delete_hook(widget_obj);

    camera_image_ = lv_obj_find_by_name(widget_obj_, "camera_image");
    camera_overlay_ = lv_obj_find_by_name(widget_obj_, "camera_overlay");
    camera_status_ = lv_obj_find_by_name(widget_obj_, "camera_status");

    language_observer_ = helix::ui::observe_language_change(this, [](CameraWidget* self) {
        if (s_camera_status_key) {
            self->set_status_key(s_camera_status_key);
        }
    });

    if (std::find(s_attached_widgets.begin(), s_attached_widgets.end(), this) ==
        s_attached_widgets.end()) {
        s_attached_widgets.push_back(this);
    }

    // Observe printer_has_webcam — URLs may not be available yet at attach
    // time (Moonraker sends webcam list asynchronously). When the subject
    // transitions to 1, try starting the stream if we're active.
    lv_subject_t* gate = lv_xml_get_subject(nullptr, "printer_has_webcam");
    if (gate) {
        webcam_observer_ = helix::ui::observe<int>(
            gate, this,
            [](CameraWidget* self, int val) {
                if (val > 0) {
                    if (self->compact_) {
                        // Compact mode: icon only, status text already hidden
                    } else {
                        self->set_status_key(TR_NOOP("Connecting Camera..."));
                        if (self->active_) {
                            self->start_stream();
                        }
                    }
                } else {
                    // Only stop if the stream isn't being actively displayed.
                    // observe<int> defers via queue_update(), so this callback can
                    // fire AFTER on_activate() already restarted the stream — killing it
                    // and leaving a gray rectangle. Also skip if fullscreen overlay is
                    // open — the stream must keep running for the fullscreen view.
                    if (!self->active_ && !self->fullscreen_overlay_) {
                        self->stop_stream();
                        self->set_status_key(kDefaultCameraStatusKey);
                    }
                }
            },
            get_printer_state().get_subjects_lifetime());
    }

    // A change in the list itself — has_webcam may stay 1 while the camera
    // this widget is configured to show appears or disappears.
    lv_subject_t* list = lv_xml_get_subject(nullptr, "webcam_count");
    if (list) {
        webcam_list_observer_ = helix::ui::observe<int>(
            list, this, [](CameraWidget* self, int /*count*/) { self->resync_source(); },
            get_printer_state().get_subjects_lifetime());
    }

    spdlog::debug("[CameraWidget] Attached");
}

void CameraWidget::detach() {
    // No early return on a null widget_obj_. on_hooked_root_deleted() nulls the
    // tile pointers when the tree dies raw, and the destructor's detach() is
    // then the only path left that reaches the fps timer, the observers and the
    // fullscreen overlay - so every step guards its own pointer instead, the
    // same shape ThermistorWidget::detach() uses.

    // Synchronously destroy fullscreen overlay. Cannot use close_fullscreen()
    // because go_back() is deferred and 'this' may be destroyed before it fires.
    destroy_fullscreen();

    // Lightweight detach: release observers and LVGL pointers but preserve
    // the camera stream. The stream token is intentionally NOT expired here
    // (unlike other widgets) — the stream keeps running during the
    // detach→reattach gap. Frame callbacks check camera_image_ (null
    // after detach) and safely no-op until re-attach.
    webcam_observer_.reset();
    webcam_list_observer_.reset();
    edit_mode_observer_.reset();
    language_observer_.reset();
    if (fps_recheck_timer_) {
        lv_timer_delete(fps_recheck_timer_);
        fps_recheck_timer_ = nullptr;
    }

    // Off before the caller condemns this tree: every detach() call site hands
    // the container to safe_clean_children() straight after, and that late
    // delete must not fire on_hooked_root_deleted() into a widget that has
    // since been re-attached.
    uninstall_delete_hook();

    forget_tile_widgets();

    auto it = std::find(s_attached_widgets.begin(), s_attached_widgets.end(), this);
    if (it != s_attached_widgets.end()) {
        s_attached_widgets.erase(it);
    }

    spdlog::debug("[CameraWidget] Detached (stream preserved)");
}

void CameraWidget::on_hooked_root_deleted() {
    // Runs inside LVGL's delete event for the tile: drop the cached tile
    // pointers ONLY.
    //
    // Do NOT expire the stream token (view_.stream_token()) here. It guards the MJPEG frame and
    // status deferrals of a stream that outlives the tree, and a token the running stream captured
    // at start_stream() never recovers from an invalidate(): LifetimeToken::expired() compares its
    // snapshot against the bumped generation, so every later frame is dropped for the life of that
    // stream. The fullscreen overlay is a child of the screen, not of the tile, so a page-container
    // delete leaves it live and still being fed - invalidating here freezes a fullscreen view the
    // user is watching.
    //
    // Dropping camera_image_ is what makes the deferrals safe instead: the
    // frame lambda resolves its target as
    // `fullscreen_image_ ? fullscreen_image_ : camera_image_` and no-ops on a
    // null target, so a frame arriving after the tile died writes nothing.
    forget_tile_widgets();
}

void CameraWidget::forget_tile_widgets() {
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    camera_image_ = nullptr;
    camera_overlay_ = nullptr;
    camera_status_ = nullptr;
}

void CameraWidget::on_activate() {
    spdlog::debug("[CameraWidget] on_activate (was active={})", active_);
    active_ = true;

    // Suspend the camera stream while the screen is off (saves ~20% CPU on Pi).
    // The handler lives in view_, so it dies with this widget.
    view_.on_display_sleep([this](bool sleeping) {
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

    // Observe edit mode changes to throttle camera fps
    if (!edit_mode_observer_) {
        lv_subject_t* edit_subj = &get_home_edit_mode_subject();
        edit_mode_observer_ = helix::ui::observe<int>(
            edit_subj, this, [](CameraWidget* self, int /*val*/) { self->update_stream_fps(); },
            get_app_globals_subjects_lifetime());
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
    compact_ = (width_px < widget_size::w_normal());

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
    if (view_.is_running()) {
        spdlog::debug("[CameraWidget] start_stream: already running, skipping");
        return;
    }

    const bool started = view_.start(
        configured_source(), config_, {fullscreen_image_, camera_image_},
        [this](lv_draw_buf_t* frame) {
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
        },
        [this](const std::string& status) { set_status_text(status.c_str()); });
    if (!started) {
        spdlog::debug("[CameraWidget] start_stream: no URLs available yet, waiting for observer");
        return;
    }
    update_stream_fps();

    set_status_key(TR_NOOP("Connecting Camera..."));

    // Unhide the overlay so the user sees "Connecting Camera..." instead of
    // a bare gray rectangle. The overlay was hidden when the previous stream's
    // first frame arrived — we need to re-show it for the new connection attempt.
    if (camera_overlay_) {
        lv_obj_remove_flag(camera_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CameraWidget::stop_stream() {
    ++stop_stream_calls_;
    if (!view_.has_stream()) {
        spdlog::trace("[CameraWidget] stop_stream: no stream to stop");
        return;
    }
    spdlog::debug("[CameraWidget] stop_stream: stopping active stream (active={})", active_);
    view_.stop({fullscreen_image_, camera_image_});
}

void CameraWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    resync_source();
}

std::string CameraWidget::configured_source() const {
    if (config_.contains("source") && config_["source"].is_string())
        return config_["source"].get<std::string>();
    return "";
}

void CameraWidget::resync_source() {
    if (!view_.has_stream())
        return; // Nothing running: start_stream() reads the config when it does
    auto feed = CameraStream::resolve_from_printer(configured_source());
    if (feed && feed->stream_url == view_.feed().stream_url &&
        feed->snapshot_url == view_.feed().snapshot_url) {
        return;
    }
    spdlog::info("[CameraWidget] Camera feed changed to '{}' — restarting stream",
                 !feed ? "<none>" : (feed->name.empty() ? "<auto>" : feed->name));
    bool keep_running = fullscreen_overlay_ || (active_ && !compact_);
    stop_stream();
    if (!feed) {
        set_status_key(kDefaultCameraStatusKey);
        return;
    }
    if (keep_running) {
        start_stream();
    }
}

bool CameraWidget::on_edit_configure() {
    spdlog::info("[CameraWidget] Configure requested - showing config modal");
    config_modal_ =
        std::make_unique<CameraConfigModal>(id(), panel_id(), [this](const nlohmann::json& config) {
            config_ = config;
            apply_transform();
            resync_source();
        });
    config_modal_->show(lv_screen_active());
    return false;
}

void CameraWidget::apply_transform() {
    view_.apply_transform(config_);
}

void CameraWidget::update_stream_fps() {
    if (!view_.has_stream())
        return;

    // Check if overlays are covering us (but NOT our own fullscreen overlay)
    if (!fullscreen_overlay_ && NavigationManager::instance().has_open_overlays()) {
        view_.set_max_fps(0); // Paused — overlay covering us
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
        view_.set_max_fps(2); // Reduced — edit mode
        return;
    }

    view_.set_max_fps(view_.target_fps()); // Normal — capped at configured fps
}

void CameraWidget::set_status_text(const char* text) {
    s_camera_status_key = nullptr;
    if (s_subjects.initialized()) {
        lv_subject_copy_string(&s_camera_status_subject, text);
    }
}

void CameraWidget::set_status_key(const char* key) {
    set_status_text(lv_tr(key));
    s_camera_status_key = key;
}

void CameraWidget::open_fullscreen() {
    if (fullscreen_overlay_ || !parent_screen_) {
        return;
    }

    // In compact mode, the stream isn't running yet — start it now
    if (!view_.has_stream()) {
        start_stream();
    }
    if (!view_.has_stream()) {
        return; // No URLs available
    }
    show_fullscreen_overlay();
}

void CameraWidget::show_fullscreen_overlay() {
    // Only one fullscreen camera at a time across all instances
    if (s_fullscreen_owner) {
        return;
    }

    // Create overlay as child of active screen (standard overlay pattern)
    lv_obj_t* screen = lv_display_get_screen_active(nullptr);
    if (!screen) {
        return;
    }

    // Hidden until push_overlay shows it
    auto* overlay = helix::ui::create_xml_hidden(screen, "camera_fullscreen");
    if (!overlay) {
        spdlog::warn("[CameraWidget] Failed to create camera_fullscreen component");
        return;
    }

    fullscreen_overlay_ = overlay;
    fullscreen_image_ =
        helix::ui::find_required(overlay, "fullscreen_camera_image", "CameraWidget");
    fullscreen_spinner_ = helix::ui::find_required(overlay, "fullscreen_spinner", "CameraWidget");
    s_fullscreen_owner = this;
    lv_obj_add_event_cb(overlay, on_fullscreen_overlay_deleted, LV_EVENT_DELETE, nullptr);

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
    helix::nav::register_overlay(overlay, nullptr);
    helix::nav::on_close(overlay, [this]() {
        // Clear image source before deletion to prevent dangling draw buf reference
        if (fullscreen_image_) {
            lv_image_set_src(fullscreen_image_, nullptr);
        }
        fullscreen_image_ = nullptr;
        fullscreen_spinner_ = nullptr;
        // A raw overlay delete frees the slot before this queued close runs, so
        // another widget may own it by now.
        if (s_fullscreen_owner == this) {
            s_fullscreen_owner = nullptr;
        }

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
    helix::nav::set_overlay_width_unmanaged(overlay);
    helix::nav::push_overlay(overlay);

    spdlog::info("[CameraWidget] Opened fullscreen camera");
}

void CameraWidget::on_fullscreen_overlay_deleted(lv_event_t* e) {
    // Resolved through s_fullscreen_owner rather than user data: every
    // deliberate close clears the owner before deleting the overlay, so only an
    // unplanned delete reaches the body, and a widget destroyed first is never
    // dereferenced.
    CameraWidget* owner = s_fullscreen_owner;
    if (!owner || owner->fullscreen_overlay_ != lv_event_get_target(e)) {
        return;
    }
    owner->fullscreen_overlay_ = nullptr;
    owner->fullscreen_image_ = nullptr;
    owner->fullscreen_spinner_ = nullptr;
    s_fullscreen_owner = nullptr;
    spdlog::debug("[CameraWidget] Fullscreen overlay deleted without a close");
}

void CameraWidget::close_fullscreen() {
    if (!fullscreen_overlay_) {
        return;
    }

    // go_back() fires the registered close callback which handles all cleanup
    helix::nav::go_back();
}

StandaloneCameraViewer* StandaloneCameraViewer::s_open_ = nullptr;

StandaloneCameraViewer::StandaloneCameraViewer() {
    // Another view taking this camera over. Stopping clears the image, so the
    // viewer stays blank until it is closed; nothing restarts the stream here.
    view_.on_preempted([this]() { view_.stop({image_}); });
}

void StandaloneCameraViewer::close() {
    StandaloneCameraViewer* viewer = std::exchange(s_open_, nullptr);
    if (!viewer) {
        return;
    }

    // Clears the image and waits out the render thread before the frame
    // buffers go, and joins the stream off the main thread.
    viewer->view_.stop({viewer->image_});

    // Taken out first: deleting it fires on_overlay_deleted(), which must see
    // this close already under way.
    lv_obj_t* overlay = std::exchange(viewer->overlay_, nullptr);
    helix::ui::safe_delete_obj(overlay);

    delete viewer;
    spdlog::debug("[CameraWidget] Standalone fullscreen closed");
}

void StandaloneCameraViewer::on_overlay_deleted(lv_event_t* e) {
    if (!s_open_ || s_open_->overlay_ != lv_event_get_target(e)) {
        return;
    }
    s_open_->overlay_ = nullptr;
    s_open_->image_ = nullptr;
    s_open_->spinner_ = nullptr;
    close();
}

bool StandaloneCameraViewer::open(lv_obj_t* parent_screen) {
    if (!parent_screen) {
        return false;
    }

    // Single fullscreen at a time across all entry points
    if (s_fullscreen_owner || s_open_) {
        spdlog::debug(
            "[CameraWidget] Standalone fullscreen open requested, but one is already open");
        return false;
    }

    if (!get_printer_state().has_webcam()) {
        spdlog::debug("[CameraWidget] Standalone fullscreen requested, but no webcam configured");
        return false;
    }

    // Prefer an existing attached CameraWidget: its fullscreen shows the
    // stream it already owns.
    if (!s_attached_widgets.empty()) {
        spdlog::debug("[CameraWidget] Delegating standalone fullscreen to attached widget");
        s_attached_widgets.front()->open_fullscreen();
        return s_fullscreen_owner != nullptr;
    }

    lv_obj_t* screen = lv_display_get_screen_active(nullptr);
    if (!screen) {
        return false;
    }

    auto* overlay = helix::ui::create_xml_hidden(screen, "camera_fullscreen");
    if (!overlay) {
        spdlog::warn("[CameraWidget] Standalone: failed to create camera_fullscreen component");
        return false;
    }

    auto* viewer = new StandaloneCameraViewer();
    viewer->overlay_ = overlay;
    viewer->image_ = helix::ui::find_required(overlay, "fullscreen_camera_image", "CameraWidget");
    viewer->spinner_ = helix::ui::find_required(overlay, "fullscreen_spinner", "CameraWidget");
    if (viewer->image_) {
        lv_image_set_inner_align(viewer->image_, LV_IMAGE_ALIGN_COVER);
    }

    // No per-user transform is persisted for this view: the camera's own flips.
    const bool started = viewer->view_.start(
        "", nlohmann::json::object(), {viewer->image_},
        [viewer](lv_draw_buf_t* frame) {
            if (!viewer->image_) {
                return;
            }
            lv_image_set_src(viewer->image_, frame);
            // First frame: hide the "Connecting..." spinner
            if (viewer->spinner_ && !lv_obj_has_flag(viewer->spinner_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(viewer->spinner_, LV_OBJ_FLAG_HIDDEN);
            }
        },
        [](const std::string& status) {
            spdlog::debug("[CameraWidget] Standalone stream status: {}", status);
        });
    if (!started) {
        spdlog::warn("[CameraWidget] Standalone fullscreen: no webcam URLs available");
        delete viewer;
        helix::ui::safe_delete_obj(overlay);
        return false;
    }
    s_open_ = viewer;

    helix::nav::register_overlay(overlay, nullptr);
    lv_obj_add_event_cb(overlay, on_overlay_deleted, LV_EVENT_DELETE, nullptr);
    // The close is queued; a stale one must not shut a viewer opened since.
    helix::nav::on_close(overlay, [overlay]() {
        if (s_open_ && s_open_->overlay_ == overlay) {
            close();
        }
    });

    // Same width-unmanaged marking as CameraWidget::open_fullscreen(): this is
    // the identical camera_fullscreen component, and without this it goes
    // through the overlay geometry calculator and renders at a partial
    // transient width offset to one side.
    helix::nav::set_overlay_width_unmanaged(overlay);
    helix::nav::push_overlay(overlay);
    spdlog::info("[CameraWidget] Standalone fullscreen opened (stream={}, snapshot={})",
                 viewer->view_.feed().stream_url, viewer->view_.feed().snapshot_url);
    return true;
}

bool open_standalone_camera_fullscreen(lv_obj_t* parent_screen) {
    return StandaloneCameraViewer::open(parent_screen);
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
    fullscreen_spinner_ = nullptr;
    s_fullscreen_owner = nullptr;

    helix::nav::clear_on_close(fullscreen_overlay_);
    helix::nav::unregister_overlay(fullscreen_overlay_);
    helix::ui::safe_delete_obj(fullscreen_overlay_);

    spdlog::debug("[CameraWidget] Fullscreen overlay destroyed (synchronous)");
}

} // namespace helix

#endif // HELIX_HAS_CAMERA
