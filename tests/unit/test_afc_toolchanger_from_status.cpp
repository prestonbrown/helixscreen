// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_toolchanger_from_status.cpp
 * @brief Toolchanger discovery from the payload the status subscription really sends.
 *
 * `num_extruders_` and `extruders_` were populated ONLY from `AFC.system`, and
 * AFC does not emit that object on the status subscription. Verified against
 * the add-on source on a live BoxTurtle: `AFC.py` `get_status()` publishes
 * current_load / current_lane / next_lane / current_state / spoolman /
 * error_state / units / lanes and no `system` key. The only writers of
 * `str["system"]['num_extruders']` are `_webhooks_status()` — the
 * `/printer/afc/status` HTTP endpoint, which we never call — and `save_vars()`,
 * which writes the vars file.
 *
 * So on real hardware `num_extruders_` stayed at its default 1 and `extruders_`
 * stayed empty, which made every toolchanger branch unreachable and
 * `AFC_SELECT_TOOL` was never dispatched on an actual AFC toolchanger.
 *
 * The dispatch branch no longer gates on `num_extruders_ > 1` — that counts
 * `[AFC_extruder]` sections, which an IDEX or standalone-toolhead machine also
 * has, and `AFC_SELECT_TOOL` exists only where an `[AFC_Toolchanger <name>]`
 * section does (AFC_Toolchanger.py:47-49, v1.2.0 only). `extruders_` discovery
 * from the flat array is still exactly as covered below; what changed is that
 * the frames which drive a DISPATCH now have to say whether a toolchanger is
 * present, because that is the real question.
 *
 * The existing multi-extruder tests all feed a `system` object, so they passed
 * while exercising a shape production never receives — the same failure mode as
 * the string `spool_id` fixtures in #1154. These tests use the flat top-level
 * `AFC.extruders` array that the firmware DOES send.
 *
 * `AFC.system` parsing is deliberately KEPT: it is real on the webhook surface
 * and carries strictly more (per-extruder lane_loaded and tool_stn distances),
 * so it stays authoritative whenever it is present.
 */

#include "ams_backend_afc.h"
#include "ams_types.h"

#include <algorithm>
#include <any>
#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

class AfcToolchangerStatusHelper : public AmsBackendAfc {
  public:
    AfcToolchangerStatusHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        initialize_slots(names);
        running_ = true;
    }

    /// Feed AFC's global object exactly as the status subscription delivers it.
    void feed_afc(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    [[nodiscard]] int extruder_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_extruders_;
    }

    [[nodiscard]] std::vector<std::string> extruder_infos() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.reserve(extruders_.size());
        for (const auto& e : extruders_) {
            out.push_back(e.name);
        }
        return out;
    }
};

namespace {

/// What get_status() actually publishes — note the absence of "system".
nlohmann::json status_payload(const std::vector<std::string>& extruders) {
    return nlohmann::json{{"current_state", "Idle"},
                          {"lanes", nlohmann::json::array({"lane1", "lane2"})},
                          {"extruders", extruders},
                          {"hubs", nlohmann::json::array({"Turtle_1"})}};
}

} // namespace

