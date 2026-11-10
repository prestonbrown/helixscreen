// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One pressure-advance calibration in flight, whatever kind of procedure the
// printer needs (see pa_calibration.h). The panel holds a Run and never learns
// which kind it is.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerAPI;
class IMoonrakerClient;
class PrinterDiscovery;
} // namespace helix

namespace helix::pacal {

/// A candidate's judgement. Unknown when the firmware reports progress without one.
enum class Verdict : uint8_t { Unknown, Pass, Fail };

struct Attempt {
    int index = 0;       ///< 1-based count of candidates tried so far
    int expected = 0;    ///< typical total; 0 when the firmware does not say
    float k_tried = 0.f; ///< <= 0 when the firmware reports no value
    Verdict verdict = Verdict::Unknown;
};

struct RunRequest {
    int tool_index = 0;
    int temp_c = 0;
    std::string extruder; ///< Klipper's name for the tool's extruder ("extruder", "extruder1")
};

/// Every callback fires off the LVGL thread; route it through tok.defer.
struct RunCallbacks {
    std::function<void(const Attempt&)> on_attempt;
    std::function<void(float k)> on_result;
    std::function<void(const std::string& message)> on_error;
};

class Run {
  public:
    virtual ~Run() = default;

    /// End the run at the next point the printer is safe in, then send what
    /// the run owes the printer (restored state, released latches). No
    /// callback fires after stop() returns. Main thread.
    virtual void stop() = 0;

    /// The link is about to go down or the app is quitting: send what is owed
    /// now, fire-and-forget. Klipper queues it behind the command that is
    /// running. No callback fires afterwards. Main thread.
    virtual void before_disconnect() = 0;
};

/// Start a calibration on this printer, or nullptr when it cannot measure PA.
[[nodiscard]] std::shared_ptr<Run> start_run(IMoonrakerAPI& api, IMoonrakerClient& client,
                                             const PrinterDiscovery& hw, const RunRequest& req,
                                             RunCallbacks callbacks);

/// Look for what an interrupted run left on the printer, and undo it once it is
/// shown to be stale. Does nothing on a printer whose runs leave nothing behind.
/// `api_now` is asked for the API at each step, since the check takes seconds
/// and the connection can be replaced meanwhile. `on_recovered` runs on the
/// main thread after the printer accepted the fix.
void check_interrupted_run(const PrinterDiscovery& hw, std::function<IMoonrakerAPI*()> api_now,
                           std::function<void()> on_recovered);

} // namespace helix::pacal
