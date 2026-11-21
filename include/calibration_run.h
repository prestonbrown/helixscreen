// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The guided calibration run: which steps it offers, what the owner must have
// ready for the picked ones, and the runner that takes the picked steps one
// after another to a single save or discard at the end. No LVGL.

#include "calibration_catalog.h"
#include "calibration_step.h"
#include "helix_shared_callback.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerAPI;
}

namespace helix::calibration {

/// The steps a guided run offers, in run order: Z offset, heaters, bed mesh and
/// input shaper where @p applicable has them and their tier is not OnRequest.
[[nodiscard]] std::vector<StepInfo> guided_run_steps(const std::vector<StepInfo>& applicable);

/// Whether a run starts with this step ticked: everything but Optional.
[[nodiscard]] bool picked_by_default(const StepInfo& step);

/// One line of the "Before you start" list: a prerequisite and the picked steps
/// that need it, in run order.
struct PrereqLine {
    Prereq prereq;
    std::vector<const StepInfo*> for_steps;
};

/// Every prerequisite of @p picked, each once, in a fixed order. The pointers
/// point into @p picked.
[[nodiscard]] std::vector<PrereqLine> prereq_lines(const std::vector<StepInfo>& picked);

/// Every prerequisite, in the order prereq_lines() lists them.
inline constexpr Prereq kPrereqOrder[] = {
    kPrereqBedClear,       kPrereqBuildPlate, kPrereqToolheadUnloaded,
    kPrereqFilamentLoaded, kPrereqPaper,      kPrereqAccelerometer};

/// The run's last act: one save of everything the steps staged, or one restart
/// that drops it. Main thread.
class RunFinisher {
  public:
    virtual ~RunFinisher() = default;
    /// One SAVE_CONFIG of everything pending.
    virtual void save_all(helix::SharedCallback<void()> on_saved,
                          helix::SharedCallback<void(const std::string&)> on_failed) = 0;
    /// Drop everything pending in Klipper.
    virtual void discard_all(std::function<void()> on_done,
                             std::function<void(const std::string&)> on_failed) = 0;
    /// Keep a saved step's result in the calibration history.
    virtual void record(const StepInfo& step, const StepResult& result) = 0;
};

enum class StepStatus : uint8_t { Pending, Running, Finished, Failed, Skipped, Aborted, NotRun };

enum class RunPhase : uint8_t {
    Ready,      ///< built, not started
    Running,    ///< a step is running
    Results,    ///< every step ended, or the run was stopped
    Saving,     ///< SAVE_CONFIG sent, waiting for its outcome
    Saved,      ///< saved, history recorded
    SaveFailed, ///< the save did not happen; the results are still pending
    Discarding, ///< the restart that drops the results is on its way
    Discarded,  ///< nothing was saved
};

class CalibrationRun {
  public:
    struct Entry {
        std::unique_ptr<CalibrationStep> step;
        StepStatus status = StepStatus::Pending;
        StepProgress progress;
        StepResult result;
        std::string message;
    };

    /// @p on_change runs on the main thread after every status or progress change.
    CalibrationRun(IMoonrakerAPI& api, std::vector<std::unique_ptr<CalibrationStep>> steps,
                   std::function<void()> on_change);
    ~CalibrationRun();
    CalibrationRun(const CalibrationRun&) = delete;
    CalibrationRun& operator=(const CalibrationRun&) = delete;

    /// Run the steps in order. A failed or skipped step does not stop the rest.
    void start();
    /// Abort the running step and go to the results with what finished.
    void stop();
    /// Save everything that finished with one SAVE_CONFIG; record the history
    /// only once the save is known to have happened.
    void save_all(RunFinisher& finisher);
    /// Drop everything that finished; record nothing.
    void discard_all(RunFinisher& finisher);

    [[nodiscard]] RunPhase phase() const {
        return phase_;
    }
    [[nodiscard]] const std::vector<Entry>& entries() const {
        return entries_;
    }
    /// The running entry, or -1.
    [[nodiscard]] int current() const {
        return phase_ == RunPhase::Running ? current_ : -1;
    }
    [[nodiscard]] int finished_count() const;
    /// Stopping restarted the firmware, so the finished results are no longer
    /// pending in Klipper and cannot be saved.
    [[nodiscard]] bool results_lost() const {
        return results_lost_;
    }
    /// There is something a save would write.
    [[nodiscard]] bool can_save() const {
        return (phase_ == RunPhase::Results || phase_ == RunPhase::SaveFailed) &&
               finished_count() > 0 && !results_lost_;
    }
    /// Why the last save or discard failed: the save in SaveFailed, the discard
    /// back in Results.
    [[nodiscard]] const std::string& error() const {
        return error_;
    }

  private:
    void advance();
    void start_next();
    void on_step_done(size_t index, StepOutcome outcome);
    void on_step_progress(size_t index, const StepProgress& progress);
    void finish_steps();
    void changed();

    IMoonrakerAPI& api_;
    std::function<void()> on_change_;
    /// One context per entry, alive as long as the run, since a step keeps it.
    std::vector<std::unique_ptr<StepContext>> contexts_;
    /// Declared after contexts_ so the steps go first.
    std::vector<Entry> entries_;
    RunPhase phase_ = RunPhase::Ready;
    int current_ = -1;
    bool results_lost_ = false;
    bool advancing_ = false;
    bool advance_again_ = false;
    std::string error_;
    /// The finisher's callbacks check it, so a run closed mid-save ignores them.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace helix::calibration
