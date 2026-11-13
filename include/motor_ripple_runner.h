// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Runs a motor ripple sweep: one G-code script per motor and speed, with the
// accelerometer stream cut into one capture per script. The caller homes the
// printer and checks it is free to move before start().
//
// Threading: start(), cancel() and the destructor are main-thread calls, and
// every callback fires on the main thread. Stream batches arrive on the
// source's own thread and only touch a shared sample buffer; analysis runs on
// the injected executor.

#include "live_sweep_preview.h"
#include "motor_ripple_plan.h"
#include "motor_ripple_types.h"
#include "stepper_resonance_calibration.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace helix {
class IMoonrakerAPI;
}

namespace helix::calibration::ripple {

class MotorRippleRunner {
  public:
    enum class Mode { Sweep, CompensationCompare };
    enum class Phase { Idle, Preparing, Measuring, Restoring, Done, Failed, Cancelled };

    struct Request {
        Mode mode = Mode::Sweep;
        SweepPlanInputs plan;
        std::map<MotorId, MotorGeometry> geometry;
        std::map<MotorId, std::string> accel_chip; ///< [resonance_tester] chip per motor
        std::string klippy_socket;
        stepper_resonance_cal::CompensationToggle toggle; ///< used only in CompensationCompare
        std::string tool_select_gcode;                    ///< empty: none
        std::string tool_return_gcode;
    };

    struct Callbacks { ///< all fire on the main thread
        std::function<void(Phase)> on_phase;
        std::function<void(bool compensated, MotorId, const SpeedResult&, size_t index,
                           size_t total)>
            on_speed;
        std::function<void(const SweepResult&)> on_done;
        std::function<void(const std::string& reason)> on_failed; ///< untranslated
    };

    using SourceFactory = std::function<std::unique_ptr<IAccelBatchSource>()>;
    /// Runs analysis off the main thread; the default posts to HttpExecutor::fast().
    using Executor = std::function<void(std::function<void()>)>;

    MotorRippleRunner(IMoonrakerAPI& api, SourceFactory sources, Executor analysis = {});
    /// Stops the stream; a run still moving the printer gets its restore script.
    ~MotorRippleRunner();
    MotorRippleRunner(const MotorRippleRunner&) = delete;
    MotorRippleRunner& operator=(const MotorRippleRunner&) = delete;

    /// False, with nothing sent, for a plan that refuses, missing geometry or a
    /// run already in progress.
    bool start(Request req, Callbacks cb);
    void cancel();
    [[nodiscard]] Phase phase() const;

    struct Impl;

  private:
    std::shared_ptr<Impl> impl_;
};

} // namespace helix::calibration::ripple
