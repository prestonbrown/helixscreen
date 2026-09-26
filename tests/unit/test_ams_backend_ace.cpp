// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail.h"
#include "ui_test_utils.h"
#include "ui_update_queue.h"

#include "ams_backend_ace.h"
#include "ams_bypass_policy.h"
#include "ams_types.h"
#include "fake_moonraker_client.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_translation.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_types.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/ace_test_access.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/registered_backend.h"

#include <algorithm>
#include <filesystem>
#include <json.hpp> // nlohmann/json from libhv
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using helix::AceTestAccess;
using helix::AmsAction;
using helix::AmsBackend;
using helix::AmsBackendAce;
using helix::AmsSystemInfo;
using helix::AmsType;
using helix::DryerInfo;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;

using json = nlohmann::json;

// Friend-class shim for FilamentSlotOverrideStore. Same idiom as IFS/Snapmaker
// tests — redirects the store's on-disk read-cache to a per-test tmp dir so
// save_async doesn't pollute the developer's real helixscreen config.
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

namespace {
// Per-test tmp cache dir — same idiom as IFS/Snapmaker tests.
struct AceTmpCacheDir {
    std::filesystem::path path;
    explicit AceTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("ace_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~AceTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Build a single-slot ace object payload. `status_str` is one of "empty",
// "available", "loaded", "ready". `color_rgb` is packed as a [r,g,b] array
// (ValgACE's native format). `material_str` goes into "type". `rfid` is the
// bay's tag-reader flag: true means the colour and type came off the spool's
// tag, false means the hub is stating its own memory of the bay.
json make_ace_slot_payload(const std::string& status_str, uint32_t color_rgb,
                           const std::string& material_str, const json& rfid = json(false)) {
    uint8_t r = (color_rgb >> 16) & 0xFF;
    uint8_t g = (color_rgb >> 8) & 0xFF;
    uint8_t b = color_rgb & 0xFF;
    return json{
        {"model", "ACE Pro"},
        {"firmware", "1.2.3"},
        {"status", "ready"},
        {"slots", json::array({json{{"status", status_str},
                                    {"color", json::array({r, g, b})},
                                    {"type", material_str},
                                    {"rfid", rfid}}})},
    };
}

// Build a native Anycubic GoKlipper `filament_hub` get_status() payload (the
// FLAT, single-hub schema Rinkhals firmware exposes): 4 slots, one "ready"
// PLA slot with an RGB color, the rest "empty"; a live "dryer" object in the
// "drying" state; and current_filament "0-1" (local slot 1 loaded).
// --- #1069 live-capture builders (Kobra S1 mainline-Python fork / "ACEPRO"
// driver, 1x ACE Pro, fw V1.3.856). Field-for-field the user's captures: the
// `ace` object is a MANAGER (no slots) whose `current_index` is the ONLY
// loaded-tool signal (-1 = nothing loaded); `ace_instance_0` carries the four
// slots with `material` (no `type`) and the dryer nested under `dryer_status`.

json make_kobra_manager_object(int current_index) {
    return json{
        {"ace_instances", 1},
        {"current_index", current_index},
        {"target_index", -1},
        {"endless_spool_enabled", false},
        {"endless_spool_match_mode", "exact"},
        {"ace_pro_enabled", true},
        {"toolhead_sensor", true},
        {"rdm_sensor", true},
    };
}

json make_kobra_slots_array() {
    return json::array({
        json{{"index", 0},
             {"tool", 0},
             {"status", "ready"},
             {"color", json::array({0, 230, 118})},
             {"material", "PLA"},
             {"temp", 225},
             {"rfid", false}},
        json{{"index", 1},
             {"tool", 1},
             {"status", "ready"},
             {"color", json::array({255, 255, 255})},
             {"material", "PETG"},
             {"temp", 245},
             {"rfid", false}},
        json{{"index", 2},
             {"tool", 2},
             {"status", "ready"},
             {"color", json::array({229, 57, 53})},
             {"material", "PETG"},
             {"temp", 255},
             {"rfid", false}},
        json{{"index", 3},
             {"tool", 3},
             {"status", "ready"},
             {"color", json::array({255, 255, 255})},
             {"material", "PLA"},
             {"temp", 225},
             {"rfid", false}},
    });
}

json make_kobra_instance_object() {
    return json{
        {"status", "ready"},
        {"dryer_status",
         json{{"status", "stop"}, {"target_temp", 0}, {"duration", 0}, {"remain_time", 0}}},
        {"temp", 34},
        {"enable_rfid", 1},
        {"fan_speed", 7000},
        {"feed_assist_count", 0},
        {"cont_assist_time", 0.0},
        {"slots", make_kobra_slots_array()},
        {"instance", 0},
        {"protocol", "ace1_json"},
        {"rfid_sync_enabled", true},
        {"feed_assist_slot", -1},
        {"model", "Anycubic Color Engine Pro"},
        {"firmware", "V1.3.856"},
        {"boot_firmware", "V1.0.1"},
        {"structure_version", "3"},
        {"usb_port", "/dev/ttyACM2"},
        {"usb_path", "1-1.3.4.3"},
        {"connection_state", "connected"},
    };
}

// printer.objects.query answer carrying both the manager and the slot-bearing
// instance, as the fork's initial query does.
json make_kobra_objects_query_response(int current_index) {
    json status = json::object();
    status["ace"] = make_kobra_manager_object(current_index);
    status["ace_instance_0"] = make_kobra_instance_object();
    return json{{"result", {{"eventtime", 33117.349534149}, {"status", status}}}};
}

// GET /server/ace/status result: instance 0's get_status() verbatim plus the
// fork's envelope keys (instance_index, instances[], ace_manager,
// ace_instance_count).
json make_kobra_rest_status_result() {
    json result = make_kobra_instance_object();
    result["instance_index"] = 0;
    result["instances"] = json::array({make_kobra_instance_object()});
    result["ace_manager"] = make_kobra_manager_object(2);
    result["ace_instance_count"] = 1;
    return result;
}

json make_native_filament_hub_payload() {
    return json{
        {"status", "ready"},
        {"temp", 28},
        {"enable_rfid", 1},
        {"fan_speed", 0},
        {"feed_assist_count", 0},
        {"cont_assist_time", 0.0},
        {"dryer",
         json{
             {"status", "drying"},
             {"target_temp", 55},
             {"duration", 240},
             {"remain_time", 180},
         }},
        {"slots", json::array({
                      json{{"index", 0},
                           {"status", "empty"},
                           {"sku", ""},
                           {"type", ""},
                           {"color", json::array({0, 0, 0})}},
                      json{{"index", 1},
                           {"status", "ready"},
                           {"sku", ""},
                           {"type", "PLA"},
                           {"color", json::array({255, 85, 0})}},
                      json{{"index", 2},
                           {"status", "empty"},
                           {"sku", ""},
                           {"type", ""},
                           {"color", json::array({0, 0, 0})}},
                      json{{"index", 3},
                           {"status", "empty"},
                           {"sku", ""},
                           {"type", ""},
                           {"color", json::array({0, 0, 0})}},
                  })},
        {"current_filament", "0-1"},
    };
}

} // namespace

/**
 * @brief Test helper class providing access to AmsBackendAce internals
 *
 * This class provides controlled access to private members for unit testing.
 * It does NOT start the backend (no Moonraker connection needed).
 */
class AmsBackendAceTestHelper : public AmsBackendAce {
  public:
    AmsBackendAceTestHelper() : AmsBackendAce(nullptr, nullptr) {}
    explicit AmsBackendAceTestHelper(helix::IMoonrakerClient* client)
        : AmsBackendAce(nullptr, client) {}

    // Parse response helpers - call the protected parsing methods
    void test_parse_info_response(const json& data) {
        parse_info_response(data);
    }

    bool test_parse_status_response(const json& data) {
        return parse_status_response(data);
    }

    bool test_parse_slots_response(const json& data) {
        return parse_slots_response(data);
    }

    // Drive the WebSocket status-update path (protected hook) so tests can
    // exercise the filament_hub/ace key-picking logic, not just parse_ace_object.
    void test_handle_status_update(const json& notification) {
        handle_status_update(notification);
    }

    // Drive the subscription bootstrap (initial printer.objects.query). The
    // parse lands on the UpdateQueue — drain after calling.
    void test_on_started() {
        on_started();
    }

    // State accessors for verification
    AmsSystemInfo get_test_system_info() const {
        return get_system_info();
    }

    DryerInfo get_test_dryer_info() const {
        return get_dryer_info();
    }

    // Filament ops refuse unless the backend is started; these tests drive the
    // op hooks without a Moonraker connection.
    void set_running(bool state) {
        running_ = state;
    }

    // Load and unload resolve through this seam, so a test can assert what was
    // sent and fire the driver's ack when it chooses. A driver that ignores a
    // toolchange still acks it, which is the case worth reproducing.
    std::vector<std::string> captured_gcodes;
    std::function<void()> pending_ack;

    // Fire-and-forget sends (bypass) take this form; the completion form below
    // is for ops whose ack matters. Both capture, so a test reads one list.
    helix::AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }

    helix::AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete,
                                  std::function<void(const MoonrakerError&)> on_error,
                                  bool silent) override {
        if (dispatch_via_base) {
            return AmsSubscriptionBackend::execute_gcode(gcode, std::move(on_complete),
                                                         std::move(on_error), silent);
        }
        captured_gcodes.push_back(gcode);
        pending_ack = std::move(on_complete);
        return helix::AmsErrorHelper::success();
    }

  public:
    // Routes the completion-form dispatch to the REAL AmsSubscriptionBackend
    // implementation (api_ is null here) instead of the capture seam, so a
    // test can exercise the dispatch's own refusal legs.
    bool dispatch_via_base = false;
};

// ============================================================================
// Type and Topology Tests
// ============================================================================

TEST_CASE("ACE returns correct type", "[ams][ace][type]") {
    AmsBackendAceTestHelper helper;
    REQUIRE(helper.get_type() == AmsType::ACE);
}

TEST_CASE("ACE uses hub topology", "[ams][ace][topology]") {
    // ACE uses hub topology (4 slots merge to single output)
    AmsBackendAceTestHelper helper;
    REQUIRE(helper.get_topology() == PathTopology::HUB);
}

TEST_CASE("ACE without a master switch reports no bypass", "[ams][ace][bypass]") {
    AmsBackendAceTestHelper helper;
    helper.set_running(true);

    // A hub that never publishes ace_pro_enabled has no switch to throw.
    AceTestAccess::parse_ace(helper, make_ace_slot_payload("ready", 0xFF5500, "PLA"));

    REQUIRE_FALSE(helper.get_test_system_info().supports_bypass);
    REQUIRE_FALSE(helper.is_bypass_active());

    auto err = helper.enable_bypass();
    REQUIRE(!err.success());
    REQUIRE(err.result == helix::AmsResult::WRONG_STATE);
}

