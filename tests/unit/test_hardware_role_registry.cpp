// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later#include "config.h"
#include "hardware_role_registry.h"
#include "hardware_validator.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_fan_state.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

static const HardwareRoleDescriptor& part_fan_desc() {
    return *role_descriptor(HardwareRoleId::PartFan);
}

TEST_CASE("resolve_role: Tier 0 keeps a live saved value", "[hwrole][resolve]") {
    auto r = resolve_role(part_fan_desc(), "fan_generic Aux_Cooling_Fan",
                          {"fan", "fan_generic Aux_Cooling_Fan"});
    REQUIRE(r.status == RoleResolutionStatus::Resolved);
    REQUIRE(r.object == "fan_generic Aux_Cooling_Fan");
}

TEST_CASE("resolve_role: bundle scenario auto-heals stale output_pin to canonical fan",
          "[hwrole][resolve]") {
    // ULAV93T2: fans/part=output_pin fan0 (silenced/absent), real [fan] present.
    auto r = resolve_role(part_fan_desc(), "output_pin fan0",
                          {"fan", "heater_fan hotend_fan", "fan_generic Aux_Cooling_Fan"});
    REQUIRE(r.status == RoleResolutionStatus::AutoHealed);
    REQUIRE(r.object == "fan");
}

TEST_CASE("resolve_role: auto-heals via guess when canonical absent", "[hwrole][resolve]") {
    // No bare "fan"; guess_part_cooling_fan falls back to first fan.
    auto r = resolve_role(part_fan_desc(), "output_pin fan0", {"fan_generic Aux_Cooling_Fan"});
    REQUIRE(r.status == RoleResolutionStatus::AutoHealed);
    REQUIRE(r.object == "fan_generic Aux_Cooling_Fan");
}

TEST_CASE("resolve_role: bed heater unresolved when no bed-ish heater exists",
          "[hwrole][resolve]") {
    const auto& bed = *role_descriptor(HardwareRoleId::BedHeater);
    auto r = resolve_role(bed, "heater_bed", {"extruder"});
    REQUIRE(r.status == RoleResolutionStatus::Unresolved);
    REQUIRE(r.object.empty());
}

TEST_CASE("resolve_role: bed heater keeps canonical when present", "[hwrole][resolve]") {
    const auto& bed = *role_descriptor(HardwareRoleId::BedHeater);
    auto r = resolve_role(bed, "heater_bed", {"extruder", "heater_bed"});
    REQUIRE(r.status == RoleResolutionStatus::Resolved);
    REQUIRE(r.object == "heater_bed");
}

TEST_CASE("resolve_role: Tier 1b never returns an object outside discovered", "[hwrole][resolve]") {
    const auto& part = *role_descriptor(HardwareRoleId::PartFan);
    auto r = resolve_role(part, "output_pin fan0", {}); // nothing live
    REQUIRE(r.status == RoleResolutionStatus::Unresolved);
    REQUIRE(r.object.empty());
}

TEST_CASE("resolve_role: stale part fan with only wrong-category candidates stays Unresolved",
          "[hwrole][resolve]") {
    // Regression: without is_candidate, guess_part_cooling_fan() falls back to fans_[0]
    // (controller_fan Case_Fan) and that was silently accepted. With the predicate it is
    // rejected, so we correctly stay Unresolved rather than persisting a wrong-category fan.
    auto r = resolve_role(part_fan_desc(), "output_pin fan0",
                          {"controller_fan Case_Fan", "heater_fan hotend_fan"});
    REQUIRE(r.status == RoleResolutionStatus::Unresolved);
    REQUIRE(r.object.empty());
}

TEST_CASE("resolve_role: Tier 0 honors live saved wrong-category fan (user choice)",
          "[hwrole][resolve]") {
    // A user who explicitly saved a controller_fan as their part fan must have that
    // choice respected at Tier 0 — candidacy is intentionally not checked there.
    auto r = resolve_role(part_fan_desc(), "controller_fan Case_Fan",
                          {"controller_fan Case_Fan", "heater_fan hotend_fan"});
    REQUIRE(r.status == RoleResolutionStatus::Resolved);
    REQUIRE(r.object == "controller_fan Case_Fan");
}

TEST_CASE("registry integrity: every descriptor has a usable config key", "[hwrole][drift]") {
    const auto& reg = hardware_role_registry();
    REQUIRE(!reg.empty());
    for (const auto& d : reg) {
        REQUIRE(d.config_key != nullptr);
        REQUIRE(std::string(d.config_key).find("/") != std::string::npos);
        REQUIRE(role_descriptor(d.id) == &d);
    }
}

