// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string_view>

namespace helix {

/**
 * @brief Print start initialization phase (detected from G-code response output)
 *
 * Represents the current phase during PRINT_START macro execution.
 * Used to show progress to the user during the initialization sequence
 * before actual printing begins.
 *
 * @note Phases are detected via best-effort pattern matching on G-code responses.
 *       Not all macros output all phases - progress estimation handles missing phases.
 *
 * @note The numeric order is load-bearing. PrintStartCollector's silent
 *       progression refuses to move to a phase whose value does not exceed the
 *       current one, so a phase belongs at the sequence position where it
 *       occurs, not appended at the end. COMPLETE stays terminal:
 *       print_start_phase_count() derives the range from it.
 *
 * @note The NAME, not the number, is what leaves the process. Prediction history
 *       keys each entry's "phases" object by name, and PRINT_START macros and
 *       printer profiles name phases in gcode and JSON. Inserting a phase
 *       therefore costs nothing outside this file.
 */
enum class PrintStartPhase {
    IDLE = 0,           ///< Not in PRINT_START (normal operation)
    INITIALIZING = 1,   ///< PRINT_START detected, waiting for phases
    HOMING = 2,         ///< G28 / Home All Axes detected
    HEATING_BED = 3,    ///< M140/M190 / Heating bed detected
    SOAKING = 4,        ///< Holding at temperature: heat-soak dwell or chamber wait
    HEATING_NOZZLE = 5, ///< M104/M109 / Heating nozzle detected
    QGL = 6,            ///< QUAD_GANTRY_LEVEL detected
    Z_TILT = 7,         ///< Z_TILT_ADJUST detected
    BED_MESH = 8,       ///< BED_MESH_CALIBRATE or BED_MESH_PROFILE LOAD detected
    CLEANING = 9,       ///< CLEAN_NOZZLE / nozzle wipe detected
    PURGING = 10,       ///< VORON_PURGE / LINE_PURGE detected
    COMPLETE = 11       ///< Transitioning to PRINTING state
};

/// Number of declared phases; every int in [0, count) is a PrintStartPhase.
inline constexpr int print_start_phase_count() {
    return static_cast<int>(PrintStartPhase::COMPLETE) + 1;
}

/**
 * @brief Canonical name of a phase — the spelling that gets persisted and parsed
 *
 * The switch is exhaustive with no default, so declaring a phase without a name
 * fails the build rather than shipping a phase that silently drops out of the
 * prediction history.
 */
inline constexpr std::string_view print_start_phase_name(PrintStartPhase phase) {
    switch (phase) {
    case PrintStartPhase::IDLE:
        return "IDLE";
    case PrintStartPhase::INITIALIZING:
        return "INITIALIZING";
    case PrintStartPhase::HOMING:
        return "HOMING";
    case PrintStartPhase::HEATING_BED:
        return "HEATING_BED";
    case PrintStartPhase::SOAKING:
        return "SOAKING";
    case PrintStartPhase::HEATING_NOZZLE:
        return "HEATING_NOZZLE";
    case PrintStartPhase::QGL:
        return "QGL";
    case PrintStartPhase::Z_TILT:
        return "Z_TILT";
    case PrintStartPhase::BED_MESH:
        return "BED_MESH";
    case PrintStartPhase::CLEANING:
        return "CLEANING";
    case PrintStartPhase::PURGING:
        return "PURGING";
    case PrintStartPhase::COMPLETE:
        return "COMPLETE";
    }
    return {};
}

/// One accepted spelling of a phase.
struct PrintStartPhaseAlias {
    std::string_view name;
    PrintStartPhase phase;
};

/**
 * @brief Alternative spellings accepted alongside the canonical names
 *
 * PRINT_START macros in the field emit these through "HELIX:PHASE:<name>", and
 * printer profiles may name a phase either way. Removing an entry breaks phase
 * detection on machines whose macros this build never sees.
 */
inline constexpr PrintStartPhaseAlias kPrintStartPhaseAliases[] = {
    {"START", PrintStartPhase::INITIALIZING},
    {"STARTING", PrintStartPhase::INITIALIZING},
    {"BED_HEATING", PrintStartPhase::HEATING_BED},
    {"HEAT_SOAK", PrintStartPhase::SOAKING},
    {"SOAK", PrintStartPhase::SOAKING},
    {"NOZZLE_HEATING", PrintStartPhase::HEATING_NOZZLE},
    {"HEATING_HOTEND", PrintStartPhase::HEATING_NOZZLE},
    {"QUAD_GANTRY_LEVEL", PrintStartPhase::QGL},
    {"Z_TILT_ADJUST", PrintStartPhase::Z_TILT},
    {"BED_LEVELING", PrintStartPhase::BED_MESH},
    {"NOZZLE_CLEAN", PrintStartPhase::CLEANING},
    {"PURGE", PrintStartPhase::PURGING},
    {"PRIMING", PrintStartPhase::PURGING},
    {"DONE", PrintStartPhase::COMPLETE},
};

/**
 * @brief Resolve a canonical name or documented alias to its phase
 *
 * Matching is exact, including case: callers that accept free-form input
 * upper-case it first. An unrecognised name yields no value rather than IDLE,
 * so a caller cannot mistake a failed parse for a request to reset the phase.
 */
inline std::optional<PrintStartPhase> print_start_phase_from_name(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (int i = 0; i < print_start_phase_count(); ++i) {
        const auto phase = static_cast<PrintStartPhase>(i);
        if (print_start_phase_name(phase) == name) {
            return phase;
        }
    }
    for (const auto& alias : kPrintStartPhaseAliases) {
        if (alias.name == name) {
            return alias.phase;
        }
    }
    return std::nullopt;
}

/**
 * @brief Whether the prediction history keeps a measured duration for a phase
 *
 * Heating is predicted from measured heat rates by ThermalRateModel rather than
 * from history, and IDLE/INITIALIZING/COMPLETE are transitions with no dwell of
 * their own. Both listings are exhaustive so a new phase has to be classified.
 */
inline constexpr bool print_start_phase_stores_duration(PrintStartPhase phase) {
    switch (phase) {
    case PrintStartPhase::HOMING:
    case PrintStartPhase::SOAKING:
    case PrintStartPhase::QGL:
    case PrintStartPhase::Z_TILT:
    case PrintStartPhase::BED_MESH:
    case PrintStartPhase::CLEANING:
    case PrintStartPhase::PURGING:
        return true;
    case PrintStartPhase::IDLE:
    case PrintStartPhase::INITIALIZING:
    case PrintStartPhase::HEATING_BED:
    case PrintStartPhase::HEATING_NOZZLE:
    case PrintStartPhase::COMPLETE:
        return false;
    }
    return false;
}

} // namespace helix
