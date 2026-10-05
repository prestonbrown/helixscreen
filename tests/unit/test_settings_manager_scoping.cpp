// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pins the per-printer vs global storage contract of SettingsManager.
//
// Every SettingsManager accessor persists through one of two shapes:
//
//   per-printer  config->set(config->df() + "<leaf>", v)  -> /printers/<active>/<leaf>
//   global       config->set("/<root path>", v)           -> /<root path>
//
// The console user-filter lists are the one setting that uses BOTH: a global
// layer and a per-printer layer, written independently and unioned at read, so
// a pattern can be muted install-wide or on one machine only.
//
// Nothing else in the suite asserts which is which. A key silently moving from
// df()-scoped to root-scoped leaks one printer's setting onto every other
// printer; moving the other way makes a global preference reset itself every
// time the user switches machines. Both are invisible to a round-trip test that
// only ever has one printer configured, which is what the existing incidental
// coverage (test_detection_settings.cpp, test_hidden_macros_settings.cpp,
// test_page_scroll_setting.cpp) does.
//
// Two mechanisms are covered, because SettingsManager reads config two ways:
//
//   * Live readers  — get_hidden_macros(), get_external_spool_info(),
//     get_console_filter_user_*() hit Config on every call, so switching the
//     active printer changes what they return immediately.
//   * Cached readers — everything subject-backed, plus the chamber/scanner
//     strings, is loaded once in init_subjects(). Those only follow the active
//     printer across the deinit/init cycle that a real switch performs:
//     PrinterSession::switch_printer() -> tear_down_printer_state() (step 16,
//     StaticSubjectRegistry::deinit_all(), which runs the deinit_subjects()
//     SettingsManager self-registers at settings_manager.cpp:225) ->
//     init_printer_state() -> init_subjects() against the new df().
//     SettingsScopeFixture::switch_to() reproduces exactly that pair.
//
// Isolation note: SettingsManager and Config are process-wide singletons, and
// HelixTestFixture::reset_all() does NOT clear SettingsManager's subjects (see
// the same hazard documented at tests/unit/test_hidden_macros_settings.cpp:24).
// The fixture therefore seeds config and rebuilds the subjects on entry, and
// rebuilds them against a clean config on exit, so these tests neither depend
// on nor disturb run order.

#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "ams_backend_cfs.h"
#include "ams_state.h"
#include "config.h"
#include "settings_manager.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

constexpr const char* PRINTER_A = "printer-a";
constexpr const char* PRINTER_B = "printer-b";

// Mirrors the shipped shape: `show_printer_switcher` is a bool SIBLING of the
// printer objects inside the printers map (config.cpp:1139), not a child of one.
json two_printer_config() {
    json c = {{"config_version", CURRENT_CONFIG_VERSION},
              {"active_printer_id", PRINTER_A},
              {"printers",
               {{"show_printer_switcher", false},
                {PRINTER_A, {{"moonraker_host", "192.168.1.10"}}},
                {PRINTER_B, {{"moonraker_host", "192.168.1.11"}}}}}};
    return c;
}

class SettingsScopeFixture : public HelixTestFixture {
  protected:
    Config* cfg = Config::get_instance();
    SettingsManager& sm = SettingsManager::instance();

    SettingsScopeFixture() {
        ConfigTestAccess::data(*cfg) = two_printer_config();
        ConfigTestAccess::active_printer_id(*cfg) = PRINTER_A;
        reload();
    }

    ~SettingsScopeFixture() override {
        // init_subjects() is a one-shot no-op for every later test in the
        // binary, so leaving this test's values in the subjects would leak into
        // them. Rebuild against a cleared config: every read falls back to the
        // built-in default.
        helix::test::reset_config_singleton();
        reload();
    }

    // The subject/member reload half of a printer switch (see file header).
    void reload() {
        sm.deinit_subjects();
        sm.init_subjects();
    }

    // The config-routing half only. Enough for the live readers.
    void activate(const char* id) {
        REQUIRE(cfg->set_active_printer(id));
    }

    // A printer switch as the application performs it.
    void switch_to(const char* id) {
        activate(id);
        reload();
    }

    static std::string a(const std::string& leaf) {
        return std::string("/printers/") + PRINTER_A + "/" + leaf;
    }
    static std::string b(const std::string& leaf) {
        return std::string("/printers/") + PRINTER_B + "/" + leaf;
    }
};

