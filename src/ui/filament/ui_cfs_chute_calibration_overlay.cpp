// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_cfs_chute_calibration_overlay.cpp
 * @brief Guided purge-chute calibration for the Creality CFS (K1 dialect)
 */

#if HELIX_HAS_CFS

#include "ui_cfs_chute_calibration_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_nav.h"
#include "ui_position_utils.h"

#include "ams_backend.h"
#include "ams_cfs_calibration.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "printer_state.h"
#include "unit_conversions.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>

namespace helix::ui {

// The step subject is static (like the z-offset calibration panel's): XML
// bindings resolve it by name from the current scope, and SubjectManager
// deinit on the singleton keeps observers disconnected across rebuilds.
static lv_subject_t s_cfs_chute_state;
static lv_subject_t s_cfs_chute_x_text;
static char s_cfs_chute_x_buf[16];
static lv_subject_t s_cfs_chute_y_text;
static char s_cfs_chute_y_buf[16];
static lv_subject_t s_cfs_chute_saved_text;
static char s_cfs_chute_saved_buf[96];
static lv_subject_t s_cfs_chute_step_idx;

namespace {

/// The active filament system's chute calibration, or nullptr when it has none.
ICfsCalibration* chute_backend() {
    AmsBackend* backend = AmsState::instance().get_backend();
    return backend ? backend->as<ICfsCalibration>() : nullptr;
}

} // namespace

CfsChuteCalibrationOverlay::~CfsChuteCalibrationOverlay() {
    if (subjects_initialized_) {
        subjects_.deinit_all();
    }
}

// ============================================================================
// SUBJECTS / CALLBACKS
// ============================================================================

void CfsChuteCalibrationOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(s_cfs_chute_state, 0, "cfs_chute_state", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_cfs_chute_x_text, s_cfs_chute_x_buf, "0.00 mm",
                                  "cfs_chute_x_display", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_cfs_chute_y_text, s_cfs_chute_y_buf, "0.00 mm",
                                  "cfs_chute_y_display", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_cfs_chute_saved_text, s_cfs_chute_saved_buf, "",
                                  "cfs_chute_saved_pos", subjects_);
        UI_MANAGED_SUBJECT_INT(s_cfs_chute_step_idx, helix::kChuteJogDefaultStepIndex,
                               "cfs_chute_step_idx", subjects_);
    });
}

void CfsChuteCalibrationOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_chute_start_clicked",
         [](lv_event_t*) { get_cfs_chute_calibration_overlay().start_calibration(); }},
        {"on_chute_jog_up_clicked",
         [](lv_event_t*) {
             auto& overlay = get_cfs_chute_calibration_overlay();
             overlay.jog(Axis::Y, overlay.current_step_mm_);
         }},
        {"on_chute_jog_down_clicked",
         [](lv_event_t*) {
             auto& overlay = get_cfs_chute_calibration_overlay();
             overlay.jog(Axis::Y, -overlay.current_step_mm_);
         }},
        {"on_chute_jog_left_clicked",
         [](lv_event_t*) {
             auto& overlay = get_cfs_chute_calibration_overlay();
             overlay.jog(Axis::X, -overlay.current_step_mm_);
         }},
        {"on_chute_jog_right_clicked",
         [](lv_event_t*) {
             auto& overlay = get_cfs_chute_calibration_overlay();
             overlay.jog(Axis::X, overlay.current_step_mm_);
         }},
        {"on_chute_step_clicked",
         [](lv_event_t* e) {
             const char* index_str = static_cast<const char*>(lv_event_get_user_data(e));
             if (index_str) {
                 get_cfs_chute_calibration_overlay().set_step_index(std::atoi(index_str));
             }
         }},
        {"on_chute_save_clicked",
         [](lv_event_t*) { get_cfs_chute_calibration_overlay().save_position(); }},
        // The exit gcode (Y_SAFE after PREPARE) rides on_deactivating(), so the
        // buttons only pop the overlay.
        {"on_chute_cancel_clicked", [](lv_event_t*) { helix::nav::go_back(); }},
        {"on_chute_done_clicked", [](lv_event_t*) { helix::nav::go_back(); }},
    });
}

// ============================================================================
// UI CREATION / LIFECYCLE
// ============================================================================

lv_obj_t* CfsChuteCalibrationOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    // Live toolhead X and Y: the save step writes whatever position the head is
    // in, so the readouts are the values being calibrated.
    auto& ps = get_printer_state();
    x_observer_ = observe<int>(
        ps.motion_state().get_position_x_subject(), this,
        [](CfsChuteCalibrationOverlay* self, int centimm) { self->update_x_display(centimm); },
        ps.get_subjects_lifetime());
    y_observer_ = observe<int>(
        ps.motion_state().get_position_y_subject(), this,
        [](CfsChuteCalibrationOverlay* self, int centimm) { self->update_y_display(centimm); },
        ps.get_subjects_lifetime());
    return overlay_root_;
}

void CfsChuteCalibrationOverlay::on_activate() {
    OverlayBase::on_activate();
    cancel_requested_->store(false);
    // A coarse step left selected from an earlier session must not carry over.
    set_step_index(helix::kChuteJogDefaultStepIndex);
    jog_guard_.disarm();
    set_state(State::IDLE);
}

