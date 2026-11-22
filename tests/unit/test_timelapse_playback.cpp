// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_playback.h"

#include <algorithm>
#include <numeric>

#include "../catch_amalgamated.hpp"

using namespace helix::timelapse;

// --- Routing ---

static VideoOpenInputs inputs(bool ffmpeg, bool board, bool local, std::string url) {
    VideoOpenInputs in;
    in.ffmpeg_available = ffmpeg;
    in.board_can_decode = board;
    in.moonraker_local = local;
    in.phone_url = std::move(url);
    return in;
}

TEST_CASE("Video open: ffmpeg on a capable board plays in the app",
          "[timelapse][player][routing]") {
    CHECK(choose_video_open_action(inputs(true, true, true, "")) == VideoOpenAction::PlayInApp);
    CHECK(choose_video_open_action(inputs(true, true, true, "http://x/v.mp4")) ==
          VideoOpenAction::PlayInApp);
}

TEST_CASE("Video open: a remote Moonraker's video is downloaded, then played in the app",
          "[timelapse][player][routing]") {
    CHECK(choose_video_open_action(inputs(true, true, false, "http://x/v.mp4")) ==
          VideoOpenAction::DownloadThenPlay);
}

TEST_CASE("Video open: without ffmpeg or on a weak board, the phone QR, else a toast",
          "[timelapse][player][routing]") {
    CHECK(choose_video_open_action(inputs(false, true, true, "http://x/v.mp4")) ==
          VideoOpenAction::PhoneQr);
    CHECK(choose_video_open_action(inputs(true, false, true, "http://x/v.mp4")) ==
          VideoOpenAction::PhoneQr);
    CHECK(choose_video_open_action(inputs(false, true, true, "")) ==
          VideoOpenAction::NoPlayerToast);
    CHECK(choose_video_open_action(inputs(true, false, false, "")) ==
          VideoOpenAction::NoPlayerToast);
}

TEST_CASE("Video open: the card badge follows the action", "[timelapse][player][routing]") {
    CHECK(tap_badge_for(VideoOpenAction::PlayInApp) == 1);
    CHECK(tap_badge_for(VideoOpenAction::DownloadThenPlay) == 1);
    CHECK(tap_badge_for(VideoOpenAction::PhoneQr) == 2);
    CHECK(tap_badge_for(VideoOpenAction::NoPlayerToast) == 0);
}

TEST_CASE("Video open: embedded-class boards do not decode video", "[timelapse][player][routing]") {
    CHECK(inapp_player_supported(helix::PlatformTier::STANDARD));
    CHECK(inapp_player_supported(helix::PlatformTier::BASIC));
    CHECK_FALSE(inapp_player_supported(helix::PlatformTier::EMBEDDED));
}

TEST_CASE("Video open: during a print a tap goes to the phone, as thumbnails wait",
          "[timelapse][player][routing]") {
    auto printing = [](bool local, std::string url) {
        VideoOpenInputs in = inputs(true, true, local, std::move(url));
        in.print_active = true;
        return choose_video_open_action(in);
    };
    CHECK(printing(true, "http://x/v.mp4") == VideoOpenAction::PhoneQr);
    CHECK(printing(false, "http://x/v.mp4") == VideoOpenAction::PhoneQr);
    CHECK(printing(true, "") == VideoOpenAction::NoPlayerToast);
}

// --- Probe parsing ---

TEST_CASE("Probe: ffprobe's key=value output gives size, rate and duration",
          "[timelapse][player][probe]") {
    const auto info = parse_ffprobe_info("width=1280\nheight=720\nr_frame_rate=30/1\n"
                                         "avg_frame_rate=30000/1001\nduration=10.000000\n");
    REQUIRE(info);
    CHECK(info->width == 1280);
    CHECK(info->height == 720);
    CHECK(info->fps == Catch::Approx(29.97).margin(0.01));
    CHECK(info->duration_s == Catch::Approx(10.0));
}

TEST_CASE("Probe: an unusable average rate falls back to the stream rate",
          "[timelapse][player][probe]") {
    const auto info =
        parse_ffprobe_info("width=640\nheight=480\nr_frame_rate=25/1\navg_frame_rate=0/0\n"
                           "duration=N/A\n");
    REQUIRE(info);
    CHECK(info->fps == Catch::Approx(25.0));
    CHECK(info->duration_s == 0.0);
}

