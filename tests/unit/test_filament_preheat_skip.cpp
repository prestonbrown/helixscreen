// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_preheat_skip.cpp
 * @brief When a dispatch surface must NOT heat the hotend itself.
 *
 * Run with: ./build/bin/helix-tests "[filament][preheat]"
 *
 * Three independent reasons to skip a preheat:
 *
 *   - the user said their own macros heat ("Allow cold load/unload"),
 *   - the AMS backend heats on load,
 *   - the stock macro about to run heats in its own body.
 *
 * The tier is load-bearing in two of the three: a macro that heats itself is
 * only a reason to skip when the macro is what actually runs. A rule that
 * ignored the tier would suppress the preheat on the backend and raw-gcode
 * paths too, where nothing but us heats and Klipper rejects the extrusion.
 */

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_snapmaker.h"
#include "ams_backend_toolchanger.h"
#include "filament_macro_profiles.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "printer_discovery.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using helix::ui::FilamentOpPlan;
using helix::ui::FilamentTier;
using helix::ui::PreheatSkip;

namespace {

/// AFC reports supports_auto_heat_on_load() == true.
class AutoHeatBackend : public helix::AmsBackendAfc {
  public:
    AutoHeatBackend() : helix::AmsBackendAfc(nullptr, nullptr) {}
};

/// Happy Hare does not override it, so it inherits the base "false".
class ColdBackend : public helix::AmsBackendHappyHare {
  public:
    ColdBackend() : helix::AmsBackendHappyHare(nullptr, nullptr) {}
};

FilamentOpPlan plan_at(FilamentTier tier) {
    FilamentOpPlan plan;
    plan.tier = tier;
    if (tier == FilamentTier::AmsBackend) {
        plan.ams_call = helix::ui::AmsCall::Load;
    }
    return plan;
}

/// Point the LoadFilament slot at whatever macros @p names describes.
void detect_with(std::initializer_list<const char*> names) {
    helix::PrinterDiscovery hardware;
    json objects = json::array({"extruder"});
    for (const char* n : names) {
        objects.push_back(std::string("gcode_macro ") + n);
    }
    hardware.parse_objects(objects);
    StandardMacros::instance().reset();
    StandardMacros::instance().init(hardware);
}

struct PreheatSkipFixture : public LVGLTestFixture {
    PreheatSkipFixture() {
        helix::SafetySettingsManager::instance().init_subjects();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
    }
    ~PreheatSkipFixture() {
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(false);
        StandardMacros::instance().reset();
    }
};

} // namespace

// ============================================================================
// Which macro names carry the "heats itself" claim
// ============================================================================

TEST_CASE("macro_heats_hotend: QIDI's stock load/unload heat in their own body",
          "[filament][preheat]") {
    // M604 sets the hotend with `M104 S{hotendtemp}`; M603 heats before it
    // retracts, which is why AmsBackendQidi drives it as `M603 S<temp>`.
    REQUIRE(helix::filament_macros::macro_heats_hotend("M604"));
    REQUIRE(helix::filament_macros::macro_heats_hotend("M603"));
}

TEST_CASE("macro_heats_hotend: Klipper macro names are case-insensitive", "[filament][preheat]") {
    REQUIRE(helix::filament_macros::macro_heats_hotend("m604"));
    REQUIRE(helix::filament_macros::macro_heats_hotend("m603"));
}

TEST_CASE("macro_heats_hotend: a generic name proves nothing about the body",
          "[filament][preheat]") {
    // The whole table rests on the name being a firmware-specific spelling. A
    // name any user could write themselves says nothing about whether the body
    // heats, and guessing wrong here means a cold extrude Klipper refuses.
    // Those users are served by "Allow cold load/unload" instead.
    REQUIRE_FALSE(helix::filament_macros::macro_heats_hotend("LOAD_FILAMENT"));
    REQUIRE_FALSE(helix::filament_macros::macro_heats_hotend("UNLOAD_FILAMENT"));
    REQUIRE_FALSE(helix::filament_macros::macro_heats_hotend("M701"));
    REQUIRE_FALSE(helix::filament_macros::macro_heats_hotend("M702"));
    REQUIRE_FALSE(helix::filament_macros::macro_heats_hotend(""));
}

// ============================================================================
// preheat_skip_reason
// ============================================================================

