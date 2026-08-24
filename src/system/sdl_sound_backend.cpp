// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_DISPLAY_SDL

#include "sdl_sound_backend.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

SDLSoundBackend::SDLSoundBackend() = default;

SDLSoundBackend::~SDLSoundBackend() {
    shutdown();
}

bool SDLSoundBackend::initialize() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        spdlog::error("[SDLSound] SDL_InitSubSystem(AUDIO) failed: {}", SDL_GetError());
        return false;
    }
    // The audio device itself is opened lazily by resume() and closed by
    // suspend(). Holding an AudioTrack open at idle makes Android's audio
    // framework log a steady stream of PlayerBase::stop() lines (#1253) and
    // burns CPU on every platform writing silence. Pausing is NOT enough — a
    // paused AudioTrack still churns the log — so the device must be fully
    // closed at idle. The sequencer drives resume/suspend at the idle<->active
    // transition.
    initialized_ = true;

    // Report which SDL audio driver is backing the subsystem. The name is the
    // only visible signal that the dummy driver is in use — a desktop run
    // under SDL_VIDEODRIVER=dummy silently forces it via main()'s
    // silence_audio_if_headless(), and without this line the log would still
    // say "Audio initialized" right up until the user notices nothing is
    // coming out of the speakers.
    const char* driver_name = SDL_GetCurrentAudioDriver();
    spdlog::info("[SDLSound] Audio subsystem ready (device opens on first sound): driver '{}'",
                 driver_name ? driver_name : "(unknown)");
    return true;
}

void SDLSoundBackend::shutdown() {
    if (!initialized_)
        return;
    if (device_id_) {
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    // Match the SDL_InitSubSystem(SDL_INIT_AUDIO) in initialize(). Without this
    // the audio subsystem stays up after shutdown() claims to have torn it down,
    // and is only reaped later by the SDL_Quit() in lv_sdl_quit() — during
    // *display* teardown, an unrelated subsystem's shutdown path. Closing the
    // device does not stop the backend's own threads (SDL's PulseAudio backend
    // keeps a mainloop thread), so leaving the subsystem initialized leaves
    // those threads running against state the device close already freed.
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    initialized_ = false;
    spdlog::info("[SDLSound] Audio shutdown");
}

void SDLSoundBackend::suspend() {
    if (!initialized_ || device_id_ == 0)
        return;
    // Close the device entirely. Pausing is not enough on Android: a paused
    // AudioTrack still makes the framework spam PlayerBase::stop() (#1253).
    // Only a closed device (no AudioTrack at all) silences the log and stops
    // the idle render thread. The sequencer reopens it on the next sound.
    SDL_CloseAudioDevice(device_id_);
    device_id_ = 0;
    spdlog::debug("[SDLSound] Audio device closed (idle)");
}

void SDLSoundBackend::resume() {
    if (!initialized_ || device_id_ != 0)
        return; // already open

    SDL_AudioSpec desired{};
    desired.freq = sample_rate_;
    desired.format = AUDIO_F32SYS;
    desired.channels = 1;
    desired.samples = 64; // Very low latency buffer — keeps callback period ~1.5ms
    desired.callback = audio_callback;
    desired.userdata = this;

    SDL_AudioSpec obtained{};
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (device_id_ == 0) {
        spdlog::warn("[SDLSound] Lazy SDL_OpenAudioDevice failed: {}", SDL_GetError());
        return;
    }

    sample_rate_ = obtained.freq;
    // Resize the mix buffer BEFORE unpausing — SDL's audio thread starts
    // calling audio_callback the instant playback unpauses, and the callback
    // memsets mix_buf_.data(). If the callback fires before the resize,
    // data() is nullptr and the memset segfaults the audio thread.
    mix_buf_.resize(obtained.samples);
    SDL_PauseAudioDevice(device_id_, 0); // Start playback
    spdlog::debug("[SDLSound] Audio device opened ({} Hz, {} samples)", sample_rate_,
                  obtained.samples);
}

// ============================================================================
// SoundBackend interface — write through to voice_slots_[0] or all slots
// ============================================================================

void SDLSoundBackend::set_tone(float freq_hz, float amplitude, float duty_cycle) {
    set_voice(0, freq_hz, amplitude, duty_cycle);
}

void SDLSoundBackend::silence() {
    for (int v = 0; v < MAX_VOICES; ++v) {
        voice_slots_[v].event.velocity = 0;
        voice_slots_[v].generation.fetch_add(1, std::memory_order_release);
    }
}

void SDLSoundBackend::set_waveform(Waveform w) {
    set_voice_waveform(0, w);
}

// Legacy voice interface: writes individual fields and bumps generation so the
// audio callback picks up the change atomically on the next generation check.
void SDLSoundBackend::set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) {
    if (slot < 0 || slot >= MAX_VOICES)
        return;
    auto& s = voice_slots_[slot];
    s.event.freq_hz = freq_hz;
    s.event.velocity = amplitude;
    s.event.duty_cycle = duty_cycle;
    s.generation.fetch_add(1, std::memory_order_release);
}

