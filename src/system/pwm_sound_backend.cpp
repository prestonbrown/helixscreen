// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pwm_sound_backend.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// Floor division for signed int64. C++ '/' truncates toward zero, but sample
// arithmetic needs floor so a partial-sample deficit counts as a full sample
// late (and a partial lead as a full sample early).
static int64_t floor_div_i64(int64_t n, int64_t d) {
    int64_t q = n / d;
    int64_t r = n % d;
    return (r != 0 && ((r < 0) != (d < 0))) ? q - 1 : q;
}

static struct timespec to_timespec_ns(int64_t ns) {
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ns / 1000000000LL);
    ts.tv_nsec = static_cast<long>(ns % 1000000000LL);
    return ts;
}

static int64_t monotonic_now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/// Default sleep seam: TIMER_ABSTIME sleep to deadline minus the spin
/// budget, then spin the final stretch — absorbs hrtimer wake lateness
/// without burning real CPU across the whole sample interval. CLOCK_MONOTONIC
/// is used directly here; the now seam WRAPS this whole function (virtual
/// clock tests replace wait_until_fn_ entirely, so this never runs there).
static void default_wait_until(int64_t deadline_ns, const std::function<int64_t()>& now) {
    const int64_t sleep_target = deadline_ns - PWMSoundBackend::PCM_SPIN_BUDGET_NS;
    if (now() < sleep_target) {
        struct timespec ts = to_timespec_ns(sleep_target);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
            // retry on signal
        }
    }
    while (now() < deadline_ns) {
        // spin — at SCHED_IDLE the scheduler preempts this against any real work
    }
}

PWMSoundBackend::PWMSoundBackend(const std::string& base_path, int chip, int channel)
    : base_path_(base_path), chip_(chip), channel_(channel) {}

PWMSoundBackend::~PWMSoundBackend() {
    shutdown();
}

std::string PWMSoundBackend::channel_path() const {
    return base_path_ + "/pwmchip" + std::to_string(chip_) + "/pwm" + std::to_string(channel_);
}

uint32_t PWMSoundBackend::freq_to_period_ns(float freq_hz) {
    if (freq_hz <= 0.0f) {
        return 0;
    }
    return static_cast<uint32_t>(1e9f / freq_hz);
}

float PWMSoundBackend::waveform_duty_ratio(Waveform w) {
    switch (w) {
    case Waveform::SQUARE:
        return 0.50f;
    case Waveform::SAW:
        return 0.25f;
    case Waveform::TRIANGLE:
        return 0.35f;
    case Waveform::SINE:
        return 0.40f;
    }
    return 0.50f;
}

int64_t PWMSoundBackend::park_probe_frames() {
    return PCM_PARK_POLL_NS * PCM_SAMPLE_RATE / 1000000000LL;
}

int64_t PWMSoundBackend::samples_behind(int64_t now_ns, int64_t base_ns, int64_t sample_index,
                                        int64_t interval_ns) {
    return floor_div_i64(now_ns - (base_ns + sample_index * interval_ns), interval_ns);
}

int64_t PWMSoundBackend::resync_sample_index(int64_t now_ns, int64_t base_ns, int64_t interval_ns) {
    int64_t index = floor_div_i64(now_ns - base_ns, interval_ns);
    return index < 0 ? 0 : index;
}

bool PWMSoundBackend::supports_waveforms() const {
    return false;
}

bool PWMSoundBackend::supports_amplitude() const {
    return true;
}

bool PWMSoundBackend::supports_filter() const {
    return false;
}

float PWMSoundBackend::min_tick_ms() const {
    return 2.0f;
}

bool PWMSoundBackend::is_enabled() const {
    return enabled_;
}

bool PWMSoundBackend::try_export_channel() {
    std::string chip_dir = base_path_ + "/pwmchip" + std::to_string(chip_);
    if (!std::filesystem::exists(chip_dir)) {
        return false;
    }

    std::string export_path = chip_dir + "/export";
    int fd = ::open(export_path.c_str(), O_WRONLY);
    if (fd < 0) {
        spdlog::warn("[PWMSoundBackend] Cannot open {} for export (errno {})", export_path, errno);
        return true;
    }

    std::string channel = std::to_string(channel_);
    ssize_t written = ::write(fd, channel.c_str(), channel.size());
    int write_errno = errno; // captured before close() can touch it
    ::close(fd);

    // EBUSY means the kernel already has this channel exported. Any other
    // failure is worth a log line, but the caller's re-check of the channel
    // directory decides the outcome.
    if (written < 0 && write_errno != EBUSY) {
        spdlog::warn("[PWMSoundBackend] Export write to {} failed (errno {})", export_path,
                     write_errno);
    }
    return true;
}

