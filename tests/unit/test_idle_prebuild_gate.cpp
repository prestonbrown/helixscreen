// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// IdlePrebuildGate decides when a deferred panel may be built ahead of its first
// visit: only after the printer has stayed connected for a few ticks, while
// nobody is touching the screen and nothing is open over the panels.
// IdlePrebuilder runs it from a timer, builds once and stops.

#include "../lvgl_test_fixture.h"
#include "panel_factory.h"

#include "../catch_amalgamated.hpp"

using helix::IdlePrebuilder;
using helix::IdlePrebuildGate;
using helix::IdlePrebuildInputs;

namespace {
constexpr uint32_t kQuiet = IdlePrebuildGate::kQuietMs;

IdlePrebuildInputs idle() {
    IdlePrebuildInputs in;
    in.connected = true;
    in.inactive_ms = kQuiet;
    return in;
}

IdlePrebuildInputs with_connected(bool c) {
    auto in = idle();
    in.connected = c;
    return in;
}

/// Ticks the gate through the settle ticks that cannot open it yet.
void settle(IdlePrebuildGate& gate) {
    for (int i = 1; i < IdlePrebuildGate::kSettleTicks; ++i) {
        CAPTURE(i);
        REQUIRE_FALSE(gate.tick(idle()));
    }
}
} // namespace

TEST_CASE("IdlePrebuildGate opens after the connection settles and input is quiet",
          "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    settle(gate);
    REQUIRE(gate.tick(idle()));
}

TEST_CASE("IdlePrebuildGate stays shut while disconnected", "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    auto in = with_connected(false);
    in.inactive_ms = kQuiet * 10;
    for (int i = 0; i < 10; ++i) {
        REQUIRE_FALSE(gate.tick(in));
    }
}

TEST_CASE("IdlePrebuildGate restarts the settle count after a disconnect",
          "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    settle(gate);
    REQUIRE_FALSE(gate.tick(with_connected(false)));
    settle(gate);
    REQUIRE(gate.tick(idle()));
}

TEST_CASE("IdlePrebuildGate waits out recent input", "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    auto in = idle();
    in.inactive_ms = kQuiet - 1;
    for (int i = 0; i < IdlePrebuildGate::kSettleTicks + 5; ++i) {
        REQUIRE_FALSE(gate.tick(in));
    }
    REQUIRE(gate.tick(idle()));
}

TEST_CASE("IdlePrebuildGate stays shut while the wizard, a modal or an overlay is up",
          "[panel_factory][idle_prebuild]") {
    IdlePrebuildGate gate;
    auto busy = idle();
    busy.busy = true;
    for (int i = 0; i < IdlePrebuildGate::kSettleTicks + 5; ++i) {
        REQUIRE_FALSE(gate.tick(busy));
    }
    // Being busy does not restart the connection's settle count.
    REQUIRE(gate.tick(idle()));
}

namespace {
class IdlePrebuilderFixture : public LVGLTestFixture {
  public:
    IdlePrebuilder::Hooks hooks() {
        return {[this] { return built; }, [this] { return inputs; },
                [this] {
                    ++builds;
                    built = true;
                }};
    }

    bool built = false;
    int builds = 0;
    IdlePrebuildInputs inputs = idle();
    IdlePrebuilder prebuilder;
};
} // namespace

TEST_CASE_METHOD(IdlePrebuilderFixture,
                 "IdlePrebuilder builds on a connected idle tick, then stops",
                 "[panel_factory][idle_prebuild]") {
    prebuilder.start(hooks());
    REQUIRE(prebuilder.running());

    for (int i = 0; i < IdlePrebuildGate::kSettleTicks; ++i) {
        prebuilder.tick();
    }
    REQUIRE(builds == 1);
    REQUIRE_FALSE(prebuilder.running());

    // A stopped prebuilder never builds again.
    prebuilder.tick();
    REQUIRE(builds == 1);
}

TEST_CASE_METHOD(IdlePrebuilderFixture,
                 "IdlePrebuilder stops without building once a visit built the panel",
                 "[panel_factory][idle_prebuild]") {
    prebuilder.start(hooks());
    built = true;

    prebuilder.tick();
    REQUIRE(builds == 0);
    REQUIRE_FALSE(prebuilder.running());
}

TEST_CASE_METHOD(IdlePrebuilderFixture, "IdlePrebuilder keeps waiting while the gate is shut",
                 "[panel_factory][idle_prebuild]") {
    inputs.busy = true;
    prebuilder.start(hooks());

    for (int i = 0; i < IdlePrebuildGate::kSettleTicks + 3; ++i) {
        prebuilder.tick();
    }
    REQUIRE(builds == 0);
    REQUIRE(prebuilder.running());
}
