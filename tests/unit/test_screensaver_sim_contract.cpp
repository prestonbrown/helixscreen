// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../test_helpers/screensaver_sim_kit.h"
#include "screensaver_aqua_sim.h"
#include "screensaver_fireflies_sim.h"
#include "screensaver_fireworks_sim.h"
#include "screensaver_flip_clock_sim.h"
#include "screensaver_highfly_sim.h"
#include "screensaver_hyperspace_sim.h"
#include "screensaver_maze_sim.h"
#include "screensaver_pipes_sim.h"
#include "screensaver_starfield_sim.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <type_traits>

#include "../catch_amalgamated.hpp"

// What every sim promises its saver, checked once over every sim: dirty boxes cover what a step
// changes, rows stay inside their stride, XRGB8888 stays opaque, a seed replays and another
// seed differs, the heap never grows past what init sized and a still rung frees its scene
// (rule 7 in docs/devel/SCREENSAVERS.md), and clear() frees it all. A sim that takes a
// ClockReading is checked again on a print counting down: adding it to HELIX_EVERY_PRINT_SIM is
// all it takes. A sim's own tests cover the runs particular to it.

using helix::test::check_replays;
using helix::test::diff_against;
using helix::test::FrameDiff;
using helix::test::hms;
using helix::test::kUnevenFrameMs;
using helix::test::print_reading_for;
using helix::test::SimRun;
using helix::ui::AquaSim;
using helix::ui::ClockMode;
using helix::ui::ClockReading;
using helix::ui::FirefliesSim;
using helix::ui::FireworksSim;
using helix::ui::FlipClockSim;
using helix::ui::HighFlySim;
using helix::ui::HyperspaceSim;
using helix::ui::MazeArt;
using helix::ui::MazeSim;
using helix::ui::PipesSim;
using helix::ui::PixelFormat;
using helix::ui::StarfieldSim;

namespace {

/// A frame size no sim rounds evenly: odd, and no multiple of any rung's scale.
constexpr uint32_t W = 321;
constexpr uint32_t H = 203;

constexpr PixelFormat kFormats[] = {PixelFormat::XRGB8888, PixelFormat::RGB565};

/// Progress of the print a print run reads.
constexpr int32_t kContractPrintPct = 40;

/// A sim run with the inputs its sim reads besides time. The clock sims read the time of day,
/// which starts two seconds before a minute set by the seed, so a sim without an rng still
/// differs by seed and a short run turns a minute. A sim that takes a ClockReading reads that
/// time with no print (`mode` CLOCK), or a print in `mode` at kContractPrintPct counting down.
template <typename Sim> struct ContractRun {
    ContractRun(uint32_t seed, uint32_t w, uint32_t h, PixelFormat format,
                ClockMode mode = ClockMode::CLOCK)
        : run(seed, w, h, format), clock_start_s(hms(9, 41, 58) + static_cast<int32_t>(seed) * 60),
          mode(mode) {
        // A saver's canvas starts opaque black.
        helix::ui::PixelWriter(run.frame.target).fill({});
    }

    void init(size_t level);
    void step(uint32_t dt_ms) {
        elapsed_ms += dt_ms;
        run.dirty.clear();
        step_with_inputs(dt_ms);
    }
    int32_t seconds_of_day() const {
        return clock_start_s + static_cast<int32_t>(elapsed_ms / 1000);
    }
    ClockReading reading() const {
        const int32_t left = std::max(0, 3723 - static_cast<int32_t>(elapsed_ms / 1000));
        return print_reading_for(mode, kContractPrintPct, seconds_of_day(), left);
    }

    SimRun<Sim> run;
    int32_t clock_start_s;
    ClockMode mode;
    uint32_t elapsed_ms = 0;

  private:
    void step_with_inputs(uint32_t dt_ms);
};

template <typename Sim> void ContractRun<Sim>::init(size_t level) {
    if constexpr (helix::ui::sim_inits_with_v<Sim, const ClockReading&>) {
        run.init(level, reading());
    } else {
        run.init(level);
    }
}
template <typename Sim> void ContractRun<Sim>::step_with_inputs(uint32_t dt_ms) {
    if constexpr (helix::ui::sim_steps_with_v<Sim, const ClockReading&>) {
        run.step(dt_ms, reading());
    } else {
        run.step(dt_ms);
    }
}

template <> void ContractRun<MazeSim>::init(size_t level) {
    run.init(level, MazeArt{});
}

template <> void ContractRun<FlipClockSim>::init(size_t level) {
    run.init(level, helix::ui::clock_face(seconds_of_day(), true));
}
template <> void ContractRun<FlipClockSim>::step_with_inputs(uint32_t dt_ms) {
    run.step(dt_ms, helix::ui::clock_face(seconds_of_day(), true));
}

template <typename Sim> constexpr size_t level_count() {
    return std::size(Sim::kLevels);
}

/// True when the sim's ladder rows have a `still` flag: its last rung paints once, then keeps
/// only what redraws the clock.
template <typename Sim, typename = void> struct has_still_rung : std::false_type {};
template <typename Sim>
struct has_still_rung<Sim, std::void_t<decltype(Sim::kLevels[0].still)>> : std::true_type {};

template <typename Sim> void check_dirty_contract(ClockMode mode) {
    size_t changed = 0;
    for (const PixelFormat format : kFormats) {
        for (size_t level = 0; level < level_count<Sim>(); level++) {
            INFO("format " << static_cast<int>(format) << " level " << level);
            ContractRun<Sim> r(7, W, H, format, mode);
            r.init(level);
            const bool xrgb = format == PixelFormat::XRGB8888;
            REQUIRE(r.run.frame.padding_intact());
            if (xrgb) {
                REQUIRE(helix::test::non_opaque_pixels(r.run.frame.target) == 0);
            }
            size_t uncovered = 0;
            size_t overruns = 0;
            size_t see_through = 0;
            for (int i = 0; i < 60; i++) {
                const std::vector<uint8_t> before = r.run.frame.snapshot();
                r.step(kUnevenFrameMs[i % std::size(kUnevenFrameMs)]);
                const FrameDiff d = diff_against(before, r.run.frame, r.run.dirty);
                changed += d.changed;
                uncovered += d.uncovered;
                overruns += r.run.frame.padding_intact() ? 0 : 1;
                see_through += xrgb ? helix::test::non_opaque_pixels(r.run.frame.target) : 0;
            }
            CHECK(uncovered == 0);
            CHECK(overruns == 0);
            CHECK(see_through == 0);
        }
    }
    // The checks covered frames that moved, not only a still picture.
    CHECK(changed > 0);
}

template <typename Sim> void check_replays_by_seed(ClockMode mode) {
    auto run = [mode](uint32_t seed) {
        ContractRun<Sim> r(seed, W, H, PixelFormat::XRGB8888, mode);
        r.init(0);
        for (int i = 0; i < 90; i++) {
            r.step(kUnevenFrameMs[i % std::size(kUnevenFrameMs)]);
        }
        return helix::test::frame_fnv1a(r.run.frame.target);
    };
    check_replays(run, 11, 12);
}

template <typename Sim> void check_heap_contract(ClockMode mode) {
    for (const PixelFormat format : kFormats) {
        for (size_t start = 0; start < level_count<Sim>(); start++) {
            INFO("format " << static_cast<int>(format) << " start " << start);
            ContractRun<Sim> r(5, W, H, format, mode);
            r.init(start);
            const size_t sized = r.run.sim.heap_bytes();
            REQUIRE(sized > 0);
            size_t most = sized;
            for (size_t level = start; level < level_count<Sim>(); level++) {
                r.run.sim.request_level(level);
                for (int i = 0; i < 12; i++) {
                    r.step(kUnevenFrameMs[i % std::size(kUnevenFrameMs)]);
                    most = std::max(most, r.run.sim.heap_bytes());
                }
            }
            CHECK(most == sized);
            if constexpr (has_still_rung<Sim>::value) {
                // Once the still rung has painted, the scene behind the clock is freed.
                if (start + 1 < level_count<Sim>()) {
                    CHECK(r.run.sim.heap_bytes() * 2 < sized);
                }
            }
            r.run.sim.clear();
            CHECK(r.run.sim.heap_bytes() == 0);
        }
    }
}

} // namespace

