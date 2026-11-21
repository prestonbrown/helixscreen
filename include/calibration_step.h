// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One calibration step as the guided run drives it. Each step wraps the code
// that already runs its calibration (a Moonraker API call, a calibrator, a
// panel); the run only ever sees this interface. No LVGL.

#include "calibration_catalog.h"

#include <functional>
#include <string>

#include "hv/json.hpp" // JSON_HPP_OK: StepResult holds its values by value

namespace helix {
class IMoonrakerAPI;
}

namespace helix::calibration {

/// What a finished step measured.
struct StepResult {
    /// Ready to show on the results screen and in the history ("0.125 mm").
    std::string summary;
    /// The numbers the history keeps for later steps.
    nlohmann::json values = nlohmann::json::object();
};

/// How a step ended.
struct StepOutcome {
    enum class Kind : uint8_t {
        Finished, ///< measured; result holds it, staged for the run's one save
        Failed,   ///< message says why
        Skipped,  ///< the owner left a hands-on step without accepting it
        Aborted,  ///< stopped by the run
    };
    Kind kind = Kind::Failed;
    StepResult result;
    std::string message;

    static StepOutcome finished(StepResult r) {
        return {Kind::Finished, std::move(r), {}};
    }
    static StepOutcome failed(std::string why) {
        return {Kind::Failed, {}, std::move(why)};
    }
    static StepOutcome skipped() {
        return {Kind::Skipped, {}, {}};
    }
    static StepOutcome aborted() {
        return {Kind::Aborted, {}, {}};
    }
};

/// Where a running step is: a translated phase line and a percentage, or -1
/// while the step cannot tell how far along it is.
struct StepProgress {
    std::string phase;
    int percent = -1;
};

/// What a step is given to run. Both callbacks are main-thread only, and done
/// is called exactly once per start().
struct StepContext {
    IMoonrakerAPI& api;
    std::function<void(const StepProgress&)> progress;
    std::function<void(StepOutcome)> done;
};

class CalibrationStep {
  public:
    virtual ~CalibrationStep() = default;
    [[nodiscard]] virtual const StepInfo& info() const = 0;
    /// Begin. Never sends SAVE_CONFIG: a result stays pending in Klipper until
    /// the run saves everything at once. Main thread.
    virtual void start(StepContext& ctx) = 0;
    /// Stop a running step; done() then reports Aborted. Safe at any point.
    virtual void abort() = 0;
    /// The last abort() restarted the firmware, which drops every result the
    /// run had staged in Klipper.
    [[nodiscard]] virtual bool abort_restarted_firmware() const {
        return false;
    }
    /// This finished step's result took effect outside Klipper's pending config
    /// (the firmware persisted it), so no restart undoes it and Discard all
    /// must put the old value back with restore().
    [[nodiscard]] virtual bool restores_on_discard() const {
        return false;
    }
    /// Put the value from before the step back, the way the step set the new
    /// one. @p done runs once on the main thread with a translated line saying
    /// what happened ("Restored to -1.245 mm").
    virtual void restore(IMoonrakerAPI& api, std::function<void(std::string note)> done) {
        (void)api;
        done({});
    }
};

} // namespace helix::calibration
