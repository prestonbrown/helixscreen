// SPDX-License-Identifier: GPL-3.0-or-later

#include "jz_pwm_sound_backend.h"

#include "note_event.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kVoices = 4;
constexpr const char* kDevice = "/dev/jz_pwm";
// fx-pwm's home depends on whose namespace we exec from: on the rig
// HelixScreen runs INSIDE the Forge-X chroot, where the tool is at its
// ordinary path; a host-side dev build reaches it through the chroot
// mount. tone_player needs the chroot prefix only because klippy runs
// on the host.
constexpr const char* kFxPwmPaths[] = {"/usr/bin/fx-pwm", "/usr/data/.mod/.forge-x/usr/bin/fx-pwm",
                                       nullptr};
constexpr const char* kGpio = "pc12";
// Channel-register bookkeeping only (the DMA rate is fixed by the
// encoded words); 770 MHz / prescale 2 = the calibrated 385 MHz clock.
constexpr long kPrescale = 2;
constexpr const char* kWordsFile = "/tmp/helixscreen-jz-pwm.words";
constexpr const char* kVoiceLog = "/tmp/jz-voice.log";
constexpr const char* kDaemonSock = "/tmp/fx-pwm.sock";
/* Intra-burst calls land microseconds apart; bursts are one tracker tick
 * apart (>= 2500/255 ~ 10 ms even at max tempo). Anything >= 4 ms since
 * the last call reliably means a new burst started. */
constexpr double kVoiceBurstGapMs = 4.0;

/// The app builds at most one piezo backend; the lab modal addresses it
/// through this pointer (null everywhere the backend doesn't compile).
JzPwmSoundBackend* g_live_jz_pwm = nullptr;

Waveform jz_wave_for_int(int w) {
    switch (w) {
    case 1:
        return Waveform::SQUARE;
    case 2:
        return Waveform::SAW;
    default:
        return Waveform::TRIANGLE;
    }
}

} // namespace

std::vector<uint32_t> jz_pwm_render_step(const NoteEvent* events, int n_events,
                                         const JzPwmRenderParams& p) {
    std::vector<uint32_t> words;

    if (events == nullptr || n_events <= 0)
        return words;

    VoiceSlot slots[kVoices];
    int active = 0;
    float total_ms = 0;

    for (int v = 0; v < n_events && v < kVoices; ++v) {
        slots[v].event = events[v];
        slots[v].reset_for_new_note();
        if (events[v].velocity > 0.001f && events[v].freq_hz > 0)
            active++;
        total_ms = std::max(total_ms, events[v].duration_ms);
    }
    if (active == 0 || total_ms <= 0)
        return words;

    /* Render the step at its TRUE duration, then tile that content out
     * to the audible floor - a 6 ms theme tick cannot be reproduced
     * duty-encoded (rig-measured), but its tone repeated for the floor
     * is both audible and honest to the note's content. */
    long floor_ms = p.min_note_ms;
    long cap_ms = p.max_note_ms;
    long play_ms = std::clamp((long)total_ms, floor_ms, cap_ms);
    size_t n_true = static_cast<size_t>(total_ms * p.carrier_hz / 1000.0);
    n_true = std::max(n_true, p.word_align);
    n_true -= n_true % p.word_align;
    size_t n = static_cast<size_t>(play_ms * p.carrier_hz / 1000.0);
    n -= n % p.word_align;

    const long period = static_cast<long>(p.clock_hz / p.carrier_hz);
    if (period < 4)
        return words;

    std::vector<uint32_t> rendered;
    rendered.reserve(std::min(n, n_true));
    for (size_t i = 0; i < n_true; ++i) {
        float mix = 0;

        for (int v = 0; v < n_events && v < kVoices; ++v)
            mix += slots[v].render_sample(static_cast<float>(p.carrier_hz));
        mix /= active; /* normalize by sounding voices, not slots */
        mix = std::clamp(mix, -1.0f, 1.0f);

        long on = static_cast<long>(period * (0.5 + p.duty_swing * mix));
        on = std::clamp(on, 1L, period - 1L);
        /* upper 16 bits: inactive counts; lower 16: active counts */
        rendered.push_back(static_cast<uint32_t>(((period - on) << 16) | on));
    }

    words.reserve(n);
    for (size_t i = 0; i < n; ++i)
        words.push_back(rendered[i % n_true]);

    /* Peak-normalize to the (scaled) duty swing. Theme velocities and
     * sustain levels leave the effective modulation far under this
     * transducer's audible floor - every rig-verified sound used the full
     * +-40% swing. A one-shot UI sound has no dynamic-range budget worth
     * preserving; the voice path scales the TARGET down instead so row
     * loudness (master volume) survives normalization. */
    const long half = period / 2;
    const long full = (long)(period * p.duty_swing * p.swing_scale);
    long peak = 0;
    for (uint32_t w : words)
        peak = std::max(peak, std::abs((long)(w & 0xffff) - half));
    if (peak > 0 && peak != full) {
        double gain = (double)full / peak;
        for (auto& w : words) {
            long on = (long)(w & 0xffff);
            long dev = (long)std::lround((on - half) * gain);
            on = std::clamp(half + dev, 1L, period - 1L);
            w = (uint32_t)(((period - on) << 16) | on);
        }
    }
    return words;
}

