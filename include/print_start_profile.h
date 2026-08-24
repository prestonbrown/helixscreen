// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printer_state.h"

#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file print_start_profile.h
 * @brief JSON-driven print start signal matching profiles
 *
 * Profiles define how to detect PRINT_START phases for specific printer firmware.
 * Each profile contains signal format mappings (exact prefix matching) and
 * regex response patterns, loaded from JSON config files.
 *
 * @see config/print_start_profiles/default.json - Generic patterns for unknown printers
 * @see config/print_start_profiles/forge_x.json - FlashForge AD5M Forge-X mod
 */
class PrintStartProfile {
  public:
    /**
     * @brief Result of a signal or pattern match
     */
    struct MatchResult {
        helix::PrintStartPhase phase;
        std::string message;
        int progress; // 0-100, only meaningful in sequential mode
    };

    /**
     * @brief A single signal format mapping (exact prefix + value lookup)
     */
    struct SignalFormat {
        std::string prefix;
        std::unordered_map<std::string, MatchResult> mappings;
    };

    /**
     * @brief A regex response pattern
     */
    struct ResponsePattern {
        std::regex pattern;
        helix::PrintStartPhase phase;
        std::string message_template; // supports $1, $2 capture group substitution
        int weight;                   // only used in weighted mode
    };

    /**
     * @brief Time-based phase advancement when the firmware emits no gcode
     * responses between heat-complete and first-layer.
     *
     * Some firmwares run cleaning + purge as silent built-in macros — no
     * RESPOND, no command echo. Without a fallback the user sees "Preparing
     * Print..." frozen for 20-30 seconds after temps are ready. Each entry
     * fires after `after_temps_ready_seconds` of elapsed time *since temps
     * first became ready*, advancing the phase to give visible motion.
     * Entries that would regress phase (current_phase >= entry.phase) are
     * skipped, so real gcode signals still win when available.
     */
    struct SilentPhaseEntry {
        int after_temps_ready_seconds = 0;
        helix::PrintStartPhase phase = helix::PrintStartPhase::IDLE;
        std::string message;
    };

    /**
     * @brief Progress calculation mode
     */
    enum class ProgressMode {
        WEIGHTED,  ///< Sum weights of detected phases (default, handles missing phases)
        SEQUENTIAL ///< Each signal maps to specific progress % (for known firmware)
    };

    // =========================================================================
    // Factory Methods
    // =========================================================================

    /**
     * @brief Load a named profile from config/print_start_profiles/{name}.json
     *
     * Falls back to default profile if the named profile can't be loaded.
     *
     * @param profile_name Profile name (without .json extension)
     * @return Loaded profile, or default profile on error
     */
    static std::shared_ptr<PrintStartProfile> load(const std::string& profile_name);

    /**
     * @brief Load the default profile
     *
     * Loads from config/print_start_profiles/default.json.
     * If that file is missing, returns a built-in fallback with the same patterns
     * currently hardcoded in PrintStartCollector.
     *
     * @return Default profile
     */
    static std::shared_ptr<PrintStartProfile> load_default();

    // =========================================================================
    // Matching Methods (called by PrintStartCollector)
    // =========================================================================

    /**
     * @brief Try to match a line against signal format mappings
     *
     * Checks line against each signal format's prefix. If prefix matches,
     * looks up the remainder in the mappings table.
     *
     * @param line G-code response line
     * @param[out] result Match result (phase, message, progress)
     * @return true if matched
     */
    bool try_match_signal(const std::string& line, MatchResult& result) const;

    /**
     * @brief Try to match a line against response patterns (regex)
     *
     * Runs regex search against each response pattern. Supports $1, $2
     * capture group substitution in message templates.
     *
     * @param line G-code response line
     * @param[out] result Match result (phase, message, weight in progress field)
     * @return true if matched
     */
    bool try_match_pattern(const std::string& line, MatchResult& result) const;

    // =========================================================================
    // Progress Calculation
    // =========================================================================

    /**
     * @brief Get the progress mode for this profile
     */
    ProgressMode progress_mode() const {
        return progress_mode_;
    }

    /**
     * @brief Get phase weight for weighted progress calculation
     *
     * Returns the weight assigned to a phase, or 0 if not defined.
     */
    int get_phase_weight(helix::PrintStartPhase phase) const;

    // =========================================================================
    // Accessors
    // =========================================================================

    const std::string& name() const {
        return name_;
    }
    const std::string& description() const {
        return description_;
    }
    bool has_signal_formats() const {
        return !signal_formats_.empty();
    }

    /// True if this is the default/generic fallback profile, not printer-specific
    bool is_default() const {
        return is_default_;
    }

    /// Silent-phase progression entries, in firing order (after_temps_ready_seconds
    /// is monotonically non-decreasing — sorted at parse time).
    const std::vector<SilentPhaseEntry>& silent_progression() const {
        return silent_progression_;
    }

    /// True when this printer uses adaptive bed-mesh probing (slicer overrides
    /// MESH_MIN/MESH_MAX). The configfile probe_count overstates the actual
    /// probe count (Snapmaker U1: 169 config vs ~16 adaptive), so the
    /// collector skips that fallback and shows a live count without a total
    /// until a `probed_matrix` from a prior print is available.
    bool adaptive_meshing() const {
        return adaptive_meshing_;
    }

    /// True when this printer emits the Creality tag stream
    /// ("// num: N, velocity: V, percent F" purge progress, "[box]" CFS
    /// filament-load events) in gcode_response. The collector only runs the
    /// tag matchers for profiles that declare this — the vocabulary is
    /// vendor-specific, and a custom macro on another printer echoing
    /// "percent" plus "num:" must not hijack the phase into PURGING.
    bool cfs_signals() const {
        return cfs_signals_;
    }

    /// True when this printer's pre-print chain goes silent (no
    /// gcode_response markers) for minutes while it homes Z, validates the
    /// mesh at its corners, and sweeps a calibration mesh — but toolhead
    /// position keeps flowing. The collector feeds those position samples to
    /// PrintStartPositionClassifier and refines the status line through the
    /// silence ("Probing Z...", "Checking Bed Mesh...", mesh entry on the
    /// sweep). Off by default: the geometric zones are meaningful for any
    /// printer, but the inference is only wanted where the console actually
    /// goes quiet.
    bool position_signals() const {
        return position_signals_;
    }

  private:
    std::string name_;
    std::string description_;
    bool is_default_{false};
    ProgressMode progress_mode_ = ProgressMode::WEIGHTED;
    bool adaptive_meshing_{false};
    bool cfs_signals_{false};
    bool position_signals_{false};
    std::vector<SignalFormat> signal_formats_;
    std::vector<ResponsePattern> response_patterns_;
    std::unordered_map<helix::PrintStartPhase, int> phase_weights_;
    std::vector<SilentPhaseEntry> silent_progression_;

    /**
     * @brief Parse a JSON object into this profile
     * @return true on success
     */
    bool parse_json(const nlohmann::json& j, const std::string& source_path);

    /**
     * @brief Convert phase name string to helix::PrintStartPhase enum
     */
    static helix::PrintStartPhase parse_phase_name(const std::string& name);

    /**
     * @brief Substitute regex capture groups ($1, $2, ...) in a template
     */
    static std::string substitute_captures(const std::string& tmpl, const std::smatch& match);
};
