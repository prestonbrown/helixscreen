// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_player_decoder.h"

#if HELIX_HAS_TIMELAPSE_VIEWER

#include "cloexec_pipe.h"
#include "helix_thread.h"
#include "system/child_process.h"
#include "system/process_runner.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace helix::timelapse {

namespace {

using Clock = std::chrono::steady_clock;

// A local file probes in well under a second; this bounds a stuck one.
constexpr int kProbeTimeoutSec = 10;
// How long a read waits before the worker looks at stop, pause and seek again.
constexpr int kReadPollMs = 50;
// ffmpeg exits on SIGTERM at once; the grace covers a loaded board.
constexpr auto kReapGrace = std::chrono::milliseconds(500);
constexpr auto kReapPoll = std::chrono::milliseconds(5);
// The decoder yields to Klipper and the UI.
constexpr int kDecoderNice = 10;

struct Child {
    pid_t pid = -1;
    int fd = -1;
};

/// Start @p args with stdout on a pipe, stdin and stderr on /dev/null.
Child spawn_reader(const std::vector<std::string>& args) {
    if (args.empty()) {
        return {};
    }
    // Built before fork: the child may only make async-signal-safe calls.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

    int fds[2];
    if (!helix::open_cloexec_pipe(fds)) {
        return {};
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return {};
    }
    if (pid == 0) {
#ifdef __linux__
        // A crashed UI must not leave ffmpeg decoding into a dead pipe.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        dup2(fds[1], STDOUT_FILENO);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        setpriority(PRIO_PROCESS, 0, kDecoderNice);
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    close(fds[1]);
    return {pid, fds[0]};
}

} // namespace

std::optional<VideoInfo> probe_video(const std::string& path, const std::atomic<bool>& cancel) {
    const ProcessRunner run = cancellable_process_runner(cancel);
    const CommandResult probe = run(ffprobe_info_args(path), kProbeTimeoutSec);
    if (probe.succeeded()) {
        if (auto info = parse_ffprobe_info(probe.output)) {
            return info;
        }
    }
    if (cancel.load()) {
        return std::nullopt;
    }
    // No ffprobe (or it could not read the file): ffmpeg describes its input
    // before it complains that no output was named.
    const CommandResult header = run(ffmpeg_header_args(path), kProbeTimeoutSec);
    return parse_ffmpeg_header(header.output);
}

PlayerDecoder::~PlayerDecoder() {
    stop();
}

bool PlayerDecoder::start(Config config, Events events) {
    if (worker_.joinable()) {
        return false;
    }
    config_ = std::move(config);
    events_ = std::move(events);
    stop_.store(false);
    paused_ = false;
    paused_at_s_ = 0.0;
    pending_shift_s_ = 0.0;
    seek_to_.reset();
    exchange_.clear();
    worker_ = helix::make_thread([this] { run(); });
    return true;
}

void PlayerDecoder::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!paused_) {
        paused_ = true;
        paused_at_s_ = monotonic_seconds();
    }
}

void PlayerDecoder::resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (paused_) {
            // The clock, and every frame already queued, pick up where the pause left them.
            const double paused_for = monotonic_seconds() - paused_at_s_;
            pending_shift_s_ += paused_for;
            exchange_.shift_due(paused_for);
        }
        paused_ = false;
    }
    cv_.notify_all();
}

void PlayerDecoder::seek(double seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seek_to_ = std::max(0.0, seconds);
    }
    cv_.notify_all();
}

void PlayerDecoder::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true);
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool PlayerDecoder::wait_while_paused(std::unique_lock<std::mutex>& lock) {
    cv_.wait(lock, [this] { return !paused_ || stop_.load() || seek_to_.has_value(); });
    return stop_.load() || seek_to_.has_value();
}

