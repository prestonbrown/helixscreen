// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure tests for the plugin-driven power-loss resume (include/plr_wizard.h):
// the bed and confirmation decisions, the wizard's step machine, and the
// readers for the plugin's server.helix.plr.* replies. No LVGL, no network.

#include "plr_plugin_rpc.h"
#include "plr_wizard.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PlrBedState;
using helix::PlrConfirmation;
using helix::PlrPlanView;
using helix::PlrWizardInputs;
using helix::PlrWizardStep;
using json = nlohmann::json;

namespace {

PlrPlanView plan_with_tier(int tier) {
    PlrPlanView plan;
    plan.tier = tier;
    plan.plan_id = "p1";
    plan.bed_target = 60.0f;
    plan.bed_current = 60.0f;
    plan.backend = helix::PlrPluginBackend::Generic;
    return plan;
}

PlrWizardInputs at(PlrWizardStep step, const PlrPlanView* plan) {
    PlrWizardInputs in;
    in.step = step;
    in.plan = plan;
    return in;
}

const std::vector<PlrConfirmation> kTier1All = {
    PlrConfirmation::PartAttached, PlrConfirmation::NozzleClear, PlrConfirmation::ZJogged};
const std::vector<PlrConfirmation> kTier2All = {PlrConfirmation::PartAttached,
                                                PlrConfirmation::ZConfirmed};

} // namespace

// ===========================================================================
// plr_prepare_moves_head / plr_z_step_kind
// ===========================================================================

TEST_CASE("plr_z_step_kind: the Z step says where prepare left the nozzle", "[plr][wizard]") {
    using helix::PlrPluginBackend;
    using helix::PlrZStepKind;

    PlrPlanView generic = plan_with_tier(2);
    CHECK(helix::plr_prepare_moves_head(generic));
    CHECK(helix::plr_z_step_kind(generic) == PlrZStepKind::ConfirmLifted);

    // QIDI's prepare only heats: the nozzle is still where the power cut left it.
    PlrPlanView qidi = plan_with_tier(2);
    qidi.backend = PlrPluginBackend::QidiPrintState;
    CHECK_FALSE(helix::plr_prepare_moves_head(qidi));
    CHECK(helix::plr_z_step_kind(qidi) == PlrZStepKind::ConfirmInPlace);

    // Tier 1 always jogs down to the part, whatever prepare did.
    PlrPlanView jog = plan_with_tier(1);
    CHECK(helix::plr_z_step_kind(jog) == PlrZStepKind::Jog);
    jog.backend = PlrPluginBackend::QidiPrintState;
    CHECK(helix::plr_z_step_kind(jog) == PlrZStepKind::Jog);

    PlrPlanView none = plan_with_tier(2);
    none.backend = PlrPluginBackend::None;
    CHECK_FALSE(helix::plr_prepare_moves_head(none));
}

// ===========================================================================
// plr_bed_decision
// ===========================================================================

TEST_CASE("plr_bed_decision: ok, wait, cold", "[plr][wizard]") {
    CHECK(helix::plr_bed_decision(60.0f, 60.0f, 5.0f) == PlrBedState::Ok);
    CHECK(helix::plr_bed_decision(60.0f, 50.0f, 5.0f) == PlrBedState::NeedsWait);
    CHECK(helix::plr_bed_decision(60.0f, 22.0f, 5.0f) == PlrBedState::ColdWarn);
    CHECK(helix::plr_bed_decision(0.0f, 22.0f, 5.0f) == PlrBedState::Ok);
}

TEST_CASE("plr_bed_decision: edges of each band", "[plr][wizard]") {
    // Within max_diff is at temperature; exactly the cold threshold still waits.
    CHECK(helix::plr_bed_decision(60.0f, 55.0f, 5.0f) == PlrBedState::Ok);
    CHECK(helix::plr_bed_decision(60.0f, 54.9f, 5.0f) == PlrBedState::NeedsWait);
    CHECK(helix::plr_bed_decision(60.0f, 60.0f - helix::kPlrColdBedDiffC, 5.0f) ==
          PlrBedState::NeedsWait);
    CHECK(helix::plr_bed_decision(60.0f, 49.9f, 5.0f) == PlrBedState::ColdWarn);
    // A bed above target is never cold.
    CHECK(helix::plr_bed_decision(60.0f, 70.0f, 5.0f) == PlrBedState::Ok);
}

