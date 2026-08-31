// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_tool_topology.cpp
 * @brief Which backends hand ToolState an AMS tool->slot topology.
 *
 * build_ams_topology() decides whether ToolState adopts a backend's tool->slot
 * table or keeps enumerating extruders. It is NOT the remap gate, and the two
 * part company on shipped hardware: the Snapmaker U1 carries out every remap the
 * user picks, through its pre-print send, and owns no table — its four extruders
 * are independent, so the extruder enumeration is the correct model and a
 * four-lane topology would be a fiction.
 *
 * These are characterization tests. Every answer here is what the code already
 * gave when the gate read a capability struct's `supported` field; they exist so
 * that routing this gate through remap capability — which reads as the obvious
 * simplification, and which an earlier design note actually proposed — fails
 * loudly instead of silently changing what the U1 reports.
 */

#include "../test_helpers/ad5x_ifs_test_access.h"
#include "../test_helpers/ams_backend_probes.h"
#include "ams_backend_ace.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_qidi.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "ams_remap.h"
#include "ams_tool_topology.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("Table-owning backends produce a ToolTopology", "[ams][topology]") {
    SECTION("AFC") {
        AfcProbe afc;
        CHECK(helix::build_ams_topology(&afc, 0).has_value());
    }
    SECTION("Happy Hare") {
        HappyHareProbe hh;
        CHECK(helix::build_ams_topology(&hh, 0).has_value());
    }
    SECTION("CFS") {
        CfsProbe cfs;
        CHECK(helix::build_ams_topology(&cfs, 0).has_value());
    }
    SECTION("QIDI Box") {
        QidiProbe qidi;
        CHECK(helix::build_ams_topology(&qidi, 0).has_value());
    }
    SECTION("ToolChanger") {
        ToolChangerProbe tc;
        CHECK(helix::build_ams_topology(&tc, 0).has_value());
    }
}

TEST_CASE("Snapmaker produces no topology despite being able to remap",
          "[ams][topology][snapmaker]") {
    // Both halves asserted together on purpose. This is the pair that must not
    // collapse into one question: routing the gate through can_remap() would
    // flip the second line and hand ToolState a lane topology for a machine
    // whose tools ARE its extruders.
    SnapmakerProbe sm;
    REQUIRE(helix::printer::can_remap(sm));
    CHECK_FALSE(helix::build_ams_topology(&sm, 0).has_value());
}

TEST_CASE("ACE produces no topology", "[ams][topology]") {
    // Single tool, many slots — the backend does not own the tool list.
    AceProbe ace;
    CHECK_FALSE(helix::build_ams_topology(&ace, 0).has_value());
}

TEST_CASE("AD5X IFS produces no topology until _IFS_VARS is discovered", "[ams][topology][ad5x]") {
    // Without the macro get_tool_mapping() returns {}, so there is nothing to
    // build from — and the 1:1 slot-count fallback must not paper over that,
    // which it would if the gate asked any question but table ownership.
    Ad5xIfsProbe ad5x;
    CHECK_FALSE(helix::build_ams_topology(&ad5x, 0).has_value());

    Ad5xIfsTestAccess::set_has_ifs_vars(ad5x, true);
    CHECK(helix::build_ams_topology(&ad5x, 0).has_value());
}

TEST_CASE("No backend produces no topology", "[ams][topology]") {
    CHECK_FALSE(helix::build_ams_topology(nullptr, 0).has_value());
}
