// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "spoolman_types.h" // SpoolInfo + apply_spool_to_slot (the picker-side writer)

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;

using json = nlohmann::json;

/// This file had no fixture at all, so nothing ever drained the UpdateQueue.
/// handle_status_update() publishes first-gate port presence through
/// AmsState::set_active_tool_port_present, which marshals to the main thread via
/// AsyncLifetimeGuard::defer — the single largest named producer in the
/// cross-test leak report (prestonbrown/helixscreen#1169). The drain sits in the
/// derived destructor body so it runs while AmsState's subjects and the test's
/// backend are still alive, before HelixTestFixture's own teardown.
struct SnapmakerFixture : public HelixTestFixture {
    ~SnapmakerFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

// Friend-class shim for FilamentSlotOverrideStore — same idiom as IFS tests
// and test_filament_slot_override_store.cpp. Lets us redirect the store's
// on-disk read-cache to a per-test tmp dir so save_async doesn't pollute
// the developer's real helixscreen config.
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

// Friend-class shim for AmsBackendSnapmaker — declared as friend in the
// backend header. Provides narrow, purpose-built accessors for the private
// override and hardware-event-detection state so tests don't have to reach
// into the backend via public APIs (which layer apply_overrides on top and
// obscure what the internal maps actually hold).
class SnapmakerTestAccess {
  public:
    static void handle_status(AmsBackendSnapmaker& b, const json& n) {
        b.handle_status_update(n);
    }
    static void seed_override(AmsBackendSnapmaker& b, int slot_index,
                              const helix::ams::FilamentSlotOverride& ovr) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.overrides_[slot_index] = ovr;
    }
    static std::optional<helix::ams::FilamentSlotOverride>
    get_override(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        auto it = b.overrides_.find(slot_index);
        if (it == b.overrides_.end())
            return std::nullopt;
        return it->second;
    }
    static void inject_override_store(AmsBackendSnapmaker& b,
                                      std::unique_ptr<helix::ams::FilamentSlotOverrideStore> s) {
        b.override_store_ = std::move(s);
    }
    static std::optional<std::string> last_rfid_uid(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.rfid_tracker_.baseline(slot_index);
    }
    static void set_sensor_present(AmsBackendSnapmaker& b, int slot_index, bool present) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.sensor_filament_present_[slot_index] = present;
    }
    static void set_port_sensor_present(AmsBackendSnapmaker& b, int slot_index, bool present) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.port_sensor_filament_present_[slot_index] = present;
    }
    static void set_current_slot(AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_slot = slot_index;
    }
    // The channel_state-driven "loaded at toolhead" latch (the core fix). Read
    // directly so tests can assert the latch independently of the query methods
    // that consume it.
    static bool loaded_at_toolhead(const AmsBackendSnapmaker& b, int slot_index) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.loaded_at_toolhead_[slot_index];
    }
    static void set_loaded_at_toolhead(AmsBackendSnapmaker& b, int slot_index, bool loaded) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.loaded_at_toolhead_[slot_index] = loaded;
    }
    static void set_current_tool(AmsBackendSnapmaker& b, int tool) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.current_tool = tool;
    }
};

// Build a filament_feed status notification for a single channel/extruder with
// a given channel_state (+ optional filament_detected / channel_error). Routes
// extruders 0,1 to "filament_feed left" and 2,3 to "filament_feed right" to
// mirror the U1 feed topology, though the backend reads either object for any
// extruder key. Shared by the channel_state coverage tests below.
static json make_feed_status(int extruder_idx, const std::string& channel_state,
                             bool filament_detected = true,
                             const std::string& channel_error = "ok") {
    const char* feed_key = (extruder_idx <= 1) ? "filament_feed left" : "filament_feed right";
    std::string ext_key = "extruder" + std::to_string(extruder_idx);
    return json{{feed_key, json{{ext_key, json{{"filament_detected", filament_detected},
                                               {"channel_state", channel_state},
                                               {"channel_error", channel_error}}}}}};
}

namespace {
// Per-test tmp cache dir — same idiom as test_ams_backend_ad5x_ifs.cpp.
struct SnapmakerTmpCacheDir {
    std::filesystem::path path;
    explicit SnapmakerTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("snapmaker_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~SnapmakerTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Build a filament_detect status notification for a single slot with
// configurable material, color, brand, and CARD_UID. Slot index is 0-based;
// the notification's info array is padded to NUM_TOOLS=4 with "NONE" entries.
json make_filament_detect_status(int slot_index, const std::string& main_type, uint32_t argb_color,
                                 const std::string& manufacturer, const json& card_uid) {
    json info_arr = json::array();
    for (int i = 0; i < 4; ++i) {
        if (i == slot_index) {
            info_arr.push_back(json{{"MAIN_TYPE", main_type},
                                    {"SUB_TYPE", "Basic"},
                                    {"MANUFACTURER", manufacturer},
                                    {"VENDOR", "Snapmaker"},
                                    {"ARGB_COLOR", argb_color},
                                    {"HOTEND_MIN_TEMP", 190},
                                    {"HOTEND_MAX_TEMP", 220},
                                    {"BED_TEMP", 60},
                                    {"WEIGHT", 1000},
                                    {"CARD_UID", card_uid}});
        } else {
            info_arr.push_back(json{{"MAIN_TYPE", "NONE"}});
        }
    }
    return json{{"filament_detect", json{{"info", info_arr}}}};
}

// Captures execute_gcode() so the load/unload command path can be asserted
// without a live Moonraker connection — same idiom as test_ams_backend_afc.cpp.
class CapturingSnapmakerBackend : public AmsBackendSnapmaker {
  public:
    // running_ has to be set: the filament ops gate on check_preconditions(),
    // which answers not_connected on a backend that never started. api_ stays
    // null, so the print-active half of that gate passes (unknown print state is
    // not blocked) and these tests keep asserting the command, not the gate —
    // the gate itself is covered in test_ams_paused_filament_ops.cpp.
    CapturingSnapmakerBackend() : AmsBackendSnapmaker(nullptr, nullptr) {
        running_.store(true);
    }

    std::vector<std::string> captured_gcodes;

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // current_slot is protected on the subscription base; expose a setter so
    // tests can stand in for "filament currently loaded in slot N".
    void set_loaded_slot(int slot) {
        system_info_.current_slot = slot;
    }
};
} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker type enum", "[ams][snapmaker]") {
    SECTION("SNAPMAKER is a valid AmsType") {
        auto t = AmsType::SNAPMAKER;
        REQUIRE(t != AmsType::NONE);
        REQUIRE(static_cast<int>(t) == 7);
    }

    SECTION("SNAPMAKER is both a tool changer and filament system") {
        REQUIRE(is_tool_changer(AmsType::SNAPMAKER));
        REQUIRE(is_filament_system(AmsType::SNAPMAKER));
    }

    SECTION("ams_type_to_string returns Snapmaker") {
        REQUIRE(std::string(ams_type_to_string(AmsType::SNAPMAKER)) == "Snapmaker");
    }

    SECTION("ams_type_from_string parses Snapmaker variants") {
        REQUIRE(ams_type_from_string("snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("Snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("snapswap") == AmsType::SNAPMAKER);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker detection via filament_detect", "[ams][snapmaker]") {
    PrinterDiscovery discovery;

    SECTION("filament_detect triggers SNAPMAKER detection") {
        nlohmann::json objects = nlohmann::json::array(
            {"extruder", "extruder1", "extruder2", "extruder3", "toolchanger", "filament_detect",
             "toolhead", "heater_bed", "print_task_config"});
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_snapmaker());
        REQUIRE(discovery.mmu_type() == AmsType::SNAPMAKER);
    }

    SECTION("empty toolchanger without filament_detect is not SNAPMAKER") {
        nlohmann::json objects =
            nlohmann::json::array({"extruder", "toolchanger", "tool T0", "tool T1", "toolhead"});
        discovery.parse_objects(objects);
        REQUIRE_FALSE(discovery.has_snapmaker());
        REQUIRE(discovery.has_tool_changer());
    }
}

// ============================================================================
// Backend Construction Tests
// ============================================================================

#include "ams_backend_snapmaker.h"

#include "hv/json.hpp"

TEST_CASE_METHOD(SnapmakerFixture, "AmsBackendSnapmaker construction", "[ams][snapmaker]") {
    SECTION("type returns SNAPMAKER") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_type() == AmsType::SNAPMAKER);
    }

    SECTION("topology is PARALLEL") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_topology() == PathTopology::PARALLEL);
    }

    SECTION("name is Snapmaker SnapSwap") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.type_name == "Snapmaker SnapSwap");
    }

    SECTION("has 4 slots in 1 unit") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.total_slots == 4);
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].slot_count == 4);
    }

    SECTION("tool_to_slot_map is 1:1 identity (gates 2D toolpath colors)") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.tool_to_slot_map.size() == 4);
        REQUIRE(info.tool_to_slot_map[0] == 0);
        REQUIRE(info.tool_to_slot_map[1] == 1);
        REQUIRE(info.tool_to_slot_map[2] == 2);
        REQUIRE(info.tool_to_slot_map[3] == 3);
    }

    SECTION("tip_method is NONE (U1 has no cutter; unload is heat + retract)") {
        // Drives the unload/swap stepper to omit the "Cut & retract" tip step.
        // Default TipMethod is CUT; the backend must override it to NONE.
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.tip_method == TipMethod::NONE);
    }
}

