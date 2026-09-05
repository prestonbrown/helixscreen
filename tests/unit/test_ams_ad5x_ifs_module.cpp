// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The standalone IFS module (the drop-in extracted from zmod; ships in
// Forge-X) publishes first-class `ifs` / `ifs_materials` get_status() objects
// and maintains the `ifs_loaded` save_variable its own macros write. These
// tests pin that the module's frames land through the SAME apply path the
// zmod objects and macro responses use, that its own macro family
// (IFS_LOAD / IFS_UNLOAD / IFS_EJECT / IFS_SET_MATERIAL / T<n>) is what the
// ops dispatch once those objects are live, and that the ZMOD-era polls stand
// down. Frame shapes are taken from the module's get_status() implementations
// (ifs.py / ifs_materials.py on feat/ad5x-142).

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_state.h"
#include "ams_types.h"
#include "printer_discovery.h"
#include "test_helpers/ad5x_ifs_test_access.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

/// One `ifs` frame. `loaded` is 1-based channel numbers (the module decodes
/// the silk bitmask to a list); `active` is the F13 chan (1-based, 0 = none).
json module_ifs_frame(int active, std::vector<int> loaded, const char* activity = "ready") {
    return json{{"ifs",
                 {{"connected", true},
                  {"error", nullptr},
                  {"channel_count", 4},
                  {"state", 5},
                  {"activity", activity},
                  {"activity_channel", 0},
                  {"active_channel", active},
                  {"loaded_channels", loaded},
                  {"moving_channels", json::array()},
                  {"pending_insert_channels", json::array()},
                  {"params", json::object()}}}};
}

/// One `ifs_materials` frame: STRING slot keys (Moonraker serialises dict
/// keys that way), '#'-prefixed colours including a 3-digit form, null for an
/// unlabelled slot, and the purge table the colour-distance scaling emits.
json module_materials_frame() {
    return json{{"ifs_materials",
                 {{"available", true},
                  {"channel_count", 4},
                  {"enabled", true},
                  {"slots",
                   {{"1", {{"type", "PLA"}, {"color", "#A03CF7"}, {"temp", 220.0}}},
                    {"2", {{"type", "PETG"}, {"color", "#F80"}, {"temp", 250.0}}},
                    {"3", {{"type", nullptr}, {"color", nullptr}, {"temp", nullptr}}},
                    {"4", {{"type", "ABS"}, {"color", "#FFFFFF"}, {"temp", 250.0}}}}},
                  {"loaded", {{"type", "PLA"}, {"color", "#A03CF7"}, {"temp", 220.0}}},
                  {"purge_first_mm", {{"1>2", 61.5}, {"2>1", 61.5}}},
                  {"temperatures", {{"PLA", 220.0}, {"PETG", 250.0}, {"ABS", 250.0}}}}}};
}

/// A materials frame with every lane labelled — the rig shape the preview's
/// loaded-slot colours come from (#959). Distinct on purpose: the colour
/// engine's degeneracy guard returns {} when a routing resolves every tool to
/// one indistinguishable colour, and this frame must not trip it by accident.
json module_labeled_materials_frame() {
    return json{
        {"ifs_materials",
         {{"available", true},
          {"channel_count", 4},
          {"enabled", true},
          {"slots",
           {{"1", {{"type", "PLA"}, {"color", "#A03CF7"}, {"temp", 220.0}}},
            {"2", {{"type", "PETG"}, {"color", "#FF8800"}, {"temp", 250.0}}},
            {"3", {{"type", "TPU"}, {"color", "#00C8FF"}, {"temp", 230.0}}},
            {"4", {{"type", "ABS"}, {"color", "#FFFFFF"}, {"temp", 260.0}}}}},
          {"temperatures", {{"PLA", 220.0}, {"PETG", 250.0}, {"TPU", 230.0}, {"ABS", 260.0}}}}}};
}

