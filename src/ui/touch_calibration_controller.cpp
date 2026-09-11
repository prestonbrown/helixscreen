// SPDX-License-Identifier: GPL-3.0-or-later

#include "touch_calibration_controller.h"

#include "config.h"
#include "display_manager.h"
#include "touch_calibration_wrapper.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {
constexpr int DEFAULT_SCREEN_W = 800;
constexpr int DEFAULT_SCREEN_H = 480;
} // namespace

TouchCalibrationController::TouchCalibrationController()
    : panel_(std::make_unique<TouchCalibrationPanel>()) {}

ICalibrationSink* TouchCalibrationController::sink() const {
    if (sink_override_) {
        return sink_override_;
    }
    return DisplayManager::instance();
}

void TouchCalibrationController::begin() {
    DisplayManager* dm = DisplayManager::instance();
    if (dm && dm->is_initialized()) {
        panel_->set_screen_size(dm->width(), dm->height());
        spdlog::debug("[TouchCalController] Screen size set to {}x{}", dm->width(), dm->height());
    } else {
        panel_->set_screen_size(DEFAULT_SCREEN_W, DEFAULT_SCREEN_H);
        spdlog::warn("[TouchCalController] DisplayManager not available, using default {}x{}",
                     DEFAULT_SCREEN_W, DEFAULT_SCREEN_H);
    }

    panel_->reset();
    clear_pending();

    if (ICalibrationSink* s = sink()) {
        session_.begin_capture(*s);
        s->set_capture_active(true);
    }
}

void TouchCalibrationController::end() {
    if (panel_) {
        panel_->cancel();
    }
    if (ICalibrationSink* s = sink()) {
        session_.restore(*s);
        s->set_capture_active(false);
    }
}

void TouchCalibrationController::on_press() {
    if (!panel_) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    const auto state_before = panel_->get_state();

    // Verifying: the candidate matrix is already live, so `point` is where that
    // matrix puts the finger. calibrated_read_cb() clamps every transformed
    // coordinate to the panel, so a matrix that throws touches off-screen shows
    // up pinned to an edge rather than out of range - which is what the
    // fast-revert net counts as "did not land where the finger was".
    if (state_before == TouchCalibrationPanel::State::VERIFY) {
        if (view_) {
            view_->on_verify_feedback(Point{point.x, point.y});
        }
        lv_display_t* disp = lv_display_get_default();
        const int32_t w = disp ? lv_display_get_horizontal_resolution(disp) : 0;
        const int32_t h = disp ? lv_display_get_vertical_resolution(disp) : 0;
        const bool on_screen =
            w > 1 && h > 1 && point.x > 0 && point.y > 0 && point.x < w - 1 && point.y < h - 1;
        panel_->report_verify_touch(on_screen);
        return;
    }

    spdlog::debug("[TouchCalController] Touch at ({}, {}) during state {}", point.x, point.y,
                  static_cast<int>(state_before));

    // Pair the press with the untouched digitizer reading behind it, when the
    // backend can supply one. Fetched at the press edge, so it belongs to this
    // coordinate and not to some later motion event (#1259, #1276).
    Point device_raw{};
    const bool has_device_raw = get_last_raw_touch(device_raw);
    panel_->on_press({point.x, point.y}, has_device_raw ? &device_raw : nullptr);

    // Feedback belongs on a real calibration point, not on the "tap anywhere to
    // begin" transition out of IDLE.
    const auto state_after = panel_->get_state();
    if (view_ && state_before != TouchCalibrationPanel::State::IDLE &&
        (state_after == TouchCalibrationPanel::State::POINT_1 ||
         state_after == TouchCalibrationPanel::State::POINT_2 ||
         state_after == TouchCalibrationPanel::State::POINT_3)) {
        // Capture runs with the affine disabled, so `point` is in raw capture
        // space - which is NOT screen space on a panel whose digitizer
        // over-reports its ABS range (Qidi Q2, #943): raw space is compressed
        // ~0.5x, and a mark drawn there lands centimetres from the crosshair and
        // reads as broken even while calibration is working. Map it through the
        // calibration the user's touches were tracking under when the session
        // opened. A first-ever calibration has no backup, and the pre-calibration
        // state really is that far off, so it shows at the raw point.
        Point landed{point.x, point.y};
        const TouchCalibration& pre_session = session_.backup();
        if (pre_session.valid) {
            lv_display_t* disp = lv_display_get_default();
            const int max_x = disp ? lv_display_get_horizontal_resolution(disp) - 1 : 0;
            const int max_y = disp ? lv_display_get_vertical_resolution(disp) - 1 : 0;
            landed = transform_point(pre_session, landed, max_x, max_y);
        }
        view_->on_capture_feedback(landed);
    }

    // Reaching VERIFY is reported by the panel's verify-entry callback, which
    // fires on whichever commit path gets there (release / stall / legacy press)
    // rather than on this press edge (#1029).
    if (view_) {
        view_->on_progress();
    }
}

