// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_runout_empty_lane_scope.cpp
 * @brief Tests for backend-aware runout scoping (FilamentSensorManager::has_real_runout()).
 *
 * Run with: ./build/bin/helix-tests "[runout][scope]"
 *
 * Bug (Snapmaker U1, live): a multi-color print used heads 0 and 2; head 1 was
 * left INTENTIONALLY empty (never loaded). After the print completed, head 1's
 * empty lane raised a FALSE filament-runout alarm — its filament_motion_sensor
 * reads filament_detected=false, has_any_runout() returned true, and the idle
 * runout modal popped.
 *
 * has_real_runout() distinguishes "lane is EMPTY / never-loaded" (intentional,
 * not an alarm) from "lane was LOADED/present and lost filament mid-use" (real
 * runout). It maps each runout sensor (Snapmaker "e{N}_filament" -> slot N) to
 * its AMS lane and consults the backend's slot status.
 *
 * These tests drive a REAL AmsBackendSnapmaker (via filament_feed status) so
 * the lane EMPTY/AVAILABLE status is produced by production code, and a REAL
 * FilamentSensorManager fed via update_from_status().
 */

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "ams_subscription_backend.h"
#include "ams_types.h"
#include "filament_sensor_manager.h"
#include "filament_sensor_types.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;
using json = nlohmann::json;

// Friend shim (declared as friend in ams_backend_snapmaker.h) to reach the
// protected handle_status_update() so tests can drive slot state through the
// real firmware parse. Distinct from test_ams_backend_snapmaker.cpp's
// SnapmakerTestAccess to avoid an ODR clash when both TUs link.
class RunoutScopeTestAccess {
  public:
    static void handle_status(AmsBackendSnapmaker& b, const json& n) {
        b.handle_status_update(n);
    }

    // Set a sensor's role directly, bypassing set_sensor_role()'s single-RUNOUT
    // exclusivity. Production reaches this multi-RUNOUT state via load_config
    // (settings.json restore), which writes sensor->role per entry with no
    // exclusivity check — exactly how a Snapmaker U1's four e{N}_filament
    // sensors all end up RUNOUT-roled.
    static void force_role(helix::FilamentSensorManager& mgr, const std::string& klipper,
                           helix::FilamentSensorRole role) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        for (auto& s : mgr.sensors_) {
            if (s.klipper_name == klipper) {
                s.role = role;
                s.enabled = true;
                return;
            }
        }
    }

    // Reset the manager to a clean slate (sensors + states cleared, grace
    // period already expired) so each test starts deterministic despite the
    // process-singleton.
    static void reset(helix::FilamentSensorManager& mgr) {
        std::lock_guard<std::recursive_mutex> lock(mgr.mutex_);
        mgr.sensors_.clear();
        mgr.states_.clear();
        mgr.master_enabled_ = true;
        mgr.sync_mode_ = true;
        mgr.initial_status_received_ = false;
        mgr.startup_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    }
};

namespace {

// Builds a filament_feed status setting per-lane physical presence. detected[i]
// drives the Snapmaker backend's slot status: true -> AVAILABLE (present),
// false (and not LOADED) -> EMPTY. Lanes 0,1 go to "left", 2,3 to "right" to
// mirror the firmware split — but the backend reads both keys, so any split
// works; we put all four under "left" for simplicity.
json make_feed_status(const std::array<bool, 4>& detected) {
    json left = json::object();
    for (int i = 0; i < 4; ++i) {
        std::string key = (i == 0) ? "extruder0" : fmt::format("extruder{}", i);
        left[key] = json{
            {"filament_detected", detected[i]}, {"channel_state", "idle"}, {"channel_error", "ok"}};
    }
    return json{{"filament_feed left", left}};
}

// Feed a per-lane motion sensor reading into FilamentSensorManager. The
// Snapmaker per-tool sensors are "filament_motion_sensor e{N}_filament".
json make_sensor_status(int lane, bool filament_detected) {
    std::string key = fmt::format("filament_motion_sensor e{}_filament", lane);
    return json{{key, json{{"filament_detected", filament_detected}, {"enabled", true}}}};
}

// RAII: install a real Snapmaker backend into AmsState and remove it on scope
// exit so the global singleton is clean for other tests.
struct ScopedSnapmakerBackend {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    AmsBackendSnapmaker* backend = nullptr;

