// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v20 -> v21 config migration, which re-scopes four settings to
// match what they actually describe:
//
//   /appearance/toolhead_style  -> per-printer, fanned out to EVERY printer
//   /detection/policy_u1        -> per-printer, fanned out to EVERY printer
//   <printer>/scanner/*         -> global, taken from the ACTIVE printer only
//   /console/filter_user_*      -> unchanged (it became the global layer of a
//                                  two-layer read; nothing moves)
//
// The scanner collapse is deliberately lossy: N per-printer copies cannot
// become one without discarding N-1 of them, so "the active printer's copy
// wins" is the contract that has to be pinned, not an implementation detail.
//
// The migration is a static function in config.cpp, so it is driven through the
// public Config::init() path exactly as the v18 tests do. A sandboxed
// HELIX_CONFIG_DIR keeps backup-restore search paths inside the temp dir.

#include "config.h"
#include "test_helpers/unique_temp_dir.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class MigrationV21Fixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    void SetUp() {
        temp_dir = helix::test::unique_temp_dir("helix_migration_v21_test");
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);

        config_path = temp_dir + "/settings.json";
    }

    void TearDown() {
        fs::remove_all(temp_dir);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        config.clear_path();
    }

    /// Write a settings.json, then init Config from it so the real versioned
    /// migrations run.
    void write_and_init(const json& contents) {
        std::ofstream f(config_path);
        f << contents.dump(2);
        f.close();
        config.init(config_path);
    }

    static std::string p(const char* printer, const std::string& leaf) {
        return std::string("/printers/") + printer + "/" + leaf;
    }

  public:
    MigrationV21Fixture() {
        SetUp();
    }
    ~MigrationV21Fixture() {
        TearDown();
    }
};

/// A v20 config with two printers, both carrying divergent scanner settings,
/// plus the mixed-map non-printer sibling that every printer iteration has to
/// skip.
json v20_two_printers() {
    return json{
        {"config_version", 20},
        {"active_printer_id", "voronv2"},
        {"appearance", {{"toolhead_style", 5}, {"show_widget_labels", true}}},
        {"detection", {{"enabled", false}, {"policy_u1", 1}}},
        {"console", {{"filter_temps", false}, {"filter_user_add", json::array({"prefix:KEEP"})}}},
        {"printers",
         {{"show_printer_switcher", true},
          {"voronv2",
           {{"moonraker_host", "192.168.1.112"},
            {"scanner",
             {{"usb_vendor_product", "1a2b:3c4d"},
              {"usb_device_name", "Active Scanner"},
              {"bt_address", "AA:BB:CC:DD:EE:FF"},
              {"keymap", "qwertz"}}}}},
          {"prusamk4",
           {{"moonraker_host", "192.168.1.113"},
            {"scanner",
             {{"usb_vendor_product", "9999:8888"},
              {"usb_device_name", "Inactive Scanner"},
              {"keymap", "azerty"}}}}}}}};
}

} // namespace