TEST_CASE("Probe: ffprobe output without a size is no answer", "[timelapse][player][probe]") {
    CHECK_FALSE(parse_ffprobe_info(""));
    CHECK_FALSE(parse_ffprobe_info("width=1280\nduration=3.0\n"));
    CHECK_FALSE(parse_ffprobe_info("width=0\nheight=0\n"));
}

TEST_CASE("Probe: ffmpeg's stderr header gives size, rate and duration",
          "[timelapse][player][probe]") {
    const std::string header =
        "Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'test.mp4':\n"
        "  Duration: 00:01:05.50, start: 0.000000, bitrate: 42 kb/s\n"
        "  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), "
        "yuv420p(progressive), 1280x720 [SAR 1:1 DAR 16:9], 39 kb/s, 30 fps, 30 tbr, "
        "15360 tbn (default)\n"
        "At least one output file must be specified\n";
    const auto info = parse_ffmpeg_header(header);
    REQUIRE(info);
    CHECK(info->width == 1280);
    CHECK(info->height == 720);
    CHECK(info->fps == Catch::Approx(30.0));
    CHECK(info->duration_s == Catch::Approx(65.5));
}

TEST_CASE("Probe: ffmpeg's header with a fractional rate and no duration",
          "[timelapse][player][probe]") {
    const auto info = parse_ffmpeg_header(
        "  Duration: N/A, bitrate: N/A\n"
        "  Stream #0:0: Video: mjpeg, yuvj420p(pc), 640x480, 23.98 fps, 23.98 tbr\n");
    REQUIRE(info);
    CHECK(info->width == 640);
    CHECK(info->height == 480);
    CHECK(info->fps == Catch::Approx(23.98));
    CHECK(info->duration_s == 0.0);
}

TEST_CASE("Probe: a header with no video stream is no answer", "[timelapse][player][probe]") {
    CHECK_FALSE(parse_ffmpeg_header("No such file or directory\n"));
    CHECK_FALSE(parse_ffmpeg_header("  Stream #0:0: Audio: aac, 44100 Hz, stereo\n"));
}

TEST_CASE("Probe: the pacing rate is the video's when sane, else 30",
          "[timelapse][player][probe]") {
    CHECK(effective_fps(24.0) == 24.0);
    CHECK(effective_fps(0.0) == 30.0);
    CHECK(effective_fps(-5.0) == 30.0);
    CHECK(effective_fps(1000.0) == 30.0);
}

// --- Formats and sizes ---

TEST_CASE("Frame format: the display's depth picks the raw layout", "[timelapse][player][format]") {
    CHECK(raw_format_for_display_bpp(16) == RawPixelFormat::Rgb565);
    CHECK(raw_format_for_display_bpp(24) == RawPixelFormat::Rgb888);
    CHECK(raw_format_for_display_bpp(32) == RawPixelFormat::Xrgb8888);
    CHECK(std::string(ffmpeg_pix_fmt_name(RawPixelFormat::Rgb565)) == "rgb565le");
    CHECK(std::string(ffmpeg_pix_fmt_name(RawPixelFormat::Rgb888)) == "bgr24");
    CHECK(std::string(ffmpeg_pix_fmt_name(RawPixelFormat::Xrgb8888)) == "bgra");
    CHECK(bytes_per_pixel(RawPixelFormat::Rgb565) == 2);
    CHECK(bytes_per_pixel(RawPixelFormat::Rgb888) == 3);
    CHECK(bytes_per_pixel(RawPixelFormat::Xrgb8888) == 4);
}

TEST_CASE("Frame size: a video fits the box with its aspect, even sides",
          "[timelapse][player][format]") {
    SECTION("16:9 into 800x480 is width-bound") {
        const auto s = fit_frame({1280, 720}, {800, 480});
        CHECK(s.width == 800);
        CHECK(s.height == 450);
    }
    SECTION("4:3 into 800x480 is height-bound") {
        const auto s = fit_frame({640, 480}, {800, 480});
        CHECK(s.width == 640);
        CHECK(s.height == 480);
    }
    SECTION("16:9 into 480x272") {
        const auto s = fit_frame({1280, 720}, {480, 272});
        CHECK(s.width == 480);
        CHECK(s.height == 270);
    }
    SECTION("a small video is scaled up to the box") {
        const auto s = fit_frame({320, 240}, {800, 480});
        CHECK(s.width == 640);
        CHECK(s.height == 480);
    }
    SECTION("odd results round down to even") {
        const auto s = fit_frame({1000, 1000}, {801, 333});
        CHECK(s.width == 332);
        CHECK(s.height == 332);
    }
    SECTION("degenerate input is empty") {
        CHECK(fit_frame({0, 720}, {800, 480}).empty());
        CHECK(fit_frame({1280, 720}, {0, 0}).empty());
    }
}

