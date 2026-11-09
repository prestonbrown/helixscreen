// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_telemetry_manager.cpp
 * @brief Unit tests for TelemetryManager - Anonymous device telemetry
 *
 * Tests UUID v4 generation, SHA-256 double-hash anonymization,
 * event queue management, session/print outcome event schemas,
 * enable/disable toggle, and queue persistence.
 *
 * Written TDD-style - tests WILL FAIL if TelemetryManager is removed.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_board.h"
#include "../test_helpers/scoped_moonraker_api.h"
#include "../test_helpers/scoped_moonraker_client.h"
#include "../test_helpers/telemetry_manager_test_access.h"
#include "../test_helpers/telemetry_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "display_settings_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "platform_table.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "screensaver_registry.h"
#include "screensaver_usage.h"
#include "system/debug_bundle_collector.h"
#include "system/sha256_util.h"
#include "system/telemetry_manager.h"
#include "theme_loader.h"
#include "theme_manager.h"
#include "version.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// TelemetryManager is a global singleton (not namespaced)

// ============================================================================
// UUID v4 Generation [telemetry][uuid]
// ============================================================================

TEST_CASE("UUID v4: format is 8-4-4-4-12 hex characters", "[telemetry][uuid]") {
    auto uuid = TelemetryManager::generate_uuid_v4();

    // UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    // Total length: 36 characters (32 hex + 4 dashes)
    REQUIRE(uuid.size() == 36);

    // Validate format with regex: 8-4-4-4-12 hex groups separated by dashes
    std::regex uuid_regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    REQUIRE(std::regex_match(uuid, uuid_regex));
}

TEST_CASE("UUID v4: version bits are correct (position 14 == '4')", "[telemetry][uuid]") {
    // Generate multiple UUIDs and verify version nibble
    for (int i = 0; i < 10; ++i) {
        auto uuid = TelemetryManager::generate_uuid_v4();
        // Position 14 in the string is the version nibble (first char of 3rd group)
        // Format: xxxxxxxx-xxxx-Vxxx-yxxx-xxxxxxxxxxxx
        //         0       8 9  13 14
        REQUIRE(uuid[14] == '4');
    }
}

TEST_CASE("UUID v4: variant bits are correct (position 19 is 8/9/a/b)", "[telemetry][uuid]") {
    // Generate multiple UUIDs and verify variant nibble
    for (int i = 0; i < 10; ++i) {
        auto uuid = TelemetryManager::generate_uuid_v4();
        // Position 19 is the variant nibble (first char of 4th group)
        // Format: xxxxxxxx-xxxx-4xxx-Yxxx-xxxxxxxxxxxx
        //         0       8 9  13 14 18 19
        char variant = uuid[19];
        bool valid_variant = (variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
        INFO("UUID: " << uuid << ", variant char: " << variant);
        REQUIRE(valid_variant);
    }
}

TEST_CASE("UUID v4: generated UUIDs are unique", "[telemetry][uuid]") {
    std::set<std::string> uuids;
    constexpr int kNumUuids = 50;

    for (int i = 0; i < kNumUuids; ++i) {
        auto uuid = TelemetryManager::generate_uuid_v4();
        uuids.insert(uuid);
    }

    // All generated UUIDs must be distinct
    REQUIRE(uuids.size() == kNumUuids);
}

TEST_CASE("UUID v4: only contains valid characters", "[telemetry][uuid]") {
    auto uuid = TelemetryManager::generate_uuid_v4();

    for (size_t i = 0; i < uuid.size(); ++i) {
        char c = uuid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            REQUIRE(c == '-');
        } else {
            bool valid_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            INFO("Position " << i << " has invalid character: " << c);
            REQUIRE(valid_hex);
        }
    }
}

// ============================================================================
// Double-hash Anonymization [telemetry][hash]
// ============================================================================

TEST_CASE("hash_device_id: consistent output for same inputs", "[telemetry][hash]") {
    std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
    std::string salt = "test-salt-value";

    auto hash1 = TelemetryManager::hash_device_id(uuid, salt);
    auto hash2 = TelemetryManager::hash_device_id(uuid, salt);

    REQUIRE(hash1 == hash2);
}

TEST_CASE("hash_device_id: different salts produce different output", "[telemetry][hash]") {
    std::string uuid = "550e8400-e29b-41d4-a716-446655440000";

    auto hash1 = TelemetryManager::hash_device_id(uuid, "salt-alpha");
    auto hash2 = TelemetryManager::hash_device_id(uuid, "salt-beta");

    REQUIRE(hash1 != hash2);
}

TEST_CASE("hash_device_id: different UUIDs produce different output", "[telemetry][hash]") {
    std::string salt = "shared-salt";

    auto hash1 = TelemetryManager::hash_device_id("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", salt);
    auto hash2 = TelemetryManager::hash_device_id("11111111-2222-4333-8444-555555555555", salt);

    REQUIRE(hash1 != hash2);
}

TEST_CASE("sha256_hex matches the FIPS 180-4 test vectors", "[telemetry][hash]") {
    REQUIRE(helix::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(helix::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(helix::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("hash_device_id: known value, so stored device ids stay stable", "[telemetry][hash]") {
    REQUIRE(TelemetryManager::hash_device_id("550e8400-e29b-41d4-a716-446655440000", "test-salt") ==
            "21fffeed629d1a1d7ff8bfcf6af51553abfc0ed61b5351a1f2aaa9ef513380b1");
}

TEST_CASE("hash_device_id: output is 64 hex characters (SHA-256)", "[telemetry][hash]") {
    std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
    std::string salt = "test-salt";

    auto hash = TelemetryManager::hash_device_id(uuid, salt);

    // SHA-256 produces 256 bits = 64 hex characters
    REQUIRE(hash.size() == 64);

    // All characters must be valid lowercase hex
    std::regex hex_regex("^[0-9a-f]{64}$");
    REQUIRE(std::regex_match(hash, hex_regex));
}

TEST_CASE("hash_device_id: original UUID not present in output (irreversibility)",
          "[telemetry][hash]") {
    std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
    std::string salt = "anonymization-salt";

    auto hash = TelemetryManager::hash_device_id(uuid, salt);

    // The original UUID (or any substring of it) should not appear in the hash
    REQUIRE(hash.find("550e8400") == std::string::npos);
    REQUIRE(hash.find("446655440000") == std::string::npos);

    // Also verify the hash is not just the UUID with dashes removed
    std::string uuid_no_dashes = "550e8400e29b41d4a716446655440000";
    REQUIRE(hash != uuid_no_dashes);
}

TEST_CASE("hash_device_id: empty inputs produce valid hash", "[telemetry][hash]") {
    // Edge case: empty uuid and salt should still produce a valid 64-char hash
    auto hash = TelemetryManager::hash_device_id("", "");
    REQUIRE(hash.size() == 64);

    std::regex hex_regex("^[0-9a-f]{64}$");
    REQUIRE(std::regex_match(hash, hex_regex));
}

// ============================================================================
// Event Queue [telemetry][queue]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: starts empty after init", "[telemetry][queue]") {
    auto& tm = TelemetryManager::instance();
    REQUIRE(tm.queue_size() == 0);

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.is_array());
    REQUIRE(snapshot.empty());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: enqueue adds events", "[telemetry][queue]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    REQUIRE(tm.queue_size() == 1);

    tm.record_session();
    REQUIRE(tm.queue_size() == 2);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: respects max capacity and drops oldest",
                 "[telemetry][queue]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Fill the queue to max capacity
    for (size_t i = 0; i < TelemetryManager::kMaxQueueSize; ++i) {
        tm.record_session();
    }
    REQUIRE(tm.queue_size() == TelemetryManager::kMaxQueueSize);

    // Record one more - should drop the oldest
    tm.record_print_outcome("completed", 600, 10, 1500.0f, "PLA", 210, 60);
    REQUIRE(tm.queue_size() == TelemetryManager::kMaxQueueSize);

    // The newest event should be the print outcome, not a session event
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.is_array());
    auto last_event = snapshot.back();
    REQUIRE(last_event["event"] == "print_outcome");
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: clear removes all events", "[telemetry][queue]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_session();
    REQUIRE(tm.queue_size() == 2);

    tm.clear_queue();
    REQUIRE(tm.queue_size() == 0);
    REQUIRE(tm.get_queue_snapshot().empty());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: snapshot is a copy (not a reference)",
                 "[telemetry][queue]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 1);

    // Adding another event should not affect the snapshot
    tm.record_session();
    REQUIRE(snapshot.size() == 1);
    REQUIRE(tm.queue_size() == 2);
}

// ============================================================================
// Session Event Schema [telemetry][session]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: has all required fields",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 1);

    auto event = snapshot[0];
    REQUIRE(event.contains("schema_version"));
    REQUIRE(event.contains("event"));
    REQUIRE(event.contains("device_id"));
    REQUIRE(event.contains("timestamp"));

    REQUIRE(event["event"] == "session");
    REQUIRE(event["schema_version"].is_number_integer());
    REQUIRE(event["device_id"].is_string());
    REQUIRE(event["timestamp"].is_string());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: has app section with version and platform",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    REQUIRE(event.contains("app"));
    REQUIRE(event["app"].contains("version"));
    REQUIRE(event["app"].contains("platform"));
    REQUIRE(event["app"]["version"].is_string());
    REQUIRE(event["app"]["platform"].is_string());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: app.board is the resolved board",
                 "[telemetry][session]") {
    helix::test::ScopedBoard board(helix::board::Board::Creator5Pro);
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    CHECK(event["app"]["board"] == "creator5-pro");
    CHECK(event["app"]["platform"] == helix::platform::current_key());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: app.zip_tool reports zip capability",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    REQUIRE(tm.queue_size() == 1);
    auto event = tm.get_queue_snapshot()[0];

    // Gates the .tar.gz retirement (#993): combined with app.version this says
    // whether a device could survive a zip-only release. Must be present even
    // in this test binary, which has no DisplayManager -- the field lives
    // outside that guard on purpose.
    REQUIRE(event.contains("app"));
    REQUIRE(event["app"].contains("zip_tool"));
    REQUIRE(event["app"]["zip_tool"].is_string());

    const std::string zip_tool = event["app"]["zip_tool"];
    const std::set<std::string> legal_values{"unzip", "python", "none"};
    INFO("zip_tool was: " << zip_tool);
    REQUIRE(legal_values.count(zip_tool) == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: host.screen_locality is local or remote",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    REQUIRE(tm.queue_size() == 1);
    auto event = tm.get_queue_snapshot()[0];

    // Derived from the same host-identity check that gates every klippy-UDS
    // feature (helix::is_moonraker_on_same_host). Must always be present -
    // it does not depend on DisplayManager or a live printer connection.
    REQUIRE(event.contains("host"));
    REQUIRE(event["host"].contains("screen_locality"));
    REQUIRE(event["host"]["screen_locality"].is_string());

    const std::string locality = event["host"]["screen_locality"];
    const std::set<std::string> legal_values{"local", "remote"};
    INFO("screen_locality was: " << locality);
    REQUIRE(legal_values.count(locality) == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: app.zip_tool is on EVERY session event",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // The probe is cached in a function-local static so it runs at most once
    // per process (it can exec python3, which is slow on MIPS). Caching must
    // not turn the field into a first-session-only field, and both sessions
    // must agree on the answer.
    tm.record_session();
    tm.record_session();

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 2);

    const std::set<std::string> legal_values{"unzip", "python", "none"};
    for (const auto& event : snapshot) {
        REQUIRE(event.at("app").contains("zip_tool"));
        const std::string zip_tool = event.at("app").at("zip_tool");
        REQUIRE(legal_values.count(zip_tool) == 1);
    }
    REQUIRE(snapshot[0]["app"]["zip_tool"] == snapshot[1]["app"]["zip_tool"]);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: does NOT contain PII fields",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    // Must NOT contain any PII-identifying fields
    REQUIRE_FALSE(event.contains("ip"));
    REQUIRE_FALSE(event.contains("ip_address"));
    REQUIRE_FALSE(event.contains("hostname"));
    REQUIRE_FALSE(event.contains("username"));
    REQUIRE_FALSE(event.contains("mac_address"));
    REQUIRE_FALSE(event.contains("filename"));
    REQUIRE_FALSE(event.contains("serial_number"));
    REQUIRE_FALSE(event.contains("email"));
    REQUIRE_FALSE(event.contains("ssid"));

    // Device ID should be a hash, not a raw UUID
    std::string device_id = event["device_id"];
    std::regex uuid_regex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    REQUIRE_FALSE(std::regex_match(device_id, uuid_regex));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: timestamp is ISO 8601 format",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    std::string timestamp = event["timestamp"];
    // ISO 8601: YYYY-MM-DDTHH:MM:SSZ or similar
    // At minimum, should contain 'T' separator and be reasonable length
    REQUIRE(timestamp.size() >= 19);
    REQUIRE(timestamp.find('T') != std::string::npos);
}

// ============================================================================
// Print Outcome Event Schema [telemetry][print_outcome]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Print outcome event: has required fields",
                 "[telemetry][print_outcome]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_print_outcome("completed", 3600, 10, 2500.0f, "PLA", 215, 60);
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 1);

    auto event = snapshot[0];
    REQUIRE(event.contains("schema_version"));
    REQUIRE(event.contains("event"));
    REQUIRE(event.contains("device_id"));
    REQUIRE(event.contains("timestamp"));

    REQUIRE(event["event"] == "print_outcome");
    REQUIRE(event.contains("outcome"));
    REQUIRE(event["outcome"] == "completed");
}