/// Captures issued G-code without a live Moonraker connection, mirroring the
/// TestableAd5xIfsBackend pattern in test_ams_backend_ad5x_ifs.cpp: the ops
/// route through the virtual execute_gcode() (or ensure_homed_then() with
/// toolhead_homed() true, which lands in execute_gcode() all the same).
class TestableModuleBackend : public AmsBackendAd5xIfs {
  public:
    TestableModuleBackend() : AmsBackendAd5xIfs(nullptr, nullptr) {}

    std::vector<std::string> captured_gcodes;

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, std::function<void()>) override {
        captured_gcodes.push_back(gcode);
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

TEST_CASE("AD5X IFS subscribes to the module objects only where they exist",
          "[ams][ad5x_ifs][ifs_module]") {
    helix::PrinterDiscovery hw;

    // ZMOD firmware: the module pair is absent and the ask is unchanged.
    hw.set_printer_objects({"toolhead", "extruder", "save_variables", "zmod_ifs", "zmod_color"});
    auto zmod = AmsBackendAd5xIfs::required_status_objects(hw);
    REQUIRE(zmod.size() == 3);
    CHECK(std::find(zmod.begin(), zmod.end(), "ifs") == zmod.end());

    // Module firmware: zmod's pair is absent, the module's is asked for.
    hw.set_printer_objects({"toolhead", "extruder", "save_variables", "ifs", "ifs_materials"});
    auto module = AmsBackendAd5xIfs::required_status_objects(hw);
    REQUIRE(module.size() == 3);
    CHECK(std::find(module.begin(), module.end(), "ifs") != module.end());
    CHECK(std::find(module.begin(), module.end(), "ifs_materials") != module.end());
}

TEST_CASE("PrinterDiscovery detects the standalone IFS module objects",
          "[ams][ad5x_ifs][ifs_module]") {
    SECTION("both objects") {
        helix::PrinterDiscovery hw;
        hw.parse_objects(
            json::array({"toolhead", "extruder", "save_variables", "ifs", "ifs_materials",
                         "filament_switch_sensor toolhead", "filament_switch_sensor lane1",
                         "filament_switch_sensor lane2"}));
        CHECK(hw.mmu_type() == AmsType::AD5X_IFS);
        // The objects are state, not sensors — they must not land in the
        // sensor list FilamentSensorManager reads.
        const auto& sensors = hw.filament_sensor_names();
        CHECK(std::find(sensors.begin(), sensors.end(), "ifs") == sensors.end());
        CHECK(std::find(sensors.begin(), sensors.end(), "ifs_materials") == sensors.end());
    }
    SECTION("ifs_materials alone — IFS board unplugged but registry readable") {
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array({"toolhead", "save_variables", "ifs_materials"}));
        CHECK(hw.mmu_type() == AmsType::AD5X_IFS);
    }
    SECTION("a real MMU outranks the module objects") {
        helix::PrinterDiscovery hw;
        hw.parse_objects(
            json::array({"toolhead", "save_variables", "mmu", "ifs", "ifs_materials"}));
        CHECK(hw.mmu_type() == AmsType::HAPPY_HARE);
    }
    SECTION("no module objects, no detection") {
        helix::PrinterDiscovery hw;
        // A stock-named sensor alone must NOT read as an IFS printer.
        hw.parse_objects(json::array({"toolhead", "extruder", "filament_switch_sensor toolhead"}));
        CHECK(hw.mmu_type() == AmsType::NONE);
    }
}

