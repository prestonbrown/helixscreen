// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_runout_other_head.cpp
 * @brief Continuing a runout-paused print on another loaded head (#2137).
 *
 * assess() is the generic eligibility every filament system shares; build_plan()
 * is the per-system row. The Snapmaker U1 row is the only one: its firmware takes
 * no new tool map while paused, so the plan labels the empty head with the chosen
 * head's filament record and asks the firmware's Auto-Replenish to re-route.
 */

#include "runout_other_head.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::runout_other_head;
using helix::AmsType;

namespace {

FilamentRecord record(std::string type, std::string color, std::string subtype = "Basic",
                      std::string vendor = "Snapmaker") {
    FilamentRecord r;
    r.vendor = std::move(vendor);
    r.type = std::move(type);
    r.subtype = std::move(subtype);
    r.colors = {std::move(color)};
    return r;
}

HeadFacts head(int index, std::optional<FilamentRecord> rec, bool fed = true, double nozzle = 0.4,
               std::string flow = "standard") {
    HeadFacts h;
    h.head = index;
    h.nozzle_diameter = nozzle;
    h.nozzle_flow = std::move(flow);
    h.loaded_by_feeder = fed;
    h.record = std::move(rec);
    return h;
}

/// Head 0 ran out of red PLA; heads 1-3 hold filament through their feeders.
RunoutFacts paused_runout_on_head0() {
    RunoutFacts f;
    f.runout_head = 0;
    f.active_head = 0;
    f.paused = true;
    f.auto_switch_enabled = true;
    f.original = record("PLA", "FF0000");
    f.heads = {head(0, record("PLA", "FF0000"), false), head(1, record("PLA", "FF0000", "Matte")),
               head(2, record("PETG", "FF0000")), head(3, record("PLA", "0000FF"))};
    return f;
}

const HeadVerdict& verdict(const std::vector<HeadVerdict>& all, int head_index) {
    for (const auto& v : all) {
        if (v.head == head_index) {
            return v;
        }
    }
    FAIL("no verdict for head " << head_index);
    return all.front();
}

} // namespace

TEST_CASE("runout other head: the head that ran out is never offered", "[runout_other_head]") {
    const auto all = assess(paused_runout_on_head0());
    REQUIRE(all.size() == 4);
    CHECK(verdict(all, 0).refusal == HeadRefusal::IsRunoutHead);
}

TEST_CASE("runout other head: a compatible material qualifies, a different polymer does not",
          "[runout_other_head]") {
    const auto all = assess(paused_runout_on_head0());
    CHECK(verdict(all, 1).eligible()); // PLA Matte for PLA Basic
    CHECK(verdict(all, 2).refusal == HeadRefusal::MaterialMismatch);
    CHECK(verdict(all, 3).eligible());
}

TEST_CASE("runout other head: a filled grade does not stand in for a plain one",
          "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.heads[1].record = record("PLA-CF", "FF0000");
    CHECK(verdict(assess(f), 1).refusal == HeadRefusal::MaterialMismatch);
}

TEST_CASE("runout other head: a different colour is eligible with a warning",
          "[runout_other_head]") {
    const auto all = assess(paused_runout_on_head0());
    CHECK_FALSE(verdict(all, 1).color_differs);
    CHECK(verdict(all, 3).color_differs);
}

TEST_CASE("runout other head: the nozzle must match in size and flow type", "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.heads[1].nozzle_diameter = 0.6;
    f.heads[3].nozzle_flow = "high_flow";
    const auto all = assess(f);
    CHECK(verdict(all, 1).refusal == HeadRefusal::NozzleSize);
    CHECK(verdict(all, 3).refusal == HeadRefusal::NozzleFlow);
}

TEST_CASE("runout other head: an unreported nozzle proves nothing", "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.heads[1].nozzle_diameter.reset();
    f.heads[0].nozzle_flow.clear(); // the runout head's own nozzle is the reference
    const auto all = assess(f);
    CHECK(verdict(all, 1).refusal == HeadRefusal::NozzleUnknown);
    CHECK(verdict(all, 3).refusal == HeadRefusal::NozzleUnknown);
}

TEST_CASE("runout other head: a head not fed by its feeder is refused", "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.heads[1].loaded_by_feeder = false;
    CHECK(verdict(assess(f), 1).refusal == HeadRefusal::NotFedByFeeder);
}

TEST_CASE("runout other head: a loaded head with no full filament record is refused",
          "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.heads[1].record.reset();
    f.heads[3].record->subtype.clear();
    CHECK(verdict(assess(f), 1).refusal == HeadRefusal::NoFilamentRecord);
    CHECK(verdict(assess(f), 3).refusal == HeadRefusal::NoFilamentRecord);
}

TEST_CASE("runout other head: nothing to compare against offers nothing", "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.original.reset();
    CHECK(assess(f).empty());
}

TEST_CASE("runout other head: every refusal has text, None has none", "[runout_other_head]") {
    CHECK(std::string(refusal_text(HeadRefusal::None)).empty());
    for (auto r :
         {HeadRefusal::IsRunoutHead, HeadRefusal::NozzleUnknown, HeadRefusal::NozzleSize,
          HeadRefusal::NozzleFlow, HeadRefusal::NotFedByFeeder, HeadRefusal::NoFilamentRecord,
          HeadRefusal::MaterialMismatch, HeadRefusal::RunoutHeadStillFeeds}) {
        CHECK_FALSE(std::string(refusal_text(r)).empty());
    }
}

TEST_CASE("runout other head: only the U1 has a row", "[runout_other_head]") {
    CHECK(supported(AmsType::SNAPMAKER));
    CHECK_FALSE(supported(AmsType::AFC));
    CHECK_FALSE(supported(AmsType::HAPPY_HARE));
    const auto plan = build_plan(AmsType::AFC, paused_runout_on_head0(), 1);
    CHECK(plan.refusal == PlanRefusal::Unsupported);
    CHECK(plan.gcode.empty());
}

TEST_CASE("runout other head: U1 labels the empty head, then runs Auto-Replenish",
          "[runout_other_head]") {
    const auto plan = build_plan(AmsType::SNAPMAKER, paused_runout_on_head0(), 3);
    REQUIRE(plan.ok());
    REQUIRE(plan.gcode == std::vector<std::string>{
                              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR=\"Snapmaker\" "
                              "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic COLOR_NUMS=1 "
                              "COLORS=0000FF MULTI_MODE=0 ALPHA=255 FORCE=1",
                              "INNER_AUTO_REPLENISH_FILAMENT EXTRUDER=0"});
    CHECK(plan.continues_on == 3);
    CHECK_FALSE(plan.switches_auto_switch_on);
}

TEST_CASE("runout other head: U1 copies a multi-colour record exactly", "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    FilamentRecord rainbow = record("PLA", "FF0000", "Silk");
    rainbow.colors = {"FF0000", "00ff00"};
    rainbow.alpha = 0x80;
    rainbow.multi_mode = 1;
    f.heads[3].record = rainbow;
    const auto plan = build_plan(AmsType::SNAPMAKER, f, 3);
    REQUIRE(plan.ok());
    CHECK(plan.gcode.front() ==
          "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR=\"Snapmaker\" FILAMENT_TYPE=PLA "
          "FILAMENT_SUBTYPE=Silk COLOR_NUMS=2 COLORS=FF0000,00ff00 MULTI_MODE=1 ALPHA=128 "
          "FORCE=1");
}

TEST_CASE("runout other head: U1 always quotes the vendor", "[runout_other_head]") {
    // An unquoted apostrophe opens a quoted string in the firmware's parameter
    // split and swallows every parameter after it.
    auto f = paused_runout_on_head0();
    f.heads[3].record->vendor = "O'Brien Rapid";
    const auto plan = build_plan(AmsType::SNAPMAKER, f, 3);
    REQUIRE(plan.ok());
    CHECK(plan.gcode.front().find("VENDOR=\"O'Brien Rapid\" FILAMENT_TYPE=PLA ") !=
          std::string::npos);
}

TEST_CASE("runout other head: U1 refuses a record it cannot write", "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.heads[3].record->colors = {"blue"};
    CHECK(build_plan(AmsType::SNAPMAKER, f, 3).refusal == PlanRefusal::UnsafeRecord);
    f = paused_runout_on_head0();
    f.heads[3].record->subtype = "Basic\nG28";
    CHECK(build_plan(AmsType::SNAPMAKER, f, 3).refusal == PlanRefusal::UnsafeRecord);
}

TEST_CASE("runout other head: U1 turns Auto-Replenish on for this switch only",
          "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.auto_switch_enabled = false;
    const auto plan = build_plan(AmsType::SNAPMAKER, f, 3);
    REQUIRE(plan.ok());
    CHECK(plan.switches_auto_switch_on);
    REQUIRE(plan.gcode.size() == 4);
    CHECK(plan.gcode[0] == "SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=1");
    CHECK(plan.gcode[2] == "INNER_AUTO_REPLENISH_FILAMENT EXTRUDER=0");
    CHECK(plan.gcode[3] == "SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=0");
    // A failed script stops before its last line; the caller sends this alone.
    CHECK(plan.restore_gcode == "SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=0");
    CHECK(build_plan(AmsType::SNAPMAKER, paused_runout_on_head0(), 3).restore_gcode.empty());
}

TEST_CASE("runout other head: U1 refuses while the switch state is unknown",
          "[runout_other_head]") {
    // Turning it on with nothing known to restore would leave it on.
    auto f = paused_runout_on_head0();
    f.auto_switch_enabled.reset();
    CHECK(build_plan(AmsType::SNAPMAKER, f, 3).refusal == PlanRefusal::AutoSwitchUnknown);
}

TEST_CASE("runout other head: a runout head its feeder can still supply offers nothing",
          "[runout_other_head]") {
    // Filament at the port after a jam or tangle: the firmware resumes on the
    // runout head itself and never looks at the others.
    auto f = paused_runout_on_head0();
    f.heads[0].feeder_can_supply = true;
    const auto all = assess(f);
    CHECK(verdict(all, 0).refusal == HeadRefusal::IsRunoutHead);
    CHECK(verdict(all, 1).refusal == HeadRefusal::RunoutHeadStillFeeds);
    CHECK(verdict(all, 3).refusal == HeadRefusal::RunoutHeadStillFeeds);
    CHECK(build_plan(AmsType::SNAPMAKER, f, 3).refusal == PlanRefusal::HeadNotEligible);
}

TEST_CASE("runout other head: the switch is seen by the pause ending or the routing changing",
          "[runout_other_head]") {
    auto before = paused_runout_on_head0();
    before.routing = {0, 1, 2, 3};
    auto now = before;
    CHECK_FALSE(switch_happened(before, now)); // declined: nothing moved
    now.routing = {3, 1, 2, 3};
    CHECK(switch_happened(before, now));
    now = before;
    now.paused = false;
    CHECK(switch_happened(before, now));
}

TEST_CASE("runout other head: a silent decline fails once the reply has settled",
          "[runout_other_head]") {
    auto before = paused_runout_on_head0();
    before.routing = {0, 1, 2, 3};
    auto switched = before;
    switched.paused = false;
    CHECK(judge_switch(before, before, SendReply::Ok, 100, 100) == SwitchVerdict::Pending);
    CHECK(judge_switch(before, before, SendReply::Ok, kSwitchSettleMs, 9000) ==
          SwitchVerdict::Failed);
    CHECK(judge_switch(before, before, SendReply::Refused, kSwitchSettleMs, 9000) ==
          SwitchVerdict::Failed);
    // A timed-out send may still be heating and purging: wait for the outcome.
    CHECK(judge_switch(before, before, SendReply::TransportLost, 60000, 150000) ==
          SwitchVerdict::Pending);
    CHECK(judge_switch(before, switched, SendReply::TransportLost, 60000, 170000) ==
          SwitchVerdict::Switched);
    CHECK(judge_switch(before, before, SendReply::None, 0, kSwitchGiveUpMs) ==
          SwitchVerdict::Failed);
}

TEST_CASE("runout other head: U1 names the lower head holding the identical filament",
          "[runout_other_head]") {
    // The firmware takes the first head, in order, whose record matches the
    // label; a lower head holding the same spool record wins over the choice.
    auto f = paused_runout_on_head0();
    f.heads[1].record = record("PLA", "0000FF");
    const auto plan = build_plan(AmsType::SNAPMAKER, f, 3);
    REQUIRE(plan.ok());
    CHECK(plan.continues_on == 1);
}

TEST_CASE("runout other head: U1 refuses outside a runout pause on the runout head",
          "[runout_other_head]") {
    auto f = paused_runout_on_head0();
    f.paused = false;
    CHECK(build_plan(AmsType::SNAPMAKER, f, 3).refusal == PlanRefusal::NotPaused);

    f = paused_runout_on_head0();
    f.active_head = 2;
    CHECK(build_plan(AmsType::SNAPMAKER, f, 3).refusal == PlanRefusal::RunoutHeadNotActive);

    f = paused_runout_on_head0();
    const auto refused = build_plan(AmsType::SNAPMAKER, f, 2); // PETG
    CHECK(refused.refusal == PlanRefusal::HeadNotEligible);
    CHECK(refused.gcode.empty());
    CHECK(refused.continues_on == -1);
    CHECK(build_plan(AmsType::SNAPMAKER, f, 0).refusal == PlanRefusal::HeadNotEligible);
    CHECK(build_plan(AmsType::SNAPMAKER, f, 7).refusal == PlanRefusal::HeadNotEligible);
}
