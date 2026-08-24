// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef HELIX_HAS_ALSA

#include "note_event.h"
#include "sound_backend.h"
#include "sound_synthesis.h"

#include <alsa/asoundlib.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// ALSA PCM audio backend — real waveform synthesis for Linux SBCs
/// Uses VoiceSlot for atomic note publishing and per-sample rendering.
class ALSASoundBackend : public SoundBackend {
  public:
    ALSASoundBackend();
    ~ALSASoundBackend() override;

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

    /// Initialize the named ALSA PCM device. Returns false if it can't be opened.
    bool initialize(const std::string& device);

    bool supports_device_selection() const override {
        return true;
    }

    /// Stop render thread and close ALSA device.
    void shutdown();

    /// Duplicate mono buffer to interleaved stereo (L=R). Public for testability.
    static void mono_to_stereo(const float* mono, float* stereo, size_t frame_count);

    /// Convert float [-1,1] samples to int16 with clamping. Public for testability.
    static void float_to_s16(const float* src, int16_t* dst, size_t sample_count);

    /// Whether a parking stream must be explicitly started before draining.
    ///
    /// snd_pcm_drain() on a PREPARED stream returns immediately without playing
    /// anything. A sound too short to reach the start threshold
    /// (buffer_size - period_size) therefore leaves its audio queued but never
    /// played, and the snd_pcm_prepare() on the next resume discards it - so the
    /// sound is silently swallowed. That threshold is derived from the
    /// NEGOTIATED buffer, which on real hardware can be far larger than the
    /// 2048 frames we ask for (1024/8192 observed, i.e. 162.5 ms at 44.1 kHz),
    /// putting most short UI sounds below it. Starting the stream first lets
    /// drain play the tail out.
    static bool needs_start_before_drain(snd_pcm_state_t state, bool wrote_since_prepare);

    // Voice interface (legacy — used by non-note-event callers)
    void set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) override;
    void set_voice_waveform(int slot, Waveform w) override;
    void silence_voice(int slot) override;
    int voice_count() const override {
        return MAX_VOICES;
    }

    // NoteEvent interface (primary path for synth sounds)
    void publish_note(int slot, const NoteEvent& event) override;
    bool supports_note_events() const override {
        return true;
    }

    // Render source for direct audio generation (tracker PCM playback)
    bool supports_render_source() const override {
        return true;
    }
    void set_render_source(std::function<void(float*, size_t, int)> fn) override;
    void clear_render_source() override;

    /// Park the render thread instead of writing silence to the card.
    ///
    /// SoundSequencer already calls these when its queue goes idle, but the
    /// base-class versions are no-ops, so on ALSA the render thread kept
    /// feeding the device forever: at period_size frames per write that is a
    /// wakeup every few milliseconds, permanently, with nothing playing. On a
    /// printer host that shares a CPU with Klipper, idle wakeups are not free —
    /// they are the same class of problem as idle memory.
    ///
    /// Every snd_pcm_* call stays on the render thread, which is what makes
    /// suspend() safe to call from the sequencer while the render thread may
    /// be inside snd_pcm_writei. Parking drains the device rather than
    /// dropping it, so a sound's tail — still sitting in the hardware buffer
    /// when the sequencer's clock says the sound is over — plays out instead
    /// of being discarded.
    ///
    /// resume() is a synchronous handoff: it does not return until the render
    /// thread has completed a full render pass (or the handoff times out /
    /// the thread is gone). The sequencer starts its step clock the instant
    /// resume() returns; when resume() was fire-and-forget, notes published
    /// in the gap before the thread woke were overwritten un-rendered —
    /// short sounds never played and longer ones lost their beginning
    /// (v0.99.114 field regression, Raspberry Pi). This mirrors
    /// SDLSoundBackend::resume(), which opens and unpauses the device on the
    /// caller's thread for the same reason.
    void suspend() override;
    void resume() override;

  private:
    void render_loop();
    snd_pcm_sframes_t recover_xrun(snd_pcm_sframes_t err);

    snd_pcm_t* pcm_ = nullptr;
    std::thread render_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> suspended_{false};
    std::mutex suspend_mutex_;
    std::condition_variable suspend_cv_;

    // Resume handoff: resume() bumps resume_seq_; the render thread acks the
    // newest sequence at the bottom of each completed pass. Sequence numbers
    // rather than a bool because a resume can arrive before the thread even
    // noticed the suspend — then no wake is needed, and the next completed
    // pass is the handoff either way.
    std::atomic<uint32_t> resume_seq_{0};
    std::atomic<uint32_t> resume_acked_{0};
    std::mutex ack_mutex_;
    std::condition_variable ack_cv_;

    static constexpr int MAX_VOICES = 4;

    VoiceSlot voice_slots_[MAX_VOICES];

    // Scratch buffer for mixing (sized in initialize())
    std::vector<float> mix_buf_;

    // External render source (tracker PCM playback)
    std::function<void(float*, size_t, int)> render_source_;
    std::mutex render_source_mutex_;

    // Filter for external render source (tracker) — shared, not per-voice
    std::atomic<helix::audio::FilterType> filter_type_{helix::audio::FilterType::NONE};
    std::atomic<float> filter_cutoff_{20000.0f};
    helix::audio::BiquadFilter filter_;

    // Audio format negotiated during initialize()
    unsigned int sample_rate_ = 44100;
    snd_pcm_uframes_t period_size_ = 256;
    unsigned int channels_ = 1;
    bool use_s16_ = false;
    /// Render-thread only: has anything been written since the last prepare()?
    /// Paired with needs_start_before_drain() to tell "queued but never started"
    /// apart from "nothing to play".
    bool wrote_since_prepare_ = false;
    /// Output re-locks when the clock stops, so the idle park must not run here
    /// (#1337). Decided once at initialize(); see alsa_clock_keepalive.h.
    bool keep_clock_alive_ = false;

    // Underrun log rate limiter (touched only by render thread)
    uint64_t xrun_count_ = 0;
    std::chrono::steady_clock::time_point last_xrun_log_{};
};

#endif // HELIX_HAS_ALSA