TEST_CASE_METHOD(PreheatSkipFixture, "preheat_skip_reason: the user's setting outranks every tier",
                 "[filament][preheat]") {
    detect_with({"LOAD_FILAMENT"});
    helix::SafetySettingsManager::instance().set_allow_cold_extrude(true);

    // Including RawGcode, which no detection covers: the user is telling us they
    // want the cold pull (#978), and only they can know that.
    for (FilamentTier tier :
         {FilamentTier::AmsBackend, FilamentTier::Macro, FilamentTier::RawGcode}) {
        ColdBackend backend;
        REQUIRE(helix::ui::preheat_skip_reason(plan_at(tier), StandardMacroSlot::LoadFilament,
                                               &backend) == PreheatSkip::UserOverride);
    }
}

TEST_CASE_METHOD(PreheatSkipFixture, "preheat_skip_reason: backend auto-heat, tier 1 only",
                 "[filament][preheat]") {
    detect_with({"LOAD_FILAMENT"});
    AutoHeatBackend backend;

    REQUIRE(helix::ui::preheat_skip_reason(plan_at(FilamentTier::AmsBackend),
                                           StandardMacroSlot::LoadFilament,
                                           &backend) == PreheatSkip::BackendSelfHeats);

    // A seated machine loads by swapping, and the backend heats for that too.
    FilamentOpPlan swap = plan_at(FilamentTier::AmsBackend);
    swap.ams_call = helix::ui::AmsCall::ChangeTool;
    REQUIRE(helix::ui::preheat_skip_reason(swap, StandardMacroSlot::LoadFilament, &backend) ==
            PreheatSkip::BackendSelfHeats);

    // An unload answers to its own claim, which AFC's TOOL_UNLOAD makes too.
    FilamentOpPlan unload = plan_at(FilamentTier::AmsBackend);
    unload.ams_call = helix::ui::AmsCall::Unload;
    REQUIRE(helix::ui::preheat_skip_reason(unload, StandardMacroSlot::UnloadFilament, &backend) ==
            PreheatSkip::BackendSelfHeats);

    // The same backend is no reason to skip once bypass has dropped the op past
    // tier 1 — the backend is not what runs there.
    REQUIRE(helix::ui::preheat_skip_reason(plan_at(FilamentTier::Macro),
                                           StandardMacroSlot::LoadFilament,
                                           &backend) == PreheatSkip::None);
}

TEST_CASE_METHOD(PreheatSkipFixture, "preheat_skip_reason: a backend that does not heat",
                 "[filament][preheat]") {
    detect_with({"LOAD_FILAMENT"});
    ColdBackend backend;
    REQUIRE(helix::ui::preheat_skip_reason(plan_at(FilamentTier::AmsBackend),
                                           StandardMacroSlot::LoadFilament,
                                           &backend) == PreheatSkip::None);

    // No backend at all cannot be one that heats.
    REQUIRE(helix::ui::preheat_skip_reason(plan_at(FilamentTier::AmsBackend),
                                           StandardMacroSlot::LoadFilament,
                                           nullptr) == PreheatSkip::None);
}

TEST_CASE_METHOD(PreheatSkipFixture, "preheat_skip_reason: a self-heating macro, tier 2 only",
                 "[filament][preheat]") {
    // A QIDI whose op has fallen through to the macro tier: M604 is what runs,
    // and it commands its own temperature. Preheating in front of it makes the
    // user wait twice and settles on OUR material temp first.
    detect_with({"M604"});
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).get_macro() == "M604");

    REQUIRE(helix::ui::preheat_skip_reason(plan_at(FilamentTier::Macro),
                                           StandardMacroSlot::LoadFilament,
                                           nullptr) == PreheatSkip::MacroSelfHeats);

    // Not on the raw-gcode fallback: a bare G1 E move needs the hotend above
    // min_extrude_temp or Klipper rejects it, and no macro runs there to heat.
    REQUIRE(helix::ui::preheat_skip_reason(plan_at(FilamentTier::RawGcode),
                                           StandardMacroSlot::LoadFilament,
                                           nullptr) == PreheatSkip::None);
}