// ============================================================================
// Operation Step Model Tests
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker get_operation_step_model is the per-direction firmware sequence",
                 "[ams][snapmaker][stepmodel]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SECTION("LOAD is a 5-step model ending in Purge with a live-temp Heat step") {
        auto model = backend.get_operation_step_model(StepOperationType::LOAD_FRESH);
        REQUIRE(model.steps.size() == 5);
        CHECK(std::string(model.steps[0].label) == "Home");
        CHECK(std::string(model.steps[1].label) == "Select");
        CHECK(std::string(model.steps[2].label) == "Heat nozzle");
        CHECK(std::string(model.steps[3].label) == "Feed filament");
        CHECK(std::string(model.steps[4].label) == "Purge");
        // phase_id mirrors the firmware operation_phase index the classifier emits.
        CHECK(model.steps[0].phase_id == 0);
        CHECK(model.steps[1].phase_id == 1);
        CHECK(model.steps[2].phase_id == 2);
        CHECK(model.steps[3].phase_id == 3);
        CHECK(model.steps[4].phase_id == 4);
        // Only the Heat step shows a live nozzle temperature.
        CHECK(model.steps[2].live_temp);
        CHECK_FALSE(model.steps[0].live_temp);
        CHECK_FALSE(model.steps[3].live_temp);
        CHECK_FALSE(model.steps[4].live_temp);
    }

    SECTION("LOAD_SWAP also produces the 5-step load-direction model") {
        auto model = backend.get_operation_step_model(StepOperationType::LOAD_SWAP);
        REQUIRE(model.steps.size() == 5);
        CHECK(std::string(model.steps[3].label) == "Feed filament");
        CHECK(std::string(model.steps[4].label) == "Purge");
    }

    SECTION("UNLOAD is a 4-step model ending in Retract") {
        auto model = backend.get_operation_step_model(StepOperationType::UNLOAD);
        REQUIRE(model.steps.size() == 4);
        CHECK(std::string(model.steps[0].label) == "Home");
        CHECK(std::string(model.steps[1].label) == "Select");
        CHECK(std::string(model.steps[2].label) == "Heat nozzle");
        CHECK(std::string(model.steps[3].label) == "Retract");
        CHECK(model.steps[0].phase_id == 0);
        CHECK(model.steps[3].phase_id == 3);
        CHECK(model.steps[2].live_temp);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker get_operation_step_index_subject is the firmware phase subject",
                 "[ams][snapmaker][stepmodel]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    // The U1 drives the current step from the firmware Home/Select/Heat/Move
    // phase, surfaced via AmsState's operation_phase subject (static singleton).
    for (auto op :
         {StepOperationType::LOAD_FRESH, StepOperationType::LOAD_SWAP, StepOperationType::UNLOAD}) {
        CHECK(backend.get_operation_step_index_subject(op) ==
              AmsState::instance().get_ams_operation_phase_subject());
    }
}

// ============================================================================
// Filament Operation Command Tests
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker load routes through AUTO_FEEDING LOAD=1",
                 "[ams][snapmaker][load]") {
    // The mirror of the unload contract below, and previously unasserted — the
    // gap that let a UI-layer change silently swap this command for `T{n}`.
    //
    // FEED_AUTO silent-returns when passed no LOAD/UNLOAD parameter, and `T{n}`
    // only seats the carriage without feeding, so both wrong answers look like
    // "the button did nothing". See load_filament()'s trail-of-bad-guesses note.
    SECTION("explicit slot emits AUTO_FEEDING EXTRUDER=n LOAD=1") {
        CapturingSnapmakerBackend backend;
        auto err = backend.load_filament(2);
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=2 LOAD=1");
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker never routes a load through a tool change",
                 "[ams][snapmaker][load][dispatch]") {
    // PARALLEL topology: four toolheads, each with its own extruder, sharing no
    // path to a nozzle. The base needs_unload_before_load() rule is SERIAL —
    // "clear the shared path first" — and is true here essentially always,
    // because current_slot is assigned from toolhead.extruder and a tool is
    // always picked up.
    //
    // Answering true routes a load through change_tool() -> `T{n}`, which seats
    // the carriage and feeds nothing. That is what AmsOperationSidebar has been
    // dispatching, and what plan_load() would make the filament panel dispatch
    // too. AUTO_FEEDING already targets an arbitrary extruder directly.
    CapturingSnapmakerBackend backend;

    AmsSystemInfo seated = backend.get_system_info();
    seated.filament_loaded = true;
    seated.current_slot = 0;
    CHECK_FALSE(backend.needs_unload_before_load(seated, /*target_slot=*/1));

    // And the command that would have been sent instead is the known-bad one,
    // so this stays a real distinction rather than two spellings of one thing.
    backend.captured_gcodes.clear();
    REQUIRE(backend.change_tool(1).success());
    REQUIRE(backend.captured_gcodes.size() == 1);
    CHECK(backend.captured_gcodes[0] == "T1");
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker unload routes through AUTO_FEEDING UNLOAD=1",
                 "[ams][snapmaker][unload]") {
    // INNER_FILAMENT_UNLOAD is the leaf macro the firmware's feed state machine
    // calls internally; emitting it directly skips the state transitions that
    // aftermarket feeders (e.g. the U1-Ace ACE Pro mod) hook to retract their
    // spool. Unload must mirror load and go through AUTO_FEEDING so the channel
    // reaches "unload_finish". See prestonbrown/helixscreen#974.

    SECTION("explicit slot emits AUTO_FEEDING EXTRUDER=n UNLOAD=1") {
        CapturingSnapmakerBackend backend;
        auto err = backend.unload_filament(2);
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=2 UNLOAD=1");
    }

    SECTION("unload_active_filament dispatches the loaded slot's extruder") {
        // Contract: unload_active_filament() reads current_slot from system_info_
        // (single source of truth in the base class) and forwards it. This is
        // the path the Filament panel's Unload button takes.
        CapturingSnapmakerBackend backend;
        backend.set_loaded_slot(1);
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=1 UNLOAD=1");
    }

    SECTION("unload_active_filament with T3 loaded dispatches EXTRUDER=3 (U1 field bug)") {
        // Regression for the Snapmaker U1 Filament-panel-unload wrong-tool bug
        // (Discord report from Bart, 2026-07-20): with T3 loaded, hitting Unload
        // on the Filament panel sent EXTRUDER=0 (stale current_slot) and the
        // firmware dutifully visited T0 first. The fix routes through
        // unload_active_filament(), which resolves current_slot ONCE in the base
        // class — same snapshot the UI's "is anything loaded?" guard used — so
        // the two can never diverge.
        CapturingSnapmakerBackend backend;
        backend.set_loaded_slot(3);
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "AUTO_FEEDING EXTRUDER=3 UNLOAD=1");
        // Explicit guard against the bug returning: must NOT touch T0.
        REQUIRE(backend.captured_gcodes[0].find("EXTRUDER=0") == std::string::npos);
    }

    SECTION("unload_active_filament with no active slot falls back to bare INNER_FILAMENT_UNLOAD") {
        // current_slot = -1 (no tool picked up). The base helper forwards -1 to
        // the backend override, which keeps its "trust the firmware" behavior
        // (bare leaf macro, no AUTO_FEEDING state machine).
        CapturingSnapmakerBackend backend; // current_slot defaults to -1
        auto err = backend.unload_active_filament();
        REQUIRE(err.success());
        REQUIRE(backend.captured_gcodes.size() == 1);
        REQUIRE(backend.captured_gcodes[0] == "INNER_FILAMENT_UNLOAD");
    }
}

// can_unload_from_toolhead — the U1 is a 4-toolhead machine; every tool that
// holds filament must offer per-slot Unload, not just the single active one.
// The base AmsBackend gates Unload on SlotStatus::LOADED (one active tool),
// which hid the per-slot Unload action for toolheads 2/3/4 and forced users
// onto the sidebar's active-slot Unload button — which always unloads toolhead
// 0. (U1 multi-toolhead field report.)
TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker can_unload_from_toolhead offers unload for every loaded toolhead",
                 "[ams][snapmaker][unload]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Slot 0 active (LOADED), slots 1 & 3 hold filament (AVAILABLE), slot 2 empty.
    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}}, // active tool = slot 0
        {"print_task_config", json{{"filament_exist", json::array({true, true, false, true})}}}};
    SnapmakerTestAccess::handle_status(backend, status);

    // Loaded state is keyed off channel_state now: slots 0/1/3 report load_finish
    // (filament at the toolhead), slot 2 reports wait_insert (empty channel).
    json feed = json{
        {"filament_feed left",
         json{{"extruder0", json{{"filament_detected", true}, {"channel_state", "load_finish"}}},
              {"extruder1", json{{"filament_detected", true}, {"channel_state", "load_finish"}}}}},
        {"filament_feed right",
         json{{"extruder2", json{{"filament_detected", false}, {"channel_state", "wait_insert"}}},
              {"extruder3", json{{"filament_detected", true}, {"channel_state", "load_finish"}}}}}};
    SnapmakerTestAccess::handle_status(backend, feed);

    // Sanity: the status drove the slot states the gate keys on.
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::LOADED);
    REQUIRE(backend.get_slot_info(1).status == SlotStatus::AVAILABLE);
    REQUIRE(backend.get_slot_info(2).status == SlotStatus::EMPTY);
    REQUIRE(backend.get_slot_info(3).status == SlotStatus::AVAILABLE);

    SECTION("active (LOADED) toolhead is unloadable") {
        CHECK(backend.can_unload_from_toolhead(0));
    }
    SECTION("non-active toolheads holding filament are unloadable (the fix)") {
        CHECK(backend.can_unload_from_toolhead(1));
        CHECK(backend.can_unload_from_toolhead(3));
    }
    SECTION("empty toolhead offers no unload") {
        CHECK_FALSE(backend.can_unload_from_toolhead(2));
    }
}

