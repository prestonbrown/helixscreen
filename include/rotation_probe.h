// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "subject_managed_panel.h"
#include "tap_latch.h"

#include <functional>
#include <lvgl.h>

namespace helix {

class Config;

/// What the probe reaches on the display it is detecting the orientation of.
struct RotationProbeHost {
    /// The pointer device. A reference to the owner's member because an unplug mid-probe
    /// clears it, and every read must see that.
    lv_indev_t* const& pointer;
    /// SDL cannot rotate visually; the probe then skips rotating and uses press-only detection.
    bool is_sdl = false;
    /// Rotates the display and refreshes the owner's cached resolution.
    std::function<void(lv_display_rotation_t)> settle_rotation;
    /// Holds the debounced resize fan-out off while rotations are being tried.
    std::function<void(bool)> suspend_resize_fanout;
    /// Lights the backlight. Called before every prompt is built: some stock firmwares dim the
    /// panel behind our back during boot, and a prompt on a dark panel cannot be answered.
    std::function<void()> keep_backlight_on;
    uint32_t scan_timeout_ms = 5000;
    uint32_t confirm_timeout_ms = 10000;
};

/**
 * @brief First-boot interactive orientation probe
 *
 * Shows each rotation in turn and asks the user to tap when the text reads right-side up,
 * then to tap again to confirm. A tap is read straight from the pointer's read callback,
 * because LVGL's own input processing is switched off while the probe runs.
 * Main thread only.
 */
/// Whether the first-boot probe still has a question to ask: nobody has answered it and no
/// rotation is configured. A configured /display/rotate, even 0, is an answer.
bool rotation_probe_unanswered(const Config& config);

class RotationProbe {
  public:
    explicit RotationProbe(RotationProbeHost host);
    ~RotationProbe();

    RotationProbe(const RotationProbe&) = delete;
    RotationProbe& operator=(const RotationProbe&) = delete;

    /// Runs the probe to completion and returns the rotation in degrees. A confirmed answer is
    /// saved to /display/rotate and /display/rotation_probed. After three unanswered sweeps it
    /// applies 0 for this session only and saves nothing, so the next boot asks again.
    int run();

  private:
    enum class Phase { Scan, Confirm };

    /// Replaces the active screen's content with the prompt for @p phase.
    void show_screen(Phase phase, int rot_deg, int step, int steps, uint32_t timeout_ms);
    void set_subtitle(Phase phase, int rot_deg, int secs, int step, int steps);
    bool wait_for_tap(Phase phase, uint32_t timeout_ms, int rot_deg, int step, int steps);
    void poll_pointer();
    void drain_until_release();

    RotationProbeHost m_host;
    SubjectManager m_subjects;
    lv_subject_t m_main_subject{};
    lv_subject_t m_help_subject{};
    lv_subject_t m_subtitle_subject{};
    char m_main_buf[160] = "";
    char m_help_buf[160] = "";
    char m_subtitle_buf[160] = "";
    TapLatch m_tap_latch;
};

} // namespace helix
