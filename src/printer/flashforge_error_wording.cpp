// SPDX-License-Identifier: GPL-3.0-or-later
//
// The prose errors FlashForge's community firmwares raise, as error_wording
// row sets: ZMOD (AD5M, AD5X, Creator 5), Forge-X (AD5M) and OpenCreator
// (Creator 5). None of them carries a code, so each row keys on the anchored
// prefix of its message. ZMOD's Python modules speak English or Russian by
// its language setting, so its IFS rows carry both. Messages raised from
// ZMOD's macros follow its 13 translate/ folders and are matched in English
// only.

#include "error_wording_rows.h"
#include "printer_discovery.h"
#include "translation_loader.h"

#include <algorithm>
#include <iterator>
#include <string>

namespace helix::error_wording::flashforge {
namespace {

using C = Category;
using S = Severity;

bool has_object_prefix(const PrinterDiscovery& hw, const std::string& prefix) {
    const auto& objects = hw.printer_objects();
    return std::any_of(objects.begin(), objects.end(),
                       [&prefix](const std::string& o) { return o.rfind(prefix, 0) == 0; });
}

bool has_object(const PrinterDiscovery& hw, const char* name) {
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), name) != objects.end();
}

/// Every ZMOD module registers a `zmod_*` object (zmod_ifs, zmod_color,
/// zmod_tenz, zmod_afclite).
bool detect_zmod(const PrinterDiscovery& hw) {
    return has_object_prefix(hw, "zmod");
}

bool detect_forge_x(const PrinterDiscovery& hw) {
    return hw.has_mod_params();
}

bool detect_opencreator(const PrinterDiscovery& hw) {
    return has_object(hw, "creator5_toolchanger") || has_object(hw, "c5_motion_guard");
}

// Wording several rows share.
constexpr const char* kExtractSummary = TR_NOOP("Filament stuck in the extruder");
constexpr const char* kExtractCause =
    TR_NOOP("After the cut and retract, the toolhead sensor still sees filament: the cut "
            "failed, the tip is stuck in the gears, or the sensor is dirty.");
constexpr const char* kExtractFix =
    TR_NOOP("Heat the nozzle, pull the filament out by hand at the toolhead and check the cutter "
            "blade, then try again.");
constexpr const char* kNoFilamentSummary = TR_NOOP("No filament in IFS lane {1}");
constexpr const char* kNoFilamentCause =
    TR_NOOP("Lane {1}'s sensor lost the filament while feeding: the spool is empty, the filament "
            "broke, or it never went in past the sensor.");
constexpr const char* kNoFilamentFix =
    TR_NOOP("Insert filament into lane {1} until the IFS grabs it, then load again.");
constexpr const char* kNoReplySummary = TR_NOOP("The IFS did not answer");
constexpr const char* kNoReplyCause =
    TR_NOOP("The IFS board did not reply in time: a cable problem, a board that hung, or the "
            "native screen using the same port.");
constexpr const char* kNoReplyFix =
    TR_NOOP("Check the IFS cable, make sure the native screen is off, and power the printer off "
            "and on.");
constexpr const char* kStateTimeoutSummary = TR_NOOP("The IFS did not finish moving filament");
constexpr const char* kStateTimeoutCause =
    TR_NOOP("The IFS did not reach the state it was waiting for in time, so the filament is not "
            "moving: usually a jam in that lane's tube or at the toolhead entry.");
constexpr const char* kStateTimeoutFix =
    TR_NOOP("Check the lane's tube and the toolhead entry for a jam. For a load, check that the "
            "toolhead sensor works.");
constexpr const char* kNozzleHitSummary = TR_NOOP("The nozzle hit the print or the bed");
constexpr const char* kResumeHotSummary = TR_NOOP("Heat the nozzle before resuming");
constexpr const char* kResumeHotCause =
    TR_NOOP("The nozzle cooled during the pause, so the firmware refused to resume.");
constexpr const char* kResumeHotFix = TR_NOOP("Heat the nozzle to the print temperature, then "
                                              "resume.");
constexpr const char* kResumeEmptySummary = TR_NOOP("Load filament before resuming");
constexpr const char* kResumeEmptyCause =
    TR_NOOP("The filament sensor still reads empty, so the firmware refused to resume.");
constexpr const char* kResumeEmptyFix = TR_NOOP("Load filament, then resume.");
constexpr const char* kTareSummary = TR_NOOP("The load cell would not zero");
constexpr const char* kTareCause =
    TR_NOOP("Something rests on the bed, the nozzle is touching it, or the load cell has a "
            "fault.");
