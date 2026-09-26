// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// #1162: the "preset" marker lived at the config root while every other piece
// of printer configuration sat under /printers/<id>/. Configuring a second
// printer overwrote the first printer's marker, and consumers of get_preset()
// (PanelWidgetConfig::try_populate_from_preset_seed, the wizard's step
// collapsing in wizard_step_registry.cpp) then read the wrong printer's preset.
//
// Storage is per-printer now, with lift_root_preset() in Config::init() moving
// a legacy root-level marker into the active printer. That lift is deliberately
// NOT a versioned migration — scripts/lib/installer/printer_seed.sh writes the
// root key with setdefault() on --update too, into configs whose config_version
// is already CURRENT_CONFIG_VERSION — so the lift tests below all seed at the
// current version to prove the version gate is not what drives them.
//
// Lift tests go through the public Config::init() path (lift_root_preset is a
// file-static in config.cpp), mirroring tests/unit/test_preset_filament_persistence.cpp.

#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "test_helpers/unique_temp_dir.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

// ============================================================================
// In-memory: per-printer isolation
// ============================================================================

class TwoPrinterPresetFixture : public HelixTestFixture {
  protected:
    Config config;

    TwoPrinterPresetFixture() {
        ConfigTestAccess::data(config) = {{"config_version", CURRENT_CONFIG_VERSION},
                                          {"active_printer_id", "printer-a"},
                                          {"printers",
                                           {{"printer-a", {{"moonraker_host", "192.168.1.10"}}},
                                            {"printer-b", {{"moonraker_host", "192.168.1.11"}}}}}};
        ConfigTestAccess::active_printer_id(config) = "printer-a";
    }
};

TEST_CASE_METHOD(TwoPrinterPresetFixture, "Config: two printers keep independent presets",
                 "[config][preset][multi-printer]") {
    config.set_preset("ad5m");
    REQUIRE(config.has_preset());
    REQUIRE(config.get_preset() == "ad5m");

    // Adding/configuring a second printer must not inherit — or clobber — the
    // first one's marker. Root-level storage did both.
    REQUIRE(config.set_active_printer("printer-b"));
    CHECK_FALSE(config.has_preset());
    CHECK(config.get_preset().empty());

    config.set_preset("qidi_q2");
    CHECK(config.get_preset() == "qidi_q2");

    REQUIRE(config.set_active_printer("printer-a"));
    CHECK(config.get_preset() == "ad5m");

    // Both markers land under their own printer, and nothing is left at the root.
    CHECK(config.get<std::string>("/printers/printer-a/preset", "") == "ad5m");
    CHECK(config.get<std::string>("/printers/printer-b/preset", "") == "qidi_q2");
    CHECK_FALSE(ConfigTestAccess::data(config).contains("preset"));
}

TEST_CASE_METHOD(TwoPrinterPresetFixture, "Config::clear_preset only clears the active printer",
                 "[config][preset][multi-printer]") {
    config.set_preset("ad5m");
    REQUIRE(config.set_active_printer("printer-b"));
    config.set_preset("qidi_q2");

    config.clear_preset();
    CHECK_FALSE(config.has_preset());

    REQUIRE(config.set_active_printer("printer-a"));
    CHECK(config.get_preset() == "ad5m");
}

TEST_CASE_METHOD(TwoPrinterPresetFixture, "Config::clear_preset also drops a legacy root marker",
                 "[config][preset][multi-printer]") {
    // Application::reset_wizard_state() calls clear_preset() so a wrong
    // install-time seed is recoverable. If a root marker survived, the next
    // boot's lift_root_preset() would put the preset straight back.
    ConfigTestAccess::data(config)["preset"] = "ad5x";
    config.set_preset("ad5m");

    config.clear_preset();

    CHECK_FALSE(config.has_preset());
    CHECK_FALSE(ConfigTestAccess::data(config).contains("preset"));
}

// ============================================================================
// On-disk: the legacy root-level marker lift
// ============================================================================

class RootPresetLiftFixture : public HelixTestFixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    RootPresetLiftFixture() {
        temp_dir = helix::test::unique_temp_dir("helix_root_preset_lift_test");
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        // Sandbox backup-restore search paths into the temp dir. Safe because it
        // names the same directory the config path below lives in.
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);

        config_path = temp_dir + "/settings.json";
    }

    ~RootPresetLiftFixture() override {
        fs::remove_all(temp_dir);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        config.clear_path();
    }

    void write_and_init(const json& contents) {
        std::ofstream f(config_path);
        f << contents.dump(2);
        f.close();
        config.init(config_path);
    }

    json read_back() const {
        return json::parse(std::ifstream(config_path));
    }

    // Two printers, "printer-b" active, plus whatever extra root keys are given.
    static json two_printers(const json& extra_root, const json& printer_b_extra = json::object()) {
        json printer_b = {{"moonraker_host", "192.168.1.11"}, {"wizard_completed", true}};
        printer_b.update(printer_b_extra);
        json c = {{"config_version", CURRENT_CONFIG_VERSION},
                  {"active_printer_id", "printer-b"},
                  {"printers",
                   {{"printer-a", {{"moonraker_host", "192.168.1.10"}, {"preset", "ad5m"}}},
                    {"printer-b", printer_b}}}};
        c.update(extra_root);
        return c;
    }
};

