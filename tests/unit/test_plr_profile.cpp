// SPDX-License-Identifier: GPL-3.0-or-later
//
// The power-loss recovery profile the app writes for the HelixPrint plugin
// (include/plr_profile.h): the printer database `plr` block, the resolution
// with the user's overrides, and the record shapes the plugin reads.

#include "plr_profile.h"
#include "printer_detector.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PlrBackendKind;
using helix::PlrDbBlock;
using helix::PlrHomingCheck;
using helix::PlrUserOverride;
using helix::StorageHint;
using json = nlohmann::json;

namespace {

constexpr PlrHomingCheck kSafeHoming{true, false, true};
constexpr PlrHomingCheck kUnsafeHoming{false, false, true};
constexpr PlrHomingCheck kUnknownHoming{};

PlrDbBlock block(int tier, bool z_holds) {
    PlrDbBlock db;
    db.tier = tier;
    db.z_holds_unpowered = z_holds;
    return db;
}

} // namespace

TEST_CASE("plr_parse_db_block: absent, full and bad-typed blocks", "[plr][profile]") {
    CHECK_FALSE(helix::plr_parse_db_block(json{{"name", "X"}}).has_value());
    CHECK_FALSE(helix::plr_parse_db_block(json{{"plr", "tier 2"}}).has_value());

    auto full = helix::plr_parse_db_block(json{{"plr",
                                                {{"tier", 2},
                                                 {"backend", "qidi_print_state"},
                                                 {"storage", "emmc"},
                                                 {"z_holds_unpowered", true},
                                                 {"xy_home_gcode", "G28 X0 Y0"},
                                                 {"z_lift_mm", 8.0},
                                                 {"purge_gcode", "PURGE"},
                                                 {"resume_skip_fraction", 0.25},
                                                 {"macro_vars", {{"M", {"a", "b"}}}}}}});
    REQUIRE(full.has_value());
    CHECK(full->tier == 2);
    CHECK(full->backend == PlrBackendKind::QidiPrintState);
    CHECK(full->storage == StorageHint::Emmc);
    CHECK(full->z_holds_unpowered);
    CHECK(full->xy_home_gcode == "G28 X0 Y0");
    CHECK(full->z_lift_mm == Catch::Approx(8.0f));
    CHECK(full->purge_gcode == std::optional<std::string>("PURGE"));
    CHECK(full->resume_skip_fraction == Catch::Approx(0.25f));
    CHECK(full->macro_vars.at("M") == std::vector<std::string>{"a", "b"});

    auto bad = helix::plr_parse_db_block(json{{"plr",
                                               {{"tier", "2"},
                                                {"backend", "marlin"},
                                                {"storage", 3},
                                                {"z_holds_unpowered", "yes"},
                                                {"xy_home_gcode", ""},
                                                {"z_lift_mm", -1},
                                                {"purge_gcode", 4},
                                                {"resume_skip_fraction", 2.0}}}});
    REQUIRE(bad.has_value());
    const PlrDbBlock defaults;
    CHECK(bad->tier == 0);
    CHECK(bad->backend == defaults.backend);
    CHECK(bad->storage == StorageHint::Unknown);
    CHECK_FALSE(bad->z_holds_unpowered);
    CHECK(bad->xy_home_gcode == "G28 X Y");
    CHECK(bad->z_lift_mm == Catch::Approx(defaults.z_lift_mm));
    CHECK_FALSE(bad->purge_gcode.has_value());
    CHECK(bad->resume_skip_fraction == Catch::Approx(0.0f));
    CHECK(helix::plr_parse_db_block(json{{"plr", {{"tier", 4}}}})->tier == 0);
}

