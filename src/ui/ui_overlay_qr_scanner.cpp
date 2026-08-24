// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_overlay_qr_scanner.cpp
 * @brief Fullscreen QR scanner overlay implementation
 */

#include "ui_overlay_qr_scanner.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "display_settings_manager.h"
#include "i_moonraker_api.h"
#include "printer_state.h"
#include "sound_manager.h"
#include "static_panel_registry.h"
#include "ui/ui_lazy_panel_helper.h"

#if HELIX_HAS_CAMERA
#include "camera_stream.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <thread>

namespace {

// Disable/enable all LVGL keyboard input devices. Barcode scanners appear as
// USB HID keyboards — we suppress LVGL keyboard processing while the scanner
// monitor is active to prevent keystrokes from triggering UI navigation.
void disable_lvgl_keyboards(bool disable) {
    lv_indev_t* indev = nullptr;
    while ((indev = lv_indev_get_next(indev)) != nullptr) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_indev_enable(indev, !disable);
            spdlog::debug("[QR Scanner] {} keyboard indev", disable ? "Disabled" : "Re-enabled");
        }
    }
}

} // namespace

namespace helix::ui {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<QrScannerOverlay> g_qr_scanner_overlay;

QrScannerOverlay& get_qr_scanner_overlay() {
    if (!g_qr_scanner_overlay) {
        g_qr_scanner_overlay = std::make_unique<QrScannerOverlay>();
        StaticPanelRegistry::instance().register_destroy("QrScannerOverlay",
                                                         []() { g_qr_scanner_overlay.reset(); });
    }
    return *g_qr_scanner_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

QrScannerOverlay::QrScannerOverlay() {
#if HELIX_HAS_CAMERA
    qr_decoder_ = std::make_unique<helix::QrDecoder>();
    camera_ = std::make_unique<helix::CameraStream>();
#endif
    spdlog::debug("[{}] Created", get_name());
}

QrScannerOverlay::~QrScannerOverlay() {
    stop_scanning();
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void QrScannerOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, "Point camera at QR code on spool",
                                  "qr_scanner_status", subjects_);
        UI_MANAGED_SUBJECT_INT(success_subject_, 0, "qr_scanner_success", subjects_);
    });
}

void QrScannerOverlay::register_callbacks() {
    lv_xml_register_event_cb(nullptr, "on_qr_scanner_close", on_close_clicked);
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* QrScannerOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    parent_screen_ = parent;
    cleanup_called_ = false;

    // Create fullscreen overlay from XML (not using create_overlay_from_xml since
    // this is a fullscreen overlay, not the standard overlay_panel with header)
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "qr_scanner_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find child widgets by name
    viewfinder_ = lv_obj_find_by_name(overlay_root_, "viewfinder");
    status_text_ = lv_obj_find_by_name(overlay_root_, "status_text");
    success_flash_ = lv_obj_find_by_name(overlay_root_, "success_flash");

    // Scale camera frames to cover the viewfinder area
    if (viewfinder_) {
        lv_image_set_inner_align(viewfinder_, LV_IMAGE_ALIGN_COVER);
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void QrScannerOverlay::on_ui_destroyed() {
    viewfinder_ = nullptr;
    status_text_ = nullptr;
    success_flash_ = nullptr;
    cached_overlay_ = nullptr;
}

// ============================================================================
// SHOW / LIFECYCLE
// ============================================================================

void QrScannerOverlay::show(lv_obj_t* parent, int slot_index, ResultCallback on_result,
                            CancelCallback on_cancel) {
    slot_index_ = slot_index;
    result_callback_ = std::move(on_result);
    cancel_callback_ = std::move(on_cancel);

    spdlog::debug("[{}] show() called, parent={}, cached_overlay_={}", get_name(), fmt::ptr(parent),
                  fmt::ptr(cached_overlay_));

    // Always use the active screen so the overlay renders above modals
    lv_obj_t* screen = lv_screen_active();

    bool ok = lazy_create_and_push_overlay<QrScannerOverlay>(
        get_qr_scanner_overlay, cached_overlay_, screen ? screen : parent, "QR Scanner",
        "QrScannerOverlay", true /* destroy_on_close */);
    if (!ok) {
        spdlog::error("[{}] Failed to show overlay", get_name());
    }

    // Move to front so it renders above any open modals
    if (cached_overlay_) {
        lv_obj_move_foreground(cached_overlay_);
    }
}

void QrScannerOverlay::show_for_active_spool(lv_obj_t* parent, ResultCallback on_result,
                                             CancelCallback on_cancel) {
    show(parent, -1, std::move(on_result), std::move(on_cancel));
}

void QrScannerOverlay::on_activate() {
    OverlayBase::on_activate();

    // Re-lookup viewfinder in case overlay was reused after on_deactivate nulled it
    if (!viewfinder_ && overlay_root_) {
        viewfinder_ = lv_obj_find_by_name(overlay_root_, "viewfinder");
    }

    start_scanning();

    // Auto-close after 60 seconds if no scan result
    struct TimerData {
        helix::LifetimeToken token;
    };
    auto* data = new TimerData{lifetime_.token()};
    timeout_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* d = static_cast<TimerData*>(lv_timer_get_user_data(timer));
            if (!d->token.expired()) {
                auto& overlay = get_qr_scanner_overlay();
                spdlog::info("[{}] Timeout — auto-closing", overlay.get_name());
                overlay.timeout_timer_ = nullptr;
                overlay.stop_scanning();
                if (overlay.cancel_callback_) {
                    overlay.cancel_callback_();
                }
                NavigationManager::instance().go_back();
            }
            delete d;
            lv_timer_delete(timer);
        },
        60000, data);
    lv_timer_set_repeat_count(timeout_timer_, 1);

    spdlog::debug("[{}] Activated", get_name());
}

