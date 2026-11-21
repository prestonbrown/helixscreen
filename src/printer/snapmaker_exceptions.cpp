// src/printer/snapmaker_exceptions.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_exceptions.h"

#include "core/text_io.h"
#include "display_numbering.h"
#include "lvgl.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <cctype>

#include "hv/json.hpp"

namespace helix::snapmaker {
namespace {

// The U1's fault codes and what they mean to a user, one row per (module id,
// code) or per run of codes with one meaning. docs/devel/printers/SNAPMAKER_U1_SUPPORT.md
// ("Error codes") is the map these rows follow, read from the firmware's own
// sources. A code missing here keeps the firmware's text: 522-0 (any uncoded
// command error), 523-9000, 530-9 and 531-23 all carry the real reason in the
// message.
//
// A summary with "{}" names the head the fault's index points at; the rest name
// none, because their index is a board, an axis or always 0.
struct FaultRow {
    int id;
    int code_first;
    int code_last;
    FaultWording wording;
};

// Fixes several rows share.
constexpr const char* kRestartFirmwareFix =
    TR_NOOP("Restart the firmware. If it happens again, power the printer off and on and check the "
            "cables to the toolheads.");
constexpr const char* kConfigFix =
    TR_NOOP("Undo recent changes to the printer's configuration, then restart the firmware.");
constexpr const char* kReseatHeadFix =
    TR_NOOP("Seat the toolhead fully in its dock or on the carriage, home the printer, then "
            "resume.");
constexpr const char* kPogoFix = TR_NOOP("Clean the pogo pins on the toolhead and the carriage, "
                                         "seat the toolhead again, then retry.");
constexpr const char* kResliceFix =
    TR_NOOP("Slice the file again with the U1 profile, or fix the macro or command you sent.");
constexpr const char* kHomeFirstFix = TR_NOOP("Home all axes, then try again.");
constexpr const char* kCalibrationRetryFix =
    TR_NOOP("Clean the nozzle tip and the build plate, then run the calibration again.");
constexpr const char* kLoadSummary = TR_NOOP("{} could not load its filament");
constexpr const char* kReloadFix = TR_NOOP("Cut the filament end square and push it into this "
                                           "head's feeder until it grabs, then load the head "
                                           "again.");
constexpr const char* kInsertFix =
    TR_NOOP("Insert filament into this head's feeder until it grabs, then load the head again.");
constexpr const char* kPathFix = TR_NOOP("Check the spool for a tangle and the feed tube for a "
                                         "kink or a loose fitting, then load the head again.");
constexpr const char* kSwitchSummary = TR_NOOP("Toolhead change stopped at {}");
constexpr const char* kTwoPickedCause =
    TR_NOOP("More than one toolhead reads as picked up at the same time.");
constexpr const char* kTwoPickedFix = TR_NOOP("Seat every toolhead that is not on the carriage "
                                              "fully in its dock, then home and resume. If it "
                                              "happens again, the dock sensors need checking.");
constexpr const char* kDetachedCause =
    TR_NOOP("A toolhead is neither in its dock nor on the carriage.");
constexpr const char* kConflictCause =
    TR_NOOP("A toolhead reads as docked and picked up at the same time.");
constexpr const char* kPogoCause = TR_NOOP("The toolhead's pogo pins are not making contact.");
constexpr const char* kPrintSettingSummary = TR_NOOP("Print settings not accepted");

constexpr FaultRow kFaultRows[] = {
    // 522: motion, boards and shutdowns. The index is a board or an axis.
    {522,
     2,
     2,
     {TR_NOOP("The printer shut down"),
      TR_NOOP("The printer stopped itself for safety; the firmware gave no specific reason."),
      kRestartFirmwareFix}},
    {522,
     3,
     4,
     {TR_NOOP("Printer configuration error"),
      TR_NOOP("A setting or pin in the printer's configuration is invalid."), kConfigFix}},
    {522,
     5,
     5,
     {TR_NOOP("Board firmware does not match"),
      TR_NOOP("A controller board runs firmware that does not match the printer software."),
      TR_NOOP("Run the official firmware update again so every board is updated, then "
              "restart.")}},
    {522,
     6,
     6,
     {TR_NOOP("A controller board did not answer"),
      TR_NOOP("The printer could not connect to one of its boards at startup."),
      TR_NOOP("Power the printer off and on. Check that every toolhead is seated and its cable "
              "is connected.")}},
    {522,
     7,
     7,
     {TR_NOOP("The printer could not start"),
      TR_NOOP("An unexpected internal error stopped the printer from starting."),
      kRestartFirmwareFix}},
    {522,
     8,
     8,
     {TR_NOOP("Lost contact with a controller board"),
      TR_NOOP("A board stopped answering while the printer was running."),
      TR_NOOP("Power the printer off and on, and check the toolhead cables and their "
              "connectors.")}},
    {522,
     9,
     10,
     {TR_NOOP("Accelerometer not responding"),
      TR_NOOP("The accelerometer did not respond as expected, usually from a loose cable."),
      TR_NOOP("Restart and try again. If it happens again, check the accelerometer cable.")}},
    {522,
     11,
     11,
     {TR_NOOP("Motor driver fault"),
      TR_NOOP("A motor driver reported a fault such as overheating or a short."),
      TR_NOOP("Power off, let the printer cool, check that nothing blocks the axis, then "
              "restart.")}},
    {522,
     12,
     12,
     {TR_NOOP("Move before homing"), TR_NOOP("A move was sent before that axis was homed."),
      kHomeFirstFix}},
    {522,
     13,
     13,
     {TR_NOOP("Move out of range"), TR_NOOP("A move went past the printer's travel limits."),
      TR_NOOP("Slice the file again with the U1 profile and check that the model fits on the "
              "plate.")}},
    {522,
     14,
     14,
     {TR_NOOP("Temperature sensor out of range"),
      TR_NOOP("A temperature reading went outside its valid range."),
      TR_NOOP("Restart the firmware and check the temperature sensor cables.")}},
    {522,
     15,
     15,
     {TR_NOOP("Heater not heating as expected"),
      TR_NOOP("A heater did not warm up at the expected rate."),
      TR_NOOP("Restart the firmware and check that heater's wiring.")}},
    {522,
     16,
     16,
     {TR_NOOP("A controller board fell behind"),
      TR_NOOP("A board could not keep up with its timing and shut down."),
      TR_NOOP("Restart the firmware. If it happens during prints, avoid heavy extra load such as "
              "camera streaming or large uploads.")}},
    {522,
     17,
     17,
     {TR_NOOP("Power was lost"),
      TR_NOOP("Power was lost during the print; the printer stopped and saved its progress"),
      TR_NOOP("Check the power cable and outlet. When power is back, resume the print if the "
              "printer offers it.")}},
    {522,
     18,
     19,
     {TR_NOOP("The printer was stopped"),
      TR_NOOP("An emergency stop or a shutdown request stopped the printer."),
      TR_NOOP("Make sure the printer is safe, then restart the firmware.")}},

    // 523: the toolheads. The index is the head.
    {523,
     0,
     0,
     {TR_NOOP("{} ran out of filament"),
      TR_NOOP("The head's filament sensor sees no filament: the spool ran out, or the filament "
              "broke before the head."),
      TR_NOOP("Load filament into this head, then resume. If filament is there, check for a "
              "break between the spool and the head.")}},
    {523,
     1,
     1,
     {TR_NOOP("{} is too cold to extrude"),
      TR_NOOP("The nozzle is below the lowest temperature allowed for extruding."),
      TR_NOOP("Heat the head to the filament's temperature and check that it heats at all.")}},
    {523,
     2,
     2,
     {TR_NOOP("{} temperature reading failed"),
      TR_NOOP("The nozzle temperature reading is out of range: its sensor is loose or damaged."),
      TR_NOOP("Power off, seat the toolhead again and check its connector. If it happens again, "
              "the hotend sensor needs replacing.")}},
    {523,
     3,
     3,
     {TR_NOOP("{} is not heating"), TR_NOOP("The nozzle did not heat up at the expected rate."),
      TR_NOOP("Restart, seat the toolhead again and check that the hotend is fully inserted.")}},
    {523,
     4,
     5,
     {TR_NOOP("Extrusion too large for {}"),
      TR_NOOP("The file asks for more extrusion than the head allows."),
      TR_NOOP("Slice the file again with the right nozzle size and the U1 profile.")}},
    {523,
     6,
     6,
     {TR_NOOP("Toolhead not found"),
      TR_NOOP("A temperature was set for a toolhead number that does not exist."),
      TR_NOOP("Slice the file again with the U1 profile.")}},
    {523, 7, 17, {kSwitchSummary, kTwoPickedCause, kTwoPickedFix}},
    {523,
     18,
     18,
     {kSwitchSummary, TR_NOOP("A toolhead is in an unknown position, so the change did not start."),
      kReseatHeadFix}},
    {523,
     19,
     19,
     {kSwitchSummary,
      TR_NOOP("The head on the carriage does not read as properly picked up, so it was not "
              "parked."),
      kReseatHeadFix}},
    {523,
     20,
     20,
     {kSwitchSummary, TR_NOOP("After parking, the head does not read as docked."), kReseatHeadFix}},
    {523,
     21,
     21,
     {kSwitchSummary, TR_NOOP("The head to pick up does not read as docked."), kReseatHeadFix}},
    {523,
     22,
     22,
     {kSwitchSummary, TR_NOOP("A different toolhead than the one asked for is on the carriage."),
      kReseatHeadFix}},
    {523,
     23,
     23,
     {kSwitchSummary, TR_NOOP("The pick-up failed: every toolhead still reads as docked."),
      TR_NOOP("Clear any debris from the carriage and the head's latch, seat the head again, "
              "then home and resume.")}},
    {523, 24, 34, {kSwitchSummary, kTwoPickedCause, kTwoPickedFix}},
    {523,
     35,
     35,
     {kSwitchSummary, TR_NOOP("The pick-up failed with a toolhead in an unknown position."),
      kReseatHeadFix}},
    {523,
     36,
     36,
     {TR_NOOP("Temperature out of range for {}"),
      TR_NOOP("The nozzle temperature asked for is outside the allowed range."),
      TR_NOOP("Check the nozzle temperature in the slicer's filament profile.")}},
    {523,
     37,
     37,
     {TR_NOOP("Toolhead maintenance due"),
      TR_NOOP("The toolheads have changed enough times since their last maintenance."),
      TR_NOOP("Clean the docks and the pogo pins, then reset the maintenance counter.")}},
    {523,
     38,
     38,
     {TR_NOOP("{} filament is tangled"),
      TR_NOOP("The extruder is pulling but the feeder is not turning: the filament is tangled or "
              "stuck between the spool and the head."),
      TR_NOOP("Check the spool for a tangle or a stuck spool, clear the filament path, then "
              "resume.")}},
    {523,
     39,
     39,
     {TR_NOOP("{} has no filament type set"),
      TR_NOOP("No filament type is set for a toolhead this print uses."),
      TR_NOOP("Set the filament type for this head in the filament panel, then resume.")}},
    {523, 40, 40, {kSwitchSummary, kDetachedCause, kReseatHeadFix}},
    {523, 41, 41, {kSwitchSummary, kConflictCause, kReseatHeadFix}},
    {523,
     42,
     42,
     {kSwitchSummary, TR_NOOP("The head did not go back into its dock."),
      TR_NOOP("Check the dock for debris or a bent latch, guide the head into its dock, then home "
              "and resume.")}},
    {523, 43, 43, {kSwitchSummary, kDetachedCause, kReseatHeadFix}},
    {523, 44, 44, {kSwitchSummary, kConflictCause, kReseatHeadFix}},
    {523, 45, 45, {kSwitchSummary, kPogoCause, kPogoFix}},
    {523, 46, 46, {kSwitchSummary, kDetachedCause, kReseatHeadFix}},
    {523, 47, 47, {kSwitchSummary, kConflictCause, kReseatHeadFix}},
    {523,
     48,
     48,
     {TR_NOOP("A toolhead lost contact during the print"),
      TR_NOOP("A toolhead's fan stopped reporting during the print, which points to the pogo "
              "pins."),
      kPogoFix}},
    {523,
     9001,
     9001,
     {TR_NOOP("Flow calibration not allowed for {}"),
      TR_NOOP("Flow calibration is not allowed for this filament with a 0.2 mm nozzle."),
      TR_NOOP("Turn off flow calibration for this print, or use a larger nozzle.")}},

    // 524: the camera.
    {524,
     0,
     0,
     {TR_NOOP("Timelapse did not start"),
      TR_NOOP("The camera did not confirm that timelapse recording started."),
      TR_NOOP("Check that the camera's live view works, or turn off timelapse for this print, "
              "then resume.")}},

    // 525: the feeder. Each code is set at one failure point of a preload
    // (10-15), a load (30-35 reaching the head, 50-51 at the nozzle), an
    // unload (70) or a hand feed (90).
    {525,
     0,
     0,
     {TR_NOOP("{} auto-feed setting not applied"),
      TR_NOOP("The automatic feeding setting could not be applied."),
      TR_NOOP("Try the setting again, and restart the printer if it keeps failing.")}},
    {525,
     10,
     10,
     {TR_NOOP("{} could not preload its filament"),
      TR_NOOP("The feeder could not pull the filament in."), kReloadFix}},
    {525,
     11,
     11,
     {TR_NOOP("{} could not preload its filament"),
      TR_NOOP("The feeder motor did not turn while pulling the filament in."),
      TR_NOOP("Check this head's feeder for jammed filament, then load the head again. Restart "
              "the printer if the motor still does not turn.")}},
    {525,
     12,
     12,
     {TR_NOOP("{} could not preload its filament"),
      TR_NOOP("The feeder turned, but the filament did not move with it."), kReloadFix}},
    {525,
     13,
     13,
     {TR_NOOP("{} could not preload its filament"),
      TR_NOOP("There is no filament in this head's feeder."), kInsertFix}},
    {525,
     14,
     14,
     {TR_NOOP("{} could not preload its filament"),
      TR_NOOP("The filament took too long to reach the feeder's sensor."), kPathFix}},
    {525,
     15,
     15,
     {TR_NOOP("{} could not preload its filament"),
      TR_NOOP("A piece of filament is already in this head's feed path."),
      TR_NOOP("Unload the head, or pull the leftover filament out of its feed tube, then load "
              "the head again.")}},
    {525,
     30,
     30,
     {kLoadSummary, TR_NOOP("The load failed without a more specific reason."),
      TR_NOOP("Load the head again. If it fails again, unload it, cut the filament end square "
              "and load it once more.")}},
    {525,
     31,
     31,
     {kLoadSummary, TR_NOOP("The feeder motor stalled while pushing the filament to the head."),
      kPathFix}},
    {525,
     32,
     32,
     {kLoadSummary, TR_NOOP("The filament stopped moving on its way to the head."), kPathFix}},
    {525,
     33,
     33,
     {kLoadSummary, TR_NOOP("The filament ran out or slipped out of the feeder during the load."),
      kInsertFix}},
    {525,
     34,
     34,
     {kLoadSummary, TR_NOOP("The filament did not reach the head in time."), kPathFix}},
    {525,
     35,
     35,
     {kLoadSummary,
      TR_NOOP("The feeder pushed more filament than the path holds, and it never reached the "
              "head."),
      TR_NOOP("The filament may have snapped or the feed tube come loose. Clear any broken "
              "piece and seat the tube at both ends, then load the head again.")}},
    {525,
     50,
     50,
     {kLoadSummary, TR_NOOP("The filament reached the nozzle, but purging it failed."),
      TR_NOOP("Clear any filament from around the nozzle and the purge area, then load the head "
              "again.")}},
    {525,
     51,
     51,
     {kLoadSummary,
      TR_NOOP("The head's extruder never gripped the filament: after several tries, with the "
              "nozzle cleaned between them, no movement registered at the nozzle."),
      kReloadFix}},
    {525,
     70,
     70,
     {TR_NOOP("{} could not unload its filament"),
      TR_NOOP("The filament could not be pulled out of the head."),
      TR_NOOP("Check that nothing blocks the toolhead, then unload the head again. If the "
              "filament is stuck, heat the nozzle and pull it out by hand.")}},
    {525,
     90,
     90,
     {TR_NOOP("{} hand feed did not finish"), TR_NOOP("Feeding the head by hand did not finish."),
      TR_NOOP("Push the filament into the head until the extruder grabs it, then try again.")}},

    // 526 and 527: the bed and the chamber.
    {526,
     0,
     0,
     {TR_NOOP("Bed temperature reading failed"),
      TR_NOOP("The bed temperature reading is out of range: its sensor is loose or damaged."),
      TR_NOOP("Power off and check the bed cable connection.")}},
    {526,
     1,
     1,
     {TR_NOOP("The bed is not heating"), TR_NOOP("The bed did not heat at the expected rate."),
      TR_NOOP("Restart the firmware and check the bed heater cable.")}},
    {526,
     2,
     2,
     {TR_NOOP("Bed temperature out of range"),
      TR_NOOP("The bed temperature asked for is outside the allowed range."),
      TR_NOOP("Check the bed temperature in the slicer's profile.")}},
    {527,
     0,
     0,
     {TR_NOOP("Chamber temperature reading failed"),
      TR_NOOP("The chamber temperature reading is out of range: its sensor is loose or "
              "damaged."),
      TR_NOOP("Power off and check the chamber sensor cable.")}},

    // 528: homing. The index is an endstop or a head.
    {528,
     0,
     0,
     {TR_NOOP("Z homed before X and Y"),
      TR_NOOP("Z homing was asked for before X and Y were homed."), kHomeFirstFix}},
    {528,
     1,
     1,
     {TR_NOOP("Homing stopped: toolhead state"),
      TR_NOOP("The toolheads' docked and picked-up states do not add up."),
      TR_NOOP("Make sure one toolhead is on the carriage and the rest are docked, then home.")}},
    {528,
     2,
     4,
     {TR_NOOP("Homing failed"),
      TR_NOOP("An endstop or the probe did not trigger as expected while homing."),
      TR_NOOP("Check that the build plate is in place and nothing blocks the axis, clean the "
              "nozzle tip, then home again.")}},
    {528,
     5,
     6,
     {TR_NOOP("Homing failed"), TR_NOOP("Homing gave inconsistent results."),
      TR_NOOP("Check the belts and rails for debris or looseness, then home again.")}},
    {528,
     7,
     8,
     {TR_NOOP("Homing stopped by a shutdown"),
      TR_NOOP("The printer shut down while homing or probing."),
      TR_NOOP("Fix the reason shown for the shutdown, then restart the firmware.")}},
    {528,
     9,
     9,
     {TR_NOOP("Probe triggered too early"),
      TR_NOOP("The probe was already triggered before the move started."),
      TR_NOOP("Clean the nozzle and the plate, check nothing touches the nozzle, then retry.")}},
    {528,
     10,
     10,
     {TR_NOOP("Home X and Y first"), TR_NOOP("X and Y must be homed before precise homing."),
      kHomeFirstFix}},
    {528,
     11,
     12,
     {TR_NOOP("Precise homing failed"),
      TR_NOOP("Precise homing could not find the expected position."),
      TR_NOOP("Clean the nozzle and the calibration target, then home again. If it repeats, run "
              "the home-origin calibration.")}},
    {528, 13, 13, {TR_NOOP("Homing stopped at {}"), kDetachedCause, kReseatHeadFix}},
    {528, 14, 14, {TR_NOOP("Homing stopped at {}"), kConflictCause, kReseatHeadFix}},

    // 529: G-code the firmware could not run.
    {529,
     0,
     5,
     {TR_NOOP("Invalid command value"),
      TR_NOOP("A command is missing a value, or a value is out of its allowed range."),
      kResliceFix}},
    {529,
     6,
     7,
     {TR_NOOP("Duplicate command in the configuration"),
      TR_NOOP("Two configuration sections define the same command."), kConfigFix}},
    {529,
     8,
     9,
     {TR_NOOP("Invalid command"),
      TR_NOOP("A command is malformed or names a device that does not exist."), kResliceFix}},
    {529,
     10,
     12,
     {TR_NOOP("Arc moves not supported"),
      TR_NOOP("The file uses an arc move the printer does not support."),
      TR_NOOP("Turn off arc fitting in the slicer, then slice the file again.")}},
    {529,
     13,
     14,
     {TR_NOOP("Invalid move"), TR_NOOP("A move command has an invalid speed or cannot be read."),
      kResliceFix}},
    {529,
     15,
     15,
     {TR_NOOP("Inch units not supported"), TR_NOOP("The file uses inches (G20)."),
      TR_NOOP("Slice the file in millimeters.")}},

    // 530: probing and calibration.
    {530,
     0,
     1,
     {TR_NOOP("{} nozzle sensor fault"),
      TR_NOOP("The head's nozzle sensor gives no reading, or one outside its normal range."),
      kPogoFix}},
    {530,
     2,
     2,
     {TR_NOOP("Home before probing"), TR_NOOP("Probing was asked for before homing."),
      kHomeFirstFix}},
    {530,
     3,
     3,
     {TR_NOOP("Calibration request not valid"),
      TR_NOOP("The calibration was asked for with invalid settings."),
      TR_NOOP("Start the calibration again from the screen.")}},
    {530,
     4,
     6,
     {TR_NOOP("Probing stopped: toolhead state"),
      TR_NOOP("The toolhead on the carriage is not the one the printer expected."),
      TR_NOOP("Home the printer again so it detects the toolheads, then retry.")}},
    {530,
     8,
     8,
     {TR_NOOP("Probe readings disagree"),
      TR_NOOP("Repeated probe readings differed more than allowed."), kCalibrationRetryFix}},
    {530,
     10,
     10,
     {TR_NOOP("Build plate missing"),
      TR_NOOP("The build plate should be on the bed but reads as removed."),
      TR_NOOP("Put the build plate back on, aligned, then retry.")}},
    {530,
     11,
     11,
     {TR_NOOP("Remove the build plate"),
      TR_NOOP("Remove the PEI sheet from the bed, then start again: probing through the sheet "
              "gives wrong results"),
      TR_NOOP("Take the build plate off when the printer asks, then continue.")}},
    {530,
     13,
     13,
     {TR_NOOP("No toolhead on the carriage"), TR_NOOP("Every toolhead reads as docked."),
      TR_NOOP("Home the printer again so a toolhead is picked up, then retry.")}},
    {530, 14, 14, {TR_NOOP("Probing stopped at {}"), kDetachedCause, kReseatHeadFix}},
    {530, 15, 15, {TR_NOOP("Probing stopped at {}"), kPogoCause, kPogoFix}},
    {530, 16, 16, {TR_NOOP("Probing stopped at {}"), kConflictCause, kReseatHeadFix}},
    {530,
     17,
     17,
     {TR_NOOP("Build plate not seated"),
      TR_NOOP("The probe never touched the plate: the PEI plate is not placed correctly."),
      TR_NOOP("Seat the PEI plate flat and aligned on the bed, then retry.")}},
    {530,
     18,
     18,
     {TR_NOOP("Toolhead offsets out of range"),
      TR_NOOP("The measured offset between toolheads is outside the allowed range."),
      TR_NOOP("Seat all toolheads again and rerun the offset calibration.")}},
    {530,
     20,
     20,
     {TR_NOOP("Dock every toolhead first"), TR_NOOP("All toolheads must be docked for this step."),
      TR_NOOP("Home the printer so every toolhead is docked, then retry.")}},
    {530,
     21,
     22,
     {TR_NOOP("Dock position check failed for {}"),
      TR_NOOP("Picking up or parking the head failed while checking its dock position."),
      TR_NOOP("Adjust the dock position again and rerun the check.")}},
    {530,
     23,
     23,
     {TR_NOOP("Dock position change too large"),
      TR_NOOP("The new dock position is too far from the current one."),
      TR_NOOP("Make a smaller adjustment.")}},
    {530,
     24,
     28,
     {TR_NOOP("Home position calibration failed"),
      TR_NOOP("A home-origin calibration point failed or stayed unstable."),
      TR_NOOP("Clean the nozzle and the calibration target, check the belts, then rerun the "
              "calibration.")}},
    {530,
     29,
     29,
     {TR_NOOP("Bed needs manual leveling"),
      TR_NOOP("The bed is too far out of level for automatic compensation."),
      TR_NOOP("Level the bed with the manual leveling procedure.")}},

    // 531: print files and print settings.
    {531,
     0,
     0,
     {TR_NOOP("File list unavailable"), TR_NOOP("The file list could not be read from storage."),
      TR_NOOP("Restart the printer, and check that storage has free space.")}},
    {531,
     1,
     1,
     {TR_NOOP("A print is already running"), TR_NOOP("Another print is in progress."),
      TR_NOOP("Wait for it to finish, or cancel it first.")}},
    {531,
     2,
     3,
     {TR_NOOP("Command not allowed"), TR_NOOP("That file command cannot run here."),
      TR_NOOP("Upload files from the slicer or app, and remove the command from the file.")}},
    {531,
     4,
     4,
     {TR_NOOP("Print file could not be opened"),
      TR_NOOP("The file is missing, renamed or damaged."),
      TR_NOOP("Upload the file again and start it again.")}},
    {531,
     5,
     6,
     {TR_NOOP("Power-loss recovery not available"),
      TR_NOOP("Recovery is not possible in the printer's current state."),
      TR_NOOP("Return to the idle screen and try again, or start the print fresh.")}},
    {531,
     10,
     10,
     {TR_NOOP("Print file could not be read"),
      TR_NOOP("The file could not be read part way through the print."),
      TR_NOOP("Upload the file again. If it happens again, the storage may be failing.")}},
    {531,
     14,
     14,
     {kPrintSettingSummary,
      TR_NOOP("The file was sliced for a different nozzle size than one the print uses."),
      TR_NOOP("Slice again with the installed nozzle size, or fit the matching nozzle.")}},
    {531,
     15,
     16,
     {TR_NOOP("Not while printing"),
      TR_NOOP("That setting cannot be changed while a print is running"),
      TR_NOOP("Change it before starting the print.")}},
    {531,
     17,
     17,
     {kPrintSettingSummary, TR_NOOP("The print settings sent with the file could not be applied."),
      TR_NOOP("Slice the file again and send the print again.")}},
    {531,
     18,
     18,
     {kPrintSettingSummary,
      TR_NOOP("Flow calibration is on for a filament and nozzle pair that does not allow it."),
      TR_NOOP("Turn off flow calibration for this print, or use a larger nozzle.")}},
    {531,
     19,
     19,
     {kPrintSettingSummary,
      TR_NOOP("The file was sliced for a different nozzle type than the one installed."),
      TR_NOOP("Slice again with the installed nozzle type, or set the head's nozzle type on the "
              "screen.")}},
    {531,
     21,
     21,
     {TR_NOOP("Filament still feeding"), TR_NOOP("Automatic filament feeding is still running."),
      TR_NOOP("Wait for the feeding to finish, then start the print.")}},

    // 532: what the camera and bed checks found.
    {532,
     1,
     1,
     {TR_NOOP("Something is on the build plate"),
      TR_NOOP("The bed check found the plate dirty or occupied."),
      TR_NOOP("Clear and clean the build plate, then resume.")}},
    {532,
     2,
     2,
     {TR_NOOP("The print may have failed"),
      TR_NOOP("The camera saw the print come loose or filament extruding into the air."),
      TR_NOOP("Check the print. Resume only if it looks fine; otherwise cancel and clean the "
              "plate.")}},
    {532,
     3,
     3,
     {TR_NOOP("Leftover material found"),
      TR_NOOP("The camera saw material where it should not be."),
      TR_NOOP("Remove the debris from the bed, then resume.")}},
    {532,
     4,
     4,
     {TR_NOOP("Filament stuck on the nozzle"),
      TR_NOOP("The camera saw filament stuck to the nozzle."),
      TR_NOOP("Clean the nozzle, then resume.")}},

    // 533: the chamber air purifier.
    {533,
     0,
     0,
     {TR_NOOP("Chamber fan not turning"),
      TR_NOOP("The chamber's circulation fan is on but reads no speed."),
      TR_NOOP("Check the fan for obstructions and its cable, then resume.")}},
    {533,
     1,
     1,
     {TR_NOOP("Chamber too hot"), TR_NOOP("The chamber did not cool down in time."),
      TR_NOOP("Open the door or lid to let it cool, check the exhaust fan, then resume.")}},
};

/// A feeder code the table does not know, so the advice covers the whole path.
constexpr FaultWording kFeedUnknown{
    kLoadSummary, TR_NOOP("Filament could not be fed to this head."),
    TR_NOOP("Check the spool, the feeder and the feed tube to this head, then load the head "
            "again.")};

/// The row for this fault, or nullptr.
const FaultWording* find_wording(const ExceptionCode& c) {
    for (const auto& row : kFaultRows) {
        if (row.id == c.id && c.code >= row.code_first && c.code <= row.code_last) {
            return &row.wording;
        }
    }
    return c.id == kFeedingModuleId ? &kFeedUnknown : nullptr;
}

/// Is `s[at..at+3]` four digits?
bool four_digits(const std::string& s, size_t at) {
    if (at + 4 > s.size()) {
        return false;
    }
    for (size_t i = at; i < at + 4; ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) == 0) {
            return false;
        }
    }
    return true;
}

