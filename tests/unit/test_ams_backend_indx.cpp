// SPDX-License-Identifier: GPL-3.0-or-later

// AmsBackendIndx against synthetic status shaped like Bondtech's upstream
// macros (BondtechAB/INDX). Gcode is captured, never sent.

#include "ui_batch_filament_modal.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/filament_accounting_cases.h"
#include "../test_helpers/gcode_recording_api.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/recovery_modal_presenter_test_access.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_indx.h"
#include "ams_error_bridge.h"
#include "ams_state.h"
#include "filament_accounting.h"
#include "macro_param_cache.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"

#include <any>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// The macros phase 3 reads beyond the required contract.
const std::vector<std::string> kPhase3Macros = {
    "MODE_STEALTH",     "MODE_NORMAL",        "MODE_SPORT",       "TC_CYCLE_ALL",
    "MANUAL_TOOL_SEAT", "MANUAL_TOOL_REMOVE", "SET_TOOL_FILAMENT"};

json indx_objects(int tools = 4, const std::vector<std::string>& extra = {}) {
    json objects = json::array({"indx", "save_variables", "extruder", "heater_bed", "toolhead",
                                "gcode_move", "configfile", "gcode_macro TOOL_POSITIONS",
                                "gcode_macro _TOOLCHANGE_VARS", "gcode_macro CHANGE_TOOL",
                                "gcode_macro PARK_TOOL", "gcode_macro LOAD_FILAMENT"});
    for (int i = 0; i < tools; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
    }
    for (const auto& macro : extra) {
        objects.push_back("gcode_macro " + macro);
    }
    return objects;
}

json without(const json& objects, const std::string& name) {
    json out = json::array();
    for (const auto& o : objects) {
        if (o.get<std::string>() != name) {
            out.push_back(o);
        }
    }
    return out;
}

PrinterDiscovery discover(const json& objects, int config_tools = 4) {
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    const json config = {
        {"gcode_macro TOOL_POSITIONS", {{"variable_tool_count", std::to_string(config_tools)}}}};
    hw.parse_config_keys(config);
    MacroParamCache::instance().populate_from_configfile(config, hw.macros());
    return hw;
}

/// MacroParamCache is process-wide: every case here leaves it empty.
struct IndxFixture {
    ~IndxFixture() {
        MacroParamCache::instance().clear();
    }
};

using IndxHelper = test::GcodeCaptureBackend<AmsBackendIndx>;

json active(int tool) {
    return json{{"save_variables", {{"variables", {{"active_tool", tool}}}}}};
}

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX backend: discovery sizes the tools", "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(), 6));
    CHECK(b.get_type() == AmsType::INDX);
    CHECK(b.get_system_info().total_slots == 6);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: active_tool is the mounted tool",
                 "[ams][indx][status]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(2));
    CHECK(b.get_current_tool() == 2);
    CHECK(b.get_current_slot() == 2);
    b.feed(json{{"save_variables", {{"variables", {{"toolchange_count", 9}}}}}});
    CHECK(b.get_current_tool() == 2);
    b.feed(active(-1));
    CHECK(b.get_current_tool() == -1);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: a status tool_count re-sizes the slots",
                 "[ams][indx][status]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(), 0));
    CHECK(b.get_system_info().total_slots == 0);
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"tool_count", 3}}}});
    CHECK(b.get_system_info().total_slots == 3);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: change sends T<n>, or CHANGE_TOOL without the macro",
                 "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(without(indx_objects(), "gcode_macro T3")));
    b.feed(active(0));
    REQUIRE(b.change_tool(1).success());
    CHECK(b.sent().back() == "T1");

    IndxHelper c;
    c.set_discovery(discover(without(indx_objects(), "gcode_macro T3")));
    c.feed(active(0));
    REQUIRE(c.change_tool(3).success());
    CHECK(c.sent().back() == "CHANGE_TOOL TOOL=3");
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: park sends PARK_TOOL", "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(1));
    REQUIRE(b.park_active_tool().success());
    CHECK(b.sent().back() == "PARK_TOOL");
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: nothing is sent when the macros are not recognized",
                 "[ams][indx][commands][contract]") {
    IndxHelper b;
    b.set_discovery(discover(without(indx_objects(), "gcode_macro CHANGE_TOOL")));
    b.feed(active(0));
    CHECK_FALSE(b.change_tool(0).success());
    CHECK_FALSE(b.park_active_tool().success());
    CHECK(b.sent().empty());
    auto err = b.current_error();
    REQUIRE(err.has_value());
    CHECK(err->detail.find("gcode_macro CHANGE_TOOL") != std::string::npos);
}

