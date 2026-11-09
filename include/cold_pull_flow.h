// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file cold_pull_flow.h
 * @brief The decisions of a guided cold pull, with no UI, no I/O and no clock of its own.
 *
 * A cold pull melts cleaning filament into the nozzle, cools it until it holds together
 * but lets go of the wall, then pulls it out by hand so the residue comes with it
 * (prestonbrown/helixscreen#2011). Every event, time included, returns the commands to
 * send now as a ColdPullActions; ColdPullRunner applies them. Every ending goes through
 * exit_actions(), so no stage can leave the heater on or the part fan at full.
 */

#pragma once

#include "cold_pull_temps.h"
#include "guided_load_flow.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace helix {

/// The soak is reached, like a filament preheat, within this many degrees.
inline constexpr int kColdPullReachMarginC = 5;
/// The nozzle is at the pull temperature within this many degrees above it.
inline constexpr int kPullBandC = 3;
/// Hold pressure stops for good this far above the extrusion floor, so no extrusion
/// is ever queued near min_extrude_temp.
inline constexpr int kHoldPressureMarginC = 15;
/// A target on the head further than this from the last one sent was set elsewhere.
inline constexpr int kForeignTargetToleranceC = 2;
inline constexpr int kFeedMm = 60;
inline constexpr int kFeedSpeedMmMin = 300;
inline constexpr int kPurgeMoreMm = 20;
inline constexpr int kPurgeSpeedMmMin = 300;
inline constexpr int kHoldPressureMm = 1;
inline constexpr int kHoldPressureSpeedMmMin = 60;
inline constexpr std::chrono::seconds kHoldPressureInterval{3};
inline constexpr std::chrono::seconds kColdPullTargetLandTimeout{20};
inline constexpr std::chrono::minutes kColdPullHeatTimeout{10};
/// Under Klipper's default 10 minute idle_timeout.
inline constexpr std::chrono::minutes kFeedIdleTimeout{8};
inline constexpr std::chrono::minutes kCoolTimeout{10};
inline constexpr std::chrono::minutes kCoolTimeoutNoFan{20};
inline constexpr std::chrono::minutes kPullHoldTimeout{5};

/// Values are published on the cold_pull_stage subject and matched in cold_pull_overlay.xml.
enum class ColdPullStage : int {
    Setup = 0,
    Preparing, ///< Park offered; the user makes the toolhead ready by hand
    Heating,
    Feed,
    Cooling,
    Pull,
    Inspect,
    Finished,
    Cancelled,
    Abandoned,
};

/// How filament gets into the nozzle at Feed. Published on cold_pull_feed_mode.
enum class ColdPullFeedMode : int {
    ByHand = 0, ///< No lane in the head: the user pushes filament into the extruder
    Loaded,     ///< The seated lane's filament is already in the nozzle
    Refeed,     ///< The seated lane's filament was pulled out and goes back by hand
};

enum class ColdPullEndReason : int {
    None = 0,
    UserCancel,
    Timeout,
    ForeignTarget,
    JobStarted,
    Disconnected,
    TargetNeverLanded,
};

/// Everything the flow fixes at start.
struct ColdPullParams {
    ColdPullTemps temps;
    std::string heater;          ///< Klipper heater object of the chosen head
    std::string part_fan;        ///< "" when no commandable part fan
    int prior_fan_percent = 0;   ///< Restored on every exit
    int floor_c = 0;             ///< extrusion_floor_c() for the head
    bool homed = false;          ///< Park runs only on a homed machine
    bool has_park_macro = false; ///< A park macro resolves on this printer
    /// A filament-system lane is seated in this head; its filament is the one pulled.
    bool toolhead_loaded = false;
    /// Small extrusions while cooling keep the nozzle full (Prusa's step). Off by
    /// default: they are moves the user did not tap for.
    bool hold_pressure = false;
};

/// What to send now, applied in field order by ColdPullRunner.
struct ColdPullActions {
    std::optional<int> heater_target_c; ///< via TemperatureController::set_target
    bool keep_previous_hot = false;     ///< SendOptions flag for heater_target_c
    std::optional<int> fan_percent;     ///< via fan_gcode(part_fan, ...)
    std::string gcode;                  ///< extrusion only; "" for none
    uint32_t command_id = 0;            ///< Names @c gcode in command_finished()
    bool run_park = false;              ///< park macro (only ever set when homed)
    bool home_then_park = false;        ///< ensure_homed_then + park (user tap only)
    bool reset_meter = false;           ///< maintenance cold pull meter (Done only)

    [[nodiscard]] bool empty() const {
        return !heater_target_c && !fan_percent && gcode.empty() && !run_park && !home_then_park &&
               !reset_meter;
    }
};

/// The temperature ring's fill: toward @p goal_c from where the stage began at
/// @p from_c, and full on the stages entered because the goal was reached.
[[nodiscard]] int cold_pull_ring_percent(ColdPullStage stage, int from_c, int current_c,
                                         int goal_c);

class ColdPullFlow {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;

    explicit ColdPullFlow(ColdPullParams params);

    [[nodiscard]] ColdPullStage stage() const {
        return stage_;
    }
    [[nodiscard]] ColdPullEndReason end_reason() const {
        return end_reason_;
    }
    [[nodiscard]] bool ended() const;
    [[nodiscard]] int pass() const {
        return pass_;
    }
    /// What the ring counts toward: the soak while heating and feeding, the pull while
    /// cooling and pulling, else 0.
    [[nodiscard]] int goal_c() const;
    /// The embedded feed's stage; AwaitPush outside Feed.
    [[nodiscard]] GuidedLoadStage feed_stage() const;
    /// The display must stay awake: the user has a moment to act on.
    [[nodiscard]] bool hold_awake() const {
        return stage_ == ColdPullStage::Pull;
    }
    /// A command this flow sent is still running.
    [[nodiscard]] bool busy() const;
    /// The id of the command in flight; 0 when none is.
    [[nodiscard]] uint32_t in_flight_command() const {
        return in_flight_command_;
    }
    /// How the filament reaches the nozzle for this pass.
    [[nodiscard]] ColdPullFeedMode feed_mode() const;
    /// The seated lane's filament is out of the hotend while its backend, told nothing
    /// of a hand pull, still reports it loaded.
    [[nodiscard]] bool lane_needs_reseat() const {
        return params_.toolhead_loaded && filament_out_;
    }
    [[nodiscard]] const ColdPullParams& params() const {
        return params_;
    }
    /// The target this flow holds the heater at, or nullopt when the heater is not its.
    [[nodiscard]] std::optional<int> owned_target_c() const;

    // User events
    [[nodiscard]] ColdPullActions start(TimePoint now); ///< Setup -> Preparing
    [[nodiscard]] ColdPullActions home_and_park(TimePoint now);
    [[nodiscard]] ColdPullActions toolhead_ready(TimePoint now); ///< Preparing -> Heating
    [[nodiscard]] ColdPullActions extrude(TimePoint now);
    [[nodiscard]] ColdPullActions fed(TimePoint now);    ///< Feed -> Cooling, extruded or not
    [[nodiscard]] ColdPullActions pulled(TimePoint now); ///< -> Inspect
    [[nodiscard]] ColdPullActions repeat(TimePoint now); ///< Inspect -> Heating
    [[nodiscard]] ColdPullActions done(TimePoint now);   ///< Inspect -> Finished
    [[nodiscard]] ColdPullActions cancel();

    // Printer events
    [[nodiscard]] ColdPullActions temperature(int current_c, int target_c, TimePoint now);
    [[nodiscard]] ColdPullActions filament_detected(TimePoint now);
    /// A reply to anything but the command still in flight is dropped.
    [[nodiscard]] ColdPullActions command_finished(uint32_t command_id, bool ok, TimePoint now);
    [[nodiscard]] ColdPullActions job_started();
    [[nodiscard]] ColdPullActions disconnected();
    [[nodiscard]] ColdPullActions tick(TimePoint now);

    /// The cleanup every ending applies: heater off when this flow holds it, part fan
    /// back to its prior speed when this flow changed it. Sends nothing while
    /// @p job_holds_machine, which leaves both to the job.
    [[nodiscard]] ColdPullActions exit_actions(bool job_holds_machine) const;

  private:
    enum class InFlight { None, Feed, Hold };

    [[nodiscard]] ColdPullActions enter_heating(TimePoint now);
    [[nodiscard]] ColdPullActions enter_cooling(TimePoint now);
    [[nodiscard]] ColdPullActions enter_pull(TimePoint now);
    [[nodiscard]] ColdPullActions end(ColdPullStage stage, ColdPullEndReason reason,
                                      bool send_cleanup);
    [[nodiscard]] bool live() const;
    void track_command(ColdPullActions& a, InFlight kind);
    void clear_in_flight();
    [[nodiscard]] GuidedLoadFlow new_feed() const;

    ColdPullParams params_;
    ColdPullStage stage_ = ColdPullStage::Setup;
    ColdPullEndReason end_reason_ = ColdPullEndReason::None;
    int pass_ = 1;
    std::optional<GuidedLoadFlow> feed_;
    InFlight in_flight_ = InFlight::None;
    uint32_t in_flight_command_ = 0;
    uint32_t last_command_ = 0;
    TimePoint stage_started_{};
    TimePoint last_tap_{};

    std::optional<int> sent_target_c_; ///< Last target sent while the heater is ours
    std::optional<int> prev_target_c_; ///< The one before it, until the new one lands
    bool target_seen_ = false;
    bool fan_changed_ = false;
    bool was_above_pull_ = false;
    bool hold_stopped_ = false;
    bool filament_out_ = false; ///< Pulled by hand; a completed extrude shows it is back
    bool parked_ = false;
    int last_current_c_ = 0;
    TimePoint next_hold_at_{};
};

} // namespace helix
