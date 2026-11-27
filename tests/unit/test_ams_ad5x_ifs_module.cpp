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
#include "json_utils.h"
#include "printer_discovery.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/scoped_language.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsBackendAd5xIfs;
using helix::AmsState;

using json = nlohmann::json;

namespace {

json make_save_variables(const json& variables) {
    return json{{"save_variables", {{"variables", variables}}}};
}

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
    using AmsBackendAd5xIfs::AmsBackendAd5xIfs;
    TestableModuleBackend() : AmsBackendAd5xIfs(nullptr, nullptr) {}

    std::vector<std::string> captured_gcodes;

    helix::AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        if (fail_gcode) {
            return helix::AmsErrorHelper::command_failed(gcode, "refused");
        }
        return helix::AmsErrorHelper::success();
    }
    /// The next commands fail, as a printer refusing them would.
    bool fail_gcode = false;
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()>) override {
        captured_gcodes.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                                  helix::SharedCallback<void(const MoonrakerError&)>,
                                  bool) override {
        captured_gcodes.push_back(gcode);
        return helix::AmsErrorHelper::success();
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
        CHECK(hw.mmu_type() == helix::AmsType::AD5X_IFS);
        // The objects are state, not sensors — they must not land in the
        // sensor list FilamentSensorManager reads.
        const auto& sensors = hw.filament_sensor_names();
        CHECK(std::find(sensors.begin(), sensors.end(), "ifs") == sensors.end());
        CHECK(std::find(sensors.begin(), sensors.end(), "ifs_materials") == sensors.end());
    }
    SECTION("ifs_materials alone — IFS board unplugged but registry readable") {
        helix::PrinterDiscovery hw;
        hw.parse_objects(json::array({"toolhead", "save_variables", "ifs_materials"}));
        CHECK(hw.mmu_type() == helix::AmsType::AD5X_IFS);
    }
    SECTION("a real MMU outranks the module objects") {
        helix::PrinterDiscovery hw;
        hw.parse_objects(
            json::array({"toolhead", "save_variables", "mmu", "ifs", "ifs_materials"}));
        CHECK(hw.mmu_type() == helix::AmsType::HAPPY_HARE);
    }
    SECTION("no module objects, no detection") {
        helix::PrinterDiscovery hw;
        // A stock-named sensor alone must NOT read as an IFS printer.
        hw.parse_objects(json::array({"toolhead", "extruder", "filament_switch_sensor toolhead"}));
        CHECK(hw.mmu_type() == helix::AmsType::NONE);
    }
}

TEST_CASE("AD5X IFS owns the module's stock-named sensors", "[ams][ad5x_ifs][ifs_module]") {
    helix::PrinterDiscovery hw;
    CHECK(AmsBackendAd5xIfs::owns_filament_sensor("toolhead", hw));
    // Not every bare name: the claim is shape-exact, and the caller routes on
    // the detected printer type, so these negatives pin the predicate itself.
    CHECK_FALSE(AmsBackendAd5xIfs::owns_filament_sensor("runout_sensor", hw));
    CHECK_FALSE(AmsBackendAd5xIfs::owns_filament_sensor("toolhead2", hw));
    // Per-channel presence ("lane1".."lane4") is read from the module's
    // structured `ifs` status object, never from individual sensor names, so
    // this backend claims no "lane<N>" name. PrinterHardware::is_ams_sensor
    // still matches them on its own "lane" substring rule, above the backend
    // predicate; this pins the predicate, not that outcome.
    CHECK_FALSE(AmsBackendAd5xIfs::owns_filament_sensor("lane1", hw));
    CHECK_FALSE(AmsBackendAd5xIfs::owns_filament_sensor("lane4", hw));
}

TEST_CASE("AD5X IFS does not latch on a requested-but-missing object echo",
          "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    // on_started()'s objects.query lists `ifs` / `ifs_materials`
    // unconditionally, and Klipper echoes a requested-but-missing object as an
    // EMPTY dict (the key stays present — the same shape the _ifs_vars probe
    // guards against). On ZMOD firmware that echo must not stand the module
    // path up: it would retire every ZMOD poll and reroute every op to macros
    // that firmware does not have.
    backend.fire_status(json{{"ifs", json::object()}, {"ifs_materials", json::object()}});
    CHECK_FALSE(Ad5xIfsTestAccess::module_live(backend));

    // A frame with real fields still latches afterwards.
    backend.fire_status(module_ifs_frame(0, {}));
    CHECK(Ad5xIfsTestAccess::module_live(backend));
}

TEST_CASE("AD5X IFS applies a pushed module frame", "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    REQUIRE_FALSE(Ad5xIfsTestAccess::module_live(backend));

    backend.fire_status(module_ifs_frame(2, {1, 2}));
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
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    REQUIRE(backend.get_slot_info(2).mapped_tool < 0); // plugin-less: unmapped

    backend.fire_status(module_ifs_frame(0, {}));

    // T0..T3 map to slots 1..4 in the module's own macros.
    CHECK(backend.get_slot_info(0).mapped_tool == 0);
    CHECK(backend.get_slot_info(2).mapped_tool == 2);
    CHECK(backend.get_slot_info(3).mapped_tool == 3);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AD5X IFS module routing carries the loaded lane colours to the renderer",
                 "[ams][ad5x_ifs][ifs_module][959]") {
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(false);

    auto owned = std::make_unique<helix::test::WiredBackend<AmsBackendAd5xIfs>>();
    auto* backend = owned.get();
    ams.set_backend(std::move(owned));
    REQUIRE(backend->start().success());

    // The standalone-module contract: this firmware family carries no
    // bambufy_*/less_waste_* save_variables, so the module's own frames are the
    // only thing that can populate the routing.
    backend->fire_status(module_ifs_frame(1, {1, 2, 3, 4}));
    backend->fire_status(module_labeled_materials_frame());

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

    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    backend.fire_status(make_save_variables(colors_only));
    REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

    backend.fire_status(module_ifs_frame(0, {}));

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
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        // Model what on_started established before any save_variables row
        // arrived: the plugin's `_IFS_VARS` macro exists, so the parse may
        // trust the plugin namespace (the flag starts true — pessimistic).
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(crossover));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // The module latch must not install identity over a table the plugin
        // already owns.
        backend.fire_status(module_ifs_frame(0, {}));

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
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        backend.fire_status(module_ifs_frame(0, {}));
        REQUIRE(backend.get_slot_info(0).mapped_tool == 0); // identity installed

        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(crossover));
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
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(crossover));

        backend.fire_status(module_ifs_frame(0, {}));
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
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(crossover));

        backend.fire_status(module_ifs_frame(0, {}));
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

        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(high));
        backend.fire_status(module_ifs_frame(0, {}));
        REQUIRE(Ad5xIfsTestAccess::tool_map(backend)[6] == 2);

        Ad5xIfsTestAccess::on_gcode_response_line(backend, "// Unknown command:\"_IFS_VARS\"");

        CHECK(Ad5xIfsTestAccess::tool_map(backend)[6] == AmsBackendAd5xIfs::kUnmappedPort);
    }

    SECTION("a live wire table owns the map and survives the demote") {
        // The module echoes its own tool_map by subscription, so it is already
        // current — identity would overwrite a deliberate IFS_MAP_TOOL routing.
        const json wire{{"ifs", json{{"tool_map", json{{"0", 3}, {"1", 4}, {"2", 1}, {"3", 2}}}}}};

        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(crossover));
        backend.fire_status(module_ifs_frame(0, {}));
        backend.fire_status(wire);
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
        helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.fire_status(make_save_variables(crossover));
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
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    auto& client = backend.wire().fake_client();

    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    backend.fire_status(make_save_variables(json{{"bambufy_tools", json::array({2, 1, 4, 3})}}));
    backend.fire_status(module_ifs_frame(0, {}));
    REQUIRE(backend.get_system_info().tool_to_slot_map[0] == 1);

    const auto recheck_answering = [&](const json& macro) {
        Ad5xIfsTestAccess::recheck_ifs_vars_macro(backend);
        REQUIRE_FALSE(client.rpc_calls.empty());
        const auto call = client.rpc_calls.back();
        REQUIRE(call.method == "printer.objects.query");
        REQUIRE(call.success_cb);
        call.success_cb(json{{"result", {{"status", {{"gcode_macro _ifs_vars", macro}}}}}});
        helix::ui::UpdateQueue::instance().drain();
    };

    // Klipper's webhooks query returns the key with an empty dict for an object
    // the printer does not have — presence alone never means the macro loaded.
    recheck_answering(json::object());

    REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    const auto info = backend.get_system_info();
    REQUIRE(info.tool_to_slot_map.size() >= 4);
    CHECK(info.tool_to_slot_map[0] == 0);
    CHECK(info.tool_to_slot_map[1] == 1);
    CHECK(info.tool_to_slot_map[2] == 2);
    CHECK(info.tool_to_slot_map[3] == 3);
    CHECK(backend.get_slot_info(2).mapped_tool == 2);

    // The plugin comes back after a FIRMWARE_RESTART: its standing options are read again.
    recheck_answering(json{{"variable_backup", 1}});
    CHECK(backend.plugin_backup_enabled() == std::optional<bool>(true));
}