TEST_CASE("plr_resolve_profile: tier from the database and the user", "[plr][profile]") {
    struct Row {
        std::optional<PlrDbBlock> db;
        std::optional<int> user_tier;
        std::optional<std::string> user_xy;
        PlrHomingCheck homing;
        int expected;
    };
    const Row rows[] = {
        // unlisted printer: tier 0, rule 7
        {std::nullopt, std::nullopt, std::nullopt, kSafeHoming, 0},
        {block(1, false), std::nullopt, std::nullopt, kSafeHoming, 1},
        {block(2, true), std::nullopt, std::nullopt, kSafeHoming, 2},
        // tier 2 needs Z that holds unpowered
        {block(2, false), std::nullopt, std::nullopt, kSafeHoming, 1},
        // the user raises an unlisted printer with safe homing; tier 2 needs the
        // database to say Z holds unpowered, so a request for 2 stops at 1
        {std::nullopt, 1, std::nullopt, kSafeHoming, 1},
        {std::nullopt, 2, std::nullopt, kSafeHoming, 1},
        {block(1, false), 2, std::nullopt, kSafeHoming, 1},
        {block(2, false), 2, std::nullopt, kSafeHoming, 1},
        {block(1, true), 2, std::nullopt, kSafeHoming, 2},
        // homing the app cannot judge is unsafe for a raise
        {std::nullopt, 1, std::nullopt, kUnknownHoming, 0},
        {block(1, true), 2, std::nullopt, kUnknownHoming, 0},
        // ... but never for the database's own tier, which the plugin checks
        {block(2, true), std::nullopt, std::nullopt, kUnknownHoming, 2},
        // ... with unsafe homing and no override: off
        {std::nullopt, 1, std::nullopt, kUnsafeHoming, 0},
        // ... with unsafe homing and the user's own X/Y home command
        {std::nullopt, 1, std::string("G28 X Y Z0"), kUnsafeHoming, 1},
        {std::nullopt, 1, std::nullopt, PlrHomingCheck{false, true, false}, 1},
        // lowering is always allowed
        {block(2, true), 1, std::nullopt, kUnsafeHoming, 1},
        {block(2, true), 0, std::nullopt, kSafeHoming, 0},
        // tier 3 is reserved: a user cannot select it
        {block(1, false), 3, std::nullopt, kSafeHoming, 1},
    };
    for (const auto& row : rows) {
        PlrUserOverride user;
        user.tier = row.user_tier;
        user.xy_home_gcode = row.user_xy;
        CAPTURE(row.db ? row.db->tier : -1, row.user_tier.value_or(-1), row.homing.xy_home_safe,
                row.homing.has_xy_home_override, row.user_xy.value_or(""));
        // A plugin without the unlisted-printer guards: no block is tier 0.
        CHECK(helix::plr_resolve_profile(row.db, user, row.homing, false).tier == row.expected);
    }
}

TEST_CASE("plr_resolve_profile: user overrides fold into the profile", "[plr][profile]") {
    PlrDbBlock db = block(2, true);
    db.backend = PlrBackendKind::QidiPrintState;
    db.storage = StorageHint::Emmc;
    db.purge_gcode = "DB_PURGE";
    PlrUserOverride user;
    user.enabled = false;
    user.xy_home_gcode = "MY_HOME";
    user.purge_gcode = "MY_PURGE";
    user.resume_skip_fraction = 7.0f;
    auto p = helix::plr_resolve_profile(db, user, kSafeHoming, true);
    CHECK(p.backend == PlrBackendKind::QidiPrintState);
    CHECK(p.storage_hint == StorageHint::Emmc);
    CHECK(p.z_holds_unpowered);
    CHECK(p.xy_home_gcode == "MY_HOME");
    CHECK(p.purge_gcode == std::optional<std::string>("MY_PURGE"));
    CHECK(p.resume_skip_fraction == Catch::Approx(1.0f));
    CHECK(p.user_enabled == std::optional<bool>(false));

    auto plain = helix::plr_resolve_profile(db, PlrUserOverride{}, kSafeHoming, true);
    CHECK(plain.xy_home_gcode == "G28 X Y");
    CHECK(plain.purge_gcode == std::optional<std::string>("DB_PURGE"));
}

