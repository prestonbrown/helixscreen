// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "ams_state.h"
#include "ams_types.h"
#include "consumption_sink.h"
#include "lane_source_store.h"

#include "../catch_amalgamated.hpp"

using helix::ExternalSpoolSink;

namespace {

// Uses LVGLTestFixture because AmsState::init_subjects + lv_tick_get require
// LVGL initialization. Mirrors the existing tracker tests.
struct ExternalSpoolSinkFixture : LVGLTestFixture {
    /// The database the bypass record is written to.
    MockPrinter db;

    ExternalSpoolSinkFixture() {
        auto& ams = helix::AmsState::instance();
        ams.init_subjects(false);
        ams.set_moonraker_api(&db.api);

        helix::SlotInfo info;
        info.material = "PLA";
        info.remaining_weight_g = 1000.0f;
        info.total_weight_g = 1000.0f;
        helix::AmsStateTestAccess::seed_external_spool(info);
    }

    ~ExternalSpoolSinkFixture() override {
        helix::AmsState::instance().clear_external_spool_info();
        helix::ui::UpdateQueue::instance().drain();
        helix::AmsState::instance().set_moonraker_api(nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: snapshot captures baseline when trackable",
                 "[consumption_sink][external]") {
    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: apply_delta decrements remaining_weight_g",
                 "[consumption_sink][external]") {
    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    // 1000 mm of 1.75mm PLA @ 1.24 g/cm^3 ≈ 2.98 g.
    sink.apply_delta(1000.0f);
    auto info = helix::AmsState::instance().get_external_spool_info();
    REQUIRE(info.has_value());
    REQUIRE(info->remaining_weight_g < 1000.0f);
    REQUIRE(info->remaining_weight_g > 996.0f);
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture, "ExternalSpoolSink: unknown weight not trackable",
                 "[consumption_sink][external]") {
    helix::SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = -1.0f;
    helix::AmsStateTestAccess::seed_external_spool(info);

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture, "ExternalSpoolSink: unknown material not trackable",
                 "[consumption_sink][external]") {
    helix::SlotInfo info;
    info.material = "UnknownNovelMaterial9000";
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    helix::AmsStateTestAccess::seed_external_spool(info);

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: no external spool info not trackable",
                 "[consumption_sink][external]") {
    helix::AmsState::instance().clear_external_spool_info();

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: apply_delta clamps remaining weight at zero",
                 "[consumption_sink][external]") {
    helix::SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 5.0f; // only 5 g available
    info.total_weight_g = 1000.0f;
    helix::AmsStateTestAccess::seed_external_spool(info);

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());

    // Consume ~29.8 g (would drive negative without clamp).
    sink.apply_delta(10000.0f);
    auto after = helix::AmsState::instance().get_external_spool_info();
    REQUIRE(after.has_value());
    REQUIRE(after->remaining_weight_g == 0.0f);
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: external write rebaselines instead of applying delta",
                 "[consumption_sink][external]") {
    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    sink.apply_delta(1000.0f); // ~3 g consumed → ~997 g remaining

    // Simulate an external writer replacing remaining_weight_g.
    auto current = helix::AmsState::instance().get_external_spool_info();
    REQUIRE(current.has_value());
    helix::SlotInfo edited = *current;
    edited.remaining_weight_g = 500.0f;
    helix::AmsStateTestAccess::seed_external_spool(edited);

    // Next tick detects the external write and rebaselines (no decrement).
    sink.apply_delta(1100.0f);
    auto snap_after_rebase = helix::AmsState::instance().get_external_spool_info();
    REQUIRE(snap_after_rebase->remaining_weight_g == 500.0f);

    // Subsequent extrusion decrements from 500g, not from the original baseline.
    sink.apply_delta(1200.0f); // only 100 mm past rebase
    auto after = helix::AmsState::instance().get_external_spool_info();
    REQUIRE(after->remaining_weight_g < 500.0f);
    REQUIRE(after->remaining_weight_g > 499.0f);
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture,
                 "ExternalSpoolSink: flush writes the meter's count, not the resolved view",
                 "[consumption_sink][external][1632]") {
    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    sink.apply_delta(1000.0f); // ~3 g consumed, filed as Metered
    const float counted = helix::AmsState::instance().get_external_spool_info()->remaining_weight_g;
    REQUIRE(counted < 1000.0f);
    // The record still holds the weight it was read with.
    REQUIRE(helix::AmsState::instance().raw_external_spool_info()->remaining_weight_g == 1000.0f);

    // A resolved weight that disagrees with the meter (another reading filed
    // on the lane). Flushing must write what this sink counted.
    helix::ams::Observation metered(helix::ams::ObservationSource::Metered);
    metered.remaining_weight_g = 500.0f;
    helix::ams::ingest(helix::ams::kBypassLaneId, metered);

    sink.flush();

    const auto written = db.client.mock_db_get("lane_data", "lane1");
    REQUIRE(written.is_object());
    CHECK(written["remaining_weight_g"].get<float>() == Catch::Approx(counted));
    CHECK(helix::AmsState::instance().raw_external_spool_info()->remaining_weight_g ==
          Catch::Approx(counted));
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture, "ExternalSpoolSink: Spoolman-linked spool not metered",
                 "[consumption_sink][external][1632]") {
    helix::SlotInfo info;
    info.material = "PLA";
    info.spoolman_id = 4;
    info.remaining_weight_g = 1000.0f;
    info.total_weight_g = 1000.0f;
    helix::AmsStateTestAccess::seed_external_spool(info);

    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    // The server tracks a linked spool's own consumption; a local meter would
    // fight it.
    REQUIRE_FALSE(sink.is_trackable());
}

TEST_CASE_METHOD(ExternalSpoolSinkFixture, "ExternalSpoolSink: linking mid-print pauses the meter",
                 "[consumption_sink][external][1632]") {
    ExternalSpoolSink sink;
    sink.snapshot(0.0f);
    REQUIRE(sink.is_trackable());

    helix::SlotInfo linked;
    linked.material = "PLA";
    linked.spoolman_id = 4;
    linked.remaining_weight_g = 1000.0f;
    linked.total_weight_g = 1000.0f;
    helix::AmsStateTestAccess::seed_external_spool(linked);

    sink.apply_delta(1000.0f); // ~3 g consumed

    // Paused, not counting: the bypass lane carries no Metered record, and the
    // shown weight is the binding's own, not a local decrement of it.
    CHECK_FALSE(helix::ams::lane_sources(helix::ams::kBypassLaneId).metered.has_value());
    auto shown = helix::AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->remaining_weight_g == 1000.0f);
}
