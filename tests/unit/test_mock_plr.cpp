// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_plr.cpp
 * @brief The mock plugin's server.helix.plr.* endpoints and their knobs
 *
 * Run with: ./build/bin/helix-tests "[plr][mock]"
 *
 * The --test resume wizard is only worth anything if the mock answers in the
 * shapes the real plugin's contract names, so these read the replies through
 * the app's own readers.
 */

#include "../test_helpers/scoped_env.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "plr_backend.h"
#include "plr_profile.h"
#include "plr_wizard.h"

#include <algorithm>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"

using nlohmann::json;

namespace {

struct Reply {
    std::optional<json> result;
    std::optional<MoonrakerError> error;
};

Reply call(MoonrakerClientMock& client, const std::string& method, const json& params = {}) {
    Reply r;
    client.send_jsonrpc(
        method, params.is_null() ? json::object() : params,
        [&r](const json& response) { r.result = response.value("result", json::object()); },
        [&r](const MoonrakerError& err) { r.error = err; });
    return r;
}

struct MockPlrFixture {
    helix::ScopedEnv plugin{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    helix::ScopedEnv plr{"HELIX_MOCK_PLR", "generic"};
    helix::ScopedEnv tier{"HELIX_MOCK_PLR_TIER"};
    helix::ScopedEnv warnings{"HELIX_MOCK_PLR_WARNINGS"};
    helix::ScopedEnv fail{"HELIX_MOCK_PLR_FAIL"};
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
};

} // namespace

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: the plugin reports the plr feature and an offer",
                 "[plr][mock]") {
    Reply status = call(client, "server.helix.status");
    REQUIRE(status.result);
    const json& plr = status.result->at("features").at("plr");
    CHECK(plr.at("version") == helix::kPlrUnlistedTierMinVersion);
    CHECK(plr.at("enabled") == true);

    Reply plr_status = call(client, helix::kPlrStatusRpc);
    REQUIRE(plr_status.result);
    auto parsed = helix::plr_parse_plugin_status(*plr_status.result);
    CHECK(parsed.backend == helix::PlrPluginBackend::Generic);
    CHECK(parsed.tier == 2);
    CHECK(parsed.offer_available);
    REQUIRE(parsed.snapshot);
    CHECK_FALSE(parsed.snapshot->filename.empty());
    CHECK(client.mock_plr_interrupted());
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: off unless both knobs say so", "[plr][mock]") {
    SECTION("no HELIX_MOCK_PLR") {
        helix::ScopedEnv off{"HELIX_MOCK_PLR", nullptr};
        CHECK_FALSE(call(client, "server.helix.status").result->at("features").contains("plr"));
        CHECK(call(client, helix::kPlrStatusRpc).error);
    }
    SECTION("plugin disabled") {
        helix::ScopedEnv disabled{"HELIX_MOCK_HELIX_PLUGIN", "disabled"};
        CHECK_FALSE(call(client, "server.helix.status").result->at("features").contains("plr"));
        CHECK_FALSE(client.mock_plr_interrupted());
    }
    SECTION("an unknown backend") {
        helix::ScopedEnv bogus{"HELIX_MOCK_PLR", "bogus"};
        CHECK(call(client, helix::kPlrStatusRpc).error);
    }
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: the plan follows the tier and warning knobs",
                 "[plr][mock]") {
    helix::ScopedEnv t{"HELIX_MOCK_PLR_TIER", "1"};
    helix::ScopedEnv w{"HELIX_MOCK_PLR_WARNINGS", "mesh_unavailable, cold_bed"};
    Reply plan = call(client, helix::kPlrPlanRpc);
    REQUIRE(plan.result);
    auto parsed = helix::plr_parse_plan_reply(*plan.result);
    REQUIRE(parsed);
    CHECK(parsed->tier == 1);
    CHECK(parsed->warnings == std::vector<std::string>{"mesh_unavailable", "cold_bed"});
    CHECK(parsed->bed_target - parsed->bed_current == Catch::Approx(30.0f));
    auto view = helix::plr_plan_view(*parsed, 2, helix::PlrPluginBackend::Generic);
    CHECK(helix::plr_plan_bed_state(view) == helix::PlrBedState::ColdWarn);
    using helix::PlrConfirmation;
    CHECK(parsed->requires_confirmations ==
          std::vector<PlrConfirmation>{PlrConfirmation::PartAttached, PlrConfirmation::NozzleClear,
                                       PlrConfirmation::ZJogged, PlrConfirmation::AcceptNoMesh,
                                       PlrConfirmation::AcceptColdBed});
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: layer_start requires accept_next_layer",
                 "[plr][mock]") {
    helix::ScopedEnv w{"HELIX_MOCK_PLR_WARNINGS", "layer_start"};
    Reply plan = call(client, helix::kPlrPlanRpc);
    REQUIRE(plan.result);
    CHECK(plan.result->at("method") == "layer_start");
    const json& req = plan.result->at("requires");
    CHECK(std::find(req.begin(), req.end(), json("accept_next_layer")) != req.end());
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: the plr entry reports saving apart from enabled",
                 "[plr][mock]") {
    client.mock_db_set(helix::kPlrDbNamespace, helix::kPlrUserKey, helix::plr_user_json(false));
    Reply status = call(client, "server.helix.status");
    REQUIRE(status.result);
    const json& plr = status.result->at("features").at("plr");
    CHECK(plr.at("saving") == false);
    // A snapshot waits, so the feature stays enabled for the offer.
    CHECK(plr.at("enabled") == true);
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: the fail knob refuses that RPC", "[plr][mock]") {
    SECTION("plan") {
        helix::ScopedEnv f{"HELIX_MOCK_PLR_FAIL", "plan"};
        Reply plan = call(client, helix::kPlrPlanRpc);
        REQUIRE(plan.error);
        CHECK(plan.error->message.find("plr_klippy_error") != std::string::npos);
    }
    SECTION("prepare") {
        helix::ScopedEnv f{"HELIX_MOCK_PLR_FAIL", "prepare"};
        Reply plan = call(client, helix::kPlrPlanRpc);
        REQUIRE(plan.result);
        Reply prepare =
            call(client, helix::kPlrPrepareRpc, json{{"plan_id", plan.result->at("plan_id")}});
        REQUIRE(prepare.error);
        CHECK(prepare.error->message.find("plr_klippy_error") != std::string::npos);
    }
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: the no_lift_room knob withholds the offer",
                 "[plr][mock]") {
    helix::ScopedEnv f{"HELIX_MOCK_PLR_FAIL", "no_lift_room"};
    Reply status = call(client, helix::kPlrStatusRpc);
    REQUIRE(status.result);
    const auto parsed = helix::plr_parse_plugin_status(*status.result);
    REQUIRE(parsed.snapshot);
    CHECK_FALSE(parsed.offer_available);
    CHECK(parsed.blocked_by == std::vector<std::string>{"no_lift_room"});
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: stale plans, missing confirmations, discard",
                 "[plr][mock]") {
    Reply first = call(client, helix::kPlrPlanRpc);
    Reply second = call(client, helix::kPlrPlanRpc);
    REQUIRE(first.result);
    REQUIRE(second.result);
    Reply stale =
        call(client, helix::kPlrPrepareRpc, json{{"plan_id", first.result->at("plan_id")}});
    REQUIRE(stale.error);
    CHECK(stale.error->message.find("plr_stale_plan") != std::string::npos);

    Reply commit = call(client, helix::kPlrCommitRpc,
                        json{{"plan_id", second.result->at("plan_id")},
                             {"confirmations", {{"part_attached", true}}},
                             {"z_reference", "trusted"}});
    REQUIRE(commit.error);
    CHECK(commit.error->message.find("plr_missing_confirmation") != std::string::npos);

    REQUIRE(call(client, helix::kPlrDiscardRpc).result);
    CHECK_FALSE(client.mock_plr_interrupted());
    auto after = helix::plr_parse_plugin_status(*call(client, helix::kPlrStatusRpc).result);
    CHECK_FALSE(after.offer_available);
    CHECK(after.blocked_by == std::vector<std::string>{"no_snapshot"});
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: the newest history job reads interrupted",
                 "[plr][mock]") {
    Reply history = call(client, "server.history.list", json{{"limit", 1}});
    REQUIRE(history.result);
    REQUIRE_FALSE(history.result->at("jobs").empty());
    CHECK(history.result->at("jobs")[0].at("status") == "interrupted");

    call(client, helix::kPlrDiscardRpc);
    history = call(client, "server.history.list", json{{"limit", 1}});
    CHECK(history.result->at("jobs")[0].at("status") != "interrupted");
}

TEST_CASE_METHOD(MockPlrFixture, "Mock PLR: the saving switch follows plr_user", "[plr][mock]") {
    auto enabled = [this]() {
        return call(client, "server.helix.status").result->at("features").at("plr").at("saving");
    };
    CHECK(enabled() == true);
    client.mock_db_set(helix::kPlrDbNamespace, helix::kPlrUserKey, helix::plr_user_json(false));
    CHECK(enabled() == false);
    CHECK(helix::plr_parse_plugin_status(*call(client, helix::kPlrStatusRpc).result).saving ==
          false);
    client.mock_db_set(helix::kPlrDbNamespace, helix::kPlrUserKey, helix::plr_user_json(true));
    CHECK(enabled() == true);
}