// ============================================================================
// Bypass via the ACE master switch (prestonbrown/helixscreen#1677)
//
// `ace_pro_enabled` gates the whole ACE path. Rigs that run a fifth spool by
// hand turn it off, which is what bypass means on this hardware.
// ============================================================================

TEST_CASE("ACE bypass follows the driver's master switch", "[ams][ace][bypass][1677]") {
    AmsBackendAceTestHelper helper;
    helper.set_running(true);
    helper.set_bypass_macros(helix::BypassMacros{"ACE_BYPASS_ON", "ACE_BYPASS_OFF"});

    AceTestAccess::parse_ace(helper, make_kobra_instance_object());

    SECTION("switch on: the ACE feeds the toolhead, so bypass is not active") {
        AceTestAccess::parse_ace(helper, make_kobra_manager_object(-1));

        CHECK(helper.get_test_system_info().supports_bypass);
        CHECK_FALSE(helper.is_bypass_active());
    }

    SECTION("switch off: the ACE path is disabled, so bypass is active") {
        json mgr = make_kobra_manager_object(-1);
        mgr["ace_pro_enabled"] = false;
        AceTestAccess::parse_ace(helper, mgr);

        CHECK(helper.get_test_system_info().supports_bypass);
        CHECK(helper.is_bypass_active());
    }

    SECTION("engaging bypass sends the configured macro") {
        AceTestAccess::parse_ace(helper, make_kobra_manager_object(-1));

        REQUIRE(helper.enable_bypass().success());
        REQUIRE(helper.captured_gcodes.size() == 1);
        CHECK(helper.captured_gcodes[0] == "ACE_BYPASS_ON");

        REQUIRE(helper.disable_bypass().success());
        REQUIRE(helper.captured_gcodes.size() == 2);
        CHECK(helper.captured_gcodes[1] == "ACE_BYPASS_OFF");
    }

    SECTION("a seated tool refuses, rather than reporting a success the macro will reject") {
        AceTestAccess::parse_ace(helper, make_kobra_manager_object(2));
        REQUIRE(helper.get_test_system_info().filament_loaded);

        auto err = helper.enable_bypass();
        CHECK(!err.success());
        CHECK(err.result == helix::AmsResult::WRONG_STATE);
        CHECK(helper.captured_gcodes.empty());
    }
}

// ----------------------------------------------------------------------------
// Resolving the macros from discovery. The pure overload, so no SettingsManager
// is involved.
// ----------------------------------------------------------------------------

namespace {
helix::PrinterDiscovery discovery_with(const std::vector<std::string>& objects) {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json(objects));
    return hw;
}
} // namespace

TEST_CASE("Bypass macros resolve from discovery", "[ams][ace][bypass][1677]") {
    SECTION("both conventional macros present: auto-detected") {
        auto hw = discovery_with(
            {"configfile", "ace", "gcode_macro ACE_BYPASS_ON", "gcode_macro ACE_BYPASS_OFF"});

        const auto macros = helix::resolve_bypass_macros(hw, "auto", "auto");
        CHECK(macros.on == "ACE_BYPASS_ON");
        CHECK(macros.off == "ACE_BYPASS_OFF");
    }

    SECTION("neither present: no bypass offered") {
        auto hw = discovery_with({"configfile", "ace"});

        const auto macros = helix::resolve_bypass_macros(hw, "auto", "auto");
        CHECK(macros.on.empty());
        CHECK(macros.off.empty());
    }

    SECTION("only one half present: neither offered, since it cannot round-trip") {
        auto hw = discovery_with({"configfile", "ace", "gcode_macro ACE_BYPASS_ON"});

        const auto macros = helix::resolve_bypass_macros(hw, "auto", "auto");
        CHECK(macros.on.empty());
        CHECK(macros.off.empty());
    }

    SECTION("an explicit override is honoured even when discovery has not seen it") {
        auto hw = discovery_with({"configfile", "ace"});

        const auto macros = helix::resolve_bypass_macros(hw, "MY_BYPASS_ON", "MY_BYPASS_OFF");
        CHECK(macros.on == "MY_BYPASS_ON");
        CHECK(macros.off == "MY_BYPASS_OFF");
    }

    SECTION("an override on one half still needs the other to resolve") {
        auto hw = discovery_with({"configfile", "ace"});

        const auto macros = helix::resolve_bypass_macros(hw, "MY_BYPASS_ON", "auto");
        CHECK(macros.on.empty());
        CHECK(macros.off.empty());
    }
}

TEST_CASE("ACE names no bypass macros, so it offers no bypass", "[ams][ace][bypass][1677]") {
    AmsBackendAceTestHelper helper;
    helper.set_running(true);

    // The switch exists, but nothing safe is configured to throw it.
    AceTestAccess::parse_ace(helper, make_kobra_instance_object());
    AceTestAccess::parse_ace(helper, make_kobra_manager_object(-1));

    CHECK_FALSE(helper.get_test_system_info().supports_bypass);
    CHECK(helper.enable_bypass().result == helix::AmsResult::WRONG_STATE);
}

// ============================================================================
// Dryer Default State Tests
// ============================================================================

TEST_CASE("ACE dryer defaults", "[ams][ace][dryer]") {
    AmsBackendAceTestHelper helper;
    DryerInfo dryer = helper.get_test_dryer_info();

    // ACE always reports dryer as supported
    REQUIRE(dryer.supported == true);
    REQUIRE(dryer.allows_during_print == false); // Safe default: block during print

    // Default state should be inactive
    REQUIRE(dryer.active == false);

    // Should have reasonable temperature limits
    REQUIRE(dryer.min_temp_c >= 30.0f);
    REQUIRE(dryer.min_temp_c <= 40.0f);
    REQUIRE(dryer.max_temp_c >= 50.0f);
    REQUIRE(dryer.max_temp_c <= 80.0f);

    // Should have reasonable duration limit
    REQUIRE(dryer.max_duration_min >= 480);  // At least 8 hours
    REQUIRE(dryer.max_duration_min <= 1440); // At most 24 hours
}

TEST_CASE("ACE dryer progress calculation", "[ams][ace][dryer]") {
    DryerInfo dryer;
    dryer.supported = true;
    dryer.active = true;
    dryer.duration_min = 240;  // 4 hours
    dryer.remaining_min = 120; // 2 hours left

    // Should be 50% complete
    REQUIRE(dryer.get_progress_pct() == 50);

    // When not active, progress should be -1
    dryer.active = false;
    REQUIRE(dryer.get_progress_pct() == -1);
}

TEST_CASE("ACE drying presets available", "[ams][ace][dryer]") {
    AmsBackendAceTestHelper helper;
    auto presets = helper.get_drying_presets();

    // Should have at least 3 presets (PLA, PETG, ABS)
    REQUIRE(presets.size() >= 3);

    // Verify PLA preset exists and has reasonable values
    bool found_pla = false;
    for (const auto& preset : presets) {
        if (preset.name == "PLA") {
            found_pla = true;
            REQUIRE(preset.temp_c >= 40.0f);
            REQUIRE(preset.temp_c <= 50.0f);
            REQUIRE(preset.duration_min >= 180); // At least 3 hours
            break;
        }
    }
    REQUIRE(found_pla);
}

// ============================================================================
// Info Response Parsing Tests
// ============================================================================

TEST_CASE("ACE parse_info_response: valid response", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"model", "ACE Pro"}, {"version", "1.2.3"}, {"slot_count", 4}};

    helper.test_parse_info_response(data);
    auto info = helper.get_test_system_info();

    REQUIRE(info.type_name == "ACE");
    REQUIRE(info.units[0].name == "ACE Pro");
    REQUIRE(info.version == "1.2.3");
    REQUIRE(info.total_slots == 4);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slots.size() == 4);
}

TEST_CASE("ACE parse_info_response: missing fields", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // Empty response should not crash
    json data = json::object();
    helper.test_parse_info_response(data);

    auto info = helper.get_test_system_info();
    // Type name should be ACE
    REQUIRE(info.type == AmsType::ACE);
}

TEST_CASE("ACE parse_info_response: wrong types ignored", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // String where int expected should be ignored, not crash
    json data = {
        {"model", 12345},      // Wrong type (number instead of string)
        {"version", true},     // Wrong type (bool instead of string)
        {"slot_count", "four"} // Wrong type (string instead of int)
    };

    // Should not throw or crash
    REQUIRE_NOTHROW(helper.test_parse_info_response(data));
}

TEST_CASE("ACE parse_info_response: excessive slot count rejected", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {
        {"slot_count", 100} // Unreasonable value
    };

    helper.test_parse_info_response(data);
    auto info = helper.get_test_system_info();

    // Should reject unreasonable slot count
    REQUIRE(info.total_slots != 100);
}

// ============================================================================
// Status Response Parsing Tests
// ============================================================================

TEST_CASE("ACE parse_status_response: loaded slot", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"loaded_slot", 2}, {"action", "idle"}};

    bool changed = helper.test_parse_status_response(data);
    REQUIRE(changed == true);

    auto info = helper.get_test_system_info();
    REQUIRE(info.current_slot == 2);
    REQUIRE(info.current_tool == 2); // 1:1 mapping
    REQUIRE(info.filament_loaded == true);
}

TEST_CASE("ACE parse_status_response: no filament loaded", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"loaded_slot", -1}};

    helper.test_parse_status_response(data);
    auto info = helper.get_test_system_info();

    REQUIRE(info.current_slot == -1);
    REQUIRE(info.filament_loaded == false);
}

TEST_CASE("ACE parse_status_response: action states", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // Test loading action
    json data = {{"action", "loading"}};
    helper.test_parse_status_response(data);
    REQUIRE(helper.get_test_system_info().action == AmsAction::LOADING);

    // Test unloading action
    data = {{"action", "unloading"}};
    helper.test_parse_status_response(data);
    REQUIRE(helper.get_test_system_info().action == AmsAction::UNLOADING);

    // Test error action
    data = {{"action", "error"}};
    helper.test_parse_status_response(data);
    REQUIRE(helper.get_test_system_info().action == AmsAction::ERROR);

    // Test idle action
    data = {{"action", "idle"}};
    helper.test_parse_status_response(data);
    REQUIRE(helper.get_test_system_info().action == AmsAction::IDLE);
}

TEST_CASE("ACE parse_status_response: dryer state", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"dryer",
                  {{"active", true},
                   {"current_temp", 45.5},
                   {"target_temp", 55.0},
                   {"remaining_minutes", 180},
                   {"duration_minutes", 240}}}};

    helper.test_parse_status_response(data);
    auto dryer = helper.get_test_dryer_info();

    REQUIRE(dryer.active == true);
    REQUIRE(dryer.current_temp_c == Catch::Approx(45.5f));
    REQUIRE(dryer.target_temp_c == Catch::Approx(55.0f));
    REQUIRE(dryer.remaining_min == 180);
    REQUIRE(dryer.duration_min == 240);
}