constexpr const char* kTareFix = TR_NOOP("Clear the bed and the nozzle, then try again.");
constexpr const char* kDamagedFileSummary = TR_NOOP("The print file is damaged");
constexpr const char* kDamagedFileCause =
    TR_NOOP("The file's checksum does not match its contents, so the upload was corrupted. The "
            "print was canceled.");
constexpr const char* kDamagedFileFix = TR_NOOP("Upload the file again.");
constexpr const char* kToolSensorSummary = TR_NOOP("Toolhead sensors disagree");
constexpr const char* kToolSensorCause =
    TR_NOOP("A dock or grab sensor reads wrong: a head not seated in its dock, a dirty or failed "
            "switch, or the carriage still holding a head.");
constexpr const char* kToolSensorFix =
    TR_NOOP("Check that every head is seated in its dock and that the dock and grab switches "
            "work.");

// The dialogs ZMOD's macros raise for a fault they also print as `!!`. Its
// RESUME refusal repeats the reason under a lone Ok; some builds drop the space
// before "!!!", so the title is matched without it. The Creator 5's runout
// dialog for a head offers to keep printing while the user loads by hand, which
// the pause that follows overtakes.
constexpr const char* kResumeRefusedPrompt = "Resumption interrupted";
constexpr const char* kHeadRunoutPrompt = "T{1} runout";

