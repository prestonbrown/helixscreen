// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_player_decoder.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::timelapse;
using namespace std::chrono_literals;

// The decoder runs a real child process, but a shell producing bytes instead of
// ffmpeg: a 4x4 frame in the 32-bit layout is 64 bytes.
namespace {

constexpr size_t kFrameBytes = 4 * 4 * 4;

template <typename Pred> bool wait_for(Pred pred, std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

/// waitpid's view of @p pid: true once it is no longer our child to reap.
bool reaped(pid_t pid) {
    int status = 0;
    return waitpid(pid, &status, WNOHANG) < 0 && errno == ECHILD;
}

struct Harness {
    PlayerDecoder decoder;
    std::atomic<int> ends{0};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> last_end_generation{999};
    std::mutex mutex;
    std::vector<double> starts;

    PlayerDecoder::Config config(const std::string& shell, double fps = 200.0,
                                 bool probe_ok = true) {
        PlayerDecoder::Config c;
        c.path = "/fake.mp4";
        c.box = {4, 4};
        c.format = RawPixelFormat::Xrgb8888;
        c.probe = [fps, probe_ok](const std::atomic<bool>&) -> std::optional<VideoInfo> {
            if (!probe_ok) {
                return std::nullopt;
            }
            return VideoInfo{4, 4, fps, 10.0};
        };
        c.decode_argv = [this, shell](double start_s, FrameSize) {
            std::lock_guard<std::mutex> lock(mutex);
            starts.push_back(start_s);
            return std::vector<std::string>{"sh", "-c", shell};
        };
        return c;
    }

    PlayerDecoder::Events events() {
        PlayerDecoder::Events e;
        e.on_end = [this](bool f, uint64_t generation) {
            failed.store(f);
            last_end_generation.store(generation);
            ends.fetch_add(1);
        };
        return e;
    }

    uint64_t published() const {
        return const_cast<PlayerDecoder&>(decoder).exchange().stats().published;
    }
};

} // namespace

TEST_CASE("Decoder: closing mid-playback stops the worker and reaps the child",
          "[timelapse][player][decoder]") {
    Harness h;
    REQUIRE(h.decoder.start(h.config("exec cat /dev/zero"), h.events()));
    REQUIRE(wait_for([&] { return h.decoder.child_pid() > 0 && h.published() > 2; }));
    const pid_t child = h.decoder.child_pid();

    h.decoder.stop();

    CHECK(h.decoder.child_pid() == -1);
    CHECK(reaped(child));
    CHECK(h.ends.load() == 0);
    const uint64_t after_stop = h.published();
    std::this_thread::sleep_for(50ms);
    CHECK(h.published() == after_stop);
}

TEST_CASE("Decoder: destroying it mid-playback reaps the child", "[timelapse][player][decoder]") {
    pid_t child = -1;
    {
        Harness h;
        REQUIRE(h.decoder.start(h.config("exec cat /dev/zero"), h.events()));
        REQUIRE(wait_for([&] { return h.decoder.child_pid() > 0; }));
        child = h.decoder.child_pid();
    }
    CHECK(reaped(child));
}

TEST_CASE("Decoder: a stream that ends reports it, and a seek replays it",
          "[timelapse][player][decoder]") {
    Harness h;
    const std::string three_frames =
        "exec head -c " + std::to_string(kFrameBytes * 3) + " /dev/zero";
    REQUIRE(h.decoder.start(h.config(three_frames), h.events()));
    REQUIRE(wait_for([&] { return h.ends.load() == 1; }));
    CHECK_FALSE(h.failed.load());
    CHECK(h.published() == 3);
    CHECK(h.decoder.child_pid() == -1);

    h.decoder.seek(0.0);
    REQUIRE(wait_for([&] { return h.ends.load() == 2; }));
    CHECK(h.published() == 6);
}

TEST_CASE("Decoder: a video it cannot probe fails without starting a child",
          "[timelapse][player][decoder]") {
    Harness h;
    REQUIRE(h.decoder.start(h.config("exec cat /dev/zero", 200.0, false), h.events()));
    REQUIRE(wait_for([&] { return h.ends.load() == 1; }));
    CHECK(h.failed.load());
    std::lock_guard<std::mutex> lock(h.mutex);
    CHECK(h.starts.empty());
}

TEST_CASE("Decoder: a stream that ends before its first frame is a failure",
          "[timelapse][player][decoder]") {
    Harness h;
    REQUIRE(h.decoder.start(h.config("exec head -c 10 /dev/zero"), h.events()));
    REQUIRE(wait_for([&] { return h.ends.load() == 1; }));
    CHECK(h.failed.load());
}

TEST_CASE("Decoder: pause stops frames and resume brings them back",
          "[timelapse][player][decoder]") {
    Harness h;
    REQUIRE(h.decoder.start(h.config("exec cat /dev/zero"), h.events()));
    REQUIRE(wait_for([&] { return h.published() > 2; }));

    h.decoder.pause();
    std::this_thread::sleep_for(60ms); // a frame already waiting may still land
    const uint64_t paused_at = h.published();
    std::this_thread::sleep_for(150ms);
    CHECK(h.published() == paused_at);

    h.decoder.resume();
    CHECK(wait_for([&] { return h.published() > paused_at + 2; }));
}

TEST_CASE("Decoder: a seek restarts the child at the new time and numbers frames from it",
          "[timelapse][player][decoder]") {
    Harness h;
    REQUIRE(h.decoder.start(h.config("exec cat /dev/zero", 100.0), h.events()));
    REQUIRE(wait_for([&] { return h.decoder.child_pid() > 0 && h.published() > 0; }));
    const pid_t first = h.decoder.child_pid();

    h.decoder.seek(2.0);
    REQUIRE(wait_for([&] {
        std::lock_guard<std::mutex> lock(h.mutex);
        return h.starts.size() == 2;
    }));
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        CHECK(h.starts[1] == 2.0);
    }
    CHECK(reaped(first));

