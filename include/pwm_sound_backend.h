// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sound_backend.h"
#include "sound_theme.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// PWM sysfs backend -- generates tones via hardware PWM on embedded Linux (AD5M)
///
/// Two modes of operation:
/// 1. Tone mode: set_tone() writes frequency/duty to sysfs (SFX, system sounds)
/// 2. PCM mode: render thread calls external render source, modulates PWM duty cycle
///    at 8kHz to reproduce audio waveforms (tracker playback)
///
/// Writes to /sys/class/pwm/pwmchipN/pwmM/{period,duty_cycle,enable}
class PWMSoundBackend : public SoundBackend {
  public:
    /// @param base_path  Override sysfs base path (for testing with temp dirs)
    /// @param chip       pwmchip number (e.g. 0 for pwmchip0)
    /// @param channel    PWM channel number (e.g. 6 for pwm6)
    explicit PWMSoundBackend(const std::string& base_path = "/sys/class/pwm", int chip = 0,
                             int channel = 6);
    ~PWMSoundBackend() override;

    // SoundBackend interface
    void set_tone(float freq_hz, float amplitude, float duty_cycle) override;
    void silence() override;
    void set_waveform(Waveform w) override;

    bool supports_waveforms() const override;
    bool supports_amplitude() const override;
    bool supports_filter() const override;
    float min_tick_ms() const override;

    // PCM render source support (tracker playback via duty cycle modulation)
    //
    // Hardware-verified 2026-08-30 on the AD5M Pro: the buzzer is a resonant
    // piezo with no reconstruction filter, so a duty-modulated carrier
    // demodulates as static, not audio. PWM is tone-only; tracker playback
    // uses the set_voice note fallback. The PCM render machinery below stays
    // for hardware that can demodulate it (documented, tested, dormant here).
    bool supports_render_source() const override {
        return false;
    }
    void set_render_source(std::function<void(float*, size_t, int)> fn) override;
    void clear_render_source() override;

    /// Initialize: verify sysfs paths exist and are writable
    /// @return false if paths don't exist or aren't writable
    bool initialize();

    /// Shutdown: disable PWM output and stop render thread
    void shutdown();

    /// Get the constructed path to the PWM channel directory
    /// e.g. "/sys/class/pwm/pwmchip0/pwm6"
    std::string channel_path() const;

    /// Convert frequency in Hz to period in nanoseconds
    /// @return period_ns = 1e9 / freq_hz, or 0 if freq_hz <= 0
    static uint32_t freq_to_period_ns(float freq_hz);

    /// Shift a frequency into the piezo's loud band [500, 2500] Hz by whole
    /// octaves (preserves pitch class). Below 500 doubles, above 2500 halves,
    /// in-band and <= 0 pass through unchanged. The band was measured on the
    /// AD5M Pro 2026-08-30/31: the transducer is weak under ~300 Hz and
    /// effectively ultrasonic past ~4 kHz. SFX theme tones are in-band and
    /// unaffected.
    static float shift_to_piezo_band(float freq_hz);

    /// Get the base duty cycle ratio for a given waveform type
    /// Square=0.50, Saw=0.25, Triangle=0.35, Sine=0.40
    static float waveform_duty_ratio(Waveform w);

    /// Check if PWM is currently enabled
    bool is_enabled() const;

    /// PCM render sample rate (Hz) — 8 kHz: the piezo's response rolls off
    /// around 3-4 kHz, so rendering faster adds no audible content while
    /// halving the wall-time each duty-cycle write gets
    static constexpr int PCM_SAMPLE_RATE = 8000;

    /// PWM carrier frequency for PCM mode (Hz) — above audible range
    static constexpr int PCM_CARRIER_HZ = 62500;

    /// Frames requested per render-source call — 512 @ 8 kHz = 64 ms per
    /// buffer, amortizing sysfs write overhead without audible lag
    static constexpr int PCM_RENDER_BUFFER_FRAMES = 512;

    /// Consecutive exactly-silent buffers before the render loop parks the
    /// channel (duty 0, enable 0) and drops to a 10 ms poll — 8 @ 64 ms
    /// = ~512 ms of true silence means playback is over, not a quiet passage
    static constexpr int PCM_PARK_SILENT_BUFFERS = 8;

    /// Park poll interval (ns) — how often the parked loop re-checks for a
    /// render source: 10 ms
    static constexpr int64_t PCM_PARK_POLL_NS = 10000000LL;

    /// Max samples to skip in one catch-up step after a scheduling stall —
    /// a larger gap resyncs instead of replaying
    static constexpr int PCM_CATCHUP_MAX_SAMPLES = 2;

