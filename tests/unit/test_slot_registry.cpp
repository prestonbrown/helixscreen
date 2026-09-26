// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "slot_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

TEST_CASE("SlotRegistry single-unit initialization", "[slot_registry][init]") {
    SlotRegistry reg;

    REQUIRE_FALSE(reg.is_initialized());
    REQUIRE(reg.slot_count() == 0);

    std::vector<std::string> names = {"lane0", "lane1", "lane2", "lane3"};
    reg.initialize("Turtle_1", names);

    SECTION("basic state") {
        REQUIRE(reg.is_initialized());
        REQUIRE(reg.slot_count() == 4);
        REQUIRE(reg.unit_count() == 1);
    }

    SECTION("slot access by index") {
        for (int i = 0; i < 4; ++i) {
            const auto* entry = reg.get(i);
            REQUIRE(entry != nullptr);
            REQUIRE(entry->global_index == i);
            REQUIRE(entry->unit_index == 0);
            REQUIRE(entry->backend_name == names[i]);
        }
    }

    SECTION("slot access by name") {
        REQUIRE(reg.index_of("lane2") == 2);
        REQUIRE(reg.name_of(3) == "lane3");
        REQUIRE(reg.index_of("nonexistent") == -1);
        REQUIRE(reg.name_of(99) == "");
        REQUIRE(reg.name_of(-1) == "");
    }

    SECTION("find_by_name") {
        const auto* entry = reg.find_by_name("lane1");
        REQUIRE(entry != nullptr);
        REQUIRE(entry->global_index == 1);
        REQUIRE(reg.find_by_name("nope") == nullptr);
    }

    SECTION("unit info") {
        const auto& u = reg.unit(0);
        REQUIRE(u.name == "Turtle_1");
        REQUIRE(u.first_slot == 0);
        REQUIRE(u.slot_count == 4);

        auto [first, end] = reg.unit_slot_range(0);
        REQUIRE(first == 0);
        REQUIRE(end == 4);
    }

    SECTION("unit_for_slot") {
        for (int i = 0; i < 4; ++i) {
            REQUIRE(reg.unit_for_slot(i) == 0);
        }
        REQUIRE(reg.unit_for_slot(-1) == -1);
        REQUIRE(reg.unit_for_slot(4) == -1);
    }

    SECTION("is_valid_index") {
        REQUIRE(reg.is_valid_index(0));
        REQUIRE(reg.is_valid_index(3));
        REQUIRE_FALSE(reg.is_valid_index(-1));
        REQUIRE_FALSE(reg.is_valid_index(4));
    }

    SECTION("default slot info") {
        const auto* entry = reg.get(0);
        REQUIRE(entry->info.global_index == 0);
        REQUIRE(entry->info.slot_index == 0); // unit-local
        REQUIRE(entry->info.mapped_tool == -1);
        REQUIRE(entry->info.status == helix::SlotStatus::UNKNOWN);
    }
}

TEST_CASE("SlotRegistry multi-unit initialization", "[slot_registry][init]") {
    SlotRegistry reg;

    std::vector<std::pair<std::string, std::vector<std::string>>> units = {
        {"Turtle_1", {"lane0", "lane1", "lane2", "lane3"}},
        {"AMS_1", {"lane4", "lane5", "lane6", "lane7"}},
    };
    reg.initialize_units(units);

    REQUIRE(reg.slot_count() == 8);
    REQUIRE(reg.unit_count() == 2);

    SECTION("unit boundaries") {
        auto [f0, e0] = reg.unit_slot_range(0);
        REQUIRE(f0 == 0);
        REQUIRE(e0 == 4);
        auto [f1, e1] = reg.unit_slot_range(1);
        REQUIRE(f1 == 4);
        REQUIRE(e1 == 8);
    }

    SECTION("global index continuity") {
        for (int i = 0; i < 8; ++i) {
            REQUIRE(reg.get(i)->global_index == i);
        }
    }

    SECTION("unit-local indices") {
        REQUIRE(reg.get(0)->info.slot_index == 0);
        REQUIRE(reg.get(3)->info.slot_index == 3);
        REQUIRE(reg.get(4)->info.slot_index == 0); // first slot in unit 1
        REQUIRE(reg.get(7)->info.slot_index == 3);
    }

    SECTION("unit_for_slot across units") {
        REQUIRE(reg.unit_for_slot(0) == 0);
        REQUIRE(reg.unit_for_slot(3) == 0);
        REQUIRE(reg.unit_for_slot(4) == 1);
        REQUIRE(reg.unit_for_slot(7) == 1);
    }

    SECTION("name lookup across units") {
        REQUIRE(reg.index_of("lane4") == 4);
        REQUIRE(reg.name_of(7) == "lane7");
    }
}