bool PWMSoundBackend::initialize() {
    std::string path = channel_path();
    if (!std::filesystem::exists(path)) {
#ifdef HELIX_PWM_AUTO_EXPORT
        // The stock AD5M kernel ships the beeper channel unexported: nothing
        // materializes pwm6 until its number is written to pwmchip0/export.
        try_export_channel();
#endif
        if (!std::filesystem::exists(path)) {
            return false;
        }
    }

    // Pre-open file descriptors for fast writes in render loop
    fd_duty_ = ::open((path + "/duty_cycle").c_str(), O_WRONLY);
    fd_period_ = ::open((path + "/period").c_str(), O_WRONLY);
    fd_enable_ = ::open((path + "/enable").c_str(), O_WRONLY);

    if (fd_duty_ < 0 || fd_period_ < 0 || fd_enable_ < 0) {
        spdlog::warn("[PWMSoundBackend] Failed to open sysfs fds, falling back to ofstream");
        if (fd_duty_ >= 0)
            ::close(fd_duty_);
        if (fd_period_ >= 0)
            ::close(fd_period_);
        if (fd_enable_ >= 0)
            ::close(fd_enable_);
        fd_duty_ = fd_period_ = fd_enable_ = -1;
    }

    render_buf_.resize(PCM_RENDER_BUFFER_FRAMES);
    park_probe_buf_.resize(static_cast<size_t>(park_probe_frames()));

    initialized_ = true;
    return true;
}

void PWMSoundBackend::shutdown() {
    if (!initialized_) {
        return;
    }

    stop_render_thread();
    silence();

    if (fd_duty_ >= 0)
        ::close(fd_duty_);
    if (fd_period_ >= 0)
        ::close(fd_period_);
    if (fd_enable_ >= 0)
        ::close(fd_enable_);
    fd_duty_ = fd_period_ = fd_enable_ = -1;

    initialized_ = false;
}

// ============================================================================
// Tone mode (SFX, system sounds) — variable frequency via period
// ============================================================================

static void sysfs_write_fd(int fd, int value) {
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", value);
    ::lseek(fd, 0, SEEK_SET);
    ::write(fd, buf, len);
    // sysfs writes replace the file's content; a plain file (test mock, or a
    // tmpfs stand-in) keeps the stale tail past the new length. Truncating
    // makes both behave the same. On real sysfs ftruncate just fails, ignored
    // like the write result above.
    ::ftruncate(fd, len);
}

void PWMSoundBackend::set_tone(float freq_hz, float amplitude, float /* duty_cycle */) {
    if (!initialized_) {
        return;
    }

    // While PCM render thread is running (tracker playback), skip tone-mode
    // sounds entirely — the single PWM channel can't do both simultaneously.
    // SoundManager already handles priority (ALARM kills tracker).
    if (render_running_.load()) {
        return;
    }

    amplitude = std::clamp(amplitude, 0.0f, 1.0f);

    if (amplitude == 0.0f || freq_hz <= 0.0f) {
        silence();
        return;
    }

    // If in PCM mode (shouldn't happen with guard above, but be safe)
    if (in_pcm_mode_) {
        exit_pcm_mode();
    }

    uint32_t period_ns = freq_to_period_ns(freq_hz);
    if (period_ns == 0) {
        silence();
        return;
    }

    float ratio = waveform_duty_ratio(current_wave_);
    uint32_t duty_ns = static_cast<uint32_t>(static_cast<float>(period_ns) * ratio * amplitude);

    // Dedup: the sequencer re-sends the same note every tick, so a held
    // tone would otherwise rewrite sysfs hundreds of times per second
    // (mirrors M300SoundBackend::last_freq_). Keyed on the written values
    // and only while the channel is already emitting; silence() clears it.
    if (enabled_ && period_ns == last_tone_period_ns_ && duty_ns == last_tone_duty_ns_) {
        return;
    }

    if (fd_duty_ >= 0) {
        if (!enabled_) {
            // First tone: set up from scratch
            sysfs_write_fd(fd_duty_, 0);
            sysfs_write_fd(fd_period_, static_cast<int>(period_ns));
            sysfs_write_fd(fd_duty_, static_cast<int>(duty_ns));
            sysfs_write_fd(fd_enable_, 1);
            enabled_ = true;
        } else {
            // Already playing: glitch-free transition
            // Set duty to 0 first (always safe), then period, then new duty
            // This avoids the disable/enable cycle that causes audible pops
            sysfs_write_fd(fd_duty_, 0);
            sysfs_write_fd(fd_period_, static_cast<int>(period_ns));
            sysfs_write_fd(fd_duty_, static_cast<int>(duty_ns));
        }
        last_period_ns_ = period_ns;
    } else {
        std::string path = channel_path();
        std::ofstream(path + "/duty_cycle") << "0";
        std::ofstream(path + "/period") << period_ns;
        std::ofstream(path + "/duty_cycle") << duty_ns;
        if (!enabled_) {
            std::ofstream(path + "/enable") << "1";
            enabled_ = true;
        }
    }

    last_tone_period_ns_ = period_ns;
    last_tone_duty_ns_ = duty_ns;
    tone_write_count_.fetch_add(1, std::memory_order_relaxed);
}