// ============================================================================
// Positive — all four blocks, one v20 config
// ============================================================================

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: re-scopes all four settings from a v20 config",
                 "[config][migration]") {
    write_and_init(v20_two_printers());

    REQUIRE(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);

    // 1. toolhead_style copied to EVERY printer, root key retired.
    CHECK(config.get<int>(p("voronv2", "appearance/toolhead_style"), -1) == 5);
    CHECK(config.get<int>(p("prusamk4", "appearance/toolhead_style"), -1) == 5);
    CHECK_FALSE(config.exists("/appearance/toolhead_style"));
    // The sibling that did NOT change scope is still at the root.
    CHECK(config.get<bool>("/appearance/show_widget_labels", false) == true);

    // 2. policy_u1 copied to EVERY printer, root key retired; the master
    //    toggle beside it deliberately stays global.
    CHECK(config.get<int>(p("voronv2", "detection/policy_u1"), -1) == 1);
    CHECK(config.get<int>(p("prusamk4", "detection/policy_u1"), -1) == 1);
    CHECK_FALSE(config.exists("/detection/policy_u1"));
    CHECK(config.get<bool>("/detection/enabled", true) == false);

    // 3. Scanner lifted from the ACTIVE printer; both printer nodes dropped.
    CHECK(config.get<std::string>("/scanner/usb_vendor_product", "") == "1a2b:3c4d");
    CHECK(config.get<std::string>("/scanner/usb_device_name", "") == "Active Scanner");
    CHECK(config.get<std::string>("/scanner/bt_address", "") == "AA:BB:CC:DD:EE:FF");
    CHECK(config.get<std::string>("/scanner/keymap", "") == "qwertz");
    CHECK_FALSE(config.exists(p("voronv2", "scanner")));
    CHECK_FALSE(config.exists(p("prusamk4", "scanner")));

    // 4. Console user filters are untouched — they became the global layer.
    const std::vector<std::string> kept_filters{"prefix:KEEP"};
    CHECK(config.get<std::vector<std::string>>("/console/filter_user_add", {}) == kept_filters);
    CHECK_FALSE(config.exists(p("voronv2", "console/filter_user_add")));
    CHECK(config.get<bool>("/console/filter_temps", true) == false);

    // Unrelated per-printer data survived the rewrite of those nodes.
    CHECK(config.get<std::string>(p("voronv2", "moonraker_host"), "") == "192.168.1.112");
    CHECK(config.get<std::string>(p("prusamk4", "moonraker_host"), "") == "192.168.1.113");
}

// ============================================================================
// The lossy half — which printer's scanner survives
// ============================================================================

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: only the active printer's scanner values are taken",
                 "[config][migration]") {
    // The positive case above proves the last-sorting printer wins when it is
    // active. The complement is this one: the active printer holds a PARTIAL
    // set, and the gap must stay a gap rather than being filled in from the
    // printer next door — a merge across printers would silently pair one
    // machine's BT address with another's USB id.
    json v20 = v20_two_printers();
    v20["active_printer_id"] = "prusamk4";
    write_and_init(v20);

    CHECK(config.get<std::string>("/scanner/usb_vendor_product", "") == "9999:8888");
    CHECK(config.get<std::string>("/scanner/usb_device_name", "") == "Inactive Scanner");
    CHECK(config.get<std::string>("/scanner/keymap", "") == "azerty");
    // prusamk4 has no bt_address, and the inactive printer's must NOT fill in.
    CHECK_FALSE(config.exists("/scanner/bt_address"));

    // The non-active printer's node is dropped all the same.
    CHECK_FALSE(config.exists(p("voronv2", "scanner")));
    CHECK_FALSE(config.exists(p("prusamk4", "scanner")));
}

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: a dangling active_printer_id falls back to a real printer",
                 "[config][migration]") {
    // Migrations run BEFORE init() resolves the active printer, so the fallback
    // has to be re-applied here. Naming a printer that does not exist must not
    // silently discard every scanner setting.
    json v20 = v20_two_printers();
    v20["active_printer_id"] = "deleted-printer";
    write_and_init(v20);

    // Resolves to the first printer OBJECT — never to show_printer_switcher.
    CHECK(config.get<std::string>("/scanner/usb_vendor_product", "") == "9999:8888");
    CHECK(config.get<bool>("/printers/show_printer_switcher", false) == true);
}