std::vector<NoteEvent> jz_pwm_voices_to_events(const float* freq, const float* amp, int n_voices,
                                               float dur_ms, float attack_ms, float release_ms,
                                               Waveform wave, int octave_shift) {
    std::vector<NoteEvent> events;
    for (int v = 0; v < n_voices && v < kVoices; ++v) {
        if (freq[v] <= 0 || amp[v] <= 0.001f)
            continue;
        /* Global register shift first (the piezo band is ~1-4 kHz and the
         * tune's fundamentals sit at 110-175 Hz - rig-measured inaudible
         * there), then the hard ultrasonic ceiling: period-only tracker
         * rows emit the Paula playback rate (8-14 kHz) as a fallback. */
        float f = freq[v];
        for (int i = 0; i < octave_shift; ++i)
            f *= 2.0f;
        for (int i = 0; i > octave_shift; --i)
            f *= 0.5f;
        while (f > 4000.0f)
            f *= 0.5f;
        NoteEvent e;
        e.freq_hz = f;
        e.velocity = amp[v];
        e.duration_ms = dur_ms;
        e.wave = wave;
        e.attack_ms = attack_ms;
        e.decay_ms = 0;
        e.sustain_level = 1.0f;
        e.release_ms = release_ms;
        events.push_back(e);
    }
    return events;
}

bool jz_voice_should_flush(bool dirty, double ms_since_flush, double ms_since_call,
                           double flush_interval_ms, double burst_gap_ms) {
    if (!dirty)
        return false;
    /* Mid-burst (last call microseconds ago) the slots hold a mix of the
     * previous and current tick - rendering there was the static bug:
     * every chord carried three stale voices. Only a burst START (gap
     * since the last call) guarantees one complete tick state. */
    if (ms_since_call < burst_gap_ms)
        return false;
    return ms_since_flush >= flush_interval_ms;
}

bool JzPwmSoundBackend::available() {
    if (access(kDevice, F_OK) != 0)
        return false;
    for (int i = 0; kFxPwmPaths[i]; ++i)
        if (access(kFxPwmPaths[i], X_OK) == 0)
            return true;
    return false;
}

// ============================================================================
// Phrase renderer - the tracker path's DSP
// ============================================================================

