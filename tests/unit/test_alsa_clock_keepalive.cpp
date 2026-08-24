// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_alsa_clock_keepalive.cpp
 * @brief Which outputs must keep their audio clock running while idle.
 *
 * Bug context (prestonbrown/helixscreen#1337): v0.99.114 parked the ALSA render
 * thread when idle, which stops the audio clock after every sound. HDMI and
 * S/PDIF receivers mute themselves while re-locking a restarted clock — measured
 * at 0.5-1.0 s on a reporter's BTT HDMI5 with a beep ladder, against UI sounds
 * that are 7-100 ms long. Every one of them landed inside the mute.
 *
 * The park is still worth having on outputs that do not re-lock, so the decision
 * has to be per-device rather than a blanket revert. These cases pin the two
 * ways a sink can be recognised, because either one alone misses real hardware.
 */

#include "alsa_clock_keepalive.h"

#include "../catch_amalgamated.hpp"

using helix::audio::device_needs_clock_keepalive;

TEST_CASE("Clock keepalive: HDMI recognised from the caller's device string",
          "[audio][keepalive]") {
    // The reporter's Pi 4. The driver's PCM name is not relied on here.
    CHECK(device_needs_clock_keepalive("plughw:CARD=vc4hdmi0,DEV=0", ""));
    CHECK(device_needs_clock_keepalive("plughw:CARD=allwinnerhdmi,DEV=0", ""));
}

TEST_CASE("Clock keepalive: HDMI recognised from the driver's PCM name", "[audio][keepalive]") {
    // A device addressed by index gives the predicate nothing to match on, so
    // the driver's own name for the PCM is what identifies it. Missing this
    // case would silently leave the bug in place for anyone using hw:N,M.
    CHECK(device_needs_clock_keepalive("hw:1,0", "hdmi i2s-hifi-0"));
}

TEST_CASE("Clock keepalive: S/PDIF and IEC958 count too", "[audio][keepalive]") {
    // Same mechanism: a clock handed to a receiver that must acquire it.
    CHECK(device_needs_clock_keepalive("iec958:CARD=Intel,DEV=0", ""));
    CHECK(device_needs_clock_keepalive("hw:0,1", "SPDIF Out"));
}

TEST_CASE("Clock keepalive: matching is case-insensitive", "[audio][keepalive]") {
    CHECK(device_needs_clock_keepalive("plughw:CARD=HDMI,DEV=0", ""));
    CHECK(device_needs_clock_keepalive("hw:2,0", "HDMI Audio Out"));
}

TEST_CASE("Clock keepalive: ordinary codecs still park", "[audio][keepalive]") {
    // The park exists to save ~172 writer wakeups/sec on a host sharing a CPU
    // with Klipper. Keeping it everywhere would throw that away, so a plain
    // codec must NOT be treated as needing keep-alive.
    CHECK_FALSE(device_needs_clock_keepalive("plughw:CARD=Codec,DEV=0", "CDC PCM Codec-0"));
    CHECK_FALSE(device_needs_clock_keepalive("default", ""));
    CHECK_FALSE(device_needs_clock_keepalive("hw:0,0", "Media Stream sunxi-ahub-aif1-0"));
}