TEST_CASE("AD5X IFS owns the module's stock-named sensors", "[ams][ad5x_ifs][ifs_module]") {
    helix::PrinterDiscovery hw;
    CHECK(AmsBackendAd5xIfs::owns_filament_sensor("toolhead", hw));
    CHECK(AmsBackendAd5xIfs::owns_filament_sensor("lane1", hw));
    CHECK(AmsBackendAd5xIfs::owns_filament_sensor("lane4", hw));
    // Not every bare name: the claim is shape-exact, and the caller routes on
    // the detected printer type, so these negatives pin the predicate itself.
    CHECK_FALSE(AmsBackendAd5xIfs::owns_filament_sensor("runout_sensor", hw));
    CHECK_FALSE(AmsBackendAd5xIfs::owns_filament_sensor("toolhead2", hw));
    CHECK_FALSE(AmsBackendAd5xIfs::owns_filament_sensor("lane", hw));
    CHECK_FALSE(AmsBackendAd5xIfs::owns_filament_sensor("lanes", hw));
}

TEST_CASE("AD5X IFS does not latch on a requested-but-missing object echo",
          "[ams][ad5x_ifs][ifs_module]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // on_started()'s objects.query lists `ifs` / `ifs_materials`
    // unconditionally, and Klipper echoes a requested-but-missing object as an
    // EMPTY dict (the key stays present — the same shape the _ifs_vars probe
    // guards against). On ZMOD firmware that echo must not stand the module
    // path up: it would retire every ZMOD poll and reroute every op to macros
    // that firmware does not have.
    Ad5xIfsTestAccess::handle_status(
        backend, json{{"ifs", json::object()}, {"ifs_materials", json::object()}});
    CHECK_FALSE(Ad5xIfsTestAccess::module_live(backend));

    // A frame with real fields still latches afterwards.
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));
    CHECK(Ad5xIfsTestAccess::module_live(backend));
}

TEST_CASE("AD5X IFS applies a pushed module frame", "[ams][ad5x_ifs][ifs_module]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE_FALSE(Ad5xIfsTestAccess::module_live(backend));

    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(2, {1, 2}));
    REQUIRE(Ad5xIfsTestAccess::module_live(backend));

    CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));
    CHECK(Ad5xIfsTestAccess::port_presence(backend, 1));
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));

    // active_channel is the F13 chan — same seated authority zmod's Chan is.
    CHECK(backend.get_current_slot() == 1);
}

TEST_CASE("AD5X IFS installs the identity tool map when the module goes live",
          "[ams][ad5x_ifs][ifs_module]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(backend.get_slot_info(2).mapped_tool < 0); // plugin-less: unmapped

    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));

    // T0..T3 map to slots 1..4 in the module's own macros.
    CHECK(backend.get_slot_info(0).mapped_tool == 0);
    CHECK(backend.get_slot_info(2).mapped_tool == 2);
    CHECK(backend.get_slot_info(3).mapped_tool == 3);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AD5X IFS module routing carries the loaded lane colours to the renderer",
                 "[ams][ad5x_ifs][ifs_module][959]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto owned = std::make_unique<AmsBackendAd5xIfs>(nullptr, nullptr);
    auto* backend = owned.get();
    ams.set_backend(std::move(owned));

    // The standalone-module contract: this firmware family carries no
    // bambufy_*/less_waste_* save_variables, so the module's own frames are the
    // only thing that can populate the routing.
    Ad5xIfsTestAccess::handle_status(*backend, module_ifs_frame(1, {1, 2, 3, 4}));
    Ad5xIfsTestAccess::handle_status(*backend, module_labeled_materials_frame());

    // Backend-level: the attachment map the composition falls back to is
    // identity, not the all -1 "no opinion" a plugin-less backend published
    // before the module latch seeded the table.
    const auto info = backend->get_system_info();
    REQUIRE(info.tool_to_slot_map.size() >= 4);
    CHECK(info.tool_to_slot_map[0] == 0);
    CHECK(info.tool_to_slot_map[1] == 1);
    CHECK(info.tool_to_slot_map[2] == 2);
    CHECK(info.tool_to_slot_map[3] == 3);

    // The accessor the print-status preview actually calls — the real
    // composition, not a hand-rolled mirror of it. The routing resolves to the
    // LANES' colours (T0 purple, T1 orange, T2 cyan, T3 white), never to the
    // slicer's stand-ins the preview fell back to on this firmware family.
    const auto colors = ams.routed_tool_colors();
    REQUIRE(colors.size() >= 4);
    CHECK(colors[0] == 0xA03CF7);
    CHECK(colors[1] == 0xFF8800);
    CHECK(colors[2] == 0x00C8FF);
    CHECK(colors[3] == 0xFFFFFF);

    ams.clear_backends();
    ams.deinit_subjects();
}