TEST_CASE("Frame size: bytes per packed frame", "[timelapse][player][format]") {
    CHECK(frame_bytes({800, 450}, RawPixelFormat::Rgb565) == 800u * 450u * 2u);
    CHECK(frame_bytes({800, 450}, RawPixelFormat::Xrgb8888) == 800u * 450u * 4u);
    CHECK(frame_bytes({}, RawPixelFormat::Rgb565) == 0u);
}

// --- ffmpeg arguments ---

static bool has_arg_pair(const std::vector<std::string>& args, const std::string& flag,
                         const std::string& value) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag && args[i + 1] == value) {
            return true;
        }
    }
    return false;
}

static ptrdiff_t arg_index(const std::vector<std::string>& args, const std::string& word) {
    auto it = std::find(args.begin(), args.end(), word);
    return it == args.end() ? -1 : it - args.begin();
}

TEST_CASE("Decode args: raw frames of the fitted size and display format on stdout",
          "[timelapse][player][args]") {
    const auto args =
        ffmpeg_decode_args("/data/tl/a b.mp4", 0.0, {800, 450}, RawPixelFormat::Rgb565);
    REQUIRE_FALSE(args.empty());
    CHECK(args.front() == "ffmpeg");
    CHECK(has_arg_pair(args, "-i", "/data/tl/a b.mp4"));
    CHECK(has_arg_pair(args, "-pix_fmt", "rgb565le"));
    CHECK(has_arg_pair(args, "-f", "rawvideo"));
    CHECK(args.back() == "-");
    CHECK(arg_index(args, "-an") >= 0);
    CHECK(arg_index(args, "-nostdin") >= 0);
    const auto vf = arg_index(args, "-vf");
    REQUIRE(vf >= 0);
    CHECK(args[static_cast<size_t>(vf) + 1].rfind("scale=800:450", 0) == 0);
    SECTION("one thread for the decoder, the filters and the raw encoder") {
        const auto in = arg_index(args, "-i");
        int before = 0, after = 0;
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "-threads" && args[i + 1] == "1") {
                (static_cast<ptrdiff_t>(i) < in ? before : after)++;
            }
        }
        CHECK(before == 1);
        CHECK(after == 1);
        CHECK(has_arg_pair(args, "-filter_threads", "1"));
        CHECK(arg_index(args, "-probesize") < in);
    }
    SECTION("no nobuffer flag: it stops an MP4 part way through") {
        CHECK(arg_index(args, "nobuffer") < 0);
    }
    SECTION("no seek from the start") {
        CHECK(arg_index(args, "-ss") < 0);
    }
}

TEST_CASE("Seek args: the seek goes before the input", "[timelapse][player][args]") {
    const auto args = ffmpeg_decode_args("/v.mp4", 12.5, {640, 480}, RawPixelFormat::Xrgb8888);
    const auto ss = arg_index(args, "-ss");
    const auto in = arg_index(args, "-i");
    REQUIRE(ss >= 0);
    REQUIRE(in >= 0);
    CHECK(ss < in);
    CHECK(args[static_cast<size_t>(ss) + 1] == "12.500");
    CHECK(has_arg_pair(args, "-pix_fmt", "bgra"));
}

TEST_CASE("Probe args: ffprobe and the ffmpeg header fallback name the file",
          "[timelapse][player][args]") {
    const auto probe = ffprobe_info_args("/v.mp4");
    CHECK(probe.front() == "ffprobe");
    CHECK(probe.back() == "/v.mp4");
    const auto header = ffmpeg_header_args("/v.mp4");
    CHECK(header.front() == "ffmpeg");
    CHECK(has_arg_pair(header, "-i", "/v.mp4"));
}

TEST_CASE("Scrub: permille and seconds convert both ways, clamped", "[timelapse][player][scrub]") {
    CHECK(seek_seconds_for(500, 60.0) == Catch::Approx(30.0));
    CHECK(seek_seconds_for(0, 60.0) == 0.0);
    CHECK(seek_seconds_for(-10, 60.0) == 0.0);
    CHECK(seek_seconds_for(1000, 60.0) < 60.0);
    CHECK(seek_seconds_for(2000, 60.0) < 60.0);
    CHECK(seek_seconds_for(500, 0.0) == 0.0);
    CHECK(permille_for(15.0, 60.0) == 250);
    CHECK(permille_for(90.0, 60.0) == 1000);
    CHECK(permille_for(-1.0, 60.0) == 0);
    CHECK(permille_for(5.0, 0.0) == 0);
}