    ScopedSnapmakerBackend() {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        auto be = std::make_unique<AmsBackendSnapmaker>(api.get(), nullptr);
        backend = be.get();
        AmsState::instance().set_backend(std::move(be));
    }
    ~ScopedSnapmakerBackend() {
        AmsState::instance().set_backend(nullptr);
    }

    void set_lane_presence(const std::array<bool, 4>& detected) {
        RunoutScopeTestAccess::handle_status(*backend, make_feed_status(detected));
    }
};

void drain() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

// Sets up FilamentSensorManager with four per-lane runout sensors enabled, all
// initially showing filament present. Roles are forced directly (mirroring the
// settings.json restore path) so all four lanes are RUNOUT-roled — the real
// Snapmaker U1 configuration, which set_sensor_role()'s exclusivity can't model.
void setup_four_lane_sensors(FilamentSensorManager& fsm) {
    RunoutScopeTestAccess::reset(fsm);
    fsm.set_master_enabled(true);
    std::vector<std::string> names;
    for (int i = 0; i < 4; ++i) {
        names.push_back(fmt::format("filament_motion_sensor e{}_filament", i));
    }
    fsm.discover_sensors(names);
    for (int i = 0; i < 4; ++i) {
        std::string klipper = fmt::format("filament_motion_sensor e{}_filament", i);
        RunoutScopeTestAccess::force_role(fsm, klipper, FilamentSensorRole::RUNOUT);
        // Establish availability + present baseline.
        fsm.update_from_status(make_sensor_status(i, true));
    }
}

// Like setup_four_lane_sensors but does NOT feed any status, so the sensors are
// discovered + RUNOUT-roled but their state stays available=false — i.e. no
// fresh data has arrived yet (early connect). Exercises the freshness gate.
void setup_four_lane_sensors_no_status(FilamentSensorManager& fsm) {
    RunoutScopeTestAccess::reset(fsm);
    fsm.set_master_enabled(true);
    std::vector<std::string> names;
    for (int i = 0; i < 4; ++i) {
        names.push_back(fmt::format("filament_motion_sensor e{}_filament", i));
    }
    fsm.discover_sensors(names);
    for (int i = 0; i < 4; ++i) {
        RunoutScopeTestAccess::force_role(
            fsm, fmt::format("filament_motion_sensor e{}_filament", i), FilamentSensorRole::RUNOUT);
    }
}

} // namespace

TEST_CASE("has_real_runout: empty/never-loaded lane sensor=false is NOT a runout",
          "[runout][scope]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Lane 1 is empty/never-loaded; lanes 0,2 present.
    backend.set_lane_presence({true, false, true, false});
    REQUIRE(AmsState::instance().get_backend()->get_slot_info(1).is_present() == false);

    // Lane 1's runout sensor reads no filament — but the lane is EMPTY.
    fsm.update_from_status(make_sensor_status(1, false));
    drain();

    // has_any_runout() (the OLD behavior) DOES fire — proving the sensor reads false.
    CHECK(fsm.has_any_runout());
    // has_real_runout() (the FIX) must NOT fire for an empty lane.
    CHECK_FALSE(fsm.has_real_runout());
}

TEST_CASE("has_real_runout: loaded/present lane that loses filament IS a runout",
          "[runout][scope]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Lane 0 is present (loaded for the print).
    backend.set_lane_presence({true, false, false, false});
    REQUIRE(AmsState::instance().get_backend()->get_slot_info(0).is_present());

    // Lane 0 runs out mid-use: present lane, sensor now false.
    fsm.update_from_status(make_sensor_status(0, false));
    drain();

    CHECK(fsm.has_any_runout());
    // A genuine runout on a present/loaded lane MUST still alarm.
    CHECK(fsm.has_real_runout());
}