TEST_CASE_METHOD(TelemetryTestFixture, "Print outcome event: does NOT contain filename or gcode",
                 "[telemetry][print_outcome]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_print_outcome("failed", 1800, 5, 800.0f, "PETG", 240, 80);
    auto event = tm.get_queue_snapshot()[0];

    // Must NOT leak file or gcode information
    REQUIRE_FALSE(event.contains("filename"));
    REQUIRE_FALSE(event.contains("file"));
    REQUIRE_FALSE(event.contains("filepath"));
    REQUIRE_FALSE(event.contains("gcode"));
    REQUIRE_FALSE(event.contains("gcode_file"));
    REQUIRE_FALSE(event.contains("path"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Print outcome event: captures duration and filament",
                 "[telemetry][print_outcome]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_print_outcome("completed", 7200, 10, 3200.5f, "ABS", 250, 110);
    auto event = tm.get_queue_snapshot()[0];

    // Duration should be captured
    REQUIRE(event.contains("duration_sec"));
    REQUIRE(event["duration_sec"] == 7200);

    // Filament info should be captured
    REQUIRE(event.contains("filament_used_mm"));
    REQUIRE(event["filament_used_mm"].is_number());

    REQUIRE(event.contains("filament_type"));
    REQUIRE(event["filament_type"] == "ABS");
}

TEST_CASE_METHOD(TelemetryTestFixture, "Print outcome event: captures temperature and phase info",
                 "[telemetry][print_outcome]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_print_outcome("cancelled", 900, 3, 150.0f, "PLA", 200, 55);
    auto event = tm.get_queue_snapshot()[0];

    REQUIRE(event.contains("phases_completed"));
    REQUIRE(event["phases_completed"] == 3);

    REQUIRE(event.contains("nozzle_temp"));
    REQUIRE(event["nozzle_temp"] == 200);

    REQUIRE(event.contains("bed_temp"));
    REQUIRE(event["bed_temp"] == 55);
}

// ============================================================================
// Enable/Disable Toggle [telemetry][toggle]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Toggle: disabled by default", "[telemetry][toggle]") {
    auto& tm = TelemetryManager::instance();
    // Fixture sets disabled; verify the API reports that
    REQUIRE_FALSE(tm.is_enabled());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Toggle: when disabled, record_session does not add events",
                 "[telemetry][toggle]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);

    tm.record_session();
    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Toggle: when disabled, record_print_outcome does not add events",
                 "[telemetry][toggle]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);

    tm.record_print_outcome("completed", 3600, 10, 2500.0f, "PLA", 215, 60);
    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Toggle: when enabled, events are added normally",
                 "[telemetry][toggle]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    REQUIRE(tm.is_enabled());

    tm.record_session();
    REQUIRE(tm.queue_size() == 1);

    tm.record_print_outcome("completed", 1000, 5, 500.0f, "PLA", 200, 60);
    REQUIRE(tm.queue_size() == 2);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Toggle: mid-session toggle respects new state",
                 "[telemetry][toggle]") {
    auto& tm = TelemetryManager::instance();

    // Start enabled
    tm.set_enabled(true);
    tm.record_session();
    REQUIRE(tm.queue_size() == 1);

    // Disable mid-session
    tm.set_enabled(false);
    tm.record_session();
    REQUIRE(tm.queue_size() == 1); // No new event added

    // Re-enable
    tm.set_enabled(true);
    tm.record_session();
    REQUIRE(tm.queue_size() == 2); // New event added
}

TEST_CASE_METHOD(TelemetryTestFixture, "Toggle: disable does not clear existing queue",
                 "[telemetry][toggle]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_session();
    REQUIRE(tm.queue_size() == 2);

    // Disabling should not erase already-queued events
    tm.set_enabled(false);
    REQUIRE(tm.queue_size() == 2);
}

// ============================================================================
// Queue File Round-Trip (Persistence) [telemetry][persistence]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Persistence: write queue to file and read back",
                 "[telemetry][persistence]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_print_outcome("completed", 1200, 8, 1000.0f, "PLA", 210, 60);
    REQUIRE(tm.queue_size() == 2);

    auto snapshot_before = tm.get_queue_snapshot();

    // Save to disk
    tm.save_queue();

    // Clear in-memory queue
    tm.clear_queue();
    REQUIRE(tm.queue_size() == 0);

    // Load from disk
    tm.load_queue();
    REQUIRE(tm.queue_size() == 2);

    auto snapshot_after = tm.get_queue_snapshot();
    REQUIRE(snapshot_before == snapshot_after);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Persistence: empty queue produces empty JSON array",
                 "[telemetry][persistence]") {
    auto& tm = TelemetryManager::instance();

    // Save empty queue
    tm.save_queue();

    // Load it back
    tm.load_queue();
    REQUIRE(tm.queue_size() == 0);

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.is_array());
    REQUIRE(snapshot.empty());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Persistence: load from missing file produces empty queue",
                 "[telemetry][persistence]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Add an event so queue is non-empty
    tm.record_session();
    REQUIRE(tm.queue_size() == 1);

    // Load from a config dir that has no queue file - should reset to empty
    // (Re-init with a fresh empty dir to simulate missing file)
    auto empty_dir = fs::temp_directory_path() /
                     ("helix_telemetry_empty_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(empty_dir);

    tm.shutdown();
    tm.init(empty_dir.string());
    tm.load_queue();

    REQUIRE(tm.queue_size() == 0);

    // Cleanup
    std::error_code ec;
    fs::remove_all(empty_dir, ec);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Persistence: load from corrupt file produces empty queue (graceful degradation)",
                 "[telemetry][persistence]") {
    auto& tm = TelemetryManager::instance();

    // Write corrupt data to the queue file location
    write_file("telemetry_queue.json", "{{not valid json at all!!!");

    // Loading should not crash and should produce empty queue
    tm.load_queue();
    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Persistence: queue survives multiple write/read cycles",
                 "[telemetry][persistence]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Cycle 1: add events and save
    tm.record_session();
    tm.save_queue();

    // Cycle 2: load, add more, save again
    tm.clear_queue();
    tm.load_queue();
    REQUIRE(tm.queue_size() == 1);

    tm.record_print_outcome("completed", 600, 5, 300.0f, "PETG", 230, 70);
    tm.save_queue();

    // Cycle 3: load and verify everything persisted
    tm.clear_queue();
    tm.load_queue();
    REQUIRE(tm.queue_size() == 2);

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot[0]["event"] == "session");
    REQUIRE(snapshot[1]["event"] == "print_outcome");
}

TEST_CASE_METHOD(TelemetryTestFixture, "Persistence: saved file is valid JSON",
                 "[telemetry][persistence]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_print_outcome("failed", 120, 2, 50.0f, "PLA", 200, 60);
    tm.save_queue();

    // Find and read the queue file
    // The file should be in the temp config directory
    bool found_valid_json = false;
    for (auto& entry : fs::recursive_directory_iterator(temp_dir_)) {
        if (entry.path().extension() == ".json") {
            std::ifstream ifs(entry.path());
            std::string content{std::istreambuf_iterator<char>(ifs),
                                std::istreambuf_iterator<char>()};

            // Should parse without throwing
            auto parsed = json::parse(content, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_array()) {
                found_valid_json = true;
                REQUIRE(parsed.size() == 2);
            }
        }
    }
    REQUIRE(found_valid_json);
}

// ============================================================================
// kMaxQueueSize constant [telemetry][queue]
// ============================================================================

TEST_CASE("MAX_QUEUE_SIZE is 100", "[telemetry][queue]") {
    REQUIRE(TelemetryManager::kMaxQueueSize == 100);
}

// ============================================================================
// Drain capacity and retry spacing [telemetry][send][1476]
// ============================================================================

TEST_CASE("Drain: one send window can empty a full queue", "[telemetry][send][1476]") {
    // The periodic producers enqueue faster than a single batch per window
    // drains, so a window that cannot clear a full queue leaves it saturated
    // and enqueue_event() discards the oldest events unsent.
    REQUIRE(TelemetryManager::kMaxBatchesPerSend * TelemetryManager::kMaxBatchSize >=
            TelemetryManager::kMaxQueueSize);
}

TEST_CASE("Drain: the hourly producer alone outruns a single batch per window",
          "[telemetry][send][1476]") {
    // record_memory_snapshot("hourly") fires from the auto-send timer, so a
    // day enqueues one event per hour with nobody touching the device. A drain
    // capped at one batch per kSendInterval is below that rate on its own.
    constexpr int hourly_events_per_send_interval =
        static_cast<int>(TelemetryManager::kSendInterval.count());
    REQUIRE(hourly_events_per_send_interval > static_cast<int>(TelemetryManager::kMaxBatchSize));

    // The drain has to cover that rate, which is what the batch bound buys.
    REQUIRE(static_cast<int>(TelemetryManager::kMaxBatchesPerSend *
                             TelemetryManager::kMaxBatchSize) > hourly_events_per_send_interval);
}

TEST_CASE("Queue budget: an unattended send window fits the queue", "[telemetry][send][1476]") {
    // With nobody touching the device the periodic timers are the whole
    // producer set, and the queue is drained once per kSendInterval. A budget
    // above kMaxQueueSize is the number of events enqueue_event() discards
    // unsent every day — on every platform, not only the K1-class ones where
    // the drop log was read.
    REQUIRE(TelemetryManager::kUnattendedEventsPerSendWindow <= TelemetryManager::kMaxQueueSize);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: a day of unattended production survives to the send",
                 "[telemetry][send][1476]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // The hourly memory snapshot is one of the producers the budget counts, so
    // enqueueing the whole budget through it reproduces a day's worth of
    // unattended enqueue pressure without waiting on the timers.
    for (size_t i = 0; i < TelemetryManager::kUnattendedEventsPerSendWindow; ++i) {
        tm.record_memory_snapshot("hourly");
    }

    REQUIRE(tm.queue_size() == TelemetryManager::kUnattendedEventsPerSendWindow);
    REQUIRE(TelemetryManagerTestAccess::events_dropped_since_send(tm) == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: a discarded event is counted, not silent",
                 "[telemetry][send][1476]") {
    // A queue at kMaxQueueSize looks the same whether it just filled or has
    // been shedding events for hours, so the count is the only thing that can
    // tell the next batch's window is incomplete.
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    for (size_t i = 0; i < TelemetryManager::kMaxQueueSize; ++i) {
        tm.record_session();
    }
    REQUIRE(TelemetryManagerTestAccess::events_dropped_since_send(tm) == 0);

    tm.record_session();
    REQUIRE(tm.queue_size() == TelemetryManager::kMaxQueueSize);
    REQUIRE(TelemetryManagerTestAccess::events_dropped_since_send(tm) == 1);

    // The count covers the span since the last batch reached the server, so a
    // batch that lands starts a fresh window rather than carrying the old
    // shortfall into every send that follows.
    tm.remove_sent_events(TelemetryManager::kMaxBatchSize);
    REQUIRE(TelemetryManagerTestAccess::events_dropped_since_send(tm) == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: a purge ends the window the drop count describes",
                 "[telemetry][send][1476]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    for (size_t i = 0; i < TelemetryManager::kMaxQueueSize + 1; ++i) {
        tm.record_session();
    }
    REQUIRE(TelemetryManagerTestAccess::events_dropped_since_send(tm) == 1);

    // clear_queue() is the opt-out purge. Nothing it discards belongs to a
    // batch anyone will send, so the shortfall it leaves is not one to report
    // against the first batch of the next opt-in.
    tm.clear_queue();
    REQUIRE(tm.queue_size() == 0);
    REQUIRE(TelemetryManagerTestAccess::events_dropped_since_send(tm) == 0);
}

TEST_CASE("Send delay: a healthy sender waits the full send interval", "[telemetry][send][1476]") {
    REQUIRE(TelemetryManager::next_attempt_delay(1) == TelemetryManager::kSendInterval);
}

TEST_CASE("Send delay: a first failure retries sooner than the daily cadence",
          "[telemetry][send][1476]") {
    // A backoff above 1 means the previous attempt failed. Spacing that first
    // retry off the full send interval would leave the queue filling for a day
    // over a server that was unreachable for a moment.
    REQUIRE(TelemetryManager::next_attempt_delay(2) < TelemetryManager::kSendInterval);
}

TEST_CASE("Send delay: a sustained failure backs off past the daily cadence",
          "[telemetry][send][1476]") {
    // The counterweight to the test above. A printer that never reaches the
    // endpoint at all must not end up attempting MORE often than a healthy one
    // — each attempt is a thread, a DNS lookup and a 30s timeout, and on a
    // LAN-only machine every one of them is waste that never succeeds.
    REQUIRE(TelemetryManager::next_attempt_delay(TelemetryManager::kMaxBackoffMultiplier) >=
            TelemetryManager::kSendInterval);
}

TEST_CASE("Send delay: retries lengthen as the backoff grows", "[telemetry][send][1476]") {
    REQUIRE(TelemetryManager::next_attempt_delay(2) < TelemetryManager::next_attempt_delay(4));
    REQUIRE(TelemetryManager::next_attempt_delay(4) < TelemetryManager::next_attempt_delay(64));
}

TEST_CASE("Send delay: the backoff ceiling still reaches seven days", "[telemetry][send][1476]") {
    // kRetryInterval is the base, so the multiplier has to be able to climb far
    // enough that the ceiling is still a week rather than a few hours.
    REQUIRE(TelemetryManager::next_attempt_delay(TelemetryManager::kMaxBackoffMultiplier) ==
            std::chrono::hours{24 * 7});
}

TEST_CASE("Send delay: never exceeds seven days", "[telemetry][send][1476]") {
    REQUIRE(TelemetryManager::next_attempt_delay(100000) <= std::chrono::hours{24 * 7});
}

// try_send() has to actually consult next_attempt_delay(). Pinning the helper
// alone leaves the call site free to keep its own spacing rule, so these drive
// the gate: it stamps last_send_time_ when it passes and leaves it when it
// skips, which is the only outward sign of the decision.

TEST_CASE_METHOD(TelemetryTestFixture, "Send window: a backed-off sender retries within hours",
                 "[telemetry][send][1476]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);
    tm.record_session();
    REQUIRE(tm.queue_size() > 0); // an empty queue returns before the gate

    // The previous attempt failed (backoff 2) three hours ago. Spaced off
    // kRetryInterval that is a 2h wait, so this attempt is due.
    const auto three_hours_ago = std::chrono::steady_clock::now() - std::chrono::hours{3};
    TelemetryManagerTestAccess::set_last_send_time(tm, three_hours_ago);
    TelemetryManagerTestAccess::set_backoff(tm, 2);

    tm.try_send();

    REQUIRE(TelemetryManagerTestAccess::last_send_time(tm) > three_hours_ago);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Send window: a healthy sender still waits the full day",
                 "[telemetry][send][1476]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);
    tm.record_session();
    REQUIRE(tm.queue_size() > 0);

    // Backoff 1 means the last send succeeded, so the 24h cadence applies and
    // three hours is not yet due. Without this the shorter retry spacing could
    // widen to every send and quietly become an hourly upload.
    // steady_clock counts from boot, so a stamp older than the box is a
    // negative time point and try_send() reads it as never sent. Age the stamp
    // three hours, or just inside uptime, whichever is shorter: both sit
    // inside the 24 h cadence this case is about.
    const auto now = std::chrono::steady_clock::now();
    const auto age = std::min<std::chrono::steady_clock::duration>(
        std::chrono::hours{3}, now.time_since_epoch() - std::chrono::seconds{1});
    REQUIRE(age.count() > 0);
    const auto earlier = now - age;
    TelemetryManagerTestAccess::set_last_send_time(tm, earlier);
    TelemetryManagerTestAccess::set_backoff(tm, 1);

    tm.try_send();

    REQUIRE(TelemetryManagerTestAccess::last_send_time(tm) == earlier);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Send window: the gate reads the elapsed time, not the clock's sign",
                 "[telemetry][send][1476]") {
    // steady_clock's epoch is boot, so on a machine up less than the stamp's
    // age a perfectly ordinary "sent a while ago" time point is negative.
    // Deciding "never sent" from that sign would let the gate pass on every
    // try_send() a freshly booted device makes, which is the opposite of the
    // spacing the window exists to keep. Half a send interval is under the
    // healthy cadence on any uptime, and is negative on a young clock, so this
    // exercises the sentinel wherever it is exercisable.
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);
    tm.record_session();
    REQUIRE(tm.queue_size() > 0);

    const auto half_a_window =
        std::chrono::steady_clock::now() - (TelemetryManager::kSendInterval / 2);
    TelemetryManagerTestAccess::set_last_send_time(tm, half_a_window);
    TelemetryManagerTestAccess::set_backoff(tm, 1);

    tm.try_send();

    REQUIRE(TelemetryManagerTestAccess::last_send_time(tm) == half_a_window);
}

// ============================================================================
// Singleton behavior [telemetry]
// ============================================================================

TEST_CASE("TelemetryManager: instance returns same object", "[telemetry]") {
    auto& inst1 = TelemetryManager::instance();
    auto& inst2 = TelemetryManager::instance();

    REQUIRE(&inst1 == &inst2);
}

// ============================================================================
// Device ID consistency across events [telemetry][hash]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Device ID: consistent across session and print outcome events",
                 "[telemetry][hash]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_print_outcome("completed", 600, 5, 300.0f, "PLA", 200, 60);

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 2);

    // Both events should use the same anonymized device ID
    std::string id1 = snapshot[0]["device_id"];
    std::string id2 = snapshot[1]["device_id"];
    REQUIRE(id1 == id2);

    // And the device ID should be a hash (64 hex chars), not a UUID
    REQUIRE(id1.size() == 64);
    std::regex hex_regex("^[0-9a-f]{64}$");
    REQUIRE(std::regex_match(id1, hex_regex));
}

// ============================================================================
// Transmission [telemetry][transmission]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Transmission: try_send is no-op when disabled",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();

    // Enqueue an event while enabled, then disable
    tm.set_enabled(true);
    tm.record_session();
    REQUIRE(tm.queue_size() == 1);

    tm.set_enabled(false);

    // try_send should not crash and should not remove events
    tm.try_send();
    REQUIRE(tm.queue_size() == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Transmission: try_send is no-op when queue is empty",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    REQUIRE(tm.queue_size() == 0);

    // try_send with empty queue should not crash
    tm.try_send();
    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Transmission: build_batch takes at most MAX_BATCH_SIZE events",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Fill queue with more events than kMaxBatchSize
    for (size_t i = 0; i < TelemetryManager::kMaxBatchSize + 10; ++i) {
        tm.record_session();
    }
    REQUIRE(tm.queue_size() == TelemetryManager::kMaxBatchSize + 10);

    // Build a batch and verify it respects the limit
    auto batch = tm.build_batch();
    REQUIRE(batch.is_array());
    REQUIRE(batch.size() == TelemetryManager::kMaxBatchSize);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Transmission: build_batch returns all events when fewer than limit",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_print_outcome("completed", 600, 5, 300.0f, "PLA", 200, 60);
    REQUIRE(tm.queue_size() == 2);

    auto batch = tm.build_batch();
    REQUIRE(batch.is_array());
    REQUIRE(batch.size() == 2);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Transmission: build_batch returns empty array when queue is empty",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();

    REQUIRE(tm.queue_size() == 0);
    auto batch = tm.build_batch();
    REQUIRE(batch.is_array());
    REQUIRE(batch.empty());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Transmission: build_batch does not modify the queue",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_session();
    REQUIRE(tm.queue_size() == 2);

    auto batch = tm.build_batch();
    REQUIRE(batch.size() == 2);

    // Queue should be unchanged after building a batch
    REQUIRE(tm.queue_size() == 2);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Transmission: batch payload contains valid event JSON",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_print_outcome("completed", 3600, 10, 2500.0f, "PLA", 215, 60);

    auto batch = tm.build_batch();
    REQUIRE(batch.size() == 2);

    // Each event in the batch should have the required schema fields
    for (const auto& event : batch) {
        REQUIRE(event.contains("schema_version"));
        REQUIRE(event.contains("event"));
        REQUIRE(event.contains("device_id"));
        REQUIRE(event.contains("timestamp"));
    }

    // Verify event types are preserved in order
    REQUIRE(batch[0]["event"] == "session");
    REQUIRE(batch[1]["event"] == "print_outcome");
}