void PWMSoundBackend::silence() {
    if (!initialized_) {
        return;
    }

    // Don't silence the PWM while the PCM render thread owns it
    if (render_running_.load() && in_pcm_mode_) {
        return;
    }

    // Already off: the tracker fallback calls silence_voice(0) every tick
    // through rests, so without this guard a rest rewrites enable ~500x/s
    // (M300SoundBackend guards its silence the same way).
    if (!enabled_) {
        return;
    }

    if (fd_enable_ >= 0) {
        sysfs_write_fd(fd_enable_, 0);
    } else {
        std::string path = channel_path();
        std::ofstream(path + "/enable") << "0";
    }
    enabled_ = false;

    // Channel is off — the next set_tone must emit, even for the same tone
    last_tone_period_ns_ = 0;
    last_tone_duty_ns_ = 0;
}

void PWMSoundBackend::set_waveform(Waveform w) {
    if (!initialized_) {
        return;
    }
    current_wave_ = w;
}

// ============================================================================
// PCM mode (tracker playback) — fixed carrier, variable duty cycle
// ============================================================================

void PWMSoundBackend::set_render_source(std::function<void(float*, size_t, int)> fn) {
    {
        std::lock_guard<std::mutex> lock(render_source_mutex_);
        render_source_ = std::move(fn);
    }
    start_render_thread();
}

void PWMSoundBackend::clear_render_source() {
    spdlog::info("[PWMSoundBackend] clear_render_source() called");
    stop_render_thread();
    {
        std::lock_guard<std::mutex> lock(render_source_mutex_);
        render_source_ = nullptr;
    }
    spdlog::info("[PWMSoundBackend] clear_render_source() done");
}

void PWMSoundBackend::enter_pcm_mode() {
    if (in_pcm_mode_)
        return;

    uint32_t carrier_period = static_cast<uint32_t>(1e9 / PCM_CARRIER_HZ);

    if (fd_duty_ >= 0) {
        sysfs_write_fd(fd_enable_, 0);
        sysfs_write_fd(fd_duty_, 0);
        sysfs_write_fd(fd_period_, static_cast<int>(carrier_period));
        sysfs_write_fd(fd_enable_, 1);
    } else {
        std::string path = channel_path();
        std::ofstream(path + "/enable") << "0";
        std::ofstream(path + "/duty_cycle") << "0";
        std::ofstream(path + "/period") << carrier_period;
        std::ofstream(path + "/enable") << "1";
    }

    enabled_ = true;
    in_pcm_mode_ = true;
    spdlog::debug("[PWMSoundBackend] Entered PCM mode (carrier {}Hz, sample rate {}Hz)",
                  PCM_CARRIER_HZ, PCM_SAMPLE_RATE);
}

void PWMSoundBackend::exit_pcm_mode() {
    if (!in_pcm_mode_)
        return;

    silence();
    in_pcm_mode_ = false;
    spdlog::debug("[PWMSoundBackend] Exited PCM mode");
}

void PWMSoundBackend::apply_render_thread_priority() {
    // SCHED_IDLE: this thread runs only when nothing else wants the CPU, so
    // its pacing can never starve klippy — the failure that got PCM playback
    // pulled from ad5m in the first place. Setting it needs no privileges.
    struct sched_param sp = {};
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) == 0) {
        // sched_getscheduler takes a kernel TID, not a pthread_t (opaque) —
        // passing pthread_self() there always fails ESRCH.
        applied_sched_policy_ = sched_getscheduler(static_cast<pid_t>(::syscall(SYS_gettid)));
    } else {
        spdlog::debug("[PWMSoundBackend] SCHED_IDLE refused (errno {}) - staying SCHED_OTHER",
                      errno);
    }
    // Timerslack only affects relative sleeps (the 1 ms no-source poll); the
    // render loop paces with TIMER_ABSTIME, so this is belt-and-suspenders.
    prctl(PR_SET_TIMERSLACK, 1UL);
}

