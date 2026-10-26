// SPDX-License-Identifier: GPL-3.0-or-later

#include "pcm_pipe_sound_backend.h"

#include "helix_thread.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr const char* kSndDir = "/dev/snd";
/// A write that makes no progress for this long means the player stopped reading.
constexpr int kWriteStallMs = 2000;
constexpr int kWritePollMs = 200;
/// Bounded wait for the player to drain and exit after EOF before SIGKILL.
constexpr int kReapTimeoutMs = 2000;
/// A run that streamed this long without error clears the failure count.
constexpr uint64_t kHealthyFrames = PcmPipeSoundBackend::kSampleRate / 2;
constexpr int kResumeHandoffMs = 200;

bool env_disables(const char* v) {
    return std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "no") == 0;
}

} // namespace

// ============================================================================
// Probes (pure, for tests)
// ============================================================================

std::vector<std::string> PcmPipeSoundBackend::default_players() {
    return {"/usr/bin/aplay", "/usr/local/bin/aplay", "/bin/aplay", "/usr/sbin/aplay"};
}

std::string PcmPipeSoundBackend::find_player(const char* env_override,
                                             const std::vector<std::string>& candidates) {
    if (env_override && env_override[0] != '\0') {
        if (env_disables(env_override))
            return "";
        return access(env_override, X_OK) == 0 ? std::string(env_override) : std::string();
    }
    for (const auto& c : candidates) {
        if (access(c.c_str(), X_OK) == 0)
            return c;
    }
    return "";
}

bool PcmPipeSoundBackend::has_playback_device(const std::string& snd_dir) {
    DIR* d = opendir(snd_dir.c_str());
    if (!d)
        return false;
    bool found = false;
    while (dirent* e = readdir(d)) {
        const char* n = e->d_name;
        size_t len = std::strlen(n);
        // pcmC<card>D<dev>p is a playback stream; the 'c' twin is capture.
        if (len > 4 && std::strncmp(n, "pcmC", 4) == 0 && n[len - 1] == 'p') {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

std::vector<std::string> PcmPipeSoundBackend::player_args(const std::string& player) {
    return {player,
            "-q",
            "-t",
            "raw",
            "-f",
            "S16_LE",
            "-c",
            "1",
            "-r",
            std::to_string(kSampleRate),
            "-B",
            std::to_string(kPlayerBufferUs),
            "-"};
}

bool PcmPipeSoundBackend::available() {
    const char* env = std::getenv(kPlayerEnv);
    std::string player = find_player(env);
    if (player.empty()) {
        if (env && env[0] != '\0' && !env_disables(env))
            spdlog::warn("[PcmPipeSound] {}='{}' is not executable", kPlayerEnv, env);
        return false;
    }
    // An explicit override is the operator's word that the player can play;
    // the default probe also wants a card the player could open.
    if (env && env[0] != '\0')
        return true;
    return has_playback_device(kSndDir);
}

// ============================================================================
// Lifecycle
// ============================================================================

PcmPipeSoundBackend::PcmPipeSoundBackend() = default;

PcmPipeSoundBackend::~PcmPipeSoundBackend() {
    shutdown();
}

bool PcmPipeSoundBackend::initialize(const std::string& player_path) {
    if (running_.load(std::memory_order_relaxed))
        return true;
    if (player_path.empty() || access(player_path.c_str(), X_OK) != 0) {
        spdlog::warn("[PcmPipeSound] player '{}' is not executable", player_path);
        return false;
    }
    player_ = player_path;
    running_.store(true, std::memory_order_relaxed);
    render_thread_ = helix::make_thread(&PcmPipeSoundBackend::render_loop, this);
    spdlog::info("[PcmPipeSound] Audio initialized: {} Hz mono S16 via {}", kSampleRate, player_);
    return true;
}

void PcmPipeSoundBackend::shutdown() {
    if (!running_.load(std::memory_order_relaxed))
        return;
    running_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        active_ = false;
    }
    state_cv_.notify_all();
    { std::lock_guard<std::mutex> ack_lock(ack_mutex_); }
    ack_cv_.notify_all();
    if (render_thread_.joinable())
        render_thread_.join();
    stop_player();
    spdlog::info("[PcmPipeSound] Audio shutdown");
}

// ============================================================================
// Player process
// ============================================================================

bool PcmPipeSoundBackend::spawn_player() {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) {
        spdlog::warn("[PcmPipeSound] pipe: {}", std::strerror(errno));
        return false;
    }

    auto args = player_args(player_);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], STDIN_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    // The child inherits nothing of our signal setup: dispositions and the
    // mask survive exec, and a player with SIGPIPE ignored would never exit
    // on a closed pipe.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attr, &empty);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGALRM);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = -1;
    int rc = posix_spawn(&pid, player_.c_str(), &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fds[0]);
    if (rc != 0) {
        close(fds[1]);
        spdlog::warn("[PcmPipeSound] spawn {}: {}", player_, std::strerror(rc));
        return false;
    }
    // Non-blocking so a player that stops reading is a bounded stall, never a
    // render thread wedged in write() that shutdown() can no longer join.
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    pipe_wr_ = fds[1];
    player_pid_ = pid;
    spawn_count_.fetch_add(1, std::memory_order_acq_rel);
    spdlog::debug("[PcmPipeSound] player started (pid {})", pid);
    return true;
}

