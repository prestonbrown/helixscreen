// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * @file snapmaker_nozzle_offset.h
 * @brief The Snapmaker U1's multi-toolhead offset calibration, as a step plan
 *        and a runner that drives it
 *
 * The firmware's extruder_offset_calibration module (in probe_inductance_coil.py)
 * registers EXTRUDER_OFFSET_ACTION_* commands, each a single blocking step that
 * runs a `_EXTRUDER_OFFSET_ACTION_<STEP>[_Tn]` macro from xyz_offset_calibration.cfg.
 * Sequencing them, and the human steps between, is the stock screen's job:
 *
 *   PRESTART                       enter XYZ_OFFSET_CALIBRATE, preheat, home
 *   per head n: HEAT, AUTO_CLEAN,  heat to the loaded filament's temp, purge and wipe,
 *               MANUAL_CLEAN,      park the head at the front for brushing,
 *               [user cleans]      the stock "Clean the nozzle with the wire brush",
 *               WAIT_COOL TEMP=140 cool to probing temperature
 *   [user removes the PEI sheet]
 *   DETECT_PLATE PRESENCE=0        asserts the sheet is off; fails while it is on
 *   per head n: PROBE_CALIBRATE    measure X/Y/Z against the bed's reference hole;
 *                                  the last head's step saves all results itself
 *   EXIT                           heaters off, idle timeout restored, Z up, state IDLE
 *
 * Every command but PRESTART refuses outside XYZ_OFFSET_CALIBRATE, and EXIT is
 * the only way back out, so every terminal path sends it.
 */

namespace helix {
class IMoonrakerClient;
class PrinterDiscovery;
} // namespace helix

namespace helix::snapmaker::nozzle_offset {

/// The module maps exactly T0..T3 and saves only once all four have a result.
inline constexpr int kHeads = 4;

/// Probing temperature the stock screen cools each nozzle to.
inline constexpr int kProbeTempC = 140;

/// The stock screen's SAFE_MOVE_Y_POS for the auto-clean step.
inline constexpr int kSafeMoveYPos = 250;

/// Each step is one blocking command; heating or cooling a nozzle, or probing
/// one, takes minutes. Moonraker never times printer.gcode.script out itself.
inline constexpr uint32_t kStepTimeoutMs = 600000;

inline constexpr const char* kExitGcode = "EXTRUDER_OFFSET_ACTION_EXIT";

/// Moonraker method for the module's webhook that stops a probe run early, so
/// a cancel does not wait out a head's whole probing pass.
inline constexpr const char* kProbeAbortMethod = "printer.extruder_offset_calibration.probe_abort";

enum class StepKind { Prestart, Heat, AutoClean, ManualClean, WaitCool, DetectPlate, Probe };

/// What the user must do before a step may run.
enum class Prompt { None, CleanNozzle, RemovePlate };

struct Step {
    StepKind kind;
    int tool = -1; ///< 0-based head, -1 for the machine-wide steps
    std::string gcode;
    Prompt prompt_before = Prompt::None;
};

/// True when the firmware ships the macros the sequence runs, for all kHeads heads.
[[nodiscard]] bool supported(const helix::PrinterDiscovery& discovery);

/// The guided flow (ui_guided_device_flow.h) that runs the calibration. The
/// Calibration panel's Tool Offsets entry starts it (helix::tool_offset_calibration).
inline constexpr const char* kGuidedFlowId = "snapmaker_nozzle_offset";

/// The full command plan, in the stock screen's order.
[[nodiscard]] std::vector<Step> build_sequence();

/**
 * @brief User-facing text for a failed step's error
 *
 * The module raises coded faults as `{"coded": "...", "msg": "..."}`: a fault we
 * have our own wording for gets it, otherwise the firmware's `msg`, otherwise
 * the message as it arrived.
 */
[[nodiscard]] std::string failure_text(const std::string& error_message);

/// Each head's extruder_offset out of an objects.query response (or its
/// `result.status` map), T0 first. nullopt when any head's triple is missing.
[[nodiscard]] std::optional<std::vector<std::array<double, 3>>>
offsets_from_query(const nlohmann::json& response);

/// The objects.query params offsets_from_query() reads.
[[nodiscard]] nlohmann::json offsets_query_params();

/// machine_state_manager.main_state while the firmware holds the offset
/// calibration state (MachineMainState.XYZ_OFFSET_CALIBRATE). IDLE is 0.
inline constexpr int kMainStateXyzOffsetCalibrate = 2;

/// True while a Runner in this process is mid-run (Running, AwaitingUser or
/// Finishing). A held firmware state is then ours, not stale.
[[nodiscard]] bool run_live();

/// What a connect-time read of main_state calls for.
enum class HeldState {
    None,    ///< the state is not held, or a run in this process owns it
    Exit,    ///< held for a run this process owned whose EXIT never landed: end it
    AskUser, ///< held with no run here behind it, possibly another screen's: ask first
};

/**
 * @brief Classifies an objects.query response (or its status map) for the
 *        connect-time reconcile
 *
 * Every step but PRESTART refuses outside the state and EXIT is the only way
 * out, so a run whose driver died leaves the heaters at the run's targets and
 * the idle timeout at the pause value until something sends EXIT. Another
 * client (the stock screen, a second HelixScreen) drives its own run through
 * the same state, so only a run this process owned is ended unasked.
 */
[[nodiscard]] HeldState held_state_action(const nlohmann::json& response, bool run_live,
                                          bool exit_pending);

/// A run in this process ended without its EXIT ever sent: the firmware still
/// holds the state on our behalf, so the next reconcile ends it unasked.
/// Recorded against the printer the last reconcile_on_connect() named, so
/// another printer's held state never reads as ours. In memory only; a
/// restart forgets it.
void mark_exit_pending();
/// True when an unsent exit belongs to the printer last connected.
[[nodiscard]] bool exit_pending();
/// EXIT went out but no reply confirmed it. The firmware may have ended the
/// state and another screen started its own since, so a held state then is
/// asked about, never ended unasked. Same printer keying as mark_exit_pending().
void mark_exit_unconfirmed();
/// True when an unconfirmed exit belongs to the printer last connected.
[[nodiscard]] bool exit_unconfirmed();
/// Called wherever the state is seen not held: nothing is left to end.
void clear_exit_pending();

/// Sends EXIT and forgets the reply beyond the pending-exit flag: cleared when
/// it lands, set when it fails. @p client must outlive the round trip.
void send_exit(helix::IMoonrakerClient& client);

/// Stops a probe pass in progress (kProbeAbortMethod), fire-and-forget.
void send_probe_abort(helix::IMoonrakerClient& client);

/**
 * @brief Connect-time cleanup of a calibration state a previous run left
 *
 * Reads main_state fresh (the discovery subscription does not always carry
 * machine_state_manager). A state this process owned gets EXIT at once; one it
 * did not own calls @p ask_user, on the thread the response arrives on, and
 * sends nothing. @p client must outlive the round trip.
 */
void reconcile_on_connect(helix::IMoonrakerClient& client, std::function<void()> ask_user);

/// How long leave_live_run() waits for EXIT's reply before teardown goes on.
inline constexpr uint32_t kTeardownExitWaitMs = 1000;

/// Overrides kTeardownExitWaitMs for the leave_live_run() calls that follow.
/// Main thread, like leave_live_run().
void set_teardown_exit_wait_ms(uint32_t ms);

/**
 * @brief Ends the run in progress before the client disconnects
 *
 * A run left behind holds the heaters on. With @p client connected this aborts
 * a probe pass in progress, sends EXIT and waits up to kTeardownExitWaitMs
 * (or what set_teardown_exit_wait_ms() last set) for
 * the reply, marking it unconfirmed when none comes; otherwise it marks the
 * exit pending. Either way the run ends here (its changed hook refreshes the
 * overlay's subjects),
 * without sending anything further. No-op when no run is live. Main thread;
 * replies must arrive on another thread for the wait to see them.
 */
void leave_live_run(helix::IMoonrakerClient* client);

/**
 * @brief Walks build_sequence() one command at a time
 *
 * Pure state machine: the caller supplies the transport and is told about every
 * change. Command completions must arrive on the thread that drives the runner.
 *
 * Every terminal path sends EXIT exactly once: a completed run, a failed step,
 * and a cancel. A plate check that fails because the PEI sheet is still on the
 * bed is not terminal: the runner asks for it to be removed again.
 *
 * @threading Single thread (the UI thread in the app)
 */
class Runner {
  public:
    enum class Phase {
        Idle,         ///< not started
        Running,      ///< a command is in flight
        AwaitingUser, ///< prompt() names what the user must do, then confirm()
        Finishing,    ///< the last probe passed; EXIT is in flight
        Done,
        Failed, ///< error() says why; EXIT was sent if a step ran
        Cancelled,
    };

