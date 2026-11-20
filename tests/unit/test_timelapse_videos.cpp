// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix::timelapse;

TEST_CASE("Playback: argument list construction", "[timelapse][videos][playback]") {
    SECTION("mpv arguments") {
        auto args = build_player_args("mpv", "/tmp/video.mp4");
        REQUIRE(args.size() == 4);
        REQUIRE(args[0] == "mpv");
        REQUIRE(args[1] == "--fs");
        REQUIRE(args[2] == "--keep-open=no");
        REQUIRE(args[3] == "/tmp/video.mp4");
    }

    SECTION("ffplay arguments") {
        auto args = build_player_args("ffplay", "/tmp/video.mp4");
        REQUIRE(args.size() == 5);
        REQUIRE(args[0] == "ffplay");
        REQUIRE(args[1] == "-autoexit");
        REQUIRE(args[2] == "-exitonmousedown");
        REQUIRE(args[3] == "-fs");
        REQUIRE(args[4] == "/tmp/video.mp4");
    }

    SECTION("unknown player defaults to ffplay") {
        auto args = build_player_args("unknown", "/tmp/video.mp4");
        REQUIRE(args[0] == "ffplay");
    }
}

TEST_CASE("Playback: args are safe from shell injection", "[timelapse][videos][playback]") {
    auto args = build_player_args("mpv", "/tmp/$(rm -rf /).mp4");
    // Malicious path is a single argument, never shell-interpreted
    REQUIRE(args.back() == "/tmp/$(rm -rf /).mp4");
}

TEST_CASE("Phone URL: a LAN-facing Moonraker URL is used as configured",
          "[timelapse][videos][phone]") {
    CHECK(phone_video_url("http://192.168.1.20:7125", "benchy.mp4", "10.0.0.9") ==
          "http://192.168.1.20:7125/server/files/timelapse/benchy.mp4");
    CHECK(phone_video_url("http://printer.local:7125/", "benchy.mp4", "") ==
          "http://printer.local:7125/server/files/timelapse/benchy.mp4");
}

TEST_CASE("Phone URL: a loopback Moonraker is reached on the LAN address",
          "[timelapse][videos][phone]") {
    CHECK(phone_video_url("http://127.0.0.1:7125", "a.mp4", "192.168.1.50") ==
          "http://192.168.1.50:7125/server/files/timelapse/a.mp4");
    CHECK(phone_video_url("http://localhost:7125", "a.mp4", "192.168.1.50") ==
          "http://192.168.1.50:7125/server/files/timelapse/a.mp4");
    CHECK(phone_video_url("http://[::1]:7125", "a.mp4", "192.168.1.50") ==
          "http://192.168.1.50:7125/server/files/timelapse/a.mp4");
    CHECK(phone_video_url("http://localhost", "a.mp4", "192.168.1.50") ==
          "http://192.168.1.50/server/files/timelapse/a.mp4");
}

TEST_CASE("Phone URL: no address a phone could reach gives no URL", "[timelapse][videos][phone]") {
    CHECK(phone_video_url("http://127.0.0.1:7125", "a.mp4", "").empty());
    CHECK(phone_video_url("", "a.mp4", "192.168.1.50").empty());
    CHECK(phone_video_url("not a url", "a.mp4", "192.168.1.50").empty());
}

TEST_CASE("Phone URL: the file name is percent-encoded", "[timelapse][videos][phone]") {
    CHECK(phone_video_url("http://192.168.1.20:7125", "my print #2 & co.mp4", "") ==
          "http://192.168.1.20:7125/server/files/timelapse/my%20print%20%232%20%26%20co.mp4");
}

TEST_CASE("Video open: a player plays, else the phone QR, else a toast",
          "[timelapse][videos][phone]") {
    CHECK(choose_video_open_action(true, "http://x/a.mp4") == VideoOpenAction::Play);
    CHECK(choose_video_open_action(true, "") == VideoOpenAction::Play);
    CHECK(choose_video_open_action(false, "http://x/a.mp4") == VideoOpenAction::PhoneQr);
    CHECK(choose_video_open_action(false, "") == VideoOpenAction::NoPlayerToast);
}

TEST_CASE("Player exit: only a clean exit is a success", "[timelapse][videos][playback]") {
    const int clean = std::system("exit 0");
    const int failed = std::system("exit 3");
    const int killed = std::system("kill -9 $$");
    CHECK_FALSE(player_exit_failed(clean));
    CHECK(player_exit_failed(failed));
    CHECK(player_exit_failed(killed));
    CHECK(describe_wait_status(failed) == "exit 3");
    CHECK(describe_wait_status(killed) == "signal 9");
}

namespace {

/// Poll until the watch settles; the watcher reports within its window.
PlayerWatchPoll settle(PlayerWatch& watch) {
    for (int i = 0; i < 100; ++i) {
        PlayerWatchPoll poll = poll_player_watch(watch);
        if (poll.state != PlayerWatchState::Pending) {
            return poll;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return {PlayerWatchState::Pending, 0};
}

} // namespace

TEST_CASE("Player watch: a player that dies at once reports its status",
          "[timelapse][videos][playback]") {
    SECTION("non-zero exit") {
        auto watch = spawn_watched_player({"sh", "-c", "exit 7"}, 1000);
        REQUIRE(watch);
        PlayerWatchPoll poll = settle(*watch);
        REQUIRE(poll.state == PlayerWatchState::Exited);
        CHECK(describe_wait_status(poll.wait_status) == "exit 7");
    }
    SECTION("a binary that does not exist") {
        auto watch = spawn_watched_player({"helix-no-such-player-xyz"}, 1000);
        REQUIRE(watch);
        PlayerWatchPoll poll = settle(*watch);
        REQUIRE(poll.state == PlayerWatchState::Exited);
        CHECK(describe_wait_status(poll.wait_status) == "exit 127");
    }
}

TEST_CASE("Player watch: a player still running after the window is left to run",
          "[timelapse][videos][playback]") {
    auto watch = spawn_watched_player({"sleep", "1"}, 200);
    REQUIRE(watch);
    PlayerWatchPoll poll = settle(*watch);
    CHECK(poll.state == PlayerWatchState::Running);
}

TEST_CASE("Player watch: a watcher that is not ours to reap settles without blocking",
          "[timelapse][videos][playback]") {
    // waitpid() fails with ECHILD for a pid that is not our child, while the
    // pipe's write end stays open: a blocking read would hang here.
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    PlayerWatch watch{getpid(), fds[0]};
    PlayerWatchPoll poll = poll_player_watch(watch);
    CHECK(poll.state == PlayerWatchState::Running);
    CHECK(watch.status_fd == -1);
    close(fds[1]);
}

TEST_CASE("Player watch: a clean quick exit is reported as a clean exit",
          "[timelapse][videos][playback]") {
    auto watch = spawn_watched_player({"true"}, 1000);
    REQUIRE(watch);
    PlayerWatchPoll poll = settle(*watch);
    REQUIRE(poll.state == PlayerWatchState::Exited);
    CHECK_FALSE(player_exit_failed(poll.wait_status));
}