TEST_CASE("AD5X IFS module identity installs when a latched plugin owns no tool map",
          "[ams][ad5x_ifs][ifs_module][959]") {
    // has_ifs_vars_ latches on the plugin's `_colors` rows alone; `_tools` is
    // not required. A latched-but-mapless plugin must not block the identity
    // install — every slot's mapped_tool would stay -1 and the op dispatch's
    // change_tool rewrite would have no tool to name.
    const json colors_only{
        {"bambufy_colors", json::array({"#A03CF7", "#7EC8E3", "#101010", "#FFFFFF"})}};

    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    Ad5xIfsTestAccess::parse_vars(backend, colors_only);
    REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));

    const auto info = backend.get_system_info();
    REQUIRE(info.tool_to_slot_map.size() >= 4);
    CHECK(info.tool_to_slot_map[0] == 0);
    CHECK(info.tool_to_slot_map[1] == 1);
    CHECK(info.tool_to_slot_map[2] == 2);
    CHECK(info.tool_to_slot_map[3] == 3);
    CHECK(backend.get_slot_info(0).mapped_tool == 0);
    CHECK(backend.get_slot_info(3).mapped_tool == 3);
}

TEST_CASE("AD5X IFS module identity yields to a parsed plugin tool map",
          "[ams][ad5x_ifs][ifs_module][959]") {
    // A plugin crossover: T0 -> port 2, T1 -> port 1, T2 -> port 4, T3 -> port 3.
    // Identity would answer [0,1,2,3] and get every one of them wrong.
    const json crossover{{"bambufy_tools", json::array({2, 1, 4, 3})}};

    SECTION("plugin vars observed before the module goes live") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        // Model what on_started established before any save_variables row
        // arrived: the plugin's `_IFS_VARS` macro exists, so the parse may
        // trust the plugin namespace (the flag starts true — pessimistic).
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend, crossover);
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // The module latch must not install identity over a table the plugin
        // already owns.
        Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));

        const auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() >= 4);
        CHECK(info.tool_to_slot_map[0] == 1);
        CHECK(info.tool_to_slot_map[1] == 0);
        CHECK(info.tool_to_slot_map[2] == 3);
        CHECK(info.tool_to_slot_map[3] == 2);
        CHECK(backend.get_slot_info(0).mapped_tool == 1);
        CHECK(backend.get_slot_info(1).mapped_tool == 0);
    }

    SECTION("plugin vars observed after the module goes live") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));
        REQUIRE(backend.get_slot_info(0).mapped_tool == 0); // identity installed

        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend, crossover);
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        const auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() >= 4);
        CHECK(info.tool_to_slot_map[0] == 1);
        CHECK(info.tool_to_slot_map[1] == 0);
        CHECK(info.tool_to_slot_map[2] == 3);
        CHECK(info.tool_to_slot_map[3] == 2);
    }
}