// can_unload_from_toolhead must require filament AT the toolhead, not merely
// present in the buffer. After an unload the U1 retracts filament to the buffer:
// filament_exist stays true (slot AVAILABLE) and — the firmware bug this fix
// targets — the motion sensor e{N}_filament even stays true, but channel_state
// reports unload_finish. Keying off the channel_state latch (not is_present or
// the motion sensor) stops offering Unload for an already-unloaded tool.
TEST_CASE_METHOD(
    SnapmakerFixture,
    "Snapmaker can_unload_from_toolhead requires filament at the toolhead, not just buffer",
    "[ams][snapmaker][unload]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // All four tools loaded to their toolhead (channel_state load_finish).
    json status = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"print_task_config", json{{"filament_exist", json::array({true, true, true, true})}}}};
    SnapmakerTestAccess::handle_status(backend, status);
    for (int e = 0; e < 4; ++e) {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(e, "load_finish"));
    }

    // Every loaded tool is unloadable.
    REQUIRE(backend.can_unload_from_toolhead(2));

    SECTION("a tool unloaded to the buffer (channel_state unload_finish) is NOT unloadable") {
        // Post-unload: filament parked in the buffer (still detected=true, the
        // firmware bug), channel_state unload_finish → the loaded latch clears.
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "unload_finish", true));
        CHECK_FALSE(backend.can_unload_from_toolhead(2));
        // Tools still loaded at their toolhead remain unloadable.
        CHECK(backend.can_unload_from_toolhead(1));
        CHECK(backend.can_unload_from_toolhead(3));
    }
}

// channel_error scoping — the firmware reports channel_error="no_filament" for
// any lane sitting empty, INCLUDING a lane deliberately left unloaded for a
// multi-color print (heads 0+2 used, head 1 empty). Post-print that idle empty
// lane must NOT latch the whole backend into action=Error and pop a spurious
// AMS loading-error modal. A genuine load FAILURE (during/after a load attempt,
// or on a present/active lane) MUST still raise Error. (Snapmaker U1 false-alarm.)
TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker channel_error on idle empty lane does NOT raise Error",
                 "[ams][snapmaker]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Heads 0 and 2 present, head 1 left empty. Head 1 reports the firmware's
    // idle-empty token while sitting idle (channel_state "wait_insert" = no
    // filament in channel), not mid-load.
    json status =
        json{{"filament_feed left", json{{"extruder0", json{{"filament_detected", true},
                                                            {"channel_state", "inited"},
                                                            {"channel_error", "ok"}}},
                                         {"extruder1", json{{"filament_detected", false},
                                                            {"channel_state", "wait_insert"},
                                                            {"channel_error", "no_filament"}}},
                                         {"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "inited"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, status);

    // Lane 1 is empty, idle, and not the active lane → no error.
    REQUIRE(backend.get_slot_info(1).status == SlotStatus::EMPTY);
    CHECK(backend.get_system_info().action != AmsAction::ERROR);
    CHECK(backend.get_system_info().operation_detail.empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker channel_error during an active load DOES raise Error",
                 "[ams][snapmaker]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // A real load FAILURE: lane 1 is mid-load (channel_state "load_heating") when
    // the firmware reports an error. This must surface as Error so the user is told.
    json status =
        json{{"filament_feed left", json{{"extruder1", json{{"filament_detected", false},
                                                            {"channel_state", "load_heating"},
                                                            {"channel_error", "no_filament"}}}}}};
    SnapmakerTestAccess::handle_status(backend, status);

    CHECK(backend.get_system_info().action == AmsAction::ERROR);
    // Raw firmware token mapped to a friendly, lane-numbered message.
    CHECK(backend.get_system_info().operation_detail.find("No filament in lane 2") !=
          std::string::npos);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker channel_error on the ACTIVE lane DOES raise Error",
                 "[ams][snapmaker]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Make lane 0 the active/current lane, then have it report an error while
    // idle. An error on the in-use lane is a genuine fault, not a quiet empty
    // lane — it must alarm.
    SnapmakerTestAccess::set_current_slot(backend, 0);
    json status =
        json{{"filament_feed left", json{{"extruder0", json{{"filament_detected", false},
                                                            {"channel_state", "inited"},
                                                            {"channel_error", "some_fault"}}}}}};
    SnapmakerTestAccess::handle_status(backend, status);

    CHECK(backend.get_system_info().action == AmsAction::ERROR);
    // Unknown token falls through unmapped so we never hide a novel error.
    CHECK(backend.get_system_info().operation_detail == "some_fault");
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker granular unload sub-states drive UNLOADING action",
                 "[ams][snapmaker][unload]") {
    // The U1 firmware NEVER sends a flat "unloading" — it emits granular
    // sub-states unload_homing/picking/heating/doing. Each must set action to
    // UNLOADING so the on-screen step bar stays visible during the unload.
    for (const char* state :
         {"unload_homing", "unload_picking", "unload_heating", "unload_doing"}) {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        json status =
            json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                                {"channel_state", state},
                                                                {"channel_error", "ok"}}}}}};
        SnapmakerTestAccess::handle_status(backend, status);
        INFO("state=" << state);
        CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker granular load sub-states drive LOADING action",
                 "[ams][snapmaker][load]") {
    // Symmetric to the unload case: every granular load sub-state → LOADING.
    for (const char* state : {"load_prepare", "load_homing", "load_picking", "load_heating",
                              "load_feeding", "load_extruding", "load_flushing"}) {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        json status =
            json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                                {"channel_state", state},
                                                                {"channel_error", "ok"}}}}}};
        SnapmakerTestAccess::handle_status(backend, status);
        INFO("state=" << state);
        CHECK(backend.get_system_info().action == AmsAction::LOADING);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker channel_state maps to granular operation_phase",
                 "[ams][snapmaker][phase]") {
    // The U1 firmware emits four sequential sub-phases per direction
    // (<load|unload>_<homing|picking|heating|doing>), each many seconds long.
    // operation_phase mirrors these into a 0..3 index that drives the sidebar's
    // 4-step bar (0=Home, 1=Select, 2=Heat, 3=Move). All non-active states
    // (finish/idle/preload_finish) collapse to -1 = "no active step".
    struct Case {
        const char* channel_state;
        int expected_phase;
    };
    const Case cases[] = {
        {"unload_homing", 0},  {"unload_picking", 1}, {"unload_heating", 2},  {"unload_doing", 3},
        {"load_homing", 0},    {"load_picking", 1},   {"load_heating", 2},    {"load_feeding", 3},
        {"unload_finish", -1}, {"load_finish", -1},   {"preload_finish", -1}, {"inited", -1},
    };

    for (const auto& c : cases) {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        // A loaded lane so unload_finish doesn't early-return before the phase
        // is parsed; filament_detected=true keeps the lane present.
        json status =
            json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                                {"channel_state", c.channel_state},
                                                                {"channel_error", "ok"}}}}}};
        SnapmakerTestAccess::handle_status(backend, status);
        INFO("channel_state=" << c.channel_state);
        CHECK(backend.get_system_info().operation_phase == c.expected_phase);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker unload_finish resolves UNLOADING to IDLE",
                 "[ams][snapmaker][unload]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Mid-unload: action becomes UNLOADING.
    json doing =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_doing"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, doing);
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    // unload_finish is the TRUE end of the operation → IDLE.
    json finish =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_finish"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, finish);
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker preload_finish is NOT matched by the load_ prefix branch",
                 "[ams][snapmaker][unload]") {
    // preload_finish starts with "preload_", NOT "load_" — it must reach the
    // existing preload_finish handler (which leaves the action alone) rather than
    // being misread as a granular load sub-state.
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Start mid-unload so there is an action to (incorrectly) clobber.
    json doing =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "unload_heating"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, doing);
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    json preload =
        json{{"filament_feed left", json{{"extruder2", json{{"filament_detected", true},
                                                            {"channel_state", "preload_finish"},
                                                            {"channel_error", "ok"}}}}}};
    SnapmakerTestAccess::handle_status(backend, preload);
    // preload_finish leaves the in-progress action alone (does NOT become LOADING
    // and does NOT resolve to IDLE — only unload_finish/load_finish/idle do).
    CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
}

// get_slot_filament_segment — on the U1's PARALLEL multi-toolhead topology
// every tool loaded to its own dedicated nozzle renders all the way into the
// toolhead (NOZZLE); multiple tools can be loaded at once, each to its own
// nozzle. "Loaded to the nozzle" is the channel_state latch (load_finish), NOT
// the motion sensor — the sensor lingers present after an unload, which used to
// leave unloaded lanes rendering as fully loaded. A tool that is present in the
// buffer but not loaded renders OUTPUT (line to the toolhead entry dot, hollow);
// an empty lane renders nothing.
TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker get_slot_filament_segment renders NOZZLE for every loaded tool",
                 "[ams][snapmaker][path]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SECTION("active loaded tool renders into the toolhead") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "load_finish"));
        CHECK(backend.get_slot_filament_segment(0) == PathSegment::NOZZLE);
    }
    SECTION("multiple non-active loaded tools each render into their own toolhead") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_finish"));
        SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "load_finish"));
        CHECK(backend.get_slot_filament_segment(1) == PathSegment::NOZZLE);
        CHECK(backend.get_slot_filament_segment(3) == PathSegment::NOZZLE);
    }
    SECTION("empty tool renders no filament line") {
        SnapmakerTestAccess::set_sensor_present(backend, 2, false);
        SnapmakerTestAccess::set_port_sensor_present(backend, 2, false);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "wait_insert",
                                                                     /*filament_detected=*/false));
        CHECK(backend.get_slot_filament_segment(2) == PathSegment::NONE);
    }
    SECTION("a tool with neither sensor present (real runout) renders no line") {
        // Not loaded (latch defaults false), and both the toolhead motion sensor
        // AND the buffer/port sensor read empty → genuine runout / no filament.
        SnapmakerTestAccess::set_sensor_present(backend, 1, false);
        SnapmakerTestAccess::set_port_sensor_present(backend, 1, false);
        CHECK(backend.get_slot_filament_segment(1) == PathSegment::NONE);
    }
    SECTION("filament staged in the bowden (not loaded, port present) renders to the dot") {
        // U1 post-unload state: filament retracted out of the toolhead but left
        // in the feed tube. Latch clear (unload_finish), toolhead motion sensor
        // reads empty, buffer/port sensor still present → draw the line down to
        // the toolhead entry sensor but no farther (OUTPUT), not nothing.
        SnapmakerTestAccess::set_sensor_present(backend, 1, false);
        SnapmakerTestAccess::set_port_sensor_present(backend, 1, true);
        CHECK(backend.get_slot_filament_segment(1) == PathSegment::OUTPUT);
    }
}

