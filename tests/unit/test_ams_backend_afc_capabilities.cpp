// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Pin the AFC backend's tool_mapping capability advertisement (#956).
//
// AmsState builds a ToolTopology from owns_tool_mapping_table() +
// get_tool_mapping() and forwards it to ToolState's tool switcher widget. If
// the real AmsBackendAfc silently stops advertising support, the switcher
// falls back to the printer's native tools and the AFC lane->T mapping never
// reaches the UI. This test defends against that regression at the unit
// boundary, complementing the AmsState-level end-to-end coverage in
// test_tool_state_ams_topology.cpp ("AFC mock with 4 lanes drives ToolState").

#include "../test_helpers/ams_backend_probes.h"
#include "ams_backend_afc.h"
#include "ams_remap.h"
#include "ams_types.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("AFC backend advertises tool_mapping support", "[ams][afc][tool-state][capabilities]") {
    AfcProbe afc;

    // build_ams_topology() reads this to decide whether to publish a
    // ToolTopology built from get_tool_mapping(). If it flips to false, the tool
    // switcher silently falls back to the printer's native tools.
    REQUIRE(afc.owns_tool_mapping_table());

    // And the remap side, which gates the UI affordance for changing the lane->T
    // binding (SET_MAP gcode). AFC's whole point is per-lane tool assignment, so
    // it declares the route that writes a table, and is ready without discovery.
    REQUIRE(helix::printer::can_remap(afc));
    REQUIRE(helix::printer::remap_is_persistent(afc.get_remap_strategy()));
    REQUIRE(afc.remap_ready());
}

TEST_CASE("AFC backend get_tool_mapping returns the SlotRegistry tool_to_slot vector",
          "[ams][afc][tool-state][capabilities]") {
    AfcProbe afc;

    // Without an active connection the registry is empty; we don't assert any
    // specific shape here. We only assert the call is safe (no UB, no throw)
    // and that the type matches AmsState's expectation (vector<int>).
    auto mapping = afc.get_tool_mapping();
    static_assert(std::is_same_v<decltype(mapping), std::vector<int>>,
                  "AmsState relies on std::vector<int> from get_tool_mapping()");
    SUCCEED("get_tool_mapping() returned a std::vector<int> (size=" +
            std::to_string(mapping.size()) + ")");
}
