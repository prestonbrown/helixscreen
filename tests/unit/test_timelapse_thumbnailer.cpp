// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::timelapse;

TEST_CASE("Thumbnailer: companion thumbnail lookup", "[timelapse][thumbnailer]") {
    SECTION("moonraker-timelapse preview <name>.jpg is used") {
        const std::set<std::string> listing = {"timelapse__20261111_0506.mp4",
                                               "timelapse__20261111_0506.jpg"};
        REQUIRE(find_companion_thumbnail("timelapse__20261111_0506.mp4", listing) ==
                "timelapse__20261111_0506.jpg");
    }

    SECTION(".thumb.jpg companion is still found") {
        const std::set<std::string> listing = {"benchy.mp4", "benchy.thumb.jpg"};
        REQUIRE(find_companion_thumbnail("benchy.mp4", listing) == "benchy.thumb.jpg");
    }

    SECTION("the plugin preview wins when both exist") {
        const std::set<std::string> listing = {"benchy.mp4", "benchy.jpg", "benchy.thumb.jpg"};
        REQUIRE(find_companion_thumbnail("benchy.mp4", listing) == "benchy.jpg");
    }

    SECTION("no companion gives an empty name") {
        const std::set<std::string> listing = {"benchy.mp4", "other.jpg"};
        REQUIRE(find_companion_thumbnail("benchy.mp4", listing).empty());
    }

    SECTION("a dotted stem keeps every dot but the extension's") {
        const std::set<std::string> listing = {"a.b.mp4", "a.jpg", "a.b.jpg"};
        REQUIRE(find_companion_thumbnail("a.b.mp4", listing) == "a.b.jpg");
    }

    SECTION("a name without an extension") {
        const std::set<std::string> listing = {"clip", "clip.jpg"};
        REQUIRE(find_companion_thumbnail("clip", listing) == "clip.jpg");
    }
}

TEST_CASE("Thumbnailer: ffmpeg argument list construction", "[timelapse][thumbnailer]") {
    auto args = ffmpeg_extract_args("/home/pi/printer_data/timelapse/benchy.mp4",
                                    "/home/pi/printer_data/timelapse/benchy.thumb.jpg",
                                    ExtractFrame::First);

    REQUIRE(args.size() == 11);
    REQUIRE(args[0] == "ffmpeg");
    REQUIRE(args[1] == "-y");
    REQUIRE(args[2] == "-i");
    REQUIRE(args[3] == "/home/pi/printer_data/timelapse/benchy.mp4");
    REQUIRE(args[4] == "-vframes");
    REQUIRE(args[5] == "1");
    REQUIRE(args[6] == "-q:v");
    REQUIRE(args[7] == "3");
    // Small enough that pre-scaling it is a cheap decode.
    REQUIRE(args[8] == "-vf");
    REQUIRE(args[9] == "scale=480:-2");
    REQUIRE(args[10] == "/home/pi/printer_data/timelapse/benchy.thumb.jpg");
}

TEST_CASE("Thumbnailer: the last frame seeks from the end before the input",
          "[timelapse][thumbnailer]") {
    auto args = ffmpeg_extract_args("/v/a.mp4", "/c/a.jpg", ExtractFrame::Last);

    REQUIRE(args.size() == 13);
    CHECK(args[0] == "ffmpeg");
    CHECK(args[1] == "-y");
    CHECK(args[2] == "-sseof");
    CHECK(args[3] == "-1");
    CHECK(args[4] == "-i");
    CHECK(args[5] == "/v/a.mp4");
    CHECK(args[6] == "-vframes");
    CHECK(args[7] == "1");
    CHECK(args.back() == "/c/a.jpg");
}

TEST_CASE("Thumbnailer: an extracted thumbnail is keyed by name, size and time",
          "[timelapse][thumbnailer]") {
    const std::string key = extracted_thumbnail_key("a.mp4", 1000, 1772985600.0);
    CHECK(key == extracted_thumbnail_key("a.mp4", 1000, 1772985600.0));
    CHECK(key != extracted_thumbnail_key("b.mp4", 1000, 1772985600.0));
    CHECK(key != extracted_thumbnail_key("a.mp4", 1001, 1772985600.0));
    CHECK(key != extracted_thumbnail_key("a.mp4", 1000, 1772985700.0));
    CHECK(key != "a.mp4"); // never collides with a companion's cache key
}

TEST_CASE("Thumbnailer: a frame is extracted only for a local video with no preview",
          "[timelapse][thumbnailer]") {
    ThumbnailExtractInputs in;
    in.has_companion = false;
    in.ffmpeg_available = true;
    in.moonraker_local = true;
    in.print_active = false;
    in.already_failed = false;
    CHECK(should_extract_thumbnail(in));

    SECTION("a companion preview wins") {
        in.has_companion = true;
        CHECK_FALSE(should_extract_thumbnail(in));
    }
    SECTION("no ffmpeg") {
        in.ffmpeg_available = false;
        CHECK_FALSE(should_extract_thumbnail(in));
    }
    SECTION("a remote Moonraker: the video is not on this disk") {
        in.moonraker_local = false;
        CHECK_FALSE(should_extract_thumbnail(in));
    }
    SECTION("a print is running") {
        in.print_active = true;
        CHECK_FALSE(should_extract_thumbnail(in));
    }
    SECTION("this file already failed") {
        in.already_failed = true;
        CHECK_FALSE(should_extract_thumbnail(in));
    }
}

TEST_CASE("Thumbnailer: ffmpeg args are safe from shell injection", "[timelapse][thumbnailer]") {
    auto args = ffmpeg_extract_args("/tmp/$(rm -rf /).mp4", "/tmp/out.jpg", ExtractFrame::First);

    // The malicious filename is passed as a single argument element, not shell-interpreted
    REQUIRE(args[3] == "/tmp/$(rm -rf /).mp4");
}

TEST_CASE("Thumbnailer: video file filtering", "[timelapse][thumbnailer]") {
    REQUIRE(is_video_file("benchy.mp4") == true);
    REQUIRE(is_video_file("print.mkv") == true);
    REQUIRE(is_video_file("timelapse.avi") == true);
    REQUIRE(is_video_file("thumbnail.jpg") == false);
    REQUIRE(is_video_file("benchy.thumb.jpg") == false);
    REQUIRE(is_video_file("config.cfg") == false);
    REQUIRE(is_video_file("") == false);
}