// ============================================================================
// channel_state "loaded at toolhead" latch (the core fix)
// ============================================================================
// The per-tool motion sensor (e{N}_filament) does NOT drop to false after an
// unload on current firmware — a freshly-unloaded lane still reads
// filament_detected=true. The authoritative load signal is
// filament_feed.channel_state: load_finish means loaded, unload_finish /
// wait_insert / preload_finish mean not-loaded. The backend derives a per-slot
// latch from those transitions and the toolhead-load queries key off it.

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker load_finish latches loaded + offers unload",
                 "[ams][snapmaker][channel_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "load_finish"));

    CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
    CHECK(backend.slot_has_filament_at_toolhead(2));
    CHECK(backend.can_unload_from_toolhead(2));
}

TEST_CASE_METHOD(
    SnapmakerFixture,
    "Snapmaker unload_finish clears the loaded latch even while motion sensor stays true",
    "[ams][snapmaker][channel_state]") {
    // The exact live-captured condition (U1 firmware 20260608, lanes 3&4 just
    // unloaded): channel_state=unload_finish but the motion sensor still reads
    // filament_detected=true. Before the fix the toolhead-load queries keyed off
    // the motion sensor, so an unloaded lane kept rendering filament at the
    // toolhead and kept offering Unload. Now the channel_state latch governs.
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Start loaded.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "load_finish"));
    REQUIRE(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));

    // Motion sensor is (wrongly) still reporting present — the firmware bug.
    SnapmakerTestAccess::set_sensor_present(backend, 2, true);
    // Now the lane reports unload_finish while filament_detected is still true.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "unload_finish",
                                                                 /*filament_detected=*/true));

    CHECK_FALSE(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
    CHECK_FALSE(backend.slot_has_filament_at_toolhead(2));
    CHECK_FALSE(backend.can_unload_from_toolhead(2));
    // Filament is still physically in the buffer (detected=true) so the slot is
    // AVAILABLE, NOT EMPTY — the spool is present, just not at the toolhead.
    CHECK(backend.get_slot_info(2).status == SlotStatus::AVAILABLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker unload_finish preserves current_slot for the picked-up tool",
                 "[ams][snapmaker][channel_state][unload]") {
    // Bart's U1 field report (Discord 2026-07-20): T3 picked up on the carriage,
    // channel_state=unload_finish (TPU loaded directly into the toolhead, bypassing
    // the feeder state machine). With current_slot=-1 reset on every unload_finish,
    // the Filament panel's Unload routed through the bare INNER_FILAMENT_UNLOAD
    // leaf macro (no tool specifier), and the firmware defaulted to T0.
    //
    // current_slot / current_tool track which toolhead is PICKED UP (authority:
    // toolhead.extruder, parsed at the top of handle_status_update). They are
    // independent of whether feeder filament is at the nozzle. Only
    // filament_loaded should flip on unload_finish.
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Simulate T2 picked up (toolhead.extruder = "extruder2").
    json picked_up = json{{"toolhead", json{{"extruder", "extruder2"}}}};
    SnapmakerTestAccess::handle_status(backend, picked_up);
    REQUIRE(backend.get_system_info().current_slot == 2);
    REQUIRE(backend.get_system_info().current_tool == 2);

    // T2's feeder channel reaches unload_finish (filament retracted from nozzle,
    // but toolhead still mounted). filament_loaded must clear; current_slot must NOT.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "unload_finish",
                                                                 /*filament_detected=*/true));

    CHECK(backend.get_system_info().current_slot == 2);
    CHECK(backend.get_system_info().current_tool == 2);
    CHECK_FALSE(backend.get_system_info().filament_loaded);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker wait_insert means not loaded",
                 "[ams][snapmaker][channel_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    // Seed a loaded latch, then a wait_insert (channel empty) must clear it.
    SnapmakerTestAccess::set_loaded_at_toolhead(backend, 1, true);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "wait_insert",
                                                                 /*filament_detected=*/false));
    CHECK_FALSE(SnapmakerTestAccess::loaded_at_toolhead(backend, 1));
    CHECK_FALSE(backend.slot_has_filament_at_toolhead(1));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker preload_finish is present-but-not-loaded",
                 "[ams][snapmaker][channel_state]") {
    // preload stages filament into the buffer, NOT to the nozzle. Present
    // (detected=true → slot AVAILABLE) but the loaded latch must be clear.
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::set_loaded_at_toolhead(backend, 0, true);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "preload_finish",
                                                                 /*filament_detected=*/true));
    CHECK_FALSE(SnapmakerTestAccess::loaded_at_toolhead(backend, 0));
    CHECK(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(backend.can_unload_from_toolhead(0));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker loaded latch is KEPT across transient states",
                 "[ams][snapmaker][channel_state]") {
    // Only terminal transitions move the latch. Every in-progress state leaves
    // it untouched, so a mid-unload sequence keeps "loaded" true until the
    // actual unload_finish lands.
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "load_finish"));
    REQUIRE(SnapmakerTestAccess::loaded_at_toolhead(backend, 3));

    for (const char* transient : {"unload_prepare", "unload_homing", "unload_picking",
                                  "unload_heating", "unload_heat_finish", "unload_doing"}) {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(3, transient));
        INFO("transient=" << transient);
        CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 3));
    }

    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "unload_finish"));
    CHECK_FALSE(SnapmakerTestAccess::loaded_at_toolhead(backend, 3));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker fail channel_state does NOT flip the loaded latch",
                 "[ams][snapmaker][channel_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "load_finish"));
    REQUIRE(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
    // A subsequent unload_fail must not clear the latch (filament is still at
    // the toolhead — the unload failed).
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "unload_fail"));
    CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
}