TEST_CASE("ACE parse_status_response: dryer not active", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"dryer", {{"active", false}, {"current_temp", 25.0}, {"target_temp", 0}}}};

    helper.test_parse_status_response(data);
    auto dryer = helper.get_test_dryer_info();

    REQUIRE(dryer.active == false);
    REQUIRE(dryer.target_temp_c == Catch::Approx(0.0f));
}

// ============================================================================
// Slots Response Parsing Tests
// ============================================================================

TEST_CASE("ACE parse_slots_response: valid slots", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // First initialize with info response to set slot count
    json info = {{"slot_count", 4}};
    helper.test_parse_info_response(info);

    // Colors must be strings - ACE API returns hex strings like "#FF0000"
    json data = {
        {"slots",
         {{{"index", 0}, {"color", "#FF0000"}, {"material", "PLA"}, {"status", "available"}},
          {{"index", 1}, {"color", "#00FF00"}, {"material", "PETG"}, {"status", "empty"}},
          {{"index", 2}, {"color", "#0000FF"}, {"material", "ABS"}, {"status", "loaded"}},
          {{"index", 3}, {"color", "#FFFFFF"}, {"material", ""}, {"status", "unknown"}}}}};

    bool changed = helper.test_parse_slots_response(data);
    REQUIRE(changed == true);

    // Verify first slot
    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_rgb == 0xFF0000);
    REQUIRE(slot0.material == "PLA");
    REQUIRE(slot0.status == SlotStatus::AVAILABLE);

    // Verify empty slot
    auto slot1 = helper.get_slot_info(1);
    REQUIRE(slot1.status == SlotStatus::EMPTY);

    // Verify "loaded" status - ACE maps both "available" and "loaded" to AVAILABLE
    // (LOADED enum is for when filament is actively in the extruder path)
    auto slot2 = helper.get_slot_info(2);
    REQUIRE(slot2.status == SlotStatus::AVAILABLE);
}

TEST_CASE("ACE parse_slots_response: missing slots array", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = json::object(); // No "slots" key

    bool changed = helper.test_parse_slots_response(data);
    REQUIRE(changed == false);
}

TEST_CASE("ACE parse_slots_response: excessive slots rejected", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // Create an array with too many slots
    json slots_array = json::array();
    for (int i = 0; i < 20; ++i) {
        slots_array.push_back({{"index", i}});
    }

    json data = {{"slots", slots_array}};

    bool changed = helper.test_parse_slots_response(data);
    REQUIRE(changed == false); // Should reject excessive count
}

// ============================================================================
// Filament Segment Tests
// ============================================================================

TEST_CASE("ACE filament segment when nothing loaded", "[ams][ace][segment]") {
    AmsBackendAceTestHelper helper;

    // Initialize with slots
    json info = {{"slot_count", 4}};
    helper.test_parse_info_response(info);

    json status = {{"loaded_slot", -1}};
    helper.test_parse_status_response(status);

    REQUIRE(helper.get_filament_segment() == PathSegment::NONE);
}

TEST_CASE("ACE filament segment when loaded", "[ams][ace][segment]") {
    AmsBackendAceTestHelper helper;

    // Initialize with slots
    json info = {{"slot_count", 4}};
    helper.test_parse_info_response(info);

    // Set slot 1 as loaded
    json status = {{"loaded_slot", 1}};
    helper.test_parse_status_response(status);

    // Mark slot 1 as available
    json slots = {{"slots",
                   {{{"index", 0}, {"status", "empty"}},
                    {{"index", 1}, {"status", "loaded"}},
                    {{"index", 2}, {"status", "empty"}},
                    {{"index", 3}, {"status", "empty"}}}}};
    helper.test_parse_slots_response(slots);

    // Overall segment should show filament at nozzle
    REQUIRE(helper.get_filament_segment() == PathSegment::NOZZLE);
}

// ============================================================================
// The filament path from the driver's own sensors
// (prestonbrown/helixscreen#1678)
//
// An unloaded strand on this driver is not at the spool: it is retracted to a
// parking position just short of the hub. The manager publishes the two path
// sensors, so the path can show where the filament actually is instead of
// jumping from Spool to Nozzle.
// ============================================================================

TEST_CASE("ACE filament path follows the driver's sensors", "[ams][ace][segment][1678]") {
    AmsBackendAceTestHelper helper;
    helper.set_running(true);
    AceTestAccess::parse_ace(helper, make_kobra_instance_object());

    SECTION("nothing seated, neither sensor made: nothing on the path") {
        json mgr = make_kobra_manager_object(-1);
        mgr["rdm_sensor"] = false;
        mgr["toolhead_sensor"] = false;
        AceTestAccess::parse_ace(helper, mgr);

        CHECK(helper.get_filament_segment() == PathSegment::NONE);
    }

    SECTION("hub sensor only: the strand is between hub and toolhead") {
        json mgr = make_kobra_manager_object(-1);
        mgr["rdm_sensor"] = true;
        mgr["toolhead_sensor"] = false;
        mgr["target_index"] = 2;
        AceTestAccess::parse_ace(helper, mgr);

        CHECK(helper.get_filament_segment() == PathSegment::OUTPUT);
    }

    SECTION("toolhead sensor made but nothing seated yet: it has reached the toolhead") {
        json mgr = make_kobra_manager_object(-1);
        mgr["rdm_sensor"] = true;
        mgr["toolhead_sensor"] = true;
        mgr["target_index"] = 2;
        AceTestAccess::parse_ace(helper, mgr);

        CHECK(helper.get_filament_segment() == PathSegment::TOOLHEAD);
    }

    SECTION("a seated tool outranks the sensors") {
        json mgr = make_kobra_manager_object(2);
        mgr["rdm_sensor"] = true;
        mgr["toolhead_sensor"] = true;
        AceTestAccess::parse_ace(helper, mgr);

        CHECK(helper.get_filament_segment() == PathSegment::NOZZLE);
    }

    SECTION("the sensors the driver publishes reach the unit, for the path to draw") {
        json mgr = make_kobra_manager_object(-1);
        mgr["rdm_sensor"] = true;
        mgr["toolhead_sensor"] = false;
        AceTestAccess::parse_ace(helper, mgr);

        const auto info = helper.get_test_system_info();
        REQUIRE_FALSE(info.units.empty());
        CHECK(info.units[0].has_hub_sensor);
        CHECK(info.units[0].hub_sensor_triggered);
        CHECK(info.units[0].has_toolhead_sensor);
    }
}

TEST_CASE("ACE error segment inference", "[ams][ace][segment]") {
    AmsBackendAceTestHelper helper;

    // Set error state
    json status = {{"action", "error"}};
    helper.test_parse_status_response(status);

    // Should infer error at hub
    REQUIRE(helper.infer_error_segment() == PathSegment::HUB);
}

// ============================================================================
// Invalid Slot Handling Tests
// ============================================================================

TEST_CASE("ACE returns invalid markers for out-of-bounds slot", "[ams][ace][slot]") {
    AmsBackendAceTestHelper helper;

    // Before any initialization, getting any slot should return invalid markers
    auto slot = helper.get_slot_info(0);
    REQUIRE(slot.slot_index == -1);
    REQUIRE(slot.global_index == -1);

    // Invalid negative index should also return invalid markers
    slot = helper.get_slot_info(-1);
    REQUIRE(slot.slot_index == -1);
    REQUIRE(slot.global_index == -1);

    // Initialize with slots
    json info = {{"slot_count", 4}};
    helper.test_parse_info_response(info);

    // Valid slot should work
    auto valid_slot = helper.get_slot_info(0);
    REQUIRE(valid_slot.slot_index == 0);
    REQUIRE(valid_slot.global_index == 0);

    // Out-of-bounds should return invalid markers
    auto out_of_bounds = helper.get_slot_info(10);
    REQUIRE(out_of_bounds.slot_index == -1);
    REQUIRE(out_of_bounds.global_index == -1);

    // Negative index should return invalid markers
    auto negative = helper.get_slot_info(-5);
    REQUIRE(negative.slot_index == -1);
    REQUIRE(negative.global_index == -1);
}

// ============================================================================
// Not Running State Tests
// ============================================================================

TEST_CASE("ACE not running initially", "[ams][ace][state]") {
    AmsBackendAceTestHelper helper;
    REQUIRE(helper.is_running() == false);
}

TEST_CASE("ACE operations require API", "[ams][ace][preconditions]") {
    AmsBackendAceTestHelper helper;

    // Without API, operations should fail
    auto err = helper.load_filament(0);
    REQUIRE(!err.success());

    err = helper.unload_active_filament();
    REQUIRE(!err.success());

    err = helper.start_drying(45.0f, 240);
    REQUIRE(!err.success());
}

// A dispatch that never goes out still has to unwind the optimistic action the
// op set before it, or is_busy() refuses every later op (prestonbrown/helixscreen#1720).
TEST_CASE("ACE unload refused at the send unwinds UNLOADING", "[ams][ace][1720]") {
    AmsBackendAceTestHelper helper;
    helper.set_running(true);
    helper.dispatch_via_base = true;

    auto err = helper.unload_filament(0);
    REQUIRE_FALSE(err.success());

    // The refusal came from the dispatch itself, not the capture seam.
    REQUIRE(helper.captured_gcodes.empty());

    helix::ui::UpdateQueue::instance().drain();

    CHECK(helper.get_test_system_info().action == AmsAction::IDLE);
}

// ============================================================================
// The G-code ack is not proof of a load (prestonbrown/helixscreen#1676)
//
// The ACEPRO driver answers a toolchange it will not perform with a plain
// respond_info and no error, so the ack arrives for a load that never moved.
// The manager's current_index is the seat signal, and an ignored load leaves it
// unchanged at -1 — Klipper notifies on field changes, so no frame follows and
// nothing arrives to contradict a stamp taken from the ack.
// ============================================================================

TEST_CASE("ACE load does not seat a slot the driver never moved to", "[ams][ace][1676]") {
    AmsBackendAceTestHelper helper;
    helper.set_running(true);

    // Fork rig with nothing loaded: four ready slots, manager seat -1.
    AceTestAccess::parse_ace(helper, make_kobra_instance_object());
    AceTestAccess::parse_ace(helper, make_kobra_manager_object(-1));
    REQUIRE_FALSE(helper.get_test_system_info().filament_loaded);

    REQUIRE(helper.load_filament(2).success());

    // The absence checked below is only meaningful if the load actually
    // dispatched and an ack is really waiting.
    REQUIRE(helper.captured_gcodes.size() == 1);
    REQUIRE(helper.captured_gcodes[0] == "ACE_CHANGE_TOOL TOOL=2");
    REQUIRE(helper.pending_ack != nullptr);

    helper.pending_ack();
    helix::ui::UpdateQueue::instance().drain();

    const auto info = helper.get_test_system_info();
    CHECK_FALSE(info.filament_loaded);
    CHECK(info.current_slot == -1);
    CHECK(helper.get_slot_info(2).status != SlotStatus::LOADED);
}