TEST_CASE("resolve_role: stale aux fan heals to a live aux-vocabulary fan", "[hwrole][resolve]") {
    const auto* desc = role_descriptor(HardwareRoleId::AuxFan);
    REQUIRE(desc != nullptr);
    auto r = resolve_role(*desc, "fan_generic gone", {"fan_generic internal_fan"});
    REQUIRE(r.status == RoleResolutionStatus::AutoHealed);
    REQUIRE(r.object == "fan_generic internal_fan");
}

TEST_CASE("aux fan role: descriptor is fans/aux, resolves live value, never invents one",
          "[hwrole][config]") {
    const auto* desc = role_descriptor(HardwareRoleId::AuxFan);
    REQUIRE(desc != nullptr);
    REQUIRE(std::string(desc->config_key) == "fans/aux");
    REQUIRE(std::string(desc->canonical_default).empty());

    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + helix::wizard::kAuxFan;
    const std::string orig = cfg->get<std::string>(key, "");

    cfg->set<std::string>(key, std::string("fan_generic internal_fan"));
    std::string r =
        resolve_role_from_config(HardwareRoleId::AuxFan, cfg, {"fan_generic internal_fan"}, false);
    REQUIRE(r == "fan_generic internal_fan");

    // An empty saved value means the role is unconfigured — no heuristic guess.
    cfg->set<std::string>(key, std::string(""));
    r = resolve_role_from_config(HardwareRoleId::AuxFan, cfg, {"fan_generic internal_fan"}, false);
    REQUIRE(r.empty());

    cfg->set<std::string>(key, orig); // restore
}

// FanRoleConfig::from_config is the batch the discovery sequence actually calls. A role
// the registry knows about but that batch never asks for stays unresolved forever.
TEST_CASE("FanRoleConfig::from_config resolves the aux fan role", "[hwrole][config]") {
    Config* cfg = Config::get_instance();

    const char* keys[] = {helix::wizard::PART_FAN, helix::wizard::HOTEND_FAN,
                          helix::wizard::CHAMBER_FAN, helix::wizard::EXHAUST_FAN,
                          helix::wizard::kAuxFan};
    std::vector<std::string> saved;
    for (const char* k : keys) {
        saved.push_back(cfg->get<std::string>(cfg->df() + k, ""));
    }
    // Leave every other role empty so from_config has nothing it would persist: this
    // test shares the process-wide Config and must not write the caller's settings.
    for (const char* k : keys) {
        cfg->set<std::string>(cfg->df() + k, std::string(""));
    }
    cfg->set<std::string>(cfg->df() + helix::wizard::kAuxFan,
                          std::string("fan_generic internal_fan"));

    auto roles = FanRoleConfig::from_config(cfg, {"fan", "fan_generic internal_fan"});
    REQUIRE(roles.aux_fan == "fan_generic internal_fan");

    for (size_t i = 0; i < saved.size(); ++i) {
        cfg->set<std::string>(cfg->df() + keys[i], saved[i]);
    }
}

TEST_CASE("resolve_role_from_config: unconfigured optional role stays empty", "[hwrole][config]") {
    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + helix::wizard::CHAMBER_FAN;
    const std::string orig = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, std::string(""));
    std::string r = resolve_role_from_config(HardwareRoleId::ChamberFan, cfg,
                                             {"fan", "fan_generic chamber_fan"}, false);
    REQUIRE(r.empty());               // empty saved => do not invent a role
    cfg->set<std::string>(key, orig); // restore
}

TEST_CASE("resolve_role_from_config: persists auto-healed part fan", "[hwrole][config]") {
    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + helix::wizard::PART_FAN;
    const std::string orig = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, std::string("output_pin fan0"));
    std::string r = resolve_role_from_config(HardwareRoleId::PartFan, cfg,
                                             {"fan", "fan_generic Aux_Cooling_Fan"}, true);
    REQUIRE(r == "fan");
    REQUIRE(cfg->get<std::string>(key, "") == "fan");
    cfg->set<std::string>(key, orig); // restore
}