TEST_CASE("AD5X IFS module identity returns when the plugin contract demotes",
          "[ams][ad5x_ifs][ifs_module][1420]") {
    // The module latch is one-shot, so it cannot correct the table a second
    // time. A plugin whose contract is withdrawn mid-session must therefore
    // hand the table back at the demote, or the removed plugin's routing is
    // what the UI and the preview keep resolving for the rest of the session.
    // Crossover: T0 -> lane 2, T1 -> lane 1, T2 -> lane 4, T3 -> lane 3 — every
    // entry differs from the identity the module's own macros route.
    const json crossover{{"bambufy_tools", json::array({2, 1, 4, 3})}};

    SECTION("Klipper rejects _IFS_VARS as an unknown command") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend, crossover);

        Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));
        REQUIRE(Ad5xIfsTestAccess::module_live(backend));
        // The latch correctly yields to the plugin while the plugin still owns
        // the table — this is the state the demote has to undo.
        REQUIRE(backend.get_system_info().tool_to_slot_map[0] == 1);

        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Unknown command:\"_IFS_VARS\"");
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        const auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() >= 4);
        CHECK(info.tool_to_slot_map[0] == 0);
        CHECK(info.tool_to_slot_map[1] == 1);
        CHECK(info.tool_to_slot_map[2] == 2);
        CHECK(info.tool_to_slot_map[3] == 3);
        CHECK(backend.get_slot_info(0).mapped_tool == 0);
        CHECK(backend.get_slot_info(3).mapped_tool == 3);
    }

    SECTION("FIRMWARE_RESTART unloads the plugin under a running session") {
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend, crossover);

        Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));
        REQUIRE(backend.get_system_info().tool_to_slot_map[0] == 1);

        Ad5xIfsTestAccess::apply_ifs_vars_macro_absent(backend);
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        const auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() >= 4);
        CHECK(info.tool_to_slot_map[0] == 0);
        CHECK(info.tool_to_slot_map[1] == 1);
        CHECK(info.tool_to_slot_map[2] == 2);
        CHECK(info.tool_to_slot_map[3] == 3);
        CHECK(backend.get_slot_info(1).mapped_tool == 1);
    }

    SECTION("tools the plugin mapped past the lane count are released too") {
        // `_tools` is a 16-entry tool-indexed array. Leaving a high tool aimed
        // at a lane keeps it in get_system_info()'s attachment map, which is
        // published without the contract gate get_tool_mapping() applies.
        json high = crossover;
        high["bambufy_tools"] = json::array({2, 1, 4, 3, 5, 5, 2, 5});

        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend, high);
        Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));
        REQUIRE(Ad5xIfsTestAccess::tool_map(backend)[6] == 2);

        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Unknown command:\"_IFS_VARS\"");

        CHECK(Ad5xIfsTestAccess::tool_map(backend)[6] == AmsBackendAd5xIfs::UNMAPPED_PORT);
    }

    SECTION("a live wire table owns the map and survives the demote") {
        // The module echoes its own tool_map by subscription, so it is already
        // current — identity would overwrite a deliberate IFS_MAP_TOOL routing.
        const json wire{
            {"ifs", json{{"tool_map", json{{"0", 3}, {"1", 4}, {"2", 1}, {"3", 2}}}}}};

        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend, crossover);
        Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));
        Ad5xIfsTestAccess::handle_status(backend, wire);
        REQUIRE(backend.get_system_info().tool_to_slot_map[0] == 2);

        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Unknown command:\"_IFS_VARS\"");

        const auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() >= 4);
        CHECK(info.tool_to_slot_map[0] == 2);
        CHECK(info.tool_to_slot_map[1] == 3);
        CHECK(info.tool_to_slot_map[2] == 0);
        CHECK(info.tool_to_slot_map[3] == 1);
    }

    SECTION("without the module there is no firmware default to restore") {
        // Native ZMOD routes through the plugin's table alone. Nothing else
        // claims T0..T3 map to lanes 1..4 there, so the demote has no better
        // answer to install.
        AmsBackendAd5xIfs backend(nullptr, nullptr);
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        Ad5xIfsTestAccess::parse_vars(backend, crossover);
        REQUIRE_FALSE(Ad5xIfsTestAccess::module_live(backend));

        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Unknown command:\"_IFS_VARS\"");

        CHECK(Ad5xIfsTestAccess::tool_map(backend)[0] == 2);
        CHECK(Ad5xIfsTestAccess::tool_map(backend)[1] == 1);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AD5X IFS macro recheck re-derives the tool map when the plugin is gone",
                 "[ams][ad5x_ifs][ifs_module][1420]") {
    // The whole path a plugin uninstall takes: notify_klippy_ready fires the
    // macro re-query, Klipper answers that `_ifs_vars` is not there, and the
    // reply lands on the main thread through the update queue.
    helix::test::FakeMoonrakerClient client;
    AmsBackendAd5xIfs backend(nullptr, &client);

    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    Ad5xIfsTestAccess::parse_vars(backend,
                                  json{{"bambufy_tools", json::array({2, 1, 4, 3})}});
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}));
    REQUIRE(backend.get_system_info().tool_to_slot_map[0] == 1);

    Ad5xIfsTestAccess::recheck_ifs_vars_macro(backend);
    REQUIRE_FALSE(client.rpc_calls.empty());
    const auto& call = client.rpc_calls.back();
    REQUIRE(call.method == "printer.objects.query");
    REQUIRE(call.success_cb);

    // Klipper's webhooks query returns the key with an empty dict for an object
    // the printer does not have — presence alone never means the macro loaded.
    call.success_cb(json{{"result", {{"status", {{"gcode_macro _ifs_vars", json::object()}}}}}});
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    const auto info = backend.get_system_info();
    REQUIRE(info.tool_to_slot_map.size() >= 4);
    CHECK(info.tool_to_slot_map[0] == 0);
    CHECK(info.tool_to_slot_map[1] == 1);
    CHECK(info.tool_to_slot_map[2] == 2);
    CHECK(info.tool_to_slot_map[3] == 3);
    CHECK(backend.get_slot_info(2).mapped_tool == 2);
}