void PcmPipeSoundBackend::stop_player() {
    if (pipe_wr_ >= 0) {
        close(pipe_wr_);
        pipe_wr_ = -1;
    }
    if (player_pid_ <= 0)
        return;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReapTimeoutMs);
    while (waitpid(player_pid_, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            spdlog::warn("[PcmPipeSound] player (pid {}) did not exit after EOF; killing",
                         player_pid_);
            kill(player_pid_, SIGKILL);
            waitpid(player_pid_, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    spdlog::debug("[PcmPipeSound] player (pid {}) exited", player_pid_);
    player_pid_ = -1;
}

bool PcmPipeSoundBackend::write_all(const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    int stalled_ms = 0;
    while (len > 0) {
        ssize_t n = write(pipe_wr_, p, len);
        if (n > 0) {
            p += n;
            len -= static_cast<size_t>(n);
            stalled_ms = 0;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno != EAGAIN) {
            spdlog::debug("[PcmPipeSound] write: {}", std::strerror(errno));
            return false;
        }
        if (!running_.load(std::memory_order_relaxed))
            return false;
        pollfd pfd{pipe_wr_, POLLOUT, 0};
        int r = poll(&pfd, 1, kWritePollMs);
        if (r < 0 && errno != EINTR)
            return false;
        if (r == 0) {
            stalled_ms += kWritePollMs;
            if (stalled_ms >= kWriteStallMs) {
                spdlog::warn("[PcmPipeSound] player stopped reading for {} ms", stalled_ms);
                return false;
            }
        }
    }
    return true;
}

// ============================================================================
// Render thread
// ============================================================================

void PcmPipeSoundBackend::ack_pass() {
    const uint32_t seq = resume_seq_.load(std::memory_order_relaxed);
    if (resume_acked_.load(std::memory_order_relaxed) == seq)
        return;
    {
        std::lock_guard<std::mutex> lock(ack_mutex_);
        resume_acked_.store(seq, std::memory_order_relaxed);
    }
    ack_cv_.notify_all();
}

void PcmPipeSoundBackend::render_chunk(std::vector<float>& mix, std::vector<int16_t>& out) {
    const size_t frames = mix.size();
    std::memset(mix.data(), 0, frames * sizeof(float));
    const float sr = static_cast<float>(kSampleRate);

    {
        std::function<void(float*, size_t, int)> source;
        {
            std::lock_guard<std::mutex> lock(render_source_mutex_);
            source = render_source_;
        }
        if (source) {
            source(mix.data(), frames, kSampleRate);
            auto ft = filter_type_.load(std::memory_order_acquire);
            if (ft != helix::audio::FilterType::NONE) {
                float cutoff = filter_cutoff_.load(std::memory_order_relaxed);
                helix::audio::update_filter_if_needed(filter_, ft, cutoff, sr);
                helix::audio::apply_filter(filter_, mix.data(), static_cast<int>(frames));
            }
        }
    }

    for (int v = 0; v < kMaxVoices; ++v) {
        auto& slot = voice_slots_[v];
        slot.start_pending_note(sr);
        if (slot.active.velocity <= 0.001f && slot.current_amplitude <= 0.001f)
            continue;
        for (size_t i = 0; i < frames; ++i)
            mix[i] += slot.render_sample(sr);
    }

    for (size_t i = 0; i < frames; ++i)
        out[i] = static_cast<int16_t>(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f);
}

void PcmPipeSoundBackend::render_loop() {
    // Blocked, not ignored: a player that exits turns write() into EPIPE on
    // this thread instead of a process-wide SIGPIPE.
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &block, nullptr);

    std::vector<float> mix(kFramesPerChunk);
    std::vector<int16_t> out(kFramesPerChunk);

    while (running_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lock(state_mutex_);
            parked_.store(true, std::memory_order_release);
            state_cv_.wait(lock, [this] {
                return active_ || !running_.load(std::memory_order_relaxed);
            });
            parked_.store(false, std::memory_order_release);
        }
        if (!running_.load(std::memory_order_relaxed))
            break;

        bool failed = false;
        if (player_pid_ < 0 && !spawn_player()) {
            failed = true;
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            uint64_t frames = 0;
            bool healthy = false;
            for (;;) {
                bool stop = false;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    if (!running_.load(std::memory_order_relaxed)) {
                        stop = true;
                    } else if (suspend_requested_ &&
                               std::chrono::steady_clock::now() - suspend_at_ >=
                                   std::chrono::milliseconds(idle_grace_ms_)) {
                        suspend_requested_ = false;
                        active_ = false;
                        stop = true;
                    }
                }
                if (stop)
                    break;

                render_chunk(mix, out);
                if (!write_all(out.data(), out.size() * sizeof(int16_t))) {
                    failed = true;
                    break;
                }
                frames += kFramesPerChunk;
                if (!healthy && frames >= kHealthyFrames) {
                    healthy = true;
                    consecutive_failures_ = 0;
                }
                ack_pass();

                // Pace against our own clock: never more than kLeadMs of audio
                // ahead of wall time, so a note published mid-sound is heard
                // at most that late. Behind schedule, catch up without sleeping.
                const auto deadline =
                    t0 + std::chrono::microseconds(frames * 1000000ULL / kSampleRate) -
                    std::chrono::milliseconds(kLeadMs);
                std::this_thread::sleep_until(deadline);
            }
        }

        if (failed) {
            stop_player();
            if (++consecutive_failures_ >= kMaxConsecutiveFailures &&
                !disabled_.exchange(true)) {
                spdlog::warn("[PcmPipeSound] {} failed {} times in a row; sound disabled",
                             player_, consecutive_failures_);
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                active_ = false;
                suspend_requested_ = false;
            }
            ack_pass();
            continue;
        }
        if (!running_.load(std::memory_order_relaxed))
            break;
        // Idle grace over: EOF lets the player drain its buffer and exit.
        stop_player();
    }
    stop_player();
}

