// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ace_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;
using namespace helix::ace;

TEST_CASE("ACE status parse reads only the fields an object carries", "[ace][status_parse]") {
    const ObjectDelta d = parse_ace_object(json::object());
    CHECK_FALSE(d.model);
    CHECK_FALSE(d.firmware);
    CHECK_FALSE(d.action);
    CHECK_FALSE(d.slots.entries);
    CHECK_FALSE(d.first_loaded_entry);
    CHECK_FALSE(d.dryer.current_temp_c);
    CHECK_FALSE(d.dryer.active);
    CHECK_FALSE(d.humidity);
    CHECK_FALSE(d.current_index);
    CHECK_FALSE(d.sensors.stated());
    CHECK_FALSE(d.target_index);
    CHECK_FALSE(d.endless_spool_enabled);
}

TEST_CASE("ACE status parse reads the object's own fields", "[ace][status_parse]") {
    const ObjectDelta d = parse_ace_object(json{{"model", "ACE Pro"},
                                                {"protocol", "ace2_proto"},
                                                {"firmware", "V1.3"},
                                                {"status", "loading"},
                                                {"humidity", 41.5},
                                                {"loaded_slot", 2},
                                                {"current_index", -1},
                                                {"rdm_sensor", true},
                                                {"target_index", 3},
                                                {"ace_pro_enabled", false},
                                                {"feed_assist_slot", 1},
                                                {"endless_spool_enabled", true},
                                                {"endless_spool_match_mode", "material"}});
    CHECK(d.model == "ACE Pro");
    CHECK(d.protocol == "ace2_proto");
    CHECK(d.firmware == "V1.3");
    CHECK(d.action == AmsAction::LOADING);
    CHECK(d.humidity == Catch::Approx(41.5f));
    CHECK(d.loaded_slot == 2);
    CHECK(d.current_index == -1);
    CHECK(d.sensors.rdm == true);
    CHECK_FALSE(d.sensors.toolhead);
    CHECK(d.target_index == 3);
    CHECK(d.ace_pro_enabled == false);
    CHECK(d.feed_assist_slot == 1);
    CHECK(d.endless_spool_enabled == true);
    CHECK(d.endless_spool_match_mode == "material");
}

TEST_CASE("ACE status parse refuses object fields of the wrong type", "[ace][status_parse]") {
    const ObjectDelta d = parse_ace_object(json{{"model", 12},
                                                {"humidity", nullptr},
                                                {"loaded_slot", 1.5},
                                                {"current_index", "2"},
                                                {"ace_pro_enabled", 1},
                                                {"slots", "four"}});
    CHECK_FALSE(d.model);
    CHECK_FALSE(d.humidity);
    CHECK_FALSE(d.loaded_slot);
    CHECK_FALSE(d.current_index);
    CHECK_FALSE(d.ace_pro_enabled);
    CHECK_FALSE(d.slots.entries);
    CHECK(d.slots.refused_size == 0);
}

TEST_CASE("ACE status parse reads a slot entry", "[ace][status_parse]") {
    const SlotDelta s = read_slot(json{{"status", "ready"},
                                       {"color", json::array({255, 85, 0})},
                                       {"material", "PETG"},
                                       {"type", "PLA"},
                                       {"tool", 5},
                                       {"rfid", 2},
                                       {"park_state", "parked"},
                                       {"temp_min", "190"},
                                       {"temp_max", 230}});
    CHECK(s.status_word == "ready");
    CHECK(s.status == SlotStatus::AVAILABLE);
    CHECK(s.color == 0xFF5500u);
    CHECK(s.material == "PETG");
    CHECK(s.tool == 5);
    CHECK(s.tag_read == TagRead::Identified);
    CHECK(s.park_state == ParkState::Parked);
    CHECK(s.temp_min == 190);
    CHECK(s.temp_max == 230);

    const SlotDelta bare = read_slot(json{{"type", "ABS"}, {"park_state", "loaded"}});
    CHECK_FALSE(bare.status);
    CHECK_FALSE(bare.color);
    CHECK(bare.material == "ABS");
    CHECK(bare.tool == -1);
    CHECK(bare.tag_read == TagRead::Silent);
    CHECK(bare.park_state == ParkState::Loaded);
    CHECK_FALSE(bare.temp_min);

    CHECK(read_slot(json{{"park_state", "staged"}}).park_state == ParkState::Unknown);
    CHECK_FALSE(read_slot(json{{"temp_min", "hot"}}).temp_min);
}

