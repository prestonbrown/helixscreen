// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file probe_preparation.h
 * @brief G-code a printer must run before any probing operation
 *
 * Some firmwares need the probe prepared before it can be trusted. The AD5X
 * under ZMOD probes with a load cell whose zero drifts whenever the mechanical
 * preload changes - which is exactly what turning a bed screw does - and ZMOD
 * only tares on its print path, never on the calibration entry points our UI
 * uses. Probing without the tare yields "Probe triggered prior to movement".
 *
 * The rules live in the printer database as runtime data (a top-level
 * `probe_preparation` array), NOT as a table in this file, so a firmware rename
 * or a new mod is a JSON drop-in rather than a release. This module names no
 * vendor; the vendor lives in the data.
 *
 * @threading Main thread only. Pure with respect to the rule array.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

#include "hv/json.hpp"

namespace helix {
class PrinterDiscovery;
}

namespace helix::probe_prep {

/// Probing operations a rule can attach preparation to.
enum class Operation {
    ScrewsTilt,       ///< SCREWS_TILT_CALCULATE / BED_LEVEL_SCREWS_TUNE
    BedMesh,          ///< BED_MESH_CALIBRATE / G29
    ProbeAccuracy,    ///< PROBE_ACCURACY
    ZOffsetCalibrate, ///< PROBE_CALIBRATE / Z_ENDSTOP_CALIBRATE
};

/// The database key for an operation ("screws_tilt", "bed_mesh", ...).
[[nodiscard]] const char* operation_key(Operation op);

/// What to run before an operation. An empty `gcode` means "send nothing".
struct Preparation {
    std::string gcode;             ///< Joined block; prepend to the operation's script
    std::string label;             ///< Optional progress text while it runs
    uint32_t extra_timeout_ms = 0; ///< ADDED to the operation's existing budget
    std::string rule_id;           ///< Which rule matched, for logging

    [[nodiscard]] bool empty() const {
        return gcode.empty();
    }
};

/**
 * @brief Evaluate a rule array. Pure - this is the unit-testable core.
 *
 * Rules are tried in array order and the FIRST match wins. A rule matches when
 * it is enabled, lists `op` in its `operations`, every `when` predicate holds
 * (AND), and `skip_if_macro_in` does not name `resolved_macro`.
 *
 * Fails closed in every ambiguous case: a malformed rule, an unknown predicate
 * type, or a predicate this build cannot evaluate all mean "does not match".
 * Silently ignoring an unrecognised predicate would let a rule fire on a
 * printer it was never meant to touch, which sends g-code to hardware.
 *
 * @param rules             The `probe_preparation` array (any other type = no match)
 * @param macros_upper      Available macro names, ALREADY uppercased
 * @param op                Operation being prepared for
 * @param resolved_macro    The macro the operation will actually run, for
 *                          `skip_if_macro_in`. May be empty.
 */
[[nodiscard]] Preparation resolve_from_rules(const nlohmann::json& rules,
                                             const std::unordered_set<std::string>& macros_upper,
                                             Operation op, const std::string& resolved_macro = "");

/**
 * @brief The `probe_preparation` array from the loaded printer database.
 *
 * Defined in printer_detector.cpp, which owns the database. Declared here so
 * printer_detector.h does not have to grow a json include for its 40 includers.
 * Returns an empty array when the key is absent, which is the common case.
 */
[[nodiscard]] nlohmann::json database_rules();

/**
 * @brief Resolve against the live printer and the loaded printer database.
 *
 * Thin wrapper over resolve_from_rules(). Returns an empty Preparation when the
 * database has no `probe_preparation` array, which is the case for every
 * printer that needs nothing.
 */
[[nodiscard]] Preparation resolve(const PrinterDiscovery& hw, Operation op,
                                  const std::string& resolved_macro = "");

/**
 * @brief Append this printer's preparation for `op` to a script under construction
 *
 * Call immediately BEFORE appending the probe command itself, and after any
 * homing or pre-positioning: the tare must run with the toolhead clear of the
 * bed, which is the order ZMOD itself uses (move, tare, probe).
 *
 * The preparation deliberately shares one script with the probe, so a failed
 * preparation aborts before probing rather than probing on a bad zero.
 *
 * @return Extra timeout budget in ms to ADD to the operation's own. 0 = nothing
 *         was appended.
 */
uint32_t append_preparation(std::string& script, Operation op,
                            const std::string& resolved_macro = "");

} // namespace helix::probe_prep