// ============================================================================
// Task 13: FilamentSlotOverrideStore integration.
//
// ACE's legacy per-backend override plumbing (slot_overrides_ map,
// save/load_slot_overrides*, apply_slot_overrides_json, slot_overrides_to_json)
// has been replaced by the shared FilamentSlotOverrideStore. The tests below
// lock the behavior commitments the migration preserves:
//   1. An override loaded at init is applied over firmware data on parse.
//   2. Migration from helix-screen:ace_slot_overrides to lane_data happens
//      automatically on the first load_blocking() call (Task 8 logic).
//   3. apply_user_edit() writes through to the store.
//   4. Slot status transition empty/unknown -> present clears the override
//      (ACE's analogue to RFID UID change on Snapmaker).
//   5. Slot status transition loaded -> empty does NOT clear the override.
// ============================================================================

TEST_CASE("ACE override loaded at init is applied over firmware data",
          "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("task13_override_applied");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    // Seed an override (brand="Polymaker", color=FF5500, material=PLA). ACE is
    // override-wins-for-everything — color and material must come from the
    // override even though firmware reports different values below.
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;
    ovr.material = "PLA";
    AceTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports slot 0 with DIFFERENT color (green) and material (ABS).
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x00FF00, "ABS"));

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite Orange");
    CHECK(info.spoolman_id == 42);
    // Override wins for color and material on ACE.
    CHECK(info.color_rgb == 0xFF5500u);
    CHECK(info.material == "PLA");
}

TEST_CASE("ACE migrates from helix-screen:ace_slot_overrides on first startup",
          "[ams][ace][filament_slot_override][migration]") {
    // Pre-Task-8 ACE wrote per-slot overrides to
    // helix-screen:ace_slot_overrides. On first startup post-upgrade, the
    // store's load_blocking() migrates that data into lane_data and deletes
    // the legacy namespace. Tests through the store + MoonrakerAPIMock
    // directly so we don't need to drive on_started() (which requires a
    // started subscription backend).
    AceTmpCacheDir tmp("task13_migration");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed legacy namespace with a PLA Orange override on slot 0.
    // lane_data is untouched -> forces migration.
    json legacy = {
        {"0",
         {
             {"brand", "Polymaker"},
             {"material", "PLA"},
             {"color_rgb", 0xFF5500},
             {"spoolman_id", 42},
             {"spool_name", "PolyLite Orange"},
         }},
    };
    api.mock_set_db_value("helix-screen", "ace_slot_overrides", legacy);

    helix::ams::FilamentSlotOverrideStore store(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    auto loaded = store.load_blocking();

    // Migrated slot is returned from load_blocking as if it came from lane_data.
    REQUIRE(loaded.count(0) == 1);
    CHECK(loaded[0].brand == "Polymaker");
    CHECK(loaded[0].material == "PLA");
    CHECK(loaded[0].color_rgb == 0xFF5500u);
    CHECK(loaded[0].spoolman_id == 42);
    CHECK(loaded[0].spool_name == "PolyLite Orange");

    // lane_data now holds the AFC-shaped record (1-based key on disk, 0-based
    // "lane" field inside — see to_lane_data_record's invariant).
    auto lane1 = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!lane1.is_null());
    CHECK(lane1["vendor"] == "Polymaker");
    CHECK(lane1["lane"] == "0");

    // Legacy namespace deleted post-migration — second startup sees lane_data
    // populated and skips the migration codepath entirely.
    CHECK(api.mock_get_db_value("helix-screen", "ace_slot_overrides").is_null());
}

TEST_CASE("ACE apply_user_edit writes to store", "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("task13_persist_true");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    // Prime the backend with 4 slots so apply_user_edit's index check passes.
    AceTestAccess::parse_ace(backend, json{{"model", "ACE Pro"},
                                           {"slots", json::array({
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                     })}});

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.spoolman_filament_id = 55;
    edit.remaining_weight_g = 850.0f;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = helix::test::apply_edit(backend, 0, edit);
    REQUIRE(err.success());
    REQUIRE(backend.get_slot_info(0).spoolman_filament_id == 55);

    // In-memory map carries the override.
    auto staged = AceTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0xFF5500u);

    // Moonraker DB received the AFC-shaped record via save_async (dispatched
    // synchronously in-call by MoonrakerAPIMock).
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["helix_spoolman_filament_id"] == 55);
    CHECK(stored["material"] == "PLA");
    CHECK(stored["color"] == "#FF5500");

    // Legacy namespace NOT touched — ACE no longer writes there.
    CHECK(api.mock_get_db_value("helix-screen", "ace_slot_overrides").is_null());
}

TEST_CASE("ACE sync_external_identity does NOT write to store",
          "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("task13_persist_false");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    AceTestAccess::parse_ace(backend, json{{"model", "ACE Pro"},
                                           {"slots", json::array({
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                     })}});

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.sync_external_identity(0, edit);
    REQUIRE(err.success());

    // No override staged, no DB write.
    CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    // Preview edit still visible via get_slot_info (in-memory only).
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Draft");
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x123456u);
}

TEST_CASE_METHOD(HelixTestFixture, "ACE weight persist leaves the lane's declarations standing",
                 "[ams][ace][filament_slot_override]") {
    // persist=true means "write this down", not "a person typed this": the
    // consumption meter reaches this path at pause and at print completion
    // with no user edit behind it, and its diff moves the weight alone. A
    // record that took its authorship from that diff would drop every choice
    // the lane already carried.
    AceTmpCacheDir tmp("task18_weight_persist");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    AceTestAccess::parse_ace(backend, json{{"model", "ACE Pro"},
                                           {"slots", json::array({
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                     })}});

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PETG";
    edit.color_rgb = 0x1E5AA8;
    REQUIRE(helix::test::apply_edit(backend, 0, edit).success());

    const auto declared = AceTestAccess::get_override(backend, 0);
    REQUIRE(declared.has_value());
    REQUIRE(helix::ams::declared_field_names(declared->declared) ==
            json::array({"color_rgb", "material", "brand"}));

    // What the meter does, through the one entry point weight reaches a lane by.
    backend.update_slot_weight(0, 730.0f, 1000.0f, /*persist=*/true);

    const auto after = AceTestAccess::get_override(backend, 0);
    REQUIRE(after.has_value());
    CHECK(after->remaining_weight_g == Catch::Approx(730.0f));
    CHECK(after->total_weight_g == Catch::Approx(1000.0f));
    CHECK(helix::ams::declared_field_names(after->declared) ==
          json::array({"color_rgb", "material", "brand"}));
    CHECK(after->brand == "Polymaker");
    CHECK(after->material == "PETG");
    CHECK(after->color_rgb == 0x1E5AA8u);

    // And in the record a restart reads back, where the declared set and the
    // lock keys written from it are what tell a stored declaration from a
    // stored memory.
    const auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["helix_locked_color"] == true);
    CHECK(stored["helix_locked_material"] == true);
    CHECK(stored["helix_declared"] == json::array({"color_rgb", "material", "brand"}));
    CHECK(stored["remaining_weight_g"] == 730.0f);
}

TEST_CASE("ACE inserting a different tagged spool clears the override",
          "[ams][ace][filament_slot_override][1710]") {
    AceTmpCacheDir tmp("task13_empty_to_present_clears");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    // Seed override AND lane_data entry so we can verify clear_async really
    // deletes it from the mock Moonraker DB.
    api.mock_set_db_value(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    AceTestAccess::seed_override(backend, 0, ovr);

    // First parse: slot AVAILABLE with a TAGGED spool in it (rfid=true). First
    // observation is a baseline and never fires; it records the occupant's tag
    // reading as the comparison side for the next insert.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0xFF5500, "PLA", true));
    REQUIRE(AceTestAccess::get_override(backend, 0).has_value());
    REQUIRE(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: EMPTY, the user pulled the spool. Not a swap signal.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));
    REQUIRE(AceTestAccess::get_override(backend, 0).has_value());

    // Third parse: AVAILABLE with a DIFFERENT tagged spool. EMPTY -> present is
    // the insert edge, both sides were read off tags, material and colour
    // differ: the insert rule says DifferentSpool, override MUST be cleared
    // (in-memory and in MR DB).
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG", true));

    CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    // Firmware data for the new spool is visible; override-exclusive fields
    // were reset.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand.empty());
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.color_rgb == 0x0055FFu); // new firmware color flows through
    CHECK(info.material == "PETG");     // new firmware material
}

TEST_CASE("ACE reloading the same tagged spool keeps the override",
          "[ams][ace][filament_slot_override][1710]") {
    // User unloaded the current spool and put the same one back. The tag read
    // the same material and colour on both sides of the empty interval, so the
    // insert rule says SameSpool and the override stands. Only a spool whose
    // tag reads differently is a swap.
    AceTmpCacheDir tmp("task13_loaded_to_empty_preserves");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    AceTestAccess::seed_override(backend, 0, ovr);

    // First parse: slot LOADED with a tagged spool. Baseline, never fires.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("loaded", 0xFF5500, "PLA", true));
    REQUIRE(AceTestAccess::get_override(backend, 0).has_value());

    // Second parse: LOADED -> EMPTY (user unloaded). Not an insert; no verdict.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));
    CHECK(AceTestAccess::get_override(backend, 0).has_value());

    // Third parse: EMPTY -> LOADED with the SAME tag reading. SameSpool: the
    // override the user set on that spool survives the unload/reload.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("loaded", 0xFF5500, "PLA", true));
    CHECK(AceTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ACE insert of an untagged spool offers the same-spool notice, not a clear",
                 "[ams][ace][filament_slot_override][1710]") {
    // The hub states colour and type from its own memory when the bay has no
    // tag read (rfid=false): the insert rule has nothing to compare, so the
    // record stays and the user is asked. Tapping Clear is what clears it.
    AceTmpCacheDir tmp("task1710_untagged_insert_notice");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.spoolman_id = 42;
    AceTestAccess::seed_override(backend, 0, ovr);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // Tagged occupant leaves, an UNTAGGED spool arrives: the hub's PETG
    // statement is memory, not a read. The insert frame carries no read, so
    // the verdict holds for the read that never comes.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0xFF5500, "PLA", true));
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG"));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty()); // holding the verdict, not asking yet

    // Frames keep coming with no read (the REST poll restates the bay every
    // pass) until the held insert expires to the no-evidence notice.
    for (int i = 0; i < 8; ++i) {
        AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG"));
    }
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].first == ToastSeverity::INFO);
    CHECK(AceTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // The expired insert is gone: later no-read frames never ask again.
    for (int i = 0; i < 4; ++i) {
        AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG"));
    }
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.size() == 1);

    // Tapping Clear is the user answering the question.
    REQUIRE(helix::ui::fire_last_toast_action());
    CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ACE a tag read landing after the insert frame is judged when it lands",
                 "[ams][ace][filament_slot_override][1710]") {
    // The frame that reports the spool can precede the one carrying its tag
    // read. The insert holds its verdict until the read lands, the occupant
    // that left stays the comparison side while it waits (a no-read frame
    // states hub memory, not the reader), and the late read is judged: a
    // different tag reading clears, and a judged swap never asks.
    AceTmpCacheDir tmp("task1710_late_tag_swap");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.spoolman_id = 42;
    AceTestAccess::seed_override(backend, 0, ovr);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // Tagged PLA occupant read, then the bay empties.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0xFF5500, "PLA", true));
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));

    // The PETG spool arrives before its tag read lands.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG"));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());
    CHECK(AceTestAccess::get_override(backend, 0).has_value());

    // The read lands and names a different spool than the one that left.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG", true));

    CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE("ACE an rfid state of 2 (identified) reads as a tag read",
          "[ams][ace][filament_slot_override][1710]") {
    // ACEResearch PROTOCOL.md spells the bay's tag-reader state as an
    // integer: 0 information not found, 1 failed to identify, 2 identified,
    // 3 identifying. ValgACE's bridge and the multiACE lineage send this
    // form. Two is a read, so the material and colour it accompanies are tag
    // evidence and a swap is a judged DifferentSpool, not a verdict held
    // forever.
    AceTmpCacheDir tmp("task1710_numeric_rfid");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    AceTestAccess::seed_override(backend, 0, ovr);

    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0xFF5500, "PLA", true));
    REQUIRE(AceTestAccess::get_override(backend, 0).has_value());
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));

    // Numeric rfid 2 (identified) on the insert frame: judged immediately.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG", 2));

    CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());
}

