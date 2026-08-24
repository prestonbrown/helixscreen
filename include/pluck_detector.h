// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <cstddef>
#include <vector>

/**
 * @file pluck_detector.h
 * @brief Onset detection and strength gating for belt plucks
 *
 * Thresholds here are measured, not chosen. Across 60 real captures on a
 * Voron 2.4: below 5x the noise floor nothing was a pluck at all, between
 * 5-9x pitch estimation was right 64% of the time, and above 9x it was right
 * 95% of the time. Rejecting weak strikes contributes more accuracy than
 * averaging does - ungated, a median never exceeded 48% at any sample count.
 */

namespace helix::calibration {

/// A ring-down segment ready for spectral analysis.
///
/// Deliberately has no strength field: extract_ringdown() is static and has
/// no access to the detection window it was pulled from, so it cannot
/// compute one. The detection-window ratio (see PluckDetector::rms_ratio())
/// belongs to whichever caller owns the live buffer.
struct PluckWindow {
    std::vector<AccelSample> samples;
};

class PluckDetector {
  public:
    /// Minimum strength, as a multiple of the noise floor, for a strike to count.
    static constexpr float MIN_RMS_RATIO = 9.0f;
    /// Skip past the impact transient before analysing.
    static constexpr float SKIP_MS = 40.0f;
    /// Length of ring-down to analyse.
    static constexpr float ANALYZE_MS = 500.0f;

    /// Sub-window the temporal shape checks measure the envelope in.
    static constexpr float ENVELOPE_SEGMENT_MS = 10.0f;
    /// How far before the loudest sub-window to sample the pre-strike level.
    static constexpr float ONSET_LOOKBACK_MS = 20.0f;
    /// Peak-to-pre-strike RMS ratio a pluck must clear.
    ///
    /// Measured across every 10-sample window phase of a batch period, on all
    /// six real captures that clear the energy gate: 26.1-71.4. A steady tone
    /// reads 1.1 and an envelope that grows across the window reads 1.2. The
    /// figure is quoted across phases deliberately - an earlier revision
    /// anchored this ratio to the loudest segment rather than to the strike
    /// and read 34-46 at the single phase its harness happened to exercise,
    /// while dropping to 1.16 at phases that harness never presented.
    static constexpr float MIN_ONSET_RISE = 3.0f;
    /// Sub-windows the post-onset envelope is split into.
    static constexpr int DECAY_SEGMENTS = 4;
    /// How much louder a decay sub-window may be than the one before it.
    /// Measured 1.03x worst case across the real captures.
    static constexpr float MAX_DECAY_RISE = 1.5f;
    /// Last sub-window as a fraction of the first. Measured 0.13-0.21 on the
    /// real captures; a steady tone reads 0.98 and the weak-pluck capture,
    /// which never rang, reads 0.79.
    static constexpr float MAX_DECAY_END_RATIO = 0.5f;
    /// Shortest post-onset region the decay check can judge. An event whose
    /// onset lands at the very end of the window has no envelope to read, and
    /// "cannot tell" must mean rejected here - that is what an event ramping
    /// up into the future looks like.
    static constexpr float MIN_DECAY_SPAN_MS = 100.0f;
    /// Post-onset signal a window needs before it is worth resolving at all.
    ///
    /// extract_ringdown() always returns ANALYZE_MS of samples, padding the
    /// front with whatever preceded the strike when the strike landed late.
    /// At 70% of ANALYZE_MS that padding cannot outweigh the ring-down.
    /// Measured on tests/fixtures/belt_plucks/ spliced into a live stream:
    /// 350-400 ms accepts every capture that clears the energy gate, and by
    /// 450 ms the window has slid so far that a 500 ms capture has run out
    /// and the analysis starts reading past its end.
    static constexpr float MIN_RINGDOWN_MS = 350.0f;

    /// Broadband RMS with per-axis DC removed. Static so callers can measure a
    /// buffer without owning a detector.
    static float window_rms(const AccelSample* samples, size_t count);

    void set_noise_floor(float rms) {
        noise_floor_ = rms;
    }
    [[nodiscard]] float noise_floor() const {
        return noise_floor_;
    }