TEST_CASE("AD5X IFS honors a loaded-only module diff frame", "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    backend.fire_status(module_ifs_frame(1, {1}));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));

    // THE GATE. Moonraker sends only what changed, so a spool going into lane
    // 4 arrives as loaded_channels alone with no active_channel. Presence
    // updates must not live inside the channel block's entry condition.
    backend.fire_status(json{{"ifs", {{"loaded_channels", json::array({1, 4})}}}});

    CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));
    CHECK(Ad5xIfsTestAccess::port_presence(backend, 3));
}

TEST_CASE("AD5X IFS applies pushed module materials slots", "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;

    // Presence first: colours are only trusted for lanes the silk sensors see.
    backend.fire_status(module_ifs_frame(1, {1, 2, 3, 4}));
    backend.fire_status(module_materials_frame());

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
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    // Latch the module, then deliver the module's own success-confirmed lane
    // record (SAVE_VARIABLE at the end of IFS_LOAD; survives restarts).
    backend.fire_status(module_ifs_frame(0, {3}));
    backend.fire_status(json{{"save_variables", {{"variables", {{"ifs_loaded", 3}}}}}});

    CHECK(backend.get_current_slot() == 2);

    // Unload clears it back to 0 — nothing seated.
    backend.fire_status(json{{"save_variables", {{"variables", {{"ifs_loaded", 0}}}}}});
    CHECK(backend.get_current_slot() < 0);
}

TEST_CASE("AD5X IFS module's own lane record outranks the remembered seated lane",
          "[ams][ad5x_ifs][ifs_module][1897]") {
    // At boot the module reports active_channel=0 with the head loaded, which is
    // its normal idle state, while the remembered lane names lane 4 and the
    // module's save_variables name lane 2.
    helix::test::StartedBackend<TestableModuleBackend> backend;
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 3);
    const json lane2_vars{
        {"save_variables", {{"variables", {{"ifs_loaded", 2}, {"ifs_at_hub", 2}}}}}};

    SECTION("save_variables before the module latches") {
        backend.fire_status(lane2_vars);
        backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    }
    SECTION("save_variables after the module latches") {
        backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
        backend.fire_status(lane2_vars);
        backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    }

    REQUIRE(Ad5xIfsTestAccess::module_live(backend));
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 2);
    CHECK(backend.get_current_slot() == 1);
    // The remembered lane is corrected so a later boot cannot bring lane 4 back.
    CHECK(Ad5xIfsTestAccess::persisted_seated_slot(backend) == std::optional<int>(1));

    backend.captured_gcodes.clear();
    REQUIRE(backend.unload_filament(-1).success());
    CHECK(backend.has_gcode("IFS_UNLOAD SLOT=2"));
    CHECK_FALSE(backend.has_gcode_containing("SLOT=4"));
}

TEST_CASE("AD5X IFS module with nothing recorded does not seat the remembered lane",
          "[ams][ad5x_ifs][ifs_module][1897]") {
    // ifs_loaded=0 is the module saying no load completed; the remembered lane is
    // dropped and the at-hub lane, the module's own record, names the lane.
    helix::test::StartedBackend<TestableModuleBackend> backend;
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 3);
    backend.fire_status(
        json{{"save_variables", {{"variables", {{"ifs_loaded", 0}, {"ifs_at_hub", 2}}}}}});
    backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));

    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    CHECK_FALSE(Ad5xIfsTestAccess::persisted_seated_slot(backend).has_value());
    backend.captured_gcodes.clear();
    REQUIRE(backend.unload_filament(-1).success());
    CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_UNLOAD SLOT=2"});
}

TEST_CASE("AD5X IFS module with no lane record keeps the remembered-lane fallback",
          "[ams][ad5x_ifs][ifs_module][1897]") {
    // No ifs_loaded row at all: the remembered lane is the only record.
    helix::test::StartedBackend<TestableModuleBackend> backend;
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 3);
    backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));

    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 4);
    backend.captured_gcodes.clear();
    REQUIRE(backend.unload_filament(-1).success());
    CHECK(backend.has_gcode("IFS_UNLOAD SLOT=4"));
}

TEST_CASE("AD5X IFS module ignores an ifs_loaded that names no real lane",
          "[ams][ad5x_ifs][ifs_module][1897]") {
    // A save_variables-only update, with no ifs frame after it to re-derive the
    // seated lane, must not seat a lane the IFS does not have.
    helix::test::StartedBackend<TestableModuleBackend> backend;
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    backend.fire_status(module_ifs_frame(0, {1, 2}));
    REQUIRE(Ad5xIfsTestAccess::module_live(backend));

    backend.fire_status(json{{"save_variables", {{"variables", {{"ifs_loaded", 5}}}}}});
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    CHECK(backend.get_current_slot() < 0);
    backend.captured_gcodes.clear();
    (void)backend.unload_filament(-1);
    CHECK_FALSE(backend.has_gcode_containing("SLOT=5"));
}

TEST_CASE("AD5X IFS module keeps the ifs_loaded lane while another lane's eject engages",
          "[ams][ad5x_ifs][ifs_module][1897]") {
    // Lane 2 is loaded; an IFS_EJECT of lane 3 moves active_channel to 3 while
    // lane 3 still reads present. The seated lane must stay on lane 2.
    helix::test::StartedBackend<TestableModuleBackend> backend;
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    backend.fire_status(module_ifs_frame(2, {2, 3}));
    backend.fire_status(json{{"save_variables", {{"variables", {{"ifs_loaded", 2}}}}}});
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 2);

    backend.fire_status(module_ifs_frame(3, {2, 3}));
    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 2);
    CHECK(backend.get_current_slot() == 1);
    backend.captured_gcodes.clear();
    REQUIRE(backend.unload_filament(-1).success());
    CHECK(backend.has_gcode("IFS_UNLOAD SLOT=2"));
    CHECK_FALSE(backend.has_gcode_containing("SLOT=3"));
}