void CfsChuteCalibrationOverlay::on_deactivating(DeactivateReason reason) {
    // Leaving mid-flow must stop the step chain: the flag first, so a cancel
    // during the home is already visible when the home-done defer runs.
    cancel_requested_->store(true);
    // Leaving while adjusting leaves the toolhead off-park; re-park it. Before
    // ADJUSTING there is nothing to unwind (the home cannot be aborted); during
    // SAVING the park is idempotent, so sending it again is safe.
    if (reason != DeactivateReason::Shutdown) {
        auto* backend = chute_backend();
        if (backend) {
            const int state = lv_subject_get_int(&s_cfs_chute_state);
            if (state == static_cast<int>(State::ADJUSTING) ||
                state == static_cast<int>(State::SAVING)) {
                backend->exit_chute_calibration();
            }
        }
    }
}

// ============================================================================
// STEP MACHINE
// ============================================================================

void CfsChuteCalibrationOverlay::set_state(State state) {
    lv_subject_set_int(&s_cfs_chute_state, static_cast<int>(state));
}

void CfsChuteCalibrationOverlay::start_calibration() {
    auto* backend = chute_backend();
    if (!backend) {
        return;
    }
    set_state(State::HOMING);
    cancel_requested_->store(false);
    auto err = backend->start_chute_calibration(
        [this]() {
            // Armed here and not in set_state(): a refused save also returns to
            // ADJUSTING, with the head possibly past park.
            jog_guard_.arm(lv_tick_get());
            set_state(State::ADJUSTING);
        },
        [this](const std::string& klipper_msg) {
            NOTIFY_ERROR(lv_tr("Purge chute calibration failed: {}"), klipper_msg);
            set_state(State::IDLE);
        },
        cancel_requested_);
    if (!err.success()) {
        set_state(State::IDLE);
        notify_ams_error(err);
    }
}

void CfsChuteCalibrationOverlay::set_step_index(int index) {
    current_step_mm_ = helix::chute_jog_step_mm(index);
    lv_subject_set_int(&s_cfs_chute_step_idx, index);
}

void CfsChuteCalibrationOverlay::jog(Axis axis, double delta_mm) {
    auto* backend = chute_backend();
    if (!backend) {
        return;
    }

    // Both axes must be known on either axis's jog: the first one latches the
    // park position the X limits are measured from.
    const auto bounds = get_printer_state().motion_state().get_axis_bounds();
    if (helix::jog_refused_for_unknown_position(bounds.has_x && bounds.has_y,
                                                x_known_ && y_known_)) {
        NOTIFY_INFO(lv_tr("Toolhead position unknown"));
        return;
    }

    const uint32_t now = lv_tick_get();
    const auto decision =
        axis == Axis::X
            ? jog_guard_.jog_x(delta_mm, now, current_x_mm_, current_y_mm_,
                               static_cast<double>(bounds.x_min), static_cast<double>(bounds.x_max))
            : jog_guard_.jog_y(delta_mm, now, current_x_mm_, current_y_mm_,
                               static_cast<double>(bounds.y_min),
                               static_cast<double>(bounds.y_max));

    switch (decision.outcome) {
    case helix::ChuteJogGuard::Outcome::Move:
        if (axis == Axis::X) {
            backend->jog_chute_x(static_cast<float>(decision.delta));
        } else {
            backend->jog_chute_y(static_cast<float>(decision.delta));
        }
        break;
    case helix::ChuteJogGuard::Outcome::PositionUnknown:
        NOTIFY_INFO(lv_tr("Toolhead position unknown"));
        break;
    case helix::ChuteJogGuard::Outcome::AxisLimit:
        if (decision.warn) {
            NOTIFY_INFO(axis == Axis::X ? lv_tr("X axis limit reached")
                                        : lv_tr("Y axis limit reached"));
        }
        break;
    case helix::ChuteJogGuard::Outcome::PastPark:
        NOTIFY_INFO(lv_tr("Jog Y back to {:.1f} mm before moving X"), jog_guard_.park_y());
        break;
    }
}

void CfsChuteCalibrationOverlay::save_position() {
    auto* backend = chute_backend();
    if (!backend) {
        return;
    }
    set_state(State::SAVING);
    // The done screen names the pair the firmware wrote when the response
    // line was captured, and stays a plain confirmation otherwise. A failed
    // save re-parks in the backend, so the overlay resets to IDLE.
    auto err = backend->save_chute_position(
        [this, backend]() {
            double x = 0.0, y = 0.0;
            const std::string text =
                backend->last_chute_saved_position(x, y)
                    ? fmt::format(lv_tr("Chute position: X {:.1f}, Y {:.1f}"), x, y)
                    : std::string(lv_tr("Purge chute position saved."));
            lv_subject_copy_string(&s_cfs_chute_saved_text, text.c_str());
            set_state(State::DONE);
        },
        [this](const std::string& klipper_msg) {
            NOTIFY_ERROR(lv_tr("Purge chute calibration failed: {}"), klipper_msg);
            set_state(State::IDLE);
        });
    if (!err.success()) {
        set_state(State::ADJUSTING);
        notify_ams_error(err);
    }
}

void CfsChuteCalibrationOverlay::update_x_display(int centimm) {
    current_x_mm_ = helix::units::from_centimm(centimm);
    x_known_ = true;
    helix::ui::position::format_position(centimm, s_cfs_chute_x_buf, sizeof(s_cfs_chute_x_buf));
    lv_subject_copy_string(&s_cfs_chute_x_text, s_cfs_chute_x_buf);
}

void CfsChuteCalibrationOverlay::update_y_display(int centimm) {
    current_y_mm_ = helix::units::from_centimm(centimm);
    y_known_ = true;
    helix::ui::position::format_position(centimm, s_cfs_chute_y_buf, sizeof(s_cfs_chute_y_buf));
    lv_subject_copy_string(&s_cfs_chute_y_text, s_cfs_chute_y_buf);
}

} // namespace helix::ui

#endif // HELIX_HAS_CFS