void PWMSoundBackend::park_output() {
    // Duty first, then disable: the channel must rest low before enable drops.
    if (fd_duty_ >= 0) {
        sysfs_write_fd(fd_duty_, 0);
        sysfs_write_fd(fd_enable_, 0);
    } else {
        std::string path = channel_path();
        std::ofstream(path + "/duty_cycle") << "0";
        std::ofstream(path + "/enable") << "0";
    }
    // enabled_/in_pcm_mode_ stay as they are: silence()'s render_running_
    // guard and set_tone's first-tone branch depend on them.
}

void PWMSoundBackend::resume_output() {
    if (fd_duty_ >= 0) {
        sysfs_write_fd(fd_duty_, 0);
        sysfs_write_fd(fd_enable_, 1);
    } else {
        std::string path = channel_path();
        std::ofstream(path + "/duty_cycle") << "0";
        std::ofstream(path + "/enable") << "1";
    }
}

void PWMSoundBackend::start_render_thread() {
    if (render_running_.load())
        return;

    render_running_.store(true);
    render_thread_ = std::thread(&PWMSoundBackend::render_loop, this);
    spdlog::info("[PWMSoundBackend] PCM render thread started");
}

void PWMSoundBackend::stop_render_thread() {
    if (!render_running_.load())
        return;

    render_running_.store(false);
    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    if (in_pcm_mode_) {
        exit_pcm_mode();
    }

    spdlog::info("[PWMSoundBackend] PCM render thread stopped");
}

