// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Characterization tests for AMS backend methods that will be extracted
// into AmsSubscriptionBackend base class. These capture current behavior
// to verify it is preserved after extraction.

#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_toolchanger.h"
#include "ams_types.h"

#include "../catch_amalgamated.hpp"

// Test helpers that expose private methods for characterization testing.
// Each backend declares these as friend classes already (or we access
// through the existing friend mechanism).

namespace helix {

// AFC: AmsBackendAfcTestHelper is already a friend in the header.
class AfcCharHelper : public AmsBackendAfc {
  public:
    AfcCharHelper() : AmsBackendAfc(nullptr, nullptr) {}

    void call_emit_event(const std::string& event, const std::string& data = "") {
        emit_event(event, data);
    }
    AmsError call_check_preconditions() const {
        return check_preconditions();
    }
    AmsError call_execute_gcode(const std::string& gcode) {
        return execute_gcode(gcode);
    }
};
} // namespace helix

namespace helix {

// HappyHare: AmsBackendHappyHareTestHelper is a friend in the header.
class HappyHareCharHelper : public AmsBackendHappyHare {
  public:
    HappyHareCharHelper() : AmsBackendHappyHare(nullptr, nullptr) {}

    void call_emit_event(const std::string& event, const std::string& data = "") {
        emit_event(event, data);
    }
    AmsError call_check_preconditions() const {
        return check_preconditions();
    }
    AmsError call_execute_gcode(const std::string& gcode) {
        return execute_gcode(gcode);
    }
};
} // namespace helix

namespace helix {

// ToolChanger: emit_event/execute_gcode/check_preconditions are private,
// but we can subclass and access them since they're declared in the private
// section but are not virtual-final. We use the friend mechanism if available.
class ToolChangerCharHelper : public AmsBackendToolChanger {
  public:
    ToolChangerCharHelper() : AmsBackendToolChanger(nullptr, nullptr) {}

    void call_emit_event(const std::string& event, const std::string& data = "") {
        emit_event(event, data);
    }
    AmsError call_check_preconditions() const {
        return check_preconditions();
    }
    AmsError call_execute_gcode(const std::string& gcode) {
        return execute_gcode(gcode);
    }
};
} // namespace helix

// --- emit_event ---

TEST_CASE("AMS backends: emit_event calls registered callback", "[ams][characterization]") {
    SECTION("AFC") {
        helix::AfcCharHelper backend;
        std::string received_event;
        std::string received_data;
        backend.set_event_callback([&](const std::string& e, const std::string& d) {
            received_event = e;
            received_data = d;
        });
        backend.call_emit_event(helix::AmsBackend::EVENT_STATE_CHANGED, "test_data");
        REQUIRE(received_event == helix::AmsBackend::EVENT_STATE_CHANGED);
        REQUIRE(received_data == "test_data");
    }
    SECTION("HappyHare") {
        helix::HappyHareCharHelper backend;
        std::string received_event;
        backend.set_event_callback(
            [&](const std::string& e, const std::string&) { received_event = e; });
        backend.call_emit_event(helix::AmsBackend::EVENT_STATE_CHANGED, "");
        REQUIRE(received_event == helix::AmsBackend::EVENT_STATE_CHANGED);
    }
    SECTION("ToolChanger") {
        helix::ToolChangerCharHelper backend;
        std::string received_event;
        backend.set_event_callback(
            [&](const std::string& e, const std::string&) { received_event = e; });
        backend.call_emit_event(helix::AmsBackend::EVENT_STATE_CHANGED, "");
        REQUIRE(received_event == helix::AmsBackend::EVENT_STATE_CHANGED);
    }
}

TEST_CASE("AMS backends: emit_event with no callback is safe", "[ams][characterization]") {
    helix::AfcCharHelper backend;
    REQUIRE_NOTHROW(backend.call_emit_event(helix::AmsBackend::EVENT_STATE_CHANGED, ""));
}

// --- check_preconditions ---

TEST_CASE("AMS backends: check_preconditions when not running", "[ams][characterization]") {
    SECTION("AFC") {
        helix::AfcCharHelper backend;
        auto err = backend.call_check_preconditions();
        REQUIRE_FALSE(err.success());
        REQUIRE(err.result == helix::AmsResult::NOT_CONNECTED);
    }
    SECTION("HappyHare") {
        helix::HappyHareCharHelper backend;
        auto err = backend.call_check_preconditions();
        REQUIRE_FALSE(err.success());
        REQUIRE(err.result == helix::AmsResult::NOT_CONNECTED);
    }
    SECTION("ToolChanger") {
        helix::ToolChangerCharHelper backend;
        auto err = backend.call_check_preconditions();
        REQUIRE_FALSE(err.success());
        REQUIRE(err.result == helix::AmsResult::NOT_CONNECTED);
    }
}

