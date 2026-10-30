// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/cfs_box_fixtures.h"
#include "cfs_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;
using helix::printer::CfsMaterialDb;
using helix::test::make_cfs_status_json;
using helix::test::make_unit_box_explicit;

TEST_CASE("CFS status parse classifies a box frame by the keys it carries", "[cfs][status_parse]") {
    const auto bare = cfs::classify_box_frame(json{{"measuring_wheel", 3}});
    CHECK_FALSE(bare.has_top_level);
    CHECK_FALSE(bare.has_unit_data);

    const auto top = cfs::classify_box_frame(json{{"map", json::object()}});
    CHECK(top.has_top_level);
    CHECK_FALSE(top.has_unit_data);

    // The box resends the auto-refill bit alone when it changes.
    const auto refill = cfs::classify_box_frame(json{{"auto_refill", 0}});
    CHECK(refill.has_top_level);
    CHECK_FALSE(refill.has_unit_data);

    // The box's own bus state changes alone when a scan settles.
    const auto bus = cfs::classify_box_frame(json{{"state", "disconnect"}});
    CHECK(bus.has_top_level);
    CHECK_FALSE(bus.has_unit_data);

    const auto unit = cfs::classify_box_frame(json{{"T3", json::object()}});
    CHECK_FALSE(unit.has_top_level);
    CHECK(unit.has_unit_data);
}

TEST_CASE("CFS status parse reads the sibling objects only when present", "[cfs][status_parse]") {
    const json none = json::object();
    CHECK_FALSE(cfs::parse_filament_sensor(none));
    CHECK_FALSE(cfs::parse_extruder(none));
    CHECK_FALSE(cfs::parse_motor_control(none));

    const auto sensor = cfs::parse_filament_sensor(
        json{{"filament_switch_sensor filament_sensor", json{{"filament_detected", true}}}});
    REQUIRE(sensor);
    CHECK(sensor->filament_detected == true);

    // The object is there but the sensor has taken no reading: present, empty.
    const auto null_sensor = cfs::parse_filament_sensor(
        json{{"filament_switch_sensor filament_sensor", json{{"filament_detected", nullptr}}}});
    REQUIRE(null_sensor);
    CHECK_FALSE(null_sensor->filament_detected);

    const auto extruder = cfs::parse_extruder(json{{"extruder", json{{"target", 220}}}});
    REQUIRE(extruder);
    CHECK(extruder->target_c == Catch::Approx(220.0));
    CHECK_FALSE(extruder->temperature_c);

    const auto motor =
        cfs::parse_motor_control(json{{"motor_control", json{{"motor_ready", false}}}});
    REQUIRE(motor);
    CHECK(motor->motor_ready == false);
    CHECK_FALSE(cfs::parse_motor_control(json{{"motor_control", json{{"motor_ready", "yes"}}}})
                    ->motor_ready);
}

TEST_CASE("CFS status parse reads the registered units from printer.cfg's address table",
          "[cfs][status_parse][1933]") {
    // configfile.settings from a K2 Plus with one CFS at address 1.
    const json k2_plus = json::parse(R"({"auto_addr": {"mb_addr_table_uniids":
        [[19, 103, 59, 72, 36, 34, 176, 3, 54, 53, 52, 52], [0], [0], [0]]}})");
    CHECK(cfs::has_registered_unit(k2_plus));

    SECTION("the table as its config string") {
        const json as_string =
            json{{"auto_addr", {{"mb_addr_table_uniids", "\n[[19, 103, 59, 72], [0], [0], [0]]"}}}};
        CHECK(cfs::has_registered_unit(as_string));
    }
    SECTION("every address empty") {
        CHECK_FALSE(cfs::has_registered_unit(
            json::parse(R"({"auto_addr": {"mb_addr_table_uniids": [[0], [0], [0], [0]]}})")));
    }
    SECTION("no table, no section, or one that does not parse") {
        CHECK_FALSE(cfs::has_registered_unit(json::parse(R"({"auto_addr": {}})")));
        CHECK_FALSE(cfs::has_registered_unit(json::parse(R"({"printer": {}})")));
        CHECK_FALSE(
            cfs::has_registered_unit(json{{"auto_addr", {{"mb_addr_table_uniids", "[[19, 103"}}}}));
        CHECK_FALSE(cfs::has_registered_unit(json::array()));
    }
}