TEST_CASE("AFC discovers toolchanger extruders from the status payload",
          "[ams][afc][toolchanger]") {
    SECTION("a single extruder is not a toolchanger") {
        AfcToolchangerStatusHelper afc;
        afc.feed_afc(status_payload({"extruder"}));
        CHECK(afc.extruder_count() == 1);
        CHECK(afc.extruder_infos() == std::vector<std::string>{"extruder"});
    }

    SECTION("two extruders are both discovered, with no AFC.system present") {
        // This is the case that was broken: the firmware names both extruders in
        // the flat array, but nothing derived num_extruders_ from it, so every
        // toolchanger branch stayed unreachable.
        //
        // Discovering two extruders is NOT the same as being a toolchanger —
        // see "AFC toolchanger detection" in test_ams_backend_afc.cpp. This
        // section pins discovery only.
        AfcToolchangerStatusHelper afc;
        afc.feed_afc(status_payload({"extruder", "extruder1"}));
        CHECK(afc.extruder_count() == 2);
        CHECK(afc.extruder_infos() == std::vector<std::string>{"extruder", "extruder1"});
    }

    SECTION("positional order is preserved — extruders_ is indexed as a tool number") {
        AfcToolchangerStatusHelper afc;
        afc.feed_afc(status_payload({"extruder", "extruder1", "extruder2"}));
        REQUIRE(afc.extruder_count() == 3);
        const auto names = afc.extruder_infos();
        REQUIRE(names.size() == 3);
        CHECK(names[0] == "extruder");
        CHECK(names[1] == "extruder1");
        CHECK(names[2] == "extruder2");
    }

    SECTION("an absent extruders key leaves the previous discovery alone") {
        AfcToolchangerStatusHelper afc;
        afc.feed_afc(status_payload({"extruder", "extruder1"}));
        REQUIRE(afc.extruder_count() == 2);

        // Deltas: a frame that omits `extruders` must not reset us to 1.
        afc.feed_afc(nlohmann::json{{"current_state", "Loading"}});
        CHECK(afc.extruder_count() == 2);
        CHECK(afc.extruder_infos().size() == 2);
    }

    SECTION("an empty extruders array does not claim a toolchanger") {
        AfcToolchangerStatusHelper afc;
        afc.feed_afc(status_payload({}));
        CHECK(afc.extruder_count() == 1);
    }
}

TEST_CASE("AFC.system still wins when present — it carries strictly more",
          "[ams][afc][toolchanger]") {
    // The webhook surface supplies per-extruder lane_loaded and tool_stn
    // distances that the flat name array cannot. Deriving from names must not
    // clobber a richer system-sourced record.
    AfcToolchangerStatusHelper afc;

    nlohmann::json payload = status_payload({"extruder", "extruder1"});
    payload["system"] = nlohmann::json{
        {"num_extruders", 2},
        {"extruders",
         nlohmann::json{{"extruder", nlohmann::json{{"lane_loaded", "lane1"}, {"tool_stn", 42.0}}},
                        {"extruder1", nlohmann::json{{"lane_loaded", ""}, {"tool_stn", 55.0}}}}}};

    afc.feed_afc(payload);
    CHECK(afc.extruder_count() == 2);
    CHECK(afc.extruder_infos() == std::vector<std::string>{"extruder", "extruder1"});
}

// ============================================================================
// End-to-end: status payload in, toolchanger G-code out (#1200)
// ============================================================================
//
// The discovery tests above stop at `num_extruders_`. Everything that CONSUMES
// it is covered elsewhere by two shapes that a real printer never produces:
//
//  * `AmsBackendAfcTestHelper::setup_toolchanger(n)` in test_ams_backend_afc.cpp
//    assigns `num_extruders_` and `extruders_` directly, skipping the parser
//    entirely.
//  * The `AFC.system` fixtures in test_ams_afc_multi_extruder.cpp feed the
//    /printer/afc/status webhook shape, which the status subscription we
//    actually subscribe to does not carry.
//
// Both were green for the entire time `AFC_SELECT_TOOL` was dead on hardware,
// because neither one asks the question that matters: does the payload the
// firmware really sends reach the dispatch? These do — the only input is a
// status frame, with one deliberate exception noted below.
//
// That exception is the lane-fed toolchanger. A toolchanger whose heads are all
// fed by real lanes publishes NO `Toolchanger` entry in `units` (AFC drops any
// unit with no lanes, AFC.py v1.2.0:2554, and AFCExtruder.check_lanes() removes
// the synthetic per-head lane once a real one exists, AFC_extruder.py:391-401),
// so its status frame is byte-identical to an IDEX machine's. There is no
// status-only answer; the `[AFC_Toolchanger …]` section in configfile.settings
// is the only discriminator, and that case seeds it directly.

/// Four lanes, gcode captured, driven only by fed status frames.
class AfcStatusDispatchHelper : public AmsBackendAfc {
  public:
    AfcStatusDispatchHelper() : AmsBackendAfc(nullptr, nullptr) {
        // initialize_slots() gives every lane mapped_tool == slot_index and
        // status UNKNOWN, which is what a freshly subscribed backend holds
        // before the per-lane AFC_stepper frames land.
        std::vector<std::string> names{"lane1", "lane2", "lane3", "lane4"};
        initialize_slots(names);
        running_ = true;
    }

