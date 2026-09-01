// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sound_backend.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/// Render parameters for duty-encoded one-shot buffers (rig-measured).
struct JzPwmRenderParams {
    /// The DMA engine's step rate. Two-point tuner calibration on the rig
    /// (2026-08-31): commanded 700 Hz read 3234 and 1400 Hz read 6469
    /// while fx-pwm computed halves at 500 MHz / 6 - the waveform steps
    /// at 385 MHz and the channel's prescale register never reaches it.
    double clock_hz = 385000000.0;
    /// Carrier: one 16-bit-halves period word per cycle. 32768 Hz puts
    /// the carrier above the piezo's response and divides 4096-word
    /// buffers into phase-continuous loops for 8 Hz-multiple notes.
    double carrier_hz = 32768.0;
    /// Driver rule: word counts must be a multiple of 4.
    size_t word_align = 4;
    /// Floor for one buffer's play time, MEASURED on the rig: 30 cycles
    /// of a 10 ms duty-encoded tone were each clearly audible, and the
    /// listener judged 10 ms the bound before static. Short theme steps
    /// (6 ms ticks) tile out to just this much. The original 200 ms
    /// guess was an order of magnitude conservative.
    long min_note_ms = 10;
    /// Cap one buffer's hold time: cumulative DMA loop time in the tens
    /// of seconds wedges the vendor driver's teardown path (measured on
    /// the rig; recovery is a reboot).
    long max_note_ms = 2500;
    /// Duty swing around 50%. The piezo demodulates this; +-40% was
    /// ear-tuned on the rig (a chord rendered this way is recognizable).
    double duty_swing = 0.4;
    /// Multiplier on the normalization target (0..1). The renderer
    /// peak-normalizes each buffer; scaling the target instead of the
    /// input is what lets a volume setting survive normalization - a
    /// quieter buffer normalizes to a quieter peak, not to the same one.
    double swing_scale = 1.0;
};

/// When the voice path may render: only at the START of an emission burst
/// (ms_since_call >= burst_gap - the slots hold one complete tracker tick,
/// never a half-updated mix), and at most once per flush_interval_ms.
/// Pure, for unit tests.
bool jz_voice_should_flush(bool dirty, double ms_since_flush, double ms_since_call,
                           double flush_interval_ms, double burst_gap_ms);

/// Pure renderer: mix the NoteEvents (one per chord voice) into one
/// duty-encoded period-word buffer, reusing VoiceSlot's per-sample
/// synthesis (ADSR, sweep, LFO, waveform). Exposed for unit tests.
std::vector<uint32_t> jz_pwm_render_step(const NoteEvent* events, int n_events,
                                         const JzPwmRenderParams& params);

/// Live voice-path tuning state. The piezo lab modal adjusts these while
/// the tracker loops; the same struct carries the HELIX_JZ_* env knobs.
struct JzPwmVoiceKnobs {
    float buf_ms = 1900.0f;  // phrase length (one daemon buffer)
    float flush_ms = 120.0f; // unused by the phrase path (kept for env compat)
    float attack_ms = 2.0f;
    float release_ms = 10.0f;
    int wave = 0;         // 0=triangle 1=square 2=saw
    int octave_shift = 0; // global register shift (piezo band is ~1-4 kHz)
    float swing = 0.4f;   // duty depth around 50%
    /// Carrier. The driver copies buffer words at ~30 us each (measured),
    /// so upload time scales with words-per-second = carrier: at 32768 Hz
    /// upload ~= playback (50% duty ceiling); 8192 Hz buys ~80% duty at
    /// the cost of an audible-band carrier whine. Period = clock/carrier
    /// must keep both 16-bit halves positive: carrier >= ~3 kHz.
    float carrier_hz = 32768.0f;
};

/// The app constructs at most one JzPwmSoundBackend; these address that
/// live instance. get returns false when no piezo backend exists, which
/// is how lab reachability is gated.
bool jz_pwm_get_voice_knobs(JzPwmVoiceKnobs* out);
void jz_pwm_set_voice_knobs(const JzPwmVoiceKnobs& knobs);

/**
 * @brief AD5X piezo via the jz_pwm DMA engine, as duty-encoded one-shot
 * buffers exec'd through fx-pwm.
 *
 * The buzzer hangs off the Ingenic PWM2 DMA engine, which replays a
 * buffer of period words and refuses buffer updates while a loop runs -
 * so audio here is one complete buffer per theme step, never a stream.
 * Rendering happens in-process (the full NoteEvent pipeline); the words
 * go to fx-pwm(1) via a file, keeping the wedge-prone ioctl lifecycle
 * in a child process: a driver wedge then costs one sound, not the UI.
 * A rig session accumulated thirteen wedged fx-pwm children with zero
 * impact on the printer - that separation is the design.
 *
 * Chords: the sequencer publishes one NoteEvent per voice slot for each
 * step (voice_count() = 4); the buffer renders when the last slot lands.
 */
class JzPwmSoundBackend : public SoundBackend {
  public:
    /// /dev/jz_pwm exists and fx-pwm is executable (chroot or host path).
    static bool available();

    JzPwmSoundBackend();
    ~JzPwmSoundBackend() override;

    bool initialize();

