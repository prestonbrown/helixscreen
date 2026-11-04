// SPDX-License-Identifier: GPL-3.0-or-later

#include "klipper_error_table.h"

#include "lvgl.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

#include <string>
#include <unordered_map>

#include "hv/json.hpp"

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
    return " " + fmt::format(fmt::runtime(lv_tr("in unit {} slot {}")), unit, slot);
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
    return " " + fmt::format(fmt::runtime(lv_tr("on unit {}")), unit);
}

// Format-callback aliases keep the table tidy. Only key849 has been
// confirmed against real telemetry (`[1,"B"]`); the other SLOT/UNIT
// codes are wired up to the same formatters on the assumption that
// Creality uses a consistent shape, but they'll degrade gracefully
// (no extra locator displayed) if the assumption is wrong.
static auto* const fmt_unit_slot = &format_unit_slot;
static auto* const fmt_unit_only = &format_unit_only;

/// The box is busy rather than broken, but stays reachable when it is wedged.
constexpr KlipperErrorAction kResetBoxAction{TR_NOOP("Reset CFS"), "BOX_ERROR_CLEAR",
                                             "error_classify::key840_reset"};
/// firmware_restart alone cannot recover the rpi MCU bridge, so a service
/// does the work and the gcode is empty.
constexpr KlipperErrorAction kMcuBridgeRecoverAction{TR_NOOP("Recover"), "",
                                                     kKlipperErrorRecoveryServiceTag};

/// Every code in this family is a box hardware fault: one missing from the
/// table still blocks, and its modal names the filament system.
constexpr const char* kBoxFaultPrefix = "key8";

bool is_box_fault(const std::string& key_code) {
    return key_code.rfind(kBoxFaultPrefix, 0) == 0;
}

