// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_capture.h"
#include "belt_stream_client.h"
#include "belt_tension_types.h"
#include "pitch_estimator.h"
#include "pluck_aggregator.h"
#include "pluck_detector.h"

#include <optional>
#include <vector>

namespace helix::calibration {

// PluckReject lives in belt_capture.h - CaptureVerdict needs it too, and
// belt_capture.h has no reason to depend back on this header.

/// One resolved strike. A rejected event still carries its ratio so the UI can
/// say "too soft" rather than staying silent.
struct PluckEvent {
    float frequency_hz = 0.0f;
    float rms_ratio = 0.0f;
    bool accepted = false;
    PluckReject reject = PluckReject::NONE;
};

/**
 * @brief Turns a live sample stream into a running median belt frequency
 *
 * Owns the sliding detection window, the strength gate, the harmonic pitch
 * estimate and the running median. Pure over sample buffers: no sockets, no
 * LVGL, no threads. Callers hand it whatever AccelBatch arrives.
 *
 * @par Window size is load-bearing
 * DETECTION_WINDOW_SAMPLES is 2048 because PluckDetector::MIN_RMS_RATIO was
 * calibrated against 2048-sample detection windows over 60 real captures.
 * Changing the window silently invalidates the threshold.
 *
 * @par The gate runs on the detection window
 * Never on an extracted ring-down. A ring-down has decayed for SKIP_MS before
 * anyone sees it and reads roughly three times weaker; gating on one rejects
 * strikes that were genuinely firm. See pluck_detector.h.
 *
 * @par Energy is not enough
 * Clearing MIN_RMS_RATIO says something happened, not that it was a pluck. A
 * door closing, a stepper cogging or a fan stepping through a mode all clear
 * 9x easily - three such events measured 44-53x on the reference machine and
 * were reported as plucks nobody made. The shape checks in PluckDetector and
 * the harmonic-concentration check on the resulting spectrum are what decide.
 *
 * @par Cooldown
 * One physical pluck appears in many overlapping windows. After resolving a
 * strike the session ignores detections until the buffer has advanced past the
 * analysed region, so a single pluck is counted once.
 */
class BeltListenSession {
  public:
    /// Calibrated with MIN_RMS_RATIO. Not a free parameter - see above.
    static constexpr size_t DETECTION_WINDOW_SAMPLES = 2048;

    /**
     * @param span_mm Free belt span, which sets the harmonic search window
     * @param sample_rate_hz Measured stream rate. Take it from
     *        BeltStreamClient::sample_rate_hz(), not from the configured rate -
     *        the reference machine is configured for 3200 Hz and delivers 3053.
     */
    BeltListenSession(float span_mm, float sample_rate_hz);

    /// Measure the floor from a buffer captured while the machine was still.
    /// Learns the quiet SPECTRUM from the same buffer, which is what rejects a
    /// fan tone sitting inside the search window.
    bool learn_noise_floor(const std::vector<AccelSample>& quiet);

    /// Set the scalar floor directly. Leaves the quiet spectrum unlearned, so
    /// a session set up this way has no background rejection - prefer
    /// learn_noise_floor() wherever the samples are available.
    void set_noise_floor(float rms);
    [[nodiscard]] float noise_floor() const;

    /// The spectrum of the quiet window, learned alongside the scalar floor.
    /// Not to be confused with last_spectrum(), which is the most recent
    /// accepted PLUCK's spectrum, for display.
    ///
    /// Learned once, when learn_noise_floor() runs, and never re-learned: a
    /// fan that spins up part-way through a session is not in it and will not
    /// be discounted. Re-learning would need a quiet window the user is not
    /// plucking into, which the listening flow does not have.
    [[nodiscard]] const QuietSpectrum& quiet_spectrum() const {
        return quiet_spectrum_;
    }

    /**
     * @brief Consume a batch
     * @return the event, if this batch completed a strike; nullopt otherwise
     *
     * A batch with nonzero errors or overflows is appended for display but
     * never analysed - klippy dropped samples, so the window is not a real
     * ring-down and any frequency read from it would be invented.
     */
    std::optional<PluckEvent> push(const AccelBatch& batch);

    [[nodiscard]] size_t accepted_count() const;
    [[nodiscard]] size_t rejected_count() const;
    [[nodiscard]] float median_hz() const;
    [[nodiscard]] bool committed() const;

    /// The current detection window, for the live waveform.
    [[nodiscard]] const std::vector<AccelSample>& window() const {
        return window_;
    }

    /// The PSD of the most recently ACCEPTED pluck, for the live spectrum
    /// strip. Empty until the first accepted strike, and again after reset()
    /// - a rejected strike leaves the previous spectrum in place rather than
    /// clearing it, since there is nothing new to show and the last real
    /// reading is more honest than a blank strip.
    [[nodiscard]] const std::vector<std::pair<float, float>>& last_spectrum() const {
        return last_spectrum_;
    }

    void reset();

  private:
    float span_mm_;
    float sample_rate_hz_;
    PluckDetector detector_;
    PluckAggregator aggregator_;
    QuietSpectrum quiet_spectrum_;
    std::vector<AccelSample> window_;
    std::vector<std::pair<float, float>> last_spectrum_;
    size_t rejected_ = 0;
    size_t cooldown_samples_ = 0;

    /// Writes every resolved event to HELIX_BELT_CAPTURE_DIR when set;
    /// every method is a no-op otherwise. See belt_capture.h.
    BeltCaptureWriter capture_;
};

} // namespace helix::calibration