void QrScannerOverlay::on_deactivate() {
    // Expire all outstanding lifetime tokens FIRST, before any cleanup.
    // Camera BG thread may be mid-frame with a valid token; invalidating
    // early ensures queued lambdas are skipped. (#632)
    lifetime_.invalidate();

    // Clear image source BEFORE stopping scanner — scanner's stop() frees the
    // frame buffers, and LVGL must not hold a dangling pointer to them.
    if (viewfinder_ && lv_is_initialized()) {
        lv_image_set_src(viewfinder_, nullptr);
    }
    viewfinder_ = nullptr;

    stop_scanning();
    snapshot_scanner_.reset();

    // Cancel pending timers
    if (success_timer_) {
        lv_timer_delete(success_timer_);
        success_timer_ = nullptr;
    }
    if (timeout_timer_) {
        lv_timer_delete(timeout_timer_);
        timeout_timer_ = nullptr;
    }

    // Clear callbacks to prevent stale references on reuse
    result_callback_ = nullptr;
    cancel_callback_ = nullptr;

    OverlayBase::on_deactivate();
    spdlog::debug("[{}] Deactivated", get_name());
}

// ============================================================================
// SCANNING
// ============================================================================

void QrScannerOverlay::start_scanning() {
    scan_active_ = true;
    last_decoded_id_ = -1;
    lv_subject_set_int(&success_subject_, 0);
    update_status(lv_tr("Point camera at QR code on spool"));

    // On moving-bed printers, lower the bed to give room for QR scanning
    auto& state = get_printer_state();
    bool bed_moves = lv_subject_get_int(state.get_printer_bed_moves_subject()) != 0;
    const char* homed = lv_subject_get_string(state.get_homed_axes_subject());
    bool z_homed = homed && strchr(homed, 'z') != nullptr;

    if (bed_moves && z_homed) {
        int z_centimm = lv_subject_get_int(state.get_position_z_subject());
        double z_mm = z_centimm / 100.0;
        constexpr double QR_SCAN_Z = 150.0;
        if (z_mm < QR_SCAN_Z) {
            auto* api = get_moonraker_api();
            if (api) {
                spdlog::info("[QR Scanner] Lowering bed from {:.0f}mm to {:.0f}mm for scanning",
                             z_mm, QR_SCAN_Z);
                api->motion().move_to_position('Z', QR_SCAN_Z, 600.0, nullptr, nullptr);
            }
        }
    }

#if HELIX_HAS_CAMERA
    // Start camera if webcam URL is available
    std::string stream_url, snapshot_url;
    if (camera_->configure_from_printer(stream_url, snapshot_url)) {
        decode_busy_ = false;

        // Decode at display resolution — the QR scanner is fullscreen
        auto* disp = lv_display_get_default();
        if (disp) {
            camera_->set_target_size(lv_display_get_horizontal_resolution(disp),
                                     lv_display_get_vertical_resolution(disp));
        }

        auto tok = lifetime_.token();
        camera_->start(
            stream_url, snapshot_url,
            [this, tok](lv_draw_buf_t* frame) {
                if (!tok.expired()) {
                    on_camera_frame(frame);
                }
            },
            [tok](const char* msg) {
                if (!tok.expired()) {
                    spdlog::warn("[QR Scanner] Camera error: {}", msg);
                }
            });
        spdlog::info("[{}] Camera started: stream={}", get_name(), stream_url);
    } else {
        // Camera stream not available — try snapshot fallback
        if (!snapshot_url.empty()) {
            snapshot_scanner_ = std::make_unique<helix::SnapshotQrScanner>();
            auto snap_tok = lifetime_.token();
            snapshot_scanner_->start(
                snapshot_url,
                [this, snap_tok](lv_draw_buf_t* frame) {
                    if (snap_tok.expired())
                        return;
                    on_snapshot_frame(frame);
                },
                [this, snap_tok](int spool_id) {
                    if (snap_tok.expired())
                        return;
                    snap_tok.defer([this, spool_id]() { on_spool_id_detected(spool_id); });
                },
                [snap_tok](const char* msg) {
                    if (!snap_tok.expired()) {
                        spdlog::warn("[QR Scanner] Snapshot error: {}", msg);
                    }
                });
            spdlog::info("[{}] Snapshot QR scanner started (no stream): {}", get_name(),
                         snapshot_url);
        } else {
            update_status(lv_tr("No camera — use USB barcode scanner"));
            spdlog::info("[{}] No webcam URL, USB scanner only", get_name());
        }
    }
#else
    // No compiled camera support — try snapshot polling as fallback
    {
        auto& state = get_printer_state();
        std::string snapshot_url = state.get_webcam_snapshot_url();
        auto* api = get_moonraker_api();
        if (api && !snapshot_url.empty()) {
            api->resolve_webcam_url(snapshot_url);
        }

        if (!snapshot_url.empty()) {
            snapshot_scanner_ = std::make_unique<helix::SnapshotQrScanner>();
            auto tok = lifetime_.token();
            snapshot_scanner_->start(
                snapshot_url,
                [this, tok](lv_draw_buf_t* frame) {
                    if (tok.expired())
                        return;
                    on_snapshot_frame(frame);
                },
                [this, tok](int spool_id) {
                    if (tok.expired())
                        return;
                    tok.defer([this, spool_id]() { on_spool_id_detected(spool_id); });
                },
                [tok](const char* msg) {
                    if (!tok.expired()) {
                        spdlog::warn("[QR Scanner] Snapshot error: {}", msg);
                    }
                });
            spdlog::info("[{}] Snapshot QR scanner started: {}", get_name(), snapshot_url);
        } else {
            update_status(lv_tr("No camera — use USB barcode scanner"));
            spdlog::info("[{}] No webcam URL available, USB scanner only", get_name());
        }
    }
#endif

    // Disable LVGL keyboard input while the scanner monitor is active.
    // Barcode scanners appear as USB HID keyboards — without this, their
    // keystrokes trigger unintended UI navigation.
    disable_lvgl_keyboards(true);

    // Start USB barcode scanner monitor
    auto tok_usb = lifetime_.token();
    usb_monitor_.start([this, tok_usb](int spool_id) {
        if (tok_usb.expired())
            return;
        tok_usb.defer([this, spool_id]() { on_spool_id_detected(spool_id); });
    });
    spdlog::debug("[{}] USB scanner monitor started", get_name());
}