TEST_CASE("ACE status parse maps every driver's slot words", "[ace][status_parse]") {
    for (const char* word : {"available", "loaded", "ready", "preload", "running", "feeding",
                             "unwinding", "shifting", "identifying"}) {
        CAPTURE(word);
        CHECK(read_slot(json{{"status", word}}).status == SlotStatus::AVAILABLE);
    }
    CHECK(read_slot(json{{"status", "empty"}}).status == SlotStatus::EMPTY);
    CHECK(read_slot(json{{"status", "runout"}}).status == SlotStatus::EMPTY);
    CHECK(read_slot(json{{"status", "unknown"}}).status == SlotStatus::UNKNOWN);
}

TEST_CASE("ACE status parse keeps a non-object slot's place in the array", "[ace][status_parse]") {
    const SlotArray a =
        read_slots(json{{"slots", json::array({json{{"status", "empty"}}, 7, json::object()})}});
    REQUIRE(a.entries);
    REQUIRE(a.entries->size() == 3);
    CHECK((*a.entries)[0]);
    CHECK_FALSE((*a.entries)[1]);
    CHECK((*a.entries)[2]);
    CHECK(a.refused_size == 0);
}

TEST_CASE("ACE status parse refuses a slots array over sixteen entries", "[ace][status_parse]") {
    json slots = json::array();
    for (int i = 0; i < 17; ++i) {
        slots.push_back(json{{"status", i == 16 ? "loaded" : "ready"}});
    }
    const ObjectDelta over = parse_ace_object(json{{"slots", slots}});
    CHECK_FALSE(over.slots.entries);
    CHECK(over.slots.refused_size == 17);
    // The ValgACE seat scan reads the raw array, refused or not.
    CHECK(over.first_loaded_entry == 16);

    slots.erase(slots.begin());
    const SlotArray at_limit = read_slots(json{{"slots", slots}});
    REQUIRE(at_limit.entries);
    CHECK(at_limit.entries->size() == 16);
    CHECK_FALSE(read_slots(json::object()).entries);
}

TEST_CASE("ACE status parse names the first slot stating loaded", "[ace][status_parse]") {
    const ObjectDelta d = parse_ace_object(
        json{{"slots", json::array({json{{"status", "ready"}}, 3, json{{"status", "loaded"}},
                                    json{{"status", "loaded"}}})}});
    CHECK(d.first_loaded_entry == 2);
    CHECK_FALSE(parse_ace_object(json{{"slots", json::array({json{{"status", "ready"}}})}})
                    .first_loaded_entry);
}

TEST_CASE("ACE status parse reads the slot native current_filament names", "[ace][status_parse]") {
    const auto slot = [](const char* cf) {
        return parse_ace_object(json{{"current_filament", cf}}).current_filament_slot;
    };
    CHECK(slot("0-2") == 2);
    CHECK(slot("1-0") == 0);
    CHECK_FALSE(slot(""));
    CHECK_FALSE(slot("2"));
    CHECK_FALSE(slot("0-"));
    CHECK_FALSE(slot("0-x"));
    CHECK_FALSE(slot("0--1"));
}

TEST_CASE("ACE status parse reads the ACEPRO unit link", "[ace][status_parse]") {
    const auto linked = [](const char* state) {
        return parse_ace_object(json{{"connection_state", state}}).linked;
    };
    CHECK(linked("connected") == true);
    for (const char* down : {"initializing", "connecting", "reconnecting", "disabled"}) {
        CAPTURE(down);
        CHECK(linked(down) == false);
    }
    CHECK_FALSE(linked("rebooting"));
}

