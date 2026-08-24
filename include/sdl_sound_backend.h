// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_DISPLAY_SDL

#include "note_event.h"
#include "sound_backend.h"
#include "sound_synthesis.h"

#include <SDL.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

/// SDL2 audio backend -- generates real waveform audio for desktop simulator
class SDLSoundBackend : public SoundBackend {
  public:
    SDLSoundBackend();
    ~SDLSoundBackend() override;

    // SoundBackend interface
    void set_tone(float freq_hz, float amplitude, float duty_cycle) override;
    void silence() override;
    void set_waveform(Waveform w) override;
    void set_filter(const std::string& type, float cutoff) override;
    bool supports_waveforms() const override {
        return true;
    }
    bool supports_amplitude() const override {
        return true;
    }
    bool supports_filter() const override {
        return true;
    }
    float min_tick_ms() const override {
        return 1.0f;
    }

    /// Initialize SDL audio device. Returns false on failure.
    bool initialize();

    /// Shutdown SDL audio device.
    void shutdown();

    // Device lifecycle — the sequencer suspends when idle so the render
    // callback stops streaming silence (Android AudioTrack churn, #1253).
    void suspend() override;
    void resume() override;

    // Legacy voice interface (non-note-event callers)
    void set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) override;
    void set_voice_waveform(int slot, Waveform w) override;
    void silence_voice(int slot) override;
    int voice_count() const override {
        return MAX_VOICES;
    }

    // NoteEvent interface (primary path — per-sample envelope/sweep/LFO in callback)
    void publish_note(int slot, const NoteEvent& event) override;
    bool supports_note_events() const override {
        return true;
    }

    // Render source for tracker PCM playback
    bool supports_render_source() const override {
        return true;
    }
    void set_render_source(std::function<void(float*, size_t, int)> fn) override;
    void clear_render_source() override;

  private:
    static constexpr int MAX_VOICES = 4;

    static void audio_callback(void* userdata, uint8_t* stream, int len);

    VoiceSlot voice_slots_[MAX_VOICES];

    // Scratch buffer for mixing (sized in initialize())
    std::vector<float> mix_buf_;

    // External render source (tracker PCM playback)
    std::function<void(float*, size_t, int)> render_source_;
    std::mutex render_source_mutex_;

    SDL_AudioDeviceID device_id_ = 0;
    int sample_rate_ = 44100;
    bool initialized_ = false;
};

#endif // HELIX_DISPLAY_SDL