// ===========================================================================
// plr_required_confirmations / plr_commit_allowed
// ===========================================================================

TEST_CASE("plr_required_confirmations: per tier and per warning", "[plr][wizard]") {
    SECTION("tier 1 jogs to the part") {
        CHECK(helix::plr_required_confirmations(plan_with_tier(1), PlrBedState::Ok) == kTier1All);
    }
    SECTION("tier 2 trusts the held Z") {
        CHECK(helix::plr_required_confirmations(plan_with_tier(2), PlrBedState::Ok) == kTier2All);
    }
    SECTION("mesh_unavailable adds accept_no_mesh, layer_start adds accept_next_layer") {
        PlrPlanView plan = plan_with_tier(2);
        plan.warnings = {"layer_start", "mesh_unavailable"};
        auto need = helix::plr_required_confirmations(plan, PlrBedState::Ok);
        CHECK(need == std::vector<PlrConfirmation>{
                          PlrConfirmation::PartAttached, PlrConfirmation::ZConfirmed,
                          PlrConfirmation::AcceptNoMesh, PlrConfirmation::AcceptNextLayer});
        CHECK(std::string(helix::plr_confirmation_name(PlrConfirmation::AcceptNextLayer)) ==
              "accept_next_layer");
    }
    SECTION("ColdWarn adds accept_cold_bed, NeedsWait does not") {
        auto cold = helix::plr_required_confirmations(plan_with_tier(1), PlrBedState::ColdWarn);
        CHECK(cold.back() == PlrConfirmation::AcceptColdBed);
        CHECK(helix::plr_required_confirmations(plan_with_tier(1), PlrBedState::NeedsWait) ==
              kTier1All);
    }
    SECTION("the plugin's own requires join without duplicates") {
        PlrPlanView plan = plan_with_tier(2);
        plan.plugin_requires = {PlrConfirmation::PartAttached, PlrConfirmation::NozzleClear};
        auto need = helix::plr_required_confirmations(plan, PlrBedState::Ok);
        CHECK(need == std::vector<PlrConfirmation>{PlrConfirmation::PartAttached,
                                                   PlrConfirmation::ZConfirmed,
                                                   PlrConfirmation::NozzleClear});
    }
    SECTION("a tier that cannot resume needs nothing because it never commits") {
        CHECK(helix::plr_required_confirmations(plan_with_tier(0), PlrBedState::Ok).empty());
        CHECK(helix::plr_required_confirmations(plan_with_tier(3), PlrBedState::Ok).empty());
    }
}

TEST_CASE("plr_check_confirmations: everything but the Z answer", "[plr][wizard]") {
    PlrPlanView plan = plan_with_tier(1);
    plan.warnings = {"mesh_unavailable"};
    CHECK(helix::plr_check_confirmations(plan, PlrBedState::ColdWarn) ==
          std::vector<PlrConfirmation>{PlrConfirmation::PartAttached, PlrConfirmation::NozzleClear,
                                       PlrConfirmation::AcceptNoMesh,
                                       PlrConfirmation::AcceptColdBed});
}