TEST_CASE("Transmission: endpoint URL is HTTPS", "[telemetry][transmission]") {
    // The kMaxBatchSize / kSendInterval assertions that used to sit here copied
    // the header's literals and compared them to themselves — a constant cannot
    // fail to equal its own definition. The scheme check is the one thing here
    // that constrains the value rather than mirroring it: device telemetry must
    // never leave over plaintext.
    const std::string url(TelemetryManager::kEndpointUrl);
    REQUIRE(url.find("https://") == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Transmission: remove_sent_events removes from front of queue",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Add 5 events
    tm.record_session();
    tm.record_session();
    tm.record_print_outcome("completed", 600, 5, 300.0f, "PLA", 200, 60);
    tm.record_session();
    tm.record_session();
    REQUIRE(tm.queue_size() == 5);

    // Remove the first 3
    tm.remove_sent_events(3);
    REQUIRE(tm.queue_size() == 2);

    // The remaining events should be the last two (both session events)
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot[0]["event"] == "session");
    REQUIRE(snapshot[1]["event"] == "session");
}

TEST_CASE_METHOD(TelemetryTestFixture, "Transmission: remove_sent_events with 0 does nothing",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    REQUIRE(tm.queue_size() == 1);

    tm.remove_sent_events(0);
    REQUIRE(tm.queue_size() == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Transmission: remove_sent_events with count > queue size removes all",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_session();
    REQUIRE(tm.queue_size() == 2);

    // Removing more than exists should safely clear the queue
    tm.remove_sent_events(100);
    REQUIRE(tm.queue_size() == 0);
}

// ============================================================================
// Auto-send Scheduler [telemetry][scheduler]
// ============================================================================

/**
 * @brief TelemetryTestFixture plus a live LVGL, for the auto-send timers
 *
 * The scheduler cases used to run on the bare TelemetryTestFixture and assert
 * nothing, which hid a second problem: that fixture's ctor ends with
 * set_enabled(false), so start_auto_send() returned at the !is_enabled() guard
 * (src/system/telemetry_manager.cpp:1015) and never reached lv_timer_create()
 * at all. The idempotence branch (:1023) and stop_auto_send() were unreachable.
 * Enabling telemetry first makes the timers real, and real timers need LVGL up.
 *
 * The LVGL base is constructed first and destroyed last, so the composed
 * telemetry fixture's init/shutdown always runs against a live LVGL.
 */
class TelemetrySchedulerFixture : public LVGLTestFixture {
  public:
    ~TelemetrySchedulerFixture() override {
        TelemetryManager::instance().stop_auto_send();
    }

    /// Count the LVGL timers this TelemetryManager owns.
    ///
    /// Every telemetry timer passes `this` as its lv_timer user_data
    /// (auto-send, snapshot, frame-perf, settings-debounce),
    /// so keying on that ignores whatever else the suite has left armed and
    /// keeps the count independent of how many timers start_auto_send()
    /// happens to create.
    static int telemetry_timer_count() {
        void* self = &TelemetryManager::instance();
        int n = 0;
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
            if (lv_timer_get_user_data(t) == self) {
                n++;
            }
        }
        return n;
    }

  protected:
    [[nodiscard]] fs::path temp_dir() const {
        return telemetry_.temp_dir();
    }

  private:
    TelemetryTestFixture telemetry_;
};

TEST_CASE_METHOD(TelemetrySchedulerFixture,
                 "Scheduler: start_auto_send arms timers only when telemetry is enabled",
                 "[telemetry][scheduler]") {
    auto& tm = TelemetryManager::instance();
    REQUIRE(telemetry_timer_count() == 0);

    // Disabled: the !is_enabled() guard must arm nothing. Sending telemetry
    // from a user who opted out is the failure this pins.
    tm.set_enabled(false);
    tm.start_auto_send();
    REQUIRE(telemetry_timer_count() == 0);

    // Enabled: timers appear. (set_enabled(true) can arm them itself once
    // discovery is complete, which the start_auto_send() above just latched.)
    tm.set_enabled(true);
    tm.start_auto_send();
    const int armed = telemetry_timer_count();
    REQUIRE(armed > 0);

    // Idempotent: the `if (auto_send_timer_)` early return must not arm a
    // duplicate that stop_auto_send() would then leak.
    tm.start_auto_send();
    REQUIRE(telemetry_timer_count() == armed);

    tm.stop_auto_send();
    REQUIRE(telemetry_timer_count() == 0);
}

TEST_CASE_METHOD(TelemetrySchedulerFixture, "Scheduler: stop_auto_send is idempotent",
                 "[telemetry][scheduler]") {
    auto& tm = TelemetryManager::instance();

    // Stop before any start.
    tm.stop_auto_send();
    REQUIRE(telemetry_timer_count() == 0);

    tm.set_enabled(true);
    tm.start_auto_send();
    REQUIRE(telemetry_timer_count() > 0);

    tm.stop_auto_send();
    REQUIRE(telemetry_timer_count() == 0);

    // Second stop must see null pointers, not freed timers.
    tm.stop_auto_send();
    REQUIRE(telemetry_timer_count() == 0);
}

TEST_CASE_METHOD(TelemetrySchedulerFixture, "Scheduler: shutdown stops auto-send",
                 "[telemetry][scheduler]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.start_auto_send();
    REQUIRE(telemetry_timer_count() > 0);

    tm.shutdown();
    REQUIRE(telemetry_timer_count() == 0);

    // Re-init for fixture cleanup
    tm.init(temp_dir().string());
}

// ============================================================================
// Schema Version 2 - Hardware Survey [telemetry][session][v2]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Session event v2: schema_version is the current version",
                 "[telemetry][session][v2]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    REQUIRE(event["schema_version"] == TelemetryManager::kSchemaVersion);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event v2: app section has display backend fields",
                 "[telemetry][session][v2]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    // App section should exist and have version/platform
    REQUIRE(event.contains("app"));
    REQUIRE(event["app"].contains("version"));
    REQUIRE(event["app"].contains("platform"));

    // Display backend fields are booleans when DisplayManager is available
    // In test context, DisplayManager may not be initialized, so just verify
    // the app object itself is present and well-formed
    REQUIRE(event["app"].is_object());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event v2: no PII in printer/features/host sections",
                 "[telemetry][session][v2]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    // Serialize entire event to verify no PII leaked
    std::string event_str = event.dump();

    // Must NOT contain any PII-identifying fields at any level
    REQUIRE(event_str.find("\"hostname\"") == std::string::npos);
    REQUIRE(event_str.find("\"ip\"") == std::string::npos);
    REQUIRE(event_str.find("\"mac_address\"") == std::string::npos);
    REQUIRE(event_str.find("\"username\"") == std::string::npos);
    REQUIRE(event_str.find("\"serial_number\"") == std::string::npos);
    REQUIRE(event_str.find("\"email\"") == std::string::npos);
    REQUIRE(event_str.find("\"ssid\"") == std::string::npos);

    // If printer section exists, verify no hostname
    if (event.contains("printer")) {
        REQUIRE_FALSE(event["printer"].contains("hostname"));
    }

    // If host section exists, it should only have os
    if (event.contains("host")) {
        REQUIRE_FALSE(event["host"].contains("hostname"));
        REQUIRE_FALSE(event["host"].contains("ip"));
    }
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event v2: features is an array when present",
                 "[telemetry][session][v2]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    // A test printer may report no features at all, so the contract is
    // conditional: absent, or an array whose every element is a string. Stated
    // as one assertion it holds on both paths - an `if` around the REQUIREs
    // checks nothing whenever the key is missing.
    const bool features_well_formed =
        !event.contains("features") ||
        (event["features"].is_array() &&
         std::all_of(event["features"].begin(), event["features"].end(),
                     [](const nlohmann::json& f) { return f.is_string(); }));
    REQUIRE(features_well_formed);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event v2: app has theme and locale",
                 "[telemetry][session][v2]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    REQUIRE(event.contains("app"));
    const auto& app = event["app"];

    // Theme should be "dark" or "light"
    REQUIRE(app.contains("theme"));
    REQUIRE(app["theme"].is_string());
    std::string theme = app["theme"].get<std::string>();
    REQUIRE((theme == "dark" || theme == "light"));

    // Locale should be a non-empty language code
    REQUIRE(app.contains("locale"));
    REQUIRE(app["locale"].is_string());
    REQUIRE(!app["locale"].get<std::string>().empty());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event v2: host section has hardware info",
                 "[telemetry][session][v2]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    // Host section should always be present (doesn't require printer)
    REQUIRE(event.contains("host"));
    const auto& host = event["host"];
    REQUIRE(host.is_object());

    // Architecture should be present on any platform
    REQUIRE(host.contains("arch"));
    REQUIRE(host["arch"].is_string());
    REQUIRE(!host["arch"].get<std::string>().empty());

    // Verify no PII leakage in host section
    REQUIRE_FALSE(host.contains("hostname"));
    REQUIRE_FALSE(host.contains("ip"));
}

// ============================================================================
// Print Outcome - Filament Metadata [telemetry][print_outcome]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Print outcome event includes filament_type when set",
                 "[telemetry][print_outcome]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_print_outcome("success", 3600, 10, 1234.5f, "PLA", 210, 60);

    auto batch = tm.build_batch();
    REQUIRE(batch.size() == 1);
    REQUIRE(batch[0]["filament_type"] == "PLA");
    REQUIRE(batch[0]["filament_used_mm"].get<float>() == Catch::Approx(1234.5f));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Print outcome event has empty filament_type by default",
                 "[telemetry][print_outcome]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_print_outcome("success", 3600, 10, 0.0f, "", 210, 60);

    auto batch = tm.build_batch();
    REQUIRE(batch.size() == 1);
    REQUIRE(batch[0]["filament_type"] == "");
    REQUIRE(batch[0]["filament_used_mm"].get<float>() == Catch::Approx(0.0f));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Print outcome event: preserves filament type across material types",
                 "[telemetry][print_outcome]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Test various filament types including multi-tool separator
    std::vector<std::pair<std::string, float>> cases = {
        {"PLA", 1500.0f},
        {"PETG", 2200.5f},
        {"ABS;PLA", 3100.0f},
    };

    for (const auto& [ftype, fmm] : cases) {
        tm.clear_queue();
        tm.record_print_outcome("success", 600, 5, fmm, ftype, 200, 60);

        auto batch = tm.build_batch();
        REQUIRE(batch.size() == 1);
        INFO("Testing filament_type: " << ftype);
        REQUIRE(batch[0]["filament_type"] == ftype);
        REQUIRE(batch[0]["filament_used_mm"].get<float>() == Catch::Approx(fmm));
    }
}

// ============================================================================
// Update Failed Event [telemetry][update]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Update failed event: has required envelope fields",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("download_failed", "0.14.0", "ad5m");

    REQUIRE(tm.queue_size() == 1);
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    REQUIRE(event["schema_version"] == TelemetryManager::kSchemaVersion);
    REQUIRE(event["event"] == "update_failed");
    REQUIRE(event.contains("device_id"));
    REQUIRE(event.contains("timestamp"));
    REQUIRE(event["reason"] == "download_failed");
    REQUIRE(event["version"] == "0.14.0");
    REQUIRE(event["platform"] == "ad5m");
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update failed event: includes optional fields when provided",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("corrupt_download", "0.14.0", "pi", 200, 1048576);

    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    REQUIRE(event["reason"] == "corrupt_download");
    REQUIRE(event["http_code"] == 200);
    REQUIRE(event["file_size"] == 1048576);
    REQUIRE_FALSE(event.contains("exit_code"));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update failed event: includes exit_code for install failures",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("install_failed", "0.14.0", "ad5m", -1, -1, 127);

    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    REQUIRE(event["reason"] == "install_failed");
    REQUIRE(event["exit_code"] == 127);
    REQUIRE_FALSE(event.contains("http_code"));
    REQUIRE_FALSE(event.contains("file_size"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Update failed event: carries the failure detail",
                 "[telemetry][update][1813]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    TelemetryManager::UpdateFailureDetail download;
    download.transfer_error = "tls_cert";
    download.transfer_code = 20;
    download.host = "releases.helixscreen.org";
    tm.record_update_failure("download_failed", "1.0.6", "k1", -1, -1, -1, download);

    TelemetryManager::UpdateFailureDetail install;
    install.install_detail = "[ERROR] Cannot write <path>";
    tm.record_update_failure("install_failed", "1.0.6", "k1", -1, -1, 1, install);

    tm.record_update_failure("download_failed", "1.0.6", "k1");

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 3);
    CHECK(snapshot[0]["transfer_error"] == "tls_cert");
    CHECK(snapshot[0]["transfer_code"] == 20);
    CHECK(snapshot[0]["host"] == "releases.helixscreen.org");
    CHECK_FALSE(snapshot[0].contains("install_detail"));

    CHECK(snapshot[1]["install_detail"] == "[ERROR] Cannot write <path>");
    CHECK(snapshot[1]["exit_code"] == 1);
    CHECK_FALSE(snapshot[1].contains("transfer_error"));
    CHECK_FALSE(snapshot[1].contains("host"));

    for (const char* field : {"transfer_error", "transfer_code", "host", "install_detail"})
        CHECK_FALSE(snapshot[2].contains(field));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Update failed event: not recorded when telemetry disabled",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);

    tm.record_update_failure("download_failed", "0.14.0", "pi");

    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Update failed event: from_version included when available",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("download_failed", "0.14.0", "pi");

    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    // from_version should be current HELIX_VERSION
    REQUIRE(event.contains("from_version"));
}

