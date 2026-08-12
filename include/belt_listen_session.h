// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_stream_client.h"
#include "belt_tension_types.h"
#include "pluck_aggregator.h"
#include "pluck_detector.h"

#include <optional>
#include <vector>

namespace helix::calibration {

/// One resolved strike. A rejected event still carries its ratio so the UI can
/// say "too soft" rather than staying silent.
struct PluckEvent {
    float frequency_hz = 0.0f;
    float rms_ratio = 0.0f;
    bool accepted = false;
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
    bool learn_noise_floor(const std::vector<AccelSample>& quiet);
    void set_noise_floor(float rms);
    [[nodiscard]] float noise_floor() const;

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
    std::vector<AccelSample> window_;
    std::vector<std::pair<float, float>> last_spectrum_;
    size_t rejected_ = 0;
    size_t cooldown_samples_ = 0;
};

} // namespace helix::calibration