    void feed_afc(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // Filament operations reach the wire through the completion-callback form
    // (dispatch_operation -> ensure_homed_then -> execute_gcode(gcode, cb)).
    // Overriding only the 1-arg form captures nothing and the dispatch fails
    // with "MoonrakerAPI not available".
    AmsError execute_gcode(const std::string& gcode, std::function<void()> on_complete) override {
        captured.push_back(gcode);
        pending_macro_ack = std::move(on_complete);
        return AmsErrorHelper::success();
    }

    std::function<void()> pending_macro_ack;

    /// Stand in for an `[AFC_Toolchanger …]` section in configfile.settings.
    /// client_ is null here, so query_afc_configfile_topology() never runs.
    void seed_configfile_toolchanger(bool present) {
        std::lock_guard<std::mutex> lock(mutex_);
        configfile_has_toolchanger_ = present;
    }

    [[nodiscard]] bool sent(const std::string& gcode) const {
        return std::find(captured.begin(), captured.end(), gcode) != captured.end();
    }

    [[nodiscard]] bool sent_starting_with(const std::string& prefix) const {
        return std::any_of(captured.begin(), captured.end(),
                           [&](const std::string& g) { return g.rfind(prefix, 0) == 0; });
    }

    [[nodiscard]] bool has_action(const std::string& id) const {
        auto actions = get_device_actions();
        return std::any_of(actions.begin(), actions.end(),
                           [&](const helix::printer::DeviceAction& a) { return a.id == id; });
    }

    std::vector<std::string> captured;
};

namespace {

/// The base frame every topology below shares. No `system` key, because
/// get_status() does not emit one — and deliberately NO `units` key either, so
/// nothing inherits a toolchanger by accident. Callers opt in explicitly.
nlohmann::json base_status(const std::vector<std::string>& extruders) {
    return nlohmann::json{{"current_state", "Idle"},
                          {"lanes", nlohmann::json::array({"lane1", "lane2", "lane3", "lane4"})},
                          {"extruders", extruders},
                          {"hubs", nlohmann::json::array({"Turtle_1"})}};
}

/// A two-extruder AFC TOOLCHANGER, with the standalone toolheads AFC publishes
/// as a `Toolchanger` unit. Each head carries its own lane, so the unit keeps
/// `len(unit.lanes) > 0` and survives AFC's units filter (AFC.py v1.2.0:2554).
nlohmann::json toolchanger_status() {
    nlohmann::json j = base_status({"extruder", "extruder1"});
    j["units"] = nlohmann::json::array({"Toolchanger TC_1"});
    return j;
}

/// A plain single-extruder BoxTurtle. One extruder, no toolchanger.
nlohmann::json single_extruder_status() {
    nlohmann::json j = base_status({"extruder"});
    j["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    return j;
}

/// TWO extruders and NO toolchanger — IDEX, or two standalone toolheads driven
/// by their own [AFC_extruder] sections. `AFC_SELECT_TOOL` does not exist on
/// this machine; emitting it is the #1200 follow-up defect.
nlohmann::json multi_extruder_no_toolchanger_status() {
    nlohmann::json j = base_status({"extruder", "extruder1"});
    j["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    return j;
}

} // namespace

TEST_CASE("AFC_SELECT_TOOL dispatches from the status payload alone (#1200)",
          "[ams][afc][toolchanger][status][1200]") {
    SECTION("load_filament routes through the toolchanger on a toolchanger frame") {
        AfcStatusDispatchHelper afc;
        afc.feed_afc(toolchanger_status());

        REQUIRE(afc.load_filament(1));
        CHECK(afc.sent("AFC_SELECT_TOOL TOOL=extruder1"));
        CHECK_FALSE(afc.sent_starting_with("CHANGE_TOOL"));
    }

    SECTION("load_filament falls back to CHANGE_TOOL on a single-extruder frame") {
        // Both halves of the branch: a one-extruder frame must NOT be mistaken
        // for a toolchanger, or every BoxTurtle load turns into a tool select.
        AfcStatusDispatchHelper afc;
        afc.feed_afc(single_extruder_status());

        REQUIRE(afc.load_filament(1));
        CHECK(afc.sent("CHANGE_TOOL LANE=lane2"));
        CHECK_FALSE(afc.sent_starting_with("AFC_SELECT_TOOL"));
    }

    SECTION("change_tool routes through the toolchanger on a toolchanger frame") {
        AfcStatusDispatchHelper afc;
        afc.feed_afc(toolchanger_status());

        REQUIRE(afc.change_tool(1));
        CHECK(afc.sent("AFC_SELECT_TOOL TOOL=extruder1"));
        CHECK_FALSE(afc.sent("T1"));
    }

    SECTION("change_tool falls back to T{n} on a single-extruder frame") {
        AfcStatusDispatchHelper afc;
        afc.feed_afc(single_extruder_status());

        REQUIRE(afc.change_tool(1));
        CHECK(afc.sent("T1"));
        CHECK_FALSE(afc.sent_starting_with("AFC_SELECT_TOOL"));
    }

    SECTION("a multi-extruder frame with NO toolchanger must not tool-select") {
        // #1200 follow-up. `num_extruders_ > 1` was the old gate, so an IDEX or
        // standalone-toolhead machine — two [AFC_extruder] sections, no
        // [AFC_Toolchanger] — got `AFC_SELECT_TOOL TOOL=extruder1`. Klipper has
        // no such command there, answered `// Unknown command:"AFC_SELECT_TOOL"`
        // and the load SILENTLY never happened.
        AfcStatusDispatchHelper afc;
        afc.feed_afc(multi_extruder_no_toolchanger_status());

        REQUIRE(afc.load_filament(1));
        CHECK(afc.sent("CHANGE_TOOL LANE=lane2"));
        CHECK_FALSE(afc.sent_starting_with("AFC_SELECT_TOOL"));
    }

    SECTION("a multi-extruder frame with NO toolchanger falls back to T{n} on change_tool") {
        AfcStatusDispatchHelper afc;
        afc.feed_afc(multi_extruder_no_toolchanger_status());

        REQUIRE(afc.change_tool(1));
        CHECK(afc.sent("T1"));
        CHECK_FALSE(afc.sent_starting_with("AFC_SELECT_TOOL"));
    }

    SECTION("a lane-fed toolchanger publishes no Toolchanger unit — configfile carries it") {
        // Every toolhead is fed by a real lane, so AFCExtruder.check_lanes()
        // popped the synthetic per-toolhead lane off the Toolchanger unit
        // (AFC_extruder.py:391-401) and AFC's `len(unit.lanes) > 0` filter
        // (AFC.py v1.2.0:2554) dropped the unit from `units` altogether. The
        // status frame is indistinguishable from the IDEX case above; only the
        // [AFC_Toolchanger] section in configfile.settings tells them apart.
        AfcStatusDispatchHelper afc;
        afc.seed_configfile_toolchanger(true);
        afc.feed_afc(multi_extruder_no_toolchanger_status());

        REQUIRE(afc.load_filament(1));
        CHECK(afc.sent("AFC_SELECT_TOOL TOOL=extruder1"));
        CHECK_FALSE(afc.sent_starting_with("CHANGE_TOOL"));
    }

    SECTION("tool number indexes extruders_ positionally, not by name order") {
        // AFC emits the array in tool order and extruders_ is indexed as a tool
        // number, so T0 must be the FIRST name in the frame even when a later
        // name sorts ahead of it. The webhook parse sorts its map keys; the
        // status parse must not, or T0 and T1 silently swap.
        AfcStatusDispatchHelper afc;
        nlohmann::json frame = toolchanger_status();
        frame["extruders"] = nlohmann::json::array({"extruder_b", "extruder_a"});
        afc.feed_afc(frame);

        REQUIRE(afc.change_tool(0));
        CHECK(afc.sent("AFC_SELECT_TOOL TOOL=extruder_b"));
    }
}

TEST_CASE("Per-extruder device actions materialise from the status payload alone (#1200)",
          "[ams][afc][toolchanger][status][1200]") {
    AfcStatusDispatchHelper afc;
    afc.feed_afc(toolchanger_status());
    REQUIRE(afc.get_device_actions().size() > 0);

    SECTION("bowden sliders split per tool") {
        CHECK(afc.has_action("bowden_T0"));
        CHECK(afc.has_action("bowden_T1"));
        CHECK_FALSE(afc.has_action("bowden_length"));
    }

    SECTION("toolhead distance sliders split per tool") {
        CHECK(afc.has_action("tool_stn_T0"));
        CHECK(afc.has_action("tool_stn_T1"));
        CHECK(afc.has_action("tool_stn_unload_T0"));
        CHECK(afc.has_action("tool_stn_unload_T1"));
        CHECK_FALSE(afc.has_action("tool_stn"));
        CHECK_FALSE(afc.has_action("tool_stn_unload"));
    }

    SECTION("toolhead LED toggles split per tool") {
        CHECK(afc.has_action("led_extruder_T0"));
        CHECK(afc.has_action("led_extruder_T1"));
        CHECK_FALSE(afc.has_action("led_extruder"));
    }

    SECTION("a single-extruder frame keeps the generic actions") {
        AfcStatusDispatchHelper single;
        single.feed_afc(single_extruder_status());
        CHECK(single.has_action("bowden_length"));
        CHECK_FALSE(single.has_action("bowden_T0"));
        CHECK_FALSE(single.has_action("led_extruder_T0"));
    }

    SECTION("a hub-less topology does not advertise the hub-keyed bowden slider") {
        // SET_BOWDEN_LENGTH is HUB= keyed, and a PARALLEL / direct_load
        // machine publishes no hub object for the slider to address — the
        // control cannot succeed there.
        AfcStatusDispatchHelper hubless;
        nlohmann::json frame = single_extruder_status();
        frame["hubs"] = nlohmann::json::array();
        hubless.feed_afc(frame);
        CHECK_FALSE(hubless.has_action("bowden_length"));
    }
}

TEST_CASE("Per-extruder actions resolve names discovered from the status payload (#1200)",
          "[ams][afc][toolchanger][status][1200]") {
    // execute_device_action() indexes extruders_ to build the G-code. With the
    // vector empty — which is what the status subscription used to leave behind
    // — every one of these returns "Invalid extruder index" instead of sending.
    AfcStatusDispatchHelper afc;
    afc.feed_afc(toolchanger_status());

    SECTION("led_extruder_T1 names the second extruder from the frame") {
        REQUIRE(afc.execute_device_action("led_extruder_T1"));
        CHECK(afc.sent("AFC_SET_EXTRUDER_LED EXTRUDER=extruder1 TURN_ON=1"));
    }

    SECTION("bowden_T1 resolves the hub from the same frame") {
        REQUIRE(afc.execute_device_action("bowden_T1", std::any(500.0f)));
        CHECK(afc.sent("SET_BOWDEN_LENGTH HUB=Turtle_1 LENGTH=500"));
    }

    SECTION("an index past the discovered extruders is refused, not sent") {
        CHECK_FALSE(afc.execute_device_action("led_extruder_T7"));
        CHECK(afc.captured.empty());
    }

    SECTION("plain led_extruder drives the first extruder on a single-extruder frame") {
        AfcStatusDispatchHelper single;
        single.feed_afc(single_extruder_status());

        REQUIRE(single.execute_device_action("led_extruder"));
        CHECK(single.sent("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=1"));
        REQUIRE(single.execute_device_action("led_extruder"));
        CHECK(single.sent("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=0"));
    }

    SECTION("plain led_extruder addresses Klipper's default extruder when none is discovered") {
        // A frame that published no extruder names must not brick the toggle:
        // Klipper's first extruder is always named `extruder`.
        AfcStatusDispatchHelper bare;
        nlohmann::json frame = single_extruder_status();
        frame["extruders"] = nlohmann::json::array();
        bare.feed_afc(frame);

        REQUIRE(bare.execute_device_action("led_extruder"));
        CHECK(bare.sent("AFC_SET_EXTRUDER_LED EXTRUDER=extruder TURN_ON=1"));
    }
}