namespace {
/// The shared body of the rfid-state cases: an insert whose first frame
/// says 3 (identifying) holds its verdict, and the follow-up frame decides
/// it. @p closing_clears pairs with @p closing_state: 2 identified with a
/// different tag clears the override; 1 (the reader finished without a
/// tag) asks, whatever values the frame carries. @p edge_state is the
/// insert frame's own state, 3 identifying or 0 not yet started.
void assert_rfid_state_sequence(std::int64_t closing_state, bool closing_clears,
                                std::int64_t edge_state = 3) {
    AceTmpCacheDir tmp("task1710_rfid_states");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    AceTestAccess::seed_override(backend, 0, ovr);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    // Baseline: the bay holds the spool the override describes, and the
    // reader identified it, so the remembered evidence is a real reading.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0xFF5500, "PLA", 2));
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(toasts.empty());

    // Insert frame: rfid 3, identifying, while the hub still reports the
    // OLD spool's material and colour. Judging on that stale memory would
    // read SameSpool; the read has not landed, so the verdict must hold.
    AceTestAccess::parse_ace(backend,
                             make_ace_slot_payload("available", 0xFF5500, "PLA", edge_state));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(toasts.empty());
    CHECK(AceTestAccess::get_override(backend, 0).has_value());

    // The deciding frame. A 2 carries the new spool's own reading; a 1
    // carries only hub memory, and the values on it are deliberately the
    // stale ones, so the state - not the values - decides.
    if (closing_clears) {
        AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG", 2));
    } else {
        AceTestAccess::parse_ace(
            backend, make_ace_slot_payload("available", 0xFF5500, "PLA", closing_state));
    }
    helix::ui::UpdateQueue::instance().drain();

    if (closing_clears) {
        CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
        CHECK(toasts.empty());
    } else {
        REQUIRE(toasts.size() == 1);
        CHECK(toasts[0].first == ToastSeverity::INFO);
        CHECK(AceTestAccess::get_override(backend, 0).has_value());
    }

    helix::ui::set_test_toast_hook(nullptr);
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ACE rfid 3 holds the insert, then 2 with a different tag clears",
                 "[ams][ace][filament_slot_override][1710]") {
    assert_rfid_state_sequence(2, /*closing_clears=*/true);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ACE rfid 3 holds the insert, then 1 asks instead",
                 "[ams][ace][filament_slot_override][1710]") {
    assert_rfid_state_sequence(1, /*closing_clears=*/false);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ACE rfid 0 on the insert frame holds the verdict too",
                 "[ams][ace][filament_slot_override][1710]") {
    // 0 is the reader not yet started. Read as a finished no-tag read it
    // would judge a tagged spool's reinsert DifferentSpool on the spot.
    assert_rfid_state_sequence(2, /*closing_clears=*/true, /*edge_state=*/0);
    assert_rfid_state_sequence(1, /*closing_clears=*/false, /*edge_state=*/0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ACE an insert with no prior reading offers the notice, never clears",
                 "[ams][ace][filament_slot_override][1710]") {
    // The bay was empty since boot, so no reading of the spool that was in it
    // before the override was set can exist: the rule reads NoEvidence on
    // principle and asks instead of clearing.
    AceTmpCacheDir tmp("task1710_no_prior_reading");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    AceTestAccess::seed_override(backend, 0, ovr);

    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    helix::ui::set_test_toast_hook([&](ToastSeverity severity, const std::string& msg, uint32_t) {
        toasts.emplace_back(severity, msg);
    });

    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG", true));
    helix::ui::UpdateQueue::instance().drain();

    CHECK(toasts.size() == 1);
    CHECK(AceTestAccess::get_override(backend, 0).has_value());

    helix::ui::set_test_toast_hook(nullptr);
}

TEST_CASE("ACE partial override only replaces specified fields",
          "[ams][ace][filament_slot_override]") {
    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(nullptr, nullptr);
    AmsBackendAce& backend = *backend_reg;

    // Override with only `brand` set — every other field must fall through
    // to firmware data (or SlotInfo default). Seed override AFTER an initial
    // baseline parse (first observation is the baseline and would otherwise
    // trigger the EMPTY-not-yet-seen path cleanly, but a saved override
    // should already be in place before the parse that establishes the
    // baseline. In production the load_blocking() call precedes any parse
    // entirely; here the seed-then-parse ordering matches that contract).
    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    AceTestAccess::seed_override(backend, 0, ovr);

    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0xFF5500, "PLA"));

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");        // override wins
    CHECK(info.color_rgb == 0xFF5500u);      // firmware untouched
    CHECK(info.material == "PLA");           // firmware untouched
    CHECK(info.spool_name.empty());          // default
    CHECK(info.spoolman_id == 0);            // default
    CHECK(info.remaining_weight_g == -1.0f); // default
}

// ============================================================================
// Task 16: explicit clear_slot_override
// ============================================================================

TEST_CASE("ACE clear_slot_override erases in-memory override and MR DB entry",
          "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("task16_clear_slot_override");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    // Seed both halves of the override so the clear has something to remove
    // at each layer (in-memory + Moonraker lane_data).
    api.mock_set_db_value(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    ovr.total_weight_g = 1000.0f;
    ovr.remaining_weight_g = 800.0f;
    AceTestAccess::seed_override(backend, 0, ovr);

    // Prime a parse so system_info_ has slots populated (otherwise
    // clear_slot_override can't find the live SlotInfo to reset).
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("loaded", 0xFF5500, "PLA"));

    {
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spoolman_id == 42);
    }
    REQUIRE(AceTestAccess::get_override(backend, 0).has_value());
    REQUIRE(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // User presses "Clear slot metadata". Override must disappear everywhere.
    backend.clear_slot_override(0);

    CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    auto info = backend.get_slot_info(0);
    CHECK(info.brand.empty());
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.spoolman_vendor_id == 0);
    CHECK(info.remaining_weight_g < 0.0f);
    CHECK(info.total_weight_g < 0.0f);
    CHECK(info.color_name.empty());
    // Firmware-sourced color/material flow through — clear only wipes
    // override-exclusive fields for ACE.
    CHECK(info.color_rgb == 0xFF5500u);
    CHECK(info.material == "PLA");
}

TEST_CASE("ACE clear_slot_override is a no-op when no override is present",
          "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("task16_clear_slot_override_noop");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    AceTestAccess::parse_ace(backend, make_ace_slot_payload("loaded", 0xAA55FF, "PETG"));

    // No override staged. Should not crash, should not touch firmware state.
    backend.clear_slot_override(0);

    CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == 0xAA55FFu);
    CHECK(info.material == "PETG");
}

TEST_CASE("ACE clear_slot_override drops the whole Spoolman link",
          "[ams][ace][filament_slot_override][1625]") {
    AceTmpCacheDir tmp("clear_spoolman_link");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;

    AceTestAccess::parse_ace(backend, make_ace_slot_payload("loaded", 0xFF5500, "PLA"));
    REQUIRE(AceTestAccess::seed_live_spoolman_link(backend, 0, 42, 77, 3));
    REQUIRE(backend.get_slot_info(0).spoolman_filament_id == 77);

    backend.clear_slot_override(0);

    // All three handles die together: a slot cleared of its override must not
    // keep naming a Spoolman record, and a surviving filament handle would.
    auto info = backend.get_slot_info(0);
    CHECK(info.spoolman_id == 0);
    CHECK(info.spoolman_vendor_id == 0);
    CHECK(info.spoolman_filament_id == 0);
    CHECK(info.spool_name.empty());
}

// ============================================================================
// Native Anycubic GoKlipper (filament_hub) schema parsing.
//
// Rinkhals firmware ships native Anycubic GoKlipper, which registers the
// status object as `filament_hub` (singular) and exposes a FLAT, single-hub
// get_status() schema with live dryer state and a "current_filament"
// "<unitId>-<localIndex>" string. These tests lock the parse behavior for that
// schema.
// ============================================================================

TEST_CASE("ACE parses native filament_hub schema (slots, dryer, current_filament)",
          "[ams][ace][native][parse]") {
    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(nullptr, nullptr);
    AmsBackendAce& backend = *backend_reg;

    AceTestAccess::parse_ace(backend, make_native_filament_hub_payload());

    auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 4);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slots.size() == 4);

    // Slot 0/2/3 are empty; slot 1 is "ready" PLA with color #FF5500.
    CHECK(backend.get_slot_info(0).status == SlotStatus::EMPTY);
    CHECK(backend.get_slot_info(2).status == SlotStatus::EMPTY);
    CHECK(backend.get_slot_info(3).status == SlotStatus::EMPTY);

    auto slot1 = backend.get_slot_info(1);
    // Native "ready" maps to AVAILABLE via the vocabulary map, then the seat
    // derived from current_filament "0-1" promotes this one slot to LOADED
    // (#1199). A "ready" slot the seat does NOT name stays AVAILABLE — see
    // test_ams_ace_per_slot_loaded.cpp.
    CHECK(slot1.status == SlotStatus::LOADED);
    CHECK(slot1.material == "PLA");
    CHECK(slot1.color_rgb == 0xFF5500u); // [255,85,0] -> 0xFF5500

    // Dryer reflects the native "drying" state.
    auto dryer = backend.get_dryer_info();
    CHECK(dryer.active == true);
    CHECK(dryer.target_temp_c == Catch::Approx(55.0f));
    CHECK(dryer.duration_min == 240);
    CHECK(dryer.remaining_min == 180); // native remain_time -> remaining_min

    // current_filament "0-1" => local slot 1 loaded.
    CHECK(info.current_slot == 1);
    CHECK(info.current_tool == 1);
    CHECK(info.filament_loaded == true);
}