std::vector<uint32_t> jz_pwm_render_phrase(const JzPhraseRow* rows, int n_rows,
                                           const JzPwmRenderParams& p, const JzPwmVoiceKnobs& k) {
    std::vector<uint32_t> words;
    if (!rows || n_rows <= 0)
        return words;

    float total_ms = 0;
    for (int r = 0; r < n_rows; ++r)
        total_ms += rows[r].ms;
    if (total_ms <= 0)
        return words;

    /* Driver buffer limit: 65536 words = 2 s at the 32768 Hz carrier. */
    long max_ms = (long)(65536L * 1000.0L / p.carrier_hz);
    long play_ms = std::min((long)total_ms, max_ms);

    const long period = static_cast<long>(p.clock_hz / p.carrier_hz);
    if (period < 4)
        return words;

    /* Map each row's slots: octave shift + ultrasonic ceiling, same rules
     * as the per-step path, but no per-note envelope - rows flow and the
     * envelope wraps the whole phrase. */
    struct ChannelState {
        double phase = 0;
        double freq = 0;
        float amp = 0;
        bool active = false;
    };
    ChannelState chans[kVoices];

    size_t n = static_cast<size_t>(play_ms * p.carrier_hz / 1000.0);
    n -= n % p.word_align;
    words.reserve(n);

    const Waveform wave = jz_wave_for_int(k.wave);
    double octave = std::pow(2.0, k.octave_shift);
    size_t i = 0;
    long emitted_ms = 0;
    for (int r = 0; r < n_rows && emitted_ms < play_ms; ++r) {
        for (int v = 0; v < kVoices; ++v) {
            chans[v].active = rows[r].freq[v] > 0 && rows[r].amp[v] > 0.001f;
            chans[v].amp = rows[r].amp[v];
            double f = rows[r].freq[v] * octave;
            while (f > 4000.0)
                f *= 0.5;
            chans[v].freq = chans[v].active ? f : 0.0;
        }

        long row_ms = std::min((long)rows[r].ms, play_ms - emitted_ms);
        size_t row_n = static_cast<size_t>(row_ms * p.carrier_hz / 1000.0);
        for (size_t j = 0; j < row_n && i < n; ++j, ++i) {
            float t_ms = (float)(i * 1000.0 / p.carrier_hz);
            float env = 1.0f;
            if (k.attack_ms > 0 && t_ms < k.attack_ms)
                env = t_ms / k.attack_ms;
            if (k.release_ms > 0 && t_ms > play_ms - k.release_ms)
                env = std::min(env, (play_ms - t_ms) / k.release_ms);

            int active = 0;
            float mix = 0;
            for (int v = 0; v < kVoices; ++v) {
                if (!chans[v].active || chans[v].freq <= 0)
                    continue;
                active++;
                chans[v].phase += (double)chans[v].freq / p.carrier_hz;
                chans[v].phase -= std::floor(chans[v].phase);
                double ph = chans[v].phase;
                double shape;
                switch (wave) {
                case Waveform::SQUARE:
                    shape = ph < 0.5 ? 1.0 : -1.0;
                    break;
                case Waveform::SAW:
                    shape = 2.0 * ph - 1.0;
                    break;
                default:
                    shape = 4.0 * std::abs(ph - 0.5) - 1.0;
                }
                mix += (float)(chans[v].amp * env * shape);
            }
            if (active == 0) {
                words.push_back((uint32_t)(((period - period / 2) << 16) | (period / 2)));
                continue;
            }
            mix /= active;
            mix = std::clamp(mix, -1.0f, 1.0f);
            long on = static_cast<long>(period * (0.5 + k.swing * mix));
            on = std::clamp(on, 1L, period - 1L);
            words.push_back(static_cast<uint32_t>(((period - on) << 16) | on));
        }
        emitted_ms += row_ms;
    }

    /* Peak-normalize to the scaled swing target, same contract as the
     * step renderer: quiet rows stay audible, master volume survives. */
    float loudest = 0;
    for (int r = 0; r < n_rows; ++r)
        for (int v = 0; v < kVoices; ++v)
            loudest = std::max(loudest, rows[r].amp[v]);
    const long half = period / 2;
    const long full = (long)(period * k.swing * std::clamp(loudest, 0.2f, 1.0f));
    long peak = 0;
    for (uint32_t w : words)
        peak = std::max(peak, std::abs((long)(w & 0xffff) - half));
    if (peak > 0 && peak != full) {
        double gain = (double)full / peak;
        for (auto& w : words) {
            long on = (long)(w & 0xffff);
            long dev = (long)std::lround((on - half) * gain);
            on = std::clamp(half + dev, 1L, period - 1L);
            w = (uint32_t)(((period - on) << 16) | on);
        }
    }
    return words;
}

int jz_phrase_clip(JzPhraseRow* rows, int n_rows, float budget_ms) {
    float acc = 0;
    for (int r = 0; r < n_rows; ++r) {
        if (acc + rows[r].ms > budget_ms) {
            rows[r].ms = budget_ms - acc;
            return r + 1;
        }
        acc += rows[r].ms;
    }
    return n_rows;
}

// ============================================================================
// JzPwmSoundBackend
// ============================================================================

JzPwmSoundBackend::JzPwmSoundBackend() {
    g_live_jz_pwm = this;
}

