// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_menu_context.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::plugin_menu;

namespace {
FileMetadata full_meta() {
    FileMetadata m;
    m.estimated_time = 3600;
    m.layer_height = 0.2;
    m.object_height = 48.5;
    return m;
}
} // namespace

TEST_CASE("print file context for a printer file with full metadata", "[plugin][menu][context]") {
    PrintFileFacts f;
    f.name = "benchy.gcode";
    f.dir = "boats";
    f.meta = full_meta();
    f.materials = {"PLA", "PETG"};
    f.colors = {"#ff0000", "00FF00"};
    f.tool_grams = {12.5, 0};
    f.tools_used = {1, 0};

    json j = print_file_context(f);
    CHECK(j["source"] == "printer");
    CHECK(j["path"] == "boats/benchy.gcode");
    CHECK(j["name"] == "benchy.gcode");
    CHECK_FALSE(j.contains("local_path"));
    CHECK(j["estimated_time_s"] == 3600.0);
    CHECK(j["layer_height"] == 0.2);
    CHECK(j["object_height"] == 48.5);
    REQUIRE(j["filament"].size() == 2);
    CHECK(j["filament"][0] ==
          json{{"tool", 0}, {"material", "PLA"}, {"color", "#FF0000"}, {"grams", 12.5}});
    CHECK(j["filament"][1] ==
          json{{"tool", 1}, {"material", "PETG"}, {"color", "#00FF00"}, {"grams", 0.0}});
    CHECK(j["tools_used"] == json::array({0, 1}));

    f.dir.clear();
    CHECK(print_file_context(f)["path"] == "benchy.gcode");
}

TEST_CASE("print file context for a USB file", "[plugin][menu][context]") {
    PrintFileFacts f;
    f.name = "part.gcode";
    f.local_path = "/media/usb0/part.gcode";
    json j = print_file_context(f);
    CHECK(j["source"] == "usb");
    CHECK_FALSE(j.contains("path"));
    CHECK(j["local_path"] == "/media/usb0/part.gcode");
    CHECK(j["name"] == "part.gcode");
}

TEST_CASE("print file context before metadata arrives", "[plugin][menu][context]") {
    PrintFileFacts f;
    f.name = "x.gcode";
    json j = print_file_context(f);
    for (const char* key : {"estimated_time_s", "layer_height", "object_height", "filament",
                            "tools_used", "local_path"})
        CHECK_FALSE(j.contains(key));

    // Metadata present but missing fields: still absent, never 0.
    f.meta = FileMetadata{};
    f.meta->layer_height = 0.28;
    j = print_file_context(f);
    CHECK_FALSE(j.contains("estimated_time_s"));
    CHECK_FALSE(j.contains("object_height"));
    CHECK(j["layer_height"] == 0.28);

    // Per-tool facts of different lengths: each unknown member is absent.
    f.materials = {"PLA", ""};
    f.colors = {"not a colour"};
    j = print_file_context(f);
    REQUIRE(j["filament"].size() == 2);
    CHECK(j["filament"][0] == json{{"tool", 0}, {"material", "PLA"}});
    CHECK(j["filament"][1] == json{{"tool", 1}});
}

TEST_CASE("slot context with nothing known", "[plugin][menu][context]") {
    SlotInfo s;
    s.slot_index = 2;
    s.status = SlotStatus::EMPTY;
    s.material = "PLA"; // what it last held
    s.color_rgb = 0x112233;
    s.spoolman_id = 9;
    json j = slot_context(s, 2, nullptr, "Lane 3");
    CHECK(j == json{{"slot", 2}, {"label", "Lane 3"}});

    s.status = SlotStatus::AVAILABLE;
    s.material.clear();
    s.color_rgb = kAmsDefaultSlotColor;
    s.spoolman_id = 0;
    CHECK(slot_context(s, 2, nullptr, "Lane 3") == json{{"slot", 2}, {"label", "Lane 3"}});
}

TEST_CASE("slot context with everything", "[plugin][menu][context]") {
    SlotInfo s;
    s.slot_index = 1;
    s.status = SlotStatus::LOADED;
    s.mapped_tool = 3;
    s.material = "PETG";
    s.brand = "Polymaker";
    s.color_rgb = 0x00a0ff;
    s.spoolman_id = 42;
    AmsUnit u;
    u.unit_index = 1;
    u.first_slot_global_index = 4;
    json j = slot_context(s, 5, &u, "Turtle 2 slot 2");
    CHECK(j == json{{"unit", 1},
                    {"slot", 1},
                    {"label", "Turtle 2 slot 2"},
                    {"tool", 3},
                    {"material", "PETG"},
                    {"brand", "Polymaker"},
                    {"color", "#00A0FF"},
                    {"spool_id", 42}});
}

TEST_CASE("slot context counts the slot within its own unit", "[plugin][menu][context]") {
    // Two four-slot units; the menu is on the second unit's first slot. The
    // backend's own slot_index is not trusted: some report it globally.
    AmsSystemInfo sys;
    for (int u = 0; u < 2; ++u) {
        AmsUnit unit;
        unit.unit_index = u;
        unit.slot_count = 4;
        unit.first_slot_global_index = u * 4;
        for (int i = 0; i < 4; ++i) {
            SlotInfo slot;
            slot.slot_index = u * 4 + i; // a backend that fills it globally
            slot.global_index = u * 4 + i;
            unit.slots.push_back(slot);
        }
        sys.units.push_back(unit);
    }
    const AmsUnit* unit = sys.unit_for_slot(4);
    REQUIRE(unit);
    json j = slot_context(*sys.get_slot_global(4), 4, unit, "Box 2 slot 1");
    CHECK(j["unit"] == 1);
    CHECK(j["slot"] == 0);
}

TEST_CASE("spool context", "[plugin][menu][context]") {
    SpoolInfo s;
    s.id = 17;
    s.material = "ABS";
    s.vendor = "Hatchbox";
    s.color_hex = "1a1a2e";
    s.remaining_weight_g = 0;
    json j = spool_context(s);
    CHECK(j ==
          json{{"spool_id", 17}, {"material", "ABS"}, {"brand", "Hatchbox"}, {"color", "#1A1A2E"}});
    s.remaining_weight_g = 812.5;
    CHECK(spool_context(s)["remaining_g"] == 812.5);
}

TEST_CASE("tile context strips the plugin prefix", "[plugin][menu][context]") {
    CHECK(tile_context("orca-cal__status") == json{{"widget", "status"}});
    CHECK(tile_context("orca-cal__a__b") == json{{"widget", "a__b"}});
    CHECK(tile_context("plain") == json{{"widget", "plain"}});
}