TEST_CASE_METHOD(IndxFixture,
                 "INDX backend: a half-seated tool or uncalibrated docks refuse motion",
                 "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(json{{"save_variables", {{"variables", {{"active_tool", 0}, {"seat_suspect", 2}}}}}});
    CHECK_FALSE(b.change_tool(1).success());
    REQUIRE(b.current_error().has_value());
    CHECK(b.current_error()->recovery_actions.empty());

    b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", -1}}}}},
                {"gcode_macro TOOL_POSITIONS", {{"docks_calibrated", false}}}});
    CHECK_FALSE(b.current_error().has_value());
    CHECK_FALSE(b.change_tool(1).success());
    CHECK(b.sent().empty());
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: load names a preset or a temperature",
                 "[ams][indx][commands]") {
    test::RegisteredBackend<IndxHelper> harness;
    IndxHelper& b = *harness;
    b.set_discovery(discover(indx_objects()));
    b.feed(json{
        {"save_variables", {{"variables", {{"active_tool", -1}}}}},
        {"gcode_macro LOAD_FILAMENT",
         {{"filament_presets", {{"PETG", {{"temp", 240}, {"density", 1.27}, {"hc", 1.3}}}}}}}});
    SlotInfo petg = b.get_slot_info(1);
    petg.material = "PETG";
    REQUIRE(test::apply_edit(b, 1, petg).success());
    REQUIRE(b.get_slot_info(1).material == "PETG");
    REQUIRE(b.load_filament(1).success());
    CHECK(b.sent().back() == "LOAD_FILAMENT TOOL=1 TYPE=PETG");
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: unload is manual, a no-heat tool has no filament ops",
                 "[ams][indx][commands]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", json::array({3})}}}});
    CHECK(b.head_filament_ops(0).load == FilamentOpSupport::Automatic);
    CHECK(b.head_filament_ops(0).unload == FilamentOpSupport::Manual);
    CHECK(b.head_filament_ops(3).load == FilamentOpSupport::Unavailable);
    CHECK_FALSE(b.load_filament(3).success());
    CHECK(b.sent().empty());

    IndxHelper no_load;
    no_load.set_discovery(discover(without(indx_objects(), "gcode_macro LOAD_FILAMENT")));
    CHECK(no_load.head_filament_ops(0).load == FilamentOpSupport::Manual);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: heating needs a mounted tool", "[ams][indx][heat]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    CHECK(b.nozzle_heat_blocked_reason().empty()); // no reading yet
    b.feed(active(-1));
    CHECK_FALSE(b.nozzle_heat_blocked_reason().empty());
    b.feed(active(1));
    CHECK(b.nozzle_heat_blocked_reason().empty());
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", json::array({1})}}}});
    CHECK_FALSE(b.nozzle_heat_blocked_reason().empty());
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: per-extruder usage is never charged to a slot",
                 "[ams][indx][spool]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(2));
    CHECK_FALSE(b.slot_for_extruder(0).has_value());
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: own spool macros manage the active spool",
                 "[ams][indx][spool]") {
    auto hw = discover(indx_objects(2), 2);
    MacroParamCache::instance().populate_from_configfile(
        json{{"gcode_macro T0", {{"gcode", "SET_ACTIVE_SPOOL ID=3\nCHANGE_TOOL TOOL=0"}}}},
        hw.macros());
    IndxHelper managed;
    managed.set_discovery(hw);
    CHECK(managed.manages_active_spool());

    MacroParamCache::instance().clear();
    IndxHelper stock;
    stock.set_discovery(hw);
    CHECK_FALSE(stock.manages_active_spool());
}

TEST_CASE_METHOD(IndxFixture,
                 "INDX backend: reports the manages_active_spool the shared case table claims",
                 "[ams][indx][spool][plugin_filament]") {
    const auto rows = helix::test::ownership_rows_for("INDX");
    REQUIRE(rows.size() >= 2);
    for (const auto& row : rows) {
        INFO(row["name"].get<std::string>());
        auto hw = discover(indx_objects(2), 2);
        MacroParamCache::instance().clear();
        if (row["macros_set_active_spool"].get<bool>()) {
            MacroParamCache::instance().populate_from_configfile(
                json{{"gcode_macro T0", {{"gcode", "SET_ACTIVE_SPOOL ID=3\nCHANGE_TOOL TOOL=0"}}}},
                hw.macros());
        }
        IndxHelper b;
        b.set_discovery(hw);
        CHECK(b.manages_active_spool() == row["manages_active_spool"].get<bool>());
    }
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: the mounted tool is the feed signal",
                 "[ams][indx][spool][plugin_filament]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(3), 3));
    const auto signals = b.feed_slot_signals();
    REQUIRE(signals.size() == 1);
    CHECK(signals[0].object == "save_variables");
    CHECK(signals[0].path == std::vector<std::string>{"variables", "active_tool"});
    REQUIRE(signals[0].value_slots.size() == 3);
    INFO("each value is spelled as the status reports it, naming its own slot");
    for (int tool = 0; tool < 3; ++tool) {
        const json status = active(tool);
        const std::string key =
            feed_signal_key(status["save_variables"]["variables"]["active_tool"]);
        const auto& entry = signals[0].value_slots[static_cast<size_t>(tool)];
        CHECK(entry.first == key);
        CHECK(entry.second == tool);
    }
}

namespace {

/// An INDX backend whose print is paused, without a printer behind it.
class PausedIndx : public IndxHelper {
  public:
    bool paused = true;