TEST_CASE("resolve_role_from_config: no persist leaves config untouched", "[hwrole][config]") {
    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + helix::wizard::PART_FAN;
    const std::string orig = cfg->get<std::string>(key, "");
    cfg->set<std::string>(key, std::string("output_pin fan0"));
    std::string r = resolve_role_from_config(HardwareRoleId::PartFan, cfg, {"fan"}, false);
    REQUIRE(r == "fan");
    REQUIRE(cfg->get<std::string>(key, "") == "output_pin fan0");
    cfg->set<std::string>(key, orig); // restore
}

// ---------------------------------------------------------------------------
// unresolved_guided_steps: routes unresolvable guided roles to the targeted wizard
// ---------------------------------------------------------------------------

// Save/restore helper for the seven guided role keys so each [reconfig] test starts
// from a known clean slate regardless of residual singleton Config state.
namespace {
struct GuidedKeyStash {
    Config* cfg;
    std::vector<std::pair<std::string, std::string>> saved;
    explicit GuidedKeyStash(Config* c) : cfg(c) {
        for (const char* suffix :
             {helix::wizard::PART_FAN, helix::wizard::HOTEND_FAN, helix::wizard::CHAMBER_FAN,
              helix::wizard::EXHAUST_FAN, helix::wizard::kAuxFan, helix::wizard::HOTEND_HEATER,
              helix::wizard::BED_HEATER}) {
            std::string k = cfg->df() + suffix;
            saved.push_back({k, cfg->get<std::string>(k, "")});
        }
    }
    // Set every guided key to "" (unconfigured) as a baseline; callers then set the
    // specific keys they want present.
    void clear_all() {
        for (const auto& kv : saved)
            cfg->set<std::string>(kv.first, std::string(""));
    }
    void set(const char* suffix, const std::string& v) {
        cfg->set<std::string>(cfg->df() + suffix, v);
    }
    ~GuidedKeyStash() {
        for (const auto& kv : saved)
            cfg->set<std::string>(kv.first, kv.second);
    }
};
} // namespace

TEST_CASE("unresolved_guided_steps: bed-less printer (absent bed key) does NOT yield HeaterSelect",
          "[hwrole][reconfig]") {
    Config* cfg = Config::get_instance();
    GuidedKeyStash stash(cfg);
    stash.clear_all();

    // Bed-less printer: heaters/bed is "" (absent/declined) and discovery has no bed.
    // The hotend heater resolves; fans/part is "" (absent). Neither HeaterSelect nor
    // FanSelect must appear — an absent key is legitimate, not a reconfig prompt.
    stash.set(helix::wizard::HOTEND_HEATER, "extruder");
    stash.set(helix::wizard::BED_HEATER, ""); // explicit: no bed configured
    stash.set(helix::wizard::PART_FAN, "");   // explicit: no part fan configured

    MoonrakerClientMock client;
    client.set_heaters({"extruder"}); // bed-less
    client.set_fans({});

    auto steps = helix::unresolved_guided_steps(cfg, client.hardware());
    REQUIRE(std::find(steps.begin(), steps.end(), helix::wizard::StepId::HeaterSelect) ==
            steps.end());
    REQUIRE(std::find(steps.begin(), steps.end(), helix::wizard::StepId::FanSelect) == steps.end());
}

TEST_CASE("decline_unresolved_guided_roles: present unsatisfiable role becomes \"\", resolvable "
          "untouched",
          "[hwrole][reconfig]") {
    Config* cfg = Config::get_instance();
    GuidedKeyStash stash(cfg);
    stash.clear_all();

    // PART_FAN: present but unsatisfiable (no candidate fan discovered) → must be declined.
    // HOTEND_HEATER: present and resolvable (extruder is live) → must be left untouched.
    stash.set(helix::wizard::PART_FAN, "output_pin fan0");
    stash.set(helix::wizard::HOTEND_HEATER, "extruder");

    MoonrakerClientMock client;
    client.set_fans({"controller_fan Case_Fan"}); // no part-fan candidate
    client.set_heaters({"extruder", "heater_bed"});

    bool changed = helix::decline_unresolved_guided_roles(cfg, client.hardware());
    REQUIRE(changed);
    // Stale unsatisfiable role was written to "" (declined sentinel).
    REQUIRE(cfg->get<std::string>(cfg->df() + helix::wizard::PART_FAN, "MISSING").empty());
    // Resolvable role left exactly as configured.
    REQUIRE(cfg->get<std::string>(cfg->df() + helix::wizard::HOTEND_HEATER, "") == "extruder");

    // Idempotent: a second pass with the role now declined ("") changes nothing.
    REQUIRE_FALSE(helix::decline_unresolved_guided_roles(cfg, client.hardware()));
}