static const std::unordered_map<std::string, KlipperErrorEntry> KlipperErrorTable = {
    // Klipper-layer faults, reported with no filament hardware involved. These
    // are why the table cannot live behind HELIX_HAS_CFS: a machine with no CFS
    // box still emits them and still needs the friendly text.
    {"key111",
     {TR_NOOP("Pre-heat the extruder first"),
      TR_NOOP("Filament can't be loaded below the minimum extrude temperature. Set a hotend target "
              "(e.g. "
              "220°C for PLA, 240°C for PETG), wait for it to reach temperature, then try again"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::WARNING, nullptr}},
    {"key298",
     {TR_NOOP("MCU bridge daemon is shut down"),
      TR_NOOP("Tap Firmware Restart to recover — on K2 this also bounces the rpi MCU bridge"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::WARNING, nullptr, /*prefer_fw_msg=*/false,
      &kMcuBridgeRecoverAction}},
    {"key585",
     {TR_NOOP("Move out of range"),
      TR_NOOP("The requested position is outside the printer's bounds"), AmsAlertLevel::SYSTEM,
      ErrorSeverity::WARNING, nullptr}},

    // Motor controller init errors (motor_control_wrapper.so). Typically fire
    // during CFS bring-up. Users can't fix these directly — power cycle is the
    // standard remedy. Surfacing them avoids the raw chinglish from Creality
    // ("Motor set pin restore io status error").
    {"key800",
     {TR_NOOP("Motor controller error"),
      TR_NOOP("A motor IO pin couldn't be restored. Power-cycle the printer to recover"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key801",
     {TR_NOOP("Z motor calibration failed"),
      TR_NOOP("Stall detection setup failed for the Z motor — power-cycle to retry"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key802",
     {TR_NOOP("Extruder motor calibration failed"),
      TR_NOOP("Stall detection setup failed for the extruder — power-cycle to retry"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key803",
     {TR_NOOP("Motor parameter setup failed"),
      TR_NOOP("A motor's parameters couldn't be written. Power-cycle the printer to retry"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},

    {"key831",
     {TR_NOOP("Lost connection to CFS unit"),
      TR_NOOP("Check the RS-485 cable between printer and CFS"), AmsAlertLevel::SYSTEM,
      ErrorSeverity::CRITICAL, nullptr}},
    {"key832",
     {TR_NOOP("Retract failed"),
      TR_NOOP("Check the spool and the filament path for a jam, then retry"), AmsAlertLevel::SLOT,
      ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key833",
     {TR_NOOP("Feed failed"),
      TR_NOOP("Check the spool and the filament path for a jam, then retry"), AmsAlertLevel::SLOT,
      ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key834",
     {TR_NOOP("CFS system error"),
      TR_NOOP(
          "The unit reported a fault with no further detail. Restart the printer if it repeats"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key835",
     {TR_NOOP("Filament never reached the CFS hub sensor"),
      TR_NOOP("Check the path from the slot to the hub for a tangle or drag. If the slot feeds "
              "freely, the "
              "hub sensor or the feeder motor may have failed"),
      AmsAlertLevel::SLOT, ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key836",
     {TR_NOOP("Filament stalled between CFS hub and extruder sensor"),
      TR_NOOP(
          "Check the PTFE tube along the drag chain for kinks, and the bend where it enters the "
          "extruder sensor"),
      AmsAlertLevel::SLOT, ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key837",
     {TR_NOOP("Filament jammed at the extruder gear"),
      TR_NOOP(
          "Retract to unload. If it comes back cleanly, snip the chewed end off and feed again"),
      AmsAlertLevel::SLOT, ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key838",
     {TR_NOOP("Filament stuck inside the CFS hub"),
      TR_NOOP("It is jammed between the hub sensor and the hub gear. Open the CFS and clear the "
              "hub before "
              "retrying"),
      AmsAlertLevel::SLOT, ErrorSeverity::CRITICAL, fmt_unit_slot}},
    // key839 restates the slot's emptiness the runout flow is already
    // prompting about (#1388) and offers no action that prompt lacks, so a
    // blocking modal would stack a second one on one runout.
    {"key839",
     {TR_NOOP("Filament ran out"),
      TR_NOOP("Load a new spool in this slot, or turn on Auto-Refill to switch to a matching one"),
      AmsAlertLevel::SLOT, ErrorSeverity::WARNING, fmt_unit_slot}},
    // key840 is "command not supported in the current scenario": the box is
    // running something else and clears when it finishes. Blocking would ask
    // for a decision about a healthy unit, so it is a toast that keeps the
    // reset reachable for a box that really is wedged.
    {"key840",
     {TR_NOOP("CFS is busy"),
      TR_NOOP("The unit is already running another operation. Wait for it to finish, or reset the "
              "CFS if "
              "it stays stuck"),
      AmsAlertLevel::UNIT, ErrorSeverity::WARNING, fmt_unit_only, /*prefer_fw_msg=*/false,
      &kResetBoxAction}},
    {"key841",
     {TR_NOOP("Filament cutter stuck"),
      TR_NOOP("The cutter blade didn't return — check for filament wrapped around the cutting "
              "mechanism"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    // key843 is most often the box answering busy mid-operation rather than a
    // broken tag (#1387). It self-resolves, and a blocking modal's advice
    // would assert one cause for all of them, so it is a toast.
    {"key843",
     {TR_NOOP("Can't read filament RFID tag"),
      // The busy-box case (#1387) needs no spool handling at all, so the
      // re-seat remedy is conditional, not asserted: the firmware msg names
      // the actual cause.
      TR_NOOP(
          "Set the filament type and color by hand for this slot. If the tag should be readable, "
          "re-seat the spool with its label facing the reader once the box is idle"),
      AmsAlertLevel::SLOT, ErrorSeverity::WARNING, fmt_unit_slot, /*prefer_fw_msg=*/true}},
    {"key844",
     {TR_NOOP("PTFE tube connection loose"),
      TR_NOOP("Re-seat the Bowden tube connector on the CFS unit"), AmsAlertLevel::UNIT,
      ErrorSeverity::CRITICAL, fmt_unit_only}},
    {"key845",
     {TR_NOOP("Nozzle clog detected"), TR_NOOP("Run a cold pull or replace the nozzle"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key846",
     {TR_NOOP("Filament buffer stopped moving"),
      TR_NOOP(
          "The buffer saw no movement for 16 seconds. Check for a blockage between the CFS and the "
          "extruder, or a stuck buffer"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key847",
     {TR_NOOP("Filament is dragging"),
      TR_NOOP("The extruder cannot pull it smoothly. Check the spool for a tangle or knot, and the "
              "PTFE "
              "tube for a severe bend"),
      AmsAlertLevel::SLOT, ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key848",
     {TR_NOOP("Filament snapped inside CFS"),
      TR_NOOP("Open the CFS unit and remove the broken filament from the slot"),
      AmsAlertLevel::SLOT, ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key849",
     {TR_NOOP("Retract failed — filament stuck in connector"),
      TR_NOOP("Manually pull the filament back through the connector"), AmsAlertLevel::SLOT,
      ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key850",
     {TR_NOOP("Retract error — multiple connectors triggered"),
      TR_NOOP("Check that only one filament path is active"), AmsAlertLevel::UNIT,
      ErrorSeverity::CRITICAL, fmt_unit_only}},
    {"key851",
     {TR_NOOP("Retract didn't reach buffer empty position"),
      TR_NOOP("The filament may not have fully retracted — try again or manually pull"),
      AmsAlertLevel::SLOT, ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key852",
     {TR_NOOP("Sensor mismatch — check extruder and CFS sensors"),
      TR_NOOP("Extruder and CFS disagree on filament state — inspect both sensors"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key853",
     {TR_NOOP("Temperature and humidity sensor not responding"),
      TR_NOOP(
          "The sensor in this CFS unit cannot be read. Check its connection, or the unit may need "
          "service"),
      AmsAlertLevel::UNIT, ErrorSeverity::CRITICAL, fmt_unit_only}},
    {"key854",
     {TR_NOOP("Cutter blade didn't sever filament"),
      TR_NOOP("Filament is still present after the cut — the blade may be dull or misaligned"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key855",
     {TR_NOOP("Filament cutter position error"),
      TR_NOOP("The cutter is out of alignment — recalibrate with CALIBRATE_CUT_POS"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key856",
     {TR_NOOP("Filament cutter not detected"),
      TR_NOOP("Check that the cutter mechanism is properly installed"), AmsAlertLevel::SYSTEM,
      ErrorSeverity::CRITICAL, nullptr}},
    {"key857",
     {TR_NOOP("CFS motor overloaded"),
      TR_NOOP("A spool may be tangled or the drive gear is jammed"), AmsAlertLevel::UNIT,
      ErrorSeverity::CRITICAL, fmt_unit_only}},
    {"key858",
     {TR_NOOP("EEPROM error on CFS unit"),
      TR_NOOP("The unit's onboard storage has failed. The CFS mainboard needs replacing"),
      AmsAlertLevel::UNIT, ErrorSeverity::CRITICAL, fmt_unit_only}},
    {"key859",
     {TR_NOOP("Measuring wheel error"),
      TR_NOOP("The CFS odometer hardware is faulty and needs repair or replacement"),
      AmsAlertLevel::UNIT, ErrorSeverity::CRITICAL, fmt_unit_only}},
    {"key860",
     {TR_NOOP("Buffer tube problem"), TR_NOOP("Check the buffer unit on the back of the printer"),
      AmsAlertLevel::SYSTEM, ErrorSeverity::CRITICAL, nullptr}},
    {"key861",
     {TR_NOOP("RFID reader malfunction (left)"),
      TR_NOOP("The left RFID reader in this CFS unit may need service"), AmsAlertLevel::UNIT,
      ErrorSeverity::CRITICAL, fmt_unit_only}},
    {"key862",
     {TR_NOOP("RFID reader malfunction (right)"),
      TR_NOOP("The right RFID reader in this CFS unit may need service"), AmsAlertLevel::UNIT,
      ErrorSeverity::CRITICAL, fmt_unit_only}},
    {"key863",
     {TR_NOOP("Retract error — filament still detected"),
      TR_NOOP("Filament didn't fully retract, may need manual removal"), AmsAlertLevel::SLOT,
      ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key864",
     {TR_NOOP("Filament buffer failure during feed"),
      TR_NOOP("The buffer may be disconnected, stuck, or faulty. Check it before retrying"),
      AmsAlertLevel::SLOT, ErrorSeverity::CRITICAL, fmt_unit_slot}},
    {"key865",
     {TR_NOOP("Retract error — failed to exit connector"),
      TR_NOOP("Filament stuck in connector during unload"), AmsAlertLevel::SLOT,
      ErrorSeverity::CRITICAL, fmt_unit_slot}},
    // key866 is a second "no cutter" variant emitted from the motor driver path
    // (key856 comes from the box driver). Same root cause, same remedy.
    {"key866",
     {TR_NOOP("Filament cutter not detected"),
      TR_NOOP("Check that the cutter mechanism is properly installed"), AmsAlertLevel::SYSTEM,
      ErrorSeverity::CRITICAL, nullptr}},
};

} // namespace

const KlipperErrorEntry* klipper_error_lookup(const std::string& key_code) {
    auto it = KlipperErrorTable.find(key_code);
    return it == KlipperErrorTable.end() ? nullptr : &it->second;
}

KlipperErrorClassification klipper_error_classify(const std::string& key_code) {
    KlipperErrorClassification c;
    // The table is the CFS family's code space, Klipper-layer codes included.
    c.source = ErrorSource::CFS;
    const KlipperErrorEntry* entry = klipper_error_lookup(key_code);
    if (entry) {
        c.severity = entry->severity;
    } else {
        c.severity = is_box_fault(key_code) ? ErrorSeverity::CRITICAL : ErrorSeverity::WARNING;
    }
    if (const char* title = klipper_error_modal_title(key_code)) {
        c.title = title;
    }
    if (entry && entry->recovery) {
        c.recovery_actions.push_back(
            {lv_tr(entry->recovery->label), entry->recovery->gcode, entry->recovery->log_tag});
    }
    return c;
}

const char* klipper_error_modal_title(const std::string& key_code) {
    return is_box_fault(key_code) ? lv_tr("Filament System Error") : nullptr;
}

} // namespace helix::printer