JzPwmSoundBackend::~JzPwmSoundBackend() {
    if (g_live_jz_pwm == this)
        g_live_jz_pwm = nullptr;
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            worker_exit_ = true;
        }
        send_cv_.notify_all();
        worker_.join();
    }
    if (sock_ >= 0)
        close(sock_);
    if (daemon_pid_ > 0) {
        kill(daemon_pid_, SIGTERM);
        int status = 0;
        waitpid(daemon_pid_, &status, 0);
    }
    if (voice_log_)
        fclose(voice_log_);
}

JzPwmVoiceKnobs JzPwmSoundBackend::voice_knobs() const {
    return knobs_;
}

void JzPwmSoundBackend::set_voice_knobs(const JzPwmVoiceKnobs& k) {
    knobs_.buf_ms = std::clamp(k.buf_ms, 300.0f, 2000.0f);
    knobs_.flush_ms = k.flush_ms;
    knobs_.attack_ms = std::clamp(k.attack_ms, 0.0f, 100.0f);
    knobs_.release_ms = std::clamp(k.release_ms, 0.0f, 200.0f);
    knobs_.wave = std::clamp(k.wave, 0, 2);
    knobs_.octave_shift = std::clamp(k.octave_shift, 0, 5);
    knobs_.swing = std::clamp(k.swing, 0.1f, 0.5f);
    knobs_.carrier_hz = std::clamp(k.carrier_hz, 4000.0f, 32768.0f);
}

bool jz_pwm_get_voice_knobs(JzPwmVoiceKnobs* out) {
    if (!g_live_jz_pwm || !out)
        return false;
    *out = g_live_jz_pwm->voice_knobs();
    return true;
}

void jz_pwm_set_voice_knobs(const JzPwmVoiceKnobs& knobs) {
    if (g_live_jz_pwm)
        g_live_jz_pwm->set_voice_knobs(knobs);
}

bool JzPwmSoundBackend::initialize() {
    for (int i = 0; kFxPwmPaths[i]; ++i) {
        if (access(kFxPwmPaths[i], X_OK) == 0) {
            fx_pwm_ = kFxPwmPaths[i];
            break;
        }
    }
    if (fx_pwm_.empty() || access(kDevice, F_OK) != 0)
        return false;

    /* Ear-sweep knobs: the rig tunes these per restart instead of per
     * rebuild, one variable per listening round. */
    auto env_float = [](const char* name, float fallback) {
        const char* v = getenv(name);
        if (!v || !*v)
            return fallback;
        float f = std::strtof(v, nullptr);
        return (f > 0) ? f : fallback;
    };
    auto env_int = [](const char* name, int fallback) {
        const char* v = getenv(name);
        if (!v || !*v)
            return fallback;
        return atoi(v);
    };
    knobs_.buf_ms = env_float("HELIX_JZ_VOICE_MS", knobs_.buf_ms);
    knobs_.attack_ms = env_float("HELIX_JZ_ATTACK_MS", knobs_.attack_ms);
    knobs_.release_ms = env_float("HELIX_JZ_RELEASE_MS", knobs_.release_ms);
    knobs_.swing = env_float("HELIX_JZ_SWING", knobs_.swing);
    knobs_.wave = std::clamp(env_int("HELIX_JZ_WAVE", knobs_.wave), 0, 2);
    knobs_.octave_shift = std::clamp(env_int("HELIX_JZ_OCTAVE", knobs_.octave_shift), 0, 5);
    knobs_.carrier_hz = env_float("HELIX_JZ_CARRIER", knobs_.carrier_hz);
    set_voice_knobs(knobs_); /* apply clamps */

    const char* log = getenv("HELIX_JZ_LOG_VOICE");
    if (log && log[0] == '1') {
        voice_log_ = fopen(kVoiceLog, "a");
        if (voice_log_) {
            voice_log_epoch_ = std::chrono::steady_clock::now();
            fprintf(voice_log_,
                    "# session phrase_ms=%.0f wave=%d oct=%d atk=%.0f rel=%.0f swing=%.2f "
                    "carrier=%.0f\n",
                    knobs_.buf_ms, knobs_.wave, knobs_.octave_shift, knobs_.attack_ms,
                    knobs_.release_ms, knobs_.swing, knobs_.carrier_hz);
            fflush(voice_log_);
        }
    }
    return true;
}

float JzPwmSoundBackend::min_tick_ms() const {
    // The voice path only appends to the row history per tick; the DSP
    // runs once per phrase. Cheap enough to tick as fast as the tracker.
    return 20.0f;
}

