// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "pa_result_store.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::pa;
using nlohmann::json;

namespace {

struct StoreFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPI api;

    StoreFixture() : api(client, get_printer_state()) {
        get_printer_state().init_subjects(false);
        client.connect("ws://mock/websocket", []() {}, []() {});
        client.set_mock_spoolman_enabled(true);
    }
    ~StoreFixture() override {
        client.disconnect();
    }

    /// Give spool `id` filament `filament_id`; mock spools carry none.
    void set_filament(int id, int filament_id) {
        for (auto& spool : client.spoolman_mock().get_mock_spools()) {
            if (spool.id == id) {
                spool.filament_id = filament_id;
            }
        }
    }

    int other_spool() {
        const int active = client.spoolman_mock().get_mock_active_spool_id();
        for (const auto& spool : client.spoolman_mock().get_mock_spools()) {
            if (spool.id != active) {
                return spool.id;
            }
        }
        return 0;
    }

    struct Found {
        bool called = false;
        SpoolLookup lookup = SpoolLookup::Unavailable;
        std::optional<SpoolInfo> spool;
    };

    Found find(int tool_spool) {
        auto out = std::make_shared<Found>();
        find_result_spool(api, tool_spool, [out](SpoolLookup l, const std::optional<SpoolInfo>& s) {
            out->called = true;
            out->lookup = l;
            out->spool = s;
        });
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
        return *out;
    }
};

} // namespace

TEST_CASE("a saved K records whether it was tuned or measured", "[pa_tuning]") {
    SavedK tuned;
    tuned.k = 0.04f;
    tuned.filament_id = 7;
    CHECK(merge_moonraker_record(nullptr, tuned)["last"]["source"] == "tuned");

    SavedK measured = tuned;
    measured.source = measured_source("helix_gauge");
    const auto rec = merge_moonraker_record(nullptr, measured);
    CHECK(rec["last"]["source"] == "measured:helix_gauge");
    CHECK(rec["filaments"]["7"]["source"] == "measured:helix_gauge");
}

TEST_CASE("only a spool with a filament can hold a K", "[pa_tuning]") {
    CHECK(classify_spool(std::nullopt) == SpoolLookup::NoSpool);
    SpoolInfo spool;
    spool.id = 3;
    CHECK(classify_spool(spool) == SpoolLookup::NoSpool);
    spool.filament_id = 9;
    CHECK(classify_spool(spool) == SpoolLookup::Found);
}

TEST_CASE("with_spool names the spool and leaves the rest", "[pa_tuning]") {
    SavedK saved;
    saved.k = 0.05f;
    saved.extruder = "extruder";
    CHECK(with_spool(saved, std::nullopt).spool_id == 0);
    SpoolInfo spool;
    spool.id = 3;
    spool.filament_id = 9;
    const auto named = with_spool(saved, spool);
    CHECK(named.spool_id == 3);
    CHECK(named.filament_id == 9);
    CHECK(named.extruder == "extruder");
    CHECK(named.k == Catch::Approx(0.05f));
}

TEST_CASE_METHOD(StoreFixture, "a tool's own spool wins over Spoolman's active one",
                 "[pa_tuning][spoolman]") {
    const int tool_spool = other_spool();
    REQUIRE(tool_spool > 0);
    set_filament(tool_spool, 11);
    const auto found = find(tool_spool);
    REQUIRE(found.called);
    CHECK(found.lookup == SpoolLookup::Found);
    REQUIRE(found.spool);
    CHECK(found.spool->id == tool_spool);
}

TEST_CASE_METHOD(StoreFixture, "without a tool spool the active spool is the result's",
                 "[pa_tuning][spoolman]") {
    const int active = client.spoolman_mock().get_mock_active_spool_id();
    REQUIRE(active > 0);
    set_filament(active, 12);
    const auto found = find(0);
    REQUIRE(found.called);
    CHECK(found.lookup == SpoolLookup::Found);
    REQUIRE(found.spool);
    CHECK(found.spool->id == active);
}

TEST_CASE_METHOD(StoreFixture, "Spoolman switched off reads as no spool", "[pa_tuning][spoolman]") {
    client.set_mock_spoolman_enabled(false);
    const auto found = find(0);
    REQUIRE(found.called);
    CHECK(found.lookup == SpoolLookup::NoSpool);
    CHECK_FALSE(found.spool);
}

TEST_CASE_METHOD(StoreFixture, "publishing keeps the record's other filaments", "[pa_tuning]") {
    SavedK earlier;
    earlier.k = 0.03f;
    earlier.filament_id = 3;
    client.mock_db_set(kMoonrakerNamespace, kMoonrakerKey,
                       merge_moonraker_record(nullptr, earlier));

    SavedK now;
    now.k = 0.0812f;
    now.filament_id = 7;
    now.source = measured_source("helix_gauge");
    publish_result(api, now);
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::UpdateQueue::instance().drain();

    const json rec = client.mock_db_get(kMoonrakerNamespace, kMoonrakerKey);
    REQUIRE(rec.is_object());
    CHECK(rec["filaments"]["3"]["k"].get<double>() == Catch::Approx(0.03));
    CHECK(rec["filaments"]["7"]["k"].get<double>() == Catch::Approx(0.0812));
    CHECK(rec["last"]["source"] == "measured:helix_gauge");
}