// ============================================================================
// Live readers — no subject cache, so routing alone decides the answer
// ============================================================================

TEST_CASE_METHOD(
    SettingsScopeFixture,
    "SettingsManager: hidden macros are per-printer, console user filters have a global layer",
    "[settings][multi-printer]") {
    REQUIRE(cfg->get_active_printer_id() == PRINTER_A);

    const std::vector<std::string> a_macros{"A_ONLY"};
    const std::vector<std::string> b_macros{"B_ONLY", "B_ALSO"};
    const std::vector<std::string> shared_first{"prefix:SHARED"};
    const std::vector<std::string> shared_second{"prefix:CHANGED"};

    sm.set_hidden_macros(a_macros);
    sm.set_console_filter_user_add(shared_first);

    REQUIRE(sm.hidden_macros_key_exists());
    CHECK(sm.get_hidden_macros() == a_macros);

    activate(PRINTER_B);

    // Per-printer: B has never been configured, so it must NOT inherit A's set.
    // hidden_macros_key_exists() is the distinction the macro panel relies on to
    // tell "never configured" from "user unhid everything".
    CHECK_FALSE(sm.hidden_macros_key_exists());
    CHECK(sm.get_hidden_macros().empty());

    // Global layer: B sees exactly what A wrote, because neither printer has a
    // per-printer layer of its own for the union to add to.
    CHECK(sm.get_console_filter_user_add() == shared_first);

    sm.set_hidden_macros(b_macros);
    sm.set_console_filter_user_add(shared_second);

    activate(PRINTER_A);

    // A's set survived B being configured...
    CHECK(sm.get_hidden_macros() == a_macros);
    // ...and the global layer really is one value: B's write is visible from A.
    CHECK(sm.get_console_filter_user_add() == shared_second);

    // Storage paths spelled out — this is what moves if a key changes scope.
    CHECK(cfg->get<std::vector<std::string>>(a("macros/hidden"), {}) == a_macros);
    CHECK(cfg->get<std::vector<std::string>>(b("macros/hidden"), {}) == b_macros);
    // Writes default to the global layer, so neither printer node was touched.
    CHECK(cfg->exists("/console/filter_user_add"));
    CHECK_FALSE(cfg->exists(a("console/filter_user_add")));
    CHECK_FALSE(cfg->exists(b("console/filter_user_add")));
}

TEST_CASE_METHOD(SettingsScopeFixture,
                 "SettingsManager: console user filters union the global and per-printer layers",
                 "[settings][multi-printer]") {
    // Two layers, read together. The global one exists so a user can mute a
    // pattern everywhere; the per-printer one so a pattern that only makes
    // sense on one machine does not follow them to the next.
    sm.set_console_filter_user_add({"prefix:EVERYWHERE"}, ConsoleFilterScope::Global);
    sm.set_console_filter_user_add({"prefix:A_ONLY"}, ConsoleFilterScope::Printer);
    sm.set_console_filter_user_remove({"substring:GLOBAL_DROP"}, ConsoleFilterScope::Global);
    sm.set_console_filter_user_remove({"substring:A_DROP"}, ConsoleFilterScope::Printer);

    // On A: both layers, global entries first.
    const std::vector<std::string> a_add{"prefix:EVERYWHERE", "prefix:A_ONLY"};
    const std::vector<std::string> a_remove{"substring:GLOBAL_DROP", "substring:A_DROP"};
    CHECK(sm.get_console_filter_user_add() == a_add);
    CHECK(sm.get_console_filter_user_remove() == a_remove);

    activate(PRINTER_B);

    // On B: the global entry is still in force, A's per-printer one is not.
    const std::vector<std::string> global_only_add{"prefix:EVERYWHERE"};
    const std::vector<std::string> global_only_remove{"substring:GLOBAL_DROP"};
    CHECK(sm.get_console_filter_user_add() == global_only_add);
    CHECK(sm.get_console_filter_user_remove() == global_only_remove);

    // B writes its own per-printer entry. A's must be unaffected.
    sm.set_console_filter_user_add({"regex:B_ONLY.*"}, ConsoleFilterScope::Printer);
    const std::vector<std::string> b_add{"prefix:EVERYWHERE", "regex:B_ONLY.*"};
    CHECK(sm.get_console_filter_user_add() == b_add);

    activate(PRINTER_A);
    CHECK(sm.get_console_filter_user_add() == a_add);

    // Single-layer reads, so a settings UI can show which list an entry is in.
    CHECK(sm.get_console_filter_user_add(ConsoleFilterScope::Global) ==
          std::vector<std::string>{"prefix:EVERYWHERE"});
    CHECK(sm.get_console_filter_user_add(ConsoleFilterScope::Printer) ==
          std::vector<std::string>{"prefix:A_ONLY"});

    // Both layers really are stored in two different places.
    CHECK(cfg->exists("/console/filter_user_add"));
    CHECK(cfg->exists(a("console/filter_user_add")));
    CHECK(cfg->exists(b("console/filter_user_add")));
    CHECK(cfg->exists("/console/filter_user_remove"));
    CHECK(cfg->exists(a("console/filter_user_remove")));
    CHECK_FALSE(cfg->exists(b("console/filter_user_remove")));

    // An entry present in BOTH layers is applied once, not twice — a duplicate
    // regex spec would otherwise be compiled and matched on every console line.
    sm.set_console_filter_user_add({"prefix:EVERYWHERE", "prefix:A_ONLY"},
                                   ConsoleFilterScope::Printer);
    CHECK(sm.get_console_filter_user_add() == a_add);
}

