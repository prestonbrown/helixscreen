// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if HELIX_HAS_CFS

#include "ui_observer_guard.h"

#include "chute_jog_guard.h"
#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <memory>

namespace helix {
enum class JogDirection;
enum class JogMode;
} // namespace helix

namespace helix::ui {

/**
 * @brief Guided purge-chute calibration overlay for the Creality CFS on K1
 * printers (K1 stock command dialect).
 *
 * Walks the stock screen's verified choreography (prestonbrown/helixscreen#1282):
 * full home (XYZ_ZERO), prepare (COORDINATES_ADJUST_PREPARE), jogging to line
 * the toolhead up with the chute, then save (COORDINATES_ADJUST_SAVE_POS +
 * Y_SAFE). The stock screen jogs Y only; X jogging is an addition here, limited
 * by ChuteJogGuard. All gcode goes through AmsBackendCfs; this overlay owns only
 * the step machine and the live readouts.
 *
 * @pattern Overlay (lazy init, singleton via get_cfs_chute_calibration_overlay())
 * @threading Main thread only
 */
class CfsChuteCalibrationOverlay : public OverlayBase {
  public:
    ~CfsChuteCalibrationOverlay() override;

    // === OverlayBase Interface ===

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "CFS Purge Chute Calibration";
    }

    const char* xml_component() const override {
        return "cfs_chute_calibration_overlay";
    }
    /// Freed on close; the next open rebuilds it.
    bool destroy_on_close() const override {
        return true;
    }

    lv_obj_t* create(lv_obj_t* parent) override;

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;

  protected:
    void on_ui_destroyed() override;

  private:
    /// Step machine values published on s_cfs_chute_state. Order matters: the
    /// cancel path treats everything before ADJUSTING as "nothing to unwind".
    enum class State {
        IDLE = 0,
        HOMING = 1,
        ADJUSTING = 2,
        SAVING = 3,
        DONE = 4,
    };

    /// True from a Suspended deactivate until the wake's on_activate.
    bool suspended_ = false;
    ObserverGuard print_takeover_observer_;

    void set_state(State state);
    void start_calibration();
    enum class Axis { X, Y };
    /// True when the jog went out, which is what lets a hold keep repeating.
    bool jog(Axis axis, double delta_mm, bool is_repeat);
    static bool jog_pad_jog_cb(helix::JogDirection direction, float distance_mm, bool is_repeat,
                               void* user_data);
    void set_jog_mode(helix::JogMode mode);
    void fit_jog_pad();
    void save_position();
    void update_x_display(int centimm);
    void update_y_display(int centimm);

    /// Live toolhead X/Y in mm, from the position subjects; drives the readouts
    /// and the jog limits.
    double current_x_mm_ = 0.0;
    bool x_known_ = false;
    double current_y_mm_ = 0.0;
    bool y_known_ = false;

    helix::ChuteJogGuard jog_guard_;
    /// Child of the overlay tree; on_ui_destroyed() clears it.
    lv_obj_t* jog_pad_ = nullptr;

    /// Set the moment the overlay starts leaving; the backend polls it at
    /// each step boundary so a cancel during the home drops the flow instead
    /// of parking the toolhead at the chute in adjust mode.
    std::shared_ptr<std::atomic<bool>> cancel_requested_ =
        std::make_shared<std::atomic<bool>>(false);

    ObserverGuard x_observer_;
    ObserverGuard y_observer_;
    ObserverGuard motion_blocked_observer_;
    SubjectManager subjects_;
};

inline CfsChuteCalibrationOverlay& get_cfs_chute_calibration_overlay() {
    return lazy_global<CfsChuteCalibrationOverlay>("CfsChuteCalibrationOverlay");
}

} // namespace helix::ui

#endif // HELIX_HAS_CFS
