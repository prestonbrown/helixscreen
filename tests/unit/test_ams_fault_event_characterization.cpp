// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_fault_event_characterization.cpp
 * @brief Byte-level pin on the ErrorEvents Happy Hare and AFC emit (#1250).
 *
 * The existing per-backend tests spot-check a field or two (source, severity,
 * "RESUME is first", "MMU_RECOVER LOADED=1 is present"). Nothing pinned the
 * WHOLE event: title text, exact detail, sticky, and the recovery list in order
 * with every RecoveryAction member — label, gcode, log_tag, style and
 * needs_hot_nozzle.
 *
 * Phase A of #1250 moved the `!!` guard, the prefix strip and the CRITICAL +
 * sticky event shape into helix::ams_fault_event helpers and promoted
 * build_recovery_actions() to a base-class virtual. Phase B will add runout
 * detection on top. Both are supposed to be invisible to the user, and this
 * file is what makes that claim checkable: it is a characterization test, so a
 * failure here means behaviour moved, not that the expectation is stale. Change
 * these strings only alongside a deliberate, described UX change.
 *
 * Also pins the base-class default: an empty recovery vector. Non-empty would
 * flip decide_presentation() from MODAL to MODAL_WITH_RECOVER for every backend
 * that does not override.
 */

#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_fault_event.h"
#include "ams_types.h"
#include "error_event.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ErrorEvent;
using helix::RecoveryAction;

namespace {

// ---------------------------------------------------------------------------
// Expectation plumbing
// ---------------------------------------------------------------------------

/// Every field of a RecoveryAction, so nothing can drift unnoticed.
struct ExpectedAction {
    const char* label;
    const char* gcode;
    const char* log_tag;
    const char* style;
    bool needs_hot_nozzle;
};

void check_actions(const std::vector<RecoveryAction>& actual,
                   const std::vector<ExpectedAction>& expected) {
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        INFO("recovery_actions[" << i << "] gcode=" << actual[i].gcode);
        CHECK(actual[i].label == expected[i].label);
        CHECK(actual[i].gcode == expected[i].gcode);
        CHECK(actual[i].log_tag == expected[i].log_tag);
        CHECK(actual[i].style == expected[i].style);
        CHECK(actual[i].needs_hot_nozzle == expected[i].needs_hot_nozzle);
    }
}

/// The fields every AMS fault event is supposed to share.
void check_common_shape(const ErrorEvent& e, helix::ErrorSource source) {
    CHECK(e.source == source);
    CHECK(e.severity == helix::ErrorSeverity::CRITICAL);
    CHECK(e.sticky);
    // Neither AMS backend populates these; the generic classifier does.
    CHECK(e.raw_detail.empty());
    CHECK(e.code.empty());
}

} // namespace

// ---------------------------------------------------------------------------
// Backend drivers. Global scope, and named in the friend lists on
// ams_backend_happy_hare.h / ams_backend_afc.h — the repo's established way for
// a test to drive private backend state (an anonymous-namespace class cannot be
// befriended).
// ---------------------------------------------------------------------------

class HhFaultEventCharHelper : public AmsBackendHappyHare {
  public:
    HhFaultEventCharHelper() : AmsBackendHappyHare(nullptr, nullptr) {
        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Happy Hare MMU";
        unit.slot_count = 4;
        unit.first_slot_global_index = 0;
        for (int i = 0; i < 4; ++i) {
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            unit.slots.push_back(slot);
        }
        system_info_.units.push_back(unit);
    }

    void feed_mmu(const nlohmann::json& mmu) {
        nlohmann::json params;
        params["mmu"] = mmu;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }
};

class AfcFaultEventCharHelper : public AmsBackendAfc {
  public:
    AfcFaultEventCharHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2", "lane3", "lane4"};
        initialize_slots(names);
    }

    void feed_afc(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    void set_toolhead_sensor(bool state) {
        std::lock_guard<std::mutex> lock(mutex_);
        tool_start_sensor_ = state;
    }

    void set_current_lane(const std::string& lane) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_lane_name_ = lane;
    }
};