void TouchCalibrationController::on_release() {
    if (!panel_) {
        return;
    }
    // Forward the finger-lift so a pending press commits (#943). No-op when
    // debounce is off. Main-thread input only.
    panel_->on_release();

    // The commit lands here rather than on press, so refresh now: otherwise the
    // instruction keeps showing the pre-commit "touch N of 3" until the next tap.
    if (view_) {
        view_->on_progress();
    }
}

void TouchCalibrationController::revert_candidate() {
    if (ICalibrationSink* s = sink()) {
        session_.revert_for_retry(*s);
    }
}

void TouchCalibrationController::retry() {
    if (!panel_) {
        return;
    }
    revert_candidate();
    clear_pending();

    // panel_->retry() only leaves VERIFY, and a view that auto-accepts may already
    // have driven the panel past it to COMPLETE. Fall back to cancel()+start()
    // there - noting cancel() fires the completion callback with nullptr, which
    // retry() does not, so it is not a drop-in for the VERIFY case.
    if (panel_->get_state() == TouchCalibrationPanel::State::VERIFY) {
        panel_->retry();
    } else {
        panel_->cancel();
        panel_->start();
    }

    if (view_) {
        view_->on_progress();
    }
}

CommitOutcome TouchCalibrationController::commit() {
    const TouchCalibration* cal = nullptr;
    TouchRangeFit fit{};

    if (has_pending_) {
        cal = &pending_calibration_;
        fit = pending_range_fit_;
    } else if (panel_) {
        cal = panel_->get_calibration();
        fit = panel_->get_range_fit();
    }

    // The same predicate for both entry points. `valid` alone lets through
    // matrices the residual check rejects.
    if (!cal || !cal->valid || !is_calibration_valid(*cal)) {
        spdlog::error("[TouchCalController] No usable calibration to commit");
        return CommitOutcome::NoCalibration;
    }

    const bool applied = commit_calibration_result(sink(), *cal, fit);

    if (Config* config = Config::get_instance()) {
        config->save();
    } else {
        spdlog::error("[TouchCalController] Config not available - calibration not persisted");
    }

    clear_pending();
    session_.commit();
    return applied ? CommitOutcome::Applied : CommitOutcome::Persisted;
}

int TouchCalibrationController::active_target_index() const {
    if (!panel_) {
        return -1;
    }
    switch (panel_->get_state()) {
    case TouchCalibrationPanel::State::POINT_1:
        return 0;
    case TouchCalibrationPanel::State::POINT_2:
        return 1;
    case TouchCalibrationPanel::State::POINT_3:
        return 2;
    default:
        return -1;
    }
}

Point TouchCalibrationController::active_target_position() const {
    const int step = active_target_index();
    if (!panel_ || step < 0) {
        return Point{0, 0};
    }
    return panel_->get_target_position(step);
}

void TouchCalibrationController::stash_pending(const TouchCalibration& cal,
                                               const TouchRangeFit& fit) {
    pending_calibration_ = cal;
    pending_range_fit_ = fit;
    has_pending_ = true;
}

void TouchCalibrationController::clear_pending() {
    has_pending_ = false;
    pending_range_fit_ = TouchRangeFit{};
}

} // namespace helix::ui