TEST_CASE("ACE status-update path prefers filament_hub over ace key", "[ams][ace][native][parse]") {
    AmsBackendAceTestHelper helper;

    // Notification envelope: {"params": [ {status...}, timestamp ]}. The status
    // object carries BOTH keys; filament_hub must win.
    json filament_hub = make_native_filament_hub_payload();
    json ace_obj = make_ace_slot_payload("empty", 0x000000, ""); // would yield 1 slot

    json status = json::object();
    status["filament_hub"] = filament_hub;
    status["ace"] = ace_obj;

    json notification = {{"params", json::array({status, 12345.0})}};

    helper.test_handle_status_update(notification);

    auto info = helper.get_system_info();
    // 4 slots from filament_hub, not 1 from the ace fallback.
    CHECK(info.total_slots == 4);
    CHECK(info.current_slot == 1);
    CHECK(helper.get_slot_info(1).material == "PLA");
}

TEST_CASE("ACE status-update path falls back to ace key when filament_hub absent",
          "[ams][ace][native][parse]") {
    AmsBackendAceTestHelper helper;

    json ace_obj = json{
        {"model", "ACE Pro"},
        {"status", "ready"},
        {"slots",
         json::array({
             json{{"status", "available"}, {"type", "PETG"}, {"color", json::array({0, 85, 255})}},
             json{{"status", "empty"}},
         })},
    };

    json status = json::object();
    status["ace"] = ace_obj;

    json notification = {{"params", json::array({status, 999.0})}};

    helper.test_handle_status_update(notification);

    auto info = helper.get_system_info();
    CHECK(info.total_slots == 2);
    CHECK(helper.get_slot_info(0).material == "PETG");
    CHECK(helper.get_slot_info(0).color_rgb == 0x0055FFu);
}

TEST_CASE("ACE native current_filament empty leaves loaded state unmanaged",
          "[ams][ace][native][parse]") {
    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(nullptr, nullptr);
    AmsBackendAce& backend = *backend_reg;

    // First load slot 2 via a payload that says current_filament "0-2".
    json p = make_native_filament_hub_payload();
    p["current_filament"] = "0-2";
    AceTestAccess::parse_ace(backend, p);
    CHECK(backend.get_system_info().current_slot == 2);

    // Now an empty current_filament must NOT force current_slot to -1 — the
    // backend's load/unload bookkeeping owns that transition.
    p["current_filament"] = "";
    AceTestAccess::parse_ace(backend, p);
    CHECK(backend.get_system_info().current_slot == 2);
}

TEST_CASE("ACE maps native runout slot status to EMPTY", "[ams][ace][native][parse]") {
    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(nullptr, nullptr);
    AmsBackendAce& backend = *backend_reg;

    json p = make_native_filament_hub_payload();
    p["slots"][1]["status"] = "runout";

    SECTION("an unseated slot reads EMPTY straight from the vocabulary map") {
        // Clear the seat so nothing layers on top of the parsed status.
        p["current_filament"] = "";
        AceTestAccess::parse_ace(backend, p);

        // runout = ran dry mid-print; mapped to EMPTY (no dedicated RUNOUT status).
        CHECK(backend.get_slot_info(1).status == SlotStatus::EMPTY);
    }

    SECTION("the seated slot stays LOADED through a runout (#1199)") {
        // current_filament still names slot 1: the spool ran dry but filament
        // is still threaded to the toolhead, and the user has to be able to
        // unload it. Refusing to stamp here would blank the active-lane
        // highlight and disable Unload in exactly the case that needs it.
        AceTestAccess::parse_ace(backend, p);

        CHECK(backend.get_slot_info(1).status == SlotStatus::LOADED);
        CHECK(backend.can_unload_from_toolhead(1));

        // ...and the EMPTY the parse wrote comes back once nothing is seated.
        p["current_filament"] = "";
        p["loaded_slot"] = -1;
        AceTestAccess::parse_ace(backend, p);
        CHECK(backend.get_slot_info(1).status == SlotStatus::EMPTY);
    }
}

// ============================================================================
// #1069: Kobra S1 mainline-Python Klipper fork REST path.
//
// The fork ships a custom ace_status.py Moonraker component that exposes ACE
// state through /server/ace/status + /server/ace/slots (but NOT /info). The
// underlying `ace` Klipper object is a MANAGER (ace_instances/current_index,
// no slots), so the WebSocket subscription path parses zero slots and the REST
// bridge must take over. These tests lock the four fixes for that path.
// ============================================================================

// --- Fix 1: manager-only object must fall through to REST fallback ----------

TEST_CASE("ACE manager-only ace object (no slots) selects REST fallback",
          "[ams][ace][rest_fallback][kobra]") {
    // Kobra S1 fork: `ace` is a manager with ace_instances/current_index and NO
    // slots array. select_ace_object must return nullptr so on_started
    // commits to REST polling instead of parsing zero slots off the manager.
    json status = json::object();
    status["ace"] = json{{"ace_instances", 1}, {"current_index", 0}};

    std::string key;
    const json* picked = AceTestAccess::select_ace_object(status, &key);

    CHECK(picked == nullptr);
    CHECK(key.empty());
}

TEST_CASE("ACE object WITH slots array still selects subscription path",
          "[ams][ace][rest_fallback][kobra]") {
    // Regression: a genuine slot-bearing `ace` (ValgACE) or `filament_hub`
    // (native) object must still be chosen for the WebSocket subscription path.
    SECTION("community ace key") {
        json status = json::object();
        status["ace"] = json{
            {"model", "ACE Pro"},
            {"slots", json::array({json{{"status", "available"}, {"type", "PLA"}}})},
        };
        std::string key;
        const json* picked = AceTestAccess::select_ace_object(status, &key);
        REQUIRE(picked != nullptr);
        CHECK(key == "ace");
    }

    SECTION("native filament_hub key preferred") {
        json status = json::object();
        status["filament_hub"] = make_native_filament_hub_payload();
        // A manager-only ace present alongside must NOT be picked over the
        // slot-bearing filament_hub.
        status["ace"] = json{{"ace_instances", 1}, {"current_index", 0}};
        std::string key;
        const json* picked = AceTestAccess::select_ace_object(status, &key);
        REQUIRE(picked != nullptr);
        CHECK(key == "filament_hub");
    }

    SECTION("empty slots array is not slot-bearing") {
        // An object with an empty slots array carries no slot data — treat it
        // like the manager-only case and fall through to REST.
        json status = json::object();
        status["ace"] = json{{"model", "ACE Pro"}, {"slots", json::array()}};
        std::string key;
        const json* picked = AceTestAccess::select_ace_object(status, &key);
        CHECK(picked == nullptr);
    }
}

// --- #1107: Kobra S1 mainline-Python fork registers `ace_instance_N` ---------
//
// The fork exposes each ACE unit as its own `ace_instance_N` Klipper object
// (has get_status()), NOT a top-level `ace`/`filament_hub`. If a future build
// of that fork carries a `slots` array on ace_instance_N, the object path must
// engage; if not, it falls through to the REST bridge like the manager case.

TEST_CASE("ACE ace_instance_0 WITH slots selects subscription path",
          "[ams][ace][rest_fallback][kobra]") {
    json status = json::object();
    status["ace_instance_0"] = json{
        {"model", "ACE Pro"},
        {"slots", json::array({json{{"status", "available"}, {"type", "PLA"}}})},
    };
    std::string key;
    const json* picked = AceTestAccess::select_ace_object(status, &key);
    REQUIRE(picked != nullptr);
    CHECK(key == "ace_instance_0");
}

TEST_CASE("ACE ace_instance_0 manager-shaped (no slots) selects REST fallback",
          "[ams][ace][rest_fallback][kobra]") {
    // The verified Kobra S1 fork exposes ace_instance_0 without a slots array
    // (has_get_status=True but manager-shaped). Must fall through to REST.
    json status = json::object();
    status["ace_instance_0"] = json{{"current_index", 0}, {"ace_count", 1}};
    std::string key;
    const json* picked = AceTestAccess::select_ace_object(status, &key);
    CHECK(picked == nullptr);
    CHECK(key.empty());
}

TEST_CASE("ACE select prefers filament_hub/ace over ace_instance_N",
          "[ams][ace][rest_fallback][kobra]") {
    SECTION("filament_hub beats a slot-bearing ace_instance_0") {
        json status = json::object();
        status["filament_hub"] = make_native_filament_hub_payload();
        status["ace_instance_0"] =
            json{{"slots", json::array({json{{"status", "available"}, {"type", "PLA"}}})}};
        std::string key;
        const json* picked = AceTestAccess::select_ace_object(status, &key);
        REQUIRE(picked != nullptr);
        CHECK(key == "filament_hub");
    }
    SECTION("lowest ace_instance_N wins when several carry slots") {
        json status = json::object();
        status["ace_instance_1"] =
            json{{"slots", json::array({json{{"status", "available"}, {"type", "ABS"}}})}};
        status["ace_instance_0"] =
            json{{"slots", json::array({json{{"status", "available"}, {"type", "PLA"}}})}};
        std::string key;
        const json* picked = AceTestAccess::select_ace_object(status, &key);
        REQUIRE(picked != nullptr);
        CHECK(key == "ace_instance_0");
    }
}

TEST_CASE("ACE status-update path parses ace_instance_0 with slots",
          "[ams][ace][native][parse][kobra]") {
    AmsBackendAceTestHelper helper;

    json inst = json{
        {"model", "ACE Pro"},
        {"status", "ready"},
        {"slots",
         json::array({
             json{{"status", "available"}, {"type", "PETG"}, {"color", json::array({0, 85, 255})}},
             json{{"status", "empty"}},
         })},
    };
    json status = json::object();
    status["ace_instance_0"] = inst;

    json notification = {{"params", json::array({status, 4242.0})}};
    helper.test_handle_status_update(notification);

    auto info = helper.get_system_info();
    CHECK(info.total_slots == 2);
    CHECK(helper.get_slot_info(0).material == "PETG");
    CHECK(helper.get_slot_info(0).color_rgb == 0x0055FFu);
}

