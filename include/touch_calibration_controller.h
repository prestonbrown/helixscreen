// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "touch_calibration.h"
#include "touch_calibration_panel.h"
#include "touch_calibration_session.h"

#include <lvgl.h>
#include <memory>

namespace helix::ui {

/**
 * @brief What a calibration view draws. Presentation only.
 *
 * The controller decides WHEN each of these is warranted; the view decides what
 * it looks like. Everything here is optional to act on - the wizard ignores the
 * marker its overlay sibling draws, and neither has to do anything on a state it
 * does not render.
 */
class ITouchCalibrationView {
  public:
    virtual ~ITouchCalibrationView() = default;

    /// The state machine or the sample count moved. Refresh instruction text,
    /// crosshair placement, and whatever chrome tracks progress.
    virtual void on_progress() = 0;

    /// A press was accepted as a calibration sample. `landed` is where it should
    /// be shown, already mapped out of raw capture space.
    virtual void on_capture_feedback(Point landed) = 0;

    /// A press arrived while verifying, at screen-absolute `p`.
    virtual void on_verify_feedback(Point p) {
        (void)p;
    }
};

/// What became of a commit. A bare bool cannot carry it: "nothing worth writing"
/// and "written, but the device could not take it right now" call for different
/// things from the view, and only the second is worth a toast.
enum class CommitOutcome {
    NoCalibration, ///< No usable matrix; nothing was written
    Persisted,     ///< Written to config, but the device did not accept it
    Applied,       ///< Written and live on the device
};

/**
 * @brief The session logic behind a touch calibration, shared by both entry points
 *
 * Owns the panel and the session, and runs everything that is a decision rather
 * than a rendering: capture, retry, commit, teardown. Held by value in each view.
 *
 * Deliberately not a base class - the wizard step and the Settings overlay sit on
 * incompatible framework bases - and deliberately ignorant of navigation. It
 * reports an outcome and the view dismisses itself, so nothing here can run after
 * the view it belongs to has gone away.
 */
class TouchCalibrationController {
  public:
    TouchCalibrationController();

    TouchCalibrationController(const TouchCalibrationController&) = delete;
    TouchCalibrationController& operator=(const TouchCalibrationController&) = delete;

    /// Bind the view this controller draws through. Called once, from the view's
    /// own constructor: the pairing is fixed for the controller's whole life, so
    /// it does not come and go with a session. A controller with no view still
    /// runs - it just draws nothing.
    void attach(ITouchCalibrationView& view) {
        view_ = &view;
    }

    /// Open a capture session: re-sample the screen, reset the panel, suppress the
    /// global debug ripple, and back up whatever calibration is currently live.
    ///
    /// The screen size is read HERE and not at construction: a view built once and
    /// shown many times would otherwise lay its targets out against whatever the
    /// display measured at startup, and a rotation since then puts every crosshair
    /// at the wrong ratio and biases the solve.
    void begin();

    /// Close the session and put the device back the way it was. Idempotent: the
    /// overlay unwinds from two lifecycle hooks and both call this.
    void end();

    /// A press on the capture surface. Reads the active input device itself.
    void on_press();

    /// A finger-lift on the capture surface.
    void on_release();

    /// Put the device back on the pre-session calibration, leaving the session
    /// armed for another capture attempt. What an unattended revert needs - a
    /// verify timeout or a broken-matrix fast-revert - where the view is not
    /// restarting the point sequence itself.
    void revert_candidate();

    /// Throw the candidate away and start the point sequence again.
    void retry();

    /// Persist the calibration and close the session.
    CommitOutcome commit();

    /// Which calibration target is being captured: 0, 1 or 2, or -1 when none is
    /// (idle, verifying, or done). The views place their own crosshair; this is the
    /// decision they were both making separately.
    int active_target_index() const;

    /// Where that target sits, in logical screen coordinates.
    Point active_target_position() const;

    TouchCalibrationPanel* panel() {
        return panel_.get();
    }
    const TouchCalibrationPanel* panel() const {
        return panel_.get();
    }
    TouchCalibrationSession& session() {
        return session_;
    }

    /// Resolve the device to calibrate: the test override when set, else the
    /// display manager. One spelling, so an override reaches every phase.
    ICalibrationSink* sink() const;
    void set_sink_override(ICalibrationSink* sink) {
        sink_override_ = sink;
    }

    /// Hold the solved calibration without writing it, for a view that commits
    /// later (the wizard's Next button).
    void stash_pending(const TouchCalibration& cal, const TouchRangeFit& fit);
    bool has_pending() const {
        return has_pending_;
    }
    void clear_pending();

  private:
    std::unique_ptr<TouchCalibrationPanel> panel_;
    TouchCalibrationSession session_;
    ITouchCalibrationView* view_ = nullptr;
    ICalibrationSink* sink_override_ = nullptr;

    TouchCalibration pending_calibration_{};
    TouchRangeFit pending_range_fit_{};
    bool has_pending_ = false;
};

} // namespace helix::ui
