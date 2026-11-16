// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "calibration_types.h"
#include "helix_shared_callback.h"
#include "i_moonraker_api.h"
#include "json_fwd.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * @file snapmaker_screws_tilt.h
 * @brief Mapper for the Snapmaker U1 [auto_screws_tilt_adjust] status object
 *
 * The U1's firmware module replaces upstream SCREWS_TILT_CALCULATE console
 * output with a status object, so the ScrewTiltResult set the rest of the
 * screws-tilt UI consumes is derived here from fields instead of lines.
 */

namespace helix {

class IMoonrakerClient;

/**
 * @brief Outcome of mapping one auto_screws_tilt_adjust status object
 *
 * A non-empty `error` means the payload is unusable and `screws` is empty: a
 * screw silently zeroed to the target would read as a level bed.
 */
struct AutoScrewsTiltResults {
    std::vector<ScrewTiltResult> screws;
    std::string error; ///< Names the missing/null status field, when set

    [[nodiscard]] bool ok() const {
        return error.empty();
    }
};

/**
 * @brief Map the U1 status object plus its config section to screw results
 *
 * diff_mm = target_z - base_point_i, the same sign convention as upstream's
 * z_base - z: above the target renders CW, below renders CCW. Every screw is
 * measured against target_z, so none is a reference the way upstream's first
 * screw is. target_z is read from the field, never recomputed as the mean of
 * the base points — the firmware's adjust_screws path re-derives it as a
 * midrange after a tuning round, so the field is not always the mean.
 *
 * @param status         auto_screws_tilt_adjust status object
 * @param config_section configfile.settings.auto_screws_tilt_adjust
 *                       (screw1..4 as "x,y", screw1_name..4_name)
 * @param pitch_mm       bed screw thread pitch for the clock-minute conversion
 */
AutoScrewsTiltResults parse_auto_screws_tilt(const nlohmann::json& status,
                                             const nlohmann::json& config_section,
                                             float pitch_mm = kScrewPitchDefaultMm);

namespace snapmaker {
namespace screws_tilt {

/// machine_state_manager.main_state while the firmware holds the
/// screws-tilt calibration state. IDLE is 0; the module defines other
/// values HelixScreen does not model and must not name.
inline constexpr int kMainStateScrewsTiltAdjust = 8;

/// Klipper object name, and configfile section name, of the module.
inline constexpr const char* kModuleName = "auto_screws_tilt_adjust";

/// The firmware command sequence that replaces SCREWS_TILT_CALCULATE. ENTRY
/// switches main_state to SCREWS_TILT_ADJUST; the remaining commands are
/// valid only inside that state.
inline constexpr const char* kCmdEntry = "AUTO_SCREWS_TILT_ADJUST_ENTRY";
inline constexpr const char* kCmdHoming = "AUTO_SCREWS_TILT_ADJUST_HOMING";
/// DETECT_BED_PLATE is an assertion, not a query: PRESENCE=0 asserts the PEI
/// sheet is OFF the bed, and the firmware raises fault 530/0/11 when
/// it is not. The wizard's AUTO_SCREWS_TILT_ADJUST_DETECT_PLATE wrapper runs
/// it with no arguments, so PRESENCE defaults to 1 - the opposite assertion -
/// and errors exactly when the sheet is off. PRESENCE=0 is sent directly so
/// the command's own verdict is the gate.
inline constexpr const char* kCmdDetectBedPlate = "DETECT_BED_PLATE PRESENCE=0";

inline constexpr const char* kCmdProbeReferencePoints =
    "AUTO_SCREWS_TILT_ADJUST_PROBE_REFERENCE_POINTS";
/// Restores IDLE and lifts Z. Throws inside the firmware unless main_state
/// is SCREWS_TILT_ADJUST, so every send is gated on the state being ours.
inline constexpr const char* kCmdExit = "AUTO_SCREWS_TILT_ADJUST_EXIT";

/**
 * @brief True only when the held calibration state is stale and safe to clear
 *
 * Requires main_state == kMainStateScrewsTiltAdjust and a probe_step that
 * is terminal or never started: nothing is running. Every other probe step
 * means work in flight - adjust_refpoint_done and adjust_wait_manual are a
 * wizard paused for a human who may be driving it from another client - and
 * clearing those aborts live work. A missing or unknown probe_step is
 * treated as in flight: the conservative answer.
 */
[[nodiscard]] bool stale_calibration_state(int main_state, const std::string& probe_step);

/// machine_state_manager.main_state out of a status map (a subscription
/// snapshot or an objects.query result). The single field of that object
/// HelixScreen reads.
[[nodiscard]] std::optional<int> main_state_from_status(const nlohmann::json& status);

/// True when a DETECT_BED_PLATE PRESENCE=0 failure message is the firmware
/// reporting the PEI sheet is still on the bed (matched on the structured
/// fault code the message carries; the wording around it is the firmware's to
/// change and proves nothing). False for any other failure, which is a
/// genuine detection problem - callers must refuse, not probe, either way.
[[nodiscard]] bool plate_still_on_bed(const std::string& error_message);

/// The module's status object plus its configfile section, taken from one
/// objects.query response and mapped through parse_auto_screws_tilt.
[[nodiscard]] AutoScrewsTiltResults results_from_query(const nlohmann::json& response);

/**
 * @brief Leave the firmware calibration state, if it is ours to leave
 *
 * Reads main_state, then sends kCmdExit only when it is
 * kMainStateScrewsTiltAdjust: the macro throws otherwise, so an
 * unconditional send is not a cleanup. No-op on a printer without the
 * module (the query comes back without the object). @p client must outlive
 * the round trip - it holds the callback.
 */
void request_exit(IMoonrakerClient& client);

/**
 * @brief Connect-time reconciliation of a leftover calibration state
 *
 * A previous session that died mid-run can leave main_state at
 * SCREWS_TILT_ADJUST, which makes unrelated firmware operations refuse.
 * When the discovery snapshot says the state is held, probe_step decides:
 * stale states get kCmdExit - the wizard's own exit, which also restores the
 * configured idle timeout, clears probe_step and lifts Z; a bare state
 * restore would leave the idle timeout at the wizard's pause value, so
 * heaters and steppers never idle out. Anything in flight is left to
 * whoever is driving it. @p client must outlive the round trip.
 */
void reconcile_on_connect(IMoonrakerClient& client, const nlohmann::json& initial_status);

/**
 * @brief Run the five-command calibration sequence that replaces
 *        SCREWS_TILT_CALCULATE, delivering results or an error once
 *
 * Every terminal path releases the firmware calibration state.
 */
void start_sequence(IMoonrakerClient& client, IMoonrakerAPI& api, ScrewTiltCallback on_success,
                    helix::SharedCallback<void(const MoonrakerError&)> on_error);

} // namespace screws_tilt
} // namespace snapmaker
} // namespace helix
