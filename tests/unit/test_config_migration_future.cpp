// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises what run_versioned_migrations() does with a config_version NEWER
// than this build understands.
//
// This became reachable the moment update channels are user-switchable: moving
// from the devel channel back to stable installs an OLDER binary on top of a
// config the newer build already migrated. Every migration gate is `version <
// N`, so none of them fire — but the stamp at the end of the chain is
// unconditional, so the older build used to rewrite config_version DOWN to its
// own. The newer build would then re-run migrations it had already applied,
// against data already in the new shape.
//
// The contract pinned here: a future config is left entirely alone — not
// migrated, not stamped, and its unknown keys survive a save round trip through
// the older build.
//
// The second half of the file covers the OTHER half of that round trip: what
// happens when the version stamp is rolled BACKWARD and the ladder replays over
// data already in the new shape. The guard above is what normally prevents it,
// but it only shipped in v0.99.112 — every earlier release stamps a newer
// config down to its own version, so a downgrade to any of them and back is a
// real replay. Those tests pin which settings survive a replay and which four
// migrations rewrite data on the way through.
//
// Driven through the public Config::init() path (the migration runner is a
// static function in config.cpp), same as the v18/v21 migration tests.

#include "config.h"
#include "config_testing.h"
#include "platform_capabilities.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class MigrationFutureFixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    void SetUp() {
        // Per-process directory. `make test-run` shards across ~3x cores of concurrent
        // helix-tests processes, and a fixed path let two of them clobber each other's
        // settings.json mid-migration — surfacing as a nlohmann operator[] abort on a
        // key the other process had just removed. Which shard a test lands in shifts
        // whenever test cases are added, so a shared path fails only intermittently.
        temp_dir = (fs::temp_directory_path() /
                    ("helix_migration_future_test_" + std::to_string(::getpid())))
                       .string();
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

    void write_and_init(const json& contents) {
        std::ofstream f(config_path);
        f << contents.dump(2);
        f.close();
        config.init(config_path);
    }

    /// One boot of a build against @p contents: write it, run the real load +
    /// migration path on a throwaway Config, flush, and hand back the document
    /// that ended up on disk.
    ///
    /// A fresh Config per call rather than the fixture's member, because these
    /// tests boot repeatedly against the same file and a reused instance would
    /// carry the previous boot's in-memory document into the next assertion.
    json boot(const json& contents) const {
        {
            std::ofstream f(config_path);
            f << contents.dump(2);
        }
        Config c;
        c.init(config_path);
        c.save();
        c.clear_path();
        std::ifstream f(config_path);
        return json::parse(f);
    }

    /// Boot against @p doc with its version stamp rewritten to @p stamp — i.e.
    /// what an upgrade back to this build sees after an older build stamped the
    /// config down to its own version.
    json replay_from(const json& doc, int stamp) const {
        json rolled = doc;
        rolled["config_version"] = stamp;
        return boot(rolled);
    }

    /// Read the on-disk document directly, bypassing Config's accessors, so a
    /// test can prove what was actually persisted.
    json read_raw() const {
        std::ifstream f(config_path);
        return json::parse(f);
    }

  public:
    MigrationFutureFixture() {
        SetUp();
    }
    ~MigrationFutureFixture() {
        TearDown();
    }
};

/// A config as a NEWER build would have written it: a version this build has
/// never heard of, plus a settings block introduced after this build shipped.
json future_config(int version) {
    return json{{"config_version", version},
                {"active_printer_id", "voronv2"},
                {"appearance", {{"show_widget_labels", true}}},
                // A key this build knows nothing about — stands in for whatever
                // the devel track added.
                {"feature_from_the_future", {{"enabled", true}, {"threshold", 42}}},
                {"printers", {{"voronv2", {{"moonraker_host", "192.168.1.112"}}}}}};
}

