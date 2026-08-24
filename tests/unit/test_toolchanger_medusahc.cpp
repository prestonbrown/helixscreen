// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolchanger_medusahc.cpp
 * @brief klipper-toolchanger init states, and the feeder MedusaHC adds.
 *
 * Two defects in status_to_action(), both found reading viesturz/klipper-toolchanger
 * @main klipper/extras/toolchanger.py:
 *
 *   STATUS_UNINITALIZED = 'uninitialized'
 *   STATUS_INITIALIZING = 'initializing'   <- we never mapped this one
 *
 *   def select_tool(self, gcmd, tool, restore_axis):
 *       if self.status == STATUS_UNINITALIZED and self.initialize_on == INIT_FIRST_USE:
 *           self.initialize(self.detected_tool)
 *       if self.status != STATUS_READY:
 *           raise gcmd.error("Cannot select tool, toolchanger status is %s, ...")
 *
 * 1. 'uninitialized' mapped to RESETTING, which is_busy(). Every tool changer
 *    boots uninitialized, so check_preconditions() refused the first tap on
 *    every klipper-toolchanger printer. On the DEFAULT initialize_on: first-use
 *    that refusal is self-inflicted: the tap is what would have triggered
 *    self.initialize(). We blocked the only thing that clears the state.
 *
 * 2. 'initializing' was unmapped and fell through to IDLE — not busy — while
 *    the toolchanger is homing and moving the carriage for real.
 *
 * MedusaHC (Irbis3D/MedusaHC) ships initialize_on: manual, so its cold boot sits
 * in 'uninitialized' until INITIALIZE_TOOLCHANGER, which is what the Reset
 * button already sends. Its feeder is a [servo my_servo] driven by OPEN/CLOSE.
 */

#include "ui_update_queue.h"

#include "ams_backend_toolchanger.h"
#include "ams_types.h"
#include "printer_discovery.h"
#include "toolchanger_addon.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

/// Real backend with gcode captured. client_ is null, so ensure_homed_then()
/// routes straight to execute_gcode().
class ToolChangerHelper : public AmsBackendToolChanger {
  public:
    explicit ToolChangerHelper(int tool_count) : AmsBackendToolChanger(nullptr, nullptr) {
        std::vector<std::string> names;
        for (int i = 0; i < tool_count; ++i) {
            names.push_back("T" + std::to_string(i));
        }
        set_discovered_tools(std::move(names));
        running_ = true;
    }

    ~ToolChangerHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    AmsError execute_gcode(const std::string& gcode) override {
        sent_.push_back(gcode);
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        sent_.push_back(gcode);
        (void)on_complete;
        return AmsErrorHelper::success();
    }

    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }

    void feed_status(const char* status, int tool_number) {
        feed(json{{"toolchanger", {{"status", status}, {"tool_number", tool_number}}}});
    }

    [[nodiscard]] const std::vector<std::string>& sent() const {
        return sent_;
    }

  private:
    std::vector<std::string> sent_;
};

/// A stock upstream MedusaHC object list — see test_medusahc_detection.cpp.
PrinterDiscovery medusahc_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3",
                                  "pin_watch io", "servo my_servo", "extruder"}));
    return hw;
}

PrinterDiscovery plain_toolchanger_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "extruder"}));
    return hw;
}

} // namespace

// ============================================================================
// Init states
// ============================================================================

TEST_CASE("A cold-boot toolchanger refuses the tool tap", "[ams][toolchanger][coldstart]") {
    ToolChangerHelper h(4);
    h.feed_status("uninitialized", -1);

    // Refusing is the lesser evil, not a clean answer. On the default
    // initialize_on: first-use, select_tool() would have auto-initialized and
    // this tap is what would have cleared the state, so the refusal costs the
    // user a Reset they should not have needed.
    //
    // Letting it through is worse. On initialize_on: manual (what MedusaHC
    // ships) Klipper raises "Cannot select tool, toolchanger status is
    // uninitialized". That rejection reaches execute_gcode()'s error callback,
    // which only logs: on_complete never fires, so the optimistic SELECTING that
    // dispatch_operation() stamped is never unwound, and execute_gcode() returns
    // success() so the `if (!result)` net misses it too. is_busy() would then
    // refuse every later op, and Moonraker only republishes CHANGED fields, so
    // no second 'uninitialized' frame arrives to reset it. A latched SELECTING
    // is unrecoverable without a restart; a refusal clears with Reset, which
    // sends INITIALIZE_TOOLCHANGER.
    CHECK(h.get_system_info().is_busy());
    CHECK(h.get_current_action() == AmsAction::RESETTING);

    auto err = h.change_tool(2);
    CHECK_FALSE(err.success());
    CHECK(h.sent().empty());
}

