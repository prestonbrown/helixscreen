// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "note_event.h"
#include "sound_backend.h"
#include "sound_synthesis.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

/// PCM audio through an external player process (aplay) fed over a pipe.
///
/// For boards whose binary cannot link or dlopen libasound (the static musl
/// MIPS build) but whose userland ships a working `aplay` and an ALSA card
/// (Creality K1 series). Synthesis is the same in-process VoiceSlot pipeline
/// the SDL/ALSA backends run; only the sink differs: a child `aplay` reading
/// raw S16_LE mono from stdin.
///
/// Process model: one child per sound burst, not per note. resume() spawns the
/// player (or reuses one still alive from the idle grace), suspend() lets the
/// render thread feed silence for kIdleGraceMs and then closes the pipe, so
/// the player drains and exits. A player that fails kMaxConsecutiveFailures
/// times in a row disables the backend for the rest of the run.
class PcmPipeSoundBackend : public SoundBackend {
  public:
    static constexpr int kSampleRate = 44100;
    static constexpr size_t kFramesPerChunk = 512;
    /// How far the render clock runs ahead of wall time. Bounds the audio
    /// buffered in the pipe and the player, so a note published mid-sound
    /// lands this late at most instead of a pipe-buffer-full late.
    static constexpr int kLeadMs = 60;
    /// The player's ALSA buffer (and start threshold), slightly above the lead.
    static constexpr int kPlayerBufferUs = 80000;
    /// Keep the player alive this long after the last sound: rapid UI taps
    /// then cost no process spawn.
    static constexpr int kIdleGraceMs = 1500;
    static constexpr int kMaxConsecutiveFailures = 3;
    static constexpr const char* kPlayerEnv = "HELIX_PCM_PLAYER";

    PcmPipeSoundBackend();
    ~PcmPipeSoundBackend() override;

    /// The player binary for @p env_override (the HELIX_PCM_PLAYER value, or
    /// null): "0"/"off" disables, a path names the binary (empty if it is not
    /// executable), unset falls back to the first executable in @p candidates.
    static std::string find_player(const char* env_override,
                                   const std::vector<std::string>& candidates = default_players());
    static std::vector<std::string> default_players();

    /// A playback PCM node (pcmC<n>D<m>p) exists under @p snd_dir.
    static bool has_playback_device(const std::string& snd_dir);

    /// The argv the player is started with. Exposed for tests.
    static std::vector<std::string> player_args(const std::string& player);

    /// find_player(getenv(kPlayerEnv)) is non-empty and /dev/snd has a playback node.
    static bool available();

    /// Start the render thread. The player is spawned lazily by resume().
    bool initialize(const std::string& player_path);
    void shutdown();

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
    void set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) override;
    void set_voice_waveform(int slot, Waveform w) override;
    void silence_voice(int slot) override;
    int voice_count() const override {
        return kMaxVoices;
    }
    void publish_note(int slot, const NoteEvent& event) override;
    bool supports_note_events() const override {
        return true;
    }
    bool supports_render_source() const override {
        return true;
    }
    void set_render_source(std::function<void(float*, size_t, int)> fn) override;
    void clear_render_source() override;
    void suspend() override;
    void resume() override;

    /// Test observability.
    bool is_parked() const {
        return parked_.load(std::memory_order_acquire);
    }
    bool player_disabled() const {
        return disabled_.load(std::memory_order_acquire);
    }
    int spawn_count() const {
        return spawn_count_.load(std::memory_order_acquire);
    }
    void set_idle_grace_ms(int ms) {
        idle_grace_ms_ = ms;
    }

  private:
    void render_loop();
    bool spawn_player();
    /// Close the pipe (EOF drains the player) and reap it; SIGKILL after a bounded wait.
    void stop_player();
    bool write_all(const void* buf, size_t len);
    void render_chunk(std::vector<float>& mix, std::vector<int16_t>& out);
    void ack_pass();

    std::string player_;
    int pipe_wr_ = -1;
    pid_t player_pid_ = -1;
    int idle_grace_ms_ = kIdleGraceMs;

    std::thread render_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> disabled_{false};
    std::atomic<bool> parked_{false};
    std::atomic<int> spawn_count_{0};
    int consecutive_failures_ = 0;

    // Sequencer <-> render thread handoff, same shape as the ALSA backend:
    // active_ flips under state_mutex_, resume() waits for one completed pass.
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool active_ = false;
    bool suspend_requested_ = false;
    std::chrono::steady_clock::time_point suspend_at_{};
    std::atomic<uint32_t> resume_seq_{0};
    std::atomic<uint32_t> resume_acked_{0};
    std::mutex ack_mutex_;
    std::condition_variable ack_cv_;

    static constexpr int kMaxVoices = 4;
    VoiceSlot voice_slots_[kMaxVoices];

    std::function<void(float*, size_t, int)> render_source_;
    std::mutex render_source_mutex_;

    // Shared filter for the external render source (tracker); voices filter per slot.
    std::atomic<helix::audio::FilterType> filter_type_{helix::audio::FilterType::NONE};
    std::atomic<float> filter_cutoff_{20000.0f};
    helix::audio::BiquadFilter filter_;
};
