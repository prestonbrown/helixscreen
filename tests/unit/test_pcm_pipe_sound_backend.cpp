// SPDX-License-Identifier: GPL-3.0-or-later

#include "note_event.h"
#include "pcm_pipe_sound_backend.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

std::string make_temp_dir() {
    std::string tmpl = "/tmp/pcm_pipe_test_XXXXXX";
    char* dir = mkdtemp(tmpl.data());
    REQUIRE(dir != nullptr);
    return dir;
}

void write_executable(const std::string& path, const std::string& body) {
    {
        std::ofstream f(path);
        f << body;
    }
    REQUIRE(chmod(path.c_str(), 0755) == 0);
}

void touch(const std::string& path) {
    std::ofstream f(path);
}

/// A player that records its argv and copies stdin to a file, like aplay
/// reading raw PCM until EOF.
std::string fake_player(const std::string& dir) {
    std::string path = dir + "/aplay";
    write_executable(path,
                     "#!/bin/sh\necho \"$@\" > \"" + dir + "/args\"\ncat > \"" + dir + "/pcm\"\n");
    return path;
}

/// A player whose body is @p script.
std::string scripted_player(const std::string& dir, const std::string& script) {
    std::string path = dir + "/aplay-scripted";
    write_executable(path, "#!/bin/sh\n" + script + "\n");
    return path;
}

/// A player that records its pid, never reads and ignores EOF: a wedged device.
std::string stuck_player(const std::string& dir) {
    return scripted_player(dir, "echo $$ > \"" + dir + "/pid\"\nexec sleep 30");
}

