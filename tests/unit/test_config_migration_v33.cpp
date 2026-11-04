// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the v32 -> v33 config migration: an AD5M Pro set up from the Forge-X or
// Z-Mod preset gets chamber_fan (the side fan) as its aux fan, and on Z-Mod loses the
// chamber and exhaust roles that name fans the firmware never exposes.

#include "config.h"
#include "config_migrations.h"

#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using nlohmann::json;

namespace {

constexpr const char* kSideFan = "fan_generic chamber_fan";
constexpr const char* kCirculation = "fan_generic internal_fan";
constexpr const char* kExhaust = "fan_generic external_fan";

json old_preset_fans() {
    return {{"hotend", "heater_fan heat_fan"},
            {"part", "fan_generic fanM106"},
            {"chamber", kSideFan},
            {"exhaust", kExhaust},
            {"aux", kCirculation}};
}

json migrate(json config) {
    config["config_version"] = 32;
    helix::config_detail::run_versioned_migrations(config, "");
    REQUIRE(config["config_version"] == helix::kCurrentConfigVersion);
    return config;
}

json single_printer(json printer) {
    return {{"active_printer_id", "a"}, {"printers", {{"a", printer}}}};
}

} // namespace

TEST_CASE("v33 swaps a Forge-X AD5M Pro's aux and chamber fans", "[config][migration][ad5m]") {
    json config =
        migrate(single_printer({{"preset", "ad5m_pro_forgex"}, {"fans", old_preset_fans()}}));
    const json& fans = config["printers"]["a"]["fans"];
    CHECK(fans["aux"] == kSideFan);
    CHECK(fans["chamber"] == kCirculation);
    CHECK(fans["exhaust"] == kExhaust);
    CHECK(fans["part"] == "fan_generic fanM106");
}

TEST_CASE("v33 recognises a Forge-X AD5M Pro by its type when it has no preset",
          "[config][migration][ad5m]") {
    json config = migrate(single_printer(
        {{"type", "FlashForge Adventurer 5M Pro (ForgeX)"}, {"fans", old_preset_fans()}}));
    CHECK(config["printers"]["a"]["fans"]["aux"] == kSideFan);
    CHECK(config["printers"]["a"]["fans"]["chamber"] == kCirculation);
}

TEST_CASE("v33 gives a Z-Mod AD5M Pro the side fan as aux and drops the hidden fans",
          "[config][migration][ad5m]") {
    json config =
        migrate(single_printer({{"preset", "ad5m_pro_zmod"}, {"fans", old_preset_fans()}}));
    const json& fans = config["printers"]["a"]["fans"];
    CHECK(fans["aux"] == kSideFan);
    CHECK_FALSE(fans.contains("chamber"));
    CHECK_FALSE(fans.contains("exhaust"));
    CHECK(fans["hotend"] == "heater_fan heat_fan");
}

TEST_CASE("v33 leaves a Z-Mod AD5M Pro with the fan roles its preset ships",
          "[config][migration][ad5m]") {
    const std::filesystem::path shipped = "assets/config/presets/ad5m_pro_zmod.json";
    INFO("reading " << shipped.string() << " (tests must run from the repo root)");
    std::ifstream in(shipped);
    REQUIRE(in.good());
    const json preset = json::parse(in, nullptr, false);
    REQUIRE(preset.is_object());
    const json& shipped_fans = preset["printer"]["fans"];
    REQUIRE(shipped_fans.is_object());

    json config =
        migrate(single_printer({{"preset", "ad5m_pro_zmod"}, {"fans", old_preset_fans()}}));
    CHECK(config["printers"]["a"]["fans"] == shipped_fans);
}

TEST_CASE("v33 keeps a Z-Mod user's own chamber and exhaust choices", "[config][migration][ad5m]") {
    json fans = old_preset_fans();
    fans["chamber"] = "fan_generic nevermore";
    fans["exhaust"] = "";
    json config = migrate(single_printer({{"preset", "ad5m_pro_zmod"}, {"fans", fans}}));
    const json& out = config["printers"]["a"]["fans"];
    CHECK(out["aux"] == kSideFan);
    CHECK(out["chamber"] == "fan_generic nevermore");
    CHECK(out["exhaust"] == "");
}

TEST_CASE("v33 leaves fan roles the user chose alone", "[config][migration][ad5m]") {
    json fans = old_preset_fans();
    fans["aux"] = "fan_generic bed_fan";
    json config = migrate(single_printer({{"preset", "ad5m_pro_forgex"}, {"fans", fans}}));
    CHECK(config["printers"]["a"]["fans"] == fans);

    json chamber_changed = old_preset_fans();
    chamber_changed["chamber"] = "fan_generic nevermore";
    config = migrate(single_printer({{"preset", "ad5m_pro_forgex"}, {"fans", chamber_changed}}));
    CHECK(config["printers"]["a"]["fans"] == chamber_changed);
}

TEST_CASE("v33 touches no other printer", "[config][migration][ad5m]") {
    for (const json& printer :
         {json{{"preset", "ad5m_pro"}, {"fans", old_preset_fans()}},
          json{{"type", "FlashForge Adventurer 5M Pro"}, {"fans", old_preset_fans()}},
          json{{"preset", "ad5x"}, {"fans", old_preset_fans()}},
          json{{"preset", "ad5m_pro_zmod"}, {"type", "FlashForge Adventurer 5M Pro"}}}) {
        json config = migrate(single_printer(printer));
        INFO(printer.dump());
        CHECK(config["printers"]["a"] == printer);
    }
}

TEST_CASE("v33 reads a root-level preset marker for the active printer only",
          "[config][migration][ad5m]") {
    json config =
        migrate({{"active_printer_id", "a"},
                 {"preset", "ad5m_pro_zmod"},
                 {"printers",
                  {{"a", {{"fans", old_preset_fans()}}}, {"b", {{"fans", old_preset_fans()}}}}}});
    CHECK(config["printers"]["a"]["fans"]["aux"] == kSideFan);
    CHECK_FALSE(config["printers"]["a"]["fans"].contains("exhaust"));
    CHECK(config["printers"]["b"]["fans"] == old_preset_fans());
}