    using Completion = std::function<void(bool ok, const std::string& error)>;

    struct Hooks {
        /// Run one gcode command; call the completion once when it finishes.
        /// EXIT's completion feeds the pending-exit flag (mark_exit_pending()).
        std::function<void(const std::string& gcode, uint32_t timeout_ms, Completion done)> send;
        /// Stop a probe pass in progress (kProbeAbortMethod). Optional.
        std::function<void()> abort_probe;
        /// Called after every phase or step change.
        std::function<void()> changed;
        /// Why a run may not start now (a print holds the printer), or empty.
        /// Optional; a refused start() ends Failed with this as error() and
        /// sends nothing.
        std::function<std::string()> start_refusal;
    };

    explicit Runner(Hooks hooks);
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    /// Begins a run from any phase but Running, AwaitingUser and Finishing.
    void start();
    /// The user did what prompt() asked; runs the step it was holding.
    void confirm();
    /// Abandons the run. Sends EXIT once unless nothing has started or the run
    /// already ended; aborts a probe pass first so EXIT is not queued behind it.
    void cancel();
    /// Forgets a run that ended (Done, Failed, Cancelled), back to Idle.
    void reset();
    /// Ends a live run as Cancelled without sending anything: the caller has
    /// dealt with the firmware state itself. Late completions are dropped.
    void abandon();

    [[nodiscard]] Phase phase() const {
        return phase_;
    }
    /// True while a probe pass is the command in flight.
    [[nodiscard]] bool probing() const;
    /// The step running or waiting, or nullptr outside a run.
    [[nodiscard]] const Step* current_step() const;
    /// 0-based index of current_step() in steps().
    [[nodiscard]] size_t step_index() const {
        return index_;
    }
    [[nodiscard]] const std::vector<Step>& steps() const {
        return steps_;
    }
    /// What the user must do while AwaitingUser.
    [[nodiscard]] Prompt prompt() const;
    /// Why the run failed (Failed), or why the plate check refused
    /// (AwaitingUser on RemovePlate after a refusal). Empty otherwise.
    [[nodiscard]] const std::string& error() const {
        return error_;
    }

  private:
    void run_current();
    void on_step_done(uint64_t run, bool ok, const std::string& error);
    void send_exit(Phase then);
    void set_phase(Phase phase);

    Hooks hooks_;
    std::vector<Step> steps_;
    Phase phase_ = Phase::Idle;
    size_t index_ = 0;
    std::string error_;
    /// Bumped by every start and cancel, so a completion from an abandoned run
    /// is recognised and dropped.
    uint64_t run_ = 0;
};

} // namespace helix::snapmaker::nozzle_offset