TEST_CASE_METHOD(SettingsScopeFixture,
                 "SettingsManager: external spool assignment does not leak between printers",
                 "[settings][multi-printer]") {
    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PETG";
    info.brand = "Acme";
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    info.bed_temp = 80;
    sm.set_external_spool_info(info);

    auto on_a = sm.get_external_spool_info();
    REQUIRE(on_a.has_value());
    CHECK(on_a->material == "PETG");
    CHECK(on_a->slot_index == -2); // external-spool sentinel

    activate(PRINTER_B);

    // The bypass spool is physically loaded on ONE machine. If this ever reads
    // A's spool, the other printer's filament panel shows a spool that is not
    // there and the AMS mapping picks its material.
    CHECK_FALSE(sm.get_external_spool_info().has_value());

    // Clearing on B must not reach into A's section.
    sm.clear_external_spool_info();

    activate(PRINTER_A);
    auto back_on_a = sm.get_external_spool_info();
    REQUIRE(back_on_a.has_value());
    CHECK(back_on_a->brand == "Acme");

    CHECK(cfg->exists(a("filament/external_spool/assigned")));
    CHECK_FALSE(cfg->exists(b("filament/external_spool/assigned")));
    CHECK_FALSE(cfg->exists("/filament/external_spool/assigned"));
}

// ============================================================================
// Cached readers — the subject/member reload is what makes the switch visible
// ============================================================================

TEST_CASE_METHOD(SettingsScopeFixture,
                 "SettingsManager: subject-backed settings reload per printer across a switch",
                 "[settings][multi-printer]") {
    sm.set_extrude_speed(17);
    sm.set_qidi_eject_distance(555);
    // Globals: both default to true, so setting false proves the value crossed
    // the switch rather than the default being re-applied.
    sm.set_detection_enabled(false);
    sm.set_console_filter_temps(false);

    switch_to(PRINTER_B);

    // Per-printer: B falls back to the built-in defaults, NOT A's values.
    CHECK(sm.get_extrude_speed() == 5);
    CHECK(sm.get_qidi_eject_distance() == 878);
    // Global: B inherits what A set.
    CHECK(sm.get_detection_enabled() == false);
    CHECK(sm.get_console_filter_temps() == false);

    sm.set_extrude_speed(31);
    sm.set_qidi_eject_distance(1200);
    sm.set_detection_enabled(true);

    switch_to(PRINTER_A);

    // A's values survived B being configured.
    CHECK(sm.get_extrude_speed() == 17);
    CHECK(sm.get_qidi_eject_distance() == 555);
    // The global toggle B flipped is visible from A.
    CHECK(sm.get_detection_enabled() == true);

    CHECK(cfg->get<int>(a("filament/extrude_speed"), -1) == 17);
    CHECK(cfg->get<int>(b("filament/extrude_speed"), -1) == 31);
    CHECK(cfg->get<int>(a("ams/qidi_eject_distance"), -1) == 555);
    CHECK(cfg->get<int>(b("ams/qidi_eject_distance"), -1) == 1200);

    CHECK(cfg->exists("/detection/enabled"));
    CHECK_FALSE(cfg->exists(a("detection/enabled")));
    CHECK_FALSE(cfg->exists(b("detection/enabled")));
    CHECK(cfg->exists("/console/filter_temps"));
    CHECK_FALSE(cfg->exists(a("console/filter_temps")));
}

