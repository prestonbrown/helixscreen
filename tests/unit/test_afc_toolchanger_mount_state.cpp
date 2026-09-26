// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_toolchanger_mount_state.cpp
 * @brief AFC multi-unit toolchanger state derivation (#1229).
 *
 * Drives the real backend with a scrubbed capture from a Voron 2.4 StealthChanger
 * running AFC 1.2.1 — three heterogeneous units (HTLF selector, OpenAMS, AFC
 * Toolchanger), 10 lanes, 6 extruders, 5 Klipper tool objects. Nothing else in
 * tests/fixtures/ resembles it: the existing AFC fixtures are a single Box Turtle
 * with one unit, four lanes and one extruder, which is why none of the defects in
 * #1229 were caught.
 *
 * The machine was idle at capture. AFC.current_lane and current_load are both
 * null, toolchanger.tool is null with tool_number -1, every AFC_extruder reports
 * on_shuttle false, and Klipper's own log line reads "Tool unselected". Nothing
 * was mounted. One lane (lane1, ASA) held filament in a *parked* toolhead and one
 * (lane5, ABS) was staged at its hub — neither of which makes a tool current.
 *
 * The fixture carries objects/list as well as the status blob, because slot
 * indices are assigned from lane discovery order and a status-only capture cannot
 * reproduce a slot-numbering defect.
 */

#include "ui_bypass_spool_widget.h"

#include "ams_backend_afc.h"
#include "ams_bypass_policy.h"
#include "ams_types.h"
#include "settings_manager.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Resolve tests/fixtures/ from __FILE__ so the test does not depend on cwd.
std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/";
    }
    return "tests/fixtures/";
}

nlohmann::json load_fixture(const std::string& name) {
    const std::string path = fixture_dir() + name;
    std::ifstream f(path);
    INFO("fixture missing or unreadable: " << path);
    REQUIRE(f.is_open());
    nlohmann::json j;
    f >> j;
    return j;
}

/// Drives AmsBackendAfc the way production does: discovery first (which fixes the
/// slot ordering), then a status frame.
class AfcToolchangerHelper : public AmsBackendAfc {
  public:
    AfcToolchangerHelper() : AmsBackendAfc(nullptr, nullptr) {}

    /// Mirror PrinterDiscovery: walk objects/list in order, in the same order the
    /// real discovery does (printer_discovery.h:351).
    void discover(const nlohmann::json& object_list) {
        for (const auto& entry : object_list) {
            const std::string name = entry.get<std::string>();
            if (name.rfind("AFC_lane ", 0) == 0) {
                lane_names_.push_back(name.substr(9));
            } else if (name.rfind("AFC_hub ", 0) == 0) {
                hub_names_seen_.push_back(name.substr(8));
            }
        }
        // Only the discovery half. The backend initializes its own slots on the
        // first status frame (ams_backend_afc.cpp:1872, :2892) — going through
        // that path rather than calling initialize_slots() directly keeps the
        // slot ordering whatever production would actually produce, which is the
        // entire point of the exercise.
        set_discovered_lanes(lane_names_, hub_names_seen_);
    }

    void feed(const nlohmann::json& params_inner) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    /// A real subscription delivers many frames. The first one is what creates
    /// the slots, and per-lane data parsed during that same frame lands before
    /// the slots exist — so a single frame leaves every slot blank and any
    /// assertion about elected state passes vacuously. Feed until the lane data
    /// has actually populated.
    void feed_until_settled(const nlohmann::json& params_inner, int frames = 3) {
        for (int i = 0; i < frames; ++i) {
            feed(params_inner);
        }
    }

    const std::vector<std::string>& discovered_lanes() const {
        return lane_names_;
    }

  private:
    std::vector<std::string> lane_names_;
    std::vector<std::string> hub_names_seen_;
};