TEST_CASE("Clock: the elapsed label shows the presented frame's time while playing",
          "[timelapse][player][scrub]") {
    CHECK(elapsed_label_seconds(false, 0, 3.4, 10.0) == Catch::Approx(3.4));
    CHECK(elapsed_label_seconds(false, 900, 3.4, 10.0) == Catch::Approx(3.4));
}

TEST_CASE("Clock: the elapsed label follows the finger while scrubbing",
          "[timelapse][player][scrub]") {
    CHECK(elapsed_label_seconds(true, 500, 3.4, 10.0) == Catch::Approx(5.0));
    CHECK(elapsed_label_seconds(true, 1000, 3.4, 10.0) == Catch::Approx(10.0));
    CHECK(elapsed_label_seconds(true, -5, 3.4, 10.0) == 0.0);
    CHECK(elapsed_label_seconds(true, 500, 3.4, 0.0) == 0.0);
}

TEST_CASE("Clock: elapsed and total read m:ss, h:mm:ss from an hour",
          "[timelapse][player][scrub]") {
    CHECK(format_clock(0.0) == "0:00");
    CHECK(format_clock(9.9) == "0:09");
    CHECK(format_clock(65.0) == "1:05");
    CHECK(format_clock(3725.0) == "1:02:05");
    CHECK(format_clock(-3.0) == "0:00");
}

// --- Pacing ---

TEST_CASE("Pacing: an early frame waits until it is due", "[timelapse][player][pace]") {
    const auto d = pace_frame(10, 20.0, 0.4);
    CHECK(d.wait_s == Catch::Approx(0.1));
    CHECK_FALSE(d.reanchor);
}

TEST_CASE("Pacing: a lead hands the frame over that much before it is due",
          "[timelapse][player][pace]") {
    CHECK(pace_frame(10, 20.0, 0.4, 0.05).wait_s == Catch::Approx(0.05));
    CHECK(pace_frame(10, 20.0, 0.47, 0.05).wait_s == 0.0);
    CHECK_FALSE(pace_frame(10, 20.0, 0.47, 0.05).reanchor);
}

TEST_CASE("Pacing: a slightly late frame goes at once", "[timelapse][player][pace]") {
    const auto d = pace_frame(10, 20.0, 0.6);
    CHECK(d.wait_s == 0.0);
    CHECK_FALSE(d.reanchor);
}

TEST_CASE("Pacing: a frame far behind restarts the clock", "[timelapse][player][pace]") {
    const auto d = pace_frame(10, 20.0, 1.0);
    CHECK(d.wait_s == 0.0);
    CHECK(d.reanchor);
    CHECK(pace_frame(10, 20.0, 1.0, 0.05).reanchor);
}

// --- Presentation ---

static int pick(const std::vector<double>& due, double now) {
    return pick_due_frame(due.data(), due.size(), now);
}

TEST_CASE("Present: nothing pending or nothing due shows nothing", "[timelapse][player][present]") {
    CHECK(pick({}, 5.0) == -1);
    CHECK(pick({5.1, 5.2}, 5.0) == -1);
}

TEST_CASE("Present: the frame whose time has come is shown", "[timelapse][player][present]") {
    CHECK(pick({5.0, 5.033}, 5.0) == 0);
    CHECK(pick({4.99, 5.033}, 5.01) == 0);
}

TEST_CASE("Present: a UI that looked late shows the newest due frame",
          "[timelapse][player][present]") {
    CHECK(pick({4.9, 4.95, 5.2}, 5.0) == 1);
    CHECK(pick({4.9, 4.95}, 5.0) == 1);
}

TEST_CASE("Present: the timer looks several times per frame", "[timelapse][player][present]") {
    CHECK(present_poll_ms(30.0) == 8);
    CHECK(present_poll_ms(15.0) == 10);
    CHECK(present_poll_ms(60.0) == 4);
    CHECK(present_poll_ms(240.0) == 4);
    CHECK(present_poll_ms(0.0) == 8); // an unknown rate paces at 30
}

// --- The decoder-to-UI handoff ---