TEST_CASE_METHOD(SettingsScopeFixture,
                 "SettingsManager: cached chamber strings are per-printer, scanner strings global",
                 "[settings][multi-printer]") {
    // These have no subject at all — init_subjects() copies them into plain
    // std::string members. The cache is what makes them easy to get wrong: a
    // getter that keeps returning the previous printer's value looks identical
    // to a correctly-scoped one until you actually switch.
    //
    // The scanner is a USB/Bluetooth device plugged into the host running
    // HelixScreen, so it is one setting for the install; the chamber heater and
    // sensor name a config section on one specific Klipper machine.
    sm.set_scanner_keymap("qwertz");
    sm.set_scanner_device_id("1a2b:3c4d");
    sm.set_scanner_device_name("Acme Scanner");
    sm.set_scanner_bt_address("AA:BB:CC:DD:EE:FF");
    sm.set_chamber_heater_assignment("heater_generic chamber");
    sm.set_chamber_sensor_assignment("temperature_sensor chamber");

    switch_to(PRINTER_B);

    // Global: the scanner survives the switch rather than resetting to defaults.
    CHECK(sm.get_scanner_keymap() == "qwertz");
    CHECK(sm.get_scanner_device_id() == "1a2b:3c4d");
    CHECK(sm.get_scanner_device_name() == "Acme Scanner");
    CHECK(sm.get_scanner_bt_address() == "AA:BB:CC:DD:EE:FF");
    // Per-printer: B has never been configured, so it falls back to "auto".
    CHECK(sm.get_chamber_heater_assignment() == "auto");
    CHECK(sm.get_chamber_sensor_assignment() == "auto");

    // Re-plugging the scanner on B is a change for the whole install.
    sm.set_scanner_keymap("azerty");

    switch_to(PRINTER_A);

    CHECK(sm.get_scanner_keymap() == "azerty"); // B's write, seen from A
    CHECK(sm.get_scanner_device_id() == "1a2b:3c4d");
    CHECK(sm.get_chamber_heater_assignment() == "heater_generic chamber");
    CHECK(sm.get_chamber_sensor_assignment() == "temperature_sensor chamber");

    CHECK(cfg->get<std::string>("/scanner/keymap", "") == "azerty");
    CHECK_FALSE(cfg->exists(a("scanner/keymap")));
    CHECK_FALSE(cfg->exists(b("scanner/keymap")));
    CHECK(cfg->get<std::string>(a(wizard::CHAMBER_HEATER), "") == "heater_generic chamber");
    CHECK_FALSE(cfg->exists(b(wizard::CHAMBER_HEATER)));
}

// ============================================================================
// show_printer_switcher — the trap: global, but stored inside /printers
// ============================================================================

TEST_CASE_METHOD(SettingsScopeFixture,
                 "SettingsManager: show_printer_switcher is global despite living under /printers",
                 "[settings][multi-printer]") {
    // Read/written at "/printers/show_printer_switcher"
    // (settings_manager.cpp:146 and :467) — inside the printers map but a
    // sibling of the printer objects, never routed through df(). Being adjacent
    // to per-printer storage is exactly what makes it easy to "fix" into
    // df() + "show_printer_switcher", which would hide the switcher on the one
    // printer you need it from.
    REQUIRE(sm.get_show_printer_switcher() == false); // seeded value

    sm.set_show_printer_switcher(true);

    CHECK(cfg->get<bool>("/printers/show_printer_switcher", false) == true);
    CHECK_FALSE(cfg->exists(a("show_printer_switcher")));
    CHECK_FALSE(cfg->exists(b("show_printer_switcher")));
    CHECK_FALSE(cfg->exists("/show_printer_switcher"));

    switch_to(PRINTER_B);
    // Shared: not reset to the false default on the other printer.
    CHECK(sm.get_show_printer_switcher() == true);

    sm.set_show_printer_switcher(false);

    switch_to(PRINTER_A);
    CHECK(sm.get_show_printer_switcher() == false);
    CHECK(cfg->get<bool>("/printers/show_printer_switcher", true) == false);

    // The bool sibling must never be mistaken for a printer entry — that
    // is-object() filter (config.cpp:1721, :1707) is the only thing making this
    // storage location safe.
    auto ids = cfg->get_printer_ids();
    std::sort(ids.begin(), ids.end());
    const std::vector<std::string> expected{PRINTER_A, PRINTER_B};
    CHECK(ids == expected);
    CHECK_FALSE(cfg->set_active_printer("show_printer_switcher"));
    CHECK(cfg->get_active_printer_id() == PRINTER_A);
}