/// Read one of the exceptions array's numeric fields. A field that arrives as
/// any other type reads as 0: the shape of a printer's payload is data, not a
/// contract violation to throw on, so a string field is never parsed.
int field_int(const nlohmann::json& entry, const char* key) {
    auto it = entry.find(key);
    return it != entry.end() && it->is_number_integer() ? it->get<int>() : 0;
}

/// Read the persistence flag. Both spellings are live: a Python bool
/// serialises true/false, an int serialises 0/1.
bool field_persistent(const nlohmann::json& entry, const char* key) {
    auto it = entry.find(key);
    if (it == entry.end()) {
        return false;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    return it->is_number_integer() && it->get<int>() != 0;
}

} // namespace

std::string format_exception_code(const ExceptionCode& c) {
    return fmt::format("{:04d}-{:04d}-{:04d}-{:04d}", c.level, c.id, c.index, c.code);
}

FeedRetry feed_retry(const ExceptionCode& c) {
    if (c.id != kFeedingModuleId) {
        return FeedRetry::None;
    }
    switch (c.code) {
    case 0:  // the auto-feed setting
    case 90: // a hand feed
        return FeedRetry::None;
    case 70:
        return FeedRetry::Unload;
    default:
        // Preload, load and purge codes, and an unknown code, which words as a load failure.
        return FeedRetry::Load;
    }
}

