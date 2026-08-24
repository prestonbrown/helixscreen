// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_touch_calibration_overlay.h"

namespace helix::ui {

/**
 * @brief Test-only access to TouchCalibrationOverlay internals.
 *
 * The overlay's capture markers (#1082) must be drawn where the finger lands
 * under the mapping active when the session opened — the session backup — not
 * at the raw capture point, which on over-reporting digitizers (Qidi Q2, #943)
 * is compressed ~0.5x relative to the screen and makes every dot read as
 * "broken". Driving that path needs the session armed exactly as show() arms
 * it, without the NavigationManager push show() also performs.
 */
class TouchCalibrationOverlayTestAccess {
  public:
    /// Arm the capture session exactly as show() does: snapshot the stored
    /// calibration into the session backup and disable the affine transform.
    static void begin_session(TouchCalibrationOverlay& o) {
        if (helix::ICalibrationSink* sink = o.calibration_sink()) {
            o.session_.begin_capture(*sink);
        }
    }
};

} // namespace helix::ui