TEST_CASE("ACE status parse reads the dryer under either spelling", "[ace][status_parse]") {
    const ObjectDelta klipper = parse_ace_object(json{
        {"temp", 41},
        {"dryer_status",
         json{{"status", "run"}, {"target_temp", 55}, {"duration", 240}, {"remain_time", 200}}}});
    CHECK(klipper.dryer.active == true);
    CHECK(klipper.dryer.target_temp_c == Catch::Approx(55.0f));
    CHECK(klipper.dryer.duration_min == 240);
    CHECK(klipper.dryer.remaining_min == 200);
    CHECK(klipper.dryer.current_temp_c == Catch::Approx(41.0f));

    // A dryer's own current temp is more specific than the ambient one.
    const ObjectDelta bridge = parse_ace_object(
        json{{"temp", 28}, {"dryer", json{{"active", false}, {"current_temp", 45.5}}}});
    CHECK(bridge.dryer.active == false);
    CHECK(bridge.dryer.current_temp_c == Catch::Approx(45.5f));
    CHECK_FALSE(bridge.dryer.target_temp_c);

    CHECK(parse_ace_object(json{{"dryer", json{{"status", "stop"}}}}).dryer.active == false);
}

TEST_CASE("ACE status parse reads the REST info response", "[ace][status_parse]") {
    const InfoResponse ok =
        parse_info_response(json{{"model", "ACE Pro"}, {"version", "1.2.3"}, {"slot_count", 4}});
    CHECK(ok.names_model);
    CHECK(ok.version == "1.2.3");
    CHECK(ok.slot_count == 4);
    CHECK_FALSE(ok.refused_slot_count);

    const InfoResponse wrong =
        parse_info_response(json{{"model", 12345}, {"version", true}, {"slot_count", "four"}});
    CHECK_FALSE(wrong.names_model);
    CHECK_FALSE(wrong.version);
    CHECK_FALSE(wrong.slot_count);
    CHECK_FALSE(wrong.refused_slot_count);

    CHECK(parse_info_response(json{{"slot_count", 16}}).slot_count == 16);
    CHECK(parse_info_response(json{{"slot_count", 17}}).refused_slot_count == 17);
    CHECK(parse_info_response(json{{"slot_count", -1}}).refused_slot_count == -1);
}

TEST_CASE("ACE status parse reads the REST status response", "[ace][status_parse]") {
    const StatusResponse r = parse_status_response(json{
        {"model", "ACE Pro"},
        {"firmware", "2.5.1"},
        {"loaded_slot", -1},
        {"instance_index", 1},
        {"ace_manager", json{{"current_index", 3}, {"toolhead_sensor", true}, {"target_index", 2}}},
        {"dryer", json{{"active", true}, {"remaining_minutes", 180}, {"duration_minutes", 240}}}});
    CHECK(r.model == "ACE Pro");
    CHECK(r.firmware == "2.5.1");
    CHECK(r.loaded_slot == -1);
    CHECK(r.names_manager);
    CHECK(r.instance_index == 1);
    CHECK(r.manager_object);
    CHECK(r.manager_current_index == 3);
    CHECK(r.manager_sensors.toolhead == true);
    CHECK(r.manager_target_index == 2);
    CHECK(r.dryer.active == true);
    CHECK(r.dryer.remaining_min == 180);
    CHECK(r.dryer.duration_min == 240);

    const StatusResponse bare =
        parse_status_response(json{{"instance_index", -1}, {"ace_manager", "x"}});
    CHECK_FALSE(bare.instance_index);
    CHECK(bare.names_manager);
    CHECK_FALSE(bare.manager_object);
}

TEST_CASE("ACE status parse takes the bridge action over the unit status", "[ace][status_parse]") {
    const auto action = [](const json& data) { return parse_status_response(data).action; };
    CHECK(action(json{{"action", "loading"}}) == AmsAction::LOADING);
    CHECK(action(json{{"action", "unloading"}}) == AmsAction::UNLOADING);
    CHECK(action(json{{"action", "error"}}) == AmsAction::ERROR);
    CHECK(action(json{{"action", "idle"}, {"status", "loading"}}) == AmsAction::IDLE);
    CHECK(action(json{{"status", "loading"}}) == AmsAction::LOADING);
    // A stated empty action is the bridge's answer: the unit status is not read.
    CHECK_FALSE(action(json{{"action", ""}, {"status", "loading"}}));

    CHECK_FALSE(parse_status_response(json{{"action", "idle"}}).action_from_unit_status);
    CHECK(parse_status_response(json{{"status", "ready"}}).action_from_unit_status);
}