// ============================================================================
// Update Success Event [telemetry][update]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update success: check_previous_update enqueues event from flag file",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Write a flag file simulating a successful update
    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());

    tm.check_previous_update();

    REQUIRE(tm.queue_size() == 1);
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    REQUIRE(event["schema_version"] == TelemetryManager::kSchemaVersion);
    REQUIRE(event["event"] == "update_success");
    REQUIRE(event["version"] == "0.14.0");
    REQUIRE(event["from_version"] == "0.13.4");
    REQUIRE(event["platform"] == "pi");
    REQUIRE(event.contains("device_id"));
    REQUIRE(event.contains("timestamp"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Update success: flag file deleted after reading",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());

    tm.check_previous_update();

    REQUIRE_FALSE(fs::exists(temp_dir() / "update_success.json"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Update success: no-op when no flag file exists",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.check_previous_update();

    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Update success: discarded when telemetry disabled",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);

    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());

    tm.check_previous_update();

    REQUIRE(tm.queue_size() == 0);
    // Flag file should still be removed even if telemetry is disabled
    REQUIRE_FALSE(fs::exists(temp_dir() / "update_success.json"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Update success: malformed flag file handled gracefully",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    write_file("update_success.json", "not valid json {{{{");

    tm.check_previous_update();

    REQUIRE(tm.queue_size() == 0);
    // Malformed file should still be cleaned up
    REQUIRE_FALSE(fs::exists(temp_dir() / "update_success.json"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Write update success flag: creates valid JSON file",
                 "[telemetry][update]") {
    TelemetryManager::write_update_success_flag(temp_dir().string(), "0.14.0", "0.13.4", "pi");

    REQUIRE(fs::exists(temp_dir() / "update_success.json"));
    auto content = read_file("update_success.json");
    auto flag = json::parse(content);

    REQUIRE(flag["version"] == "0.14.0");
    REQUIRE(flag["from_version"] == "0.13.4");
    REQUIRE(flag["platform"] == "pi");
    REQUIRE(flag.contains("timestamp"));
}

// ============================================================================
// Post-Update Crash Suppression [telemetry][update][crash]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Post-update crash: crash.txt preserved when update_success.json present",
                 "[telemetry][update][crash]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Simulate: update succeeded, but process crashed before _exit(0)
    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());
    write_file("crash.txt", "signal:6\nname:SIGABRT\nversion:0.13.4\n");

    tm.check_previous_crash();

    // crash.txt should still exist (Application rotates it via consume_crash_file)
    REQUIRE(fs::exists(temp_dir() / "crash.txt"));
    // No crash event should be enqueued
    REQUIRE(tm.queue_size() == 0);
    // Flag should be set for Application to check
    REQUIRE(tm.had_update_restart());
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Post-update crash: update_success.json preserved for check_previous_update",
                 "[telemetry][update][crash]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());
    write_file("crash.txt", "signal:6\nname:SIGABRT\nversion:0.13.4\n");

    tm.check_previous_crash();

    // update_success.json should NOT be deleted by check_previous_crash —
    // it is consumed later by check_previous_update
    REQUIRE(fs::exists(temp_dir() / "update_success.json"));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Normal crash: crash event enqueued when no update_success.json",
                 "[telemetry][update][crash]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Only crash.txt, no update flag — this is a real crash
    write_file("crash.txt", "signal:11\nname:SIGSEGV\nversion:0.14.0\n");

    tm.check_previous_crash();

    // Crash event should be enqueued
    REQUIRE(tm.queue_size() == 1);
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot[0]["event"] == "crash");
    // Flag should NOT be set
    REQUIRE_FALSE(tm.had_update_restart());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Crash event: app_board is the resolved board",
                 "[telemetry][crash]") {
    helix::test::ScopedBoard board(helix::board::Board::Creator5Pro);
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    write_file("crash.txt", "signal:11\nname:SIGSEGV\nversion:0.14.0\n");

    tm.check_previous_crash();

    REQUIRE(tm.queue_size() == 1);
    auto event = tm.get_queue_snapshot()[0];
    CHECK(event["app_board"] == "creator5-pro");
    CHECK(event["app_platform"] == helix::platform::current_key());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Post-update crash: had_update_restart is false by default",
                 "[telemetry][update][crash]") {
    auto& tm = TelemetryManager::instance();
    REQUIRE_FALSE(tm.had_update_restart());
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Clean update: no crash flag when update_success.json exists without crash.txt",
                 "[telemetry][update][crash]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Happy path: _exit(0) worked, no crash.txt written
    json flag;
    flag["version"] = "0.14.0";
    flag["from_version"] = "0.13.4";
    flag["platform"] = "pi";
    flag["timestamp"] = "2026-02-26T12:00:00Z";
    write_file("update_success.json", flag.dump());

    tm.check_previous_crash();

    // No crash file means had_update_restart stays false
    REQUIRE_FALSE(tm.had_update_restart());
    REQUIRE(tm.queue_size() == 0);
}

// ============================================================================
// Bundle-Consent Crash Reporting [telemetry][crash][consent]
//
// When a user explicitly consents to send a crash *bundle* (the crash report
// modal's "Send" action), that consent also covers a lightweight telemetry
// crash event — even if the telemetry opt-in is OFF. This keeps fleet-wide
// crash rates visible. enqueue_crash_event_unconditional() implements the
// opt-in bypass; the modal calls it after a successful bundle upload.
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Bundle consent: crash event enqueued even when telemetry disabled",
                 "[telemetry][crash][consent]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false); // opt-in OFF

    write_file("crash.txt", "signal:11\nname:SIGSEGV\nversion:0.14.0\n");

    // Baseline: the normal opt-in path discards the crash when disabled.
    tm.check_previous_crash();
    REQUIRE(tm.queue_size() == 0);

    // Bundle consent bypasses the opt-in gate and enqueues the crash event.
    REQUIRE(tm.enqueue_crash_event_unconditional());
    REQUIRE(tm.queue_size() == 1);
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot[0]["event"] == "crash");
    REQUIRE(snapshot[0]["signal_name"] == "SIGSEGV");
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Bundle consent: no double-enqueue when telemetry already captured the crash",
                 "[telemetry][crash][consent]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true); // opt-in ON — boot path already enqueues

    write_file("crash.txt", "signal:11\nname:SIGSEGV\nversion:0.14.0\n");

    tm.check_previous_crash();
    REQUIRE(tm.queue_size() == 1);

    // Consent enqueue must not add a second copy of the same crash.
    REQUIRE_FALSE(tm.enqueue_crash_event_unconditional());
    REQUIRE(tm.queue_size() == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Bundle consent: no event when no crash file is present",
                 "[telemetry][crash][consent]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);

    // No crash.txt written.
    REQUIRE_FALSE(tm.enqueue_crash_event_unconditional());
    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Bundle consent: suppressed for post-update restart crash",
                 "[telemetry][crash][consent]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);

    // A crash alongside update_success.json is the expected post-update restart,
    // not a real crash — it must not be reported even with consent.
    write_file("update_success.json", "{\"version\":\"0.14.0\"}");
    write_file("crash.txt", "signal:6\nname:SIGABRT\nversion:0.13.4\n");

    REQUIRE_FALSE(tm.enqueue_crash_event_unconditional());
    REQUIRE(tm.queue_size() == 0);
}

// ============================================================================
// Memory Snapshot Event [telemetry][memory]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "record_memory_snapshot creates valid event",
                 "[telemetry][memory]") {
    TelemetryManager::instance().set_enabled(true);
    TelemetryManager::instance().record_memory_snapshot("session_start");

    REQUIRE(TelemetryManager::instance().queue_size() == 1);

    auto snapshot = TelemetryManager::instance().get_queue_snapshot();
    auto& event = snapshot[0];

    CHECK(event["event"] == "memory_snapshot");
    CHECK(event["schema_version"] == TelemetryManager::kSchemaVersion);
    CHECK(event.contains("device_id"));
    CHECK(event.contains("timestamp"));
    CHECK(event["trigger"] == "session_start");
    CHECK(event.contains("uptime_sec"));
    CHECK(event.contains("rss_kb"));
    CHECK(event.contains("vm_size_kb"));
    CHECK(event.contains("vm_data_kb"));
    CHECK(event.contains("vm_swap_kb"));
    CHECK(event.contains("vm_peak_kb"));
    CHECK(event.contains("vm_hwm_kb"));

    // uptime should be non-negative
    CHECK(event["uptime_sec"].get<int>() >= 0);
}

// ============================================================================
// Hardware Profile Event [telemetry][hardware]
// ============================================================================

TEST_CASE("classify_moonraker_locality distinguishes local, remote and unknown",
          "[telemetry][hardware]") {
    using TM = TelemetryManager;

    SECTION("a loopback websocket URL is local") {
        CHECK(TM::classify_moonraker_locality("ws://localhost:7125/websocket") == true);
        CHECK(TM::classify_moonraker_locality("ws://127.0.0.1:7125/websocket") == true);
        CHECK(TM::classify_moonraker_locality("ws://[::1]:7125/websocket") == true);
    }

    SECTION("a routable host is remote") {
        CHECK(TM::classify_moonraker_locality("ws://192.168.1.100:7125/websocket") == false);
        CHECK(TM::classify_moonraker_locality("ws://printer.local:7125/websocket") == false);
        CHECK(TM::classify_moonraker_locality("wss://klipper.lan/websocket") == false);
    }

    SECTION("no URL is unknown, not remote") {
        // The mock client never records a URL, and a client that has not
        // connected yet has none either. Reporting those as "remote" would
        // invent data; the field must be omitted so the aggregate can tell
        // "not measured" apart from a real answer.
        CHECK_FALSE(TM::classify_moonraker_locality("").has_value());
    }
}

namespace {

/// Mock client that reports a caller-supplied websocket URL.
///
/// MoonrakerClientMock::connect() does record the URL, so these cases could be
/// driven through it. Overriding the accessor instead keeps them synchronous:
/// the mock's connect() sleeps to simulate a realistic handshake and starts the
/// temperature simulation, neither of which this test wants three times over.
class StubUrlClient : public MoonrakerClientMock {
  public:
    explicit StubUrlClient(std::string url) : url_(std::move(url)) {}
    const std::string& get_last_url() const override {
        return url_;
    }

  private:
    std::string url_;
};

/// Record a hardware_profile with `websocket_url` in place and return the event.
///
/// Uses the shared PrinterState rather than a fresh one: a default-constructed
/// PrinterState whose init_subjects() never ran corrupts memory on destruction
/// (see the DEFERRED note in test_helix_macro_manager.cpp).
json hardware_profile_with_url(const std::string& websocket_url) {
    StubUrlClient client(websocket_url);
    MoonrakerAPI api(client, get_printer_state());
    ScopedMoonrakerApi installed(&api);

    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.clear_queue();
    tm.record_hardware_profile();

    for (const auto& event : tm.get_queue_snapshot()) {
        if (event.value("event", "") == "hardware_profile") {
            return event;
        }
    }
    return json();
}

} // namespace

TEST_CASE_METHOD(TelemetryTestFixture,
                 "hardware_profile reports Moonraker topology from the websocket URL",
                 "[telemetry][hardware]") {
    SECTION("a loopback URL is recorded as local") {
        auto event = hardware_profile_with_url("ws://localhost:7125/websocket");
        REQUIRE(event.contains("moonraker_is_local"));
        CHECK(event["moonraker_is_local"] == true);
    }

    SECTION("a routable URL is recorded as remote") {
        auto event = hardware_profile_with_url("ws://192.168.1.50:7125/websocket");
        REQUIRE(event.contains("moonraker_is_local"));
        CHECK(event["moonraker_is_local"] == false);
    }

    SECTION("an API with no URL yet omits the field") {
        // The API is present and consulted here -- only the URL is missing. The
        // field has to be absent rather than present-and-false, so the
        // aggregate can tell "not measured" from a measured "remote".
        auto event = hardware_profile_with_url("");
        REQUIRE_FALSE(event.is_null());
        CHECK_FALSE(event.contains("moonraker_is_local"));
    }
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "hardware_profile omits moonraker_is_local when there is no Moonraker API",
                 "[telemetry][hardware]") {
    // Distinct from the empty-URL case above: here get_moonraker_api() itself
    // returns null, which is the state before the client has been wired up.
    REQUIRE(get_moonraker_api() == nullptr);

    TelemetryManager::instance().set_enabled(true);
    TelemetryManager::instance().clear_queue();
    TelemetryManager::instance().record_hardware_profile();

    auto snapshot = TelemetryManager::instance().get_queue_snapshot();
    REQUIRE_FALSE(snapshot.empty());

    for (const auto& event : snapshot) {
        if (event.value("event", "") == "hardware_profile") {
            CHECK_FALSE(event.contains("moonraker_is_local"));
        }
    }
}

TEST_CASE_METHOD(TelemetryTestFixture, "record_hardware_profile creates valid event",
                 "[telemetry][hardware]") {
    TelemetryManager::instance().set_enabled(true);
    TelemetryManager::instance().record_hardware_profile();

    REQUIRE(TelemetryManager::instance().queue_size() == 1);

    auto snapshot = TelemetryManager::instance().get_queue_snapshot();
    auto& event = snapshot[0];

    CHECK(event["event"] == "hardware_profile");
    CHECK(event["schema_version"] == TelemetryManager::kSchemaVersion);
    CHECK(event.contains("device_id"));
    CHECK(event.contains("timestamp"));
    // Hardware profile may have empty sections in test mode (no printer connected)
    // but the event itself should be valid
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "session and hardware_profile extruder counts use the strict extruder grammar",
                 "[telemetry][extruder_count][1502]") {
    // Both events count tools, and a tool count is exactly the question
    // is_extruder_name() answers: an extruder-prefixed heater that is not a
    // numbered extruder (extruder_mixing, extruder_stepper) is not a tool and
    // must not inflate the count.
    MoonrakerClientMock client;
    client.set_heaters({"extruder", "extruder1", "extruder2", "extruder_mixing", "extruder_stepper",
                        "heater_bed"});
    ScopedMoonrakerClient installed(&client);

    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.clear_queue();
    tm.record_session();
    tm.record_hardware_profile();

    bool saw_session = false;
    bool saw_hw_profile = false;
    for (const auto& event : tm.get_queue_snapshot()) {
        if (event.value("event", "") == "session") {
            saw_session = true;
            CHECK(event.at("printer").at("extruder_count") == 3);
        } else if (event.value("event", "") == "hardware_profile") {
            saw_hw_profile = true;
            CHECK(event.at("extruders").at("count") == 3);
        }
    }
    REQUIRE(saw_session);
    REQUIRE(saw_hw_profile);
}

// ============================================================================
// Settings Snapshot Event [telemetry][settings]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "record_settings_snapshot creates valid event",
                 "[telemetry][settings]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.clear_queue();
    tm.record_settings_snapshot();

    // Find our settings_snapshot event in the queue. Background threads (e.g., event loop
    // health timer) may enqueue error_encountered events between clear and snapshot.
    auto snapshot = tm.get_queue_snapshot();
    auto it = std::find_if(snapshot.begin(), snapshot.end(), [](const json& e) {
        return e.value("event", "") == "settings_snapshot";
    });
    REQUIRE(it != snapshot.end());

    auto& event = *it;

    CHECK(event["event"] == "settings_snapshot");
    CHECK(event["schema_version"] == TelemetryManager::kSchemaVersion);
    CHECK(event.contains("device_id"));
    CHECK(event.contains("timestamp"));
    // Settings should have at least theme and locale
    CHECK(event.contains("theme"));
    CHECK(event.contains("locale"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "record_settings_snapshot carries the adoption flags",
                 "[telemetry][settings]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.clear_queue();
    tm.record_settings_snapshot();

    auto snapshot = tm.get_queue_snapshot();
    auto it = std::find_if(snapshot.begin(), snapshot.end(), [](const json& e) {
        return e.value("event", "") == "settings_snapshot";
    });
    REQUIRE(it != snapshot.end());

    for (const char* flag : {"spoolman_configured", "theme_changed", "multi_printer"}) {
        INFO("flag: " << flag);
        REQUIRE(it->contains(flag));
        CHECK(it->at(flag).is_boolean());
    }
}

namespace {
json settings_snapshot_now() {
    auto& tm = TelemetryManager::instance();
    tm.clear_queue();
    tm.record_settings_snapshot();
    for (const auto& e : tm.get_queue_snapshot()) {
        if (e.value("event", "") == "settings_snapshot") {
            return e;
        }
    }
    return json::object();
}
} // namespace

namespace {
/// theme_manager_apply_theme() is a no-op until the theme runtime has a
/// display, so a theme test brings up LVGL and the theme itself.
class TelemetryThemeFixture : public LVGLUITestFixture {
  protected:
    TelemetryTestFixture telemetry_;
};
} // namespace

TEST_CASE_METHOD(TelemetryThemeFixture, "settings_snapshot theme_changed follows the active theme",
                 "[telemetry][settings]") {
    TelemetryManager::instance().set_enabled(true);

    const helix::ThemeData saved_theme = theme_manager_get_active_theme();
    const bool saved_dark = theme_manager_is_dark_mode();

    helix::ThemeData fallback = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(fallback, saved_dark);
    REQUIRE(theme_manager_get_active_theme().filename == fallback.filename);
    auto event = settings_snapshot_now();
    REQUIRE(event.contains("theme_changed"));
    CHECK(event["theme_changed"] == false);

    helix::ThemeData onedark = helix::load_theme_from_file("onedark");
    REQUIRE(onedark.is_valid());
    theme_manager_apply_theme(onedark, saved_dark);
    REQUIRE(theme_manager_get_active_theme().filename == onedark.filename);
    event = settings_snapshot_now();
    REQUIRE(event.contains("theme_changed"));
    CHECK(event["theme_changed"] == true);

    theme_manager_apply_theme(saved_theme, saved_dark);
}

TEST_CASE_METHOD(TelemetryTestFixture, "settings_snapshot multi_printer follows the printer count",
                 "[telemetry][settings]") {
    TelemetryManager::instance().set_enabled(true);
    auto* cfg = helix::Config::get_instance();
    auto& data = helix::ConfigTestAccess::data(*cfg);
    const json saved_printers = data.contains("printers") ? data["printers"] : json();

    data["printers"] = json::object({{"one", json::object()}});
    auto event = settings_snapshot_now();
    REQUIRE(event.contains("multi_printer"));
    CHECK(event["multi_printer"] == false);

    data["printers"]["two"] = json::object();
    event = settings_snapshot_now();
    REQUIRE(event.contains("multi_printer"));
    CHECK(event["multi_printer"] == true);

    if (saved_printers.is_null()) {
        data.erase("printers");
    } else {
        data["printers"] = saved_printers;
    }
}

TEST_CASE_METHOD(TelemetryTestFixture, "settings_snapshot spoolman_configured follows availability",
                 "[telemetry][settings]") {
    lv_init_safe();
    TelemetryManager::instance().set_enabled(true);
    auto& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    auto& queue = helix::ui::UpdateQueue::instance();

    state.capabilities_state().set_spoolman_available(true);
    helix::ui::UpdateQueueTestAccess::drain_all(queue);
    auto event = settings_snapshot_now();
    REQUIRE(event.contains("spoolman_configured"));
    CHECK(event["spoolman_configured"] == true);

    state.capabilities_state().set_spoolman_available(false);
    helix::ui::UpdateQueueTestAccess::drain_all(queue);
    event = settings_snapshot_now();
    REQUIRE(event.contains("spoolman_configured"));
    CHECK(event["spoolman_configured"] == false);
}

// ============================================================================
// Overlay ids [telemetry][overlay_id]
// ============================================================================

TEST_CASE("overlay_telemetry_id prefers the root's object name", "[telemetry][overlay_id]") {
    CHECK(helix::overlay_telemetry_id("fan_control_overlay", "Fan Control") ==
          "fan_control_overlay");
    CHECK(helix::overlay_telemetry_id("fan_control_overlay", nullptr) == "fan_control_overlay");
}

TEST_CASE("overlay_telemetry_id falls back to the lifecycle name", "[telemetry][overlay_id]") {
    CHECK(helix::overlay_telemetry_id(nullptr, "Fan Control") == "Fan Control");
    CHECK(helix::overlay_telemetry_id("", "Fan Control") == "Fan Control");
}

TEST_CASE("overlay_telemetry_id is 'unnamed' when nothing names the overlay",
          "[telemetry][overlay_id]") {
    CHECK(helix::overlay_telemetry_id("", nullptr) == "unnamed");
    CHECK(helix::overlay_telemetry_id(nullptr, nullptr) == "unnamed");
    CHECK(helix::overlay_telemetry_id(nullptr, "") == "unnamed");
}

// ============================================================================
// Panel Usage Event [telemetry][panel_usage]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "record_panel_usage creates valid event",
                 "[telemetry][panel_usage]") {
    TelemetryManager::instance().set_enabled(true);

    // Simulate panel navigation
    TelemetryManager::instance().notify_panel_changed("home");
    TelemetryManager::instance().notify_panel_changed("controls");
    TelemetryManager::instance().notify_panel_changed("settings");
    TelemetryManager::instance().notify_overlay_opened("temp_graph");
    TelemetryManager::instance().notify_overlay_opened("wifi_settings");
    TelemetryManager::instance().notify_overlay_opened("temp_graph");

    TelemetryManager::instance().record_panel_usage();

    REQUIRE(TelemetryManager::instance().queue_size() == 1);

    auto snapshot = TelemetryManager::instance().get_queue_snapshot();
    auto& event = snapshot[0];

    CHECK(event["event"] == "panel_usage");
    CHECK(event.contains("session_duration_sec"));
    CHECK(event.contains("panel_time_sec"));
    CHECK(event.contains("panel_visits"));
    CHECK(event["overlay_open_count"] == 3);

    // Check panel visits are tracked
    auto& visits = event["panel_visits"];
    CHECK(visits["home"] == 1);
    CHECK(visits["controls"] == 1);
    CHECK(visits["settings"] == 1);

    // Check per-overlay visits are tracked
    REQUIRE(event.contains("overlay_visits"));
    auto& overlay_visits = event["overlay_visits"];
    CHECK(overlay_visits["temp_graph"] == 2);
    CHECK(overlay_visits["wifi_settings"] == 1);
}

// ============================================================================
// Connection Stability Event [telemetry][connection]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "record_connection_stability creates valid event",
                 "[telemetry][connection]") {
    TelemetryManager::instance().set_enabled(true);

    // Simulate connection lifecycle
    TelemetryManager::instance().notify_connection_state_changed(2); // connected
    TelemetryManager::instance().notify_connection_state_changed(0); // disconnected
    TelemetryManager::instance().notify_connection_state_changed(2); // reconnected
    TelemetryManager::instance().notify_klippy_state_changed(3);     // klippy error

    TelemetryManager::instance().record_connection_stability();

    REQUIRE(TelemetryManager::instance().queue_size() == 1);

    auto snapshot = TelemetryManager::instance().get_queue_snapshot();
    auto& event = snapshot[0];

    CHECK(event["event"] == "connection_stability");
    CHECK(event.contains("session_duration_sec"));
    CHECK(event["connect_count"] == 2);
    CHECK(event["disconnect_count"] == 1);
    CHECK(event.contains("total_connected_sec"));
    CHECK(event.contains("total_disconnected_sec"));
    CHECK(event.contains("longest_disconnect_sec"));
    CHECK(event["klippy_error_count"] == 1);
    CHECK(event["klippy_shutdown_count"] == 0);
}

// ============================================================================
// Print Start Context Event [telemetry][print_start]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "record_print_start_context creates valid event",
                 "[telemetry][print_start]") {
    TelemetryManager::instance().set_enabled(true);
    TelemetryManager::instance().record_print_start_context("local", true, 5 * 1024 * 1024, 7200,
                                                            "PrusaSlicer", 1, false);

    REQUIRE(TelemetryManager::instance().queue_size() == 1);

    auto snapshot = TelemetryManager::instance().get_queue_snapshot();
    auto& event = snapshot[0];

    CHECK(event["event"] == "print_start_context");
    CHECK(event["source"] == "local");
    CHECK(event["has_thumbnail"] == true);
    CHECK(event["file_size_bucket"] == "1-10MB");
    CHECK(event["estimated_duration_bucket"] == "1-4hr");
    CHECK(event["slicer"] == "PrusaSlicer");
    CHECK(event["tool_count_used"] == 1);
    CHECK(event["ams_active"] == false);
}

TEST_CASE_METHOD(TelemetryTestFixture, "print_start_context file size bucketing",
                 "[telemetry][print_start]") {
    TelemetryManager::instance().set_enabled(true);

    // Test various file size buckets
    TelemetryManager::instance().record_print_start_context("local", false, 500 * 1024, 300, "Cura",
                                                            1, false); // <1MB, <30min

    auto snapshot = TelemetryManager::instance().get_queue_snapshot();
    auto& event = snapshot[0];
    CHECK(event["file_size_bucket"] == "<1MB");
    CHECK(event["estimated_duration_bucket"] == "<30min");
}

// ============================================================================
// Error Encountered Event [telemetry][error]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "record_error creates valid event", "[telemetry][error]") {
    TelemetryManager::instance().set_enabled(true);
    TelemetryManager::instance().record_error("moonraker_api", "timeout", "get_printer_objects");

    REQUIRE(TelemetryManager::instance().queue_size() == 1);

    auto snapshot = TelemetryManager::instance().get_queue_snapshot();
    auto& event = snapshot[0];

    CHECK(event["event"] == "error_encountered");
    CHECK(event["category"] == "moonraker_api");
    CHECK(event["code"] == "timeout");
    CHECK(event["context"] == "get_printer_objects");
    CHECK(event.contains("uptime_sec"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "record_error rate limits by category",
                 "[telemetry][error]") {
    TelemetryManager::instance().set_enabled(true);

    // First error should be recorded
    TelemetryManager::instance().record_error("moonraker_api", "timeout", "ctx1");
    CHECK(TelemetryManager::instance().queue_size() == 1);

    // Second error in same category should be rate-limited
    TelemetryManager::instance().record_error("moonraker_api", "timeout", "ctx2");
    CHECK(TelemetryManager::instance().queue_size() == 1); // Still 1

    // Different category should NOT be rate-limited
    TelemetryManager::instance().record_error("websocket", "connection_refused", "reconnect");
    CHECK(TelemetryManager::instance().queue_size() == 2);
}

// ============================================================================
// New Events Disabled Behavior [telemetry][disabled]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "new events are no-op when disabled",
                 "[telemetry][disabled]") {
    // Telemetry disabled by default
    CHECK_FALSE(TelemetryManager::instance().is_enabled());

    TelemetryManager::instance().record_memory_snapshot("session_start");
    TelemetryManager::instance().record_hardware_profile();
    TelemetryManager::instance().record_settings_snapshot();
    TelemetryManager::instance().record_panel_usage();
    TelemetryManager::instance().record_connection_stability();
    TelemetryManager::instance().record_print_start_context("local", true, 1000, 60, "Slicer", 1,
                                                            false);
    TelemetryManager::instance().record_error("moonraker_api", "timeout", "get_objects");

    CHECK(TelemetryManager::instance().queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "record_error accepts any identifier-shaped category",
                 "[telemetry][error]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_error("ui", "main_loop_hang", "ctx");
    REQUIRE(tm.queue_size() == 1);
    CHECK(tm.get_queue_snapshot()[0]["category"] == "ui");

    // Digits and underscores are part of the alphabet; 32 characters is the cap.
    tm.record_error("a_1", "code", "ctx");
    tm.record_error(std::string(32, 'x'), "code", "ctx");
    CHECK(tm.queue_size() == 3);
}

TEST_CASE_METHOD(TelemetryTestFixture, "record_error rejects categories outside [a-z0-9_]{1,32}",
                 "[telemetry][error]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    for (const std::string& bad :
         {std::string(""), std::string("Bad Category"), std::string("UPPER"),
          std::string("dash-ed"), std::string("dot.ted"), std::string("slash/ed"),
          std::string(33, 'x'), std::string("caf\xc3\xa9"), std::string("new\nline")}) {
        INFO("category: " << bad);
        tm.record_error(bad, "code", "ctx");
        CHECK(tm.queue_size() == 0);
    }

    // The rejects above did not consume any category's rate-limit slot.
    tm.record_error("moonraker_api", "timeout", "ctx");
    CHECK(tm.queue_size() == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture, "record_error rate limits each new category separately",
                 "[telemetry][error]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_error("ui", "main_loop_hang", "a");
    tm.record_error("ui", "main_loop_hang", "b");
    CHECK(tm.queue_size() == 1);
    tm.record_error("display_driver", "init_failed", "c");
    CHECK(tm.queue_size() == 2);
}

// ============================================================================
// PII Absence Tests for New Event Types [telemetry][pii]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "New events do not leak PII", "[telemetry]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Record each new event type
    tm.record_memory_snapshot("session_start");
    tm.record_hardware_profile();
    tm.record_settings_snapshot();
    tm.record_print_start_context("local", true, 5000000, 3600, "PrusaSlicer", 1, false);
    tm.record_error("moonraker_api", "timeout", "get_printer_objects");

    // Panel usage requires panel navigation first
    tm.notify_panel_changed("home");
    tm.record_panel_usage();

    // Connection stability requires connection state changes first
    tm.notify_connection_state_changed(2);
    tm.record_connection_stability();

    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 7);

    // Get the test machine's hostname for checking
    char hostname_buf[256] = {};
    gethostname(hostname_buf, sizeof(hostname_buf));
    std::string machine_hostname(hostname_buf);

    // IP address regex pattern
    std::regex ip_regex("\\d+\\.\\d+\\.\\d+\\.\\d+");

    for (size_t i = 0; i < snapshot.size(); i++) {
        auto& event = snapshot[i];
        std::string event_str = event.dump();
        std::string event_type = event.value("event", "unknown");

        INFO("Checking event: " << event_type << " (index " << i << ")");

        // Must NOT contain the hostname of the test machine, matched as a whole
        // token: a hostname like "vm" is also a substring of field names such as
        // "vm_size_kb", and a key is not a leak. A hostname leaking as a value,
        // a path segment or a URL host is still surrounded by non-word bytes.
        if (!machine_hostname.empty()) {
            std::string escaped;
            for (char c : machine_hostname) {
                if (!std::isalnum(static_cast<unsigned char>(c))) {
                    escaped += '\\';
                }
                escaped += c;
            }
            std::regex host_regex("(^|[^A-Za-z0-9_])" + escaped + "($|[^A-Za-z0-9_])");
            REQUIRE_FALSE(std::regex_search(event_str, host_regex));
        }

        // Must NOT contain IP address patterns
        REQUIRE_FALSE(std::regex_search(event_str, ip_regex));

        // Must NOT contain serial number references
        REQUIRE(event_str.find("\"serial\"") == std::string::npos);

        // Must NOT contain file paths
        REQUIRE(event_str.find("/home/") == std::string::npos);
        REQUIRE(event_str.find("/tmp/") == std::string::npos);

        // Must NOT contain username-like fields
        REQUIRE(event_str.find("\"root\"") == std::string::npos);
        REQUIRE(event_str.find("\"username\"") == std::string::npos);
    }
}

// ============================================================================
// Frame Time Sampling [telemetry][frame]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: samples are recorded in ring buffer",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    tm.record_frame_time(8000);  // 8ms
    tm.record_frame_time(12000); // 12ms
    tm.record_frame_time(16000); // 16ms

    tm.record_performance_snapshot();
    REQUIRE(tm.queue_size() == 1);

    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["event"] == "performance_snapshot");
    REQUIRE(event.contains("frame_time_p50_ms"));
    REQUIRE(event.contains("frame_time_p95_ms"));
    REQUIRE(event.contains("frame_time_p99_ms"));
    REQUIRE(event.contains("dropped_frame_count"));
    REQUIRE(event.contains("total_frame_count"));
    REQUIRE(event["total_frame_count"] == 3);
    REQUIRE(event["dropped_frame_count"] == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: dropped frames detected above 33ms threshold",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("temperature");
    tm.record_frame_time(10000); // 10ms - ok
    tm.record_frame_time(35000); // 35ms - dropped
    tm.record_frame_time(50000); // 50ms - dropped

    tm.record_performance_snapshot();
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["dropped_frame_count"] == 2);
    REQUIRE(event["total_frame_count"] == 3);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: drop rate is dropped over total, exactly",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    constexpr int N = 40;
    constexpr int D = 10;
    for (int i = 0; i < N; ++i) {
        tm.record_frame_time(i < D ? 40000 : 8000);
    }

    tm.record_performance_snapshot();
    auto event = tm.get_queue_snapshot()[0];
    CHECK(event["dropped_frame_count"] == D);
    CHECK(event["total_frame_count"] == N);
    CHECK(event["dropped_frame_count"].get<double>() / event["total_frame_count"].get<double>() ==
          Catch::Approx(0.25));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: total shares the dropped count's window",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Older than the ring: all dropped, and overwritten by the second batch.
    tm.notify_panel_changed("status");
    const size_t ring = TelemetryManager::kFrameRingSize;
    for (size_t i = 0; i < ring; ++i) {
        tm.record_frame_time(40000);
    }
    // The ring's retained window: a quarter dropped.
    for (size_t i = 0; i < ring; ++i) {
        tm.record_frame_time(i % 4 == 0 ? 40000 : 8000);
    }

    tm.record_performance_snapshot();
    auto event = tm.get_queue_snapshot()[0];
    CHECK(event["total_frame_count"] == static_cast<int64_t>(ring));
    CHECK(event["dropped_frame_count"] == static_cast<int64_t>(ring / 4));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: the event carries no task_handler_max_ms",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    tm.record_frame_time(8000);
    tm.record_performance_snapshot();
    CHECK_FALSE(tm.get_queue_snapshot()[0].contains("task_handler_max_ms"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: worst panel is identified by p95",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    for (int i = 0; i < 20; ++i) {
        tm.record_frame_time(8000);
    }

    tm.notify_panel_changed("temperature");
    for (int i = 0; i < 20; ++i) {
        tm.record_frame_time(25000);
    }

    tm.record_performance_snapshot();
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["worst_panel"] == "temperature");
}

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: idle frames below threshold are not recorded",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    tm.record_frame_time(100);  // 0.1ms - idle, should be filtered
    tm.record_frame_time(300);  // 0.3ms - idle, should be filtered
    tm.record_frame_time(8000); // 8ms - real render, should be recorded

    tm.record_performance_snapshot();
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["total_frame_count"] == 1); // Only the 8ms frame
}

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: frames at idle threshold boundary are recorded",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    tm.record_frame_time(499); // Just below 500us threshold - filtered
    tm.record_frame_time(500); // Exactly at threshold - recorded
    tm.record_frame_time(501); // Just above threshold - recorded

    tm.record_performance_snapshot();
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["total_frame_count"] == 2); // 500 and 501 only
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Frame time: snapshot with no active frames produces no event",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Record only idle frames (below threshold) — ring buffer stays empty
    tm.notify_panel_changed("status");
    tm.record_frame_time(100);
    tm.record_frame_time(200);

    tm.record_performance_snapshot();
    // No active frames → no event queued
    REQUIRE(tm.queue_size() == 0);
}