TEST_CASE("AD5X IFS module beside a plugin keeps following active_channel",
          "[ams][ad5x_ifs][ifs_module][1897]") {
    // A lessWaste/bambufy swap need not run IFS_LOAD, so ifs_loaded can lag on
    // a plugin rig; neither the seated lane nor the remembered lane follows it.
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    backend.fire_status(
        json{{"save_variables",
              {{"variables", {{"bambufy_tools", json::array({1, 2, 3, 4})}, {"ifs_loaded", 2}}}}}});
    REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    backend.fire_status(module_ifs_frame(3, {2, 3}));
    REQUIRE(Ad5xIfsTestAccess::module_live(backend));

    CHECK(Ad5xIfsTestAccess::seated_chan(backend) == 3);
    CHECK(Ad5xIfsTestAccess::persisted_seated_slot(backend) == std::optional<int>(2));
}

TEST_CASE("AD5X IFS module writes the remembered lane only when ifs_loaded changes",
          "[ams][ad5x_ifs][ifs_module][1897]") {
    MockPrinter mock_printer;
    helix::test::StartedBackend<TestableModuleBackend> backend;
    Ad5xIfsTestAccess::inject_override_store(
        backend, std::make_unique<helix::ams::FilamentSlotOverrideStore>(&mock_printer.api, "ifs"));
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    const json lane2_vars{{"save_variables", {{"variables", {{"ifs_loaded", 2}}}}}};

    backend.fire_status(module_ifs_frame(0, {1, 2}));
    backend.fire_status(lane2_vars);
    const int posts = mock_printer.client.call_count("server.database.post_item");
    REQUIRE(posts == 1);
    CHECK(mock_printer.client.mock_db_get("helix-screen", "ifs_seated") == json(1));

    for (int i = 0; i < 5; ++i) {
        backend.fire_status(module_ifs_frame(0, {1, 2}));
        backend.fire_status(lane2_vars);
    }
    CHECK(mock_printer.client.call_count("server.database.post_item") == posts);
}

TEST_CASE("AD5X IFS module eject of its loaded lane names a temperature when the module "
          "has none",
          "[ams][ad5x_ifs][ifs_module][1897]") {
    // The head reads empty but ifs_loaded still names a lane (a runout tail
    // cleared, then a restart). IFS_EJECT of the module's loaded lane clears the
    // extruder and takes its temperature from TEMP=, else the slot's material,
    // else the nozzle target; with none of them it raises an error.
    helix::test::StartedBackend<TestableModuleBackend> backend;
    backend.fire_status(module_ifs_frame(0, {1, 2, 3}));
    backend.fire_status(module_materials_frame());
    Ad5xIfsTestAccess::set_head_switch(backend, true, false);
    Ad5xIfsTestAccess::set_head_filament(backend, false);

    const auto record = [&](int ifs_loaded) {
        backend.fire_status(
            json{{"save_variables", {{"variables", {{"ifs_loaded", ifs_loaded}}}}}});
        backend.fire_status(module_ifs_frame(0, {1, 2, 3}));
        backend.captured_gcodes.clear();
    };

    SECTION("an unlabelled loaded lane on a cold nozzle gets the fallback temperature") {
        record(3);
        REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 3);
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_EJECT SLOT=3 TEMP=220"});
    }
    SECTION("a nozzle the user already heated keeps its target") {
        record(3);
        backend.fire_status(json{{"extruder", {{"target", 255.0}, {"temperature", 30.0}}}});
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_EJECT SLOT=3 TEMP=255"});
    }
    SECTION("a labelled loaded lane leaves the temperature to the module's material") {
        record(2);
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_EJECT SLOT=2"});
    }
    SECTION("an unlabelled lane that is not the loaded one needs no temperature") {
        record(1);
        REQUIRE(backend.eject_lane(2).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_EJECT SLOT=3"});
    }
}

TEST_CASE("AD5X IFS module driver_error surfaces as ERROR", "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;
    backend.fire_status(module_ifs_frame(0, {}, "ready"));
    REQUIRE(backend.get_current_action() == helix::AmsAction::IDLE);

    backend.fire_status(json{{"ifs",
                              {{"activity", "driver_error"},
                               {"error", "stepper driver fault: overcurrent"},
                               {"active_channel", 0},
                               {"loaded_channels", json::array()}}}});
    CHECK(backend.get_current_action() == helix::AmsAction::ERROR);
    CHECK(backend.get_system_info().operation_detail.find("overcurrent") != std::string::npos);

    // Without the board's own words the line is a catalog key, worded when shown.
    helix::test::StartedBackend<AmsBackendAd5xIfs> bare;
    bare.fire_status(module_ifs_frame(0, {}, "ready"));
    bare.fire_status(json{{"ifs",
                           {{"activity", "driver_error"},
                            {"active_channel", 0},
                            {"loaded_channels", json::array()}}}});
    helix::ScopedLanguage fr("fr");
    REQUIRE(bare.current_error().has_value());
    CHECK(bare.current_error()->detail == "Erreur du pilote IFS");
}

TEST_CASE("AD5X IFS module clear_fault leaves a driver fault standing and sends nothing",
          "[ams][ad5x_ifs][ifs_module][clear_fault]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    backend.fire_status(module_ifs_frame(0, {}, "ready"));
    backend.fire_status(json{{"ifs",
                              {{"activity", "driver_error"},
                               {"error", "stepper driver fault: overcurrent"},
                               {"active_channel", 0},
                               {"loaded_channels", json::array()}}}});
    REQUIRE(backend.get_current_action() == helix::AmsAction::ERROR);

    // The board reports driver_error once, so a cleared mirror would never come
    // back while the hardware stays faulted.
    CHECK(backend.clear_fault(0).result == helix::AmsResult::NOT_SUPPORTED);
    CHECK(backend.get_current_action() == helix::AmsAction::ERROR);
    CHECK(backend.captured_gcodes.empty());
}

TEST_CASE("AD5X IFS module clear_fault retires its own fault without IFS_UNLOCK",
          "[ams][ad5x_ifs][ifs_module][clear_fault]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    backend.fire_status(module_ifs_frame(0, {}, "ready"));
    Ad5xIfsTestAccess::set_action(backend, helix::AmsAction::LOADING);
    Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
    REQUIRE(backend.get_current_action() == helix::AmsAction::ERROR);

    REQUIRE(backend.clear_fault(0).success());
    CHECK(backend.get_current_action() == helix::AmsAction::IDLE);
    CHECK(backend.captured_gcodes.empty());
}

TEST_CASE("AD5X IFS module activity tracks an externally-started op",
          "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<AmsBackendAd5xIfs> backend;

    // A slicer's T1 or a console IFS_LOAD sets no tracker and installs no ack
    // callback here — the board's activity is the only signal it produces.
    backend.fire_status(module_ifs_frame(0, {}, "loading"));
    CHECK(backend.get_current_action() == helix::AmsAction::LOADING);

    backend.fire_status(module_ifs_frame(0, {}, "ready"));
    CHECK(backend.get_current_action() == helix::AmsAction::IDLE);

    backend.fire_status(module_ifs_frame(0, {}, "unloading"));
    CHECK(backend.get_current_action() == helix::AmsAction::UNLOADING);
}

