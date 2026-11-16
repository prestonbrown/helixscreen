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
        in.now_ms += 60000;
        in.inactive_ms += 60000;
        seen = latch.step(in);
    }
    return seen;
}

/// Steps `latch` after `ms` more with no input.
ClockPrintPhase after(PrintEndLatch& latch, PrintLatchInput& in, uint32_t ms) {
    in.now_ms += ms;
    in.inactive_ms += ms;
    return latch.step(in);
}

/// A latch that has watched a print run, with `in` left reporting it running.
PrintEndLatch running_latch(PrintLatchInput& in) {
    PrintEndLatch latch;
    in.host = {ClockPrintPhase::PRINTING, 600, 64};
    in.printer_epoch = 1;
    in.now_ms = 1000;
    in.inactive_ms = 0;
    latch.step(in);
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

    in.host.phase = end;
    CHECK(after(latch, in, 16) == end);
    CHECK(latch.latched() == end);
    CHECK(idle_minutes(latch, in, 120) == end);
}

TEST_CASE("running phases pass through the latch", "[screensaver][screensaver_print_latch]") {
    const ClockPrintPhase phase =
        GENERATE(ClockPrintPhase::PREPARING, ClockPrintPhase::PRINTING, ClockPrintPhase::PAUSED);
    PrintLatchInput in;
    in.host.phase = phase;
    PrintEndLatch latch;
    CHECK(latch.step(in) == phase);
    CHECK(latch.latched() == ClockPrintPhase::NONE);
}

TEST_CASE("an end the latch never saw running reads as no print",
          "[screensaver][screensaver_print_latch]") {
    const ClockPrintPhase end =
        GENERATE(ClockPrintPhase::COMPLETE, ClockPrintPhase::FAILED, ClockPrintPhase::CANCELLED);
    PrintLatchInput in;
    in.host.phase = end;
    in.inactive_ms = 3600000;
    PrintEndLatch latch;
    CHECK(latch.step(in) == ClockPrintPhase::NONE);
    CHECK(idle_minutes(latch, in, 10) == ClockPrintPhase::NONE);
    CHECK(latch.latched() == ClockPrintPhase::NONE);
}

TEST_CASE("input after the end acknowledges it, input before does not",
          "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host.phase = ClockPrintPhase::COMPLETE;
    after(latch, in, 16);
    // Five idle minutes since the end, ten since the last input: the input came before it.
    idle_minutes(latch, in, 5);
    REQUIRE(latch.latched() == ClockPrintPhase::COMPLETE);

    // A touch: the display has been idle for less time than the end has been latched.
    in.inactive_ms = 0;
    CHECK(after(latch, in, 1000) == ClockPrintPhase::NONE);
    CHECK(latch.latched() == ClockPrintPhase::NONE);
    // The printer still reports complete: it stays acknowledged.
    CHECK(idle_minutes(latch, in, 30) == ClockPrintPhase::NONE);
}

TEST_CASE("a new print, a reconnect and a printer going idle clear the latch",
          "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host.phase = ClockPrintPhase::FAILED;
    after(latch, in, 16);
    REQUIRE(latch.latched() == ClockPrintPhase::FAILED);

    SECTION("a new print") {
        in.host.phase = ClockPrintPhase::PREPARING;
        CHECK(after(latch, in, 16) == ClockPrintPhase::PREPARING);
        CHECK(latch.latched() == ClockPrintPhase::NONE);
    }
    SECTION("a reconnect or another printer, still reporting the old end") {
        in.printer_epoch = 2;
        CHECK(after(latch, in, 16) == ClockPrintPhase::NONE);
        CHECK(latch.latched() == ClockPrintPhase::NONE);
    }
    SECTION("another printer that is printing") {
        in.printer_epoch = 2;
        in.host.phase = ClockPrintPhase::PRINTING;
        CHECK(after(latch, in, 16) == ClockPrintPhase::PRINTING);
    }
    SECTION("the printer going idle") {
        in.host.phase = ClockPrintPhase::NONE;
        CHECK(after(latch, in, 16) == ClockPrintPhase::NONE);
        in.host.phase = ClockPrintPhase::FAILED;
        CHECK(after(latch, in, 16) == ClockPrintPhase::NONE);
    }
}

TEST_CASE("a touch moments after the end acknowledges it",
          "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host.phase = ClockPrintPhase::COMPLETE;
    after(latch, in, 250);
    REQUIRE(latch.latched() == ClockPrintPhase::COMPLETE);
    // Touched 100 ms later; the next step comes another 150 ms on.
    in.now_ms += 250;
    in.inactive_ms = 150;
    CHECK(latch.step(in) == ClockPrintPhase::NONE);
}

TEST_CASE("an end keeps the progress the print reached", "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host = {ClockPrintPhase::CANCELLED, -1, 0};
    after(latch, in, 16);
    CHECK(latch.latched() == ClockPrintPhase::CANCELLED);
    CHECK(latch.latched_progress_pct() == 64);
}

TEST_CASE("display sleep does not clear the latch", "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host.phase = ClockPrintPhase::COMPLETE;
    after(latch, in, 16);
    CHECK(idle_minutes(latch, in, 8 * 60) == ClockPrintPhase::COMPLETE);
}

TEST_CASE("reset forgets the latch and what it saw", "[screensaver][screensaver_print_latch]") {
    PrintLatchInput in;
    PrintEndLatch latch = running_latch(in);
    in.host.phase = ClockPrintPhase::COMPLETE;
    after(latch, in, 16);
    latch.reset();
    CHECK(latch.latched() == ClockPrintPhase::NONE);
    CHECK(after(latch, in, 16) == ClockPrintPhase::NONE);
}

#endif // HELIX_ENABLE_SCREENSAVER