/// A populated config in the CURRENT shape — two printers, custom macros, LED
/// state, filament slot overrides, panel widget layout, branded material
/// presets, a captured touch affine, print-start history. Synthesized, but
/// shaped like a settings.json that has actually been lived in, because the
/// replay failures below only surface on keys a stub config never carries.
///
/// Several values are deliberately set to what a migration's trigger looks for,
/// so a replay is visible rather than a no-op:
///   brightness 50              — the v6→v7 / v8→v9 "still on the old default" bump
///   jitter_threshold 15        — the v2→v3 reset
///   toolhead_style 2 and 3     — POST-v8 values that the v7→v8 remap also matches
///   screensaver_type 1         — the v15→v16 Flying Toasters disable
///   sleep_backlight_off/hardware_blank — the v12→v13 / v14→v15 AD5X repair
/// A user who deliberately chose any of these is indistinguishable, in the
/// stored JSON, from a user who never touched it.
json populated_config() {
    return json{
        {"config_version", CURRENT_CONFIG_VERSION},
        {"active_printer_id", "voronv2"},
        {"sounds_enabled", true},
        {"brightness", 50},
        {"dark_mode", true},
        {"telemetry_enabled", true},
        {"wizard_completed", true},
        {"input",
         {{"jitter_threshold", 15},
          {"scroll_limit", 10},
          {"touch_device", "/dev/input/event3"},
          {"calibration",
           {{"a", 1.0021},
            {"b", 0.0013},
            {"c", -4.5},
            {"d", 0.0009},
            {"e", 0.9987},
            {"f", 2.25},
            {"valid", true},
            {"recheck_pending", false}}}}},
        {"display",
         {{"sleep_sec", 900},
          {"dim_sec", 300},
          {"dim_brightness", 25},
          {"rotate", 0},
          {"drm_device", "/dev/dri/card0"},
          {"gcode_render_mode", 1},
          {"screensaver_type", 1},
          {"sleep_backlight_off", false},
          {"hardware_blank", 0}}},
        {"preset_materials",
         json::array({json{{"type", "PLA"}, {"brand", "GenericCo"}, {"nozzle_temp", 210}},
                      json{{"type", "PETG"}, {"brand", "GenericCo"}, {"nozzle_temp", 240}},
                      json{{"type", "ABS"}}, json{{"type", "TPU"}}})},
        {"thermal",
         {{"rates", {{"extruder", {{"heat_rate", 2.75}}}, {"heater_bed", {{"heat_rate", 0.55}}}}}}},
        {"calibration", {{"pid_history", {{"extruder", {{"oscillation_duration", 12.5}}}}}}},
        {"print_start_history",
         {{"entries",
           json::array({json{{"total", 88.0}, {"phases", {{"0", 3.0}, {"5", 40.0}}}}})}}},
        {"console", {{"filter_temps", true}, {"filter_user_add", json::array({"prefix:KEEP"})}}},
        {"scanner", {{"usb_vendor_product", "1a2b:3c4d"}, {"keymap", "qwerty"}}},
        {"printers",
         {{"show_printer_switcher", true},
          {"voronv2",
           {{"moonraker_host", "10.0.0.11"},
            {"moonraker_port", 7125},
            {"name", "Voron V2"},
            {"type", "Voron 2.4"},
            {"wizard_completed", true},
            {"preset", "voron"},
            {"printer_image", "shipped:voron-v2"},
            {"appearance", {{"toolhead_style", 2}}},
            {"detection", {{"policy_u1", 1}}},
            {"heaters",
             {{"bed", "heater_bed"},
              {"hotend", "extruder"},
              {"chamber", "heater_generic chamber"}}},
            {"temp_sensors", {{"chamber", "temperature_sensor chamber"}}},
            {"leds",
             {{"selected", json::array({"neopixel caselight"})},
              {"selected_strips", json::array({"neopixel caselight"})},
              {"last_color", "#FF8800"},
              {"last_brightness", 70},
              {"color_presets", json::array({"#FFFFFF", "#FF0000"})},
              {"led_on_at_start", true},
              {"auto_state", {{"enabled", true}, {"mappings", {{"printing", "#00FF00"}}}}}}},
            {"panel_widgets",
             {{"home",
               {{"pages", json::array({json{
                              {"widgets", json::array({json{{"id", "power_device:1"},
                                                            {"config", {{"device", "__all__"}}}},
                                                       json{{"id", "temperature"}}})}}})}}}}},
            {"default_macros",
             {{"cooldown", "TURN_OFF_HEATERS"},
              {"load_filament", {{"macro", "LOAD"}, {"params", "LEN=100"}}}}},
            {"filament_sensors",
             {{"master_enabled", true},
              {"sensors", json::array({"filament_switch_sensor runout"})}}},
            {"filament_slots",
             {{"0", {{"material", "PLA"}, {"color", "#00AAFF"}, {"brand", "GenericCo"}}}}}}},
          {"prusamk4",
           {{"moonraker_host", "10.0.0.12"},
            {"name", "MK4"},
            {"printer_image", "shipped:voron-v0"},
            {"appearance", {{"toolhead_style", 3}}},
            {"detection", {{"policy_u1", 1}}},
            {"leds", {{"selected", json::array()}}}}}}}};
}