TEST_CASE("SlotRegistry reorganize preserves given order", "[slot_registry][reorganize]") {
    SlotRegistry reg;

    // Initialize in non-alphabetical order
    std::vector<std::pair<std::string, std::vector<std::string>>> units = {
        {"Zebra", {"z0", "z1"}},
        {"Alpha", {"a0", "a1"}},
    };
    reg.initialize_units(units);

    // Verify initial order (as given)
    REQUIRE(reg.unit(0).name == "Zebra");
    REQUIRE(reg.get(0)->backend_name == "z0");

    // Reorganize — order is preserved as given (Zebra first, then Alpha)
    std::vector<std::pair<std::string, std::vector<std::string>>> unit_vec = {
        {"Zebra", {"z0", "z1"}},
        {"Alpha", {"a0", "a1"}},
    };
    reg.reorganize(unit_vec);

    SECTION("units in given order") {
        REQUIRE(reg.unit(0).name == "Zebra");
        REQUIRE(reg.unit(1).name == "Alpha");
    }

    SECTION("slots reindexed to match") {
        REQUIRE(reg.get(0)->backend_name == "z0");
        REQUIRE(reg.get(1)->backend_name == "z1");
        REQUIRE(reg.get(2)->backend_name == "a0");
        REQUIRE(reg.get(3)->backend_name == "a1");
    }

    SECTION("reverse maps rebuilt") {
        REQUIRE(reg.index_of("z0") == 0);
        REQUIRE(reg.index_of("a0") == 2);
        REQUIRE(reg.name_of(0) == "z0");
    }
}

TEST_CASE("SlotRegistry reorganize preserves slot data", "[slot_registry][reorganize]") {
    SlotRegistry reg;

    // Initialize, then set some slot state
    reg.initialize("Unit_A", {"s0", "s1", "s2"});
    reg.get_mut(1)->info.color_rgb = 0xFF0000;
    reg.get_mut(1)->info.material = "PLA";
    reg.get_mut(1)->info.status = helix::SlotStatus::AVAILABLE;
    reg.get_mut(1)->endless_spool_backup = 2;

    // Reorganize into 2 units — s1 moves from index 1 to a new position
    // Unit_B first (s1 at global 0), then Unit_Z (s0 at global 1, s2 at global 2)
    std::vector<std::pair<std::string, std::vector<std::string>>> unit_vec = {
        {"Unit_B", {"s1"}},
        {"Unit_Z", {"s0", "s2"}},
    };
    reg.reorganize(unit_vec);

    SECTION("s1 data preserved at new position") {
        const auto* entry = reg.find_by_name("s1");
        REQUIRE(entry != nullptr);
        REQUIRE(entry->global_index == 0); // moved from 1 to 0
        REQUIRE(entry->info.color_rgb == 0xFF0000);
        REQUIRE(entry->info.material == "PLA");
        REQUIRE(entry->info.status == helix::SlotStatus::AVAILABLE);
        REQUIRE(entry->endless_spool_backup == 2);
    }

    SECTION("indices and unit membership updated") {
        const auto* s1 = reg.find_by_name("s1");
        REQUIRE(s1->unit_index == 0);
        REQUIRE(s1->info.slot_index == 0); // unit-local

        const auto* s0 = reg.find_by_name("s0");
        REQUIRE(s0->global_index == 1);
        REQUIRE(s0->unit_index == 1);
        REQUIRE(s0->info.slot_index == 0); // first in Unit_Z
    }
}

