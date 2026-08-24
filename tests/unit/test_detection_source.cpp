// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_fixtures.h"
#include "../test_helpers/update_queue_test_access.h"
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

    // The print-state-enum observer is deferred via the UpdateQueue (observe_int_sync),
    // so drain after each frame to run on_print_state() once the frame is fully parsed.
    auto drain = [] {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    };

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
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    REQUIRE(fired == 1);
}
