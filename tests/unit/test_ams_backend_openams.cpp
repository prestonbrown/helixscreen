// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_test_utils.h"
#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "ams_backend_openams.h"
#include "ams_error.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/openams_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsAction;
using helix::AmsBackendOpenAms;
using helix::AmsResult;
using helix::AmsType;
using helix::PathTopology;
using helix::SlotStatus;
using json = nlohmann::json;

class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

namespace {

/// The real backend, started, with every command it sends captured. Load and
/// unload hand back their completion and error callbacks so a case can decide
/// how the macro ends.
class OpenAmsHarness : public AmsBackendOpenAms {
  public:
    explicit OpenAmsHarness(IMoonrakerAPI* api = nullptr) : AmsBackendOpenAms(api, nullptr) {
        running_ = true;
        set_event_callback(
            [this](const std::string& event, const std::string&) { events.push_back(event); });
    }

    ~OpenAmsHarness() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    void feed(const json& manager) {
        handle_status_update(json{{"method", "notify_status_update"},
                                  {"params", json::array({json{{"oams_manager", manager}}, 0.0})}});
    }

    helix::AmsError execute_gcode(const std::string& gcode) override {
        commands.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }

    void complete_operation() {
        REQUIRE(on_complete);
        on_complete();
        helix::ui::UpdateQueue::instance().drain();
    }

    void fail_operation(const std::string& message) {
        REQUIRE(on_error);
        MoonrakerError error;
        error.message = message;
        on_error(error);
        helix::ui::UpdateQueue::instance().drain();
    }

    std::vector<std::string> operations;
    std::vector<std::string> commands;
    std::vector<std::string> events;
    bool send_succeeds = true;

  protected:
    helix::AmsError
    send_operation_gcode(const std::string& gcode, std::function<void()> complete,
                         std::function<void(const MoonrakerError&)> error) override {
        operations.push_back(gcode);
        on_complete = std::move(complete);
        on_error = std::move(error);
        return send_succeeds ? helix::AmsErrorHelper::success()
                             : helix::AmsErrorHelper::not_connected("fixture send failure");
    }