TEST_CASE("AD5X IFS honors a loaded-only module diff frame", "[ams][ad5x_ifs][ifs_module]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(1, {1}));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));

    // THE GATE. Moonraker sends only what changed, so a spool going into lane
    // 4 arrives as loaded_channels alone with no active_channel. Presence
    // updates must not live inside the channel block's entry condition.
    Ad5xIfsTestAccess::handle_status(backend,
                                     json{{"ifs", {{"loaded_channels", json::array({1, 4})}}}});

    CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));
    CHECK(Ad5xIfsTestAccess::port_presence(backend, 3));
}

TEST_CASE("AD5X IFS applies pushed module materials slots", "[ams][ad5x_ifs][ifs_module]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Presence first: colours are only trusted for lanes the silk sensors see.
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(1, {1, 2, 3, 4}));
    Ad5xIfsTestAccess::handle_status(backend, module_materials_frame());

    CHECK(backend.get_slot_info(0).material == "PLA");
    CHECK(backend.get_slot_info(0).color_rgb == 0xA03CF7);
    // "#F80" is the 3-digit form the stock UI writes — expanded, not dropped.
    CHECK(backend.get_slot_info(1).material == "PETG");
    CHECK(backend.get_slot_info(1).color_rgb == 0xFF8800);
    // An unlabelled slot is null on the wire, not "?" — either way, no
    // material renders where the UI should show "--".
    CHECK(backend.get_slot_info(2).material.empty());
    CHECK(backend.get_slot_info(3).material == "ABS");
    CHECK(backend.get_slot_info(3).color_rgb == 0xFFFFFF);
}

TEST_CASE("AD5X IFS module ifs_loaded is the seated authority", "[ams][ad5x_ifs][ifs_module]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // Latch the module, then deliver the module's own success-confirmed lane
    // record (SAVE_VARIABLE at the end of IFS_LOAD; survives restarts).
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {3}));
    Ad5xIfsTestAccess::handle_status(
        backend, json{{"save_variables", {{"variables", {{"ifs_loaded", 3}}}}}});

    CHECK(backend.get_current_slot() == 2);

    // Unload clears it back to 0 — nothing seated.
    Ad5xIfsTestAccess::handle_status(
        backend, json{{"save_variables", {{"variables", {{"ifs_loaded", 0}}}}}});
    CHECK(backend.get_current_slot() < 0);
}

