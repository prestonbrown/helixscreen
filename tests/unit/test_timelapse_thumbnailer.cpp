// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_thumbnailer.h"

#include <algorithm>
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

namespace {

ptrdiff_t position(const std::vector<std::string>& args, const std::string& word) {
    auto it = std::find(args.begin(), args.end(), word);
    return it == args.end() ? -1 : it - args.begin();
}

bool pair_at(const std::vector<std::string>& args, ptrdiff_t at, const std::string& flag,
             const std::string& value) {
    return at >= 0 && static_cast<size_t>(at) + 1 < args.size() &&
           args[static_cast<size_t>(at)] == flag && args[static_cast<size_t>(at) + 1] == value;
}

} // namespace

TEST_CASE("Thumbnailer: ffmpeg argument list construction", "[timelapse][thumbnailer]") {
    auto args = ffmpeg_extract_args("/home/pi/printer_data/timelapse/benchy.mp4",
                                    "/home/pi/printer_data/timelapse/benchy.thumb.jpg",
                                    ExtractFrame::First);

    REQUIRE(args.front() == "ffmpeg");
    CHECK(args[1] == "-y");
    const auto in = position(args, "-i");
    REQUIRE(in > 0);
    CHECK(args[static_cast<size_t>(in) + 1] == "/home/pi/printer_data/timelapse/benchy.mp4");
    CHECK(pair_at(args, position(args, "-vframes"), "-vframes", "1"));
    CHECK(pair_at(args, position(args, "-q:v"), "-q:v", "3"));
    // Small enough that pre-scaling it is a cheap decode.
    CHECK(pair_at(args, position(args, "-vf"), "-vf", "scale=480:-2"));
    CHECK(args.back() == "/home/pi/printer_data/timelapse/benchy.thumb.jpg");
    CHECK(position(args, "-sseof") < 0);
}

TEST_CASE("Thumbnailer: extraction runs as lean as the player's decoder",
          "[timelapse][thumbnailer]") {
    for (const auto frame : {ExtractFrame::First, ExtractFrame::Last}) {
        const auto args = ffmpeg_extract_args("/v/a.mkv", "/c/a.jpg", frame);
        const auto in = position(args, "-i");
        REQUIRE(in > 0);
        // The shared input options, in order, before -i.
        const auto lean_in = ffmpeg_lean_input_args();
        const auto at = std::search(args.begin(), args.end(), lean_in.begin(), lean_in.end());
        REQUIRE(at != args.end());
        CHECK(at - args.begin() < in);
        // The shared output options just before the output file.
        const auto lean_out = ffmpeg_lean_output_args();
        REQUIRE(args.size() > lean_out.size() + 1);
        CHECK(std::equal(lean_out.begin(), lean_out.end(),
                         args.end() - 1 - static_cast<ptrdiff_t>(lean_out.size())));
    }
    // What those options are: one thread everywhere and a small probe.
    const auto lean_in = ffmpeg_lean_input_args();
    CHECK(pair_at(lean_in, position(lean_in, "-threads"), "-threads", "1"));
    CHECK(pair_at(lean_in, position(lean_in, "-filter_threads"), "-filter_threads", "1"));
    CHECK(position(lean_in, "-probesize") >= 0);
    CHECK(position(lean_in, "nobuffer") < 0);
    CHECK(ffmpeg_lean_output_args() == std::vector<std::string>{"-threads", "1"});
}

TEST_CASE("Thumbnailer: the last frame seeks from the end before the input",
          "[timelapse][thumbnailer]") {
    auto args = ffmpeg_extract_args("/v/a.mp4", "/c/a.jpg", ExtractFrame::Last);

    const auto sseof = position(args, "-sseof");
    const auto in = position(args, "-i");
    REQUIRE(sseof > 0);
    CHECK(pair_at(args, sseof, "-sseof", "-1"));
    CHECK(sseof < in);
    CHECK(args[static_cast<size_t>(in) + 1] == "/v/a.mp4");
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
    const auto in = position(args, "-i");
    REQUIRE(in > 0);
    REQUIRE(args[static_cast<size_t>(in) + 1] == "/tmp/$(rm -rf /).mp4");
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
