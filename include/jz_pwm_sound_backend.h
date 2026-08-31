// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sound_backend.h"

#include <cstdint>
#include <string>
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
    /// Floor for one buffer's play time. Theme steps as written are often
    /// 6-80 ms ticks, which this transducer cannot reproduce duty-encoded
    /// at any volume - everything measured audible on the rig was a
    /// second-plus. Short steps render at their true duration and then
    /// TILE that content out to this floor, so a click becomes its tone
    /// repeated, not a longer envelope.
    long min_note_ms = 200;
    /// Cap one buffer's hold time: cumulative DMA loop time in the tens
    /// of seconds wedges the vendor driver's teardown path (measured on
    /// the rig; recovery is a reboot).
    long max_note_ms = 2500;
    /// Duty swing around 50%. The piezo demodulates this; +-40% was
    /// ear-tuned on the rig (a chord rendered this way is recognizable).
    double duty_swing = 0.4;
};

/// Pure renderer: mix the NoteEvents (one per chord voice) into one
/// duty-encoded period-word buffer, reusing VoiceSlot's per-sample
/// synthesis (ADSR, sweep, LFO, waveform). Exposed for unit tests.
std::vector<uint32_t> jz_pwm_render_step(const NoteEvent* events, int n_events,
                                         const JzPwmRenderParams& params);

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
    float min_tick_ms() const override;

  private:
    void flush_step();
    void stop_child();

    std::string fx_pwm_;
    NoteEvent pending_[4];
    int received_ = 0;
    pid_t child_ = -1;
    JzPwmRenderParams params_;
};