void QrScannerOverlay::stop_scanning() {
    scan_active_ = false;

#if HELIX_HAS_CAMERA
    if (camera_ && camera_->is_running()) {
        camera_->stop();
    }
    // If the stream thread was detached, leaking the CameraStream is safer
    // than destroying it while the thread still holds `this` (#624).
    if (camera_ && camera_->was_detached()) {
        spdlog::warn("[QR Scanner] Stream thread was detached, leaking CameraStream to avoid UAF");
        (void)camera_.release();
    }

    // Wait for any in-flight detached QR decode thread to finish before
    // we return — the decode thread accesses qr_decoder_ which the
    // destructor will free.
    for (int i = 0; i < 50 && decode_busy_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (decode_busy_.load()) {
        spdlog::warn("[QR Scanner] QR decode thread still running after 500ms");
    }
#endif

    if (snapshot_scanner_) {
        snapshot_scanner_->stop();
        snapshot_scanner_.reset();
    }

    if (usb_monitor_.is_running()) {
        usb_monitor_.stop();
    }

    // Re-enable LVGL keyboard input
    disable_lvgl_keyboards(false);

    spdlog::debug("[QR Scanner] Scanning stopped");
}

// ============================================================================
// CAMERA FRAME PROCESSING
// ============================================================================

#if HELIX_HAS_CAMERA
void QrScannerOverlay::on_camera_frame(lv_draw_buf_t* frame) {
    // Called on background camera thread — must return quickly to keep
    // the MJPEG stream flowing at full frame rate.

    if (!scan_active_.load() || !frame) {
        return;
    }

    const int width = static_cast<int>(frame->header.w);
    const int height = static_cast<int>(frame->header.h);
    const auto* pixels = static_cast<const uint8_t*>(frame->data);

    if (!pixels || width <= 0 || height <= 0) {
        return;
    }

    const int stride = static_cast<int>(frame->header.stride);
    const int pixel_size = (frame->header.cf == LV_COLOR_FORMAT_ARGB8888) ? 4 : 3;

    // Subsample during grayscale conversion — QR codes are easily readable
    // at 480px, so processing full 1080p frames wastes ~16x CPU cycles.
    const int max_dim = std::max(width, height);
    const int step = std::max(1, max_dim / QR_MAX_DIMENSION);
    qr_width_ = width / step;
    qr_height_ = height / step;

    const auto buf_size = static_cast<size_t>(qr_width_ * qr_height_);
    grayscale_buf_.resize(buf_size);
    for (int y = 0; y < qr_height_; ++y) {
        const uint8_t* row = pixels + (y * step) * stride;
        for (int x = 0; x < qr_width_; ++x) {
            const uint8_t* px = row + (x * step) * pixel_size;
            grayscale_buf_[static_cast<size_t>(y * qr_width_ + x)] = px[1];
        }
    }

    // Display the frame on the UI thread.
    lifetime_.token().defer("QrScannerOverlay::on_camera_frame", [this, frame]() {
        if (scan_active_.load() && viewfinder_ && lv_obj_is_valid(viewfinder_)) {
            lv_image_set_src(viewfinder_, frame);
        }
    });

    // Run QR decode on a separate thread so the camera stream thread
    // returns immediately and can keep receiving MJPEG frames.
    if (!decode_busy_.exchange(true)) {
        // Copy the subsampled grayscale data for the decode thread
        auto qr_buf = std::make_shared<std::vector<uint8_t>>(grayscale_buf_);
        int qr_w = qr_width_;
        int qr_h = qr_height_;
        auto decode_tok = lifetime_.token();

        // Wrap thread spawn in try/catch — pthread_create EAGAIN on resource-constrained
        // ARM (AD5M/CC1) throws std::system_error which aborts with std::terminate
        // if it escapes an LVGL event frame (#724, #837, [L083]).
        try {
            std::thread([this, qr_buf, qr_w, qr_h, decode_tok]() {
                auto result = qr_decoder_->decode(qr_buf->data(), qr_w, qr_h);
                decode_busy_ = false;

                if (decode_tok.expired())
                    return;

                if (result.success && result.spool_id >= 0) {
                    int spool_id = result.spool_id;
                    decode_tok.defer([this, spool_id]() { on_spool_id_detected(spool_id); });
                }
            }).detach();
        } catch (const std::system_error& e) {
            spdlog::warn("[QrScanner] Failed to spawn decode thread: {} — skipping frame",
                         e.what());
            decode_busy_ = false;
        }
    }
}
#endif

void QrScannerOverlay::on_snapshot_frame(lv_draw_buf_t* frame) {
    if (!scan_active_.load() || !frame) {
        if (snapshot_scanner_)
            snapshot_scanner_->frame_consumed();
        return;
    }

    // The frame_consumed() call used to sit outside the token check, so it read
    // snapshot_scanner_ off `this` whether or not the overlay was still alive.
    // Inside the guard is correct: if the overlay is gone the scanner it owns is
    // gone too, and there is nothing left to tell to continue (#1165).
    lifetime_.token().defer("QrScannerOverlay::on_snapshot_frame", [this, frame]() {
        if (scan_active_.load() && viewfinder_) {
            lv_image_set_src(viewfinder_, frame);
        }
        if (snapshot_scanner_) {
            snapshot_scanner_->frame_consumed();
        }
    });
}

// ============================================================================
// SPOOL LOOKUP
// ============================================================================

void QrScannerOverlay::on_spool_id_detected(int spool_id) {
    // On UI thread
    if (!scan_active_.load()) {
        return;
    }

    // Debounce: skip if same ID detected again
    if (spool_id == last_decoded_id_.load()) {
        return;
    }

    last_decoded_id_ = spool_id;
    spdlog::info("[{}] Spool ID detected: {}", get_name(), spool_id);
    update_status("Looking up spool #" + std::to_string(spool_id) + "...");
    lookup_spool(spool_id);
}

void QrScannerOverlay::lookup_spool(int spool_id) {
    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::error("[{}] No IMoonrakerAPI available", get_name());
        update_status("Error: not connected to printer");
        last_decoded_id_ = -1;
        return;
    }

    auto tok = lifetime_.token();
    api->spoolman().get_spoolman_spool(
        spool_id,
        [this, tok, spool_id](const std::optional<SpoolInfo>& spool) {
            if (tok.expired())
                return;
            tok.defer([this, spool_id, spool]() {
                if (spool.has_value()) {
                    on_spool_found(spool.value());
                } else {
                    spdlog::warn("[{}] Spool #{} not found in Spoolman", get_name(), spool_id);
                    update_status("Spool #" + std::to_string(spool_id) + " not found");
                    last_decoded_id_ = -1; // Allow retry
                }
            });
        },
        [this, tok, spool_id](const MoonrakerError& err) {
            if (tok.expired())
                return;
            tok.defer([this, spool_id, err]() {
                spdlog::error("[{}] Spool lookup failed: {}", get_name(), err.message);
                update_status("Spool #" + std::to_string(spool_id) + " lookup failed");
                last_decoded_id_ = -1; // Allow retry
            });
        });
}