TEST_CASE("SlotRegistry reorganize with new and removed slots", "[slot_registry][reorganize]") {
    SlotRegistry reg;

    reg.initialize("Unit", {"s0", "s1", "s2"});
    reg.get_mut(0)->info.color_rgb = 0xAAAAAA;

    // Reorganize: s1 removed, s3 added
    std::vector<std::pair<std::string, std::vector<std::string>>> unit_vec = {
        {"Unit", {"s0", "s2", "s3"}},
    };
    reg.reorganize(unit_vec);

    SECTION("s0 preserved") {
        REQUIRE(reg.find_by_name("s0")->info.color_rgb == 0xAAAAAA);
    }

    SECTION("s1 removed") {
        REQUIRE(reg.find_by_name("s1") == nullptr);
        REQUIRE(reg.index_of("s1") == -1);
    }

    SECTION("s3 added with defaults") {
        const auto* s3 = reg.find_by_name("s3");
        REQUIRE(s3 != nullptr);
        REQUIRE(s3->info.status == helix::SlotStatus::UNKNOWN);
    }

    SECTION("slot count updated") {
        REQUIRE(reg.slot_count() == 3);
    }
}

TEST_CASE("SlotRegistry idempotent reorganize", "[slot_registry][reorganize]") {
    SlotRegistry reg;

    std::vector<std::pair<std::string, std::vector<std::string>>> unit_vec = {
        {"Alpha", {"a0", "a1"}},
        {"Beta", {"b0", "b1"}},
    };

    reg.initialize("temp", {"a0", "a1", "b0", "b1"});
    reg.get_mut(0)->info.color_rgb = 0x112233;
    reg.reorganize(unit_vec);

    // Capture state
    auto color_before = reg.get(0)->info.color_rgb;
    auto name_before = reg.get(0)->backend_name;

    // Reorganize again with same layout
    reg.reorganize(unit_vec);

    REQUIRE(reg.get(0)->info.color_rgb == color_before);
    REQUIRE(reg.get(0)->backend_name == name_before);
}

TEST_CASE("SlotRegistry tool mapping", "[slot_registry][tool_mapping]") {
    SlotRegistry reg;
    reg.initialize("Unit", {"s0", "s1", "s2", "s3"});

    SECTION("no mapping by default") {
        REQUIRE(reg.tool_for_slot(0) == -1);
        REQUIRE(reg.slot_for_tool(0) == -1);
    }

    SECTION("set and get single mapping") {
        reg.set_tool_mapping(2, 5);
        REQUIRE(reg.tool_for_slot(2) == 5);
        REQUIRE(reg.slot_for_tool(5) == 2);
        REQUIRE(reg.get(2)->info.mapped_tool == 5);
    }

    SECTION("remapping a tool clears previous") {
        reg.set_tool_mapping(0, 3);
        reg.set_tool_mapping(1, 3); // T3 moves from slot 0 to slot 1
        REQUIRE(reg.slot_for_tool(3) == 1);
        REQUIRE(reg.tool_for_slot(0) == -1); // cleared
        REQUIRE(reg.tool_for_slot(1) == 3);
    }

    SECTION("bulk set_tool_map") {
        // TTG-style: tool_to_slot[0]=2, tool_to_slot[1]=0, tool_to_slot[2]=3, tool_to_slot[3]=1
        reg.set_tool_map({2, 0, 3, 1});
        REQUIRE(reg.slot_for_tool(0) == 2);
        REQUIRE(reg.slot_for_tool(1) == 0);
        REQUIRE(reg.slot_for_tool(2) == 3);
        REQUIRE(reg.slot_for_tool(3) == 1);
        REQUIRE(reg.tool_for_slot(2) == 0);
        REQUIRE(reg.tool_for_slot(0) == 1);
    }

    SECTION("tool mapping survives reorganize") {
        reg.set_tool_mapping(1, 7);

        // Pass A first so s0,s1 are at global 0-1, then B with s2,s3 at 2-3
        std::vector<std::pair<std::string, std::vector<std::string>>> unit_vec = {
            {"A", {"s0", "s1"}},
            {"B", {"s2", "s3"}},
        };
        reg.reorganize(unit_vec);

        // s1 moved — verify via name lookup
        const auto* s1 = reg.find_by_name("s1");
        REQUIRE(s1->info.mapped_tool == 7);
        REQUIRE(reg.slot_for_tool(7) == s1->global_index);
    }

    SECTION("invalid indices") {
        REQUIRE(reg.tool_for_slot(-1) == -1);
        REQUIRE(reg.tool_for_slot(99) == -1);
        REQUIRE(reg.slot_for_tool(-1) == -1);
        REQUIRE(reg.slot_for_tool(99) == -1);
    }
}

