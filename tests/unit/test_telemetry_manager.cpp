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
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

// TelemetryManager is a global singleton (not namespaced)

// ============================================================================
// UUID v4 Generation [telemetry][uuid]
// ============================================================================

TEST_CASE("UUID v4: random 8-4-4-4-12 hex with version 4 and an RFC 4122 variant",
          "[telemetry][uuid]") {
    // xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx: version nibble at 14, variant at 19
    const std::regex uuid_regex(
        "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    constexpr int kNumUuids = 50;
    std::set<std::string> uuids;
    for (int i = 0; i < kNumUuids; ++i) {
        auto uuid = TelemetryManager::generate_uuid_v4();
        INFO("UUID: " << uuid);
        REQUIRE(uuid.size() == 36);
        REQUIRE(std::regex_match(uuid, uuid_regex));
        uuids.insert(uuid);
    }
    REQUIRE(uuids.size() == kNumUuids);
}

// ============================================================================
// Double-hash Anonymization [telemetry][hash]
// ============================================================================

TEST_CASE("sha256_hex matches the FIPS 180-4 test vectors", "[telemetry][hash]") {
    REQUIRE(helix::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(helix::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(helix::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("hash_device_id: a stable, salted 64-hex SHA-256 of the UUID", "[telemetry][hash]") {
    const std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
    const std::regex hex64("^[0-9a-f]{64}$");

    // A known value keeps stored device ids stable across releases; it also
    // shows the UUID itself does not survive into the output.
    REQUIRE(TelemetryManager::hash_device_id(uuid, "test-salt") ==
            "21fffeed629d1a1d7ff8bfcf6af51553abfc0ed61b5351a1f2aaa9ef513380b1");
    CHECK(TelemetryManager::hash_device_id(uuid, "salt-alpha") !=
          TelemetryManager::hash_device_id(uuid, "salt-beta"));
    CHECK(TelemetryManager::hash_device_id("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", "shared-salt") !=
          TelemetryManager::hash_device_id("11111111-2222-4333-8444-555555555555", "shared-salt"));
    CHECK(std::regex_match(TelemetryManager::hash_device_id("", ""), hex64));
}

// ============================================================================
// Event Queue [telemetry][queue]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Queue: enqueue, snapshot and clear", "[telemetry][queue]") {
    auto& tm = TelemetryManager::instance();
    REQUIRE(tm.queue_size() == 0);
    REQUIRE(tm.get_queue_snapshot().is_array());
    REQUIRE(tm.get_queue_snapshot().empty());

    tm.set_enabled(true);
    tm.record_session();
    REQUIRE(tm.queue_size() == 1);

    // The snapshot is a copy: later events do not reach it
    auto snapshot = tm.get_queue_snapshot();
    tm.record_session();
    CHECK(snapshot.size() == 1);
    REQUIRE(tm.queue_size() == 2);

    tm.clear_queue();
    CHECK(tm.queue_size() == 0);
    CHECK(tm.get_queue_snapshot().empty());
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

// ============================================================================
// Session Event Schema [telemetry][session]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Session event: envelope, app and host fields, no PII",
                 "[telemetry][session]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Two sessions: the zip probe is cached per process, and caching must not
    // make zip_tool a first-session-only field.
    tm.record_session();
    tm.record_session();
    auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 2);

    const std::set<std::string> zip_tools{"unzip", "python", "none"};
    for (const auto& event : snapshot) {
        CHECK(event["event"] == "session");
        CHECK(event["schema_version"].is_number_integer());
        REQUIRE(event["device_id"].is_string());
        // ISO 8601: YYYY-MM-DDTHH:MM:SS...
        const std::string timestamp = event["timestamp"];
        CHECK(timestamp.size() >= 19);
        CHECK(timestamp.find('T') != std::string::npos);

        CHECK(event["app"]["version"].is_string());
        CHECK(event["app"]["platform"].is_string());
        // Gates the .tar.gz retirement (#993); present even with no DisplayManager
        const std::string zip_tool = event["app"]["zip_tool"];
        INFO("zip_tool was: " << zip_tool);
        CHECK(zip_tools.count(zip_tool) == 1);
        // From helix::is_moonraker_on_same_host, independent of any connection
        const std::string locality = event["host"]["screen_locality"];
        INFO("screen_locality was: " << locality);
        CHECK((locality == "local" || locality == "remote"));

        for (const char* pii : {"ip", "ip_address", "hostname", "username", "mac_address",
                                "filename", "serial_number", "email", "ssid"}) {
            INFO("PII field: " << pii);
            CHECK_FALSE(event.contains(pii));
        }
        // The device id is a hash, not the raw UUID
        const std::regex uuid_regex(
            "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
        CHECK_FALSE(std::regex_match(event["device_id"].get<std::string>(), uuid_regex));
    }
    CHECK(snapshot[0]["app"]["zip_tool"] == snapshot[1]["app"]["zip_tool"]);

    const auto& event = snapshot[0];
    CHECK(event["schema_version"] == TelemetryManager::kSchemaVersion);
    const std::string theme = event["app"]["theme"];
    CHECK((theme == "dark" || theme == "light"));
    CHECK_FALSE(event["app"]["locale"].get<std::string>().empty());
    // host is present without a printer and always names the architecture
    CHECK_FALSE(event["host"]["arch"].get<std::string>().empty());
    // A test printer may report no features: absent, or an array of strings
    CHECK((!event.contains("features") ||
           (event["features"].is_array() &&
            std::all_of(event["features"].begin(), event["features"].end(),
                        [](const json& f) { return f.is_string(); }))));
    // No PII key at any depth, printer/features/host included
    const std::string dumped = event.dump();
    for (const char* key : {"\"hostname\"", "\"ip\"", "\"mac_address\"", "\"username\"",
                            "\"serial_number\"", "\"email\"", "\"ssid\""}) {
        INFO("key: " << key);
        CHECK(dumped.find(key) == std::string::npos);
    }
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

// ============================================================================
// Print Outcome Event Schema [telemetry][print_outcome]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Print outcome event: carries the job's figures, no file",
                 "[telemetry][print_outcome]") {
    struct Row {
        const char* outcome;
        int duration;
        int phases;
        float filament_mm;
        const char* type;
        int nozzle;
        int bed;
    };
    const Row rows[] = {
        {"completed", 7200, 10, 3200.5f, "ABS", 250, 110},
        {"failed", 1800, 5, 800.0f, "PETG", 240, 80},
        {"cancelled", 900, 3, 150.0f, "PLA", 200, 55},
        // A multi-tool job lists every material; a job with none reports ""
        {"success", 600, 5, 3100.0f, "ABS;PLA", 200, 60},
        {"success", 3600, 10, 0.0f, "", 210, 60},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.outcome << " " << r.type) {
            auto& tm = TelemetryManager::instance();
            tm.set_enabled(true);
            tm.record_print_outcome(r.outcome, r.duration, r.phases, r.filament_mm, r.type,
                                    r.nozzle, r.bed);
            auto snapshot = tm.get_queue_snapshot();
            REQUIRE(snapshot.size() == 1);
            const auto& event = snapshot[0];

            CHECK(event["event"] == "print_outcome");
            CHECK(event.contains("schema_version"));
            CHECK(event.contains("device_id"));
            CHECK(event.contains("timestamp"));
            CHECK(event["outcome"] == r.outcome);
            CHECK(event["duration_sec"] == r.duration);
            CHECK(event["phases_completed"] == r.phases);
            CHECK(event["filament_used_mm"].get<float>() == Catch::Approx(r.filament_mm));
            CHECK(event["filament_type"] == r.type);
            CHECK(event["nozzle_temp"] == r.nozzle);
            CHECK(event["bed_temp"] == r.bed);
            for (const char* leak :
                 {"filename", "file", "filepath", "gcode", "gcode_file", "path"}) {
                INFO("field: " << leak);
                CHECK_FALSE(event.contains(leak));
            }
        }
    }
}

// ============================================================================
// Enable/Disable Toggle [telemetry][toggle]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Toggle: recording follows the enabled flag",
                 "[telemetry][toggle]") {
    auto& tm = TelemetryManager::instance();
    REQUIRE_FALSE(tm.is_enabled()); // the fixture's default

    tm.record_session();
    tm.record_print_outcome("completed", 3600, 10, 2500.0f, "PLA", 215, 60);
    REQUIRE(tm.queue_size() == 0);

    tm.set_enabled(true);
    REQUIRE(tm.is_enabled());
    tm.record_session();
    tm.record_print_outcome("completed", 1000, 5, 500.0f, "PLA", 200, 60);
    REQUIRE(tm.queue_size() == 2);

    // Disabling keeps what is queued and stops new events; re-enabling resumes
    tm.set_enabled(false);
    REQUIRE(tm.queue_size() == 2);
    tm.record_session();
    REQUIRE(tm.queue_size() == 2);
    tm.set_enabled(true);
    tm.record_session();
    REQUIRE(tm.queue_size() == 3);
}

// ============================================================================
// Queue File Round-Trip (Persistence) [telemetry][persistence]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Persistence: the queue file round-trips the queue",
                 "[telemetry][persistence]") {
    auto& tm = TelemetryManager::instance();

    SECTION("an empty queue reads back empty") {
        tm.save_queue();
        tm.load_queue();
        REQUIRE(tm.queue_size() == 0);
        REQUIRE(tm.get_queue_snapshot().is_array());
    }

    SECTION("events survive several save/load cycles in order") {
        tm.set_enabled(true);
        tm.record_session();
        tm.save_queue();
        tm.clear_queue();
        tm.load_queue();
        REQUIRE(tm.queue_size() == 1);

        tm.record_print_outcome("completed", 600, 5, 300.0f, "PETG", 230, 70);
        const auto before = tm.get_queue_snapshot();
        tm.save_queue();
        tm.clear_queue();
        tm.load_queue();
        REQUIRE(tm.get_queue_snapshot() == before);
        CHECK(before[0]["event"] == "session");
        CHECK(before[1]["event"] == "print_outcome");

        // What sits on disk is a plain JSON array of those events
        bool found = false;
        for (auto& entry : fs::recursive_directory_iterator(temp_dir_)) {
            if (entry.path().extension() != ".json") {
                continue;
            }
            std::ifstream ifs(entry.path());
            auto parsed = json::parse(ifs, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_array()) {
                found = true;
                CHECK(parsed.size() == 2);
            }
        }
        CHECK(found);
    }
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

// ============================================================================
// Queue size, drain capacity and retry spacing [telemetry][send][1476]
// ============================================================================

TEST_CASE("Drain: a send window keeps up with the unattended producers",
          "[telemetry][queue][send][1476]") {
    REQUIRE(TelemetryManager::kMaxQueueSize == 100);

    // A window that cannot clear a full queue leaves it saturated, and
    // enqueue_event() then discards the oldest events unsent.
    CHECK(TelemetryManager::kMaxBatchesPerSend * TelemetryManager::kMaxBatchSize >=
          TelemetryManager::kMaxQueueSize);

    // record_memory_snapshot("hourly") fires from the auto-send timer: one event
    // an hour with nobody at the device, more than one batch per kSendInterval
    // can carry. The batch bound is what covers that rate.
    constexpr int hourly_events_per_send_interval =
        static_cast<int>(TelemetryManager::kSendInterval.count());
    CHECK(hourly_events_per_send_interval > static_cast<int>(TelemetryManager::kMaxBatchSize));
    CHECK(static_cast<int>(TelemetryManager::kMaxBatchesPerSend * TelemetryManager::kMaxBatchSize) >
          hourly_events_per_send_interval);

    // With nobody touching the device the periodic timers are the whole producer
    // set; a budget above kMaxQueueSize is what gets discarded unsent every day.
    CHECK(TelemetryManager::kUnattendedEventsPerSendWindow <= TelemetryManager::kMaxQueueSize);
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

TEST_CASE("Send delay: retries back off from hours to a seven-day ceiling",
          "[telemetry][send][1476]") {
    using TM = TelemetryManager;
    // A healthy sender (backoff 1) keeps the daily cadence.
    CHECK(TM::next_attempt_delay(1) == TM::kSendInterval);
    // A first failure retries sooner, so a momentary outage does not leave the
    // queue filling for a day.
    CHECK(TM::next_attempt_delay(2) < TM::kSendInterval);
    CHECK(TM::next_attempt_delay(2) < TM::next_attempt_delay(4));
    CHECK(TM::next_attempt_delay(4) < TM::next_attempt_delay(64));
    // A printer that never reaches the endpoint must not try MORE often than a
    // healthy one: each attempt is a thread, a DNS lookup and a 30s timeout.
    // kRetryInterval is the base, so the multiplier climbs to a full week.
    CHECK(TM::next_attempt_delay(TM::kMaxBackoffMultiplier) >= TM::kSendInterval);
    CHECK(TM::next_attempt_delay(TM::kMaxBackoffMultiplier) == std::chrono::hours{24 * 7});
    CHECK(TM::next_attempt_delay(100000) <= std::chrono::hours{24 * 7});
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

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Transmission: try_send leaves the queue alone with nothing to do",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();

    SECTION("telemetry disabled") {
        tm.set_enabled(true);
        tm.record_session();
        tm.set_enabled(false);
        tm.try_send();
        REQUIRE(tm.queue_size() == 1);
    }
    SECTION("an empty queue") {
        tm.set_enabled(true);
        tm.try_send();
        REQUIRE(tm.queue_size() == 0);
    }
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Transmission: build_batch copies up to kMaxBatchSize events",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    REQUIRE(tm.build_batch().is_array());
    REQUIRE(tm.build_batch().empty());

    tm.set_enabled(true);
    tm.record_session();
    tm.record_print_outcome("completed", 3600, 10, 2500.0f, "PLA", 215, 60);
    auto batch = tm.build_batch();
    REQUIRE(batch.size() == 2);
    REQUIRE(tm.queue_size() == 2); // building a batch leaves the queue intact
    for (const auto& event : batch) {
        CHECK(event.contains("schema_version"));
        CHECK(event.contains("device_id"));
        CHECK(event.contains("timestamp"));
    }
    CHECK(batch[0]["event"] == "session");
    CHECK(batch[1]["event"] == "print_outcome");

    while (tm.queue_size() < TelemetryManager::kMaxBatchSize + 10) {
        tm.record_session();
    }
    CHECK(tm.build_batch().size() == TelemetryManager::kMaxBatchSize);
}

TEST_CASE("Transmission: endpoint URL is HTTPS", "[telemetry][transmission]") {
    // Device telemetry must never leave over plaintext.
    const std::string url(TelemetryManager::kEndpointUrl);
    REQUIRE(url.find("https://") == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture, "Transmission: remove_sent_events drops from the front",
                 "[telemetry][transmission]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.record_session();
    tm.record_session();
    tm.record_print_outcome("completed", 600, 5, 300.0f, "PLA", 200, 60);
    tm.record_session();
    tm.record_session();
    REQUIRE(tm.queue_size() == 5);

    tm.remove_sent_events(0);
    REQUIRE(tm.queue_size() == 5);

    tm.remove_sent_events(3);
    REQUIRE(tm.queue_size() == 2);
    auto snapshot = tm.get_queue_snapshot();
    CHECK(snapshot[0]["event"] == "session");
    CHECK(snapshot[1]["event"] == "session");

    // More than the queue holds clears it
    tm.remove_sent_events(100);
    CHECK(tm.queue_size() == 0);
}

// ============================================================================
// Auto-send Scheduler [telemetry][scheduler]
// ============================================================================

/**
 * @brief TelemetryTestFixture plus a live LVGL, for the auto-send timers
 *
 * start_auto_send() returns early unless telemetry is enabled, and the timers
 * it then creates need LVGL running.
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
    tm.stop_auto_send(); // a stop before any start is harmless
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

    // A second stop must see null pointers, not freed timers.
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
// Update Failed Event [telemetry][update]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update failed event: envelope and the fields each failure carries",
                 "[telemetry][update]") {
    auto& tm = TelemetryManager::instance();

    SECTION("not recorded while telemetry is disabled") {
        tm.set_enabled(false);
        tm.record_update_failure("download_failed", "0.14.0", "pi");
        REQUIRE(tm.queue_size() == 0);
    }

    struct Row {
        const char* reason;
        const char* platform;
        int http_code; // -1: omitted
        int file_size;
        int exit_code;
    };
    const Row rows[] = {
        {"download_failed", "ad5m", -1, -1, -1},
        {"corrupt_download", "pi", 200, 1048576, -1},
        {"install_failed", "ad5m", -1, -1, 127},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.reason) {
            tm.set_enabled(true);
            tm.record_update_failure(r.reason, "0.14.0", r.platform, r.http_code, r.file_size,
                                     r.exit_code);
            REQUIRE(tm.queue_size() == 1);
            const auto event = tm.get_queue_snapshot()[0];

            CHECK(event["schema_version"] == TelemetryManager::kSchemaVersion);
            CHECK(event["event"] == "update_failed");
            CHECK(event.contains("device_id"));
            CHECK(event.contains("timestamp"));
            // version is the target; from_version and app_version the running build
            CHECK(event["from_version"] == HELIX_VERSION);
            CHECK(event["app_version"] == HELIX_VERSION);
            CHECK(event["reason"] == r.reason);
            CHECK(event["version"] == "0.14.0");
            CHECK(event["platform"] == r.platform);
            const std::pair<const char*, int> optional[] = {
                {"http_code", r.http_code}, {"file_size", r.file_size}, {"exit_code", r.exit_code}};
            for (const auto& [key, value] : optional) {
                INFO("field: " << key);
                if (value < 0) {
                    CHECK_FALSE(event.contains(key));
                } else {
                    CHECK(event[key] == value);
                }
            }
        }
    }
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

// ============================================================================
// Update Success Event [telemetry][update]
// ============================================================================

namespace {

/// The flag file the installer leaves for the next boot after an update.
const std::string kUpdateFlag = json{
    {"version", "0.14.0"},
    {"from_version", "0.13.4"},
    {"platform", "pi"},
    {"timestamp",
     "2026-02-26T12:00:00Z"}}.dump();

} // namespace

// check_previous_update() reports the update named by update_success.json and
// removes the file in every case, so a stale flag can never report twice.
TEST_CASE_METHOD(TelemetryTestFixture,
                 "Update success: the flag file is reported once, then removed",
                 "[telemetry][update]") {
    struct Row {
        const char* name;
        bool enabled;
        const char* flag; // nullptr: no flag file
        bool reported;
    };
    const Row rows[] = {
        {"a valid flag is reported", true, kUpdateFlag.c_str(), true},
        {"no flag file is a no-op", true, nullptr, false},
        {"telemetry off discards it", false, kUpdateFlag.c_str(), false},
        {"a malformed flag is discarded", true, "not valid json {{{{", false},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto& tm = TelemetryManager::instance();
            tm.set_enabled(r.enabled);
            if (r.flag) {
                write_file("update_success.json", r.flag);
            }

            tm.check_previous_update();

            CHECK_FALSE(fs::exists(temp_dir() / "update_success.json"));
            if (!r.reported) {
                CHECK(tm.queue_size() == 0);
                continue;
            }
            REQUIRE(tm.queue_size() == 1);
            const auto event = tm.get_queue_snapshot()[0];
            CHECK(event["schema_version"] == TelemetryManager::kSchemaVersion);
            CHECK(event["event"] == "update_success");
            CHECK(event["version"] == "0.14.0");
            CHECK(event["from_version"] == "0.13.4");
            CHECK(event["platform"] == "pi");
            CHECK(event.contains("device_id"));
            CHECK(event.contains("timestamp"));
        }
    }
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

// A crash.txt beside update_success.json is the expected restart that ends an
// update, not a real crash: no event, both files left for their owners
// (Application rotates crash.txt via consume_crash_file, check_previous_update
// consumes the flag), and had_update_restart() tells Application which it was.
TEST_CASE_METHOD(TelemetryTestFixture,
                 "Post-update crash: an update restart is not reported as a crash",
                 "[telemetry][update][crash]") {
    struct Row {
        const char* name;
        bool update_flag;
        bool crash_file;
        bool crash_event;
        bool update_restart;
    };
    const Row rows[] = {
        {"crash beside an update flag", true, true, false, true},
        {"crash with no update flag", false, true, true, false},
        {"update flag with no crash (clean update)", true, false, false, false},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto& tm = TelemetryManager::instance();
            tm.set_enabled(true);
            REQUIRE_FALSE(tm.had_update_restart());
            if (r.update_flag) {
                write_file("update_success.json", kUpdateFlag);
            }
            if (r.crash_file) {
                write_file("crash.txt", "signal:6\nname:SIGABRT\nversion:0.13.4\n");
            }

            tm.check_previous_crash();

            CHECK(tm.had_update_restart() == r.update_restart);
            CHECK(tm.queue_size() == (r.crash_event ? 1u : 0u));
            if (r.crash_event) {
                CHECK(tm.get_queue_snapshot()[0]["event"] == "crash");
            }
            if (r.update_flag && r.crash_file) {
                CHECK(fs::exists(temp_dir() / "crash.txt"));
                CHECK(fs::exists(temp_dir() / "update_success.json"));
            }
        }
    }
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
                 "Bundle consent: a consented bundle reports its crash exactly once",
                 "[telemetry][crash][consent]") {
    struct Row {
        const char* name;
        bool enabled;
        bool crash_file;
        bool update_flag;
        bool enqueued;
        size_t queued;
    };
    const Row rows[] = {
        // Consent bypasses the opt-in that discarded the crash at boot
        {"telemetry off", false, true, false, true, 1},
        // The boot path already queued it; consent adds no second copy
        {"telemetry on", true, true, false, false, 1},
        {"no crash file", false, false, false, false, 0},
        {"an update restart, even with consent", false, true, true, false, 0},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto& tm = TelemetryManager::instance();
            tm.set_enabled(r.enabled);
            if (r.update_flag) {
                write_file("update_success.json", "{\"version\":\"0.14.0\"}");
            }
            if (r.crash_file) {
                write_file("crash.txt", "signal:11\nname:SIGSEGV\nversion:0.14.0\n");
                if (!r.update_flag) {
                    tm.check_previous_crash();
                    REQUIRE(tm.queue_size() == (r.enabled ? 1u : 0u));
                }
            }

            CHECK(tm.enqueue_crash_event_unconditional() == r.enqueued);
            REQUIRE(tm.queue_size() == r.queued);
            if (r.queued > 0) {
                CHECK(tm.get_queue_snapshot()[0]["event"] == "crash");
                CHECK(tm.get_queue_snapshot()[0]["signal_name"] == "SIGSEGV");
            }
        }
    }
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

TEST_CASE("overlay_telemetry_id: object name, then lifecycle name, then 'unnamed'",
          "[telemetry][overlay_id]") {
    struct Row {
        const char* object_name;
        const char* lifecycle_name;
        const char* expected;
    };
    const Row rows[] = {
        {"fan_control_overlay", "Fan Control", "fan_control_overlay"},
        {"fan_control_overlay", nullptr, "fan_control_overlay"},
        {nullptr, "Fan Control", "Fan Control"},
        {"", "Fan Control", "Fan Control"},
        {"", nullptr, "unnamed"},
        {nullptr, nullptr, "unnamed"},
        {nullptr, "", "unnamed"},
    };
    for (const Row& r : rows) {
        INFO("object: " << (r.object_name ? r.object_name : "(null)")
                        << " lifecycle: " << (r.lifecycle_name ? r.lifecycle_name : "(null)"));
        CHECK(helix::overlay_telemetry_id(r.object_name, r.lifecycle_name) == r.expected);
    }
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

TEST_CASE_METHOD(TelemetryTestFixture, "record_error records one event per category",
                 "[telemetry][error]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_error("moonraker_api", "timeout", "get_printer_objects");
    REQUIRE(tm.queue_size() == 1);
    const auto event = tm.get_queue_snapshot()[0];
    CHECK(event["event"] == "error_encountered");
    CHECK(event["category"] == "moonraker_api");
    CHECK(event["code"] == "timeout");
    CHECK(event["context"] == "get_printer_objects");
    CHECK(event.contains("uptime_sec"));

    // A second error in the same category is rate-limited; another category is not
    tm.record_error("moonraker_api", "timeout", "ctx2");
    CHECK(tm.queue_size() == 1);
    tm.record_error("websocket", "connection_refused", "reconnect");
    CHECK(tm.queue_size() == 2);

    // Any identifier-shaped category: digits and underscores, up to 32 characters
    tm.record_error("a_1", "code", "ctx");
    tm.record_error(std::string(32, 'x'), "code", "ctx");
    CHECK(tm.queue_size() == 4);
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

TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: the snapshot counts frames and drops over 33ms",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("temperature");
    tm.record_frame_time(10000); // ok
    tm.record_frame_time(35000); // dropped
    tm.record_frame_time(50000); // dropped

    tm.record_performance_snapshot();
    REQUIRE(tm.queue_size() == 1);
    const auto event = tm.get_queue_snapshot()[0];
    CHECK(event["event"] == "performance_snapshot");
    CHECK(event.contains("frame_time_p50_ms"));
    CHECK(event.contains("frame_time_p95_ms"));
    CHECK(event.contains("frame_time_p99_ms"));
    CHECK(event["dropped_frame_count"] == 2);
    CHECK(event["total_frame_count"] == 3);
    CHECK_FALSE(event.contains("task_handler_max_ms"));
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

// Frames under 500us are idle loop passes, not renders: they are not recorded,
// and a window of nothing else produces no snapshot at all.
TEST_CASE_METHOD(TelemetryTestFixture, "Frame time: idle frames under 500us are not counted",
                 "[telemetry][frame]") {
    struct Row {
        const char* name;
        std::vector<uint32_t> frames_us;
        int expected_total; // 0: no event
    };
    const Row rows[] = {
        {"idle frames beside a render", {100, 300, 8000}, 1},
        {"the 500us boundary counts", {499, 500, 501}, 2},
        {"only idle frames", {100, 200}, 0},
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION(r.name) {
            auto& tm = TelemetryManager::instance();
            tm.set_enabled(true);
            tm.notify_panel_changed("status");
            for (auto us : r.frames_us) {
                tm.record_frame_time(us);
            }

            tm.record_performance_snapshot();
            if (r.expected_total == 0) {
                CHECK(tm.queue_size() == 0);
            } else {
                REQUIRE(tm.queue_size() == 1);
                CHECK(tm.get_queue_snapshot()[0]["total_frame_count"] == r.expected_total);
            }
        }
    }
}

// ============================================================================
// Periodic Snapshots [telemetry][snapshot]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture, "Snapshot: panel_usage carries snapshot_seq and is_shutdown",
                 "[telemetry][snapshot]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    tm.notify_panel_changed("status");

    SECTION("a direct record is not a shutdown snapshot") {
        tm.record_panel_usage();
        REQUIRE(tm.queue_size() == 1);
        const auto event = tm.get_queue_snapshot()[0];
        CHECK(event["event"] == "panel_usage");
        CHECK(event.contains("snapshot_seq"));
        CHECK(event["is_shutdown"] == false);
    }

    SECTION("snapshot_seq increments on each periodic fire") {
        tm.fire_periodic_snapshot();
        tm.fire_periodic_snapshot();
        // Each fire produces panel_usage + connection_stability + performance_snapshot
        REQUIRE(tm.queue_size() >= 4);
        std::vector<int> seqs;
        for (const auto& ev : tm.get_queue_snapshot()) {
            if (ev.value("event", "") == "panel_usage") {
                seqs.push_back(ev.value("snapshot_seq", -1));
            }
        }
        CHECK(seqs == std::vector<int>{0, 1});
    }
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
                 "Queue: at capacity a delta merges into the queued one of its kind",
                 "[telemetry][queue][delta]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session(); // the oldest event, and the one a drop-oldest would take
    tm.notify_connection_state_changed(2);
    tm.notify_panel_changed("home");
    TelemetryManagerTestAccess::advance_clock(tm, 1h);
    tm.fire_periodic_snapshot(); // seq 0
    fill_queue_with_sessions(tm, TelemetryManager::kMaxQueueSize);

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

    fill_queue_behind_panel_row(tm, "home");

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

    fill_queue_behind_panel_row(tm, "home");
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
    fill_queue_with_sessions(tm, TelemetryManager::kMaxQueueSize);
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

    fill_queue_behind_panel_row(tm, "home");
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

    fill_queue_behind_panel_row(tm, "home");
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

    fill_queue_with_sessions(tm, TelemetryManager::kMaxQueueSize);
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
    fill_queue_with_sessions(tm, TelemetryManager::kMaxQueueSize);

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

TEST_CASE_METHOD(TelemetryTestFixture, "Settings changes: pending changes flush as one event",
                 "[telemetry][settings]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.flush_settings_changes(); // nothing pending: no event
    REQUIRE(tm.queue_size() == 0);

    tm.notify_setting_changed("theme", "dark", "light");
    tm.notify_setting_changed("brightness", "80", "60");
    tm.flush_settings_changes();

    REQUIRE(tm.queue_size() == 1);
    const auto event = tm.get_queue_snapshot()[0];
    CHECK(event["event"] == "settings_changes");
    REQUIRE(event["changes"].size() == 2);
    CHECK(event["changes"][0]["setting"] == "theme");
    CHECK(event["changes"][0]["old_value"] == "dark");
    CHECK(event["changes"][0]["new_value"] == "light");
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

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Envelope: boot_id is 16 hex, stable within a boot, new per init",
                 "[telemetry][envelope]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.record_session();
    tm.record_error("moonraker_api", "timeout", "ctx");
    auto queue = tm.get_queue_snapshot();
    REQUIRE(queue.size() == 2);
    const auto boot_id = queue[0]["boot_id"].get<std::string>();
    REQUIRE(boot_id.size() == 16);
    CHECK(std::all_of(boot_id.begin(), boot_id.end(),
                      [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    CHECK(queue[1]["boot_id"] == boot_id);

    tm.shutdown();
    tm.init(temp_dir().string());
    tm.set_enabled(true);
    tm.clear_queue();
    tm.record_session();
    CHECK(tm.get_queue_snapshot()[0]["boot_id"] != boot_id);
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
