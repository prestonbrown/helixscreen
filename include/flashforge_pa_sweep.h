// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The stock FlashForge Creator 5 pressure-advance sweep, run from this app.
//
// On stock and Z-Mod firmware the touchscreen app runs the sweep, not Klipper:
// Klipper only forwards two eBoard MCU commands ([pa_adjust]: PA_ACTION and
// PA_GET). With this app replacing the stock screen, this module runs the same
// procedure, recovered from the stock app (firmwareExe, CommMgr::paTestMgr):
//
//   park over the purge chute, then SET_PIN enable_pin_tmc_x/y VALUE=1.00.
//   That second driver latch DISABLES X and Y, so the G1 moves below are
//   stepped against dead drivers and the head never leaves the chute.
//   For each candidate K, one "line":
//     PA_ACTION ACTION=11 PC=666      arm the eBoard's capture
//     SET_PRESSURE_ADVANCE ...        install the candidate
//     G1 moves, slow/fast/slow        a real pressure transient (~1 g total)
//     PA_ACTION ACTION=0 PC=666       stop capture; NO M400 before it
//     M400, PA_GET                    "Result is value=<n>"; only 9 passes
//   A sweep's winner is its smallest passing K. Up to five sweeps; the result
//   is the mean of the first three winners.
//   Restore: release both pins, put the motion limits and K back, and tell
//   Klipper the carriage is still where it parked (SET_KINEMATIC_POSITION).
//
// The scorer lives in the closed eBoard firmware and was tuned against these
// exact moves and timings, so the constants are the stock app's.
//
// Sweep is the procedure as a pure state machine; the driver that sends its
// steps and reads PA_GET lives behind helix::pacal::start_run().

#include "json_fwd.h"
#include "pa_calibration_run.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::flashforge::pa_sweep {

/// One extrusion segment of a line, ending at `x`.
struct Segment {
    float x;
    float e;
    int feed;
};

struct Config {
    /// The stock order. It is deliberately not monotonic; kept because the
    /// scorer was tuned against it.
    std::array<float, 7> candidates{0.0100f, 0.0200f, 0.0150f, 0.0350f, 0.0250f, 0.0300f, 0.0400f};
    float x_start = 40.f;
    float y_start = 50.f;
    float y_step = 5.f;
    int travel_feed = 30000;
    /// E is 0.0568 mm per mm in every segment; only the speed changes.
    std::array<Segment, 6> segments{{{60.f, 1.13573f, 1080},
                                     {100.f, 2.27146f, 10980},
                                     {120.f, 1.13573f, 1080},
                                     {140.f, 1.13573f, 1080},
                                     {180.f, 2.27146f, 10980},
                                     {200.f, 1.13573f, 1080}}};
    float sweep_accel = 5000.f;
    float sweep_scv = 9.f;
    int max_sweeps = 5;
    int winners_needed = 3;
    int pass_verdict = 9;
    /// The purge chute in G-code coordinates, plus a per-tool offset.
    float park_x = 275.f;
    float park_y = 254.f;
    std::array<float, 4> tool_dx{0.f, -2.f, -2.f, -2.f};
    std::array<float, 4> tool_dy{0.f, 0.f, 1.5f, -8.f};
    /// The chute is approached on X from this far left of it.
    float park_approach_dx = 25.f;
    int park_x_feed = 6000;
    int park_y_feed = 24000;
    int park_approach_feed = 6000;
    /// Line height, G-code Z. Raised to, never lowered to.
    float line_z = 8.f;
    int line_z_feed = 3000;
};

/// What the printer reported before the run, so the restore can put it back.
struct Snapshot {
    bool homed = false; ///< toolhead.homed_axes covers x, y and z
    float max_accel = 0.f;
    float square_corner_velocity = 0.f;
    float pressure_advance = 0.f;
    float smooth_time = 0.f;
    float gcode_z = 0.f; ///< gcode_move.gcode_position Z
};

enum class Phase : uint8_t {
    Home,       ///< G28, only when the snapshot is not homed
    ReadZ,      ///< no script: after a home, the driver reads gcode Z again, then on_z()
    Prologue,   ///< state saved, limits set, Z raised, parked over the chute
    Locate,     ///< no script: the driver reads toolhead.position, then on_located()
    Latch,      ///< X/Y drivers disabled
    Line,       ///< one candidate; ends with PA_GET, answered by on_verdict()
    CheckHomed, ///< no script: the driver reads homed_axes before the restore, then on_homed()
    Restore,    ///< position replayed, pins released, limits and K restored
    Apply,      ///< the measured K installed for this Klipper session
    Done,
};

/// What to send next. `script` is ONE gcode/script call; empty for Locate and Done.
struct Step {
    Phase phase = Phase::Done;
    std::string script;
    int candidate = -1; ///< index into Config::candidates, for a Line
};

enum class Failure : uint8_t {
    None,
    TooFewWinners,    ///< fewer than winners_needed sweeps had a passing line
    NoDiscrimination, ///< every line got the same verdict: the sensor is not judging
    Stopped,          ///< stop() before a result
    Error,            ///< fail(): a command or a query failed
};

class Sweep {
  public:
    Sweep(Config cfg, int tool_index, std::string extruder_name, Snapshot snapshot);

    /// The step to send now. Call once per step, then report it: step_done()
    /// for a script, on_located() for Locate, on_verdict() for a Line.
    [[nodiscard]] Step next();