TEST_CASE("plr_commit_allowed: every required confirmation, a resumable tier, a plan",
          "[plr][wizard]") {
    PlrPlanView t1 = plan_with_tier(1);
    CHECK(helix::plr_commit_allowed(t1, kTier1All, PlrBedState::Ok));
    CHECK_FALSE(helix::plr_commit_allowed(
        t1, {PlrConfirmation::PartAttached, PlrConfirmation::NozzleClear}, PlrBedState::Ok));
    // Tier 2's confirmation does not stand in for tier 1's jog.
    CHECK_FALSE(helix::plr_commit_allowed(
        t1,
        {PlrConfirmation::PartAttached, PlrConfirmation::NozzleClear, PlrConfirmation::ZConfirmed},
        PlrBedState::Ok));
    CHECK_FALSE(helix::plr_commit_allowed(t1, kTier1All, PlrBedState::ColdWarn));

    PlrPlanView t0 = plan_with_tier(0);
    CHECK_FALSE(helix::plr_commit_allowed(t0, kTier2All, PlrBedState::Ok));

    PlrPlanView unknown = plan_with_tier(2);
    unknown.unsatisfiable = true;
    CHECK_FALSE(helix::plr_commit_allowed(unknown, kTier2All, PlrBedState::Ok));

    PlrPlanView no_id = plan_with_tier(2);
    no_id.plan_id.clear();
    CHECK_FALSE(helix::plr_commit_allowed(no_id, kTier2All, PlrBedState::Ok));
}

// ===========================================================================
// plr_next_wizard_step
// ===========================================================================

TEST_CASE("plr_next_wizard_step: the happy path, tier 2", "[plr][wizard]") {
    PlrPlanView plan = plan_with_tier(2);

    auto in = at(PlrWizardStep::Offer, nullptr);
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Offer);
    in.plan = &plan;
    in.rpc_ok = true;
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Checks);

    in = at(PlrWizardStep::Checks, &plan);
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Checks);
    in.given = {PlrConfirmation::PartAttached};
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Preparing);

    in = at(PlrWizardStep::Preparing, &plan);
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Preparing);
    in.rpc_ok = true;
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::ZStep);

    in = at(PlrWizardStep::ZStep, &plan);
    in.given = {PlrConfirmation::PartAttached};
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::ZStep);
    in.given = kTier2All;
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Committing);

    in = at(PlrWizardStep::Committing, &plan);
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Committing);
    in.rpc_ok = true;
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Done);
}

TEST_CASE("plr_next_wizard_step: Checks waits for every pre-motion answer", "[plr][wizard]") {
    PlrPlanView plan = plan_with_tier(1);
    plan.warnings = {"mesh_unavailable"};
    auto in = at(PlrWizardStep::Checks, &plan);
    in.given = {PlrConfirmation::PartAttached, PlrConfirmation::NozzleClear};
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Checks);
    in.given.push_back(PlrConfirmation::AcceptNoMesh);
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Preparing);
}

TEST_CASE("plr_next_wizard_step: abort from every live step", "[plr][wizard]") {
    PlrPlanView plan = plan_with_tier(1);
    for (PlrWizardStep s : {PlrWizardStep::Offer, PlrWizardStep::Checks, PlrWizardStep::Preparing,
                            PlrWizardStep::ZStep, PlrWizardStep::Committing}) {
        auto in = at(s, &plan);
        in.user_abort = true;
        in.rpc_ok = true;
        in.given = kTier1All;
        CAPTURE(static_cast<int>(s));
        CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Aborted);
    }
}

TEST_CASE("plr_next_wizard_step: a lost connection fails, never commits", "[plr][wizard]") {
    PlrPlanView plan = plan_with_tier(2);
    for (PlrWizardStep s : {PlrWizardStep::Offer, PlrWizardStep::Checks, PlrWizardStep::Preparing,
                            PlrWizardStep::ZStep, PlrWizardStep::Committing}) {
        auto in = at(s, &plan);
        in.connected = false;
        in.rpc_ok = true;
        in.given = kTier2All;
        CAPTURE(static_cast<int>(s));
        CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Failed);
    }
}

TEST_CASE("plr_next_wizard_step: a failed RPC fails its step", "[plr][wizard]") {
    PlrPlanView plan = plan_with_tier(2);
    for (PlrWizardStep s :
         {PlrWizardStep::Offer, PlrWizardStep::Preparing, PlrWizardStep::Committing}) {
        auto in = at(s, &plan);
        in.rpc_failed = true;
        CAPTURE(static_cast<int>(s));
        CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Failed);
    }
}