    /// Busy-wait budget at each sample deadline (ns) — sleep (TIMER_ABSTIME)
    /// to deadline minus this budget, then spin the final stretch: 20 µs
    /// absorbs hrtimer wake lateness without burning real CPU
    static constexpr int64_t PCM_SPIN_BUDGET_NS = 20000LL;

    /// Audio frames elapsed in one park poll interval: PCM_PARK_POLL_NS at
    /// PCM_SAMPLE_RATE (10 ms @ 8 kHz = 80 frames)
    static int64_t park_probe_frames();

    /// Whole samples the render clock is late for sample_index's deadline —
    /// floor((now_ns - (base_ns + sample_index*interval_ns)) / interval_ns).
    /// Negative when ahead of schedule; floors (not truncates) so fractional
    /// lateness in either direction counts as a full sample.
    static int64_t samples_behind(int64_t now_ns, int64_t base_ns, int64_t sample_index,
                                  int64_t interval_ns);

    /// Sample index due at now_ns, clamped to 0 — the index to resume from
    /// after a stall, floor((now_ns - base_ns) / interval_ns)
    static int64_t resync_sample_index(int64_t now_ns, int64_t base_ns, int64_t interval_ns);

  private:
    friend class PWMSoundBackendTestAccess;

    void start_render_thread();
    void stop_render_thread();
    void render_loop();

    /// Demote the render thread to SCHED_IDLE (plus 1 ns timerslack) so its
    /// pacing can never starve klippy or the UI, no matter how it schedules
    void apply_render_thread_priority();

    /// Park the channel after sustained digital silence: duty 0 + enable 0,
    /// then the caller drops to the cheap poll. Leaves enabled_ and
    /// in_pcm_mode_ untouched — silence()'s render_running_ guard and
    /// set_tone's first-tone branch key off them.
    void park_output();

    /// Leave the parked state: duty 0 + enable 1 (period was already set by
    /// enter_pcm_mode; re-writing duty keeps the enable transition pop-free)
    void resume_output();

    /// Write channel_ to <base>/pwmchip<N>/export so the kernel materializes
    /// the channel directory. Returns false only when the chip directory is
    /// missing; a failed write is not fatal here — the caller re-checks the
    /// channel directory, which is the authority on whether it worked.
    bool try_export_channel();

    /// Switch PWM to PCM carrier frequency (fixed period, variable duty)
    void enter_pcm_mode();

    /// Switch PWM back to tone mode (variable period for frequency control)
    void exit_pcm_mode();

    std::string base_path_;
    int chip_;
    int channel_;
    bool enabled_ = false;
    bool initialized_ = false;
    Waveform current_wave_ = Waveform::SQUARE;

    // Tone dedup (mirrors M300SoundBackend::last_freq_): the sequencer
    // re-sends the same note every tick, so a held tone would otherwise
    // rewrite sysfs hundreds of times per second. Keyed on the values that
    // are actually written (period + duty); cleared whenever the channel
    // goes disabled.
    uint32_t last_tone_period_ns_ = 0;
    uint32_t last_tone_duty_ns_ = 0;

    // Tone sysfs write batches emitted by set_tone (tests)
    std::atomic<uint64_t> tone_write_count_{0};

    // PCM render state
    std::function<void(float*, size_t, int)> render_source_;
    std::mutex render_source_mutex_;
    std::thread render_thread_;
    std::atomic<bool> render_running_{false};
    bool in_pcm_mode_ = false;

    // Park/catch-up state — written on the render thread, read by tests
    std::atomic<bool> parked_{false};
    std::atomic<int> silent_buffer_run_{0};
    std::atomic<uint64_t> duty_write_count_{0};
    std::atomic<int> applied_sched_policy_{-1};
    int park_silent_buffers_ = PCM_PARK_SILENT_BUFFERS;

    // Clock/sleep seams — injected by tests before the render thread starts,
    // otherwise defaulted by render_loop() itself
    std::function<int64_t()> now_fn_;
    std::function<void(int64_t)> wait_until_fn_;

    // Pre-opened file descriptors for fast sysfs writes in render loop
    int fd_duty_ = -1;
    int fd_period_ = -1;
    int fd_enable_ = -1;

    // Track last period for glitch-free tone transitions
    uint32_t last_period_ns_ = 0;

    // Render buffer (PCM_RENDER_BUFFER_FRAMES per source call) and the
    // smaller probe buffer the parked loop polls with
    std::vector<float> render_buf_;
    std::vector<float> park_probe_buf_;
};