TEST_CASE("has_real_runout: mixed heads 0,2 loaded + head 1 empty", "[runout][scope]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Exact bug shape: heads 0 and 2 used (present), head 1 left empty.
    backend.set_lane_presence({true, false, true, false});

    // Head 1 (empty) sensor reads false post-print -> NO false runout.
    fsm.update_from_status(make_sensor_status(1, false));
    drain();
    CHECK_FALSE(fsm.has_real_runout());

    // Now head 0 (a LOADED lane) loses filament -> real runout MUST fire.
    fsm.update_from_status(make_sensor_status(0, false));
    drain();
    CHECK(fsm.has_real_runout());
}

TEST_CASE("has_real_runout: no AMS backend -> behaves like has_any_runout", "[runout][scope]") {
    HelixTestFixture fx;
    AmsState::instance().set_backend(nullptr);
    auto& fsm = FilamentSensorManager::instance();

    // A plain single-extruder runout sensor whose name does NOT encode a lane.
    RunoutScopeTestAccess::reset(fsm);
    fsm.set_master_enabled(true);
    fsm.discover_sensors({"filament_switch_sensor fsensor"});
    fsm.set_sensor_role("filament_switch_sensor fsensor", FilamentSensorRole::RUNOUT);
    fsm.set_sensor_enabled("filament_switch_sensor fsensor", true);
    fsm.update_from_status(
        json{{"filament_switch_sensor fsensor", json{{"filament_detected", true}}}});
    drain();
    CHECK_FALSE(fsm.has_real_runout());

    // Sensor goes empty: with no lane mapping, this is a real runout.
    fsm.update_from_status(
        json{{"filament_switch_sensor fsensor", json{{"filament_detected", false}}}});
    drain();
    CHECK(fsm.has_real_runout());
}

// ============================================================================
// FIX A core — find_empty_required_lanes(tools_used, remap): lane-truth scoped
// to the tools the print actually uses, names the offending (tool, slot) pairs.
// FIX B core — compute_scoped_runout_value(tools_used, remap): the -1/0/1/2
// badge encoding scoped to the active print's tools, lane truth.
//
// Lane truth comes from the AMS backend (filament_exist[head] -> slot present),
// NOT the per-tool motion sensor (which reads false when staged-but-retracted).
// ============================================================================

TEST_CASE("scoped runout: required tool present in lane, toolhead sensor false -> NO warning",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Head 0 filament IS staged in the lane (present), but retracted from the
    // toolhead so the per-tool motion sensor reads false. This is the exact
    // false-positive case the fix must NOT warn on.
    backend.set_lane_presence({true, false, false, false});
    fsm.update_from_status(make_sensor_status(0, false)); // sensor false, lane present
    drain();

    REQUIRE(AmsState::instance().get_backend()->get_slot_info(0).is_present());

    std::set<int> tools_used{0};
    std::map<int, int> identity{}; // no remap -> tool 0 routes to head 0

    auto empty = fsm.find_empty_required_lanes(tools_used, identity);
    CHECK(empty.empty()); // lane present -> not empty -> NO warning

    // Badge: all required lanes present -> "loaded" (1), green, no red badge.
    CHECK(fsm.compute_scoped_runout_value(tools_used, identity) == 1);
}

TEST_CASE("scoped runout: required tool lane genuinely empty -> warning names tool+lane",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // The print needs tool 0, but lane 0 is genuinely empty.
    backend.set_lane_presence({false, true, true, true});
    fsm.update_from_status(make_sensor_status(0, false));
    drain();

    REQUIRE_FALSE(AmsState::instance().get_backend()->get_slot_info(0).is_present());

    std::set<int> tools_used{0};
    std::map<int, int> identity{};

    auto empty = fsm.find_empty_required_lanes(tools_used, identity);
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].first == 0);  // tool 0
    CHECK(empty[0].second == 0); // routes to lane/slot 0

    // Badge: a required lane is empty -> "runout" (0), red.
    CHECK(fsm.compute_scoped_runout_value(tools_used, identity) == 0);
}