TEST_CASE("SlotRegistry build_system_info keeps the base's own fields",
          "[slot_registry][snapshot]") {
    SlotRegistry reg;
    reg.initialize_units({{"Unit_A", {"s0", "s1"}}, {"Unit_B", {"s2"}}});
    reg.get_mut(0)->info.material = "PLA";
    reg.set_tool_mapping(2, 0);

    helix::AmsSystemInfo base;
    base.version = "1.2.3";
    base.filament_runout = true;
    base.action = helix::AmsAction::ERROR;
    base.total_slots = 99;
    base.tool_to_slot_map = {7, 7, 7};
    helix::AmsUnit stale;
    stale.name = "Pretty A";
    stale.display_name = "Box A";
    stale.connected = true;
    stale.slot_count = 9;
    stale.first_slot_global_index = 40;
    stale.slots.resize(9);
    base.units.push_back(stale);

    auto info = reg.build_system_info(base);

    // Everything the registry does not own passes through.
    CHECK(info.version == "1.2.3");
    CHECK(info.filament_runout);
    CHECK(info.action == helix::AmsAction::ERROR);
    REQUIRE(info.units.size() == 2);
    CHECK(info.units[0].name == "Pretty A");
    CHECK(info.units[0].display_name == "Box A");
    CHECK(info.units[0].connected);

    // Slot-owned fields come from the registry.
    CHECK(info.total_slots == 3);
    CHECK(info.tool_to_slot_map == std::vector<int>{2});
    CHECK(info.units[0].unit_index == 0);
    CHECK(info.units[0].slot_count == 2);
    CHECK(info.units[0].first_slot_global_index == 0);
    REQUIRE(info.units[0].slots.size() == 2);
    CHECK(info.units[0].slots[0].material == "PLA");

    // A unit the base lacks is built from the registry alone.
    CHECK(info.units[1].name == "Unit_B");
    CHECK(info.units[1].unit_index == 1);
    CHECK(info.units[1].first_slot_global_index == 2);
    CHECK(info.units[1].slots.size() == 1);
}

TEST_CASE("SlotRegistry build_system_info", "[slot_registry][snapshot]") {
    SlotRegistry reg;

    std::vector<std::pair<std::string, std::vector<std::string>>> units = {
        {"Unit_A", {"s0", "s1"}},
        {"Unit_B", {"s2", "s3", "s4"}},
    };
    reg.initialize_units(units);

    // Set some state
    reg.get_mut(0)->info.color_rgb = 0xFF0000;
    reg.get_mut(0)->info.material = "PLA";
    reg.get_mut(0)->info.status = helix::SlotStatus::AVAILABLE;
    reg.set_tool_mapping(0, 0);
    reg.set_tool_mapping(2, 1);

    auto info = reg.build_system_info();

    SECTION("total slots") {
        REQUIRE(info.total_slots == 5);
    }

    SECTION("unit structure") {
        REQUIRE(info.units.size() == 2);
        REQUIRE(info.units[0].name == "Unit_A");
        REQUIRE(info.units[0].slot_count == 2);
        REQUIRE(info.units[0].first_slot_global_index == 0);
        REQUIRE(info.units[1].name == "Unit_B");
        REQUIRE(info.units[1].slot_count == 3);
        REQUIRE(info.units[1].first_slot_global_index == 2);
    }

    SECTION("slot data in units") {
        REQUIRE(info.units[0].slots[0].color_rgb == 0xFF0000);
        REQUIRE(info.units[0].slots[0].material == "PLA");
        REQUIRE(info.units[0].slots[0].status == helix::SlotStatus::AVAILABLE);
        REQUIRE(info.units[0].slots[0].global_index == 0);
        REQUIRE(info.units[0].slots[0].slot_index == 0);
    }

    SECTION("tool_to_slot_map") {
        REQUIRE(info.tool_to_slot_map.size() >= 2);
        REQUIRE(info.tool_to_slot_map[0] == 0);
        REQUIRE(info.tool_to_slot_map[1] == 2);
    }
}

TEST_CASE("SlotRegistry endless spool", "[slot_registry][endless_spool]") {
    SlotRegistry reg;
    reg.initialize("Unit", {"s0", "s1", "s2"});

    REQUIRE(reg.backup_for_slot(0) == -1); // default

    reg.set_backup(0, 2);
    REQUIRE(reg.backup_for_slot(0) == 2);

    reg.set_backup(0, -1); // clear
    REQUIRE(reg.backup_for_slot(0) == -1);

    SECTION("invalid index") {
        REQUIRE(reg.backup_for_slot(-1) == -1);
        REQUIRE(reg.backup_for_slot(99) == -1);
    }
}