// ZMOD. IFS rows come from zmod_ifs.py, the rest from its English macros.
constexpr Row kZmodRows[] = {
    {"FF-I15", "Filament jam detected (IFS).", nullptr, nullptr, false, C::Filament, S::FollowPrint,
     TR_NOOP("Filament jam in the IFS"),
     TR_NOOP("The IFS motion sensor saw no filament movement while printing: a tangled spool, a "
             "kinked tube, the wrong slot recorded as loaded, or old filament still in the "
             "head."),
     TR_NOOP("Check the active spool and its tube. If the slot is wrong, set it with "
             "SET_EXTRUDER_SLOT. For false alarms, raise detection_length of the IFS motion "
             "sensor (8 to 17).")},
    {"FF-I1", "Failed to extract filament from extruder", nullptr, nullptr, false, C::Filament,
     S::FollowPrint, kExtractSummary, kExtractCause, kExtractFix},
    {"FF-I1", "Не удалось извлечь пруток из экструдера", nullptr, nullptr, false, C::Filament,
     S::FollowPrint, kExtractSummary, kExtractCause, kExtractFix},
    {"FF-I2", "No filament ", " in IFS", "^No filament (\\d+) in IFS", false, C::Filament,
     S::FollowPrint, kNoFilamentSummary, kNoFilamentCause, kNoFilamentFix},
    {"FF-I2", "Нет прутка ", " в IFS", "^Нет прутка (\\d+) в IFS", false, C::Filament,
     S::FollowPrint, kNoFilamentSummary, kNoFilamentCause, kNoFilamentFix},
    {"FF-I3", "Filament ", " stalled in IFS", "^Filament (\\d+) stalled in IFS", false, C::Filament,
     S::FollowPrint, TR_NOOP("Filament stalled in IFS lane {1}"),
     TR_NOOP("Lane {1}'s motor turns but its motion sensor sees no movement: a tangle on the "
             "spool, a kinked PTFE tube, or a slipping drive roller."),
     TR_NOOP("Check that the spool turns freely and lane {1}'s tube is not kinked, and clear "
             "debris from the pinch roller.")},
    {"FF-I6", "IFS driver failure", nullptr, nullptr, false, C::Filament, S::FollowPrint,
     TR_NOOP("IFS driver failure"),
     TR_NOOP("The IFS board reported a driver error on every retry."),
     TR_NOOP("Power the printer off and on and reseat the IFS cable. IFS_F15 resets the "
             "driver.")},
    {"FF-I8", "Timeout waiting for response from command ", nullptr, nullptr, false, C::Filament,
     S::FollowPrint, kNoReplySummary, kNoReplyCause, kNoReplyFix},
    {"FF-I8", "Таймаут ожидания ответа от команды", nullptr, nullptr, false, C::Filament,
     S::FollowPrint, kNoReplySummary, kNoReplyCause, kNoReplyFix},
    {"FF-I9", "IFS: Timeout waiting for status ", nullptr, nullptr, false, C::Filament,
     S::FollowPrint, kStateTimeoutSummary, kStateTimeoutCause, kStateTimeoutFix},
    {"FF-I9", "IFS: Вышло время для получения статуса", nullptr, nullptr, false, C::Filament,
     S::FollowPrint, kStateTimeoutSummary, kStateTimeoutCause, kStateTimeoutFix},
    {"FF-I17", "Filament runout detected (Head)", nullptr, nullptr, false, C::Filament,
     S::FollowPrint, TR_NOOP("Filament ran out at the toolhead"),
     TR_NOOP("The toolhead sensor went empty, and no other lane holds the same type and color, "
             "or backup is off."),
     TR_NOOP("Load filament into the empty lane or another one, then resume.")},
    {"FF-Z7", "Filament runout detected T", nullptr, "^Filament runout detected T(\\d+)", false,
     C::Filament, S::FollowPrint, TR_NOOP("Filament ran out on head T{1}"),
     TR_NOOP("Head T{1}'s filament sensor went empty."),
     TR_NOOP("Load filament into head T{1}, then resume."), kHeadRunoutPrompt},
    {"FF-Z8", "Filament jam detected T", nullptr, "^Filament jam detected T(\\d+)", false,
     C::Filament, S::FollowPrint, TR_NOOP("Clog in head T{1}"),
     TR_NOOP("Head T{1} has filament, but its motion sensor stopped seeing it move."),
     TR_NOOP("Clear the clog in head T{1} (a cold pull works), then resume.")},
    {"FF-Z5", "Filament runout or jam detected", nullptr, nullptr, false, C::Filament,
     S::FollowPrint, TR_NOOP("Filament ran out or jammed"),
     TR_NOOP("The filament sensor saw the filament stop: the spool is empty, or the filament is "
             "not moving."),
     TR_NOOP("Load filament or clear the jam, then resume. For false alarms, tune "
             "detection_length (8 to 17).")},
    {"FF-Z6", "Filament runout detected", nullptr, nullptr, false, C::Filament, S::FollowPrint,
     TR_NOOP("Filament ran out"), TR_NOOP("The filament sensor reads empty."),
     TR_NOOP("Load filament, then resume.")},
    {"FF-Z1", "Nozzle hit bed or part detachment. Weight ", ". PAUSE",
     "^Nozzle hit bed or part detachment\\. Weight ([0-9.]+)>([0-9.]+)\\. "
     "Z=(-?[0-9]+(?:\\.[0-9]+)?)",
     false, C::Probe, S::FollowPrint, kNozzleHitSummary,
     TR_NOOP("The bed's load cell read {1} g, above the {2} g limit, at Z {3}: the nozzle is "
             "dragging on the part, a part came loose, or the print warped. The print paused."),
     TR_NOOP("Remove any loose part and clear the nozzle, then resume. For false alarms, raise "
             "the limit with NOZZLE_CONTROL WEIGHT=.")},
    {"FF-Z2", "Nozzle hit bed or part detachment. Weight ", ". FIRMWARE_RESTART",
     "^Nozzle hit bed or part detachment\\. Weight ([0-9.]+)>([0-9.]+)\\. "
     "Z=(-?[0-9]+(?:\\.[0-9]+)?)",
     false, C::Probe, S::FollowPrint, kNozzleHitSummary,
     TR_NOOP("The bed's load cell read {1} g, above the {2} g limit, at Z {3}, and the printer "
             "shut down to protect itself."),
     TR_NOOP("Clear the bed and the nozzle, then restart the firmware.")},
    {"FF-Z3", "Cell Tare: Error.", nullptr, nullptr, false, C::Probe, S::FollowPrint, kTareSummary,
     kTareCause, kTareFix},
    {"FF-Z4", "Load cell reset error.", nullptr, nullptr, false, C::Probe, S::FollowPrint,
     kTareSummary, kTareCause, kTareFix},
    {"FF-Z9", "Resumption interrupted !!! ", "not hot enough", nullptr, false, C::Command,
     S::FollowPrint, kResumeHotSummary, kResumeHotCause, kResumeHotFix, kResumeRefusedPrompt},
    {"FF-Z10", "Resumption interrupted !!! ", "filament has run out", nullptr, false, C::Filament,
     S::FollowPrint, kResumeEmptySummary, kResumeEmptyCause, kResumeEmptyFix, kResumeRefusedPrompt},
    {"FF-Z13", "File ", " MD5 mismatch", nullptr, false, C::Command, S::FollowPrint,
     kDamagedFileSummary, kDamagedFileCause, kDamagedFileFix},
    {"FF-Z13", "MD5 check failed. Print canceled.", nullptr, nullptr, false, C::Command,
     S::FollowPrint, kDamagedFileSummary, kDamagedFileCause, kDamagedFileFix},
    {"FF-Z14", "File ", " contains arc commands (G2, G3)", nullptr, false, C::Command,
     S::FollowPrint, TR_NOOP("The file uses arc moves"),
     TR_NOOP("The file contains G2 and G3 arc moves, which this firmware refuses."),
     TR_NOOP("Turn off arc fitting in the slicer and slice the file again.")},
    {"FF-Z15", "WARNING: Last probe ", nullptr, nullptr, false, C::Probe, S::FollowPrint,
     TR_NOOP("The bed no longer matches the saved mesh"),
     TR_NOOP("The probe before the print disagrees with the saved bed mesh: a different plate, a "
             "different temperature, or debris on the bed."),
     TR_NOOP("Clean the plate and run a new bed mesh, or let ZMOD measure before each print "
             "(SAVE_ZMOD_DATA MESH_TEST).")},
};