void QrScannerOverlay::on_spool_found(const SpoolInfo& spool) {
    spdlog::info("[{}] Spool found: #{} {} {}", get_name(), spool.id, spool.vendor, spool.material);

    // Clear viewfinder image BEFORE stopping — stop frees the frame buffers
    if (viewfinder_ && lv_is_initialized()) {
        lv_image_set_src(viewfinder_, nullptr);
    }

    // Stop scanning immediately
    stop_scanning();

    // Play success sound
    SoundManager::instance().play("print_complete");

    // Show success flash
    show_success_flash();

    // Fire result callback and close after a brief delay
    struct TimerData {
        ResultCallback callback;
        SpoolInfo spool;
        helix::LifetimeToken token;
    };
    auto* data = new TimerData{result_callback_, spool, lifetime_.token()};

    success_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* d = static_cast<TimerData*>(lv_timer_get_user_data(timer));
            if (!d->token.expired()) {
                auto& overlay = get_qr_scanner_overlay();
                overlay.success_timer_ = nullptr;
                // Copy callback — go_back() will destroy the overlay and
                // invalidate all member pointers
                auto callback = d->callback;
                auto spool = d->spool;
                // Close the overlay properly via navigation (handles backdrop cleanup)
                NavigationManager::instance().go_back();
                // Fire callback AFTER close — the caller (modal) was hidden by
                // go_back but is re-shown by the modal's own show logic
                if (callback) {
                    callback(spool);
                }
            }
            delete d;
            lv_timer_delete(timer);
        },
        500, data);
}

void QrScannerOverlay::show_success_flash() {
    // Show the flash overlay
    lv_subject_set_int(&success_subject_, 1);

    if (!success_flash_)
        return;

    if (DisplaySettingsManager::instance().get_animations_enabled()) {
        // Animate opacity: full white → transparent (flash-bulb effect)
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, success_flash_);
        lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&anim, 400);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
            lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value),
                                    LV_PART_MAIN);
        });
        lv_anim_start(&anim);
    }
}

void QrScannerOverlay::update_status(const std::string& text) {
    // Set the string subject for XML binding
    lv_subject_copy_string(&status_subject_, text.c_str());
    spdlog::debug("[{}] Status: {}", get_name(), text);
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void QrScannerOverlay::on_close_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[QrScannerOverlay] on_close_clicked");

    auto& overlay = get_qr_scanner_overlay();
    spdlog::info("[{}] Close clicked", overlay.get_name());

    overlay.stop_scanning();

    // Fire cancel callback
    if (overlay.cancel_callback_) {
        overlay.cancel_callback_();
    }

    // Navigate back
    NavigationManager::instance().go_back();

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
