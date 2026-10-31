// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if HELIX_HAS_CFS

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <memory>

namespace helix::ui {

/**
 * @brief Guided purge-chute calibration overlay for the Creality CFS on K1
 * printers (K1 stock command dialect).
 *
 * Walks the stock screen's verified choreography (prestonbrown/helixscreen#1282):
 * full home (XYZ_ZERO), prepare (COORDINATES_ADJUST_PREPARE), Y-only jogging
 * to line the toolhead up with the chute, then save (COORDINATES_ADJUST_SAVE_POS
 * + Y_SAFE). All gcode goes through AmsBackendCfs; this overlay owns only the
 * step machine, the live-Y readout and the jog clamping.
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

    void set_state(State state);
    void start_calibration();
    void handle_jog(double delta_mm);
    void handle_jog_y(double delta_mm);
    void handle_jog_x(double delta_mm);
    void jog_up();
    void jog_down();
    void jog_left();
    void jog_right();
    void set_step_size(double step_mm);
    void save_position();
    void update_x_display(int centimm);
    void update_y_display(int centimm);

    /// Live toolhead X/Y in mm, from the position subjects; drives the readout
    /// and the jog clamp.
    double current_x_mm_ = 0.0;
    bool x_known_ = false;
    bool edge_warned_x_ = false;

    double current_y_mm_ = 0.0;
    bool y_known_ = false;
    bool edge_warned_y_ = false;

    double current_step_mm_ = 0.5;

    /// Set the moment the overlay starts leaving; the backend polls it at
    /// each step boundary so a cancel during the home drops the flow instead
    /// of parking the toolhead at the chute in adjust mode.
    std::shared_ptr<std::atomic<bool>> cancel_requested_ =
        std::make_shared<std::atomic<bool>>(false);

    ObserverGuard x_observer_;
    ObserverGuard y_observer_;
    SubjectManager subjects_;
};

inline CfsChuteCalibrationOverlay& get_cfs_chute_calibration_overlay() {
    return lazy_global<CfsChuteCalibrationOverlay>("CfsChuteCalibrationOverlay");
}

} // namespace helix::ui

#endif // HELIX_HAS_CFS