#define HELIX_EVERY_SIM                                                                            \
    StarfieldSim, PipesSim, FireworksSim, MazeSim, HyperspaceSim, FlipClockSim, FirefliesSim,      \
        AquaSim, HighFlySim

/// The sims whose init() and step() take a ClockReading, run again on a print counting down.
#define HELIX_EVERY_PRINT_SIM AquaSim, HighFlySim, StarfieldSim, HyperspaceSim, FirefliesSim

TEMPLATE_TEST_CASE("a sim's dirty boxes cover what it changes, inside its rows and opaque, at "
                   "every level in both formats",
                   "[screensaver][screensaver_sim_contract]", HELIX_EVERY_SIM) {
    check_dirty_contract<TestType>(ClockMode::CLOCK);
}

TEMPLATE_TEST_CASE("a sim run replays exactly from a seed, and another seed differs",
                   "[screensaver][screensaver_sim_contract]", HELIX_EVERY_SIM) {
    check_replays_by_seed<TestType>(ClockMode::CLOCK);
}

TEMPLATE_TEST_CASE("a sim never holds more than init sized across frames and step-downs, from "
                   "every starting rung, the still rung frees its scene, and clear frees it all",
                   "[screensaver][screensaver_sim_contract]", HELIX_EVERY_SIM) {
    check_heap_contract<TestType>(ClockMode::CLOCK);
}

TEMPLATE_TEST_CASE("during a print, a sim's dirty boxes still cover what it changes, inside its "
                   "rows and opaque",
                   "[screensaver][screensaver_sim_contract]", HELIX_EVERY_PRINT_SIM) {
    check_dirty_contract<TestType>(ClockMode::COUNTDOWN);
}

TEMPLATE_TEST_CASE("during a print, a sim run still replays from a seed and differs by seed",
                   "[screensaver][screensaver_sim_contract]", HELIX_EVERY_PRINT_SIM) {
    check_replays_by_seed<TestType>(ClockMode::COUNTDOWN);
}

TEMPLATE_TEST_CASE("during a print, a sim still holds no more than init sized and clear frees it",
                   "[screensaver][screensaver_sim_contract]", HELIX_EVERY_PRINT_SIM) {
    check_heap_contract<TestType>(ClockMode::COUNTDOWN);
}

TEMPLATE_TEST_CASE("sim frame cost per level", "[.][saver_bench]", HELIX_EVERY_SIM) {
    std::ostringstream report;
    report << Catch::getResultCapture().getCurrentTestName() << "\n";
    for (size_t level = 0; level < level_count<TestType>(); level++) {
        ContractRun<TestType> r(3, 800, 480, PixelFormat::XRGB8888);
        r.init(level);
        const uint32_t period = helix::ui::ladder_period(TestType::kLevels, level);
        for (uint32_t t = 0; t < 5000; t += period) {
            r.step(period);
        }
        const double ms = helix::test::bench_ms_per_frame([&] { r.step(period); }, 300);
        report << "L" << level << " " << ms << " ms/frame\n";
    }
    WARN(report.str());
}

#endif // HELIX_ENABLE_SCREENSAVER
