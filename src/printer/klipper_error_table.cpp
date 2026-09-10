// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_error_table.h"

#include <string>
#include <unordered_map>

namespace helix::printer {

namespace {

/// Format the [unit, slot] payload observed in `key849` (and likely all
/// SLOT-level CFS errors) into a human locator. Klipper emits unit as an
/// integer (1-based) and slot as a single uppercase letter ("A"..."D").
/// Returns "" when the payload doesn't match the expected shape — caller
/// then displays the un-augmented message rather than guessing.
///
/// Real-world sample (telemetry 2026-05-05):
///     !! {"code":"key849","values":[1,"B"]}
///   → " in unit 1 slot B"
std::string format_unit_slot(const nlohmann::json& values) {
    if (!values.is_array() || values.size() < 2)
        return "";
    if (!values[0].is_number_integer())
        return "";
    if (!values[1].is_string())
        return "";
    int unit = values[0].get<int>();
    std::string slot = values[1].get<std::string>();
    if (unit < 1 || slot.size() != 1)
        return "";
    char c = slot[0];
    if (c < 'A' || c > 'D')
        return "";
    return " in unit " + std::to_string(unit) + " slot " + slot;
}

/// Format unit-level errors where values is `[unit]` or `[unit, ...]`.
/// We just grab the leading int and ignore tails. Empty string if the
/// shape doesn't match.
std::string format_unit_only(const nlohmann::json& values) {
    if (!values.is_array() || values.empty())
        return "";
    if (!values[0].is_number_integer())
        return "";
    int unit = values[0].get<int>();
    if (unit < 1)
        return "";
    return " on unit " + std::to_string(unit);
}

// Format-callback aliases keep the table tidy. Only key849 has been
// confirmed against real telemetry (`[1,"B"]`); the other SLOT/UNIT
// codes are wired up to the same formatters on the assumption that
// Creality uses a consistent shape, but they'll degrade gracefully
// (no extra locator displayed) if the assumption is wrong.
static auto* const fmt_unit_slot = &format_unit_slot;
static auto* const fmt_unit_only = &format_unit_only;

static const std::unordered_map<std::string, KlipperErrorEntry> KlipperErrorTable = {
    // Klipper-layer faults, reported with no filament hardware involved. These
    // are why the table cannot live behind HELIX_HAS_CFS: a machine with no CFS
    // box still emits them and still needs the friendly text.
    {"key111",
     {"Pre-heat the extruder first",
      "Filament can't be loaded below the minimum extrude temperature. Set a hotend target (e.g. "
      "220°C for PLA, 240°C for PETG), wait for it to reach temperature, then try again",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key298",
     {"MCU bridge daemon is shut down",
      "Tap Firmware Restart to recover — on K2 this also bounces the rpi MCU bridge",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key585",
     {"Move out of range", "The requested position is outside the printer's bounds",
      AmsAlertLevel::SYSTEM, nullptr}},

    // Motor controller init errors (motor_control_wrapper.so). Typically fire
    // during CFS bring-up. Users can't fix these directly — power cycle is the
    // standard remedy. Surfacing them avoids the raw chinglish from Creality
    // ("Motor set pin restore io status error").
    {"key800",
     {"Motor controller error",
      "A motor IO pin couldn't be restored. Power-cycle the printer to recover",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key801",
     {"Z motor calibration failed",
      "Stall detection setup failed for the Z motor — power-cycle to retry", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key802",
     {"Extruder motor calibration failed",
      "Stall detection setup failed for the extruder — power-cycle to retry", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key803",
     {"Motor parameter setup failed",
      "A motor's parameters couldn't be written. Power-cycle the printer to retry",
      AmsAlertLevel::SYSTEM, nullptr}},

    {"key831",
     {"Lost connection to CFS unit", "Check the RS-485 cable between printer and CFS",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key832",
     {"Retract failed", "Check the spool and the filament path for a jam, then retry",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key833",
     {"Feed failed", "Check the spool and the filament path for a jam, then retry",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key834",
     {"CFS system error",
      "The unit reported a fault with no further detail. Restart the printer if it repeats",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key835",
     {"Filament never reached the CFS hub sensor",
      "Check the path from the slot to the hub for a tangle or drag. If the slot feeds freely, the "
      "hub sensor or the feeder motor may have failed",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key836",
     {"Filament stalled between CFS hub and extruder sensor",
      "Check the PTFE tube along the drag chain for kinks, and the bend where it enters the "
      "extruder sensor",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key837",
     {"Filament jammed at the extruder gear",
      "Retract to unload. If it comes back cleanly, snip the chewed end off and feed again",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key838",
     {"Filament stuck inside the CFS hub",
      "It is jammed between the hub sensor and the hub gear. Open the CFS and clear the hub before "
      "retrying",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key839",
     {"Filament ran out",
      "Load a new spool in this slot, or turn on auto-refill to switch to a matching one",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key840",
     {"CFS is busy",
      "The unit is already running another operation. Wait for it to finish, or reset the CFS if "
      "it stays stuck",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key841",
     {"Filament cutter stuck",
      "The cutter blade didn't return — check for filament wrapped around the cutting mechanism",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key843",
     {"Can't read filament RFID tag",
      // The busy-box case (#1387) needs no spool handling at all, so the
      // re-seat remedy is conditional, not asserted: the firmware msg names
      // the actual cause.
      "Set the filament type and colour by hand for this slot. If the tag should be readable, "
      "re-seat the spool with its label facing the reader once the box is idle",
      AmsAlertLevel::SLOT, fmt_unit_slot, /*prefer_fw_msg=*/true}},
    {"key844",
     {"PTFE tube connection loose", "Re-seat the Bowden tube connector on the CFS unit",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key845",
     {"Nozzle clog detected", "Run a cold pull or replace the nozzle", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key846",
     {"Filament buffer stopped moving",
      "The buffer saw no movement for 16 seconds. Check for a blockage between the CFS and the "
      "extruder, or a stuck buffer",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key847",
     {"Filament is dragging",
      "The extruder cannot pull it smoothly. Check the spool for a tangle or knot, and the PTFE "
      "tube for a severe bend",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key848",
     {"Filament snapped inside CFS",
      "Open the CFS unit and remove the broken filament from the slot", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key849",
     {"Retract failed — filament stuck in connector",
      "Manually pull the filament back through the connector", AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key850",
     {"Retract error — multiple connectors triggered",
      "Check that only one filament path is active", AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key851",
     {"Retract didn't reach buffer empty position",
      "The filament may not have fully retracted — try again or manually pull", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key852",
     {"Sensor mismatch — check extruder and CFS sensors",
      "Extruder and CFS disagree on filament state — inspect both sensors", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key853",
     {"Temperature and humidity sensor not responding",
      "The sensor in this CFS unit cannot be read. Check its connection, or the unit may need "
      "service",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key854",
     {"Cutter blade didn't sever filament",
      "Filament is still present after the cut — the blade may be dull or misaligned",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key855",
     {"Filament cutter position error",
      "The cutter is out of alignment — recalibrate with CALIBRATE_CUT_POS", AmsAlertLevel::SYSTEM,
      nullptr}},
    {"key856",
     {"Filament cutter not detected", "Check that the cutter mechanism is properly installed",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key857",
     {"CFS motor overloaded", "A spool may be tangled or the drive gear is jammed",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key858",
     {"EEPROM error on CFS unit",
      "The unit's onboard storage has failed. The CFS mainboard needs replacing",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key859",
     {"Measuring wheel error",
      "The CFS odometer hardware is faulty and needs repair or replacement", AmsAlertLevel::UNIT,
      fmt_unit_only}},
    {"key860",
     {"Buffer tube problem", "Check the buffer unit on the back of the printer",
      AmsAlertLevel::SYSTEM, nullptr}},
    {"key861",
     {"RFID reader malfunction (left)", "The left RFID reader in this CFS unit may need service",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key862",
     {"RFID reader malfunction (right)", "The right RFID reader in this CFS unit may need service",
      AmsAlertLevel::UNIT, fmt_unit_only}},
    {"key863",
     {"Retract error — filament still detected",
      "Filament didn't fully retract, may need manual removal", AmsAlertLevel::SLOT,
      fmt_unit_slot}},
    {"key864",
     {"Filament buffer failure during feed",
      "The buffer may be disconnected, stuck, or faulty. Check it before retrying",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    {"key865",
     {"Retract error — failed to exit connector", "Filament stuck in connector during unload",
      AmsAlertLevel::SLOT, fmt_unit_slot}},
    // key866 is a second "no cutter" variant emitted from the motor driver path
    // (key856 comes from the box driver). Same root cause, same remedy.
    {"key866",
     {"Filament cutter not detected", "Check that the cutter mechanism is properly installed",
      AmsAlertLevel::SYSTEM, nullptr}},
};

} // namespace

const KlipperErrorEntry* klipper_error_lookup(const std::string& key_code) {
    auto it = KlipperErrorTable.find(key_code);
    return it == KlipperErrorTable.end() ? nullptr : &it->second;
}

} // namespace helix::printer
