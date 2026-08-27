// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printer_state.h" // ZOffsetCalibrationStrategy

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

class IMoonrakerAPI;

namespace helix {
class PrinterDiscovery;
}

namespace helix::ui {
class SaveConfigWatch;
}

namespace helix {
class PrinterState;
} // namespace helix

namespace helix::zoffset {

/// Returns true and shows toast if strategy auto-persists (FIRMWARE_MANAGED).
/// Callers should return early when this returns true.
bool is_auto_saved(ZOffsetCalibrationStrategy strategy);

/// Format microns as "+0.050mm" or "-0.025mm". Empty string if microns == 0.
void format_delta(int microns, char* buf, size_t buf_size);

/// Format microns as "+0.050mm" (always shows value, even for 0).
void format_offset(int microns, char* buf, size_t buf_size);

/// Compact variant: drops leading zero for |value| < 1.0 → "+.050mm".
void format_offset_compact(int microns, char* buf, size_t buf_size);

/// Pick which z-offset to show the user, in microns.
///
/// Firmware that persists the z-offset itself (ZMOD, via its SET_GCODE_OFFSET
/// override) zeroes Klipper's live gcode_move offset in END_PRINT/CANCEL_PRINT
/// and re-applies the stored value at START_PRINT. So while idle the live offset
/// reads 0.000 and lies about what the next print will use; the stored value is
/// the truth. Mid-print the live offset is authoritative again, because baby
/// steps land there first.
///
/// @param live_microns       gcode_move.homing_origin[2]
/// @param persisted_microns  firmware-stored offset, or nullopt when unknown
///                           (every non-ZMOD printer, and ZMOD before its
///                           save_variables frame has arrived)
/// @param print_active       a print is currently running
int displayed_z_offset_microns(int live_microns, std::optional<int> persisted_microns,
                               bool print_active);

/// Convenience overload resolving all three inputs from PrinterState. Use this
/// at UI call sites so the selection rule lives in exactly one place.
int displayed_z_offset_microns(helix::PrinterState& state);

/// Build the SET_GCODE_OFFSET command for a baby-step of @p delta_microns taken
/// from the offset we showed the user (@p base_microns).
///
/// Relative `Z_ADJUST=` is only correct when the base IS Klipper's live offset,
/// because Klipper resolves it against homing_origin. On ZMOD at idle the live
/// offset has been zeroed, so a relative nudge would land on just the delta - and
/// ZMOD's override persists whatever it lands on, silently discarding the stored
/// offset. Send an absolute `Z=` in that case so the result is what the user saw
/// plus what they asked for.
///
/// @param base_microns  offset the UI displayed and is adjusting from
/// @param live_microns  Klipper's current gcode_move.homing_origin[2]
/// @param delta_microns signed baby-step
/// @param all_homed     x, y and z are all homed (MOVE=1 errors otherwise)
std::string build_z_adjust_gcode(int base_microns, int live_microns, int delta_microns,
                                 bool all_homed);

/// How far one tuning session may travel from the offset it opened on, in
/// either direction, in millimetres. The guard bounds the *travel*, not the
/// absolute offset: printers with several toolheads or nozzles legitimately run
/// offsets past this, and clamping the absolute value snapped them toward zero
/// on the first tap and drove the nozzle into the print.
inline constexpr double kZOffsetMaxSessionTravelMm = 2.0;

/// Z baby-step increments, largest first. Index 2 (0.01mm) is the default.
inline constexpr double kZStepAmountsMm[] = {0.05, 0.025, 0.01, 0.005};
inline constexpr int kZStepDefaultIndex = 2;

/// Read the user's last-chosen step index from Config, clamped to a valid
/// index. Out-of-range or unset returns kZStepDefaultIndex.
int persisted_step_index();

/// Persist the chosen step index (per-printer). Out-of-range values are
/// rejected outright — the previously persisted value is left untouched and
/// a warning is logged — rather than clamped and written. The read path
/// (persisted_step_index()) already fully defends against a corrupt on-disk
/// value, so clamping on write would only give a future caller bug a way to
/// silently overwrite the user's real setting with the default.
void set_persisted_step_index(int idx);

struct AdjustResult {
    double applied_delta_mm; ///< delta actually applied after clamping
    double new_offset_mm;    ///< resulting offset, rounded to the micron
    bool sent;               ///< false when clamped to a no-op or api was null
    bool clamped_to_noop;    ///< true when the requested delta was clamped away to
                             ///< nothing (already at the session-travel limit) — disambiguates
                             ///< that case from `sent == false` meaning a null api.
                             ///< Only one of the two reasons for `sent == false` can
                             ///< be true at once: a clamped-to-noop call returns
                             ///< before the null-api check ever runs.
};

/// Apply a Z baby-step: bound the travel from @p session_base_mm, round to the
/// micron, accumulate the pending delta, optimistically publish gcode_z_offset,
/// and send the SET_GCODE_OFFSET that build_z_adjust_gcode() picks.
///
/// @p session_base_mm is the offset the tuning surface opened on; the step is
/// refused once the result would sit more than kZOffsetMaxSessionTravelMm from
/// it. The window is widened to always contain @p current_offset_mm, so a stale
/// base costs a refused step rather than a jump.
///
/// MOVE=1 is appended only when x, y and z are all homed — it makes the toolhead
/// move immediately, which is the point of baby-stepping during a print, but
/// Klipper errors on it when the axes are not homed. A null `ps` is treated as
/// not homed.
///
/// The caller owns any UI mirror of the offset and should update it from
/// `AdjustResult::new_offset_mm` rather than tracking its own running total.
///
/// @warning Side effects happen before the null-`api` check: the pending delta
/// (`PrinterState::add_pending_z_offset_delta()`) and the optimistic
/// `gcode_z_offset` subject write both happen unconditionally whenever `ps` is
/// non-null, even when `api` is null and no gcode is ever sent. A null-api call
/// therefore still publishes a new offset the printer never actually received
/// — `AdjustResult::sent` is `false` in that case, so callers that care must
/// check it rather than assuming the subject reflects reality. This is
/// intentional carried-over behaviour from the original overlay code, not a
/// bug to fix incidentally.
///
/// @warning Main thread only — reads and writes LVGL subjects.
AdjustResult adjust(IMoonrakerAPI* api, PrinterState* ps, double session_base_mm,
                    double current_offset_mm, double delta_mm);

/// Execute strategy-aware save sequence:
///   PROBE_CALIBRATE -> Z_OFFSET_APPLY_PROBE -> SAVE_CONFIG
///   ENDSTOP -> Z_OFFSET_APPLY_ENDSTOP -> SAVE_CONFIG
///   FIRMWARE_MANAGED -> no-op (firmware/macros auto-persist)
///
/// SAVE_CONFIG's rpc is dropped by the restart it triggers, so @p save_watch -
/// not the rpc - decides the outcome: it absorbs the dropped reply and reports
/// success once Klipper is back READY. Reporting the drop cost every successful
/// save a red "SAVE_CONFIG failed" and, worse here, skipped @p ps's pending-delta
/// clear entirely (prestonbrown/helixscreen#1359).
///
/// @param api           Moonraker API for gcode execution (must not be null)
/// @param save_watch    Owned by the caller (a panel member), because it has to
///                      outlive the restart it is watching for
/// @param strategy      Calibration strategy determining command sequence
/// @param on_success    Called once the save is known to have succeeded
/// @param on_error      Called with user-facing message on a REAL failure
/// @param ps            When non-null, cleared via clear_pending_z_offset_delta()
///                      once the save actually succeeds (either success path).
void apply_and_save(IMoonrakerAPI* api, helix::ui::SaveConfigWatch& save_watch,
                    ZOffsetCalibrationStrategy strategy, std::function<void()> on_success,
                    std::function<void(const std::string& error)> on_error,
                    PrinterState* ps = nullptr);

/// Persist EVERY unsaved z-offset: the machine-wide one and each tool's.
///
/// One entry point because two surfaces offer the save (the header button and
/// the Controls panel button) and they must not diverge on what "save" means.
///
/// Tool offsets go first and unconditionally, because their command carries the
/// value: on klipper-toolchanger SAVE_TOOL_PARAMETER stages a config change
/// that the machine-wide SAVE_CONFIG then commits in the SAME restart, so
/// ordering them the other way would strand them until the next save. When only
/// tools are dirty and the firmware still needs a SAVE_CONFIG, this issues one;
/// when the firmware persists immediately (a save-variables store) it does not,
/// and the caller is told not to expect a restart.
///
/// @param api        Moonraker API (must not be null)
/// @param save_watch Restart watch, shared with apply_and_save()
/// @param strategy   Calibration strategy for the machine-wide save
/// @param hw         Hardware, for the per-tool capability questions
/// @param global_dirty  Whether the machine-wide offset needs saving
/// @param on_success Called once everything is known to have been saved
/// @param on_error   Called with a user-facing message on a real failure
/// @param ps         Forwarded to apply_and_save()
/// Save every dirty z-offset from a surface that owns no panel state — the
/// header button, which appears on any panel and cannot borrow one panel's
/// SaveConfigWatch.
///
/// The watch it uses is torn down through StaticSubjectRegistry, i.e. BEFORE
/// lv_deinit(): a process-lifetime static would run ~SaveConfigWatch (and its
/// ObserverGuard::reset()) after LVGL was gone (#705).
void save_dirty_offsets_shared();

void save_dirty_offsets(IMoonrakerAPI* api, helix::ui::SaveConfigWatch& save_watch,
                        ZOffsetCalibrationStrategy strategy, const PrinterDiscovery& hw,
                        bool global_dirty, std::function<void()> on_success,
                        std::function<void(const std::string& error)> on_error,
                        PrinterState* ps = nullptr);

/// Tracks Klipper restart activity observed while a SAVE_CONFIG is in flight.
///
/// Sampling "is a restart expected right now" at the moment a save timeout fires
/// is useless: the recovery-suppression window (RecoverySuppression::LONG, 15s)
/// closes long before a 30s save guard first fires, so the instantaneous check
/// is always false by then. This latches the restart instead — "was a restart
/// seen at any point since this save began" — which is the question the timeout
/// actually needs answered.
///
/// It also detects the restart *completing*. SAVE_CONFIG restarts Klipper, and
} // namespace helix::zoffset