  protected:
    [[nodiscard]] bool print_paused() const override {
        return paused;
    }
};

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX backend: no swap while a print is paused",
                 "[ams][indx][commands][paused]") {
    PausedIndx b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(1));
    const AmsError change = b.change_tool(2);
    CHECK_FALSE(change.success());
    CHECK(change.user_msg.find("different nozzle") != std::string::npos);
    CHECK_FALSE(b.park_active_tool().success());
    CHECK_FALSE(b.load_filament(2).success());
    CHECK(b.sent().empty());
    CHECK_FALSE(b.traits().filament_ops_self_home);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: the mounted tool loads while a print is paused",
                 "[ams][indx][commands][paused]") {
    PausedIndx b;
    b.set_discovery(discover(indx_objects()));
    b.feed(active(1));
    REQUIRE(b.load_filament(1).success());
    CHECK(b.sent().back().rfind("LOAD_FILAMENT TOOL=1 ", 0) == 0);
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: an active_tool past the tool count is unknown",
                 "[ams][indx][status]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(), 4));
    b.feed(active(1));
    REQUIRE(b.get_current_tool() == 1);
    b.feed(active(7));
    CHECK(b.get_current_tool() == 1);
    CHECK(b.nozzle_heat_blocked_reason().empty());
}

TEST_CASE_METHOD(IndxFixture, "INDX backend: a frame that changes nothing is not news",
                 "[ams][indx][status]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects()));
    int events = 0;
    b.set_event_callback([&events](const std::string&, const std::string&) { ++events; });
    b.feed(active(2));
    const int after_first = events;
    REQUIRE(after_first > 0);
    b.feed(active(2));
    b.feed(json{{"gcode_macro _TOOLCHANGE_VARS", {{"toolhead_state", 0}}}});
    b.feed(json{{"gcode_macro _TOOLCHANGE_VARS", {{"toolhead_state", 0}}}});
    CHECK(events == after_first + 1);
}

// ============================================================================
// Device actions
// ============================================================================

namespace {

/// An INDX backend whose nozzle target the test sets.
class HeatedIndx : public IndxHelper {
  public:
    double target_c = 0.0;

  protected:
    [[nodiscard]] double nozzle_target_c() const override {
        return target_c;
    }
};

const helix::printer::DeviceAction* find_action(const std::vector<helix::printer::DeviceAction>& as,
                                                const std::string& id, int tool_index = -1) {
    for (const auto& a : as) {
        if (a.id == id && (tool_index < 0 || a.tool_index == tool_index)) {
            return &a;
        }
    }
    return nullptr;
}

std::vector<std::string> section_ids(const AmsBackend& b) {
    std::vector<std::string> ids;
    for (const auto& s : b.get_device_sections()) {
        ids.push_back(s.id);
    }
    return ids;
}

std::string info_text(const helix::printer::DeviceAction* a) {
    REQUIRE(a != nullptr);
    const auto* text = std::any_cast<std::string>(&a->current_value);
    REQUIRE(text != nullptr);
    return *text;
}

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX device: sections follow the macros present",
                 "[ams][indx][device]") {
    HeatedIndx all;
    all.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    all.feed(active(1));
    CHECK(section_ids(all) ==
          std::vector<std::string>{"speed", "calibration", "maintenance", "tool_offsets"});

    HeatedIndx some;
    some.set_discovery(
        discover(indx_objects(4, {"MANUAL_TOOL_SEAT", "MANUAL_TOOL_REMOVE", "SET_TOOL_FILAMENT"})));
    some.feed(active(1));
    CHECK(section_ids(some) == std::vector<std::string>{"calibration", "tool_offsets"});

    HeatedIndx unrecognized;
    unrecognized.set_discovery(
        discover(without(indx_objects(4, kPhase3Macros), "gcode_macro CHANGE_TOOL")));
    unrecognized.feed(active(1));
    for (const auto& a : unrecognized.get_device_actions()) {
        INFO(a.id);
        CHECK_FALSE((a.section == "speed" || a.section == "maintenance"));
    }
}

TEST_CASE_METHOD(IndxFixture, "INDX device: speed mode reads _TOOLCHANGE_VARS.mode",
                 "[ams][indx][device]") {
    HeatedIndx b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(json{{"gcode_macro _TOOLCHANGE_VARS", {{"mode", "sport"}}}});
    auto actions = b.get_device_actions();
    const auto* speed = find_action(actions, "speed_mode");
    REQUIRE(speed != nullptr);
    CHECK(speed->type == helix::printer::ActionType::DROPDOWN);
    CHECK(speed->options == std::vector<std::string>{"Stealth", "Normal", "Sport"});
    CHECK(std::any_cast<std::string>(speed->current_value) == "Sport");
    CHECK_FALSE(speed->moves_machine);

    b.feed(json{{"gcode_macro _TOOLCHANGE_VARS", {{"mode", "ludicrous"}}}});
    actions = b.get_device_actions();
    speed = find_action(actions, "speed_mode");
    REQUIRE(speed != nullptr);
    CHECK_FALSE(speed->current_value.has_value());
    CHECK(speed->enabled);
    CHECK(b.sent().empty());

    REQUIRE(b.execute_device_action("speed_mode", std::string("Stealth")).success());
    CHECK(b.sent() == std::vector<std::string>{"MODE_STEALTH"});
    CHECK_FALSE(b.execute_device_action("speed_mode", std::string("Warp")).success());
    CHECK(b.sent().size() == 1);
}

