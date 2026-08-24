// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "note_event.h"

#include <functional>
#include <string>

/// Sound priority levels (higher numeric value = more important)
enum class SoundPriority {
    UI = 0,    // button taps, nav sounds — can be interrupted by anything
    EVENT = 1, // print complete, errors — only interrupted by ALARM
    ALARM = 2  // critical alerts — never interrupted
};

/**
 * @brief Abstract interface for sound output backends
 *
 * The sequencer calls set_tone() at ~1ms tick rate to produce sound.
 * Backends report their capabilities so the sequencer can adapt behavior
 * (e.g., skip waveform selection for M300, skip filter for PWM).
 *
 * Implementations: SDLBackend (desktop), PWMBackend (AD5M), M300Backend (Klipper)
 */
class SoundBackend {
  public:
    virtual ~SoundBackend() = default;

    /// Called by sequencer at ~1ms tick rate to set current output
    /// @param freq_hz   Frequency in Hz (20-20000)
    /// @param amplitude Volume level 0.0-1.0
    /// @param duty_cycle Duty cycle 0.0-1.0 (for square-ish waveforms)
    virtual void set_tone(float freq_hz, float amplitude, float duty_cycle) = 0;

    /// Stop all sound output immediately
    virtual void silence() = 0;

    /// Whether backend can synthesize different waveform shapes
    virtual bool supports_waveforms() const {
        return false;
    }

    /// Whether backend has real amplitude/volume control
    virtual bool supports_amplitude() const {
        return false;
    }

    /// Whether backend can apply DSP filters (lowpass/highpass)
    virtual bool supports_filter() const {
        return false;
    }

    /// Whether this backend can switch output devices at runtime (ALSA only).
    virtual bool supports_device_selection() const {
        return false;
    }

    /// Whether this backend emits sound through the printer rather than through
    /// host audio hardware, and so is only usable while a Moonraker client is
    /// connected (M300 only). SoundManager drops such a backend when the client
    /// is cleared. A capability query rather than a dynamic_cast to the concrete
    /// backend, because the firmware builds -fno-rtti.
    virtual bool needs_moonraker_client() const {
        return false;
    }

    /// Set the active waveform type (only called if supports_waveforms() is true)
    virtual void set_waveform(Waveform /* w */) {}

    /// Set active filter parameters (only called if supports_filter() is true)
    /// @param type "lowpass" or "highpass"
    /// @param cutoff Filter cutoff frequency in Hz
    virtual void set_filter(const std::string& /* type */, float /* cutoff */) {}

    /// Set a specific voice slot (0-based). Default: slot 0 maps to set_tone().
    virtual void set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) {
        if (slot == 0)
            set_tone(freq_hz, amplitude, duty_cycle);
    }

    /// Set waveform for a specific voice slot. Default: slot 0 maps to set_waveform().
    virtual void set_voice_waveform(int slot, Waveform w) {
        if (slot == 0)
            set_waveform(w);
    }

    /// Silence a specific voice slot. Default: slot 0 maps to silence().
    virtual void silence_voice(int slot) {
        if (slot == 0)
            silence();
    }

    /// Number of independent voice slots. Default: 1 (monophonic).
    virtual int voice_count() const {
        return 1;
    }

    /// Publish a complete note event for a voice. PCM backends (SDL, ALSA) use
    /// this for per-sample rendering. Non-PCM backends ignore it.
    virtual void publish_note(int /*slot*/, const NoteEvent& /*event*/) {}

    /// Whether this backend uses NoteEvent-based rendering (per-sample envelope,
    /// sweep, LFO in the render thread). When true, the sequencer skips per-tick
    /// set_tone/set_filter/set_waveform calls — the callback handles everything.
    virtual bool supports_note_events() const {
        return false;
    }

    /// Minimum tick interval the backend can handle (ms)
    virtual float min_tick_ms() const {
        return 1.0f;
    }

    /// Suspend audio output when no voice is active. The sequencer calls this
    /// when it goes idle so backends that run a continuous render callback
    /// (SDL on Android) stop their device instead of streaming silence — which
    /// keeps the host AudioTrack churning and spamming the log (#1253).
    /// Default: no-op (backends with no always-on device are unaffected).
    virtual void suspend() {}

    /// Resume audio output on the next note. Pairs with suspend(); the sequencer
    /// calls this when transitioning idle -> active.
    virtual void resume() {}

    /// Whether this backend supports direct audio rendering via set_render_source.
    /// Backends with real audio output (SDL, ALSA) return true.
    /// Frequency-only backends (PWM, M300) return false.
    virtual bool supports_render_source() const {
        return false;
    }

    /// Set an external audio render source. When set, the backend's render loop
    /// calls this instead of per-voice waveform generation.
    /// Callback signature: void(float* output_buffer, size_t frame_count, int sample_rate)
    virtual void set_render_source(std::function<void(float*, size_t, int)> /* fn */) {}

    /// Clear the external render source, reverting to per-voice synthesis.
    virtual void clear_render_source() {}
};
