// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Firmware-persisted z-offset.
//
// On most printers the z-offset the next print will use IS Klipper's live
// gcode_move offset (homing_origin[2]), so reading that is the whole story.
// Some firmwares instead keep the authoritative value in their own storage and
// only push it into gcode_move for the duration of a print - zeroing the live
// offset in their END_PRINT/CANCEL_PRINT macros. On those, homing_origin reads
// 0.000 whenever the printer is idle and is an outright lie about what the next
// print will apply.
//
// This module is the ONLY place that knows which firmwares behave that way and
// where each one stores its value. Generic code (PrinterMotionState, the
// discovery subscription, Application startup) asks these functions and never
// names a firmware.
//
// Adding a firmware means adding one Provider to the table in
// z_offset_persistence.cpp - no call site changes.

#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class Config;
class PrinterDiscovery;
} // namespace helix

namespace helix::zoffset {

/// Klipper status objects that must be subscribed for
/// read_persisted_offset_microns() to ever return a value on this printer.
/// Empty for printers whose z-offset lives only in gcode_move.
std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// The firmware-persisted z-offset in microns, read out of a Moonraker status
/// frame. nullopt when this frame carries no persisted offset - either because
/// the printer has no such firmware, or because the status object is delta-only
/// and simply is not in this frame. Callers must treat nullopt as "no news",
/// never as "cleared".
std::optional<int> read_persisted_offset_microns(const nlohmann::json& status);

/// One-shot gcode that switches the firmware's z-offset persistence on, or an
/// empty string when the printer needs no such call. It writes PERSISTENT
/// firmware state and makes the firmware ignore the slicer's per-print
/// Z_OFFSET / SKIP_ZOFFSET parameters, so send it at most once per printer and
/// only while idle - it is a gcode injection. claim_persistence_enable() below
/// is the gate that enforces both.
std::string persistence_enable_gcode(const PrinterDiscovery& hw);

/// Give the one shot back after a send that did not reach the printer.
///
/// claim_persistence_enable() records BEFORE the gcode goes out, which is what
/// stops two discoveries in the same session from each injecting it. The cost
/// is that a send which never lands - klippy not ready, socket dropped - spends
/// the claim for the life of the install and the firmware is never told. This
/// hands it back so the next discovery retries.
///
/// Main thread only: Config is not synchronised, and the send's error callback
/// runs on the response thread, so callers there must marshal.
void release_persistence_enable(Config* config);

/// Whether the matched firmware's persistence setting is ALREADY on, read out
/// of a Moonraker status frame.
///
/// nullopt means this frame does not say: the printer has no such firmware, the
/// firmware exposes no readable flag, or the key is simply not in this frame.
/// It is never evidence either way. ZMOD in particular materializes its key
/// about ten seconds into a Klipper session, so a fresh connection reads nullopt
/// for that whole window.
///
/// A firmware's off value carries different weight per firmware, which is why
/// this answers the raw state and the decision lives in
/// should_enable_persistence() rather than here.
std::optional<bool> persistence_already_enabled(const PrinterDiscovery& hw,
                                                const nlohmann::json& status);

/// Gcode that clears a stale probe-delta variable the firmware's
/// SET_GCODE_OFFSET override subtracts before persisting, or an empty string
/// when the firmware has no such mechanism. ZMOD saves every adjustment as
/// `z - _TEST_POINT.temp_z_offset`, where the variable holds the last
/// print-start probe delta. Through ZMOD 1.7.2 it survived
/// END_PRINT/CANCEL_PRINT, so an adjustment made while idle stored the
/// intended value minus a stale delta (ghzserg/zmod#699);
/// ghzserg/z_ad5x@6a0adf3 zeroes it at _COMMON_END_PRINT, unreleased so far.
/// Sending the clear is correct against both: on fixed firmware it writes zero
/// over zero. Send this immediately before the adjustment, on the
/// same script, and ONLY while no print is running: mid-print the subtraction
/// is correct, excluding the live per-print transient.
std::string stale_probe_delta_clear_gcode(const PrinterDiscovery& hw);

/// Whether this printer keeps its authoritative z-offset outside gcode_move.
bool firmware_persists_z_offset(const PrinterDiscovery& hw);

/// Whether this status frame PROVES the matched provider's storage is absent -
/// that detection over-matched and this printer does not in fact persist the
/// offset anywhere.
///
/// Detection is deliberately asymmetric. A match latches "Save Z Offset stands
/// down" from the first moment, because the opposite mistake is the damaging
/// one: on a printer that really does persist, the save path folds the gcode
/// offset into the probe and the firmware re-applies the same offset on top at
/// every boot, so the probe value grows without bound until the nozzle reaches
/// the bed (prestonbrown/helixscreen#1401). Losing the Save button on a printer
/// that did not need the stand-down is merely annoying.
///
/// Some rows must detect on a signature that proves a SET_GCODE_OFFSET wrapper
/// exists without proving the wrapper stores anything - wrapping the command
/// for logging, clamping, or per-tool offsets is a standard Voron / Klippain /
/// toolchanger pattern. Those rows carry a refutation, and only a frame that
/// satisfies it relaxes the strategy back to the type-derived one.
///
/// false is the safe answer and the default in every uncertain case: a frame
/// that merely lacks news never refutes, and a row detecting on an unambiguous
/// vendor macro is not refutable at all.
bool status_refutes_persistence(const PrinterDiscovery& hw, const nlohmann::json& status);

/// Human-readable name of the matched firmware, for logging. Empty when none.
std::string persistence_provider_name(const PrinterDiscovery& hw);

/// Gate for sending persistence_enable_gcode(): the printer needs it, no print
/// is running, this printer has never been sent it, and the firmware does not
/// already hold the setting on.
///
/// already_sent must come from persistent storage. The gcode writes persistent
/// firmware state, so a per-process guard would re-force the setting at every
/// launch and a user who deliberately turned it back off could never keep it off
/// (prestonbrown/helixscreen#1432).
///
/// already_enabled is tri-state on purpose: only a definite true suppresses the
/// send. nullopt is a frame that carries no answer, and answering it as "off"
/// would be inventing evidence.
inline bool should_enable_persistence(bool firmware_needs_enable, bool print_active,
                                      bool already_sent, std::optional<bool> already_enabled) {
    const bool firmware_already_has_it = already_enabled.value_or(false);
    return firmware_needs_enable && !print_active && !already_sent && !firmware_already_has_it;
}

/// Decide whether to send persistence_enable_gcode() and, when the answer is
/// yes, record that decision durably in one step.
///
/// Returns true exactly once per printer: on the first idle discovery for a
/// firmware that needs the command and does not already hold the setting on.
/// Every later call returns false, across restarts - the record goes to
/// persistent config, so a user who turns the setting back off keeps it off
/// (prestonbrown/helixscreen#1432).
///
/// The record is written ONLY when the answer is yes: a mid-print discovery, or
/// one that finds the firmware already enabled, must not consume the one shot.
///
/// @param status the discovery's status frame, or nullptr when there is none -
///        no frame is "no news", exactly as an absent key is.
bool claim_persistence_enable(Config* config, const PrinterDiscovery& hw,
                              const nlohmann::json* status, bool print_active);

} // namespace helix::zoffset
