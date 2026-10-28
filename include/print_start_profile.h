// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_regex.h"
#include "printer_state.h"

#include <memory>
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
    friend class PrintStartProfileTestAccess;

  public:
    /**
     * @brief Result of a signal or pattern match
     */
    struct MatchResult {
        helix::PrintStartPhase phase;
        std::string message;
        int progress; // 0-100, only meaningful in sequential mode
        /// Seconds the matched text says the printer will spend without a
        /// word (a heat soak's G4); 0 when it announces none.
        int hold_seconds = 0;
        /// Whether the text is the printer narrating its sequence. A label
        /// that only names a target ("Bed: 90c") says nothing about what runs
        /// next, so it must not silence the collector's own inference.
        bool narrates = true;
        /// For a BED_MESH match: whether its probe lines are a mesh to count
        /// against the mesh's point total. A validation that probes a few
        /// points of a stored mesh is not.
        bool counts_probes = true;
        /// The 0-100 progress the text itself reports, from a pattern's
        /// progress_group; -1 when it reports none.
        int reported_percent = -1;
        /// Phases whose signals belong to the step this text begins, for as
        /// long as it runs (see the pattern's `owns`); empty for most text.
        std::vector<helix::PrintStartPhase> owns;
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
        helix::Regex pattern;
        helix::PrintStartPhase phase;
        std::string message_template; // supports $1, $2 capture group substitution
        int weight;                   // only used in weighted mode
        /// Capture group holding a number of minutes the printer stays silent
        /// after this text; 0 when the pattern declares no hold.
        int hold_minutes_group = 0;
        /// False for a label that states a target rather than narrating a step
        bool narrates = true;
        /// False for a BED_MESH step whose probes are not the mesh's points
        bool counts_probes = true;
        /// Capture group holding a progress value the text reports (see
        /// percent_from_report()); 0 when the pattern declares none.
        int progress_group = 0;
        /// Phases the step this text begins emits signals for on its own,
        /// such as a nozzle clean that parks at the purge position
        std::vector<helix::PrintStartPhase> owns;
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
     * @brief One predicate over a status object's field
     *
     * The left-hand side is a dot-path into the rule object's status frame,
     * optionally selecting one array element (toolhead.position is [x,y,z]).
     * The right-hand side is a literal `value`, or — when `ref_field` names a
     * sibling field of the same object — that field's value; `offset` is added
     * to either. `near` additionally requires a positive `tolerance`
     * (|lhs - rhs| < tolerance).
     */
    struct StatusPredicate {
        enum class Op { EQ, NE, GT, LT, NEAR };

        std::string field; ///< Dot-path to the left-hand side
        int index = -1;    ///< Array element selector; -1 = scalar field
        Op op = Op::EQ;
        double value = 0.0;     ///< Literal right-hand side
        std::string ref_field;  ///< Same-object field as right-hand side (replaces value)
        double offset = 0.0;    ///< Added to the right-hand side (literal or field)
        double tolerance = 0.0; ///< NEAR only: |lhs - rhs| < tolerance
    };

    /**
     * @brief A physical phase-inference rule over one status object
     *
     * `when` is an AND-list: the rule holds on a frame only while every
     * predicate resolves and passes. phase/message/weight carry the identical
     * contract a response_pattern entry does — a match feeds the collector's
     * shared apply path, so weights arbitrate against console narration the
     * same way (weight doubles as sequential-mode progress).
     */
    struct StatusSignalRule {
        std::string name;   ///< Rule identity; doubles as the collector's latch key
        std::string object; ///< Klipper status object the rule watches
        std::vector<StatusPredicate> when;
        helix::PrintStartPhase phase = helix::PrintStartPhase::IDLE;
        std::string message;
        int weight = 0;
        /// Hold only once every heater with a target is at it (the collector's
        /// at-target band): evidence that also appears earlier in a start
        /// means this phase only after the heat.
        bool after_heat = false;
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
     * If that file cannot be read or parsed, returns an empty profile named
     * "Generic": no text matching, the engine's own signals still work.
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
     * @brief Whether a start-block command prints @p console_line while it runs
     *
     * From `start_block_owned_lines`: per upper-case command word, the text a
     * console line contains when that command printed it (an action code, a
     * probe report). Such a line refines the step it belongs to rather than
     * naming the step before it.
     */
    bool start_block_step_owns(const std::string& command, const std::string& console_line) const;

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

    /**
     * @brief Try to match a phase-object state string against state patterns
     *
     * Same compiled-pattern machinery as try_match_pattern, run over the
     * state_patterns list against the phase object's field value instead of a
     * console line. Capture substitution and weight-in-progress behave
     * identically.
     *
     * A profile with no state_patterns matches against its response_patterns
     * instead: the two feeds carry the same phase vocabulary, so one list
     * serves both and a profile declaring a phase object needs no second copy
     * of the same regexes.
     *
     * @param state State string read from the phase object's field
     * @param[out] result Match result (phase, message, weight in progress field)
     * @return true if matched
     */
    bool try_match_state(const std::string& state, MatchResult& result) const;

    /**
     * @brief Try to match a line against the tag patterns
     *
     * Tag patterns are the lines a firmware prints to announce a step outright
     * (a purge progress tag, a filament-unit load event). The collector checks
     * them right after HELIX:PHASE and applies a match directly, ahead of
     * completion detection and the weighted response patterns. Same compiled
     * pattern machinery as try_match_pattern; a match's reported_percent is
     * set when the pattern declares a progress_group.
     *
     * @param line G-code response line
     * @param[out] result Match result
     * @return true if matched
     */
    bool try_match_tag(const std::string& line, MatchResult& result) const;

    /// A reported progress value as 0-100: a value at or below 1.5 is a 0-1
    /// fraction, anything larger is already a percent; clamped to 0-100.
    /// -1 for a negative or non-finite value.
    static int percent_from_report(double value);

    // =========================================================================
    // Progress Calculation
    // =========================================================================

    /**
     * @brief Get the progress mode for this profile
     */
    ProgressMode progress_mode() const {
        return progress_mode_;
    }

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

    /// True when the slicer writes this printer's start sequence into the
    /// file instead of calling a PRINT_START macro that runs it, so the line
    /// at virtual_sdcard.file_position names the step running now.
    bool scripted_start() const {
        return scripted_start_;
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

    // =========================================================================
    // Phase-Object Status Source
    // =========================================================================

    /// True when this profile declares a status object carrying structured
    /// phase state. When set, the collector reads the declared field out of
    /// every status frame carrying the object and maps the state string to a
    /// phase through the state_patterns — the same matching pipeline a console
    /// line takes. Absent declaration means exactly today's behavior: the
    /// collector never consults status frames for phases.
    bool has_phase_object() const {
        return !phase_object_name_.empty();
    }

    /// The declared status object name. Empty when has_phase_object() is
    /// false.
    const std::string& phase_object_name() const {
        return phase_object_name_;
    }

    /// The field inside the phase object holding the state string. Empty when
    /// has_phase_object() is false.
    const std::string& phase_object_field() const {
        return phase_object_field_;
    }

    /// The field inside the phase object naming the step the state belongs
    /// to; empty when the profile declares none. Firmware that nests steps
    /// reuses one state string across them (LEVELING while a print decides
    /// how to level, LEVELING while a mesh runs), so with this set the
    /// state_patterns match "<innermost context>/<state>" instead of the bare
    /// state (see qualify_state()).
    const std::string& phase_object_context_field() const {
        return phase_object_context_field_;
    }

    /// The innermost step named by a context field's value: the last element
    /// of an array (a stack, outermost first), a string as-is, and empty for
    /// null, an empty stack or any other shape.
    static std::string innermost_context(const nlohmann::json& value);

    /// The text state_patterns match for a context-qualified phase object.
    static std::string qualify_state(const std::string& context, const std::string& state);

    /// Klipper status objects that must be subscribed for this profile's
    /// phase-object source to ever see a frame: the declared object name, or
    /// an empty list when the profile declares none. The subscription builder
    /// subscribes whatever this returns.
    std::vector<std::string> required_status_objects() const;

    // =========================================================================
    // Status-Signal Rules (physical phase inference)
    // =========================================================================

    /// Rules declared under "status_signals", in file order. Empty for
    /// profiles that declare none — which is exactly the pre-feature behavior.
    const std::vector<StatusSignalRule>& status_signals() const {
        return status_signals_;
    }

    /**
     * @brief Evaluate one status-signal rule against its object's status frame
     *
     * Pure predicate check over the object node the collector hands in:
     * resolves each predicate's fields, applies the op, and on a full
     * AND-match fills `result` with the rule's phase/message/weight — the
     * same shape try_match_pattern produces, so the collector's apply path is
     * unchanged. A frame that does not carry a predicate's field (Klipper
     * pushes deltas) reads as not-holding; the caller's edge latch decides
     * whether that is a miss or a re-arm.
     *
     * @param object_status The rule object's node from a status frame
     * @param rule The rule to evaluate
     * @param[out] result Match result (phase, message, weight in progress)
     * @return true when every predicate holds
     */
    bool evaluate_status_signal(const nlohmann::json& object_status, const StatusSignalRule& rule,
                                MatchResult& result) const;

  private:
    std::string name_;
    std::string description_;
    bool is_default_{false};
    ProgressMode progress_mode_ = ProgressMode::WEIGHTED;
    bool adaptive_meshing_{false};
    bool scripted_start_{false};
    std::unordered_map<std::string, std::vector<std::string>> start_block_owned_lines_;
    bool position_signals_{false};
    std::vector<SignalFormat> signal_formats_;
    std::vector<ResponsePattern> response_patterns_;
    std::vector<ResponsePattern> state_patterns_;
    std::vector<ResponsePattern> tag_patterns_;
    std::vector<SilentPhaseEntry> silent_progression_;
    std::string phase_object_name_;
    std::string phase_object_field_;
    std::string phase_object_context_field_;
    std::vector<StatusSignalRule> status_signals_;

    /**
     * @brief Parse a JSON object into this profile
     * @return true on success
     */
    bool parse_json(const nlohmann::json& j, const std::string& source_path);

    /**
     * @brief Parse a response-pattern-shaped array (pattern/phase/message/
     * weight) into `out`
     *
     * Shared by response_patterns and state_patterns — the entry shape is
     * identical, only the text each list is matched against differs.
     */
    void parse_pattern_array(const nlohmann::json& array, std::vector<ResponsePattern>& out,
                             const char* kind, const std::string& source_path);

    /**
     * @brief Parse the "status_signals" array into status_signals_
     *
     * Tolerant of malformed entries like every other block: each bad entry
     * warns and is skipped whole (a rule with one bad predicate must not
     * survive as a narrower AND-list), and parsing continues to the next.
     */
    void parse_status_signals(const nlohmann::json& array, const std::string& source_path);

    /**
     * @brief Run the compiled-pattern matching loop over one pattern list
     */
    bool match_pattern_list(const std::vector<ResponsePattern>& patterns, const std::string& text,
                            MatchResult& result) const;

    /**
     * @brief Convert phase name string to helix::PrintStartPhase enum
     */
    static helix::PrintStartPhase parse_phase_name(const std::string& name);

    /**
     * @brief Substitute regex capture groups ($1, $2, ...) in a template
     */
    static std::string substitute_captures(const std::string& tmpl, const helix::RegexMatch& match);
};