    void step_done();
    /// The raw toolhead X/Y right after the park.
    void on_located(float raw_x, float raw_y);
    /// The PA_GET value for the Line just sent.
    void on_verdict(int value);
    /// gcode_move.gcode_position Z after the home.
    void on_z(float gcode_z);
    /// Whether toolhead.homed_axes still covers x, y and z before the restore.
    /// When it does not, something unhomed the machine mid-run, so the restore
    /// keeps it unhomed instead of replaying a position.
    void on_homed(bool homed);

    /// The restore to send at once, with no queries, when the link is about to
    /// go down; empty when nothing is owed. Ends the run.
    [[nodiscard]] std::string force_restore();

    /// End the run: the next step is Restore if anything needs restoring, else Done.
    void stop();
    void fail(std::string message);

    [[nodiscard]] bool finished() const {
        return phase_ == Phase::Done;
    }
    /// True from the Latch step until the Restore step is sent.
    [[nodiscard]] bool latched() const {
        return latched_;
    }
    [[nodiscard]] std::optional<float> result() const {
        return result_;
    }
    [[nodiscard]] Failure failure() const {
        return failure_;
    }
    [[nodiscard]] const std::string& error() const {
        return error_;
    }
    [[nodiscard]] int lines_done() const {
        return lines_done_;
    }
    /// A typical run: winners_needed sweeps of every candidate.
    [[nodiscard]] int expected_lines() const;
    [[nodiscard]] float candidate_k(int index) const;

  private:
    [[nodiscard]] std::string prologue_script() const;
    [[nodiscard]] std::string latch_script() const;
    [[nodiscard]] std::string line_script(int index) const;
    [[nodiscard]] std::string restore_script(bool homed) const;
    void end_sweep_if_complete();
    [[nodiscard]] Phase restore_phase() const;
    void enter_restore_or_done();

    Config cfg_;
    int tool_;
    std::string extruder_;
    Snapshot snap_;

    Phase phase_ = Phase::Home;
    bool awaiting_ = false;
    bool prologue_sent_ = false;
    bool latched_ = false;
    bool line_sent_ = false;
    bool located_ = false;
    bool homed_for_restore_ = true;
    float raw_x_ = 0.f;
    float raw_y_ = 0.f;

    int sweep_ = 0;
    int index_ = 0;
    int lines_done_ = 0;
    std::vector<std::vector<int>> verdicts_; ///< per sweep, in candidate order
    std::vector<float> winners_;

    std::optional<float> result_;
    Failure failure_ = Failure::None;
    std::string error_;
};

/// "// Result is value=9" (or without the "// ") -> 9; nullopt for any other line.
[[nodiscard]] std::optional<int> parse_pa_get(std::string_view line);

/// The stock rule over finished sweeps: each sweep's smallest passing candidate
/// wins it, and the result is the mean of the first `winners_needed` winners.
/// nullopt when fewer sweeps than that had a pass.
[[nodiscard]] std::optional<float> select(const std::vector<std::vector<int>>& verdicts_by_sweep,
                                          const Config& cfg);

/// True when not every line got the same verdict. A sensor answering 9 to
/// everything would otherwise report the smallest candidate as a measurement.
[[nodiscard]] bool discriminates(const std::vector<std::vector<int>>& verdicts_by_sweep);

/// Release both latch pins, then M84 so nothing moves until the machine is homed
/// again. Empty when both pins read released.
[[nodiscard]] std::string latch_recovery_gcode(float pin_x_value, float pin_y_value);

/// The connect-time fix for a run that died with the latch set, judged from two
/// printer.objects.query results taken kStaleCheckMs apart (each the response's
/// result.status: the two pins, toolhead.position, idle_timeout.state and
/// configfile.settings). A live sweep moves the planned position every few
/// seconds and keeps Klipper busy, so only a latch that stayed set while
/// nothing moved and Klipper sat idle is stale. Then: release the latch and
/// unhome (latch_recovery_gcode), put the motion limits and every extruder's
/// pressure advance back to the config's values, and restore the gcode state
/// the run saved. Empty when the latch is not stale.
[[nodiscard]] std::string stale_latch_recovery_gcode(const nlohmann::json& first,
                                                     const nlohmann::json& second);

/// Gap between the two looks: longer than two test lines.
inline constexpr uint32_t kStaleCheckMs = 12000;

/// Look for a latch an interrupted run left set, and undo it when it is stale.
/// `api_now` is asked for the API at each step, since the second look comes
/// seconds later and the connection may have been replaced. `on_recovered`
/// runs on the main thread after the fix was accepted.
void check_interrupted_run(std::function<IMoonrakerAPI*()> api_now,
                           std::function<void()> on_recovered);

/// Run the sweep on a printer, reporting through `callbacks`. Reached only
/// through pacal::start_run(), which decides that this printer needs it.
[[nodiscard]] std::shared_ptr<pacal::Run> start_run(IMoonrakerAPI& api, IMoonrakerClient& client,
                                                    const pacal::RunRequest& req,
                                                    pacal::RunCallbacks callbacks);

/// Status objects the driver and the recovery read.
inline constexpr std::string_view kPinX = "output_pin enable_pin_tmc_x";
inline constexpr std::string_view kPinY = "output_pin enable_pin_tmc_y";

} // namespace helix::flashforge::pa_sweep