TEST_CASE("scoped runout: unused empty head is ignored -> NO warning",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Print uses only head 0 (present). Heads 1,2,3 are empty but UNUSED.
    backend.set_lane_presence({true, false, false, false});
    fsm.update_from_status(make_sensor_status(0, true));
    for (int i = 1; i < 4; ++i) {
        fsm.update_from_status(make_sensor_status(i, false));
    }
    drain();

    std::set<int> tools_used{0}; // only head 0 used
    std::map<int, int> identity{};

    // Empty heads 1-3 are not in tools_used -> NOT flagged.
    auto empty = fsm.find_empty_required_lanes(tools_used, identity);
    CHECK(empty.empty());
    CHECK(fsm.compute_scoped_runout_value(tools_used, identity) == 1);
}

TEST_CASE("scoped runout: multi-tool print, one required lane empty -> only that tool named",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Print uses heads 0 and 2. Head 0 present, head 2 EMPTY. Head 1 (unused) also empty.
    backend.set_lane_presence({true, false, false, false});
    drain();

    std::set<int> tools_used{0, 2};
    std::map<int, int> identity{};

    auto empty = fsm.find_empty_required_lanes(tools_used, identity);
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].first == 2); // only tool 2 flagged
    CHECK(empty[0].second == 2);
    CHECK(fsm.compute_scoped_runout_value(tools_used, identity) == 0);
}

TEST_CASE("scoped runout: remap routes a tool to a different (present) lane -> NO warning",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Tool 0's identity lane (0) is EMPTY, but the user remapped tool 0 -> lane 2,
    // which is present. Lane truth must follow the remap, not the identity head.
    backend.set_lane_presence({false, false, true, false});
    drain();

    std::set<int> tools_used{0};
    std::map<int, int> remap{{0, 2}}; // tool 0 -> slot 2

    auto empty = fsm.find_empty_required_lanes(tools_used, remap);
    CHECK(empty.empty()); // remapped lane 2 is present
    CHECK(fsm.compute_scoped_runout_value(tools_used, remap) == 1);

    // Sanity: without the remap, identity lane 0 is empty -> would warn.
    auto empty_identity = fsm.find_empty_required_lanes(tools_used, {});
    REQUIRE(empty_identity.size() == 1);
    CHECK(empty_identity[0].second == 0);
}

TEST_CASE("scoped runout: no used tools -> no opinion (badge hidden)",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);
    backend.set_lane_presence({false, false, false, false});
    drain();

    // Empty tools_used -> nothing to scope -> -1 (hidden), never warns.
    CHECK(fsm.find_empty_required_lanes({}, {}).empty());
    CHECK(fsm.compute_scoped_runout_value({}, {}) == -1);
}

TEST_CASE("scoped runout: master disabled -> badge muted (2), no warning",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);
    backend.set_lane_presence({false, false, false, false});
    fsm.set_master_enabled(false);
    drain();

    std::set<int> tools_used{0};
    // Master off: runout protection inactive -> badge muted, never warns.
    CHECK(fsm.compute_scoped_runout_value(tools_used, {}) == 2);
    CHECK(fsm.find_empty_required_lanes(tools_used, {}).empty());
}

