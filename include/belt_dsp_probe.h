// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace helix::calibration {

/// Samples per analysis window. Matches the live meter's window, so the probe
/// measures the work the meter will actually do.
inline constexpr int PROBE_WINDOW_SAMPLES = 2048;

/// Sample rate to probe at. The reference machine is configured for 3200 Hz and
/// delivers about 3053 Hz; the probe uses the configured figure because it only
/// needs to size the transform, not to be physically accurate.
inline constexpr float PROBE_SAMPLE_RATE_HZ = 3200.0f;

/// Bandwidth to probe at. Must match what the pluck path requests, or the probe
/// measures a cheaper transform than the one that will run - roughly 700 Hz for
/// a 150 mm span with four harmonics.
inline constexpr float PROBE_BANDWIDTH_HZ = 700.0f;

/**
 * @brief Longest acceptable time for one windowed PSD, in milliseconds
 *
 * The reference BTT CB1 (Allwinner H616) measures about 23 ms for a 2048-point
 * window at 700 Hz with the phasor inner loop. The gate sits at 60 ms - roughly
 * 2.6x the reference - so the reference machine passes with real margin while a
 * board three times slower than it is excluded. The live meter targets about
 * ten updates a second, so 60 ms still leaves more than a third of the budget
 * for decoding and drawing.
 */
inline constexpr double MAX_PSD_MS = 60.0;

struct DspProbeResult {
    double psd_ms = 0.0; ///< measured milliseconds for one windowed PSD
    bool capable = false;
};

/// Pure threshold predicate. A non-positive measurement is never capable - it
/// means the timing itself is untrustworthy, not that the hardware is fast.
bool dsp_ms_is_capable(double psd_ms);

/// Run the benchmark now. Costs roughly one window's worth of work.
DspProbeResult probe_dsp_throughput();

/// Run it once per process and reuse the answer.
///
/// Deliberately not persisted to settings: a stored number goes stale on a
/// rebuild with different flags or a board swap, and a wrong cached "too slow"
/// removes the feature with no way for the user to find out why.
const DspProbeResult& cached_dsp_probe();

} // namespace helix::calibration