void JzPwmSoundBackend::set_tone(float freq_hz, float amplitude, float duty_cycle) {
    // Never called while supports_note_events() is true (the sequencer
    // drives steps through publish_note); kept for interface parity.
    (void)freq_hz;
    (void)amplitude;
    (void)duty_cycle;
}

void JzPwmSoundBackend::publish_note(int slot, const NoteEvent& event) {
    if (slot < 0 || slot >= kVoices)
        return;
    pending_[slot] = event;
    if (++received_ < kVoices)
        return;
    received_ = 0;
    flush_step();
}

void JzPwmSoundBackend::flush_step() {
    params_.swing_scale = 1.0; /* theme sounds always play at full swing */
    params_.duty_swing = 0.4;  /* ...and the full default depth */
    params_.carrier_hz = knobs_.carrier_hz;
    std::vector<uint32_t> words = jz_pwm_render_step(pending_, kVoices, params_);
    if (words.empty())
        return;

    float ms = 0;
    for (const NoteEvent& e : pending_)
        ms = std::max(ms, e.duration_ms);
    ms = std::clamp(ms, static_cast<float>(params_.min_note_ms),
                    static_cast<float>(params_.max_note_ms));
    enqueue_frame(std::move(words), (uint32_t)ms);
}

// ============================================================================
// Sound daemon client (fx-pwm serve over /tmp/fx-pwm.sock)
// ============================================================================

bool JzPwmSoundBackend::ensure_daemon() {
    if (sock_ >= 0)
        return true;

    auto try_connect = []() {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0)
            return -1;
        /* A stuck daemon must never freeze the UI thread that sends:
         * both directions time out. The first frozen-UI incident was
         * exactly an unbounded write into a daemon that had stopped
         * draining. */
        timeval tv{0, 400 * 1000};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, kDaemonSock, sizeof(addr.sun_path) - 1);
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(s);
            return -1;
        }
        return s;
    };

    sock_ = try_connect();
    if (sock_ >= 0)
        return true;

    /* No daemon: spawn it once. Same child hygiene as the old per-buffer
     * spawn (signal dispositions and masks survive execv), but this is a
     * one-time cost for a process that lives as long as sound does. */
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);
        signal(SIGALRM, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        const char* args[] = {fx_pwm_.c_str(), "serve", kGpio, "--prescale=2", nullptr};
        execv(fx_pwm_.c_str(), const_cast<char**>(args));
        _exit(127);
    }
    if (pid < 0) {
        spdlog::warn("[JzPwmBackend] daemon fork failed: {}", std::strerror(errno));
        return false;
    }
    daemon_pid_ = pid;

    /* The daemon needs a beat to bind its socket. Retry briefly. */
    for (int attempt = 0; attempt < 20 && sock_ < 0; ++attempt) {
        usleep(50 * 1000);
        sock_ = try_connect();
    }
    if (sock_ < 0) {
        spdlog::warn("[JzPwmBackend] daemon did not come up at {}", kDaemonSock);
        /* reap if it died */
        if (daemon_pid_ > 0) {
            int status = 0;
            if (waitpid(daemon_pid_, &status, WNOHANG) == daemon_pid_)
                daemon_pid_ = -1;
        }
        return false;
    }
    spdlog::info("[JzPwmBackend] sound daemon up (pid {})", daemon_pid_);
    return true;
}

int JzPwmSoundBackend::send_to_daemon(const std::vector<uint32_t>& words, uint32_t hold_ms) {
    /* Runs on the sender worker only. Writes loop on partials: a phrase
     * body is larger than the socket buffer and the daemon cannot drain
     * while inside its copy ioctl, so a single write() returning short
     * is normal - abandoning a frame mid-body is what corrupts the
     * stream. */
    auto write_all = [&](const void* buf, size_t len) -> bool {
        const char* p = (const char*)buf;
        while (len > 0) {
            ssize_t n = write(sock_, p, len);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            p += n;
            len -= (size_t)n;
        }
        return true;
    };
    auto send_once = [&](uint32_t* ack_out) -> bool {
        uint32_t head[3] = {0x4A5A5331u, hold_ms, (uint32_t)words.size()};
        if (!write_all(head, sizeof(head)))
            return false;
        if (!write_all(words.data(), words.size() * 4))
            return false;
        uint8_t ack = 2;
        if (read(sock_, &ack, 1) != 1)
            return false;
        *ack_out = ack;
        return true;
    };

    if (!ensure_daemon())
        return -1;

    uint32_t ack = 2;
    if (!send_once(&ack)) {
        spdlog::warn("[JzPwmBackend] daemon send failed; respawning");
        close(sock_);
        sock_ = -1;
        if (daemon_pid_ > 0) {
            int status = 0;
            if (waitpid(daemon_pid_, &status, WNOHANG) == daemon_pid_)
                daemon_pid_ = -1;
        }
        if (!ensure_daemon() || !send_once(&ack))
            return -1;
    }
    return (int)ack;
}

