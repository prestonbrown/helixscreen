// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_name_sync.cpp
 * @brief Unit tests for PrinterNameSync — name resolution and DB write-back
 *
 * Covers all branches of PrinterNameSync::resolve():
 *   - Local config already set → no DB reads
 *   - Mainsail has name → seeds from Mainsail
 *   - Mainsail missing, Fluidd has name → seeds from Fluidd
 *   - Both DBs empty, hostname available → seeds from hostname
 *   - Everything empty → no change
 *   - "unknown" hostname treated as empty
 *   - write_back() with null/empty args is safe
 *   - resolve() with null API falls back to hostname
 *
 * The resolve() implementation captures the passed-in api pointer in all lambda
 * callbacks, so every branch is fully testable with MoonrakerAPIMock.
 */

#include "../test_helpers/config_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_name_sync.h"
#include "printer_state.h"
#include "wizard_config_paths.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

// ============================================================================
// Test Fixture
// ============================================================================

class PrinterNameSyncFixture {
  public:
    PrinterNameSyncFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        lv_init_safe();
        state_.init_subjects(false);
        api_ = std::make_unique<MoonrakerAPIMock>(client_, state_);

        // Ensure Config singleton exists and clear printer name before each test
        config_ = Config::get_instance();
        clear_name();
    }

    ~PrinterNameSyncFixture() {
        // Drain any remaining callbacks to avoid cross-test pollution
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        clear_name();
    }

    void clear_name() {
        if (config_) {
            config_->set<std::string>(config_->df() + wizard::PRINTER_NAME, "");
        }
    }

    std::string get_name() {
        if (!config_)
            return "";
        return config_->get<std::string>(config_->df() + wizard::PRINTER_NAME, "");
    }

    void drain() {
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    }

  protected:
    MoonrakerClientMock client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPIMock> api_;
    Config* config_ = nullptr;
};

// ============================================================================
// Tests
// ============================================================================

TEST_CASE_METHOD(PrinterNameSyncFixture, "resolve: local name already set skips DB reads",
                 "[name-sync]") {
    // Pre-set a local config name
    config_->set<std::string>(config_->df() + wizard::PRINTER_NAME, "My Local Printer");

    // Set a Mainsail DB value that should NOT be used
    client_.mock_db_set("mainsail", "general.printername", "Should Not Be Used");

    PrinterNameSync::resolve(api_.get(), "hostname.local");
    drain();

    // Local name wins — must remain unchanged
    REQUIRE(get_name() == "My Local Printer");
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "resolve: Mainsail has name seeds from Mainsail",
                 "[name-sync]") {
    client_.mock_db_set("mainsail", "general.printername", std::string("Mainsail Printer"));

    PrinterNameSync::resolve(api_.get(), "fallback.local");
    drain();

    REQUIRE(get_name() == "Mainsail Printer");
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "resolve: Mainsail missing Fluidd has name seeds Fluidd",
                 "[name-sync]") {
    // Only Fluidd is in the mock DB; Mainsail lookup will trigger the error callback,
    // which then queries Fluidd via the passed-in api parameter.
    client_.mock_db_set("fluidd", "general.instanceName", std::string("Fluidd Printer"));

    PrinterNameSync::resolve(api_.get(), "fallback.local");
    drain();

    REQUIRE(get_name() == "Fluidd Printer");
}

TEST_CASE_METHOD(PrinterNameSyncFixture,
                 "resolve: both DBs empty hostname available seeds from hostname", "[name-sync]") {
    // Neither Mainsail nor Fluidd keys exist in the mock DB — both will call on_error.
    PrinterNameSync::resolve(api_.get(), "printer-hostname");
    drain();

    REQUIRE(get_name() == "printer-hostname");
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "resolve: everything empty no change", "[name-sync]") {
    // No DB values, empty hostname
    PrinterNameSync::resolve(api_.get(), "");
    drain();

    REQUIRE(get_name() == "");
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "resolve: 'unknown' hostname treated as empty",
                 "[name-sync]") {
    // "unknown" hostname must not be seeded
    PrinterNameSync::resolve(api_.get(), "unknown");
    drain();

    REQUIRE(get_name() == "");
}

