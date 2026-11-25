// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Snapmaker Resume on a head fed by hand, and what the tag reader takes back from its lane.

#include "../helix_test_fixture.h"
#include "ams_backend_snapmaker.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "lane_source_store.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/registered_backend.h"

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::printer;

using json = nlohmann::json;

namespace {

/// Head 0's feeder over an empty port, reading @p state: a feeder that is there, in automatic mode.
json feeder_over_empty_port(const std::string& state) {
    return json{{"filament_feed left",
                 {{"extruder0",
                   {{"filament_detected", false},
                    {"channel_state", state},
                    {"channel_error", "ok"},
                    {"module_exist", true},
                    {"disable_auto", false}}}}}};
}

// The toolhead switch of head 0 reading filament in the toolhead.
const json kHead0SwitchOn{
    {"filament_motion_sensor e0_filament", {{"enabled", true}, {"filament_detected", true}}}};

/// The tag reader's entry for head 0: PLA by Snapmaker, a 190-220°C nozzle range, a 60°C bed,
/// 1 kg, and @p colors colors where the tag carries more than one. The other heads have no entry.
json tag_on_head0(int colors = 1) {
    json entry{{"MAIN_TYPE", "PLA"},
               {"SUB_TYPE", "Basic"},
               {"MANUFACTURER", "Snapmaker"},
               {"VENDOR", "Snapmaker"},
               {"ARGB_COLOR", 0xFF202020},
               {"HOTEND_MIN_TEMP", 190},
               {"HOTEND_MAX_TEMP", 220},
               {"BED_TEMP", 60},
               {"WEIGHT", 1000},
               {"CARD_UID", json::array({1, 2, 3, 4})}};
    if (colors > 1) {
        entry["COLOR_NUMS"] = colors;
        entry["RGB_1"] = 0xFF0000;
        entry["RGB_2"] = 0x0000FF;
    }
    return json{{"filament_detect", {{"info", json::array({entry, nullptr, nullptr, nullptr})}}}};
}

// The tag reader naming no tag on any head.
const json kNoTagAnywhere{
    {"filament_detect",
     {{"info", json::array({json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}},
                            json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}};

} // namespace

TEST_CASE("Snapmaker Resume trusts a hand push over a feeder that reports a failed load",
          "[ams][snapmaker][resume][load_unconfirmed]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;

    // The automatic load found nothing at the port and failed.
    backend.fire_status(feeder_over_empty_port("load_fail"));
    const auto feeder = backend.channel_snapshot(0);
    REQUIRE(feeder.state == "load_fail");
    REQUIRE(feeder.module_exist);
    REQUIRE_FALSE(feeder.disable_auto);

    // A hand push then raises the toolhead switch over that still-empty port.
    backend.fire_status(kHead0SwitchOn);
    REQUIRE(backend.head_filament_ops(0).load == FilamentOpSupport::Manual);

    std::optional<AmsError> answer;
    backend.prepare_for_resume(0, [&](const AmsError& err) { answer = err; });
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(answer.has_value());
    INFO(answer->technical_msg << " / " << answer->user_msg);
    CHECK(answer->success());
}

TEST_CASE("Snapmaker reader naming no tag keeps tag-read values while the lane's edit is out",
          "[ams][snapmaker][rfid][filament_slot_override]") {
    HelixTestFixture fixture;
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(json{{"print_stats", {{"state", "standby"}}}});
    backend.fire_status(tag_on_head0());
    REQUIRE(backend.get_slot_info(0).nozzle_temp_min == 190);

    // A person's edit of the color is on its way to the head. Nothing has
    // echoed that color, so the lane still holds the edit, and it binds no spool.
    SlotInfo edit = backend.get_slot_info(0);
    edit.color_rgb = 0x123456;
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), edit).success());
    const auto lane = backend.lane_id(0);
    REQUIRE(helix::ams::lane_sources(lane).pending.has_value());
    REQUIRE_FALSE(helix::ams::lane_sources(lane).spoolman.has_value());

    // The temperatures and the weight are still what the tag stated. Whether
    // they are the person's own can be told only from the lane holding the edit.
    backend.fire_status(kNoTagAnywhere);
    const auto info = backend.get_slot_info(0);
    CHECK(info.nozzle_temp_min == 190);
    CHECK(info.nozzle_temp_max == 220);
    CHECK(info.bed_temp == 60);
    CHECK(info.total_weight_g == 1000.0f);
}

TEST_CASE("Snapmaker drops the mode an edit gave the tag's colors once both have gone",
          "[ams][snapmaker][rfid][filament_slot_override]") {
    HelixTestFixture fixture;
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
    auto& backend = *backend_reg;

    backend.fire_status(json{{"print_stats", {{"state", "standby"}}}});
    backend.fire_status(tag_on_head0(/*colors=*/2));
    REQUIRE(backend.get_slot_info(0).multi_color_hexes == "#FF0000,#0000FF");

    // A person keeps the tag's two colors and says only how they run.
    SlotInfo edit = backend.get_slot_info(0);
    edit.color_mode = "gradient";
    REQUIRE(backend.commit_user_edit(0, backend.get_slot_info(0), edit).success());
    REQUIRE(backend.get_slot_info(0).color_mode == "gradient");

    // The lane lets go of the edit. Nothing states the mode any more, while the
    // slot still shows it beside the list the tag laid down.
    backend.clear_slot_override(0);
    const auto lane = backend.lane_id(0);
    REQUIRE_FALSE(helix::ams::lane_sources(lane).pending.has_value());
    REQUIRE_FALSE(helix::ams::lane_sources(lane).record.has_value());
    REQUIRE(backend.get_slot_info(0).color_mode == "gradient");

    SECTION("the reader then names one color") {
        backend.fire_status(tag_on_head0());
    }
    SECTION("the reader then names no tag") {
        backend.fire_status(kNoTagAnywhere);
    }

    // The list was the tag's, so the mode that described it goes with it.
    const auto info = backend.get_slot_info(0);
    CHECK(info.multi_color_hexes.empty());
    CHECK(info.color_mode.empty());
}