TEST_CASE("plr_next_wizard_step: terminal steps stay put", "[plr][wizard]") {
    PlrPlanView plan = plan_with_tier(2);
    for (PlrWizardStep s : {PlrWizardStep::Done, PlrWizardStep::Failed, PlrWizardStep::Aborted}) {
        auto in = at(s, &plan);
        in.connected = false;
        in.user_abort = true;
        in.rpc_ok = true;
        CAPTURE(static_cast<int>(s));
        CHECK(helix::plr_next_wizard_step(in) == s);
    }
}

TEST_CASE("plr_next_wizard_step: a plan the app cannot complete fails at the offer",
          "[plr][wizard]") {
    PlrPlanView unknown = plan_with_tier(2);
    unknown.unsatisfiable = true;
    auto in = at(PlrWizardStep::Offer, &unknown);
    in.rpc_ok = true;
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Failed);

    PlrPlanView tier0 = plan_with_tier(0);
    in.plan = &tier0;
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Failed);
}

TEST_CASE("plr_next_wizard_step: a step past Offer without a plan fails", "[plr][wizard]") {
    auto in = at(PlrWizardStep::ZStep, nullptr);
    in.given = kTier2All;
    CHECK(helix::plr_next_wizard_step(in) == PlrWizardStep::Failed);
}

// ===========================================================================
// Wire readers
// ===========================================================================

TEST_CASE("plr_confirmation_name round-trips every confirmation", "[plr][wizard]") {
    for (PlrConfirmation c : {PlrConfirmation::PartAttached, PlrConfirmation::NozzleClear,
                              PlrConfirmation::ZConfirmed, PlrConfirmation::ZJogged,
                              PlrConfirmation::AcceptNoMesh, PlrConfirmation::AcceptColdBed}) {
        auto parsed = helix::plr_parse_confirmation(helix::plr_confirmation_name(c));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == c);
    }
    CHECK(std::string(helix::plr_confirmation_name(PlrConfirmation::ZJogged)) == "z_jogged");
    CHECK_FALSE(helix::plr_parse_confirmation("bogus").has_value());
}

TEST_CASE("plr_parse_plugin_status: the status reply", "[plr][wizard]") {
    json result = {{"saving", true},
                   {"storage",
                    {{"storage_class", "emmc"},
                     {"wear_tolerant", true},
                     {"source", "runtime"},
                     {"device", "mmcblk0"}}},
                   {"tier", 2},
                   {"backend", "generic"},
                   {"snapshot",
                    {{"job_id", "000012"},
                     {"filename", "dir/part.gcode"},
                     {"layer", 42},
                     {"z", 8.4},
                     {"saved_at", 1760000000.0},
                     {"age_s", 720.0}}},
                   {"offer", {{"available", true}, {"blocked_by", json::array()}}}};
    auto s = helix::plr_parse_plugin_status(result);
    CHECK(s.saving);
    CHECK(s.tier == 2);
    CHECK(s.backend == helix::PlrPluginBackend::Generic);
    REQUIRE(s.snapshot.has_value());
    CHECK(s.snapshot->filename == "dir/part.gcode");
    CHECK(s.snapshot->layer == 42);
    CHECK(s.snapshot->z == Catch::Approx(8.4));
    CHECK(s.snapshot->age_s == Catch::Approx(720.0));
    CHECK(s.offer_available);
    CHECK(s.blocked_by.empty());
}