// ============================================================================
// Periodic Snapshots [telemetry][snapshot]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot: panel_usage includes snapshot_seq and is_shutdown fields",
                 "[telemetry][snapshot]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    tm.record_panel_usage();

    REQUIRE(tm.queue_size() == 1);
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["event"] == "panel_usage");
    REQUIRE(event.contains("snapshot_seq"));
    REQUIRE(event.contains("is_shutdown"));
    REQUIRE(event["is_shutdown"] == false);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Snapshot: snapshot_seq increments on each periodic fire",
                 "[telemetry][snapshot]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");

    tm.fire_periodic_snapshot();
    tm.fire_periodic_snapshot();

    // Each fire produces panel_usage + connection_stability + performance_snapshot = 3 events
    REQUIRE(tm.queue_size() >= 4);
    auto queue = tm.get_queue_snapshot();

    int first_seq = -1, second_seq = -1;
    for (const auto& ev : queue) {
        if (ev.value("event", "") == "panel_usage") {
            if (first_seq < 0)
                first_seq = ev.value("snapshot_seq", -1);
            else
                second_seq = ev.value("snapshot_seq", -1);
        }
    }
    REQUIRE(first_seq == 0);
    REQUIRE(second_seq == 1);
}

// ============================================================================
// Delta snapshots [telemetry][snapshot][delta]
// ============================================================================