// ============================================================================
// Full 39-state channel_state coverage (action + operation_phase)
// ============================================================================
// Every firmware channel_state (filament_feed.py:34-72, firmware 20260608) maps
// to the correct AmsAction and 4-phase step index. Driven off the single
// classify_channel_state() table in the backend.

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker every channel_state maps to the right action and phase",
                 "[ams][snapmaker][channel_state]") {
    struct Case {
        const char* state;
        AmsAction action;
        int phase;
    };
    const Case cases[] = {
        // idle / init
        {"none", AmsAction::IDLE, -1},
        {"inited", AmsAction::IDLE, -1},
        {"wait_insert", AmsAction::IDLE, -1},
        {"test", AmsAction::IDLE, -1},
        // preload
        {"preload_prepare", AmsAction::LOADING, 0},
        {"preload_feeding", AmsAction::LOADING, 3},
        {"preload_finish", AmsAction::IDLE, -1},
        {"preload_fail", AmsAction::ERROR, -1},
        // load
        {"load_prepare", AmsAction::LOADING, 0},
        {"load_homing", AmsAction::LOADING, 0},
        {"load_picking", AmsAction::LOADING, 1},
        {"load_heating", AmsAction::LOADING, 2},
        {"load_feeding", AmsAction::LOADING, 3},
        {"load_extruding", AmsAction::LOADING, 3},
        {"load_flushing", AmsAction::LOADING, 4},
        {"load_finish", AmsAction::IDLE, -1},
        {"load_fail", AmsAction::ERROR, -1},
        // unload
        {"unload_prepare", AmsAction::UNLOADING, 0},
        {"unload_homing", AmsAction::UNLOADING, 0},
        {"unload_picking", AmsAction::UNLOADING, 1},
        {"unload_heating", AmsAction::UNLOADING, 2},
        {"unload_heat_finish", AmsAction::UNLOADING, 2},
        {"unload_doing", AmsAction::UNLOADING, 3},
        {"unload_finish", AmsAction::IDLE, -1},
        {"unload_fail", AmsAction::ERROR, -1},
        // manual feed
        {"manual_sta_prepare", AmsAction::LOADING, 0},
        {"manual_sta_homing", AmsAction::LOADING, 0},
        {"manual_sta_picking", AmsAction::LOADING, 1},
        {"manual_sta_prepare_finish", AmsAction::LOADING, 1},
        {"manual_sta_prepare_fail", AmsAction::ERROR, -1},
        {"manual_sta_heating", AmsAction::LOADING, 2},
        {"manual_sta_extruding", AmsAction::LOADING, 3},
        {"manual_sta_extrude_finish", AmsAction::LOADING, 3},
        {"manual_sta_extrude_fail", AmsAction::ERROR, -1},
        {"manual_sta_flushing", AmsAction::LOADING, 4},
        {"manual_sta_flush_finish", AmsAction::LOADING, 4},
        {"manual_sta_flush_fail", AmsAction::ERROR, -1},
        {"manual_sta_finish", AmsAction::IDLE, -1},
        {"manual_sta_fail", AmsAction::ERROR, -1},
    };

    for (const auto& c : cases) {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        // Make extruder 0 the active lane so *_fail states surface as ERROR
        // (the false-alarm guard only suppresses errors on idle empty NON-active
        // lanes) and feed present filament.
        SnapmakerTestAccess::set_current_slot(backend, 0);
        SnapmakerTestAccess::set_current_tool(backend, 0);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(0, c.state));
        INFO("channel_state=" << c.state);
        CHECK(backend.get_system_info().action == c.action);
        CHECK(backend.get_system_info().operation_phase == c.phase);
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker fail channel_state raises ERROR with detail",
                 "[ams][snapmaker][channel_state]") {
    // A *_fail channel_state alone (channel_error still "ok") must surface an
    // error — Change 2 adds this on top of the existing channel_error handling.
    for (const char* fail : {"load_fail", "unload_fail", "preload_fail", "manual_sta_fail"}) {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        SnapmakerTestAccess::set_current_slot(backend, 1);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(1, fail));
        INFO("fail state=" << fail);
        CHECK(backend.get_system_info().action == AmsAction::ERROR);
        CHECK_FALSE(backend.get_system_info().operation_detail.empty());
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker fail on an idle empty non-active lane stays quiet",
                 "[ams][snapmaker][channel_state]") {
    // The multi-color false-alarm guard must still hold: a fail on a lane that
    // is empty, idle, and not active should not pop a spurious error modal.
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::set_current_slot(backend, 0); // active lane is 0, not 2
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "preload_fail",
                                                                 /*filament_detected=*/false));
    CHECK(backend.get_system_info().action != AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker full load progression walks the phase bar",
                 "[ams][snapmaker][channel_state][phase]") {
    struct Step {
        const char* state;
        AmsAction action;
        int phase;
    };
    const Step seq[] = {
        {"load_prepare", AmsAction::LOADING, 0},  {"load_homing", AmsAction::LOADING, 0},
        {"load_picking", AmsAction::LOADING, 1},  {"load_heating", AmsAction::LOADING, 2},
        {"load_feeding", AmsAction::LOADING, 3},  {"load_extruding", AmsAction::LOADING, 3},
        {"load_flushing", AmsAction::LOADING, 4}, {"load_finish", AmsAction::IDLE, -1},
    };
    AmsBackendSnapmaker backend(nullptr, nullptr);
    for (const auto& s : seq) {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, s.state));
        INFO("state=" << s.state);
        CHECK(backend.get_system_info().action == s.action);
        CHECK(backend.get_system_info().operation_phase == s.phase);
    }
    CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker full manual-feed progression is visible",
                 "[ams][snapmaker][channel_state]") {
    // Before the fix the entire manual_sta_* family was unhandled, so manual
    // feed left the action at IDLE and the step bar dead. Now each phase shows.
    const char* seq[] = {"manual_sta_prepare", "manual_sta_homing", "manual_sta_picking",
                         "manual_sta_heating", "manual_sta_extruding"};
    AmsBackendSnapmaker backend(nullptr, nullptr);
    for (const char* state : seq) {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, state));
        INFO("state=" << state);
        CHECK(backend.get_system_info().action == AmsAction::LOADING);
    }
    SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "manual_sta_finish"));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker motion-sensor runout path is independent of the loaded latch",
                 "[ams][snapmaker][channel_state][runout]") {
    // No regression: mid-print runout is still driven by the per-tool motion
    // sensor (e{N}_filament), a different question from "is the lane loaded".
    // The loaded latch must NOT be touched by a motion-sensor runout — filament
    // that stops moving is still loaded at the toolhead, it just ran out upstream.
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Active tool 0 loaded (pin state) with filament present everywhere.
    json loaded = json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"print_task_config", json{{"filament_exist", json::array({true, true, true, true})}}}};
    SnapmakerTestAccess::handle_status(backend, loaded);
    REQUIRE(backend.get_system_info().filament_loaded);
    // And latch it loaded via channel_state.
    SnapmakerTestAccess::set_loaded_at_toolhead(backend, 0, true);

    // Active lane's motion sensor drops during extrusion → runout.
    json runout = json{{"filament_motion_sensor e0_filament", json{{"filament_detected", false}}}};
    SnapmakerTestAccess::handle_status(backend, runout);

    // Runout path fired: the global loaded flag reflects the motion sensor.
    CHECK_FALSE(backend.get_system_info().filament_loaded);
    // But the channel_state loaded latch is untouched by the motion sensor.
    CHECK(SnapmakerTestAccess::loaded_at_toolhead(backend, 0));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker filament path segment follows the loaded latch, not the motion sensor",
                 "[ams][snapmaker][channel_state]") {
    // The canvas draws NOZZLE (filament threaded to the hotend) only when the
    // lane is actually loaded. The live bug: an unloaded lane kept its motion
    // sensor present, so the segment rendered NOZZLE and the lane looked fully
    // loaded on screen even though its Unload button was correctly disabled.
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SECTION("loaded lane draws to the nozzle") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "load_finish"));
        CHECK(backend.get_slot_filament_segment(2) == PathSegment::NOZZLE);
    }

    SECTION("unloaded lane with motion sensor still present draws OUTPUT, not NOZZLE") {
        // Exact live condition: unload_finish but the toolhead motion sensor and
        // buffer port sensor both still read present.
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "load_finish"));
        SnapmakerTestAccess::set_sensor_present(backend, 2, true);
        SnapmakerTestAccess::set_port_sensor_present(backend, 2, true);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(2, "unload_finish",
                                                                     /*filament_detected=*/true));
        CHECK_FALSE(SnapmakerTestAccess::loaded_at_toolhead(backend, 2));
        // Filament is staged in the buffer but NOT at the nozzle.
        CHECK(backend.get_slot_filament_segment(2) == PathSegment::OUTPUT);
    }

    SECTION("empty lane draws nothing") {
        SnapmakerTestAccess::set_sensor_present(backend, 1, false);
        SnapmakerTestAccess::set_port_sensor_present(backend, 1, false);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "wait_insert",
                                                                     /*filament_detected=*/false));
        CHECK(backend.get_slot_filament_segment(1) == PathSegment::NONE);
    }
}

// ============================================================================
// Extruder State Parser Tests
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker extruder state parsing", "[ams][snapmaker]") {
    SECTION("parses parked extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "park_pin": true,
            "active_pin": false,
            "grab_valid_pin": false,
            "activating_move": false,
            "extruder_offset": [0.073, -0.037, 0.0],
            "switch_count": 86,
            "retry_count": 0,
            "error_count": 1
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "PARKED");
        REQUIRE(state.park_pin == true);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.073f));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(-0.037f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
        REQUIRE(state.switch_count == 86);
        REQUIRE(state.retry_count == 0);
        REQUIRE(state.error_count == 1);
    }

    SECTION("parses active extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "ACTIVE",
            "park_pin": false,
            "active_pin": true,
            "activating_move": false,
            "extruder_offset": [0.0, 0.0, 0.0],
            "switch_count": 12,
            "retry_count": 2,
            "error_count": 0
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "ACTIVE");
        REQUIRE(state.park_pin == false);
        REQUIRE(state.active_pin == true);
        REQUIRE(state.switch_count == 12);
        REQUIRE(state.retry_count == 2);
        REQUIRE(state.error_count == 0);
    }

    SECTION("parses activating move in progress") {
        auto j = nlohmann::json::parse(R"({
            "state": "ACTIVATING",
            "park_pin": false,
            "active_pin": false,
            "activating_move": true,
            "extruder_offset": [0.0, 0.0, 0.0],
            "switch_count": 5,
            "retry_count": 0,
            "error_count": 0
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "ACTIVATING");
        REQUIRE(state.activating_move == true);
    }

    SECTION("handles missing fields gracefully") {
        auto j = nlohmann::json::parse("{}");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state.empty());
        REQUIRE(state.park_pin == false);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.switch_count == 0);
        REQUIRE(state.retry_count == 0);
        REQUIRE(state.error_count == 0);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
    }

    SECTION("handles partial extruder_offset array") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "extruder_offset": [1.5]
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(1.5f));
        // Missing indices stay at default
        REQUIRE(state.extruder_offset[1] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
    }
}

// ============================================================================
// RFID Info Parser Tests
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker RFID info parsing", "[ams][snapmaker]") {
    SECTION("parses full RFID tag data") {
        // ARGB 0xFF080A0D -> RGB 0x080A0D
        auto j = nlohmann::json::parse(R"({
            "VERSION": 1,
            "VENDOR": "Snapmaker",
            "MANUFACTURER": "Polymaker",
            "MAIN_TYPE": "PLA",
            "SUB_TYPE": "SnapSpeed",
            "ARGB_COLOR": 4278716941,
            "DIAMETER": 175,
            "WEIGHT": 500,
            "HOTEND_MAX_TEMP": 230,
            "HOTEND_MIN_TEMP": 190,
            "BED_TEMP": 60,
            "OFFICIAL": true
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type == "PLA");
        REQUIRE(info.sub_type == "SnapSpeed");
        REQUIRE(info.manufacturer == "Polymaker");
        REQUIRE(info.vendor == "Snapmaker");
        REQUIRE(info.hotend_min_temp == 190);
        REQUIRE(info.hotend_max_temp == 230);
        REQUIRE(info.bed_temp == 60);
        REQUIRE(info.weight_g == 500);
        // ARGB 4278716941 = 0xFF080A0D → mask off alpha → 0x080A0D
        REQUIRE(info.color_rgb == 0x080A0Du);
    }

    SECTION("ARGB alpha byte is masked to produce RGB") {
        // 0xFF0000FF (opaque blue) -> 0x0000FF
        auto j = nlohmann::json::parse(R"({"ARGB_COLOR": 4278190335})");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.color_rgb == 0x0000FFu);
    }

    SECTION("stores both MANUFACTURER and VENDOR independently") {
        auto j = nlohmann::json::parse(R"({
            "VENDOR": "Generic",
            "MANUFACTURER": "",
            "MAIN_TYPE": "PETG"
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        // Parser stores fields as-is; brand fallback logic is in handle_status_update
        REQUIRE(info.vendor == "Generic");
        REQUIRE(info.manufacturer.empty());
        REQUIRE(info.main_type == "PETG");
    }

    SECTION("handles missing RFID fields with safe defaults") {
        auto j = nlohmann::json::parse("{}");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type.empty());
        REQUIRE(info.sub_type.empty());
        REQUIRE(info.manufacturer.empty());
        REQUIRE(info.vendor.empty());
        REQUIRE(info.hotend_min_temp == 0);
        REQUIRE(info.hotend_max_temp == 0);
        REQUIRE(info.bed_temp == 0);
        REQUIRE(info.weight_g == 0);
        // Default color is 0x808080 (grey)
        REQUIRE(info.color_rgb == 0x808080u);
    }

    SECTION("parses PETG with different temperatures") {
        auto j = nlohmann::json::parse(R"({
            "MANUFACTURER": "Generic3D",
            "MAIN_TYPE": "PETG",
            "SUB_TYPE": "Basic",
            "HOTEND_MIN_TEMP": 220,
            "HOTEND_MAX_TEMP": 250,
            "BED_TEMP": 80,
            "WEIGHT": 1000
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type == "PETG");
        REQUIRE(info.sub_type == "Basic");
        REQUIRE(info.manufacturer == "Generic3D");
        REQUIRE(info.hotend_min_temp == 220);
        REQUIRE(info.hotend_max_temp == 250);
        REQUIRE(info.bed_temp == 80);
        REQUIRE(info.weight_g == 1000);
    }

    SECTION("parses CARD_UID array as comma-joined string") {
        auto j = json::parse(R"({"CARD_UID": [144, 32, 196, 2]})");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.uid == "144,32,196,2");
    }

    SECTION("missing CARD_UID leaves uid empty") {
        auto j = json::parse(R"({"MAIN_TYPE": "PLA"})");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.uid.empty());
    }
}