TEST_CASE("scoped runout: no AMS backend -> falls back to aggregate runout (unchanged)",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    AmsState::instance().set_backend(nullptr);
    auto& fsm = FilamentSensorManager::instance();

    // Plain single-extruder runout sensor, no lane mapping, no backend.
    RunoutScopeTestAccess::reset(fsm);
    fsm.set_master_enabled(true);
    fsm.discover_sensors({"filament_switch_sensor fsensor"});
    fsm.set_sensor_role("filament_switch_sensor fsensor", FilamentSensorRole::RUNOUT);
    fsm.set_sensor_enabled("filament_switch_sensor fsensor", true);

    // With no backend, find_empty_required_lanes returns empty (no lane truth to
    // consult) — the print-start controller falls back to the aggregate sensor
    // check, which is the unchanged non-AMS path. The badge value mirrors the
    // unscoped runout subject in that case.
    fsm.update_from_status(
        json{{"filament_switch_sensor fsensor", json{{"filament_detected", false}}}});
    drain();
    CHECK(fsm.find_empty_required_lanes({0}, {}).empty());
    // No backend -> compute_scoped_runout_value defers to the aggregate role value (0 = runout).
    CHECK(fsm.compute_scoped_runout_value({0}, {}) == 0);

    fsm.update_from_status(
        json{{"filament_switch_sensor fsensor", json{{"filament_detected", true}}}});
    drain();
    CHECK(fsm.compute_scoped_runout_value({0}, {}) == 1);
}

// ============================================================================
// Review fixes — issues 6, 8, 5/7 (remap consistency, multi-backend scope)
// ============================================================================

TEST_CASE("scoped runout (issue 6): unresolvable/UNKNOWN slot is NOT flagged empty",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Lanes 0-3 all present. Tool 0 is remapped to slot 99 (no such slot ->
    // get_slot_info returns slot_index=-1, status=UNKNOWN). An UNKNOWN slot must
    // NOT be treated as a genuinely-empty lane.
    backend.set_lane_presence({true, true, true, true});
    drain();
    REQUIRE(AmsState::instance().get_backend()->get_slot_info(99).slot_index < 0);

    std::map<int, int> remap{{0, 99}};
    auto empty = fsm.find_empty_required_lanes({0}, remap);
    CHECK(empty.empty()); // unresolvable slot skipped, not flagged
    // No genuinely-empty required lane -> "loaded" (1), not "runout" (0).
    CHECK(fsm.compute_scoped_runout_value({0}, remap) == 1);
}

TEST_CASE(
    "scoped runout (issue 6): out-of-range tool default head resolves, real empty still flags",
    "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Lane 0 empty, others present. Tool 9 has no identity head (clamps to head
    // 0), which IS a resolvable, genuinely-empty lane -> flagged.
    backend.set_lane_presence({false, true, true, true});
    drain();
    auto empty = fsm.find_empty_required_lanes({9}, {});
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].first == 9);
    CHECK(empty[0].second == 0); // clamped default head
}

TEST_CASE("scoped runout (issue 8): no fresh status -> no warning, badge hidden",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();

    // Sensors discovered + RUNOUT-roled, but NO status fed yet (available=false).
    setup_four_lane_sensors_no_status(fsm);
    // Backend reports lane 0 empty (stale filament_exist before any status).
    backend.set_lane_presence({false, true, true, true});
    drain();

    // Freshness gate: without fresh sensor data, do NOT manufacture a warning.
    CHECK(fsm.find_empty_required_lanes({0}, {}).empty());
    // Badge: no opinion (hidden) rather than a premature red.
    CHECK(fsm.compute_scoped_runout_value({0}, {}) == -1);

    // Once fresh status arrives (lane 0 sensor reports, marking available), the
    // empty lane 0 is now actionable.
    fsm.update_from_status(make_sensor_status(0, false));
    drain();
    auto empty = fsm.find_empty_required_lanes({0}, {});
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].second == 0);
    CHECK(fsm.compute_scoped_runout_value({0}, {}) == 0);
}