TEST_CASE_METHOD(PreheatSkipFixture, "preheat_skip_reason: an ordinary macro still preheats",
                 "[filament][preheat]") {
    detect_with({"LOAD_FILAMENT"});
    REQUIRE(helix::ui::preheat_skip_reason(plan_at(FilamentTier::Macro),
                                           StandardMacroSlot::LoadFilament,
                                           nullptr) == PreheatSkip::None);
}

TEST_CASE_METHOD(PreheatSkipFixture, "preheat_skip_reason: a refused plan heats nothing",
                 "[filament][preheat]") {
    // Nothing is about to run, so there is nothing to be redundant with. The
    // caller returns on the refusal before it ever asks, but answering None here
    // keeps "skip" from ever meaning "dispatch".
    detect_with({"M604"});
    REQUIRE(helix::ui::preheat_skip_reason(plan_at(FilamentTier::Refused),
                                           StandardMacroSlot::LoadFilament,
                                           nullptr) == PreheatSkip::None);
}

// ============================================================================
// needs_prerequisite_home
// ============================================================================

TEST_CASE("prerequisite_home_needed: homes only for a backend or profile that needs it",
          "[filament][homing]") {
    struct Row {
        const char* what;
        FilamentTier tier;
        bool toolhead_homed;
        bool backend_delegates_homing;
        bool macro_needs_home_first;
        bool expect_home;
    };
    // An unrecognized macro is the user's own and runs as it is (#1819): its
    // body may only extrude, and a G28 in front of it leaves the head low.
    const Row row = GENERATE(values<Row>({
        {"unrecognized macro, unhomed", FilamentTier::Macro, false, false, false, false},
        {"profile needing a home, unhomed", FilamentTier::Macro, false, false, true, true},
        {"profile needing a home, already homed", FilamentTier::Macro, true, false, true, false},
        {"backend homing for itself, unhomed", FilamentTier::AmsBackend, false, false, false, true},
        {"backend delegating to the printer, unhomed", FilamentTier::AmsBackend, false, true, false,
         false},
        {"backend, already homed", FilamentTier::AmsBackend, true, false, false, false},
        {"profile claim does not reach the backend tier", FilamentTier::AmsBackend, false, true,
         true, false},
        {"backend claim does not reach the macro tier", FilamentTier::Macro, false, true, true,
         true},
        {"raw gcode, unhomed", FilamentTier::RawGcode, false, false, true, false},
        {"refused, unhomed", FilamentTier::Refused, false, false, true, false},
    }));
    INFO(row.what);
    CHECK(helix::ui::prerequisite_home_needed(row.tier, row.toolhead_homed,
                                              row.backend_delegates_homing,
                                              row.macro_needs_home_first) == row.expect_home);
}

TEST_CASE_METHOD(PreheatSkipFixture,
                 "needs_prerequisite_home: an unrecognized load macro runs unhomed as it is",
                 "[filament][preheat][homing]") {
    detect_with({"LOAD_FILAMENT"});
    REQUIRE_FALSE(helix::filament_macros::macro_needs_home_first("LOAD_FILAMENT"));
    REQUIRE_FALSE(helix::ui::needs_prerequisite_home(plan_at(FilamentTier::Macro),
                                                     StandardMacroSlot::LoadFilament, nullptr,
                                                     /*toolhead_homed=*/false));
}

TEST_CASE_METHOD(PreheatSkipFixture,
                 "needs_prerequisite_home: an unrecognized unload macro runs unhomed as it is",
                 "[filament][preheat][homing]") {
    detect_with({"UNLOAD_FILAMENT"});
    REQUIRE_FALSE(helix::ui::needs_prerequisite_home(plan_at(FilamentTier::Macro),
                                                     StandardMacroSlot::UnloadFilament, nullptr,
                                                     /*toolhead_homed=*/false));
}

TEST_CASE_METHOD(PreheatSkipFixture, "needs_prerequisite_home: a macro carrying its own home",
                 "[filament][preheat][homing]") {
    // M604 opens with _CG28, so a G28 of ours in front of it adds nothing.
    detect_with({"M604"});
    REQUIRE_FALSE(helix::filament_macros::macro_needs_home_first("M604"));
    REQUIRE_FALSE(helix::ui::needs_prerequisite_home(plan_at(FilamentTier::Macro),
                                                     StandardMacroSlot::LoadFilament, nullptr,
                                                     /*toolhead_homed=*/false));

    // The claim is about that macro run alone. A backend merely composing it can
    // add unguarded moves of its own, so tier 1 does not inherit the answer.
    AutoHeatBackend backend;
    REQUIRE(helix::ui::needs_prerequisite_home(plan_at(FilamentTier::AmsBackend),
                                               StandardMacroSlot::LoadFilament, &backend,
                                               /*toolhead_homed=*/false));
}