TEST_CASE("settle_targeted_reconfig: finishing declines guided roles the session's steps "
          "cannot show",
          "[hwrole][reconfig]") {
    // A preset can save a role the current printer cannot satisfy (ForgeX/zmod
    // AD5M Pro: fans/aux = "fan_generic internal_fan" with no such fan live).
    // FanSelect has no aux dropdown, so finishing the session can never resolve
    // the aux role — without the settlement the wizard relaunches every boot.
    Config* cfg = Config::get_instance();
    GuidedKeyStash stash(cfg);
    stash.clear_all();
    stash.set(helix::wizard::kAuxFan, "fan_generic internal_fan");
    stash.set(helix::wizard::PART_FAN, "fan"); // resolvable: the step has a control for it

    MoonrakerClientMock client;
    client.set_fans({"fan"}); // live part fan; nothing matches the aux vocabulary
    client.set_heaters({});

    // Discovery routes the unresolved aux role into a FanSelect session...
    std::vector<helix::wizard::StepId> expected = {helix::wizard::StepId::FanSelect};
    REQUIRE(helix::unresolved_guided_steps(cfg, client.hardware()) == expected);

    // ...the user finishes it (the app's on_complete runs the settlement)...
    REQUIRE(helix::settle_targeted_reconfig(cfg, client.hardware()));

    // ...and the next boot must not relaunch the wizard.
    REQUIRE(helix::unresolved_guided_steps(cfg, client.hardware()).empty());
}

// ---------------------------------------------------------------------------
// heal_heater_roles: the discovery-time batch over the hotend and bed roles.
// ---------------------------------------------------------------------------

namespace {

/// Saves the two heater role keys, restores them on scope exit.
struct HeaterRoleKeys {
    Config* cfg = Config::get_instance();
    std::string hotend_key = cfg->df() + helix::wizard::HOTEND_HEATER;
    std::string bed_key = cfg->df() + helix::wizard::BED_HEATER;
    std::string hotend_orig = cfg->get<std::string>(hotend_key, "");
    std::string bed_orig = cfg->get<std::string>(bed_key, "");
    ~HeaterRoleKeys() {
        cfg->set<std::string>(hotend_key, hotend_orig);
        cfg->set<std::string>(bed_key, bed_orig);
    }
    void set(const std::string& hotend, const std::string& bed) {
        cfg->set<std::string>(hotend_key, hotend);
        cfg->set<std::string>(bed_key, bed);
    }
};

} // namespace

TEST_CASE("heal_heater_roles: a stale saved role is replaced and persisted", "[hwrole][config]") {
    HeaterRoleKeys keys;
    keys.set("extruder9", "heater_bed");

    REQUIRE(helix::heal_heater_roles(keys.cfg, {"extruder", "heater_bed"}));
    CHECK(keys.cfg->get<std::string>(keys.hotend_key, "") == "extruder");
    CHECK(keys.cfg->get<std::string>(keys.bed_key, "") == "heater_bed");
}

TEST_CASE("heal_heater_roles: roles that already resolve change nothing", "[hwrole][config]") {
    HeaterRoleKeys keys;
    keys.set("extruder", "heater_bed");

    CHECK_FALSE(helix::heal_heater_roles(keys.cfg, {"extruder", "heater_bed"}));
    CHECK(keys.cfg->get<std::string>(keys.hotend_key, "") == "extruder");
}

TEST_CASE("heal_heater_roles: a bed-less printer keeps its saved bed role", "[hwrole][config]") {
    HeaterRoleKeys keys;
    keys.set("extruder", "heater_bed");

    CHECK_FALSE(helix::heal_heater_roles(keys.cfg, {"extruder"}));
    CHECK(keys.cfg->get<std::string>(keys.bed_key, "") == "heater_bed");
}

TEST_CASE("heal_heater_roles: no config is not a heal", "[hwrole][config]") {
    CHECK_FALSE(helix::heal_heater_roles(nullptr, {"extruder"}));
}