// ============================================================================
// Task 12: filament slot override integration
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker override loaded at init is applied over firmware data",
                 "[ams][snapmaker][filament_slot_override]") {
    // Seed lane_data in the mock Moonraker DB with a slot 0 override.
    // Inject the pre-loaded override into the backend directly (skipping
    // on_started() since the backend is built with api=nullptr for simplicity —
    // on_started's load_blocking path is covered by store tests elsewhere).
    // Then push a firmware status update whose values differ from the
    // override and verify the override wins for override-eligible fields
    // while firmware wins for hardware-truth fields (color is present on
    // both, but the override's color_rgb is non-zero and wins per policy).
    SnapmakerTmpCacheDir tmp("task12_override_applied");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite PLA Orange";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;
    ovr.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // Firmware pushes a different color (blue) and a different material.
    // 0xFF0000FF = opaque red in ARGB.
    json status =
        make_filament_detect_status(0, "ABS", 0xFF0000FFu, "OtherBrand", json::array({1, 2, 3, 4}));
    SnapmakerTestAccess::handle_status(backend, status);

    auto info = backend.get_slot_info(0);
    // Override-eligible fields won.
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite PLA Orange");
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0xFF5500u);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker set_slot_info(persist=true) writes override and survives status update",
                 "[ams][snapmaker][filament_slot_override]") {
    // This is the core behavior that was BROKEN before Task 12: set_slot_info
    // ignored its persist parameter and the next firmware status update
    // wiped user edits. The override must now survive subsequent parses.
    SnapmakerTmpCacheDir tmp("task12_persist_survives");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    // Override is staged in-memory AND written to the Moonraker DB.
    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!api.mock_get_db_value("lane_data", "T0").is_null());

    // Simulate a subsequent Klipper status update with conflicting firmware
    // data. Pre-Task-12 this wiped the user's edit; the fix is that
    // apply_overrides re-layers the saved override over the parse output.
    json status =
        make_filament_detect_status(0, "ABS", 0xFF0000FFu, "OtherBrand", json::array({1, 2, 3, 4}));
    SnapmakerTestAccess::handle_status(backend, status);

    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Polymaker");                // survived
    CHECK(info.spool_name == "PolyLite PLA Orange"); // survived
    CHECK(info.spoolman_id == 42);                   // survived
    CHECK(info.material == "PLA");                   // override material wins
    CHECK(info.color_rgb == 0xFF5500u);              // override color wins
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker set_slot_info(persist=false) preview does NOT write store",
                 "[ams][snapmaker][filament_slot_override]") {
    SnapmakerTmpCacheDir tmp("task12_no_persist");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    // No override staged, no DB write.
    CHECK_FALSE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "T0").is_null());

    // Preview edit is still visible via get_slot_info (in-memory only).
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Draft");
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x123456u);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker RFID UID change clears override (hardware swap detected)",
                 "[ams][snapmaker][filament_slot_override]") {
    SnapmakerTmpCacheDir tmp("task12_uid_swap_clears");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    // Seed an override AND the corresponding DB entry so we can verify
    // clear_async deletes it on swap.
    api.mock_set_db_value(
        "lane_data", "T0",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // First parse: CARD_UID=[1,2,3,4] establishes the baseline. No clear.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));

    REQUIRE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");
    REQUIRE(!api.mock_get_db_value("lane_data", "T0").is_null());

    // Second parse: DIFFERENT CARD_UID — physical swap detected. Override
    // must be cleared in-memory AND the Moonraker DB entry deleted.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PETG", 0xFF00FF00u, "Generic", json::array({5, 6, 7, 8})));

    CHECK_FALSE(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "T0").is_null());
    // Baseline advanced to the new UID.
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "5,6,7,8");

    // Override-exclusive fields reset on the live slot. spool_name is NOT
    // override-exclusive for Snapmaker — RFID's SUB_TYPE populates it (the
    // helper hard-codes "Basic"), and the clear preserves firmware writes
    // the same way it preserves brand / total_weight_g.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Generic");    // firmware's new brand flows through
    CHECK(info.spool_name == "Basic"); // firmware's SUB_TYPE flows through
    CHECK(info.spoolman_id == 0);
    CHECK(info.material == "PETG"); // firmware's new material
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker first RFID UID observation does NOT clear override",
                 "[ams][snapmaker][filament_slot_override]") {
    // Even when the override was saved against a different (now-stale) UID,
    // the very first observation is a BASELINE and must never fire a clear.
    SnapmakerTmpCacheDir tmp("task12_first_uid_baseline");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "T0", json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a UID on the FIRST observation — no prior baseline,
    // so this must NOT trigger a clear. Override survives.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFF0055FFu, "Polymaker",
                                             json::array({99, 99, 99, 99})));

    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!api.mock_get_db_value("lane_data", "T0").is_null());

    // A second parse of the SAME UID stays the baseline — no clear, no
    // "weird state" that fires on unchanged polls.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFF0055FFu, "Polymaker",
                                             json::array({99, 99, 99, 99})));

    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "T0").is_null());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker empty RFID UID does not update baseline or clear",
                 "[ams][snapmaker][filament_slot_override]") {
    // Empty UID = no tag / reader disabled / unreadable. Must not update
    // the baseline and must not clear. This is the contract that keeps
    // transient tag-read failures from masking a genuine hardware swap
    // on the next good read.
    SnapmakerTmpCacheDir tmp("task12_empty_uid_noop");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "T0", json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    SnapmakerTestAccess::seed_override(backend, 0, ovr);

    // First parse: valid UID "1,2,3,4" — baseline established.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    REQUIRE(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4");

    // Second parse: EMPTY UID (no CARD_UID field). Must NOT update baseline
    // and must NOT clear the override.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array()));
    CHECK(SnapmakerTestAccess::last_rfid_uid(backend, 0) == "1,2,3,4"); // unchanged
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "T0").is_null());

    // Third parse: same original UID "1,2,3,4" — still matches baseline,
    // no clear. Proves the empty-UID pass didn't corrupt state.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    CHECK(SnapmakerTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "T0").is_null());
}