TEST_CASE("CFS stock box parse reads units, bays and environment", "[cfs][status_parse]") {
    auto status = make_cfs_status_json();
    auto info = cfs::parse_stock_box(status["box"]);

    SECTION("system-level fields") {
        REQUIRE(info.type == AmsType::CFS);
        // box.auto_refill=1 -> the ENABLE bit, which caps.enabled derives from.
        REQUIRE(info.endless_spool_enabled == true);
    }

    SECTION("connected unit created, disconnected skipped") {
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].name == "T1");
        REQUIRE(info.units[0].slot_count == 4);
        REQUIRE(info.total_slots == 4);
    }

    SECTION("unit environment data") {
        REQUIRE(info.units[0].environment.has_value());
        REQUIRE(info.units[0].environment->temperature_c == 27.0f);
        REQUIRE(info.units[0].environment->humidity_pct == 48.0f);
    }

    SECTION("unit hardware info") {
        REQUIRE(info.units[0].firmware_version == "1.1.3");
        REQUIRE(info.units[0].serial_number == "10000882925L125DBZC");
    }

    SECTION("slot colors parsed") {
        REQUIRE(info.units[0].slots[0].color_rgb == 0x000000);
        REQUIRE(info.units[0].slots[1].color_rgb == 0xFFFFFF);
        REQUIRE(info.units[0].slots[2].color_rgb == 0x0A2989);
        REQUIRE(info.units[0].slots[3].color_rgb == 0xC12E1F);
    }

    SECTION("slot materials resolved from database") {
        // material_type[0] == "101001" -> strip_code -> "01001", which
        // resolves in assets/filaments.json to Creality "Hyper PLA"
        // (type=PLA, nozzle_min=190, nozzle_max=240).
        REQUIRE(info.units[0].slots[0].material == "PLA");
        REQUIRE(info.units[0].slots[0].brand == "Creality");
        REQUIRE(info.units[0].slots[0].nozzle_temp_min > 0);
        REQUIRE(info.units[0].slots[0].nozzle_temp_max >= info.units[0].slots[0].nozzle_temp_min);
        REQUIRE(info.units[0].slots[0].nozzle_temp_min == 190);
        REQUIRE(info.units[0].slots[0].nozzle_temp_max == 240);
    }

    SECTION("slot remaining length") {
        REQUIRE(info.units[0].slots[0].remaining_length_m == 35.0f);
        REQUIRE(info.units[0].slots[1].remaining_length_m == 57.0f);
    }

    SECTION("slot status derived correctly") {
        REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
    }

    SECTION("topology is HUB") {
        REQUIRE(info.units[0].topology == PathTopology::HUB);
    }
}

