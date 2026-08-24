// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "ams_backend_ace.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lvgl_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_types.h"
#include "printer_state.h"

#include <algorithm>
#include <filesystem>
#include <json.hpp> // nlohmann/json from libhv
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

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

// Friend-class shim for AmsBackendAce — declared as friend in the backend
// header. Gives tests narrow accessors for override state without going
// through public get_slot_info (which layers apply_overrides on top).
class AceTestAccess {
  public:
    static void seed_override(AmsBackendAce& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }
    static std::optional<helix::ams::FilamentSlotOverride> get_override(const AmsBackendAce& b,
                                                                        int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    static void inject_override_store(AmsBackendAce& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    // Drive parse_ace_object directly (public-ish parse entry point on the
    // production class) — saves every test from rebuilding a full status
    // notify envelope.
    static void parse_ace(AmsBackendAce& b, const json& data) {
        b.parse_ace_object(data);
    }

    // Expose the on_started() subscription-vs-REST decision (#1069). Returns
    // the slot-bearing object, or nullptr when the REST fallback should run.
    static const json* select_slot_bearing_object(const json& status, std::string* key) {
        return AmsBackendAce::select_slot_bearing_object(status, key);
    }

    // Drive the individual REST poll methods (private; async-defer their work
    // through the UpdateQueue, so tests must drain the queue afterward).
    static void poll_info(AmsBackendAce& b) {
        b.poll_info();
    }
    static void poll_status(AmsBackendAce& b) {
        b.poll_status();
    }
    static void poll_slots(AmsBackendAce& b) {
        b.poll_slots();
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
// (ValgACE's native format). `material_str` goes into "type".
json make_ace_slot_payload(const std::string& status_str, uint32_t color_rgb,
                           const std::string& material_str) {
    uint8_t r = (color_rgb >> 16) & 0xFF;
    uint8_t g = (color_rgb >> 8) & 0xFF;
    uint8_t b = color_rgb & 0xFF;
    return json{
        {"model", "ACE Pro"},
        {"firmware", "1.2.3"},
        {"status", "ready"},
        {"slots",
         json::array({json{
             {"status", status_str}, {"color", json::array({r, g, b})}, {"type", material_str}}})},
    };
}

// Build a native Anycubic GoKlipper `filament_hub` get_status() payload (the
// FLAT, single-hub schema Rinkhals firmware exposes): 4 slots, one "ready"
// PLA slot with an RGB color, the rest "empty"; a live "dryer" object in the
// "drying" state; and current_filament "0-1" (local slot 1 loaded).
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

    // State accessors for verification
    AmsSystemInfo get_test_system_info() const {
        return get_system_info();
    }

    DryerInfo get_test_dryer_info() const {
        return get_dryer_info();
    }
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

TEST_CASE("ACE bypass not supported", "[ams][ace][bypass]") {
    AmsBackendAceTestHelper helper;
    REQUIRE(helper.is_bypass_active() == false);

    auto err = helper.enable_bypass();
    REQUIRE(!err.success());
    REQUIRE(err.result == AmsResult::NOT_SUPPORTED);

    err = helper.disable_bypass();
    REQUIRE(!err.success());
    REQUIRE(err.result == AmsResult::NOT_SUPPORTED);
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
//   3. set_slot_info(persist=true) writes through to the store.
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

    AmsBackendAce backend(&api, nullptr);
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

TEST_CASE("ACE set_slot_info(persist=true) writes to store", "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("task13_persist_true");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAce backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    // Prime the backend with 4 slots so set_slot_info's index check passes.
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
    edit.remaining_weight_g = 850.0f;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

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
    CHECK(stored["material"] == "PLA");
    CHECK(stored["color"] == "#FF5500");

    // Legacy namespace NOT touched — ACE no longer writes there.
    CHECK(api.mock_get_db_value("helix-screen", "ace_slot_overrides").is_null());
}

TEST_CASE("ACE set_slot_info(persist=false) does NOT write to store",
          "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("task13_persist_false");
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
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
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

TEST_CASE("ACE slot transition empty -> present clears override",
          "[ams][ace][filament_slot_override]") {
    AceTmpCacheDir tmp("task13_empty_to_present_clears");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAce backend(&api, nullptr);
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

    // First parse: slot is EMPTY. prev_slot_status_ is unset (baseline UNKNOWN);
    // UNKNOWN -> EMPTY is NOT a swap (curr is not "present"), so no clear.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));
    REQUIRE(AceTestAccess::get_override(backend, 0).has_value());
    REQUIRE(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: slot becomes AVAILABLE with a different color — EMPTY ->
    // AVAILABLE is the swap signal. Override MUST be cleared (in-memory and
    // in MR DB).
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("available", 0x0055FF, "PETG"));

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

TEST_CASE("ACE slot transition loaded -> empty does NOT clear override",
          "[ams][ace][filament_slot_override]") {
    // User unloaded the current spool but hasn't swapped yet. The override
    // must survive — they may reinsert the same spool. Only the inverse
    // transition (empty -> present) is a swap signal.
    AceTmpCacheDir tmp("task13_loaded_to_empty_preserves");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendAce backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "ace");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    AceTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    AceTestAccess::seed_override(backend, 0, ovr);

    // First parse: slot LOADED. First observation is a BASELINE and never
    // fires a clear (caller skips the helper when prev_slot_status_ has no
    // entry). Override survives intact.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("loaded", 0xFF5500, "PLA"));
    REQUIRE(AceTestAccess::get_override(backend, 0).has_value());

    // Second parse: LOADED -> EMPTY (user unloaded). Must NOT clear — user
    // may still reinsert the same spool.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("empty", 0x000000, ""));
    CHECK(AceTestAccess::get_override(backend, 0).has_value());

    // Third parse: EMPTY -> LOADED (user reinserts SAME spool). This IS a
    // "present" transition, so the override IS cleared — the status-based
    // heuristic can't distinguish "reinsert same spool" from "insert new
    // spool." Documented limitation: ACE users who unload and reload the
    // same spool will lose their override. Acceptable tradeoff — far less
    // common than the new-spool path the heuristic is built for.
    AceTestAccess::parse_ace(backend, make_ace_slot_payload("loaded", 0xFF5500, "PLA"));
    CHECK_FALSE(AceTestAccess::get_override(backend, 0).has_value());
}

TEST_CASE("ACE partial override only replaces specified fields",
          "[ams][ace][filament_slot_override]") {
    AmsBackendAce backend(nullptr, nullptr);

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

    AmsBackendAce backend(&api, nullptr);
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

    AmsBackendAce backend(&api, nullptr);
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
    AmsBackendAce backend(nullptr, nullptr);

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
    AmsBackendAce backend(nullptr, nullptr);

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
    AmsBackendAce backend(nullptr, nullptr);

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
    // slots array. select_slot_bearing_object must return nullptr so on_started
    // commits to REST polling instead of parsing zero slots off the manager.
    json status = json::object();
    status["ace"] = json{{"ace_instances", 1}, {"current_index", 0}};

    std::string key;
    const json* picked = AceTestAccess::select_slot_bearing_object(status, &key);

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
        const json* picked = AceTestAccess::select_slot_bearing_object(status, &key);
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
        const json* picked = AceTestAccess::select_slot_bearing_object(status, &key);
        REQUIRE(picked != nullptr);
        CHECK(key == "filament_hub");
    }

    SECTION("empty slots array is not slot-bearing") {
        // An object with an empty slots array carries no slot data — treat it
        // like the manager-only case and fall through to REST.
        json status = json::object();
        status["ace"] = json{{"model", "ACE Pro"}, {"slots", json::array()}};
        std::string key;
        const json* picked = AceTestAccess::select_slot_bearing_object(status, &key);
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
    const json* picked = AceTestAccess::select_slot_bearing_object(status, &key);
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
    const json* picked = AceTestAccess::select_slot_bearing_object(status, &key);
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
        const json* picked = AceTestAccess::select_slot_bearing_object(status, &key);
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
        const json* picked = AceTestAccess::select_slot_bearing_object(status, &key);
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

// --- Fix 3: REST slot parser accepts `type` as a material alias -------------

TEST_CASE("ACE parse_slots_response falls back to type for material", "[ams][ace][parse][kobra]") {
    AmsBackendAceTestHelper helper;

    // Kobra S1 /slots returns `type` (like the object path), not `material`.
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

    AmsBackendAce backend(&api, nullptr);

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

    AmsBackendAce backend(&api, nullptr);

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
