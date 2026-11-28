// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/text_io.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// Bed Screw Thread Geometry
// ============================================================================

/// Z travel per full turn (mm) for every thread Klipper's `screw_thread` accepts.
/// Mirrors the `threads_factor` table in Klipper's screws_tilt_adjust.py.
constexpr float kScrewPitchM3Mm = 0.5f;
constexpr float kScrewPitchM4Mm = 0.7f;
constexpr float kScrewPitchM5Mm = 0.8f;
constexpr float kScrewPitchM6Mm = 1.0f;

/// Pitch assumed when `screw_thread` is unreadable — Klipper's own default (CW-M3).
constexpr float kScrewPitchDefaultMm = kScrewPitchM3Mm;

/// Corner-to-corner bed spread at or below which the bed counts as level.
/// Expressed in mm because a clock-minute is a different physical distance on
/// every thread pitch (5 min is 0.042 mm on M3 but 0.083 mm on M6).
constexpr float kScrewLevelToleranceMm = 0.05f;

/// Adjustment magnitude above which a screw is drawn as severe rather than warned.
constexpr float kScrewSevereAdjustmentMm = 0.25f;

/**
 * @brief Z travel per full turn for a Klipper `screw_thread` value
 *
 * Accepts the full config token ("CW-M4", "CCW-M6") or a bare thread ("M5").
 * Falls back to kScrewPitchDefaultMm when the value is missing or unparseable.
 */
[[nodiscard]] inline float screw_thread_pitch_mm(const std::string& screw_thread) {
    size_t m = screw_thread.find_last_of("mM");
    if (m != std::string::npos && m + 1 < screw_thread.length()) {
        switch (screw_thread[m + 1]) {
        case '3':
            return kScrewPitchM3Mm;
        case '4':
            return kScrewPitchM4Mm;
        case '5':
            return kScrewPitchM5Mm;
        case '6':
            return kScrewPitchM6Mm;
        default:
            break;
        }
    }
    return kScrewPitchDefaultMm;
}

/**
 * @brief Convert a bed-height distance to clock-minutes of screw rotation
 *
 * One "minute" is 1/60 of a full turn, matching Klipper's TT:MM output.
 * Sign is preserved so a caller can take CW/CCW from the diff's sign; a
 * distance that rounds to zero still returns one minute, keeping its sign, so
 * a tolerance can't collapse to an exact-match test.
 */
[[nodiscard]] inline int screw_minutes_for_mm(float distance_mm, float pitch_mm) {
    if (!(pitch_mm > 0.0f)) {
        pitch_mm = kScrewPitchDefaultMm;
    }
    int minutes = static_cast<int>(std::lround(distance_mm / pitch_mm * 60.0f));
    return minutes != 0 ? minutes : (distance_mm < 0.0f ? -1 : 1);
}

/// Level tolerance in clock-minutes for a given thread pitch (the single source
/// of truth — every threshold in the screws-tilt UI derives from this).
[[nodiscard]] inline int screw_level_tolerance_minutes(float pitch_mm = kScrewPitchDefaultMm) {
    return screw_minutes_for_mm(kScrewLevelToleranceMm, pitch_mm);
}

