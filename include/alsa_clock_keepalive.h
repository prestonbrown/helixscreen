// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace helix::audio {

/**
 * @brief Whether an output must keep its audio clock running while idle.
 *
 * HDMI and S/PDIF receivers re-lock when the audio clock stops and restarts,
 * and mute themselves while they do. Measured on a reporter's BTT HDMI5 via a
 * beep-ladder test: 0.5-1.0 s swallowed on every restart
 * (prestonbrown/helixscreen#1337). Every UI sound we ship is 7-100 ms, so all
 * of them land inside that window and are never heard at all.
 *
 * The idle park added in v0.99.114 stops the clock after every sound, which is
 * what exposed this. Parking is still worth having on outputs that do NOT
 * re-lock — an on-board codec costs ~172 writer wakeups/sec at our period size,
 * which matters on a host sharing a CPU with Klipper — so the park is kept and
 * suppressed only for this class of sink.
 *
 * The Linux kernel solves the same problem the same way, deliberately: see
 * CONFIG_SND_HDA_INTEL_HDMI_SILENT_STREAM, whose notes record receivers muting
 * "up to 2-3 seconds" at playback start.
 *
 * Pure so it can be tested without an ALSA device. Callers pass whatever they
 * have; either string matching is enough, because a device can be addressed by
 * a name that hides its nature (`hw:1,0`) while the driver's own PCM name says
 * `hdmi`, and vice versa.
 *
 * @param device_name The PCM name the caller opened (e.g. "plughw:CARD=vc4hdmi0,DEV=0").
 * @param pcm_name    The driver's own name for the PCM (e.g. "hdmi i2s-hifi-0"), or "".
 * @return true when the clock must be kept running.
 */
inline bool device_needs_clock_keepalive(const std::string& device_name,
                                         const std::string& pcm_name) {
    auto lowered = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    const std::string haystack = lowered(device_name) + " " + lowered(pcm_name);
    // iec958/spdif carry the same re-lock behaviour as HDMI — both hand a
    // clock to a receiver that has to acquire it before it will emit anything.
    return haystack.find("hdmi") != std::string::npos ||
           haystack.find("iec958") != std::string::npos ||
           haystack.find("spdif") != std::string::npos;
}

} // namespace helix::audio
