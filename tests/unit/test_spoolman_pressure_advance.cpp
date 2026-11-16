// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_spoolman_api.h"
#include "printer_state.h"
#include "spoolman_active_spool_sync.h"
#include "spoolman_types.h"
#include "tune_controller.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using nlohmann::json;

TEST_CASE("parse_extra_number reads Spoolman's JSON-encoded extra values", "[spoolman][pa_tune]") {
    const json extra = {{"pressure_advance", "0.042"},
                        {"as_text", "\"0.0315\""},
                        {"raw", 0.5},
                        {"words", "\"soft\""},
                        {"broken", "{"}};
    using helix::spoolman_detail::parse_extra_number;
    CHECK(*parse_extra_number(extra, "pressure_advance") == Catch::Approx(0.042));
    CHECK(*parse_extra_number(extra, "as_text") == Catch::Approx(0.0315));
    CHECK(*parse_extra_number(extra, "raw") == Catch::Approx(0.5));
    CHECK_FALSE(parse_extra_number(extra, "words"));
    CHECK_FALSE(parse_extra_number(extra, "broken"));
    CHECK_FALSE(parse_extra_number(extra, "missing"));
    CHECK_FALSE(parse_extra_number(json::array(), "pressure_advance"));
}

TEST_CASE("encode_extra_number quotes only for a text field", "[spoolman][pa_tune]") {
    using helix::spoolman_detail::encode_extra_number;
    CHECK(encode_extra_number("float", 0.042) == "0.0420");
    CHECK(encode_extra_number("integer", 1.0) == "1.0000");
    CHECK(encode_extra_number("text", 0.042) == "\"0.0420\"");
}

TEST_CASE("a spool's filament extra carries its saved pressure advance", "[spoolman][pa_tune]") {
    json spool = {{"id", 3},
                  {"filament", {{"id", 9}, {"extra", {{"pressure_advance", "0.0375"}}}}}};
    const SpoolInfo info = helix::spoolman_detail::parse_spool_info(spool);
    REQUIRE(info.pressure_advance);
    CHECK(*info.pressure_advance == Catch::Approx(0.0375f));

    spool["filament"]["extra"] = json::object();
    CHECK_FALSE(helix::spoolman_detail::parse_spool_info(spool).pressure_advance);
}

namespace {

/// Put the mock's first spool on a filament with a real id and return it.
int first_filament_id(MoonrakerClientMock& client) {
    auto& spools = client.spoolman_mock().get_mock_spools();
    REQUIRE_FALSE(spools.empty());
    spools.front().filament_id = 7;
    return 7;
}

} // namespace

TEST_CASE("saving a K defines the extra field once, then writes it", "[spoolman][pa_tune]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& server = client.spoolman_mock();
    const int filament_id = first_filament_id(client);

    bool ok = false;
    api.spoolman().set_spoolman_filament_extra_number(
        filament_id, kSpoolmanPaExtraKey, "Pressure advance", 0.042, [&]() { ok = true; },
        [](const MoonrakerError&) {});
    REQUIRE(ok);
    REQUIRE(server.defined_filament_fields.size() == 1);
    CHECK(server.defined_filament_fields[0].first == "pressure_advance");
    CHECK(server.defined_filament_fields[0].second["field_type"] == "float");
    REQUIRE_FALSE(server.filament_updates.empty());
    CHECK(server.filament_updates.back().data["extra"]["pressure_advance"] == "0.0420");

    // The value round-trips through the spool record.
    std::optional<SpoolInfo> fetched;
    const int spool_id = server.get_mock_spools().front().id;
    api.spoolman().get_spoolman_spool(
        spool_id, [&](const std::optional<SpoolInfo>& s) { fetched = s; },
        [](const MoonrakerError&) {});
    REQUIRE(fetched);
    REQUIRE(fetched->pressure_advance);
    CHECK(*fetched->pressure_advance == Catch::Approx(0.042f));

    // A second save finds the field and does not redefine it.
    ok = false;
    api.spoolman().set_spoolman_filament_extra_number(
        filament_id, kSpoolmanPaExtraKey, "Pressure advance", 0.05, [&]() { ok = true; },
        [](const MoonrakerError&) {});
    REQUIRE(ok);
    CHECK(server.defined_filament_fields.size() == 1);
    CHECK(server.filament_updates.back().data["extra"]["pressure_advance"] == "0.0500");
}

TEST_CASE("a field the user defined as text is written as text", "[spoolman][pa_tune]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& server = client.spoolman_mock();
    const int filament_id = first_filament_id(client);

    json result;
    MoonrakerError err;
    REQUIRE(server.proxy({{"request_method", "POST"},
                          {"path", "/v1/field/filament/pressure_advance"},
                          {"body", {{"name", "PA"}, {"field_type", "text"}}}},
                         result, err));
    server.defined_filament_fields.clear();

    bool ok = false;
    api.spoolman().set_spoolman_filament_extra_number(
        filament_id, kSpoolmanPaExtraKey, "Pressure advance", 0.042, [&]() { ok = true; },
        [](const MoonrakerError&) {});
    REQUIRE(ok);
    CHECK(server.defined_filament_fields.empty());
    CHECK(server.filament_updates.back().data["extra"]["pressure_advance"] == "\"0.0420\"");
}