/// Emit the derived state so a failing run explains itself instead of just
/// reporting a number. Logged at warn so it survives the default test level.
void dump_state(const AmsSystemInfo& info, const std::vector<std::string>& lanes) {
    spdlog::warn("=== #1229 derived state ===");
    spdlog::warn("  units={} total_slots={} current_slot={} current_tool={} filament_loaded={}",
                 info.units.size(), info.total_slots, info.current_slot, info.current_tool,
                 info.filament_loaded);
    spdlog::warn("  current_toolchange={} number_of_toolchanges={} supports_bypass={}",
                 info.current_toolchange, info.number_of_toolchanges, info.supports_bypass);

    for (const auto& unit : info.units) {
        spdlog::warn("  unit '{}' slots={} first_global={}", unit.name, unit.slot_count,
                     unit.first_slot_global_index);
    }

    // Global index comes from unit reorganization, NOT from lane discovery order,
    // so do not label slots with lanes[g] — that conflates two different orderings
    // and is how the first version of this test misread the mapping.
    for (const auto& unit : info.units) {
        for (int local = 0; local < unit.slot_count; ++local) {
            const int g = unit.first_slot_global_index + local;
            const SlotInfo* s = info.get_slot_global(g);
            if (s == nullptr) {
                spdlog::warn("  slot[{}] (1-based {}) <null>", g, g + 1);
                continue;
            }
            spdlog::warn("  slot[{}] (1-based {:2d}) unit='{}' local={} mapped_tool={:3d} "
                         "material='{}' spoolman_id={}",
                         g, g + 1, unit.name, local, s->mapped_tool, s->material, s->spoolman_id);
        }
    }
    std::string order;
    for (const auto& lane : lanes) {
        if (!order.empty()) {
            order += ", ";
        }
        order += lane;
    }
    spdlog::warn("  (lane discovery order was: {})", order);
}

/// The capture has exactly two lanes carrying filament. If neither shows up in
/// the derived slots, the lane data never reached them and every assertion about
/// elected state below would pass vacuously.
void require_lane_data_landed(const AmsSystemInfo& info) {
    bool saw_asa = false;
    bool saw_abs = false;
    for (int g = 0; g < info.total_slots; ++g) {
        const SlotInfo* s = info.get_slot_global(g);
        if (s == nullptr) {
            continue;
        }
        if (s->spoolman_id == 66 || s->material == "ASA") {
            saw_asa = true;
        }
        if (s->spoolman_id == 53 || s->material == "ABS") {
            saw_abs = true;
        }
    }
    INFO("lane1 (ASA, spool 66) never reached a slot — the harness is not exercising "
         "the parse path and any state assertion would be vacuous");
    REQUIRE(saw_asa);
    INFO("lane5 (ABS, spool 53) never reached a slot — same problem");
    REQUIRE(saw_abs);
}

} // namespace

// ============================================================================
// Nothing is mounted — the machine is idle and every source agrees
// ============================================================================

TEST_CASE("AFC toolchanger: idle machine elects no current slot", "[ams][afc][1229][toolchanger]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    const AmsSystemInfo info = afc.get_system_info();
    dump_state(info, afc.discovered_lanes());
    require_lane_data_landed(info);

    // Ground truth from the capture, restated so a future reader does not have to
    // open the fixture to know why these are the expected values.
    REQUIRE(fixture["status"]["AFC"]["current_lane"].is_null());
    REQUIRE(fixture["status"]["AFC"]["current_load"].is_null());
    REQUIRE(fixture["status"]["toolchanger"]["tool"].is_null());
    REQUIRE(fixture["status"]["toolchanger"]["tool_number"].get<int>() == -1);
    for (const char* ext :
         {"extruder", "extruder1", "extruder2", "extruder3", "extruder4", "extruder5"}) {
        const std::string key = std::string("AFC_extruder ") + ext;
        INFO(key << " unexpectedly reports on_shuttle");
        REQUIRE_FALSE(fixture["status"][key]["on_shuttle"].get<bool>());
    }

    // Nothing is on the shuttle, so no slot is current. Today this elects a slot
    // that no source names, and that phantom then drives the header, the topology
    // render, and an unload the user never asked for (ui_ams_sidebar.cpp).
    INFO("a slot was elected current on a machine with an empty shuttle");
    CHECK(info.current_slot == -1);

    INFO("a tool was elected current on a machine with an empty shuttle");
    CHECK(info.current_tool == -1);
}

// ============================================================================
// A tool that goes away must take its elected slot with it
// ============================================================================