  private:
    std::function<void()> on_complete;
    std::function<void(const MoonrakerError&)> on_error;
};

json all_commands() {
    return json{{"load", "OPENAMS_LOAD"},
                {"unload", "OPENAMS_UNLOAD"},
                {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                {"reset", "OAMSM_CLEAR_ERRORS"}};
}

json lane(const std::string& state, const json& group = nullptr, const json& slot = nullptr) {
    return json{{"id", "fps"},          {"state", state},     {"current_group", group},
                {"current_slot", slot}, {"following", false}, {"direction", 0},
                {"message", nullptr}};
}

json slot(int id, int bay, bool ready, bool loaded = false) {
    return json{{"id", id}, {"bay", bay}, {"ready", ready}, {"loaded", loaded}};
}

/// One four-bay hub unit on lane `fps`, slot ids 0-3. T0 owns bays 0 and 1,
/// T1 bay 2, T2 bay 3. Bay 0 is empty, the rest hold a spool, nothing loaded.
json manager(json lanes = json::array({lane("unloaded")}), json commands = all_commands()) {
    return json{{"api_version", 1},
                {"schema", "openams.manager"},
                {"ready", true},
                {"commands", std::move(commands)},
                {"lanes", std::move(lanes)},
                {"units",
                 json::array({json{{"id", "1"},
                                   {"name", "unit1"},
                                   {"kind", "oams"},
                                   {"topology", "hub"},
                                   {"lane", "fps"},
                                   {"connected", true},
                                   {"slots", json::array({slot(0, 0, false), slot(1, 1, true),
                                                          slot(2, 2, true), slot(3, 3, true)})}}})},
                {"groups", json::array({json{{"name", "T0"}, {"lane", "fps"}, {"slots", {0, 1}}},
                                        json{{"name", "T1"}, {"lane", "fps"}, {"slots", {2}}},
                                        json{{"name", "T2"}, {"lane", "fps"}, {"slots", {3}}}})},
                {"current_group", nullptr}};
}

/// manager() with slot 2 (T1) loaded.
json loaded_manager(json commands = all_commands()) {
    json m = manager(json::array({lane("loaded", "T1", 2)}), std::move(commands));
    m["units"][0]["slots"][2]["loaded"] = true;
    return m;
}

struct TmpCacheDir {
    std::filesystem::path path;
    explicit TmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("openams_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

// ============================================================================
// Discovery: who claims the printer
// ============================================================================

TEST_CASE("OpenAMS claims the printer only once its status shows the UI API",
          "[ams][openams][discovery]") {
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(json::array({"oams_manager", "fps", "oams unit1", "extruder"}));

    // The name alone decides nothing.
    CHECK_FALSE(discovery.has_mmu());
    CHECK(discovery.detected_ams_systems().empty());
    const json query = discovery.claim_status_query();
    REQUIRE(query.contains("oams_manager"));

    SECTION("a supported API claims it") {
        discovery.settle_status_claims(
            json{{"oams_manager", {{"api_version", 1}, {"schema", "openams.manager"}}}});
        CHECK(discovery.has_mmu());
        CHECK(discovery.mmu_type() == AmsType::OPENAMS);
        REQUIRE(discovery.detected_ams_systems().size() == 1);
        CHECK(discovery.detected_ams_systems()[0].type == AmsType::OPENAMS);
    }

    SECTION("a manager that predates the API does not") {
        discovery.settle_status_claims(json{{"oams_manager", {{"current_group", "T0"}}}});
        CHECK_FALSE(discovery.has_mmu());
        CHECK(discovery.detected_ams_systems().empty());
    }

    SECTION("an API version this build does not read does not") {
        discovery.settle_status_claims(
            json{{"oams_manager", {{"api_version", 2}, {"schema", "openams.manager"}}}});
        CHECK_FALSE(discovery.has_mmu());
    }

    SECTION("a failed query does not") {
        discovery.settle_status_claims(json::object());
        CHECK_FALSE(discovery.has_mmu());
        CHECK(discovery.detected_ams_systems().empty());
    }
}

TEST_CASE("AFC keeps the printer when oams_manager is also present", "[ams][openams][discovery]") {
    const auto afc_first = json::array({"AFC", "AFC_OpenAMS AMS_1", "oams_manager"});
    const auto oams_first = json::array({"oams_manager", "AFC_OpenAMS AMS_1", "AFC"});
    for (const auto& objects : {afc_first, oams_first}) {
        helix::PrinterDiscovery discovery;
        discovery.parse_objects(objects);

        CHECK(discovery.mmu_type() == AmsType::AFC);
        CHECK(discovery.claim_status_query().empty());
        discovery.settle_status_claims(
            json{{"oams_manager", {{"api_version", 1}, {"schema", "openams.manager"}}}});
        CHECK(discovery.mmu_type() == AmsType::AFC);
        REQUIRE(discovery.detected_ams_systems().size() == 1);
        CHECK(discovery.detected_ams_systems()[0].type == AmsType::AFC);
    }
}

// ============================================================================
// Status model
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS snapshot maps units, slots, groups and topology",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    json m = loaded_manager();
    m["units"].push_back(json{{"id", "2"},
                              {"name", "Direct feeder"},
                              {"kind", "feeder"},
                              {"topology", "linear"},
                              {"lane", "fps"},
                              {"connected", true},
                              {"slots", json::array({slot(9, 0, true)})}});
    m["groups"].push_back(json{{"name", "T3"}, {"lane", "fps"}, {"slots", {9}}});
    backend.feed(m);

    const auto info = backend.get_system_info();
    CHECK(info.type == AmsType::OPENAMS);
    REQUIRE(info.units.size() == 2);
    CHECK(info.total_slots == 5);
    CHECK(info.units[0].topology == PathTopology::HUB);
    CHECK(info.units[1].topology == PathTopology::LINEAR);
    CHECK(backend.get_topology() == PathTopology::MIXED);
    CHECK(info.units[0].slots[0].status == SlotStatus::EMPTY);
    CHECK(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    CHECK(info.units[0].slots[2].status == SlotStatus::LOADED);
    CHECK(info.current_slot == 2);
    CHECK(info.current_tool == 1);
    CHECK(info.filament_loaded);
    CHECK(info.units[0].slots[1].mapped_tool == 0);
    CHECK(info.units[1].slots[0].mapped_tool == 3);
    CHECK(backend.get_tool_mapping() == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS keeps the snapshot across partial status updates",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    backend.feed(loaded_manager());
    backend.feed(json{{"lanes", json::array({lane("unloading", "T1", 2)})}});

    const auto info = backend.get_system_info();
    CHECK(info.total_slots == 4);
    CHECK(info.action == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS without a supported API presents nothing, not an error",
                 "[ams][openams]") {
    OpenAmsHarness backend;

    SECTION("a manager that predates the API") {
        backend.feed(json{{"current_group", "T0"}});
    }
    SECTION("an unknown API version") {
        json m = manager();
        m["api_version"] = 2;
        backend.feed(m);
    }

    const auto info = backend.get_system_info();
    CHECK(info.total_slots == 0);
    CHECK(info.units.empty());
    CHECK(info.action == AmsAction::IDLE);
    CHECK(info.operation_detail.empty());
    CHECK(backend.load_filament(0).result != AmsResult::SUCCESS);
    CHECK(backend.operations.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS draws an unknown unit topology as a hub",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    json m = manager();
    m["units"][0]["topology"] = "carousel";
    backend.feed(m);

    CHECK(backend.get_system_info().total_slots == 4);
    CHECK(backend.get_unit_topology(0) == PathTopology::HUB);
    REQUIRE(backend.load_filament(2).success());
    CHECK(backend.operations == std::vector<std::string>{"OPENAMS_LOAD GROUP=T1 SLOT=2"});
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS does not fold two loaded lanes into one current slot",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    json m = manager(json::array({lane("loaded", "T1", 2), lane("loaded", "T2", 3)}));
    m["lanes"][1]["id"] = "fps1";
    backend.feed(m);

    const auto info = backend.get_system_info();
    CHECK(info.filament_loaded);
    CHECK(info.current_slot == -1);
    CHECK(info.current_tool == -1);
    // The v1 unload names no lane, so it cannot say which one it would empty.
    CHECK(backend.unload_filament(2).result == AmsResult::NOT_SUPPORTED);
    CHECK(backend.operations.empty());
}

// ============================================================================
// Load and tool change, through the public entry points
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS load dispatches the advertised command and completes",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    backend.feed(manager());

    REQUIRE(backend.load_filament(1).success());
    CHECK(backend.operations == std::vector<std::string>{"OPENAMS_LOAD GROUP=T0 SLOT=1"});
    CHECK(backend.get_current_action() == AmsAction::LOADING);
    CHECK(backend.get_system_info().pending_target_slot == 1);

    backend.events.clear();
    backend.complete_operation();

    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK(backend.get_system_info().pending_target_slot == -1);
    CHECK(std::find(backend.events.begin(), backend.events.end(),
                    helix::AmsBackend::EVENT_LOAD_COMPLETE) != backend.events.end());
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS load failure unwinds and keeps the reason",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    backend.feed(manager());

    REQUIRE(backend.load_filament(2).success());
    backend.events.clear();
    backend.fail_operation("OpenAMS slot 2 is not ready");

    auto info = backend.get_system_info();
    CHECK(info.action == AmsAction::IDLE);
    CHECK(info.pending_target_slot == -1);
    CHECK(info.operation_detail == "OpenAMS slot 2 is not ready");
    CHECK(std::find(backend.events.begin(), backend.events.end(),
                    helix::AmsBackend::EVENT_LOAD_COMPLETE) == backend.events.end());

    // The reason survives the next frame, and clear_fault() retires it.
    backend.feed(json{{"ready", true}});
    CHECK(backend.get_system_info().operation_detail == "OpenAMS slot 2 is not ready");
    REQUIRE(backend.clear_fault(-1).success());
    CHECK(backend.get_system_info().operation_detail.empty());

    // A new operation is accepted once the failed one has unwound.
    REQUIRE(backend.load_filament(2).success());
    CHECK(backend.operations.size() == 2);
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS unwinds when the load cannot be sent",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    backend.feed(manager());
    backend.send_succeeds = false;

    CHECK_FALSE(backend.load_filament(1).success());
    CHECK(backend.get_current_action() == AmsAction::IDLE);
    CHECK(backend.get_system_info().pending_target_slot == -1);
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS refuses to load a slot with no spool ready",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    backend.feed(manager());

    CHECK(backend.load_filament(0).result == AmsResult::SLOT_NOT_AVAILABLE);
    CHECK(backend.operations.empty());
    CHECK(backend.get_current_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS tool change picks a ready slot in the tool's group",
                 "[ams][openams]") {
    OpenAmsHarness backend;

    SECTION("the first member with a spool ready, skipping an empty bay") {
        backend.feed(manager());
        REQUIRE(backend.change_tool(0).success());
        CHECK(backend.operations == std::vector<std::string>{"OPENAMS_LOAD GROUP=T0 SLOT=1"});
    }

    SECTION("the member already loaded, over an earlier ready one") {
        json m = manager(json::array({lane("loaded", "T0", 1)}));
        m["units"][0]["slots"][0]["ready"] = true;
        m["units"][0]["slots"][1]["loaded"] = true;
        backend.feed(m);
        REQUIRE(backend.change_tool(0).success());
        CHECK(backend.operations == std::vector<std::string>{"OPENAMS_LOAD GROUP=T0 SLOT=1"});
    }

    SECTION("no member ready refuses without sending") {
        json m = manager();
        m["units"][0]["slots"][1]["ready"] = false;
        backend.feed(m);
        CHECK(backend.change_tool(0).result == AmsResult::SLOT_NOT_AVAILABLE);
        CHECK(backend.operations.empty());
    }

    SECTION("a tool with no group refuses without sending") {
        backend.feed(manager());
        CHECK(backend.change_tool(7).result == AmsResult::INVALID_TOOL);
        CHECK(backend.operations.empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS refuses every action while the manager is not ready",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    json m = loaded_manager();
    m["ready"] = false;
    backend.feed(m);

    // Status is still shown.
    CHECK(backend.get_system_info().total_slots == 4);
    CHECK(backend.load_filament(1).result == AmsResult::WRONG_STATE);
    CHECK(backend.change_tool(0).result == AmsResult::WRONG_STATE);
    CHECK(backend.unload_filament(2).result == AmsResult::WRONG_STATE);
    CHECK(backend.operations.empty());
}

// ============================================================================
// Missing commands disable only their own action
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS without load macros still shows status and unloads",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    json commands = all_commands();
    commands.erase("load");
    backend.feed(loaded_manager(commands));

    CHECK(backend.get_system_info().total_slots == 4);
    CHECK(backend.load_filament(1).result == AmsResult::NOT_SUPPORTED);
    CHECK(backend.change_tool(0).result == AmsResult::NOT_SUPPORTED);
    CHECK(backend.operations.empty());

    CHECK(backend.can_unload_from_toolhead(2));
    REQUIRE(backend.unload_filament(2).success());
    CHECK(backend.operations == std::vector<std::string>{"OPENAMS_UNLOAD"});
    backend.complete_operation();
    CHECK(std::find(backend.events.begin(), backend.events.end(),
                    helix::AmsBackend::EVENT_UNLOAD_COMPLETE) != backend.events.end());

    REQUIRE(backend.reset().success());
    CHECK(backend.commands == std::vector<std::string>{"OAMSM_CLEAR_ERRORS"});
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS without an unload macro offers no unload",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    json commands = all_commands();
    commands.erase("unload");
    backend.feed(loaded_manager(commands));

    CHECK_FALSE(backend.can_unload_from_toolhead(2));
    CHECK(backend.unload_filament(2).result == AmsResult::NOT_SUPPORTED);
    CHECK(backend.operations.empty());
    REQUIRE(backend.load_filament(1).success());
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS without reset or cancel refuses just those",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    json commands = all_commands();
    commands.erase("reset");
    commands.erase("cancel");
    backend.feed(manager(json::array({lane("loading", "T1", nullptr)}), commands));

    CHECK(backend.reset().result == AmsResult::NOT_SUPPORTED);
    CHECK(backend.cancel().result == AmsResult::NOT_SUPPORTED);
    CHECK(backend.commands.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS ignores a command name that is not one G-code word",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    json commands = all_commands();
    commands["load"] = "OPENAMS_LOAD\nG28";
    backend.feed(manager(json::array({lane("unloaded")}), commands));

    CHECK(backend.load_filament(1).result == AmsResult::NOT_SUPPORTED);
    CHECK(backend.operations.empty());
}

// ============================================================================
// Cancel
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS cancel", "[ams][openams]") {
    OpenAmsHarness backend;

    SECTION("is refused while a load started here holds the G-code queue") {
        backend.feed(manager());
        REQUIRE(backend.load_filament(1).success());
        backend.feed(json{{"lanes", json::array({lane("loading", "T0", nullptr)})}});

        CHECK(backend.cancel().result == AmsResult::BUSY);
        CHECK(backend.commands.empty());
        // Our own record of the load is untouched: it ends when the macro does.
        CHECK(backend.get_current_action() == AmsAction::LOADING);
        CHECK(backend.get_system_info().pending_target_slot == 1);
    }

    SECTION("reaches a load the manager is running on its own") {
        backend.feed(manager(json::array({lane("loading", "T1", nullptr)})));
        REQUIRE(backend.cancel().success());
        CHECK(backend.commands == std::vector<std::string>{"OAMSM_LOAD_FILAMENT_CANCEL"});
    }

    SECTION("with no load running is refused") {
        backend.feed(loaded_manager());
        CHECK(backend.cancel().result == AmsResult::WRONG_STATE);
        CHECK(backend.commands.empty());
    }
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS offers Abort only when cancel can reach the load",
                 "[ams][openams]") {
    OpenAmsHarness backend;
    backend.feed(manager());
    CHECK(backend.can_cancel_operation());

    REQUIRE(backend.load_filament(1).success());
    CHECK_FALSE(backend.can_cancel_operation());

    backend.complete_operation();
    CHECK(backend.can_cancel_operation());

    json no_cancel = all_commands();
    no_cancel.erase("cancel");
    backend.feed(manager(json::array({lane("loading", "T1", nullptr)}), no_cancel));
    CHECK_FALSE(backend.can_cancel_operation());
}

// ============================================================================
// Slot identity persistence
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS repaints a slot from the lane, not the stored record",
                 "[ams][openams][filament_slot_override]") {
    // A resync refreshes the lane but not overrides_, so a repaint that
    // restated a field from overrides_ would disagree with the next frame.
    OpenAmsHarness backend;
    backend.feed(manager());
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "Galaxy Black";
    helix::OpenAmsTestAccess::seed_override(backend, 1, ovr);

    backend.repaint_slot_from_lane(1);

    const auto info = backend.get_slot_info(1);
    CHECK(info.brand.empty());
    CHECK(info.spool_name.empty());
}

TEST_CASE_METHOD(HelixTestFixture, "OpenAMS persists metered weight and clears slot metadata",
                 "[ams][openams][filament_slot_override]") {
    TmpCacheDir tmp("persist");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    OpenAmsHarness backend(&api);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "openams");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    helix::OpenAmsTestAccess::inject_override_store(backend, std::move(store));
    backend.feed(manager());

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.spoolman_id = 42;
    ovr.total_weight_g = 1000.0f;
    ovr.remaining_weight_g = 800.0f;
    helix::OpenAmsTestAccess::seed_override(backend, 1, ovr);

    backend.update_slot_weight(1, 640.0f, 1000.0f, /*persist=*/true);

    auto stored = helix::OpenAmsTestAccess::get_override(backend, 1);
    REQUIRE(stored.has_value());
    CHECK(stored->remaining_weight_g == 640.0f);
    CHECK(stored->brand == "Polymaker");
    const json record = api.mock_get_db_value("lane_data", "lane2");
    REQUIRE(record.is_object());
    CHECK(record.value("remaining_weight_g", -1.0f) == 640.0f);

    backend.clear_slot_override(1);

    CHECK_FALSE(helix::OpenAmsTestAccess::get_override(backend, 1).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane2").is_null());
    const auto info = backend.get_slot_info(1);
    CHECK(info.brand.empty());
    CHECK(info.material.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);
    CHECK(info.remaining_weight_g < 0.0f);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "OpenAMS asks about a spool inserted into an observed empty slot, not a clear",
                 "[ams][openams][filament_slot_override][1710]") {
    // OpenAMS reads nothing off a spool, so an insert has no evidence either
    // way: the record stays and the user is asked. Tapping Clear clears it.
    TmpCacheDir tmp("insert_notice");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<OpenAmsHarness> backend_reg(&api);
    OpenAmsHarness& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "openams");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    helix::OpenAmsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane2",
                          json{{"vendor", "Polymaker"}, {"material", "PLA"}, {"color", "#FF5500"}});
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    helix::OpenAmsTestAccess::seed_override(backend, 1, ovr);
    helix::test::file_override_as_lane_records(backend, 1, ovr);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // The first frame is a baseline, even for a slot holding a spool.
    backend.feed(manager());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    json emptied = manager();
    emptied["units"][0]["slots"][1]["ready"] = false;
    backend.feed(emptied);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty()); // a spool leaving is not an insert

    backend.feed(manager());
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::INFO);
    CHECK(helix::OpenAmsTestAccess::get_override(backend, 1).has_value());

    // Frames that keep the spool present never ask again.
    backend.feed(manager());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.size() == 1);

    REQUIRE(helix::ui::fire_last_toast_action());
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(helix::OpenAmsTestAccess::get_override(backend, 1).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane2").is_null());

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "OpenAMS judges no insert from bays an offline unit or unready manager reports",
                 "[ams][openams][filament_slot_override][1710]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<OpenAmsHarness> backend_reg(&api);
    OpenAmsHarness& backend = *backend_reg;

    // Every bay carries details, so any insert the backend judged would ask.
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    for (int bay = 0; bay < 4; ++bay) {
        helix::test::file_override_as_lane_records(backend, bay, ovr);
    }

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // Baseline: bay 0 empty, bays 1-3 holding a spool.
    backend.feed(manager());
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.empty());

    json unread = manager();
    for (auto& bay : unread["units"][0]["slots"]) {
        bay["ready"] = false;
    }
    SECTION("the unit goes offline") {
        unread["units"][0]["connected"] = false;
    }
    SECTION("the manager is not ready") {
        unread["ready"] = false;
    }
    backend.feed(unread);
    helix::ui::UpdateQueue::instance().drain();

    // Bays 1-3 come back as they were: nothing was inserted.
    backend.feed(manager());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    // Bay 0 was last read empty, so a spool there now is still an insert.
    json inserted = manager();
    inserted["units"][0]["slots"][0]["ready"] = true;
    backend.feed(inserted);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.size() == 1);

    helix::ui::set_test_toast_hook(nullptr);
}