// ============================================================================
// Full accessor inventory — one write each, exact storage location pinned
// ============================================================================

TEST_CASE_METHOD(SettingsScopeFixture,
                 "SettingsManager: every accessor writes to its pinned config scope",
                 "[settings][multi-printer]") {
    // Drives every persisting setter once, under printer-a only, then asserts
    // the value landed at the documented pointer AND at neither of the two
    // wrong places (the root twin for per-printer keys, the per-printer twin
    // for global keys). A scope change moves a key from one list to the other
    // and trips at least two assertions here.
    auto expect_per_printer = [&](const std::string& leaf) {
        INFO("per-printer leaf: " << leaf);
        CHECK(cfg->exists(a(leaf)));          // under the active printer
        CHECK_FALSE(cfg->exists(b(leaf)));    // never written to the other one
        CHECK_FALSE(cfg->exists("/" + leaf)); // and not at the root
    };
    auto expect_global = [&](const std::string& root_ptr) {
        INFO("global pointer: " << root_ptr);
        const std::string leaf = root_ptr.substr(1);
        CHECK(cfg->exists(root_ptr));
        CHECK_FALSE(cfg->exists(a(leaf)));
        CHECK_FALSE(cfg->exists(b(leaf)));
    };

    SECTION("per-printer keys") {
        sm.set_z_movement_style(ZMovementStyle::NOZZLE_MOVES);
        sm.set_extrude_speed(12);
        sm.set_qidi_eject_distance(500);
        sm.set_qidi_eject_velocity(120);
        sm.set_auto_color_map(true);
        sm.set_afc_unload_after_print(true);
        sm.set_filament_auto_cooldown(false);
        sm.set_hidden_macros({"_HOME_Z"});
        sm.set_chamber_heater_assignment("heater_generic chamber");
        sm.set_chamber_sensor_assignment("temperature_sensor chamber");
        sm.set_toolhead_style(ToolheadStyle::A4T);
        sm.set_detection_policy_u1(1);
        sm.set_console_filter_user_add({"prefix:HERE"}, ConsoleFilterScope::Printer);
        sm.set_console_filter_user_remove({"prefix:NOT_HERE"}, ConsoleFilterScope::Printer);
        SlotInfo info;
        info.material = "PLA";
        sm.set_external_spool_info(info);

        expect_per_printer("z_movement_style");
        expect_per_printer("filament/extrude_speed");
        expect_per_printer("ams/qidi_eject_distance");
        expect_per_printer("ams/qidi_eject_velocity");
        expect_per_printer("filament/auto_color_map");
        expect_per_printer("ams/afc_unload_after_print");
        expect_per_printer("filament/auto_cooldown");
        expect_per_printer("macros/hidden");
        expect_per_printer(wizard::CHAMBER_HEATER); // "heaters/chamber"
        expect_per_printer(wizard::CHAMBER_SENSOR); // "temp_sensors/chamber"
        expect_per_printer("filament/external_spool/assigned");
        expect_per_printer("filament/external_spool/material");
        expect_per_printer("appearance/toolhead_style");
        expect_per_printer("detection/policy_u1");
        // The per-printer LAYER of the two-layer console user filters. Its root
        // twin is a real storage location too, so the "not at the root" half of
        // expect_per_printer would be wrong here — assert directly instead.
        CHECK(cfg->exists(a("console/filter_user_add")));
        CHECK_FALSE(cfg->exists(b("console/filter_user_add")));
        CHECK_FALSE(cfg->exists("/console/filter_user_add"));
        CHECK(cfg->exists(a("console/filter_user_remove")));
        CHECK_FALSE(cfg->exists(b("console/filter_user_remove")));
        CHECK_FALSE(cfg->exists("/console/filter_user_remove"));

        // Restore the printer-state capability set_z_movement_style() applied,
        // so the AUTO/kinematics default is what later tests observe.
        sm.set_z_movement_style(ZMovementStyle::AUTO);
    }

    SECTION("global keys") {
        sm.set_show_widget_labels(true);
        sm.set_console_filter_temps(false);
        sm.set_console_filter_firmware_noise(false);
        sm.set_console_filter_user_add({"prefix:FOO"});
        sm.set_console_filter_user_remove({"prefix:BAR"});
        sm.set_detection_enabled(false);
        sm.set_scanner_device_id("1a2b:3c4d");
        sm.set_scanner_device_name("Acme Scanner");
        sm.set_scanner_bt_address("AA:BB:CC:DD:EE:FF");
        sm.set_scanner_keymap("qwertz");
        sm.set_show_printer_switcher(true);

        expect_global("/appearance/show_widget_labels");
        expect_global("/console/filter_temps");
        expect_global("/console/filter_firmware_noise");
        // The global LAYER of the console user filters — the setters default to
        // it, so an unqualified write must still land at the root.
        expect_global("/console/filter_user_add");
        expect_global("/console/filter_user_remove");
        expect_global("/detection/enabled");
        expect_global("/scanner/usb_vendor_product");
        expect_global("/scanner/usb_device_name");
        expect_global("/scanner/bt_address");
        expect_global("/scanner/keymap");

        // show_printer_switcher is global too, but its root pointer already
        // starts with /printers, so expect_global's "not under a printer" form
        // does not apply. See the dedicated test above.
        CHECK(cfg->exists("/printers/show_printer_switcher"));
        CHECK_FALSE(cfg->exists(a("show_printer_switcher")));
    }
}

