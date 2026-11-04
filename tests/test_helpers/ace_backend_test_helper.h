// tests/test_helpers/ace_backend_test_helper.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ACEPRO driver (github.com/Kobra-S1/ACEPRO) status fixtures and the AmsBackendAce test subclass,
// shared by the ACE backend test files.
#pragma once

#include "ams_backend_ace.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "fake_moonraker_client.h"
#include "helix_shared_callback.h"
#include "moonraker_types.h"

#include <functional>
#include <json.hpp> // nlohmann/json from libhv
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

using helix::AmsAction;
using helix::AmsBackendAce;
using helix::AmsSystemInfo;
using helix::DryerInfo;

/**
 * @brief Test helper class providing access to AmsBackendAce internals
 *
 * This class provides controlled access to private members for unit testing.
 * It does NOT start the backend (no Moonraker connection needed).
 */
class AmsBackendAceTestHelper : public AmsBackendAce {
  public:
    using AmsSubscriptionBackend::send_endless_spool_switch;
    AmsBackendAceTestHelper() : AmsBackendAce(nullptr, nullptr) {}
    explicit AmsBackendAceTestHelper(helix::IMoonrakerClient* client)
        : AmsBackendAce(nullptr, client) {}
    AmsBackendAceTestHelper(helix::IMoonrakerAPI* api, helix::IMoonrakerClient* client)
        : AmsBackendAce(api, client) {}

    // Parse response helpers - call the protected parsing methods
    void test_parse_info_response(const json& data) {
        parse_info_response(data);
    }

    bool test_parse_status_response(const json& data) {
        return parse_status_response(data);
    }

    bool test_parse_slots_response(const json& data) {
        return parse_slots_response(data);
    }

    // Drive the WebSocket status-update path (protected hook) so tests can
    // exercise the filament_hub/ace key-picking logic, not just parse_ace_object.
    void test_handle_status_update(const json& notification) {
        handle_status_update(notification);
    }

    // Drive the subscription bootstrap (initial printer.objects.query). The
    // parse lands on the UpdateQueue — drain after calling.
    void test_on_started() {
        on_started();
    }

    // State accessors for verification
    AmsSystemInfo get_test_system_info() const {
        return get_system_info();
    }

    DryerInfo get_test_dryer_info() const {
        return get_dryer_info();
    }

    // Filament ops refuse unless the backend is started; these tests drive the
    // op hooks without a Moonraker connection.
    void set_running(bool state) {
        running_ = state;
    }

    // parse_slots_response resizes units[0].slots without touching slot_count
    // when the size is unchanged, so the two can disagree.
    void set_stale_slot_count(int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.units[0].slot_count = count;
    }

    void set_test_action(AmsAction action) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = action;
    }

    // Load and unload resolve through this seam, so a test can assert what was
    // sent and fire the driver's ack when it chooses. A driver that ignores a
    // toolchange still acks it, which is the case worth reproducing.
    std::vector<std::string> captured_gcodes;
    helix::SharedCallback<void()> pending_ack;
    helix::SharedCallback<void(const MoonrakerError&)> pending_error;

    // Fire-and-forget sends (bypass) take this form; the completion form below
    // is for ops whose ack matters. Both capture, so a test reads one list.
    helix::AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }

    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete,
                                  helix::SharedCallback<void(const MoonrakerError&)> on_error,
                                  bool silent) override {
        if (dispatch_via_base) {
            return AmsSubscriptionBackend::execute_gcode(gcode, std::move(on_complete),
                                                         std::move(on_error), silent);
        }
        captured_gcodes.push_back(gcode);
        pending_ack = std::move(on_complete);
        pending_error = std::move(on_error);
        return helix::AmsErrorHelper::success();
    }

  public:
    // Routes the completion-form dispatch to the REAL AmsSubscriptionBackend
    // implementation (api_ is null here) instead of the capture seam, so a
    // test can exercise the dispatch's own refusal legs.
    bool dispatch_via_base = false;
};