TEST_CASE("AD5X IFS module slotless unload names a lane the module can retract",
          "[ams][ad5x_ifs][ifs_module][1324]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    // Filament at the head that no lane claims: the module reports no active
    // channel while lanes 1 and 2 hold filament and the head switch reads present.
    backend.fire_status(module_ifs_frame(0, {1, 2}));
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    REQUIRE(Ad5xIfsTestAccess::module_live(backend));
    REQUIRE(backend.get_current_slot() < 0);
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    REQUIRE(backend.toolhead_filament_unaccounted() == std::optional<bool>(true));
    backend.captured_gcodes.clear();

    SECTION("a remembered lane that still holds filament is unloaded by number") {
        Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 1);
        CHECK(backend.unload_lane_question().lanes.empty());
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.has_gcode("IFS_UNLOAD SLOT=2"));
        CHECK_FALSE(backend.has_gcode("IFS_UNLOAD"));
    }
    SECTION("with no remembered lane the present lanes are offered and nothing is sent") {
        Ad5xIfsTestAccess::set_persisted_seated_slot(backend, std::nullopt);
        CHECK(backend.unload_lane_question().lanes == std::vector<int>{0, 1});
        const helix::AmsError err = backend.unload_filament(-1);
        CHECK_FALSE(err.success());
        CHECK(backend.captured_gcodes.empty());
        CHECK(backend.get_current_action() == helix::AmsAction::IDLE);
    }
    SECTION("a remembered lane that has since emptied is not trusted") {
        Ad5xIfsTestAccess::set_persisted_seated_slot(backend, 3);
        CHECK(backend.unload_lane_question().lanes == std::vector<int>{0, 1});
        CHECK_FALSE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes.empty());
    }
    SECTION("an explicit lane is sent as given") {
        Ad5xIfsTestAccess::set_persisted_seated_slot(backend, std::nullopt);
        REQUIRE(backend.unload_filament(0).success());
        CHECK(backend.has_gcode("IFS_UNLOAD SLOT=1"));
    }
}

TEST_CASE("AD5X IFS module offers no lane choice when a lane is accounted for",
          "[ams][ad5x_ifs][ifs_module][1324]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    backend.fire_status(module_ifs_frame(2, {1, 2}));
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    REQUIRE(backend.get_current_slot() == 1);
    CHECK(backend.unload_lane_question().lanes.empty());

    SECTION("an empty head needs no lane either") {
        backend.fire_status(module_ifs_frame(0, {1, 2}));
        Ad5xIfsTestAccess::set_head_switch(backend, true, false);
        Ad5xIfsTestAccess::set_persisted_seated_slot(backend, std::nullopt);
        CHECK(backend.unload_lane_question().lanes.empty());
    }
}

namespace {

using Verdict = AmsBackendAd5xIfs::UnloadLaneVerdict;

AmsBackendAd5xIfs::UnloadLaneDecision table_verdict(bool module, int recorded, int at_hub,
                                                    std::array<bool, 4> present, bool head = true) {
    AmsBackendAd5xIfs::UnloadLaneInputs in;
    in.module = module;
    in.recorded_lane = recorded;
    in.at_hub_lane = at_hub;
    std::copy(present.begin(), present.end(), in.present.begin());
    in.head_loaded = head;
    return AmsBackendAd5xIfs::decide_unload_lane(in);
}

} // namespace

TEST_CASE("AD5X IFS decides how a toolhead unload finds its lane", "[ams][ad5x_ifs][1324]") {
    // Rows that read the same on both firmwares.
    const bool module = GENERATE(true, false);
    CAPTURE(module);
    SECTION("an empty head is not a toolhead unload") {
        CHECK(table_verdict(module, 1, 1, {true, true, false, false}, /*head=*/false).verdict ==
              Verdict::NotToolhead);
    }
    SECTION("row 1: a record names the lane, even one reading empty") {
        const auto d = table_verdict(module, 2, 0, {true, false, false, false});
        CHECK(d.verdict == Verdict::UnloadLane);
        CHECK(d.lane == 2);
    }
    SECTION("rows 3 and 6: one lane holding filament is confirmed, never assumed") {
        const auto d = table_verdict(module, -1, -1, {false, false, true, false});
        CHECK(d.verdict == Verdict::ConfirmLane);
        CHECK(d.lane == 2);
    }
    SECTION("rows 4 and 6: several lanes holding filament are picked from") {
        const auto d = table_verdict(module, -1, -1, {true, false, true, true});
        CHECK(d.verdict == Verdict::PickLane);
        CHECK(d.lane == -1);
    }
    SECTION("row 5: no lane holding filament asks to confirm an extruder-only clear") {
        CHECK(table_verdict(module, -1, -1, {false, false, false, false}).verdict ==
              Verdict::ConfirmClear);
    }

    SECTION("row 2: the module's at-hub lane is unloaded when it still holds filament") {
        const auto d = table_verdict(true, -1, 3, {true, false, false, true});
        CHECK(d.verdict == Verdict::UnloadLane);
        CHECK(d.lane == 3);
    }
    SECTION("row 2: a record outranks the at-hub lane") {
        const auto d = table_verdict(true, 0, 3, {true, false, false, true});
        CHECK(d.verdict == Verdict::UnloadLane);
        CHECK(d.lane == 0);
    }
    SECTION("row 2: an at-hub lane reading empty is not trusted") {
        CHECK(table_verdict(true, -1, 3, {true, true, false, false}).verdict == Verdict::PickLane);
        CHECK(table_verdict(true, -1, 3, {false, false, false, false}).verdict ==
              Verdict::ConfirmClear);
    }
    SECTION("row 2: native ZMOD keeps no at-hub record, so it is ignored") {
        const auto d = table_verdict(false, -1, 3, {false, false, false, true});
        CHECK(d.verdict == Verdict::ConfirmLane);
        CHECK(d.lane == 3);
    }
    SECTION("an out-of-range at-hub lane is ignored") {
        CHECK(table_verdict(true, -1, 7, {true, false, false, false}).verdict ==
              Verdict::ConfirmLane);
    }
}

namespace {

/// The module with filament at the head and nothing recorded in ifs_loaded.
void seat_unrecorded_module(helix::test::StartedBackend<TestableModuleBackend>& backend,
                            std::vector<int> lanes, int at_hub) {
    backend.fire_status(module_ifs_frame(0, std::move(lanes)));
    backend.fire_status(
        json{{"save_variables", {{"variables", {{"ifs_loaded", 0}, {"ifs_at_hub", at_hub}}}}}});
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, std::nullopt);
    REQUIRE(Ad5xIfsTestAccess::module_live(backend));
    REQUIRE(backend.get_current_slot() < 0);
    backend.captured_gcodes.clear();
}

} // namespace

TEST_CASE("AD5X IFS module unloads the lane holding the hub without asking",
          "[ams][ad5x_ifs][ifs_module][1324]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    seat_unrecorded_module(backend, {1, 2, 3}, /*at_hub=*/2);

    CHECK(backend.unload_lane_question().lanes.empty());
    REQUIRE(backend.unload_filament(-1).success());
    CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_UNLOAD SLOT=2"});

    SECTION("an at-hub lane that has emptied asks instead") {
        helix::test::StartedBackend<TestableModuleBackend> emptied;
        seat_unrecorded_module(emptied, {1, 3}, /*at_hub=*/2);
        CHECK(emptied.unload_lane_question().lanes == std::vector<int>{0, 2});
        CHECK_FALSE(emptied.unload_filament(-1).success());
        CHECK(emptied.captured_gcodes.empty());
    }
}