TEST_CASE_METHOD(RootPresetLiftFixture,
                 "Config: legacy root preset lifts into the active printer only",
                 "[config][preset][multi-printer][migration]") {
    // config_version is already current: this is the installer --update shape,
    // which no `version < N` gate would ever reach.
    write_and_init(two_printers({{"preset", "qidi_q2"}}));

    // Active printer (b) gains the lifted marker...
    CHECK(config.get_active_printer_id() == "printer-b");
    CHECK(config.has_preset());
    CHECK(config.get_preset() == "qidi_q2");

    // ...printer-a keeps its own, unchanged.
    REQUIRE(config.set_active_printer("printer-a"));
    CHECK(config.get_preset() == "ad5m");

    // The root key is gone, in memory and on disk. Leaving it would let a
    // subsequent clear_preset() be silently undone by the next boot's lift.
    CHECK_FALSE(ConfigTestAccess::data(config).contains("preset"));
    json on_disk = read_back();
    CHECK_FALSE(on_disk.contains("preset"));
    CHECK(on_disk["printers"]["printer-b"]["preset"] == "qidi_q2");
    CHECK(on_disk["printers"]["printer-a"]["preset"] == "ad5m");
}

TEST_CASE_METHOD(RootPresetLiftFixture,
                 "Config: lift never overwrites a preset the printer already has",
                 "[config][preset][multi-printer][migration]") {
    // A stale root marker (installer setdefault after the user configured this
    // printer) must lose to the per-printer value, not clobber it.
    write_and_init(two_printers({{"preset", "ad5x"}}, {{"preset", "qidi_q2"}}));

    CHECK(config.get_preset() == "qidi_q2");
    CHECK_FALSE(ConfigTestAccess::data(config).contains("preset"));
}

TEST_CASE_METHOD(RootPresetLiftFixture, "Config: root preset lift is idempotent across boots",
                 "[config][preset][multi-printer][migration]") {
    write_and_init(two_printers({{"preset", "qidi_q2"}}));
    REQUIRE(config.get_preset() == "qidi_q2");

    // Second boot over the already-lifted file: nothing to lift, nothing lost.
    Config second;
    second.init(config_path);
    CHECK(second.get_preset() == "qidi_q2");
    CHECK_FALSE(ConfigTestAccess::data(second).contains("preset"));
    CHECK(second.get<std::string>("/printers/printer-a/preset", "") == "ad5m");
    second.clear_path();
}

TEST_CASE_METHOD(RootPresetLiftFixture, "Config: empty root preset marker is simply dropped",
                 "[config][preset][migration]") {
    write_and_init(two_printers({{"preset", ""}}));

    CHECK_FALSE(config.has_preset());
    CHECK_FALSE(ConfigTestAccess::data(config).contains("preset"));
    // An empty marker must not erase the OTHER printer's real one.
    CHECK(config.get<std::string>("/printers/printer-a/preset", "") == "ad5m");
}

TEST_CASE_METHOD(RootPresetLiftFixture,
                 "Config: v14->v15 AD5X repair still reads the root preset before the lift",
                 "[config][preset][migration][versioning]") {
    // migrate_v12_to_v13() and migrate_v14_to_v15() detect AD5X by reading the
    // ROOT "preset" key raw. run_versioned_migrations() runs before
    // lift_root_preset(), so upgrading an old single-printer config must still
    // repair the display block AND end with the marker under the printer.
    json v14 = {{"config_version", 14},
                {"preset", "ad5x"},
                {"active_printer_id", "default"},
                {"display",
                 {{"hardware_blank", 0},
                  {"sleep_backlight_off", false},
                  {"backlight_enable_ioctl", false}}},
                {"printers", {{"default", {{"moonraker_host", "127.0.0.1"}}}}}};
    write_and_init(v14);

    // The v15 repair fired — proof the migration still saw the root key.
    CHECK(config.get<int>("/display/hardware_blank") == 1);
    CHECK(config.get<bool>("/display/sleep_backlight_off") == true);

    // ...and the marker was then lifted out from under it.
    CHECK(config.get_preset() == "ad5x");
    CHECK_FALSE(ConfigTestAccess::data(config).contains("preset"));
}

} // namespace
