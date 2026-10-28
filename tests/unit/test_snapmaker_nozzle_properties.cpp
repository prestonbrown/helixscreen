// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_nozzle_properties.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix::snapmaker::nozzle;

TEST_CASE("nozzle rows list the firmware's diameters and flow types",
          "[snapmaker][nozzle][nozzle_properties]") {
    CHECK(diameter_options() == std::vector<std::string>{"0.2 mm", "0.4 mm", "0.6 mm", "0.8 mm"});
    CHECK(flow_options() == std::vector<std::string>{"Standard", "High Flow"});
}

TEST_CASE("a reported nozzle maps to the option a row shows",
          "[snapmaker][nozzle][nozzle_properties]") {
    CHECK(diameter_option(0.6) == "0.6 mm");
    // A float reading of a listed diameter is that diameter.
    CHECK(diameter_option(static_cast<double>(0.2f)) == "0.2 mm");
    CHECK(diameter_option(0.25).empty());
    CHECK(diameter_option(1.0).empty());

    CHECK(flow_option("standard") == "Standard");
    CHECK(flow_option("high_flow") == "High Flow");
    CHECK(flow_option("High Flow").empty());
    CHECK(flow_option("").empty());
}

TEST_CASE("an option maps back to the exact value the firmware accepts",
          "[snapmaker][nozzle][nozzle_properties]") {
    REQUIRE(diameter_from_option("0.2 mm"));
    CHECK(*diameter_from_option("0.2 mm") == 0.2);
    CHECK(*diameter_from_option("0.8 mm") == 0.8);
    CHECK_FALSE(diameter_from_option("0.25 mm"));
    CHECK_FALSE(diameter_from_option("0.4"));

    CHECK(volume_type_from_option("High Flow") == std::optional<std::string>("high_flow"));
    CHECK(volume_type_from_option("Standard") == std::optional<std::string>("standard"));
    CHECK_FALSE(volume_type_from_option("high_flow"));
}

TEST_CASE("a change carries only the field that differs from the report",
          "[snapmaker][nozzle][nozzle_properties]") {
    const Nozzle reported{0.4, std::string("standard")};

    SECTION("a new diameter sends the diameter alone") {
        const auto p = change_params(2, reported, Nozzle{0.6, std::nullopt});
        REQUIRE(p);
        CHECK(*p == json{{"extruder", 2}, {"diameter", 0.6}});
    }
    SECTION("a new flow type sends the flow type alone") {
        const auto p = change_params(0, reported, Nozzle{std::nullopt, std::string("high_flow")});
        REQUIRE(p);
        CHECK(*p == json{{"extruder", 0}, {"volume_type", "high_flow"}});
    }
    SECTION("the reported value sends nothing") {
        CHECK_FALSE(change_params(1, reported, Nozzle{0.4, std::nullopt}));
        CHECK_FALSE(change_params(1, reported, Nozzle{std::nullopt, std::string("standard")}));
        CHECK_FALSE(change_params(1, reported, reported));
        // A float reading of the same diameter is the same diameter.
        CHECK_FALSE(change_params(1, Nozzle{static_cast<double>(0.2f), std::nullopt},
                                  Nozzle{0.2, std::nullopt}));
    }
    SECTION("an unreported diameter counts as differing") {
        const auto p = change_params(3, Nozzle{}, Nozzle{0.2, std::nullopt});
        REQUIRE(p);
        CHECK(*p == json{{"extruder", 3}, {"diameter", 0.2}});
    }
    SECTION("a diameter the firmware does not accept is never sent") {
        CHECK_FALSE(change_params(1, reported, Nozzle{0.5, std::nullopt}));
    }
    SECTION("nothing wanted sends nothing") {
        CHECK_FALSE(change_params(1, reported, Nozzle{}));
    }
}

TEST_CASE("the firmware's reply decides whether the change was made",
          "[snapmaker][nozzle][nozzle_properties]") {
    SECTION("success, whole message or result alone") {
        CHECK(read_reply(json{{"jsonrpc", "2.0"}, {"id", 7}, {"result", {{"state", "success"}}}})
                  .kind == ReplyKind::Accepted);
        CHECK(read_reply(json{{"state", "success"}}).kind == ReplyKind::Accepted);
    }
    SECTION("an error inside a successful reply is a refusal, with its words") {
        const auto r =
            read_reply(json{{"result",
                             {{"state", "error"},
                              {"message", "Cannot change nozzle properties during printing!"}}}});
        CHECK(r.kind == ReplyKind::Refused);
        CHECK(r.message == "Cannot change nozzle properties during printing!");

        const auto bare = read_reply(json{{"result", {{"state", "error"}}}});
        CHECK(bare.kind == ReplyKind::Refused);
        CHECK(bare.message.empty());
    }
    SECTION("any other shape is not taken as success") {
        CHECK(read_reply(json{{"result", "ok"}}).kind == ReplyKind::Unexpected);
        CHECK(read_reply(json::object()).kind == ReplyKind::Unexpected);
        CHECK(read_reply(json{{"result", {{"state", "busy"}}}}).kind == ReplyKind::Unexpected);
        CHECK(read_reply(json{{"result", {{"state", 1}}}}).kind == ReplyKind::Unexpected);
        CHECK(read_reply(json()).kind == ReplyKind::Unexpected);
    }
}