TEST_CASE_METHOD(IndxFixture,
                 "INDX device: calibrations need a mounted tool with a hotend and a cold target",
                 "[ams][indx][device]") {
    HeatedIndx b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(active(-1));
    auto actions = b.get_device_actions();
    const auto* heater = find_action(actions, "calibrate_heater");
    const auto* fan = find_action(actions, "calibrate_fan");
    REQUIRE(heater != nullptr);
    REQUIRE(fan != nullptr);
    CHECK_FALSE(heater->enabled);
    CHECK_FALSE(fan->enabled);
    CHECK_THAT(heater->disable_reason, Catch::Matchers::ContainsSubstring("No tool"));

    b.feed(active(1));
    b.target_c = 210.0;
    actions = b.get_device_actions();
    heater = find_action(actions, "calibrate_heater");
    fan = find_action(actions, "calibrate_fan");
    CHECK_FALSE(heater->enabled);
    CHECK_THAT(heater->disable_reason, Catch::Matchers::ContainsSubstring("heater off"));
    CHECK(fan->enabled);

    b.target_c = 0.0;
    actions = b.get_device_actions();
    heater = find_action(actions, "calibrate_heater");
    fan = find_action(actions, "calibrate_fan");
    CHECK(heater->enabled);
    CHECK(fan->enabled);
    for (const auto* a : {heater, fan}) {
        CHECK(a->moves_machine);
        CHECK_FALSE(a->allowed_while_paused);
    }
    CHECK(heater->guided_flow == "indx_heater_calibration");
    CHECK(fan->guided_flow == "indx_fan_calibration");
}

TEST_CASE_METHOD(IndxFixture, "INDX device: seat a tool by hand", "[ams][indx][device]") {
    HeatedIndx bare;
    bare.set_discovery(discover(indx_objects()));
    const auto bare_actions = bare.get_device_actions();
    CHECK(find_action(bare_actions, "seat_by_hand") == nullptr);

    HeatedIndx b;
    b.set_discovery(discover(indx_objects(6, kPhase3Macros), 6));
    b.feed(json{{"save_variables", {{"variables", {{"active_tool", -1}, {"seat_suspect", 2}}}}},
                {"gcode_macro TOOL_POSITIONS", {{"docks_calibrated", false}}}});
    const auto actions = b.get_device_actions();
    const auto* seat = find_action(actions, "seat_by_hand");
    REQUIRE(seat != nullptr);
    CHECK(seat->section == "calibration");
    CHECK(seat->enabled);
    CHECK(seat->moves_machine);
    CHECK_FALSE(seat->allowed_while_paused);
    CHECK(seat->guided_flow == indx::kSeatByHandFlowId);

    REQUIRE(b.execute_device_action("seat_by_hand", std::any(3)).success());
    CHECK(b.sent() == std::vector<std::string>{"MANUAL_TOOL_SEAT TOOL=3"});
    CHECK_FALSE(b.execute_device_action("seat_by_hand", std::any(9)).success());
    CHECK_FALSE(b.execute_device_action("seat_by_hand", std::any(std::string("3"))).success());
    CHECK(b.sent().size() == 1);
}

TEST_CASE_METHOD(IndxFixture, "INDX device: cycle every tool", "[ams][indx][device]") {
    HeatedIndx b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(json{{"save_variables", {{"variables", {{"active_tool", 0}, {"seat_suspect", 2}}}}}});
    auto actions = b.get_device_actions();
    const auto* cycle = find_action(actions, "cycle_all");
    REQUIRE(cycle != nullptr);
    CHECK_FALSE(cycle->enabled);
    CHECK_THAT(cycle->disable_reason, Catch::Matchers::ContainsSubstring("half seated"));
    CHECK(cycle->guided_flow == "indx_cycle_all");

    b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", -1}}}}}});
    actions = b.get_device_actions();
    cycle = find_action(actions, "cycle_all");
    REQUIRE(cycle != nullptr);
    CHECK(cycle->enabled);
    REQUIRE(b.execute_device_action("cycle_all").success());
    CHECK(b.sent() == std::vector<std::string>{"TC_CYCLE_ALL"});
}

