// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"
#include "hardware_role_registry.h"
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
    REQUIRE(cfg != nullptr);
    const std::string key = cfg->df() + helix::wizard::AUX_FAN;
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
    REQUIRE(cfg != nullptr);

    const char* keys[] = {helix::wizard::PART_FAN, helix::wizard::HOTEND_FAN,
                          helix::wizard::CHAMBER_FAN, helix::wizard::EXHAUST_FAN,
                          helix::wizard::AUX_FAN};
    std::vector<std::string> saved;
    for (const char* k : keys) {
        saved.push_back(cfg->get<std::string>(cfg->df() + k, ""));
    }
    // Leave every other role empty so from_config has nothing it would persist: this
    // test shares the process-wide Config and must not write the caller's settings.
    for (const char* k : keys) {
        cfg->set<std::string>(cfg->df() + k, std::string(""));
    }
    cfg->set<std::string>(cfg->df() + helix::wizard::AUX_FAN,
                          std::string("fan_generic internal_fan"));

    auto roles = FanRoleConfig::from_config(cfg, {"fan", "fan_generic internal_fan"});
    REQUIRE(roles.aux_fan == "fan_generic internal_fan");

    for (size_t i = 0; i < saved.size(); ++i) {
        cfg->set<std::string>(cfg->df() + keys[i], saved[i]);
    }
}

TEST_CASE("resolve_role_from_config: unconfigured optional role stays empty", "[hwrole][config]") {
    Config* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
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
              helix::wizard::EXHAUST_FAN, helix::wizard::AUX_FAN, helix::wizard::HOTEND_HEATER,
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

TEST_CASE("unresolved_guided_steps: present stale part fan with no candidate yields exactly "
          "{FanSelect}",
          "[hwrole][reconfig]") {
    Config* cfg = Config::get_instance();
    GuidedKeyStash stash(cfg);
    stash.clear_all();

    // PRESENT stale role: a part fan that no longer exists. Discovered fans exist but
    // none are a part-fan candidate (controller_fan/heater_fan are excluded), so neither
    // the canonical default nor the heuristic guess can resolve it. All other guided keys
    // are "" (unconfigured) → must be skipped, so the result is EXACTLY {FanSelect}.
    stash.set(helix::wizard::PART_FAN, "output_pin fan0");

    MoonrakerClientMock client;
    client.set_fans({"controller_fan Case_Fan", "heater_fan hotend_fan"});
    client.set_heaters({}); // no heaters → heater roles (all "") contribute nothing

    auto steps = helix::unresolved_guided_steps(cfg, client.hardware());
    std::vector<helix::wizard::StepId> expected = {helix::wizard::StepId::FanSelect};
    REQUIRE(steps == expected);
}

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

TEST_CASE("unresolved_guided_steps: empty when every guided role resolves", "[hwrole][reconfig]") {
    Config* cfg = Config::get_instance();
    struct KV {
        std::string key;
        std::string orig;
    };
    std::vector<KV> stash;
    auto save = [&](const char* suffix) {
        std::string k = cfg->df() + suffix;
        stash.push_back({k, cfg->get<std::string>(k, "")});
    };
    save(helix::wizard::PART_FAN);
    save(helix::wizard::HOTEND_FAN);
    save(helix::wizard::CHAMBER_FAN);
    save(helix::wizard::EXHAUST_FAN);
    save(helix::wizard::HOTEND_HEATER);
    save(helix::wizard::BED_HEATER);

    cfg->set<std::string>(cfg->df() + helix::wizard::PART_FAN, std::string("fan"));
    cfg->set<std::string>(cfg->df() + helix::wizard::HOTEND_FAN, std::string(""));
    cfg->set<std::string>(cfg->df() + helix::wizard::CHAMBER_FAN, std::string(""));
    cfg->set<std::string>(cfg->df() + helix::wizard::EXHAUST_FAN, std::string(""));
    cfg->set<std::string>(cfg->df() + helix::wizard::HOTEND_HEATER, std::string("extruder"));
    cfg->set<std::string>(cfg->df() + helix::wizard::BED_HEATER, std::string("heater_bed"));

    MoonrakerClientMock client;
    client.set_fans({"fan"});
    client.set_heaters({"extruder", "heater_bed"});

    auto steps = helix::unresolved_guided_steps(cfg, client.hardware());
    REQUIRE(steps.empty());

    for (const auto& kv : stash)
        cfg->set<std::string>(kv.key, kv.orig); // restore
}

TEST_CASE("unresolved_guided_steps: dedups multiple unresolved fan roles to one FanSelect",
          "[hwrole][reconfig]") {
    Config* cfg = Config::get_instance();
    const std::string pkey = cfg->df() + helix::wizard::PART_FAN;
    const std::string hkey = cfg->df() + helix::wizard::HOTEND_FAN;
    const std::string porig = cfg->get<std::string>(pkey, "");
    const std::string horig = cfg->get<std::string>(hkey, "");

    // Two distinct fan roles both unresolvable (no fans discovered). Heaters resolve
    // so only FAN-category steps can appear.
    cfg->set<std::string>(pkey, std::string("output_pin fan0"));
    cfg->set<std::string>(hkey, std::string("heater_fan gone_fan"));

    MoonrakerClientMock client;
    client.set_fans({});
    client.set_heaters({"extruder", "heater_bed"});

    auto steps = helix::unresolved_guided_steps(cfg, client.hardware());
    long fan_count = std::count(steps.begin(), steps.end(), helix::wizard::StepId::FanSelect);
    REQUIRE(fan_count == 1);

    cfg->set<std::string>(pkey, porig); // restore
    cfg->set<std::string>(hkey, horig);
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
    stash.set(helix::wizard::AUX_FAN, "fan_generic internal_fan");
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
