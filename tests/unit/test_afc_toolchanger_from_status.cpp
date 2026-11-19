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
 * The extruder list therefore has to come from the flat top-level
 * `AFC.extruders` array, which is what these tests feed.
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
#include "test_helpers/afc_test_access.h"

#include <algorithm>
#include <any>
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace helix {

class AfcToolchangerStatusHelper : public AmsBackendAfc {
  public:
    AfcToolchangerStatusHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        AfcTestAccess::initialize_slots(*this, names);
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
        return AfcTestAccess::num_extruders(*this);
    }

    [[nodiscard]] std::vector<std::string> extruder_infos() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.reserve(AfcTestAccess::extruders(*this).size());
        for (const auto& e : AfcTestAccess::extruders(*this)) {
            out.push_back(e.name);
        }
        return out;
    }
};
} // namespace helix

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
        AfcToolchangerStatusHelper afc;
        afc.feed_afc(status_payload({"extruder", "extruder1"}));
        CHECK(afc.extruder_count() == 2);
        CHECK(afc.extruder_infos() == std::vector<std::string>{"extruder", "extruder1"});
    }

    SECTION("positional order is preserved — per-extruder actions address extruders_ by position") {
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
// The only input is a status frame, so these prove the payload the firmware
// really sends reaches the dispatch.

namespace helix {

/// Four lanes, gcode captured, driven only by fed status frames.
class AfcStatusDispatchHelper : public AmsBackendAfc {
  public:
    explicit AfcStatusDispatchHelper(std::vector<std::string> names = {"lane1", "lane2", "lane3",
                                                                       "lane4"})
        : AmsBackendAfc(nullptr, nullptr) {
        // initialize_slots() gives every lane mapped_tool == slot_index and
        // status UNKNOWN, which is what a freshly subscribed backend holds
        // before the per-lane AFC_stepper frames land.
        AfcTestAccess::initialize_slots(*this, names);
        running_ = true;
    }

    void feed_afc(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        feed(params);
    }

    /// Feed a whole status frame: the AFC object plus any per-lane objects.
    void feed(const nlohmann::json& params) {
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
    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        captured.push_back(gcode);
        pending_macro_ack = std::move(on_complete);
        return AmsErrorHelper::success();
    }

    helix::SharedCallback<void()> pending_macro_ack;

    [[nodiscard]] bool sent(const std::string& gcode) const {
        return std::find(captured.begin(), captured.end(), gcode) != captured.end();
    }

    [[nodiscard]] bool has_action(const std::string& id) const {
        auto actions = get_device_actions();
        return std::any_of(actions.begin(), actions.end(),
                           [&](const helix::printer::DeviceAction& a) { return a.id == id; });
    }

    std::vector<std::string> captured;
};
} // namespace helix

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

/// The toolchanger frame with each lane's own object, as the subscription
/// delivers it: AFC_stepper publishes the lane's T number (`map`) and the
/// head it feeds (`extruder`). The T numbers are interleaved across the heads
/// so a tool change has to read them rather than count extruders.
nlohmann::json toolchanger_frame() {
    nlohmann::json params;
    params["AFC"] = toolchanger_status();
    const std::vector<std::array<const char*, 3>> lanes{{"lane1", "T1", "extruder1"},
                                                        {"lane2", "T0", "extruder"},
                                                        {"lane3", "T2", "extruder"},
                                                        {"lane4", "T3", "extruder1"}};
    for (const auto& [lane, map, extruder] : lanes) {
        params[std::string("AFC_stepper ") + lane] = {{"map", map}, {"extruder", extruder}};
    }
    return params;
}

/// A plain single-extruder BoxTurtle. One extruder, no toolchanger.
nlohmann::json single_extruder_status() {
    nlohmann::json j = base_status({"extruder"});
    j["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    return j;
}

/// TWO extruders and NO toolchanger — IDEX, or two standalone toolheads driven
/// by their own [AFC_extruder] sections.
nlohmann::json multi_extruder_no_toolchanger_status() {
    nlohmann::json j = base_status({"extruder", "extruder1"});
    j["units"] = nlohmann::json::array({"Box_Turtle Turtle_1"});
    return j;
}

} // namespace

TEST_CASE("AFC load and tool change name the lane or T number on every topology",
          "[ams][afc][toolchanger][status][1200][1824][1850]") {
    // AFC owns lane -> extruder and T -> lane, so a load names the lane on
    // every topology. A tool change sends T{n} except on a toolchanger, where
    // it only mounts the head that T number's lane feeds.
    const std::vector<std::pair<const char*, nlohmann::json>> frames{
        {"toolchanger", toolchanger_frame()},
        {"single extruder", {{"AFC", single_extruder_status()}}},
        {"multi-extruder, no toolchanger", {{"AFC", multi_extruder_no_toolchanger_status()}}}};

    for (const auto& [label, frame] : frames) {
        CAPTURE(label);

        AfcStatusDispatchHelper load;
        load.feed(frame);
        REQUIRE(load.load_filament(1));
        CHECK(load.captured == std::vector<std::string>{"CHANGE_TOOL LANE=lane2"});

        AfcStatusDispatchHelper change;
        change.feed(frame);
        REQUIRE(change.change_tool(1));
        if (std::string(label) == "toolchanger") {
            CHECK(change.captured == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=extruder1"});
        } else {
            CHECK(change.captured == std::vector<std::string>{"T1"});
        }
    }
}

TEST_CASE("AFC toolchanger refuses a tool change while lanes have named no head",
          "[ams][afc][toolchanger][status][1850]") {
    // Before the per-lane objects arrive, every lane's head is unknown. Picking
    // one would be a guess, so nothing is sent.
    AfcStatusDispatchHelper afc;
    afc.feed_afc(toolchanger_status());
    CHECK_FALSE(afc.change_tool(1));
    CHECK(afc.captured.empty());
}

TEST_CASE("Per-extruder actions follow the frame's extruder order, not name order",
          "[ams][afc][toolchanger][status][1200]") {
    // The webhook parse sorts its map keys; the status parse must not, or the
    // _T0 and _T1 actions silently swap heads.
    AfcStatusDispatchHelper afc;
    nlohmann::json frame = toolchanger_status();
    frame["extruders"] = nlohmann::json::array({"extruder_b", "extruder_a"});
    afc.feed_afc(frame);

    REQUIRE(afc.execute_device_action("led_extruder_T0"));
    CHECK(afc.sent("AFC_SET_EXTRUDER_LED EXTRUDER=extruder_b TURN_ON=1"));
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

TEST_CASE("AFC toolchanger: Reforge's Creator 5 is one standalone lane per head",
          "[ams][afc][toolchanger][reforge]") {
    // Reforge's ff-afc.cfg (Klipper4FlashForge/firmware eda86c1): one
    // [AFC_Toolchanger Tools] unit and an [AFC_extruder e<n>] per head with
    // map T<n>. AFC builds the lane `e<n>` itself, in unit Tools, hub direct
    // (AFC_extruder.py).
    const std::vector<std::string> heads{"e0", "e1", "e2", "e3"};
    AfcStatusDispatchHelper afc(heads);
    nlohmann::json frame = {{"AFC",
                             {{"current_state", "Idle"},
                              {"units", {"Toolchanger Tools"}},
                              {"lanes", heads},
                              {"extruders", heads},
                              {"hubs", nlohmann::json::array()}}},
                            {"AFC_Toolchanger Tools", {{"lanes", heads}, {"extruders", heads}}}};
    for (std::size_t i = 0; i < heads.size(); ++i) {
        frame["AFC_lane " + heads[i]] = {{"unit", "Tools"},      {"hub", "direct"},
                                         {"extruder", heads[i]}, {"map", "T" + std::to_string(i)},
                                         {"prep", true},         {"load", true}};
    }
    afc.feed(frame);

    REQUIRE(afc.get_system_info().total_slots == 4);
    CHECK(afc.get_topology() == PathTopology::PARALLEL);

    SECTION("a tool change mounts that T number's head through AFC") {
        REQUIRE(afc.change_tool(2));
        CHECK(afc.captured == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e2"});
    }

    SECTION("SET_MAP moves a T number to another head and the tool change follows") {
        afc.feed({{"AFC_lane e2", {{"map", "T0"}}}, {"AFC_lane e0", {{"map", "T2"}}}});
        REQUIRE(afc.change_tool(0));
        CHECK(afc.captured == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=e2"});
    }
}