void JzPwmSoundBackend::sender_worker() {
    std::unique_lock<std::mutex> lock(send_mutex_);
    for (;;) {
        send_cv_.wait(lock, [&] { return worker_exit_ || pending_frame_.has_value(); });
        if (worker_exit_)
            return;
        PendingFrame frame = std::move(*pending_frame_);
        pending_frame_.reset();
        lock.unlock();

        int ack;
        if (frame.words.empty()) {
            /* cancel marker */
            if (sock_ >= 0) {
                uint32_t cancel[3] = {0x4A5A5331u, 0, 0};
                ssize_t r = write(sock_, cancel, sizeof(cancel));
                (void)r;
            }
            ack = 0;
        } else {
            ack = send_to_daemon(frame.words, frame.hold_ms);
        }

        lock.lock();
        if (ack == 1 && !pending_frame_.has_value()) {
            /* Daemon still holding: requeue for a retry, but back off -
             * an instant resend makes the daemon's socket perpetually
             * readable and its poll-based hold restarts forever. */
            pending_frame_ = std::move(frame);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            lock.lock();
            continue;
        }
    }
}

void JzPwmSoundBackend::enqueue_frame(std::vector<uint32_t>&& words, uint32_t hold_ms) {
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        pending_frame_ = PendingFrame{std::move(words), hold_ms};
    }
    if (!worker_.joinable()) {
        worker_ = std::thread([this] { sender_worker(); });
        spdlog::info("[JzPwmBackend] sender worker started");
    }
    send_cv_.notify_one();
}

// ============================================================================
// Voice path: rolling row history -> phrases
// ============================================================================

void JzPwmSoundBackend::set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) {
    (void)duty_cycle; /* the wave render carries the tone */
    if (slot < 0 || slot >= kVoices)
        return;
    auto now = std::chrono::steady_clock::now();
    double since_call = std::chrono::duration<double, std::milli>(now - last_voice_call_).count();
    if (jz_voice_should_flush(voices_dirty_, 0, since_call, 0, kVoiceBurstGapMs))
        assemble_phrase(now);
    last_voice_call_ = now;

    if (voice_freq_[slot] == freq_hz && voice_amp_[slot] == amplitude)
        return;
    voice_freq_[slot] = freq_hz;
    voice_amp_[slot] = amplitude;
    voices_dirty_ = true;
    if (voice_log_) {
        fprintf(voice_log_, "V %10.1f s%d f=%.1f a=%.2f\n", since_call, slot, freq_hz, amplitude);
        fflush(voice_log_);
    }
}

void JzPwmSoundBackend::silence_voice(int slot) {
    if (slot < 0 || slot >= kVoices)
        return;
    auto now = std::chrono::steady_clock::now();
    double since_call = std::chrono::duration<double, std::milli>(now - last_voice_call_).count();
    if (jz_voice_should_flush(voices_dirty_, 0, since_call, 0, kVoiceBurstGapMs))
        assemble_phrase(now);
    last_voice_call_ = now;

    if (voice_freq_[slot] == 0)
        return;
    voice_freq_[slot] = 0;
    voice_amp_[slot] = 0;
    voices_dirty_ = true;
    if (voice_log_) {
        fprintf(voice_log_, "V %10.1f s%d silence\n", since_call, slot);
        fflush(voice_log_);
    }
}