namespace {

using namespace std::chrono_literals;

/// Queued events named @p kind, oldest first.
std::vector<json> queued_events(const TelemetryManager& tm, const std::string& kind) {
    std::vector<json> out;
    for (const auto& e : tm.get_queue_snapshot()) {
        if (e.value("event", "") == kind) {
            out.push_back(e);
        }
    }
    return out;
}

int sum_field(const std::vector<json>& events, const char* field) {
    int total = 0;
    for (const auto& e : events) {
        total += e.value(field, 0);
    }
    return total;
}

int sum_map_entry(const std::vector<json>& events, const char* map, const std::string& key) {
    int total = 0;
    for (const auto& e : events) {
        if (e.contains(map) && e[map].contains(key)) {
            total += e[map][key].get<int>();
        }
    }
    return total;
}

const char* kAccumulatorFile = "telemetry_snapshot.json";

} // namespace

TEST_CASE_METHOD(TelemetryTestFixture, "Snapshot deltas: three windows sum to the true totals",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_connection_state_changed(2); // t=0 connected
    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    tm.notify_panel_changed("controls"); // t=1h
    tm.notify_overlay_opened("temp_graph");
    tm.notify_widget_interaction("led_control:2");
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    tm.fire_periodic_snapshot(); // t=2h, seq 0

    TelemetryManagerTestAccess::advance_clock(tm, 30min);
    tm.notify_connection_state_changed(0); // t=2h30 outage starts
    TelemetryManagerTestAccess::advance_clock(tm, 10min);
    tm.notify_connection_state_changed(2); // t=2h40 outage ends
    tm.notify_panel_changed("home");
    tm.notify_overlay_opened("temp_graph");
    tm.fire_periodic_snapshot(); // t=2h40, seq 1

    TelemetryManagerTestAccess::advance_clock(tm, 2h);
    tm.notify_klippy_state_changed(3);
    tm.fire_periodic_snapshot(); // t=4h40, seq 2

    auto panels = queued_events(tm, "panel_usage");
    auto conns = queued_events(tm, "connection_stability");
    REQUIRE(panels.size() == 3);
    REQUIRE(conns.size() == 3);

    // Each window covers its own span and nothing before it.
    const int window_sec[] = {7200, 2400, 7200};
    for (int i = 0; i < 3; ++i) {
        INFO("window " << i);
        CHECK(panels[i]["snapshot_seq"] == i);
        CHECK(conns[i]["snapshot_seq"] == i);
        CHECK(panels[i]["session_duration_sec"] == window_sec[i]);
        CHECK(conns[i]["session_duration_sec"] == window_sec[i]);
    }

    CHECK(sum_field(panels, "session_duration_sec") == 16800);
    CHECK(sum_map_entry(panels, "panel_time_sec", "home") == 3600 + 7200);
    CHECK(sum_map_entry(panels, "panel_time_sec", "controls") == 6000);
    CHECK(sum_map_entry(panels, "panel_visits", "home") == 2);
    CHECK(sum_map_entry(panels, "panel_visits", "controls") == 1);
    CHECK(sum_map_entry(panels, "overlay_visits", "temp_graph") == 2);
    CHECK(sum_field(panels, "overlay_open_count") == 2);
    CHECK(sum_map_entry(panels, "widget_interactions", "led_control") == 1);

    CHECK(sum_field(conns, "connect_count") == 2);
    CHECK(sum_field(conns, "disconnect_count") == 1);
    CHECK(sum_field(conns, "total_connected_sec") == 16800 - 600);
    CHECK(sum_field(conns, "total_disconnected_sec") == 600);
    CHECK(sum_field(conns, "klippy_error_count") == 1);
    CHECK(conns[1]["longest_disconnect_sec"] == 600);
    CHECK(conns[2]["longest_disconnect_sec"] == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: a window's sub-second remainder carries into the next",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.notify_connection_state_changed(2);
    tm.notify_panel_changed("home");

    for (int i = 0; i < 4; ++i) {
        TelemetryManagerTestAccess::advance_clock(tm, 1500ms);
        tm.fire_periodic_snapshot();
    }

    auto panels = queued_events(tm, "panel_usage");
    auto conns = queued_events(tm, "connection_stability");
    REQUIRE(panels.size() == 4);
    REQUIRE(conns.size() == 4);
    CHECK(sum_field(panels, "session_duration_sec") == 6);
    CHECK(sum_map_entry(panels, "panel_time_sec", "home") == 6);
    CHECK(sum_field(conns, "session_duration_sec") == 6);
    CHECK(sum_field(conns, "total_connected_sec") == 6);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: an outage spanning a snapshot is measured whole, once",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_connection_state_changed(2);
    TelemetryManagerTestAccess::advance_clock(tm, 3h + 50min);
    tm.notify_connection_state_changed(0); // t=3h50
    TelemetryManagerTestAccess::advance_clock(tm, 10min);
    tm.fire_periodic_snapshot(); // t=4h, outage open
    TelemetryManagerTestAccess::advance_clock(tm, 20min);
    tm.notify_connection_state_changed(2); // t=4h20, outage closes after 1800s
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    tm.fire_periodic_snapshot();

    auto conns = queued_events(tm, "connection_stability");
    REQUIRE(conns.size() == 2);

    CHECK(conns[0]["longest_disconnect_sec"].get<int>() >= 600);
    CHECK(conns[0]["total_disconnected_sec"] == 600);
    CHECK(conns[1]["longest_disconnect_sec"] == 1800);
    CHECK(conns[1]["total_disconnected_sec"] == 1200);
    CHECK(sum_field(conns, "disconnect_count") == 1);
    CHECK(sum_field(conns, "total_disconnected_sec") == 1800);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: an unclean exit's persisted window is sent under its own boot",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);

    tm.notify_connection_state_changed(2);
    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 4h);
    tm.fire_periodic_snapshot(); // seq 0 covers [0, 4h]

    TelemetryManagerTestAccess::advance_clock(tm, 10min);
    tm.notify_panel_changed("controls");
    tm.notify_overlay_opened("temp_graph");
    TelemetryManagerTestAccess::advance_clock(tm, 30min);
    TelemetryManagerTestAccess::fire_auto_send_tick(tm); // persists [4h, 4h40]

    // Activity after the last persist dies with the power.
    TelemetryManagerTestAccess::advance_clock(tm, 20min);
    tm.notify_panel_changed("home");

    const std::string old_boot = TelemetryManagerTestAccess::boot_id(tm);
    TelemetryManagerTestAccess::simulate_unclean_exit(tm);
    tm.init(temp_dir().string());
    REQUIRE(tm.is_enabled());
    const std::string new_boot = TelemetryManagerTestAccess::boot_id(tm);
    REQUIRE(new_boot != old_boot);

    std::vector<json> recovered_panels;
    for (const auto& e : queued_events(tm, "panel_usage")) {
        if (e["boot_id"] == old_boot && e["snapshot_seq"] == 1) {
            recovered_panels.push_back(e);
        }
    }
    REQUIRE(recovered_panels.size() == 1);
    const auto& panel = recovered_panels[0];
    CHECK(panel["session_duration_sec"] == 2400);
    CHECK(panel["panel_visits"] == json{{"controls", 1}});
    CHECK(panel["panel_time_sec"] == json{{"home", 600}, {"controls", 1800}});
    CHECK(panel["overlay_visits"] == json{{"temp_graph", 1}});
    CHECK(panel["overlay_open_count"] == 1);
    CHECK(panel["is_shutdown"] == false);
    CHECK(panel["schema_version"] == 3);
    CHECK(panel["app_version"] == HELIX_VERSION);

    std::vector<json> recovered_conns;
    for (const auto& e : queued_events(tm, "connection_stability")) {
        if (e["boot_id"] == old_boot && e["snapshot_seq"] == 1) {
            recovered_conns.push_back(e);
        }
    }
    REQUIRE(recovered_conns.size() == 1);
    CHECK(recovered_conns[0]["session_duration_sec"] == 2400);
    CHECK(recovered_conns[0]["total_connected_sec"] == 2400);
    CHECK(recovered_conns[0]["connect_count"] == 0);

    // Recovered once: the file is gone, so a second boot has nothing to resend,
    // and the deltas are already in the saved queue in case one follows.
    CHECK_FALSE(fs::exists(temp_dir() / kAccumulatorFile));
    int saved_recovered = 0;
    for (const auto& e : json::parse(read_file("telemetry_queue.json"))) {
        if (e.value("boot_id", "") == old_boot && e.value("snapshot_seq", -1) == 1) {
            ++saved_recovered;
        }
    }
    CHECK(saved_recovered == 2);

    // The new boot's first delta starts from zero at seq 0.
    tm.clear_queue();
    tm.notify_panel_changed("status");
    TelemetryManagerTestAccess::advance_clock(tm, 5min);
    tm.fire_periodic_snapshot();
    auto fresh = queued_events(tm, "panel_usage");
    REQUIRE(fresh.size() == 1);
    CHECK(fresh[0]["boot_id"] == new_boot);
    CHECK(fresh[0]["snapshot_seq"] == 0);
    CHECK(fresh[0]["panel_visits"] == json{{"status", 1}});
    CHECK(fresh[0]["session_duration_sec"] == 300);
}

namespace {

/// A screensaver usage source attached to the manager for the life of the object.
class ScopedSaverUsage {
  public:
    explicit ScopedSaverUsage(TelemetryManager& tm) : tm_(tm) {
        tm_.set_screensaver_usage(&usage);
    }
    ~ScopedSaverUsage() {
        tm_.set_screensaver_usage(nullptr);
    }
    ScopedSaverUsage(const ScopedSaverUsage&) = delete;
    ScopedSaverUsage& operator=(const ScopedSaverUsage&) = delete;

    helix::SaverUsage usage;

  private:
    TelemetryManager& tm_;
};

} // namespace

