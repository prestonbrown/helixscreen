// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// ============================================================================
// Bed Screw Thread Geometry
// ============================================================================

/// Z travel per full turn (mm) for every thread Klipper's `screw_thread` accepts.
/// Mirrors the `threads_factor` table in Klipper's screws_tilt_adjust.py.
constexpr float SCREW_PITCH_M3_MM = 0.5f;
constexpr float SCREW_PITCH_M4_MM = 0.7f;
constexpr float SCREW_PITCH_M5_MM = 0.8f;
constexpr float SCREW_PITCH_M6_MM = 1.0f;

/// Pitch assumed when `screw_thread` is unreadable — Klipper's own default (CW-M3).
constexpr float SCREW_PITCH_DEFAULT_MM = SCREW_PITCH_M3_MM;

/// Corner-to-corner bed spread at or below which the bed counts as level.
/// Expressed in mm because a clock-minute is a different physical distance on
/// every thread pitch (5 min is 0.042 mm on M3 but 0.083 mm on M6).
constexpr float SCREW_LEVEL_TOLERANCE_MM = 0.05f;

/// Adjustment magnitude above which a screw is drawn as severe rather than warned.
constexpr float SCREW_SEVERE_ADJUSTMENT_MM = 0.25f;

/**
 * @brief Z travel per full turn for a Klipper `screw_thread` value
 *
 * Accepts the full config token ("CW-M4", "CCW-M6") or a bare thread ("M5").
 * Falls back to SCREW_PITCH_DEFAULT_MM when the value is missing or unparseable.
 */
[[nodiscard]] inline float screw_thread_pitch_mm(const std::string& screw_thread) {
    size_t m = screw_thread.find_last_of("mM");
    if (m != std::string::npos && m + 1 < screw_thread.length()) {
        switch (screw_thread[m + 1]) {
        case '3':
            return SCREW_PITCH_M3_MM;
        case '4':
            return SCREW_PITCH_M4_MM;
        case '5':
            return SCREW_PITCH_M5_MM;
        case '6':
            return SCREW_PITCH_M6_MM;
        default:
            break;
        }
    }
    return SCREW_PITCH_DEFAULT_MM;
}

/**
 * @brief Convert a bed-height distance to clock-minutes of screw rotation
 *
 * One "minute" is 1/60 of a full turn, matching Klipper's TT:MM output.
 * Never returns less than 1 so a tolerance can't collapse to an exact-match test.
 */
[[nodiscard]] inline int screw_minutes_for_mm(float distance_mm, float pitch_mm) {
    if (!(pitch_mm > 0.0f)) {
        pitch_mm = SCREW_PITCH_DEFAULT_MM;
    }
    int minutes = static_cast<int>(std::lround(distance_mm / pitch_mm * 60.0f));
    return minutes > 0 ? minutes : 1;
}

/// Level tolerance in clock-minutes for a given thread pitch (the single source
/// of truth — every threshold in the screws-tilt UI derives from this).
[[nodiscard]] inline int screw_level_tolerance_minutes(float pitch_mm = SCREW_PITCH_DEFAULT_MM) {
    return screw_minutes_for_mm(SCREW_LEVEL_TOLERANCE_MM, pitch_mm);
}

/// Magnitude in clock-minutes above which an adjustment is drawn as severe.
[[nodiscard]] inline int screw_severe_adjustment_minutes(float pitch_mm = SCREW_PITCH_DEFAULT_MM) {
    return screw_minutes_for_mm(SCREW_SEVERE_ADJUSTMENT_MM, pitch_mm);
}

/**
 * @brief Flip the leading CW↔CCW direction token in an adjustment string
 *
 * Rewrites `"CW 01:15"` → `"CCW 01:15"` and `"CCW 00:30"` → `"CW 00:30"`
 * in place. No-op if the string doesn't begin with a direction token (e.g.
 * already stripped, empty, or malformed). Used by the screws-tilt parser
 * to apply the printer-database `screws_tilt_direction` override.
 */
inline void flip_screws_tilt_direction(std::string& adjustment) {
    if (adjustment.rfind("CCW", 0) == 0) {
        adjustment.replace(0, 3, "CW");
    } else if (adjustment.rfind("CW", 0) == 0) {
        adjustment.replace(0, 2, "CCW");
    }
}