TEST_CASE("AD5X IFS module asks to confirm the one lane holding filament",
          "[ams][ad5x_ifs][ifs_module][1324]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    seat_unrecorded_module(backend, {3}, /*at_hub=*/0);

    const helix::UnloadLaneQuestion question = backend.unload_lane_question();
    CHECK(question.lanes == std::vector<int>{2});
    CHECK_FALSE(question.confirm_clear_toolhead);
    // Never dispatched on the inference alone.
    CHECK_FALSE(backend.unload_filament(-1).success());
    CHECK(backend.captured_gcodes.empty());

    SECTION("a confirmed lane is unloaded by number") {
        REQUIRE(backend.unload_filament(2).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_UNLOAD SLOT=3"});
    }
}

TEST_CASE("AD5X IFS module clears the extruder alone when no lane can be named",
          "[ams][ad5x_ifs][ifs_module][1324]") {
    const std::string clear220 = "_IFS_SENSOR_RESUME\n_IFS_CLEAR_EXTRUDER TEMP=220";
    SECTION("the clear restores the toolhead sensor, then clears and retracts no lane") {
        helix::test::StartedBackend<TestableModuleBackend> backend;
        seat_unrecorded_module(backend, {1, 2}, /*at_hub=*/0);
        REQUIRE(backend.unload_filament(helix::kClearToolheadSlot).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{clear220});
        CHECK(backend.get_current_action() != helix::AmsAction::IDLE);
    }
    SECTION("no lane holding filament asks to confirm the clear, and never clears unasked") {
        helix::test::StartedBackend<TestableModuleBackend> backend;
        seat_unrecorded_module(backend, {}, /*at_hub=*/0);
        const helix::UnloadLaneQuestion question = backend.unload_lane_question();
        CHECK(question.lanes.empty());
        CHECK(question.confirm_clear_toolhead);
        const helix::AmsError err = backend.unload_filament(-1);
        CHECK_FALSE(err.success());
        CHECK(err.result == helix::AmsResult::WRONG_STATE);
        CHECK(backend.captured_gcodes.empty());
        REQUIRE(backend.unload_filament(helix::kClearToolheadSlot).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{clear220});
    }
    SECTION("a nozzle the user already heated keeps its target") {
        helix::test::StartedBackend<TestableModuleBackend> backend;
        seat_unrecorded_module(backend, {}, /*at_hub=*/0);
        backend.fire_status(json{{"extruder", {{"target", 255.0}, {"temperature", 30.0}}}});
        REQUIRE(backend.unload_filament(helix::kClearToolheadSlot).success());
        CHECK(backend.captured_gcodes ==
              std::vector<std::string>{"_IFS_SENSOR_RESUME\n_IFS_CLEAR_EXTRUDER TEMP=255"});
    }
    SECTION("an empty head has nothing to clear") {
        helix::test::StartedBackend<TestableModuleBackend> backend;
        seat_unrecorded_module(backend, {1, 2}, /*at_hub=*/0);
        Ad5xIfsTestAccess::set_head_switch(backend, true, false);
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        CHECK_FALSE(backend.unload_filament(helix::kClearToolheadSlot).success());
        CHECK(backend.captured_gcodes.empty());
    }
    SECTION("an empty head never turns the clear into a cold eject of the recorded lane") {
        // A record can name a lane while the switch reads empty; the empty-head
        // routing would cold-eject that lane, which "clear the toolhead" never means.
        helix::test::StartedBackend<TestableModuleBackend> backend;
        seat_unrecorded_module(backend, {1, 2}, /*at_hub=*/0);
        backend.fire_status(json{{"save_variables", {{"variables", {{"ifs_loaded", 2}}}}}});
        Ad5xIfsTestAccess::set_head_switch(backend, true, false);
        Ad5xIfsTestAccess::set_head_filament(backend, false);
        REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 2);
        const helix::AmsError err = backend.unload_filament(helix::kClearToolheadSlot);
        CHECK_FALSE(err.success());
        CHECK(err.result == helix::AmsResult::WRONG_STATE);
        CHECK(backend.captured_gcodes.empty());
    }
}

TEST_CASE("AD5X IFS module never sends a bare IFS_UNLOAD", "[ams][ad5x_ifs][ifs_module][1324]") {
    // Bare, the module's IFS_UNLOAD falls back on its own records; Helix always
    // names the lane or sends the explicit clear, in every row of the table.
    struct Row {
        const char* name;
        int active;
        std::vector<int> lanes;
        int ifs_loaded;
        int at_hub;
    };
    const std::vector<Row> rows = {
        {"active lane", 2, {1, 2}, 2, 2},    {"recorded lane", 0, {1, 2}, 2, 2},
        {"at-hub lane", 0, {1, 2, 3}, 0, 3}, {"one lane", 0, {3}, 0, 0},
        {"several lanes", 0, {1, 4}, 0, 0},  {"no lanes", 0, {}, 0, 0},
    };
    int unloads_sent = 0;
    for (const Row& row : rows) {
        CAPTURE(row.name);
        for (const int slot : {-1, 0, 1, 2, 3, helix::kClearToolheadSlot}) {
            CAPTURE(slot);
            helix::test::StartedBackend<TestableModuleBackend> backend;
            backend.fire_status(module_ifs_frame(row.active, row.lanes));
            backend.fire_status(json{
                {"save_variables",
                 {{"variables", {{"ifs_loaded", row.ifs_loaded}, {"ifs_at_hub", row.at_hub}}}}}});
            Ad5xIfsTestAccess::set_head_switch(backend, true, true);
            Ad5xIfsTestAccess::set_head_filament(backend, true);
            Ad5xIfsTestAccess::set_persisted_seated_slot(backend, std::nullopt);
            backend.captured_gcodes.clear();
            (void)backend.unload_filament(slot);
            for (const std::string& g : backend.captured_gcodes) {
                if (g.rfind("IFS_UNLOAD", 0) == 0) {
                    ++unloads_sent;
                    CHECK(g.rfind("IFS_UNLOAD SLOT=", 0) == 0);
                }
            }
        }
    }
    // The sweep reached the unload dispatch, not only refusals and ejects.
    CHECK(unloads_sent >= 6);
}

TEST_CASE("AD5X IFS module ops dispatch the module's macros", "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    // Seat lane 2 so the unload routes to the toolhead, not a cold eject.
    backend.fire_status(module_ifs_frame(2, {1, 2}));
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
    SECTION("unload whatever is active names the active lane") {
        REQUIRE(backend.unload_filament(-1).success());
        CHECK(backend.captured_gcodes == std::vector<std::string>{"IFS_UNLOAD SLOT=2"});
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
        CHECK(err.result == helix::AmsResult::NOT_SUPPORTED);
        CHECK(backend.captured_gcodes.empty());
    }
}

TEST_CASE("AD5X IFS apply_user_edit writes through IFS_SET_MATERIAL on the module",
          "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));

    helix::SlotInfo info;
    info.color_rgb = 0x7EC8E3;
    info.material = "PLA";

    REQUIRE(helix::test::apply_edit(backend, 1, info).success());

    // Bare hex — klipper's parser eats '#' as a comment start, and the module
    // re-prefixes on its side. SLOT is 1-based.
    CHECK(backend.has_gcode("IFS_SET_MATERIAL SLOT=2 TYPE=PLA COLOR=7EC8E3"));
    // The zmod write path (Adventurer5M.json via Moonraker upload) must not
    // also have run: nothing answers its read, which would fail the call above.
}

