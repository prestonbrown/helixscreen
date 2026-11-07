// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "screensaver_print_latch.h"

#include "../catch_amalgamated.hpp"

using helix::ui::ClockPrintPhase;
using helix::ui::PrintEndLatch;
using helix::ui::PrintLatchInput;

namespace {

/// Steps `latch` one minute at a time for `minutes`, with no user input throughout.
ClockPrintPhase idle_minutes(PrintEndLatch& latch, PrintLatchInput& in, int minutes) {
    ClockPrintPhase seen = ClockPrintPhase::NONE;
    for (int i = 0; i < minutes; i++) {
        in.inactive_ms += 60000;
        seen = latch.step(60000, in);
    }
    return seen;
}

/// A latch that has watched a print run, with `in` left reporting it running.
PrintEndLatch running_latch(PrintLatchInput& in) {
    PrintEndLatch latch;
    in.host_phase = ClockPrintPhase::PRINTING;
    in.printer_epoch = 1;
    in.inactive_ms = 0;
    latch.step(0, in);
    idle_minutes(latch, in, 5);
    return latch;
}

} // namespace

TEST_CASE("a print that ends while running latches its end",
          "[screensaver][screensaver_print_latch]") {
    const ClockPrintPhase end =
        GENERATE(ClockPrintPhase::COMPLETE, ClockPrintPhase::FAILED, ClockPrintPhase::CANCELLED);
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    CHECK(latch.latched() == ClockPrintPhase::NONE);

    in.host_phase = end;
    CHECK(latch.step(16, in) == end);
    CHECK(latch.latched() == end);
    CHECK(idle_minutes(latch, in, 120) == end);
}

TEST_CASE("running phases pass through the latch", "[screensaver][screensaver_print_latch]") {
    const ClockPrintPhase phase =
        GENERATE(ClockPrintPhase::PREPARING, ClockPrintPhase::PRINTING, ClockPrintPhase::PAUSED);
    PrintLatchInput in;
    in.host_phase = phase;
    PrintEndLatch latch;
    CHECK(latch.step(0, in) == phase);
    CHECK(latch.latched() == ClockPrintPhase::NONE);
}

TEST_CASE("an end the latch never saw running reads as no print",
          "[screensaver][screensaver_print_latch]") {
    const ClockPrintPhase end =
        GENERATE(ClockPrintPhase::COMPLETE, ClockPrintPhase::FAILED, ClockPrintPhase::CANCELLED);
    PrintLatchInput in;
    in.host_phase = end;
    in.inactive_ms = 3600000;
    PrintEndLatch latch;
    CHECK(latch.step(0, in) == ClockPrintPhase::NONE);
    CHECK(idle_minutes(latch, in, 10) == ClockPrintPhase::NONE);
    CHECK(latch.latched() == ClockPrintPhase::NONE);
}

TEST_CASE("input after the end acknowledges it, input before does not",
          "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host_phase = ClockPrintPhase::COMPLETE;
    latch.step(16, in);
    // Five idle minutes since the end, ten since the last input: the input came before it.
    idle_minutes(latch, in, 5);
    REQUIRE(latch.latched() == ClockPrintPhase::COMPLETE);

    // A touch: the display has been idle for less time than the end has been latched.
    in.inactive_ms = 1000;
    CHECK(latch.step(1000, in) == ClockPrintPhase::NONE);
    CHECK(latch.latched() == ClockPrintPhase::NONE);
    // The printer still reports complete: it stays acknowledged.
    CHECK(idle_minutes(latch, in, 30) == ClockPrintPhase::NONE);
}

TEST_CASE("a new print, a reconnect and a printer going idle clear the latch",
          "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host_phase = ClockPrintPhase::FAILED;
    latch.step(16, in);
    REQUIRE(latch.latched() == ClockPrintPhase::FAILED);

    SECTION("a new print") {
        in.host_phase = ClockPrintPhase::PREPARING;
        CHECK(latch.step(16, in) == ClockPrintPhase::PREPARING);
        CHECK(latch.latched() == ClockPrintPhase::NONE);
    }
    SECTION("a reconnect or another printer, still reporting the old end") {
        in.printer_epoch = 2;
        CHECK(latch.step(16, in) == ClockPrintPhase::NONE);
        CHECK(latch.latched() == ClockPrintPhase::NONE);
    }
    SECTION("another printer that is printing") {
        in.printer_epoch = 2;
        in.host_phase = ClockPrintPhase::PRINTING;
        CHECK(latch.step(16, in) == ClockPrintPhase::PRINTING);
    }
    SECTION("the printer going idle") {
        in.host_phase = ClockPrintPhase::NONE;
        CHECK(latch.step(16, in) == ClockPrintPhase::NONE);
        in.host_phase = ClockPrintPhase::FAILED;
        CHECK(latch.step(16, in) == ClockPrintPhase::NONE);
    }
}

TEST_CASE("display sleep does not clear the latch", "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host_phase = ClockPrintPhase::COMPLETE;
    latch.step(16, in);
    CHECK(idle_minutes(latch, in, 8 * 60) == ClockPrintPhase::COMPLETE);
}

TEST_CASE("reset forgets the latch and what it saw", "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host_phase = ClockPrintPhase::COMPLETE;
    latch.step(16, in);
    latch.reset();
    CHECK(latch.latched() == ClockPrintPhase::NONE);
    CHECK(latch.step(16, in) == ClockPrintPhase::NONE);
}

#endif // HELIX_ENABLE_SCREENSAVER