    /// Measure the floor from a buffer captured while the machine is still.
    bool learn_noise_floor(const std::vector<AccelSample>& quiet);

    /// Strength of a window as a multiple of the noise floor. 0 if no floor set.
    ///
    /// Operates on the live detection window (the buffer being watched for a
    /// strike, onset included) - not on an extracted ring-down. A ring-down
    /// has already decayed for SKIP_MS+ before the caller ever sees it, so its
    /// RMS reads several times lower than the detection window that triggered
    /// it. Feeding extract_ringdown() output back into this rejects strikes
    /// that were genuinely firm.
    [[nodiscard]] float rms_ratio(const AccelSample* samples, size_t count) const;

    /// True if this window is strong enough to analyse. Same detection-window
    /// contract as rms_ratio() - see its comment. MIN_RMS_RATIO was calibrated
    /// against detection windows across 60 captures; it is not meaningful
    /// against a ring-down.
    [[nodiscard]] bool passes_gate(const AccelSample* samples, size_t count) const;

    /// Index of the strongest instantaneous deviation from the buffer mean -
    /// the strike, when there was one. Returns 0 for an empty buffer.
    ///
    /// This is the loudest SAMPLE, which for a plucked string is its attack
    /// transient (measured across every window phase of the six real captures,
    /// the rise anchored here runs 26-71x). For broadband energy it is instead
    /// a random draw somewhere inside the event, so an onset located this way
    /// can sit tens of samples past a thump's true leading edge.
    static size_t find_onset(const AccelSample* samples, size_t count);

    /// True if energy jumps from the pre-strike level to the peak within a few
    /// milliseconds.
    ///
    /// Operates on the live DETECTION WINDOW, like rms_ratio() and
    /// passes_gate(): the evidence is the quiet that came BEFORE the strike,
    /// and an extracted ring-down has already thrown that away. A window whose
    /// STRIKE sits within ONSET_LOOKBACK_MS of its start returns false - there
    /// is nothing to compare against, and a steady tone looks exactly like
    /// that.
    ///
    /// The reference is taken from the strike, located by find_onset(), and
    /// not from the loudest segment: on a real capture the loudest 10 ms
    /// frequently lands after the strike, which puts a peak-anchored reference
    /// inside the ring-down. See the note on MIN_ONSET_RISE.
    [[nodiscard]] static bool has_sharp_onset(const AccelSample* samples, size_t count,
                                              float sample_rate);

    /// True when the window holds enough signal after the onset to be worth
    /// resolving - at least MIN_RINGDOWN_MS of it.
    ///
    /// A firm strike trips the energy gate as soon as its leading edge enters
    /// the window, before there is any envelope to read. The answer to false
    /// here is to WAIT: the window slides and the rest of the ring-down is
    /// already on its way. Rejecting instead throws away exactly the firmest
    /// plucks, since those are the ones that trip the gate earliest.
    ///
    /// An event that never stops growing is therefore never judged at all -
    /// find_onset() keeps returning a sample near the end, the post-onset span
    /// stays short, and the caller waits indefinitely. That is safe (nothing
    /// can be accepted without being judged) but it means a swell is handled
    /// by being ignored rather than by has_pluck_decay(); the envelope checks
    /// see it only once it peaks and starts to fall.
    [[nodiscard]] static bool ringdown_ready(const AccelSample* samples, size_t count,
                                             float sample_rate);

    /// True if the envelope after the onset falls the way a plucked string's
    /// does: each sub-window no more than MAX_DECAY_RISE louder than the one
    /// before, and the last well below the first.
    ///
    /// Locates its own onset, so it reads the same on a detection window and
    /// on an extracted ring-down. A steady tone fails on the end ratio; a
    /// rattle fails on the rise tolerance.
    [[nodiscard]] static bool has_pluck_decay(const AccelSample* samples, size_t count,
                                              float sample_rate);

    /// Locate the strongest transient in `buffer` and extract the ring-down
    /// beginning SKIP_MS after it.
    static bool extract_ringdown(const std::vector<AccelSample>& buffer, float sample_rate,
                                 PluckWindow* out);

  private:
    float noise_floor_ = 0.0f;
};

} // namespace helix::calibration