TEST_CASE("An initializing toolchanger IS busy", "[ams][toolchanger][coldstart]") {
    ToolChangerHelper h(4);
    h.feed_status("initializing", -1);

    // STATUS_INITIALIZING was unmapped and fell through to IDLE, so a tap landed
    // mid-initialization while the carriage was moving.
    CHECK(h.get_system_info().is_busy());
    CHECK(h.get_current_action() == AmsAction::RESETTING);

    auto err = h.change_tool(1);
    CHECK_FALSE(err.success());
    CHECK(h.sent().empty());
}

TEST_CASE("The settled toolchanger states are unchanged", "[ams][toolchanger][coldstart]") {
    // Regression guard: the two fixes above must not disturb the states that
    // already worked.
    ToolChangerHelper h(4);

    h.feed_status("ready", 0);
    CHECK(h.get_current_action() == AmsAction::IDLE);

    h.feed_status("changing", 0);
    CHECK(h.get_current_action() == AmsAction::SELECTING);

    h.feed_status("error", -1);
    CHECK(h.get_current_action() == AmsAction::ERROR);
}

TEST_CASE("The raw toolchanger status survives as operation detail",
          "[ams][toolchanger][coldstart]") {
    // IDLE alone cannot tell the user why a tap was rejected by Klipper. The
    // status string is what names the state.
    ToolChangerHelper h(4);
    h.feed_status("uninitialized", -1);
    CHECK(h.get_system_info().operation_detail == "uninitialized");
}

// ============================================================================
// Feeder capability
// ============================================================================

TEST_CASE("MedusaHC resolves a feeder", "[ams][toolchanger][feeder]") {
    auto hw = medusahc_discovery();
    auto feeder = helix::toolchanger_addon::resolve_feeder(hw);

    REQUIRE(feeder.present);
    CHECK(feeder.provider_name == "MedusaHC");
    CHECK(feeder.open_gcode == "OPEN");
    CHECK(feeder.close_gcode == "CLOSE");
}

TEST_CASE("A plain tool changer resolves no feeder", "[ams][toolchanger][feeder]") {
    auto hw = plain_toolchanger_discovery();
    auto feeder = helix::toolchanger_addon::resolve_feeder(hw);

    CHECK_FALSE(feeder.present);
    CHECK(feeder.open_gcode.empty());
    CHECK(feeder.close_gcode.empty());
}

TEST_CASE("A tool changer with a feeder exposes open and close actions",
          "[ams][toolchanger][feeder]") {
    ToolChangerHelper h(4);
    h.set_feeder(helix::toolchanger_addon::resolve_feeder(medusahc_discovery()));

    auto actions = h.get_device_actions();
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].id == "open_feeder");
    CHECK(actions[1].id == "close_feeder");
    // A BUTTON with no section renders nowhere in the device overlay.
    CHECK(actions[0].type == helix::printer::ActionType::BUTTON);
    CHECK_FALSE(actions[0].section.empty());
    CHECK_FALSE(h.get_device_sections().empty());
}

TEST_CASE("Feeder actions send the provider's gcode", "[ams][toolchanger][feeder]") {
    ToolChangerHelper h(4);
    h.set_feeder(helix::toolchanger_addon::resolve_feeder(medusahc_discovery()));
    h.feed_status("ready", 0);

    REQUIRE(h.execute_device_action("open_feeder").success());
    REQUIRE(h.execute_device_action("close_feeder").success());

    REQUIRE(h.sent().size() == 2);
    CHECK(h.sent()[0] == "OPEN");
    CHECK(h.sent()[1] == "CLOSE");
}

TEST_CASE("A tool changer without a feeder exposes no device actions",
          "[ams][toolchanger][feeder]") {
    // Every non-MedusaHC klipper-toolchanger printer must be exactly as it was.
    ToolChangerHelper h(4);

    CHECK(h.get_device_actions().empty());
    CHECK(h.get_device_sections().empty());
    CHECK_FALSE(h.execute_device_action("open_feeder").success());
}

TEST_CASE("An unknown device action is refused even with a feeder", "[ams][toolchanger][feeder]") {
    ToolChangerHelper h(4);
    h.set_feeder(helix::toolchanger_addon::resolve_feeder(medusahc_discovery()));

    CHECK_FALSE(h.execute_device_action("purge_everything").success());
    CHECK(h.sent().empty());
}