TEST_CASE_METHOD(IndxFixture, "INDX device: tool offsets are read-only rows",
                 "[ams][indx][device]") {
    HeatedIndx b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(json{{"save_variables",
                 {{"variables",
                   {{"active_tool", 0},
                    {"t1_offset_x", 0.12},
                    {"t1_offset_y", -0.045},
                    {"t1_offset_z", 0.03},
                    {"global_z_offset", -0.05}}}}}});
    auto actions = b.get_device_actions();
    CHECK(info_text(find_action(actions, "tool_offset", 1)) == "X 0.120  Y -0.045  Z 0.030 mm");
    CHECK(info_text(find_action(actions, "tool_offset", 0)) == "-");
    CHECK(info_text(find_action(actions, "global_z_offset")) == "-0.050 mm");
    for (const auto& a : actions) {
        if (a.section == "tool_offsets") {
            CHECK(a.type == helix::printer::ActionType::INFO);
        }
    }

    b.feed(active(2));
    actions = b.get_device_actions();
    CHECK(info_text(find_action(actions, "tool_offset", 1)) == "X 0.120  Y -0.045  Z 0.030 mm");
    CHECK(info_text(find_action(actions, "global_z_offset")) == "-0.050 mm");

    b.feed(json{{"save_variables", {{"variables", {{"t2_offset_z", 0.1}}}}}});
    actions = b.get_device_actions();
    CHECK(info_text(find_action(actions, "tool_offset", 2)) == "X -  Y -  Z 0.100 mm");
}

namespace {

/// An INDX backend sending through a recording API, so the rpc's own answer
/// and timeout are what the test drives.
class RecordingIndx : public AmsBackendIndx {
  public:
    RecordingIndx(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
        : AmsBackendIndx(api, client) {
        running_ = true;
    }
    ~RecordingIndx() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

    bool paused = false;

  protected:
    [[nodiscard]] double nozzle_target_c() const override {
        return 0.0;
    }
    [[nodiscard]] bool print_paused() const override {
        return paused;
    }
};

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX device: a calibration reports when it ends",
                 "[ams][indx][device]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    test::GcodeRecordingApi api{client, state};
    RecordingIndx b{&api, &client};
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(active(1));

    int calls = 0;
    AmsError last;
    auto on_done = [&](const AmsError& r) {
        ++calls;
        last = r;
    };

    SECTION("success arrives with the macro's answer") {
        api.hold_acks = true;
        REQUIRE(b.run_device_action("calibrate_heater", {}, on_done).success());
        REQUIRE(api.sent == std::vector<std::string>{"INDX_CALIBRATE"});
        CHECK(api.timeouts == std::vector<uint32_t>{indx::kCalibrationTimeoutMs});
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 0);
        api.ack_held();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 1);
        CHECK(last.success());
    }
    SECTION("a timeout is reported as one") {
        api.time_out = {"INDX_CALIBRATE"};
        REQUIRE(b.run_device_action("calibrate_heater", {}, on_done).success());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 1);
        CHECK(last.result == AmsResult::TIMEOUT);
    }
    SECTION("a dropped connection reads as no answer, not a failure") {
        api.connection_lost = {"INDX_CALIBRATE"};
        REQUIRE(b.run_device_action("calibrate_heater", {}, on_done).success());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 1);
        CHECK(last.result == AmsResult::TIMEOUT);
    }
    SECTION("Klipper's refusal is a failure") {
        api.fail = {"INDX_FAN_CALIBRATE"};
        REQUIRE(b.run_device_action("calibrate_fan", {}, on_done).success());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(calls == 1);
        CHECK_FALSE(last.success());
        CHECK(last.result != AmsResult::TIMEOUT);
    }
    SECTION("the tool cycle is tracked too") {
        REQUIRE(b.run_device_action("cycle_all", {}, on_done).success());
        helix::ui::UpdateQueue::instance().drain();
        CHECK(api.sent == std::vector<std::string>{"TC_CYCLE_ALL"});
        CHECK(calls == 1);
        CHECK(last.success());
    }
}

// ============================================================================
// Seat-fault recovery
// ============================================================================

TEST_CASE_METHOD(IndxFixture,
                 "INDX recovery: a half-seated tool offers seat and remove behind confirms",
                 "[ams][indx][recovery]") {
    const json fault = {{"save_variables", {{"variables", {{"seat_suspect", 2}}}}}};

    IndxHelper b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(fault);
    auto err = b.current_error();
    REQUIRE(err.has_value());
    REQUIRE(err->recovery_actions.size() == 2);
    const auto& seat = err->recovery_actions[0];
    const auto& remove = err->recovery_actions[1];
    CHECK(seat.gcode == "MANUAL_TOOL_SEAT TOOL=2");
    CHECK(remove.gcode == "MANUAL_TOOL_REMOVE");
    CHECK(seat.confirm_message == indx::seat_confirm_message(2));
    CHECK(remove.confirm_message == indx::remove_confirm_message(2));
    CHECK_THAT(seat.confirm_message, Catch::Matchers::ContainsSubstring("Tool 3 (T2)"));
    CHECK_THAT(remove.confirm_message, Catch::Matchers::ContainsSubstring("Tool 3 (T2)"));
    CHECK(seat.style == "primary");
    CHECK(remove.style == "danger");

    IndxHelper bare;
    bare.set_discovery(discover(indx_objects()));
    bare.feed(fault);
    REQUIRE(bare.current_error().has_value());
    CHECK(bare.current_error()->recovery_actions.empty());

    b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", -1}}}}}});
    CHECK_FALSE(b.current_error().has_value());
}

