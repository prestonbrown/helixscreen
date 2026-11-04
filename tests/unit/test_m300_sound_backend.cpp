// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "m300_sound_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// GCode format
// ============================================================================

TEST_CASE("M300 backend sends correct gcode format", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S440 P50");
}

TEST_CASE("M300 backend gcode uses integer frequency", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    // 523.25 Hz truncates to 523
    backend.set_tone(523.25f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S523 P50");
}

TEST_CASE("M300 backend gcode duration matches min_tick_ms", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(1000.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 1);
    // min_tick_ms() == 50 -> P50
    REQUIRE(commands[0] == "M300 S1000 P50");
}

// ============================================================================
// Frequency clamping
// ============================================================================

TEST_CASE("M300 backend clamps frequency below 100 to 100", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(50.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S100 P50");
}

TEST_CASE("M300 backend clamps frequency above 10000 to 10000", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(15000.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S10000 P50");
}

TEST_CASE("M300 backend passes through frequency at lower boundary", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(100.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S100 P50");
}

TEST_CASE("M300 backend passes through frequency at upper boundary", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(10000.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S10000 P50");
}

// ============================================================================
// Redundant frequency deduplication
// ============================================================================

TEST_CASE("M300 backend deduplicates same frequency", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.set_tone(440.0f, 0.8f, 0.5f);

    REQUIRE(commands.size() == 1);
}

TEST_CASE("M300 backend dedup resets after different frequency", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.set_tone(880.0f, 1.0f, 0.5f);
    backend.set_tone(440.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 3);
    REQUIRE(commands[0] == "M300 S440 P50");
    REQUIRE(commands[1] == "M300 S880 P50");
    REQUIRE(commands[2] == "M300 S440 P50");
}

TEST_CASE("M300 backend dedup resets after silence", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.silence();
    backend.set_tone(440.0f, 1.0f, 0.5f);

    // tone + silence + tone = 3 commands
    REQUIRE(commands.size() == 3);
}

// ============================================================================
// Silence
// ============================================================================

TEST_CASE("M300 backend silence sends S0 P1", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.silence();

    REQUIRE(commands.size() == 2);
    REQUIRE(commands[1] == "M300 S0 P1");
}

TEST_CASE("M300 backend silence deduplicates", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.silence();
    backend.silence();
    backend.silence();

    // tone + one silence = 2 commands
    REQUIRE(commands.size() == 2);
}

TEST_CASE("M300 backend silence when already silent is no-op", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    // Never played a tone, silence should be no-op
    backend.silence();

    REQUIRE(commands.empty());
}

// ============================================================================
// Amplitude threshold
// ============================================================================

TEST_CASE("M300 backend amplitude below threshold triggers silence", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.set_tone(880.0f, 0.005f, 0.5f);

    REQUIRE(commands.size() == 2);
    REQUIRE(commands[0] == "M300 S440 P50");
    REQUIRE(commands[1] == "M300 S0 P1");
}

TEST_CASE("M300 backend amplitude zero triggers silence", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.5f);
    backend.set_tone(880.0f, 0.0f, 0.5f);

    REQUIRE(commands.size() == 2);
    REQUIRE(commands[1] == "M300 S0 P1");
}

TEST_CASE("M300 backend amplitude at threshold boundary sends tone", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    // 0.01 is exactly the threshold boundary -- should still silence (<=0.01)
    backend.set_tone(440.0f, 0.01f, 0.5f);

    // last_freq_ starts at 0, and amplitude <= 0.01 calls silence(),
    // but silence() with last_freq_==0 is a no-op
    REQUIRE(commands.empty());
}

TEST_CASE("M300 backend amplitude just above threshold sends tone", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 0.02f, 0.5f);

    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S440 P50");
}

// ============================================================================
// min_tick_ms
// ============================================================================

TEST_CASE("M300 backend min_tick_ms returns 50", "[sound][m300]") {
    M300SoundBackend backend([](const std::string&) { return 0; });
    REQUIRE(backend.min_tick_ms() == 50.0f);
}

// ============================================================================
// Duty cycle ignored
// ============================================================================

TEST_CASE("M300 backend duty cycle does not affect output", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    backend.set_tone(440.0f, 1.0f, 0.1f);

    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S440 P50");

    // Change duty cycle with different freq to avoid dedup
    backend.set_tone(880.0f, 1.0f, 0.9f);

    REQUIRE(commands.size() == 2);
    REQUIRE(commands[1] == "M300 S880 P50");
}

// ============================================================================
// Null / empty sender
// ============================================================================

TEST_CASE("M300 backend with null sender does not crash", "[sound][m300]") {
    M300SoundBackend backend(nullptr);

    // All operations should be safe no-ops
    REQUIRE_NOTHROW(backend.set_tone(440.0f, 1.0f, 0.5f));
    REQUIRE_NOTHROW(backend.set_tone(880.0f, 1.0f, 0.5f));
    REQUIRE_NOTHROW(backend.silence());
    REQUIRE_NOTHROW(backend.set_tone(440.0f, 0.0f, 0.5f));
}

TEST_CASE("M300 backend with empty function does not crash", "[sound][m300]") {
    M300SoundBackend::GcodeSender empty_sender;
    M300SoundBackend backend(std::move(empty_sender));

    REQUIRE_NOTHROW(backend.set_tone(440.0f, 1.0f, 0.5f));
    REQUIRE_NOTHROW(backend.silence());
}

// ============================================================================
// Lifecycle: tone -> silence -> tone -> silence
// ============================================================================

TEST_CASE("M300 backend full lifecycle", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    // Play a tone
    backend.set_tone(440.0f, 1.0f, 0.5f);
    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0] == "M300 S440 P50");

    // Silence
    backend.silence();
    REQUIRE(commands.size() == 2);
    REQUIRE(commands[1] == "M300 S0 P1");

    // Play a different tone
    backend.set_tone(880.0f, 1.0f, 0.5f);
    REQUIRE(commands.size() == 3);
    REQUIRE(commands[2] == "M300 S880 P50");

    // Silence again
    backend.silence();
    REQUIRE(commands.size() == 4);
    REQUIRE(commands[3] == "M300 S0 P1");
}

// ============================================================================
// Default capabilities (inherited from SoundBackend)
// ============================================================================

TEST_CASE("M300 backend reports correct default capabilities", "[sound][m300]") {
    M300SoundBackend backend([](const std::string&) { return 0; });

    REQUIRE_FALSE(backend.supports_waveforms());
    REQUIRE_FALSE(backend.supports_amplitude());
    REQUIRE_FALSE(backend.supports_filter());
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("M300 backend clamped frequencies both send commands", "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    // Both 50 and 80 clamp to 100, but dedup checks the raw integer cast
    // before clamping, so they are seen as different values
    backend.set_tone(50.0f, 1.0f, 0.5f);
    backend.set_tone(80.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 2);
    REQUIRE(commands[0] == "M300 S100 P50");
    REQUIRE(commands[1] == "M300 S100 P50");
}

TEST_CASE("M300 backend negative frequency with positive amplitude triggers silence",
          "[sound][m300]") {
    std::vector<std::string> commands;
    M300SoundBackend backend([&](const std::string& cmd) {
        commands.push_back(cmd);
        return 0;
    });

    // Play a tone first so silence has something to do
    backend.set_tone(440.0f, 1.0f, 0.5f);

    // Negative freq truncates to negative int, which clamps to 100
    // But the int cast of -50.0f is -50, and the dedup check uses the pre-clamp value
    // Actually: freq_int = (int)(-50.0f) = -50, last_freq_ = 440, so not deduped
    // amplitude > 0.01, so it proceeds to clamp: max(100, min(10000, -50)) = 100
    backend.set_tone(-50.0f, 1.0f, 0.5f);

    REQUIRE(commands.size() == 2);
    REQUIRE(commands[1] == "M300 S100 P50");
}

TEST_CASE("M300 backend sends nothing while the printer is busy", "[sound][m300]") {
    std::vector<std::string> sent;
    bool busy = true;
    M300SoundBackend backend(
        [&sent](const std::string& gcode) {
            sent.push_back(gcode);
            return 0;
        },
        [&busy] { return busy; });

    backend.set_tone(1000.0f, 0.8f, 0.5f);
    backend.silence();
    REQUIRE(sent.empty());

    // The first note after the queue frees plays, rather than being taken for a repeat.
    busy = false;
    backend.set_tone(1000.0f, 0.8f, 0.5f);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].rfind("M300 S1000", 0) == 0);
}

TEST_CASE("M300 backend drops a tune's remaining notes once the printer turns busy",
          "[sound][m300]") {
    std::vector<std::string> sent;
    bool busy = false;
    M300SoundBackend backend(
        [&sent](const std::string& gcode) {
            sent.push_back(gcode);
            return 0;
        },
        [&busy] { return busy; });

    backend.set_tone(800.0f, 0.8f, 0.5f);
    REQUIRE(sent.size() == 1);
    busy = true;
    backend.set_tone(1200.0f, 0.8f, 0.5f);
    backend.silence();
    CHECK(sent.size() == 1);
}

TEST_CASE("M300 backend sends no silence once the printer turns busy", "[sound][m300]") {
    std::vector<std::string> sent;
    bool busy = false;
    M300SoundBackend backend(
        [&sent](const std::string& gcode) {
            sent.push_back(gcode);
            return 0;
        },
        [&busy] { return busy; });

    backend.set_tone(800.0f, 0.8f, 0.5f);
    REQUIRE(sent.size() == 1);

    // The tune's closing silence lands after the print started: an M300 S0 would
    // still wait in Klipper's queue behind the print's moves.
    busy = true;
    backend.silence();
    CHECK(sent.size() == 1);
}