// ============================================================================
// The mixed printers map
// ============================================================================

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: non-printer siblings in the printers map are not touched",
                 "[config][migration]") {
    write_and_init(v20_two_printers());

    // show_printer_switcher is a bool sibling of the printer objects. A fan-out
    // that skipped the is_object() filter would either throw or turn it into an
    // object holding a toolhead style.
    REQUIRE(config.exists("/printers/show_printer_switcher"));
    CHECK(config.get<bool>("/printers/show_printer_switcher", false) == true);
    CHECK_FALSE(config.exists("/printers/show_printer_switcher/appearance/toolhead_style"));

    auto ids = config.get_printer_ids();
    std::sort(ids.begin(), ids.end());
    const std::vector<std::string> expected_ids{"prusamk4", "voronv2"};
    CHECK(ids == expected_ids);
}

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: a printers map holding ONLY non-printer keys keeps the "
                 "root values",
                 "[config][migration]") {
    // The shipped settings.json.template shape. With no printer to fan out
    // into, deleting the root key would destroy the setting with nothing put in
    // its place, so it has to stay for a later boot to migrate.
    json v20 = {{"config_version", 20},
                {"active_printer_id", ""},
                {"appearance", {{"toolhead_style", 3}}},
                {"detection", {{"policy_u1", 0}}},
                {"printers", {{"show_printer_switcher", false}}}};
    REQUIRE_NOTHROW(write_and_init(v20));

    CHECK(config.get<int>("/appearance/toolhead_style", -1) == 3);
    CHECK(config.get<int>("/detection/policy_u1", -1) == 0);
    CHECK_FALSE(config.exists("/scanner/keymap"));
}

// ============================================================================
// Idempotence and the already-migrated skip
// ============================================================================

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: is idempotent — re-initing a migrated config changes "
                 "nothing",
                 "[config][migration]") {
    write_and_init(v20_two_printers());
    REQUIRE(config.save());

    std::ifstream first_read(config_path);
    json after_first;
    first_read >> after_first;
    first_read.close();

    // Second pass over the file the first pass produced.
    Config second;
    second.init(config_path);
    REQUIRE(second.save());

    std::ifstream second_read(config_path);
    json after_second;
    second_read >> after_second;
    second_read.close();
    second.clear_path();

    CHECK(after_first == after_second);
}

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: skips a config already stamped at the current version",
                 "[config][migration]") {
    // A config that is already v21 must not have its per-printer values
    // clobbered by a stale root key, nor its scanner node re-collapsed.
    json v21 = {{"config_version", CURRENT_CONFIG_VERSION},
                {"active_printer_id", "voronv2"},
                {"appearance", {{"toolhead_style", 7}}}, // stale leftover; must be ignored
                {"scanner", {{"keymap", "qwerty"}}},
                {"printers",
                 {{"voronv2",
                   {{"appearance", {{"toolhead_style", 2}}}, {"scanner", {{"keymap", "azerty"}}}}},
                  {"prusamk4", {{"moonraker_host", "192.168.1.113"}}}}}};
    write_and_init(v21);

    CHECK(config.get<int>(p("voronv2", "appearance/toolhead_style"), -1) == 2);
    // Not fanned out — the migration never ran.
    CHECK_FALSE(config.exists(p("prusamk4", "appearance/toolhead_style")));
    CHECK(config.get<int>("/appearance/toolhead_style", -1) == 7);
    CHECK(config.get<std::string>("/scanner/keymap", "") == "qwerty");
    CHECK(config.get<std::string>(p("voronv2", "scanner/keymap"), "") == "azerty");
}

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: an existing per-printer value is never overwritten",
                 "[config][migration]") {
    // Downgrade-then-upgrade, or a hand-edited config: the printer already has
    // its own toolhead style. The root value must not stomp it, while printers
    // without one still get the copy.
    json v20 = {{"config_version", 20},
                {"active_printer_id", "voronv2"},
                {"appearance", {{"toolhead_style", 5}}},
                {"printers",
                 {{"voronv2", {{"appearance", {{"toolhead_style", 2}}}}},
                  {"prusamk4", {{"moonraker_host", "192.168.1.113"}}}}}};
    write_and_init(v20);

    CHECK(config.get<int>(p("voronv2", "appearance/toolhead_style"), -1) == 2);
    CHECK(config.get<int>(p("prusamk4", "appearance/toolhead_style"), -1) == 5);
    CHECK_FALSE(config.exists("/appearance/toolhead_style"));
}