/**
 * @file calibration_types.h
 * @brief Data structures for printer calibration features
 *
 * Types for bed leveling, input shaping, and machine limits.
 * Used by the screws tilt panel, input shaper panel, and calibration wizards.
 */

// ============================================================================
// Bed Leveling Types
// ============================================================================

/**
 * @brief Result from SCREWS_TILT_CALCULATE command
 *
 * Represents a single bed adjustment screw with its measured height
 * deviation and the required adjustment.
 */
struct ScrewTiltResult {
    std::string screw_name; ///< Screw identifier (e.g., "front_left", "rear_right")
    float x_pos = 0.0f;     ///< Bed X coordinate of screw position (mm)
    float y_pos = 0.0f;     ///< Bed Y coordinate of screw position (mm)
    float z_height = 0.0f;  ///< Probed Z height at screw position
    std::string
        adjustment; ///< Adjustment string (e.g., "CW 0:15" for clockwise 0 turns 15 minutes)
    bool is_reference = false; ///< True if this is the reference screw (no adjustment needed)

    /**
     * @brief Signed arc-minutes of this screw's adjustment, CW positive
     *
     * Klipper picks the first screw in config order as the base and reports
     * every other screw as `diff = z_base - z`, emitting CW for a positive
     * diff and CCW for a negative one (inverted for a CCW-M* thread). So the
     * signed minutes are a monotonic stand-in for the screw's bed height, and
     * the difference between two of them is the real error between those two
     * corners — which the magnitude alone is not.
     *
     * @return signed minutes, 0 for the base screw, or nullopt when the
     *         adjustment string can't be parsed (a malformed line must surface
     *         as an error, never as a screw that happens to read as level).
     */
    [[nodiscard]] std::optional<int> signed_adjustment_minutes() const {
        if (is_reference) {
            return 0;
        }
        char direction[8] = {};
        int turns = 0;
        int minutes = 0;
        if (std::sscanf(adjustment.c_str(), "%7s %d:%d", direction, &turns, &minutes) != 3) {
            return std::nullopt;
        }
        int magnitude = turns * 60 + minutes;
        if (std::strcmp(direction, "CW") == 0) {
            return magnitude;
        }
        if (std::strcmp(direction, "CCW") == 0) {
            return -magnitude;
        }
        return std::nullopt;
    }

    /**
     * @brief Unsigned arc-minute magnitude of this screw's adjustment
     *
     * Parses "CW 01:30" / "CCW 00:15" into total minutes (turns*60 + minutes).
     * Returns 0 for reference screws or unparseable strings — only use this for
     * describing *how far* to turn. Level verdicts must go through
     * evaluate_screw_level(), which is sign-aware and fails loudly.
     */
    [[nodiscard]] int adjustment_minutes() const {
        return std::abs(signed_adjustment_minutes().value_or(0));
    }

    /**
     * @brief Get prettified screw name for display
     *
     * Converts snake_case to Title Case (e.g., "front_left" -> "Front Left")
     * @return Human-readable screw name
     */
    [[nodiscard]] std::string display_name() const {
        std::string result;
        bool capitalize_next = true;
        for (char c : screw_name) {
            if (c == '_') {
                result += ' ';
                capitalize_next = true;
            } else if (capitalize_next) {
                result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                capitalize_next = false;
            } else {
                result += c;
            }
        }
        return result;
    }