// --- execute_gcode ---

TEST_CASE("AMS backends: execute_gcode without API returns error", "[ams][characterization]") {
    SECTION("AFC") {
        helix::AfcCharHelper backend;
        auto err = backend.call_execute_gcode("G28");
        REQUIRE_FALSE(err.success());
        REQUIRE(err.result == helix::AmsResult::NOT_CONNECTED);
    }
    SECTION("HappyHare") {
        helix::HappyHareCharHelper backend;
        auto err = backend.call_execute_gcode("G28");
        REQUIRE_FALSE(err.success());
        REQUIRE(err.result == helix::AmsResult::NOT_CONNECTED);
    }
    SECTION("ToolChanger") {
        helix::ToolChangerCharHelper backend;
        auto err = backend.call_execute_gcode("G28");
        REQUIRE_FALSE(err.success());
        REQUIRE(err.result == helix::AmsResult::NOT_CONNECTED);
    }
}

// --- State query defaults ---

TEST_CASE("AMS backends: default state after construction", "[ams][characterization]") {
    SECTION("AFC") {
        helix::AmsBackendAfc backend(nullptr, nullptr);
        REQUIRE(backend.get_type() == helix::AmsType::AFC);
        REQUIRE(backend.get_current_tool() == -1);
        REQUIRE(backend.get_current_slot() == -1);
        REQUIRE_FALSE(backend.is_filament_loaded());
        REQUIRE(backend.get_current_action() == helix::AmsAction::IDLE);
        REQUIRE_FALSE(backend.is_running());
    }
    SECTION("HappyHare") {
        helix::AmsBackendHappyHare backend(nullptr, nullptr);
        REQUIRE(backend.get_type() == helix::AmsType::HAPPY_HARE);
        REQUIRE(backend.get_current_tool() == -1);
        REQUIRE(backend.get_current_slot() == -1);
        REQUIRE_FALSE(backend.is_filament_loaded());
        REQUIRE(backend.get_current_action() == helix::AmsAction::IDLE);
        REQUIRE_FALSE(backend.is_running());
    }
    SECTION("ToolChanger") {
        helix::AmsBackendToolChanger backend(nullptr, nullptr);
        REQUIRE(backend.get_type() == helix::AmsType::TOOL_CHANGER);
        REQUIRE(backend.get_current_tool() == -1);
        REQUIRE(backend.get_current_slot() == -1);
        REQUIRE_FALSE(backend.is_filament_loaded());
        REQUIRE(backend.get_current_action() == helix::AmsAction::IDLE);
        REQUIRE_FALSE(backend.is_running());
    }
}

// --- is_running / stop ---

TEST_CASE("AMS backends: stop when not running is safe", "[ams][characterization]") {
    helix::AmsBackendAfc afc(nullptr, nullptr);
    REQUIRE_NOTHROW(afc.stop());
    helix::AmsBackendHappyHare hh(nullptr, nullptr);
    REQUIRE_NOTHROW(hh.stop());
    helix::AmsBackendToolChanger tc(nullptr, nullptr);
    REQUIRE_NOTHROW(tc.stop());
}

// --- start without client/api ---

TEST_CASE("AMS backends: start without client returns not_connected", "[ams][characterization]") {
    SECTION("AFC") {
        helix::AmsBackendAfc backend(nullptr, nullptr);
        auto err = backend.start();
        REQUIRE_FALSE(err.success());
        REQUIRE(err.result == helix::AmsResult::NOT_CONNECTED);
    }
    SECTION("HappyHare") {
        helix::AmsBackendHappyHare backend(nullptr, nullptr);
        auto err = backend.start();
        REQUIRE_FALSE(err.success());
        REQUIRE(err.result == helix::AmsResult::NOT_CONNECTED);
    }
}

TEST_CASE("Tool changer names its positions tools", "[ams][toolchanger][numbering]") {
    helix::AmsBackendToolChanger backend(nullptr, nullptr);
    CHECK(backend.lane_noun() == helix::ui::LaneNoun::Tool);
}