// ============================================================================
// #1069 live capture: loaded-slot and live-shape support.
//
// On this fork no slot is ever "loaded" — the manager's current_index is the
// only seat signal, slots carry `material` (not `type`), the dryer nests
// under `dryer_status`, and REST /status carries the seat as
// ace_manager.current_index.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "ACE initial query seats the loaded slot from manager current_index",
                 "[ams][ace][1069]") {
    helix::test::FakeMoonrakerClient client;
    AmsBackendAceTestHelper helper(&client);

    helper.test_on_started();
    // The bootstrap must ask printer.objects.query for the known ACE object
    // names; the reply defers through the UpdateQueue like the real socket.
    REQUIRE_FALSE(client.rpc_calls.empty());
    const auto& call = client.rpc_calls.back();
    REQUIRE(call.method == "printer.objects.query");
    REQUIRE(call.success_cb);
    call.success_cb(make_kobra_objects_query_response(2));
    helix::ui::UpdateQueue::instance().drain();

    auto info = helper.get_test_system_info();
    // Sibling proof the instance parse ran, so the seat asserts below cannot
    // read green off an empty parse.
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slots.size() == 4);
    CHECK(info.units[0].name == "Anycubic Color Engine Pro");
    CHECK(info.version == "V1.3.856");
    CHECK(helper.get_slot_info(0).material == "PLA");
    CHECK(helper.get_slot_info(0).color_rgb == 0x00E676u);

    // T2 is loaded, per the manager's current_index.
    CHECK(info.filament_loaded == true);
    CHECK(info.current_tool == 2);
    CHECK(helper.get_slot_info(2).status == SlotStatus::LOADED);
    CHECK(helper.get_slot_info(1).status != SlotStatus::LOADED);

    // Capture dryer state: stopped, ambient 34.
    auto dryer = helper.get_test_dryer_info();
    CHECK(dryer.active == false);
    CHECK(dryer.current_temp_c == 34.0f);
}

TEST_CASE("ACE on_started without a client is a no-op, not a crash", "[ams][ace][1069]") {
    AmsBackendAceTestHelper helper;

    helper.test_on_started();
    helix::ui::UpdateQueue::instance().drain();

    auto info = helper.get_test_system_info();
    CHECK(info.units.empty());
    CHECK(info.filament_loaded == false);
}

TEST_CASE("ACE manager-only notify with current_index -1 clears the seat", "[ams][ace][1069]") {
    AmsBackendAceTestHelper helper;
    AceTestAccess::parse_ace(helper, make_kobra_instance_object());
    AceTestAccess::parse_ace(helper, make_kobra_manager_object(2));

    auto info = helper.get_test_system_info();
    REQUIRE(info.filament_loaded == true);
    REQUIRE(helper.get_slot_info(2).status == SlotStatus::LOADED);

    json frame = json::object();
    frame["ace"] = make_kobra_manager_object(-1);
    helper.test_handle_status_update({{"params", json::array({frame, 4242.0})}});

    info = helper.get_test_system_info();
    CHECK(info.filament_loaded == false);
    CHECK(info.current_tool == -1);
    CHECK(info.current_slot == -1);
    CHECK(helper.get_slot_info(2).status != SlotStatus::LOADED);
    // Sibling proof the notify parse ran against live slot state.
    CHECK(helper.get_slot_info(0).material == "PLA");
    CHECK(helper.get_slot_info(0).color_rgb == 0x00E676u);
}

TEST_CASE("ACE combined notify frame applies instance slots AND manager seat", "[ams][ace][1069]") {
    AmsBackendAceTestHelper helper;
    AceTestAccess::parse_ace(helper, make_kobra_instance_object());

    // Next frame carries both objects: slot 3's spool swapped (ABS, new
    // color) in ace_instance_0, and the manager still holding the T2 seat.
    json changed_instance = make_kobra_instance_object();
    changed_instance["slots"][3]["material"] = "ABS";
    changed_instance["slots"][3]["color"] = json::array({63, 81, 181});

    json frame = json::object();
    frame["ace"] = make_kobra_manager_object(2);
    frame["ace_instance_0"] = changed_instance;
    helper.test_handle_status_update({{"params", json::array({frame, 4243.0})}});

    // The instance half of the frame landed.
    CHECK(helper.get_slot_info(3).material == "ABS");
    CHECK(helper.get_slot_info(3).color_rgb == 0x3F51B5u);
    // And the manager half seated T2 on top of it.
    auto info = helper.get_test_system_info();
    CHECK(info.filament_loaded == true);
    CHECK(helper.get_slot_info(2).status == SlotStatus::LOADED);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE REST fallback seats from ace_manager current_index",
                 "[ams][ace][1069]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    RestResponse not_found;
    not_found.success = false;
    not_found.status_code = 404;
    not_found.error = "Not Found";
    api.rest_mock().mock_set_get_response("/server/ace/info", not_found);

    RestResponse status_ok;
    status_ok.success = true;
    status_ok.status_code = 200;
    status_ok.data = json{{"result", make_kobra_rest_status_result()}};
    api.rest_mock().mock_set_get_response("/server/ace/status", status_ok);

    RestResponse slots_ok;
    slots_ok.success = true;
    slots_ok.status_code = 200;
    slots_ok.data = json{{"result", json{{"slots", make_kobra_slots_array()}}}};
    api.rest_mock().mock_set_get_response("/server/ace/slots", slots_ok);

    AmsBackendAce backend(&api, nullptr);

    // Two poll cycles, as the 500 ms loop runs them: the first /status seat
    // arrives before /slots has populated any unit, and the next /status
    // re-states it against a populated slot vector.
    for (int i = 0; i < 2; ++i) {
        AceTestAccess::poll_status(backend);
        AceTestAccess::poll_slots(backend);
        helix::ui::UpdateQueue::instance().drain();
    }

    auto info = backend.get_system_info();
    REQUIRE(info.units.size() == 1);
    CHECK(info.filament_loaded == true);
    CHECK(info.current_tool == 2);
    CHECK(backend.get_slot_info(2).status == SlotStatus::LOADED);
    CHECK(backend.get_slot_info(0).material == "PLA");

    // Capture stop-state dryer values and the instance's ambient temp.
    auto dryer = backend.get_dryer_info();
    CHECK(dryer.active == false);
    CHECK(dryer.target_temp_c == 0.0f);
    CHECK(dryer.duration_min == 0);
    CHECK(dryer.remaining_min == 0);
    CHECK(dryer.current_temp_c == 34.0f);
}

TEST_CASE("ACE object path reads dryer under the dryer_status key", "[ams][ace][1069]") {
    AmsBackendAceTestHelper helper;

    json inst = make_kobra_instance_object();
    inst["dryer_status"] =
        json{{"status", "run"}, {"target_temp", 55}, {"duration", 240}, {"remain_time", 200}};
    inst["temp"] = 41;
    AceTestAccess::parse_ace(helper, inst);

    auto d = helper.get_test_dryer_info();
    CHECK(d.active == true);
    CHECK(d.target_temp_c == 55.0f);
    CHECK(d.duration_min == 240);
    CHECK(d.remaining_min == 200);
    CHECK(d.current_temp_c == 41.0f);
}

TEST_CASE("ACE dryer current_temp beats the ambient temp when stated", "[ams][ace][1069]") {
    AmsBackendAceTestHelper helper;

    // A bridge stating both: the dryer's own current temp is the more
    // specific reading than the top-level ambient.
    AceTestAccess::parse_ace(
        helper,
        json{{"temp", 28},
             {"dryer", json{{"active", true}, {"current_temp", 45.5}, {"target_temp", 55.0}}}});

    auto d = helper.get_test_dryer_info();
    CHECK(d.current_temp_c == 45.5f);
    CHECK(d.active == true);
    CHECK(d.target_temp_c == 55.0f);
}

TEST_CASE("ACE manager delta without current_index keeps the instance delta", "[ams][ace][1069]") {
    AmsBackendAceTestHelper helper;
    AceTestAccess::parse_ace(helper, make_kobra_instance_object());

    // Toolchange starting: the manager moved only target_index (current_index
    // still holds the old tool, so it is not in the delta), and the instance
    // reports "loading" in the same status cycle.
    json mgr = make_kobra_manager_object(2);
    mgr.erase("current_index");
    mgr["target_index"] = 3;

    json frame = json::object();
    frame["ace"] = mgr;
    frame["ace_instance_0"] = json{{"status", "loading"}};
    helper.test_handle_status_update({{"params", json::array({frame, 4244.0})}});

    // The instance half landed (its "loading" shows); the manager half had
    // nothing to say and must not have swallowed it.
    CHECK(helper.get_test_system_info().action == AmsAction::LOADING);
    CHECK(helper.get_slot_info(0).material == "PLA");
}

TEST_CASE("ACE lowest instance wins over a higher instance's slots", "[ams][ace][1069]") {
    AmsBackendAceTestHelper helper;
    AceTestAccess::parse_ace(helper, make_kobra_instance_object());

    // Unit 1's delta carries a whole slots array; unit 0's carries only a
    // temp delta. The display stays anchored to unit 0's inventory.
    json unit1 = make_kobra_instance_object();
    unit1["instance"] = 1;
    unit1["slots"][0]["material"] = "ABS";
    unit1["slots"][0]["color"] = json::array({63, 81, 181});

    json frame = json::object();
    frame["ace_instance_0"] = json{{"temp", 35.2}};
    frame["ace_instance_1"] = unit1;
    helper.test_handle_status_update({{"params", json::array({frame, 4245.0})}});

    CHECK(helper.get_slot_info(0).material == "PLA");
    CHECK(helper.get_slot_info(0).color_rgb == 0x00E676u);
    CHECK(helper.get_test_dryer_info().current_temp_c == 35.2f);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE REST unit-status idle keeps an in-flight load",
                 "[ams][ace][1069]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    RestResponse status_ok;
    status_ok.success = true;
    status_ok.status_code = 200;
    status_ok.data = json{{"result", make_kobra_rest_status_result()}};
    api.rest_mock().mock_set_get_response("/server/ace/status", status_ok);

    AmsBackendAce backend(&api, nullptr);

    // A local load is in flight: do_load_filament sets LOADING optimistically
    // precisely because the module may not report "loading" itself.
    AceTestAccess::parse_ace(backend, json{{"status", "loading"}});
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    // The fork's /status states the UNIT's status ("ready"), which resolves
    // to IDLE — it must not demote the in-flight op.
    AceTestAccess::poll_status(backend);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(backend.get_system_info().action == AmsAction::LOADING);

    // Sibling proof the polls are being parsed: /slots populates the unit,
    // and the next /status seats onto it (the first seat landed before any
    // unit existed, so current_slot stayed -1) — still without demoting.
    AceTestAccess::poll_slots(backend);
    AceTestAccess::poll_status(backend);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(backend.get_system_info().action == AmsAction::LOADING);
    CHECK(backend.get_system_info().current_tool == 2);
    CHECK(backend.get_slot_info(2).status == SlotStatus::LOADED);

    // The same fallback still surfaces recognized words: an errored unit
    // shows ERROR.
    json errored = make_kobra_rest_status_result();
    errored["status"] = "error";
    RestResponse status_err;
    status_err.success = true;
    status_err.status_code = 200;
    status_err.data = json{{"result", errored}};
    api.rest_mock().mock_set_get_response("/server/ace/status", status_err);

    AceTestAccess::poll_status(backend);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(backend.get_system_info().action == AmsAction::ERROR);
}