TEST_CASE_METHOD(SettingsScopeFixture,
                 "SettingsManager: toolhead style override follows the printer its AUTO input does",
                 "[settings][multi-printer]") {
    // The manual override has to be scoped the same way as the AUTO resolution
    // it replaces. get_effective_toolhead_style() resolves AUTO from
    // df() + wizard::PRINTER_TYPE, so an install-wide override would render one
    // machine's toolhead on both.
    cfg->set<std::string>(a(wizard::PRINTER_TYPE), "voron_v2");
    cfg->set<std::string>(b(wizard::PRINTER_TYPE), "creality_k1");

    sm.set_toolhead_style(ToolheadStyle::A4T);
    CHECK(cfg->get<int>(a("appearance/toolhead_style"), -1) ==
          static_cast<int>(ToolheadStyle::A4T));
    CHECK_FALSE(cfg->exists("/appearance/toolhead_style"));

    switch_to(PRINTER_B);
    // B never had an override set, so it resolves on its own rather than
    // inheriting A's.
    CHECK(sm.get_toolhead_style() == ToolheadStyle::AUTO);
    CHECK_FALSE(cfg->exists(b("appearance/toolhead_style")));

    // Each machine can now carry its own override.
    sm.set_toolhead_style(ToolheadStyle::STEALTHBURNER);
    CHECK(sm.get_toolhead_style() == ToolheadStyle::STEALTHBURNER);

    // The AUTO resolution input follows the active printer the same way.
    CHECK(cfg->get<std::string>(cfg->df() + wizard::PRINTER_TYPE, "") == "creality_k1");
    switch_to(PRINTER_A);
    CHECK(cfg->get<std::string>(cfg->df() + wizard::PRINTER_TYPE, "") == "voron_v2");

    // A's override survived B being configured.
    CHECK(sm.get_toolhead_style() == ToolheadStyle::A4T);
    CHECK(cfg->get<int>(b("appearance/toolhead_style"), -1) ==
          static_cast<int>(ToolheadStyle::STEALTHBURNER));

    sm.set_toolhead_style(ToolheadStyle::AUTO);
}

// ============================================================================
// Persistence — the on-disk shape a reboot actually sees
// ============================================================================