// ============================================================================
// Totality — a broken config must still migrate and stamp
// ============================================================================

TEST_CASE_METHOD(MigrationV21Fixture,
                 "Config migration v21: survives missing keys, null leaves and wrong types",
                 "[config][migration]") {
    // Migration failure leaves the config unstamped and retries every boot, so
    // none of these may throw.
    SECTION("no printers map at all") {
        json v20 = {{"config_version", 20},
                    {"active_printer_id", "voronv2"},
                    {"appearance", {{"toolhead_style", 4}}},
                    {"detection", {{"policy_u1", 0}}}};
        REQUIRE_NOTHROW(write_and_init(v20));
        CHECK(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
        // Nothing to fan out into, so the root values are kept for a later boot.
        CHECK(config.get<int>("/appearance/toolhead_style", -1) == 4);
    }

    SECTION("none of the four keys present") {
        json v20 = {{"config_version", 20},
                    {"active_printer_id", "voronv2"},
                    {"printers", {{"voronv2", {{"moonraker_host", "192.168.1.112"}}}}}};
        REQUIRE_NOTHROW(write_and_init(v20));
        CHECK(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
        CHECK_FALSE(config.exists("/scanner/keymap"));
        CHECK_FALSE(config.exists(p("voronv2", "appearance/toolhead_style")));
    }

    SECTION("null leaves where values are expected") {
        json v20 = {{"config_version", 20},
                    {"active_printer_id", "voronv2"},
                    {"appearance", {{"toolhead_style", nullptr}}},
                    {"detection", {{"policy_u1", nullptr}}},
                    {"printers", {{"voronv2", {{"scanner", {{"keymap", nullptr}}}}}}}};
        REQUIRE_NOTHROW(write_and_init(v20));
        CHECK(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
        // A null is "absent", so nothing is copied and no null is propagated.
        CHECK_FALSE(config.exists(p("voronv2", "appearance/toolhead_style")));
        CHECK_FALSE(config.exists("/scanner/keymap"));
    }

    SECTION("printers map is not an object") {
        json v20 = {{"config_version", 20},
                    {"active_printer_id", "voronv2"},
                    {"appearance", {{"toolhead_style", 4}}},
                    {"printers", json::array({"voronv2"})}};
        REQUIRE_NOTHROW(write_and_init(v20));
        CHECK(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
    }

    SECTION("wrong-typed nodes where objects are expected") {
        // /appearance is a string, /detection a number, and a printer's scanner
        // node is a bare string rather than an object.
        json v20 = {{"config_version", 20},
                    {"active_printer_id", "voronv2"},
                    {"appearance", "not-an-object"},
                    {"detection", 42},
                    {"printers", {{"voronv2", {{"scanner", "not-an-object"}}}}}};
        REQUIRE_NOTHROW(write_and_init(v20));
        CHECK(config.get<int>("/config_version") == CURRENT_CONFIG_VERSION);
        // The junk scanner node is dropped along with the well-formed ones.
        CHECK_FALSE(config.exists(p("voronv2", "scanner")));
    }

    SECTION("a printer entry is not an object") {
        json v20 = {{"config_version", 20},
                    {"active_printer_id", "voronv2"},
                    {"appearance", {{"toolhead_style", 4}}},
                    {"printers",
                     {{"voronv2", {{"moonraker_host", "192.168.1.112"}}},
                      {"show_printer_switcher", true},
                      {"stray_number", 7},
                      {"stray_list", json::array({1, 2})}}}};
        REQUIRE_NOTHROW(write_and_init(v20));
        CHECK(config.get<int>(p("voronv2", "appearance/toolhead_style"), -1) == 4);
        CHECK(config.get<bool>("/printers/show_printer_switcher", false) == true);
        CHECK(config.get<int>("/printers/stray_number", -1) == 7);
    }
}