// ============================================================================
// Batch Load
// ============================================================================

namespace {

/// A registered INDX on a recording API with 4 tools, presets for PLA and
/// PETG, PLA on slot 0 and PETG on slot 2.
struct BatchRig {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    test::GcodeRecordingApi api{client, state};
    test::RegisteredBackend<RecordingIndx> harness{&api, &client};
    RecordingIndx& b = *harness;

    explicit BatchRig(const json& objects) {
        b.set_discovery(discover(objects));
        b.feed(json{{"save_variables", {{"variables", {{"active_tool", -1}}}}},
                    {"gcode_macro LOAD_FILAMENT",
                     {{"filament_presets",
                       {{"PLA", {{"temp", 210}, {"density", 1.24}, {"hc", 1.8}}},
                        {"PETG", {{"temp", 240}, {"density", 1.27}, {"hc", 1.3}}}}}}}});
        set_material(0, "PLA");
        set_material(2, "PETG");
    }
    void set_material(int slot, const char* material) {
        SlotInfo info = b.get_slot_info(slot);
        info.material = material;
        REQUIRE(test::apply_edit(b, slot, info).success());
    }
};

} // namespace

TEST_CASE_METHOD(IndxFixture, "INDX batch: one LOAD_FILAMENT per tool, in order",
                 "[ams][indx][batch]") {
    BatchRig rig(indx_objects());
    CHECK(rig.b.max_heads_per_filament_op() == 4);
    REQUIRE(rig.b.load_filament_batch({2, 0}).success());
    REQUIRE(rig.api.sent.size() == 1);
    CHECK(rig.api.sent[0] == "LOAD_FILAMENT TOOL=2 TYPE=PETG\nLOAD_FILAMENT TOOL=0 TYPE=PLA");
    CHECK(rig.api.timeouts[0] == 2 * IMoonrakerAPI::kAmsOperationTimeoutMs);
}

TEST_CASE_METHOD(IndxFixture, "INDX batch: one ineligible tool refuses the whole batch",
                 "[ams][indx][batch]") {
    BatchRig rig(indx_objects());
    rig.b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", json::array({3})}}}});
    const AmsError no_heat = rig.b.load_filament_batch({0, 3});
    CHECK_FALSE(no_heat.success());
    CHECK_THAT(no_heat.user_msg, Catch::Matchers::ContainsSubstring("no hotend"));
    CHECK(rig.api.sent.empty());

    rig.b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", 1}}}}}});
    CHECK_FALSE(rig.b.load_filament_batch({0}).success());
    CHECK(rig.api.sent.empty());

    rig.b.feed(
        json{{"save_variables", {{"variables", {{"seat_suspect", -1}, {"active_tool", 1}}}}}});
    rig.b.paused = true;
    const AmsError docked = rig.b.load_filament_batch({1, 2});
    CHECK_FALSE(docked.success());
    CHECK_THAT(docked.user_msg, Catch::Matchers::ContainsSubstring("different nozzle"));
    CHECK(rig.api.sent.empty());
    REQUIRE(rig.b.load_filament_batch({1}).success());
    REQUIRE(rig.api.sent.size() == 1);
    CHECK(rig.api.sent[0].rfind("LOAD_FILAMENT TOOL=1 ", 0) == 0);
}

TEST_CASE_METHOD(IndxFixture, "INDX batch: unload is not a batch", "[ams][indx][batch]") {
    BatchRig rig(indx_objects());
    CHECK_FALSE(rig.b.unload_filament_batch({0}).success());
    CHECK(rig.api.sent.empty());
}

TEST_CASE_METHOD(IndxFixture, "INDX batch: no LOAD_FILAMENT, no batch", "[ams][indx][batch]") {
    IndxHelper b;
    b.set_discovery(discover(without(indx_objects(), "gcode_macro LOAD_FILAMENT")));
    CHECK(b.max_heads_per_filament_op() == 1);
}

// ============================================================================
// Heater-model follow-up after a material edit
// ============================================================================

namespace {

const json kPresetFrame = {{"gcode_macro LOAD_FILAMENT",
                            {{"filament_presets",
                              {{"PLA", {{"temp", 210}, {"density", 1.24}, {"hc", 1.8}}},
                               {"PETG", {{"temp", 240}, {"density", 1.27}, {"hc", 1.3}}},
                               {"ABS", {{"temp", 250}, {"density", 1.04}, {"hc", 1.4}}}}}}}};

SlotInfo with_material(const char* material) {
    SlotInfo s;
    s.material = material;
    return s;
}

} // namespace

