// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_u1_virtual_tool_switch.cpp
 * @brief Selecting a head on an AFC toolchanger whose lanes carry several T numbers (#2191).
 *
 * The fixture is the reporter's Snapmaker U1 under AFC: four heads (e0..e3),
 * seven units, and AFC virtual tools, so lane16 answers to T0, T6 and T16 and
 * lane20 to T2, T4 and T20. A head's representative tool is the lowest T number
 * of its lanes, which on e2 and e3 is one of those extra T numbers.
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "ams_types.h"
#include "test_helpers/afc_test_access.h"
#include "tool_state.h"

#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

nlohmann::json load_u1_fixture() {
    std::string dir = __FILE__;
    const auto pos = dir.rfind("/tests/unit/");
    dir = pos != std::string::npos ? dir.substr(0, pos) + "/tests/fixtures/" : "tests/fixtures/";
    std::ifstream f(dir + "afc_u1_virtual_tools.json");
    REQUIRE(f.is_open());
    nlohmann::json j;
    f >> j;
    return j["status"];
}

} // namespace

namespace helix {

// Not in the anonymous namespace: AmsBackendAfc befriends this by name.
class AfcU1VirtualToolHelper : public AmsBackendAfc {
  public:
    AfcU1VirtualToolHelper() : AmsBackendAfc(nullptr, nullptr) {}

    void load(const nlohmann::json& status) {
        std::vector<std::string> lanes;
        std::vector<std::string> hubs;
        for (const auto& lane : status["AFC"]["lanes"]) {
            lanes.push_back(lane.get<std::string>());
        }
        for (const auto& hub : status["AFC"]["hubs"]) {
            hubs.push_back(hub.get<std::string>());
        }
        set_discovered_lanes(lanes, hubs);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // The reporter's `[AFC_extruder eN] extruder_name:` lines.
            AfcTestAccess::extruder_klipper_names(*this) = {
                {"e0", "extruder"}, {"e1", "extruder1"}, {"e2", "extruder2"}, {"e3", "extruder3"}};
            AfcTestAccess::configfile_answered(*this) = true;
        }
        running_ = true;
        // The first frame creates the slots, so per-lane data parsed during it
        // lands before they exist.
        for (int i = 0; i < 3; ++i) {
            feed(status);
        }
    }

    void feed(const nlohmann::json& status) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({status, 0.0});
        handle_status_update(notification);
    }

    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        captured.push_back(gcode);
        pending_macro_ack = std::move(on_complete);
        return AmsErrorHelper::success();
    }

    helix::SharedCallback<void()> pending_macro_ack;
    std::vector<std::string> captured;
};

} // namespace helix

namespace {

/// The U1 fixture in AmsState, its topology pushed into ToolState.
struct U1VirtualToolFixture : public HelixTestFixture {
    U1VirtualToolFixture() {
        lv_init_safe();
        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(false);
        AmsState::instance().deinit_subjects();
        AmsState::instance().init_subjects(true);

        auto backend = std::make_unique<AfcU1VirtualToolHelper>();
        afc = backend.get();
        afc->load(load_u1_fixture());
        AmsState::instance().set_backend(std::move(backend));
        sync();
    }

    ~U1VirtualToolFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        AmsState::instance().set_backend(nullptr);
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
    }

    void sync() {
        AmsState::instance().sync_from_backend();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Puts @p extruder on the shuttle and docks every other head.
    void mount(const std::string& extruder) {
        nlohmann::json frame;
        for (const auto& [section, klipper] :
             std::vector<std::pair<std::string, std::string>>{{"e0", "extruder"},
                                                              {"e1", "extruder1"},
                                                              {"e2", "extruder2"},
                                                              {"e3", "extruder3"}}) {
            frame["AFC_extruder " + section]["on_shuttle"] = klipper == extruder;
        }
        afc->feed(frame);
        sync();
    }

    /// The index of the head that prints through @p extruder.
    static int head_for(const std::vector<PhysicalHead>& heads, const std::string& extruder) {
        for (size_t i = 0; i < heads.size(); ++i) {
            if (heads[i].extruder_name == extruder) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    AfcU1VirtualToolHelper* afc = nullptr;
};

} // namespace

TEST_CASE_METHOD(U1VirtualToolFixture,
                 "AFC U1 virtual tools: selecting any head mounts it through AFC_SELECT_TOOL",
                 "[ams][afc][toolchanger][tool-change][2191]") {
    // The state before the reporter's swaps: carriage empty, and AFC driving
    // lane16 as T16 (its log: "using T16, the rest are not tracked").
    afc->feed({{"AFC_lane lane16", {{"current_map", "T16"}}}});
    mount("");

    auto& ts = ToolState::instance();
    const auto heads = ts.physical_heads();
    REQUIRE(heads.size() == 4);

    const std::string extruder = GENERATE("extruder", "extruder1", "extruder2", "extruder3");
    CAPTURE(extruder);
    const int head = head_for(heads, extruder);
    REQUIRE(head >= 0);
    const int tool = heads[static_cast<size_t>(head)].representative_tool(ts.active_tool_index());
    CAPTURE(tool);

    std::string error;
    ts.request_tool_change(tool, nullptr, nullptr, [&](const std::string& e) { error = e; });

    // With no API, a plain T<n> fallback fails with "No API connection": the
    // change has to go through the backend for every head.
    CHECK(error.empty());
    CHECK(afc->captured == std::vector<std::string>{"AFC_SELECT_TOOL TOOL=" + extruder});
}

TEST_CASE_METHOD(U1VirtualToolFixture,
                 "AFC U1 virtual tools: the mounted head is the active tool's head",
                 "[ams][afc][toolchanger][tool-change][2191]") {
    const std::string extruder = GENERATE("extruder", "extruder1", "extruder2", "extruder3");
    CAPTURE(extruder);
    mount(extruder);

    auto& ts = ToolState::instance();
    const auto heads = ts.physical_heads();
    const int active = ts.active_tool_index();
    CAPTURE(active);
    REQUIRE(active >= 0);
    CHECK(head_index_for_tool(heads, active) == head_for(heads, extruder));
}

TEST_CASE_METHOD(U1VirtualToolFixture,
                 "AFC U1 virtual tools: the captured frame's mounted head is active",
                 "[ams][afc][toolchanger][tool-change][2191]") {
    // The capture has e2 on the shuttle and no lane seated in any head.
    auto& ts = ToolState::instance();
    const auto heads = ts.physical_heads();
    const int active = ts.active_tool_index();
    CAPTURE(active, afc->get_current_tool());
    REQUIRE(active >= 0);
    CHECK(head_index_for_tool(heads, active) == head_for(heads, "extruder2"));
}
