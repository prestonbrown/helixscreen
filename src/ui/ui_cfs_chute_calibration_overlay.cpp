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
#include "ams_state.h"
#include "app_globals.h"
#include "jog_coalescer.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "unit_conversions.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>
#include <memory>

namespace helix::ui {

// The step subject is static (like the z-offset calibration panel's): XML
// bindings resolve it by name from the current scope, and SubjectManager
// deinit on the singleton keeps observers disconnected across rebuilds.
static lv_subject_t s_cfs_chute_state;
static lv_subject_t s_cfs_chute_y_text;
static char s_cfs_chute_y_buf[16];
static lv_subject_t s_cfs_chute_saved_text;
static char s_cfs_chute_saved_buf[96];

namespace {

/// The active filament system, or nullptr when there is none. Backends without chute
/// calibration answer every call with not_supported.
AmsBackend* chute_backend() {
    return AmsState::instance().get_backend();
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
        UI_MANAGED_SUBJECT_STRING(s_cfs_chute_y_text, s_cfs_chute_y_buf, "0.00 mm",
                                  "cfs_chute_y_display", subjects_);
        UI_MANAGED_SUBJECT_STRING(s_cfs_chute_saved_text, s_cfs_chute_saved_buf, "",
                                  "cfs_chute_saved_pos", subjects_);
    });
}

void CfsChuteCalibrationOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_chute_start_clicked",
         [](lv_event_t*) { get_cfs_chute_calibration_overlay().start_calibration(); }},
        // user_data carries the jog delta as a string
        {"on_chute_jog_clicked",
         [](lv_event_t* e) {
             const char* delta_str = static_cast<const char*>(lv_event_get_user_data(e));
             if (delta_str) {
                 get_cfs_chute_calibration_overlay().handle_jog(std::atof(delta_str));
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

    // Live toolhead Y: the save step writes whatever position the head is in,
    // so the readout is the value being calibrated.
    auto& ps = get_printer_state();
    print_takeover_observer_ = observe<int>(
        ps.print_state().get_machine_motion_blocked_subject(), this,
        [](CfsChuteCalibrationOverlay* self, int blocked) {
            // A print owns the machine now, and nothing in the session still holds
            // once it ends: the head will not be where the flow left it.
            if (blocked != 0 &&
                lv_subject_get_int(&s_cfs_chute_state) == static_cast<int>(State::ADJUSTING)) {
                NOTIFY_INFO(lv_tr("Printer is busy with a print"));
                self->set_state(State::IDLE);
            }
        },
        ps.get_subjects_lifetime());
    y_observer_ = observe<int>(
        ps.motion_state().get_position_y_subject(), this,
        [](CfsChuteCalibrationOverlay* self, int centimm) { self->update_y_display(centimm); },
        ps.get_subjects_lifetime());
    return overlay_root_;
}

void CfsChuteCalibrationOverlay::on_activate() {
    OverlayBase::on_activate();
    if (suspended_) {
        // A wake, not a fresh open: the session carries on where it was.
        suspended_ = false;
        return;
    }
    set_state(State::IDLE);
}

void CfsChuteCalibrationOverlay::on_deactivating(DeactivateReason reason) {
    // Nobody walked away from a blanked screen: the head stays where it is, and
    // a home under way carries on.
    if (reason == DeactivateReason::Suspended) {
        suspended_ = true;
        return;
    }
    suspended_ = false;
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
    if (state == State::ADJUSTING) {
        edge_warned_y_ = false;
    }
}

void CfsChuteCalibrationOverlay::start_calibration() {
    auto* backend = chute_backend();
    if (!backend) {
        return;
    }
    set_state(State::HOMING);
    // One flag per run: a chain still waiting on its home keeps the one it was
    // cancelled with, so no later session can revive it.
    cancel_requested_ = std::make_shared<std::atomic<bool>>(false);
    auto err = backend->start_chute_calibration(
        [this]() { set_state(State::ADJUSTING); },
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

void CfsChuteCalibrationOverlay::handle_jog(double delta_mm) {
    auto* backend = chute_backend();
    if (!backend) {
        return;
    }

    // Soft-stop against the live envelope. The jog script's M400 means each
    // RPC returns with the move finished, so there is no uncommitted travel
    // to fold in (unlike the motion panel's coalescer).
    double delta = delta_mm;
    const auto bounds = get_printer_state().motion_state().get_axis_bounds();
    if (helix::jog_refused_for_unknown_position(bounds.has_y, y_known_)) {
        // With no envelope or no live position there is nothing to clamp
        // against, so an unclamped jog must not go out.
        NOTIFY_INFO(lv_tr("Toolhead position unknown"));
        return;
    }
    const auto result =
        helix::clamp_jog_with_warn(current_y_mm_, 0.0, delta, static_cast<double>(bounds.y_min),
                                   static_cast<double>(bounds.y_max), edge_warned_y_);
    edge_warned_y_ = result.latch;
    delta = result.allowed;
    if (result.warn) {
        NOTIFY_INFO(lv_tr("Y axis limit reached"));
    }
    if (std::abs(delta) <= helix::AxisMove::kEpsilonMm) {
        return;
    }
    backend->jog_chute_y(static_cast<float>(delta));
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
        // Refused because a print holds the machine or homing was lost: either
        // way the session has to start over.
        set_state(State::IDLE);
        notify_ams_error(err);
    }
}

void CfsChuteCalibrationOverlay::update_y_display(int centimm) {
    current_y_mm_ = helix::units::from_centimm(centimm);
    y_known_ = true;
    helix::ui::position::format_position(centimm, s_cfs_chute_y_buf, sizeof(s_cfs_chute_y_buf));
    lv_subject_copy_string(&s_cfs_chute_y_text, s_cfs_chute_y_buf);
}

} // namespace helix::ui

#endif // HELIX_HAS_CFS
