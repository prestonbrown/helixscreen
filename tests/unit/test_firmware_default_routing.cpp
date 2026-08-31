// SPDX-License-Identifier: GPL-3.0-or-later
//
// The firmware DEFAULT tool->head map, declared per backend.
//
// This exists because a four-head constant used to decide the answer for every
// AMS. It was a transcription of the Snapmaker U1's real table (verified live:
// print_task_config.extruder_map_table reads [0,1,2,3,0,0,0,0]) applied to
// lane-per-tool systems where it is simply false - an 8-lane AFC maps lanes 0..7
// to T0..T7, and the constant collapsed every tool above 3 onto lane 0.

#include "ams_backend_snapmaker.h"
#include "firmware_routing.h"

#include "../catch_amalgamated.hpp"

using helix::FirmwareRouting;

TEST_CASE("FirmwareRouting: identity is the lane-per-tool shape",
          "[filament][routing][firmware_routing]") {
    auto routing = FirmwareRouting::identity();

    // No upper bound: an AMS can grow lanes without the rule changing.
    CHECK(routing.head(0) == 0);
    CHECK(routing.head(3) == 3);
    CHECK(routing.head(7) == 7);
    CHECK(routing.head(31) == 31);

    // A negative tool has no head; it must not index anything.
    CHECK(routing.head(-1) == -1);
}

TEST_CASE("FirmwareRouting: fixed_heads models a bounded-head machine",
          "[filament][routing][firmware_routing]") {
    auto routing = FirmwareRouting::fixed_heads(4, 0);

    CHECK(routing.head(0) == 0);
    CHECK(routing.head(3) == 3);

    // Past the last head the machine falls back - it has no lane 5 to route to.
    CHECK(routing.head(4) == 0);
    CHECK(routing.head(5) == 0);
    CHECK(routing.head(31) == 0);
}

TEST_CASE("FirmwareRouting: an explicit table wins, and -1 means unmapped",
          "[filament][routing][firmware_routing]") {
    FirmwareRouting routing;
    routing.head_for_tool = {2, 0, 3, -1};
    routing.fallback_head = -1;

    CHECK(routing.head(0) == 2);
    CHECK(routing.head(1) == 0);
    CHECK(routing.head(2) == 3);
    CHECK(routing.head(3) == -1); // declared unmapped
    CHECK(routing.head(9) == -1); // past the table
}

TEST_CASE("AmsBackendSnapmaker declares the four-head U1 table", "[filament][routing][snapmaker]") {
    // The U1 has four physical heads and accepts up to 32 logical tools. Its own
    // firmware table is [0,1,2,3,0,0,...], so T5 really does print from head 0 -
    // this is the one backend for which the old constant was right.
    AmsBackendSnapmaker backend(nullptr, nullptr);
    auto routing = backend.firmware_default_routing();

    CHECK(routing.head(0) == 0);
    CHECK(routing.head(1) == 1);
    CHECK(routing.head(2) == 2);
    CHECK(routing.head(3) == 3);
    CHECK(routing.head(4) == 0);
    CHECK(routing.head(5) == 0);
}
