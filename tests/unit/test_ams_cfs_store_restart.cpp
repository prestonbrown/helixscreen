// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fingerprint sink across a restart of the lane record store, on CFS. The
// QIDI Box file carries the same case for that backend.

#include "../lvgl_test_fixture.h"
#include "../test_helpers/mock_printer.h"
#include "ams_backend_cfs.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/cfs_box_fixtures.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/registered_backend.h"

#include "../catch_amalgamated.hpp"

using helix::CfsTestAccess;
using helix::printer::AmsBackendCfs;

using json = nlohmann::json;

// A start opens a new lane record store, which frees the one before it. The
// tracker's baseline sink runs inline on every Baseline and Unchanged
// observation and outlives that start, so it must follow the backend's pointer
// to its store rather than hold the store: the one the last read saw is freed.
//
// The record in the window is a meter's: update_slot_weight with persist, the
// call the consumption tracker makes, stages one whenever it lands. The store
// the start opened has not read, so the gate refuses that record's own save,
// and the fingerprint a poll confirms must not slip past the gate through the
// old store.
//
// Break: bind the sink to override_store_.get() instead of override_store_.
// The sink the first read bound then names the store open_lane_record_store()
// freed, and the poll in the window writes through it: a heap-use-after-free
// under ASAN.
TEST_CASE("CFS a restart never leaves the fingerprint sink on the replaced store",
          "[ams][cfs][store_restart]") {
    MockPrinter mock_printer;
    auto& api = mock_printer.api;

    helix::test::RegisteredWired<AmsBackendCfs> harness(&api);
    auto& backend = *harness;
    // One present, tagged bay: its material and colour are the fingerprint.
    const json box = helix::test::make_unit_box_explicit(
        {"101001", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"},
        {"unknown", "none", "none", "none"}, {"52", "-1", "-1", "-1"});
    const auto poll = [&backend, box]() { backend.fire_status(json{{"box", box}}); };

    // The first start reads its store, and that read binds the sink.
    poll();
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500");

    // A restart frees that store and opens another. Reading CFS's records is
    // two requests, the bypass declaration and then the lanes; both are held,
    // so the window stays open.
    mock_printer.client.defer_next("server.database.get_item");
    mock_printer.client.defer_next("server.database.get_item");
    CfsTestAccess::call_on_started(backend);
    REQUIRE_FALSE(backend.lane_records_ready());

    // A meter's flush lands in that window and stages the bay's record.
    backend.update_slot_weight(0, 300.0f, 1000.0f, /*persist=*/true);
    REQUIRE(CfsTestAccess::get_override(backend, 0).has_value());

    // The box states the same spool again.
    poll();
    CHECK_FALSE(
        mock_printer.client.mock_db_get("lane_data", "lane1").contains("helix_fingerprint"));

    // Both reads land. The same poll now persists, through the new store.
    mock_printer.client.fire_deferred("server.database.get_item");
    mock_printer.client.fire_deferred("server.database.get_item");
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(backend.lane_records_ready());
    backend.update_slot_weight(0, 300.0f, 1000.0f, /*persist=*/true);
    poll();
    const auto stored = mock_printer.client.mock_db_get("lane_data", "lane1");
    REQUIRE(stored.contains("helix_fingerprint"));
    CHECK(stored.at("helix_fingerprint") == "101001|0FF5500");
}
