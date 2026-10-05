// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "ui_modal.h"

#include "moonraker_types.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {

/**
 * @brief Configuration modal for the camera panel widget
 *
 * Allows configuring:
 * - Source: which of the printer's webcams this widget shows (Automatic =
 *   the discovery auto-pick, or one camera by name — see webcam_selection.h)
 * - Rotation (0°, 90°, 180°, 270°)
 * - Flip horizontal
 * - Flip vertical
 *
 * Uses C++-owned subjects for reactive XML bindings (per-button selected
 * state, one row per named webcam). Opened from grid edit mode via the gear
 * icon on the camera widget. Keys of the widget config it does not own are
 * carried through a save untouched.
 */
class CameraConfigModal : public Modal {
  public:
    using SaveCallback = std::function<void(const nlohmann::json& config)>;

    CameraConfigModal(const std::string& widget_id, const std::string& panel_id,
                      SaveCallback on_save = nullptr);
    ~CameraConfigModal() override;

    const char* get_name() const override {
        return "Camera Config";
    }
    const char* component_name() const override {
        return "camera_config_modal";
    }

    /// Cameras the source picker can offer; a printer with more lists the first 8.
    static constexpr size_t MAX_SOURCES = 8;
    /// Picker rows: Automatic is row 0, cameras follow.
    static constexpr size_t MAX_ROWS = MAX_SOURCES + 1;

    // Static event callbacks — registered once in register_camera_widget()
    static void on_source_clicked(lv_event_t* e); // user_data: row index, 0 = Automatic
    static void on_rotate_0(lv_event_t* e);
    static void on_rotate_90(lv_event_t* e);
    static void on_rotate_180(lv_event_t* e);
    static void on_rotate_270(lv_event_t* e);
    static void on_flip_h_toggled(lv_event_t* e);
    static void on_flip_v_toggled(lv_event_t* e);

  protected:
    void on_show() override;
    void on_ok() override;

  private:
    void init_subjects();
    void deinit_subjects();
    void sync_rotation_subjects();
    void sync_flip_subjects();
    void sync_source_subjects();

    /// Take the widget's saved config as the editing baseline.
    void load_config(const nlohmann::json& config);
    /// The baseline with this modal's fields applied — what on_ok() saves.
    nlohmann::json build_config() const;
    /// Fill the source rows from the printer's named webcams.
    void publish_sources(const std::vector<WebcamInfo>& cams);
    /// Pick row @p index (0 = Automatic, i = source_names_[i - 1]).
    void select_source(int index);

    std::string widget_id_;
    std::string panel_id_;
    SaveCallback on_save_;
    nlohmann::json base_config_; // saved config; keys not edited here pass through
    int rotation_ = 0;           // 0, 90, 180, 270
    bool flip_h_ = false;
    bool flip_v_ = false;
    std::string source_;                    // webcam name; "" = Automatic
    std::vector<std::string> source_names_; // row index -> webcam name

    // Active instance for static event-callback routing. Only one camera config
    // modal is open at a time (owned by the single CameraWidget). Avoids walking
    // the parent chain for user_data, which can land on a ui_button's own
    // user_data and miscast (L069).
    static CameraConfigModal* s_active_;

    // C++-owned subjects for XML bindings
    bool subjects_initialized_ = false;
    SubjectManager subjects_;
    lv_subject_t rot_0_active_{};
    lv_subject_t rot_90_active_{};
    lv_subject_t rot_180_active_{};
    lv_subject_t rot_270_active_{};
    lv_subject_t flip_h_active_{};
    lv_subject_t flip_v_active_{};
    lv_subject_t source_count_{}; // rows in the picker (Automatic + cameras); drives the <repeat>
    std::array<lv_subject_t, MAX_ROWS> source_active_{}; // 1 on the selected row
    std::array<lv_subject_t, MAX_ROWS> source_name_{};
    std::array<lv_subject_t, MAX_ROWS> source_note_{};
    static constexpr size_t SOURCE_TEXT_LEN = 64;
    std::array<std::array<char, SOURCE_TEXT_LEN>, MAX_ROWS> source_name_buf_{};
    std::array<std::array<char, SOURCE_TEXT_LEN>, MAX_ROWS> source_note_buf_{};

    friend class CameraConfigModalTestAccess;
};

} // namespace helix

#endif // HELIX_HAS_CAMERA
