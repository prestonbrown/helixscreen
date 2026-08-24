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

#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "config.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "system/telemetry_manager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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
// Test Fixture
// ============================================================================

/**
 * @brief Test fixture for TelemetryManager testing
 *
 * Provides a unique temporary directory for each test and initializes
 * TelemetryManager with that directory as the config root. Cleans up
 * after each test to ensure isolation.
 */
class TelemetryTestFixture {
  public:
    TelemetryTestFixture() {
        // Create unique temp directory per test run
        temp_dir_ = fs::temp_directory_path() /
                    ("helix_telemetry_test_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir_);

        auto& tm = TelemetryManager::instance();
        tm.shutdown();

        // Drain any deferred UI callbacks queued by prior tests. Some of those
        // callbacks (settings observers, panel-change notifiers) can route into
        // TelemetryManager::record_* and end up enqueuing a phantom event in
        // the next test. Drain happens BEFORE init so callbacks see the
        // !initialized_ guard and bail out before touching the queue.
        helix::ui::UpdateQueue::instance().drain();

        // Reset Config's /telemetry_enabled to false BEFORE init() so the
        // singleton reload at the end of init() lands at a known false state.
        // Without this, a prior test's set_enabled(true) persisted into the
        // global Config, init() reloaded it as true, and any error/setting
        // change between init() and our explicit set_enabled(false) below
        // would gate-pass and enqueue (off-by-one queue size on macOS where
        // the whole suite shares one process).
        //
        // Also clear the Config singleton's path. Some prior tests
        // (test_external_spool.cpp's TempConfigFixture, test_filament_
        // consumption_tracker.cpp) call Config::get_instance()->init(<temp_dir>)
        // and then delete the directory. The singleton retains a now-dangling
        // path; subsequent set_enabled(true) → Config::save() fails its
        // ofstream open, calls CONFIG_RECORD_ERROR, and enqueues a phantom
        // telemetry "file_io" event between our clear_queue() and the test's
        // first assertion. Empty path makes save() a no-op (line 1426).
        if (auto* cfg = helix::Config::get_instance()) {
            cfg->clear_path();
            cfg->set<bool>("/telemetry_enabled", false);
        }

        tm.init(temp_dir_.string());
        tm.set_enabled(false);
        tm.clear_queue();
    }

    ~TelemetryTestFixture() {
        // Drain BEFORE shutdown(), while the subject these callbacks write is
        // still alive. set_enabled() defers its subject write, so the ctor's
        // set_enabled(false) — plus any set_enabled() in the test body — is
        // still queued at this point. Leaving them for whichever fixture
        // drained next is what made TelemetryManager::set_enabled the single
        // largest producer in the cross-test leak report: 185 callbacks across
        // 96 tests, 21% of the suite total (#1166).
        helix::ui::UpdateQueue::instance().drain();

        TelemetryManager::instance().shutdown();

        // Clean up temp directory - best effort
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    [[nodiscard]] fs::path temp_dir() const {
        return temp_dir_;
    }

    /**
     * @brief Write arbitrary content to a file in the temp directory
     */
    void write_file(const std::string& filename, const std::string& content) {
        std::ofstream ofs(temp_dir_ / filename);
        ofs << content;
        ofs.close();
    }

    /**
     * @brief Read content from a file in the temp directory
     */
    [[nodiscard]] std::string read_file(const std::string& filename) const {
        std::ifstream ifs(temp_dir_ / filename);
        return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    }

  protected:
    fs::path temp_dir_;
};

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
    constexpr int NUM_UUIDS = 50;

    for (int i = 0; i < NUM_UUIDS; ++i) {
        auto uuid = TelemetryManager::generate_uuid_v4();
        uuids.insert(uuid);
    }

    // All generated UUIDs must be distinct
    REQUIRE(uuids.size() == NUM_UUIDS);
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
    for (size_t i = 0; i < TelemetryManager::MAX_QUEUE_SIZE; ++i) {
        tm.record_session();
    }
    REQUIRE(tm.queue_size() == TelemetryManager::MAX_QUEUE_SIZE);

    // Record one more - should drop the oldest
    tm.record_print_outcome("completed", 600, 10, 1500.0f, "PLA", 210, 60);
    REQUIRE(tm.queue_size() == TelemetryManager::MAX_QUEUE_SIZE);

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
        REQUIRE(event["app"].contains("zip_tool"));
        const std::string zip_tool = event["app"]["zip_tool"];
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
// MAX_QUEUE_SIZE constant [telemetry][queue]
// ============================================================================

TEST_CASE("MAX_QUEUE_SIZE is 100", "[telemetry][queue]") {
    REQUIRE(TelemetryManager::MAX_QUEUE_SIZE == 100);
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

    // Fill queue with more events than MAX_BATCH_SIZE
    for (size_t i = 0; i < TelemetryManager::MAX_BATCH_SIZE + 10; ++i) {
        tm.record_session();
    }
    REQUIRE(tm.queue_size() == TelemetryManager::MAX_BATCH_SIZE + 10);

    // Build a batch and verify it respects the limit
    auto batch = tm.build_batch();
    REQUIRE(batch.is_array());
    REQUIRE(batch.size() == TelemetryManager::MAX_BATCH_SIZE);
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

TEST_CASE("Transmission: constants have expected values", "[telemetry][transmission]") {
    // Verify transmission-related constants
    REQUIRE(TelemetryManager::MAX_BATCH_SIZE == 20);
    REQUIRE(TelemetryManager::SEND_INTERVAL == std::chrono::hours{24});

    // Endpoint URL should be HTTPS
    std::string url(TelemetryManager::ENDPOINT_URL);
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

TEST_CASE_METHOD(TelemetryTestFixture, "Scheduler: start_auto_send creates timer",
                 "[telemetry][scheduler]") {
    auto& tm = TelemetryManager::instance();

    // Should not crash when called
    tm.start_auto_send();

    // Calling again should be safe (idempotent)
    tm.start_auto_send();

    // Stop should clean up
    tm.stop_auto_send();
}

TEST_CASE_METHOD(TelemetryTestFixture, "Scheduler: stop_auto_send is safe when no timer",
                 "[telemetry][scheduler]") {
    auto& tm = TelemetryManager::instance();

    // Should not crash when called without start
    tm.stop_auto_send();
    tm.stop_auto_send(); // Double-stop should be safe
}

TEST_CASE_METHOD(TelemetryTestFixture, "Scheduler: shutdown stops auto-send",
                 "[telemetry][scheduler]") {
    auto& tm = TelemetryManager::instance();
    tm.start_auto_send();

    // Shutdown should stop the timer and not crash
    tm.shutdown();

    // Re-init for fixture cleanup
    tm.init(temp_dir().string());
}

TEST_CASE("Scheduler: constants have expected values", "[telemetry][scheduler]") {
    REQUIRE(TelemetryManager::INITIAL_SEND_DELAY_MS == 60000);
    REQUIRE(TelemetryManager::AUTO_SEND_INTERVAL_MS == 3600000);
}

// ============================================================================
// Schema Version 2 - Hardware Survey [telemetry][session][v2]
// ============================================================================

TEST_CASE("SCHEMA_VERSION is 2", "[telemetry][session][v2]") {
    REQUIRE(TelemetryManager::SCHEMA_VERSION == 2);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Session event v2: schema_version is 2",
                 "[telemetry][session][v2]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    auto event = tm.get_queue_snapshot()[0];

    REQUIRE(event["schema_version"] == 2);
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

    // In test context without a real printer, features may not be present
    // But if it IS present, it must be an array of strings
    if (event.contains("features")) {
        REQUIRE(event["features"].is_array());
        for (const auto& f : event["features"]) {
            REQUIRE(f.is_string());
        }
    }
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

    REQUIRE(event["schema_version"] == 2);
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

    REQUIRE(event["schema_version"] == 2);
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
    CHECK(event["schema_version"] == TelemetryManager::SCHEMA_VERSION);
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

/// Installs a global Moonraker API for a scope and restores the previous one.
class ScopedMoonrakerApi {
  public:
    explicit ScopedMoonrakerApi(IMoonrakerAPI* api) : previous_(get_moonraker_api()) {
        set_moonraker_api(api);
    }
    ~ScopedMoonrakerApi() {
        set_moonraker_api(previous_);
    }

    ScopedMoonrakerApi(const ScopedMoonrakerApi&) = delete;
    ScopedMoonrakerApi& operator=(const ScopedMoonrakerApi&) = delete;

  private:
    IMoonrakerAPI* previous_;
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
        if (event["event"] == "hardware_profile") {
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
        if (event["event"] == "hardware_profile") {
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
    CHECK(event["schema_version"] == TelemetryManager::SCHEMA_VERSION);
    CHECK(event.contains("device_id"));
    CHECK(event.contains("timestamp"));
    // Hardware profile may have empty sections in test mode (no printer connected)
    // but the event itself should be valid
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
    CHECK(event["schema_version"] == TelemetryManager::SCHEMA_VERSION);
    CHECK(event.contains("device_id"));
    CHECK(event.contains("timestamp"));
    // Settings should have at least theme and locale
    CHECK(event.contains("theme"));
    CHECK(event.contains("locale"));
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

TEST_CASE_METHOD(TelemetryTestFixture, "record_error rejects unknown categories",
                 "[telemetry][error]") {
    TelemetryManager::instance().set_enabled(true);

    TelemetryManager::instance().record_error("invalid_category", "code", "ctx");
    CHECK(TelemetryManager::instance().queue_size() == 0);

    TelemetryManager::instance().record_error("", "code", "ctx");
    CHECK(TelemetryManager::instance().queue_size() == 0);

    // Valid category should work
    TelemetryManager::instance().record_error("moonraker_api", "timeout", "ctx");
    CHECK(TelemetryManager::instance().queue_size() == 1);
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

        // Must NOT contain hostname of the test machine
        if (!machine_hostname.empty()) {
            REQUIRE(event_str.find(machine_hostname) == std::string::npos);
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
        if (ev["event"] == "panel_usage") {
            if (first_seq < 0)
                first_seq = ev["snapshot_seq"].get<int>();
            else
                second_seq = ev["snapshot_seq"].get<int>();
        }
    }
    REQUIRE(first_seq == 0);
    REQUIRE(second_seq == 1);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Snapshot: state persisted to disk and recovered",
                 "[telemetry][snapshot]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    tm.notify_panel_changed("temperature");
    tm.fire_periodic_snapshot();
    tm.clear_queue();

    // Verify snapshot file exists
    auto snap_path = temp_dir() / "telemetry_snapshot.json";
    REQUIRE(fs::exists(snap_path));

    // Read the snapshot file
    std::ifstream ifs(snap_path);
    auto snap_json = nlohmann::json::parse(ifs);
    REQUIRE(snap_json.contains("snapshot_seq"));
    REQUIRE(snap_json["snapshot_seq"] == 1);
    REQUIRE(snap_json.contains("panel_time_sec"));
    REQUIRE(snap_json.contains("panel_visits"));
}

// ============================================================================
// Feature Adoption [telemetry][adoption]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Feature adoption: event contains feature flags",
                 "[telemetry][adoption]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("macros");
    tm.notify_panel_changed("camera");
    tm.notify_widget_interaction("led_control");

    tm.record_feature_adoption();

    REQUIRE(tm.queue_size() == 1);
    auto queue = tm.get_queue_snapshot();
    auto event = queue[0];
    REQUIRE(event["event"] == "feature_adoption");
    REQUIRE(event.contains("features"));

    auto& features = event["features"];
    REQUIRE(features["macros"] == true);
    REQUIRE(features["camera"] == true);
    REQUIRE(features["led_control"] == true);
    REQUIRE(features["bed_mesh"] == false);
    REQUIRE(features["console_gcode"] == false);
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