std::string file_contents(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<int16_t> read_pcm(const std::string& path) {
    std::string raw = file_contents(path);
    std::vector<int16_t> samples(raw.size() / 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(static_cast<uint8_t>(raw[2 * i]) |
                                          (static_cast<uint8_t>(raw[2 * i + 1]) << 8));
    return samples;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

NoteEvent tone(float freq, float ms) {
    NoteEvent e;
    e.freq_hz = freq;
    e.velocity = 0.8f;
    e.duration_ms = ms;
    e.wave = Waveform::SQUARE;
    e.attack_ms = 1;
    e.decay_ms = 0;
    e.sustain_level = 1.0f;
    e.release_ms = 5;
    return e;
}

void cleanup(const std::string& dir) {
    std::filesystem::remove_all(dir);
}

} // namespace

// ============================================================================
// Probes
// ============================================================================

TEST_CASE("find_player: env override names the binary or disables", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    std::string player = fake_player(dir);

    CHECK(PcmPipeSoundBackend::find_player(player.c_str(), {}) == player);
    CHECK(PcmPipeSoundBackend::find_player("0", {player}).empty());
    CHECK(PcmPipeSoundBackend::find_player("off", {player}).empty());
    CHECK(PcmPipeSoundBackend::find_player("no", {player}).empty());
    CHECK(PcmPipeSoundBackend::find_player((dir + "/missing").c_str(), {player}).empty());

    cleanup(dir);
}

TEST_CASE("find_player: no override takes the first executable candidate", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    std::string player = fake_player(dir);
    touch(dir + "/not-executable");

    CHECK(PcmPipeSoundBackend::find_player(
              nullptr, {dir + "/missing", dir + "/not-executable", player}) == player);
    CHECK(PcmPipeSoundBackend::find_player("", {dir + "/missing"}).empty());
    CHECK(PcmPipeSoundBackend::find_player(nullptr, {}).empty());

    cleanup(dir);
}

TEST_CASE("has_playback_device: only a pcmC*D*p node counts", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();

    CHECK_FALSE(PcmPipeSoundBackend::has_playback_device(dir + "/absent"));
    CHECK_FALSE(PcmPipeSoundBackend::has_playback_device(dir));

    touch(dir + "/controlC0");
    touch(dir + "/pcmC0D0c");
    CHECK_FALSE(PcmPipeSoundBackend::has_playback_device(dir));

    touch(dir + "/pcmC0D0p");
    CHECK(PcmPipeSoundBackend::has_playback_device(dir));

    cleanup(dir);
}

TEST_CASE("player_args: raw S16_LE mono at the render rate from stdin", "[sound][pcmpipe]") {
    auto args = PcmPipeSoundBackend::player_args("/usr/bin/aplay");
    REQUIRE(args.size() >= 3);
    CHECK(args.front() == "/usr/bin/aplay");
    CHECK(args.back() == "-");
    auto has_pair = [&](const std::string& flag, const std::string& value) {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == flag && args[i + 1] == value)
                return true;
        return false;
    };
    CHECK(has_pair("-t", "raw"));
    CHECK(has_pair("-f", "S16_LE"));
    CHECK(has_pair("-c", "1"));
    CHECK(has_pair("-r", std::to_string(PcmPipeSoundBackend::kSampleRate)));
    CHECK(std::find(args.begin(), args.end(), "-q") != args.end());
}

TEST_CASE("initialize refuses a player that is not executable", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    touch(dir + "/plain");

    PcmPipeSoundBackend backend;
    CHECK_FALSE(backend.initialize(dir + "/plain"));
    CHECK_FALSE(backend.initialize(""));
    CHECK(backend.spawn_count() == 0);

    cleanup(dir);
}

// ============================================================================
// End to end against a fake player
// ============================================================================

TEST_CASE("a sound burst spawns the player once and streams the rendered tone",
          "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    std::string player = fake_player(dir);

    {
        PcmPipeSoundBackend backend;
        // Longer than the note's worst-case end, so the stream's tail is silence
        // even when a loaded machine delays the render thread.
        backend.set_idle_grace_ms(500);
        REQUIRE(backend.initialize(player));
        REQUIRE(wait_until([&] { return backend.is_parked(); }, 1000));

        backend.resume();
        backend.publish_note(0, tone(440.0f, 100.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        backend.suspend();
        REQUIRE(wait_until([&] { return backend.is_parked(); }, 3000));

        CHECK(backend.spawn_count() == 1);
        CHECK_FALSE(backend.player_disabled());
    }

    std::string args = file_contents(dir + "/args");
    CHECK(args.find("-t raw") != std::string::npos);
    CHECK(args.find("-f S16_LE") != std::string::npos);
    CHECK(args.find("-c 1") != std::string::npos);
    CHECK(args.find("-r 44100") != std::string::npos);

    auto pcm = read_pcm(dir + "/pcm");
    // At least the 100 ms note (4410 frames) reached the player...
    REQUIRE(pcm.size() >= 4410);
    // ...and it is a tone, not silence: a 0.8 square wave hits +-26000. The
    // note was published after resume() returned, so it may start up to
    // kLeadMs into the stream; look at the first 200 ms.
    int16_t peak = 0;
    for (size_t i = 0; i < std::min<size_t>(pcm.size(), 8820); ++i)
        peak = std::max<int16_t>(peak, static_cast<int16_t>(std::abs(pcm[i])));
    CHECK(peak > 20000);
    // The idle grace after the note is silence.
    CHECK(pcm.back() == 0);

    cleanup(dir);
}

TEST_CASE("resume within the idle grace reuses the live player", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    std::string player = fake_player(dir);

    PcmPipeSoundBackend backend;
    backend.set_idle_grace_ms(500);
    REQUIRE(backend.initialize(player));

    backend.resume();
    REQUIRE(wait_until([&] { return backend.spawn_count() == 1; }, 3000));
    backend.suspend();
    backend.resume(); // inside the grace: the same player keeps reading
    backend.suspend();
    REQUIRE(wait_until([&] { return backend.is_parked(); }, 3000));
    CHECK(backend.spawn_count() == 1);

    backend.resume(); // after the park: a fresh player
    REQUIRE(wait_until([&] { return backend.spawn_count() == 2; }, 3000));
    backend.suspend();
    REQUIRE(wait_until([&] { return backend.is_parked(); }, 3000));
    CHECK(backend.spawn_count() == 2);

    backend.shutdown();
    cleanup(dir);
}

TEST_CASE("a player that keeps failing disables the backend instead of looping",
          "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    PcmPipeSoundBackend backend;
    backend.set_idle_grace_ms(50);
    backend.set_failure_timeouts_ms(100, 50);
    int expected_spawns = PcmPipeSoundBackend::kMaxConsecutiveFailures;

    SECTION("it exits before reading, so the pipe breaks") {
        REQUIRE(backend.initialize(scripted_player(dir, "exit 1")));
    }
    SECTION("it reads to EOF and only then reports failure") {
        REQUIRE(backend.initialize(scripted_player(dir, "cat > /dev/null\nexit 3")));
    }
    SECTION("it cannot be spawned") {
        std::string player = scripted_player(dir, "exit 0");
        REQUIRE(backend.initialize(player));
        REQUIRE(unlink(player.c_str()) == 0);
        expected_spawns = 0;
    }
    SECTION("it stops reading and ignores EOF") {
        REQUIRE(backend.initialize(stuck_player(dir)));
    }

    for (int i = 0; i < PcmPipeSoundBackend::kMaxConsecutiveFailures; ++i) {
        backend.resume();
        backend.suspend();
        REQUIRE(wait_until([&] { return backend.consecutive_failures() == i + 1; }, 5000));
    }
    REQUIRE(wait_until([&] { return backend.is_parked(); }, 3000));
    CHECK(backend.player_disabled());

    backend.resume(); // latched: no further attempt
    CHECK(backend.is_parked());
    CHECK(backend.spawn_count() == expected_spawns);

    backend.shutdown();
    cleanup(dir);
}

TEST_CASE("a stalled player fills the pipe without ever counting as healthy", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();

    PcmPipeSoundBackend backend;
    backend.set_failure_timeouts_ms(100, 50);
    REQUIRE(backend.initialize(stuck_player(dir)));

    // No suspend: only the stall can end each burst.
    for (int i = 0; i < PcmPipeSoundBackend::kMaxConsecutiveFailures; ++i) {
        backend.resume();
        REQUIRE(wait_until([&] { return backend.consecutive_failures() == i + 1; }, 5000));
        REQUIRE(wait_until([&] { return backend.is_parked(); }, 3000));
    }
    CHECK(backend.player_disabled());

    backend.shutdown();
    cleanup(dir);
}

TEST_CASE("a long clean burst clears the failure count", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    // Fails its first run, then plays normally.
    std::string player = scripted_player(dir, "[ -f \"" + dir + "/ran\" ] || { touch \"" + dir +
                                                  "/ran\"; exit 1; }\ncat > /dev/null");

    PcmPipeSoundBackend backend;
    backend.set_idle_grace_ms(1100);
    REQUIRE(backend.initialize(player));

    backend.resume();
    REQUIRE(wait_until([&] { return backend.consecutive_failures() == 1; }, 5000));
    REQUIRE(wait_until([&] { return backend.is_parked(); }, 3000));

    backend.resume();
    backend.suspend();
    REQUIRE(wait_until([&] { return backend.consecutive_failures() == 0; }, 5000));

    backend.shutdown();
    cleanup(dir);
}

TEST_CASE("clear_render_source waits out a render call in flight", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    std::string player = fake_player(dir);

    PcmPipeSoundBackend backend;
    REQUIRE(backend.initialize(player));

    std::atomic<bool> in_call{false};
    std::atomic<int> calls{0};
    backend.set_render_source([&](float*, size_t, int) {
        in_call = true;
        ++calls;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        in_call = false;
    });
    backend.resume();
    REQUIRE(wait_until([&] { return in_call.load(); }, 3000));

    backend.clear_render_source();
    CHECK_FALSE(in_call.load());
    const int calls_at_clear = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(calls.load() == calls_at_clear);

    backend.shutdown();
    cleanup(dir);
}

#ifdef __linux__
TEST_CASE("the pipe queues only a few chunks ahead of a player that is not reading",
          "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();

    PcmPipeSoundBackend backend;
    backend.set_failure_timeouts_ms(2000, 50);
    REQUIRE(backend.initialize(stuck_player(dir)));

    std::atomic<int> chunks{0};
    backend.set_render_source([&](float*, size_t, int) { ++chunks; });
    backend.resume();
    // The wall clock alone would pace about 34 chunks into a pipe that had room.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    const int page = static_cast<int>(sysconf(_SC_PAGESIZE));
    const int chunk_bytes = PcmPipeSoundBackend::kFramesPerChunk * sizeof(int16_t);
    CHECK(chunks.load() <= page / chunk_bytes + 2);

    backend.shutdown();
    cleanup(dir);
}
#endif

TEST_CASE("shutdown kills and reaps a player that ignores EOF", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    std::string pid_file = dir + "/pid";

    {
        PcmPipeSoundBackend backend;
        backend.set_failure_timeouts_ms(2000, 50);
        REQUIRE(backend.initialize(stuck_player(dir)));
        backend.resume();
        REQUIRE(wait_until([&] { return !file_contents(pid_file).empty(); }, 5000));
    }

    const pid_t pid = static_cast<pid_t>(std::atoi(file_contents(pid_file).c_str()));
    REQUIRE(pid > 0);
    // A zombie still answers kill(0), so ESRCH proves it was reaped as well as killed.
    errno = 0;
    CHECK(kill(pid, 0) == -1);
    CHECK(errno == ESRCH);

    cleanup(dir);
}

TEST_CASE("shutdown while a sound is streaming joins cleanly", "[sound][pcmpipe]") {
    std::string dir = make_temp_dir();
    std::string player = fake_player(dir);

    {
        PcmPipeSoundBackend backend;
        REQUIRE(backend.initialize(player));
        backend.resume();
        backend.publish_note(1, tone(660.0f, 500.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        // Destructor: no suspend, player still reading.
    }

    CHECK(file_contents(dir + "/args").find("-t raw") != std::string::npos);
    cleanup(dir);
}