// Presence regression (prestonbrown/helixscreen#1077). Covers the three cases
// CFS presence has to get right, all in one fixture:
//   A = tagged spool present  (vender set, RFID vendor unresolved → "unknown")
//   B = UNTAGGED spool present (vender sentinel, but a real remain_len)
//   C = genuinely empty bay    (vender sentinel, no length, but color/material
//                               still LATCHED from the last spool)
//   D = empty bay, zero length (remain_len "0" must not count as present)
// color_value/material_type latch after removal, so they must NOT drive
// presence (that faked the ghost slots); vender OR a positive remain_len does.
TEST_CASE("CFS stock box presence combines vender and remain_len", "[cfs][status_parse][1077]") {
    json box = json::parse(R"({
        "state": "connect", "filament": 1, "auto_refill": 1, "enable": 1, "filament_useup": 0,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {
            "state": "connect", "filament": "None", "temperature": "27", "dry_and_humidity": "40",
            "version": "1.1.3", "sn": "SERIAL", "mode": "0",
            "vender": ["unknown", "none", "none", "none"],
            "remain_len": ["-1", "42", "-1", "0"],
            "color_value": ["0FFFFFF", "0FF0000", "0C12E1F", "00A2989"],
            "material_type": ["unknown", "-1", "101001", "101001"],
            "change_color_num": ["-1", "-1", "-1", "-1"]
        }
    })");
    auto info = cfs::parse_stock_box(box);
    REQUIRE(info.units.size() == 1);
    const auto& slots = info.units[0].slots;

    SECTION("tagged spool present (A)") {
        REQUIRE(slots[0].status == SlotStatus::AVAILABLE);
    }

    SECTION("untagged spool with remaining length is present, not empty (B)") {
        // The key case: vender is a sentinel ("none") but remain_len is real, so
        // an untagged 3rd-party spool must NOT parse EMPTY.
        REQUIRE(slots[1].status == SlotStatus::AVAILABLE);
        REQUIRE(slots[1].remaining_length_m == 42.0f);
    }

    SECTION("genuinely empty bay is EMPTY despite latched color/material (C, D)") {
        REQUIRE(slots[2].status == SlotStatus::EMPTY); // vender none, remain -1
        REQUIRE(slots[3].status == SlotStatus::EMPTY); // vender none, remain "0"
    }

    SECTION("empty bay scrubs latched color/material so no ghost renders (C)") {
        // Slot 2 carries a fully-latched color (0xC12E1F) and a resolvable
        // Creality material code (101001) — both cleared once the bay reads
        // empty (scrubbed color resolves to the 0x808080 sentinel).
        REQUIRE(slots[2].color_rgb == CfsMaterialDb::parse_color("-1"));
        REQUIRE(slots[2].material.empty());
        REQUIRE(slots[2].brand.empty());
    }

    SECTION("present bay with unresolved RFID vendor defaults brand to Creality (A)") {
        // Slot 0: vender "unknown" (tag present, vendor unresolved) + unmapped
        // material code. CFS RFID tags are Creality-only → brand resolves to
        // Creality rather than staying blank.
        REQUIRE(slots[0].brand == "Creality");
    }
}

// A tagged bay's `vender` is the tag's whole 40-character record. With a
// material code the catalog does not know, the record's supplier field is
// what names the brand, and a record is never a brand itself.
TEST_CASE("CFS brand fallback reads the tag record's supplier, never the record",
          "[cfs][status_parse]") {
    const json box = make_unit_box_explicit(
        {"199999", "199999", "199999", "-1"}, {"0FFFFFF", "0FFFFFF", "0FFFFFF", "-1"},
        {"BB5250276A21999990FFFFFF0165000001000000", "BB5251234A21999990FFFFFF0165000001000000",
         "Sunlu", "none"},
        {"-1", "-1", "-1", "-1"});
    auto info = cfs::parse_stock_box(box);
    REQUIRE(info.units.size() == 1);
    const auto& slots = info.units[0].slots;

    SECTION("Creality supplier code reads as Creality") {
        REQUIRE(slots[0].status == SlotStatus::AVAILABLE);
        REQUIRE(slots[0].brand == "Creality");
    }

    SECTION("another supplier's record names no brand") {
        REQUIRE(slots[1].status == SlotStatus::AVAILABLE);
        REQUIRE(slots[1].brand.empty());
    }

    SECTION("a vendor name the box reports is the brand") {
        REQUIRE(slots[2].brand == "Sunlu");
    }
}

TEST_CASE("CFS tag record names a Spoolman spool through its serial",
          "[cfs][status_parse][tag_link]") {
    // The two known stock reads, a white and an orange Hyper PLA from two
    // printers (CREALITY_CFS_K2_INTERNALS.md § "RFID tags").
    SECTION("stock Creality records carry the factory serial and name no spool") {
        CHECK_FALSE(cfs::tag_record_spoolman_id("BB5250276A21010010FFFFFF0165000001000000"));
        CHECK_FALSE(cfs::tag_record_spoolman_id("3C5250276A21010010C12E1F0165000001000000"));
    }
    SECTION("a CFTag serial is a zero-padded decimal spool id") {
        CHECK(cfs::tag_record_spoolman_id("AB1240276A21010010FFFFFF0165000042000000") == 42);
        CHECK(cfs::tag_record_spoolman_id("AB1240276A21010010FFFFFF0165999999000000") == 999999);
        CHECK(cfs::tag_record_spoolman_id("AB1240276A21010010FFFFFF0165000002000000") == 2);
    }
    SECTION("an all-zero serial names no spool") {
        CHECK_FALSE(cfs::tag_record_spoolman_id("AB1240276A21010010FFFFFF0165000000000000"));
    }
    SECTION("a serial that is not all digits names no spool") {
        CHECK_FALSE(cfs::tag_record_spoolman_id("AB1240276A21010010FFFFFF016500004A000000"));
        CHECK_FALSE(cfs::tag_record_spoolman_id("AB1240276A21010010FFFFFF0165 00042000000"));
    }
    SECTION("anything but a 40-character record names no spool") {
        CHECK_FALSE(cfs::tag_record_spoolman_id("AB1240276A21010010FFFFFF016500004200000"));
        CHECK_FALSE(cfs::tag_record_spoolman_id("AB1240276A21010010FFFFFF01650000420000000"));
        CHECK_FALSE(cfs::tag_record_spoolman_id("unknown"));
        CHECK_FALSE(cfs::tag_record_spoolman_id("none"));
        CHECK_FALSE(cfs::tag_record_spoolman_id("-1"));
        CHECK_FALSE(cfs::tag_record_spoolman_id(""));
    }
}