// ============================================================================
// Firmware writeback (paxx12 Extended Firmware POST /printer/filament_detect/set)
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker set_slot_info(persist=true) POSTs to /printer/filament_detect/set",
                 "[ams][snapmaker][firmware_writeback]") {
    SnapmakerTmpCacheDir tmp("firmware_writeback_post");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "SnapSpeed"; // known SUB_TYPE — must round-trip
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    edit.nozzle_temp_min = 195;
    edit.nozzle_temp_max = 225;
    edit.bed_temp = 60;

    auto err = backend.set_slot_info(2, edit, /*persist=*/true);
    REQUIRE(err.success());

    auto history = api.rest_mock().mock_get_post_history();
    REQUIRE(history.size() == 1);
    CHECK(history[0].endpoint == "/printer/filament_detect/set");

    const auto& body = history[0].body;
    REQUIRE(body.is_object());
    CHECK(body["channel"].get<int>() == 2);
    REQUIRE(body.contains("info"));
    const auto& info_obj = body["info"];
    CHECK(info_obj["VENDOR"].get<std::string>() == "Polymaker");
    CHECK(info_obj["MAIN_TYPE"].get<std::string>() == "PLA");
    CHECK(info_obj["SUB_TYPE"].get<std::string>() == "SnapSpeed");
    CHECK(info_obj["RGB_1"].get<uint32_t>() == 0xFF5500u);
    CHECK(info_obj["ALPHA"].get<int>() == 255);
    CHECK(info_obj["HOTEND_MIN_TEMP"].get<int>() == 195);
    CHECK(info_obj["HOTEND_MAX_TEMP"].get<int>() == 225);
    CHECK(info_obj["BED_TEMP"].get<int>() == 60);
    // CARD_UID and SKU intentionally omitted — let firmware preserve them.
    CHECK_FALSE(info_obj.contains("CARD_UID"));
    CHECK_FALSE(info_obj.contains("SKU"));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker firmware POST omits unknown SUB_TYPE strings",
                 "[ams][snapmaker][firmware_writeback]") {
    // spool_name is a free-form user field. Only round-trip to firmware when
    // it matches one of the known Snapmaker product lines; otherwise omit it
    // and let firmware keep whatever it had. The free-form string still
    // lives in lane_data via the override store.
    SnapmakerTmpCacheDir tmp("firmware_writeback_unknown_subtype");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SlotInfo edit;
    edit.brand = "Generic";
    edit.spool_name = "My Custom Roll"; // NOT a known SUB_TYPE
    edit.material = "PETG";
    edit.color_rgb = 0x00FF00;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    auto history = api.rest_mock().mock_get_post_history();
    REQUIRE(history.size() == 1);
    const auto& info_obj = history[0].body["info"];
    CHECK(info_obj["VENDOR"].get<std::string>() == "Generic");
    CHECK(info_obj["MAIN_TYPE"].get<std::string>() == "PETG");
    CHECK_FALSE(info_obj.contains("SUB_TYPE"));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker firmware POST omits a Spoolman filament name as SUB_TYPE",
                 "[ams][snapmaker][firmware_writeback][spoolman]") {
    // Snapmaker is the one consumer that puts spool_name back on the wire to
    // firmware. It is the same free-form guard as the test above, reached
    // through the Spoolman picker's writer instead of a hand-typed string: a
    // real filament name ("Ambrosia Pink") is not one of the eight known
    // product lines, so SUB_TYPE is omitted and firmware keeps what it had.
    SnapmakerTmpCacheDir tmp("firmware_writeback_spoolman_name");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.material = "PLA";
    spool.filament_name = "Ambrosia Pink"; // parse_spool_info maps filament.name here
    spool.color_hex = "FFB6C1";

    SlotInfo edit;
    apply_spool_to_slot(edit, spool);
    REQUIRE(edit.spool_name == "Ambrosia Pink");

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    auto history = api.rest_mock().mock_get_post_history();
    REQUIRE(history.size() == 1);
    const auto& info_obj = history[0].body["info"];
    CHECK(info_obj["VENDOR"].get<std::string>() == "Polymaker");
    CHECK(info_obj["MAIN_TYPE"].get<std::string>() == "PLA");
    CHECK_FALSE(info_obj.contains("SUB_TYPE"));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker firmware POST omits zero temperatures",
                 "[ams][snapmaker][firmware_writeback]") {
    SnapmakerTmpCacheDir tmp("firmware_writeback_zero_temps");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    // All temps left at default 0 — must be omitted so firmware keeps prior values.

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    auto history = api.rest_mock().mock_get_post_history();
    REQUIRE(history.size() == 1);
    const auto& info_obj = history[0].body["info"];
    CHECK_FALSE(info_obj.contains("HOTEND_MIN_TEMP"));
    CHECK_FALSE(info_obj.contains("HOTEND_MAX_TEMP"));
    CHECK_FALSE(info_obj.contains("BED_TEMP"));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker set_slot_info(persist=false) does NOT POST to firmware",
                 "[ams][snapmaker][firmware_writeback]") {
    // Preview edits (persist=false) are in-memory only and must not write to
    // firmware OR the override store. Mirrors the existing "no DB write" test
    // for the override-store path.
    SnapmakerTmpCacheDir tmp("firmware_writeback_no_persist");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    CHECK(api.rest_mock().mock_get_post_history().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker firmware POST 404 on stock firmware does not fail set_slot_info",
                 "[ams][snapmaker][firmware_writeback]") {
    // Stock firmware (no Extended Firmware extension) returns 404 for the
    // endpoint. The override is still persisted to lane_data, so the user's
    // edit isn't lost — set_slot_info must report success regardless.
    SnapmakerTmpCacheDir tmp("firmware_writeback_404");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    api.rest_mock().mock_clear_post_history();
    RestResponse not_found;
    not_found.success = false;
    not_found.status_code = 404;
    not_found.error = "Not Found";
    api.rest_mock().mock_queue_post_response("/printer/filament_detect/set", not_found);

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    // POST was attempted...
    CHECK(api.rest_mock().mock_get_post_history().size() == 1);
    // ...override still made it to lane_data so the UI's view is preserved.
    auto staged = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(!api.mock_get_db_value("lane_data", "T0").is_null());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker auto-mirror uses OverwriteAlways policy after firmware writeback",
                 "[ams][snapmaker][firmware_writeback]") {
    // With the firmware-writeback path live, user edits round-trip to firmware
    // (paxx12 endpoint) and firmware-truth converges with user-truth on the
    // very next status update. The auto-mirror tail in handle_status_update
    // therefore overwrites lane_data unconditionally — picking up external
    // edits (CHANGE_ZCOLOR from a print, slicer, etc) AND keeping user edits
    // in sync. This test exercises that overwrite by seeding lane_data with
    // a stale color, then firing a status update with a different firmware
    // color, and verifying lane_data was overwritten.
    SnapmakerTmpCacheDir tmp("firmware_writeback_overwrite");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    // Pre-seed lane_data AND the in-memory override map with a stale color
    // (e.g. set externally before this backend started). With FillUnsetOnly
    // the stale color would survive; with OverwriteAlways the next firmware
    // status overwrites it. We seed the in-memory override too because the
    // mirror helper compares against `overrides_[slot]` (creating a default
    // empty entry otherwise) and writes only when the value differs.
    helix::ams::FilamentSlotOverride stale;
    stale.color_rgb = 0xABCDEF;
    stale.material = "PLA";
    SnapmakerTestAccess::seed_override(backend, 0, stale);
    api.mock_set_db_value("lane_data", "T0", json{{"color", "#ABCDEF"}, {"material", "PLA"}});

    // Build a status update that ALSO sets filament_detect.state[0]=1 so the
    // slot resolves to AVAILABLE — the mirror helper short-circuits when the
    // slot has no filament loaded ("no signal" contract). Stack the existing
    // info builder with an explicit state array.
    json status =
        make_filament_detect_status(0, "PETG", 0xFF112233u, "Polymaker", json::array({1, 2, 3, 4}));
    status["filament_detect"]["state"] = json::array({1, 0, 0, 0});
    SnapmakerTestAccess::handle_status(backend, status);

    auto lane = api.mock_get_db_value("lane_data", "T0");
    REQUIRE(!lane.is_null());
    REQUIRE(lane.contains("color"));
    // Firmware-truth color (0x112233) wins over stale lane_data (#ABCDEF).
    auto color_str = lane["color"].get<std::string>();
    // Color is stored as "#RRGGBB"; case may vary, so compare lowercase.
    std::string lower = color_str;
    for (auto& c : lower)
        c = static_cast<char>(std::tolower(c));
    CHECK(lower == "#112233");
}

// ============================================================================
// prepare_for_resume — virtual_sdcard.is_active=false with classifier (#991)
//
// The old blunt sdcard gate has been replaced by classify_pause(). With the
// terminal-matcher list currently empty (tasks 5+6 add matchers), sdcard
// inactive alone is no longer terminal — it feeds into PauseSignals and the
// classifier returns Recoverable. The resume path proceeds, and with sensor
// reporting present the "skipping recovery" fast path returns SUCCESS.
//
// Terminal matchers (dirty-bed message, exception_id=532) produce
// RESUME_REQUIRES_RESTART up front; a RESUME that truly no-ops returns an ERROR
// through the RESUME gcode callback, handled by the dispatch layer.
// ============================================================================

TEST_CASE_METHOD(
    SnapmakerFixture,
    "Snapmaker prepare_for_resume: sdcard inactive + no matchers → not terminal (Recoverable)",
    "[ams][snapmaker][resume]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    // SD inactive — old gate would have returned RESUME_REQUIRES_RESTART here.
    // New classifier (empty matchers) treats it as Recoverable.
    json deactivated = {{"print_stats", {{"state", "paused"}}},
                        {"virtual_sdcard", {{"is_active", false}}}};
    ps.update_from_status(deactivated);
    REQUIRE(ps.is_sdcard_active() == false);

    AmsBackendSnapmaker backend(nullptr, nullptr);

    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART}; // poison value
    bool callback_fired = false;
    // slot 0 with sensor_present=true (default) → fast-path SUCCESS
    backend.prepare_for_resume(/*slot_index=*/0, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });

    REQUIRE(callback_fired);
    // Classifier returns Recoverable (no matchers); sensor present → success
    REQUIRE(captured.result == AmsResult::SUCCESS);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker prepare_for_resume proceeds normally when SD active",
                 "[ams][snapmaker][resume]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    json active = {{"print_stats", {{"state", "paused"}}},
                   {"virtual_sdcard", {{"is_active", true}}}};
    ps.update_from_status(active);
    REQUIRE(ps.is_sdcard_active() == true);

    AmsBackendSnapmaker backend(nullptr, nullptr);

    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART}; // start with a poison value
    bool callback_fired = false;
    // slot_index=-1 + no active tool resolution falls into the "no active tool" branch
    // which immediately returns SUCCESS — proves the SD-active guard didn't trip.
    backend.prepare_for_resume(/*slot_index=*/-1, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });

    REQUIRE(callback_fired);
    REQUIRE(captured.result != AmsResult::RESUME_REQUIRES_RESTART);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker prepare_for_resume skips recovery when sensor reports filament present",
                 "[ams][snapmaker][resume]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    json active = {{"print_stats", {{"state", "paused"}}},
                   {"virtual_sdcard", {{"is_active", true}}}};
    ps.update_from_status(active);

    // Backend constructed with nullptr api_ — proves we never reach the gcode
    // chain. Default sensor_filament_present_ is true for all slots, so passing
    // a valid slot_index hits the "skip recovery" branch.
    AmsBackendSnapmaker backend(nullptr, nullptr);

    AmsError captured{AmsResult::NOT_CONNECTED}; // poison
    bool callback_fired = false;
    backend.prepare_for_resume(/*slot_index=*/0, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });

    REQUIRE(callback_fired);
    REQUIRE(captured.success());
    REQUIRE(captured.result == AmsResult::SUCCESS);
}

