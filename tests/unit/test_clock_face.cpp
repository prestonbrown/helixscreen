// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/screensaver_sim_kit.h"
#include "screensaver_analog_clock_sim.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_nixie_clock_sim.h"
#include "screensaver_pixel_writer.h"
#include "src/ui/panel_widgets/clock_face.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ClockFace;
using helix::ClockFaceTempo;
using helix::kClockFaceCount;
using helix::test::hms;
using helix::test::reading_at;
using helix::test::TestFrame;
using helix::ui::AnalogClockOptions;
using helix::ui::AnalogClockSim;
using helix::ui::AnalogFace;
using helix::ui::AnalogStyle;
using helix::ui::AnalogText;
using helix::ui::AnalogTextRole;
using helix::ui::DirtyRect;
using helix::ui::PixelFormat;
using helix::ui::SaverLevelEntry;

namespace {

constexpr ClockFace kPainted[] = {ClockFace::CHRONOGRAPH, ClockFace::STATION, ClockFace::NEON,
                                  ClockFace::BAUHAUS,     ClockFace::ORRERY,  ClockFace::FLIP,
                                  ClockFace::NIXIE};

SaverLevelEntry entry(size_t level, bool too_heavy) {
    return SaverLevelEntry{level, too_heavy, "v", "b"};
}

} // namespace

TEST_CASE("every clock face round-trips through its stored name, and an unknown name reads Text",
          "[clock_face]") {
    for (size_t i = 0; i < kClockFaceCount; ++i) {
        const auto face = static_cast<ClockFace>(i);
        CHECK(helix::parse_clock_face(helix::clock_face_id(face)) == face);
    }
    CHECK(helix::parse_clock_face("") == ClockFace::TEXT);
    CHECK(helix::parse_clock_face("sundial") == ClockFace::TEXT);
}

TEST_CASE("a face shows from its minimum size up and Text below it", "[clock_face]") {
    for (const ClockFace face : kPainted) {
        CAPTURE(helix::clock_face_id(face));
        // The smallest square the face fits, found by growing one pixel at a time.
        int32_t side = 8;
        while (helix::clock_face_shown(face, side, side) != face) {
            REQUIRE(side < 600);
            ++side;
        }
        CHECK(helix::clock_face_measure_px(face, side, side) >= helix::clock_face_min_px(face));
        CHECK(helix::clock_face_shown(face, side - 1, side - 1) == ClockFace::TEXT);
    }
    CHECK(helix::clock_face_shown(ClockFace::TEXT, 400, 400) == ClockFace::TEXT);
}

TEST_CASE("a 1x1 clock shows a dial at 800x480 and Text at 480x320", "[clock_face]") {
    // A one-cell tile's content box: about 100x98 at 800x480 and 70x64 at 480x320.
    CHECK(helix::clock_face_shown(ClockFace::STATION, 100, 98) == ClockFace::STATION);
    CHECK(helix::clock_face_shown(ClockFace::FLIP, 100, 98) == ClockFace::FLIP);
    for (const ClockFace face : kPainted) {
        CAPTURE(helix::clock_face_id(face));
        CHECK(helix::clock_face_shown(face, 70, 64) == ClockFace::TEXT);
    }
    // The chronograph needs room for its subdials: Text where a plain dial fits.
    CHECK(helix::clock_face_shown(ClockFace::CHRONOGRAPH, 100, 98) == ClockFace::TEXT);
}

TEST_CASE("a face reuses its saver's measurement: seconds where the board affords them",
          "[clock_face]") {
    SECTION("too heavy: the bottom rung, a change a minute") {
        const ClockFaceTempo t = helix::clock_face_tempo(ClockFace::NEON, entry(2, true), 4);
        CHECK(t.level == 2);
        CHECK_FALSE(t.seconds);
    }
    SECTION("measured: the settled rung, never the saver's 16 ms top rung") {
        const ClockFaceTempo top = helix::clock_face_tempo(ClockFace::NEON, entry(0, false), 4);
        CHECK(top.level == helix::kClockTileFastestLevel);
        CHECK(top.seconds);
        CHECK(top.period_ms == helix::ui::kAnalogClockLevels[1].period_ms);
        const ClockFaceTempo low = helix::clock_face_tempo(ClockFace::NEON, entry(2, false), 4);
        CHECK(low.level == 2);
        CHECK(low.seconds);
    }
    SECTION("never measured: ticking seconds on two cores or more, none on one") {
        const ClockFaceTempo multi = helix::clock_face_tempo(ClockFace::STATION, std::nullopt, 2);
        CHECK(multi.level == 2);
        CHECK(multi.seconds);
        CHECK_FALSE(helix::clock_face_tempo(ClockFace::STATION, std::nullopt, 1).seconds);
    }
    SECTION("cards animate where their rung animates, and never show seconds") {
        const ClockFaceTempo flip = helix::clock_face_tempo(ClockFace::FLIP, entry(0, false), 4);
        CHECK(flip.level == 1);
        CHECK(flip.animate);
        CHECK_FALSE(flip.seconds);
        CHECK_FALSE(helix::clock_face_tempo(ClockFace::NIXIE, std::nullopt, 4).animate);
        CHECK_FALSE(helix::clock_face_tempo(ClockFace::NIXIE, entry(1, true), 4).animate);
    }
}