// Same approach as RootPresetLiftFixture in test_config_preset_per_printer.cpp:
// a temp dir plus HELIX_CONFIG_DIR, so a second Config instance re-reads the
// exact file the singleton saved.
class SettingsPersistenceFixture : public SettingsScopeFixture {
  protected:
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    SettingsPersistenceFixture() {
        temp_dir = (fs::temp_directory_path() / "helix_settings_scope_persist_test").string();
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);

        config_path = temp_dir + "/settings.json";
        ConfigTestAccess::path(*cfg) = config_path;
    }

    ~SettingsPersistenceFixture() override {
        // Runs BEFORE ~SettingsScopeFixture, which resets the singleton's path
        // back into the per-process sandbox.
        fs::remove_all(temp_dir);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
    }
};

TEST_CASE_METHOD(SettingsPersistenceFixture,
                 "SettingsManager: per-printer and global settings survive a Config re-init",
                 "[settings][multi-printer]") {
    sm.set_extrude_speed(21);
    sm.set_hidden_macros({"A_ONLY"});
    sm.set_show_printer_switcher(true);
    sm.set_detection_policy_u1(0);
    sm.set_scanner_keymap("qwertz");

    switch_to(PRINTER_B);
    sm.set_extrude_speed(44);
    REQUIRE(cfg->save());

    // A fresh Config over the same file — nothing in memory carries over, so
    // only the serialized layout can satisfy these.
    Config second;
    second.init(config_path);

    CHECK(second.get_active_printer_id() == PRINTER_B);

    CHECK(second.get<int>(std::string("/printers/") + PRINTER_A + "/filament/extrude_speed", -1) ==
          21);
    CHECK(second.get<int>(std::string("/printers/") + PRINTER_B + "/filament/extrude_speed", -1) ==
          44);

    const std::vector<std::string> expected_hidden{"A_ONLY"};
    CHECK(second.get<std::vector<std::string>>(
              std::string("/printers/") + PRINTER_A + "/macros/hidden", {}) == expected_hidden);
    CHECK_FALSE(second.exists(std::string("/printers/") + PRINTER_B + "/macros/hidden"));

    // Globals stayed at the root (and, for the switcher, at the printers-map root).
    CHECK(second.get<bool>("/printers/show_printer_switcher", false) == true);
    CHECK(second.get<std::string>("/scanner/keymap", "") == "qwertz");
    CHECK_FALSE(second.exists(std::string("/printers/") + PRINTER_A + "/scanner/keymap"));

    // The U1 detection policy serialized under the printer it was set on, and
    // only that one.
    CHECK(second.get<int>(std::string("/printers/") + PRINTER_A + "/detection/policy_u1", -1) == 0);
    CHECK_FALSE(second.exists(std::string("/printers/") + PRINTER_B + "/detection/policy_u1"));
    CHECK_FALSE(second.exists("/detection/policy_u1"));

    // df() on the reloaded instance routes at printer-b, so the live read
    // through it returns B's value rather than A's.
    CHECK(second.get<int>(second.df() + "filament/extrude_speed", -1) == 44);

    second.clear_path();
}

} // namespace

TEST_CASE_METHOD(SettingsScopeFixture,
                 "SettingsManager: AUTO toolhead style comes from the DB, then the AMS backend",
                 "[settings][toolhead]") {
    auto effective = [&](const char* type) {
        cfg->set<std::string>(a(wizard::PRINTER_TYPE), type);
        return sm.get_effective_toolhead_style();
    };
    AmsState::instance().set_backend(nullptr);

    SECTION("the database names the style") {
        CHECK(effective("PFA Micron") == ToolheadStyle::ANTHEAD);
        CHECK(effective("PFA Stealthfork") == ToolheadStyle::ANTHEAD);
        CHECK(effective("Creality K1") == ToolheadStyle::CREALITY_K1);
        CHECK(effective("Voron 2.4") == ToolheadStyle::DEFAULT);
    }

    SECTION("a backend hint applies only where the database has no style") {
        AmsState::instance().set_backend(
            std::make_unique<helix::printer::AmsBackendCfs>(nullptr, nullptr));
        CHECK(effective("Some Custom Printer") == ToolheadStyle::CREALITY_K2);
        CHECK(effective("PFA Micron") == ToolheadStyle::ANTHEAD);
    }

    AmsState::instance().set_backend(nullptr);
}
