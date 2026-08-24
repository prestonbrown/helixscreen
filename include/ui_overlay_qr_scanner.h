// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_overlay_qr_scanner.h
 * @brief Fullscreen QR scanner overlay for spool assignment
 *
 * Displays camera viewfinder with QR detection, and accepts USB barcode
 * scanner input simultaneously. When a Spoolman spool ID is detected,
 * looks up the spool via Moonraker and fires the result callback.
 *
 * @pattern Overlay (lazy init, singleton)
 * @threading Camera/USB callbacks fire on background threads; all LVGL
 *           interaction marshalled via ui_queue_update().
 */

#pragma once

#include "overlay_base.h"
#include "qr_decoder.h"
#include "snapshot_qr_scanner.h"
#include "spoolman_types.h"
#include "subject_managed_panel.h"
#include "usb_scanner_monitor.h"

#include <atomic>
#include <functional>
#include <memory>

// Forward declarations — CameraStream is only available on camera-capable builds
#if HELIX_HAS_CAMERA
namespace helix {
class CameraStream;
}
#endif

namespace helix::ui {

/**
 * @class QrScannerOverlay
 * @brief Fullscreen camera overlay for scanning Spoolman QR codes
 *
 * ## Usage:
 * @code
 * auto& overlay = helix::ui::get_qr_scanner_overlay();
 * overlay.show(parent_screen, slot_index,
 *     [](const SpoolInfo& spool) { // handle result },
 *     []() { // handle cancel });
 * @endcode
 */
class QrScannerOverlay : public OverlayBase {
  public:
    using ResultCallback = std::function<void(const SpoolInfo& spool)>;
    using CancelCallback = std::function<void()>;

    QrScannerOverlay();
    ~QrScannerOverlay() override;

    // Non-copyable
    QrScannerOverlay(const QrScannerOverlay&) = delete;
    QrScannerOverlay& operator=(const QrScannerOverlay&) = delete;

    // OverlayBase interface
    void init_subjects() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void register_callbacks() override;
    const char* get_name() const override {
        return "QR Scanner";
    }
    void on_activate() override;
    void on_deactivate() override;
    void on_ui_destroyed() override;

    /**
     * @brief Show overlay for a specific filament slot
     *
     * @param parent Parent screen for overlay creation
     * @param slot_index AMS/AFC slot index (-1 for active spool)
     * @param on_result Called with spool info on successful scan
     * @param on_cancel Called when user closes without scanning
     */
    void show(lv_obj_t* parent, int slot_index, ResultCallback on_result,
              CancelCallback on_cancel = nullptr);

    /**
     * @brief Show for active spool (single-extruder, no slot)
     */
    void show_for_active_spool(lv_obj_t* parent, ResultCallback on_result,
                               CancelCallback on_cancel = nullptr);

    // Static event callbacks (registered with XML)
    static void on_close_clicked(lv_event_t* e);

  private:
    void start_scanning();
    void stop_scanning();
    void on_spool_id_detected(int spool_id);
    void lookup_spool(int spool_id);
    void on_spool_found(const SpoolInfo& spool);
    void show_success_flash();
    void update_status(const std::string& text);

#if HELIX_HAS_CAMERA
    void on_camera_frame(lv_draw_buf_t* frame);
#endif
    void on_snapshot_frame(lv_draw_buf_t* frame);

    // State
    int slot_index_ = -1;
    ResultCallback result_callback_;
    CancelCallback cancel_callback_;
    std::atomic<bool> scan_active_{false};
    std::atomic<int> last_decoded_id_{-1};

    // Camera + QR
#if HELIX_HAS_CAMERA
    std::unique_ptr<helix::CameraStream> camera_;
    std::unique_ptr<helix::QrDecoder> qr_decoder_;
    std::atomic<bool> decode_busy_{false};
    std::vector<uint8_t> grayscale_buf_; // Subsampled grayscale for QR decode
    int qr_width_ = 0;                   // Dimensions of subsampled QR buffer
    int qr_height_ = 0;

    // Target max dimension for QR decode — QR codes are readable at low
    // resolution, so we subsample to avoid burning CPU on full-res frames.
    static constexpr int QR_MAX_DIMENSION = 480;
#endif
    std::unique_ptr<helix::SnapshotQrScanner> snapshot_scanner_;
    helix::UsbScannerMonitor usb_monitor_;

    // LVGL subjects for XML binding
    SubjectManager subjects_;
    lv_subject_t status_subject_{};
    lv_subject_t success_subject_{};
    char status_buf_[128]{};

    // Cached widget pointers
    lv_obj_t* cached_overlay_ = nullptr;
    lv_obj_t* viewfinder_ = nullptr;
    lv_obj_t* status_text_ = nullptr;
    lv_obj_t* success_flash_ = nullptr;

    // Timers (tracked for cleanup)
    // Neither callback touches `this`. Each owns a LifetimeToken and reaches the
    // overlay through get_qr_scanner_overlay(), so once the AsyncLifetimeGuard
    // member is destroyed the token reports expired and the callback deletes its
    // payload and returns. on_deactivate() still cancels them eagerly; the
    // destructor does not need to.
    lv_timer_t* success_timer_ = nullptr; // TIMER_DTOR_OK: token-guarded, see above
    lv_timer_t* timeout_timer_ = nullptr; // TIMER_DTOR_OK: token-guarded, see above
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton QrScannerOverlay
 */
QrScannerOverlay& get_qr_scanner_overlay();

} // namespace helix::ui
