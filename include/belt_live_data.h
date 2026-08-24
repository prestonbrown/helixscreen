// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

/**
 * @file belt_live_data.h
 * @brief Scalars and traces the live belt tuner hands to its widgets
 *
 * Everything here is display-side. The measurement itself lives in
 * BeltListenSession; this header owns the two questions the UI has to answer
 * about a measurement: how well two belts match, and what to draw.
 */

namespace helix::calibration {

/**
 * @brief Smallest belt-to-belt difference this instrument can actually resolve
 *
 * Not a tolerance and not a preference - a physical limit. ANALYZE_MS fixes the
 * ring-down at 500 ms, and a 500 ms observation cannot resolve finer than about
 * 1/0.5 s = 2 Hz. It shows up concretely as bin quantisation: at a 2048-sample
 * window and the measured 3053 Hz stream rate the PSD bins are 1.51 Hz apart,
 * and the estimator returns bin centres. The reference machine read belt A at
 * 86.03 Hz (bin 57) and belt B at 80.00 Hz (bin 53) against an 82 Hz ground
 * truth that is not representable at all.
 *
 * Two consequences the UI must honour: frequencies are displayed as whole Hz,
 * because a decimal claims precision that is not there, and an A-vs-B delta
 * below this reads as matched rather than as a difference. Zero-padding the FFT
 * would interpolate finer bins but would add no information.
 */
inline constexpr float BELT_RESOLUTION_HZ = 2.0f;

/**
 * @brief Quiet stretch after which the panel prompts for a pluck
 *
 * A stream with no plucks in it is not an error - the user is walking round the
 * machine. Long enough that a deliberate pause between strikes does not nag,
 * short enough that someone who has lost the thread gets told what to do.
 */
inline constexpr uint32_t IDLE_HINT_MS = 8000;

/**
 * @brief How long a rejected strike keeps saying "pluck harder"
 *
 * Rejections are instantaneous events on a 10 Hz batch stream. Without a hold
 * the hint would appear and vanish within 100 ms, which is unreadable.
 */
inline constexpr uint32_t REJECT_HINT_MS = 3000;

/**
 * @brief How closely a measured belt matches the reference belt, 0-100
 *
 * 100 when they are identical, falling linearly with the absolute difference
 * as a fraction of the reference: a belt off by the whole reference frequency
 * reads 0. Clamped, so an octave error reads 0 rather than a negative number
 * that a progress bar would render as a full bar.
 *
 * @param reference_hz Belt A's committed median. Non-positive means there is no
 *        reference yet, which yields 0.
 */
float belt_match_percent(float reference_hz, float measured_hz);

/**
 * @brief Are two belt readings the same, as far as this instrument can tell?
 *
 * The verdict wording and the match bar go through here rather than comparing
 * frequencies directly, so the UI never reports a difference smaller than
 * BELT_RESOLUTION_HZ - a difference it cannot actually measure.
 */
bool belt_frequencies_match(float a_hz, float b_hz);

/// True once a healthy stream has gone this long without resolving a strike.
bool belt_should_show_idle_hint(uint32_t ms_since_last_event);

/**
 * @brief Downsampled traces for the LISTEN widgets
 *
 * @par Main thread only, and therefore unlocked
 * The writer is the panel's queued update (BeltTensionPanel::publish_live_values,
 * which runs inside UpdateQueue on the LVGL thread) and the reader is a widget
 * draw callback, which LVGL also runs on that thread. There is no second writer:
 * the accelerometer batch callback hands its numbers across through the update
 * queue and never touches this class. Adding a mutex would only lock the main
 * thread against itself.
 *
 * Both reductions take the peak of each bucket rather than its mean. A mean
 * flattens a ring-down into a straight line at the DC level and buries an 86 Hz
 * spike in the noise around it, which erases the only thing either trace exists
 * to show.
 */
class BeltLiveData {
  public:
    static BeltLiveData& instance();

    /// Horizontal resolution of both traces. Sized for the widest trace widget
    /// on a 1024 px panel without redrawing more points than a pixel column.
    static constexpr size_t TRACE_POINTS = 128;

    /**
     * @brief Replace the waveform trace from a detection window
     *
     * Each output point is the largest DC-free deviation magnitude in its
     * bucket, matching PluckDetector::window_rms()'s definition of signal:
     * per-axis mean removed, then the three-axis norm. Gravity is a per-axis
     * DC term, so removing the mean is what keeps a 9800 mm/s^2 constant from
     * swamping a pluck. A window shorter than TRACE_POINTS yields one point per
     * sample rather than nothing, so the widget is never blank at the start of
     * a measurement. An empty window clears the trace.
     */
    void set_waveform(const std::vector<AccelSample>& window);

    /// Replace the spectrum trace from a (frequency, power) PSD. Keeps bucket
    /// maxima. An empty PSD clears the trace and spectrum_peak_hz().
    void set_spectrum(const std::vector<std::pair<float, float>>& psd);

    [[nodiscard]] const std::vector<float>& waveform() const {
        return waveform_;
    }
    [[nodiscard]] const std::vector<float>& spectrum() const {
        return spectrum_;
    }

    /**
     * @brief The frequency of the tallest bin in the last spectrum passed to set_spectrum()
     *
     * Read from the full PSD, not the downsampled bars spectrum() returns:
     * bucket-maxima reduction keeps the peak's power but not reliably its
     * original bin, and this is the number the peak label exists to show
     * precisely (see BeltTrace's spectrum draw branch). 0 before the first
     * spectrum arrives.
     */
    [[nodiscard]] float spectrum_peak_hz() const {
        return spectrum_peak_hz_;
    }

    void clear();

  private:
    BeltLiveData() = default;

    std::vector<float> waveform_;
    std::vector<float> spectrum_;
    float spectrum_peak_hz_ = 0.0f;
};

} // namespace helix::calibration