TEST_CASE("SlotRegistry mixed topology slot index correctness", "[slot_registry][regression]") {
    // Reproduces the production bug: 6-toolhead mixed system
    // Box Turtle (4 lanes PARALLEL) + 2 OpenAMS (4 lanes HUB each)
    // AFC discovery order may differ from alphabetical sort order

    SlotRegistry reg;

    // Simulate AFC discovery order (may NOT be alphabetical)
    std::vector<std::pair<std::string, std::vector<std::string>>> discovery_order = {
        {"OpenAMS AMS_1", {"lane4", "lane5", "lane6", "lane7"}},
        {"OpenAMS AMS_2", {"lane8", "lane9", "lane10", "lane11"}},
        {"Box_Turtle Turtle_1", {"lane0", "lane1", "lane2", "lane3"}},
    };
    reg.initialize_units(discovery_order);

    // Now reorganize — pass in desired order (Box_Turtle first, then OpenAMS units)
    std::vector<std::pair<std::string, std::vector<std::string>>> unit_vec = {
        {"Box_Turtle Turtle_1", {"lane0", "lane1", "lane2", "lane3"}},
        {"OpenAMS AMS_1", {"lane4", "lane5", "lane6", "lane7"}},
        {"OpenAMS AMS_2", {"lane8", "lane9", "lane10", "lane11"}},
    };
    reg.reorganize(unit_vec);

    SECTION("Box Turtle is first (given order)") {
        REQUIRE(reg.unit(0).name == "Box_Turtle Turtle_1");
        REQUIRE(reg.unit(0).first_slot == 0);
    }

    SECTION("AMS_1 starts at global index 4") {
        REQUIRE(reg.unit(1).name == "OpenAMS AMS_1");
        REQUIRE(reg.unit(1).first_slot == 4);
        REQUIRE(reg.name_of(4) == "lane4");
    }

    SECTION("AMS_2 starts at global index 8") {
        REQUIRE(reg.unit(2).name == "OpenAMS AMS_2");
        REQUIRE(reg.unit(2).first_slot == 8);
        REQUIRE(reg.name_of(11) == "lane11");
    }

    SECTION("every slot resolves to correct lane name") {
        for (int i = 0; i < 12; ++i) {
            std::string expected = "lane" + std::to_string(i);
            REQUIRE(reg.name_of(i) == expected);
        }
    }

    SECTION("reverse lookup also correct") {
        for (int i = 0; i < 12; ++i) {
            std::string name = "lane" + std::to_string(i);
            REQUIRE(reg.index_of(name) == i);
        }
    }
}

TEST_CASE("SlotRegistry reorganize respects given unit order", "[slot_registry][reorganize]") {
    SlotRegistry reg;

    // 4 units, passed in lane-number order (as the AFC backend now sorts them)
    std::vector<std::pair<std::string, std::vector<std::string>>> units = {
        {"Box_Turtle Turtle_1", {"lane0", "lane1", "lane2", "lane3"}},
        {"OpenAMS AMS_1", {"lane4", "lane5", "lane6", "lane7"}},
        {"ACE ace_1", {"lane8", "lane9", "lane10", "lane11"}},
        {"Box_Turtle Turtle_2", {"lane12", "lane13", "lane14", "lane15"}},
    };
    // Flat initialization with all 16 lanes before reorganizing into units
    std::vector<std::string> all_lanes;
    for (int i = 0; i < 16; ++i)
        all_lanes.push_back("lane" + std::to_string(i));
    reg.initialize("temp", all_lanes);
    reg.reorganize(units);

    SECTION("units in given order, not alphabetical") {
        // With std::map, ACE would sort first. With vector, order is preserved.
        REQUIRE(reg.unit(0).name == "Box_Turtle Turtle_1");
        REQUIRE(reg.unit(1).name == "OpenAMS AMS_1");
        REQUIRE(reg.unit(2).name == "ACE ace_1");
        REQUIRE(reg.unit(3).name == "Box_Turtle Turtle_2");
    }

    SECTION("lane indices match physical order") {
        for (int i = 0; i < 16; ++i) {
            REQUIRE(reg.name_of(i) == "lane" + std::to_string(i));
        }
    }
}
