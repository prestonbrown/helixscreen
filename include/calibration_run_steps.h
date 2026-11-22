// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The guided run's real steps and its real finisher. Each step wraps the code
// that runs its calibration on its own screen: the heater calibration API calls,
// the bed mesh panel's calibration, the input shaper calibrator, and the Z
// offset panel opened in run mode.

#include "calibration_run.h"
#include "save_config_restart.h"

#include <memory>

namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

namespace helix::calibration {

/// The step that runs @p info with @p options, or nullptr for a step a guided
/// run does not run.
[[nodiscard]] std::unique_ptr<CalibrationStep> make_run_step(const StepInfo& info,
                                                             const RunOptions& options = {});

/// The run's busy watch over @p state's idle_timeout. Main thread.
[[nodiscard]] std::unique_ptr<PrinterBusyWatch> make_printer_busy_watch(PrinterState& state);

/// Saves through SaveConfigWatch, discards with a host restart through the
/// expected-restart contract, records into CalibrationHistory. Main thread.
class MoonrakerRunFinisher : public RunFinisher {
  public:
    explicit MoonrakerRunFinisher(IMoonrakerAPI& api) : api_(api) {}
    void save_all(helix::SharedCallback<void()> on_saved,
                  helix::SharedCallback<void(const std::string&)> on_failed) override;
    void discard_all(std::function<void()> on_done,
                     std::function<void(const std::string&)> on_failed) override;
    void record(const StepInfo& step, const StepResult& result) override;
    void read_pending(std::function<void(bool any_pending)> done) override;

  private:
    IMoonrakerAPI& api_;
    helix::ui::SaveConfigWatch watch_;
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix::calibration
