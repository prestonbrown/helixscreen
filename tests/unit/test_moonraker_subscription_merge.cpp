// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_subscription_merge.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

TEST_CASE("merge keeps every app object and field", "[moonraker][subscription]") {
    json app = {{"extruder", json::array({"temperature", "target"})}, {"print_stats", nullptr}};
    json extra = {{"extruder", json::array({"power"})}, {"temperature_sensor chamber", nullptr}};
    json m = helix::merge_subscription_objects(app, extra);
    CHECK(m["extruder"] == json::array({"temperature", "target", "power"}));
    CHECK(m["print_stats"].is_null());
    CHECK(m.contains("print_stats"));
    CHECK(m["temperature_sensor chamber"].is_null());
    CHECK(m.size() == 3);
}

TEST_CASE("null in either side subscribes every field", "[moonraker][subscription]") {
    json a = {{"fan", json::array({"speed"})}};
    CHECK(helix::merge_subscription_objects(a, {{"fan", nullptr}})["fan"].is_null());
    CHECK(helix::merge_subscription_objects({{"fan", nullptr}},
                                            {{"fan", json::array({"rpm"})}})["fan"]
              .is_null());
}

TEST_CASE("empty or malformed extras leave the app map unchanged", "[moonraker][subscription]") {
    json app = {{"toolhead", json::array({"position"})}};
    CHECK(helix::merge_subscription_objects(app, json::object()) == app);
    CHECK(helix::merge_subscription_objects(app, json()) == app);
    CHECK(helix::merge_subscription_objects(app, {{"x", 5}}) == app);
    CHECK(helix::merge_subscription_objects(app, {{"y", json::array({1, 2})}}) == app);
}

TEST_CASE("duplicate fields are not repeated", "[moonraker][subscription]") {
    json m = helix::merge_subscription_objects({{"e", json::array({"t"})}},
                                               {{"e", json::array({"t", "t", "p"})}});
    CHECK(m["e"] == json::array({"t", "p"}));
}