TEST_CASE("guess never returns a non-candidate", "[hwrole][guess]") {
    SECTION("a bed_fans fan is not a chamber fan") {
        const auto* desc = role_descriptor(HardwareRoleId::ChamberFan);
        CHECK(desc->guess({"fan", "fan_generic bed_fans"}).empty());
    }
    SECTION("a heat_fan fan_generic is not a hotend fan") {
        const auto* desc = role_descriptor(HardwareRoleId::HotendFan);
        CHECK(desc->guess({"fan", "fan_generic heat_fan"}).empty());
    }
    SECTION("a heater_generic named for e0 is not the hotend heater") {
        const auto* desc = role_descriptor(HardwareRoleId::HotendHeater);
        CHECK(desc->guess({"heater_bed", "heater_generic e0_block"}).empty());
    }
    SECTION("the part fan fallback skips auto-controlled fans") {
        const auto* desc = role_descriptor(HardwareRoleId::PartFan);
        CHECK(desc->guess({"controller_fan board", "temperature_fan chamber",
                           "fan_generic blower"}) == "fan_generic blower");
    }
    SECTION("every guess over a mixed list is accepted by its own predicate") {
        const std::vector<std::string> mixed = {
            "heater_fan hotend_fan", "controller_fan mcu_fan", "temperature_fan chamber",
            "fan_generic bed_fans",  "fan_generic heat_fan",   "fan_generic exhaust",
            "fan_generic aux",       "fan_generic nevermore",  "output_pin fan0"};
        for (const auto id :
             {HardwareRoleId::PartFan, HardwareRoleId::HotendFan, HardwareRoleId::ChamberFan,
              HardwareRoleId::ExhaustFan, HardwareRoleId::AuxFan}) {
            const auto* desc = role_descriptor(id);
            const std::string g = desc->guess(mixed);
            CHECK((g.empty() || desc->is_candidate(g)));
        }
    }
}

// ---------------------------------------------------------------------------
// guided_reconfig_steps: the targeted wizard asks only about hardware that went away
// ---------------------------------------------------------------------------

// A K1C on a modded Klipper config keeps the k1c preset's stock exhaust name,
// output_pin fan1, which this printer never had. Every boot() saves its snapshot as
// discovery does, whether or not anyone was asked (a print running, a wizard left open).
TEST_CASE("guided_reconfig_steps: asks only about hardware this printer has reported",
          "[hwrole][reconfig]") {
    Config* cfg = Config::get_instance();
    const std::string hw_key = cfg->df() + "hardware";
    const nlohmann::json* hw_node = cfg->try_get_json(hw_key);
    const nlohmann::json saved_hw = hw_node ? *hw_node : nlohmann::json::object();
    cfg->set<nlohmann::json>(hw_key, nlohmann::json::object());
    GuidedKeyStash stash(cfg);
    stash.clear_all();
    stash.set(helix::wizard::EXHAUST_FAN, "output_pin fan1");
    stash.set(helix::wizard::PART_FAN, "fan"); // the resolving roles are never asked about
    stash.set(helix::wizard::HOTEND_HEATER, "extruder");
    stash.set(helix::wizard::BED_HEATER, "heater_bed");
    const std::vector<std::string> fans = {"fan", "fan_generic chamber", "heater_fan hotend"};
    MoonrakerClientMock client;
    client.set_fans(fans);
    client.set_heaters({"extruder", "heater_bed"});
    const auto boot = [&] { HardwareValidator().save_session_snapshot(cfg, client.hardware()); };
    const auto steps = [&] {
        return helix::guided_reconfig_steps(cfg, client.hardware(),
                                            HardwareValidator::load_seen_hardware(cfg));
    };
    using helix::wizard::StepId;

    SECTION("a preset fan the printer never had is settled, not asked") {
        boot();
        CHECK(steps().empty());
        CHECK(cfg->get<std::string>(cfg->df() + helix::wizard::EXHAUST_FAN, "?").empty());
    }
    SECTION("a fan that went away while nobody was asked still gets the step") {
        client.set_fans({"fan", "output_pin fan1"});
        boot();
        client.set_fans(fans);
        boot();
        CHECK(steps() == std::vector<StepId>{StepId::FanSelect});
    }
    SECTION("a bed heater that went away while nobody was asked still gets the step") {
        stash.set(helix::wizard::EXHAUST_FAN, "");
        boot();
        client.set_heaters({"extruder"});
        boot();
        CHECK(steps() == std::vector<StepId>{StepId::HeaterSelect});
    }
    SECTION("with no record of this printer's fans it is asked, once for two roles") {
        stash.set(helix::wizard::HOTEND_FAN, "heater_fan gone_fan");
        client.set_fans({"fan"}); // no candidate for either stale fan role
        CHECK(steps() == std::vector<StepId>{StepId::FanSelect});
    }
    cfg->set<nlohmann::json>(hw_key, saved_hw);
}
