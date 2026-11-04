// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sound_backend.h"

#include <functional>
#include <string>

/// M300 G-code backend -- sends M300 beeper commands via Moonraker/Klipper
///
/// Uses a GcodeSender callback to decouple from MoonrakerClient, allowing
/// easy testing with a captured lambda. Deduplicates redundant frequency
/// commands and clamps to the M300-safe range of 100-10000 Hz.
///
/// M300 shares Klipper's G-code queue with the moves being printed, and a
/// firmware that implements it as a macro holds that queue for each note's
/// duration, so a tune played mid-print stalls the toolhead. While the
/// PrinterBusy check reports the queue busy, notes are dropped, not sent.
class M300SoundBackend : public SoundBackend {
  public:
    /// Callback type for sending G-code strings (e.g., wrapping MoonrakerClient::gcode_script)
    using GcodeSender = std::function<int(const std::string&)>;
    /// Whether a print job is running; called on the sequencer thread, so it
    /// must be thread-safe.
    using PrinterBusy = std::function<bool()>;

    /// @param sender  Function that sends a G-code string. May be empty/null (safe no-op).
    /// @param busy    Notes are dropped while it returns true. Empty = never busy.
    explicit M300SoundBackend(GcodeSender sender, PrinterBusy busy = {});

    // SoundBackend interface
    void set_tone(float freq_hz, float amplitude, float duty_cycle) override;
    void silence() override;
    float min_tick_ms() const override;

    /// M300 beeps are G-code sent to the printer, so this backend is only
    /// meaningful while a Moonraker client is attached.
    bool needs_moonraker_client() const override {
        return true;
    }

  private:
    bool printer_busy() const {
        return busy_ && busy_();
    }

    GcodeSender sender_;
    PrinterBusy busy_;
    int last_freq_ = 0;
};