TEST_CASE_METHOD(PreheatSkipFixture, "needs_prerequisite_home: a backend that does not delegate",
                 "[filament][preheat][homing]") {
    // Happy Hare and QIDI Box both answer delegates_homing_to_printer() false;
    // only AFC with [AFC] auto_home set answers true, and this one has no config
    // loaded, so it reads false too.
    detect_with({"LOAD_FILAMENT"});
    ColdBackend backend;
    REQUIRE(helix::ui::needs_prerequisite_home(plan_at(FilamentTier::AmsBackend),
                                               StandardMacroSlot::LoadFilament, &backend,
                                               /*toolhead_homed=*/false));
    REQUIRE_FALSE(helix::ui::needs_prerequisite_home(plan_at(FilamentTier::AmsBackend),
                                                     StandardMacroSlot::LoadFilament, &backend,
                                                     /*toolhead_homed=*/true));
}

TEST_CASE_METHOD(PreheatSkipFixture, "preheat_skip_reason: an unload answers to the unload claim",
                 "[filament][preheat][skip]") {
    FilamentOpPlan unload = plan_at(FilamentTier::AmsBackend);
    unload.ams_call = helix::ui::AmsCall::Unload;

    // Happy Hare claims neither, so the UI heats for its unload.
    ColdBackend cold;
    REQUIRE_FALSE(cold.supports_auto_heat_on_unload());
    CHECK(helix::ui::preheat_skip_reason(unload, StandardMacroSlot::UnloadFilament, &cold) ==
          PreheatSkip::None);

    // AFC's TOOL_UNLOAD heats the extruder itself.
    helix::AmsBackendAfc afc(nullptr, nullptr);
    REQUIRE(afc.supports_auto_heat_on_unload());
    CHECK(helix::ui::preheat_skip_reason(unload, StandardMacroSlot::UnloadFilament, &afc) ==
          PreheatSkip::BackendSelfHeats);

    // The unload claim alone is no reason to skip a load on a backend that
    // does not claim that too.
    helix::AmsBackendToolChanger changer(nullptr, nullptr);
    REQUIRE_FALSE(changer.supports_auto_heat_on_unload());
    FilamentOpPlan load = plan_at(FilamentTier::AmsBackend);
    CHECK(helix::ui::preheat_skip_reason(load, StandardMacroSlot::LoadFilament, &changer) ==
          PreheatSkip::None);
}

TEST_CASE_METHOD(PreheatSkipFixture, "preheat_skip_reason: an unload claim is per unload",
                 "[filament][preheat][skip]") {
    FilamentOpPlan unload = plan_at(FilamentTier::AmsBackend);
    unload.ams_call = helix::ui::AmsCall::Unload;

    // No CFS unload script we send heats; the bypass one ends in a raw retract.
    helix::printer::AmsBackendCfs cfs(nullptr, nullptr);
    unload.ams_arg = helix::ui::kExternalSpoolSlot;
    CHECK(helix::ui::preheat_skip_reason(unload, StandardMacroSlot::UnloadFilament, &cfs) ==
          PreheatSkip::None);
    unload.ams_arg = 0;
    CHECK(helix::ui::preheat_skip_reason(unload, StandardMacroSlot::UnloadFilament, &cfs) ==
          PreheatSkip::None);

    // The U1's AUTO_FEEDING unload heats; with no slot to name it falls back to
    // the bare INNER_FILAMENT_UNLOAD leaf, which does not.
    helix::AmsBackendSnapmaker u1(nullptr, nullptr);
    CHECK(u1.unload_heats_hotend(0));
    CHECK_FALSE(u1.unload_heats_hotend(-1)); // nothing loaded to resolve
    unload.ams_arg = -1;
    CHECK(helix::ui::preheat_skip_reason(unload, StandardMacroSlot::UnloadFilament, &u1) ==
          PreheatSkip::None);
}
