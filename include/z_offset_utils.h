// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printer_state.h" // ZOffsetCalibrationStrategy

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

class IMoonrakerAPI;

namespace helix::ui {
class SaveConfigWatch;
}

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

/// Execute strategy-aware save sequence:
///   PROBE_CALIBRATE -> Z_OFFSET_APPLY_PROBE -> SAVE_CONFIG
///   ENDSTOP -> Z_OFFSET_APPLY_ENDSTOP -> SAVE_CONFIG
///   FIRMWARE_MANAGED -> no-op (firmware/macros auto-persist)
///
/// SAVE_CONFIG's rpc is dropped by the restart it triggers, so @p save_watch -
/// not the rpc - decides the outcome: it absorbs the dropped reply and reports
/// success once Klipper is back READY. Reporting the drop told the user every
/// successful save had failed (prestonbrown/helixscreen#1359).
///
/// @param api           Moonraker API for gcode execution (must not be null)
/// @param save_watch    Owned by the caller (a panel member), because it has to
///                      outlive the restart it is watching for
/// @param strategy      Calibration strategy determining command sequence
/// @param on_success    Called once the save is known to have succeeded
/// @param on_error      Called with user-facing message on a REAL failure
void apply_and_save(IMoonrakerAPI* api, helix::ui::SaveConfigWatch& save_watch,
                    ZOffsetCalibrationStrategy strategy, std::function<void()> on_success,
                    std::function<void(const std::string& error)> on_error);

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