TEST_CASE("AFC toolchanger: dropping the mounted tool clears the elected slot",
          "[ams][afc][1229][toolchanger]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    // Put the `extruder` toolhead on the carriage. It holds lane1 (global slot
    // 0, the ASA spool), and a mounted extruder names its own seated lane —
    // precise even where several lanes feed one extruder, which a lane→tool map
    // cannot be.
    nlohmann::json mounted = fixture["status"];
    mounted["AFC_extruder extruder"]["on_shuttle"] = true;
    afc.feed(mounted);

    {
        const AmsSystemInfo info = afc.get_system_info();
        spdlog::warn("--- after mounting the `extruder` toolhead ---");
        dump_state(info, afc.discovered_lanes());
        INFO("mounting a toolhead did not make its seated lane current");
        CHECK(info.current_slot == 0);
    }

    // Now drop it: back to the captured idle frame, every on_shuttle false.
    // Before #1229 nothing walked current_slot back, so the header kept naming a
    // slot no source claimed for as long as the machine stayed idle — and a
    // later real toolchange was what finally "caught up with reality", exactly
    // as the second reporter described.
    afc.feed(fixture["status"]);

    const AmsSystemInfo info = afc.get_system_info();
    spdlog::warn("--- after dropping the toolhead ---");
    dump_state(info, afc.discovered_lanes());

    INFO("the carriage emptied but current_slot kept the departed toolhead's lane");
    CHECK(info.current_slot == -1);
    CHECK(info.filament_loaded == false);
}

// ============================================================================
// Klipper's toolchanger is the fallback when AFC does not report on_shuttle
// ============================================================================

TEST_CASE("AFC toolchanger: falls back to Klipper toolchanger when on_shuttle is absent",
          "[ams][afc][1229][toolchanger]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    // Older AFC omits on_shuttle entirely. "Absent" must not read as "not
    // mounted" — the Klipper toolchanger object has to carry the answer instead.
    nlohmann::json legacy = fixture["status"];
    for (auto& item : legacy.items()) {
        if (item.key().rfind("AFC_extruder ", 0) == 0) {
            item.value().erase("on_shuttle");
        }
    }

    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(legacy);

    // Klipper says T0 is on the carriage. With no AFC signal, its tool→slot map
    // decides: AFC maps T0 to the `extruder5` lane, global slot 5 — rendered as
    // "Slot 6". Note this is a DIFFERENT answer than the AFC-native path gives
    // for "T0", because Klipper's tool T0 is `extruder` while AFC's map T0 is
    // the extruder5 lane. Two numbering systems, same spelling (#1229).
    nlohmann::json mounted = legacy;
    mounted["toolchanger"]["tool_number"] = 0;
    mounted["toolchanger"]["tool"] = "tool T0";
    afc.feed(mounted);

    {
        const AmsSystemInfo info = afc.get_system_info();
        spdlog::warn("--- legacy AFC: after Klipper reports T0 mounted ---");
        dump_state(info, afc.discovered_lanes());
        CHECK(info.current_tool == 0);
        INFO("Klipper's toolchanger did not drive the slot in the absence of on_shuttle");
        CHECK(info.current_slot == 5);
    }

    afc.feed(legacy);

    const AmsSystemInfo info = afc.get_system_info();
    spdlog::warn("--- legacy AFC: after the tool is dropped ---");
    dump_state(info, afc.discovered_lanes());
    CHECK(info.current_tool == -1);
    INFO("dropping the tool left its slot elected");
    CHECK(info.current_slot == -1);
}

// ============================================================================
// AFC's own on_shuttle signal, without any Klipper toolchanger object
// ============================================================================

TEST_CASE("AFC toolchanger: on_shuttle decides mount state with no Klipper toolchanger",
          "[ams][afc][1229][toolchanger]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    // The reporter intends to retire KTC and let AFC own the toolchanger role,
    // so the Klipper objects must not be load-bearing. Strip them entirely.
    nlohmann::json afc_only = fixture["status"];
    afc_only.erase("toolchanger");
    for (const char* t : {"tool T0", "tool T1", "tool T2", "tool T3", "tool T4"}) {
        afc_only.erase(t);
    }
    REQUIRE_FALSE(afc_only.contains("toolchanger"));

    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(afc_only);

    const AmsSystemInfo info = afc.get_system_info();
    dump_state(info, afc.discovered_lanes());
    require_lane_data_landed(info);

    // Every AFC_extruder reports on_shuttle false, so the carriage is empty and
    // the parked ASA in lane1 must not be elected.
    INFO("a slot was elected from AFC data alone, with no tool on the carriage");
    CHECK(info.current_slot == -1);
    CHECK(info.filament_loaded == false);
}

