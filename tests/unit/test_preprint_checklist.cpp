// SPDX-License-Identifier: GPL-3.0-or-later

#include "preprint_checklist.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using P = PrintStartPhase;

namespace {
int K(P p) {
    return static_cast<int>(p);
}
} // namespace

TEST_CASE("done rows keep order and actual seconds", "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.done_in_order = {P::HOMING, P::HEATING_BED};
    in.actual_seconds = {{K(P::HOMING), 12}, {K(P::HEATING_BED), 95}};
    in.current = P::QGL;
    auto rows = build_preprint_checklist(in);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].phase == P::HOMING);
    CHECK(rows[0].status == PreprintStepStatus::Done);
    CHECK(rows[0].seconds == 12);
    CHECK(rows[1].seconds == 95);
    CHECK(rows[2].status == PreprintStepStatus::Current);
}

TEST_CASE("current row shows predicted minus elapsed, floored at 0", "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.current = P::QGL;
    in.predicted_seconds = {{K(P::QGL), 60}};
    in.current_elapsed_seconds = 45;
    CHECK(build_preprint_checklist(in)[0].seconds == 15);
    in.current_elapsed_seconds = 90;
    CHECK(build_preprint_checklist(in)[0].seconds == 0);
}

TEST_CASE("no expected order means no upcoming rows", "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.done_in_order = {P::HOMING};
    in.current = P::HEATING_BED;
    in.predicted_seconds = {{K(P::BED_MESH), 30}};
    auto rows = build_preprint_checklist(in);
    CHECK(rows.size() == 2);
}

TEST_CASE("upcoming rows skip done and current", "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.done_in_order = {P::HOMING};
    in.current = P::HEATING_BED;
    in.expected_order = {P::HOMING, P::HEATING_BED, P::QGL, P::BED_MESH};
    in.predicted_seconds = {{K(P::QGL), 70}};
    auto rows = build_preprint_checklist(in);
    REQUIRE(rows.size() == 4);
    CHECK(rows[2].phase == P::QGL);
    CHECK(rows[2].status == PreprintStepStatus::Upcoming);
    CHECK(rows[2].seconds == 70);
    CHECK(rows[3].phase == P::BED_MESH);
}

TEST_CASE("unpredicted upcoming row has seconds -1", "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.current = P::HOMING;
    in.expected_order = {P::HOMING, P::QGL};
    auto rows = build_preprint_checklist(in);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].seconds == -1);
}

TEST_CASE("over eight rows drops oldest done, never current", "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.done_in_order = {P::HOMING,         P::HEATING_BED, P::SOAKING,
                        P::HEATING_NOZZLE, P::QGL,         P::Z_TILT};
    in.current = P::BED_MESH;
    in.expected_order = {P::CLEANING, P::PURGING};
    // 6 done + current + 2 upcoming = 9 rows
    auto rows = build_preprint_checklist(in);
    REQUIRE(rows.size() == kPreprintChecklistMaxRows);
    CHECK(rows[0].phase == P::HEATING_BED);
    CHECK(preprint_step_position(rows).index == 6);

    PreprintChecklistInput cur_last;
    cur_last.done_in_order = in.done_in_order;
    cur_last.current = P::BED_MESH;
    cur_last.expected_order = {P::CLEANING, P::PURGING};
    cur_last.done_in_order.push_back(P::HOMING); // duplicate ignored
    CHECK(build_preprint_checklist(cur_last).size() == kPreprintChecklistMaxRows);
}

TEST_CASE("IDLE INITIALIZING COMPLETE never appear", "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.done_in_order = {P::IDLE, P::INITIALIZING, P::HOMING};
    in.current = P::COMPLETE;
    in.expected_order = {P::INITIALIZING, P::COMPLETE, P::IDLE};
    auto rows = build_preprint_checklist(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].phase == P::HOMING);
}

TEST_CASE("step position counts rows", "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.done_in_order = {P::HOMING};
    in.current = P::HEATING_BED;
    in.expected_order = {P::QGL, P::BED_MESH};
    auto pos = preprint_step_position(build_preprint_checklist(in));
    CHECK(pos.index == 2);
    CHECK(pos.count == 4);
    auto none = preprint_step_position({});
    CHECK(none.index == 0);
    CHECK(none.count == 0);
}

// A G28 on a printer that is already homed leaves homed_axes at "xyz", so the
// collector never sees the homing step the start macro runs first. A step the
// collector never saw is not claimed as done.
TEST_CASE("an expected step the sequence passed unseen drops from the list",
          "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.done_in_order = {P::HEATING_BED};
    in.actual_seconds = {{K(P::HEATING_BED), 23}};
    in.current = P::HEATING_NOZZLE;
    in.expected_order = {P::HOMING, P::HEATING_BED, P::HEATING_NOZZLE, P::PURGING};
    auto rows = build_preprint_checklist(in);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].phase == P::HEATING_BED);
    CHECK(rows[0].status == PreprintStepStatus::Done);
    CHECK(rows[0].seconds == 23);
    CHECK(rows[1].phase == P::HEATING_NOZZLE);
    CHECK(rows[1].status == PreprintStepStatus::Current);
    CHECK(rows[2].phase == P::PURGING);
    CHECK(rows[2].status == PreprintStepStatus::Upcoming);
}

TEST_CASE("an unseen expected step after every done one is still upcoming",
          "[preprint_checklist]") {
    PreprintChecklistInput in;
    in.done_in_order = {P::HOMING};
    in.current = P::HEATING_BED;
    in.expected_order = {P::HOMING, P::HEATING_BED, P::QGL, P::HEATING_NOZZLE};
    auto rows = build_preprint_checklist(in);
    REQUIRE(rows.size() == 4);
    CHECK(rows[2].phase == P::QGL);
    CHECK(rows[2].status == PreprintStepStatus::Upcoming);
}