// ============================================================================
// Sequencer side
// ============================================================================

void PcmPipeSoundBackend::suspend() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!active_ || suspend_requested_)
        return;
    suspend_requested_ = true;
    suspend_at_ = std::chrono::steady_clock::now();
}

void PcmPipeSoundBackend::resume() {
    if (disabled_.load(std::memory_order_acquire))
        return;
    uint32_t seq;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        suspend_requested_ = false;
        if (active_)
            return; // player still alive from the idle grace: no spawn
        active_ = true;
        seq = resume_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    state_cv_.notify_all();
    if (!running_.load(std::memory_order_relaxed))
        return;
    // Synchronous handoff, as the ALSA backend does: the sequencer starts its
    // step clock when this returns, so wait for one rendered chunk (or the
    // spawn failing) before it publishes the next note over the first.
    std::unique_lock<std::mutex> lock(ack_mutex_);
    const bool handed_off =
        ack_cv_.wait_for(lock, std::chrono::milliseconds(kResumeHandoffMs), [&] {
            return resume_acked_.load(std::memory_order_relaxed) >= seq ||
                   !running_.load(std::memory_order_relaxed);
        });
    if (!handed_off)
        spdlog::debug("[PcmPipeSound] resume handoff timed out");
}

// ============================================================================
// Voices
// ============================================================================

void PcmPipeSoundBackend::set_tone(float freq_hz, float amplitude, float duty_cycle) {
    set_voice(0, freq_hz, amplitude, duty_cycle);
}

void PcmPipeSoundBackend::silence() {
    for (int v = 0; v < kMaxVoices; ++v)
        voice_slots_[v].edit([](NoteEvent& ev) { ev.velocity = 0; }, true);
}

void PcmPipeSoundBackend::set_waveform(Waveform w) {
    set_voice_waveform(0, w);
}

void PcmPipeSoundBackend::set_voice(int slot, float freq_hz, float amplitude, float duty_cycle) {
    if (slot < 0 || slot >= kMaxVoices)
        return;
    voice_slots_[slot].edit(
        [&](NoteEvent& ev) {
            ev.freq_hz = freq_hz;
            ev.velocity = amplitude;
            ev.duty_cycle = duty_cycle;
        },
        true);
}

void PcmPipeSoundBackend::set_voice_waveform(int slot, Waveform w) {
    if (slot < 0 || slot >= kMaxVoices)
        return;
    voice_slots_[slot].edit([w](NoteEvent& ev) { ev.wave = w; }, false);
}

void PcmPipeSoundBackend::silence_voice(int slot) {
    if (slot < 0 || slot >= kMaxVoices)
        return;
    voice_slots_[slot].edit([](NoteEvent& ev) { ev.velocity = 0; }, true);
}

void PcmPipeSoundBackend::publish_note(int slot, const NoteEvent& event) {
    if (slot < 0 || slot >= kMaxVoices)
        return;
    voice_slots_[slot].publish(event);
}

void PcmPipeSoundBackend::set_render_source(std::function<void(float*, size_t, int)> fn) {
    std::lock_guard<std::mutex> lock(render_source_mutex_);
    render_source_ = std::move(fn);
}

void PcmPipeSoundBackend::clear_render_source() {
    std::lock_guard<std::mutex> lock(render_source_mutex_);
    render_source_ = nullptr;
}

void PcmPipeSoundBackend::set_filter(const std::string& type, float cutoff) {
    if (type.empty()) {
        filter_type_.store(helix::audio::FilterType::NONE, std::memory_order_relaxed);
        return;
    }
    auto ft = helix::audio::filter_type_from_string(type);
    bool type_changed = (ft != filter_type_.load(std::memory_order_relaxed));
    filter_cutoff_.store(cutoff, std::memory_order_relaxed);
    helix::audio::compute_biquad_coeffs(filter_, ft, cutoff, static_cast<float>(kSampleRate));
    if (type_changed) {
        filter_.z1 = 0;
        filter_.z2 = 0;
    }
    filter_type_.store(ft, std::memory_order_release);
}