    /**
     * @brief Get user-friendly adjustment description
     *
     * Converts "CW 00:18" to "Tighten 1/4 turn", "CCW 01:30" to "Loosen 1 turn", etc.
     * Mapping is CW→Tighten, CCW→Loosen. This assumes the adjustment string
     * already reflects what the user should physically do — printer-specific
     * corrections (e.g. Flashforge AD5M's inverted SCREWS_TILT output) are
     * applied at parse time in screws_tilt_parser.cpp, not here.
     *
     * @param in_spec Whether this screw is inside the level window, as decided
     *                by evaluate_screw_level() — a per-screw magnitude test is
     *                not enough to answer that (see ScrewLevelReport::in_spec).
     * @return Human-friendly adjustment string
     */
    [[nodiscard]] std::string friendly_adjustment(bool in_spec) const {
        if (is_reference) {
            return "Reference";
        }
        if (in_spec) {
            return "Level";
        }

        bool is_clockwise = adjustment.find("CW") == 0 && adjustment.find("CCW") != 0;
        bool is_counter = adjustment.find("CCW") == 0;
        int total_minutes = adjustment_minutes();

        std::string amount;
        if (total_minutes <= 10) {
            amount = "1/8 turn";
        } else if (total_minutes <= 20) {
            amount = "1/4 turn";
        } else if (total_minutes <= 35) {
            amount = "1/2 turn";
        } else if (total_minutes <= 50) {
            amount = "3/4 turn";
        } else if (total_minutes <= 70) {
            amount = "1 turn";
        } else {
            int approx_turns = (total_minutes + 30) / 60;
            amount = std::to_string(approx_turns) + " turn" + (approx_turns > 1 ? "s" : "");
        }

        if (is_clockwise) {
            return "Tighten " + amount;
        } else if (is_counter) {
            return "Loosen " + amount;
        }
        return adjustment;
    }
};

/**
 * @brief Outcome of judging a full set of screw results
 */
enum class ScrewLevelVerdict {
    LEVEL,            ///< Bed spread is within tolerance
    NEEDS_ADJUSTMENT, ///< At least one corner is outside the level window
    PARSE_ERROR       ///< A result could not be read — verdict is unknowable
};

/**
 * @brief Verdict plus per-screw detail for one set of screw results
 */
struct ScrewLevelReport {
    ScrewLevelVerdict verdict = ScrewLevelVerdict::PARSE_ERROR;
    int tolerance_minutes = 0; ///< Level window in clock-minutes for this thread pitch
    int spread_minutes = 0;    ///< Highest screw minus lowest, in clock-minutes
    std::vector<bool> in_spec; ///< Parallel to the input: screw is inside the window
    size_t worst_index = 0;    ///< Non-reference screw furthest from the level plane
    std::string parse_error;   ///< Offending screw/adjustment when verdict is PARSE_ERROR

    [[nodiscard]] bool is_level() const {
        return verdict == ScrewLevelVerdict::LEVEL;
    }
};

/**
 * @brief Judge whether a bed is level from a set of screw results
 *
 * Klipper reports every screw relative to the *base* screw, which it picks as
 * the first one in config order — not the highest or lowest. When the base sits
 * mid-range, two corners can each be within tolerance in opposite directions
 * while the real corner-to-corner error is their sum, so a per-screw magnitude
 * test calls a visibly tilted bed level (prestonbrown/helixscreen#1225).
 *
 * The verdict is therefore taken on the signed spread across all screws
 * (the base included, at 0): `max - min <= tolerance`.
 *
 * @param results   Screw results as parsed from SCREWS_TILT_CALCULATE
 * @param pitch_mm  Thread pitch from `configfile.settings.screws_tilt_adjust.screw_thread`
 */
[[nodiscard]] inline ScrewLevelReport
evaluate_screw_level(const std::vector<ScrewTiltResult>& results,
                     float pitch_mm = SCREW_PITCH_DEFAULT_MM) {
    ScrewLevelReport report;
    report.tolerance_minutes = screw_level_tolerance_minutes(pitch_mm);
    report.in_spec.assign(results.size(), false);

    if (results.empty()) {
        report.parse_error = "no screw results";
        return report;
    }

    std::vector<int> signed_minutes;
    signed_minutes.reserve(results.size());
    for (const auto& screw : results) {
        std::optional<int> minutes = screw.signed_adjustment_minutes();
        if (!minutes) {
            report.verdict = ScrewLevelVerdict::PARSE_ERROR;
            report.parse_error = screw.screw_name + ": '" + screw.adjustment + "'";
            return report;
        }
        signed_minutes.push_back(*minutes);
    }

    const int highest = *std::max_element(signed_minutes.begin(), signed_minutes.end());
    const int lowest = *std::min_element(signed_minutes.begin(), signed_minutes.end());
    report.spread_minutes = highest - lowest;

    report.verdict = report.spread_minutes <= report.tolerance_minutes
                         ? ScrewLevelVerdict::LEVEL
                         : ScrewLevelVerdict::NEEDS_ADJUSTMENT;

    // A screw is in spec when it sits within half the tolerance window of the
    // spread's midpoint. Doubled so the midpoint stays in integer arithmetic:
    // |2s - (highest + lowest)| <= tolerance. The two extremes both attain
    // (highest - lowest), so every screw is in spec exactly when the bed is
    // level — the green checkmarks can never disagree with the banner.
    int worst_magnitude = -1;
    for (size_t i = 0; i < results.size(); i++) {
        report.in_spec[i] =
            std::abs(2 * signed_minutes[i] - (highest + lowest)) <= report.tolerance_minutes;

        // The screw to highlight is the one with the biggest turn to make, which
        // is the magnitude Klipper printed. The base carries no adjustment, so
        // it is never a candidate.
        int magnitude = std::abs(signed_minutes[i]);
        if (!results[i].is_reference && magnitude > worst_magnitude) {
            worst_magnitude = magnitude;
            report.worst_index = i;
        }
    }

    return report;
}

