// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

#include "core/text_io.h"
#include "host_identity.h"
#include "hv/hurl.h"

#include <algorithm>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace helix::timelapse {

std::string find_companion_thumbnail(const std::string& video_filename,
                                     const std::set<std::string>& listing) {
    const auto dot = video_filename.rfind('.');
    const std::string stem =
        dot == std::string::npos ? video_filename : video_filename.substr(0, dot);
    for (const char* suffix : {".jpg", ".thumb.jpg"}) {
        const std::string candidate = stem + suffix;
        if (listing.count(candidate) != 0) {
            return candidate;
        }
    }
    return {};
}

std::vector<std::string> ffmpeg_extract_args(const std::string& input_path,
                                             const std::string& output_path) {
    // 480px wide: a full-resolution frame would cost every pre-scale a
    // multi-megabyte decode on a worker.
    return {"ffmpeg", "-y", "-i",  input_path,     "-vframes", "1",
            "-q:v",   "3",  "-vf", "scale=480:-2", output_path};
}

bool is_video_file(const std::string& filename) {
    if (filename.size() < 5)
        return false;
    if (filename.find(".thumb.") != std::string::npos)
        return false;

    auto dot = filename.rfind('.');
    if (dot == std::string::npos)
        return false;
    auto ext = filename.substr(dot);
    ext = helix::text_io::to_lower(ext);
    return ext == ".mp4" || ext == ".mkv" || ext == ".avi";
}

std::vector<std::string> build_player_args(const std::string& player,
                                           const std::string& file_path) {
    if (player == "mpv") {
        return {"mpv", "--fs", "--keep-open=no", file_path};
    }
    return {"ffplay", "-autoexit", "-exitonmousedown", "-fs", file_path};
}

VideoOpenAction choose_video_open_action(bool can_play, const std::string& phone_url) {
    if (can_play) {
        return VideoOpenAction::Play;
    }
    return phone_url.empty() ? VideoOpenAction::NoPlayerToast : VideoOpenAction::PhoneQr;
}

std::string phone_video_url(const std::string& http_base_url, const std::string& filename,
                            const std::string& lan_ipv4) {
    const std::string host = helix::extract_host_from_url(http_base_url);
    if (host.empty()) {
        return {};
    }
    std::string base = http_base_url;
    if (helix::is_loopback_host(host)) {
        if (lan_ipv4.empty()) {
            return {};
        }
        const auto authority = base.find("://") + 3;
        const bool bracketed = base.compare(authority, 1, "[") == 0;
        const auto span = host.size() + (bracketed ? 2 : 0);
        base.replace(authority, span, lan_ipv4);
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/server/files/timelapse/" + HUrl::escape(filename, "/.-_");
}

std::optional<PlayerWatch> spawn_watched_player(const std::vector<std::string>& args,
                                                int window_ms) {
    if (args.empty()) {
        return std::nullopt;
    }
    // Built before fork: the children may only make async-signal-safe calls.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        return std::nullopt;
    }
    pid_t watcher = fork();
    if (watcher < 0) {
        close(fds[0]);
        close(fds[1]);
        return std::nullopt;
    }
    if (watcher == 0) {
        close(fds[0]);
        pid_t player = fork();
        if (player < 0) {
            _exit(127);
        }
        if (player == 0) {
            // O_CLOEXEC closes the pipe in the player once exec succeeds.
            execvp(argv[0], const_cast<char* const*>(argv.data()));
            _exit(127);
        }
        constexpr int kPollMs = 20;
        for (int waited = 0; waited < window_ms; waited += kPollMs) {
            int status = 0;
            if (waitpid(player, &status, WNOHANG) == player) {
                ssize_t ignored = write(fds[1], &status, sizeof(status));
                (void)ignored;
                _exit(0);
            }
            struct timespec ts = {0, kPollMs * 1000L * 1000L};
            nanosleep(&ts, nullptr);
        }
        _exit(0);
    }
    close(fds[1]);
    return PlayerWatch{watcher, fds[0]};
}

PlayerWatchPoll poll_player_watch(PlayerWatch& watch) {
    if (watch.watcher_pid < 0) {
        return {PlayerWatchState::Running, 0};
    }
    int watcher_status = 0;
    if (waitpid(watch.watcher_pid, &watcher_status, WNOHANG) == 0) {
        return {PlayerWatchState::Pending, 0};
    }
    // The watcher has exited, so anything it wrote is already in the pipe.
    PlayerWatchPoll poll{PlayerWatchState::Running, 0};
    int status = 0;
    if (read(watch.status_fd, &status, sizeof(status)) == static_cast<ssize_t>(sizeof(status))) {
        poll = {PlayerWatchState::Exited, status};
    }
    close(watch.status_fd);
    watch = PlayerWatch{};
    return poll;
}

bool player_exit_failed(int wait_status) {
    return !(WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0);
}

std::string describe_wait_status(int wait_status) {
    if (WIFSIGNALED(wait_status)) {
        return "signal " + std::to_string(WTERMSIG(wait_status));
    }
    return "exit " + std::to_string(WEXITSTATUS(wait_status));
}

} // namespace helix::timelapse