/// Every setting in populated_config() that NO migration targets — the bulk of
/// a real user's config. None of these may change at any replay depth.
const std::vector<std::pair<std::string, json>>& untargeted_settings() {
    static const std::vector<std::pair<std::string, json>> settings = {
        {"/active_printer_id", "voronv2"},
        {"/dark_mode", true},
        {"/telemetry_enabled", true},
        {"/input/scroll_limit", 10},
        {"/input/touch_device", "/dev/input/event3"},
        {"/input/calibration/a", 1.0021},
        {"/input/calibration/f", 2.25},
        {"/input/calibration/valid", true},
        {"/display/sleep_sec", 900},
        {"/display/dim_brightness", 25},
        {"/display/drm_device", "/dev/dri/card0"},
        {"/preset_materials/0/brand", "GenericCo"},
        {"/preset_materials/1/nozzle_temp", 240},
        {"/thermal/rates/extruder/heat_rate", 2.75},
        {"/calibration/pid_history/extruder/oscillation_duration", 12.5},
        {"/print_start_history/entries/0/total", 88.0},
        {"/print_start_history/entries/0/phases/5", 40.0},
        {"/console/filter_user_add/0", "prefix:KEEP"},
        {"/scanner/usb_vendor_product", "1a2b:3c4d"},
        {"/scanner/keymap", "qwerty"},
        {"/printers/show_printer_switcher", true},
        {"/printers/voronv2/moonraker_host", "10.0.0.11"},
        {"/printers/voronv2/moonraker_port", 7125},
        {"/printers/voronv2/name", "Voron V2"},
        {"/printers/voronv2/preset", "voron"},
        {"/printers/voronv2/printer_image", "shipped:voron-v2"},
        {"/printers/voronv2/detection/policy_u1", 1},
        {"/printers/voronv2/heaters/chamber", "heater_generic chamber"},
        {"/printers/voronv2/temp_sensors/chamber", "temperature_sensor chamber"},
        {"/printers/voronv2/leds/selected/0", "neopixel caselight"},
        {"/printers/voronv2/leds/last_color", "#FF8800"},
        {"/printers/voronv2/leds/last_brightness", 70},
        {"/printers/voronv2/leds/color_presets/1", "#FF0000"},
        {"/printers/voronv2/leds/auto_state/mappings/printing", "#00FF00"},
        {"/printers/voronv2/panel_widgets/home/pages/0/widgets/0/id", "power_device:1"},
        {"/printers/voronv2/panel_widgets/home/pages/0/widgets/0/config/device", "__all__"},
        {"/printers/voronv2/default_macros/cooldown", "TURN_OFF_HEATERS"},
        {"/printers/voronv2/default_macros/load_filament/params", "LEN=100"},
        {"/printers/voronv2/filament_sensors/sensors/0", "filament_switch_sensor runout"},
        {"/printers/voronv2/filament_slots/0/material", "PLA"},
        {"/printers/voronv2/filament_slots/0/color", "#00AAFF"},
        {"/printers/prusamk4/moonraker_host", "10.0.0.12"},
        {"/printers/prusamk4/printer_image", "shipped:voron-v0"},
    };
    return settings;
}

void check_untargeted_survived(const json& doc, int stamp) {
    for (const auto& [pointer, expected] : untargeted_settings()) {
        const json::json_pointer ptr(pointer);
        INFO("replayed from stamp " << stamp << ", setting " << pointer);
        REQUIRE(doc.contains(ptr));
        CHECK(doc.at(ptr) == expected);
    }
}

} // namespace

