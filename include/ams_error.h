// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "display_numbering.h"

#include <string>

/**
 * @file ams_error.h
 * @brief Error types and helpers for AMS/MMU operations
 *
 * Provides structured error handling for multi-filament system operations,
 * including user-friendly messages suitable for UI display and technical
 * details for debugging.
 */

namespace helix {

/**
 * @brief AMS operation result codes
 *
 * Covers errors from both Happy Hare and AFC systems, as well as
 * general communication and state errors.
 */
enum class AmsResult {
    SUCCESS = 0, ///< Operation succeeded

    // Communication errors
    NOT_CONNECTED,   ///< No connection to Moonraker/printer
    TIMEOUT,         ///< Operation timed out
    CONNECTION_LOST, ///< Connection lost during operation
    COMMAND_FAILED,  ///< G-code command returned error

    // System state errors
    NOT_INITIALIZED, ///< AMS backend not initialized
    NO_AMS_DETECTED, ///< No AMS/MMU system found
    WRONG_STATE,     ///< Operation invalid in current state
    BUSY,            ///< Another operation in progress

    // Hardware/mechanical errors
    FILAMENT_JAM,  ///< Filament jammed in path
    SLOT_BLOCKED,  ///< Slot/lane blocked or inaccessible
    SENSOR_ERROR,  ///< Filament sensor malfunction
    ENCODER_ERROR, ///< Filament encoder malfunction
    HOMING_FAILED, ///< Selector homing failed
    EXTRUDER_COLD, ///< Extruder too cold for operation

    // Operation-specific errors
    LOAD_FAILED,        ///< Failed to load filament to extruder
    UNLOAD_FAILED,      ///< Failed to unload filament from extruder
    TOOL_CHANGE_FAILED, ///< Tool change operation failed
    TIP_FORMING_FAILED, ///< Filament tip forming failed
    SLOT_NOT_AVAILABLE, ///< Requested slot has no filament

    // Configuration errors
    INVALID_SLOT,  ///< Slot index out of range
    INVALID_TOOL,  ///< Tool index out of range
    MAPPING_ERROR, ///< Tool-to-slot mapping invalid

    // Spoolman errors
    SPOOLMAN_NOT_AVAILABLE, ///< Spoolman service not reachable
    SPOOL_NOT_FOUND,        ///< Requested spool ID not found

    // Feature not available
    NOT_SUPPORTED, ///< Feature not supported by this backend

    // Resume preparation control flow
    RESUME_REQUIRES_RESTART, ///< RESUME cannot proceed — virtual_sdcard inactive,
                             ///< caller must offer "restart from beginning" UX