TEST_CASE_METHOD(PrinterNameSyncFixture,
                 "resolve: Mainsail empty string falls through and Fluidd wins", "[name-sync]") {
    // Mainsail key exists but value is empty string — resolve() treats this as "no name"
    // and falls through to Fluidd via the captured api pointer.
    client_.mock_db_set("mainsail", "general.printername", std::string(""));
    client_.mock_db_set("fluidd", "general.instanceName", std::string("Workshop Printer"));

    PrinterNameSync::resolve(api_.get(), "hostname-fallback");
    drain();

    REQUIRE(get_name() == "Workshop Printer");
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "write_back: null API is safe no crash", "[name-sync]") {
    // Must not crash when api is nullptr
    REQUIRE_NOTHROW(PrinterNameSync::write_back(nullptr, "Some Name"));
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "write_back: empty name is no-op no crash",
                 "[name-sync]") {
    // Must not crash when name is empty
    REQUIRE_NOTHROW(PrinterNameSync::write_back(api_.get(), ""));
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "resolve: null API falls back to hostname",
                 "[name-sync]") {
    // When api is nullptr and hostname is valid, hostname is used directly
    PrinterNameSync::resolve(nullptr, "my-printer");
    // resolve() with null api does NOT use queue_update — it sets config synchronously
    // (see printer_name_sync.cpp lines 34-39)

    REQUIRE(get_name() == "my-printer");
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "resolve: null API with unknown hostname no change",
                 "[name-sync]") {
    PrinterNameSync::resolve(nullptr, "unknown");
    REQUIRE(get_name() == "");
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "write_back: writes to both Mainsail and Fluidd",
                 "[name-sync]") {
    PrinterNameSync::write_back(api_.get(), "My New Printer");

    // Mock post_item stores values — verify both namespaces were written
    nlohmann::json mainsail_val;
    bool mainsail_found = false;
    api_->database_get_item(
        "mainsail", "general.printername",
        [&](const nlohmann::json& v) {
            mainsail_val = v;
            mainsail_found = true;
        },
        nullptr);
    REQUIRE(mainsail_found);
    REQUIRE(mainsail_val == "My New Printer");

    nlohmann::json fluidd_val;
    bool fluidd_found = false;
    api_->database_get_item(
        "fluidd", "general.instanceName",
        [&](const nlohmann::json& v) {
            fluidd_val = v;
            fluidd_found = true;
        },
        nullptr);
    REQUIRE(fluidd_found);
    REQUIRE(fluidd_val == "My New Printer");
}

TEST_CASE_METHOD(PrinterNameSyncFixture,
                 "resolve: a name that lands after a switch goes to the printer it was asked for",
                 "[name-sync][multi-printer]") {
    const nlohmann::json saved_data = ConfigTestAccess::data(*config_);
    const std::string saved_active = ConfigTestAccess::active_printer_id(*config_);
    config_->add_printer("name-a", nlohmann::json::object());
    config_->add_printer("name-b", nlohmann::json::object());
    REQUIRE(config_->set_active_printer("name-a"));
    get_printer_state().init_subjects(false);

    client_.mock_db_set("mainsail", "general.printername", std::string("Printer A"));
    PrinterNameSync::resolve(api_.get(), "a.local");

    // The switch lands before the queued name does.
    REQUIRE(config_->set_active_printer("name-b"));
    get_printer_state().set_active_printer_name("Printer B");
    drain();

    CHECK(config_->get<std::string>("/printers/name-a/" + std::string(wizard::PRINTER_NAME), "") ==
          "Printer A");
    CHECK(config_->get<std::string>("/printers/name-b/" + std::string(wizard::PRINTER_NAME), "")
              .empty());
    CHECK(std::string(lv_subject_get_string(
              get_printer_state().get_active_printer_name_subject())) == "Printer B");

    ConfigTestAccess::data(*config_) = saved_data;
    ConfigTestAccess::active_printer_id(*config_) = saved_active;
}

TEST_CASE_METHOD(PrinterNameSyncFixture,
                 "resolve: a name that lands after its printer was deleted does not bring it back",
                 "[name-sync][multi-printer]") {
    const nlohmann::json saved_data = ConfigTestAccess::data(*config_);
    const std::string saved_active = ConfigTestAccess::active_printer_id(*config_);
    config_->add_printer("gone-a", nlohmann::json::object());
    config_->add_printer("kept-b", nlohmann::json::object());
    REQUIRE(config_->set_active_printer("gone-a"));

    client_.mock_db_set("mainsail", "general.printername", std::string("Printer A"));
    PrinterNameSync::resolve(api_.get(), "a.local");
    config_->remove_printer("gone-a");
    drain();

    CHECK_FALSE(config_->exists("/printers/gone-a"));

    ConfigTestAccess::data(*config_) = saved_data;
    ConfigTestAccess::active_printer_id(*config_) = saved_active;
}

TEST_CASE_METHOD(PrinterNameSyncFixture, "resolve: a lost connection does not seed the hostname",
                 "[name-sync][multi-printer]") {
    client_.fail_next("server.database.get_item",
                      MoonrakerError::connection_lost("server.database.get_item"));

    PrinterNameSync::resolve(api_.get(), "printer-hostname");
    drain();

    CHECK(get_name().empty());
}