TEST_CASE("the wait to the next whole minute", "[clock_face]") {
    CHECK(helix::clock_ms_to_next_minute(hms(10, 0, 0), 0) == 60000);
    CHECK(helix::clock_ms_to_next_minute(hms(10, 0, 59), 999) == 1);
    CHECK(helix::clock_ms_to_next_minute(hms(10, 0, 30), 250) == 29750);
}

TEST_CASE("an analog face without seconds writes nothing until the minute turns", "[clock_face]") {
    for (const bool seconds : {false, true}) {
        CAPTURE(seconds);
        TestFrame frame(160, 160, PixelFormat::XRGB8888);
        AnalogClockSim sim;
        sim.set_options({false, seconds, false});
        AnalogFace face{AnalogStyle::STATION, reading_at(hms(10, 0, 5))};
        sim.init(frame.target, 2, face);
        std::vector<DirtyRect> dirty;
        size_t frames_written = 0;
        for (int32_t s = 6; s < 60; ++s) {
            face.reading = reading_at(hms(10, 0, s));
            sim.step(1000, frame.target, face, dirty);
            frames_written += dirty.empty() ? 0 : 1;
        }
        // With seconds the hand moves every second; without, nothing moves inside the minute.
        CHECK(frames_written == (seconds ? 54u : 0u));
        face.reading = reading_at(hms(10, 1, 0));
        sim.step(1000, frame.target, face, dirty);
        CHECK_FALSE(dirty.empty());
    }
}

TEST_CASE("a face held still never drifts, and a saver's face does", "[clock_face]") {
    for (const bool drift : {false, true}) {
        CAPTURE(drift);
        TestFrame frame(200, 200, PixelFormat::XRGB8888);
        AnalogClockSim sim;
        sim.set_options({drift, true, true});
        AnalogFace face{AnalogStyle::NEON, reading_at(hms(10, 0, 0))};
        sim.init(frame.target, 2, face);
        const float cx = sim.geom().cx;
        const float cy = sim.geom().cy;
        std::vector<DirtyRect> dirty;
        bool moved = false;
        for (int32_t m = 1; m <= 10; ++m) {
            face.reading = reading_at(hms(10, m, 0));
            sim.step(helix::ui::kClockDriftStepMs, frame.target, face, dirty);
            moved = moved || sim.geom().cx != cx || sim.geom().cy != cy;
        }
        CHECK(moved == drift);
    }
}

TEST_CASE("held-still cards never relayout between minutes", "[clock_face]") {
    for (const bool drift : {false, true}) {
        CAPTURE(drift);
        TestFrame frame(220, 100, PixelFormat::XRGB8888);
        const helix::ui::FlipFace face = helix::ui::clock_face(hms(10, 0, 0), true);
        helix::ui::FlipClockSim flip;
        helix::ui::NixieClockSim nixie;
        flip.set_drift(drift);
        nixie.set_drift(drift);
        flip.init(frame.target, 2, face);
        std::vector<DirtyRect> dirty;
        bool flip_wrote = false;
        for (int i = 0; i < 10; ++i) {
            flip.step(helix::ui::kClockDriftStepMs, frame.target, face, dirty);
            flip_wrote = flip_wrote || !dirty.empty();
        }
        CHECK(flip_wrote == drift);

        TestFrame tubes(220, 100, PixelFormat::XRGB8888);
        nixie.init(tubes.target, 2, face);
        bool nixie_wrote = false;
        for (int i = 0; i < 10; ++i) {
            nixie.step(helix::ui::kClockDriftStepMs, tubes.target, face, dirty);
            nixie_wrote = nixie_wrote || !dirty.empty();
        }
        CHECK(nixie_wrote == drift);
    }
}

TEST_CASE("Bauhaus without its column centres the dial and places no column text", "[clock_face]") {
    const helix::ui::AnalogGeom g = helix::ui::analog_layout(AnalogStyle::BAUHAUS, 300, 200, false);
    CHECK(g.cx == Catch::Approx(150.0f));
    CHECK(g.col_w == 0.0f);

    TestFrame frame(300, 200, PixelFormat::XRGB8888);
    AnalogClockSim sim;
    sim.set_options({false, true, false});
    sim.init(frame.target, 2, AnalogFace{AnalogStyle::BAUHAUS, reading_at(hms(10, 8, 30))});
    for (const AnalogText& t : sim.texts()) {
        CHECK(t.role != AnalogTextRole::COLUMN_WEEKDAY);
        CHECK(t.role != AnalogTextRole::COLUMN_DATE);
    }
    // The saver keeps its column.
    const helix::ui::AnalogGeom saver = helix::ui::analog_layout(AnalogStyle::BAUHAUS, 300, 200);
    CHECK(saver.col_w > 0.0f);
}

#endif // HELIX_ENABLE_SCREENSAVER
