// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The guided calibration run: which steps it offers, what the owner must have
// ready for the picked ones, and the runner that takes the picked steps one
// after another to a single save or discard at the end. No LVGL.

#include "calibration_catalog.h"
#include "calibration_step.h"
#include "helix_shared_callback.h"

#include <cstdint>
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

/// What the Pick page sets beside which steps run.
struct RunOptions {
    /// The Z offset step warms the bed before it homes and probes, as the Z
    /// offset screen's Warm Bed switch does.
    bool zoffset_warm_bed = false;
    /// The warm-up's minutes, which the caller prices from the shared thermal
    /// model (ThermalRateManager) for the bed as it is now.
    int zoffset_warm_bed_minutes = 0;
};

/// Whole minutes for @p heating_seconds of heating; 0 when nothing is left to heat.
[[nodiscard]] int heating_minutes(float heating_seconds);

/// @p step as a run with @p options runs it: a warmed Z offset takes the
/// warm-up's minutes longer and heats the bed.
[[nodiscard]] StepInfo with_run_options(StepInfo step, const RunOptions& options);

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
    /// Whether Klipper holds anything for SAVE_CONFIG. An unreadable answer
    /// counts as pending.
    virtual void read_pending(std::function<void(bool any_pending)> done) = 0;
};

/// Whether Klipper is still running a command, and a wait for it to finish.
/// A step can fail while the command it sent keeps running (a calibration that
/// outlived its wait), and the next step's commands would only queue behind it.
/// Main thread.
class PrinterBusyWatch {
  public:
    virtual ~PrinterBusyWatch() = default;
    [[nodiscard]] virtual bool busy() const = 0;
    /// Call @p idle once, when the printer next reports idle. If it is still
    /// busy after @p ceiling_ms, or its state can no longer be read (the
    /// printer was switched), call @p gave_up once instead, with whether the
    /// printer was last seen busy. Replaces an earlier wait.
    virtual void when_idle(std::function<void()> idle, uint32_t ceiling_ms,
                           std::function<void(bool still_busy)> gave_up) = 0;
    /// Drop the wait without calling it.
    virtual void cancel() = 0;
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
        /// What Discard all did to a step that restores_on_discard().
        std::string restore_note;
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
    /// What stop() would do now: end the step quietly, restart the firmware
    /// (which drops any finished result still pending in Klipper), or end the
    /// run while the printer finishes a failed step's command on its own.
    enum class StopEffect : uint8_t { EndsStep, Restarts, RestartDropsResults, PrinterFinishes };
    [[nodiscard]] StopEffect stop_effect() const;
    /// Save everything that finished with one SAVE_CONFIG; record the history
    /// only once the save is known to have happened.
    void save_all(RunFinisher& finisher);
    /// Drop everything that finished; record nothing. A step whose result the
    /// firmware already persisted restores its old value first.
    void discard_all(RunFinisher& finisher);