TEST_CASE_METHOD(IndxFixture,
                 "INDX follow-up: a new preset material on the mounted tool offers the model",
                 "[ams][indx][follow_up]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(kPresetFrame);
    b.feed(active(1));
    const auto offer = b.edit_follow_up(1, with_material("PLA"), with_material("PETG"));
    REQUIRE(offer.has_value());
    CHECK_THAT(offer->message, Catch::Matchers::ContainsSubstring("PETG"));
    CHECK_THAT(offer->message, Catch::Matchers::ContainsSubstring("1.27"));
    CHECK_FALSE(offer->title.empty());
    CHECK_FALSE(offer->confirm_label.empty());

    REQUIRE(b.run_edit_follow_up(1, with_material("PLA"), with_material("PETG")).success());
    CHECK(b.sent() ==
          std::vector<std::string>{"SET_TOOL_FILAMENT TOOL=1 DENSITY=1.270 HEAT_CAPACITY=1.300"});
}

TEST_CASE_METHOD(IndxFixture, "INDX follow-up: the confirm re-checks the head before sending",
                 "[ams][indx][follow_up]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(kPresetFrame);
    b.feed(active(1));
    REQUIRE(b.edit_follow_up(1, with_material("PLA"), with_material("PETG")).has_value());

    // A console CHANGE_TOOL while the dialog is up: T3 now owns the live model.
    b.feed(active(3));
    CHECK_FALSE(b.run_edit_follow_up(1, with_material("PLA"), with_material("PETG")).success());
    CHECK(b.sent().empty());

    b.feed(active(1));
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", json::array({1})}}}});
    CHECK_FALSE(b.run_edit_follow_up(1, with_material("PLA"), with_material("PETG")).success());
    CHECK_FALSE(b.run_edit_follow_up(1, with_material("PETG"), with_material("PETG")).success());
    CHECK(b.sent().empty());
}

TEST_CASE_METHOD(IndxFixture, "INDX follow-up: nothing to offer", "[ams][indx][follow_up]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(kPresetFrame);
    b.feed(active(1));
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", json::array({3})}}}});

    CHECK_FALSE(b.edit_follow_up(1, with_material("PLA"), with_material("PLA")).has_value());
    CHECK_FALSE(b.edit_follow_up(1, with_material("PLA"), with_material("pla")).has_value());
    CHECK_FALSE(b.edit_follow_up(1, with_material("PLA"), with_material("Mystery")).has_value());
    CHECK_FALSE(b.edit_follow_up(2, with_material("PLA"), with_material("ABS")).has_value());

    b.feed(active(3));
    CHECK_FALSE(b.edit_follow_up(3, with_material("PLA"), with_material("ABS")).has_value());

    b.feed(active(-1));
    CHECK(b.edit_follow_up(2, with_material("PLA"), with_material("ABS")).has_value());
    CHECK_FALSE(b.edit_follow_up(3, with_material("PLA"), with_material("ABS")).has_value());

    IndxHelper no_macro;
    no_macro.set_discovery(discover(indx_objects()));
    no_macro.feed(kPresetFrame);
    no_macro.feed(active(1));
    CHECK_FALSE(
        no_macro.edit_follow_up(1, with_material("PLA"), with_material("PETG")).has_value());
    CHECK(b.sent().empty());
}

namespace {

struct PrintingIndxFixture : public LVGLTestFixture {
    PrintingIndxFixture() {
        state.init_subjects(false);
    }
    ~PrintingIndxFixture() override {
        MacroParamCache::instance().clear();
    }
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
};

} // namespace

TEST_CASE_METHOD(PrintingIndxFixture, "INDX follow-up: refused while printing",
                 "[ams][indx][follow_up]") {
    test::GcodeRecordingApi api{client, state};
    RecordingIndx b{&api, &client};
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(kPresetFrame);
    b.feed(active(1));
    REQUIRE(b.edit_follow_up(1, with_material("PLA"), with_material("PETG")).has_value());

    helix::test::set_wire_state(state, helix::PrintJobState::PRINTING);
    CHECK_FALSE(b.edit_follow_up(1, with_material("PLA"), with_material("PETG")).has_value());
    CHECK_FALSE(b.run_edit_follow_up(1, with_material("PLA"), with_material("PETG")).success());
    helix::test::set_wire_state(state, helix::PrintJobState::PAUSED);
    CHECK_FALSE(b.run_edit_follow_up(1, with_material("PLA"), with_material("PETG")).success());
    CHECK(api.sent.empty());
}

TEST_CASE_METHOD(IndxFixture, "INDX device: a tracked run holds the backend busy until it ends",
                 "[ams][indx][device]") {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    test::GcodeRecordingApi api{client, state};
    RecordingIndx b{&api, &client};
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(active(1));
    api.hold_acks = true;

    int calls = 0;
    REQUIRE(b.run_device_action("calibrate_heater", {}, [&calls](const AmsError&) {
                 ++calls;
             }).success());
    CHECK(b.device_action_running());

    CHECK_FALSE(b.load_filament(1).success());
    CHECK_FALSE(b.change_tool(2).success());
    CHECK_FALSE(b.execute_device_action("seat_by_hand", std::any(2)).success());
    CHECK_FALSE(b.run_device_action("cycle_all", {}, [](const AmsError&) {}).success());
    auto actions = b.get_device_actions();
    for (const char* id : {"calibrate_heater", "calibrate_fan", "cycle_all", "seat_by_hand"}) {
        INFO(id);
        const auto* a = find_action(actions, id);
        REQUIRE(a != nullptr);
        CHECK_FALSE(a->enabled);
    }
    // Not machine motion: the speed mode still sets.
    CHECK(b.execute_device_action("speed_mode", std::string("Sport")).success());
    CHECK(api.sent == std::vector<std::string>{"INDX_CALIBRATE", "MODE_SPORT"});

    api.ack_held();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(calls == 1);
    CHECK_FALSE(b.device_action_running());
    actions = b.get_device_actions();
    CHECK(find_action(actions, "cycle_all")->enabled);
    api.hold_acks = false;
    CHECK(b.change_tool(2).success());
}

