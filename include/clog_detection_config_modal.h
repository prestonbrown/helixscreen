// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "ams_types.h"
#include "lvgl/lvgl.h"

#include <optional>
#include <string>

namespace helix {
class AmsBackend;
}

/**
 * @brief Configuration modal for the clog detection panel widget
 *
 * Allows configuring:
 * - Detection source (Auto/Encoder/Flowguard/AFC)
 * - Detection mode (Auto/Manual) — sent to firmware via gcode
 * - Detection length (manual mode) — filament distance before clog
 * - Danger threshold — overrides computed danger zone percentage
 *
 * Uses C++-owned subjects for reactive XML bindings:
 * - clog_cfg_mode (int): detection mode, drives disabled state on det length slider
 * - clog_cfg_threshold_text (string): "Default" or "75%", drives label bind_text
 * - clog_cfg_det_length_text (string): "10mm", drives label bind_text
 * - clog_cfg_mode_supported (int): 1 when the active AMS backend understands the
 *   detection-mode gcode, 0 otherwise — drives hidden on the mode/length controls
 *
 * Detection mode and detection length are written with whatever command the
 * active backend names (AmsBackend::clog_detection_mode_gcode). The widget
 * itself is offered whenever clog_meter_mode > 0, which includes AFC buffer
 * fault detection (mode 3), so the modal is reachable on backends with no such
 * setting; they name none and the controls hide.
 *
 * Opened from grid edit mode via the gear icon on the clog detection widget.
 */
class ClogDetectionConfigModal : public Modal {
  public:
    ClogDetectionConfigModal(const std::string& widget_id, const std::string& panel_id);
    ~ClogDetectionConfigModal() override;

    /**
     * @brief Build the detection-mode gcode for a backend, if it has one.
     *
     * The backend answers (AmsBackend::clog_detection_mode_gcode); one with no
     * detection-mode setting returns nullopt so the caller sends nothing.
     *
     * @param backend    Active AMS backend, or nullptr
     * @param mode       0=off, 1=manual, 2=auto
     * @param det_length Detection length in mm (only used in manual mode)
     * @return The gcode to send, or nullopt when the backend has no equivalent
     */
    [[nodiscard]] static std::optional<std::string>
    build_detection_mode_gcode(const helix::AmsBackend* backend, int mode, float det_length);

    /**
     * @brief What Save sends: nothing unless the mode changed or, in manual
     *        mode, the length moved; the length only when it moved.
     */
    [[nodiscard]] static std::optional<std::string>
    detection_save_gcode(const helix::AmsBackend* backend, int mode, int original_mode,
                         float det_length, float original_det_length);

    const char* get_name() const override {
        return "Clog Detection Config";
    }
    const char* component_name() const override {
        return "clog_detection_config_modal";
    }

  protected:
    void on_show() override;
    void on_ok() override;

  private:
    void init_subjects();
    void deinit_subjects();
    void sync_source_subjects();
    void sync_mode_subjects();
    void sync_threshold_text();
    void sync_det_length_text();
    void send_detection_mode_gcode(const std::string& cmd, int mode);
    void update_source_visibility();

    static void on_source_auto(lv_event_t* e);
    static void on_source_encoder(lv_event_t* e);
    static void on_source_flowguard(lv_event_t* e);
    static void on_source_afc(lv_event_t* e);
    static void on_mode_auto(lv_event_t* e);
    static void on_mode_manual(lv_event_t* e);
    static void on_threshold_changed(lv_event_t* e);
    static void on_det_length_changed(lv_event_t* e);

    std::string widget_id_;
    std::string panel_id_;
    int source_ = 0;             // 0=auto, 1=encoder, 2=flowguard, 3=afc
    int detection_mode_ = 2;     // 0=off, 1=manual, 2=auto
    int danger_threshold_ = 0;   // 0=use computed default
    float detection_length_ = 0; // mm, from firmware (used for manual mode)
    int original_detection_mode_ = 2;
    float original_detection_length_ = 0; // mm, as on_show() filled it
    bool has_encoder_ = false;
    bool has_flowguard_ = false;
    bool has_afc_ = false;

    // C++-owned subjects for XML bindings (lifetime = modal lifetime)
    bool subjects_initialized_ = false;
    SubjectManager subjects_;
    lv_subject_t mode_subject_{};           // int: detection mode (1=manual, 2=auto)
    lv_subject_t threshold_text_subject_{}; // string: "Default" or "75%"
    // "Default" renders translated; ru "По умолчанию" is 23 bytes.
    char threshold_text_buf_[24]{};
    lv_subject_t det_length_text_subject_{}; // string: "10mm"
    char det_length_text_buf_[16]{};
    lv_subject_t mode_supported_subject_{}; // int: 1=backend takes detection-mode gcode

    // Per-button boolean subjects for bind_style (selected/unselected)
    lv_subject_t src_auto_active_{};
    lv_subject_t src_encoder_active_{};
    lv_subject_t src_flowguard_active_{};
    lv_subject_t src_afc_active_{};
    lv_subject_t mode_auto_active_{};
    lv_subject_t mode_manual_active_{};
};