    [[nodiscard]] RunPhase phase() const {
        return phase_;
    }
    [[nodiscard]] const std::vector<Entry>& entries() const {
        return entries_;
    }
    /// The running entry, or -1, also while the run waits for the printer.
    [[nodiscard]] int current() const {
        return phase_ == RunPhase::Running && !waiting_ ? current_ : -1;
    }
    /// Watch Klipper's busy state: after a step that failed, the run goes on
    /// only once the printer is idle. Without one it goes on at once.
    void set_busy_watch(std::unique_ptr<PrinterBusyWatch> watch) {
        busy_watch_ = std::move(watch);
    }
    /// The entry whose command the printer is still running while the run
    /// waits to go on, or -1.
    [[nodiscard]] int waiting_on() const {
        return phase_ == RunPhase::Running && waiting_ ? current_ : -1;
    }
    /// The printer is still running a failed step's command, so nothing can be
    /// saved or discarded yet: a SAVE_CONFIG would queue behind it.
    [[nodiscard]] bool printer_finishing() const {
        return printer_finishing_;
    }
    /// Why the run ended before its last step, other than Stop.
    enum class EndNote : uint8_t {
        None,
        KlipperStopped,    ///< Klipper shut down or restarted while the steps ran
        PrinterStayedBusy, ///< the wait passed its ceiling
        PrinterLost,       ///< the printer's state could no longer be read
    };
    [[nodiscard]] EndNote end_note() const {
        return end_note_;
    }
    [[nodiscard]] int finished_count() const;
    /// Stopping restarted the firmware, so the finished results are no longer
    /// pending in Klipper and cannot be saved.
    [[nodiscard]] bool results_lost() const {
        return results_lost_;
    }
    /// There is something a save would write, and the printer is free to.
    [[nodiscard]] bool can_save() const {
        return (phase_ == RunPhase::Results || phase_ == RunPhase::SaveFailed) &&
               has_pending_result() && !results_lost_ && !printer_finishing_;
    }
    /// There is something Discard all would undo: a result a save would write,
    /// or a finished one the firmware already persisted.
    [[nodiscard]] bool can_discard() const;
    /// Klipper went down (a shutdown or a restart) without the run asking: the
    /// staged results are gone. Main thread.
    void klippy_restarted();
    /// @p e left a result in Klipper's pending config: it finished, or failed
    /// after staging part of its result, and its result is not one the
    /// firmware persisted itself.
    [[nodiscard]] static bool saves(const Entry& e);
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
    /// Go on after entry @p index ended, once the printer is idle if it failed.
    void go_on_after(size_t index);
    /// The printer finished the command failed entry @p index left running.
    void printer_went_idle(size_t index);
    /// End the run after the current entry without going on: the rest did not run.
    void end_early(EndNote note);
    void changed();
    /// Discard all: restore entries from @p from on, then restart if needed.
    void restore_from(size_t from, RunFinisher& finisher);
    void restart_to_discard(RunFinisher& finisher);
    [[nodiscard]] bool has_pending_result() const;

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
    /// The current entry failed and the run waits for the printer to be idle.
    bool waiting_ = false;
    bool printer_finishing_ = false;
    /// Klipper left READY while the run was running: it never goes on again.
    bool klipper_left_ready_ = false;
    EndNote end_note_ = EndNote::None;
    std::unique_ptr<PrinterBusyWatch> busy_watch_;
    std::string error_;
    /// The finisher's callbacks check it, so a run closed mid-save ignores them.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

/// How the steps of a run came out: every screen that reports a run's end words
/// it from this. A step that failed, was skipped, stopped or never ran did not
/// finish.
struct RunTally {
    int finished = 0;
    int total = 0;
    enum class Outcome : uint8_t { AllFinished, SomeUnfinished, NoneFinished };
    [[nodiscard]] int unfinished() const {
        return total - finished;
    }
    [[nodiscard]] Outcome outcome() const {
        if (finished >= total) {
            return Outcome::AllFinished;
        }
        return finished == 0 ? Outcome::NoneFinished : Outcome::SomeUnfinished;
    }
};

[[nodiscard]] RunTally run_tally(const std::vector<CalibrationRun::Entry>& entries);

/// What of a run the owner would want to see, taken after each change.
struct RunSighting {
    RunPhase phase = RunPhase::Ready;
    /// The running entry, -1 when none.
    int current = -1;
    /// The running entry needs the owner at the printer.
    bool needs_hands = false;
    /// Entries that ended without finishing.
    int unfinished = 0;
};

[[nodiscard]] RunSighting sight(const CalibrationRun& run);

/// Whether the change from @p before to @p now is one the owner must see, so a
/// sleeping screen wakes for it: the steps ended, a step did not finish, or a
/// step that needs the owner started.
[[nodiscard]] bool calls_for_owner(const RunSighting& before, const RunSighting& now);

/// How long the run waits for the printer to finish @p step's command before it
/// gives up: twice the step's priced time and half an hour more.
[[nodiscard]] uint32_t wait_ceiling_ms(const StepInfo& step);

/// Whole minutes left of @p entry's estimate: all of it while it waits; while it
/// runs, the share its progress has not reached (all of it before it reports a
/// percentage), and at least one; none once it has ended.
[[nodiscard]] int minutes_left(const CalibrationRun::Entry& entry);

/// The run's countdown: minutes_left() of every entry, summed.
[[nodiscard]] int minutes_left(const std::vector<CalibrationRun::Entry>& entries);

} // namespace helix::calibration
