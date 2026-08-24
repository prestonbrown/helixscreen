// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief Result of parsing a bed mesh probe line
 *
 * current: 1-based probe index (from "Probing point X/Y" or fallback count)
 * total:   total expected probes (0 if unknown, e.g. fallback "probe at" lines
 *          without a known grid size)
 */
struct ProbeProgress {
    int current;
    int total; ///< 0 = unknown
};

/**
 * @brief Parse a G-code response line for bed mesh probe progress
 *
 * Handles two formats:
 *  1. "Probing point 5/25", "Probe point 5 of 25", "Probing mesh point 5/25"
 *  2. "probe at X,Y is z=Z" (fallback — caller must maintain a running count)
 *
 * For format (1), returns {current, total}.
 * For format (2), returns std::nullopt — use is_probe_result_line() to detect
 * these and maintain your own counter.
 *
 * @param line G-code response line
 * @return Parsed {current, total} or std::nullopt
 */
inline std::optional<ProbeProgress> parse_probe_progress(const std::string& line) {
    // Static regex — handles "Probing point 5/25", "Probe point 5 of 25",
    // "Probing mesh point 5/25"
    static const std::regex probe_regex(
        R"(Prob(?:ing (?:mesh )?point|e point) (\d+)[/\s]+(?:of\s+)?(\d+))");

    std::smatch match;
    if (std::regex_search(line, match, probe_regex) && match.size() == 3) {
        try {
            return ProbeProgress{std::stoi(match[1].str()), std::stoi(match[2].str())};
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/**
 * @brief Check if a line is a "probe at X,Y is z=Z" result line
 *
 * These lines appear on firmware that doesn't emit "Probing point X/Y" progress
 * markers. Callers should maintain their own running count when this returns true.
 */
inline bool is_probe_result_line(const std::string& line) {
    return line.find("probe at ") != std::string::npos && line.find(" is z=") != std::string::npos;
}

/**
 * @brief (x,y) position parsed from a "probe at X,Y is z=Z" line
 */
struct ProbePosition {
    double x;
    double y;
};

/**
 * @brief Extract (x,y) from a "probe at X,Y is z=Z" line
 *
 * Klipper's `samples` config causes the same (x,y) to appear multiple times
 * consecutively. Callers can use this to deduplicate samples and count unique
 * probe points rather than raw sample lines.
 *
 * Accepts both comma and whitespace separators after "probe at", and optional
 * "x:"/"y:" prefixes (seen on some firmwares).
 *
 * @return Parsed position or std::nullopt if line doesn't match
 */
/**
 * @brief Stock Klipper's adaptive bed_mesh emits this line when the slicer
 * passes MESH_MIN/MESH_MAX overrides (or ADAPTIVE=1) and bed_mesh.py reduces
 * the grid from configfile defaults.
 *
 * Format: "Adapted probe count: N,M" (preceded by "// " in gcode_response).
 * The total = N * M, fired once before the first probe — usable as a live
 * denominator on stock Klipper / Voron / KAMP setups.
 *
 * NOT emitted by Snapmaker U1's custom firmware fork (the count exists in
 * klippy.log as "Updated Mesh Configuration" but never reaches
 * gcode_response). U1 uses adaptive_meshing=true in its profile to skip the
 * configfile fallback and rely on probed_matrix from the prior print.
 *
 * @return Total probe count (N * M) or std::nullopt
 */
inline std::optional<int> parse_adapted_probe_count(const std::string& line) {
    static const std::regex adapt_regex(R"(Adapted probe count:\s*(\d+)\s*,\s*(\d+))");
    std::smatch match;
    if (std::regex_search(line, match, adapt_regex) && match.size() == 3) {
        try {
            int x = std::stoi(match[1].str());
            int y = std::stoi(match[2].str());
            if (x > 0 && y > 0) {
                return x * y;
            }
        } catch (...) {
            // fall through
        }
    }
    return std::nullopt;
}

inline std::optional<ProbePosition> parse_probe_position(const std::string& line) {
    // Matches "probe at x: 181.474, y: 55.048 is z=..." and "probe at 150.0,150.0 is z=..."
    static const std::regex pos_regex(
        R"(probe at (?:x:\s*)?(-?\d+(?:\.\d+)?)[,\s]+(?:y:\s*)?(-?\d+(?:\.\d+)?)\s+is z=)");
    std::smatch match;
    if (std::regex_search(line, match, pos_regex) && match.size() == 3) {
        try {
            return ProbePosition{std::stod(match[1].str()), std::stod(match[2].str())};
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/// Two probe samples are the same mesh point if they land this close (mm).
///
/// Repeated samples report byte-identical coordinates, so 0.05 was enough while
/// the only thing being collapsed was `samples: N`. Re-verification passes are
/// not that tidy: the K2 Plus `G29_RE_CHECK` re-touches a corner at the four
/// +/-0.25mm quadrant offsets around it, putting two samples of one point up to
/// 0.5mm apart. Real grid spacing is millimetres at the very tightest (tens of
/// mm on the beds that actually run re-check passes), so 0.5 separates
/// neighbours with room to spare.
inline constexpr double PROBE_POSITION_TOLERANCE_MM = 0.5;

/**
 * @brief Counts distinct mesh points from Klipper's per-sample probe output
 *
 * Klipper logs one "probe at X,Y is z=Z" line per SAMPLE, not per mesh point.
 * A printer configured with `samples: 2` therefore emits two lines per point —
 * as does any `samples_tolerance_retries` retry — so counting lines over-reports.
 * The Qidi Q2 touches the bed twice per point and showed a 36-point mesh running
 * past 36 (#1224).
 *
 * Primary strategy is positional: a line whose (x,y) matches any point already
 * seen this mesh is another sample of that point. That needs no configuration,
 * so it works on any firmware and also absorbs tolerance retries, which a fixed
 * divisor cannot.
 *
 * Matching against every point seen, rather than only the previous one, is what
 * makes it survive a firmware that leaves a point and comes back. The K2 Plus
 * closes its mesh with eight `G29_RE_CHECK` rounds alternating between two
 * corners it already probed; against the last point alone every one of those
 * touches read as new, and a 67-point mesh reported 147.
 *
 * The configured sample count remains a fallback for lines whose coordinates do
 * not parse. It is only as good as our knowledge of the printer's probe section
 * name, which is what failed here — deriving `samples` requires recognising the
 * section, and that list is open-ended across firmware forks.
 *
 * Stateful and single-threaded: feed lines in arrival order.
 */
class ProbePointCounter {
  public:
    /// @param probe_samples Configured samples per point; only used when a
    ///                      line's coordinates cannot be parsed. Values < 1
    ///                      are treated as 1.
    explicit ProbePointCounter(int probe_samples = 1)
        : samples_(probe_samples > 1 ? probe_samples : 1) {}

    /**
     * @brief Feed one G-code response line
     * @return 1-based mesh-point index, or std::nullopt if the line is not a
     *         probe result line (caller should ignore it).
     */
    std::optional<int> feed(const std::string& line) {
        if (!is_probe_result_line(line)) {
            return std::nullopt;
        }
        ++sample_lines_;

        if (auto pos = parse_probe_position(line)) {
            const bool seen_before =
                std::any_of(seen_.begin(), seen_.end(), [&](const ProbePosition& p) {
                    return std::fabs(pos->x - p.x) <= PROBE_POSITION_TOLERANCE_MM &&
                           std::fabs(pos->y - p.y) <= PROBE_POSITION_TOLERANCE_MM;
                });
            if (!seen_before) {
                seen_.push_back(*pos);
                ++points_;
            }
            return points_;
        }

        // Coordinates unparseable — ceiling-divide the raw line count. Clamped
        // upward only: a progress readout must never count backwards if a
        // malformed line lands in the middle of a positional run.
        const int divided = (sample_lines_ + samples_ - 1) / samples_;
        points_ = divided > points_ ? divided : points_;
        return points_;
    }

    /// Mesh points seen so far.
    int points() const {
        return points_;
    }

    /// Raw "probe at" lines seen so far, samples included.
    int sample_lines() const {
        return sample_lines_;
    }

    void reset() {
        points_ = 0;
        sample_lines_ = 0;
        seen_.clear();
    }

  private:
    int samples_;
    int points_ = 0;
    int sample_lines_ = 0;
    /// Every distinct point seen this mesh. Bounded by the grid size (a few
    /// hundred at the very largest), and cleared by reset() between meshes, so
    /// the linear scan per sample stays cheaper than the probe move it follows.
    std::vector<ProbePosition> seen_;
};

} // namespace helix