TEST_CASE("plr_profile_json and plr_user_json: the contract's shapes", "[plr][profile]") {
    PlrDbBlock db = block(2, true);
    db.backend = PlrBackendKind::QidiPrintState;
    db.storage = StorageHint::Emmc;
    db.macro_vars = {{"PRINT_START", {"bed"}}};
    json j = helix::plr_profile_json(
        helix::plr_resolve_profile(db, PlrUserOverride{}, kSafeHoming, true));
    CHECK(j == json{{"tier", 2},
                    {"backend", "qidi_print_state"},
                    {"storage_hint", "emmc"},
                    {"z_holds_unpowered", true},
                    {"xy_home_gcode", "G28 X Y"},
                    {"z_lift_mm", 5.0},
                    {"purge_gcode", nullptr},
                    {"resume_skip_fraction", 0.0},
                    {"macro_vars", {{"PRINT_START", {"bed"}}}},
                    {"listed", true},
                    {"reason", nullptr}});

    json unlisted = helix::plr_profile_json(
        helix::plr_resolve_profile(std::nullopt, PlrUserOverride{}, kSafeHoming, true));
    CHECK(unlisted["tier"] == 1);
    CHECK(unlisted["backend"] == "generic");
    CHECK(unlisted["storage_hint"] == "unknown");
    CHECK(unlisted["macro_vars"] == json::object());
    REQUIRE(unlisted.contains("listed"));
    CHECK(unlisted["listed"] == false);
    CHECK(unlisted["reason"].is_null());

    PlrDbBlock excluded = block(0, false);
    excluded.reason = "gantry_drops";
    json ex = helix::plr_profile_json(
        helix::plr_resolve_profile(excluded, PlrUserOverride{}, kSafeHoming, true));
    CHECK(ex["tier"] == 0);
    REQUIRE(ex.contains("listed"));
    CHECK(ex["listed"] == true);
    CHECK(ex["reason"] == "gantry_drops");

    CHECK(helix::plr_user_json(true) == json{{"enabled", true}});
}

TEST_CASE("Printer database: Qidi Q2 is listed for the plugin's print_state adapter",
          "[plr][profile]") {
    auto q2 = PrinterDetector::get_plr_block("Qidi Q2");
    REQUIRE(q2.has_value());
    CHECK(q2->backend == PlrBackendKind::QidiPrintState);
    CHECK(q2->storage == StorageHint::Emmc);
    CHECK(helix::plr_resolve_profile(q2, PlrUserOverride{}, kSafeHoming, true).tier == 2);
    CHECK_FALSE(PrinterDetector::get_plr_block("No Such Printer").has_value());
}

TEST_CASE("plr_parse_db_block: reason", "[plr][profile]") {
    auto with = helix::plr_parse_db_block(json{{"plr", {{"tier", 0}, {"reason", "gantry_drops"}}}});
    REQUIRE(with.has_value());
    CHECK(with->reason == std::optional<std::string>("gantry_drops"));
    CHECK_FALSE(helix::plr_parse_db_block(json{{"plr", {{"tier", 1}}}})->reason.has_value());
    CHECK_FALSE(helix::plr_parse_db_block(json{{"plr", {{"reason", 3}}}})->reason.has_value());
    CHECK_FALSE(helix::plr_parse_db_block(json{{"plr", {{"reason", ""}}}})->reason.has_value());
}

