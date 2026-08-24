// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

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