TEST_CASE("ACE object-path material prefers material and keeps the type fallback",
          "[ams][ace][1069]") {
    SECTION("type only (ValgACE spelling) still parses material") {
        AmsBackendAceTestHelper helper;
        AceTestAccess::parse_ace(helper, json{{"model", "ACE Pro"},
                                              {"firmware", "1.2.3"},
                                              {"status", "ready"},
                                              {"slots", json::array({json{
                                                            {"status", "available"},
                                                            {"color", json::array({255, 0, 0})},
                                                            {"type", "ABS"},
                                                        }})}});
        CHECK(helper.get_slot_info(0).material == "ABS");
    }
    SECTION("material wins when both keys are stated") {
        AmsBackendAceTestHelper helper;
        AceTestAccess::parse_ace(helper, json{{"model", "ACE Pro"},
                                              {"firmware", "1.2.3"},
                                              {"status", "ready"},
                                              {"slots", json::array({json{
                                                            {"status", "ready"},
                                                            {"material", "PETG"},
                                                            {"type", "PLA"},
                                                            {"color", json::array({0, 255, 0})},
                                                        }})}});
        CHECK(helper.get_slot_info(0).material == "PETG");
    }
}

// --- Fix 3: REST slot parser accepts `type` as a material alias -------------

TEST_CASE("ACE parse_slots_response falls back to type for material", "[ams][ace][parse][kobra]") {
    AmsBackendAceTestHelper helper;

    // The live capture shows /slots returning `material`; `type` is the
    // ValgACE spelling, kept as the fallback.
    json data = {
        {"slots", {{{"index", 0}, {"status", "ready"}, {"type", "PLA"}, {"color", "#FF0000"}}}}};

    bool changed = helper.test_parse_slots_response(data);
    REQUIRE(changed == true);

    auto slot0 = helper.get_slot_info(0);
    CHECK(slot0.material == "PLA"); // type -> material
}

TEST_CASE("ACE parse_slots_response prefers material over type when both present",
          "[ams][ace][parse][kobra]") {
    AmsBackendAceTestHelper helper;

    json data = {{"slots",
                  {{{"index", 0},
                    {"status", "ready"},
                    {"material", "PETG"},
                    {"type", "PLA"},
                    {"color", "#00FF00"}}}}};

    helper.test_parse_slots_response(data);
    CHECK(helper.get_slot_info(0).material == "PETG"); // explicit material wins
}

// --- Fix 4: widen REST slot status vocabulary to mirror the object path ------

TEST_CASE("ACE parse_slots_response maps ready/preload/running to AVAILABLE",
          "[ams][ace][parse][kobra]") {
    AmsBackendAceTestHelper helper;

    json data = {{"slots",
                  {{{"index", 0}, {"status", "ready"}},
                   {{"index", 1}, {"status", "preload"}},
                   {{"index", 2}, {"status", "running"}},
                   {{"index", 3}, {"status", "runout"}}}}};

    helper.test_parse_slots_response(data);

    CHECK(helper.get_slot_info(0).status == SlotStatus::AVAILABLE); // ready
    CHECK(helper.get_slot_info(1).status == SlotStatus::AVAILABLE); // preload
    CHECK(helper.get_slot_info(2).status == SlotStatus::AVAILABLE); // running
    CHECK(helper.get_slot_info(3).status == SlotStatus::EMPTY);     // runout
}

TEST_CASE("ACE parse_slots_response maps unknown to UNKNOWN like the object path",
          "[ams][ace][parse][kobra]") {
    AmsBackendAceTestHelper helper;

    json data = {{"slots", {{{"index", 0}, {"status", "unknown"}}}}};
    helper.test_parse_slots_response(data);
    CHECK(helper.get_slot_info(0).status == SlotStatus::UNKNOWN);
}

// --- Fix 2a: /status carries model + firmware (no /info required) ------------

TEST_CASE("ACE parse_status_response derives model and firmware", "[ams][ace][parse][kobra]") {
    AmsBackendAceTestHelper helper;

    // The fork's /server/ace/status envelope carries model + firmware, so the
    // backend no longer needs /server/ace/info to identify the hardware.
    json data = {{"model", "ACE Pro"}, {"firmware", "2.5.1"}, {"loaded_slot", -1}};

    helper.test_parse_status_response(data);
    auto info = helper.get_test_system_info();

    CHECK(info.type_name == "ACE");
    CHECK(info.version == "2.5.1"); // firmware -> version
}

// --- Fix 2b: /info optional — no false "install ValgACE" error --------------

TEST_CASE_METHOD(LVGLTestFixture,
                 "ACE missing /info does not surface an error when /status succeeds",
                 "[ams][ace][rest_fallback][kobra]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Fork surface: /info 404s, /status + /slots succeed. /status carries model.
    RestResponse info_404;
    info_404.success = false;
    info_404.status_code = 404;
    info_404.error = "Not Found";
    api.rest_mock().mock_set_get_response("/server/ace/info", info_404);

    RestResponse status_ok;
    status_ok.success = true;
    status_ok.status_code = 200;
    status_ok.data = {
        {"result", {{"model", "ACE Pro"}, {"firmware", "2.5.1"}, {"loaded_slot", -1}}}};
    api.rest_mock().mock_set_get_response("/server/ace/status", status_ok);
    // /slots falls through to the mock's built-in 4-slot canned response.

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;

    std::vector<std::string> events;
    backend.set_event_callback(
        [&events](const std::string& ev, const std::string&) { events.push_back(ev); });

    // Drive the poll methods the loop would run: /info fails repeatedly (up to
    // the give-up threshold), while /status + /slots succeed each cycle.
    for (int i = 0; i < 4; ++i) {
        AceTestAccess::poll_info(backend);
        AceTestAccess::poll_status(backend);
        AceTestAccess::poll_slots(backend);
        helix::ui::UpdateQueue::instance().drain();
    }

    // No error event surfaced — /status is succeeding, so the "install
    // ace_status.py from ValgACE" toast must NOT fire (the user already has a
    // working bridge; #1069).
    CHECK(std::count(events.begin(), events.end(), std::string(AmsBackend::EVENT_ERROR)) == 0);

    // Backend populated model + slots purely from /status + /slots.
    auto info = backend.get_system_info();
    CHECK(info.type_name == "ACE");
    CHECK(info.version == "2.5.1");
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].slots.size() == 4);
}

TEST_CASE_METHOD(LVGLTestFixture, "ACE surfaces an error when the data endpoints are all missing",
                 "[ams][ace][rest_fallback][kobra]") {
    // Genuinely-missing-bridge case: /info, /status AND /slots all 404. The
    // error must still surface — but now gated on the DATA endpoints failing,
    // not on /info.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    RestResponse not_found;
    not_found.success = false;
    not_found.status_code = 404;
    not_found.error = "Not Found";
    api.rest_mock().mock_set_get_response("/server/ace/info", not_found);
    api.rest_mock().mock_set_get_response("/server/ace/status", not_found);
    api.rest_mock().mock_set_get_response("/server/ace/slots", not_found);

    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(&api, nullptr);
    AmsBackendAce& backend = *backend_reg;

    int error_events = 0;
    backend.set_event_callback([&error_events](const std::string& ev, const std::string&) {
        if (ev == AmsBackend::EVENT_ERROR)
            ++error_events;
    });

    // Drive enough failing /status cycles to cross the threshold.
    for (int i = 0; i < 5; ++i) {
        AceTestAccess::poll_status(backend);
        helix::ui::UpdateQueue::instance().drain();
    }

    CHECK(error_events >= 1);
}

TEST_CASE("ACE adjusts a running dryer only by stopping and restarting it",
          "[ams][ace][dryer][capability]") {
    helix::test::RegisteredBackend<AmsBackendAce> backend_reg(nullptr, nullptr);
    AmsBackendAce& backend = *backend_reg;
    auto d = backend.get_dryer_info();
    REQUIRE(d.supported);
    // ACE_START_DRYING and ACE_STOP_DRYING are the entire surface; there is no
    // set-temperature-while-running command, so neither adjustment can be live.
    CHECK_FALSE(d.supports_live_temp);
    CHECK_FALSE(d.supports_live_duration);
}

namespace {

/// Captures what ACE would send, so a restart's DURATION can be inspected.
class AceCaptureBackend : public AmsBackendAce {
  public:
    AceCaptureBackend() : AmsBackendAce(nullptr, nullptr) {}
    std::vector<std::string> gcodes;
    helix::AmsError execute_gcode(const std::string& g) override {
        gcodes.push_back(g);
        return helix::AmsErrorHelper::success();
    }
    [[nodiscard]] bool sent(const std::string& g) const {
        return std::find(gcodes.begin(), gcodes.end(), g) != gcodes.end();
    }

    /// Commands refuse until the subscription is up; tests drive the backend directly.
    void mark_running() {
        running_ = true;
    }
};

} // namespace

TEST_CASE("Retargeting an ACE dryer keeps the time already served", "[ams][ace][dryer][update]") {
    AceCaptureBackend backend;
    backend.mark_running();
    // Three hours into a four-hour session.
    AceTestAccess::set_dryer_run(backend, 55.0f, 240, 60);
    backend.gcodes.clear();

    REQUIRE(backend.update_drying(50.0f).success());

    // ACE has no live retarget, so a temperature change restarts the cycle. It has to
    // restart on what is LEFT: restarting on the original session length silently hands
    // the user three more hours of drying they did not ask for.
    CHECK(backend.sent("ACE_START_DRYING TEMP=50 DURATION=60"));
    CHECK_FALSE(backend.sent("ACE_START_DRYING TEMP=50 DURATION=240"));
}

// ACE persists into the SHARED lane_data namespace, which Mainsail, OrcaSlicer
// and AFC's plugin also read. Publishing the lock flags false beside a user's
// edit tells every one of them the value is not the user's (#965, #1649).
TEST_CASE("ACE publishes a persisted edit to lane_data as the user's own",
          "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("issue1649_user_locks");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAce backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    AceTestAccess::parse_ace(backend, json{{"model", "ACE Pro"},
                                           {"slots", json::array({
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                         json{{"status", "empty"}},
                                                     })}});

    SlotInfo edit;
    edit.material = "PETG";
    edit.color_rgb = 0x1188FF;
    edit.color_name = "Blue";
    REQUIRE(helix::test::apply_edit(backend, 0, edit).success());

    auto staged = AceTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    REQUIRE(staged->material == "PETG");
    REQUIRE(staged->color_rgb == 0x1188FFu);
    CHECK(helix::ams::declares_color(*staged));
    CHECK(helix::ams::declares_material(*staged));

    // The record every other reader of the namespace actually sees.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    REQUIRE(stored["color"] == "#1188FF");
    REQUIRE(stored["material"] == "PETG");
    CHECK(stored["helix_locked_color"] == true);
    CHECK(stored["helix_locked_material"] == true);
}
