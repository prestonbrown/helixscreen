// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_fixtures.h"
#include "core/update_queue.h"
#include "detection_source.h"
#include "printer_state.h"
#include "u1_stock_detection_source.h"

#include "../catch_amalgamated.hpp"

using helix::detection::DetectionEvent;
using helix::detection::DetectionKind;
using json = nlohmann::json;

TEST_CASE("DetectionEvent defaults are inert", "[detection][source]") {
    DetectionEvent e;
    REQUIRE(e.kind == DetectionKind::Unknown);
    REQUIRE(e.attributable == false);
    REQUIRE(e.already_paused == false);
    REQUIRE(e.source_id.empty());
    REQUIRE_FALSE(e.confidence.has_value());
}

TEST_CASE("DetectionKind maps the stock U1 code space", "[detection][source]") {
    REQUIRE(helix::detection::kind_from_u1_code(2) == DetectionKind::Spaghetti);
    REQUIRE(helix::detection::kind_from_u1_code(1) == DetectionKind::DirtyBed);
    REQUIRE(helix::detection::kind_from_u1_code(3) == DetectionKind::Residue);
    REQUIRE(helix::detection::kind_from_u1_code(4) == DetectionKind::DirtyNozzle);
    REQUIRE(helix::detection::kind_from_u1_code(0) == DetectionKind::Unknown);
    REQUIRE(helix::detection::kind_from_u1_code(-1) == DetectionKind::Unknown);
}

TEST_CASE_METHOD(XMLTestFixture, "U1StockSource emits Spaghetti on paused+code2",
                 "[detection][u1]") {
    helix::detection::U1StockSource src(&state());
    src.set_capable(true);
    helix::detection::DetectionEvent got;
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent& e) {
        got = e;
        ++fired;
    });
    src.start();

    // The print-state-enum observer is deferred via the UpdateQueue (observe<int>),
    // so drain after each frame to run on_print_state() once the frame is fully parsed.
    auto drain = [] { helix::ui::UpdateQueue::instance().drain_until_empty(); };

    // printing, empty exception -> no event
    state().update_from_status(
        json{{"print_stats", {{"state", "printing"}, {"exception", json::object()}}}});
    drain();
    REQUIRE(fired == 0);

    // noodle latched + paused in one snapshot -> one event
    state().update_from_status(json{{"print_stats",
                                     {{"state", "paused"},
                                      {"exception",
                                       {{"id", 532},
                                        {"index", 0},
                                        {"code", 2},
                                        {"message", "detected noodle"},
                                        {"level", 2}}}}}});
    drain();
    REQUIRE(fired == 1);
    REQUIRE(got.kind == helix::detection::DetectionKind::Spaghetti);
    REQUIRE(got.attributable);
    REQUIRE(got.already_paused);
    REQUIRE(got.source_id == "u1_stock");
    REQUIRE(got.message == "detected noodle");
}

TEST_CASE_METHOD(XMLTestFixture, "U1StockSource ignores manual pause (no defect code)",
                 "[detection][u1]") {
    helix::detection::U1StockSource src(&state());
    src.set_capable(true);
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent&) { ++fired; });
    src.start();

    state().update_from_status(
        json{{"print_stats", {{"state", "printing"}, {"exception", json::object()}}}});
    state().update_from_status(
        json{{"print_stats", {{"state", "paused"}, {"exception", json::object()}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(fired == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "U1StockSource fires once per pause edge", "[detection][u1]") {
    helix::detection::U1StockSource src(&state());
    src.set_capable(true);
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent&) { ++fired; });
    src.start();

    state().update_from_status(json{{"print_stats", {{"state", "printing"}}}});
    state().update_from_status(json{
        {"print_stats",
         {{"state", "paused"}, {"exception", {{"code", 2}, {"message", "detected noodle"}}}}}});
    // redundant paused frame -> no second event
    state().update_from_status(json{{"print_stats", {{"state", "paused"}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(fired == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "U1StockSource fires when the noodle code lands after the pause",
                 "[detection][u1]") {
    helix::detection::U1StockSource src(&state());
    src.set_capable(true);
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent&) { ++fired; });
    src.start();
    auto drain = [] { helix::ui::UpdateQueue::instance().drain_until_empty(); };

    state().update_from_status(
        json{{"print_stats", {{"state", "printing"}, {"exception", json::object()}}}});
    drain();
    state().update_from_status(json{{"print_stats", {{"state", "paused"}}}});
    drain();
    state().update_from_status(json{{"print_stats",
                                     {{"exception",
                                       {{"id", 532},
                                        {"index", 0},
                                        {"code", 2},
                                        {"message", "detected noodle"},
                                        {"level", 2}}}}}});
    drain();
    CHECK(fired == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "U1StockSource reports a dirty bed and explains the pause",
                 "[detection][u1][bed_check]") {
    helix::detection::U1StockSource src(&state());
    src.set_capable(true);
    helix::detection::DetectionEvent got;
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent& e) {
        got = e;
        ++fired;
    });
    src.start();
    auto drain = [] { helix::ui::UpdateQueue::instance().drain_until_empty(); };

    state().update_from_status(
        json{{"print_stats", {{"state", "printing"}, {"exception", json::object()}}}});
    drain();
    CHECK_FALSE(src.explains_current_pause());

    // The U1 bed check's own report: id 532, code 1, then the pause.
    state().update_from_status(json{{"print_stats",
                                     {{"state", "paused"},
                                      {"exception",
                                       {{"id", 532},
                                        {"index", 0},
                                        {"code", 1},
                                        {"message", "detected dirty bed"},
                                        {"level", 2}}}}}});
    drain();
    REQUIRE(fired == 1);
    CHECK(got.kind == DetectionKind::DirtyBed);
    CHECK(got.already_paused);
    CHECK(got.message == "detected dirty bed");
    CHECK(src.explains_current_pause());

    // A source the probe never confirmed explains nothing.
    src.set_capable(false);
    CHECK_FALSE(src.explains_current_pause());
}

TEST_CASE_METHOD(XMLTestFixture, "U1StockSource leaves another module's code 1 alone",
                 "[detection][u1][bed_check]") {
    helix::detection::U1StockSource src(&state());
    src.set_capable(true);
    int fired = 0;
    src.set_callback([&](const helix::detection::DetectionEvent&) { ++fired; });
    src.start();

    state().update_from_status(
        json{{"print_stats", {{"state", "printing"}, {"exception", json::object()}}}});
    // Exception 523 is the feeder's runout; its code space is not the defect
    // module's, so a code 1 under it is no dirty bed.
    state().update_from_status(
        json{{"print_stats",
              {{"state", "paused"},
               {"exception", {{"id", 523}, {"index", 0}, {"code", 1}, {"message", "runout"}}}}}});
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(fired == 0);
    CHECK_FALSE(src.explains_current_pause());
}

TEST_CASE("U1 defect kinds belong to exception 532", "[detection][source][bed_check]") {
    using helix::detection::kind_from_u1_exception;
    CHECK(kind_from_u1_exception(532, 1) == DetectionKind::DirtyBed);
    CHECK(kind_from_u1_exception(532, 2) == DetectionKind::Spaghetti);
    CHECK(kind_from_u1_exception(-1, 1) == DetectionKind::DirtyBed);
    CHECK(kind_from_u1_exception(523, 1) == DetectionKind::Unknown);
    CHECK(kind_from_u1_exception(532, 0) == DetectionKind::Unknown);
}
