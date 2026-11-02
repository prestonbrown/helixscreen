// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "m300_sound_backend.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

M300SoundBackend::M300SoundBackend(GcodeSender sender, PrinterBusy busy)
    : sender_(std::move(sender)), busy_(std::move(busy)) {}

void M300SoundBackend::set_tone(float freq_hz, float amplitude, float duty_cycle) {
    (void)duty_cycle;

    if (!sender_)
        return;

    if (printer_busy()) {
        // Nothing is ringing either: each note's P duration ends it on its own.
        last_freq_ = 0;
        return;
    }

    // Skip if same frequency (avoid spamming redundant commands)
    int freq_int = static_cast<int>(freq_hz);
    if (freq_int == last_freq_ && amplitude > 0)
        return;

    if (amplitude <= 0.01f) {
        silence();
        return;
    }

    // Clamp to M300 safe range
    freq_int = std::max(100, std::min(10000, freq_int));

    // M300 format: S=frequency (Hz), P=duration (ms)
    // Use min_tick_ms as duration -- the sequencer ticks at this interval
    // and will re-send if the tone continues, or call silence() to stop.
    // This prevents short notes (e.g., 6ms tap) from ringing for too long.
    int dur_ms = static_cast<int>(min_tick_ms());
    std::string gcode = "M300 S" + std::to_string(freq_int) + " P" + std::to_string(dur_ms);

    sender_(gcode);
    last_freq_ = freq_int;

    spdlog::trace("[M300Backend] set_tone: {} Hz, P{}", freq_int, dur_ms);
}

void M300SoundBackend::silence() {
    if (last_freq_ == 0)
        return;
    last_freq_ = 0;
    if (printer_busy()) {
        return;
    }
    // M300 S0 = silence on most firmware
    if (sender_) {
        sender_("M300 S0 P1");
    }
    spdlog::trace("[M300Backend] silence");
}

float M300SoundBackend::min_tick_ms() const {
    // M300 has high latency -- no point ticking faster than ~50ms
    return 50.0f;
}
