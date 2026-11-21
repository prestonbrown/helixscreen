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
}

namespace helix::calibration {

/// Material temperatures the heater step tunes at (PLA).
inline constexpr int kRunNozzleTempC = 210;
inline constexpr int kRunBedTempC = 60;

/// The step that runs @p info, or nullptr for a step a guided run does not run.
[[nodiscard]] std::unique_ptr<CalibrationStep> make_run_step(const StepInfo& info);

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

  private:
    IMoonrakerAPI& api_;
    helix::ui::SaveConfigWatch watch_;
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix::calibration