TEST_CASE_METHOD(
    SnapmakerFixture,
    "Snapmaker prepare_for_resume reports NOT_CONNECTED when api unavailable + runout latched",
    "[ams][snapmaker][resume]") {
    lv_init_safe();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    json active = {{"print_stats", {{"state", "paused"}}},
                   {"virtual_sdcard", {{"is_active", true}}}};
    ps.update_from_status(active);

    AmsBackendSnapmaker backend(nullptr, nullptr);

    // Force the sensor-runout-latched branch: filament reads as absent on
    // slot 0. With api_=nullptr the backend should return NOT_CONNECTED
    // rather than crashing on a null api_ deref.
    SnapmakerTestAccess::set_sensor_present(backend, 0, false);

    AmsError captured{AmsResult::SUCCESS};
    bool callback_fired = false;
    backend.prepare_for_resume(/*slot_index=*/0, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });

    REQUIRE(callback_fired);
    REQUIRE(captured.result == AmsResult::NOT_CONNECTED);
}

// ---------------------------------------------------------------------------
// #991 FIX 2 — the filament-config re-assert is sent as its OWN gcode and is
// best-effort: a config rejection must NOT abort the heat/feed/extrude chain.
// FIX 3 — when the AMS load fails, prepare_for_resume reports COMMAND_FAILED so
// the caller never dispatches RESUME.
// ---------------------------------------------------------------------------

namespace {
// Wire up a full mock stack + Snapmaker backend with slot 0 populated from a
// known sub-type ("Basic"), the runout sensor latched absent, and slot 0 as the
// active tool — i.e. the post-runout recovery branch of prepare_for_resume.
struct ResumeRecoveryHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState api_state;
    MoonrakerAPIMock api;
    AmsBackendSnapmaker backend;

    ResumeRecoveryHarness() : api(client, api_state), backend(&api, nullptr) {
        api_state.init_subjects(false);
        // The MoonrakerAPIMock's execute_gcode halt gate reads ITS OWN state
        // (api_state). Mark Klipper ready there so recovery gcode isn't refused.
        api_state.update_from_status(json{{"webhooks", {{"state", "ready"}}}});

        // Global printer state (read by prepare_for_resume's classify_pause):
        // paused, SD inactive (no-op-shaped), no terminal exception
        // (empty message + id -1 => classify Unknown => recoverable).
        PrinterState& ps = get_printer_state();
        PrinterStateTestAccess::reset(ps);
        ps.init_subjects(false);
        ps.update_from_status(json{{"webhooks", {{"state", "ready"}}},
                                   {"print_stats", {{"state", "paused"}}},
                                   {"virtual_sdcard", {{"is_active", false}}}});

        // Populate slot 0: MAIN_TYPE=PLA, MANUFACTURER=Polymaker, SUB_TYPE=Basic
        // (a recognized product line, so it round-trips into the config gcode).
        SnapmakerTestAccess::handle_status(
            backend, make_filament_detect_status(0, "PLA", 0xFF112233u, "Polymaker",
                                                 json::array({1, 2, 3, 4})));

        SnapmakerTestAccess::set_sensor_present(backend, 0, false); // runout latched
        SnapmakerTestAccess::set_current_slot(backend, 0);
        client.clear_gcode_script_history();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

// True if any recorded gcode script contains `needle`.
bool any_script_contains(const std::vector<std::string>& history, const std::string& needle) {
    for (const auto& s : history) {
        if (s.find(needle) != std::string::npos)
            return true;
    }
    return false;
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker prepare_for_resume drives AUTO_FEEDING load before RESUME",
                 "[ams][snapmaker][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();

    ResumeRecoveryHarness h;

    bool callback_fired = false;
    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART}; // poison
    h.backend.prepare_for_resume(/*slot_index=*/0, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });
    ResumeRecoveryHarness::drain();

    const auto& hist = h.client.gcode_script_history();

    // (a) The single firmware load call was issued: AUTO_FEEDING with LOAD=1
    // PRINTING=1 (#991) — homes, switches tool, feeds, heats, extrudes, flushes.
    REQUIRE(any_script_contains(hist, "AUTO_FEEDING EXTRUDER=0 LOAD=1 PRINTING=1"));

    // (b) None of the old hand-rolled chain pieces survive — the sensor is NOT
    // disabled (that silently neutered FEED_AUTO), config is NOT re-asserted
    // (INNER_RESUME restores it), and there is no manual M109 heat (AUTO_FEEDING
    // does the heat itself).
    REQUIRE_FALSE(any_script_contains(hist, "SET_FILAMENT_SENSOR"));
    REQUIRE_FALSE(any_script_contains(hist, "SET_PRINT_FILAMENT_CONFIG"));
    REQUIRE_FALSE(any_script_contains(hist, "M109"));

    // Recovery succeeded => on_ready(success). RESUME will be dispatched by caller.
    REQUIRE(callback_fired);
    REQUIRE(captured.success());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker prepare_for_resume: AMS load failure reports COMMAND_FAILED",
                 "[ams][snapmaker][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();

    ResumeRecoveryHarness h;

    // Force the AMS load (matched by AUTO_FEEDING) to fail. prepare_for_resume must
    // report COMMAND_FAILED so the caller never dispatches RESUME.
    h.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                    "simulated AMS load failure", "AUTO_FEEDING");

    bool callback_fired = false;
    AmsError captured{AmsResult::SUCCESS}; // poison
    h.backend.prepare_for_resume(/*slot_index=*/0, [&](const AmsError& err) {
        callback_fired = true;
        captured = err;
    });
    ResumeRecoveryHarness::drain();

    REQUIRE(callback_fired);
    REQUIRE_FALSE(captured.success());
    REQUIRE(captured.result == AmsResult::COMMAND_FAILED);
}

// ============================================================================
// last_print_tool_mapping — the routing a reprint has to replay
// ============================================================================
//
// get_tool_mapping() answers "what routing is live right now" and deliberately
// goes empty the moment extruders_used clears, which is exactly when a reprint
// needs the answer. Nothing else on the reprint path can reconstruct it: there
// is no detail view, no swatch card and no picker, so a remapped file could not
// be reprinted at all — the empty remap resolved every used tool to its
// firmware-default head and wrote that over the crossover.
//
// The snapshot is taken while the task is still configured, so it survives both
// the gate closing and the firmware resetting its own table.

namespace {
/// print_task_config frame carrying a routing table and the used-heads flags.
json make_task_config(const std::vector<int>& extruder_map, const std::vector<bool>& used) {
    json map_arr = json::array();
    for (int head : extruder_map) {
        map_arr.push_back(head);
    }
    json used_arr = json::array();
    for (bool b : used) {
        used_arr.push_back(b);
    }
    return json{
        {"print_task_config", json{{"extruder_map_table", map_arr}, {"extruders_used", used_arr}}}};
}

/// Firmware default: logical tools 0-3 on their own head, 4-31 on head 0.
std::vector<int> identity_map() {
    std::vector<int> m(32, 0);
    for (int i = 0; i < 4; ++i) {
        m[static_cast<size_t>(i)] = i;
    }
    return m;
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker records the routing a configured task ran with",
                 "[ams][snapmaker][reprint]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // T0 crosses over to head 2, T2 to head 0 — the file that cannot be
    // reprinted correctly today.
    std::vector<int> crossover = identity_map();
    crossover[0] = 2;
    crossover[2] = 0;

    SECTION("nothing is recorded before a task is configured") {
        // Idle: the firmware reports its default table with no task. This is the
        // reading that must NOT be mistaken for a print's routing — it is
        // indistinguishable from a job that genuinely needs no remap.
        SnapmakerTestAccess::handle_status(
            backend, make_task_config(identity_map(), {false, false, false, false}));
        REQUIRE(backend.get_tool_mapping().empty());
        REQUIRE(backend.last_print_tool_mapping().empty());
    }

    SECTION("a configured task is recorded, and survives the task ending") {
        SnapmakerTestAccess::handle_status(backend,
                                           make_task_config(crossover, {true, false, true, false}));
        REQUIRE(backend.get_tool_mapping() == crossover);
        REQUIRE(backend.last_print_tool_mapping() == crossover);

        // Print ends: the firmware clears extruders_used AND resets its table to
        // the default. The live accessor correctly refuses to answer; the
        // snapshot is what the reprint replays.
        SnapmakerTestAccess::handle_status(
            backend, make_task_config(identity_map(), {false, false, false, false}));
        REQUIRE(backend.get_tool_mapping().empty());
        REQUIRE(backend.last_print_tool_mapping() == crossover);
    }

    SECTION("a later task replaces the record") {
        SnapmakerTestAccess::handle_status(backend,
                                           make_task_config(crossover, {true, false, true, false}));
        REQUIRE(backend.last_print_tool_mapping() == crossover);

        std::vector<int> other = identity_map();
        other[1] = 3;
        SnapmakerTestAccess::handle_status(backend,
                                           make_task_config(other, {false, false, false, true}));
        REQUIRE(backend.last_print_tool_mapping() == other);
    }

    SECTION("a task-configured frame with no table yet records nothing") {
        // extruders_used can land before the table on an incremental update.
        // Snapshotting an empty table would publish "known: nothing", which
        // reprint_remap cannot tell from a real answer.
        json used_only = json{{"print_task_config",
                               json{{"extruders_used", json::array({true, false, false, false})}}}};
        SnapmakerTestAccess::handle_status(backend, used_only);
        REQUIRE(backend.last_print_tool_mapping().empty());

        // The table arriving in a later frame, task still configured, records it.
        SnapmakerTestAccess::handle_status(
            backend, make_task_config(crossover, {true, false, false, false}));
        REQUIRE(backend.last_print_tool_mapping() == crossover);
    }
}