TEST_CASE("AD5X IFS module driver_error surfaces as ERROR", "[ams][ad5x_ifs][ifs_module]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}, "ready"));
    REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);

    Ad5xIfsTestAccess::handle_status(backend, json{{"ifs",
                                                    {{"activity", "driver_error"},
                                                     {"error", "stepper driver fault: overcurrent"},
                                                     {"active_channel", 0},
                                                     {"loaded_channels", json::array()}}}});
    CHECK(Ad5xIfsTestAccess::action(backend) == AmsAction::ERROR);
    CHECK(backend.get_system_info().operation_detail.find("overcurrent") != std::string::npos);
}

TEST_CASE("AD5X IFS module activity tracks an externally-started op",
          "[ams][ad5x_ifs][ifs_module]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // A slicer's T1 or a console IFS_LOAD sets no tracker and installs no ack
    // callback here — the board's activity is the only signal it produces.
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}, "loading"));
    CHECK(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}, "ready"));
    CHECK(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);

    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {}, "unloading"));
    CHECK(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);
}

TEST_CASE("AD5X IFS module ops dispatch the module's macros", "[ams][ad5x_ifs][ifs_module]") {
    TestableModuleBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    // Seat lane 2 so the unload routes to the toolhead, not a cold eject.
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(2, {1, 2}));
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    REQUIRE(backend.get_current_slot() == 1);
    backend.captured_gcodes.clear();

    SECTION("load") {
        REQUIRE(backend.load_filament(2).success());
        CHECK(backend.has_gcode("IFS_LOAD SLOT=3"));
        CHECK_FALSE(backend.has_gcode_containing("INSERT_PRUTOK_IFS"));
    }
    SECTION("toolhead unload") {
        REQUIRE(backend.unload_filament(1).success());
        CHECK(backend.has_gcode("IFS_UNLOAD SLOT=2"));
        CHECK_FALSE(backend.has_gcode_containing("_IFS_REMOVE_CURRENT_PRUTOK"));
    }
    SECTION("unload whatever is active sends it bare") {
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.has_gcode("IFS_UNLOAD"));
    }
    SECTION("cold lane eject") {
        REQUIRE(backend.eject_lane(0).success());
        CHECK(backend.has_gcode("IFS_EJECT SLOT=1"));
        CHECK_FALSE(backend.has_gcode_containing("IFS_F11"));
    }
    SECTION("tool change uses the slicer spelling") {
        REQUIRE(backend.change_tool(1).success());
        CHECK(backend.has_gcode("T1"));
        CHECK_FALSE(backend.has_gcode_containing("A_CHANGE_FILAMENT"));
    }
    SECTION("fault recovery and abort use the module's commands") {
        REQUIRE(backend.recover().success());
        CHECK(backend.has_gcode("IFS_RESET_DRIVER"));
        REQUIRE(backend.cancel().success());
        CHECK(backend.has_gcode("IFS_STOP"));
        CHECK_FALSE(backend.has_gcode_containing("IFS_UNLOCK"));
    }
    SECTION("load-free slot selection is declined, not fed") {
        const auto err = backend.select_slot(0);
        REQUIRE_FALSE(err.success());
        CHECK(err.result == AmsResult::NOT_SUPPORTED);
        CHECK(backend.captured_gcodes.empty());
    }
}