TEST_CASE("AFC toolchanger: on_shuttle picks the mounted extruder's own lane",
          "[ams][afc][1229][toolchanger]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    nlohmann::json mounted = fixture["status"];
    mounted.erase("toolchanger");
    // `extruder` holds lane1 (global slot 0, the ASA spool). Putting it on the
    // carriage must make its own seated lane current — the extruder names the
    // lane directly, which a lane→tool map cannot do for a shared extruder.
    mounted["AFC_extruder extruder"]["on_shuttle"] = true;

    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(mounted);

    const AmsSystemInfo info = afc.get_system_info();
    dump_state(info, afc.discovered_lanes());
    require_lane_data_landed(info);

    INFO("mounting the extruder holding lane1 did not make slot 0 current");
    CHECK(info.current_slot == 0);
}

// ============================================================================
// Shape — the units and lanes the capture actually describes
// ============================================================================

TEST_CASE("AFC toolchanger: three units and ten lanes are discovered",
          "[ams][afc][1229][toolchanger]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    const AmsSystemInfo info = afc.get_system_info();
    dump_state(info, afc.discovered_lanes());

    REQUIRE(afc.discovered_lanes().size() == 10);
    CHECK(info.total_slots == 10);
    CHECK(info.units.size() == 3);
}

// ============================================================================
// Bypass — off in the capture, so nothing may present it as engaged
// ============================================================================

TEST_CASE("AFC toolchanger: bypass reads inactive when the firmware says so",
          "[ams][afc][1229][toolchanger][bypass]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    REQUIRE_FALSE(fixture["status"]["AFC"]["bypass_state"].get<bool>());
    REQUIRE_FALSE(
        fixture["status"]["filament_switch_sensor virtual_bypass"]["enabled"].get<bool>());

    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    INFO("bypass reported active while AFC.bypass_state is false");
    CHECK_FALSE(afc.is_bypass_active());
}

// ============================================================================
// A lane whose routing is not yet known must not be treated as direct-routed
// ============================================================================

TEST_CASE("Bypass node visibility rule", "[ams][afc][1229][toolchanger][bypass]") {
    using helix::ui::bypass_node_visible;

    SECTION("no bypass support means no node, whatever else is true") {
        CHECK_FALSE(bypass_node_visible(false, true, true, true));
        CHECK_FALSE(bypass_node_visible(false, false, false, false));
    }

    SECTION("AFC hides the node while bypass is disengaged") {
        // The #1229 case: AFC publishes a virtual bypass whether or not one is
        // wired, so an always-visible node advertised hardware the machine does
        // not have — and got painted with the loaded lane's filament.
        CHECK_FALSE(bypass_node_visible(true, /*active=*/false, /*bypass_is_virtual=*/true,
                                        /*always_show=*/false));
    }

    SECTION("the opt-in setting brings it back") {
        CHECK(bypass_node_visible(true, /*active=*/false, /*bypass_is_virtual=*/true,
                                  /*always_show=*/true));
    }

    SECTION("an engaged bypass is always shown, setting or not") {
        CHECK(bypass_node_visible(true, /*active=*/true, /*bypass_is_virtual=*/true,
                                  /*always_show=*/false));
        CHECK(bypass_node_visible(true, /*active=*/true, /*bypass_is_virtual=*/true,
                                  /*always_show=*/true));
    }

    SECTION("non-AFC backends are unaffected — their bypass is a real position") {
        CHECK(bypass_node_visible(true, /*active=*/false, /*bypass_is_virtual=*/false,
                                  /*always_show=*/false));
    }
}

// ============================================================================
// Firmware may report no bypass on a machine that has one
// ============================================================================

