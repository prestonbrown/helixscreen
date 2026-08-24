// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_database.h"
#include "filament_op_router.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "filament_variants.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/scoped_home_confirm_prompter.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

// Friend-class shim matching the one in test_filament_slot_override_store.cpp
// (declared friend in filament_slot_override_store.h per L065). Allows our
// Task 10 tests to redirect the store's read-cache to a per-test tmp dir so
// successful save_async calls don't pollute the developer's real config.
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

namespace {
// Per-test tmp cache dir — same idiom as test_filament_slot_override_store.cpp.
// The store's save callback writes a local JSON cache so the UI can show
// last-known overrides when Moonraker is unreachable. Without redirection,
// that cache lands in the developer's real helixscreen config dir.
struct Ad5xIfsTmpCacheDir {
    std::filesystem::path path;
    explicit Ad5xIfsTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("ad5x_ifs_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~Ad5xIfsTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

using json = nlohmann::json;

// Ad5xIfsTestAccess now lives in tests/test_helpers/ad5x_ifs_test_access.h
// (verbatim move) so the toolhead-unaccounted suite can share it.

// Helper to build an extruder temperature status frame (mirrors CFS shape:
// `{"extruder": {"temperature": T, "target": Tgt}}`).
static json make_extruder(double temperature, double target) {
    return json{{"extruder", json{{"temperature", temperature}, {"target", target}}}};
}

// Helper to build a full save_variables JSON payload
static json make_save_variables(const json& variables) {
    return json{{"save_variables", json{{"variables", variables}}}};
}

// Helper to build a port sensor notification
static json make_port_sensor(int port_1based, bool detected) {
    std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port_1based);
    return json{{key, json{{"filament_detected", detected}}}};
}

// Helper to build a head sensor notification
static json make_head_sensor(bool detected) {
    return json{
        {"filament_switch_sensor head_switch_sensor", json{{"filament_detected", detected}}}};
}

// Helper to build a native ZMOD motion sensor notification
static json make_motion_sensor(bool detected) {
    return json{
        {"filament_motion_sensor ifs_motion_sensor", json{{"filament_detected", detected}}}};
}

// Helper to build a head switch sensor notification under the native Z-Mod
// custom-module namespace (zmod_ifs_switch_sensor) that the live AD5X actually
// pushes — distinct from the stock filament_switch_sensor section.
static json make_zmod_head_sensor(bool detected) {
    return json{
        {"zmod_ifs_switch_sensor head_switch_sensor", json{{"filament_detected", detected}}}};
}

// Helper to build a motion sensor notification under the native Z-Mod
// custom-module namespace (zmod_ifs_motion_sensor).
static json make_zmod_motion_sensor(bool detected) {
    return json{
        {"zmod_ifs_motion_sensor ifs_motion_sensor", json{{"filament_detected", detected}}}};
}

// Standard test variables representing a typical IFS configuration. Note:
// `<prefix>_colors` and `<prefix>_types` are no longer consumed by
// parse_save_variables (they live in lessWaste/bambufy's private namespace,
// which zmod doesn't read). Tests that need colors/materials seeded must
// also call seed_standard_colors() — the entries in this map are kept so
// existing tests that simulate notifies still pass through unchanged for the
// fields parse_save_variables still cares about (tools, current_tool,
// external).
static json standard_variables() {
    return json{{"less_waste_colors", json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"})},
                {"less_waste_types", json::array({"PLA", "PETG", "ABS", "TPU"})},
                {"less_waste_tools", json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
                {"less_waste_current_tool", 0},
                {"less_waste_external", 0}};
}

// Seed colors_/materials_/port_presence_ to match standard_variables() — the
// shape that parse_save_variables used to populate. Use after constructing a
// backend to give tests a deterministic firmware-truth baseline.
static void seed_standard_colors(AmsBackendAd5xIfs& b) {
    // Presence FIRST, then color/material — mirroring the real atomic parse
    // (apply_zcolor_result fills presence + color + material together, so the
    // detectors' first observation of a slot always sees it present). Setting
    // material while the slot still reads absent would create an artificial
    // "material-before-presence" window that the real path never produces: with
    // the #1065 presence-lag baseline-hold, that window turns the later presence
    // flip into a "" -> MATERIAL insert delta and fabricates an auto-mirror
    // override. Presence-first keeps the post-seed state override-free, matching
    // production and every test written against this helper.
    Ad5xIfsTestAccess::set_port_presence(b, 0, true);
    Ad5xIfsTestAccess::set_port_presence(b, 1, true);
    Ad5xIfsTestAccess::set_port_presence(b, 2, true);
    Ad5xIfsTestAccess::set_port_presence(b, 3, true);
    Ad5xIfsTestAccess::set_color(b, 0, "FF0000");
    Ad5xIfsTestAccess::set_color(b, 1, "00FF00");
    Ad5xIfsTestAccess::set_color(b, 2, "0000FF");
    Ad5xIfsTestAccess::set_color(b, 3, "FFFFFF");
    Ad5xIfsTestAccess::set_material(b, 0, "PLA");
    Ad5xIfsTestAccess::set_material(b, 1, "PETG");
    Ad5xIfsTestAccess::set_material(b, 2, "ABS");
    Ad5xIfsTestAccess::set_material(b, 3, "TPU");
}

// ==========================================================================
// 1. Type identification
// ==========================================================================

TEST_CASE("AD5X IFS type identification", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE(backend.get_type() == AmsType::AD5X_IFS);
    REQUIRE(backend.get_topology() == PathTopology::LINEAR);
}

TEST_CASE("AD5X IFS: manages_active_spool() is true so the UI never auto-writes Spoolman",
          "[ams][ad5x][ifs][spoolman][1071]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    CHECK(backend.manages_active_spool() == true);
}

// ==========================================================================
// 2. parse_save_variables — full JSON
// ==========================================================================

TEST_CASE("AD5X IFS parse_save_variables full JSON", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());

    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::external_mode(backend));

    // Color/material seeded separately — parse_save_variables does not write
    // colors_[]/materials_[] anymore (those live in zmod's authoritative state,
    // not in lessWaste/bambufy's private namespace).
    seed_standard_colors(backend);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xFF0000);
    REQUIRE(info.material == "PLA");
    REQUIRE(info.mapped_tool == 0); // Tool 0 maps to port 1 (slot 0)

    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.color_rgb == 0x00FF00);
    REQUIRE(info1.material == "PETG");
    REQUIRE(info1.mapped_tool == 1);

    auto info2 = backend.get_slot_info(2);
    REQUIRE(info2.color_rgb == 0x0000FF);
    REQUIRE(info2.material == "ABS");
    REQUIRE(info2.mapped_tool == 2);

    auto info3 = backend.get_slot_info(3);
    REQUIRE(info3.color_rgb == 0xFFFFFF);
    REQUIRE(info3.material == "TPU");
    REQUIRE(info3.mapped_tool == 3);
}

// ==========================================================================
// 3. parse_save_variables with -1 active tool
// ==========================================================================

TEST_CASE("AD5X IFS no active tool", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    auto vars = standard_variables();
    vars["less_waste_current_tool"] = -1;

    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_tool == -1);
    REQUIRE(sys.current_slot == -1);
    REQUIRE_FALSE(sys.filament_loaded);
}

// ==========================================================================
// 4. Color hex parsing
// ==========================================================================

TEST_CASE("AD5X IFS color hex parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("lowercase hex works") {
        Ad5xIfsTestAccess::set_color(backend, 0, "ff0000");

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
    }

    SECTION("mixed case hex works") {
        Ad5xIfsTestAccess::set_color(backend, 0, "Ff0000");

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
    }

    SECTION("empty string leaves color unchanged") {
        // Seed slot 1 with a known color, then attempt to overwrite slot 0
        // with empty hex — update_slot_from_state's stoul fallback skips the
        // parse, so other slots are unaffected.
        Ad5xIfsTestAccess::set_color(backend, 1, "00FF00");
        Ad5xIfsTestAccess::set_color(backend, 0, "");

        auto info = backend.get_slot_info(1);
        REQUIRE(info.color_rgb == 0x00FF00);
    }
}

// ==========================================================================
// 5. Tool mapping reverse lookup
// ==========================================================================

TEST_CASE("AD5X IFS tool mapping reverse lookup", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("standard 1:1 mapping") {
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

        for (int i = 0; i < 4; ++i) {
            auto info = backend.get_slot_info(i);
            REQUIRE(info.mapped_tool == i);
        }
    }

    SECTION("non-standard mapping: T0->port3, T1->port1") {
        auto vars = standard_variables();
        // T0->3, T1->1, T2->5(unmapped), T3->2, rest unmapped
        vars["less_waste_tools"] = json::array({3, 1, 5, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        // Slot 0 (port 1): first tool mapping to port 1 is T1
        REQUIRE(backend.get_slot_info(0).mapped_tool == 1);
        // Slot 1 (port 2): first tool mapping to port 2 is T3
        REQUIRE(backend.get_slot_info(1).mapped_tool == 3);
        // Slot 2 (port 3): first tool mapping to port 3 is T0
        REQUIRE(backend.get_slot_info(2).mapped_tool == 0);
        // Slot 3 (port 4): no tool maps to port 4
        REQUIRE(backend.get_slot_info(3).mapped_tool == -1);
    }
}

// ==========================================================================
// 6. Port sensor parsing via handle_status_update
// ==========================================================================

TEST_CASE("AD5X IFS port sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Set port 1 and 3 as having filament
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, true));
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(3, true));

    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == true);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1) == false);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 2) == true);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 3) == false);

    // Clear port 1
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, false));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == false);
}

// ==========================================================================
// 7. Head sensor parsing via handle_status_update
// ==========================================================================

TEST_CASE("AD5X IFS head sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

// ==========================================================================
// 7b. Native ZMOD IFS motion sensor (no lessWaste per-port sensors)
// ==========================================================================

TEST_CASE("AD5X IFS native ZMOD motion sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    // Native ZMOD motion sensor maps to head filament state
    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE("AD5X IFS native ZMOD combined update (no per-port sensors)", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Simulate a native ZMOD IFS status update:
    // save_variables + motion sensor + head switch sensor, NO per-port sensors
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_motion_sensor ifs_motion_sensor"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Verify system state — should detect filament loaded via motion sensor
    auto sys = backend.get_system_info();
    REQUIRE(sys.type == AmsType::AD5X_IFS);
    REQUIRE(sys.total_slots == 4);
    REQUIRE(sys.filament_loaded);
    REQUIRE(sys.current_tool == 0);

    // Port presence is unknown in native ZMOD (no per-port sensors)
    // but save_variables provides colors and tool mapping
    REQUIRE(sys.units.size() == 1);
    REQUIRE(sys.units[0].slots.size() == 4);
}

// ==========================================================================
// 8. Combined status update
// ==========================================================================

TEST_CASE("AD5X IFS combined status update", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Build a combined notification with save_variables + sensors
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_switch_sensor _ifs_port_sensor_1"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor _ifs_port_sensor_2"] = json{{"filament_detected", false}};
    notification["filament_switch_sensor _ifs_port_sensor_3"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor _ifs_port_sensor_4"] = json{{"filament_detected", false}};
    notification["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Verify all state
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::external_mode(backend));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 2));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_tool == 0);
    REQUIRE(sys.current_slot == 0); // T0 maps to port 1 (slot 0)
    REQUIRE(sys.filament_loaded);
}

// ==========================================================================
// 9. get_system_info
// ==========================================================================

TEST_CASE("AD5X IFS get_system_info", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    auto sys = backend.get_system_info();
    REQUIRE(sys.type == AmsType::AD5X_IFS);
    REQUIRE(sys.type_name == "AD5X IFS");
    REQUIRE(sys.total_slots == 4);
    REQUIRE(sys.units.size() == 1);
    REQUIRE(sys.units[0].slots.size() == 4);
    REQUIRE(sys.supports_bypass);
    REQUIRE(sys.supports_tool_mapping);
    // The ENABLE bit; AVAILABILITY lives in get_endless_spool_capabilities().
    REQUIRE_FALSE(sys.endless_spool_enabled);
    REQUIRE_FALSE(sys.supports_purge);

    // IFS tool mapping: 16 entries (tool→slot), first 4 mapped, rest unmapped
    REQUIRE(sys.tool_to_slot_map.size() == 16);
    REQUIRE(sys.tool_to_slot_map[0] == 0);
    REQUIRE(sys.tool_to_slot_map[1] == 1);
    REQUIRE(sys.tool_to_slot_map[2] == 2);
    REQUIRE(sys.tool_to_slot_map[3] == 3);
    for (size_t i = 4; i < 16; ++i) {
        REQUIRE(sys.tool_to_slot_map[i] == -1);
    }
}

// ==========================================================================
// 9b. get_tool_mapping() advertises only the tools the firmware actually maps
//
// build_ams_topology() (ams_state.cpp) takes ToolTopology::tool_count straight
// from this vector's length, and ToolState turns that into the tool list every
// tool-count consumer reads. A 4-port AD5X with one hotend must therefore not
// hand back all 16 addressable T-numbers.
// ==========================================================================

TEST_CASE("AD5X IFS get_tool_mapping drops the trailing unmapped tools", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    auto mapping = backend.get_tool_mapping();
    REQUIRE(mapping.size() == 4);
    REQUIRE(mapping == std::vector<int>{0, 1, 2, 3});

    // The firmware register itself is untouched — only the advertised topology
    // shrinks, so the 16-wide system_info view still reports every T-number.
    REQUIRE(backend.get_system_info().tool_to_slot_map.size() == 16);
}

TEST_CASE("AD5X IFS get_tool_mapping keeps a mid-range hole", "[ams][ad5x_ifs]") {
    // tool_to_slot[i] is indexed BY tool number, so an unmapped T1 has to stay
    // a -1 hole rather than collapsing T2 down into its place.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto vars = standard_variables();
    // T0 -> port 1, T1 unmapped, T2 -> port 3, everything above unmapped.
    vars["less_waste_tools"] = json::array({1, 5, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

    auto mapping = backend.get_tool_mapping();
    REQUIRE(mapping == std::vector<int>{0, -1, 2});
}

TEST_CASE("AD5X IFS get_tool_mapping is empty when nothing is mapped", "[ams][ad5x_ifs]") {
    // Same answer the !has_ifs_vars_ path already gives, which build_ams_topology
    // reads as "fall back to a 1:1 map from the slot count".
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto vars = standard_variables();
    vars["less_waste_tools"] = json::array({5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

    REQUIRE(backend.get_tool_mapping().empty());
}

TEST_CASE("AD5X IFS set_tool_mapping still addresses the full 0..15 tool range",
          "[ams][ad5x_ifs]") {
    // Trimming is a topology decision, not a firmware one: zmod's _IFS_VARS tool
    // map is 16 wide and the user can still pin a lane to T15.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    // The gcode write fails with a null api_, but the local map is applied first.
    backend.set_tool_mapping(15, 0);
    REQUIRE(Ad5xIfsTestAccess::tool_map(backend)[15] == 1); // port = slot + 1

    auto mapping = backend.get_tool_mapping();
    REQUIRE(mapping.size() == 16);
    REQUIRE(mapping[15] == 0);
    for (size_t t = 4; t < 15; ++t) {
        INFO("tool " << t);
        REQUIRE(mapping[t] == -1);
    }

    // One past the top is still rejected, and leaves the map alone.
    auto before = Ad5xIfsTestAccess::tool_map(backend);
    REQUIRE_FALSE(backend.set_tool_mapping(AmsBackendAd5xIfs::TOOL_MAP_SIZE, 0).success());
    REQUIRE(Ad5xIfsTestAccess::tool_map(backend) == before);
}

// ==========================================================================
// 10. Bypass mode
// ==========================================================================

TEST_CASE("AD5X IFS bypass mode", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("external=1 activates bypass") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 1;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(backend.is_bypass_active());
    }

    SECTION("external=0 deactivates bypass") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 0;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(backend.is_bypass_active());
    }

    SECTION("toggle bypass via parse") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 1;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(backend.is_bypass_active());

        vars["less_waste_external"] = 0;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(backend.is_bypass_active());
    }
}

// ==========================================================================
// 11. build_color_list_value format
// ==========================================================================

TEST_CASE("AD5X IFS build_color_list_value format", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    seed_standard_colors(backend);

    std::string colors = Ad5xIfsTestAccess::build_colors(backend);
    // var_prefix_ defaults to "less_waste" and tool_map_ is all-unmapped, so
    // the builder takes the identity fallback (T0..T3 -> ports 1..4, matching
    // lessWaste's own variable_tools default) and emits a 16-entry
    // TOOL-indexed list: the 4 port colours then 12 empty entries for the
    // unmapped virtual tools (#1247 — lessWaste's _RUNOUT_HEAD scans all 16
    // tool slots, so a 4-entry payload truncated the arrays and no backup
    // lane could ever match).
    std::string expected = "\"['FF0000', '00FF00', '0000FF', 'FFFFFF'";
    for (int i = 0; i < 12; ++i) {
        expected += ", ''";
    }
    expected += "]\"";
    REQUIRE(colors == expected);
}

TEST_CASE("AD5X IFS lessWaste list payload projects tool_map_ per tool", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    seed_standard_colors(backend);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Swapped T0/T1, T2/T3, virtual tools 4-14 unmapped, T15 -> port 1.
    json vars{{"less_waste_tools", json::array({2, 1, 4, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 1})}};
    Ad5xIfsTestAccess::parse_vars(backend, vars);

    // Tool t's entry is colors_[tool_map_[t]-1]; unmapped tools carry ''.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend) ==
            "\"['00FF00', 'FF0000', 'FFFFFF', '0000FF', '', '', '', '', '', '', '', "
            "'', '', '', '', 'FF0000']\"");
    REQUIRE(Ad5xIfsTestAccess::build_types(backend) ==
            "\"['PETG', 'PLA', 'TPU', 'ABS', '', '', '', '', '', '', '', "
            "'', '', '', '', 'PLA']\"");
}

TEST_CASE("AD5X IFS bambufy list payload stays port-indexed 4-entry", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    seed_standard_colors(backend);
    Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");

    // bambufy's _RUNOUT_HEAD iterates ifs.types (4 entries) and indexes
    // ifs.colors[port-1] — the port-indexed shape is correct there, and the
    // #1247 tool-indexing must not leak across prefixes.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend) ==
            "\"['FF0000', '00FF00', '0000FF', 'FFFFFF']\"");
    REQUIRE(Ad5xIfsTestAccess::build_types(backend) == "\"['PLA', 'PETG', 'ABS', 'TPU']\"");
}

// ==========================================================================
// 12. build_tool_map_value format
// ==========================================================================

TEST_CASE("AD5X IFS build_tool_map_value format", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());

    std::string tools = Ad5xIfsTestAccess::build_tools(backend);
    REQUIRE(tools == "\"[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]\"");
}

// ==========================================================================
// 13. set_slot_info with persist=false
// ==========================================================================

TEST_CASE("AD5X IFS set_slot_info persist=false", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // First parse standard state so slots exist
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    // Use a material in the firmware whitelist so normalize_material()
    // doesn't coerce it. See get_supported_materials() for the accepted set.
    SlotInfo new_info;
    new_info.color_rgb = 0x123456;
    new_info.material = "SILK";
    new_info.spoolman_id = 42;
    new_info.remaining_weight_g = 500;
    new_info.total_weight_g = 1000;

    auto err = backend.set_slot_info(1, new_info, false);
    REQUIRE(err.success());

    auto info = backend.get_slot_info(1);
    REQUIRE(info.color_rgb == 0x123456);
    REQUIRE(info.material == "SILK");
    REQUIRE(info.spoolman_id == 42);
    REQUIRE(info.remaining_weight_g == 500);
    REQUIRE(info.total_weight_g == 1000);
}

// ==========================================================================
// 14. Slot status mapping
// ==========================================================================

TEST_CASE("AD5X IFS slot status mapping", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("port with filament, not active → AVAILABLE") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        // Port 2 has filament, active tool is T0 (mapped to port 1)
        notification["filament_switch_sensor _ifs_port_sensor_2"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(1); // slot 1 = port 2
        REQUIRE(info.status == SlotStatus::AVAILABLE);
    }

    SECTION("port with filament, is active + head loaded → LOADED") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        // Port 1 has filament, active tool is T0 (mapped to port 1), head has filament
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(0); // slot 0 = port 1
        REQUIRE(info.status == SlotStatus::LOADED);
    }

    SECTION("port without filament → EMPTY") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_3"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(2); // slot 2 = port 3
        REQUIRE(info.status == SlotStatus::EMPTY);
    }
}

// ==========================================================================
// 15. Action state tracking
// ==========================================================================

TEST_CASE("AD5X IFS action state tracking", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("load_filament sets LOADING action (precondition fails with null api)") {
        // load_filament will fail at check_preconditions with null api,
        // so we can't test the action being set via that path.
        // Instead test the action inference: LOADING + head sensor → IDLE
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

        // Head sensor triggers → load complete
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("UNLOADING + head sensor cleared → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);

        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("LOADING + head sensor NOT triggered → stays LOADING") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }
}

// ==========================================================================
// 16. Path segments
// ==========================================================================

TEST_CASE("AD5X IFS path segments", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("get_filament_segment: no filament anywhere → NONE") {
        REQUIRE(backend.get_filament_segment() == PathSegment::NONE);
    }

    SECTION("get_filament_segment: head has filament → NOZZLE") {
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        REQUIRE(backend.get_filament_segment() == PathSegment::NOZZLE);
    }

    SECTION("get_filament_segment: port has filament, active tool set, head empty → LANE") {
        json notification;
        auto vars = standard_variables();
        vars["less_waste_current_tool"] = 0; // T0 → port 1
        notification["save_variables"] = json{{"variables", vars}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        // head sensor NOT set (defaults to false)
        Ad5xIfsTestAccess::handle_status(backend, notification);

        REQUIRE(backend.get_filament_segment() == PathSegment::LANE);
    }

    SECTION("get_slot_filament_segment: active slot with head filament → NOZZLE") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        REQUIRE(backend.get_slot_filament_segment(0) == PathSegment::NOZZLE);
    }

    SECTION("get_slot_filament_segment: non-active slot with filament → HUB") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_2"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        // Slot 1 (port 2) has filament but is not active — shows at hub
        REQUIRE(backend.get_slot_filament_segment(1) == PathSegment::HUB);
    }

    SECTION("get_slot_filament_segment: empty slot → NONE") {
        // Slot 2 with port_presence_=false → NONE.
        Ad5xIfsTestAccess::set_port_presence(backend, 2, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(backend.get_slot_filament_segment(2) == PathSegment::NONE);
    }

    SECTION("get_slot_filament_segment: non-active slot with color data → HUB") {
        // Slot 2 with port_presence_=true and not the active slot → HUB.
        seed_standard_colors(backend);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(backend.get_slot_filament_segment(2) == PathSegment::HUB);
    }

    SECTION("get_slot_filament_segment: out of range → NONE") {
        REQUIRE(backend.get_slot_filament_segment(-1) == PathSegment::NONE);
        REQUIRE(backend.get_slot_filament_segment(4) == PathSegment::NONE);
    }
}

// ==========================================================================
// Helper to wrap raw status JSON in Moonraker notify_status_update format
// ==========================================================================
static json wrap_notification(const json& status) {
    return json{{"method", "notify_status_update"}, {"params", json::array({status, 12345.678})}};
}

// ==========================================================================
// 17. Wrapped notification format (real WebSocket path)
// ==========================================================================

TEST_CASE("AD5X IFS handles wrapped notify_status_update", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("wrapped port sensor updates state") {
        auto wrapped = wrap_notification(make_port_sensor(1, true));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == true);
    }

    SECTION("wrapped head sensor updates state") {
        auto wrapped = wrap_notification(make_head_sensor(true));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    }

    SECTION("wrapped save_variables updates state") {
        seed_standard_colors(backend);
        auto wrapped = wrap_notification(make_save_variables(standard_variables()));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
        REQUIRE(info.material == "PLA");
    }

    SECTION("wrapped combined notification updates all state") {
        json status;
        status["save_variables"] = json{{"variables", standard_variables()}};
        status["filament_switch_sensor _ifs_port_sensor_1"] = json{{"filament_detected", true}};
        status["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(status));

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

        auto sys = backend.get_system_info();
        REQUIRE(sys.current_tool == 0);
        REQUIRE(sys.current_slot == 0);
        REQUIRE(sys.filament_loaded);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.status == SlotStatus::LOADED);
    }

    SECTION("wrapped notification completes load action") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(make_head_sensor(true)));

        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("wrapped notification completes unload action") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        // Head sensor was true, now cleared
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(make_head_sensor(false)));

        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("unwrapped format still works (initial query response)") {
        // on_started() callback sends unwrapped format — must still work
        Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(2, true));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1) == true);
    }
}

// ==========================================================================
// 18. Action timeout safety net
// ==========================================================================

TEST_CASE("AD5X IFS action timeout resets stuck operations", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("LOADING surfaces ERROR after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    }

    SECTION("UNLOADING surfaces ERROR after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    }

    SECTION("IDLE does not change on timeout check") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::IDLE);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("action does not reset before timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(30));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }

    SECTION("get_system_info surfaces ERROR after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));

        auto sys = backend.get_system_info();
        REQUIRE(sys.action == AmsAction::ERROR);
    }
}

TEST_CASE("AD5X IFS PURGING gets a longer dedicated timeout budget (#1065 Bug 2)",
          "[ams][ad5x_ifs]") {
    // A real purge runs far longer than the generic 90 s phase budget (raza616:
    // ~3 min whole-op from cold; Vger1700 hit the 90 s ERROR twice mid-purge).
    // PURGING gets its own longer budget so a healthy-but-slow purge isn't killed.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("does not ERROR within the dedicated budget (past the old 90 s window)") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::PURGING);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(200));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::PURGING);
    }

    SECTION("still ERRORs once the dedicated budget is exceeded") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::PURGING);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(241));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    }
}

TEST_CASE("AD5X IFS motion during PURGING resets the timeout clock (#1065 Bug 2)",
          "[ams][ad5x_ifs]") {
    // ifs_motion_sensor activity proves filament is still moving, so the purge is
    // progressing — reset the clock rather than ERRORing. The budget is then
    // effectively "time since last motion", so a long purge that keeps moving is
    // never falsely failed, while a genuinely stalled one still surfaces ERROR.
    SECTION("motion keeps a long-but-moving purge alive past the budget") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_action(backend, AmsAction::PURGING);
        Ad5xIfsTestAccess::set_action_age(backend, std::chrono::seconds(300)); // past budget
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(true));   // still moving
        Ad5xIfsTestAccess::run_action_timeout(backend);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::PURGING);
    }

    SECTION("a stalled purge with no motion still ERRORs") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_action(backend, AmsAction::PURGING);
        Ad5xIfsTestAccess::set_action_age(backend, std::chrono::seconds(300));
        Ad5xIfsTestAccess::run_action_timeout(backend);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    }
}

// ==========================================================================
// Indeterminate ("Working…") detector (#1065 row 14)
//
// During a phase-tracked load/unload the constrained AD5X's shared main-thread
// status feed can starve while klippy runs the blocking macro, freezing the
// live "Heat 225/230" number so it reads as a hang. The backend raises
// AmsSystemInfo::operation_indeterminate when no genuine progress signal (a
// temp-VALUE change, head transition, motion, or phase change) has landed within
// INDETERMINATE_THRESHOLD_SECONDS (~8s); the sidebar then swaps the frozen number
// for a "Working…" busy state. Time is INJECTED via set_progress_age (no sleeps),
// so these stay fast and untagged (L052).
// ==========================================================================

TEST_CASE("AD5X IFS indeterminate: a stalled progress feed trips the flag (#1065 row 14)",
          "[ams][ad5x_ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false); // load -> HEATING

    // One healthy heat frame — flag stays clear while progress is fresh.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(150.0, 230.0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));

    // Feed starves: no progress signal for 10s (> 8s threshold).
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(10));
    Ad5xIfsTestAccess::run_action_timeout(backend);

    REQUIRE(Ad5xIfsTestAccess::operation_indeterminate(backend));
    // Still HEATING — this is the busy mitigation, NOT the coarse ERROR timeout
    // (HEATING's 300s ERROR budget is untouched; only the progress clock aged).
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    // The UI-facing view carries the flag too.
    REQUIRE(backend.get_system_info().operation_indeterminate);
}

TEST_CASE("AD5X IFS indeterminate: a fresh progress signal clears the flag (#1065 row 14)",
          "[ams][ad5x_ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(150.0, 230.0));

    // Just under threshold — does not fire.
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(7));
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));

    // Past threshold — fires.
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(10));
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE(Ad5xIfsTestAccess::operation_indeterminate(backend));

    // A temp-VALUE change is a genuine progress signal → clears immediately.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(180.0, 230.0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));
}

// The sidebar's stall watchdog (ui_ams_sidebar) drives the detector by calling
// AmsState::sync_from_backend() on a main-thread timer while an op is active —
// which reaches the backend only through get_system_info(). So get_system_info()
// MUST run the timeout check itself, WITHOUT a prior status frame or an explicit
// run_action_timeout(): that is the only signal available when the WebSocket feed
// has stalled (#1065 row 14). If this regresses, the "Working…" state never
// appears on a real hang even though the flag logic is correct.
TEST_CASE(
    "AD5X IFS indeterminate: get_system_info() trips the flag on its own (#1065 watchdog path)",
    "[ams][ad5x_ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(150.0, 230.0));
    REQUIRE_FALSE(backend.get_system_info().operation_indeterminate);

    // Feed starves past the threshold. No run_action_timeout(), no status frame —
    // only the watchdog's get_system_info() call, exactly as sync_from_backend()
    // reaches it.
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(10));
    REQUIRE(backend.get_system_info().operation_indeterminate);
}

TEST_CASE("AD5X IFS indeterminate: healthy heat never false-fires; frozen value does "
          "(#1065 row 14)",
          "[ams][ad5x_ifs][1065]") {
    SECTION("rising temps within threshold never trip") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

        // Each distinct temp frame is a value change that resets the clock, even
        // with realistic multi-second gaps between frames — so a normal heat-up
        // never trips the detector.
        for (double t : {100.0, 130.0, 160.0, 190.0, 220.0, 230.0}) {
            Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(6));
            Ad5xIfsTestAccess::handle_status(backend, make_extruder(t, 230.0));
            REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));
        }
    }

    SECTION("identical (frozen) frames do NOT reset — the clock elapses") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

        Ad5xIfsTestAccess::handle_status(backend, make_extruder(150.0, 230.0));
        REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));

        // The subject is frozen at 150°C: the SAME value keeps (nominally)
        // arriving but the clock ages past threshold with no VALUE change, so the
        // detector must fire — this is the exact row-14 behavior.
        Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(10));
        Ad5xIfsTestAccess::handle_status(backend, make_extruder(150.0, 230.0));
        REQUIRE(Ad5xIfsTestAccess::operation_indeterminate(backend));
    }
}

TEST_CASE("AD5X IFS indeterminate: flag clears when the operation completes (#1065 row 14)",
          "[ams][ad5x_ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(150.0, 230.0));

    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(10));
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE(Ad5xIfsTestAccess::operation_indeterminate(backend));

    // Operation completes on the macro's gcode ack → IDLE, phase tracker cleared.
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/false);
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);

    // get_system_info() re-runs the detector; with no active op the flag is off.
    REQUIRE_FALSE(backend.get_system_info().operation_indeterminate);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));
}

// ==========================================================================
// 19. Variable prefix auto-detection (lessWaste vs bambufy)
// ==========================================================================

TEST_CASE("AD5X IFS variable prefix auto-detection", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    SECTION("defaults to less_waste prefix") {
        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "less_waste");
    }

    SECTION("detects bambufy prefix from colors") {
        json vars;
        vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        vars["bambufy_types"] = json::array({"PLA", "PETG", "ABS", "TPU"});
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        vars["bambufy_current_tool"] = 0;
        vars["bambufy_external"] = 0;

        seed_standard_colors(backend);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "bambufy");
        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);

        // Color/material sourced from CHANGE_ZCOLOR/GET_ZCOLOR (seeded above),
        // not from <prefix>_colors.
        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
        REQUIRE(info.material == "PLA");
    }

    SECTION("detects bambufy prefix from tools alone") {
        json vars;
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});

        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "bambufy");
    }
}

// ==========================================================================
// 20. Motion sensor triggers load/unload completion (native ZMOD)
// ==========================================================================

TEST_CASE("AD5X IFS motion sensor completes load/unload", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("LOADING + motion sensor detected → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(true));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("UNLOADING + motion sensor cleared → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("LOADING + motion sensor NOT detected → stays LOADING") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }
}

// ==========================================================================
// 20b. Phase tracker — live load/unload progress feedback
// ==========================================================================

TEST_CASE("AD5X IFS phase: unload sequence (temp + head sensor)", "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Filament present at the toolhead before unload begins.
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Begin a phased unload (firmware: HEATING → CUTTING → UNLOADING → IDLE).
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    // Before any temp or target seen — detail names neither. The backend has no
    // signal to name a target with, and asserting one it never observed would be
    // a fabrication.
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Heating nozzle");

    // First temp frame: still heating, detail gains the live current temp.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(185.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Heating nozzle to 230°C (185°C)");

    // Temp reaches target → CUTTING.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Cutting filament");

    // Head sensor drops (cut + retract started) → UNLOADING.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Retracting filament from nozzle");

    // Action timeout backstop surfaces ERROR (firmware did not confirm completion;
    // detail is preserved as the error context for the error-center bridge).
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_detail(backend).empty());
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: load sequence (temp + head sensor)", "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Fresh load: no filament at the toolhead initially.
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    // Heating not yet complete.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(190.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    // Temp reaches target → LOADING (feeding filament toward nozzle).
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Feeding filament to nozzle");

    // Head sensor rises (filament reached nozzle) → PURGING.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::PURGING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Purging old filament");

    // Timeout backstop surfaces ERROR; detail preserved for error-center bridge.
    // PURGING has a dedicated 240s budget (#1065 Bug 2), so age past it.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(250));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_detail(backend).empty());
}

TEST_CASE(
    "AD5X IFS get_system_info surfaces the phase machine's operation_phase/detail (#1065 Bug 2)",
    "[ams][ad5x_ifs][1065]") {
    // The phase machine writes operation_phase (0/1/2) + operation_detail into
    // system_info_, but AmsState::sync_from_backend reads them off the
    // AmsSystemInfo that get_system_info() returns to drive the ams_operation_phase
    // subject the right-side step tracker observes. If get_system_info() drops
    // those fields, the steps render but the active one never highlights and the
    // detail line goes blank (mkleersn v0.99.87: "the 1-2-3 steps show but fail to
    // launch any of them").
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false); // load -> HEATING, phase 0

    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 0); // internal phase-machine state
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_detail(backend).empty());

    // get_system_info() is the UI-facing view — it MUST carry the same values.
    auto info = backend.get_system_info();
    CHECK(info.operation_phase == 0);
    CHECK(info.operation_detail == Ad5xIfsTestAccess::operation_detail(backend));

    // Advance to LOADING (phase 1) and re-check the UI view tracks it.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 1);
    CHECK(backend.get_system_info().operation_phase == 1);
}

TEST_CASE("AD5X IFS phase: RESPOND line sets target before any extruder frame",
          "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    // Firmware emits the heat target via RESPOND before the first temp tick.
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Heating the nozzle to 240 degrees");

    // Detail should reflect the parsed 240°C target even with no extruder frame.
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Heating nozzle to 240°C");

    // A temp frame whose target is 240 reaches target → CUTTING.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(240.0, 240.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);
}

TEST_CASE("AD5X IFS phase: RESPOND direction disambiguation", "[ams][ad5x_ifs][phase]") {
    SECTION("\"Unloading filament\" marks an unload op") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_head_filament(backend, true);
        // Begin as a load, then a RESPOND line corrects the direction to unload.
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Unloading filament from IFS");
        // Reaching target on an unload op → CUTTING (not LOADING).
        Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);
    }

    SECTION("\"Loading filament\" marks a load op") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        // Begin as an unload, then a RESPOND line corrects the direction to load.
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Loading filament into IFS");
        // Reaching target on a load op → LOADING (not CUTTING).
        Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }
}

TEST_CASE("AD5X IFS phase: full unload with ZERO RESPOND lines (fork robustness)",
          "[ams][ad5x_ifs][phase]") {
    // Proves the English RESPOND strings are NOT load-bearing: temp + head
    // sensor alone drive the entire HEATING → CUTTING → UNLOADING sequence.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(100.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);
}

TEST_CASE("AD5X IFS phase: HEATING does not finalize before target at 90s",
          "[ams][ad5x_ifs][phase]") {
    // Regression: a real AD5X cold-start unload heats ~26°C→230°C in ~158s,
    // which exceeds the 90s ACTION_TIMEOUT. HEATING must get a longer dedicated
    // budget so the timeout doesn't snap to IDLE mid-heat (which reproduced the
    // original "nothing happening" complaint). Later phases keep the short 90s.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    // Still heating (120°C of 230°C target).
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(120.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    // 120s elapsed: under the 300s HEATING budget → must NOT finalize.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    // Reach target → CUTTING. The transition resets the start clock, so a fresh
    // op clock now governs the 90s budget for this phase.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    // 120s elapsed against the freshly-reset clock, with the 90s non-heating
    // budget → now surfaces ERROR (operation timed out).
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: clock resets on phase transition (no immediate timeout)",
          "[ams][ad5x_ifs][phase]") {
    // A phase transition occurring at elapsed > 90s must not immediately time
    // out the new phase — the start clock is reset on transition.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(150.0, 230.0));
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    // Immediately after the transition, a short elapsed must NOT finalize.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(30));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);
}

// ==========================================================================
// Swap-aware LOADING timeout (#1065 v0.99.94, bundle NJB2U558).
//
// `INSERT_PRUTOK_IFS PRUTOK=N` when another lane is currently loaded runs an
// IMPLICIT UNLOAD of the seated lane first (heat → cut → retract), THEN loads
// the new lane. HelixScreen's LOAD phase tracker only watches `seen_head_rise`
// (filament reaching nozzle), so the implicit-unload head drop is invisible to
// the timeout clock. Result: LOADING times out at 90s while the macro is still
// mid-swap, surfacing a false "Loading error, feeding filament to nozzle
// (timed out)" popup even though the op completes successfully.
//
// Two fixes:
//   1. on_head_transition_locked: a head drop during a LOAD op resets
//      action_start_time_ (the implicit unload is genuine progress — it just
//      isn't surfaced as a separate phase today).
//   2. load_filament: when dispatched with another lane currently seated,
//      set swap_expected so check_action_timeout can extend the LOADING
//      budget and cover the implicit unload + load sequence.
// ==========================================================================

TEST_CASE("AD5X IFS phase: head drop during LOAD resets the timeout clock (#1065 swap)",
          "[ams][ad5x_ifs][phase][1065]") {
    // Reproduces bundle NJB2U558's "feeding filament to nozzle (timed out)"
    // false positive: a LOAD op where the implicit unload of the previously-
    // seated lane drives a head-sensor drop. Before the fix, that drop didn't
    // reset action_start_time_, so 90s after HEATING→LOADING transition the
    // LOADING phase timed out mid-swap.
    //
    // Test helper note: check_action_timeout(elapsed) OVERWRITES
    // action_start_time_ before running the check, so it can't be used to
    // verify a reset that happened earlier. We use set_action_age to plant a
    // stale clock, fire the head drop, then run_action_timeout (no overwrite)
    // — the timeout's behavior tells us whether the reset stuck.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Swap context: another lane is currently seated, so the head reads
    // loaded at LOADING-phase start. The implicit unload will drive
    // head_filament_ true → false; starting false would make that transition
    // a no-op and on_head_transition_locked would never run.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    // Heat to target → LOADING begins. apply_phase_action_locked reset
    // action_start_time_ on the transition.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

    // Simulate 100s of LOADING-phase time passing — past the 90s default
    // budget, so without a reset the next check_action_timeout fires ERROR.
    Ad5xIfsTestAccess::set_action_age(backend, std::chrono::seconds(100));

    // Implicit unload of the previously-seated lane: head sensor drops. The
    // swap has just BEGUN — the actual load of the new lane hasn't started.
    // This must reset action_start_time_ to ~now so the LOADING budget
    // counts from here, not from 100s ago.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));

    // Run check_action_timeout WITHOUT set_action_age's overwrite — uses
    // whatever action_start_time_ the head drop left behind. With the fix,
    // elapsed is ~0s (no timeout, action stays LOADING). Without the fix,
    // elapsed is 100s (ERROR fires).
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: head drop during LOAD clears operation_indeterminate (#1065 swap)",
          "[ams][ad5x_ifs][phase][1065]") {
    // Companion to the timeout-reset test: the implicit-unload head drop is
    // a genuine progress signal, so the indeterminate ("Working…") detector
    // must clear. Before the fix, a stalled progress feed could false-fire
    // even though the macro was actively cutting/retracting the old lane.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Swap context: another lane is already seated at the head. The implicit
    // unload will drive head_filament_ true → false. Starting false would
    // miss the transition (was==detected==false) and never reach
    // on_head_transition_locked at all.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

    // Stall the progress feed past the indeterminate threshold (~8s).
    Ad5xIfsTestAccess::set_progress_age(backend, std::chrono::seconds(15));
    // Run the detector head — it should raise the indeterminate flag.
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE(Ad5xIfsTestAccess::operation_indeterminate(backend));

    // Implicit-unload head drop is a progress signal — flag must clear on the
    // next check_action_timeout call (which is when the detector recomputes).
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    Ad5xIfsTestAccess::run_action_timeout(backend);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_indeterminate(backend));
}

TEST_CASE("AD5X IFS phase: head drop during UNLOAD still works as before (#1065 regression)",
          "[ams][ad5x_ifs][phase][1065]") {
    // Regression: the existing unload path uses seen_head_drop to advance
    // HEATING → CUTTING → UNLOADING. Bug B's fix must NOT change that — the
    // head drop's phase-advancement contract for UNLOAD ops is preserved.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);
}

TEST_CASE("AD5X IFS phase: plain load (no swap) still uses 90s LOADING budget (#1065 regression)",
          "[ams][ad5x_ifs][phase][1065]") {
    // Regression: Bug B fix 2 extends LOADING timeout ONLY when another lane
    // is currently seated at dispatch. A plain load into an empty toolhead
    // must keep the 90s budget — otherwise a genuinely stuck load would hang
    // the UI for 180s before surfacing ERROR.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    // No seated lane — swap_expected must NOT be set.
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

    // 100s elapsed > 90s LOADING budget → must surface ERROR. The swap
    // extension must NOT apply here.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(100));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: swap load (another lane seated) extends LOADING budget (#1065)",
          "[ams][ad5x_ifs][phase][1065]") {
    // Reproduces bundle NJB2U558: load ch2 while ch4 is seated. The implicit
    // unload of ch4 eats ~50s; the load of ch2 then runs another ~40s. The
    // 90s LOADING budget fires mid-swap. Fix 2 extends LOADING to
    // SWAP_LOADING_TIMEOUT_SECONDS when seated_chan_ differs from the target.
    //
    // We can't easily call load_filament() here (it requires the gcode API +
    // check_preconditions). Instead we drive begin_phase directly + flip the
    // swap flag via the test accessor — exercising exactly the code path
    // check_action_timeout will consult.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    // Seed another lane as currently seated — this is what load_filament
    // sees when dispatching and what triggers swap_expected.
    Ad5xIfsTestAccess::set_ffm_channel(backend, 4); // slot 3 (0-based) seated
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    // Manually flip swap_expected — mirroring what load_filament does at
    // dispatch when seated_chan_ != target slot.
    Ad5xIfsTestAccess::set_swap_expected(backend, true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

    // 100s elapsed > 90s default LOADING budget. With swap_expected the
    // extended budget applies → must NOT surface ERROR yet.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(100));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    // Past the extended budget → ERROR fires (the extension is finite, not
    // unlimited — a genuinely stuck swap still surfaces). SWAP_LOADING_TIMEOUT_SECONDS
    // is 180; check at 190 to exceed it.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(190));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

// Helper: build a minimal valid GET_ZCOLOR result. extruder_slot ABSENT after
// a successful unload, SET after a successful load. saw_valid gates a
// meaningful read.
static AmsBackendAd5xIfs::ZColorSilentResult make_zcolor(std::optional<int> extruder_slot,
                                                         bool saw_valid) {
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = saw_valid;
    r.extruder_slot = extruder_slot;
    // slots[] left default-empty (std::array of std::optional) — the per-port
    // loop in apply_zcolor_result handles empty entries as "not loaded".
    return r;
}

TEST_CASE("AD5X IFS phase: zcolor quick-finish unload (head drop seen)", "[ams][ad5x_ifs][phase]") {
    // After the unload physically progresses past the cut (head drop), a fresh
    // GET_ZCOLOR with no extruder slot is the early terminal signal — finalize
    // to IDLE within ~1s instead of waiting out the 90s timeout backstop.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);

    // zcolor confirms the toolhead is now empty → IDLE, no timeout call.
    Ad5xIfsTestAccess::apply_zcolor_result(backend, make_zcolor(std::nullopt, /*saw_valid=*/true));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend).empty());
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: zcolor does NOT finalize before head transition",
          "[ams][ad5x_ifs][phase]") {
    // The early post-dispatch query (unload_filament schedules one immediately)
    // must NOT finalize before the op physically progresses past the cut. With
    // no head drop seen, progressed==false → stay in CUTTING.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    // zcolor with empty extruder, but no head drop yet → must NOT finalize.
    Ad5xIfsTestAccess::apply_zcolor_result(backend, make_zcolor(std::nullopt, /*saw_valid=*/true));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: zcolor quick-finish load (head rise seen)", "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::PURGING);

    // zcolor confirms slot 0 (port 1) now seated in the extruder → IDLE.
    Ad5xIfsTestAccess::apply_zcolor_result(backend,
                                           make_zcolor(std::optional<int>(0), /*saw_valid=*/true));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: zcolor invalid response does not finalize", "[ams][ad5x_ifs][phase]") {
    // A junk read (saw_valid_response=false) must never finalize the op — even
    // after a head transition. apply_zcolor_result early-returns on it.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);

    Ad5xIfsTestAccess::apply_zcolor_result(backend, make_zcolor(std::nullopt, /*saw_valid=*/false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: inactive tracker preserves legacy snap-to-IDLE",
          "[ams][ad5x_ifs][phase]") {
    // When the phase tracker is INACTIVE (external/firmware-initiated action set
    // via set_action), a head transition must still snap directly to IDLE — the
    // legacy backward-compat path. This mirrors the existing "action state
    // tracking" cases but asserts the gating contract explicitly.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));

    Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
}

// ==========================================================================
// 21. Native ZMOD IFS active slot inferred from head sensor
// ==========================================================================

TEST_CASE("AD5X IFS native ZMOD infers active slot from head sensor", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Seed colors_ + port_presence_ to simulate the post-GET_ZCOLOR / Adventurer5M
    // state. parse_save_variables no longer sets these fields (they live in
    // zmod's namespace, not lessWaste's).
    seed_standard_colors(backend);

    // No per-port sensors — only motion sensor and save_variables
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_motion_sensor ifs_motion_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Active tool is T0 → port 1 → slot 0. With head filament detected and no
    // per-port sensors, the active slot should be inferred as LOADED.
    auto info = backend.get_slot_info(0);
    REQUIRE(info.status == SlotStatus::LOADED);

    // Non-active slots with port_presence_ true are AVAILABLE (not EMPTY).
    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.status == SlotStatus::AVAILABLE);
}

// ==========================================================================
// 22. has_ifs_vars_ detection
// ==========================================================================

TEST_CASE("AD5X IFS has_ifs_vars detection", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("defaults to false") {
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set true when lessWaste variables found and macro verified") {
        // Simulate initial query confirming macro exists (clears pessimistic latch)
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set true when bambufy variables found and macro verified") {
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        json vars;
        vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("stays false when save_variables arrive before macro check completes") {
        // Latch starts true (pessimistic) — save_variables alone can't enable has_ifs_vars
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("stays false when save_variables has no recognized prefix") {
        json vars;
        vars["some_other_var"] = 42;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }
}

TEST_CASE("AD5X IFS has_ifs_vars reset when macro missing", "[ams][ad5x_ifs]") {
    // Scenario: lessWaste/bambufy plugins partially installed — save_variables data
    // exists but _IFS_VARS gcode macro is not loaded. parse_save_variables() sets
    // has_ifs_vars_ true, but on_started() should reset it when the macro is absent.
    // This test verifies the parse step sets the flag (the reset happens in on_started).
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("pessimistic latch prevents flag before macro check") {
        // Latch starts true — parse_save_variables can't set has_ifs_vars_
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Clearing the latch (as initial query would for a present macro) allows it
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION(
        "later save_variables notify cannot re-enable has_ifs_vars once macro confirmed missing") {
        // Simulate macro verified + save_variables arriving → has_ifs_vars_ = true
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Simulate on_started() discovering the macro is absent.
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, true);

        // A later subscription update carrying lessWaste save_variables arrives.
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Same for bambufy prefix.
        json bambufy_vars;
        bambufy_vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        bambufy_vars["bambufy_tools"] =
            json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(bambufy_vars));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set_slot_info uses native ZMOD path when has_ifs_vars is false") {
        // Clear latch to pre-populate slot data, then re-set to simulate macro missing
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, true);

        // set_slot_info without persist should succeed regardless
        SlotInfo info;
        info.color_rgb = 0x00FF00;
        info.material = "PETG";
        auto err = backend.set_slot_info(0, info, false);
        REQUIRE(err.success());
    }
}

// Regression: lessWaste/bambufy save_variables rows persist in
// printer_data/database/ even after the user uninstalls the plugin and the
// gcode_macro _ifs_vars goes away. Pre-fix, parse_save_variables read
// <prefix>_tools / _current_tool / _external unconditionally if the keys were
// present, so a user with stale rows would silently keep using the dead
// plugin's last-known tool map and active-tool guess as truth on every boot.
// Now those reads are gated on has_ifs_vars_ — i.e. plugin actively loaded.
TEST_CASE("AD5X IFS stale save_variables ignored when plugin macro missing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Confirm latch defaults to "macro missing" — the on_started() initial
    // query never confirmed the macro exists.
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    // Build a save_variables blob that looks like the user once had lessWaste
    // installed: tools/active/external all set to non-default values that
    // would VISIBLY change behavior if applied.
    auto stale = standard_variables();
    stale["less_waste_current_tool"] = 2; // not the default 0
    stale["less_waste_external"] = 1;     // bypass mode active
    stale["less_waste_tools"] =           // T0 -> port 4 (not 1)
        json::array({4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});

    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(stale));

    // Macro confirmed missing → none of the stale plugin state should have
    // taken effect. active_tool stays at its default -1, external_mode stays
    // false, and the slot-tool mapping in SlotRegistry was never overwritten.
    CHECK(Ad5xIfsTestAccess::active_tool(backend) == -1);
    CHECK_FALSE(Ad5xIfsTestAccess::external_mode(backend));
    CHECK_FALSE(backend.is_bypass_active());

    // Now flip the latch as on_started() would when it sees the macro is
    // present, then replay — the SAME save_variables payload now applies
    // cleanly. This proves the gate, not some other guard, is what blocked
    // the read.
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(stale));
    CHECK(Ad5xIfsTestAccess::has_ifs_vars(backend));
    CHECK(Ad5xIfsTestAccess::active_tool(backend) == 2);
    CHECK(Ad5xIfsTestAccess::external_mode(backend));
    CHECK(backend.is_bypass_active());
}

// Regression: native-ZMOD users without lessWaste/bambufy who happened to
// have stale prefixed save_variables data could land in a state where
// has_ifs_vars_ was true (e.g. theory-2 from the Vger1700 Discord report:
// Klipper/Kalico return `{}` for non-existent objects, so key presence
// alone falsely satisfied the macro-existence check). When that happened,
// every Adventurer5M.json poll fired `_IFS_VARS colors=...` / `types=...`
// and Klipper rejected them with `// Unknown command:"_IFS_VARS"`. This
// asserts the self-heal path: as soon as Klipper rejects the command, we
// demote has_ifs_vars_ and latch the macro as missing.
TEST_CASE("AD5X IFS self-heals on Unknown command:\"_IFS_VARS\" response", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed the wrong-state: macro 'confirmed present' and has_ifs_vars_ true.
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    // Klipper's gcode.cmd_default emits this exact line via respond_info
    // when an unknown command is dispatched (klippy/gcode.py).
    const std::string rejection = "// Unknown command:\"_IFS_VARS\"";
    Ad5xIfsTestAccess::on_gcode_response_line(backend, rejection);

    CHECK_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    // Once latched missing, replaying save_variables can't re-enable it
    // (same contract as the existing stale-save_variables regression test).
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
    CHECK_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
}

// ==========================================================================
// 23. parse_adventurer_json (native ZMOD Adventurer5M.json)
// ==========================================================================

TEST_CASE("AD5X IFS parse_adventurer_json", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("standard 4-slot JSON with # prefixed hex colors") {
        std::string content = R"({
            "FFMInfo": {
                "channel": 2,
                "ffmColor0": "",
                "ffmColor1": "#FF0000",
                "ffmColor2": "#00FF00",
                "ffmColor3": "#0000FF",
                "ffmColor4": "#FFFFFF",
                "ffmType0": "?",
                "ffmType1": "PLA",
                "ffmType2": "PETG",
                "ffmType3": "ABS",
                "ffmType4": "TPU"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.color_rgb == 0xFF0000);
        REQUIRE(info0.material == "PLA");

        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.color_rgb == 0x00FF00);
        REQUIRE(info1.material == "PETG");

        auto info2 = backend.get_slot_info(2);
        REQUIRE(info2.color_rgb == 0x0000FF);
        REQUIRE(info2.material == "ABS");

        auto info3 = backend.get_slot_info(3);
        REQUIRE(info3.color_rgb == 0xFFFFFF);
        REQUIRE(info3.material == "TPU");
    }

    SECTION("lowercase hex is uppercased") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#ff8800",
                "ffmType1": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF8800);
        REQUIRE(info.material == "PLA");
    }

    SECTION("missing FFMInfo section is graceful no-op") {
        std::string content = R"({"OtherSection": {"key": "value"}})";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slots should remain at defaults
        auto info = backend.get_slot_info(0);
        REQUIRE(info.material.empty());
    }

    SECTION("partial slots — only 2 of 4 populated") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#AABBCC",
                "ffmType1": "PLA",
                "ffmColor3": "#112233",
                "ffmType3": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.color_rgb == 0xAABBCC);
        REQUIRE(info0.material == "PLA");

        // Slot 1 (port 2) not in JSON — stays at default
        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.material.empty());

        auto info2 = backend.get_slot_info(2);
        REQUIRE(info2.color_rgb == 0x112233);
        REQUIRE(info2.material == "PETG");
    }

    SECTION("# prefix stripping") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#ABCDEF",
                "ffmType1": "ABS"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xABCDEF);
    }

    SECTION("empty color string defaults to gray") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "",
                "ffmType1": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0x808080);
        REQUIRE(info.material == "PLA");
    }

    SECTION("invalid JSON is graceful no-op") {
        std::string content = "this is not json {{{";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slots should remain at defaults
        auto info = backend.get_slot_info(0);
        REQUIRE(info.material.empty());
    }

    SECTION("color without # prefix still works") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "FF8800",
                "ffmType1": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF8800);
        REQUIRE(info.material == "PETG");
    }
}

// ==========================================================================
// Regression: dirty flag prevents parse_adventurer_json from clobbering
// user edits (#716)
// ==========================================================================

TEST_CASE("AD5X IFS parse_adventurer_json skips dirty slots", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed slot 0 with initial JSON data
    std::string initial = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, initial);
    REQUIRE(backend.get_slot_info(0).color_rgb == 0xFF0000);
    REQUIRE(backend.get_slot_info(0).material == "PLA");

    // User edits slot 0 (persist=false to skip actual write)
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PETG";
    backend.set_slot_info(0, edit, false);
    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 0));

    // Simulate sensor-triggered JSON re-read with stale firmware data
    std::string stale = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, stale);

    // Dirty slot must NOT be overwritten
    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0x00FF00);
    REQUIRE(info.material == "PETG");
}

TEST_CASE("AD5X IFS parse_adventurer_json updates clean slots normally", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Edit slot 0, then clear dirty to simulate completed persist
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PETG";
    backend.set_slot_info(0, edit, false);
    Ad5xIfsTestAccess::set_dirty(backend, 0, false);
    REQUIRE_FALSE(Ad5xIfsTestAccess::dirty(backend, 0));

    // JSON re-read should overwrite clean slot
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#AABBCC",
            "ffmType1": "ABS"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xAABBCC);
    REQUIRE(info.material == "ABS");
}

TEST_CASE("AD5X IFS set_slot_info persist=false sets dirty flag", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    REQUIRE_FALSE(Ad5xIfsTestAccess::dirty(backend, 1));

    SlotInfo edit;
    edit.color_rgb = 0x112233;
    edit.material = "TPU";
    backend.set_slot_info(1, edit, false);

    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 1));
}

TEST_CASE("AD5X IFS dirty flag protects against both parse paths", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed via save_variables (lessWaste path)
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    // User edits slot 0. Use a material in the firmware whitelist so
    // normalize_material() leaves it alone — this test is about the dirty
    // flag, not normalization.
    SlotInfo edit;
    edit.color_rgb = 0xDEADBE;
    edit.material = "SILK";
    backend.set_slot_info(0, edit, false);
    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 0));

    // parse_save_variables must not overwrite dirty slot
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());
    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xDEADBE);
    REQUIRE(info.material == "SILK");

    // parse_adventurer_json must not overwrite dirty slot either
    std::string stale_json = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, stale_json);
    info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xDEADBE);
    REQUIRE(info.material == "SILK");
}

// ==========================================================================
// Native ZMOD: parse_adventurer_json does NOT own presence (GET_ZCOLOR does)
// ==========================================================================
//
// History (#716): parse_adventurer_json used to *infer* presence from the
// persisted ffmColorN field — "non-empty color means filament present". That
// inference is wrong on native ZMOD: zmod persists the colour/material
// assignment across unload/eject and never writes an empty colour, so a
// previously-emptied channel got resurrected to "present" on the next
// content-changed poll (external unload not reflected; an edit to one channel
// resurrecting another). Presence is now owned solely by GET_ZCOLOR
// (apply_zcolor_result) — the RS-485 silk sensor — and the JSON parse must not
// touch port_presence_. It still refreshes colors_/materials_ for clean slots.

TEST_CASE("AD5X IFS parse_adventurer_json does not own presence on native ZMOD",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // No per-port sensors — this is native ZMOD
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    SECTION("non-empty colour does NOT set presence — only GET_ZCOLOR can") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#161616",
                "ffmColor2": "#FFFFFF",
                "ffmColor3": "#D3C4A3",
                "ffmColor4": "#F72224",
                "ffmType1": "PLA+",
                "ffmType2": "PLA+",
                "ffmType3": "PLA+",
                "ffmType4": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Presence is untouched by the parse (default false). The colour/material
        // refresh still happened — but the slot is not "present" until GET_ZCOLOR
        // (the silk sensor) confirms it.
        for (int i = 0; i < 4; ++i) {
            REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, i));
            REQUIRE(backend.get_slot_info(i).status == SlotStatus::EMPTY);
        }
    }

    SECTION("colour/material refresh survives even though presence does not flip") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor2": "#FF0000",
                "ffmType2": "PLA"
            }
        })";
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Parse must not have set presence...
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));

        // ...but once GET_ZCOLOR marks the slot present, the colour/material the
        // parse refreshed is visible. (apply_zcolor_result with an empty HEX in
        // the slot keeps the JSON colour via the old-format / empty-hex guard, so
        // assert presence only here.)
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
        REQUIRE(backend.get_slot_info(1).is_present());
    }
}

// ==========================================================================
// Native ZMOD presence resurrection regression (the bug this branch fixes)
// ==========================================================================
//
// Repro: channel 0 is LOADED (GET_ZCOLOR), then physically unloaded so the next
// GET_ZCOLOR reports it ABSENT (presence cleared). zmod keeps ffmColor1 in
// Adventurer5M.json. A subsequent edit to a DIFFERENT channel triggers a
// content-changed JSON poll. Pre-fix, parse_adventurer_json saw the persisted
// non-empty ffmColor1 and resurrected channel 0 to present. Post-fix, the parse
// never touches presence, so channel 0 stays EMPTY.

TEST_CASE("AD5X IFS emptied channel is not resurrected by a JSON edit to another channel",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // 1. Establish channel 0 LOADED via GET_ZCOLOR (the silk sensor's truth).
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "161616"};
        r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "FFFFFF"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).is_present());

    // 2. Channel 0 physically unloaded — GET_ZCOLOR now reports it ABSENT.
    //    Channel 1 stays present.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "FFFFFF"};
        // slots[0] left empty → channel 0 absent
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);

    // 3. A content-changed JSON poll edits a DIFFERENT channel (channel 1's
    //    colour) while zmod still persists channel 0's stale non-empty colour.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#161616",
            "ffmType1": "PLA",
            "ffmColor2": "#00AAFF",
            "ffmType2": "PETG"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // 4. Channel 0 must STAY empty — the persisted non-empty ffmColor1 must not
    //    resurrect it. (Pre-fix this failed: parse inferred present from colour.)
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);
}

// ==========================================================================
// Pre-SILENT zmod fallback: JSON inference is the ONLY presence source when
// GET_ZCOLOR SILENT=1 is unsupported (latched false on a prompt-dialog reply)
// ==========================================================================
//
// The resurrection fix makes GET_ZCOLOR the sole presence authority — but that
// authority only exists on modern zmod. On pre-SILENT zmod, GET_ZCOLOR returns
// a prompt dialog, zcolor_silent_supported_ latches false, and schedule_zcolor_
// query()/query_zcolor_silent() no-op forever. Without the gated fallback below
// there would be NO presence source at all (every channel stuck EMPTY). So when
// SILENT is unsupported, parse_adventurer_json must resume the legacy inference.

TEST_CASE("AD5X IFS pre-SILENT zmod falls back to JSON presence inference", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#161616", "ffmType1": "PLA",
            "ffmColor2": "#FF0000", "ffmType2": "PETG"
        }
    })";

    SECTION("SILENT supported (modern zmod): parse does NOT set presence") {
        REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend)); // default
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
    }

    SECTION("SILENT unsupported (pre-SILENT zmod): parse infers presence") {
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);
        // Non-empty colours → present; this is the only presence source here.
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
        REQUIRE(backend.get_slot_info(0).is_present());
    }
}

// ==========================================================================
// Override-clear is driven by GET_ZCOLOR's present->absent transition (Change 2)
// ==========================================================================
//
// When a spool is removed, its brand/spool_name/spoolman_id override must be
// cleared so it doesn't haunt the now-empty slot or get re-applied to whatever
// loads next. That cleanup used to ride on parse_adventurer_json's presence
// inference; it now rides on apply_zcolor_result's present->absent transition.

TEST_CASE("AD5X IFS GET_ZCOLOR present->absent clears the slot override", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // Slot 0 present with a user override staged.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "161616"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyTerra Charcoal";
    ovr.spoolman_id = 42;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // GET_ZCOLOR now reports slot 0 ABSENT → present->absent transition must
    // clear the override.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        // slots[0] left empty → absent
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    // #1071: an emptied lane now KEEPS its Spoolman link (see the dedicated
    // keep-the-link test below) — only firmware-sourced presence/status drops.
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

// #1065: after an eject, the RS-485 silk sensor lags ~1s before it reads the
// just-retracted lane clear. The eject-follow-up IFS_STATUS frame can therefore
// still report the lane present and resurrect it — and the LAST-ejected lane has
// no later query to re-correct it, so it kept offering Unload. eject_lane() now
// stamps the eject and apply_zcolor_result ignores a false->true presence flip
// for that lane within the settling window; a true re-insert after the window is
// honored.
TEST_CASE("AD5X IFS eject-settling: a lagging Ports read does not resurrect the lane (#1065)",
          "[ams][ad5x_ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // All four lanes present (establish via the presence helper so status is set).
    for (int i = 0; i < 4; ++i) {
        Ad5xIfsTestAccess::set_port_presence(backend, i, true);
    }
    REQUIRE(backend.get_slot_info(0).is_present());

    // Eject lane 0 — optimistic clear + eject stamp.
    Ad5xIfsTestAccess::mark_ejected(backend, 0);
    REQUIRE_FALSE(backend.get_slot_info(0).is_present());

    // Follow-up query lands while the silk sensor is still settling: Ports still
    // reads lane 0 present. Within the window this must NOT resurrect the lane.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.ifs_chan = 0; // real IFS_STATUS frames carry Chan alongside Ports
        r.ifs_ports = std::array<bool, 4>{true, true, true, true};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);

    // After the settling window, a genuine re-insert (Ports present) is honored.
    Ad5xIfsTestAccess::expire_eject_window(backend, 0);
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.ifs_chan = 0;
        r.ifs_ports = std::array<bool, 4>{true, true, true, true};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(backend.get_slot_info(0).is_present());
}

// #1065 load-complete flash: FFMInfo.channel is sticky (only the ~5s JSON poll
// refreshes it, only an eject clears it), so right after a load into a DIFFERENT
// lane it still names the PREVIOUS lane. If that lane reads empty in the frame's
// Ports snapshot the pointer is stale and must not be adopted as seated — else
// the load-complete frame flashes the previous lane loaded and repaints its
// retained filament. Here FFMInfo.channel=4 is stale (lane 4 empty) while a load
// into lane 3 completes (Chan=3).
TEST_CASE("AD5X IFS stale FFMInfo.channel at an empty lane is not adopted as seated (#1065)",
          "[ams][ad5x_ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    Ad5xIfsTestAccess::set_ffm_channel(backend, 4); // sticky pointer at now-empty lane 4
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.ifs_chan = 3;                                             // real seated lane = 3
        r.ifs_ports = std::array<bool, 4>{true, true, true, false}; // lane 4 empty
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "111111"};
        r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "222222"};
        r.slots[2] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "333333"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }

    // Seated follows the present lane (Chan=3), NOT the stale FFMInfo.channel=4.
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 3);
    REQUIRE(Ad5xIfsTestAccess::current_slot(backend) == 2); // lane 3 == slot index 2
    REQUIRE_FALSE(backend.get_slot_info(3).is_present());   // lane 4 does not flash loaded
    REQUIRE(backend.get_slot_info(3).status == SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(2).is_present());
}

// #1071: AD5X must match the AFC / Happy Hare backends, which never clear the
// lane->Spoolman link when a lane goes empty (runout / unload / eject). The
// emptied lane renders as removed (presence=false, status != LOADED) but
// retains spoolman_id / brand / spool_name / color / material so a re-inserted
// same spool keeps its assignment. (Previously AD5X diverged by auto-calling
// clear_override_locked on the present->absent transition.)
TEST_CASE("AD5X IFS: a lane going empty keeps its Spoolman link (AFC/HH parity)",
          "[ams][ad5x][ifs][spoolman][1071]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // Slot 0 present with a user Spoolman override staged.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    helix::ams::FilamentSlotOverride ovr;
    ovr.spoolman_id = 42;
    ovr.brand = "Polymaker";
    ovr.spool_name = "Polymaker PLA";
    ovr.color_rgb = 0xFF0000;
    ovr.color_set = true;
    ovr.material = "PLA";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Lane 0 goes empty (eject / runout): GET_ZCOLOR reports slot 0 ABSENT.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        // slots[0] left empty => absent
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }

    // Presence flips false; the slot renders empty/removed...
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.status != SlotStatus::LOADED);
    // ...but the Spoolman link is RETAINED so a re-inserted same spool keeps it.
    CHECK(info.spoolman_id == 42);
    CHECK(info.brand == "Polymaker");
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

// #1065: a physical insert must refresh material/color from firmware truth on a
// lane whose override is auto-tracked (no Spoolman binding) even though the
// override's fields were user-locked (by a menu type-set, or by the pessimistic
// !material.empty() load default). A physical insert emits no CHANGE_ZCOLOR, so
// the #981 external-edit clear never fires — the insert edge itself must unlock.
TEST_CASE("AD5X IFS: physical insert refreshes material/color on an auto-tracked lane (#1065)",
          "[ams][ad5x][ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // Slot 0 loaded with PETG / red, establishing firmware baseline.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    // User set the type via the menu: material + color locked, NO Spoolman link.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PETG";
    ovr.user_locked_material = true;
    ovr.color_rgb = 0xFF0000;
    ovr.color_set = true;
    ovr.user_locked_color = true;
    ovr.spoolman_id = 0; // auto-tracked, not a deliberate binding
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Eject (lane goes empty).
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));

    // Insert a DIFFERENT spool: firmware now reports PLA / green.
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "00FF00"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    // The new spool's material AND color surface — not the stale locked PETG/red.
    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x00FF00);
    // The locks were dropped so the auto-mirror could refresh.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK_FALSE(staged->user_locked_material);
    CHECK_FALSE(staged->user_locked_color);
    CHECK(staged->material == "PLA");
}

// Counterpart to the above: a lane with a REAL Spoolman binding is a deliberate
// identity the user attached. #1071 retains it across an eject/insert cycle, so
// the insert edge must NOT unlock it — the bound spool's material/color stick.
TEST_CASE("AD5X IFS: physical insert does NOT unlock a Spoolman-bound lane (#1065/#1071)",
          "[ams][ad5x][ifs][1065][1071]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }

    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PETG";
    ovr.user_locked_material = true;
    ovr.color_rgb = 0xFF0000;
    ovr.color_set = true;
    ovr.user_locked_color = true;
    ovr.spoolman_id = 42; // deliberate binding
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Eject then insert a physically different spool (firmware reports PLA).
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "00FF00"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }

    // The binding — and the material/color it carries — is retained (#1071).
    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == "PETG");
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->user_locked_material);
    CHECK(staged->material == "PETG");
}

// #1065 Fix B: on modern ZMOD the firmware material can surface one parse frame
// BEFORE IFS_STATUS Ports flips the slot present. The type detector must HOLD
// the baseline while the slot reads not-present, so the delta survives until a
// present-lane frame and the sync fires — instead of advancing the baseline and
// swallowing the change (color updated on screen, material stuck).
TEST_CASE("AD5X IFS: type detector holds baseline through presence lag on insert (#1065)",
          "[ams][ad5x][ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Establish a present-lane baseline of PETG.
    REQUIRE_FALSE(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PETG",
                                                                /*slot_has_filament=*/true));
    REQUIRE(Ad5xIfsTestAccess::last_firmware_material(backend, 0) == "PETG");

    // Presence-lag frame: firmware says PLA but the slot still reads not-present.
    // No sync, and — crucially — the baseline is HELD at PETG, not advanced.
    CHECK_FALSE(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PLA",
                                                              /*slot_has_filament=*/false));
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0) == "PETG");

    // Presence catches up: the held PETG->PLA delta is still detectable, so the
    // sync fires (returns true). Without the hold, the baseline would already be
    // PLA and this would return false — the swallow.
    CHECK(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PLA",
                                                        /*slot_has_filament=*/true));
}

// #1065 Fix B, color counterpart: same presence-lag hold for the color detector.
TEST_CASE("AD5X IFS: color detector holds baseline through presence lag on insert (#1065)",
          "[ams][ad5x][ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF0000,
                                                                 /*slot_has_filament=*/true));
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF0000u);

    CHECK_FALSE(Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x00FF00,
                                                               /*slot_has_filament=*/false));
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF0000u); // baseline held

    CHECK(Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x00FF00,
                                                         /*slot_has_filament=*/true)); // now syncs
}

// #1065 Fix B must NOT regress the da7d0b1a6 eject-baseline behavior: an EMPTY
// material on an empty lane (eject) still advances the baseline to "" so the
// following insert is a genuine "" -> MATERIAL delta.
TEST_CASE("AD5X IFS: eject still baselines material to empty so insert re-detects (#1065)",
          "[ams][ad5x][ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PETG",
                                                                /*slot_has_filament=*/true));

    // Eject: empty material, slot empty, a color reading present (the #808080
    // placeholder). Baseline advances to "".
    CHECK_FALSE(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "",
                                                              /*slot_has_filament=*/false,
                                                              /*observed_color=*/0x808080u));
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0) == "");

    // Insert: "" -> PLA is a real delta on a present lane, so the sync fires.
    CHECK(Ad5xIfsTestAccess::check_external_type_change(backend, 0, "PLA",
                                                        /*slot_has_filament=*/true));
}

// NOTE: tests previously here exercised parse_save_variables's color/type
// reads from <prefix>_colors / <prefix>_types — including the dirty-flag
// round-trip and port_presence inference from color emptiness. Those code
// paths were removed when CHANGE_ZCOLOR / GET_ZCOLOR became the sole
// color/type source (lessWaste/bambufy save_variables don't reflect zmod's
// authoritative state). The remaining set_slot_info port_presence tests
// below cover the local-edit branch that still drives presence inference.

TEST_CASE("AD5X IFS set_slot_info updates port_presence", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Start with empty save_variables so no color data
    json vars = standard_variables();
    vars["less_waste_colors"] = json::array({"", "", "", ""});
    vars["less_waste_types"] = json::array({"", "", "", ""});
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));

    SECTION("setting color on empty slot latches port_presence") {
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        auto slot = backend.get_slot_info(0);
        REQUIRE(slot.status == SlotStatus::AVAILABLE);
    }

    SECTION("clearing slot resets port_presence") {
        // First assign filament
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

        // Now clear it
        SlotInfo cleared;
        cleared.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        cleared.material = "";
        backend.set_slot_info(0, cleared, false);

        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
        auto slot = backend.get_slot_info(0);
        REQUIRE(slot.status == SlotStatus::EMPTY);
    }

    SECTION("setting only material (default color) latches port_presence") {
        SlotInfo info;
        info.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        info.material = "PETG";
        backend.set_slot_info(0, info, false);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    }

    SECTION("set_slot_info skips presence for per-port sensor printers") {
        // Enable per-port sensors
        json notification;
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);
        REQUIRE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

        // set_slot_info should not alter port_presence (sensors are authoritative)
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);

        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    }
}

// ==========================================================================
// can_unload_from_toolhead — #995: active slot stays unloadable after runout
// ==========================================================================
//
// A filament runout clears the head sensor, dropping the display status of the
// firmware's active slot below LOADED. Unload is exactly the recovery the user
// needs at that moment, so the action gate is decoupled from the head-sensor-
// derived display status: the active slot stays unloadable while the spool
// still renders empty/available.

TEST_CASE("AD5X IFS can_unload_from_toolhead keeps active slot unloadable after runout",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Load slot 0: active tool T0 (→ port 1 → slot 0), port + head sensors set.
    {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);
    }
    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::LOADED);
    REQUIRE(backend.can_unload_from_toolhead(0));

    // Runout: head sensor clears and the lane sensor empties, but the firmware
    // still reports slot 0 as the active/current slot.
    {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", false}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);
    }
    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    // Display status is no longer LOADED — the spool renders empty/available...
    REQUIRE(backend.get_slot_info(0).status != SlotStatus::LOADED);
    // ...but the action gate keeps the active slot unloadable (#995).
    REQUIRE(backend.can_unload_from_toolhead(0));

    // An inactive, empty slot is NOT unloadable.
    REQUIRE_FALSE(backend.can_unload_from_toolhead(2));

    // Out-of-range indices are never unloadable. The negative case exercises the
    // slot_index >= 0 guard: current_slot is 0 here, so -1 must NOT match it via
    // the active-slot short-circuit — and the base LOADED check rejects it too.
    REQUIRE_FALSE(backend.can_unload_from_toolhead(-1));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(AmsBackendAd5xIfs::NUM_PORTS));
}

TEST_CASE("AD5X IFS can_unload_from_toolhead with no active slot is never unloadable",
          "[ams][ad5x_ifs]") {
    // With no filament loaded, current_slot is -1. The slot_index >= 0 guard must
    // prevent a caller passing -1 (or 0) from matching the -1 active-slot
    // sentinel; every slot then falls through to the base LOADED check, which is
    // false for an unloaded backend.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    auto vars = standard_variables();
    vars["less_waste_current_tool"] = -1;
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
    REQUIRE(backend.get_system_info().current_slot == -1);

    REQUIRE_FALSE(backend.can_unload_from_toolhead(0));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(-1));
}

TEST_CASE("AD5X IFS toolhead filament unloadable when firmware drops active slot (#995)",
          "[ams][ad5x_ifs]") {
    // Stock-ZMOD recovery scenario: after a runout or print-end the firmware can
    // emit "Extruder: None" and drop its active-slot pointer to current_slot==-1
    // while filament is STILL seated in the toolhead (head sensor true). The
    // active-slot short-circuit can never fire (nothing matches -1), so the only
    // signal that there is removable filament is the head sensor itself.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    // Native ZMOD: no per-port sensors.
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    // No active tool (current_slot == -1) but filament present at the head.
    {
        auto vars = standard_variables();
        vars["less_waste_current_tool"] = -1;
        json notification;
        notification["save_variables"] = json{{"variables", vars}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);
    }
    REQUIRE(backend.get_system_info().current_slot == -1);
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    // The gate must open even though no slot is the active slot — filament is
    // physically in the toolhead and must be removable. (The dispatched command,
    // the _IFS_REMOVE_CURRENT_PRUTOK macro, is asserted by the unload_filament
    // dispatch test; the firmware reads FFMInfo.channel, not our slot index, so
    // the unknown origin lane is fine.)
    REQUIRE(backend.can_unload_from_toolhead(0));
}

TEST_CASE("AD5X IFS no toolhead filament leaves slot non-unloadable (#995 regression)",
          "[ams][ad5x_ifs]") {
    // The mirror of the recovery case: no filament anywhere (head sensor false,
    // current_slot == -1) must keep Unload disabled. The head_filament gate must
    // not open when there is nothing seated in the toolhead.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    {
        auto vars = standard_variables();
        vars["less_waste_current_tool"] = -1;
        json notification;
        notification["save_variables"] = json{{"variables", vars}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);
    }
    REQUIRE(backend.get_system_info().current_slot == -1);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    REQUIRE_FALSE(backend.can_unload_from_toolhead(0));
}

TEST_CASE("AD5X IFS runout does not flip active slot display status to LOADED", "[ams][ad5x_ifs]") {
    // Regression guard: decoupling the unload gate must NOT alter display status.
    // Native ZMOD path (no per-port sensors), active slot, head sensor clear —
    // the slot must still report a non-LOADED status so the spool renders
    // empty/available.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    seed_standard_colors(backend);

    // Motion sensor present briefly to establish active slot via head detection,
    // then cleared to simulate the runout.
    {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_motion_sensor ifs_motion_sensor"] =
            json{{"filament_detected", false}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);
    }

    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    REQUIRE(backend.get_slot_info(0).status != SlotStatus::LOADED);
    // Gate stays open for the active slot.
    REQUIRE(backend.can_unload_from_toolhead(0));
}

// ==========================================================================
// parse_zcolor_silent — GET_ZCOLOR SILENT=1 response parser
// ==========================================================================
//
// zmod emits one line per LOADED slot plus a summary line, all prefixed "// ":
//
//   // Extruder: None (1) | IFS: True
//   // 1: PLA/FFFFFF
//   // 2: PLA/2750E0
//
// Post-ad2802ab zmod always appends "/<HEX>" to each slot line. Hex is the
// RIGHTMOST /-segment — transparent/named-color case emits three segments
// (// 3: PLA/transparent/00000000). Missing slot numbers = physically empty.
// Old zmod (pre-fix) emits "// 1: PLA" (no /HEX); we fall back to JSON.
// Very old zmod emits an action:prompt_show dialog; also JSON fallback.

TEST_CASE("AD5X IFS parse_zcolor_silent two-segment lines", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA/FFFFFF",
        "// 2: PETG/2750E0",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.current_channel == 1);
    REQUIRE_FALSE(r.extruder_slot.has_value());
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->material == "PLA");
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->material == "PETG");
    REQUIRE(r.slots[1]->hex == "2750E0");
    REQUIRE_FALSE(r.slots[2].has_value());
    REQUIRE_FALSE(r.slots[3].has_value());
}

TEST_CASE("AD5X IFS parse_zcolor_silent named-color three-segment", "[ams][ad5x_ifs]") {
    // Transparent / any COLOR_MAPPING match produces an extra segment:
    //   // <N>: <MATERIAL>/<NAME>/<HEX>
    // Parser rule: hex is always the rightmost /-segment.
    std::vector<std::string> lines = {
        "// Extruder: 1: PLA/FFFFFF | IFS: True",
        "// 1: PLA/FFFFFF",
        "// 3: PLA/transparent/00000000",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.extruder_slot == 0); // 0-based (line says slot 1)
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[2].has_value());
    REQUIRE(r.slots[2]->material == "PLA");
    REQUIRE(r.slots[2]->hex == "00000000");
}

TEST_CASE("AD5X IFS parse_zcolor_silent empty (all slots unloaded)", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (0) | IFS: True",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.current_channel == 0);
    for (int i = 0; i < AmsBackendAd5xIfs::NUM_PORTS; ++i) {
        REQUIRE_FALSE(r.slots[static_cast<size_t>(i)].has_value());
    }
}

TEST_CASE("AD5X IFS parse_zcolor_silent IFS disabled (independent mode)", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (0) | IFS: False",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.ifs_active);
}

TEST_CASE("AD5X IFS parse_zcolor_silent old-format (no /HEX)", "[ams][ad5x_ifs]") {
    // Pre-ad2802ab zmod: silent lines are "// N: MATERIAL" with no /HEX.
    // Parser must detect this and flag is_old_format so caller falls back to JSON.
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA",
        "// 2: PETG",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE(r.is_old_format);
    // Presence info is still valid even without color — slot 1 and 2 appear.
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->material == "PLA");
    REQUIRE(r.slots[0]->hex.empty());
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->material == "PETG");
    REQUIRE(r.slots[1]->hex.empty());
}

TEST_CASE("AD5X IFS parse_zcolor_silent prompt fallback", "[ams][ad5x_ifs]") {
    // Very old zmod: SILENT=1 unsupported, emits full dialog.
    std::vector<std::string> lines = {
        "// action:prompt_begin Select filament",
        "// action:prompt_text Extruder: None",
        "// action:prompt_button 1: PLA|RUN_ZCOLOR SLOT=1 HEX=FFFFFF TYPE=PLA|primary|FFFFFF",
        "// action:prompt_show",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE(r.is_prompt_fallback);
}

TEST_CASE("AD5X IFS parse_zcolor_silent malformed lines skipped", "[ams][ad5x_ifs]") {
    // Unrelated gcode-response lines interleaved with valid silent output must
    // not confuse the parser — it should pick out the slot lines it recognises.
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA/FFFFFF",
        "// random gcode echo",
        "// 99: nonsense", // slot number out of range
        "// 2: PETG/00FF00",
        "echo: hotend temp 205",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->hex == "00FF00");
    REQUIRE_FALSE(r.slots[2].has_value());
    REQUIRE_FALSE(r.slots[3].has_value());
}

// ==========================================================================
// IFS_STATUS Chan as seated-channel authority (2026-06-15 plan)
// ==========================================================================
//
// zmod's IFS_STATUS macro emits one clean-JSON line via respond_info carrying
// the current engaged port in "Chan" (1-based, 0 = none). Unlike the
// GET_ZCOLOR "Extruder:" feed line — which reads "None (N)" while loaded-idle —
// Chan persists at the seated port. parse_zcolor_silent must extract it into
// ifs_chan, and apply_zcolor_result must derive active_tool_/current_slot from
// it even on prompt-fallback (old zmod where GET_ZCOLOR degrades to a dialog).

TEST_CASE("AD5X IFS parse_zcolor_silent extracts IFS_STATUS Chan", "[ams][ad5x_ifs]") {
    // Firmware-accurate IFS_STATUS line: a single `// `-prefixed JSON object.
    // Chan=4 means port 4 is the seated/engaged channel.
    std::vector<std::string> lines = {
        R"(// {"RawData": "...", "State": 4, "Ports": [false, true, true, true], "Silk": 14, )"
        R"("Chan": 4, "Insert": 0, "NeedInsert": false, "Stall": false, "stall_state": 0})",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE(r.saw_valid_response);
    REQUIRE(r.ifs_chan.has_value());
    REQUIRE(*r.ifs_chan == 4);
}

TEST_CASE("AD5X IFS IFS_STATUS Chan is seated authority over stale Extruder: None",
          "[ams][ad5x_ifs]") {
    // The field defect: GET_ZCOLOR says "Extruder: None (4)" (idle-extruder view)
    // while port 4 is physically seated. IFS_STATUS Chan=4 must win, so the active
    // slot is 3 (port 4) — making Unload available ONLY on slot 3 and Eject on the
    // rest, not Unload-everywhere via the current_slot<0 recovery branch.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Identity tool map so find_first_tool_for_port(N) -> tool N-1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Filament physically seated at the head (RS-485 reports true).
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // GET_ZCOLOR reports "Extruder: None (4)" (no active feed) but IFS_STATUS
    // Chan=4 is the seated channel.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.current_channel = 4; // stale "(4)" — never used
    // extruder_slot deliberately absent (the "None" feed view).
    r.ifs_chan = 4; // 1-based seated channel from IFS_STATUS

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // current_slot recomputed immediately to port 4 -> slot 3.
    REQUIRE(backend.get_system_info().current_slot == 3);
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 3);

    // Unload is offered ONLY on the seated slot; the rest fall through to the
    // base capability (false), so the UI shows Eject on them instead.
    REQUIRE(backend.can_unload_from_toolhead(3));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(0));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(1));
    REQUIRE_FALSE(backend.can_unload_from_toolhead(2));
}

TEST_CASE("AD5X IFS cold-lane eject does not pollute seated channel (#1065 Bug 3)",
          "[ams][ad5x_ifs]") {
    // Field repro (raza616/mkleersn bundle CGR6C7PA + state table): a lane is
    // loaded to the toolhead, then the user ejects a DIFFERENT, cold lane. The
    // firmware's switching mechanism engages the ejected port, so the follow-up
    // IFS_STATUS reports that port as "Chan" even though it is now EMPTY (its
    // Ports[] bit drops to false). The xlsx captured exactly this: after ejecting
    // channel 1, `Chan: 1` with `Ports: [false, true, true, true]`. Trusting that
    // stale Chan reclassified the genuinely-seated lane as a cold lane, so the
    // loaded lane offered Eject and a tap would cold-grind seated filament.
    //
    // The seated channel must only follow a Chan whose lane actually has filament.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Identity tool map so find_first_tool_for_port(N) -> tool N-1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Channel 2 seated at the toolhead, all four lanes present.
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);

    // Baseline: ch2 (slot 1) is seated -> Unload; ch1 (slot 0) is cold -> Eject.
    REQUIRE(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    REQUIRE_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));

    // Eject cold lane 1. Its post-eject IFS_STATUS: Chan=1 (last-engaged channel)
    // but port 1 now empty. seated_chan_ must NOT move to channel 1.
    AmsBackendAd5xIfs::ZColorSilentResult after_eject;
    after_eject.saw_valid_response = true;
    after_eject.ifs_active = true;
    after_eject.ifs_chan = 1; // stale: engaged-but-now-empty lane
    after_eject.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{false, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, after_eject);

    // Channel 2 is still the seated lane -> must still route to the heated
    // toolhead Unload, NOT a cold Eject. Channel 1, now empty, stays cold-eject.
    CHECK(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
}

TEST_CASE("AD5X IFS dialog slot-select does not steal the seated channel (#1065 Bug 3)",
          "[ams][ad5x_ifs][1065]") {
    // Field repro (mkleersn v0.99.87 bundle ZT8Y9WPM + 07-07 state table): channel
    // 2 is physically seated at the toolhead (Adventurer5M.json FFMInfo.channel=2).
    // The user opens the zmod COLOR menu and merely SELECTS a different lane to edit
    // its colour/type (RUN_ZCOLOR SLOT=3 / CHANGE_ZCOLOR SLOT=3) — no filament
    // moves. IFS_STATUS then reports "Chan": 3 (the switching mechanism tracks the
    // last lane the dialog referenced) while all four ports stay PRESENT, so the
    // eject-stale guard (which only rejects a Chan pointing at an EMPTY lane) does
    // not fire. The bundle log caught HelixScreen adopting it: current_slot went
    // 1 -> 2 (channel 2 -> channel 3) with zero physical motion, so the
    // genuinely-seated lane 2 offered Eject and lane 3 wrongly offered Unload.
    //
    // The seated authority is FFMInfo.channel (which stayed 2 the whole time), NOT
    // IFS_STATUS Chan. A Chan that diverges from a known FFMInfo.channel must not
    // move the seated lane. (The GET_ZCOLOR "// Extruder:" line reads "None (3)"
    // while loaded-idle on this firmware — its paren also chases the dialog — so it
    // is NOT the authority; verified against the raw bundle.)
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Identity tool map so find_first_tool_for_port(N) -> tool N-1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    Ad5xIfsTestAccess::set_head_filament(backend, true);
    // Firmware's own seated record: channel 2 (as parse_adventurer_json would set
    // from Adventurer5M.json FFMInfo.channel).
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);

    // Channel 2 seated: IFS_STATUS Chan=2 agrees with FFMInfo.channel, all present.
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);

    // Baseline: ch2 (slot 1) seated -> Unload; ch3 (slot 2) cold -> Eject.
    REQUIRE(backend.get_system_info().current_slot == 1);
    REQUIRE(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    REQUIRE_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));

    // Dialog selects slot 3 to edit. FFMInfo.channel UNCHANGED (still 2 — no
    // filament moved), but IFS_STATUS now reports Chan=3 with lane 3 still present.
    AmsBackendAd5xIfs::ZColorSilentResult dialog_select;
    dialog_select.saw_valid_response = true;
    dialog_select.ifs_active = true;
    dialog_select.ifs_chan = 3; // dialog-tracked, NOT a physical seating
    dialog_select.ifs_ports =
        std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, dialog_select);

    // Seated lane must still be channel 2. Lane 2 offers Unload; lane 3 stays cold.
    CHECK(backend.get_system_info().current_slot == 1);
    CHECK(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
}

TEST_CASE("AD5X IFS seated channel follows a moved FFMInfo.channel despite a stale Chan "
          "(#1065 Bug 3)",
          "[ams][ad5x_ifs][1065]") {
    // Counterpart to the dialog-select test: the FFMInfo.channel authority must NOT
    // freeze the seated lane. When the firmware genuinely moves filament to a new
    // lane, FFMInfo.channel updates, and the seated lane must follow it — even if
    // IFS_STATUS Chan still carries a stale value from an earlier dialog reference
    // (mkleersn 07-07: loading a new channel correctly moved current_slot). This is
    // what proves the fix tracks real head changes rather than pinning to the
    // first-seen lane.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Channel 2 seated (FFMInfo.channel=2, Chan 2).
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(backend.get_system_info().current_slot == 1);

    // Real load of lane 1: firmware moves FFMInfo.channel to 1. Chan still reads a
    // stale 3 from an earlier dialog touch. Seated must follow FFMInfo.channel to
    // channel 1, NOT the stale Chan.
    Ad5xIfsTestAccess::set_ffm_channel(backend, 1);
    AmsBackendAd5xIfs::ZColorSilentResult moved;
    moved.saw_valid_response = true;
    moved.ifs_active = true;
    moved.ifs_chan = 3; // stale dialog value, must be ignored
    moved.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, moved);

    // Seated followed FFMInfo.channel to channel 1 (slot 0).
    CHECK(backend.get_system_info().current_slot == 0);
    CHECK(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
}

TEST_CASE("AD5X IFS parse_adventurer_json reads FFMInfo.channel as the seated lane (#1065 Bug 3)",
          "[ams][ad5x_ifs][1065]") {
    // The seated authority must actually be wired from the file. parse_adventurer_json
    // must read FFMInfo.channel and set the seated lane (recomputing current_slot),
    // so a plain file poll — with no IFS_STATUS frame at all — establishes which lane
    // is seated. FFMInfo.channel is 1-based (channel 2 -> slot 1).
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Minimal Adventurer5M.json: FFMInfo with the seated channel + per-port
    // colour/type (firmware writes channel 2 as seated).
    const std::string json =
        R"({"FFMInfo":{"channel":2,)"
        R"("ffmColor1":"#161616","ffmColor2":"#FFFFFF",)"
        R"("ffmColor3":"#F72224","ffmColor4":"#898989",)"
        R"("ffmType1":"PETG","ffmType2":"PETG","ffmType3":"PETG","ffmType4":"PETG"}})";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, json);

    CHECK(Ad5xIfsTestAccess::ffm_channel(backend) == 2);
    CHECK(backend.get_system_info().current_slot == 1); // channel 2 -> 0-based slot 1
    CHECK(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
}

// ==========================================================================
// #1065 "row 28": sticky FFMInfo.channel + head-switch gate on the seated lane
//
// The firmware never blanks FFMInfo.channel / IFS_STATUS Chan on eject (same
// stickiness as ffmColor/ffmType). After ejecting a cold lane whose stale
// FFMInfo.channel still points at it, an un-gated adoption re-seats that empty
// lane, so it keeps offering Unload ("shows loaded"). The fix gates adoption on
// the toolhead SWITCH sensor: adopt only when it corroborates filament at the
// head; clear when it authoritatively reads empty. The switch (not the
// loaded-idle-false-negating motion sensor) is the authority, so a genuinely
// seated lane is never dropped, and motion-only firmware falls back to the old
// behaviour.
// ==========================================================================

TEST_CASE("AD5X IFS stale FFMInfo.channel is dropped when the head switch reads empty "
          "(#1065 row 28)",
          "[ams][ad5x_ifs][1065]") {
    // Post-eject re-poll: a cold lane 3 was ejected but FFMInfo.channel is still 3
    // (firmware kept it) and the toolhead switch reads EMPTY (nothing seated). The
    // follow-up IFS_STATUS carries the sticky Chan=3 with port 3 now absent.
    // Pre-fix, current_slot moved to 2 and lane 3 offered Unload ("shows loaded").
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Toolhead switch authoritatively reads empty (drives head_switch_seen_ +
    // head_switch_present_=false + head_filament_=false).
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::head_switch_seen(backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_switch_present(backend));

    // Firmware's sticky record still points at lane 3.
    Ad5xIfsTestAccess::set_ffm_channel(backend, 3);

    // Post-eject IFS_STATUS: Chan=3 (last-engaged, sticky), port 3 empty.
    AmsBackendAd5xIfs::ZColorSilentResult after_eject;
    after_eject.saw_valid_response = true;
    after_eject.ifs_active = true;
    after_eject.ifs_chan = 3;
    after_eject.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, false, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, after_eject);

    // No lane is seated: the empty head switch rejects the stale channel.
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    CHECK(backend.get_system_info().current_slot == -1);
    CHECK_FALSE(backend.can_unload_from_toolhead(2));
}

TEST_CASE("AD5X IFS FFMInfo.channel poll is not adopted while the head switch is empty "
          "(#1065 row 28)",
          "[ams][ad5x_ifs][1065]") {
    // Same gate on the plain file-poll path (parse_adventurer_json): a stale
    // FFMInfo.channel=3 with the toolhead switch empty must not seat lane 3.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::head_switch_seen(backend));

    const std::string json =
        R"({"FFMInfo":{"channel":3,)"
        R"("ffmColor1":"#161616","ffmColor2":"#FFFFFF",)"
        R"("ffmColor3":"#F72224","ffmColor4":"#898989",)"
        R"("ffmType1":"PETG","ffmType2":"PETG","ffmType3":"PETG","ffmType4":"PETG"}})";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, json);

    // ffm_channel_ reflects the file (3), but it is NOT adopted as seated.
    CHECK(Ad5xIfsTestAccess::ffm_channel(backend) == 3);
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    CHECK(backend.get_system_info().current_slot == -1);
    CHECK_FALSE(backend.can_unload_from_toolhead(2));
}

TEST_CASE("AD5X IFS loaded-idle lane stays seated when the head switch is present but motion "
          "reads empty (#1065 row 28)",
          "[ams][ad5x_ifs][1065]") {
    // Motion-false-negative guard — MUST pass before AND after the fix. A lane is
    // genuinely loaded (head SWITCH present=true), but the ifs_motion_sensor reads
    // filament_detected=false while idle (device-confirmed). That motion frame
    // clobbers the conflated head_filament_ to false — but the switch authority
    // still says present, so the head-gate must NOT drop the seated lane.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Lane 2 genuinely seated: switch reports present.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(backend.get_system_info().current_slot == 1);

    // Idle motion frame reads empty and clobbers head_filament_ to false — but the
    // switch's own last reading (present) is untouched.
    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend)); // motion clobbered it
    REQUIRE(Ad5xIfsTestAccess::head_switch_present(backend)); // switch still present

    // A follow-up IFS_STATUS poll while motion says empty must keep lane 2 seated.
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 2);
    CHECK(backend.get_system_info().current_slot == 1);
    CHECK(backend.can_unload_from_toolhead(1));
}

TEST_CASE("AD5X IFS motion-only firmware (no switch) still adopts FFMInfo.channel (#1065 row 28)",
          "[ams][ad5x_ifs][1065]") {
    // On firmware that publishes ONLY the motion sensor, head_switch_seen_ never
    // latches, so the head-gate cannot fire. A loaded lane whose motion sensor
    // false-negates while idle must remain seated — the fix must not regress
    // switch-less firmwares.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Only the motion sensor ever reports (reads empty while idle). No switch.
    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_switch_seen(backend));

    Ad5xIfsTestAccess::set_ffm_channel(backend, 3);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 3;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);

    // Adopted as before (no switch authority to gate on).
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 3);
    CHECK(backend.get_system_info().current_slot == 2);
    CHECK(backend.can_unload_from_toolhead(2));
}

TEST_CASE("AD5X IFS eject clears a stale seated pointer at the ejected lane (#1065 row 28)",
          "[ams][ad5x_ifs][1065]") {
    // Belt-and-suspenders: when a stale seated pointer (seated_chan_ / ffm_channel_)
    // targets the just-ejected lane, eject must zero BOTH so the Unload affordance
    // dies at once instead of persisting until the next head-gated poll.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Seat lane 2 (channel 2) via a corroborated poll.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    Ad5xIfsTestAccess::set_ffm_channel(backend, 2);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 2;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 2);
    REQUIRE(Ad5xIfsTestAccess::ffm_channel(backend) == 2);
    REQUIRE(backend.get_system_info().current_slot == 1);

    // Ejecting a DIFFERENT lane (0) leaves the seated pointer untouched.
    CHECK_FALSE(Ad5xIfsTestAccess::clear_seated_if_ejected(backend, 0));
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 2);
    CHECK(backend.get_system_info().current_slot == 1);

    // Ejecting the FFMInfo-pointed lane (1 -> channel 2) zeroes both and recomputes.
    CHECK(Ad5xIfsTestAccess::clear_seated_if_ejected(backend, 1));
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    CHECK(Ad5xIfsTestAccess::ffm_channel(backend) == 0);
    CHECK(backend.get_system_info().current_slot == -1);
}

// ==========================================================================
// BUG-B (#1065): native Z-Mod head-loaded state
//
// On native Z-Mod (v0.99.84) the head switch / motion sensors never push under
// the stock filament_*_sensor sections, so head_filament_ was never written and
// the seated lane reported AVAILABLE instead of LOADED. Two fixes:
//   PART A — derive head-loaded from GET_ZCOLOR's "Extruder:" summary.
//   PART B — also subscribe to the zmod_ifs_*_sensor namespaces.
// ==========================================================================

TEST_CASE("AD5X IFS native Z-Mod derives head-loaded from GET_ZCOLOR Extruder summary "
          "(BUG-B Part A)",
          "[ams][ad5x][ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Device-confirmed loaded frame: GET_ZCOLOR "// Extruder: 1: PLA/white | IFS:
    // True" (extruder_slot 0, 0-based) riding the same response as IFS_STATUS
    // {"Chan":1,"Ports":[true,false,false,false]}. head_filament_ is NOT seeded —
    // the backend must derive it from the Extruder summary.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.saw_silent_content = true;
    loaded.saw_extruder_summary = true; // "// Extruder: 1" line present
    loaded.ifs_active = true;
    loaded.extruder_slot = 0; // "Extruder: 1" -> 0-based slot 0
    loaded.ifs_chan = 1;      // 1-based seated channel
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);

    CHECK(Ad5xIfsTestAccess::head_filament(backend));
    CHECK(backend.get_system_info().filament_loaded);
    CHECK(backend.is_filament_loaded());
    CHECK(backend.get_slot_info(0).status == SlotStatus::LOADED);

    // Genuine unload/eject: GET_ZCOLOR "// Extruder: None | IFS: True" AND the
    // seated lane's port has gone absent (IFS_STATUS Ports all false). Physical
    // presence corroborates an empty head, so the derivation clears head-loaded.
    // (The Extruder:None-WHILE-still-present case is covered by the C1/#995 test
    // below, where head-loaded is deliberately RETAINED.)
    AmsBackendAd5xIfs::ZColorSilentResult none;
    none.saw_valid_response = true;
    none.saw_silent_content = true;
    none.saw_extruder_summary = true; // summary present, but reads "None"
    none.ifs_active = true;
    // none.extruder_slot deliberately absent ("None")
    none.ifs_chan = 1;
    none.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{false, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, none);

    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    CHECK_FALSE(backend.get_system_info().filament_loaded);
    CHECK_FALSE(backend.is_filament_loaded());
    CHECK(backend.get_slot_info(0).status != SlotStatus::LOADED);
}

TEST_CASE("AD5X IFS Extruder:None does NOT clear head-loaded while the seated lane is present "
          "(C1 / #995 post-runout-while-seated)",
          "[ams][ad5x][ifs][1065]") {
    // Some AD5X firmware drops the GET_ZCOLOR extruder pointer to "Extruder: None"
    // after a runout/print-end while filament is STILL physically seated at the
    // toolhead. Clearing head-loaded then would strand the filament (Unload
    // disappears — can_unload_from_toolhead's `head_filament_ && current_slot<0`
    // fallback can't fire) and wrongly re-enable Load (-> cold grind). The
    // derivation must RETAIN head-loaded as long as the seated lane reads present.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());

    // Establish loaded: Extruder: 1, lane 1 present + seated.
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.saw_silent_content = true;
    loaded.saw_extruder_summary = true;
    loaded.ifs_active = true;
    loaded.extruder_slot = 0;
    loaded.ifs_chan = 1;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    // Firmware quirk: Extruder: None, but lane 1 is STILL present (Ports[0] true).
    AmsBackendAd5xIfs::ZColorSilentResult none_seated;
    none_seated.saw_valid_response = true;
    none_seated.saw_silent_content = true;
    none_seated.saw_extruder_summary = true; // summary present, reads "None"
    none_seated.ifs_active = true;
    // extruder_slot deliberately absent ("None")
    none_seated.ifs_chan = 1;
    none_seated.ifs_ports =
        std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, none_seated);

    // Head-loaded MUST be retained — the seated lane is still present.
    CHECK(Ad5xIfsTestAccess::head_filament(backend));
    CHECK(backend.get_system_info().filament_loaded);
    CHECK(backend.can_unload_from_toolhead(0));
}

TEST_CASE("AD5X IFS prefers the head sensor namespace that carries filament_detected "
          "(I1 / #1065 boot scenario)",
          "[ams][ad5x][ifs][1065]") {
    // On a fresh boot Moonraker can return the stock filament_switch_sensor object
    // as an empty {} compat view (no filament_detected) while the live
    // zmod_ifs_switch_sensor carries the real reading. Selecting the head sensor
    // key purely by presence would let the empty stock object win and the real
    // zmod reading be ignored, leaving head-loaded false with filament at the head.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    json frame;
    frame["filament_switch_sensor head_switch_sensor"] = json::object(); // empty compat view
    frame["zmod_ifs_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};
    Ad5xIfsTestAccess::handle_status(backend, frame);

    CHECK(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE("AD5X IFS can_load gate: cold-seated stays loadable, truly-loaded does not, AFC "
          "keeps the loaded-hint (I3 / BUG-A)",
          "[ams][ad5x][ifs][1065]") {
    // The context menu computes can_load = !system_busy && !toolhead_unload &&
    // slot_has_filament, where toolhead_unload = slot_unloads_to_toolhead(slot,
    // is_loaded). BUG-A swapped the gate from the permissive is_loaded to this
    // backend-aware signal so a cold-seated AD5X lane (present but NOT at the
    // head) still offers Load. Assert the backend semantics the gate rests on.
    SECTION("AD5X cold-seated lane is not a toolhead unload (Load stays enabled)") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        REQUIRE(backend.set_tool_mapping(0, 0).success());
        // Lane 1 present + seated, but nothing at the head (Extruder: None).
        AmsBackendAd5xIfs::ZColorSilentResult cold;
        cold.saw_valid_response = true;
        cold.saw_silent_content = true;
        cold.saw_extruder_summary = true; // "Extruder: None"
        cold.ifs_active = true;
        cold.ifs_chan = 1;
        cold.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, cold);
        REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
        // slot_unloads_to_toolhead false -> !toolhead_unload true -> Load enabled.
        CHECK_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    }
    SECTION("AD5X truly-loaded lane is a toolhead unload (Load disabled)") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        REQUIRE(backend.set_tool_mapping(0, 0).success());
        AmsBackendAd5xIfs::ZColorSilentResult loaded;
        loaded.saw_valid_response = true;
        loaded.saw_silent_content = true;
        loaded.saw_extruder_summary = true;
        loaded.ifs_active = true;
        loaded.extruder_slot = 0; // "Extruder: 1" -> head loaded
        loaded.ifs_chan = 1;
        loaded.ifs_ports =
            std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
        CHECK(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    }
    SECTION("AFC backend returns the loaded-hint unchanged (no can_load regression)") {
        AmsBackendAfc afc(nullptr, nullptr);
        // For non-AD5X backends !toolhead_unload == !is_loaded, so the gate is
        // exactly the pre-BUG-A behavior.
        CHECK(afc.slot_unloads_to_toolhead(0, /*loaded_hint=*/true) == true);
        CHECK(afc.slot_unloads_to_toolhead(0, /*loaded_hint=*/false) == false);
    }
}

TEST_CASE("AD5X IFS Extruder-summary head derivation ignores frames without the summary line "
          "(BUG-B Part A guard)",
          "[ams][ad5x][ifs][1065]") {
    // A frame that carries only IFS_STATUS JSON (no "// Extruder:" line) has
    // extruder_slot absent for an unrelated reason — it must NOT clear a
    // previously-established head-loaded state. saw_extruder_summary gates this.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());

    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.saw_silent_content = true;
    loaded.saw_extruder_summary = true;
    loaded.ifs_active = true;
    loaded.extruder_slot = 0;
    loaded.ifs_chan = 1;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    // IFS_STATUS-only follow-up: no Extruder summary, extruder_slot absent. Head
    // state must be left untouched (still loaded).
    AmsBackendAd5xIfs::ZColorSilentResult status_only;
    status_only.saw_valid_response = true;
    status_only.saw_silent_content = false;
    status_only.saw_extruder_summary = false; // no "// Extruder:" line this frame
    status_only.ifs_chan = 1;
    status_only.ifs_ports =
        std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, status_only);

    CHECK(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE("AD5X IFS routes native Z-Mod head switch sensor namespace to head_filament_ "
          "(BUG-B Part B)",
          "[ams][ad5x][ifs][1065]") {
    // The live AD5X publishes its head switch sensor as
    // "zmod_ifs_switch_sensor head_switch_sensor", not the stock
    // "filament_switch_sensor head_switch_sensor". handle_status_update must route
    // the zmod namespace to parse_head_sensor().
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    Ad5xIfsTestAccess::handle_status(backend, make_zmod_head_sensor(true));
    CHECK(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_zmod_head_sensor(false));
    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE("AD5X IFS routes native Z-Mod motion sensor namespace to head_filament_ (BUG-B Part B)",
          "[ams][ad5x][ifs][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    Ad5xIfsTestAccess::handle_status(backend, make_zmod_motion_sensor(true));
    CHECK(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_zmod_motion_sensor(false));
    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE("AD5X IFS toolhead-unload predicate stays false on a cold-seated lane (BUG-A #1065)",
          "[ams][ad5x][ifs][1065]") {
    // BUG-A: the context-menu Load gate used the recovery-broadened is_loaded
    // (can_unload_from_toolhead, true for the seated channel regardless of head
    // state), greying out Load on a lane whose filament sits in the lane but never
    // reached the toolhead. The real toolhead-loaded signal is
    // slot_unloads_to_toolhead(), which returns false when the head is empty — so
    // the corrected gate (!toolhead_unload) keeps Load enabled. This pins the
    // backend predicate the gate reads; the UI gate itself lives in
    // ui_ams_context_menu.cpp and is verified by the program build.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());

    // Lane 1 present + seated (Chan=1, Ports[0]=true) but NOTHING at the head
    // (GET_ZCOLOR "Extruder: None"); saw_extruder_summary resolves head to false.
    AmsBackendAd5xIfs::ZColorSilentResult cold;
    cold.saw_valid_response = true;
    cold.saw_silent_content = true;
    cold.saw_extruder_summary = true;
    cold.ifs_active = true;
    // extruder_slot absent: head empty
    cold.ifs_chan = 1;
    cold.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, cold);

    // Backend truth: filament present in the lane, head empty.
    CHECK(backend.get_slot_info(0).is_present());
    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    // The ACTUAL toolhead-unload predicate is false because the head is empty,
    // even when the recovery-broadened loaded_hint is passed in — so the UI Load
    // gate (!toolhead_unload) keeps the cold-seated lane a valid Load target.
    CHECK_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
}

TEST_CASE(
    "AD5X IFS restores persisted seated lane after power-cycle Chan=0 (#1065 power-cycle floor)",
    "[ams][ad5x_ifs]") {
    // The firmware forgets the seated channel across a power cycle: IFS_STATUS
    // comes back Chan=0 even with a lane physically at the head (bundle CGR6C7PA).
    // With no seated lane and head loaded, every lane wrongly labels as Unloadable.
    // We remember the last loaded lane and restore it provisionally on cold boot.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    // Last session had channel 2 (slot 1) loaded — restored from the lane_data DB.
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 1);

    // Power cycle: a lane is at the head (head sensor true) but firmware lost which
    // one (Chan=0). All four lanes still physically present.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    AmsBackendAd5xIfs::ZColorSilentResult boot;
    boot.saw_valid_response = true;
    boot.ifs_active = true;
    boot.ifs_chan = 0; // firmware forgot the seated channel
    boot.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, boot);

    // Channel 2 (slot 1) is restored as seated -> Unload on it, Eject on cold lanes.
    CHECK(backend.get_system_info().current_slot == 1);
    CHECK(backend.slot_unloads_to_toolhead(1, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    CHECK_FALSE(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
}

TEST_CASE("AD5X IFS does not restore a seated lane whose port is now empty (#1065)",
          "[ams][ad5x_ifs]") {
    // Corroboration: the filament in the remembered lane was pulled while powered
    // off. head_filament_ says SOMETHING is loaded, but it is not the remembered
    // lane (its port reads empty), so we must NOT claim it as seated.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());

    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 1); // remembered ch2

    Ad5xIfsTestAccess::set_head_filament(backend, true);
    AmsBackendAd5xIfs::ZColorSilentResult boot;
    boot.saw_valid_response = true;
    boot.ifs_active = true;
    boot.ifs_chan = 0;
    // Channel 2 (index 1) is now empty -> the remembered lane can't be seated.
    boot.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, boot);

    // No false claim: the seated slot stays unknown.
    CHECK(backend.get_system_info().current_slot == -1);
}

TEST_CASE("AD5X IFS remembers the seated lane on load and forgets it on unload (#1065)",
          "[ams][ad5x_ifs]") {
    // The persisted marker is written when a load seats a channel (Chan>0 with the
    // lane present) and cleared when an unload empties the head (Chan==0, head off).
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(2, 2).success()); // tool 2 -> port 3

    // Load channel 3 (slot 2): head rises, Chan=3, port 3 present.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    AmsBackendAd5xIfs::ZColorSilentResult loaded;
    loaded.saw_valid_response = true;
    loaded.ifs_active = true;
    loaded.ifs_chan = 3;
    loaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{false, false, true, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
    CHECK(Ad5xIfsTestAccess::persisted_seated_slot(backend) == std::optional<int>(2));

    // Unload: head drops, Chan back to 0 -> forget the remembered lane.
    Ad5xIfsTestAccess::set_head_filament(backend, false);
    AmsBackendAd5xIfs::ZColorSilentResult unloaded;
    unloaded.saw_valid_response = true;
    unloaded.ifs_active = true;
    unloaded.ifs_chan = 0;
    unloaded.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{false, false, false, false};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, unloaded);
    CHECK_FALSE(Ad5xIfsTestAccess::persisted_seated_slot(backend).has_value());
}

TEST_CASE("AD5X IFS IFS_STATUS Chan is applied on prompt-fallback (old zmod)", "[ams][ad5x_ifs]") {
    // On old zmod, GET_ZCOLOR SILENT=1 degrades to an action:prompt dialog
    // (is_prompt_fallback). IFS_STATUS rides the same buffer as clean JSON
    // (respond_info, not a dialog), so its Chan must still drive the seated slot
    // even though the prompt-fallback flag is set and silent support is disabled.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.set_tool_mapping(3, 3).success()); // tool 3 -> port 4

    Ad5xIfsTestAccess::set_head_filament(backend, true);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.is_prompt_fallback = true; // GET_ZCOLOR dialog on old zmod
    r.saw_valid_response = true; // IFS_STATUS JSON still recognised
    r.ifs_chan = 4;

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Prompt-fallback still disables silent support for the session...
    REQUIRE_FALSE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));
    // ...but the seated channel from IFS_STATUS was applied regardless.
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 3);
    REQUIRE(backend.get_system_info().current_slot == 3);
}

TEST_CASE("AD5X IFS native ZMOD: IFS_STATUS Chan resolves the loaded slot without a populated "
          "tool map (raza616 loaded-status defect, bundle UQG4RNUA)",
          "[ams][ad5x_ifs]") {
    // Native ZMOD has no lessWaste/bambufy _IFS_VARS, so parse_save_variables
    // never populates tool_map_ — it stays all-UNMAPPED. The seated channel from
    // IFS_STATUS ("Chan") is therefore the ONLY authority for which slot is
    // loaded. Pre-fix, recompute_current_slot_locked laundered the seated channel
    // through the empty tool_map_ (find_first_tool_for_port -> -1 -> active_tool_
    // -1 -> current_slot -1), pinning current_slot at -1 for the whole session.
    // slot_is_actively_loaded is (slot == current_slot && filament_loaded), so it
    // was ALWAYS false even with filament demonstrably at the toolhead — the UI
    // showed "not loaded" and disabled Unload right after a successful load
    // (bundle UQG4RNUA: Chan=1, head sensor detected, current_slot stuck at -1).
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Production native path: has_ifs_vars_ defaults false and NO set_tool_mapping
    // is ever called — that 1:1 map is exactly what the field device lacks.
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    // Filament physically at the toolhead. handle_status also sets
    // system_info_.filament_loaded (= head_filament_) via the state_changed path.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    REQUIRE(backend.is_filament_loaded());

    // Firmware reports port 1 seated (matches the bundle's IFS_STATUS).
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 1; // 1-based seated channel -> slot 0
    r.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, true, false, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // The seated channel — not the empty tool_map_ — resolves the loaded slot.
    REQUIRE(backend.get_system_info().current_slot == 0);
    REQUIRE(backend.slot_is_actively_loaded(0));
    REQUIRE_FALSE(backend.slot_is_actively_loaded(1));

    // A later Chan=0 (nothing seated) correctly clears the loaded slot.
    AmsBackendAd5xIfs::ZColorSilentResult cleared;
    cleared.saw_valid_response = true;
    cleared.ifs_chan = 0;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, cleared);
    REQUIRE(backend.get_system_info().current_slot == -1);
    REQUIRE_FALSE(backend.slot_is_actively_loaded(0));
}

TEST_CASE("AD5X IFS a confirmed SILENT device is NOT demoted by a later prompt "
          "(raza616 #981 false latch, EE5L8LY2)",
          "[ams][ad5x_ifs]") {
    // raza616's device supports GET_ZCOLOR SILENT=1 (it returned clean silent
    // content at boot). Mid-session the user opened zmod's interactive "Select
    // print materials" colour menu — a prompt dialog — while a SILENT query was
    // in flight. Pre-fix, HelixScreen attributed that prompt to its own query and
    // latched zcolor_silent_supported_=false, demoting a capable device to the
    // resurrection-prone JSON-inference path. Once SILENT has been confirmed
    // (genuine summary/slot content seen), a later prompt must NOT demote it.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));

    // A genuine SILENT response with content confirms the device speaks SILENT.
    AmsBackendAd5xIfs::ZColorSilentResult ok;
    ok.saw_valid_response = true;
    ok.saw_silent_content = true; // a GET_ZCOLOR summary/slot line was parsed
    ok.ifs_active = true;
    ok.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, ok);
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend)); // still supported

    // The user's colour menu prompt (IFS_STATUS rides the same response).
    AmsBackendAd5xIfs::ZColorSilentResult prompt;
    prompt.is_prompt_fallback = true;
    prompt.saw_valid_response = true;
    prompt.ifs_chan = 2;
    prompt.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{false, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, prompt);

    // NOT demoted — the prompt was external (user menu), not our query degrading.
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));
}

TEST_CASE("AD5X IFS phase: IFS_STATUS Chan=0 after head drop finalizes unload to IDLE",
          "[ams][ad5x_ifs][phase]") {
    // During a tracked unload, after the head sensor drops, an IFS_STATUS with
    // Chan=0 (nothing seated) is the clean terminal signal — finalize to IDLE
    // even though extruder_slot is also absent. Works on prompt-fallback.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_chan = 0; // nothing seated -> unload complete
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS phase: head drop during unload advances past HEATING (no heat event)",
          "[ams][ad5x_ifs][phase]") {
    // _IFS_REMOVE_CURRENT_PRUTOK runs with BYPASS_TEMPERATURE_CHECK and sends no
    // preheat, so the nozzle never reaches target and reached_target_once stays
    // false -> stuck in HEATING forever (only the 300s timeout recovered). A head
    // drop physically proves the cut/retract started, so it must mark
    // reached_target_once and advance the phase past HEATING.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    // No temp ever reaches target. Head sensor drops (cut underway).
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));

    // Must have advanced past HEATING despite never seeing a heat event.
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);
}

TEST_CASE("AD5X IFS apply_zcolor_result updates port_presence", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.current_channel = 1;
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};
    r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "2750E0"};
    // slots 2 and 3 left empty — should clear any existing presence

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));
}

TEST_CASE("AD5X IFS apply_zcolor_result skips on prompt fallback", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.is_prompt_fallback = true;

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // One prompt-style response flips silent_supported to false permanently
    // for this session; subsequent query_zcolor_silent() becomes a no-op.
    REQUIRE_FALSE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));
}

TEST_CASE("AD5X IFS apply_zcolor_result skips when response has no valid content",
          "[ams][ad5x_ifs]") {
    // Regression: a transient/malformed response with zero slot lines and no
    // summary line must NOT wipe port_presence. Pre-fix, an empty ZColorSilentResult
    // would clear all four slots to "not loaded".
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed presence so we can detect an erroneous wipe.
    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    // Empty (junk response) — saw_valid_response stays false.
    AmsBackendAd5xIfs::ZColorSilentResult empty;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, empty);

    // Slot 0 must still be present — we didn't get valid data, don't overwrite.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
}

TEST_CASE("AD5X IFS apply_zcolor_result updates colors and materials", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "00FF00"};

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Color and material should be propagated.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    // build_colors returns the comma-separated list used for _IFS_VARS writes;
    // indirect but the only public window into colors_[] without friend access.
    auto colors = Ad5xIfsTestAccess::build_colors(backend);
    auto types = Ad5xIfsTestAccess::build_types(backend);
    REQUIRE(colors.find("00FF00") != std::string::npos);
    REQUIRE(types.find("PETG") != std::string::npos);
}

// raza616 v0.99.50 report: "HelixScreen seems to be unaware of which IFS lane
// is currently loaded. Doesn't update when an IFS lane is unloaded — shows
// the filament that was unloaded." Root cause: parse_zcolor_silent extracted
// extruder_slot from the GET_ZCOLOR summary line ("// Extruder: 3: PLA/HEX |
// IFS: True") but apply_zcolor_result never used it. active_tool_ was only
// updated from lessWaste/bambufy save_variables — stock-ZMOD users never
// got an active-tool signal.
TEST_CASE("AD5X IFS apply_zcolor_result derives active_tool from extruder_slot (stock ZMOD)",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Establish an identity tool map so find_first_tool_for_port(N) -> tool N-1.
    // Default tool_map_ is all UNMAPPED_PORT, so without this find_first_tool_for_port
    // returns -1 for every port and the test would always assert -1.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());

    SECTION("extruder_slot present → active_tool follows tool_map_") {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.extruder_slot = 2; // 0-based → port 3 → tool 2 under identity map
        r.slots[2] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};

        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 2);
    }

    SECTION("extruder_slot absent (unloaded) → active_tool clears to -1") {
        // Seed with a loaded slot first so we can detect the clear.
        AmsBackendAd5xIfs::ZColorSilentResult loaded;
        loaded.saw_valid_response = true;
        loaded.ifs_active = true;
        loaded.extruder_slot = 1;
        loaded.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
        Ad5xIfsTestAccess::apply_zcolor_result(backend, loaded);
        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 1);

        // Now an "Extruder: None (N)" response — extruder_slot becomes nullopt.
        AmsBackendAd5xIfs::ZColorSilentResult unloaded;
        unloaded.saw_valid_response = true;
        unloaded.ifs_active = true;
        unloaded.current_channel = 1;
        // extruder_slot deliberately not set; slots all empty
        Ad5xIfsTestAccess::apply_zcolor_result(backend, unloaded);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
    }

    SECTION("extruder_slot maps to unmapped tool → active_tool becomes -1") {
        // Wipe the map for slot 0 (port 1), then claim slot 0 is in the extruder.
        // No tool maps to port 1, so find_first_tool_for_port(1) returns -1.
        REQUIRE(backend.set_tool_mapping(0, /*slot=*/-1).success()); // unmap T0

        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.extruder_slot = 0;
        r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "808080"};

        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
    }
}

TEST_CASE("AD5X IFS apply_zcolor_result leaves active_tool alone when has_ifs_vars",
          "[ams][ad5x_ifs]") {
    // lessWaste/bambufy users get active_tool from <prefix>_current_tool in
    // save_variables, which is authoritative for them. GET_ZCOLOR's view must
    // not race against it — verify by setting has_ifs_vars=true and checking
    // active_tool_ is unchanged after apply.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed the tool map BEFORE flipping has_ifs_vars=true. set_tool_mapping
    // tries to persist via write_ifs_var when has_ifs_vars is true, which
    // requires an api_ connection — using nullptr would return "No API
    // connection" before the in-memory mutation completes.
    REQUIRE(backend.set_tool_mapping(0, 0).success());
    REQUIRE(backend.set_tool_mapping(1, 1).success());
    REQUIRE(backend.set_tool_mapping(2, 2).success());
    REQUIRE(backend.set_tool_mapping(3, 3).success());
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.extruder_slot = 2;
    r.slots[2] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // active_tool defaulted to -1 and was not updated — lessWaste's save_variables
    // path retains exclusive ownership.
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == -1);
}

TEST_CASE("AD5X IFS apply_zcolor_result skips color write on dirty slot", "[ams][ad5x_ifs]") {
    // Dirty slot means an unsaved user edit is pending — we must NOT overwrite
    // the local color with zmod's view, or we'd clobber the user's edit.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed slot 0 with a color we want preserved.
    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FF0000") != std::string::npos);

    // Mark dirty, then apply a result that would change color.
    Ad5xIfsTestAccess::set_dirty(backend, 0, true);
    AmsBackendAd5xIfs::ZColorSilentResult incoming;
    incoming.saw_valid_response = true;
    incoming.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "0000FF"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, incoming);

    // Color must still be FF0000 — dirty-slot guard held.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FF0000") != std::string::npos);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("0000FF") == std::string::npos);
}

TEST_CASE("AD5X IFS apply_zcolor_result old-format preserves colors", "[ams][ad5x_ifs]") {
    // Pre-ad2802ab zmod: slot lines carry no /HEX. Presence should still
    // update, but existing colors must NOT be overwritten with empty strings.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFAA00"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FFAA00") != std::string::npos);

    AmsBackendAd5xIfs::ZColorSilentResult old_fmt;
    old_fmt.saw_valid_response = true;
    old_fmt.is_old_format = true;
    // slot present but material only, no hex
    old_fmt.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", ""};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, old_fmt);

    // Color preserved from JSON-seeded state.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FFAA00") != std::string::npos);
    // Presence still reflects what the old-format response said.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
}

TEST_CASE("AD5X IFS parse_zcolor_silent sets saw_valid_response", "[ams][ad5x_ifs]") {
    SECTION("summary line present") {
        std::vector<std::string> lines = {"// Extruder: None (0) | IFS: True"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE(r.saw_valid_response);
    }
    SECTION("slot line present") {
        std::vector<std::string> lines = {"// 1: PLA/FFFFFF"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE(r.saw_valid_response);
    }
    SECTION("only junk lines") {
        std::vector<std::string> lines = {"echo: random output", "// not a slot line"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE_FALSE(r.saw_valid_response);
    }
    SECTION("slot-number-out-of-range line") {
        // "// 99: nonsense" is skipped and must NOT count as valid.
        std::vector<std::string> lines = {"// 99: nonsense"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE_FALSE(r.saw_valid_response);
    }
}

// ==========================================================================
// Material whitelist + normalization
// ==========================================================================

TEST_CASE("AD5X IFS get_supported_materials returns firmware whitelist", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    auto supported = backend.get_supported_materials();
    REQUIRE(supported.has_value());
    REQUIRE(supported->size() == 7);

    // Exact strings as firmware expects (order mirrors the firmware error message).
    REQUIRE((*supported)[0] == "PLA");
    REQUIRE((*supported)[1] == "PLA-CF");
    REQUIRE((*supported)[2] == "SILK");
    REQUIRE((*supported)[3] == "TPU");
    REQUIRE((*supported)[4] == "ABS");
    REQUIRE((*supported)[5] == "PETG");
    REQUIRE((*supported)[6] == "PETG-CF");
}

TEST_CASE("AD5X IFS normalize_material coerces input to whitelist", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("exact match passes through") {
        REQUIRE(backend.normalize_material("PLA") == "PLA");
        REQUIRE(backend.normalize_material("PETG-CF") == "PETG-CF");
        REQUIRE(backend.normalize_material("TPU") == "TPU");
    }

    SECTION("case-insensitive match returns canonical case") {
        REQUIRE(backend.normalize_material("pla") == "PLA");
        REQUIRE(backend.normalize_material("Pla") == "PLA");
        REQUIRE(backend.normalize_material("petg-cf") == "PETG-CF");
        REQUIRE(backend.normalize_material("silk") == "SILK");
    }

    SECTION("PLA+ collapses via compat_group to PLA") {
        // PLA+ shares compat_group "PLA" with PLA in the filament DB.
        REQUIRE(backend.normalize_material("PLA+") == "PLA");
    }

    SECTION("ASA collapses via compat_group to ABS") {
        // ASA has compat_group "ABS_ASA"; ABS is the first whitelist entry
        // with matching compat_group.
        REQUIRE(backend.normalize_material("ASA") == "ABS");
    }

    SECTION("PEEK falls back to first entry (no compat_group match)") {
        // PEEK's compat_group is "HIGH_TEMP" which no whitelist entry shares.
        REQUIRE(backend.normalize_material("PEEK") == "PLA");
    }

    SECTION("empty string falls back to first entry") {
        REQUIRE(backend.normalize_material("") == "PLA");
    }

    SECTION("unknown material falls back to first entry") {
        REQUIRE(backend.normalize_material("Nonsense") == "PLA");
    }

    SECTION("silk variants map to SILK via AD5X-specific override") {
        // AD5X treats SILK as distinct from PLA. The shared filament DB
        // groups silk variants under compat_group "PLA" because most
        // printers don't make that distinction, so the default fallback
        // would route them to "PLA". The AD5X normalize_material()
        // override catches common silk names before delegating.
        REQUIRE(backend.normalize_material("Silk PLA") == "SILK");
        REQUIRE(backend.normalize_material("PLA Silk") == "SILK");
        REQUIRE(backend.normalize_material("Silk") == "SILK");
        REQUIRE(backend.normalize_material("silk pla") == "SILK");
    }
}

TEST_CASE("AFC backend has no whitelist and passes material through unchanged",
          "[ams][whitelist]") {
    // AFC (like Happy Hare, ACE, CFS) treats material as a free-form label.
    AmsBackendAfc backend(nullptr, nullptr);

    REQUIRE_FALSE(backend.get_supported_materials().has_value());
    REQUIRE(backend.normalize_material("PLA+") == "PLA+");
    REQUIRE(backend.normalize_material("Some Random String") == "Some Random String");
    REQUIRE(backend.normalize_material("") == "");
}

// ==========================================================================
// FilamentSlotOverride integration (Task 9)
//
// The override store is loaded once in on_started() and then layered over
// every parse via update_slot_from_state(). These tests exercise the layering
// directly by seeding the in-memory overrides map (via test access) and then
// driving the parse path — parse_adventurer_json is used because it goes
// through update_slot_from_state and is the simplest hook-free entry point
// from a test fixture constructed with nullptr api/client.
// ==========================================================================

TEST_CASE("AD5X IFS applies override brand over Adventurer5M.json data",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Green";
    ovr.color_rgb = 0x00AA00; // Override to green
    ovr.color_set = true;
    ovr.material = "PETG"; // Override to PETG
    ovr.spoolman_id = 42;
    ovr.remaining_weight_g = 750.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports slot 0 (port 1) as orange PLA.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF5500",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    // Override wins for every non-default field.
    REQUIRE(info.brand == "Polymaker");
    REQUIRE(info.spool_name == "PolyLite Green");
    REQUIRE(info.color_rgb == 0x00AA00u);
    REQUIRE(info.material == "PETG");
    REQUIRE(info.spoolman_id == 42);
    REQUIRE(info.remaining_weight_g == 750.0f);
}

TEST_CASE("AD5X IFS preserves firmware color when no override present",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // No overrides seeded — firmware data must flow through unchanged.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF5500",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xFF5500u);
    REQUIRE(info.material == "PLA");
    // Default-valued fields on SlotInfo.
    REQUIRE(info.brand.empty());
    REQUIRE(info.spool_name.empty());
    REQUIRE(info.spoolman_id == 0);
}

TEST_CASE("AD5X IFS partial override only replaces specified fields",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed an override that only sets brand. Every other field must fall
    // through to the firmware-reported value (or SlotInfo default).
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF5500",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.brand == "Polymaker");        // override wins
    REQUIRE(info.color_rgb == 0xFF5500u);      // firmware untouched
    REQUIRE(info.material == "PLA");           // firmware untouched
    REQUIRE(info.spool_name.empty());          // default
    REQUIRE(info.spoolman_id == 0);            // default
    REQUIRE(info.remaining_weight_g == -1.0f); // default
}

TEST_CASE("AD5X IFS override applies to multiple slots independently",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr0;
    ovr0.brand = "Polymaker";
    ovr0.material = "PETG";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr0);

    helix::ams::FilamentSlotOverride ovr2;
    ovr2.brand = "eSUN";
    ovr2.color_rgb = 0x123456;
    ovr2.color_set = true;
    Ad5xIfsTestAccess::seed_override(backend, 2, ovr2);

    // Slots 1 and 3 have NO override — must reflect pure firmware data.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmColor2": "#00FF00",
            "ffmColor3": "#0000FF",
            "ffmColor4": "#FFFFFF",
            "ffmType1": "PLA",
            "ffmType2": "PLA",
            "ffmType3": "PLA",
            "ffmType4": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info0 = backend.get_slot_info(0);
    REQUIRE(info0.brand == "Polymaker");
    REQUIRE(info0.material == "PETG");
    REQUIRE(info0.color_rgb == 0xFF0000u); // firmware untouched by ovr0

    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.brand.empty());
    REQUIRE(info1.color_rgb == 0x00FF00u);
    REQUIRE(info1.material == "PLA");

    auto info2 = backend.get_slot_info(2);
    REQUIRE(info2.brand == "eSUN");
    REQUIRE(info2.color_rgb == 0x123456u);
    REQUIRE(info2.material == "PLA");

    auto info3 = backend.get_slot_info(3);
    REQUIRE(info3.brand.empty());
    REQUIRE(info3.color_rgb == 0xFFFFFFu);
}

TEST_CASE("AD5X IFS override re-applied on every parse",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PETG";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: firmware reports orange PLA.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    auto first = backend.get_slot_info(0);
    REQUIRE(first.brand == "Polymaker");
    REQUIRE(first.material == "PETG");

    // Second parse with the same firmware color but a different material.
    // The override must still win on re-parse. Note: deliberately keep the
    // color stable — Task 11's hardware-event detection clears overrides when
    // firmware color changes (physical spool swap), which is tested in the
    // hardware-swap test cases below. This case exercises the "override wins
    // on re-parse" property, which is a separate contract.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "ABS"}
    })");
    auto second = backend.get_slot_info(0);
    REQUIRE(second.brand == "Polymaker");
    REQUIRE(second.material == "PETG");
}

TEST_CASE("AD5X IFS override zero color_rgb does not replace firmware color",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // color_rgb=0 is the "no override" sentinel — must not clobber firmware.
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "TestBrand";
    ovr.color_rgb = 0;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#AA55FF", "ffmType1": "PLA"}
    })");

    auto info = backend.get_slot_info(0);
    REQUIRE(info.brand == "TestBrand");
    REQUIRE(info.color_rgb == 0xAA55FFu); // unchanged
}

TEST_CASE("AD5X IFS override negative weights do not replace firmware values",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed an override with -1.0 weights (the "unknown" sentinel) — must not
    // overwrite whatever weights the firmware / SlotInfo default holds.
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "WeightTest";
    ovr.remaining_weight_g = -1.0f;
    ovr.total_weight_g = -1.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF0000", "ffmType1": "PLA"}
    })");

    auto info = backend.get_slot_info(0);
    REQUIRE(info.brand == "WeightTest");
    // Firmware doesn't populate weights at all — they should remain at the
    // SlotInfo default (-1), not at zero. This verifies apply_overrides did
    // NOT write the -1 sentinel over the default (which would be a no-op
    // today but guards against a future default change).
    REQUIRE(info.remaining_weight_g == -1.0f);
    REQUIRE(info.total_weight_g == -1.0f);
}

TEST_CASE("AD5X IFS set_slot_info takes effect when no override present",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Regression lock: with no override seeded for the slot, set_slot_info's
    // edit (every SlotInfo field, not just color/material) must be visible
    // via get_slot_info. Task 9 added apply_overrides to the parse path;
    // Task 10 extended entry->info update to carry brand / spool_name /
    // spoolman_id / color_name through a persist=false "preview" write.
    // This test guards against a regression where the parse path
    // accidentally runs with stale overrides_ state and clobbers the edit.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SlotInfo edit;
    edit.color_rgb = 0xAABBCC;
    edit.material = "PETG";
    edit.brand = "UserBrand";
    edit.spool_name = "UserSpool";
    edit.spoolman_id = 99;
    backend.set_slot_info(0, edit, false);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xAABBCCu);
    REQUIRE(info.material == "PETG");
    REQUIRE(info.brand == "UserBrand");
    REQUIRE(info.spool_name == "UserSpool");
    REQUIRE(info.spoolman_id == 99);
}

// ==========================================================================
// Task 10: set_slot_info(persist=true) writes through to
// FilamentSlotOverrideStore + in-memory overrides_ map so user edits survive
// subsequent parses.
// ==========================================================================

TEST_CASE("AD5X IFS set_slot_info(persist=true) stores override in memory and store",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Build a real MoonrakerAPIMock so the backend's override store has a
    // destination to write to. on_started() is not called — overrides_
    // starts empty — so we can assert the persist path populates it.
    Ad5xIfsTmpCacheDir tmp("task10_stores_override");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    // Native ZMOD path is skipped by marking has_ifs_vars_ true — this test
    // focuses on the override-store write, not the Klipper-facing side.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.remaining_weight_g = 850.0f;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    // In-memory reads immediately see the edits — apply_overrides uses the
    // newly-staged override rather than any pre-edit value.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite PLA Orange");
    CHECK(info.spoolman_id == 42);
    CHECK(info.remaining_weight_g == 850.0f);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0xFF5500u);

    // overrides_ map was written under mutex_ as the persist staging step.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0xFF5500u);

    // Moonraker DB received the AFC-shaped record via save_async (which
    // MoonrakerAPIMock dispatches synchronously in-call).
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["material"] == "PLA");
    CHECK(stored["color"] == "#FF5500");
}

TEST_CASE("AD5X IFS set_slot_info(persist=false) does NOT write to store",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Same fixture as above, but with persist=false the override store
    // must stay untouched — set_slot_info is a pure in-memory preview.
    Ad5xIfsTmpCacheDir tmp("task10_no_persist");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    // No override staged — the in-memory entry carries the edit directly
    // (since no prior override clobbers it via apply_overrides).
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    // Moonraker DB not touched.
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    // The edit is still visible via get_slot_info — this is the preview path.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Draft");
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x123456u);
}

// ==========================================================================
// #981: update_slot_weight() is the consumption-sink's weight-only persist.
// It MUST update weight without re-asserting filament identity (material/
// color/locks) and MUST NOT rewrite Adventurer5M.json — the firmware-facing
// writers in set_slot_info() reverted externally-set materials on every 60 s
// persist.
// ==========================================================================

TEST_CASE("AD5X IFS update_slot_weight preserves identity and does not write Adventurer5M.json",
          "[ams][ad5x_ifs][filament_slot_override][981]") {
    Ad5xIfsTmpCacheDir tmp("weight_only_preserves_identity");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    seed_standard_colors(backend); // firmware truth: slot 0 = PLA / #FF0000
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // A prior in-app edit: slot 0 locked to PETG / #ABCDEF with a weight.
    helix::ams::FilamentSlotOverride locked;
    locked.material = "PETG";
    locked.user_locked_material = true;
    locked.color_rgb = 0xABCDEF;
    locked.color_set = true;
    locked.user_locked_color = true;
    locked.remaining_weight_g = 100.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, locked);

    // A sentinel Adventurer5M.json at the resolved local path. update_slot_weight
    // must leave it byte-for-byte untouched (set_slot_info would rewrite it).
    const std::string json_path = (tmp.path / "Adventurer5M.json").string();
    const std::string sentinel = "{\"FFMInfo\":{\"ffmType1\":\"PETG\",\"sentinel\":true}}";
    { std::ofstream(json_path) << sentinel; }
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, json_path);

    backend.update_slot_weight(0, /*remaining=*/42.0f, /*total=*/-1.0f, /*persist=*/true);

    // Weight updated; identity and locks untouched.
    auto ovr = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(ovr.has_value());
    CHECK(ovr->remaining_weight_g == 42.0f);
    CHECK(ovr->material == "PETG");
    CHECK(ovr->user_locked_material == true);
    CHECK(ovr->color_rgb == 0xABCDEFu);
    CHECK(ovr->user_locked_color == true);

    // Adventurer5M.json was NOT rewritten — this is the #981 regression guard.
    std::ifstream check(json_path);
    std::string after((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>());
    CHECK(after == sentinel);

    // get_slot_info weight reflects the in-memory update. (Material identity is
    // asserted on the override above; get_slot_info doesn't re-run
    // apply_overrides here, so it still shows firmware-level state — not what
    // this test is about.)
    CHECK(backend.get_slot_info(0).remaining_weight_g == 42.0f);
}

TEST_CASE("AD5X IFS update_slot_weight on an un-overridden slot does not lock identity",
          "[ams][ad5x_ifs][filament_slot_override][981]") {
    Ad5xIfsTmpCacheDir tmp("weight_only_no_lock");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    seed_standard_colors(backend); // firmware truth: slot 1 = PETG / #00FF00
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // No prior override on slot 1.
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 1).has_value());

    backend.update_slot_weight(1, /*remaining=*/55.0f, /*total=*/-1.0f, /*persist=*/true);

    // A weight-only override is created — crucially WITHOUT locking material or
    // color. set_slot_info would have stamped user_locked_material=true and
    // frozen the firmware material into the override (the bug).
    auto ovr = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(ovr.has_value());
    CHECK(ovr->remaining_weight_g == 55.0f);
    CHECK(ovr->material.empty());
    CHECK(ovr->user_locked_material == false);
    CHECK(ovr->color_set == false);
    CHECK(ovr->user_locked_color == false);

    // Identity still flows from firmware (override doesn't shadow it).
    auto info = backend.get_slot_info(1);
    CHECK(info.material == "PETG");
    CHECK(info.remaining_weight_g == 55.0f);
}

TEST_CASE("AD5X IFS set_slot_info(persist=true) survives a matching firmware parse",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // After a persist=true write, the user's color/material round-trip through
    // Adventurer5M.json — so the next firmware parse reports the SAME values
    // and apply_overrides re-lays the brand/spool_name/etc. metadata that
    // doesn't live in firmware.
    //
    // Pre-color_set bug: this test fed mismatched firmware values (#000000
    // ABS) and asserted the override won. That worked only because the
    // OverwriteAlways auto-mirror's `observed_color == 0` short-circuit
    // dropped the firmware change. With color_set + the IFS fix, mismatched
    // firmware is correctly detected as an external edit and the user's
    // values are overwritten. That's the desired production behavior — but
    // it makes the prior test premise unrealistic. The realistic flow:
    // write_adventurer_json succeeds in production, firmware reports the
    // user's color back, no change detected, override metadata survives.
    Ad5xIfsTmpCacheDir tmp("task10_next_parse");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    backend.set_slot_info(0, edit, /*persist=*/true);

    // Simulate a subsequent firmware parse that mirrors the user's edit
    // (production: write_adventurer_json succeeded). The override's brand
    // (firmware can't carry brand) must still be visible after re-parse.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");   // override fills the firmware-can't-carry field
    CHECK(info.material == "PLA");      // matches both user + firmware
    CHECK(info.color_rgb == 0xFF5500u); // matches both user + firmware
}

TEST_CASE("AD5X IFS user-edited slot survives firmware FFMInfo revert (#965 regression)",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // #965: AD5X firmware re-emits the previously-loaded material into
    // Adventurer5M.json shortly after print completion (and on some
    // restart paths). Pre-fix, the OverwriteAlways auto-mirror would clobber
    // the user's material choice through this exact code path:
    //   set_slot_info(persist=true) writes PLA to the override
    //   → firmware post-print bug rewrites material back to HIPS in the JSON
    //   → parse_adventurer_json reads HIPS into materials_[]
    //   → check_external_color_change fires (color also drifted)
    //   → mirror runs OverwriteAlways → override.material flipped from PLA to
    //     HIPS, save_async persists the wrong value to Moonraker DB.
    //
    // Post-fix: set_slot_info(persist=true) tags user_locked_material=true,
    // so the mirror's material branch is skipped even when color changes.
    // Color may still propagate (treated as firmware-authoritative drift) —
    // the regression we're guarding against is material data loss.
    Ad5xIfsTmpCacheDir tmp("ifs_postprint_revert_965");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // User sets the slot to PLA via HelixScreen UI.
    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);

    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->user_locked_color);
        CHECK(staged->user_locked_material);
    }

    // First parse — establishes color baseline at FF5500.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    // Now simulate the firmware post-print revert: material reverts to HIPS
    // and color drifts (the actual #965 reporter's scenario had a color
    // change too). With the lock in place, the user's PLA must survive even
    // though every condition that previously triggered the clobber is met.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#000000", "ffmType1": "HIPS"}
    })");

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker"); // override-only field always preserved
    CHECK(info.material == "PLA");    // #965: must NOT have been clobbered to HIPS
    // Color: locked too, so user's choice survives. Without the lock, the
    // mirror would have flipped it to 0x000000.
    CHECK(info.color_rgb == 0xFF5500u);

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->material == "PLA");
    CHECK(staged->color_rgb == 0xFF5500u);

    // Verify the Moonraker DB record was NOT overwritten with the firmware-
    // reverted values (the persistence step of the original bug).
    auto db = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!db.is_null());
    CHECK(db.value("material", "") == "PLA");
    CHECK(db.value("color", "") == "#FF5500");
}

TEST_CASE("AD5X IFS auto-mirror still tracks firmware for slots with no user lock",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Companion to the #965 regression: bootstrap (no override, or override
    // with locks=false) MUST still pick up genuine external edits so
    // OrcaSlicer's MoonrakerPrinterAgent stays in sync with the printer.
    // Only user-locked slots are sticky; everything else tracks firmware.
    Ad5xIfsTmpCacheDir tmp("ifs_bootstrap_tracks_firmware");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // First parse: firmware reports orange PLA — bootstrap fills the
    // override via the mirror (locks stay false because this came from
    // auto-mirror, not set_slot_info).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    // Second parse with different color: external edit (Mainsail / LCD)
    // — auto-mirror tracks it because no user lock is in effect.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0x0055FFu);
    CHECK(info.material == "PETG");
}

TEST_CASE("AD5X IFS set_slot_info(persist=true) with no store still updates in-memory map",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Backend constructed with no api/client AND no injected store — the
    // persist path must still stage the override in memory so the current
    // UI session sees the edit, even though there's nowhere to save it.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    // write_adventurer_json will fail with "No API connection" because api_
    // is nullptr. That's expected — but the in-memory override stage
    // happens BEFORE that write and must still be visible.
    (void)err;

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->material == "PLA");
    CHECK(staged->color_rgb == 0xFF5500u);
}

TEST_CASE("AD5X IFS set_slot_info(persist=true) with pre-existing override replaces it",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Seed an old override (simulating a prior load from disk), then overwrite
    // it via set_slot_info. get_slot_info must reflect the NEW values
    // immediately, not the old staged override.
    Ad5xIfsTmpCacheDir tmp("task10_replace");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride old;
    old.brand = "OldBrand";
    old.spool_name = "OldSpool";
    old.spoolman_id = 7;
    Ad5xIfsTestAccess::seed_override(backend, 0, old);

    // User edits with a different brand — the NEW values must win, not
    // the old override.
    SlotInfo edit;
    edit.brand = "NewBrand";
    edit.spool_name = "NewSpool";
    edit.spoolman_id = 99;
    edit.material = "PLA";
    edit.color_rgb = 0xAABBCC;
    backend.set_slot_info(0, edit, /*persist=*/true);

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "NewBrand");
    CHECK(info.spool_name == "NewSpool");
    CHECK(info.spoolman_id == 99);

    // Staged override replaced cleanly.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "NewBrand");
    CHECK(staged->spoolman_id == 99);
}

// ==========================================================================
// External color/material edits (Mainsail console, AD5X LCD, native zmod
// dialog, CHANGE_ZCOLOR from any non-Helix source) must REFRESH the Moonraker
// DB lane_data entry that Orca's MoonrakerPrinterAgent reads — they must NOT
// wipe the brand/spool_name/spoolman_id metadata. (compulsivejohnny on
// Discord: lane_data went stale after every external CHANGE_ZCOLOR because
// the previous "color change = physical swap" heuristic cleared the record.)
// Initial startup observations are a baseline and never trigger a sync.
// Genuine spool removal is detected by the eject path (presence true→false
// in parse_adventurer_json) and clears the override there.
// ==========================================================================

TEST_CASE("AD5X IFS external color change syncs lane_data, preserves brand metadata",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("ext_color_change_syncs");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // Seed a lane_data entry in the mock DB plus a matching in-memory override
    // — what the override store load would produce after a Helix-initiated edit.
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"vendor", "Polymaker"},
                                         {"spool_id", 42},
                                         {"spool_name", "PolyLite Orange"},
                                         {"material", "PLA"},
                                         {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: firmware reports color FF5500 — establishes BASELINE. No
    // sync (first observation), override still wins.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.color_rgb == 0xFF5500u);
    }
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: firmware reports a DIFFERENT color (and material). This is
    // an external edit — sync override + lane_data, KEEP brand/spool/spoolman.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    {
        auto info = backend.get_slot_info(0);
        // Brand metadata preserved.
        CHECK(info.brand == "Polymaker");
        CHECK(info.spool_name == "PolyLite Orange");
        CHECK(info.spoolman_id == 42);
        // Color + material reflect firmware truth.
        CHECK(info.color_rgb == 0x0055FFu);
        CHECK(info.material == "PETG");
    }
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0x0055FFu);
    CHECK(staged->material == "PETG");

    // Moonraker DB lane1 entry refreshed by save_async — Orca now sees the
    // new color/material plus the preserved vendor + spool_id.
    auto db = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!db.is_null());
    CHECK(db.value("color", "") == "#0055FF");
    CHECK(db.value("material", "") == "PETG");
    CHECK(db.value("vendor", "") == "Polymaker");
    CHECK(db.value("spool_id", 0) == 42);
}

TEST_CASE("AD5X IFS external color change with no override creates minimal lane_data record",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // When zmod owns slot color/material truth and the user has never touched
    // the slot via Helix, lane_data was previously empty → Orca had no way to
    // see the slot's color from MoonrakerPrinterAgent. Now we publish a
    // minimal record (color + material) so Orca's view stays useful.
    Ad5xIfsTmpCacheDir tmp("ext_color_change_creates_minimal");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // No seeded override. lane_data starts empty.
    REQUIRE(api.mock_get_db_value("lane_data", "lane1").is_null());

    // First parse: establishes baseline at FF5500. No sync (baseline).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: external color change. Minimal override created + lane_data
    // record published.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->color_rgb == 0x0055FFu);
    CHECK(staged->material == "PETG");
    // brand etc. left at defaults — no synthetic vendor name.
    CHECK(staged->brand.empty());
    CHECK(staged->spoolman_id == 0);

    auto db = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!db.is_null());
    CHECK(db.value("color", "") == "#0055FF");
    CHECK(db.value("material", "") == "PETG");
    // vendor field absent (to_lane_data_record omits empty strings).
    CHECK_FALSE(db.contains("vendor"));
}

TEST_CASE("AD5X IFS GET_ZCOLOR eject keeps the override and lane_data (#1071)",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Genuine spool removal. presence is owned solely by GET_ZCOLOR now: when
    // apply_zcolor_result reports a slot present->absent the lane renders empty,
    // but #1071: the override (brand/spool_name/spoolman) and the MR DB lane_data
    // row are RETAINED so a re-inserted same spool keeps its assignment —
    // matching the AFC and Happy Hare backends, which never clear the link on
    // empty. (Previously this drove clear_override_locked, which also deleted the
    // lane_data row via store->clear_async.)
    //
    // The sibling test "a lane going empty keeps its Spoolman link" covers the
    // override-retention without a store; this case additionally verifies the MR
    // DB lane_data row survives.
    Ad5xIfsTmpCacheDir tmp("eject_clears_override");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value(
        "lane_data", "lane1",
        nlohmann::json{
            {"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Loaded state: slot 0 present. (Establish presence explicitly — parse no
    // longer infers it.) Baseline color comes from the JSON parse.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Eject: GET_ZCOLOR reports slot 0 ABSENT. The present->absent transition in
    // apply_zcolor_result drops presence/status but RETAINS the override and the
    // MR DB lane_data entry (#1071).
    {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        // slots[0] left empty => absent
        Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    }

    // The lane renders removed (presence/status dropped)...
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.status != SlotStatus::LOADED);
        // ...but the Spoolman link survives the empty.
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
    }
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK_FALSE(api.mock_get_db_value("lane_data", "lane1").is_null());
}

// ==========================================================================
// _IFS_VARS mirror: when an external CHANGE_ZCOLOR fires, parse_adventurer_json
// must mirror the new colors_/materials_ snapshot into the lessWaste/bambufy
// plugin's <prefix>_colors / <prefix>_types save_variables. Audited 2026-05-04
// against Hrybmo/lesswaste and function3d/bambufy: neither plugin self-syncs
// in response to CHANGE_ZCOLOR, so without this mirror the plugin's runout-
// recovery alternate-port lookup, smart-purge skip decision, and
// _IFS_COLORS_ASSIGN dialog all run against stale color data and silently
// print the wrong color or skip the wrong purge.
// ==========================================================================

namespace {
class GcodeCapturingBackend : public AmsBackendAd5xIfs {
  public:
    using AmsBackendAd5xIfs::AmsBackendAd5xIfs;
    std::vector<std::string> captured_gcodes;
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode,
                           std::function<void()> /*on_complete*/) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    bool any_gcode_starts_with(const std::string& prefix) const {
        for (const auto& g : captured_gcodes) {
            if (g.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }
};
} // namespace

TEST_CASE("AD5X IFS external color change mirrors colors+types into _IFS_VARS",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_external");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // First parse establishes baseline — no sync, no mirror.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS types="));

    // External color change → sync fires, _IFS_VARS mirror dispatches.
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");
    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS types="));

    // Find the actual payloads — should reflect the new firmware truth.
    bool found_color_payload = false;
    bool found_type_payload = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g.find("_IFS_VARS colors=") == 0 && g.find("'0055FF'") != std::string::npos)
            found_color_payload = true;
        if (g.find("_IFS_VARS types=") == 0 && g.find("'PETG'") != std::string::npos)
            found_type_payload = true;
    }
    CHECK(found_color_payload);
    CHECK(found_type_payload);

    // lessWaste prefix → no SHOW=0 suffix (lessWaste's _IFS_VARS doesn't
    // accept it — adding the param would break the macro call).
    for (const auto& g : backend.captured_gcodes) {
        if (g.rfind("_IFS_VARS ", 0) == 0) {
            CHECK(g.find("SHOW=0") == std::string::npos);
        }
    }
}

TEST_CASE("AD5X IFS bambufy prefix gets SHOW=0 to suppress _IFS_VARS echo",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_bambufy_show0");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // Establish baseline + drive an external change.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    // bambufy's _IFS_VARS macro accepts SHOW=0 to skip the RESPOND echo
    // (lesswaste does not). Both colors+types pushes get the suffix.
    bool colors_has_show0 = false;
    bool types_has_show0 = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g.rfind("_IFS_VARS colors=", 0) == 0 && g.find("SHOW=0") != std::string::npos)
            colors_has_show0 = true;
        if (g.rfind("_IFS_VARS types=", 0) == 0 && g.find("SHOW=0") != std::string::npos)
            types_has_show0 = true;
    }
    CHECK(colors_has_show0);
    CHECK(types_has_show0);
}

// #1247: lessWaste's <prefix>_colors/_types save_variables are 16-entry
// TOOL-indexed arrays, but the HelixScreen mirror pushed 4-entry port-indexed
// lists — and _IFS_VARS replaces the arrays wholesale, so every push truncated
// them. _RUNOUT_HEAD's backup scan of tools 4..15 then read out of range and no
// backup spool could ever match (the "filament backup fails to switch" report).
// SAVE_VARIABLE persists the damage across reboots, so parse_save_variables
// must detect the truncated shape and dispatch a correctly-shaped repair push.
TEST_CASE("AD5X IFS repairs truncated lessWaste colors/types, leaves healthy state alone",
          "[ams][ad5x_ifs][1247]") {
    // nullptr api: the repair dispatch is captured by the execute_gcode
    // override, and a null api keeps handle_status_update's JSON-poll backstop
    // idle so the test queues no UpdateQueue work.
    GcodeCapturingBackend backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    seed_standard_colors(backend);

    // Damaged frame: 4-entry colors/types (the truncation signature) beside a
    // healthy 16-entry tools map — exactly what an affected install persists.
    const json identity_tools = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    json damaged{{"less_waste_tools", identity_tools},
                 {"less_waste_colors", json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"})},
                 {"less_waste_types", json::array({"PLA", "PETG", "ABS", "TPU"})}};
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(damaged));

    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    REQUIRE(backend.any_gcode_starts_with("_IFS_VARS types="));
    std::string expected_tail = "\"['FF0000', '00FF00', '0000FF', 'FFFFFF'";
    for (int i = 0; i < 12; ++i) {
        expected_tail += ", ''";
    }
    expected_tail += "]\"";
    bool repair_shapes_ok = false;
    for (const auto& g : backend.captured_gcodes) {
        if (g == "_IFS_VARS colors=" + expected_tail) {
            repair_shapes_ok = true;
        }
    }
    REQUIRE(repair_shapes_ok);
    // lessWaste repair must not carry the bambufy-only SHOW=0 suffix.
    for (const auto& g : backend.captured_gcodes) {
        if (g.rfind("_IFS_VARS ", 0) == 0) {
            CHECK(g.find("SHOW=0") == std::string::npos);
        }
    }

    // Healthy frame: full 16-entry arrays — no further repair dispatch.
    backend.captured_gcodes.clear();
    json healthy_colors = json::array();
    json healthy_types = json::array();
    for (int i = 0; i < 16; ++i) {
        healthy_colors.push_back(i < 4 ? "FF0000" : "");
        healthy_types.push_back(i < 4 ? "PLA" : "");
    }
    json healthy{{"less_waste_tools", identity_tools},
                 {"less_waste_colors", healthy_colors},
                 {"less_waste_types", healthy_types}};
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(healthy));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS types="));
}

TEST_CASE("AD5X IFS no repair for bambufy 4-entry arrays (#1247)", "[ams][ad5x_ifs][1247]") {
    GcodeCapturingBackend backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");

    // bambufy's arrays legitimately hold 4 port-indexed entries — its
    // _RUNOUT_HEAD iterates ifs.types (4) — so the truncation signature check
    // must not fire for that prefix.
    json frame{{"bambufy_tools", json::array({1, 2, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4})},
               {"bambufy_colors", json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"})},
               {"bambufy_types", json::array({"PLA", "PETG", "ABS", "TPU"})}};
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(frame));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS colors="));
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS types="));
}

TEST_CASE("AD5X IFS mirror skipped when has_ifs_vars_ is false (stock zmod)",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Stock zmod (no lessWaste/bambufy plugin) has no _IFS_VARS macro to call.
    // Sync still fires (lane_data still updates for Orca) but the mirror push
    // is skipped — calling _IFS_VARS on a printer without the macro just
    // produces a "Unknown command" gcode error.
    Ad5xIfsTmpCacheDir tmp("ifs_vars_mirror_stock_zmod");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    GcodeCapturingBackend backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, false); // stock zmod
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    backend.captured_gcodes.clear();
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");

    // Sync counter bumped (lane_data still sync'd via save_async).
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) > 0);
    // ...but no _IFS_VARS dispatched.
    CHECK_FALSE(backend.any_gcode_starts_with("_IFS_VARS"));
}

// Regression: bundle AQ6DALWG, raza616 v0.99.51, "filament type changing on
// boot". On startup, the initial printer.objects.query response feeds
// parse_save_variables which iterates update_slot_from_state for every slot
// BEFORE Adventurer5M.json is fetched and parsed. At that point colors_[idx]
// is empty, so the firmware-color branch in update_slot_from_state is skipped
// and entry->info.color_rgb is whatever was last left there — initially the
// SlotInfo default (AMS_DEFAULT_SLOT_COLOR / 0x808080), or a value leaked by
// a prior apply_overrides call.
//
// Pre-fix this populated last_firmware_color_ with a phantom 0x808080 baseline.
// Seconds later parse_adventurer_json arrived with the real firmware color
// (e.g. #898989); the diff against the phantom baseline was misread as a
// physical spool swap and wiped the user override (brand, spoolman_id, weights,
// material). User saw their PETG spool flip back to firmware-truth PLA on
// every boot, and after the wipe the next boot loaded 0 overrides because
// boot 1 had cleared them all.
TEST_CASE("AD5X IFS empty colors_[] on boot does NOT establish phantom baseline",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("boot_phantom_baseline_no_clear");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed a saved override (PETG, brand, spoolman_id) — what the user
    // configured in a prior session and persisted into filament_slot store.
    api.mock_set_db_value(
        "lane_data", "lane1",
        nlohmann::json{
            {"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PETG"}, {"color", "#898989"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Gray";
    ovr.spoolman_id = 42;
    ovr.material = "PETG";
    ovr.color_rgb = 0x898989;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Boot path step 1: parse_save_variables fires from the initial
    // printer.objects.query response. lessWaste/bambufy tools data is
    // present, which triggers update_slot_from_state for every slot — but
    // colors_[idx] is still empty because Adventurer5M.json hasn't been
    // fetched yet. This is the call that, pre-fix, establishes the phantom
    // 0x808080 baseline.
    json save_vars;
    save_vars["less_waste_tools"] =
        json::array({0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5}); // tool 0 -> port 1
    save_vars["less_waste_current_tool"] = -1;
    save_vars["less_waste_external"] = 0;
    Ad5xIfsTestAccess::parse_vars(backend, save_vars);

    // Override must still be intact after parse_save_variables — the helper
    // had no firmware-truth color so it MUST have skipped the swap check.
    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->material == "PETG");
        CHECK(staged->spoolman_id == 42);
    }
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Boot path step 2: Adventurer5M.json finally arrives with the real
    // firmware color. This is the FIRST real firmware reading for the slot,
    // so it MUST establish the baseline rather than be misread as a swap.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#898989", "ffmType1": "PLA"}
    })");

    // Override survives: brand + spoolman_id still present, user's chosen
    // material (PETG) still wins over firmware-reported PLA.
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.material == "PETG"); // user override, not firmware PLA
        CHECK(info.color_rgb == 0x898989u);
    }
    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->material == "PETG");
    }
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x898989u);

    // The slot is now loaded (firmware reported a real color). parse_adventurer_json
    // no longer owns presence — in production the post-parse GET_ZCOLOR establishes
    // it. Seed it here so the SUBSEQUENT external-edit parse below takes the sync
    // path. (Seeding only now preserves the phantom-baseline guard checked above:
    // the empty-colors_ parse_vars in step 1 still must not clear the override.)
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // Boot path step 3: a SUBSEQUENT firmware parse with a different color
    // is an external edit — sync override + lane_data, KEEP brand metadata.
    // (Pre-lane_data-sync rework, this branch interpreted the color delta as
    // a physical-swap signal and wiped the override entirely; that behavior
    // is what caused the lane_data sync regression compulsivejohnny hit.)
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.color_rgb == 0xFF5500u);
        // External edit changed material to firmware truth — override.material
        // is synced too, since material is firmware-owned for AD5X-IFS (it has
        // to be in zmod's whitelist or the firmware errors). Brand metadata
        // is the only thing the user owns independently and it persists.
        CHECK(info.material == "PLA");
    }
    auto staged3 = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged3.has_value());
    CHECK(staged3->brand == "Polymaker");
    CHECK(staged3->color_rgb == 0xFF5500u);
    CHECK(staged3->material == "PLA");
}

TEST_CASE("AD5X IFS first firmware color observation does NOT clear override",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Even when the override's saved color differs from what the firmware
    // reports at startup, the very first observation is a BASELINE. This
    // matches real-world startup: override loaded from lane_data arrives
    // before firmware is polled; the colors may not match exactly
    // (rounding, scheme differences). We must not clear on first observation.
    Ad5xIfsTmpCacheDir tmp("task11_first_observation_no_clear");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value(
        "lane_data", "lane1",
        nlohmann::json{
            {"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500; // saved = orange
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a DIFFERENT color on the FIRST observation — no prior
    // baseline, so this must NOT trigger a clear.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PLA"}
    })");

    {
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->brand == "Polymaker");
        CHECK(staged->spoolman_id == 42);
    }
    // DB entry preserved.
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Steady state: a SECOND parse of the SAME firmware color that was used
    // to establish the baseline must ALSO not clear. This locks in the
    // invariant that the baseline-first-observation path doesn't leave
    // last_firmware_color_ in a weird state that fires on unchanged polls.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PLA"}
    })");

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

// ------------------------------------------------------------------
// Task 11 bug fix regression coverage: set_slot_info must not wipe the
// override it just staged. Before the fix, set_slot_info staged the
// new override, then update_slot_from_state -> check_external_color_change
// compared the user's new color against the prior firmware baseline and
// wiped the freshly-staged override.
// ------------------------------------------------------------------

TEST_CASE("AD5X IFS set_slot_info(persist=true) does not wipe override on color edit",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Baseline firmware parse establishes last_firmware_color_.
    // Then user saves a new override with a DIFFERENT color.
    // The override must survive — not get treated as a hardware swap.
    Ad5xIfsTmpCacheDir tmp("task11_set_slot_info_persist_true_no_wipe");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // First parse: color FF5500, no override — establishes baseline.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // User edits: sets NEW color + brand. Before the fix this triggered a
    // spurious "physical swap" clear because the new color (0x00FF00)
    // differed from the baseline (0xFF5500).
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PLA";
    edit.brand = "Polymaker";
    backend.set_slot_info(0, edit, /*persist=*/true);

    // Assert override is present and unharmed.
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.color_rgb == 0x00FF00u);
    }
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->color_rgb == 0x00FF00u);

    // Baseline should have advanced to the user's chosen color.
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x00FF00u);

    // Follow-up firmware parse with the NEW color should also not clear
    // (baseline now matches — no swap signal).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PLA"}
    })");
    auto staged2 = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged2.has_value());
    CHECK(staged2->brand == "Polymaker");
}

TEST_CASE("AD5X IFS set_slot_info(persist=false) preview does not wipe existing override",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Seed a pre-existing override, establish baseline via firmware parse,
    // then preview a DIFFERENT color with persist=false. The preview must
    // not be misread as a physical swap — the saved override must remain
    // in overrides_.
    Ad5xIfsTmpCacheDir tmp("task11_set_slot_info_persist_false_preview_no_wipe");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed a pre-existing override that matches the upcoming firmware parse.
    helix::ams::FilamentSlotOverride saved;
    saved.brand = "Polymaker";
    saved.spool_name = "PolyLite Orange";
    saved.spoolman_id = 42;
    saved.material = "PLA";
    saved.color_rgb = 0xFF5500;
    Ad5xIfsTestAccess::seed_override(backend, 0, saved);

    // Firmware parse establishes baseline at the override's color.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Preview: persist=false with a DIFFERENT color. Before the fix, the
    // upcoming update_slot_from_state call flagged this as a physical swap
    // and wiped the pre-existing override.
    SlotInfo preview;
    preview.color_rgb = 0x00FF00;
    preview.material = "PLA";
    backend.set_slot_info(0, preview, /*persist=*/false);

    // The previously saved override must still exist in overrides_.
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0xFF5500u);

    // Baseline should have advanced to the previewed color, so a subsequent
    // parse that mirrors the preview color reads as "no change" and doesn't
    // clear either. (Saved override still wins until persist=true is called.)
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x00FF00u);
}

TEST_CASE("AD5X IFS firmware color unchanged across parses does NOT clear",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Parse twice with the same firmware color — no clear should fire on
    // either iteration (baseline then unchanged).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
}

TEST_CASE("AD5X IFS firmware color change with no override creates a minimal one",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // No override seeded and no override store wired up — sync_override_to_
    // firmware_locked still creates the in-memory minimal override (and
    // skips save_async cleanly when override_store_ is null). This proves
    // the helper is null-safe and that lane_data publication doesn't gate
    // the in-memory sync.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // parse_adventurer_json no longer owns presence (GET_ZCOLOR does); seed it so
    // the external-color-change sync path runs as it does in production after the
    // post-parse GET_ZCOLOR. (Baseline parse never syncs — first observation.)
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);

    // First parse establishes baseline only — no sync.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.color_rgb == 0xFF5500u);
        CHECK(info.brand.empty());
    }
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Second parse: external color change. Minimal override created (carries
    // color + material only; brand etc. left at defaults).
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}
    })");
    {
        auto info = backend.get_slot_info(0);
        CHECK(info.color_rgb == 0x0055FFu);
        CHECK(info.material == "PETG");
        CHECK(info.brand.empty());
    }
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->color_rgb == 0x0055FFu);
    CHECK(staged->material == "PETG");
    CHECK(staged->brand.empty());
}

TEST_CASE("AD5X IFS nullopt firmware color does not update the baseline",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // nullopt is the explicit "no reading" signal (empty colors_[idx] —
    // parse hasn't filled yet, transient JSON race). Must not update the
    // baseline. If it did, an intermittent empty poll would mask a real
    // subsequent swap (the next non-empty poll would look like a transition
    // from a phantom baseline) OR worse, clear on every unread poll.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports FF5500 — baseline established.
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF5500);
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // No reading (nullopt). Must NOT update baseline and must NOT clear.
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, std::nullopt);
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF5500u);
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0).has_value());

    // Firmware reports FF5500 again — still matches baseline, still no clear.
    // (If the no-reading signal had wrongly overwritten the baseline, this
    // would look like a transition and trigger a spurious sync.)
    Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF5500);
    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
}

TEST_CASE("AD5X IFS pure black (#000000) is a real reading, not a no-signal sentinel",
          "[ams][ad5x_ifs][filament_slot_override]") {
    // Regression for the IFS half of the color_set bug: external color edits
    // to pure black via Mainsail console / native LCD / CHANGE_ZCOLOR were
    // dropped because the prior `observed_color == 0` skip conflated black
    // with "no reading". With std::optional, 0 is a real reading and an
    // edit FROM another color TO black must trigger sync_override_to_firmware_locked.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("First observation = black establishes a black baseline") {
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x000000u);
    }

    SECTION("External edit FROM color TO black triggers sync") {
        // Establish baseline as red.
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0xFF0000);
        REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0xFF0000u);

        // External edit changes color to pure black. Baseline must update.
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x000000u);

        // Override should now exist with the black firmware color
        // (sync_override_to_firmware_locked created/updated it). color_set
        // must be true so to_lane_data_record actually emits "color":"#000000".
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        CHECK(staged->color_rgb == 0u);
        CHECK(staged->color_set == true);
    }

    SECTION("Steady-state black does not churn") {
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        // Subsequent observations of identical black must NOT bump the
        // override (it->second == color short-circuit).
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        Ad5xIfsTestAccess::check_external_color_change(backend, 0, 0x000000);
        REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 0) == 0x000000u);
        // First-observation rule applies: no sync was fired, so no override.
        CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    }
}

// ------------------------------------------------------------------
// Task 16: explicit clear_slot_override — user pressed "Clear slot metadata".
// Verifies the same clear pathway used by hardware-event detection is
// reachable through the public API with no swap signal required.
// ------------------------------------------------------------------

TEST_CASE("AD5X IFS clear_slot_override erases in-memory override and MR DB entry",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("task16_clear_slot_override");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed both halves of the override — lane_data on the Moonraker side
    // and the in-memory map on the backend side — so the clear has something
    // to remove at each layer.
    api.mock_set_db_value(
        "lane_data", "lane1",
        nlohmann::json{
            {"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.total_weight_g = 1000.0f;
    ovr.remaining_weight_g = 750.0f;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    // Prime with a firmware parse so slots_ has an entry to reset. This also
    // establishes the last_firmware_color_ baseline — unrelated to the
    // clear_slot_override path but good sanity for the test.
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#FF5500", "ffmType1": "PLA"}
    })");

    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
        CHECK(info.remaining_weight_g == 750.0f);
    }
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    REQUIRE(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // User presses "Clear slot metadata". Override MUST be removed from both
    // layers — no swap signal needed.
    backend.clear_slot_override(0);

    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    auto info = backend.get_slot_info(0);
    CHECK(info.brand.empty());
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.spoolman_vendor_id == 0);
    CHECK(info.remaining_weight_g < 0.0f); // -1.0 sentinel ("unknown")
    CHECK(info.total_weight_g < 0.0f);
    CHECK(info.color_name.empty());
    // Firmware-sourced color flows through — clear only touches override-exclusive fields.
    CHECK(info.color_rgb == 0xFF5500u);
    CHECK(info.material == "PLA");
}

TEST_CASE("AD5X IFS clear_slot_override is safe when no override is present",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("task16_clear_slot_override_noop");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG"}
    })");

    // No override staged. Calling clear must not crash and must leave
    // firmware-sourced fields intact.
    backend.clear_slot_override(0);

    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0x00FF00u);
    CHECK(info.material == "PETG");
}

TEST_CASE("AD5X IFS clear_slot_override rejects out-of-range indices",
          "[ams][ad5x_ifs][filament_slot_override]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Give slot 0 a real firmware entry and a staged override, so "did the
    // out-of-range call touch anything?" has something to be true of. Without
    // this the test asserts nothing: an empty backend has no state to clobber.
    // Seed BEFORE the parse - overrides are folded into the live SlotInfo by
    // update_slot_from_state, so one staged afterwards never reaches
    // get_slot_info().
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyTerra Charcoal";
    ovr.spoolman_id = 42;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);
    Ad5xIfsTestAccess::parse_adventurer_json(backend, R"({
        "FFMInfo": {"ffmColor1": "#00FF00", "ffmType1": "PETG"}
    })");
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    REQUIRE(backend.get_slot_info(0).brand == "Polymaker");

    backend.clear_slot_override(-1);
    backend.clear_slot_override(AmsBackendAd5xIfs::NUM_PORTS);
    backend.clear_slot_override(999);

    // The rejected indices must not have been folded/clamped onto a real slot.
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(Ad5xIfsTestAccess::get_override(backend, 0)->spoolman_id == 42);
    CHECK(backend.get_slot_info(0).brand == "Polymaker");

    // Positive control: the same call with an in-range index DOES clear, so a
    // clear_slot_override() that had simply stopped working could not pass the
    // assertions above by accident.
    backend.clear_slot_override(0);
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    CHECK(backend.get_slot_info(0).brand.empty());
}

// ==========================================================================
// Listener self-feedback regression (v0.99.51 GET_ZCOLOR spam loop)
// ==========================================================================
//
// zmod's GET_ZCOLOR macro body echoes RUN_ZCOLOR/CHANGE_ZCOLOR tokens through
// notify_gcode_response while our query is still in flight. Pre-fix, the
// listener treated those echoes as external state-change triggers and called
// schedule_zcolor_query() again, looping at ~2-4 Hz on the gcode console.
// The fix: when zcolor_query_active_ is set, buffer the line and return
// without re-arming a query.

TEST_CASE("AD5X IFS listener buffers RUN_ZCOLOR during in-flight query (no re-arm)",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, true);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    REQUIRE(Ad5xIfsTestAccess::zcolor_buffer_size(backend) == 0);

    // Lines that pre-fix would have re-armed the query loop.
    bool buffered_a =
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// RUN_ZCOLOR slot=2 color=FF0000");
    bool buffered_b = Ad5xIfsTestAccess::on_gcode_response_line(backend, "// CHANGE_ZCOLOR slot=3");

    CHECK(buffered_a);
    CHECK(buffered_b);
    CHECK(Ad5xIfsTestAccess::zcolor_buffer_size(backend) == 2);
    // Critically: schedule_zcolor_query must NOT have been re-armed.
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);
}

TEST_CASE("AD5X IFS listener fires schedule_zcolor_query on external RUN_ZCOLOR (no in-flight)",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // No in-flight query: the line is a genuine external state-change signal.
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    bool buffered =
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// CHANGE_ZCOLOR slot=1 color=00FF00");

    CHECK_FALSE(buffered); // Treated as external trigger, not buffered.
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);
}

// A single user color edit makes zmod re-emit its "Select print materials"
// prompt, which echoes a burst of CHANGE_ZCOLOR tokens on the console (20+ in a
// 40ms window — bundle ACJRZBXJ, an old-zmod AD5X). Pre-fix, schedule_zcolor_query
// submitted one HttpExecutor::fast() worker per line, each holding a pool slot
// through its 500ms debounce sleep while only a single query ever fired. The
// zcolor_schedule_armed_ gate coalesces the burst into one in-flight worker.
TEST_CASE("AD5X IFS coalesces a burst of color-change triggers into one debounce worker",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    const uint32_t sched_before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    const uint32_t submit_before = Ad5xIfsTestAccess::zcolor_worker_submit_count(backend);
    REQUIRE_FALSE(Ad5xIfsTestAccess::zcolor_schedule_armed(backend));

    constexpr int BURST = 20;
    for (int i = 0; i < BURST; ++i) {
        Ad5xIfsTestAccess::on_gcode_response_line(backend,
                                                  "// CHANGE_ZCOLOR SLOT=2 HEX=F72224 TYPE=PLA");
    }

    // Every trigger is seen (diagnostic counter rises by the full burst)...
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == sched_before + BURST);
    // ...but only ONE debounce worker was submitted; the rest hit the armed gate.
    CHECK(Ad5xIfsTestAccess::zcolor_worker_submit_count(backend) == submit_before + 1);
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_armed(backend));
}

TEST_CASE("AD5X IFS listener ignores unrelated gcode lines", "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);

    // Lines without RUN_ZCOLOR / CHANGE_ZCOLOR must not trigger anything.
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Extruder: 1: PLA/FF0000 | IFS: True");
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "ok");
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// 2: PETG/00FF00");

    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);
}

// ==========================================================================
// External-unload presence resurrection (zmod's own color macro)
// ==========================================================================
//
// When the user unloads a lane via zmod's native macro (AD5X LCD / Mainsail),
// the console stream contains NO RUN_ZCOLOR / CHANGE_ZCOLOR token, so the old
// listener never re-read presence and the emptied lane kept showing loaded.
// zmod's `_SET_EXTRUDER_SLOT` emits a bare, non-localized `Extruder: <N>` line
// (zmod_color.py cmd_SET_EXTRUDER_SLOT) at the channel-commit step near the end
// of the operation — we key off that to schedule one GET_ZCOLOR refresh.
//
// The match is deliberately strict (bare "Extruder: <int>" only): GET_ZCOLOR
// SILENT responses and the interactive prompt both carry a " | IFS:" suffix,
// and the per-slot rows look like "1: PLA/FFFFFF" — none match the bare form,
// so neither our own in-flight query echo nor a dialog render re-triggers.

TEST_CASE("AD5X IFS listener fires schedule_zcolor_query on external unload completion",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // No in-flight query: this is the genuine post-unload channel-commit line.
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);

    // zmod's _SET_EXTRUDER_SLOT respond_raw form, exactly as captured live
    // (raza616 AD5X, unloading lane 3 while lane 4 active).
    bool buffered = Ad5xIfsTestAccess::on_gcode_response_line(backend, "Extruder: 3");

    CHECK_FALSE(buffered); // Treated as external trigger, not buffered.
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);
}

TEST_CASE("AD5X IFS external-unload trigger tolerates the // console prefix",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);

    // Same line, but framed with Klipper's "// " comment prefix.
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Extruder: 2");

    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);
}

TEST_CASE("AD5X IFS dialog button-definition with IN_ZCOLOR does NOT re-read",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);

    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);

    // The unload dialog DEFINITION echoes IN_ZCOLOR when the prompt renders —
    // NOT when the unload executes. It must NOT schedule a query (IN_ZCOLOR is
    // not a watched token, and the line carries no bare "Extruder: N").
    Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "// action:prompt_button Unload|IN_ZCOLOR SLOT=3 NAPR=1|primary|");

    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);
}

TEST_CASE("AD5X IFS GET_ZCOLOR SILENT extruder line does NOT re-trigger (spam guard)",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Our own GET_ZCOLOR SILENT=1 is in flight: every response line (incl. the
    // "Extruder: ... | IFS:" header) must be buffered, not re-armed.
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, true);
    uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);

    bool b1 =
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Extruder: None (1) | IFS: True");
    bool b2 = Ad5xIfsTestAccess::on_gcode_response_line(backend, "// 2: PLA/2750E0");

    CHECK(b1);
    CHECK(b2);
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);

    // Even with no in-flight query, the SILENT header form (" | IFS:" suffix,
    // non-bare) must NOT match the strict external-unload trigger.
    Ad5xIfsTestAccess::set_zcolor_query_active(backend, false);
    uint32_t before2 = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Extruder: None (1) | IFS: True");
    Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Extruder: 3: PLA/2750E0 | IFS: True");
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before2);
}

// ==========================================================================
// JSON-content poll dedup
// ==========================================================================
//
// poll_adventurer_json downloads Adventurer5M.json and only parses + fires
// GET_ZCOLOR when the body has changed vs. last seen. The comparison is
// content-equality on the raw JSON string. Tests drive the comparison
// helper directly so they don't depend on a live download path.

TEST_CASE("AD5X IFS note_json_content reports changed only on different bytes",
          "[ams][ad5x_ifs][zcolor]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    const std::string a = R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})";
    const std::string b = R"({"FFMInfo":{"ffmColor1":"#00FF00","ffmType1":"PETG"}})";

    // First-ever observation is "changed" (last_json_content_ starts empty).
    CHECK(Ad5xIfsTestAccess::note_json_content(backend, a));
    // Re-observing the same content is NOT changed.
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, a));
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, a));
    // A different body flips back to changed.
    CHECK(Ad5xIfsTestAccess::note_json_content(backend, b));
    // And steady-state again on b.
    CHECK_FALSE(Ad5xIfsTestAccess::note_json_content(backend, b));
}

// ==========================================================================
// write_adventurer_json local-filesystem path
//
// Bug context: bundle DQK7X96B (AD5X stock-ZMOD, v0.99.52) showed Klipper
// shutdown with JSONDecodeError on Adventurer5M.json. Root cause: Moonraker's
// /server/files/upload writes to /root/printer_data/tmp/ then os.rename's to
// the symlinked /usr/prog/config/Adventurer5M.json — those two locations are
// on different mounts on AD5X stock-ZMOD, so rename throws EXDEV and the
// destination ends up empty. Klipper's zmod_color.py then crashes at startup
// trying to json.load() the empty file → printer bricked.
//
// Fix: when helix-screen runs on the same host as Moonraker, write the file
// directly via filesystem APIs (atomic temp+rename within the same dir).
// Only falls back to the Moonraker upload when remote.
// ==========================================================================

namespace {
struct Ad5xIfsTmpJsonFile {
    std::filesystem::path path;
    explicit Ad5xIfsTmpJsonFile(const std::string& suffix, const std::string& seed_content) {
        path = std::filesystem::temp_directory_path() /
               ("ad5x_ifs_advjson_" + suffix + "_" + std::to_string(::getpid()) + ".json");
        std::filesystem::remove(path);
        if (!seed_content.empty()) {
            std::ofstream f(path);
            f << seed_content;
        }
    }
    ~Ad5xIfsTmpJsonFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        // Cleanup any leftover temp from atomic write
        std::filesystem::remove(path.string() + ".tmp", ec);
    }
};
} // namespace

TEST_CASE("AD5X IFS write_adventurer_json_local read-modify-writes the on-disk file",
          "[ams][ad5x_ifs][local_write]") {
    // Seed a realistic Adventurer5M.json with all four slots.
    const std::string seed = R"({
    "FFMInfo": {
        "ffmColor1": "#FF0000",
        "ffmType1": "PLA",
        "ffmColor2": "#00FF00",
        "ffmType2": "PETG",
        "ffmColor3": "#0000FF",
        "ffmType3": "ABS",
        "ffmColor4": "#FFFFFF",
        "ffmType4": "PLA"
    }
})";
    Ad5xIfsTmpJsonFile tmp("rmw", seed);

    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    // Stage new color + material on slot 0 (port 1) and trigger the write.
    Ad5xIfsTestAccess::set_color(backend, 0, "AABBCC");
    Ad5xIfsTestAccess::set_material(backend, 0, "TPU");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0);
    REQUIRE(err.success());

    // Read the file back; slot 1 should be updated and other slots untouched.
    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor1"] == "#AABBCC");
    CHECK(doc["FFMInfo"]["ffmType1"] == "TPU");
    CHECK(doc["FFMInfo"]["ffmColor2"] == "#00FF00");
    CHECK(doc["FFMInfo"]["ffmType2"] == "PETG");
    CHECK(doc["FFMInfo"]["ffmColor3"] == "#0000FF");
    CHECK(doc["FFMInfo"]["ffmType3"] == "ABS");
    CHECK(doc["FFMInfo"]["ffmColor4"] == "#FFFFFF");
    CHECK(doc["FFMInfo"]["ffmType4"] == "PLA");
}

TEST_CASE("AD5X IFS write_adventurer_json_local creates FFMInfo if missing",
          "[ams][ad5x_ifs][local_write]") {
    // Adventurer5M.json without FFMInfo (zmod default-initialized empty file).
    Ad5xIfsTmpJsonFile tmp("missing_ffminfo", "{}");

    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());
    Ad5xIfsTestAccess::set_color(backend, 1, "112233");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 1);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor2"] == "#112233");
    CHECK(doc["FFMInfo"]["ffmType2"] == "PETG");
}

TEST_CASE("AD5X IFS write_adventurer_json_local rejects empty path",
          "[ams][ad5x_ifs][local_write]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // local path not set — direct write must report failure so caller falls
    // back to Moonraker upload.
    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0);
    CHECK_FALSE(err.success());
}

TEST_CASE("AD5X IFS write_adventurer_json_local rejects unparseable existing file",
          "[ams][ad5x_ifs][local_write]") {
    // Existing corrupted file (the exact symptom from bundle DQK7X96B —
    // an empty Adventurer5M.json that crashed Klipper). The local-write path
    // must NOT silently overwrite — return an error and let the caller decide
    // recovery. Empty file is not the same as missing file: missing means
    // first-time write; empty means prior corruption that we shouldn't mask.
    Ad5xIfsTmpJsonFile tmp("corrupt", "");
    // Re-touch the file as zero bytes (Ad5xIfsTmpJsonFile's empty seed_content
    // skips the file write). Create explicitly.
    {
        std::ofstream f(tmp.path);
        // intentionally empty
    }

    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0);
    // Recovery: write a fresh-baseline FFMInfo block. The corrupted-file case
    // is exactly the bricked-printer state — auto-repair from the values we
    // have in colors_/materials_ is the whole point of the direct-write fix.
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor1"] == "#FF0000");
}

TEST_CASE("AD5X IFS write_adventurer_json_local atomic — leaves no .tmp on success",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("atomic", R"({"FFMInfo":{"ffmColor1":"#000000","ffmType1":"PLA"}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());
    Ad5xIfsTestAccess::set_color(backend, 2, "ABCDEF");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 2);
    REQUIRE(err.success());

    // The atomic-rename pattern uses <path>.tmp as the staging file. After a
    // successful write the temp must be gone (rename consumes it).
    CHECK_FALSE(std::filesystem::exists(tmp.path.string() + ".tmp"));
}

// ==========================================================================
// #904: stale-plugin-data fallback + user-defined material types
// ==========================================================================

// TMTYD's printer in #904 had bambufy_tools=[4,2,4,3,...] AND
// less_waste_tools=[2,1,3,4] left over from past plugin activations, with
// neither plugin currently driving state. Without the fallback, our prefix
// detection picks bambufy first and applies [4,2,4,3,...] — putting T0 on
// port 4 and breaking every per-port T-badge in the UI. The fallback rule:
// when both prefixes have _tools arrays AND they disagree, neither is
// authoritative; revert to the default 1:1 mapping.
TEST_CASE("AD5X IFS #904 both prefixes conflict falls back to 1:1 tool map",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);

    // TMTYD's exact data: bambufy_tools and less_waste_tools both non-default
    // and disagreeing.
    json vars = json{
        {"bambufy_tools", json::array({4, 2, 4, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4})},
        {"bambufy_current_tool", -1},
        {"less_waste_tools", json::array({2, 1, 3, 4})},
        {"less_waste_current_tool", -1},
    };

    Ad5xIfsTestAccess::parse_vars(backend, vars);

    auto map = Ad5xIfsTestAccess::tool_map(backend);
    // Default 1:1 mapping — T0→port1, T1→port2, T2→port3, T3→port4, then
    // UNMAPPED_PORT for the remaining slots.
    CHECK(map[0] == 1);
    CHECK(map[1] == 2);
    CHECK(map[2] == 3);
    CHECK(map[3] == 4);
    for (size_t i = 4; i < map.size(); ++i) {
        CHECK(map[i] == AmsBackendAd5xIfs::UNMAPPED_PORT);
    }

    // Per-port T-badge: slot 3 (port 4) must show T3, NOT T0. Pre-fix this
    // was T0 because bambufy_tools[0]=4 made find_first_tool_for_port(4)=0.
    auto info3 = backend.get_slot_info(3);
    CHECK(info3.mapped_tool == 3);
}

// Single-prefix users still get their map applied (no false-positive
// fallback for users with a legitimately active plugin).
TEST_CASE("AD5X IFS #904 single-prefix non-default tools is honored",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);

    // Only bambufy_tools, with a custom mapping.
    json vars = json{
        {"bambufy_tools", json::array({3, 2, 1, 0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
        {"bambufy_current_tool", -1},
    };

    Ad5xIfsTestAccess::parse_vars(backend, vars);
    auto map = Ad5xIfsTestAccess::tool_map(backend);
    CHECK(map[0] == 3);
    CHECK(map[1] == 2);
    CHECK(map[2] == 1);
    CHECK(map[3] == 0);
}

// Both-prefixes-but-equal: no conflict, apply the map normally. (Edge case:
// a user with bambufy active whose less_waste_tools happens to match.)
TEST_CASE("AD5X IFS #904 both prefixes agree — no fallback", "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);

    auto same = json::array({2, 1, 4, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
    json vars = json{
        {"bambufy_tools", same},
        {"less_waste_tools", same},
    };

    Ad5xIfsTestAccess::parse_vars(backend, vars);
    auto map = Ad5xIfsTestAccess::tool_map(backend);
    CHECK(map[0] == 2);
    CHECK(map[1] == 1);
    CHECK(map[2] == 4);
    CHECK(map[3] == 3);
}

// Custom material types from bambufy_custom_types must surface in
// get_supported_materials() so the edit modal dropdown isn't restricted to
// the firmware whitelist (#904 root cause #2: PLA+ stomped to PLA on save).
TEST_CASE("AD5X IFS #904 bambufy_custom_types merged into supported materials",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    json vars = json{
        {"bambufy_custom_types", json::array({"PLA+", "rPLA", "PETG-Pro", "PLA-CF"})},
    };
    Ad5xIfsTestAccess::parse_vars(backend, vars);

    auto custom = Ad5xIfsTestAccess::custom_material_types(backend);
    REQUIRE(custom.size() == 4);
    CHECK(custom[0] == "PLA+");
    CHECK(custom[2] == "PETG-Pro");

    auto supported = backend.get_supported_materials();
    REQUIRE(supported.has_value());
    auto contains = [&](const std::string& s) {
        return std::find(supported->begin(), supported->end(), s) != supported->end();
    };
    // Firmware whitelist still present.
    CHECK(contains("PLA"));
    CHECK(contains("PETG-CF"));
    // User-defined types appended.
    CHECK(contains("PLA+"));
    CHECK(contains("rPLA"));
    CHECK(contains("PETG-Pro"));
    // PLA-CF was already in the whitelist — no duplicate (case-insensitive
    // dedup).
    auto count = std::count(supported->begin(), supported->end(), "PLA-CF");
    CHECK(count == 1);

    // Round-trip via normalize_material: the user-defined type must come
    // back unchanged (this is what stops the PLA+ → PLA stomp on save).
    CHECK(backend.normalize_material("PLA+") == "PLA+");
    CHECK(backend.normalize_material("pla+") == "PLA+"); // case-insensitive
}

// /mod_data/user.cfg parsing — the [zmod_ifs] filament_<NAME>: <TEMP>
// directive is zmod's official mechanism for user-defined material types
// (https://wiki.zmod.link/AD5X/#7-add-custom-filament-types). Out-of-section
// matches must NOT be picked up.
TEST_CASE("AD5X IFS #904 user.cfg [zmod_ifs] filament_* parser", "[ams][ad5x_ifs][issue_904]") {
    SECTION("standard wiki example") {
        const std::string body = "[zmod_ifs]\n"
                                 "filament_NEWTYPE: 300\n";
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types(body);
        REQUIRE(names.size() == 1);
        CHECK(names[0] == "NEWTYPE");
    }

    SECTION("multiple entries with comments and other sections") {
        const std::string body = "# global header\n"
                                 "[gcode_macro FOO]\n"
                                 "filament_IGNORED: 999  ; not in zmod_ifs\n"
                                 "\n"
                                 "[zmod_ifs]\n"
                                 "filament_PLA+: 220   # inline comment\n"
                                 "filament_RPLA: 215\n"
                                 "filament_HELIX: 240 ; semicolon comment\n"
                                 "other_setting: 42\n";
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types(body);
        REQUIRE(names.size() == 3);
        CHECK(names[0] == "PLA+");
        CHECK(names[1] == "RPLA");
        CHECK(names[2] == "HELIX");
    }

    SECTION("CRLF line endings (zmod files saved on Windows)") {
        const std::string body = "[zmod_ifs]\r\nfilament_FOO: 200\r\n";
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types(body);
        REQUIRE(names.size() == 1);
        CHECK(names[0] == "FOO");
    }

    SECTION("empty body") {
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types("");
        CHECK(names.empty());
    }

    SECTION("section without filament_ entries") {
        const std::string body = "[zmod_ifs]\nallowed_tool_count: 4\n";
        auto names = AmsBackendAd5xIfs::parse_user_cfg_filament_types(body);
        CHECK(names.empty());
    }
}

// End-to-end #904 root-cause-#2 fix: TMTYD's slot was PLA+ in zmod, our
// edit modal showed PLA, save wrote _IFS_VARS types="['PLA',...]"
// overwriting zmod's truth. The fix loads bambufy_custom_types into the
// supported-materials list so normalize_material's case-insensitive
// exact-match passes "PLA+" through unchanged. This test exercises the
// FULL save path — from save_variables ingestion through set_slot_info to
// the cached SlotInfo — to prove the round-trip doesn't stomp the user's
// chosen type.
TEST_CASE("AD5X IFS #904 PLA+ round-trips through set_slot_info after custom_types load",
          "[ams][ad5x_ifs][issue_904]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Step 1: ingest TMTYD's bambufy_custom_types from save_variables.
    json vars = json{
        {"bambufy_custom_types", json::array({"PLA+", "rPLA", "PETG-Pro", "PLA-CF"})},
    };
    Ad5xIfsTestAccess::parse_vars(backend, vars);

    // Step 2: user edits slot 0 to PLA+. set_slot_info runs normalize_material
    // internally, which (post-fix) sees "PLA+" in the supported list and
    // returns it unchanged. Pre-fix this returned "PLA" (compat_group fallback)
    // and silently destroyed the user's choice.
    SlotInfo edit;
    edit.color_rgb = 0x000DFF;
    edit.material = "PLA+";
    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    // Step 3: read it back and confirm PLA+ survived.
    auto info = backend.get_slot_info(0);
    CHECK(info.material == "PLA+");
    CHECK(info.color_rgb == 0x000DFF);

    // Lowercase input also round-trips (zmod COLOR macro is case-insensitive
    // when matching user.cfg types; our modal preserves the canonical case
    // from the supported list).
    SlotInfo edit_lc;
    edit_lc.color_rgb = 0xABCDEF;
    edit_lc.material = "rpla";
    err = backend.set_slot_info(1, edit_lc, /*persist=*/false);
    REQUIRE(err.success());
    auto info1 = backend.get_slot_info(1);
    CHECK(info1.material == "rPLA");
}

// ==========================================================================
// Cold per-lane eject / recover (#996)
//
// eject_lane fires zmod's clamp / cold-retract / unclamp trio for one idle
// lane: IFS_F24 (clamp), IFS_F11 LEN=<tube> SPEED=<ifs_speed> (cold retract),
// IFS_F39 (unclamp) — mirroring zmod's _REMOVE_PRUTOK_IFS macro. LEN/SPEED come
// from /mod_data/filament.json keyed by the lane's material, falling back to
// the file's "default" entry, then to a hardcoded 1000/1200. Ports are 1-based;
// HelixScreen slot_index is 0-based, so PRUTOK = slot_index + 1.
// ==========================================================================

TEST_CASE("AD5X IFS parse_filament_json populates per-material LEN/SPEED with fallback",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // default + PETG (explicit 650 tube) + PLA (no tube field → default tube).
    const std::string json = R"({
        "default": {"filament_tube_length": 1000, "filament_ifs_speed": 1200},
        "PETG":    {"filament_tube_length": 650,  "filament_ifs_speed": 1100},
        "PLA":     {"filament_ifs_speed": 900}
    })";
    Ad5xIfsTestAccess::parse_filament_json(backend, json);

    // PETG: both fields present.
    auto petg = Ad5xIfsTestAccess::filament_eject_params(backend, "PETG");
    REQUIRE(petg.has_value());
    CHECK(petg->first == 650);
    CHECK(petg->second == 1100);

    // PLA: missing tube length falls back to the default's tube length (1000),
    // but its own ifs speed (900) is honored.
    auto pla = Ad5xIfsTestAccess::filament_eject_params(backend, "PLA");
    REQUIRE(pla.has_value());
    CHECK(pla->first == 1000);
    CHECK(pla->second == 900);

    // Parsed default pair is available for unknown materials.
    auto def = Ad5xIfsTestAccess::filament_eject_default(backend);
    CHECK(def.first == 1000);
    CHECK(def.second == 1200);
}

TEST_CASE("AD5X IFS parse_filament_json with no default uses 1000/1200 literal fallback",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // No "default" entry and the one material lacks both fields → everything
    // resolves to the literal 1000/1200.
    const std::string json = R"({"ABS": {}})";
    Ad5xIfsTestAccess::parse_filament_json(backend, json);

    auto def = Ad5xIfsTestAccess::filament_eject_default(backend);
    CHECK(def.first == 1000);
    CHECK(def.second == 1200);

    auto abs = Ad5xIfsTestAccess::filament_eject_params(backend, "ABS");
    REQUIRE(abs.has_value());
    CHECK(abs->first == 1000);
    CHECK(abs->second == 1200);

    // Unknown material isn't cached → eject_lane falls back to the default pair.
    CHECK_FALSE(Ad5xIfsTestAccess::filament_eject_params(backend, "TPU").has_value());
}

namespace {
// Captures issued G-code without a live Moonraker connection by overriding the
// virtual execute_gcode(). eject_lane() must route through execute_gcode()
// (NOT ensure_homed_then(), which is non-virtual and would attempt real homing),
// so this subclass is sufficient to assert exactly what eject_lane() sends.
class TestableAd5xIfsBackend : public AmsBackendAd5xIfs {
  public:
    TestableAd5xIfsBackend() : AmsBackendAd5xIfs(nullptr, nullptr) {}

    std::vector<std::string> captured_gcodes;
    std::function<void()> captured_completion; // on_complete from the 2-arg form

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        captured_gcodes.push_back(gcode);
        captured_completion = std::move(on_complete);
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return homed;
    }

    bool homed = true;

    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }
    bool has_gcode_containing(const std::string& needle) const {
        for (const auto& g : captured_gcodes) {
            if (g.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
};
} // namespace

TEST_CASE("AD5X IFS unload non-active slot with IFS_STATUS Chan ejects that lane (raza D8Z7DAA6)",
          "[ams][ad5x_ifs]") {
    // raza616 D8Z7DAA6: channel 4 seated (Chan=4), user picks Unload on channel 2
    // (slot 1). With current_slot now correctly 3 (from Chan), the non-active
    // unload routes to cold eject_lane on port 2 — NOT _IFS_REMOVE_CURRENT_PRUTOK
    // which would back out the actually-seated channel 4.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    REQUIRE(backend.set_tool_mapping(3, 3).success()); // tool 3 -> port 4
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // IFS_STATUS Chan=4 establishes the seated slot (3) before the unload.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 4;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    REQUIRE(backend.get_system_info().current_slot == 3);

    REQUIRE(backend.unload_filament(1).success());

    // 0-based slot 1 -> 1-based port 2, cold clamp/retract/unclamp. NOT the
    // toolhead unload (which would remove the seated channel 4).
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=2"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=2 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=2"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK"));
}

TEST_CASE("AD5X IFS reports lane-eject and force-eject support", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Cold eject is always available on AD5X IFS, including for an idle lane
    // that reports EMPTY (snapped-chunk recovery) — both caps must be true.
    REQUIRE(backend.supports_lane_eject());
    REQUIRE(backend.supports_force_eject());
}

TEST_CASE("AD5X IFS eject_lane issues clamp / retract / unclamp sequence", "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);

    // Empty cache → default 1000/1200 LEN/SPEED. 0-based slot 2 -> 1-based port 3.
    AmsError err = backend.eject_lane(2);
    REQUIRE(err.success());

    // Three separate calls, in order: clamp, retract (config-sourced LEN/SPEED),
    // unclamp — mirroring zmod's _REMOVE_PRUTOK_IFS macro.
    REQUIRE(backend.captured_gcodes.size() == 3);
    REQUIRE(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=3");
    REQUIRE(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=3 LEN=1000 SPEED=1200");
    REQUIRE(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=3");

    // No heating and no homing — this is a cold idle-lane retract.
    REQUIRE_FALSE(backend.has_gcode_containing("G28"));
    REQUIRE_FALSE(backend.has_gcode_containing("SET_HEATER"));
    REQUIRE_FALSE(backend.has_gcode_containing("M104"));
    REQUIRE_FALSE(backend.has_gcode_containing("M109"));
    // Not the toolhead unload path.
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK"));
}

TEST_CASE("AD5X IFS eject_lane optimistically clears the ejected lane's presence (#1065)",
          "[ams][ad5x_ifs][1065]") {
    // Field repro (mkleersn 07-07 table): after ejecting a lane the multi-filament
    // menu kept showing it loaded and still offered Eject. Cause: eject_lane's only
    // refresh is schedule_zcolor_query, and that confirming GET_ZCOLOR/IFS_STATUS
    // poll starves behind the blocking eject gcode on the constrained AD5X, so
    // presence never updated. The eject physically retracts the filament clear of
    // the port silk sensor, so the lane IS empty — reflect that locally at once,
    // without waiting for the poll (SILENT disabled here to model the starved poll).
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // confirming poll won't run

    // Lane 3 (slot 2) present with filament -> AVAILABLE (menu offers Eject).
    Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
    Ad5xIfsTestAccess::set_color(backend, 2, "F72224");
    Ad5xIfsTestAccess::set_material(backend, 2, "PETG");
    REQUIRE(backend.get_slot_info(2).status == SlotStatus::AVAILABLE);

    REQUIRE(backend.eject_lane(2).success());

    // Menu must update immediately: the lane reads empty (offers Load, not Eject)
    // even though the confirming poll never ran.
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
    CHECK(backend.get_slot_info(2).status == SlotStatus::EMPTY);
}

TEST_CASE("AD5X IFS eject_lane port mapping is 1-based", "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);

    REQUIRE(backend.eject_lane(0).success());
    REQUIRE(backend.captured_gcodes.size() == 3);
    REQUIRE(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=1");
    REQUIRE(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=1 LEN=1000 SPEED=1200");
    REQUIRE(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=1");

    backend.captured_gcodes.clear();
    REQUIRE(backend.eject_lane(3).success());
    REQUIRE(backend.captured_gcodes.size() == 3);
    REQUIRE(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=4");
    REQUIRE(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=4 LEN=1000 SPEED=1200");
    REQUIRE(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=4");
}

TEST_CASE("AD5X IFS eject_lane uses the lane material's tube length / ifs speed",
          "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);

    // Slot 2's material is PETG; cache says PETG retracts 650mm at 1200.
    Ad5xIfsTestAccess::set_material(backend, 2, "PETG");
    Ad5xIfsTestAccess::seed_filament_eject_params(backend, "PETG", 650, 1200);

    REQUIRE(backend.eject_lane(2).success());

    REQUIRE(backend.captured_gcodes.size() == 3);
    REQUIRE(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=3");
    REQUIRE(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=3 LEN=650 SPEED=1200");
    REQUIRE(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=3");
}

TEST_CASE("AD5X IFS eject_lane falls back to 1000/1200 when filament.json absent",
          "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);

    // No filament.json fetched (empty cache) and a lane material that isn't
    // cached anyway — eject must still fire the full 3-command sequence using
    // the hardcoded 1000/1200 fallback.
    Ad5xIfsTestAccess::set_material(backend, 1, "PLA");

    REQUIRE(backend.eject_lane(1).success());

    REQUIRE(backend.captured_gcodes.size() == 3);
    REQUIRE(backend.captured_gcodes[0] == "IFS_F24 PRUTOK=2");
    REQUIRE(backend.captured_gcodes[1] == "IFS_F11 PRUTOK=2 LEN=1000 SPEED=1200");
    REQUIRE(backend.captured_gcodes[2] == "IFS_F39 PRUTOK=2");
}

TEST_CASE("AD5X IFS eject_lane refuses the slot loaded in toolhead", "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);

    // Slot 1 is the firmware's active slot AND filament is seated at the head:
    // ejecting it cold would fight the loaded filament, so refuse.
    Ad5xIfsTestAccess::set_current_slot(backend, 1, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    AmsError err = backend.eject_lane(1);
    REQUIRE_FALSE(err.success());
    REQUIRE(err.result == AmsResult::WRONG_STATE);
    REQUIRE(backend.captured_gcodes.empty());

    // A DIFFERENT idle lane is still ejectable while slot 1 is loaded.
    REQUIRE(backend.eject_lane(2).success());
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=3"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=3 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=3"));
}

TEST_CASE("AD5X IFS eject_lane rejects out-of-range slots", "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);

    SECTION("negative slot") {
        AmsError err = backend.eject_lane(-1);
        REQUIRE_FALSE(err.success());
        REQUIRE(backend.captured_gcodes.empty());
    }

    SECTION("slot == NUM_PORTS (out of range)") {
        AmsError err = backend.eject_lane(AmsBackendAd5xIfs::NUM_PORTS);
        REQUIRE_FALSE(err.success());
        REQUIRE(backend.captured_gcodes.empty());
    }
}

// ==========================================================================
// unload_filament dispatch — regression guard for raza616's
// "printer homes and nothing happens" (bare IFS_REMOVE_PRUTOK is a ZMOD no-op).
// ==========================================================================
//
// Verified against raza616's on-device cfg (bundle 7AC4SDEX) and ZMOD v1.7.1
// (../zmod, AD5X-zmod-1.7.1.tgz, mod/.shell/zmod_ifs.py):
//   * bare IFS_REMOVE_PRUTOK defaults PRUTOK=0 and returns at cmd_IFS_REMOVE_PRUTOK:1104
//     — a NO-OP. We homed (ensure_homed_then) and then did nothing.
//   * REMOVE_PRUTOK_IFS PRUTOK=N unloads the *current* channel regardless of N — it
//     never unloads "port N independently."
//   * IFS_REMOVE_CURRENT_PRUTOK (cmd_..._CURRENT_PRUTOK:1144) early-returns when the
//     extruder sensor reads EMPTY (zmod_ifs.py:1149) — so when nothing is seated at
//     the nozzle it is *also* a no-op after homing. raza616 hit exactly this on .76.
//   * _IFS_REMOVE_CURRENT_PRUTOK is the firmware's own "Remove from extruder" button:
//     it self-homes, calls IFS_REMOVE_CURRENT_PRUTOK NEED_TRASH=1
//     BYPASS_TEMPERATURE_CHECK=1, resets the hotend, and refreshes color. This is what
//     we dispatch for a loaded toolhead, raw rather than via ensure_homed_then() so we
//     don't put a "Home printer first?" prompt in front of a home the macro's own
//     _G28 already covers. (_G28 is conditional on homed_axes, so ensure_homed_then()
//     would not have caused a DOUBLE home - see the load-path test below and #1248.)
// With a null client, execute_gcode() is overridden, so captured_gcodes holds exactly
// what unload_filament() sends.

TEST_CASE("AD5X IFS unload_filament dispatches the firmware _IFS_REMOVE_CURRENT_PRUTOK macro",
          "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // no async debounce task

    // Normal case: active slot loaded AND filament seated at the toolhead.
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    REQUIRE(backend.unload_filament(0).success());

    // The firmware's own macro — not the bare Python command (which skips the
    // trash drop and leaves the nozzle hot), never the no-op, never per-port.
    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode("IFS_REMOVE_CURRENT_PRUTOK")); // bare, no underscore
    REQUIRE_FALSE(backend.has_gcode("IFS_REMOVE_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK_IFS"));
    // Raw dispatch: no G28 of ours in front of the macro's own conditional _G28.
    REQUIRE_FALSE(backend.has_gcode_containing("G28"));
}

TEST_CASE("AD5X IFS unhomed load sends exactly one G28 then the load macro (#1248)",
          "[ams][ad5x_ifs][homing][1248]") {
    // #1248 read INSERT_PRUTOK_IFS's leading _G28 as an unconditional home and
    // proposed ensure_homed_then(..., skip_homing=true) to stop a double home.
    // There is no double home to stop: _G28's whole body is
    //   {% if "xyz" not in printer.toolhead.homed_axes %} _HOME {% endif %}
    // (ZMOD 1.7.1 mod/_mod/translate/*/base.cfg:88), so it no-ops once our G28
    // has homed. What the load path DOES owe the user is the "Home printer
    // first?" prompt before a loadcell-Z probing home, and that only happens
    // while skip_homing stays false.
    //
    // The existing coverage for homed=false only exercised the DECLINE branch
    // (see "declining the pre-load home confirmation..." below), so nothing
    // pinned what actually goes out on confirm. This does.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    backend.homed = false;

    // Empty prompter -> request_home_confirmation() runs on_confirm inline,
    // which is the branch under test. Installed explicitly rather than relying
    // on the slot already being clear, so shard order can't change the branch.
    ScopedHomeConfirmPrompter no_prompter{helix::ui::HomeConfirmPrompter{}};

    REQUIRE(backend.load_filament(2).success());

    // Exactly two commands, in order: our G28, then the macro. Not the macro
    // alone (that would mean skip_homing=true and a silent unprompted home),
    // and not two homes.
    REQUIRE(backend.captured_gcodes.size() == 2);
    CHECK(backend.captured_gcodes[0] == "G28");
    CHECK(backend.captured_gcodes[1] == "INSERT_PRUTOK_IFS PRUTOK=3"); // slot 2 -> port 3
    CHECK(std::count(backend.captured_gcodes.begin(), backend.captured_gcodes.end(), "G28") == 1);

    // The macro still carries its completion callback through the homed
    // detour, so the stuck-on-Purging finalize path survives an unhomed load.
    REQUIRE(backend.captured_completion != nullptr);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    // Already homed: no G28 of ours at all, macro dispatched straight through.
    TestableAd5xIfsBackend homed_backend;
    Ad5xIfsTestAccess::set_running(homed_backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(homed_backend, false);
    homed_backend.homed = true;

    REQUIRE(homed_backend.load_filament(2).success());

    REQUIRE(homed_backend.captured_gcodes.size() == 1);
    CHECK(homed_backend.captured_gcodes[0] == "INSERT_PRUTOK_IFS PRUTOK=3");
    CHECK_FALSE(homed_backend.has_gcode_containing("G28"));
}

TEST_CASE("AD5X IFS unhomed change_tool sends G28 before A_CHANGE_FILAMENT (#1248)",
          "[ams][ad5x_ifs][homing][1248]") {
    // The #1248 companion case. A_CHANGE_FILAMENT is NOT in the ZMOD tree
    // (ZMOD ships CHANGE_FILAMENT / _A_CHANGE_FILAMENT as RESPOND-only stubs
    // and swaps via INSERT_PRUTOK_IFS in zmod_color.py), so it comes from the
    // stock FlashForge config and whether it self-homes is unverified. Homing
    // first is the safe side of that unknown; this pins it so nobody flips it
    // to skip_homing=true on the strength of the load-macro analysis.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    REQUIRE(backend.set_tool_mapping(1, 1).success()); // tool 1 -> port 2
    backend.captured_gcodes.clear(); // only the change_tool dispatch is under test
    backend.homed = false;

    ScopedHomeConfirmPrompter no_prompter{helix::ui::HomeConfirmPrompter{}};

    REQUIRE(backend.change_tool(1).success());

    REQUIRE(backend.captured_gcodes.size() == 2);
    CHECK(backend.captured_gcodes[0] == "G28");
    CHECK(backend.captured_gcodes[1] == "A_CHANGE_FILAMENT CHANNEL=2");
}

TEST_CASE("AD5X IFS unload finalizes to IDLE on the macro completion ack, not the 90s timeout "
          "(raza616 stuck-on-Retract)",
          "[ams][ad5x_ifs]") {
    // The synthesized Retract phase has no sensor event, and the confirming
    // IFS_STATUS Chan==0 / GET_ZCOLOR query can silently fail on native ZMOD —
    // leaving the unload stuck at Retract until the 90s timeout flips it to
    // ERROR (bundle EE5L8LY2). The _IFS_REMOVE_CURRENT_PRUTOK macro's own
    // completion (gcode ack) is the reliable terminal signal; the dispatch must
    // carry a completion callback, and that callback must finalize to IDLE.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // no async debounce task
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    REQUIRE(backend.unload_filament(0).success());

    // Toolhead unload dispatched WITH a completion callback, and the op is in
    // flight (phase tracker active, not yet IDLE).
    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE(backend.captured_completion != nullptr);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    REQUIRE(backend.get_system_info().action != AmsAction::IDLE);

    // Macro completes -> finalize to IDLE (not ERROR, not stuck at Retract).
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/true);

    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));

    // Idempotent: a second finalize (or a late confirming query) is a no-op.
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/true);
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("AD5X IFS commanded unload clears head-loaded even with the lane still present "
          "(park-in-lane, #1065)",
          "[ams][ad5x][ifs][1065]") {
    // A normal toolhead unload retracts filament out of the hotend but parks it
    // in the lane, so the IFS silk sensor stays PRESENT. A COMMANDED unload is
    // unambiguous, so head-loaded must clear on completion regardless of lane
    // presence — otherwise the slot stays stuck LOADED (Unload offered on an
    // empty head, Load disabled). This is DISTINCT from a passive Extruder:None
    // (#995), which DOES require presence corroboration so it can't strand
    // still-seated filament — the presence guard in
    // derive_head_loaded_from_summary_locked() would leave this stuck without the
    // dedicated commanded-unload clear at finalize.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // no async debounce task
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true); // lane 1 filament present

    REQUIRE(backend.unload_filament(0).success());
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));

    // Macro completes while the lane silk sensor still reads present.
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/true);

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(Ad5xIfsTestAccess::head_filament(backend)); // head cleared on unload
    CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));    // lane still present
    CHECK(backend.get_slot_info(0).status != SlotStatus::LOADED);
}

TEST_CASE("AD5X IFS load finalizes to IDLE on the macro completion ack (raza616 stuck-on-Purging)",
          "[ams][ad5x_ifs]") {
    // Mirror of the stuck-on-Retract unload fix for the load path. INSERT_PRUTOK_IFS
    // is a linear, synchronous zmod macro (home → heat → feed → purge); it acks
    // only after the purge fully runs. The synthesized Purge phase has no sensor
    // event, and the confirming query can silently fail on native ZMOD — so the
    // macro ack is the reliable terminal signal. With a null client, ensure_homed_then
    // dispatches directly through the (overridden) execute_gcode, capturing the
    // completion callback.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    REQUIRE(backend.load_filament(2).success());

    // Load dispatched the macro WITH a completion callback; op in flight.
    REQUIRE(backend.has_gcode("INSERT_PRUTOK_IFS PRUTOK=3")); // 0-based slot 2 -> port 3
    REQUIRE(backend.captured_completion != nullptr);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    REQUIRE(backend.get_system_info().action != AmsAction::IDLE);

    // Macro completes -> finalize to IDLE (not stuck at Purge).
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/false);
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::phase_active(backend));

    // An unload-ack must NOT finalize a load (and vice versa) — guard by direction.
    REQUIRE(backend.load_filament(1).success());
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    Ad5xIfsTestAccess::finalize_op_after_macro(backend, /*is_unload=*/true); // wrong direction
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));                       // still in flight
    REQUIRE(backend.get_system_info().action != AmsAction::IDLE);
}

TEST_CASE("AD5X IFS declining the pre-load home confirmation unwinds the phase tracker, not "
          "just the action (final-review C1)",
          "[ams][ad5x_ifs][homing][confirm]") {
    // load_filament() arms HEATING + begin_phase_tracking_locked() BEFORE
    // calling ensure_homed_then() (see the block right above the
    // ensure_homed_then() call in AmsBackendAd5xIfs::load_filament()). Before
    // the C1 fix, AmsSubscriptionBackend's generic cancel-branch handler reset
    // system_info_.action to IDLE but never touched phase_tracker_.active --
    // and apply_phase_action_locked() has no `!= IDLE` guard, so the very
    // next extruder-temp frame (on_extruder_temp_locked -> apply_phase_action_
    // locked) flipped the action straight back to HEATING with a fresh
    // action_start_time_. Left alone, check_action_timeout() would latch
    // ERROR 300s later ("Filament operation timed out") on an op the user
    // explicitly declined, and check_preconditions() refuses every AMS
    // command for the whole window.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    backend.homed = false;

    // The stub prompter below resolves synchronously (declines inline, before
    // load_filament() ever returns), so operation_detail has already been
    // cleared by the time we get control back. Capture it from inside the
    // cancel callback -- i.e. the arming that load_filament() did right
    // before calling ensure_homed_then() -- to prove there was something to
    // clear, not that the field was already empty.
    std::string detail_while_pending;
    ScopedHomeConfirmPrompter guard([&](std::function<void()>, std::function<void()> cancel) {
        detail_while_pending = Ad5xIfsTestAccess::operation_detail(backend);
        cancel();
    });

    REQUIRE(backend.load_filament(0).success());
    // load_filament() arms operation_detail (e.g. "Heating nozzle")
    // via apply_phase_action_locked() in the same locked block that begins
    // phase tracking -- assert it actually got set so the post-decline check
    // below proves something was cleared, not that it was already empty.
    REQUIRE_FALSE(detail_while_pending.empty());

    // Declined before any gcode went out, and the phase tracker + action are
    // both fully unwound -- not just the action. operation_detail must also
    // be cleared: AmsState::recompute_action_detail() gives last_operation_
    // detail_ priority over the IDLE fallback, so a stale detail string
    // would keep showing under an IDLE action until the next op overwrites
    // it (mirrors the cancel() precedent a few hundred lines above).
    CHECK(backend.captured_gcodes.empty());
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(Ad5xIfsTestAccess::phase_active(backend));
    CHECK(Ad5xIfsTestAccess::operation_detail(backend).empty());

    // The whole point: prove it STAYS unwound. Feed the exact status frame
    // apply_phase_action_locked()'s caller (on_extruder_temp_locked) would
    // have used to drive the phase machine while the op was really in
    // flight. With the tracker inactive this must be a no-op; before the fix
    // this single frame flipped the action right back to HEATING.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(/*temperature=*/25.0, /*target=*/0.0));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(Ad5xIfsTestAccess::phase_active(backend));

    // Not wedged: a subsequent load still dispatches normally. homed=true
    // means ensure_homed_then() never consults the prompter at all, so the
    // still-installed decline lambda from `guard` above is never invoked.
    backend.homed = true;
    REQUIRE(backend.load_filament(1).success());
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    CHECK(backend.has_gcode("INSERT_PRUTOK_IFS PRUTOK=2"));
}

TEST_CASE("AD5X IFS load_filament sets swap_expected when another lane is seated (#1065)",
          "[ams][ad5x_ifs][phase][1065]") {
    // Drives the REAL load_filament dispatch via TestableAd5xIfsBackend (which
    // provides a mock gcode API) to verify the swap_expected flag is set
    // correctly at dispatch time. The phase-test sibling manually flips the
    // flag; this one proves load_filament actually does the flip when
    // seated_chan_ differs from the target.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // Seed ch4 (slot 3) as currently seated via the production path
    // (apply_zcolor_result on an IFS_STATUS Chan frame).
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 4;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 4);

    // Dispatch load on ch2 (slot 1). ch4 ≠ ch2 → swap_expected MUST be set.
    REQUIRE(backend.load_filament(1).success());
    REQUIRE(Ad5xIfsTestAccess::swap_expected(backend));
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE("AD5X IFS load_filament does NOT set swap_expected for a plain load (#1065 regression)",
          "[ams][ad5x_ifs][phase][1065]") {
    // Regression: when no other lane is seated, swap_expected MUST NOT flip —
    // otherwise the extended budget would mask a genuinely stuck load.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // No seated lane.
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 0);

    // Dispatch load on ch2 (slot 1). No swap → flag MUST stay false.
    REQUIRE(backend.load_filament(1).success());
    REQUIRE_FALSE(Ad5xIfsTestAccess::swap_expected(backend));
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE(
    "AD5X IFS load_filament does NOT set swap_expected when same slot is already seated (#1065)",
    "[ams][ad5x_ifs][phase][1065]") {
    // Edge case: load the SAME slot that's already seated (a re-load / resume).
    // Firmware doesn't run an implicit unload in that case, so swap_expected
    // must NOT flip — otherwise we'd mask a real failure on a same-slot reload.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 2; // ch2 (slot 1) seated
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 2);

    // Reload the SAME slot (slot 1 = ch2 = port 2). seated_chan_ == port →
    // no swap, no flag.
    REQUIRE(backend.load_filament(1).success());
    REQUIRE_FALSE(Ad5xIfsTestAccess::swap_expected(backend));
}

TEST_CASE("AD5X IFS unload_filament with empty toolhead routes to cold lane eject (7AC4SDEX)",
          "[ams][ad5x_ifs]") {
    // raza616's actual state: filament is in the lane (ifs_motion_sensor present)
    // but NOT at the extruder (head_switch_sensor empty). The toolhead unload
    // would early-return in firmware after homing — "homes and nothing happens."
    // Unload must instead pull the lane back cold via IFS_F11.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, 2, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    REQUIRE(backend.unload_filament(2).success());

    // 0-based slot 2 -> 1-based port 3, cold clamp/retract/unclamp, no heat, no homing.
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=3"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=3 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=3"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("G28"));
    REQUIRE_FALSE(backend.has_gcode_containing("M104"));
    REQUIRE_FALSE(backend.has_gcode_containing("M109"));
}

TEST_CASE("AD5X IFS load-vs-swap: INSERT_PRUTOK_IFS self-swaps, so no helix unload-before-load",
          "[ams][ad5x_ifs][load]") {
    // ZMOD's _INSERT_PRUTOK_IFS runs IFS_REMOVE_CURRENT_PRUTOK itself (no-op on
    // an empty head sensor, self-heats to the seated lane's temp otherwise), so
    // the sidebar must dispatch load_filament directly. The unload-first swap
    // path degraded to eject_lane(-1) when the head sensor read empty and the
    // requested load was silently dropped — the sidebar froze at "Heat nozzle
    // 195/200" (Vger1700, bundle Z5V4K3NL).
    TestableAd5xIfsBackend backend;

    AmsSystemInfo info;
    info.filament_loaded = true;
    info.current_slot = 0;
    REQUIRE_FALSE(backend.needs_unload_before_load(info, /*target_slot=*/1));

    info.filament_loaded = false;
    info.current_slot = -1;
    REQUIRE_FALSE(backend.needs_unload_before_load(info, /*target_slot=*/1));
}

TEST_CASE("AD5X IFS load: INSERT_PRUTOK_IFS self-heats, so backend reports auto-heat",
          "[ams][ad5x_ifs][load]") {
    // The firmware macro resolves the target lane's configured material temp and
    // does its own M104 + TEMPERATURE_WAIT. Reporting auto-heat skips the UI
    // preheat poll (whose completion the sidebar could strand) — the backend
    // phase tracker still synthesizes the Heat-nozzle step for the step bar.
    TestableAd5xIfsBackend backend;
    REQUIRE(backend.supports_auto_heat_on_load());
}

TEST_CASE("AD5X IFS unload_filament(-1) with empty toolhead resolves the active lane for the "
          "cold eject",
          "[ams][ad5x_ifs][unload]") {
    // "Unload whatever is active" with nothing at the head must eject a concrete
    // lane, not forward -1 into eject_lane (which fails validate_slot_index and
    // used to be silently discarded by the sidebar swap path).
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, 1, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    REQUIRE(backend.unload_filament(-1).success());

    // 0-based slot 1 -> 1-based port 2, cold clamp/retract/unclamp.
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=2"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=2 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=2"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
}

TEST_CASE("AD5X IFS unload_filament(-1) with empty toolhead and no known lane fails loudly",
          "[ams][ad5x_ifs][unload]") {
    // Nothing at the head, no seated channel, no active slot: there is nothing
    // to unload. Return a real error (the sidebar toasts it) instead of
    // eject_lane(-1)'s invalid-slot, which callers used to swallow.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    AmsError err = backend.unload_filament(-1);
    REQUIRE_FALSE(err.success());
    REQUIRE(err.result == AmsResult::WRONG_STATE);
    REQUIRE_FALSE(err.user_msg.empty());
    REQUIRE(backend.captured_gcodes.empty());
}

TEST_CASE("AD5X IFS unload_filament clears toolhead when firmware dropped the active slot (#995)",
          "[ams][ad5x_ifs]") {
    // current_slot == -1 (stock-ZMOD "Extruder: None") but filament is seated at
    // the head: Unload must still dispatch the current-channel toolhead unload.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    REQUIRE(backend.unload_filament(2).success());
    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode("IFS_REMOVE_PRUTOK"));
}

TEST_CASE("AD5X IFS unload_filament on a non-active slot ejects that lane, not the loaded one "
          "(raza616 HKHZFYB2)",
          "[ams][ad5x_ifs]") {
    // raza616's exact footgun: channel 3 (0-based slot 2) is loaded at the
    // toolhead; the user asks to unload channel 1 (slot 0). The old path keyed
    // off the global head sensor and fired _IFS_REMOVE_CURRENT_PRUTOK, which the
    // firmware resolves from FFMInfo.channel — so it heated the nozzle and backed
    // out the loaded channel 3, never touching channel 1, then stalled. A
    // non-active slot's filament is in the lane, so unload must cold-eject THAT
    // port instead of running the toolhead unload.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, 2, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    REQUIRE(backend.unload_filament(0).success());

    // 0-based slot 0 -> 1-based port 1, cold clamp/retract/unclamp. Crucially NOT
    // the toolhead unload, which would back out the loaded channel 3.
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=1"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=1 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=1"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("G28"));
    REQUIRE_FALSE(backend.has_gcode_containing("M109"));
}

TEST_CASE("AD5X IFS unload of the IFS_STATUS-seated slot cuts even when current_slot disagrees "
          "(raza616 #981 v0.99.80)",
          "[ams][ad5x_ifs]") {
    // raza616 v0.99.80: tapping Unload on the channel physically seated at the
    // extruder cold-ejected it (IFS_F11, no cut) — "grinds away without removing."
    //
    // Mechanism: the unload router keyed off system_info_.current_slot, which is
    // derived through tool_map_/active_tool_. On the plugin path (has_ifs_vars_),
    // the IFS_STATUS Chan seated-channel authority is gated out of that derivation
    // (it persists at the seated port while save_variables _current_tool can go
    // stale loaded-idle), so current_slot points at a DIFFERENT slot than the one
    // physically seated. Tapping the seated slot then satisfied
    // "slot_index != current_slot" and routed to the cold lane eject, which
    // grinds a seated, un-cut filament instead of the heated toolhead unload.
    //
    // Fix: IFS_STATUS Chan is stored unconditionally and the unload router treats
    // the seated channel as the active slot regardless of the tool_map_-derived
    // current_slot, so the seated slot always routes to _IFS_REMOVE_CURRENT_PRUTOK.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    // Plugin user: Chan does NOT drive current_slot (gated on !has_ifs_vars_).
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    // Stale active pointer at slot 0 while channel 4 is the one truly seated.
    Ad5xIfsTestAccess::set_current_slot(backend, 0, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // IFS_STATUS reports Chan=4 (port 4 = slot 3) as the physically seated channel.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 4;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);
    // current_slot stays stale (0) because the plugin path owns it.
    REQUIRE(backend.get_system_info().current_slot == 0);

    // User taps Unload on the seated slot 3.
    REQUIRE(backend.unload_filament(3).success());

    // Must run the heated toolhead unload (cut + retract), NOT the cold lane eject.
    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("IFS_F11"));
    REQUIRE_FALSE(backend.has_gcode_containing("IFS_F24"));
}

TEST_CASE("AD5X IFS IFS_STATUS Ports is the presence authority — no resurrection from persisted "
          "ffmColor after a false SILENT demotion (raza616 #981 EE5L8LY2)",
          "[ams][ad5x_ifs]") {
    // Bundle EE5L8LY2 (v0.99.80): ch1 is physically empty, but Adventurer5M.json
    // persists ffmColor1=#F95D73 / ffmType1=ABS across the unload (zmod never
    // blanks the colour). The user opened zmod's interactive "Select print
    // materials" colour menu — a prompt dialog — while a GET_ZCOLOR SILENT=1
    // query was in flight; HelixScreen misattributed that prompt to its own query
    // and latched zcolor_silent_supported_=false, demoting a SILENT-capable
    // device to the legacy JSON presence inference, which then resurrected ch1
    // from the persisted colour ("Slot 0 status: 1 → 2").
    //
    // IFS_STATUS "Ports" carries the RS-485 per-port presence truth — it read
    // [false,true,true,true] throughout the incident — and rides the same clean-
    // JSON response (even behind a prompt fallback). It is the presence
    // authority: once seen, the JSON colour inference must never run, so ch1
    // stays empty regardless of the SILENT latch or the persisted ffmColor1.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // worst case: already demoted

    // IFS_STATUS reports ch1 empty, ch2-4 loaded — arriving in the SAME response
    // as a prompt fallback (the user's colour menu collided with our query).
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.is_prompt_fallback = true; // GET_ZCOLOR degraded to a prompt this turn
    r.ifs_chan = 2;
    r.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{false, true, true, true};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Ports established presence: ch1 empty, ch2-4 present.
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(1).status != SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(2).status != SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(3).status != SlotStatus::EMPTY);

    // The persisted Adventurer5M.json still carries a colour/type for the empty
    // ch1. With SILENT latched off, the pre-fix code inferred presence from it
    // and resurrected ch1. Ports-as-authority must suppress that inference.
    std::string content = R"({
        "FFMInfo": {
            "channel": 2,
            "ffmColor1": "#F95D73",
            "ffmColor2": "#FFFFFF",
            "ffmColor3": "#E53935",
            "ffmColor4": "#898989",
            "ffmType1": "ABS",
            "ffmType2": "PETG",
            "ffmType3": "PETG",
            "ffmType4": "PETG"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // ch1 must STAY empty — not resurrected by the persisted colour.
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(1).status != SlotStatus::EMPTY);
}

// ==========================================================================
// External CHANGE_ZCOLOR releases a stale locked override — #981
// (native ZMOD slot colors/types "randomly revert").
// ==========================================================================
//
// raza616 (#981, bundle HKHZFYB2): slot 1 was once edited in HelixScreen (white
// PETG, persist=true -> user-locked override), then physically changed to yellow
// PLA and re-typed on the zmod screen. The #965 user-lock made
// sync_override_to_firmware_locked() skip the locked color/material, so
// apply_overrides() re-painted white PETG over firmware truth on every parse —
// presence updated (not overridden) but color/type stayed stale, exactly his
// report. A CHANGE_ZCOLOR in the gcode stream is a deliberate external edit
// (HelixScreen never emits it), so it must drop the stale override and let
// firmware truth surface.

namespace {
helix::ams::FilamentSlotOverride make_locked_override(uint32_t color_rgb,
                                                      const std::string& material) {
    helix::ams::FilamentSlotOverride ovr;
    ovr.color_rgb = color_rgb;
    ovr.color_set = true;
    ovr.material = material;
    ovr.user_locked_color = true;
    ovr.user_locked_material = true;
    return ovr;
}

// An auto-mirror override: color/material set from a prior external edit but
// NEITHER field user-locked. This is what sync_override_to_firmware_locked
// leaves behind after an external CHANGE_ZCOLOR — it masks firmware truth in
// apply_overrides exactly like a locked one, but the OverwriteAlways mirror is
// free to refresh it. Reproduces the raza616 state where an earlier edit had
// already staged an override before the failing type-then-color sequence.
helix::ams::FilamentSlotOverride make_auto_mirror_override(uint32_t color_rgb,
                                                           const std::string& material) {
    helix::ams::FilamentSlotOverride ovr;
    ovr.color_rgb = color_rgb;
    ovr.color_set = true;
    ovr.material = material;
    ovr.user_locked_color = false;
    ovr.user_locked_material = false;
    return ovr;
}
} // namespace

TEST_CASE("AD5X IFS external CHANGE_ZCOLOR clears a stale locked override so firmware wins (#981)",
          "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    // Firmware truth for slot 0: zmod already wrote yellow PLA to Adventurer5M.json.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "FEF043");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");

    // Stale Helix edit: white PETG, user-locked. Seed it, then re-run
    // update_slot_from_state (via set_color) so the override bakes into
    // entry->info — mirroring the production parse path.
    Ad5xIfsTestAccess::seed_override(backend, 0, make_locked_override(0xFFFFFF, "PETG"));
    Ad5xIfsTestAccess::set_color(backend, 0, "FEF043");

    // Precondition: the locked override masks firmware truth.
    {
        SlotInfo before = backend.get_slot_info(0);
        REQUIRE(before.material == "PETG");
        REQUIRE(before.color_rgb == 0xFFFFFF);
    }

    // Deliberate external edit on the zmod screen. SLOT is 1-based -> slot 0.
    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=1 HEX=FEF043 TYPE=PLA"));

    // The stale override is gone and firmware truth (yellow PLA) now shows.
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    SlotInfo after = backend.get_slot_info(0);
    REQUIRE(after.material == "PLA");
    REQUIRE(after.color_rgb == 0xFEF043);
}

TEST_CASE("AD5X IFS RUN_ZCOLOR (display-only) leaves a locked override intact (#981)",
          "[ams][ad5x_ifs]") {
    // RUN_ZCOLOR only renders the material menu; it is NOT an edit, so a
    // user-locked override must survive it — only CHANGE_ZCOLOR is a real change.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    Ad5xIfsTestAccess::seed_override(backend, 0, make_locked_override(0xFFFFFF, "PETG"));

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "RUN_ZCOLOR SLOT=1 HEX=46328E TYPE=PETG"));

    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR with no locked override is a harmless no-op (#981)",
          "[ams][ad5x_ifs]") {
    // External edit to a slot HelixScreen never touched: nothing to clear, and we
    // must not crash or fabricate an override.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_color(backend, 1, "0DE2A0");
    Ad5xIfsTestAccess::set_material(backend, 1, "SILK");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=2 HEX=0DE2A0 TYPE=SILK"));

    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 1).has_value());
    SlotInfo info = backend.get_slot_info(1);
    REQUIRE(info.material == "SILK");
    REQUIRE(info.color_rgb == 0x0DE2A0);
}

// ==========================================================================
// Bug B — external CHANGE_ZCOLOR must NOT wipe the user's BRAND override
// (regression from 726747c71 / #981).
//
// The #981 clear-on-external-edit path (clear_override_locked) erases the
// ENTIRE per-slot override — color, material AND brand/spool_name/spoolman_id.
// That is correct for the firmware-carried fields (color/type): an external
// CHANGE_ZCOLOR is a deliberate edit and firmware truth should win. But brand
// is metadata the firmware CANNOT carry, and the AD5X native LCD emits a bare
// `CHANGE_ZCOLOR SLOT=N TYPE=<material>` (no brand) on every load/insert. So a
// routine physical load silently resets the user's saved vendor to empty.
//
// The #1071 insert/eject presence paths already retain the override across a
// physical spool event; this external-clear path must retain brand the same
// way. Uses the persist-capable setup (real MoonrakerAPIMock + injected
// FilamentSlotOverrideStore, same as the Task 10 "stores override" test) so the
// user edit runs through the PRODUCTION set_slot_info(persist=true) path — which
// stages a user-locked override the #981 external-clear then fires on — instead
// of seeding the override directly. Then feeds the bare firmware CHANGE_ZCOLOR
// and asserts the brand survives.
// ==========================================================================
TEST_CASE("AD5X IFS external CHANGE_ZCOLOR preserves the user brand override (#981/726747c71)",
          "[ams][ad5x_ifs][filament_slot_override]") {
    Ad5xIfsTmpCacheDir tmp("bugB_brand_survives_change_zcolor");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAd5xIfs backend(&api, nullptr);
    Ad5xIfsTestAccess::set_running(backend, true);
    // Native-ZMOD path skipped (has_ifs_vars_ true) — same as the Task 10 test;
    // set_slot_info's persist write still succeeds. GET_ZCOLOR SILENT flagged
    // unsupported so schedule_zcolor_query() early-returns and the CHANGE_ZCOLOR
    // handling stays synchronous (no HttpExecutor debounce, L052).
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ifs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    Ad5xIfsTestAccess::inject_override_store(backend, std::move(store));

    // Firmware truth for slot 0: yellow PLA present.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "FEF043");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");

    // User edit through the AMS slot editor: brand "Sunlu" for a PLA spool at
    // the firmware color. set_slot_info(persist=true) stages a user-LOCKED
    // override (so the #981 external-clear path fires) that ALSO carries the
    // firmware-can't-carry brand metadata. REQUIRE success — this is the
    // precondition that must REACH the clear path (previously the repro seeded
    // the override directly, sidestepping the production persist path).
    SlotInfo edit;
    edit.brand = "Sunlu";
    edit.material = "PLA";
    edit.color_rgb = 0xFEF043;
    REQUIRE(backend.set_slot_info(0, edit, /*persist=*/true).success());

    // Precondition: the brand override is live and user-locked (so the #981
    // external-clear path will fire on the CHANGE_ZCOLOR below).
    {
        SlotInfo before = backend.get_slot_info(0);
        REQUIRE(before.brand == "Sunlu");
        auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
        REQUIRE(staged.has_value());
        REQUIRE(staged->brand == "Sunlu");
        REQUIRE(staged->user_locked_material); // material provided -> locked
    }

    // AD5X native LCD load/insert: a bare CHANGE_ZCOLOR with the material only,
    // NO brand. SLOT is 1-based -> slot 0.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=1 TYPE=PLA"));

    // REGRESSION: the firmware-can't-carry brand must survive the external
    // edit. Before the fix, clear_override_locked() wiped the whole override, so
    // the brand dropped to empty ("Generic" at the UI layer) on every load.
    SlotInfo after = backend.get_slot_info(0);
    CHECK(after.brand == "Sunlu");
    CHECK(after.material == "PLA"); // firmware truth still wins for material
    auto ovr_after = Ad5xIfsTestAccess::get_override(backend, 0);
    CHECK(ovr_after.has_value());
    if (ovr_after.has_value()) {
        CHECK(ovr_after->brand == "Sunlu");
        // The color/material user-locks are released so firmware truth wins.
        CHECK_FALSE(ovr_after->user_locked_material);
        CHECK_FALSE(ovr_after->user_locked_color);
    }
}

// ==========================================================================
// CHANGE_ZCOLOR TYPE=/HEX= in-gcode parameter extraction (#1065 v0.99.94).
//
// When zmod's COLOR macro is invoked (from Mainsail, the AD5X LCD, or — most
// commonly — HelixScreen's own ActionPromptModal rendering zmod's prompt), it
// emits `CHANGE_ZCOLOR SLOT=N [TYPE=X] [HEX=YYYYYY]` to the gcode stream.
// The TYPE=/HEX= parameters ARE the user's intent at the moment of emission,
// BEFORE any firmware propagation lag.
//
// The previous implementation deferred entirely to a follow-up GET_ZCOLOR
// SILENT=1 query to pick up the new color/material. But that query queues on
// Klipper's serial gcode line behind any running IFS operation
// (INSERT_PRUTOK_IFS, IFS_F11 eject, even unrelated long macros), so it can
// take 1-3 minutes to return — or hit the 60s RPC timeout and miss entirely
// (bundle NJB2U558: GET_ZCOLOR timed out 1m after a ch4 type change while a
// load op was running; user marked "Failed to update material type" because
// the display stayed stale for minutes).
//
// Fix: extract TYPE=/HEX= from the gcode token itself and write them
// straight to colors_/materials_ + baselines + update_slot_from_state.
// The follow-up GET_ZCOLOR becomes a confirming no-op rather than the only
// path to refresh. Closes the "material doesn't update on successive COLOR
// macros" glitch from mkleersn's 07-18 + 07-20 sheets.
// ==========================================================================

TEST_CASE(
    "AD5X IFS CHANGE_ZCOLOR TYPE= updates material visible in get_slot_info immediately (#1065)",
    "[ams][ad5x_ifs][1065]") {
    // The user's exact scenario: a slot showing PETG, COLOR macro changes
    // TYPE to ABS. Before the fix, materials_[3] only refreshed when the
    // follow-up GET_ZCOLOR returned — which can lag minutes behind a queued
    // IFS op. The CHANGE_ZCOLOR token itself carries the new TYPE; we extract
    // it directly so the UI refreshes within the same frame.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    // Disable SILENT so schedule_zcolor_query early-returns — keeps the test
    // synchronous and proves the refresh no longer depends on GET_ZCOLOR.
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 3, true);
    Ad5xIfsTestAccess::set_color(backend, 3, "FEF043");
    Ad5xIfsTestAccess::set_material(backend, 3, "PETG");

    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=4 TYPE=ABS"));

    // The UI-visible material changed synchronously — no GET_ZCOLOR roundtrip.
    SlotInfo info = backend.get_slot_info(3);
    REQUIRE(info.material == "ABS");
    // Color is unchanged (the gcode didn't carry HEX=).
    REQUIRE(info.color_rgb == 0xFEF043);
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR HEX= updates color visible in get_slot_info immediately (#1065)",
          "[ams][ad5x_ifs][1065]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_color(backend, 1, "FFFFFF");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=2 HEX=2750E0"));

    SlotInfo info = backend.get_slot_info(1);
    REQUIRE(info.color_rgb == 0x2750E0);
    REQUIRE(info.material == "PETG"); // unchanged — no TYPE= in gcode
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR TYPE= and HEX= together update both fields (#1065)",
          "[ams][ad5x_ifs][1065]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "000000");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");

    // Real zmod format — slot, type, hex all present (cf. bundle NJB2U558
    // 21:20:08 "CHANGE_ZCOLOR SLOT=4 TYPE=PLA HEX=FEF043").
    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=1 TYPE=PETG HEX=F95D73"));

    SlotInfo info = backend.get_slot_info(0);
    REQUIRE(info.material == "PETG");
    REQUIRE(info.color_rgb == 0xF95D73);
}

TEST_CASE(
    "AD5X IFS CHANGE_ZCOLOR lower-case HEX= is upper-cased to match parse_adventurer_json (#1065)",
    "[ams][ad5x_ifs][1065]") {
    // parse_adventurer_json upper-cases hex from Adventurer5M.json (line ~4042);
    // GET_ZCOLOR's parse_zcolor_silent also receives upper-case. The gcode path
    // must produce the same canonical form so the follow-up GET_ZCOLOR response
    // compares equal (no spurious "material/color changed" re-sync).
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "000000");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=1 TYPE=PETG HEX=fef043"));

    SlotInfo info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xFEF043);
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR TYPE= updates baseline so the eventual GET_ZCOLOR response is a "
          "no-op (#1065)",
          "[ams][ad5x_ifs][1065]") {
    // The whole point of the gcode-path extraction: when GET_ZCOLOR eventually
    // returns (potentially minutes later, queued behind an IFS op), its
    // color/material for this slot must EQUAL the values we already wrote —
    // so apply_zcolor_result's `colors_[idx] != parsed->hex` check skips, no
    // double-sync fires, no spurious "external edit" log entry. We verify
    // this by checking the baselines last_firmware_color_/last_firmware_material_
    // were advanced to the gcode-supplied values.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 3, true);
    Ad5xIfsTestAccess::set_color(backend, 3, "FEF043");
    Ad5xIfsTestAccess::set_material(backend, 3, "PETG");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=4 TYPE=ABS HEX=FEF043"));

    // Baselines now match the gcode-supplied values, so a subsequent
    // observation of the same values (e.g. from GET_ZCOLOR) won't trip
    // check_external_*_change's "delta vs baseline" detector.
    REQUIRE(Ad5xIfsTestAccess::last_firmware_material(backend, 3).value_or("") == "ABS");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_color(backend, 3).value_or(0) == 0xFEF043);
}

TEST_CASE("AD5X IFS successive CHANGE_ZCOLOR TYPE= macros each refresh the display (#1065)",
          "[ams][ad5x_ifs][1065]") {
    // mkleersn's exact bug pattern from the 07-18 + 07-20 sheets: successive
    // COLOR macros — change color, then change material type — fail to refresh
    // the type display. Root cause was the GET_ZCOLOR queue; with gcode-path
    // extraction both changes land synchronously and visibly.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_color(backend, 1, "FFFFFF");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    // First macro: change color (HEX=) only.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=2 HEX=2750E0"));
    {
        SlotInfo info = backend.get_slot_info(1);
        REQUIRE(info.color_rgb == 0x2750E0);
        REQUIRE(info.material == "PETG"); // still PETG
    }

    // Second macro: change type (TYPE=) only — the previously-failing case.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=2 TYPE=PLA"));
    {
        SlotInfo info = backend.get_slot_info(1);
        REQUIRE(info.material == "PLA");     // <-- this used to stay "PETG"
        REQUIRE(info.color_rgb == 0x2750E0); // color retained
    }
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR TYPE= also refreshes a stale locked override's material (#1065)",
          "[ams][ad5x_ifs][1065]") {
    // #981's override-clear path runs FIRST when a user-locked override is
    // present. The gcode TYPE=/HEX= extraction must then apply ON TOP of the
    // cleared override — otherwise apply_overrides re-paints nothing (good)
    // but the firmware-truth arrays still hold the OLD value and the UI shows
    // stale data until the eventual GET_ZCOLOR.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "FEF043");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    // Seed the override and re-run update_slot_from_state (via set_color) so
    // apply_overrides bakes the override into entry->info — mirroring the
    // production parse path and the existing #981 test's setup.
    Ad5xIfsTestAccess::seed_override(backend, 0, make_locked_override(0xFFFFFF, "PETG"));
    Ad5xIfsTestAccess::set_color(backend, 0, "FEF043");

    // Sanity: the locked override masks firmware truth before the macro.
    REQUIRE(backend.get_slot_info(0).material == "PETG");
    REQUIRE(backend.get_slot_info(0).color_rgb == 0xFFFFFF);

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=1 TYPE=ABS HEX=FEF043"));

    // Override cleared (#981 path) AND firmware truth applied synchronously:
    // material=ABS, color=FEF043, no override remaining.
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    SlotInfo info = backend.get_slot_info(0);
    REQUIRE(info.material == "ABS");
    REQUIRE(info.color_rgb == 0xFEF043);
}

TEST_CASE("AD5X IFS RUN_ZCOLOR with TYPE=/HEX= does NOT update state (display-only)",
          "[ams][ad5x_ifs][1065]") {
    // RUN_ZCOLOR is zmod's prompt-render macro — it echoes the per-slot
    // button payloads (carrying TYPE=/HEX=) but does NOT change anything.
    // The display-only contract from #981 must be preserved: no extraction,
    // no override clear, no state mutation.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "000000");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    // Seed the override and re-bake entry->info so the precondition check
    // observes the override winning (mirrors the #981 test setup).
    Ad5xIfsTestAccess::seed_override(backend, 0, make_locked_override(0xFFFFFF, "PETG"));
    Ad5xIfsTestAccess::set_color(backend, 0, "000000");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "RUN_ZCOLOR SLOT=1 HEX=F95D73 TYPE=ABS"));

    // State untouched — locked override still wins, firmware arrays unchanged.
    REQUIRE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
    SlotInfo info = backend.get_slot_info(0);
    REQUIRE(info.material == "PETG"); // override still masks
    REQUIRE(info.color_rgb == 0xFFFFFF);
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR with malformed TYPE= value doesn't crash (#1065)",
          "[ams][ad5x_ifs][1065]") {
    // Defensive: a garbled CHANGE_ZCOLOR line must not corrupt state or crash.
    // The extractor skips TYPE=/HEX= values it can't make sense of and falls
    // back to the existing SLOT=-only behavior (override clear if locked,
    // debounced re-read).
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");

    // Garbled HEX (not hex digits) — must not throw, must not write a bogus
    // color to entry->info.
    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=1 HEX=NOTHEX TYPE=PETG"));

    SlotInfo info = backend.get_slot_info(0);
    REQUIRE(info.material == "PETG");    // valid TYPE= still applies
    REQUIRE(info.color_rgb == 0xFF0000); // invalid HEX= left color unchanged
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR with no TYPE= or HEX= preserves prior behavior (#981 regression)",
          "[ams][ad5x_ifs][1065]") {
    // Regression for #981: a CHANGE_ZCOLOR carrying only SLOT= (or with
    // unrecognized parameters) must still clear a stale locked override.
    // The new TYPE=/HEX= extraction is additive; it must NOT replace the
    // existing clear-on-CHANGE_ZCOLOR behavior.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "FEF043");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    Ad5xIfsTestAccess::seed_override(backend, 0, make_locked_override(0xFFFFFF, "PETG"));
    // Re-run update_slot_from_state so apply_overrides bakes the override.
    Ad5xIfsTestAccess::set_color(backend, 0, "FEF043");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(backend, "CHANGE_ZCOLOR SLOT=1"));

    // Override cleared, firmware truth (yellow PLA) shows. No TYPE=/HEX=
    // meant no synchronous material/color write — but the next GET_ZCOLOR
    // poll still drives the refresh exactly as before.
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR HEX= refreshes a stale auto-mirror override so the new "
          "color surfaces (#1065 type-then-color)",
          "[ams][ad5x_ifs][1065]") {
    // raza616 07-22 (bundle H2X5QMCU): change a slot's type, then its color,
    // from back-to-back COLOR macros — the new color never appeared. An earlier
    // external edit had left a NON-locked auto-mirror override on the slot; the
    // gcode-path extraction wrote colors_[idx] but never refreshed that
    // override, so apply_overrides kept re-masking the fresh color with the
    // override's stale one. Fix: the fast path refreshes an EXISTING override to
    // match what it just applied.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_color(backend, 1, "FFFFFF");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");
    // Auto-mirror override from a prior external edit, pinning the OLD white.
    Ad5xIfsTestAccess::seed_override(backend, 1, make_auto_mirror_override(0xFFFFFF, "PETG"));
    // Re-bake entry->info so apply_overrides has layered the override on top,
    // mirroring the production parse path (cf. the #981 tests' setup).
    Ad5xIfsTestAccess::set_color(backend, 1, "FFFFFF");

    // Precondition: the auto-mirror override masks firmware truth.
    REQUIRE(backend.get_slot_info(1).color_rgb == 0xFFFFFF);

    // Change the color via the COLOR macro. SLOT is 1-based -> slot 1.
    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=2 TYPE=PETG HEX=2750E0"));

    // The just-tapped color surfaces (used to stay 0xFFFFFF, masked by the
    // stale override) and the refreshed override now tracks it.
    SlotInfo info = backend.get_slot_info(1);
    REQUIRE(info.color_rgb == 0x2750E0);
    REQUIRE(info.material == "PETG");
    auto ovr = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(ovr.has_value());
    REQUIRE(ovr->color_rgb == 0x2750E0);
    // The refresh must not fabricate a lock on an auto-tracked field.
    REQUIRE_FALSE(ovr->user_locked_color);
    REQUIRE_FALSE(ovr->user_locked_material);
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR does NOT create an override for an auto-tracked slot "
          "(#1065 echo-flood guard)",
          "[ams][ad5x_ifs][1065]") {
    // The refresh above must be surgical: a slot with NO override stays
    // override-free. zmod re-emits CHANGE_ZCOLOR button-definition lines on
    // every prompt render, so syncing unconditionally would fabricate an
    // auto-mirror override (and a lane_data write) for a slot the user never
    // touched. With no override, firmware truth already shows through unaided.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
    Ad5xIfsTestAccess::set_color(backend, 2, "0000FF");
    Ad5xIfsTestAccess::set_material(backend, 2, "ABS");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=3 TYPE=ABS HEX=00FF00"));

    // Value applied directly (no override to mask it) and none fabricated.
    SlotInfo info = backend.get_slot_info(2);
    REQUIRE(info.color_rgb == 0x00FF00);
    REQUIRE(info.material == "ABS");
    REQUIRE_FALSE(Ad5xIfsTestAccess::get_override(backend, 2).has_value());
}

TEST_CASE("AD5X IFS CHANGE_ZCOLOR TYPE= stops at '|' so echoed button payloads don't poison "
          "the material (#1065 raza616)",
          "[ams][ad5x_ifs][1065]") {
    // The AD5X's native screen re-echoes received commands into a quoted
    // RESPOND MSG="…", so a button payload's `|style|hex` suffix can ride right
    // up against TYPE= on a line that is NOT an action:prompt_ definition. A
    // greedy TYPE=(\S+) captured "SILK|primary|F72224" into materials_ +
    // last_firmware_material_ — the pollution seen in bundle H2X5QMCU as
    // "firmware material changed SILK|primary|F72224 -> SILK". The extractor
    // must stop the TYPE token at the '|'.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 3, true);
    Ad5xIfsTestAccess::set_color(backend, 3, "F72224");
    Ad5xIfsTestAccess::set_material(backend, 3, "SILK");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "RESPOND MSG=\"CHANGE_ZCOLOR SLOT=4 TYPE=SILK|primary|F72224\""));

    // Material is the clean token, not the button descriptor — both the
    // UI-visible value and the change-detection baseline.
    SlotInfo info = backend.get_slot_info(3);
    REQUIRE(info.material == "SILK");
    REQUIRE(Ad5xIfsTestAccess::last_firmware_material(backend, 3).value_or("") == "SILK");
}

// ==========================================================================
// COLOR-menu echo classification (#1065, raza616 bundle 482NB943).
//
// zmod renders every COLOR-macro dialog by echoing its buttons down the gcode
// console as `// action:prompt_button <label>|<gcode>|<style>[|<hex>]`. Those
// lines are the menu's OFFER LIST — the 24-swatch palette, the material
// whitelist — not a record of anything the firmware did. Feeding them to the
// CHANGE_ZCOLOR extractor applied all 24 candidates in sequence, so the slot
// landed on the LAST swatch (#161616) / the last type (PETG-CF) until the
// confirming GET_ZCOLOR corrected it ~1s later, and each phantom apply pushed
// a lane_data write (25 server.database.post_item requests queued in 300ms on
// a 473MB MIPS AD5X).
//
// The root "Select print materials" render is the opposite case: its per-slot
// rows carry a genuine four-slot firmware snapshot, emitted ~100ms after the
// user's tap — the freshest truth available anywhere in the stream.
// ==========================================================================

TEST_CASE("AD5X IFS echoed CHANGE_ZCOLOR menu buttons are offers, not edits "
          "(#1065 bundle 482NB943)",
          "[ams][ad5x_ifs][1065]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "SILK");

    // "Select color": the swatch grid, ending on black.
    for (const char* hex : {"ffffff", "fef043", "75d9f3", "161616"}) {
        REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
            backend, std::string("// action:prompt_button _ |CHANGE_ZCOLOR SLOT=1 TYPE=SILK HEX=") +
                         hex + "|primary|" + hex));
    }
    // "Select material type": the whitelist, ending on PETG-CF.
    for (const char* type : {"PLA", "TPU", "PETG-CF"}) {
        REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
            backend, std::string("// action:prompt_button ") + type +
                         "|CHANGE_ZCOLOR SLOT=1 TYPE=" + type + " HEX=F330F9|primary|F330F9"));
    }

    // The slot still shows firmware truth, not the last candidate in either list.
    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0xF330F9);
    CHECK(info.material == "SILK");
    // Baselines held too — a moved baseline makes the next clean GET_ZCOLOR read
    // look like an external edit and fires a spurious lane_data write.
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0).value_or(0) == 0xF330F9);
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0).value_or("") == "SILK");
    // And nothing was fabricated on a slot the user never committed a change to.
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("AD5X IFS COLOR-menu slot row is a firmware snapshot (#1065 bundle 482NB943)",
          "[ams][ad5x_ifs][1065]") {
    // The user taps SILK; zmod applies it and re-renders the root menu with the
    // new value ~100ms later. Before this, HelixScreen ignored that row and
    // waited on GET_ZCOLOR — which in bundle 482NB943 raced the firmware write
    // and returned the OLD type, leaving the multi-filament menu showing PETG
    // for 40 seconds (17:34:37.765 tap -> 17:35:17.342 label catch-up).
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend,
        "// action:prompt_button 1: SILK|RUN_ZCOLOR SLOT=1 HEX=F330F9 TYPE=SILK|primary|F330F9"));

    SlotInfo info = backend.get_slot_info(0);
    CHECK(info.material == "SILK");
    CHECK(info.color_rgb == 0xF330F9);
    // Baseline advanced, so the GET_ZCOLOR that lands moments later is the
    // confirming no-op it was always meant to be.
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0).value_or("") == "SILK");
}

namespace {

// The exact swatch grid zmod renders for "Select color" (bundle 482NB943,
// 17:34:19.0-19.2 and 17:35:29.6-29.9). Order matters: the bug landed the slot
// on whichever entry came LAST, so #161616 is the value that showed on screen.
const std::vector<std::string> ZMOD_PALETTE = {
    "ffffff", "fef043", "dcf478", "0acc38", "067749", "0c6283", "0de2a0", "75d9f3",
    "45a8f9", "2750e0", "46328e", "a03cf7", "f330f9", "d4b0dc", "f95d73", "f72224",
    "7c4b00", "f98d33", "fdebd5", "d3c4a3", "af7836", "898989", "bcbcbc", "161616"};

// zmod's stock material whitelist, in render order (17:34:35.849-35.886).
// PETG-CF is last, so that is the type the slot ended up displaying.
const std::vector<std::string> ZMOD_MATERIALS = {"PLA", "PLA-CF", "SILK",   "TPU",
                                                 "ABS", "PETG",   "PETG-CF"};

std::string palette_button(int slot1, const std::string& type, const std::string& hex) {
    return "// action:prompt_button _ |CHANGE_ZCOLOR SLOT=" + std::to_string(slot1) +
           " TYPE=" + type + " HEX=" + hex + "|primary|" + hex;
}

std::string material_button(int slot1, const std::string& type, const std::string& hex) {
    return "// action:prompt_button " + type + "|CHANGE_ZCOLOR SLOT=" + std::to_string(slot1) +
           " TYPE=" + type + " HEX=" + hex + "|primary|" + hex;
}

std::string slot_row(int slot1, const std::string& type, const std::string& hex) {
    return "// action:prompt_button " + std::to_string(slot1) + ": " + type +
           "|RUN_ZCOLOR SLOT=" + std::to_string(slot1) + " HEX=" + hex + " TYPE=" + type +
           "|primary|" + hex;
}

} // namespace

TEST_CASE("AD5X IFS regression: COLOR palette render never moves the slot (#1065 bundle 482NB943)",
          "[ams][ad5x_ifs][1065][regression]") {
    // Field repro. raza616 opened the COLOR macro's colour picker on slot 1 and
    // the swatch flickered to black before snapping back, over and over —
    // "multi-filament menu failed to keep up". Every one of the 24 echoed
    // candidates was applied in sequence, so the state MID-storm is the bug,
    // not just the end state. Assert after every single line.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "898989");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");
    const size_t syncs_before = Ad5xIfsTestAccess::external_sync_count(backend);

    for (const auto& hex : ZMOD_PALETTE) {
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::on_gcode_response_line(backend, palette_button(1, "PETG", hex)));
        // Not just "ends correct" — never moves at all.
        REQUIRE(backend.get_slot_info(0).color_rgb == 0x898989);
        REQUIRE(backend.get_slot_info(0).material == "PETG");
    }

    for (const auto& type : ZMOD_MATERIALS) {
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::on_gcode_response_line(backend, material_button(1, type, "898989")));
        REQUIRE(backend.get_slot_info(0).material == "PETG");
    }

    // Baselines held, so the confirming GET_ZCOLOR arriving ~1s later reads as
    // "unchanged" instead of firing a phantom "firmware color changed
    // #161616 -> #898989" the log was full of.
    CHECK(Ad5xIfsTestAccess::last_firmware_color(backend, 0).value_or(0) == 0x898989);
    CHECK(Ad5xIfsTestAccess::last_firmware_material(backend, 0).value_or("") == "PETG");
    // And zero lane_data writes: 31 echoed buttons produced 31 Moonraker DB
    // round-trips before this (25 server.database.post_item requests queued in
    // a 300ms window on a 473MB MIPS AD5X, oldest aging to 1.15s).
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) == syncs_before);
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("AD5X IFS regression: type change surfaces on the menu re-render, not 40s later "
          "(#1065 bundle 482NB943)",
          "[ams][ad5x_ifs][1065][regression]") {
    // The user tapped SILK at 17:34:37.765. zmod applied it and re-rendered the
    // root menu 70ms later carrying "1: SILK". HelixScreen ignored that row,
    // waited on the debounced GET_ZCOLOR — which raced the firmware write and
    // returned the OLD type — and then had no trigger left, so the AMS panel
    // showed PETG until 17:35:17.342. Forty seconds of a wrong label.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    for (size_t i = 0; i < 4; ++i) {
        Ad5xIfsTestAccess::set_port_presence(backend, i, true);
    }
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");
    Ad5xIfsTestAccess::set_color(backend, 1, "FFFFFF");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    // "Select material type" renders; every entry is a candidate, PETG-CF last.
    for (const auto& type : ZMOD_MATERIALS) {
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::on_gcode_response_line(backend, material_button(1, type, "F330F9")));
    }
    REQUIRE(backend.get_slot_info(0).material == "PETG"); // no phantom PETG-CF

    // User taps SILK. HelixScreen sends the gcode itself, so nothing about the
    // tap comes back down the response stream — the ONLY evidence is zmod's
    // re-render of the root menu with the new value.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, slot_row(1, "SILK", "F330F9")));
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, slot_row(2, "PETG", "FFFFFF")));

    // Label tracks on the re-render, ~100ms after the tap.
    CHECK(backend.get_slot_info(0).material == "SILK");
    CHECK(backend.get_slot_info(0).color_rgb == 0xF330F9);
    // The untouched slot stays untouched — a menu render covers all four rows,
    // so it must not churn the ones that didn't change.
    CHECK(backend.get_slot_info(1).material == "PETG");
    CHECK(backend.get_slot_info(1).color_rgb == 0xFFFFFF);
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 1).has_value());
}

TEST_CASE("AD5X IFS regression: repeated COLOR-menu renders are idempotent (#1065 bundle 482NB943)",
          "[ams][ad5x_ifs][1065][regression]") {
    // The COLOR macro re-renders its root menu on every open and after every
    // edit — bundle 482NB943 has six renders in 80 seconds. A row that resynced
    // unconditionally would write lane_data each time; only a real delta may.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");

    const size_t before = Ad5xIfsTestAccess::external_sync_count(backend);
    // First render carries a real change (PETG -> SILK): one sync.
    REQUIRE_FALSE(
        Ad5xIfsTestAccess::on_gcode_response_line(backend, slot_row(1, "SILK", "F330F9")));
    const size_t after_first = Ad5xIfsTestAccess::external_sync_count(backend);
    CHECK(after_first > before);

    // Three more identical renders: nothing new to say, nothing written.
    for (int i = 0; i < 3; ++i) {
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::on_gcode_response_line(backend, slot_row(1, "SILK", "F330F9")));
    }
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) == after_first);
    CHECK(backend.get_slot_info(0).material == "SILK");
}

TEST_CASE("AD5X IFS regression: action-submenu buttons are not slot rows (#1065 bundle 482NB943)",
          "[ams][ad5x_ifs][1065][regression]") {
    // The per-slot action submenu ("Spool 1: SILK/magenta") renders Load/Unload
    // buttons alongside Change color/Change type. None of them is a `<n>: `
    // labelled row, so none may be read as a firmware snapshot — and IN_ZCOLOR
    // button definitions in particular fire at prompt-render time, not when the
    // load actually runs.
    //
    // The buttons below deliberately carry values that DISAGREE with our cached
    // firmware state. In the field they'd agree, but then "ignored the line" and
    // "applied a value that happened to match" are indistinguishable — and the
    // assertion has to be able to tell those apart to be worth anything.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "898989");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");
    const size_t syncs_before = Ad5xIfsTestAccess::external_sync_count(backend);

    for (const char* button :
         {"// action:prompt_button Change color|CHANGE_ZCOLOR SLOT=1 TYPE=SILK|primary|F330F9",
          "// action:prompt_button Change type|CHANGE_ZCOLOR SLOT=1 HEX=F330F9|primary",
          "// action:prompt_button Load|IN_ZCOLOR SLOT=1 NAPR=0|primary",
          "// action:prompt_button Unload|IN_ZCOLOR SLOT=1 NAPR=1|primary",
          "// action:prompt_footer_button Reset colors|RESET_ZCOLOR"}) {
        REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(backend, button));
    }

    CHECK(backend.get_slot_info(0).material == "PETG");
    CHECK(backend.get_slot_info(0).color_rgb == 0x898989);
    CHECK(Ad5xIfsTestAccess::external_sync_count(backend) == syncs_before);
    CHECK_FALSE(Ad5xIfsTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("AD5X IFS COLOR-menu slot row does not clear a user-locked override "
          "(#1065 bundle 482NB943)",
          "[ams][ad5x_ifs][1065]") {
    // The #981 lock-clear is gated on a DELIBERATE external CHANGE_ZCOLOR. A
    // menu render is not an edit — every COLOR macro invocation emits four of
    // these rows, so honouring them there would drop a user's locked material
    // just for opening the dialog.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, "F330F9");
    Ad5xIfsTestAccess::set_material(backend, 0, "PETG");

    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PLA";
    ovr.user_locked_material = true;
    Ad5xIfsTestAccess::seed_override(backend, 0, ovr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend,
        "// action:prompt_button 1: SILK|RUN_ZCOLOR SLOT=1 HEX=F330F9 TYPE=SILK|primary|F330F9"));

    auto staged = Ad5xIfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->user_locked_material);
    CHECK(staged->material == "PLA");
    // The user's locked choice still wins on screen.
    CHECK(backend.get_slot_info(0).material == "PLA");
}

// ==========================================================================
// Unload busy-state latency: publish the busy action synchronously on dispatch
// so the sidebar action buttons hide / context menu disables immediately,
// closing the re-tap window that produced "Unload failed: AMS is busy"
// (debug bundle 7L44W2B7). The UI observes the backend-agnostic ams_action
// subject, which AmsState only updates on EVENT_STATE_CHANGED — so the
// dispatch path MUST emit it, not wait for the next ~1.4s status frame.
// ==========================================================================

TEST_CASE("AD5X IFS unload publishes busy state + EVENT_STATE_CHANGED on dispatch",
          "[ams][ad5x_ifs][unload]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    // Filament seated at the toolhead so unload routes to the heated toolhead
    // unload (not the cold lane-eject early return). current_slot < 0 + head
    // loaded is the unknown-origin recovery case that falls through.
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_current_slot(backend, -1, true);

    int state_changed_events = 0;
    backend.set_event_callback([&](const std::string& event, const std::string&) {
        if (event == AmsBackend::EVENT_STATE_CHANGED) {
            ++state_changed_events;
        }
    });

    AmsError err = backend.unload_filament(-1);
    REQUIRE(err.success());

    // The busy action is published synchronously, BEFORE any temp/sensor frame.
    REQUIRE(Ad5xIfsTestAccess::action(backend) != AmsAction::IDLE);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    // And an EVENT_STATE_CHANGED fired on dispatch so the ams_action subject
    // updates immediately — this is the bit that was missing.
    REQUIRE(state_changed_events >= 1);
}

TEST_CASE("AD5X IFS load publishes busy state + EVENT_STATE_CHANGED on dispatch",
          "[ams][ad5x_ifs][load]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    int state_changed_events = 0;
    backend.set_event_callback([&](const std::string& event, const std::string&) {
        if (event == AmsBackend::EVENT_STATE_CHANGED) {
            ++state_changed_events;
        }
    });

    AmsError err = backend.load_filament(0);
    REQUIRE(err.success());

    REQUIRE(Ad5xIfsTestAccess::action(backend) != AmsAction::IDLE);
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
    REQUIRE(state_changed_events >= 1);
}

// ==========================================================================
// Granular step tracker (mirror Snapmaker): the AD5X synthesizes 3 phases for
// load/unload from extruder temp + head sensor; expose them as a backend step
// model + the operation_phase subject so the right-side vertical tracker shows
// "Heat nozzle -> Cut filament -> Retract" (unload) and
// "Heat nozzle -> Feed filament -> Purge" (load).
// ==========================================================================

TEST_CASE("AD5X IFS get_operation_step_model UNLOAD is the 3-phase synth sequence",
          "[ams][ad5x_ifs][stepmodel]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    auto model = backend.get_operation_step_model(StepOperationType::UNLOAD);
    REQUIRE(model.steps.size() == 3);
    CHECK(std::string(model.steps[0].label) == "Heat nozzle");
    CHECK(std::string(model.steps[1].label) == "Cut filament");
    CHECK(std::string(model.steps[2].label) == "Retract");
    // phase_id mirrors the synthesized operation_phase index (0/1/2).
    CHECK(model.steps[0].phase_id == 0);
    CHECK(model.steps[1].phase_id == 1);
    CHECK(model.steps[2].phase_id == 2);
    // Only the heat step shows a live nozzle temperature.
    CHECK(model.steps[0].live_temp);
    CHECK_FALSE(model.steps[1].live_temp);
    CHECK_FALSE(model.steps[2].live_temp);
}

TEST_CASE("AD5X IFS get_operation_step_model LOAD is the 3-phase synth sequence",
          "[ams][ad5x_ifs][stepmodel]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    for (auto op : {StepOperationType::LOAD_FRESH, StepOperationType::LOAD_SWAP}) {
        auto model = backend.get_operation_step_model(op);
        REQUIRE(model.steps.size() == 3);
        CHECK(std::string(model.steps[0].label) == "Heat nozzle");
        CHECK(std::string(model.steps[1].label) == "Feed filament");
        CHECK(std::string(model.steps[2].label) == "Purge");
        CHECK(model.steps[0].phase_id == 0);
        CHECK(model.steps[1].phase_id == 1);
        CHECK(model.steps[2].phase_id == 2);
        CHECK(model.steps[0].live_temp);
        CHECK_FALSE(model.steps[1].live_temp);
        CHECK_FALSE(model.steps[2].live_temp);
    }
}

TEST_CASE("AD5X IFS get_operation_step_index_subject is the AmsState phase subject",
          "[ams][ad5x_ifs][stepmodel]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    for (auto op :
         {StepOperationType::LOAD_FRESH, StepOperationType::LOAD_SWAP, StepOperationType::UNLOAD}) {
        CHECK(backend.get_operation_step_index_subject(op) != nullptr);
        CHECK(backend.get_operation_step_index_subject(op) ==
              AmsState::instance().get_ams_operation_phase_subject());
    }
}

TEST_CASE("AD5X IFS phase: operation_phase advances 0->1->2 during unload",
          "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    // HEATING → step 0.
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 0);

    // Reach target → CUTTING → step 1.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 1);

    // Head drop → UNLOADING → step 2.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 2);

    // Timeout → ERROR; tracker clears the step (no active step shown).
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == -1);
}

TEST_CASE("AD5X IFS phase: operation_phase advances 0->1->2 during load",
          "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/false);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 0);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 1);

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::PURGING);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == 2);

    // PURGING has a dedicated 240s budget (#1065 Bug 2), so age past it.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(250));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    REQUIRE(Ad5xIfsTestAccess::operation_phase(backend) == -1);
}

// ==========================================================================
// Error-center bridge: IFS surfaces ERROR on timeout, current_error(), recover
// ==========================================================================

TEST_CASE("AD5X IFS error-center: timeout sets ERROR not IDLE", "[ams][ad5x_ifs][error-center]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    // Advance to CUTTING so a non-IDLE action is in-flight.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    // Timeout fires: must land on ERROR, not IDLE.
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
}

TEST_CASE("AD5X IFS error-center: current_error returns CRITICAL IFS event on ERROR",
          "[ams][ad5x_ifs][error-center]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);

    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
    REQUIRE(ev->source == helix::ErrorSource::IFS);
    REQUIRE(ev->sticky);
    REQUIRE_FALSE(ev->recovery_actions.empty());
    REQUIRE(ev->recovery_actions[0].gcode == "IFS_UNLOCK");
}

TEST_CASE("AD5X IFS error-center: recover() clears ERROR state", "[ams][ad5x_ifs][error-center]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);

    backend.recover();

    REQUIRE(backend.get_system_info().action != AmsAction::ERROR);
    REQUIRE_FALSE(backend.current_error().has_value());
}

TEST_CASE("AD5X IFS error-center: current_error returns nullopt when IDLE",
          "[ams][ad5x_ifs][error-center]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    REQUIRE_FALSE(backend.current_error().has_value());
}

// ==========================================================================
// FIX 1: firmware-dropped-pointer wrong-lane unload cut (raza616 5HR3HHS6)
// ==========================================================================
//
// current_slot < 0 (firmware dropped its active pointer) while IFS_STATUS
// "Chan" still reports a physically seated port. A tap on a NON-seated slot
// must cold-eject that lane, NOT toolhead-cut the seated one. A tap ON the
// seated slot must still take the heated toolhead unload.

TEST_CASE("AD5X IFS unload of a NON-seated slot cold-ejects when firmware dropped active pointer "
          "(raza616 5HR3HHS6)",
          "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false); // no async debounce task
    // Plugin path: Chan does not drive current_slot (gated on !has_ifs_vars_),
    // so seated_chan_ can be set independently of current_slot.
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // IFS_STATUS Chan=2 establishes the seated slot (port 2 -> slot 1).
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 2;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Firmware has dropped its active pointer (current_slot = -1) but the head
    // sensor still reads loaded.
    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // User taps Unload on slot 3 (NOT the seated slot 1).
    REQUIRE(backend.unload_filament(3).success());

    // Must cold-eject port 4 (slot 3 + 1), NOT toolhead-cut the seated slot 1.
    REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=4"));
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=4 LEN=1000 SPEED=1200"));
    REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=4"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_PRUTOK"));
}

TEST_CASE("AD5X IFS unload of the seated slot still toolhead-cuts when firmware dropped pointer "
          "(raza616 5HR3HHS6)",
          "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    // IFS_STATUS Chan=2 -> seated slot 1.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 2;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // User taps Unload on the seated slot 1 -> heated toolhead unload (cut).
    REQUIRE(backend.unload_filament(1).success());

    REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    REQUIRE_FALSE(backend.has_gcode_containing("IFS_F11"));
    REQUIRE_FALSE(backend.has_gcode_containing("IFS_F24"));
}

TEST_CASE("AD5X IFS unload routing regression: active-slot cut + non-active cold-eject preserved "
          "(FIX 1 existing-guard regression)",
          "[ams][ad5x_ifs]") {
    // current_slot >= 0 path must be untouched by the new firmware-dropped branch.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, 2, /*filament_loaded=*/true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    SECTION("active slot still toolhead-cuts") {
        REQUIRE(backend.unload_filament(2).success());
        REQUIRE(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
        REQUIRE_FALSE(backend.has_gcode_containing("IFS_F11"));
    }

    SECTION("non-active slot still cold-ejects (existing #981 guard)") {
        REQUIRE(backend.unload_filament(0).success());
        REQUIRE(backend.has_gcode("IFS_F24 PRUTOK=1"));
        REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=1 LEN=1000 SPEED=1200"));
        REQUIRE(backend.has_gcode("IFS_F39 PRUTOK=1"));
        REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
    }
}

// ==========================================================================
// FIX 2: eject_lane schedules a status re-read on success
// ==========================================================================

TEST_CASE("AD5X IFS eject_lane schedules a status re-read on success (FIX 2)", "[ams][ad5x_ifs]") {
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    // Leave zcolor_silent_supported_ at its default (true) so schedule_zcolor_query
    // passes its gate and bumps the diagnostic schedule counter synchronously.
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));

    const uint32_t before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    REQUIRE(backend.eject_lane(1).success());

    // The successful eject must request a fresh status read so the UI refreshes.
    REQUIRE(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before + 1);
}

// ==========================================================================
// FIX 3: live RS-485 authority blocks stale JSON color/type for present slots
// ==========================================================================

TEST_CASE("AD5X IFS live authority blocks stale JSON color/type for a present slot (FIX 3)",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Establish the live RS-485 authority via IFS_STATUS Ports (ifs_status_ports_seen_).
    // Slot 0 (port 1) is present; this is the authoritative presence + colour source.
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.ifs_chan = 1;
    r.ifs_ports = std::array<bool, AmsBackendAd5xIfs::NUM_PORTS>{true, false, false, false};
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "00FF00"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Live colour/material the RS-485 source owns.
    Ad5xIfsTestAccess::set_color(backend, 0, "00FF00");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    // A JSON poll carrying a DIFFERENT stale ffmColor/ffmType arrives.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FFA500",
            "ffmType1": "Silk"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // The live colour/material must survive — not be resurrected by stale cache.
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0x00FF00);
    CHECK(info.material == "PLA");
}

TEST_CASE(
    "AD5X IFS pre-SILENT JSON still seeds color/type when no live authority (FIX 3 regression)",
    "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // No live authority: SILENT demoted and IFS_STATUS Ports never seen.
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#FFA500",
            "ffmType1": "ABS"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // With no live source competing, the JSON cache IS the authority.
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0xFFA500);
    CHECK(info.material == "ABS");
}

// ==========================================================================
// FIX 4: clear-spool persists firmware-native '?' / "" sentinels
// ==========================================================================

TEST_CASE("AD5X IFS write_adventurer_json_local persists '?'/empty sentinels for a cleared slot "
          "(FIX 4a)",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("clear_sentinel",
                           R"({"FFMInfo":{"ffmColor1":"#FF0000","ffmType1":"PLA"}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    // Cleared slot: empty material, placeholder gray colour (the in-memory
    // "no colour" sentinel parse_adventurer_json maps empty ffmColor to).
    Ad5xIfsTestAccess::set_color(backend, 0, "808080");
    Ad5xIfsTestAccess::set_material(backend, 0, "");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor1"] == "");
    CHECK(doc["FFMInfo"]["ffmType1"] == "?");
}

TEST_CASE("AD5X IFS write_adventurer_json_local clears colour for an explicitly empty colour "
          "(FIX 4a)",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("clear_empty_color",
                           R"({"FFMInfo":{"ffmColor2":"#00FF00","ffmType2":"PETG"}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    // Empty colour string and empty material -> both sentinels.
    Ad5xIfsTestAccess::set_color(backend, 1, "");
    Ad5xIfsTestAccess::set_material(backend, 1, "");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 1);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor2"] == "");
    CHECK(doc["FFMInfo"]["ffmType2"] == "?");
}

TEST_CASE("AD5X IFS write_adventurer_json_local writes real colour/type for a normal slot "
          "(FIX 4a regression)",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("normal_write", R"({"FFMInfo":{}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    Ad5xIfsTestAccess::set_color(backend, 2, "AABBCC");
    Ad5xIfsTestAccess::set_material(backend, 2, "TPU");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 2);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor3"] == "#AABBCC");
    CHECK(doc["FFMInfo"]["ffmType3"] == "TPU");
}

// ==========================================================================
// A filament-present slot must never persist an EMPTY ffmColor.
//
// zmod's cmd_RUN_ZCOLOR builds the "Change type" prompt button as
// `CHANGE_ZCOLOR SLOT=n HEX={zhex}` with no TYPE= param. When ffmColor is "",
// zhex is "" and the literal gcode becomes `CHANGE_ZCOLOR SLOT=n HEX=`, which
// cmd_CHANGE_ZCOLOR rejects (both HEX and TYPE empty) AFTER it has already
// emitted `action:prompt_end`. The dialog closes and nothing reopens.
// ==========================================================================

TEST_CASE("AD5X IFS write_adventurer_json_local writes zmod's default colour when the material is "
          "set but the colour is empty",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("typed_empty_color", R"({"FFMInfo":{}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    // Real material, no colour: the poisoned combination.
    Ad5xIfsTestAccess::set_color(backend, 0, "");
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 0);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor1"] == "#161616");
    CHECK(doc["FFMInfo"]["ffmType1"] == "PLA");
}

TEST_CASE("AD5X IFS write_adventurer_json_local writes zmod's default colour when the material is "
          "set but the colour is the 808080 placeholder",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("typed_placeholder_color", R"({"FFMInfo":{}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    // 808080 is our in-memory "no colour" placeholder; it must not reach the
    // file as an empty ffmColor either.
    Ad5xIfsTestAccess::set_color(backend, 1, "808080");
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 1);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor2"] == "#161616");
    CHECK(doc["FFMInfo"]["ffmType2"] == "PETG");
}

TEST_CASE("AD5X IFS write_adventurer_json_local keeps the empty-slot sentinels when no material is "
          "set",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("empty_slot_sentinel", R"({"FFMInfo":{}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    // No material at all: the firmware-native "no filament" pair must survive.
    // A genuinely empty slot never reaches the RUN_ZCOLOR submenu, so the empty
    // ffmColor is harmless there and is what stock ZMOD itself writes.
    SECTION("empty hex") {
        Ad5xIfsTestAccess::set_color(backend, 2, "");
        Ad5xIfsTestAccess::set_material(backend, 2, "");
    }
    SECTION("808080 placeholder hex") {
        Ad5xIfsTestAccess::set_color(backend, 2, "808080");
        Ad5xIfsTestAccess::set_material(backend, 2, "");
    }

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 2);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor3"] == "");
    CHECK(doc["FFMInfo"]["ffmType3"] == "?");
}

TEST_CASE("AD5X IFS write_adventurer_json_local leaves a fully-specified slot alone",
          "[ams][ad5x_ifs][local_write]") {
    Ad5xIfsTmpJsonFile tmp("full_slot_unchanged", R"({"FFMInfo":{}})");
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, tmp.path.string());

    Ad5xIfsTestAccess::set_color(backend, 3, "FF7700");
    Ad5xIfsTestAccess::set_material(backend, 3, "ABS");

    auto err = Ad5xIfsTestAccess::write_adventurer_json_local(backend, 3);
    REQUIRE(err.success());

    std::ifstream f(tmp.path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = json::parse(ss.str());
    CHECK(doc["FFMInfo"]["ffmColor4"] == "#FF7700");
    CHECK(doc["FFMInfo"]["ffmType4"] == "ABS");
}

// ==========================================================================
// HEATING detail must not assert a target the printer never reported.
// ==========================================================================

TEST_CASE("AD5X IFS phase: HEATING detail omits the target when none is known",
          "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // No extruder frame and no RESPOND line have been seen, so there is no
    // target and no current temperature.
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::HEATING);

    const std::string detail = Ad5xIfsTestAccess::operation_detail(backend);
    CHECK(detail == "Heating nozzle");
    CHECK(detail.find("230") == std::string::npos);
}

TEST_CASE("AD5X IFS phase: HEATING detail names the live temp but no target when only the temp is "
          "known",
          "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // Heater off (target 0) but the nozzle still reads a real temperature.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(62.0, 0.0));
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    const std::string detail = Ad5xIfsTestAccess::operation_detail(backend);
    CHECK(detail == "Heating nozzle (62°C)");
    CHECK(detail.find("230") == std::string::npos);
}

TEST_CASE("AD5X IFS phase: HEATING detail still names a real target when one is known",
          "[ams][ad5x_ifs][phase]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_head_filament(backend, true);

    // A pre-op extruder frame reports a live 220°C target at 62°C. begin_phase
    // seeds from it, so the very first detail carries the REAL target.
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(62.0, 220.0));
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);

    CHECK(Ad5xIfsTestAccess::operation_detail(backend) == "Heating nozzle to 220°C (62°C)");
}

TEST_CASE("AD5X IFS parse_adventurer_json maps firmware '?' ffmType to empty material (FIX 4b)",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Firmware-native unset sentinel for slot 0 (port 1): ffmType '?'.
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "",
            "ffmType1": "?"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    // '?' must yield an empty material so the UI renders '--'.
    auto info = backend.get_slot_info(0);
    CHECK(info.material.empty());
}

TEST_CASE("AD5X IFS parse_adventurer_json maps empty ffmType to empty material (FIX 4b)",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    std::string content = R"({"FFMInfo": {"ffmColor1": "", "ffmType1": ""}})";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);
    CHECK(backend.get_slot_info(0).material.empty());
}

// ==========================================================================
// LABEL/ROUTE CONTRACT: slot_unloads_to_toolhead() (drives the context-menu
// Unload-vs-Eject label + dispatch) MUST agree with unload_filament()'s actual
// eject-vs-toolhead routing across the full authority matrix. Any drift would
// mislabel the button or dispatch the wrong action — the bug raza616 hit
// (button said "Unload", firmware cut the seated lane). 5HR3HHS6.
// ==========================================================================

namespace {
// 0-based slot mapping: port = slot + 1; seated_chan is 1-based (0 = none).
std::unique_ptr<TestableAd5xIfsBackend> make_routing_backend(int current_slot, int seated_chan,
                                                             bool head_loaded) {
    auto backend = std::make_unique<TestableAd5xIfsBackend>();
    Ad5xIfsTestAccess::set_running(*backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(*backend, false); // no async debounce task
    // Plugin path so IFS_STATUS Chan sets seated_chan_ WITHOUT driving current_slot,
    // letting us pin the two authorities independently.
    Ad5xIfsTestAccess::set_has_ifs_vars(*backend, true);
    if (seated_chan > 0) {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.ifs_chan = seated_chan;
        Ad5xIfsTestAccess::apply_zcolor_result(*backend, r);
    }
    Ad5xIfsTestAccess::set_current_slot(*backend, current_slot, /*filament_loaded=*/head_loaded);
    Ad5xIfsTestAccess::set_head_filament(*backend, head_loaded);
    return backend;
}

// Runs the real unload and reports which route it took. Asserts exactly one route.
bool unload_routed_to_toolhead(TestableAd5xIfsBackend& b, int slot) {
    REQUIRE(b.unload_filament(slot).success());
    const bool cut = b.has_gcode_containing("REMOVE_CURRENT_PRUTOK");
    const bool eject = b.has_gcode_containing("IFS_F11") || b.has_gcode_containing("IFS_F24");
    REQUIRE(cut != eject); // exactly one route, never both / neither
    return cut;
}
} // namespace

TEST_CASE("AD5X IFS slot_unloads_to_toolhead matches unload routing across the authority matrix",
          "[ams][ad5x_ifs]") {
    struct Case {
        const char* name;
        int current_slot;
        int seated_chan; // 1-based, 0 = none
        bool head_loaded;
        int slot;
        bool expect_toolhead; // true = heated cut, false = cold eject
    };
    // clang-format off
    const Case cases[] = {
        {"known active, tap active -> cut",            2,  0, true,  2, true},
        {"known active, tap other -> eject",           2,  0, true,  0, false},
        {"known active disagrees, tap seated -> cut",  2,  4, true,  3, true},  // #981 seated authority
        {"pointer lost, tap seated -> cut",           -1,  2, true,  1, true},
        {"pointer lost, tap non-seated -> eject",     -1,  2, true,  3, false}, // raza616 5HR3HHS6
        {"pointer lost, nothing seated, head -> cut", -1,  0, true,  2, true},  // unknown-origin recovery
        {"empty toolhead -> eject",                    2,  0, false, 2, false},
    };
    // clang-format on

    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.name) {
            auto backend = make_routing_backend(c.current_slot, c.seated_chan, c.head_loaded);

            // Predicate (label/dispatch) — must ignore the recovery-broadened hint.
            const bool pred_true = backend->slot_unloads_to_toolhead(c.slot, /*loaded_hint=*/true);
            const bool pred_false =
                backend->slot_unloads_to_toolhead(c.slot, /*loaded_hint=*/false);
            CHECK(pred_true == c.expect_toolhead);
            CHECK(pred_false == c.expect_toolhead); // hint is ignored for AD5X

            // Actual routing — the contract: predicate == route taken.
            const bool routed_toolhead = unload_routed_to_toolhead(*backend, c.slot);
            CHECK(routed_toolhead == c.expect_toolhead);
            CHECK(routed_toolhead == pred_true);
        }
    }
}

TEST_CASE("AD5X IFS unload of the active slot via -1 (unload active) toolhead-cuts",
          "[ams][ad5x_ifs]") {
    auto backend =
        make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/true);
    REQUIRE(backend->unload_filament(-1).success());
    CHECK(backend->has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    CHECK_FALSE(backend->has_gcode_containing("IFS_F11"));
}

// ==========================================================================
// The unload router's "is the toolhead empty" decision must come from the
// toolhead SWITCH pair, not the conflated head_filament_.
//
// parse_head_sensor() writes head_filament_ from BOTH the switch sensor and
// ifs_motion_sensor, and the motion sensor is device-confirmed to read
// filament_detected=false on a lane that is loaded but idle. Believing that
// false negative sends seated, un-cut filament to the cold IFS_F11 retract,
// which grinds it (raza616 #981; 5HR3HHS6 is the same hazard by a different
// route). The reverse error - calling a truly empty head "loaded" - only costs
// a firmware no-op, so the predicate is biased toward "loaded" on purpose.
// ==========================================================================

TEST_CASE("AD5X IFS unload: switch says present, motion says absent -> heated cut, never a cold "
          "eject (grinding regression)",
          "[ams][ad5x_ifs][unload]") {
    // THE regression this predicate exists for. The last frame to write
    // head_filament_ came from ifs_motion_sensor on a loaded-but-idle lane, so
    // head_filament_ is false while filament is physically at the nozzle. The
    // switch sensor still says present, and it is the authority.
    auto backend =
        make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/false);
    Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(*backend)); // motion false-negative in place

    // Label and dispatch must both say "heated toolhead unload".
    CHECK(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
    CHECK(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/false));

    REQUIRE(backend->unload_filament(2).success());
    CHECK(backend->has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    CHECK_FALSE(backend->has_gcode_containing("IFS_F11")); // the cold retract that grinds
    CHECK_FALSE(backend->has_gcode_containing("IFS_F24"));
}

TEST_CASE("AD5X IFS unload: the motion false-negative arrives through the real sensor plumbing",
          "[ams][ad5x_ifs][unload]") {
    // Same regression, but reached the way a device reaches it: a switch frame
    // says present, then a motion frame says absent and clobbers head_filament_
    // through parse_head_sensor(). Nothing here pokes head_filament_ directly -
    // if the conflation ever gets fixed at the source, this test stops proving
    // anything but must still pass.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

    Ad5xIfsTestAccess::handle_status(backend, make_zmod_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));

    // The conflation is real: head_filament_ now lies, the switch pair does not.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    REQUIRE(Ad5xIfsTestAccess::head_switch_seen(backend));
    REQUIRE(Ad5xIfsTestAccess::head_switch_present(backend));

    // Pin the active slot AFTER the frames — handle_status_update() recomputes
    // current_slot from tool_map_/seated_chan_ on every sensor change.
    Ad5xIfsTestAccess::set_current_slot(backend, 2, /*filament_loaded=*/true);

    CHECK(backend.slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
    REQUIRE(backend.unload_filament(2).success());
    CHECK(backend.has_gcode("_IFS_REMOVE_CURRENT_PRUTOK"));
    CHECK_FALSE(backend.has_gcode_containing("IFS_F11"));
}

TEST_CASE("AD5X IFS unload: switch seen and absent -> cold eject even when motion says present "
          "(7AC4SDEX)",
          "[ams][ad5x_ifs][unload]") {
    // Bundle 7AC4SDEX: head_switch_sensor empty, ifs_motion_sensor present.
    // The filament is in the lane, not the toolhead, and
    // _IFS_REMOVE_CURRENT_PRUTOK would early-return on the extruder sensor after
    // homing ("homes and nothing happens"). Switch-absent is authoritative, so
    // this must reach the cold retract regardless of what motion reports.
    auto backend =
        make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/true);
    Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/false);
    REQUIRE(Ad5xIfsTestAccess::head_filament(*backend)); // motion says present; switch overrules

    CHECK_FALSE(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/true));

    REQUIRE(backend->unload_filament(2).success());
    CHECK(backend->has_gcode("IFS_F24 PRUTOK=3"));
    CHECK(backend->has_gcode_containing("IFS_F11 PRUTOK=3"));
    CHECK(backend->has_gcode("IFS_F39 PRUTOK=3"));
    CHECK_FALSE(backend->has_gcode_containing("REMOVE_CURRENT_PRUTOK"));
}

TEST_CASE("AD5X IFS unload: motion-only firmware (switch never seen) keeps the historical rule",
          "[ams][ad5x_ifs][unload]") {
    // No switch sensor is published at all, so there is no positive evidence to
    // require and no behaviour to change: the routing must be exactly what
    // head_filament_ said before this predicate existed. Both polarities, so a
    // fallback that hardcoded either answer fails.
    for (bool head : {true, false}) {
        DYNAMIC_SECTION("head_filament_ = " << head) {
            auto backend =
                make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/head);
            Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/false, /*present=*/false);
            REQUIRE_FALSE(Ad5xIfsTestAccess::head_switch_seen(*backend));

            CHECK(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/true) == head);
            CHECK(unload_routed_to_toolhead(*backend, 2) == head);
        }
    }
}

TEST_CASE("AD5X IFS unload: the slot-identity branches still win over the head predicate",
          "[ams][ad5x_ifs][unload]") {
    // The non-active-slot (#981 HKHZFYB2) and dropped-pointer (5HR3HHS6) guards
    // run BEFORE the head test and are about which lane the user tapped, not
    // about the toolhead. A switch that reads present must not promote either of
    // them to a heated cut — that is the wrong-lane heat+cut both were added to
    // stop. Conversely the unknown-origin recovery case (both authorities lost)
    // must still cut.
    SECTION("known active slot, tap a different lane -> cold eject") {
        auto backend =
            make_routing_backend(/*current_slot=*/2, /*seated_chan=*/0, /*head_loaded=*/true);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);
        CHECK_FALSE(backend->slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
        CHECK_FALSE(unload_routed_to_toolhead(*backend, 0));
        CHECK(backend->has_gcode_containing("IFS_F11 PRUTOK=1"));
    }
    SECTION("pointer lost, seated known, tap a non-seated lane -> cold eject") {
        auto backend =
            make_routing_backend(/*current_slot=*/-1, /*seated_chan=*/2, /*head_loaded=*/true);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);
        CHECK_FALSE(backend->slot_unloads_to_toolhead(3, /*loaded_hint=*/true));
        CHECK_FALSE(unload_routed_to_toolhead(*backend, 3));
        CHECK(backend->has_gcode_containing("IFS_F11 PRUTOK=4"));
    }
    SECTION("pointer lost, nothing seated, switch present -> unknown-origin cut") {
        auto backend =
            make_routing_backend(/*current_slot=*/-1, /*seated_chan=*/0, /*head_loaded=*/false);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);
        CHECK(backend->slot_unloads_to_toolhead(2, /*loaded_hint=*/true));
        CHECK(unload_routed_to_toolhead(*backend, 2));
    }
}

TEST_CASE("AD5X IFS eject_lane's seated refusal uses the same head predicate as the router",
          "[ams][ad5x_ifs][unload]") {
    // The two must agree or they deadlock against each other: the router hands
    // an empty-head unload to eject_lane(), and a refusal keyed on a different
    // notion of "empty" would bounce it back with "Unload from toolhead first".
    SECTION("switch present, motion false-negative -> a direct Eject tap is refused") {
        auto backend =
            make_routing_backend(/*current_slot=*/1, /*seated_chan=*/0, /*head_loaded=*/false);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/true);

        AmsError err = backend->eject_lane(1);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(err.user_msg == "Unload from toolhead first");
        CHECK(backend->captured_gcodes.empty()); // no cold retract against seated filament
    }
    SECTION("switch absent, motion says present -> the router's eject is NOT bounced") {
        auto backend =
            make_routing_backend(/*current_slot=*/1, /*seated_chan=*/0, /*head_loaded=*/true);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/true, /*present=*/false);

        REQUIRE(backend->unload_filament(1).success());
        CHECK(backend->has_gcode_containing("IFS_F11 PRUTOK=2"));
    }
    SECTION("motion-only firmware keeps the historical head_filament_ refusal") {
        auto backend =
            make_routing_backend(/*current_slot=*/1, /*seated_chan=*/0, /*head_loaded=*/true);
        Ad5xIfsTestAccess::set_head_switch(*backend, /*seen=*/false, /*present=*/false);

        AmsError err = backend->eject_lane(1);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
    }
}

TEST_CASE("AD5X IFS eject_lane maps each slot to its 1-based port", "[ams][ad5x_ifs]") {
    for (int slot = 0; slot < AmsBackendAd5xIfs::NUM_PORTS; ++slot) {
        DYNAMIC_SECTION("slot " << slot) {
            TestableAd5xIfsBackend backend;
            Ad5xIfsTestAccess::set_running(backend, true);
            Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
            REQUIRE(backend.eject_lane(slot).success());
            const std::string port = std::to_string(slot + 1);
            CHECK(backend.has_gcode("IFS_F24 PRUTOK=" + port));
            CHECK(backend.has_gcode_containing("IFS_F11 PRUTOK=" + port));
            CHECK(backend.has_gcode("IFS_F39 PRUTOK=" + port));
        }
    }
}

TEST_CASE("AMS base slot_unloads_to_toolhead defaults to the loaded hint (no AD5X override)",
          "[ams][ad5x_ifs]") {
    // Non-AD5X backends keep the legacy rule: toolhead unload iff the menu's
    // is_loaded snapshot says so. AD5X overrides; this guards the default.
    AmsBackendAfc afc(nullptr, nullptr);
    CHECK(afc.slot_unloads_to_toolhead(0, /*loaded_hint=*/true));
    CHECK_FALSE(afc.slot_unloads_to_toolhead(0, /*loaded_hint=*/false));
}

// ============================================================================
// Layer 2 homing guard: refuse toolhead-motion filament ops during a print.
//
// AD5X's _IFS_REMOVE_CURRENT_PRUTOK (unload) self-homes (G28) INSIDE the ZMOD
// firmware macro. Layer 1 (the gcode-send guard) never sees that buried _G28, so
// load/unload/change_tool must be refused before they start whenever a print is
// PRINTING or PAUSED. No-motion ops (eject_lane, select_slot) stay allowed.
// ============================================================================

namespace {

// Backend double that keeps a real (mock) api_ so check_preconditions() can read
// the print-job state via api_->printer_state(), while still capturing any gcode
// the action path would emit (so "sent nothing" is verifiable).
class Ad5xHomingGuardBackend : public AmsBackendAd5xIfs {
  public:
    Ad5xHomingGuardBackend(MoonrakerAPI* api, helix::MoonrakerClient* client)
        : AmsBackendAd5xIfs(api, client) {}

    std::vector<std::string> captured_gcodes;
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        captured_gcodes.push_back(gcode);
        (void)on_complete;
        return AmsErrorHelper::success();
    }
};

struct Ad5xHomingGuardFixture : public LVGLTestFixture {
    Ad5xHomingGuardFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
        backend = std::make_unique<Ad5xHomingGuardBackend>(api.get(), &mock_client);
        // check_preconditions() short-circuits on a stopped backend; flip running_
        // so the print-active gate is actually reached.
        Ad5xIfsTestAccess::set_running(*backend, true);
    }
    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }
    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<Ad5xHomingGuardBackend> backend;
};

} // namespace

TEST_CASE_METHOD(Ad5xHomingGuardFixture,
                 "check_preconditions gates toolhead motion on active print",
                 "[ams][ad5x_ifs][homing_guard]") {
    SECTION("PRINTING refuses motion ops but allows no-motion ops") {
        set_print_state(helix::PrintJobState::PRINTING);
        AmsError motion = backend->check_preconditions(/*requires_toolhead_motion=*/true);
        CHECK_FALSE(motion.success());
        CHECK(motion.result == AmsResult::WRONG_STATE);
        CHECK(motion.user_msg == "Cannot run filament operation while printing");
        // Case 7: no-motion ops (eject_lane / select / unlock) are NOT blocked.
        CHECK(backend->check_preconditions(/*requires_toolhead_motion=*/false).success());
    }

    SECTION("PAUSED refuses motion ops (head parked over the print)") {
        set_print_state(helix::PrintJobState::PAUSED);
        AmsError motion = backend->check_preconditions(true);
        CHECK_FALSE(motion.success());
        // A runout pause is the case users actually hit: Klipper prints "load it
        // and press RESUME", so they reach for Load. "while printing" reads as a
        // bug and "finish or cancel the print" is the opposite of what they want.
        // Point them at the recovery that works instead (bundle JX2FVRB9).
        CHECK(motion.user_msg == "Can't move filament while the print is paused");
        CHECK(motion.suggestion.find("Resume") != std::string::npos);
    }

    SECTION("STANDBY allows motion ops") {
        set_print_state(helix::PrintJobState::STANDBY);
        CHECK(backend->check_preconditions(true).success());
    }

    SECTION("COMPLETE allows motion ops") {
        set_print_state(helix::PrintJobState::COMPLETE);
        CHECK(backend->check_preconditions(true).success());
    }
}

TEST_CASE_METHOD(Ad5xHomingGuardFixture,
                 "load/unload/change_tool refuse and emit nothing while printing",
                 "[ams][ad5x_ifs][homing_guard]") {
    set_print_state(helix::PrintJobState::PRINTING);

    SECTION("unload_filament refused, no gcode/macro emitted") {
        AmsError err = backend->unload_filament(0);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(backend->captured_gcodes.empty());
    }

    SECTION("load_filament refused, no gcode/macro emitted") {
        AmsError err = backend->load_filament(0);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(backend->captured_gcodes.empty());
    }

    SECTION("change_tool refused, no gcode/macro emitted") {
        AmsError err = backend->change_tool(0);
        CHECK_FALSE(err.success());
        CHECK(err.result == AmsResult::WRONG_STATE);
        CHECK(backend->captured_gcodes.empty());
    }
}

TEST_CASE_METHOD(Ad5xHomingGuardFixture, "eject_lane is not blocked by an active print",
                 "[ams][ad5x_ifs][homing_guard]") {
    // eject_lane is a cold, lane-only op (no toolhead motion) — it must remain
    // available during a print. Whatever its outcome, it must NOT be refused
    // *because* a print is active.
    set_print_state(helix::PrintJobState::PRINTING);
    Ad5xIfsTestAccess::set_zcolor_supported(*backend, false);
    AmsError err = backend->eject_lane(0);
    CHECK(err.user_msg != "Cannot run filament operation while printing");
}

// ==========================================================================
// External TYPE-change detection (check_external_type_change)
// prestonbrown/helixscreen#981 / #1065: a type-only firmware edit (same
// color) never tripped the color detector, so a non-locked override's baked
// material went stale and masked firmware truth — color updated on screen,
// material stuck (e.g. on "PLA"). A firmware type change must now refresh a
// non-locked override so the new type surfaces, while a user-locked material
// (#965) is still preserved.
// ==========================================================================

TEST_CASE("AD5X IFS: firmware type change refreshes a non-locked override (#981)",
          "[ams][ad5x_ifs][override][981]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    seed_standard_colors(backend); // slot 1: firmware PETG / #00FF00, baselines set

    // Auto-mirror left a non-locked override matching firmware-at-the-time.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PETG";
    ovr.color_rgb = 0x00FF00;
    ovr.color_set = true;
    ovr.user_locked_material = false;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    // Firmware type changes externally (color unchanged): PETG -> TPU.
    Ad5xIfsTestAccess::set_material(backend, 1, "TPU");

    // The new firmware type must surface, and the stale override is refreshed.
    CHECK(backend.get_slot_info(1).material == "TPU");
    auto after = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(after.has_value());
    CHECK(after->material == "TPU");
}

TEST_CASE("AD5X IFS: firmware type change does NOT clobber a user-locked material (#965)",
          "[ams][ad5x_ifs][override][965]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    seed_standard_colors(backend); // slot 1 firmware PETG

    // Genuine user edit — locked, must survive the post-print firmware revert.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "SILK";
    ovr.color_rgb = 0x00FF00;
    ovr.color_set = true;
    ovr.user_locked_material = true;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    Ad5xIfsTestAccess::set_material(backend, 1, "TPU"); // firmware type change

    CHECK(backend.get_slot_info(1).material == "SILK");
    auto after = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(after.has_value());
    CHECK(after->material == "SILK");
}

TEST_CASE("AD5X IFS: first material observation is a baseline, not an edit",
          "[ams][ad5x_ifs][override][981]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // A pre-existing non-locked override must NOT be disturbed by the very
    // first firmware observation (startup): baseline only, no sync.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "ABS";
    ovr.user_locked_material = false;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    // First-ever material reading for slot 1 establishes the baseline.
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    // Override still wins on the baseline pass (no external edit detected yet).
    CHECK(backend.get_slot_info(1).material == "ABS");
}

// --------------------------------------------------------------------------
// #1065: insert AFTER an empty lane must refresh a stale non-locked material.
// Root cause: check_external_type_change early-returned on an empty material
// observation, so an empty lane never recorded a baseline. The FIRST insert
// then hit the "first observation" branch (baseline-only, no sync) instead of
// "changed", so the stale override.material masked firmware truth — color
// updated on screen, material stuck. The fix baselines the empty state (like
// the color path's #808080 placeholder), making "" -> PETG a genuine edit.
// --------------------------------------------------------------------------

TEST_CASE("AD5X IFS: insert after an empty lane refreshes a stale non-locked material "
          "(empty -> PETG) (#1065)",
          "[ams][ad5x_ifs][override][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // A previous spool left a non-locked override baked with the OLD material.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PLA";
    ovr.color_rgb = 0x00FF00;
    ovr.color_set = true;
    ovr.user_locked_material = false;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    // Firmware reports a color (so the sync's color-availability guard passes),
    // then the lane goes empty: presence false + empty material. Under the fix
    // this baselines the material to "".
    Ad5xIfsTestAccess::set_color(backend, 1, "00FF00");
    Ad5xIfsTestAccess::set_port_presence(backend, 1, false);
    Ad5xIfsTestAccess::set_material(backend, 1, "");

    // Sanity: while empty, the retained override still shows (lane keeps its
    // assignment across eject — #1071).
    CHECK(backend.get_slot_info(1).material == "PLA");

    // Insert a DIFFERENT material: lane present + firmware PETG. This is now a
    // genuine "" -> PETG delta and must fire the sync, refreshing the override.
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    CHECK(backend.get_slot_info(1).material == "PETG");
    auto after = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(after.has_value());
    CHECK(after->material == "PETG");
}

TEST_CASE("AD5X IFS: insert after an empty lane preserves a user-locked material (#965/#1065)",
          "[ams][ad5x_ifs][override][965][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // A deliberate user choice — locked. It must survive the empty->insert path.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "SILK";
    ovr.color_rgb = 0x00FF00;
    ovr.color_set = true;
    ovr.user_locked_material = true;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    Ad5xIfsTestAccess::set_color(backend, 1, "00FF00");
    Ad5xIfsTestAccess::set_port_presence(backend, 1, false);
    Ad5xIfsTestAccess::set_material(backend, 1, "");

    // Insert with a different firmware material — the OverwriteAlways mirror
    // skips the user-locked field, so the locked choice sticks.
    Ad5xIfsTestAccess::set_port_presence(backend, 1, true);
    Ad5xIfsTestAccess::set_material(backend, 1, "PETG");

    CHECK(backend.get_slot_info(1).material == "SILK");
    auto after = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(after.has_value());
    CHECK(after->material == "SILK");
}

TEST_CASE("AD5X IFS: an empty first material observation is a baseline, not a spurious sync "
          "(#1065)",
          "[ams][ad5x_ifs][override][1065]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // A pre-existing non-locked override must survive the FIRST (empty) firmware
    // observation untouched — baselining "" must not fire a sync that rewrites
    // the override.
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "ABS";
    ovr.color_rgb = 0x00FF00;
    ovr.color_set = true;
    ovr.user_locked_material = false;
    Ad5xIfsTestAccess::seed_override(backend, 1, ovr);

    // First-ever observation for slot 1 is an empty lane: presence false, empty
    // material. This establishes the "" baseline and must NOT sync.
    Ad5xIfsTestAccess::set_port_presence(backend, 1, false);
    Ad5xIfsTestAccess::set_material(backend, 1, "");

    // Override is undisturbed; no external edit was detected.
    CHECK(backend.get_slot_info(1).material == "ABS");
    auto after = Ad5xIfsTestAccess::get_override(backend, 1);
    REQUIRE(after.has_value());
    CHECK(after->material == "ABS");
}

// ==========================================================================
// Bundle 77TDH9N6: a cold-start IFS load (INSERT_PRUTOK_IFS) legitimately runs
// past the 300s AMS RPC ceiling (heat-from-cold + load + double purge + clean).
// The macro completes fine, but the RPC times out at 300s and — unless silent —
// surfaces a false "printer.gcode.script timed out after 300000ms" toast. The
// backend owns completion via its phase tracker + IFS_STATUS, so the RPC timeout
// is advisory here. execute_gcode() must dispatch silent=true (suppress the
// timeout toast and the tracker's generic fallback) while leaving the 300s
// ceiling unchanged.
//
// It must ALSO declare caller_surfaces_errors=false: the backend's error_cb only
// writes to the log, which the user never sees. Claiming otherwise would record
// the message for dedup and silence GcodeErrorRouter, leaving a real IFS macro
// rejection with no surface at all. See include/rpc_error_policy.h.
// ==========================================================================

TEST_CASE("AD5X IFS execute_gcode dispatches silent with the AMS timeout ceiling",
          "[ams][mock][ad5x_ifs]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    // execute_gcode()'s klippy-halted gate rejects everything until a real
    // state update arrives (subjects initialize to SHUTDOWN).
    state.set_klippy_state_sync(helix::KlippyState::READY);
    client.connect("ws://mock/websocket", []() {}, []() {});

    // Real MoonrakerAPI path: MoonrakerAPIMock does NOT override execute_gcode,
    // so the silent/timeout args reach the client mock's send capture.
    MoonrakerAPIMock api(client, state);
    AmsBackendAd5xIfs backend(&api, &client);

    SECTION("plain execute_gcode overload") {
        auto err = backend.execute_gcode("INSERT_PRUTOK_IFS PRUTOK=2");
        REQUIRE(err.success());

        REQUIRE(client.last_send_method() == "printer.gcode.script");
        // silent=true suppresses the false REQUEST_TIMEOUT toast.
        REQUIRE(client.last_send_silent() == true);
        // Ceiling unchanged — proves we did NOT alter AMS_OPERATION_TIMEOUT_MS.
        REQUIRE(client.last_send_timeout_ms() == 300000u);
        // The log-only error_cb must NOT claim the report, or the `!!` router
        // goes quiet for every AFC / Happy Hare / CFS / IFS macro rejection.
        REQUIRE(client.current_send_intent().silent == true);
        REQUIRE(client.current_send_intent().surfaces_errors == false);
    }

    SECTION("on_complete execute_gcode overload") {
        bool completed = false;
        auto err = backend.execute_gcode("INSERT_PRUTOK_IFS PRUTOK=2",
                                         [&completed]() { completed = true; });
        REQUIRE(err.success());

        REQUIRE(client.last_send_method() == "printer.gcode.script");
        REQUIRE(client.last_send_silent() == true);
        REQUIRE(client.last_send_timeout_ms() == 300000u);
        REQUIRE(client.current_send_intent().silent == true);
        REQUIRE(client.current_send_intent().surfaces_errors == false);
    }
}

// ============================================================================
// Adventurer5M.json freshness-poll cadence
// ============================================================================

TEST_CASE("AD5X IFS: JSON poll backs off to 30s while printing", "[ams][ad5x][ifs][poll]") {
    using namespace std::chrono;
    using B = AmsBackendAd5xIfs;

    SECTION("idle cadence is 5s") {
        REQUIRE_FALSE(B::should_poll_json(false, false, seconds(4)));
        REQUIRE(B::should_poll_json(false, false, seconds(5)));
    }

    SECTION("printing cadence is 30s — 5s and 29s must NOT fire") {
        REQUIRE_FALSE(B::should_poll_json(true, true, seconds(5)));
        REQUIRE_FALSE(B::should_poll_json(true, true, seconds(29)));
        REQUIRE(B::should_poll_json(true, true, seconds(30)));
    }

    SECTION("a pause keeps the fast cadence — that is when spools get relabelled") {
        // printing_now=false covers PAUSED as well as idle/complete.
        REQUIRE(B::should_poll_json(false, false, seconds(5)));
    }
}

TEST_CASE("AD5X IFS: print end forces an off-cadence poll", "[ams][ad5x][ifs][poll][965]") {
    using namespace std::chrono;
    using B = AmsBackendAd5xIfs;

    // printing -> not printing: fire immediately even with no elapsed time, so the
    // firmware's post-print FFMInfo revert isn't delayed by the slow interval.
    REQUIRE(B::should_poll_json(false, true, seconds(0)));

    // The edge is one-shot: once was_printing clears, normal cadence resumes.
    REQUIRE_FALSE(B::should_poll_json(false, false, seconds(0)));

    // Entering a print is NOT an edge — no reason to poll off-cadence there.
    REQUIRE_FALSE(B::should_poll_json(true, false, seconds(0)));
}

// ==========================================================================
// Unattended runout detection + auto-switchover plugin visibility
// prestonbrown/helixscreen#1250 (reported as #1247).
//
// Before this, a head-sensor drop at AmsAction::IDLE with no phase tracking
// produced nothing at all: detect_load_unload_completion() only reacts while
// the action is LOADING/UNLOADING and check_action_timeout() only runs during
// an operation phase. The print sat paused with an empty toolhead and the UI
// said nothing, while the reporter waited for a backup-spool switch that was
// never going to happen because no plugin was installed.
// ==========================================================================

namespace {

/// LVGL plus a live GLOBAL PrinterState.
///
/// The runout detector reads `get_printer_state().get_print_state_enum_subject()`
/// - the same global accessor handle_status_update() already uses to pick the
/// Adventurer5M.json poll cadence - NOT the PrinterState a MoonrakerAPIMock was
/// constructed with. Driving the global is therefore what a test has to do; the
/// two are separate objects and setting the wrong one silently proves nothing.
struct Ad5xRunoutFixture : public LVGLTestFixture {
    Ad5xRunoutFixture() {
        get_printer_state().init_subjects(false);
        set_print_state(helix::PrintJobState::STANDBY);
    }
    ~Ad5xRunoutFixture() override {
        set_print_state(helix::PrintJobState::STANDBY);
    }

    // Plain null check, not REQUIRE: this also runs from the destructor, where a
    // Catch2 assertion would throw during unwinding.
    static void set_print_state(helix::PrintJobState s) {
        if (get_printer_state().get_print_state_enum_subject()) {
            helix::test::set_wire_state(get_printer_state(), s);
        }
    }

    /// Seat filament at the toolhead SWITCH, then drop it - the present->absent
    /// edge the detector arms on. Two frames, because an edge needs a previous
    /// value and a fresh backend has never seen the switch.
    static void seat_then_drop_head(AmsBackendAd5xIfs& b) {
        Ad5xIfsTestAccess::handle_status(b, make_head_sensor(true));
        Ad5xIfsTestAccess::handle_status(b, make_head_sensor(false));
    }
};

/// Gcode-capturing backend wired to a MoonrakerAPIMock, so the real
/// change_tool() / unload_filament() dispatch paths run without a printer.
class Ad5xRunoutOpBackend : public AmsBackendAd5xIfs {
  public:
    explicit Ad5xRunoutOpBackend(MoonrakerAPI* api) : AmsBackendAd5xIfs(api, nullptr) {}

    std::vector<std::string> captured_gcodes;

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        captured_gcodes.push_back(gcode);
        (void)on_complete;
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return true;
    }
    bool has_gcode_containing(const std::string& needle) const {
        for (const auto& g : captured_gcodes) {
            if (g.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
};

/// Adds the mock API the operation-dispatch tests need.
///
/// Two PrinterStates are deliberately in play: `local_state` backs the mock API
/// and is what refuse_if_printing() consults (kept STANDBY so a dispatch is
/// allowed at all - this backend refuses filament ops while PAUSED, see
/// filament_ops_self_home()), while the inherited global is what the runout
/// detector reads and is what the tests drive to PAUSED.
struct Ad5xRunoutOpFixture : public Ad5xRunoutFixture {
    Ad5xRunoutOpFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        local_state.init_subjects(false);
        helix::test::set_wire_state(local_state, helix::PrintJobState::STANDBY);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, local_state);
        backend = std::make_unique<Ad5xRunoutOpBackend>(api.get());
        Ad5xIfsTestAccess::set_running(*backend, true);
        Ad5xIfsTestAccess::set_zcolor_supported(*backend, false);
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState local_state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<Ad5xRunoutOpBackend> backend;
};

} // namespace

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: head drop while paused and idle raises the fault",
                 "[ams][ad5x_ifs][runout][1250]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    // The drop happens mid-print, which is when a real runout happens.
    set_print_state(helix::PrintJobState::PRINTING);
    seat_then_drop_head(backend);
    REQUIRE(Ad5xIfsTestAccess::head_empty_armed(backend));

    // Still PRINTING: an empty head here is a firmware tool change, not a
    // runout. However long it stays empty, nothing is raised.
    Ad5xIfsTestAccess::age_head_empty(backend, std::chrono::seconds(600));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    REQUIRE_FALSE(Ad5xIfsTestAccess::filament_runout(backend));

    // Klipper's runout handling pauses the job. Now it is a runout.
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(Ad5xIfsTestAccess::runout_active(backend));
    REQUIRE(Ad5xIfsTestAccess::filament_runout(backend));
    // ERROR is the only edge AmsErrorBridge watches, so it is the only route to
    // current_error() and the recovery modal.
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    REQUIRE_FALSE(Ad5xIfsTestAccess::operation_detail(backend).empty());

    // Idempotent: re-evaluating must not report a second state change (which
    // would re-emit EVENT_STATE_CHANGED on every status frame).
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: the confirm dwell is load-bearing",
                 "[ams][ad5x_ifs][runout][1250]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    set_print_state(helix::PrintJobState::PAUSED);
    seat_then_drop_head(backend);
    REQUIRE(Ad5xIfsTestAccess::head_empty_armed(backend));

    // Freshly armed: every other condition holds, but the dwell has not elapsed.
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);

    Ad5xIfsTestAccess::age_head_empty(backend, std::chrono::seconds(600));
    REQUIRE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(Ad5xIfsTestAccess::runout_active(backend));
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: not raised while the print is not paused",
                 "[ams][ad5x_ifs][runout][1250]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    for (auto state : {helix::PrintJobState::PRINTING, helix::PrintJobState::STANDBY,
                       helix::PrintJobState::COMPLETE}) {
        set_print_state(state);
        seat_then_drop_head(backend);
        Ad5xIfsTestAccess::age_head_empty(backend, std::chrono::seconds(600));
        CHECK_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
        CHECK_FALSE(Ad5xIfsTestAccess::runout_active(backend));
        CHECK(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: a motion-sensor-only drop is not an empty toolhead",
                 "[ams][ad5x_ifs][runout][1250]") {
    // The conflation trap: parse_head_sensor() writes head_filament_ from BOTH
    // the switch and ifs_motion_sensor, and the motion sensor is device-confirmed
    // to read filament_detected=false on a loaded-but-idle lane. Gating on
    // head_filament_ would fire a runout on a perfectly healthy paused print.
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    set_print_state(helix::PrintJobState::PAUSED);

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_switch_present(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
    // head_filament_ IS now false - and that is exactly why it cannot be trusted.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
    // The switch, the actual authority, still says loaded.
    REQUIRE(Ad5xIfsTestAccess::head_switch_present(backend));

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_empty_armed(backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: a head drop during a tracked load/unload is not a runout",
                 "[ams][ad5x_ifs][runout][phase][1250]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    set_print_state(helix::PrintJobState::PAUSED);

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
    Ad5xIfsTestAccess::handle_status(backend, make_extruder(230.0, 230.0));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::CUTTING);

    // The cut drops the head sensor. That is progress, not a runout.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_empty_armed(backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::runout_active(backend));

    // Existing phase behaviour is untouched: CUTTING -> UNLOADING on the drop.
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);
    REQUIRE(Ad5xIfsTestAccess::operation_detail(backend) == "Retracting filament from nozzle");
    REQUIRE(Ad5xIfsTestAccess::phase_active(backend));
}

TEST_CASE_METHOD(Ad5xRunoutOpFixture,
                 "AD5X IFS runout: a head drop during a tool-change swap is not a runout",
                 "[ams][ad5x_ifs][runout][1250]") {
    // The regression the "operation in flight" gate exists for. do_change_tool()
    // sets LOADING but never arms the phase tracker, so gating on
    // phase_tracker_.active alone would pop a runout modal on every multicolour
    // swap that happens to land on a paused job.
    REQUIRE(backend->set_tool_mapping(0, 1).success()); // T0 -> port 2

    Ad5xIfsTestAccess::handle_status(*backend, make_head_sensor(true));
    REQUIRE(backend->change_tool(0).success());
    REQUIRE(backend->has_gcode_containing("A_CHANGE_FILAMENT CHANNEL=2"));
    REQUIRE(Ad5xIfsTestAccess::action(*backend) == AmsAction::LOADING);

    // Mid-swap the head empties, and the job is paused.
    set_print_state(helix::PrintJobState::PAUSED);
    Ad5xIfsTestAccess::handle_status(*backend, make_head_sensor(false));

    // Nothing armed: the action is LOADING, so an operation is in flight.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_empty_armed(*backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(*backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::runout_active(*backend));
    REQUIRE(Ad5xIfsTestAccess::action(*backend) == AmsAction::LOADING);

    // Even if the action snaps back to IDLE (the legacy head-rise finalize) the
    // dispatch stamp still attributes the window to the swap.
    Ad5xIfsTestAccess::set_action(*backend, AmsAction::IDLE);
    Ad5xIfsTestAccess::handle_status(*backend, make_head_sensor(true));
    Ad5xIfsTestAccess::handle_status(*backend, make_head_sensor(false));
    Ad5xIfsTestAccess::age_head_empty(*backend, std::chrono::seconds(600));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(*backend));
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: an eject-routed unload suppresses the fault window",
                 "[ams][ad5x_ifs][runout][1250]") {
    // do_unload_filament() has three early returns that hand off to eject_lane()
    // without touching phase_tracker_ or system_info_.action, so the backend
    // stays IDLE right through an operation that legitimately empties the head.
    // The dispatch stamp is the only thing standing between that and a false
    // runout.
    TestableAd5xIfsBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    set_print_state(helix::PrintJobState::PRINTING);

    seat_then_drop_head(backend);
    REQUIRE(Ad5xIfsTestAccess::head_empty_armed(backend));
    set_print_state(helix::PrintJobState::PAUSED);

    // Empty head + a requested unload routes to the cold per-lane eject.
    Ad5xIfsTestAccess::set_current_slot(backend, 0, false);
    REQUIRE(backend.unload_filament(0).success());
    REQUIRE(backend.has_gcode("IFS_F11 PRUTOK=1 LEN=1000 SPEED=1200"));
    REQUIRE_FALSE(backend.has_gcode_containing("REMOVE_CURRENT_PRUTOK"));

    // The dispatch disarmed the candidate outright.
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_empty_armed(backend));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));

    // And a candidate that re-arms inside the suppression window is still
    // attributed to that eject, not to a runout.
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    Ad5xIfsTestAccess::age_head_empty(backend, std::chrono::seconds(600));
    REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(backend));

    // Once the window expires, the same state IS a runout.
    Ad5xIfsTestAccess::age_op_dispatch(backend, std::chrono::seconds(600));
    REQUIRE(Ad5xIfsTestAccess::evaluate_runout(backend));
    REQUIRE(Ad5xIfsTestAccess::runout_active(backend));
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: clears on refill, resume and recover",
                 "[ams][ad5x_ifs][runout][1250]") {
    auto raise = [this](AmsBackendAd5xIfs& b) {
        set_print_state(helix::PrintJobState::PAUSED);
        Ad5xIfsTestAccess::handle_status(b, make_head_sensor(true));
        Ad5xIfsTestAccess::handle_status(b, make_head_sensor(false));
        Ad5xIfsTestAccess::age_head_empty(b, std::chrono::seconds(600));
        REQUIRE(Ad5xIfsTestAccess::evaluate_runout(b));
        REQUIRE(Ad5xIfsTestAccess::action(b) == AmsAction::ERROR);
    };

    SECTION("filament back at the toolhead clears it") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        raise(backend);
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        REQUIRE_FALSE(Ad5xIfsTestAccess::runout_active(backend));
        REQUIRE_FALSE(Ad5xIfsTestAccess::filament_runout(backend));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("resuming the print clears it (this is what dismisses the modal)") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        raise(backend);
        set_print_state(helix::PrintJobState::PRINTING);
        REQUIRE(Ad5xIfsTestAccess::evaluate_runout(backend));
        REQUIRE_FALSE(Ad5xIfsTestAccess::runout_active(backend));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("recover() clears it") {
        TestableAd5xIfsBackend backend;
        Ad5xIfsTestAccess::set_running(backend, true);
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        raise(backend);
        REQUIRE(backend.recover().success());
        REQUIRE_FALSE(Ad5xIfsTestAccess::runout_active(backend));
        REQUIRE_FALSE(Ad5xIfsTestAccess::filament_runout(backend));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: current_error distinguishes runout from an op timeout",
                 "[ams][ad5x_ifs][runout][error-center][1250]") {
    SECTION("runout: Resume / Purge / Recover, and deliberately NO load button") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        set_print_state(helix::PrintJobState::PAUSED);
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
        Ad5xIfsTestAccess::age_head_empty(backend, std::chrono::seconds(600));
        REQUIRE(Ad5xIfsTestAccess::evaluate_runout(backend));

        auto ev = backend.current_error();
        REQUIRE(ev.has_value());
        REQUIRE(ev->source == helix::ErrorSource::IFS);
        REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
        REQUIRE(ev->sticky);
        REQUIRE(ev->title == std::string("Filament runout"));

        REQUIRE(ev->recovery_actions.size() == 3);
        REQUIRE(ev->recovery_actions[0].gcode == "RESUME");
        REQUIRE(ev->recovery_actions[0].style == "primary");
        REQUIRE(ev->recovery_actions[0].needs_hot_nozzle);
        // A plain extruder move, not zmod's PURGE_FILAMENT macro - no homing, so
        // it cannot reach the loadcell _G28 that shuts the AD5X down mid-job.
        REQUIRE(ev->recovery_actions[1].gcode == "M83\nG1 E50 F600");
        REQUIRE(ev->recovery_actions[1].needs_hot_nozzle);
        REQUIRE(ev->recovery_actions[2].gcode == "IFS_UNLOCK");
        REQUIRE_FALSE(ev->recovery_actions[2].needs_hot_nozzle);

        // No load: every AD5X load path runs INSERT_PRUTOK_IFS, whose macro homes
        // itself, and a runout state is PAUSED by construction.
        for (const auto& a : ev->recovery_actions) {
            CHECK(a.gcode.find("INSERT_PRUTOK_IFS") == std::string::npos);
            CHECK(a.gcode.find("A_CHANGE_FILAMENT") == std::string::npos);
        }
    }

    SECTION("operation timeout keeps the historical single Recover action") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        Ad5xIfsTestAccess::set_head_filament(backend, true);
        Ad5xIfsTestAccess::begin_phase(backend, /*is_unload=*/true);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(600));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
        REQUIRE_FALSE(Ad5xIfsTestAccess::runout_active(backend));

        auto ev = backend.current_error();
        REQUIRE(ev.has_value());
        REQUIRE(ev->title == std::string("Filament System Error"));
        REQUIRE(ev->recovery_actions.size() == 1);
        REQUIRE(ev->recovery_actions[0].gcode == "IFS_UNLOCK");
        REQUIRE(ev->recovery_actions[0].style == "primary");
    }
}

TEST_CASE_METHOD(Ad5xRunoutFixture, "AD5X IFS runout: detail text names the plugin situation",
                 "[ams][ad5x_ifs][runout][1250]") {
    auto raise = [this](AmsBackendAd5xIfs& b) {
        set_print_state(helix::PrintJobState::PAUSED);
        Ad5xIfsTestAccess::handle_status(b, make_head_sensor(true));
        Ad5xIfsTestAccess::handle_status(b, make_head_sensor(false));
        Ad5xIfsTestAccess::age_head_empty(b, std::chrono::seconds(600));
        REQUIRE(Ad5xIfsTestAccess::evaluate_runout(b));
        return Ad5xIfsTestAccess::operation_detail(b);
    };

    SECTION("stock zMod: Infinite Spool Mode names the rule, no plugin mention") {
        // Source-verified: ANALOG_PRUTOK (zmod_ifs.py:cmd_ANALOG_PRUTOK) runs
        // always-on on head runout. The detail must name the feature and state
        // the type+colour+present match — NOT claim no switchover will happen.
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        const std::string detail = raise(backend);
        CHECK(detail.find("Infinite Spool Mode") != std::string::npos);
        // The retired string is gone.
        CHECK(detail.find("will not change to a backup spool") == std::string::npos);
        CHECK(detail.find("No auto-switchover plugin") == std::string::npos);
        // The same type+colour+present rule the plugin path promises.
        CHECK(detail.find("type") != std::string::npos);
        CHECK(detail.find("colour") != std::string::npos);
        CHECK(detail.find("port sensor") != std::string::npos);
    }

    SECTION("plugin installed, backup off") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
        Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 0}});
        const std::string detail = raise(backend);
        CHECK(detail.find("lessWaste") != std::string::npos);
        CHECK(detail.find("turned off") != std::string::npos);
    }

    SECTION("plugin installed, backup on: state the strict matching rule") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");
        Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 1}});
        const std::string detail = raise(backend);
        CHECK(detail.find("bambufy") != std::string::npos);
        // Both criteria promised, never one.
        CHECK(detail.find("type") != std::string::npos);
        CHECK(detail.find("colour") != std::string::npos);
        CHECK(detail.find("port sensor") != std::string::npos);
    }
}

TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: any switchover path buys the longer confirm delay",
                 "[ams][ad5x_ifs][runout][1250]") {
    // Three switchover paths now qualify for the longer dwell: stock zMod's
    // ANALOG_PRUTOK (always-on), bambufy/lessWaste with variable_backup on.
    // The short delay survives only when a plugin is installed AND backup is
    // definitively off (or was never read).
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);

    SECTION("stock zMod (always-on ANALOG_PRUTOK) gets the longer dwell") {
        const auto delay = Ad5xIfsTestAccess::runout_confirm_delay(backend);
        // Compare against a plugin-with-backup-off baseline, which stays short.
        AmsBackendAd5xIfs plugin_off(nullptr, nullptr);
        Ad5xIfsTestAccess::set_zcolor_supported(plugin_off, false);
        Ad5xIfsTestAccess::set_has_ifs_vars(plugin_off, true);
        Ad5xIfsTestAccess::parse_ifs_vars_macro(plugin_off, json{{"variable_backup", 0}});
        const auto short_delay = Ad5xIfsTestAccess::runout_confirm_delay(plugin_off);
        REQUIRE(delay > short_delay);
    }

    SECTION("plugin with backup on gets the longer dwell (unchanged)") {
        // Baseline is the SAME plugin with backup OFF; that is the only config
        // that still gets the short delay under the new matrix (stock zMod
        // always-on now also qualifies for the long delay).
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
        Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 0}});
        const auto plain = Ad5xIfsTestAccess::runout_confirm_delay(backend);

        Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 1}});
        const auto with_backup = Ad5xIfsTestAccess::runout_confirm_delay(backend);
        REQUIRE(with_backup > plain);
    }
}

// The matrix above only pins what runout_confirm_delay_locked() RETURNS. Nothing
// there reaches evaluate_runout_locked(), and every other runout test ages the
// candidate by 600s — past both thresholds — so a predicate that ignored
// runout_confirm_delay_locked() and hardcoded either constant would sail through
// the whole file. This drives the real predicate on both sides of each config's
// own dwell.
TEST_CASE_METHOD(Ad5xRunoutFixture,
                 "AD5X IFS runout: the predicate honours the per-config confirm delay",
                 "[ams][ad5x_ifs][runout][1250]") {
    // Probe ages are DERIVED from the backend's own runout_confirm_delay()
    // rather than written down, so this asserts the wiring rather than a
    // number: a predicate hardcoding the short constant fires early on a
    // long-dwell config, and one hardcoding the long constant never fires on a
    // short-dwell config. Either way a REQUIRE below goes red.
    constexpr auto SLACK = std::chrono::seconds(5);
    auto dwell_is_load_bearing = [this, SLACK](AmsBackendAd5xIfs& b) {
        set_print_state(helix::PrintJobState::PAUSED);
        seat_then_drop_head(b);
        REQUIRE(Ad5xIfsTestAccess::head_empty_armed(b));

        const auto dwell = Ad5xIfsTestAccess::runout_confirm_delay(b);
        REQUIRE(dwell > SLACK); // the two probes have to straddle it

        // Just short of the dwell: armed, paused, idle — everything else holds.
        // A too-short threshold raises here.
        Ad5xIfsTestAccess::age_head_empty(b, dwell - SLACK);
        REQUIRE_FALSE(Ad5xIfsTestAccess::evaluate_runout(b));
        REQUIRE_FALSE(Ad5xIfsTestAccess::runout_active(b));
        REQUIRE(Ad5xIfsTestAccess::action(b) == AmsAction::IDLE);
        // The failed probe must leave the candidate armed, or the second probe
        // would be testing nothing.
        REQUIRE(Ad5xIfsTestAccess::head_empty_armed(b));

        // Just past it. A too-long threshold fails to raise here.
        Ad5xIfsTestAccess::age_head_empty(b, dwell + SLACK);
        REQUIRE(Ad5xIfsTestAccess::evaluate_runout(b));
        REQUIRE(Ad5xIfsTestAccess::runout_active(b));
        REQUIRE(Ad5xIfsTestAccess::action(b) == AmsAction::ERROR);
        return dwell;
    };

    // Stock zMod: no plugin, ANALOG_PRUTOK always-on -> the long dwell.
    AmsBackendAd5xIfs stock(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(stock, false);
    const auto stock_dwell = dwell_is_load_bearing(stock);

    // Plugin installed with backup definitively OFF -> the short dwell. This is
    // the only config that still gets it.
    AmsBackendAd5xIfs backup_off(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(backup_off, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backup_off, true);
    Ad5xIfsTestAccess::set_var_prefix(backup_off, "less_waste");
    Ad5xIfsTestAccess::parse_ifs_vars_macro(backup_off, json{{"variable_backup", 0}});
    const auto backup_off_dwell = dwell_is_load_bearing(backup_off);

    // Plugin installed with backup ON -> back to the long dwell.
    AmsBackendAd5xIfs backup_on(nullptr, nullptr);
    Ad5xIfsTestAccess::set_zcolor_supported(backup_on, false);
    Ad5xIfsTestAccess::set_has_ifs_vars(backup_on, true);
    Ad5xIfsTestAccess::set_var_prefix(backup_on, "bambufy");
    Ad5xIfsTestAccess::parse_ifs_vars_macro(backup_on, json{{"variable_backup", 1}});
    const auto backup_on_dwell = dwell_is_load_bearing(backup_on);

    // Without this the three probes above could all be straddling one shared
    // number, and "honours the per-config delay" would mean nothing.
    CHECK(backup_off_dwell < stock_dwell);
    CHECK(backup_off_dwell < backup_on_dwell);
    CHECK(stock_dwell == backup_on_dwell);
}

TEST_CASE("AD5X IFS plugin visibility: which plugin, and is backup on",
          "[ams][ad5x_ifs][runout][1250]") {
    using B = AmsBackendAd5xIfs;

    SECTION("stock zMod: no plugin, switchover is a definite ON (ANALOG_PRUTOK)") {
        B backend(nullptr, nullptr);
        REQUIRE(backend.get_plugin() == B::IfsPlugin::None);
        // No plugin -> plugin_backup_enabled() is nullopt (no flag to read), but
        // backup_state_locked() is ON: stock zMod's ANALOG_PRUTOK is always-on.
        REQUIRE_FALSE(backend.plugin_backup_enabled().has_value());
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::BACKUP_ON);
    }

    SECTION("lessWaste detected from its own save_variables") {
        B backend(nullptr, nullptr);
        // The macro-existence latch has to be cleared first; stale save_variables
        // rows left by an uninstalled plugin must NOT read as installed.
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend,
                                      json{{"less_waste_tools", json::array({1, 2, 3, 4})}});
        REQUIRE(backend.get_plugin() == B::IfsPlugin::LessWaste);
        // Nothing read yet -> unknown, NOT off.
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::BACKUP_UNKNOWN);
    }

    SECTION("bambufy detected from its own save_variables") {
        B backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend, json{{"bambufy_tools", json::array({1, 2, 3, 4})}});
        REQUIRE(backend.get_plugin() == B::IfsPlugin::Bambufy);
    }

    SECTION("a stale prefix without the macro is still None") {
        B backend(nullptr, nullptr);
        // ifs_macro_confirmed_missing_ starts true (pessimistic).
        Ad5xIfsTestAccess::parse_vars(backend,
                                      json{{"less_waste_tools", json::array({1, 2, 3, 4})}});
        REQUIRE(backend.get_plugin() == B::IfsPlugin::None);
        // Treated as stock zMod -> ANALOG_PRUTOK always-on.
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::BACKUP_ON);
    }

    SECTION("variable_backup is read from the macro dict, int or bool") {
        B backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);

        REQUIRE(Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 0}}));
        REQUIRE(backend.plugin_backup_enabled() == std::optional<bool>{false});
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::BACKUP_OFF);

        REQUIRE(Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 1}}));
        REQUIRE(backend.plugin_backup_enabled() == std::optional<bool>{true});
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::BACKUP_ON);

        REQUIRE(Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", false}}));
        REQUIRE(backend.plugin_backup_enabled() == std::optional<bool>{false});

        // Unchanged value reports no change (so it does not force an event).
        REQUIRE_FALSE(
            Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 0}}));
    }

    SECTION("a dict without variable_backup leaves the answer UNKNOWN") {
        B backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        REQUIRE_FALSE(Ad5xIfsTestAccess::parse_ifs_vars_macro(
            backend, json{{"variable_tools", json::array({1, 2, 3, 4})}}));
        REQUIRE_FALSE(backend.plugin_backup_enabled().has_value());
        REQUIRE(Ad5xIfsTestAccess::backup_state(backend) == B::BACKUP_UNKNOWN);
    }

    SECTION("an empty dict is Klipper's 'no such object' answer, not data") {
        B backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        REQUIRE_FALSE(Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json::object()));
        REQUIRE_FALSE(backend.plugin_backup_enabled().has_value());
    }
}

TEST_CASE("AD5X IFS runout: backup-slot match needs type AND colour AND presence",
          "[ams][ad5x_ifs][runout][1250]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Lane 0 is the one that ran out: PLA / FF0000.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");

    SECTION("nothing else loaded: no match") {
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == -1);
    }

    SECTION("same type, different colour: no match") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "00FF00");
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == -1);
    }

    SECTION("same colour, different type: no match") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PETG");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == -1);
    }

    SECTION("both match but the port reads empty: no match") {
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        Ad5xIfsTestAccess::set_port_presence(backend, 2, false);
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == -1);
    }

    SECTION("all three: match, case-insensitively on the hex") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "pla");
        Ad5xIfsTestAccess::set_color(backend, 2, "ff0000");
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == 2);
    }

    SECTION("the ran-out lane never matches itself") {
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) != 0);
    }
}

// The runout detail is assembled from several translatable pieces. It used to
// be built by concatenation -- the subject glued on in C++ and the rest left as
// a fragment starting mid-clause ("is installed but ...", "matches.") -- which
// no translator can reorder, so those fragments sat untranslated in all eight
// non-English locales. Each piece is now a whole sentence with the variable
// part as `{}`.
//
// These assertions pin the assembled ENGLISH only. They deliberately do not
// claim to catch a relapse into concatenation: `who + " " + lv_tr("will switch
// ...")` produces a byte-identical English string, so no assertion on the
// output can tell the two apart (verified by mutating it back -- this test
// still passed). What differs is the CATALOG, and that is guarded elsewhere:
// re-splitting the sentence makes the whole-sentence key obsolete and the
// fragments new, which the "no user-facing strings are missing from the
// translation catalogs" gate in tests/shell/test_code_lint.bats fails on.
TEST_CASE("AD5X IFS runout detail reads as whole sentences", "[ams][ad5x_ifs][runout][i18n]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");

    const auto contains = [](const std::string& haystack, const char* needle) {
        return haystack.find(needle) != std::string::npos;
    };

    SECTION("stock zMod names Infinite Spool Mode as the subject") {
        Ad5xIfsTestAccess::set_runout_state(backend, 0, /*has_ifs_vars=*/false, std::nullopt);
        const std::string detail = Ad5xIfsTestAccess::runout_detail(backend);

        // Subject adjacent to its verb is exactly what concatenation could not
        // guarantee once a translator moved either one.
        REQUIRE(contains(detail, "Infinite Spool Mode will switch to a slot"));
        REQUIRE(contains(detail, "No slot currently matches."));
    }

    SECTION("a matching backup slot is named in one sentence, not three pieces") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        Ad5xIfsTestAccess::set_runout_state(backend, 0, /*has_ifs_vars=*/false, std::nullopt);

        REQUIRE(contains(Ad5xIfsTestAccess::runout_detail(backend), "Slot 3 matches."));
    }

    SECTION("an unreadable plugin setting names the plugin as the subject") {
        Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");
        Ad5xIfsTestAccess::set_runout_state(backend, 0, /*has_ifs_vars=*/true, std::nullopt);

        REQUIRE(contains(Ad5xIfsTestAccess::runout_detail(backend),
                         "bambufy is installed, but its backup-spool setting could not be read"));
    }

    SECTION("backup switching turned off names the plugin as the subject") {
        Ad5xIfsTestAccess::set_var_prefix(backend, "lessWaste");
        Ad5xIfsTestAccess::set_runout_state(backend, 0, /*has_ifs_vars=*/true, false);

        REQUIRE(contains(Ad5xIfsTestAccess::runout_detail(backend),
                         "lessWaste is installed but its backup-spool switching is turned off"));
    }
}

// ==========================================================================
// Endless spool: the shared abstraction over the plugin state
// ==========================================================================

TEST_CASE("AD5X IFS endless spool capabilities", "[ams][ad5x_ifs][endless_spool][1250]") {
    using namespace helix::printer;

    SECTION("stock zMod: Available + FirmwareManaged, ANALOG_PRUTOK always-on") {
        // The matrix that source read of zmod_ifs.py:cmd_ANALOG_PRUTOK +
        // ad5x_display_off.cfg:39-44 established (corroborated on-device by
        // raza616). Stock zMod has its own switchover — "Infinite Spool Mode" —
        // with no toggle. That is Available/FirmwareManaged/provider="zmod",
        // not RequiresPlugin/PluginMissing.
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);

        auto caps = backend.get_endless_spool_capabilities();
        REQUIRE(caps.availability == EndlessSpoolAvailability::Available);
        REQUIRE(caps.availability != EndlessSpoolAvailability::RequiresPlugin);
        REQUIRE(caps.availability != EndlessSpoolAvailability::Unsupported);
        REQUIRE(caps.enabled == EndlessSpoolEnabled::On);
        REQUIRE(caps.enabled != EndlessSpoolEnabled::Off);
        REQUIRE(caps.editability == EndlessSpoolEditability::ReadOnly);
        REQUIRE(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);
        REQUIRE(caps.provider == "zmod");
        REQUIRE(caps.available());
        REQUIRE_FALSE(caps.editable());
    }

    SECTION("plugin installed but variable_backup unreadable: Unknown, never Off") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "less_waste");
        REQUIRE_FALSE(backend.plugin_backup_enabled().has_value());

        auto caps = backend.get_endless_spool_capabilities();
        REQUIRE(caps.availability == EndlessSpoolAvailability::Available);
        REQUIRE(caps.enabled == EndlessSpoolEnabled::Unknown);
        REQUIRE(caps.enabled != EndlessSpoolEnabled::Off);
        REQUIRE(caps.provider == "lessWaste");
    }

    SECTION("variable_backup on/off maps to On/Off, and names the provider") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::set_var_prefix(backend, "bambufy");

        Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 1}});
        REQUIRE(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
        REQUIRE(backend.get_endless_spool_capabilities().provider == "bambufy");

        Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 0}});
        REQUIRE(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::Off);
    }

    SECTION("read-only even with a plugin, and writes are refused") {
        // `backup` is never written today and write_ifs_var() rides the _IFS_VARS
        // unknown-command latch, so an editable toggle would silently stop working.
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 1}});

        auto caps = backend.get_endless_spool_capabilities();
        REQUIRE(caps.editability == EndlessSpoolEditability::ReadOnly);
        REQUIRE(caps.restriction == EndlessSpoolRestriction::PluginReadOnly);

        auto result = backend.set_endless_spool_backup(0, 1);
        REQUIRE_FALSE(result.success());
        REQUIRE(result.result == AmsResult::NOT_SUPPORTED);
        REQUIRE(result.user_msg ==
                endless_spool_restriction_text(EndlessSpoolRestriction::PluginReadOnly));
        REQUIRE_FALSE(backend.reset_endless_spool().success());
    }

    SECTION("no per-slot relation: the firmware computes the match at runout time") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, true);
        Ad5xIfsTestAccess::parse_ifs_vars_macro(backend, json{{"variable_backup", 1}});
        seed_standard_colors(backend);

        REQUIRE(backend.get_endless_spool_config().empty());
    }
}

TEST_CASE("AD5X IFS overrides the eligibility rule with type+colour+presence",
          "[ams][ad5x_ifs][endless_spool][eligibility][1250]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Lane 0: PLA / FF0000, present.
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_material(backend, 0, "PLA");
    Ad5xIfsTestAccess::set_color(backend, 0, "FF0000");

    SECTION("the generic material-compatibility default would say yes; AD5X says no") {
        // Same material, different colour. filament::are_materials_compatible()
        // (the AmsBackend default) accepts this; AD5X must not, because the
        // firmware's own switchover is colour-matched too.
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "00FF00");

        REQUIRE(filament::materials_compatible("PLA", "PLA"));
        REQUIRE(backend.endless_spool_backup_eligibility(0, 2) ==
                helix::printer::BackupEligibility::Incompatible);
    }

    SECTION("an absent port is not eligible even on an exact match") {
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        Ad5xIfsTestAccess::set_port_presence(backend, 2, false);
        REQUIRE(backend.endless_spool_backup_eligibility(0, 2) ==
                helix::printer::BackupEligibility::Incompatible);
    }

    SECTION("exact type + colour + presence is eligible, case-insensitively") {
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "pla");
        Ad5xIfsTestAccess::set_color(backend, 2, "ff0000");
        REQUIRE(backend.endless_spool_backup_eligibility(0, 2) ==
                helix::printer::BackupEligibility::Eligible);
        // And it is the same rule the firmware-match scan uses.
        REQUIRE(Ad5xIfsTestAccess::find_backup_slot(backend, 0) == 2);
    }

    SECTION("self and out-of-range are never eligible") {
        using helix::printer::BackupEligibility;
        REQUIRE(backend.endless_spool_backup_eligibility(0, 0) == BackupEligibility::Incompatible);
        REQUIRE(backend.endless_spool_backup_eligibility(0, AmsBackendAd5xIfs::NUM_PORTS) ==
                BackupEligibility::Incompatible);
        REQUIRE(backend.endless_spool_backup_eligibility(-1, 0) == BackupEligibility::Incompatible);
    }

    SECTION("a filled grade is refused outright, never softened") {
        // The base rule would answer GradeDiffers here and let the user proceed.
        // AD5X must not: its firmware matches the type string exactly, so a
        // PLA-CF lane will simply never be selected for a PLA runout, and
        // offering it as a choosable option would promise a swap that cannot
        // happen. A backend's verdict is about what its firmware will do.
        Ad5xIfsTestAccess::set_port_presence(backend, 2, true);
        Ad5xIfsTestAccess::set_material(backend, 2, "PLA-CF");
        Ad5xIfsTestAccess::set_color(backend, 2, "FF0000");
        REQUIRE(backend.endless_spool_backup_eligibility(0, 2) ==
                helix::printer::BackupEligibility::Incompatible);
    }
}