TEST_CASE_METHOD(TelemetryTestFixture, "shutdown sends the screensaver window it holds",
                 "[telemetry][screensaver_usage]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    ScopedSaverUsage source(tm);
    const auto t0 = TelemetryManagerTestAccess::now(tm);
    source.usage.begin("aqua", t0);
    source.usage.end(helix::SaverRunEnd::TOUCH, 3, t0 + 45s);
    TelemetryManagerTestAccess::advance_clock(tm, 60s);

    tm.shutdown();

    auto events = queued_events(tm, "screensaver_usage");
    REQUIRE(events.size() == 1);
    CHECK(events[0]["is_shutdown"] == true);
    CHECK(events[0]["saver_shown_sec"] == json{{"aqua", 45}});
    tm.init(temp_dir().string());
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "record_screensaver_usage sends the window's runs per saver and starts a new one",
                 "[telemetry][screensaver_usage]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    ScopedSaverUsage source(tm);
    const auto t0 = TelemetryManagerTestAccess::now(tm);
    source.usage.begin("maze", t0);
    source.usage.end(helix::SaverRunEnd::TOO_HEAVY, 4, t0 + 20s);
    source.usage.begin("pipes", t0 + 20s);
    source.usage.end(helix::SaverRunEnd::TOUCH, 1, t0 + 80s);
    source.usage.begin("pipes", t0 + 100s);
    TelemetryManagerTestAccess::advance_clock(tm, 130s);

    tm.record_screensaver_usage();

    auto events = queued_events(tm, "screensaver_usage");
    REQUIRE(events.size() == 1);
    const json& e = events[0];
    CHECK(e["schema_version"] == 3);
    CHECK(e["snapshot_seq"] == 0);
    CHECK(e["is_shutdown"] == false);
    CHECK(e.contains("boot_id"));
    CHECK(e["saver_shown_sec"] == json{{"maze", 20}, {"pipes", 90}});
    CHECK(e["saver_starts"] == json{{"maze", 1}, {"pipes", 2}});
    CHECK(e["saver_woke_by_touch"] == json{{"pipes", 1}});
    CHECK(e["saver_stopped_other"] == json::object());
    CHECK(e["saver_too_heavy"] == json{{"maze", 1}});
    CHECK(e["saver_settled_levels"] == json{{"maze:4", 1}, {"pipes:1", 1}});

    // The open run carries on into the next window, counted from the reset.
    TelemetryManagerTestAccess::advance_clock(tm, 10s);
    source.usage.end(helix::SaverRunEnd::OTHER, 0, TelemetryManagerTestAccess::now(tm));
    tm.record_screensaver_usage();
    events = queued_events(tm, "screensaver_usage");
    REQUIRE(events.size() == 2);
    CHECK(events[1]["saver_shown_sec"] == json{{"pipes", 10}});
    CHECK(events[1]["saver_starts"] == json::object());
    CHECK(events[1]["saver_stopped_other"] == json{{"pipes", 1}});
    CHECK(events[1]["saver_settled_levels"] == json{{"pipes:0", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "record_screensaver_usage sends nothing for a window with no saver activity",
                 "[telemetry][screensaver_usage]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    SECTION("no source attached") {
        tm.record_screensaver_usage();
        CHECK(queued_events(tm, "screensaver_usage").empty());
    }
    SECTION("a source with nothing in its window") {
        ScopedSaverUsage source(tm);
        tm.record_screensaver_usage();
        CHECK(queued_events(tm, "screensaver_usage").empty());
    }
}

TEST_CASE_METHOD(
    TelemetryTestFixture,
    "record_screensaver_usage sends nothing while telemetry is off and keeps the window",
    "[telemetry][screensaver_usage]") {
    auto& tm = TelemetryManager::instance();
    ScopedSaverUsage source(tm);
    const auto t0 = TelemetryManagerTestAccess::now(tm);
    source.usage.begin("maze", t0);
    source.usage.end(helix::SaverRunEnd::TOUCH, 0, t0 + 5s);

    tm.record_screensaver_usage();
    CHECK(tm.get_queue_snapshot().empty());

    tm.set_enabled(true);
    tm.record_screensaver_usage();
    const auto events = queued_events(tm, "screensaver_usage");
    REQUIRE(events.size() == 1);
    CHECK(events[0]["saver_woke_by_touch"] == json{{"maze", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "screensaver_usage rides the periodic snapshot and an unclean exit's recovery",
                 "[telemetry][screensaver_usage][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);
    ScopedSaverUsage source(tm);
    const auto t0 = TelemetryManagerTestAccess::now(tm);
    source.usage.begin("fireflies", t0);
    source.usage.end(helix::SaverRunEnd::TOUCH, 2, t0 + 30s);
    TelemetryManagerTestAccess::advance_clock(tm, 4h);
    tm.fire_periodic_snapshot();
    auto events = queued_events(tm, "screensaver_usage");
    REQUIRE(events.size() == 1);
    CHECK(events[0]["snapshot_seq"] == 0);
    CHECK(events[0]["saver_shown_sec"] == json{{"fireflies", 30}});

    // Persisted by the hourly tick, then lost with the power and sent on the next boot.
    const auto t1 = TelemetryManagerTestAccess::now(tm);
    source.usage.begin("aqua", t1);
    source.usage.end(helix::SaverRunEnd::OTHER, 1, t1 + 45s);
    TelemetryManagerTestAccess::advance_clock(tm, 1min);
    TelemetryManagerTestAccess::fire_auto_send_tick(tm);
    const std::string old_boot = TelemetryManagerTestAccess::boot_id(tm);
    TelemetryManagerTestAccess::simulate_unclean_exit(tm);
    tm.init(temp_dir().string());

    std::vector<json> recovered;
    for (const auto& e : queued_events(tm, "screensaver_usage")) {
        if (e["boot_id"] == old_boot && e["snapshot_seq"] == 1) {
            recovered.push_back(e);
        }
    }
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0]["saver_shown_sec"] == json{{"aqua", 45}});
    CHECK(recovered[0]["saver_stopped_other"] == json{{"aqua", 1}});
    CHECK(recovered[0]["saver_settled_levels"] == json{{"aqua:1", 1}});
}

#if HELIX_ENABLE_SCREENSAVER
TEST_CASE_METHOD(TelemetryTestFixture,
                 "settings_snapshot names the configured screensaver by its stable name",
                 "[telemetry][settings][screensaver_usage]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    auto& display = DisplaySettingsManager::instance();
    const int saved = display.get_screensaver_type();

    auto snapshot_type = [&]() {
        tm.clear_queue();
        tm.record_settings_snapshot();
        const auto events = queued_events(tm, "settings_snapshot");
        REQUIRE(events.size() == 1);
        return events[0]["screensaver_type"];
    };

    display.set_screensaver_type(static_cast<int>(ScreensaverType::MAZE_3D));
    CHECK(snapshot_type() == "maze");
    display.set_screensaver_type(static_cast<int>(ScreensaverType::OFF));
    CHECK(snapshot_type() == "off");

    display.set_screensaver_type(saved);
}
#endif

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: a window already in the saved queue is not recovered twice",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);

    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    TelemetryManagerTestAccess::fire_auto_send_tick(tm); // accumulator: seq 0, [0, 1h]
    const std::string pre_snapshot = read_file(kAccumulatorFile);
    REQUIRE_FALSE(pre_snapshot.empty());

    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    tm.fire_periodic_snapshot(); // seq 0 queued and saved

    // Power lost after the queue save, before the accumulator file was rewritten.
    write_file(kAccumulatorFile, pre_snapshot);
    const std::string old_boot = TelemetryManagerTestAccess::boot_id(tm);
    TelemetryManagerTestAccess::simulate_unclean_exit(tm);
    tm.init(temp_dir().string());

    int old_seq0 = 0;
    for (const auto& e : queued_events(tm, "panel_usage")) {
        if (e["boot_id"] == old_boot && e["snapshot_seq"] == 0) {
            ++old_seq0;
        }
    }
    CHECK(old_seq0 == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: the accumulator file holds only the unsent window",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);

    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    tm.fire_periodic_snapshot();
    TelemetryManagerTestAccess::advance_clock(tm, 10min);
    tm.notify_panel_changed("controls");
    TelemetryManagerTestAccess::fire_auto_send_tick(tm);

    REQUIRE(fs::exists(temp_dir() / kAccumulatorFile));
    auto state = json::parse(read_file(kAccumulatorFile));
    CHECK(state["boot_id"] == TelemetryManagerTestAccess::boot_id(tm));
    CHECK(state["snapshot_seq"] == 1);
    CHECK(state["app_version"] == HELIX_VERSION);
    REQUIRE(state.contains("panel_usage"));
    CHECK(state["panel_usage"]["session_duration_sec"] == 600);
    CHECK(state["panel_usage"]["panel_visits"] == json{{"controls", 1}});
    CHECK(state["panel_usage"]["panel_time_sec"] == json{{"home", 600}});
    REQUIRE(state.contains("connection_stability"));
    CHECK(state["connection_stability"]["session_duration_sec"] == 600);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: a cumulative file without a boot id is discarded",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    write_file(kAccumulatorFile,
               R"({"snapshot_seq":5,"panel_visits":{"home":900},"connect_count":40})");

    TelemetryManagerTestAccess::simulate_unclean_exit(tm);
    tm.init(temp_dir().string());
    REQUIRE(tm.is_enabled());

    CHECK(queued_events(tm, "panel_usage").empty());
    CHECK_FALSE(fs::exists(temp_dir() / kAccumulatorFile));

    tm.fire_periodic_snapshot();
    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 1);
    CHECK(panels[0]["snapshot_seq"] == 0);
    CHECK_FALSE(panels[0]["panel_visits"].contains("home"));
    auto conns = queued_events(tm, "connection_stability");
    REQUIRE(conns.size() == 1);
    CHECK(conns[0]["connect_count"] == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: fire_periodic_snapshot leaves the new events in the queue file",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.notify_panel_changed("home");

    tm.fire_periodic_snapshot();

    REQUIRE(fs::exists(temp_dir() / "telemetry_queue.json"));
    auto saved = json::parse(read_file("telemetry_queue.json"));
    REQUIRE(saved.is_array());
    std::set<std::string> kinds;
    for (const auto& e : saved) {
        if (e.value("boot_id", "") == TelemetryManagerTestAccess::boot_id(tm) &&
            e.value("snapshot_seq", -1) == 0) {
            kinds.insert(e.value("event", ""));
        }
    }
    CHECK(kinds.count("panel_usage") == 1);
    CHECK(kinds.count("connection_stability") == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Queue: at capacity a delta merges into the queued one of its kind",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session(); // the oldest event, and the one a drop-oldest would take
    tm.notify_connection_state_changed(2);
    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    tm.fire_periodic_snapshot(); // seq 0
    while (tm.queue_size() < TelemetryManager::kMaxQueueSize) {
        tm.record_session();
    }

    tm.notify_panel_changed("controls");
    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    tm.fire_periodic_snapshot(); // seq 1, merges

    CHECK(tm.queue_size() == TelemetryManager::kMaxQueueSize);
    CHECK(tm.get_queue_snapshot()[0]["event"] == "session");

    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 1);
    CHECK(panels[0]["snapshot_seq"] == 1);
    CHECK(panels[0]["session_duration_sec"] == 7200);
    CHECK(panels[0]["panel_visits"]["home"] == 2);
    CHECK(panels[0]["panel_visits"]["controls"] == 1);
    CHECK(panels[0]["panel_time_sec"]["home"] == 7200);

    auto conns = queued_events(tm, "connection_stability");
    REQUIRE(conns.size() == 1);
    CHECK(conns[0]["snapshot_seq"] == 1);
    CHECK(conns[0]["session_duration_sec"] == 7200);
    CHECK(conns[0]["connect_count"] == 1);
    CHECK(conns[0]["total_connected_sec"] == 7200);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Queue: at capacity a non-delta evicts the oldest non-delta, never a delta",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("home");
    tm.record_panel_usage();
    while (tm.queue_size() < TelemetryManager::kMaxQueueSize) {
        tm.record_session();
    }

    tm.record_print_outcome("completed", 600, 10, 1500.0f, "PLA", 210, 60);

    auto queue = tm.get_queue_snapshot();
    REQUIRE(queue.size() == TelemetryManager::kMaxQueueSize);
    CHECK(queue[0]["event"] == "panel_usage");
    CHECK(queue.back()["event"] == "print_outcome");
    CHECK(TelemetryManagerTestAccess::events_dropped_since_send(tm) == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Queue: a merge never touches the batch a send has in flight",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("home");
    tm.record_panel_usage(); // index 0, inside the in-flight batch
    while (tm.queue_size() < TelemetryManager::kMaxQueueSize) {
        tm.record_session();
    }
    TelemetryManagerTestAccess::set_in_flight(tm, TelemetryManager::kMaxBatchSize);

    tm.notify_panel_changed("controls");
    tm.record_panel_usage();

    auto queue = tm.get_queue_snapshot();
    REQUIRE(queue.size() == TelemetryManager::kMaxQueueSize);
    REQUIRE(queue[0]["event"] == "panel_usage");
    CHECK(queue[0]["panel_visits"] == json{{"home", 1}});
    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 2);
    CHECK(panels[1]["panel_visits"] == json{{"controls", 1}});

    // remove_sent_events() ends the send, after which the newest one merges.
    tm.remove_sent_events(TelemetryManager::kMaxBatchSize);
    while (tm.queue_size() < TelemetryManager::kMaxQueueSize) {
        tm.record_session();
    }
    tm.notify_panel_changed("home");
    tm.record_panel_usage();
    panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 1);
    CHECK(panels[0]["panel_visits"]["controls"] == 1);
    CHECK(panels[0]["panel_visits"]["home"] == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: a merge never touches a row a failed send attempted",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("home");
    tm.record_panel_usage(); // index 0, inside the attempted batch
    while (tm.queue_size() < TelemetryManager::kMaxQueueSize) {
        tm.record_session();
    }
    TelemetryManagerTestAccess::fail_send(tm);

    tm.notify_panel_changed("controls");
    tm.record_panel_usage();

    REQUIRE(tm.queue_size() == TelemetryManager::kMaxQueueSize);
    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 2);
    CHECK(panels[0]["panel_visits"] == json{{"home", 1}});
    CHECK(panels[1]["panel_visits"] == json{{"controls", 1}});

    // Only the attempted prefix is excluded: the unattempted row still merges.
    tm.notify_panel_changed("status");
    tm.record_panel_usage();
    panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 2);
    CHECK(panels[0]["panel_visits"] == json{{"home", 1}});
    CHECK(panels[1]["panel_visits"] == json{{"controls", 1}, {"status", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: rows loaded from disk are never merged into",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("home");
    tm.record_panel_usage();
    while (tm.queue_size() < TelemetryManager::kMaxQueueSize) {
        tm.record_session();
    }
    tm.save_queue();
    tm.load_queue(); // as a new process would: any loaded row may be delivered

    tm.notify_panel_changed("controls");
    tm.record_panel_usage();

    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 2);
    CHECK(panels[0]["panel_visits"] == json{{"home", 1}});
    CHECK(panels[1]["panel_visits"] == json{{"controls", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: deltas from different boots are never merged",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);

    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    TelemetryManagerTestAccess::fire_auto_send_tick(tm); // persists the home window
    const std::string old_boot = TelemetryManagerTestAccess::boot_id(tm);
    TelemetryManagerTestAccess::simulate_unclean_exit(tm);
    tm.init(temp_dir().string());
    REQUIRE(queued_events(tm, "panel_usage").size() == 1); // the recovered delta

    while (tm.queue_size() < TelemetryManager::kMaxQueueSize) {
        tm.record_session();
    }
    tm.notify_panel_changed("status");
    TelemetryManagerTestAccess::advance_clock(tm, 10min);
    tm.fire_periodic_snapshot();

    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 2);
    CHECK(panels[0]["boot_id"] == old_boot);
    CHECK(panels[0]["panel_visits"] == json{{"home", 1}});
    CHECK(panels[1]["boot_id"] == TelemetryManagerTestAccess::boot_id(tm));
    CHECK(panels[1]["panel_visits"] == json{{"status", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Queue: a merge keeps the longest outage and one-sided map keys",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.notify_connection_state_changed(2);
    TelemetryManagerTestAccess::advance_clock(tm, 10min);
    tm.notify_connection_state_changed(0);
    TelemetryManagerTestAccess::advance_clock(tm, 10min); // a 600s outage
    tm.notify_connection_state_changed(2);
    tm.notify_widget_interaction("led_control");
    tm.fire_periodic_snapshot(); // seq 0
    while (tm.queue_size() < TelemetryManager::kMaxQueueSize) {
        tm.record_session();
    }

    tm.notify_connection_state_changed(0);
    TelemetryManagerTestAccess::advance_clock(tm, 5min); // a 300s outage
    tm.notify_connection_state_changed(2);
    tm.notify_widget_interaction("fan_control");
    tm.fire_periodic_snapshot(); // seq 1, merges

    auto conns = queued_events(tm, "connection_stability");
    REQUIRE(conns.size() == 1);
    CHECK(conns[0]["longest_disconnect_sec"] == 600);
    CHECK(conns[0]["disconnect_count"] == 2);
    CHECK(conns[0]["total_disconnected_sec"] == 900);

    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 1);
    CHECK(panels[0]["widget_interactions"] == json{{"fan_control", 1}, {"led_control", 1}});
}

namespace {

void fill_queue_with_sessions(TelemetryManager& tm, size_t until) {
    while (tm.queue_size() < until) {
        tm.record_session();
    }
}

/// A full queue whose only panel_usage row sits at index 0.
void fill_queue_behind_panel_row(TelemetryManager& tm, const std::string& panel) {
    tm.notify_panel_changed(panel);
    tm.record_panel_usage();
    REQUIRE(tm.queue_size() == 1);
    fill_queue_with_sessions(tm, TelemetryManager::kMaxQueueSize);
}

/// One real try_send(), past the interval gate, run to its end.
void send_now(TelemetryManager& tm) {
    TelemetryManagerTestAccess::set_backoff(tm, 1);
    TelemetryManagerTestAccess::set_last_send_time(tm, std::chrono::steady_clock::now() - 24h * 30);
    const auto before = TelemetryManagerTestAccess::last_send_time(tm);
    tm.try_send();
    TelemetryManagerTestAccess::join_send_thread(tm);
    REQUIRE(TelemetryManagerTestAccess::last_send_time(tm) != before);
}

} // namespace

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Queue: a send's claim marks its rows attempted, so nothing merges into them",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);
    fill_queue_behind_panel_row(tm, "home");

    send_now(tm);

    tm.notify_panel_changed("controls");
    tm.record_panel_usage();
    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 2);
    CHECK(panels[0]["panel_visits"] == json{{"home", 1}});
    CHECK(panels[1]["panel_visits"] == json{{"controls", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: a finished send releases its batch for eviction",
                 "[telemetry][queue]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);
    tm.record_print_outcome("completed", 600, 5, 300.0f, "PLA", 200, 60);
    fill_queue_with_sessions(tm, TelemetryManager::kMaxQueueSize);
    REQUIRE(tm.get_queue_snapshot()[0]["event"] == "print_outcome");

    send_now(tm);

    // Nothing is in flight any more, so the oldest row is the one evicted.
    tm.record_session();
    REQUIRE(tm.queue_size() == TelemetryManager::kMaxQueueSize);
    CHECK(queued_events(tm, "print_outcome").empty());
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Queue: a confirmed removal ends the claim on the rows that remain",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    fill_queue_with_sessions(tm, TelemetryManager::kMaxBatchSize);
    tm.notify_panel_changed("home");
    tm.record_panel_usage(); // first row after the claimed batch
    fill_queue_with_sessions(tm, TelemetryManager::kMaxQueueSize);

    TelemetryManagerTestAccess::claim_batch(tm);
    tm.remove_sent_events(TelemetryManager::kMaxBatchSize);
    REQUIRE(tm.get_queue_snapshot()[0]["event"] == "panel_usage");
    fill_queue_with_sessions(tm, TelemetryManager::kMaxQueueSize);

    tm.notify_panel_changed("controls");
    tm.record_panel_usage();
    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 1);
    CHECK(panels[0]["panel_visits"] == json{{"home", 1}, {"controls", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: a purge forgets which rows a send attempted",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    fill_queue_behind_panel_row(tm, "status");
    TelemetryManagerTestAccess::fail_send(tm);

    tm.clear_queue();
    fill_queue_behind_panel_row(tm, "home");
    tm.notify_panel_changed("controls");
    tm.record_panel_usage();

    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 1);
    CHECK(panels[0]["panel_visits"] == json{{"home", 1}, {"controls", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: a new boot inherits no claim on the queue",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    fill_queue_behind_panel_row(tm, "status");
    TelemetryManagerTestAccess::fail_send(tm);
    TelemetryManagerTestAccess::set_in_flight(tm, TelemetryManager::kMaxBatchSize);

    TelemetryManagerTestAccess::simulate_unclean_exit(tm);
    tm.init(temp_dir().string()); // nothing was saved, so nothing is loaded
    REQUIRE(tm.queue_size() == 0);

    fill_queue_behind_panel_row(tm, "home");
    tm.notify_panel_changed("controls");
    tm.record_panel_usage();

    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 1);
    CHECK(panels[0]["panel_visits"] == json{{"home", 1}, {"controls", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: a new boot reports none of the last boot's widget taps",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.notify_panel_changed("home");
    tm.notify_widget_interaction("led_control");

    TelemetryManagerTestAccess::simulate_unclean_exit(tm);
    tm.init(temp_dir().string());
    tm.notify_panel_changed("controls");
    tm.notify_widget_interaction("fan_control");
    tm.record_panel_usage();

    auto panels = queued_events(tm, "panel_usage");
    REQUIRE(panels.size() == 1);
    CHECK(panels[0]["widget_interactions"] == json{{"fan_control", 1}});
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot deltas: an outage the first notification opens is measured from it",
                 "[telemetry][snapshot][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_connection_state_changed(0); // first notification: already disconnected
    TelemetryManagerTestAccess::advance_clock(tm, 10min);
    tm.record_connection_stability(); // the outage is still open

    TelemetryManagerTestAccess::advance_clock(tm, 5min);
    tm.notify_connection_state_changed(2); // closes after 900s
    tm.record_connection_stability();

    auto conns = queued_events(tm, "connection_stability");
    REQUIRE(conns.size() == 2);
    CHECK(conns[0]["longest_disconnect_sec"] == 600);
    CHECK(conns[1]["longest_disconnect_sec"] == 900);
}

TEST_CASE_METHOD(TelemetrySchedulerFixture,
                 "Scheduler: the auto-send timer persists the unsent window",
                 "[telemetry][scheduler][snapshot]") {
    auto& tm = TelemetryManager::instance();
    TelemetryManagerTestAccess::disable_network(tm);
    tm.set_enabled(true);
    tm.start_auto_send();
    REQUIRE_FALSE(fs::exists(temp_dir() / kAccumulatorFile));

    lv_tick_inc(TelemetryManager::kInitialSendDelayMs);
    lv_timer_handler();

    CHECK(fs::exists(temp_dir() / kAccumulatorFile));
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Crash event: a crash file without a version reports app_version unknown",
                 "[telemetry][crash]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    write_file("crash.txt", "signal:7\nname:SIGBUS\n");

    tm.check_previous_crash();

    REQUIRE(tm.queue_size() == 1);
    auto event = tm.get_queue_snapshot()[0];
    REQUIRE(event["event"] == "crash");
    CHECK(event["app_version"] == "unknown");
}

// ============================================================================
// Settings Changes [telemetry][settings]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Settings changes: batches changes and builds event",
                 "[telemetry][settings]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_setting_changed("theme", "dark", "light");
    tm.notify_setting_changed("brightness", "80", "60");

    tm.flush_settings_changes();

    REQUIRE(tm.queue_size() == 1);
    auto queue = tm.get_queue_snapshot();
    auto event = queue[0];
    REQUIRE(event["event"] == "settings_changes");
    REQUIRE(event["changes"].size() == 2);
    REQUIRE(event["changes"][0]["setting"] == "theme");
    REQUIRE(event["changes"][0]["old_value"] == "dark");
    REQUIRE(event["changes"][0]["new_value"] == "light");
}

TEST_CASE_METHOD(TelemetryTestFixture, "Settings changes: no event if no changes pending",
                 "[telemetry][settings]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.flush_settings_changes();
    REQUIRE(tm.queue_size() == 0);
}

// ============================================================================
// Per-Panel Breakdown [telemetry][frame]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Frame time: snapshot includes per-panel breakdown in panels object",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("print_select_panel");
    for (int i = 0; i < 20; ++i) {
        tm.record_frame_time(15000); // 15ms
    }
    // Add one jank frame
    tm.record_frame_time(45000); // 45ms - over budget

    tm.notify_panel_changed("settings_panel");
    for (int i = 0; i < 10; ++i) {
        tm.record_frame_time(5000); // 5ms - fast
    }

    tm.record_performance_snapshot();
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    // Top-level fields still present (backward compat)
    REQUIRE(event.contains("worst_panel"));
    REQUIRE(event.contains("worst_panel_p95_ms"));

    // New panels object
    REQUIRE(event.contains("panels"));
    auto& panels = event["panels"];
    REQUIRE(panels.contains("print_select_panel"));
    REQUIRE(panels.contains("settings_panel"));

    // print_select_panel had the jank
    auto& ps = panels["print_select_panel"];
    REQUIRE(ps.contains("p50_ms"));
    REQUIRE(ps.contains("p95_ms"));
    REQUIRE(ps.contains("p99_ms"));
    REQUIRE(ps.contains("max_ms"));
    REQUIRE(ps.contains("frames"));
    REQUIRE(ps.contains("over_budget_pct"));
    REQUIRE(ps["frames"] == 21);
    REQUIRE(ps["max_ms"] == 45);
    CHECK(ps["over_budget_pct"] > 0.0);

    // settings_panel was clean
    auto& sp = panels["settings_panel"];
    REQUIRE(sp["frames"] == 10);
    REQUIRE(sp["max_ms"] == 5);
    CHECK(sp["over_budget_pct"] == 0.0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Frame time: panels with unknown panel_id are excluded from breakdown",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Record frames WITHOUT calling notify_panel_changed first.
    // current_panel_id_ is 0 but panel_names_ is empty, so pid >= panel_names_.size().
    tm.record_frame_time(8000);
    tm.record_frame_time(8000);

    tm.record_performance_snapshot();
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];

    // Top-level percentiles should still be computed from all frames
    REQUIRE(event["total_frame_count"] == 2);

    // But the panels object should be empty (no valid panel name)
    REQUIRE(event.contains("panels"));
    REQUIRE(event["panels"].empty());
}

// ============================================================================
// AsyncLifetime Skip Snapshot [telemetry][lifetime]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "AsyncLifetime snapshot: empty window is not enqueued",
                 "[telemetry][lifetime]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.clear_queue();
    // Drain whatever a prior test left in the counter store; take_snapshot()
    // releases every slot, so discarding the result is the reset.
    (void)helix::async_lifetime::take_snapshot();

    tm.record_async_lifetime_snapshot();

    // No callbacks were skipped — the event carries no signal, so it is
    // suppressed rather than burning an upload slot.
    REQUIRE(tm.queue_size() == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "AsyncLifetime snapshot: window carries the per-tag breakdown",
                 "[telemetry][lifetime]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.clear_queue();
    // Drain whatever a prior test left in the counter store; take_snapshot()
    // releases every slot, so discarding the result is the reset.
    (void)helix::async_lifetime::take_snapshot();

    helix::async_lifetime::note_skipped("HotProducer::cb");
    helix::async_lifetime::note_skipped("HotProducer::cb");
    helix::async_lifetime::note_skipped("QuietProducer::cb");

    tm.record_async_lifetime_snapshot();

    REQUIRE(tm.queue_size() == 1);
    auto event = tm.get_queue_snapshot()[0];
    REQUIRE(event["event"] == "async_lifetime_skips");
    REQUIRE(event["total_skips"] == 3);
    REQUIRE(event["other_count"] == 0);
    REQUIRE(event["tags"].size() == 2);
    // Sorted descending: the hot producer leads. Identifying *which* owner is
    // dying with pending work is the entire point of the event.
    REQUIRE(event["tags"][0]["tag"] == "HotProducer::cb");
    REQUIRE(event["tags"][0]["count"] == 2);
    REQUIRE(event["tags"][1]["tag"] == "QuietProducer::cb");
    REQUIRE(event["tags"][1]["count"] == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "AsyncLifetime snapshot: counters drain even while telemetry is disabled",
                 "[telemetry][lifetime]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(false);
    tm.clear_queue();
    // Drain whatever a prior test left in the counter store; take_snapshot()
    // releases every slot, so discarding the result is the reset.
    (void)helix::async_lifetime::take_snapshot();

    helix::async_lifetime::note_skipped("WhileDisabled::cb");
    tm.record_async_lifetime_snapshot();
    REQUIRE(tm.queue_size() == 0);

    // Opting in must not inherit the disabled span's counts. If the drain were
    // gated behind the enabled check, this first post-opt-in event would report
    // a whole-session total mislabeled as one window.
    tm.set_enabled(true);
    tm.clear_queue();

    helix::async_lifetime::note_skipped("AfterOptIn::cb");
    tm.record_async_lifetime_snapshot();

    REQUIRE(tm.queue_size() == 1);
    auto event = tm.get_queue_snapshot()[0];
    REQUIRE(event["total_skips"] == 1);
    REQUIRE(event["tags"].size() == 1);
    REQUIRE(event["tags"][0]["tag"] == "AfterOptIn::cb");
}

// ============================================================================
// Event envelope [telemetry][envelope]
// ============================================================================

namespace {

constexpr const char* kEnvelopeKeys[] = {"schema_version", "app_version", "app_platform",
                                         "app_board", "boot_id"};

} // namespace

TEST_CASE_METHOD(TelemetryTestFixture, "Envelope: every event carries all five envelope keys",
                 "[telemetry][envelope]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    auto events = record_one_of_every_event(tm, *this);

    // A builder dropping out of the sweep would otherwise pass vacuously.
    for (const char* name :
         {"session", "print_outcome", "update_failed", "update_success", "memory_snapshot",
          "memory_warning", "hardware_profile", "settings_snapshot", "panel_usage",
          "connection_stability", "screensaver_usage", "performance_snapshot", "settings_changes",
          "async_lifetime_skips", "print_start_context", "error_encountered", "crash"}) {
        INFO("event: " << name);
        REQUIRE(events.count(name) == 1);
    }

    for (const auto& [name, event] : events) {
        for (const char* key : kEnvelopeKeys) {
            INFO("event: " << name << ", key: " << key);
            REQUIRE(event.contains(key));
            if (std::string(key) == "schema_version") {
                CHECK(event[key] == 3);
            } else {
                REQUIRE(event[key].is_string());
                CHECK_FALSE(event[key].get<std::string>().empty());
            }
        }
        CHECK(event["app_version"] == (name == "crash" ? json("0.14.0") : json(HELIX_VERSION)));
        CHECK(event["app_platform"] == helix::platform::current_key());
        CHECK(event["app_board"] == helix::board::current_info().key);
    }
}

TEST_CASE("Envelope: SCHEMA_VERSION is 3", "[telemetry][envelope]") {
    REQUIRE(TelemetryManager::kSchemaVersion == 3);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Envelope: boot_id is 16 lowercase hex characters",
                 "[telemetry][envelope]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.record_session();
    auto boot_id = tm.get_queue_snapshot()[0]["boot_id"].get<std::string>();

    REQUIRE(boot_id.size() == 16);
    CHECK(std::all_of(boot_id.begin(), boot_id.end(),
                      [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Envelope: boot_id is stable within a boot, new per init",
                 "[telemetry][envelope]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_error("moonraker_api", "timeout", "ctx");
    auto queue = tm.get_queue_snapshot();
    REQUIRE(queue.size() == 2);
    CHECK(queue[0]["boot_id"] == queue[1]["boot_id"]);
    const auto first_boot = queue[0]["boot_id"];

    tm.shutdown();
    tm.init(temp_dir().string());
    tm.set_enabled(true);
    tm.clear_queue();
    tm.record_session();
    CHECK(tm.get_queue_snapshot()[0]["boot_id"] != first_boot);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Envelope: update events keep version fields beside app_version",
                 "[telemetry][envelope]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_update_failure("download_failed", "1.0.5", "pi");
    auto event = tm.get_queue_snapshot()[0];

    CHECK(event["version"] == "1.0.5");
    CHECK(event["from_version"] == HELIX_VERSION);
    CHECK(event["app_version"] == HELIX_VERSION);
}

TEST_CASE("Telemetry hardware profile names the filament system by its slug",
          "[telemetry][ams][indx]") {
    auto& tm = TelemetryManager::instance();
    helix::PrinterDiscovery indx;
    indx.parse_objects(nlohmann::json::array({"indx", "save_variables", "extruder"}));
    CHECK(TelemetryManagerTestAccess::hw_ams_section(tm, indx)["type"] == "indx");

    helix::PrinterDiscovery mmu;
    mmu.parse_objects(nlohmann::json::array({"mmu", "extruder"}));
    CHECK(TelemetryManagerTestAccess::hw_ams_section(tm, mmu)["type"] == "happy_hare");

    helix::PrinterDiscovery none;
    none.parse_objects(nlohmann::json::array({"extruder"}));
    CHECK(TelemetryManagerTestAccess::hw_ams_section(tm, none)["type"] == "unknown");
}

// ============================================================================
// Device identity location [telemetry][identity] (#2085)
// ============================================================================

namespace {

/// A config dir and an identity dir beside it, as on a device: config/ inside
/// the install root, the identity in the state root next to it.
struct IdentityDirs {
    fs::path install;
    fs::path config;
    fs::path state;

    explicit IdentityDirs(const fs::path& base)
        : install(base / "helixscreen"), config(install / "config"),
          state(base / "helixscreen-state") {
        fs::create_directories(config);
    }
};

std::string read_text(const fs::path& p) {
    std::ifstream in(p);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& p, const std::string& text) {
    std::ofstream(p) << text;
}

void reinit(TelemetryManager& tm, const IdentityDirs& d) {
    tm.shutdown();
    tm.init(d.config.string(), d.state.string());
}

} // namespace

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Identity: written to the identity dir, never the config dir",
                 "[telemetry][identity][2085]") {
    auto& tm = TelemetryManager::instance();
    const IdentityDirs d(temp_dir_);

    reinit(tm, d);

    REQUIRE(fs::exists(d.state / "telemetry_device.json"));
    CHECK_FALSE(fs::exists(d.config / "telemetry_device.json"));
    CHECK(tm.get_device_id_path() == (d.state / "telemetry_device.json").string());
    CHECK(read_text(d.state / "telemetry_device.json").find("\"salt\"") != std::string::npos);
    // The debug bundle reads the same file.
    CHECK_FALSE(helix::DebugBundleCollector::collect_device_id(tm.get_device_id_path()).empty());
}

TEST_CASE_METHOD(TelemetryTestFixture, "Identity: survives a simulated rmtree of the install root",
                 "[telemetry][identity][2085]") {
    auto& tm = TelemetryManager::instance();
    const IdentityDirs d(temp_dir_);

    reinit(tm, d);
    const std::string before = read_text(d.state / "telemetry_device.json");
    REQUIRE_FALSE(before.empty());

    // What a Moonraker type:web update does to the install root.
    tm.shutdown();
    fs::remove_all(d.install);
    fs::create_directories(d.config);
    tm.init(d.config.string(), d.state.string());

    CHECK(read_text(d.state / "telemetry_device.json") == before);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Identity: one left in the config dir moves once, and a second start is a no-op",
                 "[telemetry][identity][2085]") {
    auto& tm = TelemetryManager::instance();
    const IdentityDirs d(temp_dir_);
    const std::string old_identity = R"({"uuid": "old-uuid", "salt": "old-salt"})";
    write_text(d.config / "telemetry_device.json", old_identity);

    reinit(tm, d);
    REQUIRE(fs::exists(d.state / "telemetry_device.json"));
    CHECK(read_text(d.state / "telemetry_device.json") == old_identity);
    CHECK_FALSE(fs::exists(d.config / "telemetry_device.json"));

    const auto mtime = fs::last_write_time(d.state / "telemetry_device.json");
    reinit(tm, d);
    CHECK(read_text(d.state / "telemetry_device.json") == old_identity);
    CHECK(fs::last_write_time(d.state / "telemetry_device.json") == mtime);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Identity: one already in the identity dir is never replaced",
                 "[telemetry][identity][2085]") {
    auto& tm = TelemetryManager::instance();
    const IdentityDirs d(temp_dir_);
    fs::create_directories(d.state);
    const std::string kept = R"({"uuid": "state-uuid", "salt": "state-salt"})";
    const std::string other = R"({"uuid": "config-uuid", "salt": "config-salt"})";
    write_text(d.state / "telemetry_device.json", kept);
    write_text(d.config / "telemetry_device.json", other);

    reinit(tm, d);

    CHECK(read_text(d.state / "telemetry_device.json") == kept);
    // A different identity is left where it was rather than deleted.
    CHECK(read_text(d.config / "telemetry_device.json") == other);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Identity: a duplicate left in the config dir by an interrupted move is removed",
                 "[telemetry][identity][2085]") {
    auto& tm = TelemetryManager::instance();
    const IdentityDirs d(temp_dir_);
    fs::create_directories(d.state);
    const std::string same = R"({"uuid": "u", "salt": "s"})";
    write_text(d.state / "telemetry_device.json", same);
    write_text(d.config / "telemetry_device.json", same);

    reinit(tm, d);

    CHECK(read_text(d.state / "telemetry_device.json") == same);
    CHECK_FALSE(fs::exists(d.config / "telemetry_device.json"));
}

TEST_CASE_METHOD(TelemetryTestFixture, "Identity: no identity dir keeps the file in the config dir",
                 "[telemetry][identity][2085]") {
    auto& tm = TelemetryManager::instance();
    const IdentityDirs d(temp_dir_);

    tm.shutdown();
    tm.init(d.config.string());

    CHECK(fs::exists(d.config / "telemetry_device.json"));
    CHECK_FALSE(fs::exists(d.state));
}