TEST_CASE("CFS stock box parse takes the seated lane from the unit, not box.filament",
          "[cfs][status_parse]") {
    auto status = make_cfs_status_json();

    SECTION("box.filament is NOT treated as a loaded flag; tool map still parsed") {
        auto info = cfs::parse_stock_box(status["box"]);
        // box.filament = 1 is a stale active-lane SELECTION index, not a
        // "filament loaded" truth. the parse must NOT infer loaded-ness
        // from it — the toolhead sensor is the sole authority.
        REQUIRE(info.filament_loaded == false);
        REQUIRE(info.tool_to_slot_map.size() >= 1);
        REQUIRE(info.tool_to_slot_map[0] == 0); // T1A = slot 0
    }

    SECTION("box.filament = 0 also yields filament_loaded == false") {
        status["box"]["filament"] = 0;
        auto info = cfs::parse_stock_box(status["box"]);
        REQUIRE(info.filament_loaded == false);
    }

    SECTION("tool map preserved across multiple slots") {
        auto info = cfs::parse_stock_box(status["box"]);
        // Map: T1A→T1A(0), T1B→T1B(1), T1C→T1C(2), T1D→T1D(3)
        REQUIRE(info.tool_to_slot_map.size() == 4);
        REQUIRE(info.tool_to_slot_map[0] == 0);
        REQUIRE(info.tool_to_slot_map[1] == 1);
        REQUIRE(info.tool_to_slot_map[2] == 2);
        REQUIRE(info.tool_to_slot_map[3] == 3);
    }

    SECTION("active slot B → current_slot=1 and current_tool mirrors current_slot") {
        // CFS slots map 1:1 to tools; the print-status color dot reads
        // current_tool to label "T<n>". A hardcoded current_tool=0 would
        // mislabel the loaded slot.
        status["box"]["T1"]["filament"] = "B";
        auto info = cfs::parse_stock_box(status["box"]);
        REQUIRE(info.current_slot == 1);
        REQUIRE(info.current_tool == info.current_slot);
    }

    SECTION("active slot D → current_tool follows current_slot (slot 3)") {
        status["box"]["T1"]["filament"] = "D";
        auto info = cfs::parse_stock_box(status["box"]);
        REQUIRE(info.current_slot == 3);
        REQUIRE(info.current_tool == 3);
    }
}

TEST_CASE("CFS stock box bay reads EMPTY once vender and remain_len clear", "[cfs][status_parse]") {
    auto status = make_cfs_status_json();
    auto info = cfs::parse_stock_box(status["box"]);

    SECTION("available slots have HUB segment") {
        // All slots with filament (remain_len > 0) should be AVAILABLE
        REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        // get_slot_filament_segment returns HUB for AVAILABLE slots (tested via backend)
        // We can only test parse_box_status here since backend needs API
    }

    SECTION("empty slots have EMPTY status") {
        // Presence tracks `vender` + `remain_len`, not color: clear both signals
        // for the bay. The latched color_value stays populated, proving it is
        // not consulted.
        status["box"]["T1"]["vender"][0] = "none";
        status["box"]["T1"]["remain_len"][0] = "-1";
        auto info2 = cfs::parse_stock_box(status["box"]);
        REQUIRE(info2.units[0].slots[0].status == SlotStatus::EMPTY);
    }
}