TEST_CASE("AD5X IFS an edit writes its lane record once",
          "[ams][ad5x_ifs][ifs_module][lane_truth][2044]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));

    // What the firmware cannot carry (the brand) is what the record is for.
    helix::SlotInfo info = backend.get_slot_info(1);
    info.brand = "Polymaker";
    const size_t posts = backend.wire().fake_client().rpc_count("server.database.post_item");
    REQUIRE(helix::test::apply_edit(backend, 1, info).success());

    // One record, one write to the printer's database.
    CHECK(backend.wire().fake_client().rpc_count("server.database.post_item") == posts + 1);
}

TEST_CASE("AD5X IFS module poll stand-down", "[ams][ad5x_ifs][ifs_module]") {
    helix::test::StartedBackend<TestableModuleBackend> backend;
    // Seat lane 2 so an unload takes the toolhead route, whose dispatch calls
    // schedule_zcolor_query("toolhead_unload") unconditionally — the path the
    // gate has to swallow. (request_resync() proves nothing here: it has its
    // own module early-return, so the schedule gate could vanish and that
    // call would still schedule nothing.)
    backend.fire_status(module_ifs_frame(2, {1, 2}));
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    REQUIRE(Ad5xIfsTestAccess::module_live(backend));
    REQUIRE(backend.get_current_slot() == 1);

    const auto before = Ad5xIfsTestAccess::zcolor_schedule_count(backend);
    REQUIRE(backend.unload_filament(1).success());
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);

    // And a manual resync is a no-op rather than a re-read, of the IFS; the lane
    // records are still read again.
    const size_t reads = backend.wire().fake_client().rpc_count("server.database.get_item");
    backend.request_resync();
    CHECK(Ad5xIfsTestAccess::zcolor_schedule_count(backend) == before);
    CHECK(backend.wire().fake_client().rpc_count("server.database.get_item") > reads);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "AD5X IFS: a table collapsing every tool onto one lane says nothing",
                 "[ams][ad5x_ifs][ifs_module][provenance][1422]") {
    auto& ams = helix::AmsState::instance();
    ams.init_subjects(false);

    auto owned = std::make_unique<helix::test::WiredBackend<AmsBackendAd5xIfs>>();
    auto* backend = owned.get();
    ams.set_backend(std::move(owned));
    REQUIRE(backend->start().success());

    // A plugin table that names port 1 for every tool. This backend does not
    // echo its table back from the printer and no tool has been aimed at a lane
    // from our UI, so nothing stands behind the shape — and a routing carrying
    // no per-tool information must leave the file's own palette alone rather
    // than paint a four-colour model in one lane's purple.
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(*backend, false);
    backend->fire_status(make_save_variables(json{{"bambufy_tools", json::array({1, 1, 1, 1})}}));
    backend->fire_status(module_ifs_frame(1, {1, 2, 3, 4}));
    backend->fire_status(module_labeled_materials_frame());

    const std::vector<int> collapsed{0, 0, 0, 0};
    REQUIRE(backend->get_tool_mapping() == collapsed);
    REQUIRE(backend->tool_mapping_origin() == helix::printer::ToolMappingOrigin::Unvouched);
    CHECK(ams.routed_tool_colors().empty());

    ams.clear_backends();
    ams.deinit_subjects();
}

// ============================================================================
// A firmware that writes lane_data itself (helix::lane_writer)
// ============================================================================

namespace {

json plugins_frame(const char* api_version) {
    return json{{"ad5x_plugins", {{"api_version", api_version}, {"enabled", json::array()}}}};
}

helix::PrinterDiscovery module_discovery(bool with_plugins) {
    helix::PrinterDiscovery hw;
    std::vector<std::string> objects{"toolhead", "extruder", "save_variables", "ifs",
                                     "ifs_materials"};
    if (with_plugins) {
        objects.emplace_back("ad5x_plugins");
    }
    hw.set_printer_objects(objects);
    return hw;
}

/// A module backend wired, registered (so its lanes file), told what the
/// objects list holds, and started.
struct LaneWriterRig {
    explicit LaneWriterRig(bool with_plugins) : backend(*registered) {
        backend.set_discovery(module_discovery(with_plugins));
        REQUIRE(backend.start().success());
        helix::ui::UpdateQueue::instance().drain();
    }
    /// The namespaces every database write so far went to.
    std::vector<std::string> written_namespaces() {
        std::vector<std::string> out;
        for (const auto& call : backend.wire().fake_client().rpc_calls) {
            if (call.method == "server.database.post_item") {
                out.push_back(helix::json_util::safe_string(call.params, "namespace"));
            }
        }
        return out;
    }
    bool wrote_to(const std::string& ns) {
        const auto all = written_namespaces();
        return std::find(all.begin(), all.end(), ns) != all.end();
    }
    helix::test::RegisteredBackend<helix::test::WiredBackend<TestableModuleBackend>> registered;
    helix::test::WiredBackend<TestableModuleBackend>& backend;
};

/// The writes so far to @p ns, as {key, value}.
std::vector<std::pair<std::string, json>> writes_to(LaneWriterRig& rig, const std::string& ns) {
    std::vector<std::pair<std::string, json>> out;
    for (const auto& call : rig.backend.wire().fake_client().rpc_calls) {
        if (call.method == "server.database.post_item" &&
            helix::json_util::safe_string(call.params, "namespace") == ns) {
            out.emplace_back(helix::json_util::safe_string(call.params, "key"),
                             call.params.contains("value") ? call.params["value"] : json());
        }
    }
    return out;
}

/// The queries for who writes lane_data: the ones naming ad5x_plugins.
std::vector<const helix::test::FakeMoonrakerClient::RpcCall*>
lane_writer_queries(LaneWriterRig& rig) {
    std::vector<const helix::test::FakeMoonrakerClient::RpcCall*> out;
    for (const auto& call : rig.backend.wire().fake_client().rpc_calls) {
        if (call.method == "printer.objects.query" && call.params.contains("objects") &&
            call.params["objects"].contains("ad5x_plugins")) {
            out.push_back(&call);
        }
    }
    return out;
}

helix::SlotInfo linked_spool_edit() {
    helix::SlotInfo info;
    info.color_rgb = 0x7EC8E3;
    info.material = "PLA";
    info.brand = "Polymaker";
    info.spool_name = "Matte PLA";
    info.spoolman_id = 42;
    return info;
}

} // namespace

TEST_CASE("AD5X IFS on a firmware that writes lane_data sends the whole slot identity",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    rig.backend.fire_status(plugins_frame("1.12"));
    rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(rig.backend.firmware_writes_lane_records() == true);

    SECTION("a Generic preset and unset temperatures are left to the firmware") {
        REQUIRE(helix::test::apply_edit(rig.backend, 1, linked_spool_edit()).success());
        CHECK(rig.backend.captured_gcodes ==
              std::vector<std::string>{"IFS_SET_MATERIAL SLOT=2 TYPE=PLA COLOR=7EC8E3 SPOOL_ID=42 "
                                       "VENDOR=\"Polymaker\" NAME=\"Matte PLA\" FILAMENT_ID= "
                                       "BED_TEMP= TEMP="});
    }
    SECTION("a product's Orca id and set temperatures go out") {
        helix::SlotInfo info = linked_spool_edit();
        info.spoolman_orca_filament_id = "GFL99";
        info.bed_temp = 65;
        info.nozzle_temp_min = 210;
        info.nozzle_temp_max = 220;
        REQUIRE(helix::test::apply_edit(rig.backend, 1, info).success());
        CHECK(rig.backend.captured_gcodes ==
              std::vector<std::string>{"IFS_SET_MATERIAL SLOT=2 TYPE=PLA COLOR=7EC8E3 SPOOL_ID=42 "
                                       "VENDOR=\"Polymaker\" NAME=\"Matte PLA\" "
                                       "FILAMENT_ID=\"GFL99\" BED_TEMP=65 TEMP=215"});
    }

    // Our own notes go to the private namespace; lane_data is the firmware's.
    CHECK(rig.wrote_to(AmsBackendAd5xIfs::kOverrideNamespace));
    CHECK_FALSE(rig.wrote_to("lane_data"));
}