void SDLSoundBackend::set_voice_waveform(int slot, Waveform w) {
    if (slot < 0 || slot >= MAX_VOICES)
        return;
    // No generation bump — waveform alone doesn't restart the note.
    voice_slots_[slot].event.wave = w;
}

void SDLSoundBackend::silence_voice(int slot) {
    if (slot < 0 || slot >= MAX_VOICES)
        return;
    voice_slots_[slot].event.velocity = 0;
    voice_slots_[slot].generation.fetch_add(1, std::memory_order_release);
}

void SDLSoundBackend::set_filter(const std::string& type, float cutoff) {
    // Legacy path: sets filter on voice 0 for backward compat.
    // NoteEvent callers embed filter params directly in the NoteEvent.
    auto& ev = voice_slots_[0].event;
    if (type.empty()) {
        ev.filter_type = 0;
    } else if (type == "lowpass") {
        ev.filter_type = 1;
        ev.filter_cutoff = cutoff;
    } else if (type == "highpass") {
        ev.filter_type = 2;
        ev.filter_cutoff = cutoff;
    }
    // No generation bump — filter change takes effect on next note start.
}

// Primary note-event path: publish a complete NoteEvent, then bump generation.
// The audio callback snapshots event → active on the next callback, ensuring all
// parameters (freq, envelope, sweep, LFO, filter) are seen as a unit.
void SDLSoundBackend::publish_note(int slot, const NoteEvent& event) {
    if (slot < 0 || slot >= MAX_VOICES)
        return;
    voice_slots_[slot].event = event;
    voice_slots_[slot].generation.fetch_add(1, std::memory_order_release);
}

void SDLSoundBackend::set_render_source(std::function<void(float*, size_t, int)> fn) {
    std::lock_guard<std::mutex> lock(render_source_mutex_);
    render_source_ = std::move(fn);
}

void SDLSoundBackend::clear_render_source() {
    std::lock_guard<std::mutex> lock(render_source_mutex_);
    render_source_ = nullptr;
}

// ============================================================================
// Audio callback (runs in SDL audio thread)
// ============================================================================

void SDLSoundBackend::audio_callback(void* userdata, uint8_t* stream, int len) {
    auto* self = static_cast<SDLSoundBackend*>(userdata);
    auto* out = reinterpret_cast<float*>(stream);
    int num_samples = len / static_cast<int>(sizeof(float));

    auto* mix = self->mix_buf_.data();
    std::memset(mix, 0, num_samples * sizeof(float));
    bool has_audio = false;

    // Render tracker PCM if active
    {
        std::function<void(float*, size_t, int)> source;
        {
            std::lock_guard<std::mutex> lock(self->render_source_mutex_);
            source = self->render_source_;
        }
        if (source) {
            source(mix, static_cast<size_t>(num_samples), self->sample_rate_);
            has_audio = true;
        }
    }

    // Mix synth voices using VoiceSlot per-sample rendering
    float sr = static_cast<float>(self->sample_rate_);
    for (int v = 0; v < MAX_VOICES; ++v) {
        auto& slot = self->voice_slots_[v];

        // Detect new note: snapshot all event params atomically on generation change.
        // This guarantees freq/envelope/sweep/LFO are always read from the same publish.
        uint32_t gen = slot.generation.load(std::memory_order_acquire);
        if (gen != slot.cb_generation) {
            slot.cb_generation = gen;
            slot.reset_for_new_note();
            // reset_for_new_note() calls compute_biquad_coeffs with sample_rate=0;
            // fix up with the real sample rate if a filter is active.
            if (slot.active.filter_type != 0) {
                auto ft = (slot.active.filter_type == 1) ? helix::audio::FilterType::LOWPASS
                                                         : helix::audio::FilterType::HIGHPASS;
                helix::audio::compute_biquad_coeffs(slot.filter, ft, slot.active.filter_cutoff, sr);
            }
        }

        // Skip silent voices (both currently playing and envelope tail)
        if (slot.active.velocity <= 0.001f && slot.current_amplitude <= 0.001f) {
            continue;
        }

        // Render per-sample into mix — render_sample() advances phase, elapsed_samples,
        // and current_amplitude internally.
        for (int i = 0; i < num_samples; ++i) {
            mix[i] += slot.render_sample(sr);
        }
        has_audio = true;
    }

    if (!has_audio) {
        std::memset(stream, 0, static_cast<size_t>(len));
        return;
    }

    // Clamp to prevent inter-voice accumulation from overdriving
    for (int i = 0; i < num_samples; ++i)
        mix[i] = std::clamp(mix[i], -1.0f, 1.0f);

    std::memcpy(out, mix, num_samples * sizeof(float));
}

#endif // HELIX_DISPLAY_SDL