// Forge-X: its macros and klippy plugins.
constexpr Row kForgeXRows[] = {
    {"FF-F1", "Bed pressure detected: ", nullptr, "^Bed pressure detected: ([0-9.]+) g", false,
     C::Probe, S::FollowPrint, TR_NOOP("Pressure on the bed"),
     TR_NOOP("The load cell read {1} g on the bed: the nozzle hit the print or a part came "
             "loose. During a print the firmware stops the printer."),
     TR_NOOP("Clear the nozzle and the part, then restart the firmware.")},
    {"FF-F3", "Load cell tare failed", nullptr, nullptr, false, C::Probe, S::FollowPrint,
     kTareSummary, kTareCause, kTareFix},
    {"FF-F6", "Unable to start print. Filament runout detected!", nullptr, nullptr, false,
     C::Filament, S::FollowPrint, TR_NOOP("No filament loaded"),
     TR_NOOP("The runout sensor reads empty, so the print did not start."),
     TR_NOOP("Load filament, then start the print again.")},
    {"FF-F8", "Resume aborted !!! ", "not hot enough", nullptr, false, C::Command, S::FollowPrint,
     kResumeHotSummary, kResumeHotCause, kResumeHotFix},
    {"FF-F8", "Resume aborted !!! ", "detects no filament", nullptr, false, C::Filament,
     S::FollowPrint, kResumeEmptySummary, kResumeEmptyCause, kResumeEmptyFix},
};

// OpenCreator: its Kalico toolchanger and motion guard.
constexpr Row kOpenCreatorRows[] = {
    {"FF-C1", "E0163: Paused. T", " clog detected", "^E0163: Paused\\. T(\\d+) clog detected",
     false, C::Filament, S::FollowPrint, TR_NOOP("Clog in head T{1}"),
     TR_NOOP("Head T{1} has filament, but its filament wheel stopped moving."),
     TR_NOOP("Clear head T{1}'s filament path (a cold pull works), then resume.")},
    {"FF-C3", "Conflicting dock/grab sensors", nullptr, nullptr, false, C::Homing, S::FollowPrint,
     kToolSensorSummary, kToolSensorCause, kToolSensorFix},
    {"FF-C3", "Multiple heads appear attached", nullptr, nullptr, false, C::Homing, S::FollowPrint,
     kToolSensorSummary, kToolSensorCause, kToolSensorFix},
    {"FF-C3", "Missing dock confirmation for T", nullptr, nullptr, false, C::Homing, S::FollowPrint,
     kToolSensorSummary, kToolSensorCause, kToolSensorFix},
    {"FF-C4", "T", " stayed in its dock after ", "^T(\\d+) stayed in its dock after (\\d+) grabs",
     false, C::Homing, S::FollowPrint, TR_NOOP("Could not pick up head T{1}"),
     TR_NOOP("Head T{1} stayed in its dock after {2} tries, and its heaters were turned off."),
     TR_NOOP("Home the printer, check that the head moves freely in its dock, then try again.")},
    {"FF-C5", "T", " is not seated in its dock", "^T(\\d+) is not seated in its dock", false,
     C::Homing, S::FollowPrint, TR_NOOP("Head T{1} is not seated in its dock"),
     TR_NOOP("Head T{1} did not seat in its dock, so it was not released and its heaters are "
             "off. X and Y may have lost steps."),
     TR_NOOP("Home X and Y, then dock the head again.")},
    {"FF-C7", "Motion guard: ", nullptr, "^Motion guard: (\\S+) (?:skipped steps|stalled)", false,
     C::Move, S::FollowPrint, TR_NOOP("The {1} motor lost steps"),
     TR_NOOP("The {1} motor skipped steps or stalled: the toolhead hit the print, or a belt "
             "slipped."),
     TR_NOOP("Remove whatever blocks the axis and home again before resuming.")},
};

} // namespace

RowSet zmod_rows() {
    return {kZmodRows, std::size(kZmodRows), detect_zmod};
}

RowSet forge_x_rows() {
    return {kForgeXRows, std::size(kForgeXRows), detect_forge_x};
}

RowSet opencreator_rows() {
    return {kOpenCreatorRows, std::size(kOpenCreatorRows), detect_opencreator};
}

} // namespace helix::error_wording::flashforge