// ============================================================================
// A future config is left unstamped
// ============================================================================

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config migration: a newer config_version is not stamped down",
                 "[config][migration]") {
    const int future = CURRENT_CONFIG_VERSION + 1;
    write_and_init(future_config(future));

    // The load must not rewrite the version to ours. Stamping it down is what
    // makes the newer build re-run already-applied migrations on its next boot.
    CHECK(config.get<int>("/config_version", -1) == future);
    CHECK(config.get<int>("/config_version", -1) != CURRENT_CONFIG_VERSION);
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config migration: a far-future config_version is still not stamped down",
                 "[config][migration]") {
    // Not just off-by-one: several devel releases' worth of drift.
    const int future = CURRENT_CONFIG_VERSION + 7;
    write_and_init(future_config(future));

    CHECK(config.get<int>("/config_version", -1) == future);
}

// ============================================================================
// A future config's unknown keys survive a round trip
// ============================================================================

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config migration: unknown keys from a newer build survive a save",
                 "[config][migration]") {
    const int future = CURRENT_CONFIG_VERSION + 1;
    write_and_init(future_config(future));

    // The older build reads and writes settings it knows about...
    config.set<bool>("/appearance/show_widget_labels", false);
    REQUIRE(config.save());

    // ...and the block it has never heard of is still on disk afterwards. If
    // this fails, switching channels back and forth silently destroys settings.
    json raw = read_raw();
    REQUIRE(raw.contains("feature_from_the_future"));
    CHECK(raw["feature_from_the_future"]["enabled"] == true);
    CHECK(raw["feature_from_the_future"]["threshold"] == 42);

    // And the version is still the future one, not ours.
    CHECK(raw["config_version"] == future);

    // The known key really was written (proves the save actually happened and
    // the assertions above are not passing against an untouched file).
    CHECK(raw["appearance"]["show_widget_labels"] == false);
}

// ============================================================================
// Regression guard: the ordinary path still migrates
// ============================================================================

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config migration: an older config_version is still migrated and stamped",
                 "[config][migration]") {
    // The future-version early return must not swallow the normal case.
    json old = future_config(CURRENT_CONFIG_VERSION - 1);
    old.erase("feature_from_the_future");
    write_and_init(old);

    CHECK(config.get<int>("/config_version", -1) == CURRENT_CONFIG_VERSION);
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config migration: a config already at the current version is stamped unchanged",
                 "[config][migration]") {
    json current = future_config(CURRENT_CONFIG_VERSION);
    current.erase("feature_from_the_future");
    write_and_init(current);

    CHECK(config.get<int>("/config_version", -1) == CURRENT_CONFIG_VERSION);
}

// ============================================================================
// devel -> stable -> devel, with a config someone actually uses
// ============================================================================

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: a populated config is byte-identical after repeated boots",
                 "[config][migration][roundtrip]") {
    // Booting the same build twice must be a fixed point. Everything below
    // compares a replay against this, so if the no-op case is not stable the
    // rest of the file is measuring noise.
    const json first = boot(populated_config());
    const json second = boot(first);

    CHECK(second == first);
    CHECK(first["config_version"] == CURRENT_CONFIG_VERSION);
    check_untargeted_survived(first, CURRENT_CONFIG_VERSION);
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: a populated config survives devel -> stable -> devel",
                 "[config][migration][roundtrip]") {
    // The v1.0 shape of this: `main` moves to beta and stamps the config at
    // CURRENT+n; the user drops back to the release/1.0 stable build, which
    // understands only CURRENT. The guard leaves the document alone, so
    // returning to beta must find every setting exactly as it left it.
    json devel = populated_config();
    devel["config_version"] = CURRENT_CONFIG_VERSION + 2;
    devel["feature_from_the_future"] = json{{"enabled", true}, {"threshold", 42}};

    const json after_stable = boot(devel);
    CHECK(after_stable["config_version"] == CURRENT_CONFIG_VERSION + 2);
    CHECK(after_stable["feature_from_the_future"]["threshold"] == 42);
    check_untargeted_survived(after_stable, CURRENT_CONFIG_VERSION + 2);

    // ...and back to devel. Nothing the stable build did may have disturbed the
    // document, so the two sides of the trip agree key for key.
    const json back_on_devel = boot(after_stable);
    CHECK(back_on_devel == after_stable);
}

