// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "belt_tension_types.h"

#include <optional>
#include <string>
#include <vector>

/**
 * @file belt_capture.h
 * @brief Writes raw pluck events to disk so the reference machine can hand
 *        over real fixtures instead of eight captures from one evening
 *
 * Every threshold in PluckDetector and pitch_estimator was measured against a
 * small, fixed capture set, and the algorithm has since been tuned against
 * that same set - circular. This is the instrument that collects the next
 * round: every resolved event (accepted or rejected) written to disk in the
 * same format tests/fixtures/belt_plucks/ already uses, so a capture drops
 * straight in as a fixture with no conversion step.
 *
 * Gated entirely on HELIX_BELT_CAPTURE_DIR (see belt_capture_dir()) - unset
 * means BeltCaptureWriter::enabled() is false and every method is a no-op.
 */

namespace helix::calibration {

/// Why a strike was not measured. The distinction is user-facing: "pluck
/// harder" is the wrong instruction for someone who plucked firmly and had a
/// fan swamp it, and following it just makes the reading worse.
///
/// Lives here rather than in belt_listen_session.h (its more obvious home,
/// and where PluckEvent - the other type that carries it - still is)
/// because CaptureVerdict below needs it too, and belt_capture.h has no
/// reason to depend back on the session header.
enum class PluckReject {
    NONE,       ///< accepted
    TOO_SOFT,   ///< cleared MIN_DETECTABLE_RATIO but not the strength gate
    NOT_A_PLUCK ///< strong enough, but the wrong shape or no belt tone in it
};

/// Which buffer a rendered file holds. The live detection window and the
/// extracted ring-down are different buffers with different meanings -
/// PluckDetector's doc comments exist specifically to keep them from being
/// conflated - so a rendered file always says which one it is.
enum class CaptureBufferKind {
    DETECTION_WINDOW, ///< the live window the gate and onset checks judged
    RINGDOWN,         ///< the extracted post-onset segment the estimator saw
    QUIET,            ///< the buffer the noise floor and quiet spectrum were learned from
};

/// The four numbers behind a verdict, plus the estimate and the running
/// median at the moment the event resolved. A field left at nullopt was
/// genuinely never evaluated on this event's path - e.g. harmonic_concentration
/// on a strike that failed the onset check before pitch estimation ever ran -
/// and renders as `n/a` rather than a misleading 0.
struct CaptureVerdict {
    bool accepted = false;
    PluckReject reject = PluckReject::NONE;
    float rms_ratio = 0.0f;
    std::optional<float> onset_rise;
    std::optional<float> decay_end_ratio;
    std::optional<float> harmonic_concentration;
    float estimate_hz = 0.0f;
    float median_hz = 0.0f;

    /// "ACCEPTED", "TOO_SOFT", or "NOT_A_PLUCK" - what the verdict= field in
    /// the rendered header carries.
    [[nodiscard]] const char* verdict_string() const;
};

/// Render one buffer to the exact format tests/fixtures/belt_plucks/ uses:
/// three `#` comment lines, a `#time,accel_x,accel_y,accel_z` header, then
/// the data rows - parsed unchanged by parse_accel_csv(). Pure: no
/// filesystem, so a test can assert on the returned string directly.
///
/// The verdict fields ride on the third comment line, extending the existing
/// `sample_rate_hz=` / `rms_over_noise_floor=` pair rather than inventing a
/// second header line - an old and a new capture still parse identically "by
/// eye". Only rendered for DETECTION_WINDOW and RINGDOWN; a QUIET buffer has
/// no verdict, so its third line carries just the sample rate.
std::string render_capture(const std::vector<AccelSample>& samples, float sample_rate_hz,
                           CaptureBufferKind kind, const CaptureVerdict& verdict, float span_mm);

/// Read `sample_rate_hz=` back out of a capture (or fixture) header, for a
/// replay path that only has the file, not the session that wrote it.
/// Returns 0.0f if the key is missing or malformed - the same "no data"
/// convention parse_accel_csv() uses, so a caller already has to handle it.
float parse_capture_sample_rate(const std::string& csv_text);

/// HELIX_BELT_CAPTURE_DIR, or empty if unset. A plain getenv() wrapper, same
/// as every other env accessor in the tree - cheap enough that callers are
/// not expected to cache it themselves.
std::string belt_capture_dir();

/**
 * @brief Writes every resolved pluck event to a directory, if one was configured
 *
 * One instance per BeltListenSession, constructed with belt_capture_dir()'s
 * result. Disabled (every method a no-op, nothing allocated) whenever that
 * directory is empty - which is the default, since this is a diagnostic tool
 * for the reference machine, not a feature a normal user run should ever
 * write files for.
 *
 * Filenames carry an incrementing sequence number so two events landing in
 * the same wall-clock second cannot collide and a directory listing sorts in
 * capture order. The detection window and, when one was extracted, the
 * ring-down are written as separate files per event - the format holds one
 * buffer per file, and conflating the two under a shared name is exactly the
 * mistake this whole tool exists to avoid.
 */
class BeltCaptureWriter {
  public:
    /// @param dir Destination directory. Created on first write if missing.
    ///        Empty disables the writer entirely.
    /// @param span_mm Forwarded into every rendered header.
    BeltCaptureWriter(std::string dir, float span_mm);

    [[nodiscard]] bool enabled() const {
        return !dir_.empty();
    }

    /// Write the detection window - and the ring-down, when extract_ringdown()
    /// produced one - for a single resolved event. No-op if disabled.
    void write_event(const std::vector<AccelSample>& detection_window,
                     const std::vector<AccelSample>* ringdown, float sample_rate_hz,
                     const CaptureVerdict& verdict);

    /// Write the quiet buffer a session learned its noise floor and quiet
    /// spectrum from. No-op if disabled. Called once per session, alongside
    /// BeltListenSession::learn_noise_floor().
    void write_quiet(const std::vector<AccelSample>& quiet, float sample_rate_hz);

  private:
    void write_file(const std::string& stem, const std::vector<AccelSample>& samples,
                    float sample_rate_hz, CaptureBufferKind kind, const CaptureVerdict& verdict);

    std::string dir_;
    float span_mm_;
    int sequence_ = 0;
};

} // namespace helix::calibration