// Happy Hare defaults [mmu_machine] has_bypass to 0 for mmu_vendor "Other" — what
// a Qidi Box under Happy Hare reports — so the owner of a working PTFE bypass gets
// no bypass UI. The override contradicts that report on purpose. It is safe
// because Happy Hare's select_bypass() never consults has_bypass(): the command
// deselects the gear steppers and reports gate -2 either way.
TEST_CASE("Bypass availability override", "[ams][bypass][override]") {
    using helix::bypass_available;

    SECTION("firmware saying yes needs no override") {
        CHECK(bypass_available(true, /*force=*/false));
        CHECK(bypass_available(true, /*force=*/true));
    }

    SECTION("firmware saying no hides the controls by default") {
        CHECK_FALSE(bypass_available(false, /*force=*/false));
    }

    SECTION("the override is the only thing that contradicts the firmware") {
        CHECK(bypass_available(false, /*force=*/true));
    }

    SECTION("with the override off, the resolver is exactly the firmware value") {
        // Guards the short-circuit in bypass_available_for(): a machine that
        // reports a bypass must never depend on a setting to keep it.
        REQUIRE_FALSE(SettingsManager::instance().get_ams_force_bypass_controls());
        CHECK(helix::bypass_available_for(true));
        CHECK_FALSE(helix::bypass_available_for(false));
    }
}

TEST_CASE("AFC toolchanger: the captured machine hides its bypass node",
          "[ams][afc][1229][toolchanger][bypass]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");
    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    // Bypass is off in the capture, and the backend still advertises support
    // because AFC's virtual_bypass sensor exists. That combination is exactly
    // what put a green "ASA / Bypass" spool on the reporter's screen.
    REQUIRE_FALSE(afc.is_bypass_active());
    REQUIRE(afc.get_system_info().supports_bypass);
    REQUIRE(afc.bypass_is_virtual());

    INFO("the bypass node was drawn on a machine with bypass disengaged");
    CHECK_FALSE(helix::ui::bypass_node_visible_for(&afc));

    // Engaging bypass brings it back with no setting change.
    nlohmann::json engaged = fixture["status"];
    engaged["AFC"]["bypass_state"] = true;
    afc.feed(engaged);
    REQUIRE(afc.is_bypass_active());
    CHECK(helix::ui::bypass_node_visible_for(&afc));

    // No backend attached: nothing to draw.
    CHECK_FALSE(helix::ui::bypass_node_visible_for(nullptr));
}

TEST_CASE("AFC toolchanger: a lane with unknown routing does not split its unit",
          "[ams][afc][1229][toolchanger][topology]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    AfcToolchangerHelper afc;
    afc.discover(fixture["object_list"]);

    // Moonraker sends deltas, and nlohmann orders object keys so that unit
    // objects ("AFC_OpenAMS AMS_1") sort before per-lane ones ("AFC_lane …") —
    // uppercase precedes lowercase. A frame can therefore describe a unit while
    // some of its lanes have never been parsed. Dropping lane8 reproduces that
    // window deterministically.
    nlohmann::json partial = fixture["status"];
    partial.erase("AFC_lane lane8");
    REQUIRE_FALSE(partial.contains("AFC_lane lane8"));
    afc.feed_until_settled(partial);

    const AmsSystemInfo info = afc.get_system_info();
    dump_state(info, afc.discovered_lanes());

    int ams_index = -1;
    for (size_t i = 0; i < info.units.size(); ++i) {
        if (info.units[i].name.find("AMS_1") != std::string::npos) {
            ams_index = static_cast<int>(i);
        }
    }
    REQUIRE(ams_index >= 0);

    // Every AMS_1 lane is hub-routed in the capture. Three are known here and one
    // is unknown; counting the unknown as `direct` is what tipped this unit into
    // MIXED and drew a lane straight into a toolhead of its own.
    INFO("unknown lane routing was counted as direct and split the unit");
    CHECK(afc.get_unit_topology(ams_index) == PathTopology::HUB);
}

// ============================================================================
// Routing — every AMS_1 lane goes through the hub, so the unit is not MIXED
// ============================================================================

TEST_CASE("AFC toolchanger: AMS_1 lanes all route through their hub",
          "[ams][afc][1229][toolchanger][topology]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    // The capture is unambiguous: no direct-routed lane exists in this unit, and
    // all four feed the same extruder. A split render cannot be justified by the
    // data (#1229 defect 4).
    for (const char* lane : {"lane5", "lane6", "lane7", "lane8"}) {
        const std::string key = std::string("AFC_lane ") + lane;
        INFO(key << " is not hub-routed to AMS_1 as expected");
        REQUIRE(fixture["status"][key]["hub"].get<std::string>() == "AMS_1");
        REQUIRE(fixture["status"][key]["extruder"].get<std::string>() == "extruder3");
    }
}