std::optional<ExceptionCode> decode_exception_code(const std::string& text) {
    // Scan for NNNN-NNNN-NNNN-NNNN. Requiring all four groups is what keeps the
    // three-part basic form from decoding as a shifted four-part one.
    for (size_t i = 0; i + 19 <= text.size(); ++i) {
        if (!four_digits(text, i) || text[i + 4] != '-' || !four_digits(text, i + 5) ||
            text[i + 9] != '-' || !four_digits(text, i + 10) || text[i + 14] != '-' ||
            !four_digits(text, i + 15)) {
            continue;
        }
        // A fifth group means this is not the shape we think it is.
        if (i + 19 < text.size() && text[i + 19] == '-') {
            continue;
        }
        // Each field is exactly four validated digits, so each parse below is
        // bounded to 0..9999.
        ExceptionCode c;
        c.level = helix::text_io::parse_leading<int>(text.substr(i, 4)).value_or(0);
        c.id = helix::text_io::parse_leading<int>(text.substr(i + 5, 4)).value_or(0);
        c.index = helix::text_io::parse_leading<int>(text.substr(i + 10, 4)).value_or(0);
        c.code = helix::text_io::parse_leading<int>(text.substr(i + 15, 4)).value_or(0);
        return c;
    }
    return std::nullopt;
}