namespace ace_fixtures {

// --- #1069 live-capture builders (the ACEPRO driver on a Kobra S1, 1x ACE
// Pro, fw V1.3.856). Field-for-field the user's captures: the
// `ace` object is a MANAGER (no slots) whose `current_index` is the ONLY
// loaded-tool signal (-1 = nothing loaded); `ace_instance_0` carries the four
// slots with `material` (no `type`) and the dryer nested under `dryer_status`.

inline json make_kobra_manager_object(int current_index) {
    return json{
        {"ace_instances", 1},
        {"current_index", current_index},
        {"target_index", -1},
        {"endless_spool_enabled", false},
        {"endless_spool_match_mode", "exact"},
        {"ace_pro_enabled", true},
        {"toolhead_sensor", true},
        {"rdm_sensor", true},
    };
}

inline json make_kobra_slots_array() {
    return json::array({
        json{{"index", 0},
             {"tool", 0},
             {"status", "ready"},
             {"color", json::array({0, 230, 118})},
             {"material", "PLA"},
             {"temp", 225},
             {"rfid", false}},
        json{{"index", 1},
             {"tool", 1},
             {"status", "ready"},
             {"color", json::array({255, 255, 255})},
             {"material", "PETG"},
             {"temp", 245},
             {"rfid", false}},
        json{{"index", 2},
             {"tool", 2},
             {"status", "ready"},
             {"color", json::array({229, 57, 53})},
             {"material", "PETG"},
             {"temp", 255},
             {"rfid", false}},
        json{{"index", 3},
             {"tool", 3},
             {"status", "ready"},
             {"color", json::array({255, 255, 255})},
             {"material", "PLA"},
             {"temp", 225},
             {"rfid", false}},
    });
}

inline json make_kobra_instance_object() {
    return json{
        {"status", "ready"},
        {"dryer_status",
         json{{"status", "stop"}, {"target_temp", 0}, {"duration", 0}, {"remain_time", 0}}},
        {"temp", 34},
        {"enable_rfid", 1},
        {"fan_speed", 7000},
        {"feed_assist_count", 0},
        {"cont_assist_time", 0.0},
        {"slots", make_kobra_slots_array()},
        {"instance", 0},
        {"protocol", "ace1_json"},
        {"rfid_sync_enabled", true},
        {"feed_assist_slot", -1},
        {"model", "Anycubic Color Engine Pro"},
        {"firmware", "V1.3.856"},
        {"boot_firmware", "V1.0.1"},
        {"structure_version", "3"},
        {"usb_port", "/dev/ttyACM2"},
        {"usb_path", "1-1.3.4.3"},
        {"connection_state", "connected"},
    };
}

// printer.objects.query answer carrying both the manager and the slot-bearing
// instance, as the ACEPRO driver's initial query does.
inline json make_kobra_objects_query_response(int current_index) {
    json status = json::object();
    status["ace"] = make_kobra_manager_object(current_index);
    status["ace_instance_0"] = make_kobra_instance_object();
    return json{{"result", {{"eventtime", 33117.349534149}, {"status", status}}}};
}

inline json make_kobra_second_unit_object() {
    json unit1 = make_kobra_instance_object();
    unit1["instance"] = 1;
    unit1["protocol"] = "ace2_proto";
    // An ACE 2 states no model of its own.
    unit1.erase("model");
    unit1["firmware"] = "V2.0.7";
    unit1["temp"] = 51;
    unit1["humidity"] = 61;
    for (int i = 0; i < 4; ++i) {
        unit1["slots"][i]["tool"] = 4 + i;
        unit1["slots"][i]["material"] = "ABS";
        unit1["slots"][i]["color"] = json::array({63, 81, 181});
    }
    return unit1;
}

// The bootstrap query of a two-unit rig: the manager and both instances.
inline void start_two_unit_backend(AmsBackendAceTestHelper& helper,
                                   helix::test::FakeMoonrakerClient& client, int current_index) {
    helper.test_on_started();
    REQUIRE_FALSE(client.rpc_calls.empty());
    REQUIRE(client.rpc_calls.back().success_cb);
    json response = make_kobra_objects_query_response(current_index);
    response["result"]["status"]["ace"]["ace_instances"] = 2;
    response["result"]["status"]["ace_instance_1"] = make_kobra_second_unit_object();
    client.rpc_calls.back().success_cb(response);
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace ace_fixtures