namespace {

/// A backend that overrides nothing error-related, to pin the base defaults.
/// The mock implements the whole AmsBackend interface without touching
/// classify_error / current_error / build_recovery_actions.
class InertBackend : public AmsBackendMock {
  public:
    InertBackend() : AmsBackendMock(4) {}

    /// Reach the protected hook from the test.
    [[nodiscard]] std::vector<RecoveryAction> recovery_actions() const {
        return build_recovery_actions();
    }
};

} // namespace

// ===========================================================================
// Happy Hare
// ===========================================================================

TEST_CASE("Characterization: Happy Hare runout event, filament at the toolhead",
          "[ams][happy_hare][error-center][characterization][1250]") {
    HhFaultEventCharHelper hh;
    hh.feed_mmu(nlohmann::json{{"action", "Error"},
                               {"filament_pos", 8},
                               {"filament", "Loaded"},
                               {"reason_for_pause",
                                "Runout detected on gate 0  EndlessSpool mode is off - manual "
                                "intervention is required"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = hh.classify_error("!! Runout detected", ctx);

    REQUIRE(e.has_value());
    check_common_shape(*e, helix::ErrorSource::HAPPY_HARE);
    // "runout" in the detail selects the specific title.
    CHECK(e->title == "Filament runout");
    // reason_for_pause_ wins over the terse !! line.
    CHECK(e->detail == "Runout detected on gate 0  EndlessSpool mode is off - manual "
                       "intervention is required");
    check_actions(e->recovery_actions,
                  {
                      {"Resume", "RESUME", "hh::resume", "primary", true},
                      {"Recover", "MMU_RECOVER LOADED=1", "hh::recover", "", false},
                      {"Unload", "MMU_UNLOAD", "hh::unload", "", true},
                      {"Unlock", "MMU_UNLOCK", "hh::unlock", "danger", false},
                  });
}

TEST_CASE("Characterization: Happy Hare clog event, nothing at the toolhead",
          "[ams][happy_hare][error-center][characterization][1250]") {
    HhFaultEventCharHelper hh;
    hh.feed_mmu(nlohmann::json{{"action", "Error"},
                               {"filament_pos", 0}, // unloaded
                               {"filament", "Unloaded"},
                               {"reason_for_pause", "Clog detected on gate 2"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = hh.classify_error("!! Clog detected", ctx);

    REQUIRE(e.has_value());
    check_common_shape(*e, helix::ErrorSource::HAPPY_HARE);
    // No "runout" in the detail, so the generic title.
    CHECK(e->title == "Filament System Error");
    CHECK(e->detail == "Clog detected on gate 2");
    // Unload is dropped, and MMU_RECOVER flips to UNLOADED=1.
    check_actions(e->recovery_actions,
                  {
                      {"Resume", "RESUME", "hh::resume", "primary", true},
                      {"Recover", "MMU_RECOVER UNLOADED=1", "hh::recover", "", false},
                      {"Unlock", "MMU_UNLOCK", "hh::unlock", "danger", false},
                  });
}

TEST_CASE("Characterization: Happy Hare falls back to the !! text when HH gives no reason",
          "[ams][happy_hare][error-center][characterization][1250]") {
    HhFaultEventCharHelper hh;
    hh.feed_mmu(nlohmann::json{{"action", "Error"}, {"reason_for_pause", ""}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;

    SECTION("the single space after !! is consumed") {
        auto e = hh.classify_error("!! Gate 1 jammed", ctx);
        REQUIRE(e.has_value());
        CHECK(e->detail == "Gate 1 jammed");
    }
    SECTION("no space after !! means nothing extra is eaten") {
        auto e = hh.classify_error("!!Gate 1 jammed", ctx);
        REQUIRE(e.has_value());
        CHECK(e->detail == "Gate 1 jammed");
    }
    SECTION("a bare !! yields an empty detail rather than throwing") {
        auto e = hh.classify_error("!!", ctx);
        REQUIRE(e.has_value());
        CHECK(e->detail.empty());
    }
    SECTION("!! plus a lone space keeps the space") {
        auto e = hh.classify_error("!! ", ctx);
        REQUIRE(e.has_value());
        CHECK(e->detail == " ");
    }
}

TEST_CASE("Characterization: Happy Hare declines lines it does not own",
          "[ams][happy_hare][error-center][characterization][1250]") {
    HhFaultEventCharHelper hh;
    helix::ClassifyContext ctx;

    CHECK_FALSE(hh.classify_error("Error: generic klipper error", ctx).has_value());
    CHECK_FALSE(hh.classify_error("ok", ctx).has_value());
    CHECK_FALSE(hh.classify_error("!", ctx).has_value());
    CHECK_FALSE(hh.classify_error("", ctx).has_value());
    ctx.is_paused = true;
    CHECK_FALSE(hh.classify_error("!! something unrelated", ctx).has_value());
}

// ===========================================================================
// AFC
// ===========================================================================

TEST_CASE("Characterization: AFC toolhead jam from a !! line",
          "[ams][afc][error-center][characterization][1250]") {
    AfcFaultEventCharHelper afc;
    afc.set_toolhead_sensor(true);

    helix::ClassifyContext ctx; // is_jam does not need is_paused
    auto e = afc.classify_error("!! Toolhead sensor tool_end reports a jam", ctx);

    REQUIRE(e.has_value());
    check_common_shape(*e, helix::ErrorSource::AFC);
    CHECK(e->title == "Toolhead jam");
    CHECK(e->detail == "Toolhead sensor tool_end reports a jam");
    check_actions(e->recovery_actions, {
                                           {"Resume", "RESUME", "afc::resume", "primary", true},
                                           {"Unload", "TOOL_UNLOAD", "afc::tool_unload", "", true},
                                           {"Recover", "AFC_RESET", "afc::reset", "danger", false},
                                       });
}

TEST_CASE("Characterization: AFC empty toolhead offers the cold lane eject",
          "[ams][afc][error-center][characterization][1250]") {
    AfcFaultEventCharHelper afc;
    afc.set_toolhead_sensor(false);
    afc.set_current_lane("lane3");
    afc.feed_afc(nlohmann::json{{"error_state", true}, {"current_state", "Error"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = afc.classify_error("!! Lane lane3 failed to reach the hub", ctx);

    REQUIRE(e.has_value());
    check_common_shape(*e, helix::ErrorSource::AFC);
    CHECK(e->title == "Filament System Error");
    CHECK(e->detail == "Lane lane3 failed to reach the hub");
    check_actions(e->recovery_actions,
                  {
                      {"Resume", "RESUME", "afc::resume", "primary", true},
                      {"Eject", "LANE_UNLOAD LANE=lane3", "afc::lane_unload", "", false},
                      {"Recover", "AFC_RESET", "afc::reset", "danger", false},
                  });
}

TEST_CASE("Characterization: AFC with neither a loaded toolhead nor a lane",
          "[ams][afc][error-center][characterization][1250]") {
    AfcFaultEventCharHelper afc;
    afc.feed_afc(nlohmann::json{{"error_state", true}, {"current_state", "Error"}});

    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    auto e = afc.classify_error("!! Something went wrong", ctx);

    REQUIRE(e.has_value());
    check_actions(e->recovery_actions, {
                                           {"Resume", "RESUME", "afc::resume", "primary", true},
                                           {"Recover", "AFC_RESET", "afc::reset", "danger", false},
                                       });
}

TEST_CASE("Characterization: AFC status-driven current_error",
          "[ams][afc][error-center][characterization][1250]") {
    AfcFaultEventCharHelper afc;
    afc.set_toolhead_sensor(true);
    afc.feed_afc(nlohmann::json{
        {"error_state", true},
        {"current_state", "Error"},
        {"message", nlohmann::json{{"message", "Hub is already clear while trying to reset "
                                               "'lane1'"},
                                   {"type", "error"}}}});

    auto e = afc.current_error();
    REQUIRE(e.has_value());
    check_common_shape(*e, helix::ErrorSource::AFC);
    CHECK(e->title == "Filament System Error");
    CHECK(e->detail == "Hub is already clear while trying to reset 'lane1'");
    check_actions(e->recovery_actions, {
                                           {"Resume", "RESUME", "afc::resume", "primary", true},
                                           {"Unload", "TOOL_UNLOAD", "afc::tool_unload", "", true},
                                           {"Recover", "AFC_RESET", "afc::reset", "danger", false},
                                       });
}

TEST_CASE("Characterization: AFC declines lines it does not own",
          "[ams][afc][error-center][characterization][1250]") {
    AfcFaultEventCharHelper afc;
    helix::ClassifyContext ctx;

    CHECK_FALSE(afc.classify_error("Error: generic klipper error", ctx).has_value());
    CHECK_FALSE(afc.classify_error("!", ctx).has_value());
    CHECK_FALSE(afc.classify_error("", ctx).has_value());
    // Paused but no error latch and no jam signature.
    ctx.is_paused = true;
    CHECK_FALSE(afc.classify_error("!! bed mesh is not valid", ctx).has_value());
    // tool_end without a jam/break/runout word is not a jam either.
    CHECK_FALSE(afc.classify_error("!! tool_end triggered", ctx).has_value());
}

// ===========================================================================
// Shared helpers and base defaults
// ===========================================================================

TEST_CASE("helix::is_bang_line / strip_bang_prefix preserve the hand-rolled edge cases",
          "[ams][error-center][characterization][1250]") {
    CHECK(helix::is_bang_line("!!"));
    CHECK(helix::is_bang_line("!! x"));
    CHECK_FALSE(helix::is_bang_line(""));
    CHECK_FALSE(helix::is_bang_line("!"));
    CHECK_FALSE(helix::is_bang_line("! !"));
    CHECK_FALSE(helix::is_bang_line("Error: x"));

    CHECK(helix::strip_bang_prefix("!!").empty());
    CHECK(helix::strip_bang_prefix("!!x") == "x");
    // size()==3 with a space: the space is KEPT, matching the original
    // `size() > 3 && [2] == ' '` guard rather than a `>=` one.
    CHECK(helix::strip_bang_prefix("!! ") == " ");
    CHECK(helix::strip_bang_prefix("!! x") == "x");
    CHECK(helix::strip_bang_prefix("!!  x") == " x");
}

TEST_CASE("helix::make_ams_fault_event bakes in CRITICAL + sticky and nothing else",
          "[ams][error-center][characterization][1250]") {
    auto e = helix::make_ams_fault_event(helix::ErrorSource::IFS, "Title", "Detail",
                                         {{"Recover", "IFS_UNLOCK", "ifs::unlock", "primary"}});
    CHECK(e.source == helix::ErrorSource::IFS);
    CHECK(e.severity == helix::ErrorSeverity::CRITICAL);
    CHECK(e.sticky);
    CHECK(e.title == "Title");
    CHECK(e.detail == "Detail");
    CHECK(e.raw_detail.empty());
    CHECK(e.code.empty());
    REQUIRE(e.recovery_actions.size() == 1);
    CHECK(e.recovery_actions[0].gcode == "IFS_UNLOCK");
    CHECK_FALSE(e.recovery_actions[0].needs_hot_nozzle);

    // Fields the builder does not cover stay assignable by the caller.
    e.code = "key840";
    CHECK(e.code == "key840");
}

TEST_CASE("AmsBackend::build_recovery_actions defaults to empty",
          "[ams][error-center][characterization][1250]") {
    InertBackend backend;
    // Non-empty here would flip decide_presentation() from MODAL to
    // MODAL_WITH_RECOVER for every backend that does not override.
    CHECK(backend.recovery_actions().empty());
}