TEST_CASE("plr_parse_plugin_status: blocked, malformed and backend names", "[plr][wizard]") {
    SECTION("blocked_by is read") {
        auto s = helix::plr_parse_plugin_status(json{
            {"backend", "qidi_print_state"},
            {"snapshot", nullptr},
            {"offer", {{"available", false}, {"blocked_by", {"too_early", "klippy_not_ready"}}}}});
        CHECK(s.backend == helix::PlrPluginBackend::QidiPrintState);
        CHECK_FALSE(s.snapshot.has_value());
        CHECK(s.blocked_by == std::vector<std::string>{"too_early", "klippy_not_ready"});
    }
    SECTION("an offer with no snapshot is no offer") {
        auto s = helix::plr_parse_plugin_status(
            json{{"backend", "generic"}, {"offer", {{"available", true}}}});
        CHECK_FALSE(s.offer_available);
    }
    SECTION("wrong types read as defaults and never throw") {
        auto s = helix::plr_parse_plugin_status(json{
            {"saving", json::object()}, {"tier", "two"}, {"backend", 7}, {"offer", "available"}});
        CHECK_FALSE(s.saving);
        CHECK(s.tier == 0);
        CHECK(s.backend == helix::PlrPluginBackend::None);
        CHECK_FALSE(s.offer_available);
        CHECK(helix::plr_parse_plugin_status(json::array()).backend ==
              helix::PlrPluginBackend::None);
    }
    CHECK(helix::plr_parse_plugin_backend("none") == helix::PlrPluginBackend::None);
}

TEST_CASE("plr_parse_plan_reply: the plan reply", "[plr][wizard]") {
    json result = {{"plan_id", "plan-7"},
                   {"filename", "part.gcode"},
                   {"method", "measured"},
                   {"resume_offset", 123456789012LL},
                   {"layer", 42},
                   {"z_gcode", 8.4},
                   {"z_physical", 8.52},
                   {"retrace_estimate_mm", 3.5},
                   {"bed", {{"target", 60}, {"current", 31.5}}},
                   {"restores", {"mesh", "pressure_advance"}},
                   {"warnings", {"mesh_unavailable"}},
                   {"requires", {"part_attached", "z_confirmed", "accept_no_mesh"}}};
    auto plan = helix::plr_parse_plan_reply(result);
    REQUIRE(plan.has_value());
    CHECK_FALSE(plan->pending);
    CHECK(plan->plan_id == "plan-7");
    CHECK(plan->method == "measured");
    CHECK(plan->resume_offset == 123456789012LL);
    CHECK(plan->layer == 42);
    CHECK(plan->z_gcode == Catch::Approx(8.4));
    CHECK(plan->bed_target == Catch::Approx(60.0f));
    CHECK(plan->bed_current == Catch::Approx(31.5f));
    CHECK(plan->warnings == std::vector<std::string>{"mesh_unavailable"});
    CHECK(plan->requires_confirmations ==
          std::vector<PlrConfirmation>{PlrConfirmation::PartAttached, PlrConfirmation::ZConfirmed,
                                       PlrConfirmation::AcceptNoMesh});
    CHECK(plan->unknown_requires.empty());
    CHECK_FALSE(plan->tier.has_value());

    auto view = helix::plr_plan_view(*plan, 2, helix::PlrPluginBackend::Generic);
    CHECK(view.tier == 2);
    CHECK(view.plan_id == "plan-7");
    CHECK(view.bed_current == Catch::Approx(31.5f));
    CHECK_FALSE(view.unsatisfiable);
    CHECK(helix::plr_plan_bed_state(view) == PlrBedState::ColdWarn);
}

TEST_CASE("plr_parse_plan_reply: pending, unknown requirements, plan tier", "[plr][wizard]") {
    SECTION("a long scan answers pending") {
        auto plan = helix::plr_parse_plan_reply(json{{"pending", true}});
        REQUIRE(plan.has_value());
        CHECK(plan->pending);
    }
    SECTION("a plan with no id is no plan") {
        CHECK_FALSE(helix::plr_parse_plan_reply(json{{"filename", "x.gcode"}}).has_value());
        CHECK_FALSE(helix::plr_parse_plan_reply(json("plan")).has_value());
    }
    SECTION("a requirement this app does not know makes the plan unsatisfiable") {
        auto plan = helix::plr_parse_plan_reply(
            json{{"plan_id", "p"}, {"requires", {"part_attached", "new_thing"}}});
        REQUIRE(plan.has_value());
        CHECK(plan->unknown_requires == std::vector<std::string>{"new_thing"});
        CHECK(helix::plr_plan_view(*plan, 2, helix::PlrPluginBackend::Generic).unsatisfiable);
    }
    SECTION("a tier in the plan outranks the status tier") {
        auto plan = helix::plr_parse_plan_reply(json{{"plan_id", "p"}, {"tier", 1}});
        REQUIRE(plan.has_value());
        CHECK(helix::plr_plan_view(*plan, 2, helix::PlrPluginBackend::Generic).tier == 1);
    }
}

