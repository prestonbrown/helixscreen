// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <array>
#include <functional>
#include <optional>
#include <string>

namespace helix {

class IMoonrakerClient;
}

namespace helix::calibration {

/// The [resonance_tester] settings a resonance run sweeps with.
struct ResonanceTesterConfig {
    float min_freq = 5.0f;     ///< Klipper default
    float max_freq = 135.0f;   ///< Klipper default
    float hz_per_sec = 1.0f;   ///< Klipper default
    bool from_printer = false; ///< true when read from configfile, false = defaults
    /// The first probe_points entry (x, y, z), machine coordinates. nullopt
    /// when absent or unparseable; that alone does not reject the range.
    std::optional<std::array<float, 3>> probe_point;
    /// accel_chip, accel_chip_x and accel_chip_y as configured, trimmed; ""
    /// when absent or not a string. Names a config section ("adxl345",
    /// "adxl345 hotend"), not an endpoint.
    std::string accel_chip;
    std::string accel_chip_x;
    std::string accel_chip_y;

    /// Seconds one sweep takes: (max - min) / hz_per_sec.
    [[nodiscard]] float sweep_seconds() const;
};

/// Parse `configfile.settings.resonance_tester` (values may be numbers or
/// strings). Missing keys keep their defaults. `probe_points` is read as a
/// list of [x, y, z] lists or an "x, y, z" string, first point only.
[[nodiscard]] ResonanceTesterConfig parse_resonance_tester_config(const nlohmann::json& settings);

/// Ask the printer what [resonance_tester] range it will actually sweep.
/// The range varies - Klipper's default ceiling is 133.33 Hz, Kalico's is
/// 135, and the section can set anything - so it is queried rather than
/// assumed: mapping sweep progress against a guessed ceiling pins the bar at
/// an arbitrary point mid-run. `on_done` receives defaults on any error or
/// missing section, which leaves the caller's own fallbacks in force.
void query_resonance_tester_config(IMoonrakerClient& client,
                                   std::function<void(ResonanceTesterConfig)> on_done);

/// "Testing frequency 74 Hz" -> 74. nullopt for any other line.
[[nodiscard]] std::optional<float> parse_testing_frequency(const std::string& line);

/// The path from "<anything> data written to <path>.csv file"; matches both
/// "Shaper calibration data written to" and "Resonances data written to".
[[nodiscard]] std::optional<std::string> parse_written_csv_path(const std::string& line);

/// 0-100 progress of a sweep at `freq`, clamped.
[[nodiscard]] int sweep_percent(float freq, const ResonanceTesterConfig& cfg);

} // namespace helix::calibration