std::string_view exception_message(const ExceptionCode& c) {
    const FaultWording* w = find_wording(c);
    return w ? std::string_view(w->cause) : std::string_view{};
}

std::optional<FaultWording> fault_wording(const ExceptionCode& c) {
    const FaultWording* w = find_wording(c);
    return w ? std::optional<FaultWording>(*w) : std::nullopt;
}

std::optional<WordedFault> word_fault(const ExceptionCode& c) {
    const FaultWording* w = find_wording(c);
    if (w == nullptr) {
        return std::nullopt;
    }
    WordedFault out;
    // fmt ignores an argument the summary has no "{}" for.
    out.summary = fmt::format(fmt::runtime(lv_tr(w->summary)),
                              ui::lane_label(ui::LaneNoun::Toolhead, c.index));
    out.detail = fmt::format("{}\n\n{}", lv_tr(w->cause), lv_tr(w->fix));
    return out;
}

ExceptionSeverity severity_of(int level) {
    switch (level) {
    case 1:
        return ExceptionSeverity::Informational;
    case 2:
        return ExceptionSeverity::Pause;
    default:
        return ExceptionSeverity::Cancel;
    }
}

bool status_carries_exceptions(const nlohmann::json& status) {
    if (!status.is_object()) {
        return false;
    }
    auto manager = status.find("exception_manager");
    if (manager == status.end() || !manager->is_object()) {
        return false;
    }
    auto exceptions = manager->find("exceptions");
    return exceptions != manager->end() && exceptions->is_array();
}

std::optional<ActiveException> read_exception_entry(const nlohmann::json& entry) {
    if (!entry.is_object()) {
        return std::nullopt;
    }
    ActiveException active;
    active.code.level = field_int(entry, "level");
    active.code.id = field_int(entry, "id");
    active.code.index = field_int(entry, "index");
    active.code.code = field_int(entry, "code");
    auto firmware_text = entry.find("message");
    if (firmware_text != entry.end() && firmware_text->is_string()) {
        active.message = firmware_text->get<std::string>();
    }
    active.persistent = field_persistent(entry, "is_persistent");
    return active;
}

std::string coded_line(const ActiveException& fault) {
    return fmt::format("!! {} {}", snapmaker::format_exception_code(fault.code), fault.message);
}

std::vector<ActiveException> read_active_exceptions(const nlohmann::json& status) {
    std::vector<ActiveException> out;
    if (!status_carries_exceptions(status)) {
        return out;
    }
    // status_carries_exceptions() has proven both keys present.
    const auto& manager = *status.find("exception_manager");
    for (const auto& entry : *manager.find("exceptions")) {
        if (auto active = read_exception_entry(entry)) {
            out.push_back(std::move(*active));
        }
    }
    return out;
}

} // namespace helix::snapmaker