namespace {

struct Shown {
    int64_t index = -1;
    std::vector<uint8_t> pixels;
};

Shown show(FrameExchange& x, double now) {
    Shown out;
    x.consume_due(now, [&](const uint8_t* p, FrameSize s, int64_t idx) {
        out.pixels.assign(p, p + static_cast<size_t>(s.width * s.height));
        out.index = idx;
    });
    return out;
}

void put(FrameExchange& x, uint8_t value, int64_t index, double due, double now = -1.0,
         uint64_t generation = 0) {
    std::vector<uint8_t> f{value};
    x.publish(f, {1, 1}, index, due, now, generation);
}

/// Every published frame is in exactly one bucket.
void check_accounted(const FrameExchange& x) {
    const auto st = x.stats();
    CHECK(st.published == st.shown + st.skipped_late + st.replaced_late + st.replaced_early +
                              st.discarded + st.queued);
}

} // namespace

TEST_CASE("Exchange: a frame published early waits for its time, then shows",
          "[timelapse][player][exchange]") {
    FrameExchange x;
    put(x, 7, 0, 10.0);
    CHECK(show(x, 9.99).index == -1);
    CHECK(x.pending() == 1);
    const Shown s = show(x, 10.0);
    CHECK(s.index == 0);
    CHECK(s.pixels == std::vector<uint8_t>{7});
    CHECK(x.pending() == 0);
    CHECK(show(x, 10.1).index == -1);
}

TEST_CASE("Exchange: frames on time are all shown, in order", "[timelapse][player][exchange]") {
    FrameExchange x;
    for (int i = 0; i < 30; ++i) {
        put(x, static_cast<uint8_t>(i), i, 1.0 + i / 30.0);
        if (i > 0) {
            CHECK(show(x, 1.0 + (i - 1) / 30.0 + 0.004).index == i - 1);
        }
    }
    CHECK(show(x, 2.0).index == 29);
    const auto st = x.stats();
    CHECK(st.published == 30);
    CHECK(st.shown == 30);
    CHECK(st.dropped() == 0);
    check_accounted(x);
}

TEST_CASE("Exchange: a late UI skips to the newest due frame", "[timelapse][player][exchange]") {
    FrameExchange x;
    put(x, 1, 0, 1.0);
    put(x, 2, 1, 1.033);
    CHECK(show(x, 1.05).index == 1);
    CHECK(x.stats().skipped_late == 1);
    CHECK(x.stats().dropped() == 1);
    check_accounted(x);
}

TEST_CASE("Exchange: a full queue drops its oldest frame", "[timelapse][player][exchange]") {
    FrameExchange x;
    for (int i = 0; i < static_cast<int>(FrameExchange::kSlots) + 1; ++i) {
        put(x, static_cast<uint8_t>(i), i, 1.0 + i, 0.5);
    }
    CHECK(x.pending() == FrameExchange::kSlots);
    CHECK(x.stats().dropped() == 1);
    CHECK(show(x, 2.0).index == 1);
    check_accounted(x);
}

TEST_CASE("Exchange: a frame pushed out after its time is the UI's lateness",
          "[timelapse][player][exchange]") {
    FrameExchange x;
    for (int i = 0; i < static_cast<int>(FrameExchange::kSlots); ++i) {
        put(x, static_cast<uint8_t>(i), i, 1.0 + i / 30.0, 0.9);
    }
    // The UI never looked: by now frame 0 is long due, and a fourth arrives.
    put(x, 9, 3, 1.1, 1.08);
    const auto st = x.stats();
    CHECK(st.replaced_late == 1);
    CHECK(st.replaced_early == 0);
    CHECK(st.dropped() == 1);
    check_accounted(x);
}

TEST_CASE("Exchange: a frame pushed out before its time is the decoder running ahead",
          "[timelapse][player][exchange]") {
    FrameExchange x;
    for (int i = 0; i < static_cast<int>(FrameExchange::kSlots) + 1; ++i) {
        put(x, static_cast<uint8_t>(i), i, 5.0 + i, 1.0);
    }
    const auto st = x.stats();
    CHECK(st.replaced_early == 1);
    CHECK(st.replaced_late == 0);
    check_accounted(x);
}

TEST_CASE("Exchange: frames still queued at a seek or close are discarded, not lost",
          "[timelapse][player][exchange]") {
    FrameExchange x;
    put(x, 1, 0, 1.0, 0.9);
    put(x, 2, 1, 1.033, 0.9);
    show(x, 1.0);
    CHECK(x.stats().queued == 1);
    x.clear();
    const auto st = x.stats();
    CHECK(st.discarded == 1);
    CHECK(st.queued == 0);
    CHECK(st.dropped() == 0);
    check_accounted(x);
}