void PlayerDecoder::run() {
    std::optional<VideoInfo> info =
        config_.probe ? config_.probe(stop_) : probe_video(config_.path, stop_);
    if (stop_.load()) {
        return;
    }
    const FrameSize out = info ? fit_frame({info->width, info->height}, config_.box) : FrameSize{};
    if (!info || out.empty()) {
        spdlog::warn("[TimelapsePlayer] Could not read the video size of {}", config_.path);
        if (events_.on_end) {
            events_.on_end(true);
        }
        return;
    }
    const double fps = effective_fps(info->fps);
    spdlog::info("[TimelapsePlayer] {}: {}x{} at {:.2f} fps, {:.1f}s; decoding at {}x{} {}",
                 config_.path, info->width, info->height, fps, info->duration_s, out.width,
                 out.height, ffmpeg_pix_fmt_name(config_.format));
    if (events_.on_info) {
        events_.on_info(*info, out);
    }

    double start_s = 0.0;
    bool ever_decoded = false;
    while (!stop_.load()) {
        bool got_frame = false;
        const bool interrupted = decode_from(start_s, fps, out, got_frame);
        ever_decoded = ever_decoded || got_frame;
        if (stop_.load()) {
            break;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (!interrupted) {
            lock.unlock();
            if (events_.on_end) {
                events_.on_end(!ever_decoded);
            }
            lock.lock();
            // Ended: hold the last frame until a replay (a seek) or stop.
            cv_.wait(lock, [this] { return stop_.load() || seek_to_.has_value(); });
        }
        if (seek_to_) {
            start_s = *seek_to_;
            seek_to_.reset();
            paused_ = false;
            exchange_.clear();
        }
    }
}

bool PlayerDecoder::decode_from(double start_s, double fps, FrameSize out, bool& got_frame) {
    const auto args = config_.decode_argv
                          ? config_.decode_argv(start_s, out)
                          : ffmpeg_decode_args(config_.path, start_s, out, config_.format);
    Child child = spawn_reader(args);
    if (child.pid < 0) {
        spdlog::error("[TimelapsePlayer] Could not start {}: {}", args.empty() ? "" : args[0],
                      strerror(errno));
        return false;
    }
    child_pid_.store(child.pid);

    FrameAssembler assembler(frame_bytes(out, config_.format));
    const auto base_index = static_cast<int64_t>(std::llround(start_s * fps));
    // Each frame goes to the UI one period before it is due, so the
    // presentation timer finds it waiting when its moment comes.
    const double lead_s = 1.0 / fps;
    int64_t n = 0;
    double anchor_s = monotonic_seconds();
    bool interrupted = false;

    // Wait out a pause; true when the run must stop reading (stop or seek).
    // Takes up any time the clock was paused for.
    auto settle = [&](std::unique_lock<std::mutex>& lock) {
        if (paused_ && wait_while_paused(lock)) {
            return true;
        }
        if (stop_.load() || seek_to_) {
            return true;
        }
        anchor_s += pending_shift_s_;
        pending_shift_s_ = 0.0;
        return false;
    };

    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (settle(lock)) {
                interrupted = true;
                break;
            }
        }

        pollfd pfd{child.fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, kReadPollMs);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        const ssize_t got = ready > 0 ? read(child.fd, assembler.tail(), assembler.want()) : -1;
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break; // end of stream, or the child is gone
        }
        if (!assembler.commit(static_cast<size_t>(got))) {
            continue;
        }

        // Hold the frame until its hand-over time, waking for stop, seek and pause.
        while (!interrupted) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (settle(lock)) {
                interrupted = true;
                break;
            }
            const PaceDecision pace = pace_frame(n, fps, monotonic_seconds() - anchor_s, lead_s);
            if (pace.reanchor) {
                // Decoding fell behind: this frame is due now, not in the past.
                anchor_s = monotonic_seconds() - static_cast<double>(n) / fps;
            }
            if (pace.wait_s <= 0.0) {
                break;
            }
            cv_.wait_for(lock, std::chrono::duration<double>(pace.wait_s),
                         [this] { return stop_.load() || seek_to_.has_value() || paused_; });
        }
        if (interrupted) {
            break;
        }
        exchange_.publish(assembler.frame(), out, base_index + n,
                          anchor_s + static_cast<double>(n) / fps);
        got_frame = true;
        ++n;
    }

    close(child.fd);
    // The pipe is closed, so a child still writing dies of SIGPIPE. One that
    // was cut off gets SIGTERM now, one at its own end of stream a grace to
    // exit first, and either gets SIGKILL after that.
    const auto deadline = interrupted ? Clock::now() : Clock::now() + kReapGrace;
    const ChildWaitResult reaped = terminate_and_reap(child.pid, deadline, kReapGrace, kReapPoll);
    child_pid_.store(-1);
    if (reaped.outcome == ChildWaitOutcome::WaitFailed) {
        spdlog::warn("[TimelapsePlayer] Could not reap decoder {}: {}", child.pid,
                     strerror(reaped.wait_errno));
    }
    spdlog::debug("[TimelapsePlayer] Decoder {} done after {} frames{}", child.pid, n,
                  interrupted ? " (interrupted)" : "");
    return interrupted;
}

} // namespace helix::timelapse

#endif // HELIX_HAS_TIMELAPSE_VIEWER