TEST_CASE_METHOD(IndxFixture, "INDX device: the seat-by-hand entry and action agree",
                 "[ams][indx][device]") {
    struct Case {
        const char* name;
        json objects;
    };
    const std::vector<Case> cases = {
        {"seat and remove", indx_objects(4, {"MANUAL_TOOL_SEAT", "MANUAL_TOOL_REMOVE"})},
        {"seat alone", indx_objects(4, {"MANUAL_TOOL_SEAT"})},
        {"remove alone", indx_objects(4, {"MANUAL_TOOL_REMOVE"})},
        {"seat, contract not recognized",
         without(indx_objects(4, {"MANUAL_TOOL_SEAT"}), "gcode_macro CHANGE_TOOL")},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        const auto hw = discover(c.objects);
        HeatedIndx b;
        b.set_discovery(hw);
        b.feed(active(1));
        const auto actions = b.get_device_actions();
        CHECK(indx::supports_seat_by_hand(hw) == (find_action(actions, "seat_by_hand") != nullptr));
    }
    CHECK(indx::supports_seat_by_hand(discover(indx_objects(4, {"MANUAL_TOOL_SEAT"}))));
}

TEST_CASE_METHOD(IndxFixture, "INDX recovery: a half-seated tool holds the ERROR action",
                 "[ams][indx][recovery]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b.feed(active(0));
    CHECK(b.get_current_action() == AmsAction::IDLE);
    b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", 2}}}}}});
    CHECK(b.get_current_action() == AmsAction::ERROR);
    CHECK_THAT(b.get_system_info().operation_detail,
               Catch::Matchers::ContainsSubstring("half seated"));
    b.feed(json{{"save_variables", {{"variables", {{"seat_suspect", -1}}}}}});
    CHECK(b.get_current_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(LVGLUITestFixture, "INDX recovery: the error bridge presents the seat fault",
                 "[ams][indx][recovery]") {
    IndxFixture cache_reset;
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = std::make_unique<IndxHelper>();
    IndxHelper* b = owned.get();
    ams.set_backend(std::move(owned));
    b->set_discovery(discover(indx_objects(4, kPhase3Macros)));
    b->feed(active(0));
    helix::ui::UpdateQueue::instance().drain();

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    helix::AmsErrorBridge bridge(presenter);
    bridge.start();

    b->feed(json{{"save_variables", {{"variables", {{"seat_suspect", 2}}}}}});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    REQUIRE(presenter.is_visible());
    const auto& actions = RecoveryModalPresenterTestAccess::active_actions(presenter);
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].gcode == "MANUAL_TOOL_SEAT TOOL=2");
    CHECK(actions[1].gcode == "MANUAL_TOOL_REMOVE");

    b->feed(json{{"save_variables", {{"variables", {{"seat_suspect", -1}}}}}});
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(20);
    CHECK_FALSE(presenter.is_visible());

    ams.set_backend(nullptr);
}

TEST_CASE_METHOD(IndxFixture, "INDX batch: a no-hotend row reads like the other disabled rows",
                 "[ams][indx][batch]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(6), 6));
    b.feed(json{{"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", json::array({5})}}}});
    const auto ops = b.head_filament_ops(5);
    CHECK(ops.load == FilamentOpSupport::Unavailable);
    CHECK(ops.load_reason.find("Tool") == std::string::npos);

    helix::ui::BatchFilamentModal::HeadRowText text;
    text.head_noun = helix::ui::LaneNoun::Tool;
    text.slot_noun = helix::ui::LaneNoun::Slot;
    text.head = 5;
    text.slot = 5;
    text.reason = ops.load_reason;
    CHECK(helix::ui::BatchFilamentModal::head_row_label(text) == "Tool 6 (No hotend)");
}

TEST_CASE_METHOD(IndxFixture, "INDX device: a new offset beside an unchanged reading is news",
                 "[ams][indx][device]") {
    IndxHelper b;
    b.set_discovery(discover(indx_objects(4, kPhase3Macros)));
    int events = 0;
    b.set_event_callback([&events](const std::string&, const std::string&) { ++events; });
    b.feed(active(1));
    const int after_first = events;
    b.feed(json{{"save_variables", {{"variables", {{"active_tool", 1}, {"t1_offset_z", 0.03}}}}}});
    CHECK(events == after_first + 1);
}