TEST_CASE("Exchange: a pause pushes the queued frames' times back",
          "[timelapse][player][exchange]") {
    FrameExchange x;
    put(x, 1, 0, 1.0);
    x.shift_due(5.0);
    CHECK(show(x, 1.0).index == -1);
    CHECK(show(x, 6.0).index == 0);
}

TEST_CASE("Exchange: clear forgets the queued frames", "[timelapse][player][exchange]") {
    FrameExchange x;
    put(x, 1, 0, 1.0);
    x.clear();
    CHECK(x.pending() == 0);
    CHECK(show(x, 9.0).index == -1);
}

TEST_CASE("Exchange: after a seek, nothing from the old run shows",
          "[timelapse][player][exchange]") {
    FrameExchange x;
    put(x, 1, 0, 1.0, 0.9, 0);
    put(x, 2, 1, 1.033, 0.9, 0);
    x.begin_generation(1);
    CHECK(x.pending() == 0);
    // A frame the old run had in flight when the seek landed.
    put(x, 3, 2, 1.066, 0.95, 0);
    CHECK(x.pending() == 0);
    put(x, 4, 300, 1.1, 1.0, 1);
    const Shown s = show(x, 1.1);
    CHECK(s.index == 300);
    const auto st = x.stats();
    CHECK(st.discarded == 3);
    CHECK(st.dropped() == 0);
    check_accounted(x);
}

TEST_CASE("Exchange: a published frame hands back a used buffer to fill",
          "[timelapse][player][exchange]") {
    FrameExchange x;
    std::vector<uint8_t> f(64, 1);
    x.publish(f, {8, 8}, 0, 1.0, 0.9);
    show(x, 1.0);
    std::vector<uint8_t> g(64, 2);
    x.publish(g, {8, 8}, 1, 2.0, 1.9);
    CHECK(g.size() == 64);
}

// --- Frame assembly from a byte stream ---

TEST_CASE("Assembler: chunks that straddle frame edges come out as whole frames",
          "[timelapse][player][assembler]") {
    constexpr size_t kFrame = 6;
    FrameAssembler a(kFrame);
    std::vector<uint8_t> stream(kFrame * 3 + 2);
    std::iota(stream.begin(), stream.end(), uint8_t{0});

    std::vector<std::vector<uint8_t>> frames;
    const size_t chunks[] = {4, 5, 1, 7, 3};
    size_t pos = 0;
    for (size_t c : chunks) {
        a.feed(stream.data() + pos, c, [&](std::vector<uint8_t>& f) { frames.push_back(f); });
        pos += c;
    }
    REQUIRE(pos == stream.size());
    REQUIRE(frames.size() == 3);
    for (size_t i = 0; i < frames.size(); ++i) {
        CHECK(frames[i] ==
              std::vector<uint8_t>(stream.begin() + i * kFrame, stream.begin() + (i + 1) * kFrame));
    }
    CHECK(a.partial() == 2);
}

TEST_CASE("Assembler: the zero-copy reader path completes exactly at the frame size",
          "[timelapse][player][assembler]") {
    FrameAssembler a(4);
    CHECK(a.want() == 4);
    a.tail()[0] = 9;
    CHECK_FALSE(a.commit(1));
    CHECK(a.want() == 3);
    CHECK(a.partial() == 1);
    a.tail()[0] = 8;
    a.tail()[1] = 7;
    a.tail()[2] = 6;
    CHECK(a.commit(3));
    CHECK(a.frame() == std::vector<uint8_t>{9, 8, 7, 6});
    CHECK(a.partial() == 0);
    CHECK(a.want() == 4);
}

TEST_CASE("Assembler: a frame swapped out by the reader is replaced, not shrunk",
          "[timelapse][player][assembler]") {
    FrameAssembler a(3);
    const uint8_t bytes[] = {1, 2, 3, 4, 5, 6};
    std::vector<uint8_t> taken;
    a.feed(bytes, 3, [&](std::vector<uint8_t>& f) { taken.swap(f); });
    CHECK(taken == std::vector<uint8_t>{1, 2, 3});
    std::vector<uint8_t> second;
    a.feed(bytes + 3, 3, [&](std::vector<uint8_t>& f) { second = f; });
    CHECK(second == std::vector<uint8_t>{4, 5, 6});
}