/**
 * @brief Bed leveling method selection
 */
enum class BedLevelingMethod {
    AUTO_MESH,     ///< BED_MESH_CALIBRATE - Automatic probing grid
    MANUAL_SCREWS, ///< SCREWS_TILT_CALCULATE - Manual screw adjustment guidance
    QUAD_GANTRY,   ///< QUAD_GANTRY_LEVEL - Voron-style gantry leveling
    Z_TILT         ///< Z_TILT_ADJUST - Multi-motor Z adjustment
};

// ============================================================================
// Input Shaping Types
// ============================================================================

// Forward declaration for all_shapers vector
struct ShaperOption;

/**
 * @brief Which stage of SHAPER_CALIBRATE a progress report belongs to
 *
 * Klipper's resonance test has two visibly different halves: a frequency sweep
 * that moves the toolhead, then an offline fit of each shaper against the
 * captured data. The UI shows different text for each, so the phase travels
 * alongside the percentage instead of being re-derived from it — a percentage
 * threshold cannot distinguish "sweep pinned at its ceiling" from "sweep done".
 */
enum class ShaperCalibrationPhase {
    Sweeping,  ///< Toolhead is being driven through the test frequencies
    Analyzing, ///< Sweep finished; Klipper is fitting shapers to the data
    Complete,  ///< Recommendation and CSV path parsed
};

/**
 * @brief Per-shaper frequency response curve from calibration CSV
 *
 * Contains the filtered PSD response for one shaper type at all frequency bins.
 * Used for overlaying shaper response on the raw frequency spectrum chart.
 */
struct ShaperResponseCurve {
    std::string name;          ///< Shaper type (e.g., "zv", "mzv", "ei")
    float frequency = 0.0f;    ///< Fitted frequency in Hz (from CSV header)
    std::vector<float> values; ///< Filtered PSD values at each frequency bin
};

/**
 * @brief Result from resonance testing (TEST_RESONANCES or Klippain)
 *
 * Contains the recommended shaper configuration for one axis, plus
 * all fitted shaper alternatives for comparison.
 */
struct InputShaperResult {
    char axis = 'X';          ///< Axis tested ('X' or 'Y')
    std::string shaper_type;  ///< Recommended shaper (e.g., "mzv", "ei", "2hump_ei", "3hump_ei")
    float shaper_freq = 0.0f; ///< Recommended frequency in Hz
    float max_accel = 0.0f;   ///< Maximum recommended acceleration in mm/s²
    float smoothing = 0.0f;   ///< Smoothing value (0.0-1.0, lower is better)
    float vibrations = 0.0f;  ///< Remaining vibrations percentage

    /// Path to CSV calibration data file (e.g., /tmp/calibration_data_x_*.csv)
    std::string csv_path;

    /// True when Klipper reported a CSV path but its frequency-response data
    /// could not be read (missing/unreadable file — e.g. systemd PrivateTmp
    /// isolation hiding Klipper's /tmp output — or a malformed CSV). The
    /// recommendation is still valid; only the chart is unavailable.
    bool chart_data_unavailable = false;

    /// True when the firmware overwrote the staged X result with the Y-axis
    /// values at the end of this (Y-axis) run, discarding the measured X
    /// recommendation. Carried on the Y result because that is the run whose
    /// console output announces the copy; the X results card reads it to warn
    /// that its measured value never reached the saved config.
    bool x_overwritten_by_firmware = false;