// A latched/untagged color_value == "unknown" (with no remaining length)
// must NOT be treated as a present spool. A real hex color still reads AVAILABLE.
TEST_CASE("CFS parse: color_value 'unknown' is EMPTY, real hex is AVAILABLE",
          "[cfs][status_parse]") {
    // Slot 0: untagged spool — RFID reports sentinel "unknown" / "-1" with no
    // remaining length. Slot 1: a genuine hex color with remaining length.
    const json box =
        make_unit_box_explicit({"-1", "101001", "-1", "-1"}, {"unknown", "0FFFFFF", "-1", "-1"},
                               {"none", "unknown", "none", "none"}, {"-1", "57", "-1", "-1"});

    auto info = cfs::parse_stock_box(box);
    REQUIRE(info.units.size() == 1);

    SECTION("'unknown' color → EMPTY") {
        REQUIRE(info.units[0].slots[0].status == SlotStatus::EMPTY);
    }
    SECTION("real hex color + length → AVAILABLE") {
        REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    }
}

TEST_CASE("CFS parse: tagged spool removal reads EMPTY despite latched remain_len",
          "[cfs][status_parse][presence]") {
    // Live K2 T1 slot 4 (index 3), physically empty: vender correctly reads the
    // "none" sentinel while remain_len/color_value/material_type all stay
    // latched at the removed spool's values.
    json box = make_unit_box_explicit(
        /*material_types=*/{"unknown", "unknown", "101001", "101001"},
        /*color_values=*/{"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        /*venders=*/{"unknown", "unknown", "unknown", "none"},
        /*remain_lens=*/{"100", "0", "46", "50"});

    auto info = cfs::parse_stock_box(box);
    REQUIRE(info.units.size() == 1);

    SECTION("removed tagged bay is EMPTY, not pinned AVAILABLE by latched length") {
        CHECK(info.units[0].slots[3].status == SlotStatus::EMPTY);
        // Latched display fields are scrubbed so the empty bay renders blank.
        CHECK(info.units[0].slots[3].material.empty());
        CHECK(info.units[0].slots[3].remaining_length_m == 0.0f);
    }
    SECTION("seated bays are unaffected") {
        CHECK(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        CHECK(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
        CHECK(info.units[0].slots[2].status == SlotStatus::AVAILABLE);
    }
    SECTION("a seated bay reporting remain_len 0 stays AVAILABLE on its vender") {
        // Slot 1 is spooled out but still physically seated — vender says so.
        CHECK(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    }
}

TEST_CASE("CFS parse: untagged spool with remaining length stays PRESENT",
          "[cfs][status_parse][presence]") {
    // An untagged 3rd-party spool has no RFID
    // vendor and no RFID payload, but the measuring wheel reports real length.
    // Keying presence on `vender` alone — or dropping remain_len outright —
    // would wrongly parse these EMPTY.
    SECTION("hard sentinels (-1) in material/color") {
        json box =
            make_unit_box_explicit({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"},
                                   {"none", "none", "none", "none"}, {"46", "-1", "-1", "-1"});
        auto info = cfs::parse_stock_box(box);
        REQUIRE(info.units.size() == 1);
        CHECK(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        CHECK(info.units[0].slots[1].status == SlotStatus::EMPTY);
    }

    SECTION("'unknown' in material/color is also a no-payload sentinel") {
        json box = make_unit_box_explicit(
            {"unknown", "unknown", "-1", "-1"}, {"unknown", "unknown", "-1", "-1"},
            {"none", "none", "none", "none"}, {"46", "0", "-1", "-1"});
        auto info = cfs::parse_stock_box(box);
        REQUIRE(info.units.size() == 1);
        CHECK(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        CHECK(info.units[0].slots[1].status == SlotStatus::EMPTY);
    }

    SECTION("a genuinely empty never-tagged bay is EMPTY") {
        json box =
            make_unit_box_explicit({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"},
                                   {"none", "none", "none", "none"}, {"-1", "-1", "-1", "-1"});
        auto info = cfs::parse_stock_box(box);
        REQUIRE(info.units.size() == 1);
        CHECK(info.units[0].slots[0].status == SlotStatus::EMPTY);
    }
}