    // Generic
    UNKNOWN_ERROR ///< Unexpected error condition
};

/**
 * @brief Get string representation of AMS result
 * @param result The result code
 * @return Human-readable string for the result
 */
inline const char* ams_result_to_string(AmsResult result) {
    switch (result) {
    case AmsResult::SUCCESS:
        return "Success";
    case AmsResult::NOT_CONNECTED:
        return "Not Connected";
    case AmsResult::TIMEOUT:
        return "Timeout";
    case AmsResult::CONNECTION_LOST:
        return "Connection Lost";
    case AmsResult::COMMAND_FAILED:
        return "Command Failed";
    case AmsResult::NOT_INITIALIZED:
        return "Not Initialized";
    case AmsResult::NO_AMS_DETECTED:
        return "No AMS Detected";
    case AmsResult::WRONG_STATE:
        return "Wrong State";
    case AmsResult::BUSY:
        return "Busy";
    case AmsResult::FILAMENT_JAM:
        return "Filament Jam";
    case AmsResult::SLOT_BLOCKED:
        return "Slot Blocked";
    case AmsResult::SENSOR_ERROR:
        return "Sensor Error";
    case AmsResult::ENCODER_ERROR:
        return "Encoder Error";
    case AmsResult::HOMING_FAILED:
        return "Homing Failed";
    case AmsResult::EXTRUDER_COLD:
        return "Extruder Cold";
    case AmsResult::LOAD_FAILED:
        return "Load Failed";
    case AmsResult::UNLOAD_FAILED:
        return "Unload Failed";
    case AmsResult::TOOL_CHANGE_FAILED:
        return "Tool Change Failed";
    case AmsResult::TIP_FORMING_FAILED:
        return "Tip Forming Failed";
    case AmsResult::SLOT_NOT_AVAILABLE:
        return "Slot Not Available";
    case AmsResult::INVALID_SLOT:
        return "Invalid Slot";
    case AmsResult::INVALID_TOOL:
        return "Invalid Tool";
    case AmsResult::MAPPING_ERROR:
        return "Mapping Error";
    case AmsResult::SPOOLMAN_NOT_AVAILABLE:
        return "Spoolman Not Available";
    case AmsResult::SPOOL_NOT_FOUND:
        return "Spool Not Found";
    case AmsResult::NOT_SUPPORTED:
        return "Not Supported";
    case AmsResult::RESUME_REQUIRES_RESTART:
        return "Resume Requires Restart";
    default:
        return "Unknown Error";
    }
}

/**
 * @brief Check if a result indicates a recoverable error
 *
 * Recoverable errors can potentially be resolved by user intervention
 * (clearing a jam, heating extruder, etc.)
 *
 * @param result The result code to check
 * @return true if the error may be recoverable
 */
[[nodiscard]] inline bool ams_result_is_recoverable(AmsResult result) {
    switch (result) {
    case AmsResult::FILAMENT_JAM:
    case AmsResult::SLOT_BLOCKED:
    case AmsResult::EXTRUDER_COLD:
    case AmsResult::LOAD_FAILED:
    case AmsResult::UNLOAD_FAILED:
    case AmsResult::TIP_FORMING_FAILED:
    case AmsResult::HOMING_FAILED:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Detailed error information for AMS operations
 *
 * Combines a result code with human-readable messages suitable for
 * both logging and UI display.
 */
struct AmsError {
    AmsResult result;          ///< Primary error code
    std::string technical_msg; ///< Technical details for logging/debugging
    std::string user_msg;      ///< User-friendly message for UI display
    std::string suggestion;    ///< Suggested recovery action (optional)
    int slot_index = -1;       ///< Slot involved in error (-1 if N/A)

    /**
     * @brief Construct an AmsError
     * @param r Result code (default SUCCESS)
     * @param tech Technical message for logging
     * @param user User-friendly message for UI
     * @param suggest Suggested action for recovery
     * @param slot Slot index if applicable
     */
    AmsError(AmsResult r = AmsResult::SUCCESS, const std::string& tech = "",
             const std::string& user = "", const std::string& suggest = "", int slot = -1)
        : result(r), technical_msg(tech), user_msg(user), suggestion(suggest), slot_index(slot) {}

    /**
     * @brief Check if operation succeeded
     * @return true if result is SUCCESS
     */
    [[nodiscard]] bool success() const {
        return result == AmsResult::SUCCESS;
    }

    /**
     * @brief Boolean conversion for convenient if-checks
     * @return true if operation succeeded
     */
    operator bool() const {
        return success();
    }

    /**
     * @brief Check if error is potentially recoverable
     * @return true if user intervention might resolve the error
     */
    [[nodiscard]] bool is_recoverable() const {
        return ams_result_is_recoverable(result);
    }

    /**
     * @brief Both user-facing halves flattened into one string.
     *
     * For boundaries that can only carry a single string — notification
     * history rows, and ToolState::request_tool_change()'s
     * `std::function<void(const std::string&)>` on_error. UI surfaces that can
     * lay out two lines should use helix::ui::notify_ams_error() instead, which
     * renders @c suggestion as a distinct, smaller line.
     *
     * Falls back to the result name when the backend populated neither field,
     * so a caller can never end up showing an empty toast.
     */
    [[nodiscard]] std::string display_text() const {
        if (user_msg.empty() && suggestion.empty()) {
            return ams_result_to_string(result);
        }
        if (suggestion.empty()) {
            return user_msg;
        }
        if (user_msg.empty()) {
            return suggestion;
        }
        return user_msg + " - " + suggestion;
    }
};

/**
 * @brief Utility class for creating user-friendly AMS error messages
 *
 * Provides factory methods for common error scenarios with consistent
 * messaging that can be displayed directly in the UI.
 */
class AmsErrorHelper {
  public:
    /**
     * @brief The position a user sees, in the backend's own word.
     *
     * 1-based, because this is a toast and not a log line. An index that is not
     * a position at all still names its raw value, so a sentinel reaching here
     * says what was asked for instead of leaving a gap in the sentence.
     */
    static std::string position_label(ui::LaneNoun noun, int slot) {
        const std::string label = ui::lane_label(noun, slot);
        return label.empty() ? ui::noun_text(noun) + " " + std::to_string(slot) : label;
    }

    /**
     * @brief Create a success result
     * @return AmsError with SUCCESS result
     */
    static AmsError success() {
        return AmsError(AmsResult::SUCCESS);
    }

    /**
     * @brief Create a not connected error
     * @param detail Technical detail about the connection failure
     * @return AmsError configured for UI display
     */
    static AmsError not_connected(const std::string& detail = "") {
        return AmsError(AmsResult::NOT_CONNECTED,
                        detail.empty() ? "No Moonraker connection" : detail,
                        "Printer not connected",
                        "Check that the printer is powered on and connected to the network");
    }

    /**
     * @brief Create a no AMS detected error
     * @return AmsError configured for UI display
     */
    static AmsError no_ams_detected() {
        return AmsError(AmsResult::NO_AMS_DETECTED, "No mmu or afc object found in printer state",
                        "No multi-filament system detected",
                        "Ensure Happy Hare or AFC is installed and configured");
    }

    /**
     * @brief Create a timeout error
     * @param operation Name of the operation that timed out
     * @return AmsError configured for UI display
     */
    static AmsError timeout(const std::string& operation) {
        return AmsError(AmsResult::TIMEOUT, operation + " operation timed out",
                        "Operation timed out",
                        "Try the operation again. If it persists, check for mechanical issues.");
    }

    /**
     * @brief Create a busy error (operation in progress)
     * @param current_op Description of the current operation
     * @return AmsError configured for UI display
     */
    static AmsError busy(const std::string& current_op = "another operation") {
        return AmsError(AmsResult::BUSY, "Cannot start operation: " + current_op + " in progress",
                        "AMS is busy", "Wait for the current operation to complete");
    }

    /**
     * @brief Create a "print in progress" refusal for toolhead-motion ops.
     *
     * PRINTING is refused for every backend. PAUSED is refused only when the
     * backend's firmware macro homes itself (AmsBackend::filament_ops_self_home()
     * — AD5X IFS), because that buried G28 probes a loadcell-Z nozzle into the
     * part. See AmsSubscriptionBackend::refuse_if_printing().
     *
     * @param is_paused        The job is PAUSED rather than PRINTING.
     * @param pause_allows_ops This backend WOULD permit the op on a paused print,
     *                         so pausing is a recovery worth naming. Meaningless
     *                         when @p is_paused (pausing is already the state).
     * @return AmsError configured for UI display
     */
    static AmsError print_active(bool is_paused = false, bool pause_allows_ops = false) {
        // A paused print is the case that actually reaches a user: Klipper's
        // runout handler pauses and prints "load it and press RESUME", so the
        // obvious next move is the Load button. Saying "while printing" there
        // reads as a bug, and "finish or cancel the print" is the opposite of
        // what they want. Give the recovery that does work (bundle JX2FVRB9).
        //
        // Reaching this branch now means the backend self-homes (AD5X IFS), so
        // the two recoveries named have to work on THAT machine. Both do: feeding
        // filament past the toolhead sensor by hand re-triggers head_switch_sensor
        // so RESUME continues the job, and cancelling returns the printer to
        // STANDBY where Load/Unload are permitted again.
        if (is_paused) {
            return AmsError(AmsResult::WRONG_STATE,
                            "Filament operation blocked: print paused mid-job and this printer's "
                            "filament macros home the toolhead themselves",
                            "Can't move filament while the print is paused",
                            "Feed filament past the sensor by hand, then press Resume — or cancel "
                            "the print to use Load/Unload");
        }
        // PRINTING. On a backend that permits filament ops while paused, pausing
        // is the cheap recovery — naming "finish or cancel" there would push the
        // user to throw away a print they could have saved.
        return AmsError(AmsResult::WRONG_STATE, "Filament operation blocked: print in progress",
                        "Cannot run filament operation while printing",
                        pause_allows_ops
                            ? "Pause the print first, then load, unload, or change filament"
                            : "Finish or cancel the print before loading, unloading, or changing "
                              "filament");
    }

    /**
     * @brief Create a "not loaded" error (no filament/tool active)
     * @return AmsError configured for UI display
     */
    static AmsError not_loaded() {
        return AmsError(AmsResult::WRONG_STATE, "No filament or tool is currently loaded",
                        "Nothing loaded", "Load a filament first before trying to unload");
    }

    /**
     * @brief Create a filament jam error
     * @param slot Slot index where jam occurred
     * @param location Description of jam location (e.g., "bowden tube", "extruder")
     * @return AmsError configured for UI display
     */
    static AmsError filament_jam(int slot, const std::string& location = "") {
        std::string loc_detail = location.empty() ? "" : " at " + location;
        return AmsError(AmsResult::FILAMENT_JAM, "Filament jam detected" + loc_detail,
                        "Filament jam detected", "Manually clear the jam and retry the operation",
                        slot);
    }

    /**
     * @brief Create a slot blocked error
     * @param noun The backend's word for one position
     * @param slot Slot index that is blocked
     * @return AmsError configured for UI display
     */
    static AmsError slot_blocked(ui::LaneNoun noun, int slot) {
        return AmsError(AmsResult::SLOT_BLOCKED,
                        "Slot " + std::to_string(slot) + " is blocked or inaccessible",
                        position_label(noun, slot) + " blocked",
                        "Check for obstructions or misaligned filament", slot);
    }

    /**
     * @brief Create an extruder cold error
     * @param current_temp Current extruder temperature
     * @param required_temp Required temperature for operation
     * @return AmsError configured for UI display
     */
    static AmsError extruder_cold(int current_temp, int required_temp) {
        return AmsError(AmsResult::EXTRUDER_COLD,
                        "Extruder at " + std::to_string(current_temp) + "°C, need " +
                            std::to_string(required_temp) + "°C",
                        "Extruder too cold",
                        "Heat the extruder to at least " + std::to_string(required_temp) +
                            "°C before loading filament");
    }

    /**
     * @brief Create a load failed error
     * @param noun The backend's word for one position
     * @param slot Slot that failed to load
     * @param detail Technical detail about the failure
     * @return AmsError configured for UI display
     */
    static AmsError load_failed(ui::LaneNoun noun, int slot, const std::string& detail = "") {
        return AmsError(AmsResult::LOAD_FAILED, detail.empty() ? "Load operation failed" : detail,
                        "Failed to load filament from " + position_label(noun, slot),
                        "Check filament path and try again", slot);
    }

    /**
     * @brief Create an unload failed error
     * @param detail Technical detail about the failure
     * @return AmsError configured for UI display
     */
    static AmsError unload_failed(const std::string& detail = "") {
        return AmsError(
            AmsResult::UNLOAD_FAILED, detail.empty() ? "Unload operation failed" : detail,
            "Failed to unload filament",
            "Check extruder temperature and try again. Manual removal may be required.");
    }

    /**
     * @brief Create a slot not available error
     * @param noun The backend's word for one position
     * @param slot Slot index that has no filament
     * @return AmsError configured for UI display
     */
    static AmsError slot_not_available(ui::LaneNoun noun, int slot) {
        return AmsError(AmsResult::SLOT_NOT_AVAILABLE,
                        "Slot " + std::to_string(slot) + " has no filament loaded",
                        position_label(noun, slot) + " is empty",
                        "Load filament before selecting it", slot);
    }

    /**
     * @brief Create an invalid slot error
     * @param noun The backend's word for one position
     * @param slot Invalid slot index
     * @param max_slot Maximum valid slot index, 0-based
     * @return AmsError configured for UI display
     */
    static AmsError invalid_slot(ui::LaneNoun noun, int slot, int max_slot) {
        // The bare word, not a composed label: this names the kind of thing the
        // index was supposed to be, not a position that exists. tool_out_of_range()
        // below reads the same way.
        const std::string word = ui::noun_text(noun);
        // 1-based, matching the labels these positions carry everywhere else. A
        // backend that has reported no positions has no span to offer.
        const int highest = ui::lane_number(max_slot);
        const std::string suggestion =
            highest > 0 ? "Select a valid " + word + " (1-" + std::to_string(highest) + ")"
                        : "Select a valid " + word;
        return AmsError(AmsResult::INVALID_SLOT,
                        "Slot " + std::to_string(slot) + " out of range (0-" +
                            std::to_string(max_slot) + ")",
                        "Invalid " + word + " number", suggestion, slot);
    }

    /**
     * @brief Create a tool number out of range error
     * @param tool_number Invalid gcode tool index
     * @return AmsError configured for UI display
     */
    static AmsError tool_out_of_range(int tool_number) {
        // tool_label() returns empty for a negative index; fall back to the raw
        // number there so the logged detail still names what was passed in.
        const std::string label = helix::ui::tool_label(tool_number);
        return AmsError(AmsResult::INVALID_TOOL,
                        "Tool " + (label.empty() ? std::to_string(tool_number) : label) +
                            " out of range",
                        "Invalid tool number", "Select a valid tool");
    }

    /**
     * @brief Create a wrong state error
     * @param current_state Description of current state
     * @param required_state Description of required state
     * @return AmsError configured for UI display
     */
    static AmsError wrong_state(const std::string& current_state,
                                const std::string& required_state) {
        return AmsError(AmsResult::WRONG_STATE,
                        "Cannot perform operation in state: " + current_state +
                            ", need: " + required_state,
                        "Cannot perform this action now",
                        "Wait for the current operation to complete or cancel it first");
    }

    /**
     * @brief Create a G-code command failed error
     * @param command The G-code command that failed
     * @param response Error response from Klipper
     * @return AmsError configured for UI display
     */
    static AmsError command_failed(const std::string& command, const std::string& response) {
        return AmsError(AmsResult::COMMAND_FAILED, "Command '" + command + "' failed: " + response,
                        "Command failed", "Check Klipper console for details");
    }

    /**
     * @brief Create a not supported error
     * @param feature Description of the unsupported feature
     * @return AmsError configured for UI display
     */
    static AmsError not_supported(const std::string& feature) {
        return AmsError(AmsResult::NOT_SUPPORTED, feature + " is not supported by this backend",
                        "Feature not available",
                        "This feature requires different hardware or configuration");
    }

    /**
     * @brief Create an invalid parameter error
     * @param detail Description of the invalid parameter
     * @return AmsError configured for UI display
     */
    static AmsError invalid_parameter(const std::string& detail) {
        return AmsError(AmsResult::WRONG_STATE, detail, "Invalid parameter",
                        "Check the provided value and try again");
    }

    /**
     * @brief Create a result that tells the resume dispatcher to surface a
     * restart-from-beginning modal instead of firing RESUME.
     *
     * Used when virtual_sdcard.is_active=false coexists with
     * print_stats.state="paused" — a state Snapmaker U1 enters after a
     * level-2 abort exception (dirty bed #532, sensor failures, etc.).
     * pause_resume.resume() runs to completion but has no SD context, so
     * the print is effectively terminated; the only recovery is
     * SDCARD_RESET_FILE + CANCEL_PRINT_BASE + a fresh print start.
     *
     * @param detail Optional technical detail (logged, not user-shown)
     * @param reason Optional authored explanation of why restart is required
     *               (e.g. "The bed was reported dirty, so this print cannot
     *               resume."), shown to the user in place of the raw firmware
     *               pause text. Empty means the backend has not authored one
     *               for this cause; the caller falls back to firmware text.
     */
    static AmsError resume_requires_restart(const std::string& detail = "",
                                            const std::string& reason = "") {
        return AmsError(AmsResult::RESUME_REQUIRES_RESTART,
                        detail.empty() ? "virtual_sdcard.is_active=false; RESUME would no-op"
                                       : detail,
                        reason, "Restart from the beginning to recover");
    }
};

} // namespace helix