    /// SoundBackend
    void set_tone(float freq_hz, float amplitude, float duty_cycle) override;
    void silence() override;
    bool supports_note_events() const override {
        return true;
    }
    int voice_count() const override {
        return 4;
    }
    void publish_note(int slot, const NoteEvent& event) override;
    /// Tick-path voices: the tracker's PC-speaker mode drives all four
    /// channels per row through these. A burst's updates coalesce into
    /// one chord buffer per flush interval, rendered only at burst starts
    /// (see jz_voice_should_flush).
    void set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) override;
    void silence_voice(int slot) override;
    float min_tick_ms() const override;

    /// Live tuning state (the throwaway piezo-lab modal drives these).
    JzPwmVoiceKnobs voice_knobs() const;
    void set_voice_knobs(const JzPwmVoiceKnobs& knobs);

  private:
    void flush_step();
    /// Hand a rendered buffer to the sound daemon (fx-pwm serve) over its
    /// socket, from the sender worker. Returns the daemon's ack:
    /// 0 played, 1 dropped (busy), -1 no daemon / send failed.
    int send_to_daemon(const std::vector<uint32_t>& words, uint32_t hold_ms);
    /// The only socket writer. Takes the newest pending frame (or cancel
    /// marker) and sends it; requeues on busy.
    void sender_worker();
    /// UI/sequencer-thread entry: stash the latest frame (latest wins) and
    /// wake the worker. Never blocks on the socket.
    void enqueue_frame(std::vector<uint32_t>&& words, uint32_t hold_ms);
    /// Connect to the daemon; if absent, fork+exec `fx-pwm serve` once and
    /// retry. Idempotent.
    bool ensure_daemon();
    /// Burst-start phrase assembly: append the current slot state to the
    /// row history (deduped), and when the history spans a phrase, render
    /// and send it.
    void assemble_phrase(std::chrono::steady_clock::time_point now);

    std::string fx_pwm_;
    NoteEvent pending_[4];
    int received_ = 0;
    JzPwmRenderParams params_;
    JzPwmVoiceKnobs knobs_;
    float voice_freq_[4] = {0, 0, 0, 0};
    float voice_amp_[4] = {0, 0, 0, 0};
    bool voices_dirty_ = false;
    std::chrono::steady_clock::time_point last_voice_call_;
    /// Rolling row history for phrase assembly. Entries are appended at
    /// burst starts only when the 4-slot state actually changed.
    struct HistoryEntry {
        std::chrono::steady_clock::time_point t;
        float freq[4];
        float amp[4];
        double ms = 0; // duration to the NEXT entry (patched on append)
    };
    std::vector<HistoryEntry> history_;
    bool history_seeded_ = false;
    float history_back_freq_[4] = {-1, -1, -1, -1};
    float history_back_amp_[4] = {-1, -1, -1, -1};
    /// Sound daemon socket + pid (lazy-started on first sound).
    int sock_ = -1;
    pid_t daemon_pid_ = -1;
    /// Sender worker: the ONLY thread that touches the socket. The UI and
    /// sequencer threads hand over frames and return immediately - a
    /// 248 KB body write can block for seconds (the daemon cannot drain
    /// while inside its copy ioctl) and that must never stall the screen.
    struct PendingFrame {
        std::vector<uint32_t> words;
        uint32_t hold_ms = 0;
    };
    std::optional<PendingFrame> pending_frame_;
    bool worker_exit_ = false;
    std::thread worker_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    /// Earliest send time after the last phrase (its hold + upload margin):
    /// phrases queue in the history instead of colliding as busy-drops.
    std::chrono::steady_clock::time_point next_send_time_{};
    /// HELIX_JZ_LOG_VOICE=1 appends the voice stream to /tmp/jz-voice.log.
    FILE* voice_log_ = nullptr;
    std::chrono::steady_clock::time_point voice_log_epoch_;
};

/// Build the render events for a tick-path voice snapshot: sounding
/// voices become sustained notes of `dur_ms` with the given envelope and
/// wave shape, silent slots are dropped (the renderer normalizes by
/// sounding count), and octave_shift moves the whole chord's register.
/// Pure, for unit tests.
std::vector<NoteEvent> jz_pwm_voices_to_events(const float* freq, const float* amp, int n_voices,
                                               float dur_ms, float attack_ms, float release_ms,
                                               Waveform wave, int octave_shift);

/// One tracker row as the phrase renderer sees it: the four slot states
/// and how long that state holds.
struct JzPhraseRow {
    float freq[4] = {0, 0, 0, 0};
    float amp[4] = {0, 0, 0, 0};
    float ms = 0;
};

/// Render a sequence of rows into ONE duty-word phrase buffer. Voices are
/// phase-continuous across row boundaries (note changes slur mid-phrase
/// instead of re-attacking), with the envelope applied once at the phrase
/// edges. Capped at 65536 words (the driver buffer limit = 2 s at the
/// 32768 Hz carrier). Pure, for unit tests.
std::vector<uint32_t> jz_pwm_render_phrase(const JzPhraseRow* rows, int n_rows,
                                           const JzPwmRenderParams& p, const JzPwmVoiceKnobs& k);

/// Truncate rows to a phrase time budget (ms), merging nothing: the last
/// row is clipped, later rows are dropped. Returns the row count kept.
/// Pure, for unit tests.
int jz_phrase_clip(JzPhraseRow* rows, int n_rows, float budget_ms);
