// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_emergency_stop.h"

#include "display_awake_hold.h"
#include "macro_executor.h"

namespace helix::calibration {

/// What a calibration screen's current stage needs held while it shows.
struct StageNeeds {
    /// The app has the machine moving or heating: the E-stop shows (MachineBusyToken).
    bool machine_busy = false;
    /// The user's hands are on the printer: the screen stays awake (DisplayAwakeToken).
    bool hands_on = false;
};

/**
 * @brief The E-stop and keep-awake holds of one calibration screen
 *
 * A screen maps each of its stages to StageNeeds and calls apply() on every stage
 * change, and release() when it closes or is torn down. Holding both here keeps
 * the two rules in one place: a stage that moves the machine shows the E-stop,
 * a stage that waits on the user's hands keeps the display up. Main thread only.
 */
class StageHolds {
  public:
    void apply(StageNeeds needs) {
        if (needs.machine_busy) {
            busy_.acquire();
        } else {
            busy_.reset();
        }
        if (needs.hands_on) {
            awake_.acquire();
        } else {
            awake_.reset();
        }
    }
    void release() {
        busy_.reset();
        awake_.reset();
    }
    [[nodiscard]] bool machine_busy() const {
        return busy_.held();
    }
    [[nodiscard]] bool awake() const {
        return awake_.held();
    }

  private:
    MachineBusyToken busy_;
    DisplayAwakeToken awake_;
};

/// start_calibration_gcode() with the E-stop shown from the call until the routine
/// answers, fails or is refused: a firmware routine sent as console G-code holds
/// the machine while print_stats reads standby. Chains any on_finished @p cmd has.
void start_held_calibration_gcode(IMoonrakerAPI* api, MachineGcode cmd, bool needs_home,
                                  const char* caller_tag);

} // namespace helix::calibration