TEST_CASE("scoped runout (issue 5): badge and warning agree under an applied remap",
          "[runout][scope][print-start]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    // Tool 0's identity lane (0) is EMPTY; lane 3 is present. The applied map
    // routes tool 0 -> slot 3. Both the warning path (find_empty_required_lanes)
    // and the badge path (compute_scoped_runout_value) must use the SAME map and
    // therefore agree: no warning, badge loaded.
    backend.set_lane_presence({false, true, true, true});
    drain();

    std::map<int, int> applied{{0, 3}};
    auto empty_applied = fsm.find_empty_required_lanes({0}, applied);
    CHECK(empty_applied.empty());
    CHECK(fsm.compute_scoped_runout_value({0}, applied) == 1);

    // Identity (no remap) would disagree — lane 0 empty -> warn + red. This is
    // exactly the divergence issue 5 fixes by feeding the applied map to both.
    auto empty_identity = fsm.find_empty_required_lanes({0}, {});
    REQUIRE(empty_identity.size() == 1);
    CHECK(fsm.compute_scoped_runout_value({0}, {}) == 0);
}

TEST_CASE("scoped runout (issue 2): badge VALUE reacts to AMS lane-presence change",
          "[runout][scope][print-status]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    std::set<int> tools_used{0};

    // Lane 0 present -> badge "loaded" (1). No motion-sensor edge happens here;
    // only the AMS lane presence will change. The slots_version observer calls
    // compute_scoped_runout_value, so the recomputed value is what the badge
    // would show.
    backend.set_lane_presence({true, true, true, true});
    drain();
    CHECK(fsm.compute_scoped_runout_value(tools_used, {}) == 1);

    // Lane 0 empties mid-print via an AMS lane-presence change ONLY (no
    // filament_runout_detected edge). The recomputed badge value must follow the
    // lane truth -> "runout" (0). This is the value the slots_version-driven
    // recompute publishes (issue 2).
    backend.set_lane_presence({false, true, true, true});
    drain();
    CHECK(fsm.compute_scoped_runout_value(tools_used, {}) == 0);
}

// PrintStatusPanel::recompute_scoped_runout()'s two halves, both shipped:
// helix::print_scopes_runout_badge() decides whether the print has an opinion at
// all (issue 9 — a terminal state forces the badge hidden regardless of lane
// truth, so a finished print never leaves a red badge behind when tools_used has
// not cleared), and FilamentSensorManager::compute_scoped_runout_value()
// produces the value when it does.
//
// The panel itself is not driven here because its remaining input,
// get_tools_used(), reads the parsed file off a laid-out gcode viewer widget;
// with no viewer it returns {} and every state would answer -1, so the guard
// under test would never be reached.
namespace {
int scoped_runout_for_state(FilamentSensorManager& fsm, PrintJobState state,
                            const std::set<int>& tools_used,
                            const std::map<int, int>& applied_remap) {
    if (!helix::print_scopes_runout_badge(state)) {
        return -1;
    }
    return fsm.compute_scoped_runout_value(tools_used, applied_remap);
}
} // namespace

TEST_CASE("scoped runout (issue 9): print-end forces badge hidden even with empty lane",
          "[runout][scope][print-status]") {
    HelixTestFixture fx;
    ScopedSnapmakerBackend backend;
    auto& fsm = FilamentSensorManager::instance();
    setup_four_lane_sensors(fsm);

    std::set<int> tools_used{0};
    backend.set_lane_presence({false, true, true, true}); // lane 0 empty
    drain();

    // While PRINTING, the empty required lane shows red (0).
    CHECK(scoped_runout_for_state(fsm, PrintJobState::PRINTING, tools_used, {}) == 0);

    // On terminal states the badge is forced hidden (-1), even though tools_used
    // is non-empty and lane 0 is still reported empty.
    CHECK(scoped_runout_for_state(fsm, PrintJobState::COMPLETE, tools_used, {}) == -1);
    CHECK(scoped_runout_for_state(fsm, PrintJobState::CANCELLED, tools_used, {}) == -1);
    CHECK(scoped_runout_for_state(fsm, PrintJobState::ERROR, tools_used, {}) == -1);
    CHECK(scoped_runout_for_state(fsm, PrintJobState::STANDBY, tools_used, {}) == -1);

    // PAUSED is an active print -> still reflects lane truth (red).
    CHECK(scoped_runout_for_state(fsm, PrintJobState::PAUSED, tools_used, {}) == 0);
}