    int64_t index = -1;
    REQUIRE(wait_for([&] {
        return h.decoder.exchange().consume_due(
            monotonic_seconds() + 100.0, [&](const uint8_t*, FrameSize, int64_t i) { index = i; });
    }));
    CHECK(index >= 200);
}

TEST_CASE("Decoder: each end report names its run, and a seek starts a newer one",
          "[timelapse][player][decoder]") {
    Harness h;
    const std::string three_frames =
        "exec head -c " + std::to_string(kFrameBytes * 3) + " /dev/zero";
    REQUIRE(h.decoder.start(h.config(three_frames), h.events()));
    REQUIRE(wait_for([&] { return h.ends.load() == 1; }));
    const uint64_t first = h.last_end_generation.load();
    CHECK(first == h.decoder.generation());

    h.decoder.seek(0.0);
    // From here the first run's end is stale: the overlay drops it.
    CHECK(h.decoder.generation() > first);
    REQUIRE(wait_for([&] { return h.ends.load() == 2; }));
    CHECK(h.last_end_generation.load() == h.decoder.generation());
}

TEST_CASE("Decoder: a pause longer than the lateness limit resumes on the same clock",
          "[timelapse][player][decoder]") {
    // 20 fps: a 50 ms period, so the clock math shows above scheduling noise.
    Harness h;
    constexpr double kPeriod = 1.0 / 20.0;
    REQUIRE(h.decoder.start(h.config("exec cat /dev/zero", 20.0), h.events()));
    REQUIRE(wait_for([&] { return h.published() > 3; }));

    h.decoder.pause();
    std::this_thread::sleep_for(450ms); // past max_late_s
    const uint64_t before = h.published();
    const double resumed_at = monotonic_seconds();
    h.decoder.resume();

    // Every frame queued before the pause moved with it. Nothing consumes
    // here, so the queue holds up to its three slots around the pause moment;
    // unshifted they would sit the whole pause (450 ms) in the past.
    for (double due : h.decoder.exchange().due_times()) {
        CHECK(due > resumed_at - 3 * kPeriod);
    }
    // The first frame decoded after the resume keeps its place on the clock
    // (one period or more ahead), instead of restarting the clock at "now".
    REQUIRE(wait_for([&] { return h.published() > before; }));
    const auto due = h.decoder.exchange().due_times();
    const auto fresh = std::min<size_t>(h.published() - before, due.size());
    REQUIRE(fresh > 0);
    CHECK(due[due.size() - fresh] >= resumed_at + 0.5 * kPeriod);
}