TEST_CASE("plr_parse_progress_event and plan_ready read the notification payload",
          "[plr][wizard]") {
    json msg = {{"jsonrpc", "2.0"},
                {"method", "notify_plr_progress"},
                {"params", {{{"plan_id", "p1"}, {"step", "bed_wait"}, {"state", "running"}}}}};
    auto ev = helix::plr_parse_progress_event(msg);
    REQUIRE(ev.has_value());
    CHECK(ev->plan_id == "p1");
    CHECK(ev->step == "bed_wait");
    CHECK(ev->state == "running");
    CHECK_FALSE(helix::plr_parse_progress_event(json{{"params", json::array()}}).has_value());

    CHECK(helix::plr_parse_plan_ready(json{{"params", {{{"plan_id", "p9"}}}}}).plan_id == "p9");
    auto failed = helix::plr_parse_plan_ready(
        json{{"params", {{{"plan_id", "p9"}, {"error", "multi_material"}}}}});
    CHECK(failed.error == "multi_material");
    CHECK(helix::plr_parse_plan_ready(json::object()).plan_id.empty());
}

TEST_CASE("plr_commit_params: confirmations and the tier's Z reference", "[plr][wizard]") {
    json t1 = helix::plr_commit_params("p1", kTier1All, 1);
    CHECK(t1["plan_id"] == "p1");
    CHECK(t1["z_reference"] == "part_top");
    CHECK(t1["confirmations"] ==
          json{{"part_attached", true}, {"nozzle_clear", true}, {"z_jogged", true}});

    json t2 = helix::plr_commit_params("p2", kTier2All, 2);
    CHECK(t2["z_reference"] == "trusted");
    CHECK(t2["confirmations"].size() == 2);
}

TEST_CASE("plr_blocked_reason: every plugin code has words, an unknown one is itself",
          "[plr][wizard]") {
    for (const char* code : {"not_interrupted", "no_snapshot", "file_changed", "too_early",
                             "not_supported", "klippy_not_ready", "no_lift_room"}) {
        INFO(code);
        CHECK(helix::plr_blocked_reason(code) != code);
    }
    CHECK(helix::plr_blocked_reason("no_lift_room").find("headroom") != std::string::npos);
    CHECK(helix::plr_blocked_reason("some_new_rule") == "some_new_rule");
}

TEST_CASE("plr_commit_allowed: a layer_start plan waits for accept_next_layer", "[plr][wizard]") {
    PlrPlanView plan = plan_with_tier(2);
    plan.warnings = {"layer_start"};
    std::vector<PlrConfirmation> given = kTier2All;
    CHECK_FALSE(helix::plr_commit_allowed(plan, given, PlrBedState::Ok));
    given.push_back(PlrConfirmation::AcceptNextLayer);
    CHECK(helix::plr_commit_allowed(plan, given, PlrBedState::Ok));
}

TEST_CASE("PLR prepare and commit outlast the plugin's longest heat waits", "[plr][wizard]") {
    // moonraker-plugin/helix_plr.py: BED_WAIT_TIMEOUT_S, CHAMBER_WAIT_TIMEOUT_S,
    // NOZZLE_WAIT_TIMEOUT_S. Both phases can run all three waits back to back.
    constexpr uint32_t kHeatWaitsMs = (20U + 45U + 10U) * 60U * 1000U;
    constexpr uint32_t kMotionMarginMs = 10U * 60U * 1000U;
    CHECK(helix::plr_rpc::kPrepareTimeoutMs >= kHeatWaitsMs + kMotionMarginMs);
    CHECK(helix::plr_rpc::kCommitTimeoutMs >= kHeatWaitsMs + kMotionMarginMs);
}
