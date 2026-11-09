// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/screensaver_cost_model.h"
#include "../test_helpers/screensaver_test_access.h"
#include "screensaver_frame.h"
#include "screensaver_pixel_writer.h"
#include "screensaver_starfield.h"
#include "screensaver_starfield_sim.h"

#include <cstdint>
#include <random>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::DirtyRect;
using helix::ui::FrameTarget;
using helix::ui::StarfieldSim;
using StarAccess = StarfieldScreensaverTestAccess;
using helix::test::SaverLoad;

namespace {

/// A reading with no print: the sky as it flies between prints.
const helix::ui::ClockReading kNoPrint{};

// The panel a two-core board drives, so coverage is a fraction of the real canvas.
constexpr uint32_t kPanelW = 800;
constexpr uint32_t kPanelH = 480;
constexpr int64_t kPanelPx = static_cast<int64_t>(kPanelW) * kPanelH;

/// Flies `stars` for `frames` and averages what each frame asked to have redrawn.
SaverLoad measure(int stars, int frames) {
    std::vector<uint8_t> bytes(static_cast<size_t>(kPanelW) * kPanelH * 2, 0);
    FrameTarget target{bytes.data(), kPanelW * 2, kPanelW, kPanelH, helix::ui::PixelFormat::RGB565};
    std::minstd_rand rng(20260916);
    StarfieldSim sim;
    sim.init(target, rng, 0, kNoPrint);
    sim.set_active_count(stars);

    std::vector<DirtyRect> dirty;
    SaverLoad load;
    for (int f = 0; f < frames; f++) {
        sim.step(33, target, rng, kNoPrint, dirty);
        load.add_frame(dirty);
    }
    load.average_over(frames);
    return load;
}

} // namespace

// A saver that reports one box per moving object keeps a frame's redraw proportional to the
// objects that moved. Reporting one box spanning them all instead puts the whole canvas in
// every frame, which is what the coverage figures here are guarding.
TEST_CASE("a starfield frame redraws a sliver of the canvas at every rung",
          "[screensaver][starfield][perf]") {
    StarfieldScreensaver saver;
    const size_t rungs = StarAccess::ladder_size(saver);
    REQUIRE(rungs >= 2);

    for (size_t level = 0; level < rungs; level++) {
        CAPTURE(level);
        const auto stars = static_cast<int>(StarAccess::star_count(saver, level));
        REQUIRE(stars > 0);

        const SaverLoad load = measure(stars, 240);

        // One box per star that moved, and never more boxes than LVGL will hold: past that
        // it drops every pending area and invalidates the screen, which costs a full redraw.
        CHECK(load.areas <= static_cast<double>(stars));
        CHECK(load.areas <= static_cast<double>(helix::ui::kSaverMaxDirtyAreas));

        // Coverage well under the whole-canvas promotion, so a frame stays partial.
        const double coverage = 100.0 * load.pixels / static_cast<double>(kPanelPx);
        CAPTURE(coverage);
        CHECK(coverage < 5.0);
        CHECK(coverage * 100.0 < static_cast<double>(helix::ui::kSaverWholeCanvasPercent) * 100.0);
    }
}

// The ladder exists so a board that cannot afford the top rung has somewhere to go. If its
// last rung does not fit a two-core budget, the gate runs out of rungs and blanks the screen.
TEST_CASE("the starfield's lowest rung fits a two-core board", "[screensaver][starfield][perf]") {
    StarfieldScreensaver saver;
    const size_t last = StarAccess::ladder_size(saver) - 1;
    const auto stars = static_cast<int>(StarAccess::star_count(saver, last));

    const SaverLoad load = measure(stars, 240);
    CAPTURE(stars, load.areas, load.pixels);
    CHECK(load.cost() < helix::test::budget_pct(2));

    // And the rungs are ordered: each costs less than the one above it.
    double previous = 0.0;
    for (size_t level = 0; level <= last; level++) {
        const SaverLoad rung = measure(static_cast<int>(StarAccess::star_count(saver, level)), 120);
        CAPTURE(level, rung.areas);
        if (level > 0) {
            CHECK(rung.cost() < previous);
        }
        previous = rung.cost();
    }
}

#endif // HELIX_ENABLE_SCREENSAVER