TEST_CASE("the mock refuses an extra key with no field definition", "[spoolman][pa_tune]") {
    PrinterState state;
    MoonrakerClientMock client;
    auto& server = client.spoolman_mock();
    const int filament_id = first_filament_id(client);
    json result;
    MoonrakerError err;
    CHECK_FALSE(server.proxy({{"request_method", "PATCH"},
                              {"path", "/v1/filament/" + std::to_string(filament_id)},
                              {"body", {{"extra", {{"undefined_key", "1"}}}}}},
                             result, err));
}

namespace {

/// The global API and PrinterState apply_saved_pressure_advance() reaches.
struct ApplyFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPI api;
    bool prev_beta = false;

    ApplyFixture() : api(client, get_printer_state()) {
        // Off: pressure advance needs no beta features (prestonbrown/helixscreen#1736).
        prev_beta = Config::get_instance()->get<bool>("/beta_features", false);
        Config::get_instance()->set<bool>("/beta_features", false);
        get_printer_state().init_subjects(false);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        client.connect("ws://mock/websocket", []() {}, []() {});
        set_moonraker_api(&api);
    }
    ~ApplyFixture() override {
        Config::get_instance()->set<bool>("/beta_features", prev_beta);
        set_moonraker_api(nullptr);
        client.stop_temperature_simulation();
        client.disconnect();
    }

    SpoolInfo spool_with_k(std::optional<float> k) {
        SpoolInfo s;
        s.id = 4;
        s.pressure_advance = k;
        return s;
    }
};

} // namespace

TEST_CASE_METHOD(ApplyFixture, "activating a spool applies its saved K while idle",
                 "[spoolman][pa_tune]") {
    REQUIRE_FALSE(Config::get_instance()->is_beta_features_enabled());
    helix::spoolman_sync::apply_saved_pressure_advance(spool_with_k(0.0375f));
    CHECK(client.last_send_script() == "SET_PRESSURE_ADVANCE EXTRUDER=extruder ADVANCE=0.0375");
}

TEST_CASE_METHOD(ApplyFixture, "a spool with no saved K or a running print sends nothing",
                 "[spoolman][pa_tune]") {
    helix::tune::set_speed_percent(&api, 110);
    helix::spoolman_sync::apply_saved_pressure_advance(spool_with_k(std::nullopt));
    CHECK(client.last_send_script() == "M220 S110");

    get_printer_state().update_from_status({{"print_stats", {{"state", "printing"}}}});
    helix::ui::UpdateQueue::instance().drain();
    helix::spoolman_sync::apply_saved_pressure_advance(spool_with_k(0.04f));
    CHECK(client.last_send_script() == "M220 S110");
    get_printer_state().update_from_status({{"print_stats", {{"state", "standby"}}}});
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE("saving a K keeps the filament's other extra fields", "[spoolman][pa_tune]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& server = client.spoolman_mock();
    const int filament_id = first_filament_id(client);
    json result;
    MoonrakerError err;
    REQUIRE(server.proxy({{"request_method", "POST"},
                          {"path", "/v1/field/filament/drying_note"},
                          {"body", {{"name", "Drying"}, {"field_type", "text"}}}},
                         result, err));
    REQUIRE(server.proxy({{"request_method", "PATCH"},
                          {"path", "/v1/filament/" + std::to_string(filament_id)},
                          {"body", {{"extra", {{"drying_note", "\"4h at 55\""}}}}}},
                         result, err));

    bool ok = false;
    api.spoolman().set_spoolman_filament_extra_number(
        filament_id, kSpoolmanPaExtraKey, "Pressure advance", 0.042, [&]() { ok = true; },
        [](const MoonrakerError&) {});
    REQUIRE(ok);
    const json& extra = server.filament_updates.back().data["extra"];
    CHECK(extra["drying_note"] == "\"4h at 55\"");
    CHECK(extra["pressure_advance"] == "0.0420");
}

TEST_CASE("a filament read back as no object is never written over", "[spoolman][pa_tune]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    auto& server = client.spoolman_mock();
    const int filament_id = first_filament_id(client);
    json result;
    MoonrakerError err;
    REQUIRE(server.proxy({{"request_method", "POST"},
                          {"path", "/v1/field/filament/pressure_advance"},
                          {"body", {{"name", "PA"}, {"field_type", "float"}}}},
                         result, err));
    server.next_filament_get_result = json::array();
    const size_t patches = server.filament_updates.size();
    bool failed = false;
    api.spoolman().set_spoolman_filament_extra_number(
        filament_id, kSpoolmanPaExtraKey, "Pressure advance", 0.042, []() {},
        [&](const MoonrakerError&) { failed = true; });
    CHECK(failed);
    CHECK(server.filament_updates.size() == patches);
}