TEST_CASE("AD5X IFS set_slot_info writes through IFS_SET_MATERIAL on the module",
          "[ams][ad5x_ifs][ifs_module]") {
    TestableModuleBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(0, {1, 2, 3, 4}));

    SlotInfo info;
    info.color_rgb = 0x7EC8E3;
    info.material = "PLA";

    REQUIRE(backend.set_slot_info(1, info, /*persist=*/true).success());

    // Bare hex — klipper's parser eats '#' as a comment start, and the module
    // re-prefixes on its side. SLOT is 1-based.
    CHECK(backend.has_gcode("IFS_SET_MATERIAL SLOT=2 TYPE=PLA COLOR=7EC8E3"));
    // The zmod write path (Adventurer5M.json via Moonraker upload) must not
    // also have run — with a null api it would have failed the call above.
}

TEST_CASE("AD5X IFS module poll stand-down", "[ams][ad5x_ifs][ifs_module]") {
    TestableModuleBackend backend;
    Ad5xIfsTestAccess::set_running(backend, true);
    // Seat lane 2 so an unload takes the toolhead route, whose dispatch calls
    // schedule_zcolor_query("toolhead_unload") unconditionally — the path the
    // gate has to swallow. (request_resync() proves nothing here: it has its
    // own module early-return, so the schedule gate could vanish and that
    // call would still schedule nothing.)
    Ad5xIfsTestAccess::handle_status(backend, module_ifs_frame(2, {1, 2}));
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    REQUIRE(Ad5xIfsTestAccess::module_live(backend));
    REQUIRE(backend.get_current_slot() == 1);

    const auto before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    REQUIRE(backend.unload_filament(1).success());
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);

    // And a manual resync is a no-op rather than a re-read.
    backend.request_resync();
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);
}

TEST_CASE("AD5X IFS module colour normalisation", "[ams][ad5x_ifs][ifs_module]") {
    CHECK(Ad5xIfsTestAccess::normalize_module_color("#A03CF7") == "A03CF7");
    CHECK(Ad5xIfsTestAccess::normalize_module_color("A03CF7") == "A03CF7");
    CHECK(Ad5xIfsTestAccess::normalize_module_color("#F80") == "FF8800");
    CHECK(Ad5xIfsTestAccess::normalize_module_color("#ff8800") == "FF8800");
    // Not-a-colour returns empty, which the apply path reads as "no reading"
    // — never as black.
    CHECK(Ad5xIfsTestAccess::normalize_module_color("").empty());
    CHECK(Ad5xIfsTestAccess::normalize_module_color("#12345").empty());
    CHECK(Ad5xIfsTestAccess::normalize_module_color("#GGGGGG").empty());
    CHECK(Ad5xIfsTestAccess::normalize_module_color(std::string()).empty());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AD5X IFS: a table collapsing every tool onto one lane says nothing",
                 "[ams][ad5x_ifs][ifs_module][provenance][1422]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    auto owned = std::make_unique<AmsBackendAd5xIfs>(nullptr, nullptr);
    auto* backend = owned.get();
    ams.set_backend(std::move(owned));

    // A plugin table that names port 1 for every tool. This backend does not
    // echo its table back from the printer and no tool has been aimed at a lane
    // from our UI, so nothing stands behind the shape — and a routing carrying
    // no per-tool information must leave the file's own palette alone rather
    // than paint a four-colour model in one lane's purple.
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(*backend, false);
    Ad5xIfsTestAccess::parse_vars(*backend, json{{"bambufy_tools", json::array({1, 1, 1, 1})}});
    Ad5xIfsTestAccess::handle_status(*backend, module_ifs_frame(1, {1, 2, 3, 4}));
    Ad5xIfsTestAccess::handle_status(*backend, module_labeled_materials_frame());

    const std::vector<int> collapsed{0, 0, 0, 0};
    REQUIRE(backend->get_tool_mapping() == collapsed);
    REQUIRE(backend->tool_mapping_origin() == helix::printer::ToolMappingOrigin::Unvouched);
    CHECK(ams.routed_tool_colors().empty());

    ams.clear_backends();
    ams.deinit_subjects();
}