TEST_CASE("plr_resolve_profile: a printer without a block gets tier 1", "[plr][profile]") {
    const auto p = helix::plr_resolve_profile(std::nullopt, PlrUserOverride{}, kSafeHoming, true);
    CHECK(p.tier == helix::kPlrUnlistedTier);
    CHECK(p.tier == 1);
    CHECK(p.backend == PlrBackendKind::Generic);
    CHECK_FALSE(p.listed);
    CHECK_FALSE(p.z_holds_unpowered);
    CHECK(p.storage_hint == StorageHint::Unknown);
    // Homing the app cannot judge does not lower it: the plugin checks it.
    CHECK(helix::plr_resolve_profile(std::nullopt, PlrUserOverride{}, kUnknownHoming, true).tier ==
          1);
    // A plugin that cannot guard an unlisted printer gets tier 0.
    const auto old =
        helix::plr_resolve_profile(std::nullopt, PlrUserOverride{}, kSafeHoming, false);
    CHECK(old.tier == 0);
    CHECK_FALSE(old.listed);
    // A listed block always wins, at any plugin version.
    for (bool guards : {true, false}) {
        CHECK(helix::plr_resolve_profile(block(0, false), PlrUserOverride{}, kSafeHoming, guards)
                  .tier == 0);
        CHECK(helix::plr_resolve_profile(block(2, true), PlrUserOverride{}, kSafeHoming, guards)
                  .tier == 2);
        CHECK(helix::plr_resolve_profile(block(2, true), PlrUserOverride{}, kSafeHoming, guards)
                  .listed);
    }
}

TEST_CASE("Printer database: designs that cannot resume at tier 1 are listed at tier 0",
          "[plr][profile]") {
    struct Row {
        const char* name;
        const char* reason;
    };
    const Row rows[] = {
        {"Voron 2.4", "gantry_drops"},         {"PFA Micron", "gantry_drops"},
        {"FLSUN V400", "delta_kinematics"},    {"FLSUN Super Racer", "delta_kinematics"},
        {"FLSUN Delta", "delta_kinematics"},   {"FLSUN S1 / S1 Pro", "delta_kinematics"},
        {"FLSUN T1 Pro", "delta_kinematics"},  {"Doron Velta", "delta_kinematics"},
        {"Venture Delta", "delta_kinematics"},
    };
    for (const auto& row : rows) {
        CAPTURE(row.name);
        const auto db = PrinterDetector::get_plr_block(row.name);
        REQUIRE(db.has_value());
        CHECK(db->tier == 0);
        CHECK(db->reason == std::optional<std::string>(row.reason));
        const auto p = helix::plr_resolve_profile(db, PlrUserOverride{}, kSafeHoming, true);
        CHECK(p.tier == 0);
        CHECK(p.listed);
    }
}

TEST_CASE("Printer database: a firmware's own recovery keeps the plugin out", "[plr][profile]") {
    for (const char* name :
         {"Creality K1", "Creality K1C", "Creality K1 Max", "Creality K1 SE", "Creality K2 Plus",
          "Creality K2 Plus (Jacobean firmware)", "Creality Hi", "Creality Ender-3 V3",
          "Creality Ender-3 V3 KE", "Snapmaker U1", "Qidi Plus 4", "Qidi Q1 Pro", "Qidi Max 4",
          "Qidi Plus 5", "Qidi X-Max 3"}) {
        CAPTURE(name);
        const auto db = PrinterDetector::get_plr_block(name);
        REQUIRE(db.has_value());
        CHECK(db->backend == PlrBackendKind::Vendor);
        CHECK(db->reason == std::optional<std::string>("firmware_recovery"));
        CHECK(helix::plr_resolve_profile(db, PlrUserOverride{}, kSafeHoming, true).tier == 0);
    }
}

TEST_CASE("Printer database: a design without a block takes the tier 1 default", "[plr][profile]") {
    // The hardware test of the default runs on these (#1863).
    for (const char* name : {"Voron 0.2", "Voron Trident", "Creality Ender 3", "RatRig V-Core 3"}) {
        CAPTURE(name);
        const auto db = PrinterDetector::get_plr_block(name);
        CHECK_FALSE(db.has_value());
        CHECK(helix::plr_resolve_profile(db, PlrUserOverride{}, kSafeHoming, true).tier == 1);
    }
    // Lead screws hold the SV08's levelled gantry; the listing keeps the
    // plugin's gantry check for unlisted printers from refusing it.
    for (const char* name : {"Sovol SV08", "Sovol SV08 Max"}) {
        CAPTURE(name);
        const auto db = PrinterDetector::get_plr_block(name);
        REQUIRE(db.has_value());
        CHECK(db->tier == 1);
        CHECK(db->backend == PlrBackendKind::Generic);
    }
}