TEST_CASE("AD5X IFS refuses an edit until the firmware says who writes lane_data",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    rig.backend.fire_status(module_materials_frame());
    const helix::AmsError refused = helix::test::apply_edit(rig.backend, 1, linked_spool_edit());
    helix::ui::UpdateQueue::instance().drain();

    CHECK(refused.result == helix::AmsResult::BUSY);
    CHECK_FALSE(rig.backend.firmware_writes_lane_records().has_value());
    CHECK(rig.written_namespaces().empty());
    CHECK(rig.backend.captured_gcodes.empty());

    // Once it has said, the same edit goes out as it now must.
    rig.backend.fire_status(plugins_frame("1.12"));
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(helix::test::apply_edit(rig.backend, 1, linked_spool_edit()).success());
    REQUIRE(rig.backend.captured_gcodes.size() == 1);
    CHECK(rig.backend.captured_gcodes.front().rfind(
              "IFS_SET_MATERIAL SLOT=2 TYPE=PLA COLOR=7EC8E3 SPOOL_ID=42 ", 0) == 0);
    CHECK_FALSE(rig.wrote_to("lane_data"));
}

TEST_CASE("AD5X IFS asks again from the start once stopped and started",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    rig.backend.fire_status(plugins_frame("1.12"));
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(rig.backend.firmware_writes_lane_records() == true);

    rig.backend.stop();
    CHECK_FALSE(rig.backend.firmware_writes_lane_records().has_value());
    REQUIRE(rig.backend.start().success());
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK_FALSE(rig.backend.firmware_writes_lane_records().has_value());
    rig.backend.fire_status(plugins_frame("1.11"));
    CHECK(rig.backend.firmware_writes_lane_records() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X IFS asks again when the lane_data writer query fails",
                 "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    REQUIRE(lane_writer_queries(rig).size() == 1);
    // Each ask carries its own short timeout.
    CHECK(lane_writer_queries(rig).back()->timeout_ms ==
          AmsBackendAd5xIfs::kLaneWriterQueryTimeoutMs);
    const auto fail_last = [&rig](const MoonrakerError& err) {
        lane_writer_queries(rig).back()->error_cb(err);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };
    const auto wait_ms = [this](int ms) {
        process_lvgl(ms);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    };
    const MoonrakerError timed_out = MoonrakerError::timeout("printer.objects.query", 5000);

    SECTION("a failed ask waits before it asks again, and an answer opens the store") {
        fail_last(timed_out);
        CHECK(lane_writer_queries(rig).size() == 1);
        wait_ms(AmsBackendAd5xIfs::kLaneWriterRetryDelaysMs[0] + 100);
        const auto queries = lane_writer_queries(rig);
        REQUIRE(queries.size() == 2);
        CHECK_FALSE(rig.backend.firmware_writes_lane_records().has_value());
        queries.back()->success_cb(json{{"result", {{"status", plugins_frame("1.12")}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(rig.backend.firmware_writes_lane_records() == true);
    }
    SECTION("asks that keep failing leave HelixScreen the writer once the waits run out") {
        for (const uint32_t delay : AmsBackendAd5xIfs::kLaneWriterRetryDelaysMs) {
            fail_last(timed_out);
            CHECK_FALSE(rig.backend.firmware_writes_lane_records().has_value());
            wait_ms(static_cast<int>(delay) + 100);
        }
        CHECK(lane_writer_queries(rig).size() ==
              AmsBackendAd5xIfs::kLaneWriterRetryDelaysMs.size() + 1);
        fail_last(timed_out);
        CHECK(rig.backend.firmware_writes_lane_records() == false);
    }
    SECTION("Klipper not ready is no answer: the ask waits for it") {
        for (int i = 0; i < 6; ++i) {
            fail_last(
                MoonrakerError::not_ready("printer.objects.query", "Klippy host not connected"));
            wait_ms(20000);
        }
        CHECK(lane_writer_queries(rig).size() == 1);
        CHECK_FALSE(rig.backend.firmware_writes_lane_records().has_value());
        REQUIRE(rig.backend.wire().fake_client().fire_notification("notify_klippy_ready",
                                                                   json::object()));
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(lane_writer_queries(rig).size() == 2);
    }
    SECTION("a reply without the version leaves HelixScreen the writer") {
        lane_writer_queries(rig).back()->success_cb(
            json{{"result", {{"status", {{"ad5x_plugins", json::object()}}}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(rig.backend.firmware_writes_lane_records() == false);
        rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
        REQUIRE(helix::test::apply_edit(rig.backend, 1, linked_spool_edit()).success());
        CHECK(rig.wrote_to("lane_data"));
    }
}

TEST_CASE("AD5X IFS stays the lane_data writer below the firmware version that writes it",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    const bool with_plugins = GENERATE(true, false);
    CAPTURE(with_plugins);
    LaneWriterRig rig(with_plugins);
    if (with_plugins) {
        rig.backend.fire_status(plugins_frame("1.11"));
    } else {
        // No such firmware: the store opens at start, as it always has.
        CHECK_FALSE(rig.backend.firmware_writes_lane_records().value_or(true));
    }
    rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(helix::test::apply_edit(rig.backend, 1, linked_spool_edit()).success());

    CHECK(rig.backend.captured_gcodes ==
          std::vector<std::string>{"IFS_SET_MATERIAL SLOT=2 TYPE=PLA COLOR=7EC8E3"});
    CHECK(rig.wrote_to("lane_data"));
    CHECK_FALSE(rig.wrote_to(AmsBackendAd5xIfs::kOverrideNamespace));
}

TEST_CASE("AD5X IFS keeps the firmware the lane_data writer for the session",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    rig.backend.fire_status(plugins_frame("1.12"));
    rig.backend.fire_status(plugins_frame("1.11"));
    rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(rig.backend.firmware_writes_lane_records() == true);

    REQUIRE(helix::test::apply_edit(rig.backend, 1, linked_spool_edit()).success());
    CHECK_FALSE(rig.wrote_to("lane_data"));
}

namespace {

/// An `ifs_materials` frame of a firmware that keeps each slot's identity:
/// slot 1 linked to spool @p spool_id, the others unset.
json identity_materials_frame(int spool_id, const char* vendor, const char* name) {
    const json unset{{"spool_id", nullptr},
                     {"vendor", nullptr},
                     {"name", nullptr},
                     {"filament_id", nullptr},
                     {"bed_temp", nullptr}};
    json slots = json::object();
    for (const char* n : {"1", "2", "3", "4"}) {
        slots[n] = json{{"type", "PLA"}, {"color", "#A03CF7"}, {"temp", 220.0}};
        slots[n].update(unset);
    }
    slots["1"].update(json{{"spool_id", spool_id},
                           {"vendor", vendor},
                           {"name", name},
                           {"filament_id", "OFDSrzZ8"},
                           {"bed_temp", 60.0}});
    return json{{"ifs_materials", {{"available", true}, {"channel_count", 4}, {"slots", slots}}}};
}

} // namespace

TEST_CASE("AD5X IFS reads the spool link from the firmware's status",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    rig.backend.fire_status(plugins_frame("1.12"));
    rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    rig.backend.fire_status(identity_materials_frame(7, "Polymaker", "Galaxy"));
    helix::ui::UpdateQueue::instance().drain_until_empty();

    // The private store is empty, so all three are the firmware's.
    helix::SlotInfo slot = rig.backend.get_slot_info(0);
    CHECK(slot.spoolman_id == 7);
    CHECK(slot.brand == "Polymaker");
    CHECK(slot.spool_name == "Galaxy");
    CHECK(slot.material == "PLA");

    // A later frame naming another spool is the reading at once.
    rig.backend.fire_status(identity_materials_frame(9, "Elegoo", "Rapid"));
    helix::ui::UpdateQueue::instance().drain_until_empty();
    slot = rig.backend.get_slot_info(0);
    CHECK(slot.spoolman_id == 9);
    CHECK(slot.brand == "Elegoo");
}

TEST_CASE("AD5X IFS takes an identity the status stated before the firmware's answer",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    // The subscription's first frame lands before the plugin version is known,
    // and the status sends the slot again only when it changes.
    rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    rig.backend.fire_status(identity_materials_frame(7, "Polymaker", "Galaxy"));
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(rig.backend.get_slot_info(0).spoolman_id != 7);

    rig.backend.fire_status(plugins_frame("1.12"));
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(rig.backend.get_slot_info(0).spoolman_id == 7);
    CHECK(rig.backend.get_slot_info(0).brand == "Polymaker");
}

TEST_CASE("AD5X IFS reads lane_data once, for its own notes, on a firmware that writes it",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    rig.backend.wire().respond_to_namespace("lane_data", json::object());
    rig.backend.wire().respond_to_namespace(AmsBackendAd5xIfs::kOverrideNamespace, json::object());
    rig.backend.fire_status(plugins_frame("1.12"));
    helix::ui::UpdateQueue::instance().drain_until_empty();
    rig.backend.request_resync();
    rig.backend.request_resync();
    helix::ui::UpdateQueue::instance().drain_until_empty();

    int lane_data_reads = 0;
    for (const auto& call : rig.backend.wire().fake_client().rpc_calls) {
        if (call.method == "server.database.get_item" &&
            helix::json_util::safe_string(call.params, "namespace") == "lane_data") {
            ++lane_data_reads;
        }
    }
    CHECK(lane_data_reads == 1);
}

TEST_CASE("AD5X IFS shows no spool link from an edit the printer refused",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    rig.backend.fire_status(plugins_frame("1.12"));
    rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
    rig.backend.fire_status(identity_materials_frame(7, "Old", "Spool"));
    helix::ui::UpdateQueue::instance().drain_until_empty();

    rig.backend.fail_gcode = true;
    REQUIRE_FALSE(helix::test::apply_edit(rig.backend, 0, linked_spool_edit()).success());
    // The next frame repaints the slot from what the firmware states.
    rig.backend.fire_status(module_materials_frame());
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(rig.backend.get_slot_info(0).spoolman_id == 7);
}

TEST_CASE("AD5X IFS carries its own lane_data notes into an empty private store",
          "[ams][ad5x_ifs][ifs_module][lane_writer]") {
    LaneWriterRig rig(/*with_plugins=*/true);
    const json ours{{"lane", "0"},
                    {"writer", "helixscreen"},
                    {"material", "PLA"},
                    {"color", "#A03CF7"},
                    {"spool_id", 7},
                    {"total_weight_g", 1000.0},
                    {"helix_color_name", "Sunset Violet"}};
    // Kiln may have signed one of ours before it was read: our keys still say whose.
    const json resigned{
        {"lane", "2"}, {"writer", "kiln"}, {"material", "ABS"}, {"helix_catalog_id", "pick-3"}};
    const json kilns{{"lane", "1"}, {"writer", "kiln"}, {"material", "PETG"}};
    rig.backend.wire().respond_to_namespace(
        "lane_data", {{"lane1", ours}, {"lane2", kilns}, {"lane3", resigned}});

    SECTION("an empty private store takes them once") {
        rig.backend.wire().respond_to_namespace(AmsBackendAd5xIfs::kOverrideNamespace,
                                                json::object());
        rig.backend.fire_status(plugins_frame("1.12"));
        helix::ui::UpdateQueue::instance().drain_until_empty();

        auto carried = writes_to(rig, AmsBackendAd5xIfs::kOverrideNamespace);
        std::sort(carried.begin(), carried.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        REQUIRE(carried.size() == 2);
        CHECK(carried[0].first == "lane1");
        CHECK(carried[0].second["helix_color_name"] == "Sunset Violet");
        CHECK(carried[0].second["total_weight_g"] == 1000.0);
        // A private record keeps no spool id: the firmware's is the lane's.
        CHECK(carried[0].second["spool_id"].is_null());
        CHECK(carried[1].first == "lane3");
        CHECK(carried[1].second["helix_catalog_id"] == "pick-3");
        CHECK(writes_to(rig, "lane_data").empty());

        // A later read finds the private store holding them: nothing again.
        rig.backend.request_resync();
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(writes_to(rig, AmsBackendAd5xIfs::kOverrideNamespace).size() == 2);
    }
    SECTION("an edit made before lane_data is read keeps its record") {
        rig.backend.wire().respond_to_namespace(AmsBackendAd5xIfs::kOverrideNamespace,
                                                json::object());
        rig.backend.wire().hold_namespace("lane_data");
        rig.backend.fire_status(plugins_frame("1.12"));
        rig.backend.fire_status(module_ifs_frame(0, {1, 2, 3, 4}));
        helix::ui::UpdateQueue::instance().drain_until_empty();

        helix::SlotInfo edit = linked_spool_edit();
        edit.color_name = "Ice Blue";
        REQUIRE(helix::test::apply_edit(rig.backend, 0, edit).success());

        // Now the lane_data read lands, ours for lane 1 among it.
        const helix::test::FakeMoonrakerClient::RpcCall* read = nullptr;
        for (const auto& call : rig.backend.wire().fake_client().rpc_calls) {
            if (call.method == "server.database.get_item" &&
                helix::json_util::safe_string(call.params, "namespace") == "lane_data") {
                read = &call;
            }
        }
        REQUIRE(read != nullptr);
        read->success_cb(json{{"result", {{"value", {{"lane1", ours}, {"lane3", resigned}}}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();

        // Lane 1 holds the edit's record only; lane 3 is carried.
        std::vector<std::string> lane1_names;
        for (const auto& [key, value] : writes_to(rig, AmsBackendAd5xIfs::kOverrideNamespace)) {
            if (key == "lane1") {
                lane1_names.push_back(helix::json_util::safe_string(value, "helix_color_name"));
            }
        }
        CHECK(lane1_names == std::vector<std::string>{"Ice Blue"});
        CHECK(writes_to(rig, AmsBackendAd5xIfs::kOverrideNamespace).size() == 2);
    }
    SECTION("a private store already holding records takes nothing") {
        rig.backend.wire().respond_to_namespace(
            AmsBackendAd5xIfs::kOverrideNamespace,
            {{"lane3", {{"lane", "2"}, {"helix_color_name", "Kept"}}}});
        rig.backend.fire_status(plugins_frame("1.12"));
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(writes_to(rig, AmsBackendAd5xIfs::kOverrideNamespace).empty());
    }
}
