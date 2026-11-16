// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "m300_sound_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// An M300 backend recording every gcode it sends, with a printer that is
/// busy while `busy` is set.
struct M300Fixture {
    std::vector<std::string> commands;
    bool busy = false;
    M300SoundBackend backend{[this](const std::string& cmd) {
                                 commands.push_back(cmd);
                                 return 0;
                             },
                             [this] { return busy; }};
};

} // namespace

// ============================================================================
// One tone
// ============================================================================

TEST_CASE_METHOD(M300Fixture, "M300 backend sends one tone as M300 S<freq> P<min tick>",
                 "[sound][m300]") {
    REQUIRE(backend.min_tick_ms() == 50.0f);
    struct Row {
        float freq;
        float amplitude;
        float duty;
        const char* gcode; ///< nullptr: nothing is sent
    };
    // clang-format off
    const Row rows[] = {
        {440.0f, 1.0f, 0.5f, "M300 S440 P50"},
        {523.25f, 1.0f, 0.5f, "M300 S523 P50"},   // the frequency truncates to an integer
        {50.0f, 1.0f, 0.5f, "M300 S100 P50"},     // clamped to 100..10000
        {15000.0f, 1.0f, 0.5f, "M300 S10000 P50"},
        {100.0f, 1.0f, 0.5f, "M300 S100 P50"},
        {10000.0f, 1.0f, 0.5f, "M300 S10000 P50"},
        {440.0f, 1.0f, 0.1f, "M300 S440 P50"},    // the duty cycle changes nothing
        {440.0f, 0.02f, 0.5f, "M300 S440 P50"},   // just above the amplitude threshold
        {440.0f, 0.01f, 0.5f, nullptr},           // at it: silence, which is a no-op from silent
    };
    // clang-format on
    for (const Row& r : rows) {
        CAPTURE(r.freq, r.amplitude, r.duty);
        commands.clear();
        M300SoundBackend fresh([this](const std::string& cmd) {
            commands.push_back(cmd);
            return 0;
        });
        fresh.set_tone(r.freq, r.amplitude, r.duty);
        if (r.gcode == nullptr) {
            CHECK(commands.empty());
        } else {
            REQUIRE(commands.size() == 1);
            CHECK(commands[0] == r.gcode);
        }
    }
}

// ============================================================================
// Sequences
// ============================================================================

TEST_CASE_METHOD(M300Fixture, "M300 backend sends a tone or a silence only when it changes",
                 "[sound][m300]") {
    using V = std::vector<std::string>;
    SECTION("a repeated frequency is one tone, whatever its amplitude") {
        backend.set_tone(440.0f, 1.0f, 0.5f);
        backend.set_tone(440.0f, 1.0f, 0.5f);
        backend.set_tone(440.0f, 0.8f, 0.5f);
        CHECK(commands == V{"M300 S440 P50"});
    }
    SECTION("another frequency in between sends the first again") {
        backend.set_tone(440.0f, 1.0f, 0.5f);
        backend.set_tone(880.0f, 1.0f, 0.5f);
        backend.set_tone(440.0f, 1.0f, 0.5f);
        CHECK(commands == V{"M300 S440 P50", "M300 S880 P50", "M300 S440 P50"});
    }
    SECTION("a silence in between sends it again, and silences are not repeated") {
        backend.set_tone(440.0f, 1.0f, 0.5f);
        backend.silence();
        backend.silence();
        backend.set_tone(440.0f, 1.0f, 0.5f);
        backend.silence();
        CHECK(commands == V{"M300 S440 P50", "M300 S0 P1", "M300 S440 P50", "M300 S0 P1"});
    }
    SECTION("silence from silent sends nothing") {
        backend.silence();
        CHECK(commands.empty());
    }
    SECTION("an amplitude at or under the threshold is a silence") {
        const float quiet = GENERATE(0.005f, 0.0f);
        backend.set_tone(440.0f, 1.0f, 0.5f);
        backend.set_tone(880.0f, quiet, 0.5f);
        CHECK(commands == V{"M300 S440 P50", "M300 S0 P1"});
    }
    SECTION("frequencies that clamp to one value are still two tones") {
        // The repeat check reads the integer before the clamp.
        backend.set_tone(50.0f, 1.0f, 0.5f);
        backend.set_tone(80.0f, 1.0f, 0.5f);
        CHECK(commands == V{"M300 S100 P50", "M300 S100 P50"});
    }
    SECTION("a negative frequency clamps up to 100") {
        backend.set_tone(440.0f, 1.0f, 0.5f);
        backend.set_tone(-50.0f, 1.0f, 0.5f);
        CHECK(commands == V{"M300 S440 P50", "M300 S100 P50"});
    }
}

TEST_CASE("M300 backend with no sender does nothing", "[sound][m300]") {
    M300SoundBackend null_backend(nullptr);
    REQUIRE_NOTHROW(null_backend.set_tone(440.0f, 1.0f, 0.5f));
    REQUIRE_NOTHROW(null_backend.set_tone(880.0f, 1.0f, 0.5f));
    REQUIRE_NOTHROW(null_backend.silence());
    REQUIRE_NOTHROW(null_backend.set_tone(440.0f, 0.0f, 0.5f));
    M300SoundBackend empty_backend(M300SoundBackend::GcodeSender{});
    REQUIRE_NOTHROW(empty_backend.set_tone(440.0f, 1.0f, 0.5f));
    REQUIRE_NOTHROW(empty_backend.silence());
}

TEST_CASE_METHOD(M300Fixture, "M300 backend reports correct default capabilities",
                 "[sound][m300]") {
    REQUIRE_FALSE(backend.supports_waveforms());
    REQUIRE_FALSE(backend.supports_amplitude());
    REQUIRE_FALSE(backend.supports_filter());
}

// ============================================================================
// A busy printer
// ============================================================================

TEST_CASE_METHOD(M300Fixture, "M300 backend sends nothing while the printer is busy",
                 "[sound][m300]") {
    busy = true;
    backend.set_tone(1000.0f, 0.8f, 0.5f);
    backend.silence();
    REQUIRE(commands.empty());

    // The first note after the queue frees plays, rather than being taken for a repeat.
    busy = false;
    backend.set_tone(1000.0f, 0.8f, 0.5f);
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].rfind("M300 S1000", 0) == 0);
}

TEST_CASE_METHOD(M300Fixture,
                 "M300 backend drops a tune's remaining notes and its silence once "
                 "the printer turns busy",
                 "[sound][m300]") {
    backend.set_tone(800.0f, 0.8f, 0.5f);
    REQUIRE(commands.size() == 1);
    busy = true;
    SECTION("a later note and the closing silence") {
        backend.set_tone(1200.0f, 0.8f, 0.5f);
        backend.silence();
    }
    SECTION("the closing silence alone") {
        // An M300 S0 would still wait in Klipper's queue behind the print's moves.
        backend.silence();
    }
    CHECK(commands.size() == 1);
}
