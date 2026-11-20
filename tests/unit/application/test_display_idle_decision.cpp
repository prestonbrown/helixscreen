// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_idle_decision.cpp
 * @brief Truth table for decide_idle(): the dim / saver / sleep / wake tick.
 *
 * Each row is one tick's inputs and the action it must produce. Row labels name
 * the rule the row pins.
 */

#include "display_idle_decision.h"

#include <vector>

#include "../../catch_amalgamated.hpp"

using helix::IdleAction;
using helix::IdleInputs;
using helix::IdleState;

namespace {

struct Row {
    const char* label;
    IdleInputs in;
    IdleAction want;
};

IdleInputs make(IdleState st, uint32_t inactive_ms, int dim_s, int sleep_s) {
    IdleInputs in;
    in.state = st;
    in.inactive_ms = inactive_ms;
    in.dim_timeout_sec = dim_s;
    in.sleep_timeout_sec = sleep_s;
    return in;
}

template <typename F> IdleInputs with(IdleInputs in, F f) {
    f(in);
    return in;
}

} // namespace

TEST_CASE("decide_idle truth table", "[display][idle_decision]") {
    const auto A = IdleState::Awake;
    const auto D = IdleState::Dimmed;
    const auto S = IdleState::Sleeping;

    const std::vector<Row> rows = {
        // --- Awake: timeouts ---
        {"awake, fresh activity", with(make(A, 0, 60, 120), [](auto& i) { i.can_dim = true; }),
         IdleAction::None},
        {"awake, below dim", with(make(A, 59999, 60, 120), [](auto& i) { i.can_dim = true; }),
         IdleAction::None},
        {"awake, exactly at dim, backlight",
         with(make(A, 60000, 60, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Dim},
        {"awake, past dim, backlight",
         with(make(A, 90000, 60, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Dim},
        {"awake, exactly at sleep",
         with(make(A, 120000, 60, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Sleep},
        {"awake, one ms below sleep",
         with(make(A, 119999, 60, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Dim},
        // --- Awake: dim 0 / sleep 0 ---
        {"dim 0, past dim, no sleep",
         with(make(A, 999999, 0, 0), [](auto& i) { i.can_dim = true; }), IdleAction::None},
        {"dim 0, sleep set, below sleep",
         with(make(A, 50000, 0, 120), [](auto& i) { i.can_dim = true; }), IdleAction::None},
        {"dim 0, sleep set, at sleep",
         with(make(A, 120000, 0, 120), [](auto& i) { i.can_dim = true; }), IdleAction::Sleep},
        {"sleep 0, dim set, at dim", with(make(A, 60000, 60, 0), [](auto& i) { i.can_dim = true; }),
         IdleAction::Dim},
        {"sleep 0, dim set, very late",
         with(make(A, 4000000, 60, 0), [](auto& i) { i.can_dim = true; }), IdleAction::Dim},
        // --- Awake: sleep < dim (sleep is checked first) ---
        {"sleep<dim, at sleep", with(make(A, 30000, 60, 30), [](auto& i) { i.can_dim = true; }),
         IdleAction::Sleep},
        {"sleep<dim, below sleep", with(make(A, 29999, 60, 30), [](auto& i) { i.can_dim = true; }),
         IdleAction::None},
        {"sleep<dim, past both", with(make(A, 70000, 60, 30), [](auto& i) { i.can_dim = true; }),
         IdleAction::Sleep},
        // --- Awake: can_dim / has_screensaver ---
        {"past dim, neither dim nor saver", make(A, 60000, 60, 0), IdleAction::None},
        {"past dim, saver only, not running",
         with(make(A, 60000, 60, 0), [](auto& i) { i.has_screensaver = true; }),
         IdleAction::StartSaver},
        {"past dim, saver and backlight, not running",
         with(make(A, 60000, 60, 0),
              [](auto& i) {
                  i.has_screensaver = true;
                  i.can_dim = true;
              }),
         IdleAction::StartSaver},
        {"past dim, saver already running falls to Dim",
         with(make(A, 60000, 60, 0),
              [](auto& i) {
                  i.has_screensaver = true;
                  i.can_dim = true;
                  i.saver_running = true;
              }),
         IdleAction::Dim},
        {"saver only, no sleep timeout, sleep does not preempt",
         with(make(A, 60000, 60, 0), [](auto& i) { i.has_screensaver = true; }),
         IdleAction::StartSaver},
        {"saver only, at sleep",
         with(make(A, 120000, 60, 120), [](auto& i) { i.has_screensaver = true; }),
         IdleAction::Sleep},
        // --- Awake: inhibit ---
        {"awake, inhibited, past dim",
         with(make(A, 90000, 60, 0),
              [](auto& i) {
                  i.can_dim = true;
                  i.inhibit_entry = true;
              }),
         IdleAction::None},
        {"awake, inhibited, past sleep",
         with(make(A, 999999, 60, 120),
              [](auto& i) {
                  i.can_dim = true;
                  i.inhibit_entry = true;
              }),
         IdleAction::None},
        // --- Awake: wake flags are irrelevant ---
        {"awake, wake_requested ignored",
         with(make(A, 0, 60, 120), [](auto& i) { i.wake_requested = true; }), IdleAction::None},
        {"awake, host_resumed ignored",
         with(make(A, 0, 60, 120), [](auto& i) { i.host_resumed = true; }), IdleAction::None},
        // --- Dimmed ---
        {"dimmed, idle, below sleep", make(D, 90000, 60, 120), IdleAction::None},
        {"dimmed, at sleep", make(D, 120000, 60, 120), IdleAction::Sleep},
        {"dimmed, sleep 0, very late", make(D, 4000000, 60, 0), IdleAction::None},
        {"dimmed, activity 499ms", make(D, 499, 60, 120), IdleAction::Wake},
        {"dimmed, 500ms is no longer activity", make(D, 500, 60, 120), IdleAction::None},
        {"dimmed, wake_requested",
         with(make(D, 90000, 60, 120), [](auto& i) { i.wake_requested = true; }), IdleAction::Wake},
        {"dimmed, inhibited, at sleep",
         with(make(D, 120000, 60, 120), [](auto& i) { i.inhibit_entry = true; }), IdleAction::None},
        {"dimmed, inhibited, activity still wakes",
         with(make(D, 100, 60, 120), [](auto& i) { i.inhibit_entry = true; }), IdleAction::Wake},
        {"dimmed, host_resumed alone does not wake",
         with(make(D, 90000, 60, 120), [](auto& i) { i.host_resumed = true; }), IdleAction::None},
        // --- Dimmed: preview grace ---
        {"preview, activity at 749ms is ignored",
         with(make(D, 100, 60, 120),
              [](auto& i) {
                  i.is_preview = true;
                  i.preview_elapsed_ms = 749;
              }),
         IdleAction::None},
        {"preview, activity at 750ms dismisses",
         with(make(D, 100, 60, 120),
              [](auto& i) {
                  i.is_preview = true;
                  i.preview_elapsed_ms = 750;
              }),
         IdleAction::Wake},
        {"preview, wake_requested wakes inside the grace",
         with(make(D, 100, 60, 120),
              [](auto& i) {
                  i.is_preview = true;
                  i.preview_elapsed_ms = 10;
                  i.wake_requested = true;
              }),
         IdleAction::Wake},
        {"non-preview, activity at 10ms wakes",
         with(make(D, 100, 60, 120), [](auto& i) { i.preview_elapsed_ms = 10; }), IdleAction::Wake},
        // --- Sleeping ---
        {"sleeping, idle", make(S, 999999, 60, 120), IdleAction::None},
        {"sleeping, activity", make(S, 100, 60, 120), IdleAction::Wake},
        {"sleeping, 500ms is not activity", make(S, 500, 60, 120), IdleAction::None},
        {"sleeping, wake_requested",
         with(make(S, 999999, 60, 120), [](auto& i) { i.wake_requested = true; }),
         IdleAction::Wake},
        {"sleeping, host_resumed",
         with(make(S, 999999, 60, 120), [](auto& i) { i.host_resumed = true; }), IdleAction::Wake},
        {"sleeping, inhibited, wake_requested is honoured",
         with(make(S, 999999, 60, 120),
              [](auto& i) {
                  i.inhibit_entry = true;
                  i.wake_requested = true;
              }),
         IdleAction::Wake},
        {"sleeping, inhibited, activity is honoured",
         with(make(S, 10, 60, 120), [](auto& i) { i.inhibit_entry = true; }), IdleAction::Wake},
        {"sleeping, inhibited, idle stays asleep",
         with(make(S, 999999, 60, 120), [](auto& i) { i.inhibit_entry = true; }), IdleAction::None},
        {"sleeping, preview flag ignored",
         with(make(S, 100, 60, 120),
              [](auto& i) {
                  i.is_preview = true;
                  i.preview_elapsed_ms = 0;
              }),
         IdleAction::Wake},
    };

    REQUIRE(rows.size() >= 40);
    for (const auto& r : rows) {
        INFO(r.label);
        CHECK(helix::decide_idle(r.in).action == r.want);
    }
}

TEST_CASE("decide_idle: a remote viewer caps idle at dim", "[display][idle_decision]") {
    const auto A = IdleState::Awake;
    const auto D = IdleState::Dimmed;
    auto viewed = [](IdleInputs in) {
        in.remote_viewer = true;
        return in;
    };

    SECTION("no timeout combination sleeps while viewed") {
        const int timeouts[] = {0, 1, 30, 60, 120, 600};
        const uint32_t inactive[] = {0, 499, 1000, 30000, 60000, 120000, 600000, 3600000};
        int sleep_due_rows = 0;
        for (IdleState st : {A, D}) {
            for (int dim_s : timeouts) {
                for (int sleep_s : timeouts) {
                    for (uint32_t ms : inactive) {
                        for (int caps = 0; caps < 4; ++caps) {
                            IdleInputs in = make(st, ms, dim_s, sleep_s);
                            in.can_dim = (caps & 1) != 0;
                            in.has_screensaver = (caps & 2) != 0;
                            in.saver_running = st == D && in.has_screensaver;
                            if (helix::decide_idle(in).action == IdleAction::Sleep) {
                                ++sleep_due_rows;
                            }
                            const auto d = helix::decide_idle(viewed(in));
                            INFO("state " << static_cast<int>(st) << " dim " << dim_s << " sleep "
                                          << sleep_s << " inactive " << ms << " caps " << caps);
                            CHECK(d.action != IdleAction::Sleep);
                        }
                    }
                }
            }
        }
        // The sweep has to include ticks that sleep unviewed, or it proves nothing.
        REQUIRE(sleep_due_rows > 100);
    }

    SECTION("viewed, the dim stage still runs") {
        auto backlight = [](IdleInputs in) {
            in.can_dim = true;
            return in;
        };
        auto saver = [](IdleInputs in) {
            in.has_screensaver = true;
            return in;
        };
        CHECK(helix::decide_idle(viewed(backlight(make(A, 60000, 60, 120)))).action ==
              IdleAction::Dim);
        CHECK(helix::decide_idle(viewed(saver(make(A, 60000, 60, 120)))).action ==
              IdleAction::StartSaver);
        // Past the sleep timeout and never dimmed: the capped sleep dims instead.
        CHECK(helix::decide_idle(viewed(backlight(make(A, 200000, 60, 120)))).action ==
              IdleAction::Dim);
        CHECK(helix::decide_idle(viewed(saver(make(A, 200000, 60, 120)))).action ==
              IdleAction::StartSaver);
        // Sleep set, dim "never": dimming at the sleep timeout is the most it may do.
        CHECK(helix::decide_idle(viewed(backlight(make(A, 120000, 0, 120)))).action ==
              IdleAction::Dim);
        CHECK(helix::decide_idle(viewed(saver(make(A, 120000, 0, 120)))).action ==
              IdleAction::StartSaver);
        // Sleep before dim: the sleep timeout is when it dims.
        CHECK(helix::decide_idle(viewed(backlight(make(A, 60000, 120, 60)))).action ==
              IdleAction::Dim);
        // Nothing to dim with: it stays as it is.
        CHECK(helix::decide_idle(viewed(make(A, 200000, 60, 120))).action == IdleAction::None);
        // Before any timeout nothing happens.
        CHECK(helix::decide_idle(viewed(backlight(make(A, 30000, 60, 120)))).action ==
              IdleAction::None);
        // An unviewed sleep-before-dim still sleeps.
        CHECK(helix::decide_idle(backlight(make(A, 60000, 120, 60))).action == IdleAction::Sleep);
    }

    SECTION("the decision says when the viewer held a sleep back") {
        IdleInputs dimmed = make(D, 200000, 60, 120);
        dimmed.can_dim = true;
        CHECK(helix::decide_idle(viewed(dimmed)).action == IdleAction::None);
        CHECK(helix::decide_idle(viewed(dimmed)).sleep_held);
        CHECK_FALSE(helix::decide_idle(dimmed).sleep_held);
        // Not yet due: nothing is being held.
        CHECK_FALSE(helix::decide_idle(viewed(make(D, 61000, 60, 120))).sleep_held);
        // A refused entry is not the viewer's doing.
        IdleInputs inhibited = viewed(dimmed);
        inhibited.inhibit_entry = true;
        CHECK_FALSE(helix::decide_idle(inhibited).sleep_held);
        IdleInputs awake = viewed(make(A, 200000, 60, 120));
        awake.can_dim = true;
        CHECK(helix::decide_idle(awake).sleep_held);
    }

    SECTION("the viewer leaving lets the overdue sleep happen on the next tick") {
        IdleInputs in = viewed(make(D, 600000, 60, 120));
        in.can_dim = true;
        REQUIRE(helix::decide_idle(in).action == IdleAction::None);
        in.remote_viewer = false;
        CHECK(helix::decide_idle(in).action == IdleAction::Sleep);
    }

    SECTION("a viewer arriving wakes a sleeping display, once") {
        IdleInputs asleep = viewed(make(IdleState::Sleeping, 999999, 60, 120));
        asleep.remote_viewer_arrived = true;
        CHECK(helix::decide_idle(asleep).action == IdleAction::Wake);
        // Waking is never refused, even while entry is inhibited.
        asleep.inhibit_entry = true;
        CHECK(helix::decide_idle(asleep).action == IdleAction::Wake);
        // A viewer that stays does not keep waking it.
        asleep.inhibit_entry = false;
        asleep.remote_viewer_arrived = false;
        CHECK(helix::decide_idle(asleep).action == IdleAction::None);
        // Arrival changes nothing on a display that is not asleep.
        IdleInputs dimmed = viewed(make(D, 90000, 60, 120));
        dimmed.can_dim = true;
        dimmed.remote_viewer_arrived = true;
        CHECK(helix::decide_idle(dimmed).action == IdleAction::None);
        IdleInputs awake = viewed(make(A, 1000, 60, 120));
        awake.can_dim = true;
        awake.remote_viewer_arrived = true;
        CHECK(helix::decide_idle(awake).action == IdleAction::None);
    }

    SECTION("viewed, wakes are unchanged") {
        IdleInputs dimmed = viewed(make(D, 10, 60, 120));
        CHECK(helix::decide_idle(dimmed).action == IdleAction::Wake);
        IdleInputs asleep = viewed(make(IdleState::Sleeping, 999999, 60, 120));
        CHECK(helix::decide_idle(asleep).action == IdleAction::None);
        asleep.wake_requested = true;
        CHECK(helix::decide_idle(asleep).action == IdleAction::Wake);
    }
}

TEST_CASE("wake_should_auto_lock truth table", "[display][idle_decision]") {
    struct LockRow {
        const char* label;
        bool sleeping, dimmed, preview, enabled, pin, want;
    };
    const LockRow rows[] = {
        {"wake from sleep locks", true, false, false, true, true, true},
        {"wake from dim locks", false, true, false, true, true, true},
        {"wake from sleep and dim locks", true, true, false, true, true, true},
        {"a preview dismiss never locks", false, true, true, true, true, false},
        {"a preview flag blocks even from sleep", true, true, true, true, true, false},
        {"auto-lock disabled", true, false, false, false, true, false},
        {"no PIN set", true, false, false, true, false, false},
        {"already awake", false, false, false, true, true, false},
    };
    for (const auto& r : rows) {
        INFO(r.label);
        CHECK(helix::wake_should_auto_lock(r.sleeping, r.dimmed, r.preview, r.enabled, r.pin) ==
              r.want);
    }
}

TEST_CASE("pointer_loss_ends_power_off truth table", "[display][idle_decision][2069]") {
    using helix::SleepMechanism;
    struct LossRow {
        const char* label;
        bool sleeping;
        SleepMechanism mechanism;
        bool want;
    };
    const LossRow rows[] = {
        {"lost while the panel is powered off", true, SleepMechanism::PanelPowerOff, true},
        {"lost under the overlay: the panel stays powered", true, SleepMechanism::SoftwareOverlay,
         false},
        {"lost under a hardware blank", true, SleepMechanism::HardwareBlank, false},
        {"lost under host sleep", true, SleepMechanism::HostSleep, false},
        {"lost while awake after a power-off sleep", false, SleepMechanism::PanelPowerOff, false},
    };
    for (const auto& r : rows) {
        INFO(r.label);
        CHECK(helix::pointer_loss_ends_power_off(r.sleeping, r.mechanism) == r.want);
    }
}