    /// Frequency response data for graphing (frequency Hz, amplitude)
    std::vector<std::pair<float, float>> freq_response;

    /// Per-shaper filtered response curves (for chart overlay)
    std::vector<ShaperResponseCurve> shaper_curves;

    /// All fitted shaper options from calibration (not just recommended)
    std::vector<ShaperOption> all_shapers;

    /// Check if frequency response data is available for charting
    [[nodiscard]] bool has_freq_data() const {
        return !freq_response.empty();
    }

    /**
     * @brief Check if result contains valid data
     */
    [[nodiscard]] bool is_valid() const {
        return !shaper_type.empty() && shaper_freq > 0.0f;
    }
};

/**
 * @brief Single shaper option with all metrics
 *
 * Represents one fitted shaper from resonance testing, with complete
 * metrics for comparison. Used in the all_shapers vector of InputShaperResult.
 */
struct ShaperOption {
    std::string type;        ///< Shaper type (e.g., "zv", "mzv", "ei", "2hump_ei", "3hump_ei")
    float frequency = 0.0f;  ///< Fitted frequency in Hz
    float vibrations = 0.0f; ///< Remaining vibrations percentage (lower is better)
    float smoothing = 0.0f;  ///< Smoothing value (lower is sharper corners)
    float max_accel = 0.0f;  ///< Maximum recommended acceleration in mm/s²
};

/**
 * @brief Current input shaper configuration from printer state
 *
 * Represents the currently active input shaper settings as configured
 * in Klipper. Retrieved via printer.objects.query for input_shaper.
 */
struct InputShaperConfig {
    std::string shaper_type_x;    ///< Active shaper type for X axis (empty if not configured)
    float shaper_freq_x = 0.0f;   ///< Active frequency for X axis in Hz
    std::string shaper_type_y;    ///< Active shaper type for Y axis (empty if not configured)
    float shaper_freq_y = 0.0f;   ///< Active frequency for Y axis in Hz
    float damping_ratio_x = 0.0f; ///< Damping ratio for X axis (default 0.1)
    float damping_ratio_y = 0.0f; ///< Damping ratio for Y axis (default 0.1)
    bool is_configured = false;   ///< True if input shaper is actively configured
};

// ============================================================================
// Machine Limits Types
// ============================================================================

/**
 * @brief Printer motion limits (velocity, acceleration)
 *
 * Represents current or target machine limits. Can be applied temporarily
 * via SET_VELOCITY_LIMIT or permanently via SAVE_CONFIG.
 */
struct MachineLimits {
    double max_velocity = 0;           ///< Maximum velocity in mm/s
    double max_accel = 0;              ///< Maximum acceleration in mm/s²
    double max_accel_to_decel = 0;     ///< Maximum acceleration to deceleration in mm/s²
    double square_corner_velocity = 0; ///< Square corner velocity in mm/s
    double max_z_velocity = 0;         ///< Maximum Z velocity in mm/s
    double max_z_accel = 0;            ///< Maximum Z acceleration in mm/s²

    /**
     * @brief Check if limits contain valid data
     */
    [[nodiscard]] bool is_valid() const {
        return max_velocity > 0 && max_accel > 0;
    }

    /**
     * @brief Compare two limit sets for equality
     */
    [[nodiscard]] bool operator==(const MachineLimits& other) const {
        return max_velocity == other.max_velocity && max_accel == other.max_accel &&
               max_accel_to_decel == other.max_accel_to_decel &&
               square_corner_velocity == other.square_corner_velocity &&
               max_z_velocity == other.max_z_velocity && max_z_accel == other.max_z_accel;
    }

    [[nodiscard]] bool operator!=(const MachineLimits& other) const {
        return !(*this == other);
    }
};

// ============================================================================
// Calibration Callback Types
// ============================================================================

namespace helix {
/// Bed screw results callback
using ScrewTiltCallback = std::function<void(const std::vector<ScrewTiltResult>&)>;

/// Input shaper result callback
using InputShaperCallback = std::function<void(const InputShaperResult&)>;

/// Machine limits callback
using MachineLimitsCallback = std::function<void(const MachineLimits&)>;
} // namespace helix