void PWMSoundBackend::render_loop() {
    // Printer safety before anything else: demote this thread so that no
    // matter how the pacing below goes wrong, it outranks nobody.
    apply_render_thread_priority();

    // Default the clock/sleep seams (tests inject virtual ones before this
    // thread starts).
    if (!now_fn_) {
        now_fn_ = [] { return monotonic_now_ns(); };
    }
    if (!wait_until_fn_) {
        std::function<int64_t()> now = now_fn_;
        wait_until_fn_ = [now](int64_t deadline_ns) { default_wait_until(deadline_ns, now); };
    }

    // A previous playback on this backend may have left the loop parked.
    parked_.store(false);
    silent_buffer_run_.store(0);

    enter_pcm_mode();

    const uint32_t carrier_period = static_cast<uint32_t>(1e9 / PCM_CARRIER_HZ);
    const int64_t sample_interval_ns = 1000000000LL / PCM_SAMPLE_RATE;

    // Duty range: 0.5% to 99.5% for maximum buzzer deflection
    static constexpr float DUTY_MIN = 0.005f;
    static constexpr float DUTY_RANGE = 0.99f; // 0.995 - 0.005

    // Pre-allocate duty cycle string buffer (avoid per-sample allocation)
    // Max duty value ~16000 = 5 digits + null = 6 bytes per entry
    std::vector<char> duty_strs(render_buf_.size() * 8);
    std::vector<int> duty_offsets(render_buf_.size());
    std::vector<int> duty_lengths(render_buf_.size());

    // Automatic gain control: track peak and boost quiet signals
    // MOD files often have low amplitude (3-5% of full scale)
    float agc_gain = 4.0f;
    static constexpr float AGC_TARGET = 0.85f;
    static constexpr float AGC_MAX = 20.0f;
    static constexpr float AGC_ATTACK = 0.001f;   // fast attack (reduce gain on peaks)
    static constexpr float AGC_RELEASE = 0.0001f; // slow release (increase gain gradually)

    int64_t base_ns = now_fn_();
    int64_t sample_index = 0;

    while (render_running_.load()) {
        // Get render source under lock
        std::function<void(float*, size_t, int)> source;
        {
            std::lock_guard<std::mutex> lock(render_source_mutex_);
            source = render_source_;
        }

        if (!source) {
            struct timespec sl = {0, 1000000}; // 1ms
            nanosleep(&sl, nullptr);
            base_ns = now_fn_();
            sample_index = 0;
            silent_buffer_run_.store(0);
            continue;
        }

        if (parked_.load()) {
            // Parked: channel is off and no duty writes happen. Poll a small
            // probe buffer and resume the moment real audio shows up.
            const size_t probe_frames = static_cast<size_t>(park_probe_frames());
            std::memset(park_probe_buf_.data(), 0, probe_frames * sizeof(float));
            source(park_probe_buf_.data(), probe_frames, PCM_SAMPLE_RATE);
            bool any_nonzero = false;
            for (size_t i = 0; i < probe_frames && !any_nonzero; i++) {
                any_nonzero = park_probe_buf_[i] != 0.0f;
            }
            if (any_nonzero) {
                // Re-enable BEFORE publishing the flag: a reader that sees
                // parked_ == false must find the channel already on.
                resume_output();
                parked_.store(false);
                silent_buffer_run_.store(0);
                base_ns = now_fn_();
                sample_index = 0;
            } else {
                wait_until_fn_(now_fn_() + PCM_PARK_POLL_NS);
            }
            continue;
        }

        // Render a buffer of PCM samples
        size_t frames = render_buf_.size();
        std::memset(render_buf_.data(), 0, frames * sizeof(float));
        source(render_buf_.data(), frames, PCM_SAMPLE_RATE);

        // Find peak in this buffer for AGC (and the park detector)
        float peak = 0.0f;
        for (size_t i = 0; i < frames; i++) {
            float abs_val = std::abs(render_buf_[i]);
            if (abs_val > peak)
                peak = abs_val;
        }

        // Adjust AGC gain
        if (peak * agc_gain > 1.0f) {
            // Peak would clip — reduce gain quickly
            agc_gain = std::max(1.0f, 1.0f / peak);
        } else if (peak > 0.001f) {
            float desired = AGC_TARGET / peak;
            desired = std::min(desired, AGC_MAX);
            if (desired < agc_gain) {
                agc_gain += (desired - agc_gain) * AGC_ATTACK;
            } else {
                agc_gain += (desired - agc_gain) * AGC_RELEASE;
            }
        }

        // Consecutive exactly-silent buffers mean playback is over, not a
        // quiet passage — park the channel and drop to the cheap poll.
        silent_buffer_run_.store(peak == 0.0f ? silent_buffer_run_.load() + 1 : 0);
        if (silent_buffer_run_.load() >= park_silent_buffers_) {
            park_output();
            parked_.store(true);
            continue;
        }

        // Pre-convert entire buffer to duty cycle strings (batch, no per-sample overhead)
        int str_offset = 0;
        for (size_t i = 0; i < frames; i++) {
            float sample = std::clamp(render_buf_[i] * agc_gain, -1.0f, 1.0f);
            float normalized = (sample + 1.0f) / 2.0f;
            int duty = static_cast<int>(static_cast<float>(carrier_period) *
                                        (DUTY_MIN + normalized * DUTY_RANGE));

            duty_offsets[i] = str_offset;
            int len = snprintf(duty_strs.data() + str_offset, 8, "%d", duty);
            duty_lengths[i] = len;
            str_offset += len + 1; // +1 for null terminator
        }

        // Output samples with absolute-deadline pacing and bounded catch-up
        for (size_t i = 0; i < frames && render_running_.load(); i++) {
            // Write pre-formatted duty cycle string
            if (fd_duty_ >= 0) {
                ::lseek(fd_duty_, 0, SEEK_SET);
                ::write(fd_duty_, duty_strs.data() + duty_offsets[i], duty_lengths[i]);
                duty_write_count_.fetch_add(1, std::memory_order_relaxed);
            }

            sample_index++;

            if (samples_behind(now_fn_(), base_ns, sample_index, sample_interval_ns) >
                PCM_CATCHUP_MAX_SAMPLES) {
                // Scheduling stall: skip to the sample due now instead of
                // bursting the missed writes — the burst is what starved
                // klippy under the old loop.
                const int64_t snap = resync_sample_index(now_fn_(), base_ns, sample_interval_ns);
                i += static_cast<size_t>(snap - sample_index); // may overshoot: loop exits
                sample_index = snap;
                continue; // already late — no wait
            }

            wait_until_fn_(base_ns + sample_index * sample_interval_ns);
        }
    }

    // Silence on exit
    if (fd_duty_ >= 0) {
        sysfs_write_fd(fd_duty_, 0);
    }
    spdlog::debug("[PWMSoundBackend] render loop done after {} duty writes",
                  duty_write_count_.load());
}