// ============================================================================
// Stamp rollback: replaying the ladder over already-migrated data
// ============================================================================
//
// Reachable because the "don't stamp a newer config down" guard only landed in
// v0.99.112 (2026-08-13). Every release before it rewrites config_version to
// its own on load, so downgrading to one and coming back replays every
// migration between that build's version and ours — against data already in the
// new shape. The shipped tags reach back to config_version 17 within the last
// two months and to 8 within the last five, so the replay depths below are not
// hypothetical.
//
// Stamp 0 is deliberately excluded: config.cpp:1681 treats config_version == 0
// as "tarball default" and replaces the whole document from backup before any
// migration runs, so it is a different code path, not a replay.

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: settings no migration targets survive every stamp rollback",
                 "[config][migration][roundtrip]") {
    const json baseline = boot(populated_config());

    for (int stamp = CURRENT_CONFIG_VERSION - 1; stamp >= 1; --stamp) {
        const json replayed = replay_from(baseline, stamp);

        // The replay must re-stamp to current; a config stuck at an old version
        // would replay again on every subsequent boot.
        INFO("replayed from stamp " << stamp);
        CHECK(replayed["config_version"] == CURRENT_CONFIG_VERSION);

        check_untargeted_survived(replayed, stamp);
    }
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: a rollback to 18 or newer changes nothing at all",
                 "[config][migration][roundtrip]") {
    // The whole document, not a key list: v18→v19, v19→v20 and v20→v21 are
    // fully idempotent, so replaying them is a no-op down to the last byte.
    // This is the range a user reaches by downgrading to any release from
    // v0.99.80 onward, i.e. the overwhelmingly common case.
    const json baseline = boot(populated_config());

    for (int stamp : {CURRENT_CONFIG_VERSION - 1, 19, 18}) {
        INFO("replayed from stamp " << stamp);
        CHECK(replay_from(baseline, stamp) == baseline);
    }
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: a rollback past v18 re-arms the touch calibration recheck",
                 "[config][migration][roundtrip]") {
    // FINDING. migrate_v17_to_v18() (config.cpp:812-827) writes
    // recheck_pending = true unconditionally — it has no "already done" guard,
    // because at the time it was written the version stamp was the guard.
    //
    // The flag is not inert. display_backend_drm.cpp:696-719 and
    // display_backend_fbdev.cpp:416-434 consume it at boot and, on a
    // non-resistive panel whose ABS range mismatches the display, clear
    // /input/calibration/valid — discarding a calibration the user captured
    // AFTER the #943 fix, which was computed in the correct coordinate space.
    //
    // So: downgrade to any release older than v0.99.80 (config_version 18),
    // come back, and an affected user re-runs touch calibration. Pinned as the
    // current behaviour, not endorsed — flip to `== false` once v17→v18 learns
    // to skip a config that has already been rechecked.
    json baseline = boot(populated_config());
    REQUIRE(baseline["input"]["calibration"]["recheck_pending"] == false);
    REQUIRE(baseline["input"]["calibration"]["valid"] == true);

    const json replayed = replay_from(baseline, 17);
    CHECK(replayed["input"]["calibration"]["recheck_pending"] == true);

    // The affine itself is untouched — it is the backend, not the migration,
    // that would go on to invalidate it.
    CHECK(replayed["input"]["calibration"]["a"] == 1.0021);
    CHECK(replayed["input"]["calibration"]["valid"] == true);
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: a rollback past v9 overwrites a deliberate brightness of 50",
                 "[config][migration][roundtrip]") {
    // FINDING. migrate_v6_to_v7() (config.cpp:446) and migrate_v8_to_v9()
    // (config.cpp:488) both bump brightness 50 -> 80 on the assumption that 50
    // can only be the old default. After a rollback that is no longer true: a
    // user who chose 50 has it silently raised.
    const json baseline = boot(populated_config());
    REQUIRE(baseline["brightness"] == 50);

    CHECK(replay_from(baseline, 9)["brightness"] == 50); // v8→v9 gate not crossed
    CHECK(replay_from(baseline, 8)["brightness"] == 80);
    CHECK(replay_from(baseline, 6)["brightness"] == 80);
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: a rollback past v8 corrupts toolhead_style",
                 "[config][migration][roundtrip]") {
    // FINDING, and the worst of the four: migrate_v7_to_v8() (config.cpp:457)
    // remaps the toolhead enum 2 -> 5 and 3 -> 2. The NEW values overlap the old
    // ones, so the remap is not idempotent — it is a rotation. Replaying it
    // turns an already-correct A4T (2) into Stealthburner (5) and an Anthead (3)
    // into A4T (2), and a further replay would move them again.
    //
    // Unlike the other three this produces a value the user never chose and
    // cannot be explained as "an old default got bumped".
    const json baseline = boot(populated_config());
    REQUIRE(baseline["printers"]["voronv2"]["appearance"]["toolhead_style"] == 2);
    REQUIRE(baseline["printers"]["prusamk4"]["appearance"]["toolhead_style"] == 3);

    const json safe = replay_from(baseline, 8); // v7→v8 gate not crossed
    CHECK(safe["printers"]["voronv2"]["appearance"]["toolhead_style"] == 2);
    CHECK(safe["printers"]["prusamk4"]["appearance"]["toolhead_style"] == 3);

    const json replayed = replay_from(baseline, 7);
    CHECK(replayed["printers"]["voronv2"]["appearance"]["toolhead_style"] == 5);
    CHECK(replayed["printers"]["prusamk4"]["appearance"]["toolhead_style"] == 2);
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: a rollback past v3 resets a deliberate jitter_threshold",
                 "[config][migration][roundtrip]") {
    // FINDING. migrate_v2_to_v3() (config.cpp:343) reads 15 as "the old
    // default" and drops it to 5. Narrowest of the four — 15 is the one value
    // affected, and a user on a genuinely noisy panel is exactly who would have
    // set it back.
    const json baseline = boot(populated_config());
    REQUIRE(baseline["input"]["jitter_threshold"] == 15);

    CHECK(replay_from(baseline, 3)["input"]["jitter_threshold"] == 15);
    CHECK(replay_from(baseline, 2)["input"]["jitter_threshold"] == 5);
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: a rollback past v16 re-disables the screensaver on a "
                 "constrained tier",
                 "[config][migration][roundtrip]") {
    // The fifth replay, invisible to the sweep above: migrate_v15_to_v16()
    // (config.cpp:740) returns early on STANDARD hardware, which is what a
    // desktop test host detects, so every other test in this file exercises the
    // no-op branch. Forcing the tier is the only way to see the real one.
    //
    // Milder than the other four — it re-fires only for a user who deliberately
    // re-selected Flying Toasters after the first migration, and the migration
    // is arguably right that the setting breaks prints on this hardware. Pinned
    // so the behaviour is a decision rather than an accident.
    struct TierGuard {
        ~TierGuard() {
            helix::config_testing::set_forced_tier_for_migration(std::nullopt);
        }
    } tier_guard;
    helix::config_testing::set_forced_tier_for_migration(helix::PlatformTier::BASIC);

    // A current-stamp config runs no migration, so Flying Toasters standing at
    // 1 here is the user's own choice — either they never hit v15→v16, or they
    // hit it and re-selected the screensaver afterwards.
    const json baseline = boot(populated_config());
    REQUIRE(baseline["display"]["screensaver_type"] == 1);

    // A rollback past the v15→v16 gate overrides that choice.
    const json replayed = replay_from(baseline, 15);
    CHECK(replayed["display"]["screensaver_type"] == 0);
    CHECK(replayed["display"]["screensaver_migration_notice_pending"] == true);

    // One version later the gate is not crossed and the choice stands.
    const json safe = replay_from(baseline, 16);
    CHECK(safe["display"]["screensaver_type"] == 1);
    CHECK_FALSE(safe["display"].contains("screensaver_migration_notice_pending"));
}

TEST_CASE_METHOD(MigrationFutureFixture,
                 "Config round trip: the structural migrations stay idempotent under replay",
                 "[config][migration][roundtrip]") {
    // migrate_display_config() and the /display -> /input key moves run
    // unconditionally on EVERY boot, with no version gate at all — so they are
    // replayed far more often than the versioned ladder and their idempotency
    // matters more. Both key off legacy paths that a current config no longer
    // has, so they must be inert here.
    const json baseline = boot(populated_config());

    CHECK_FALSE(baseline.contains("display_rotate"));
    CHECK_FALSE(baseline["display"].contains("calibration"));
    CHECK_FALSE(baseline["display"].contains("touch_device"));

    // The values they would have moved are already at their modern paths and
    // stay there across a replay of the full ladder.
    const json replayed = replay_from(baseline, 1);
    CHECK(replayed["display"]["rotate"] == 0);
    CHECK(replayed["input"]["touch_device"] == "/dev/input/event3");
    CHECK(replayed["input"]["calibration"]["a"] == 1.0021);
}