void JzPwmSoundBackend::assemble_phrase(std::chrono::steady_clock::time_point now) {
    /* Append the current complete state to the history when it changed;
     * the row's duration is the delta to the NEXT change, patched when
     * the next entry lands. */
    bool changed = !history_seeded_;
    for (int v = 0; v < kVoices; ++v)
        changed = changed || history_back_freq_[v] != voice_freq_[v] ||
                  history_back_amp_[v] != voice_amp_[v];
    if (changed) {
        HistoryEntry e;
        e.t = now;
        for (int v = 0; v < kVoices; ++v) {
            e.freq[v] = voice_freq_[v];
            e.amp[v] = voice_amp_[v];
        }
        history_.push_back(e);
        history_seeded_ = true;
        for (int v = 0; v < kVoices; ++v) {
            history_back_freq_[v] = voice_freq_[v];
            history_back_amp_[v] = voice_amp_[v];
        }
        if (history_.size() > 1) {
            auto& prev = history_[history_.size() - 2];
            prev.ms = std::chrono::duration<double, std::milli>(e.t - prev.t).count();
        }
    } else if (!history_.empty()) {
        /* refresh the trailing row's length so a held note keeps growing */
        auto& back = history_.back();
        back.ms = std::chrono::duration<double, std::milli>(now - back.t).count();
    }

    if (history_.empty())
        return;

    double span_ms = 0;
    for (const auto& h : history_)
        span_ms += h.ms;
    bool all_quiet = true;
    for (int v = 0; v < kVoices; ++v)
        all_quiet = all_quiet && voice_freq_[v] == 0;

    if (span_ms >= knobs_.buf_ms || all_quiet) {
        /* Don't send while the previous phrase should still be holding:
         * the daemon would busy-drop it. The history keeps accumulating
         * and the next burst retries - phrases queue naturally. */
        if (!all_quiet && now < next_send_time_)
            return;

        std::vector<JzPhraseRow> rows;
        for (const auto& h : history_) {
            JzPhraseRow r;
            for (int v = 0; v < kVoices; ++v) {
                r.freq[v] = h.freq[v];
                r.amp[v] = h.amp[v];
            }
            r.ms = (float)h.ms;
            rows.push_back(r);
        }
        int kept = jz_phrase_clip(rows.data(), (int)rows.size(), 2000.0f);
        float ms = 0;
        float loudest = 0;
        for (int r = 0; r < kept; ++r) {
            ms += rows[r].ms;
            for (int v = 0; v < kVoices; ++v)
                loudest = std::max(loudest, rows[r].amp[v]);
        }

        params_.duty_swing = knobs_.swing;
        params_.carrier_hz = knobs_.carrier_hz;
        params_.swing_scale = std::clamp(loudest, 0.2f, 1.0f);
        auto words = jz_pwm_render_phrase(rows.data(), kept, params_, knobs_);
        if (voice_log_) {
            fprintf(voice_log_, "F %10.1f rows=%d ms=%.0f words=%zu\n",
                    std::chrono::duration<double, std::milli>(now - voice_log_epoch_).count(), kept,
                    ms, words.size());
            fflush(voice_log_);
        }
        int ack = 0;
        if (!words.empty()) {
            long upload_ms = (long)(words.size() * 35.0 / 1000.0);
            enqueue_frame(std::move(words), (uint32_t)ms);
            /* Schedule past BOTH the upload (~30 us/word, measured in the
             * daemon's phase log) and the hold - the daemon cannot drain
             * while its copy ioctl runs. */
            next_send_time_ = now + std::chrono::milliseconds(upload_ms + (long)ms + 100);
        } else {
            ack = -1;
        }
        /* History consumed: seed the next phrase with the current state. */
        HistoryEntry seed;
        seed.t = now;
        for (int v = 0; v < kVoices; ++v) {
            seed.freq[v] = voice_freq_[v];
            seed.amp[v] = voice_amp_[v];
            seed.ms = 0;
        }
        history_.clear();
        if (!all_quiet)
            history_.push_back(seed);
        voices_dirty_ = false;
    }
}

void JzPwmSoundBackend::silence() {
    /* Cancel any running hold (through the worker - one socket writer),
     * and drop pending history. */
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        pending_frame_ = PendingFrame{}; /* empty words = cancel marker */
    }
    if (worker_.joinable())
        send_cv_.notify_one();
    else if (sock_ >= 0) {
        uint32_t cancel[3] = {0x4A5A5331u, 0, 0};
        if (write(sock_, cancel, sizeof(cancel)) < 0) {
        }
    }
    history_.clear();
    history_seeded_ = false;
    voices_dirty_ = false;
    for (int v = 0; v < kVoices; ++v) {
        history_back_freq_[v] = -1;
        history_back_amp_[v] = -1;
    }
}