/// Magnitude in clock-minutes above which an adjustment is drawn as severe.
[[nodiscard]] inline int screw_severe_adjustment_minutes(float pitch_mm = kScrewPitchDefaultMm) {
    return screw_minutes_for_mm(kScrewSevereAdjustmentMm, pitch_mm);
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
 * @brief Render a turn magnitude the way a person would say it
 *
 * Shared by ScrewTiltResult::friendly_adjustment() and the re-centring hint so
 * the two can never describe the same physical turn differently.
 *
 * @param total_minutes Unsigned magnitude in clock-minutes
 */
[[nodiscard]] inline std::string describe_screw_turn_amount(int total_minutes) {
    if (total_minutes <= 10) {
        return "1/8 turn";
    }
    if (total_minutes <= 20) {
        return "1/4 turn";
    }
    if (total_minutes <= 35) {
        return "1/2 turn";
    }
    if (total_minutes <= 50) {
        return "3/4 turn";
    }
    if (total_minutes <= 70) {
        return "1 turn";
    }
    const int approx_turns = (total_minutes + 30) / 60;
    return std::to_string(approx_turns) + " turn" + (approx_turns > 1 ? "s" : "");
}

/**
 * @brief Render a turn as a full instruction ("Tighten 1/4 turn")
 *
 * @param tighten From the corner's height, never from Klipper's CW/CCW: see
 *                ScrewTiltResult::needs_tightening().
 */
[[nodiscard]] inline std::string describe_screw_turn(int total_minutes, bool tighten) {
    return (tighten ? "Tighten " : "Loosen ") + describe_screw_turn_amount(std::abs(total_minutes));
}

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
        std::string_view rest = adjustment;
        const auto direction = helix::text_io::scan_word(rest, 7);
        const auto turns = helix::text_io::scan_number<int>(rest);
        if (!direction || !turns || !helix::text_io::consume_literal(rest, ":")) {
            return std::nullopt;
        }
        const auto minutes = helix::text_io::scan_number<int>(rest);
        if (!minutes) {
            return std::nullopt;
        }
        int magnitude = *turns * 60 + *minutes;
        if (*direction == "CW") {
            return magnitude;
        }
        if (*direction == "CCW") {
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
     * @brief Whether this screw is tightened (rather than loosened) to level the bed
     *
     * Tightening a bed screw compresses its spring and pulls that corner down, so a
     * corner probing higher than the reference tightens and a lower one loosens.
     * Klipper's CW/CCW cannot answer this: it says which way to turn the knob
     * (CW decreases the nozzle gap), and whether that way tightens depends on
     * where the knob sits.
     */
    [[nodiscard]] bool needs_tightening(float reference_z) const {
        return z_height > reference_z;
    }

    /**
     * @brief Get user-friendly adjustment description ("Tighten 1/4 turn")
     *
     * @param in_spec Whether this screw is inside the level window, as decided
     *                by evaluate_screw_level() — a per-screw magnitude test is
     *                not enough to answer that (see ScrewLevelReport::in_spec).
     * @param reference_z The reference screw's probed height; without it the
     *                    verb is unknowable and Klipper's raw string is shown.
     * @return Human-friendly adjustment string
     */
    [[nodiscard]] std::string friendly_adjustment(bool in_spec,
                                                  std::optional<float> reference_z) const {
        if (is_reference) {
            return "Reference";
        }
        if (in_spec) {
            return "Level";
        }
        if (!reference_z || !signed_adjustment_minutes()) {
            return adjustment;
        }
        return describe_screw_turn(adjustment_minutes(), needs_tightening(*reference_z));
    }
};

/// The probed height every other screw is levelled against, or nullopt when no
/// result is marked as Klipper's base.
[[nodiscard]] inline std::optional<float>
reference_screw_z(const std::vector<ScrewTiltResult>& results) {
    for (const auto& r : results) {
        if (r.is_reference) {
            return r.z_height;
        }
    }
    return std::nullopt;
}

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
                     float pitch_mm = kScrewPitchDefaultMm) {
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
 * @brief Whether a screw is one the user must NOT be told to turn
 *
 * True for the reference screw, which carries no adjustment by construction,
 * and for any screw already inside the level window.
 *
 * `in_spec` alone does not answer this. evaluate_screw_level() measures every
 * screw against the MIDPOINT of the spread, not against zero, so the reference
 * lands out of spec whenever the tilt runs mostly one way - on a real AD5X bed
 * (0 / 84 / 229 / 174 minutes) the base scores 229 and ties the worst screw.
 * Anything drawing a screw as turnable must ask this, not `in_spec`.
 */
[[nodiscard]] inline bool screw_is_settled(const ScrewTiltResult& screw, bool in_spec) {
    return screw.is_reference || in_spec;
}

/**
 * @brief An alternative to Klipper's advice that turns fewer screws
 *
 * Klipper reports every screw relative to `screw1` and treats that as fixed.
 * The adjustment vector is only defined up to a uniform translation though -
 * adding a constant to every screw moves the bed vertically without changing
 * its tilt - so "reference = 0" is arbitrary, not cheapest.
 */
struct ScrewRecenterHint {
    bool available = false; ///< False means Klipper's advice is already the cheap one
    size_t screw_index = 0; ///< Index into results: the ONE screw to move instead
    int signed_minutes = 0; ///< Its adjustment, CW positive, CCW negative
    bool tighten = false;   ///< Moving it toward the others means tightening it
    size_t replaces = 0;    ///< How many screws Klipper's advice would have moved
};

/// One full turn in clock-minutes. Pitch-independent: the notation is turns:minutes.
inline constexpr int kScrewMinutesPerTurn = 60;

/**
 * @brief Suggest moving the reference screw instead of all the others
 *
 * Fires ONLY when the non-reference screws are already level with EACH OTHER
 * and share a large common offset from the reference. That condition is the
 * whole point: if they disagree among themselves, re-centring just changes
 * *which* screws move without reducing how many. Measured against a real AD5X
 * bed (0/84/229/174 minutes), re-centring on the median still moves three
 * screws, so this correctly declines to fire there.
 *
 * The caller shows this as an alternative alongside Klipper's numbers, never
 * instead of them - staying cross-checkable against Fluidd and the console.
 * Both forms describe the same plane, so it doubles as the escape hatch when a
 * screw runs out of travel in one direction.
 */
[[nodiscard]] inline ScrewRecenterHint
suggest_screw_recentering(const std::vector<ScrewTiltResult>& results,
                          const ScrewLevelReport& report) {
    ScrewRecenterHint hint;
    if (report.verdict != ScrewLevelVerdict::NEEDS_ADJUSTMENT) {
        return hint;
    }

    std::vector<int> others;
    size_t ref_index = results.size();
    for (size_t i = 0; i < results.size(); i++) {
        const std::optional<int> minutes = results[i].signed_adjustment_minutes();
        if (!minutes) {
            return hint; // unparseable: evaluate_screw_level() already failed loudly
        }
        if (results[i].is_reference) {
            ref_index = i;
        } else {
            others.push_back(*minutes);
        }
    }
    // Needs a reference and at least two other screws for "move one instead of
    // several" to be worth saying at all.
    if (ref_index == results.size() || others.size() < 2) {
        return hint;
    }

    const int highest = *std::max_element(others.begin(), others.end());
    const int lowest = *std::min_element(others.begin(), others.end());
    if (highest - lowest > report.tolerance_minutes) {
        return hint; // they disagree among themselves - re-centring saves nothing
    }

    std::vector<int> sorted = others;
    std::sort(sorted.begin(), sorted.end());
    const size_t mid = sorted.size() / 2;
    const int common = (sorted.size() % 2 == 1) ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;

    // Only worth interrupting the user for a turn they would actually notice.
    if (std::abs(common) <= kScrewMinutesPerTurn) {
        return hint;
    }

    hint.available = true;
    hint.screw_index = ref_index;
    hint.signed_minutes = -common; // move the datum the other way instead
    // The others agree within tolerance, so any one of them stands for the plane
    // the datum has to meet.
    const size_t other_index = ref_index == 0 ? 1 : 0;
    hint.tighten = !results[other_index].needs_tightening(results[ref_index].z_height);
    hint.replaces = others.size();
    return hint;
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
