// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "filament_op_heater.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "spoolman_types.h" // SpoolInfo + apply_spool_to_slot (the picker-side writer)

#include <array>
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
    static void call_on_started(AmsBackendSnapmaker& b) {
        b.on_started();
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
// configurable material, color, brand, and CARD_UID. Slot index is 0-based.
// The other channels carry no entry, so the frame says nothing about them;
// @p others_none reports them as readers naming no tag instead.
json make_filament_detect_status(int slot_index, const std::string& main_type, uint32_t argb_color,
                                 const std::string& manufacturer, const json& card_uid,
                                 bool others_none = false) {
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
        } else if (others_none) {
            info_arr.push_back(json{{"MAIN_TYPE", "NONE"}});
        } else {
            info_arr.push_back(nullptr);
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

TEST_CASE_METHOD(SnapmakerFixture, "AmsBackendSnapmaker unload heats only through AUTO_FEEDING",
                 "[ams][snapmaker][preheat]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    // A named slot unloads through AUTO_FEEDING ... UNLOAD=1, which heats.
    CHECK(backend.unload_heats_hotend(2));
    // No slot and none loaded: the bare INNER_FILAMENT_UNLOAD leaf, which does not.
    SnapmakerTestAccess::set_current_slot(backend, -1);
    CHECK_FALSE(backend.unload_heats_hotend(-1));
    SnapmakerTestAccess::set_current_slot(backend, 1);
    CHECK(backend.unload_heats_hotend(-1));
}

// FEED_AUTO walks load_homing -> load_feeding -> load_heating -> ... and sets
// its own nozzle target from the lane's material, so a UI preheat or G28 in
// front of it is a second wait the firmware repeats anyway.
TEST_CASE_METHOD(SnapmakerFixture, "The U1 homes and heats for its own loads",
                 "[ams][snapmaker][homing][preheat]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    CHECK(backend.supports_auto_heat_on_load());
    CHECK(backend.delegates_homing_to_printer());

    const helix::ui::FilamentOpPlan load{helix::ui::FilamentTier::AmsBackend, {},
                                         helix::ui::AmsCall::Load, 1};
    CHECK(helix::ui::preheat_skip_reason(helix::ui::FilamentHeatOp::Load, &load, &backend) ==
          helix::ui::PreheatSkip::BackendSelfHeats);
}

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

// The firmware sets an op's terminal and then, in the same reactor tick, the
// channel's resting state (wait_insert with the spool out, preload_finish with
// it in). Moonraker publishes only the last channel_state, so the terminal
// survives only in channel_action_state, which resting states leave alone
// (prestonbrown/helixscreen#1832).
namespace {
json feed_with_action(int extruder_idx, const std::string& channel_state,
                      const std::string& action_state, bool filament_detected = true) {
    json frame = make_feed_status(extruder_idx, channel_state, filament_detected);
    frame.begin().value().begin().value()["channel_action_state"] = action_state;
    return frame;
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker unload that settles in one frame ends on channel_action_state",
                 "[ams][snapmaker][unload][action_state]") {
    AmsState::instance().clear_backends();
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
    REQUIRE_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    SECTION("spool out: wait_insert") {
        SnapmakerTestAccess::handle_status(
            backend, feed_with_action(3, "wait_insert", "unload_finish", /*detected=*/false));
    }
    SECTION("spool in: preload_finish") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "preload_finish", "unload_finish"));
    }

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
    CHECK(AmsState::instance().was_slot_recently_unloaded(3));
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a failed unload that settles in one frame raises the error",
                 "[ams][snapmaker][unload][action_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "preload_finish", "unload_fail"));
    CHECK(backend.get_system_info().action == AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unload mid-heat at preload_finish keeps UNLOADING until it ends",
                 "[ams][snapmaker][unload][action_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_prepare", "unload_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    for (const char* step : {"unload_heating", "unload_heat_finish", "unload_doing"}) {
        SnapmakerTestAccess::handle_status(backend, feed_with_action(3, "preload_finish", step));
        INFO("channel_action_state=" << step);
        CHECK(backend.get_system_info().action == AmsAction::UNLOADING);
    }

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "preload_finish", "unload_finish"));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a failed op from before startup raises nothing on the first frame",
                 "[ams][snapmaker][action_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    SnapmakerTestAccess::handle_status(backend, feed_with_action(3, "preload_finish", "load_fail"));
    CHECK(backend.get_system_info().action != AmsAction::ERROR);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a spool-insert preload ends at preload_finish",
                 "[ams][snapmaker][action_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "preload_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    SECTION("every step reported") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "preload_feeding"));
        REQUIRE(backend.get_system_info().action == AmsAction::LOADING);
    }
    SECTION("preload_finish arrives straight from preload_prepare") {}

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "preload_finish"));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a preload_finish frame mid-load does not end the load",
                 "[ams][snapmaker][action_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_heating"));
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "preload_finish"));
    CHECK(backend.get_system_info().action == AmsAction::LOADING);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_finish"));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an outcome that arrives before the channel settles ends the op",
                 "[ams][snapmaker][unload][action_state]") {
    AmsState::instance().clear_backends();
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
    REQUIRE_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    SECTION("a finish") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "unload_doing", "unload_finish"));
        REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
        // The settling frame omits the unchanged channel_action_state.
        SnapmakerTestAccess::handle_status(backend,
                                           make_feed_status(3, "wait_insert", /*detected=*/false));
        CHECK(backend.get_system_info().action == AmsAction::IDLE);
        CHECK(AmsState::instance().was_slot_recently_unloaded(3));
    }
    SECTION("a failure") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "unload_doing", "unload_fail"));
        REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);
        SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "wait_insert"));
        CHECK(backend.get_system_info().action == AmsAction::ERROR);
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a channel that settles before its outcome arrives ends on the outcome",
                 "[ams][snapmaker][unload][action_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "wait_insert", false));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "wait_insert", "unload_finish", false));
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a held outcome is dropped when a new op starts",
                 "[ams][snapmaker][action_state]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_finish"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "load_prepare", "load_prepare"));
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "wait_insert"));
    CHECK(backend.get_system_info().action == AmsAction::LOADING);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker the first frame after a Klipper restart ends no operation",
                 "[ams][snapmaker][action_state][klipper_restart_expiry]") {
    // The backend object outlives a Klipper restart, so what it remembers of
    // each channel describes firmware that no longer exists. The subscription's
    // first full frame must be read as a fresh start, not compared against it.
    AmsState::instance().clear_backends();
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::set_current_slot(backend, 3);
    SnapmakerTestAccess::set_current_tool(backend, 3);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    backend.on_klipper_restarted();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    SECTION("a finished unload's action_state") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "preload_finish", "unload_finish"));
        CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));
    }
    SECTION("a failed unload's action_state") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "preload_finish", "unload_fail"));
    }

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a Klipper restart forgets a held outcome",
                 "[ams][snapmaker][action_state][klipper_restart_expiry]") {
    AmsState::instance().clear_backends();
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_doing"));
    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "unload_doing", "unload_finish"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    backend.on_klipper_restarted();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "load_feeding"));
    REQUIRE(backend.get_system_info().action == AmsAction::LOADING);

    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "wait_insert"));
    CHECK(backend.get_system_info().action == AmsAction::LOADING);
    CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a Klipper restart leaves an ERROR in place",
                 "[ams][snapmaker][action_state][klipper_restart_expiry]") {
    CapturingSnapmakerBackend backend;

    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_feeding"));
    SnapmakerTestAccess::handle_status(backend, make_feed_status(1, "load_fail"));
    REQUIRE(backend.get_current_action() == AmsAction::ERROR);

    backend.on_klipper_restarted();

    CHECK(backend.get_current_action() == AmsAction::ERROR);
}

namespace {
class SnapmakerKlipperRestartFixture : public LVGLTestFixture {
  public:
    SnapmakerKlipperRestartFixture() {
        get_printer_state().init_subjects(false);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        AmsState::instance().init_subjects(true);
    }
    ~SnapmakerKlipperRestartFixture() override {
        AmsState::instance().set_backend(nullptr);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};
} // namespace

TEST_CASE_METHOD(SnapmakerKlipperRestartFixture,
                 "A U1 operation latched busy ends with a Klipper restart",
                 "[ams][snapmaker][klipper_restart_expiry]") {
    auto owned = std::make_unique<AmsBackendSnapmaker>(nullptr, nullptr);
    auto* backend = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    SnapmakerTestAccess::handle_status(*backend, make_feed_status(1, "load_feeding"));
    REQUIRE(ams_action_is_busy(backend->get_current_action()));

    get_printer_state().set_klippy_state_sync(KlippyState::STARTUP);
    get_printer_state().set_klippy_state_sync(KlippyState::READY);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(backend->get_current_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unload settled by channel_action_state demotes the loaded slot",
                 "[ams][snapmaker][unload][action_state]") {
    // The channel rests in a state that clears nothing itself (none), so only
    // the unload_finish carried by channel_action_state can say the filament
    // left the toolhead.
    AmsBackendSnapmaker backend(nullptr, nullptr);
    SnapmakerTestAccess::handle_status(
        backend, json{{"toolhead", json{{"extruder", "extruder"}}},
                      {"print_task_config",
                       json{{"filament_exist", json::array({true, false, false, false})}}}});
    SnapmakerTestAccess::handle_status(backend, make_feed_status(0, "load_finish"));
    REQUIRE(backend.get_slot_info(0).status == SlotStatus::LOADED);
    REQUIRE(backend.get_system_info().filament_loaded);

    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(0, "unload_doing", "unload_doing"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SnapmakerTestAccess::handle_status(backend, feed_with_action(0, "none", "unload_finish"));

    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);
    CHECK_FALSE(backend.get_system_info().filament_loaded);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a previous op's channel_action_state never ends a new one",
                 "[ams][snapmaker][unload][action_state]") {
    AmsState::instance().clear_backends();
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // At rest after an earlier unload: the firmware still reports its terminal.
    SnapmakerTestAccess::handle_status(backend,
                                       feed_with_action(3, "preload_finish", "unload_finish"));
    REQUIRE(backend.get_system_info().action == AmsAction::IDLE);
    CHECK_FALSE(AmsState::instance().was_slot_recently_unloaded(3));

    // A new unload starts; this frame omits the unchanged channel_action_state.
    SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "unload_heating"));
    REQUIRE(backend.get_system_info().action == AmsAction::UNLOADING);

    SECTION("a delta omitting channel_action_state") {
        SnapmakerTestAccess::handle_status(backend, make_feed_status(3, "preload_finish"));
    }
    SECTION("a full frame repeating the held channel_action_state") {
        SnapmakerTestAccess::handle_status(backend,
                                           feed_with_action(3, "preload_finish", "unload_finish"));
    }

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

    // Override-exclusive fields reset on the live slot; firmware's new
    // reading flows through. Its SUB_TYPE (the helper hard-codes "Basic")
    // names no spool beside a Generic brand, so the spool name is blank.
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Generic");
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.material == "PETG"); // firmware's new material
}

namespace {
json all_loaded_frame() {
    return json{
        {"print_task_config", json{{"filament_exist", json::array({true, true, true, true})}}}};
}

json untagged_frame() {
    return json{
        {"filament_detect",
         json{{"info", json::array({json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}},
                                    json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}};
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker product line beside another vendor's brand names no spool",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // "Basic" is the placeholder SUB_TYPE firmware keeps beside any vendor.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Generic", json::array({1, 2, 3, 4})));
    CHECK(backend.get_slot_info(0).spool_name.empty());

    backend.clear_slot_override(0);
    auto info = backend.get_slot_info(0);
    CHECK(info.spool_name.empty());
    CHECK(info.material == "PLA");
    CHECK(info.brand == "Generic");

    // Snapmaker's own brand does carry the line.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(1, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 5})));
    CHECK(backend.get_slot_info(1).spool_name == "Basic");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker tag reader naming no tag leaves nothing of the old tag after a clear",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(1, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(1).material == "PLA");
    REQUIRE(backend.get_slot_info(1).spool_name == "Basic");

    // An untagged spool: the firmware's info reads NONE throughout.
    const json untagged{
        {"filament_detect",
         json{{"info", json::array({json{{"MAIN_TYPE", "NONE"}},
                                    json{{"MAIN_TYPE", "NONE"},
                                         {"SUB_TYPE", "NONE"},
                                         {"VENDOR", "NONE"},
                                         {"MANUFACTURER", "NONE"},
                                         {"CARD_UID", json::array({0, 0, 0, 0})}},
                                    json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}};
    SnapmakerTestAccess::handle_status(backend, untagged);

    backend.clear_slot_override(1);
    auto info = backend.get_slot_info(1);
    CHECK(info.material.empty());
    CHECK(info.brand.empty());
    CHECK(info.spool_name.empty());
    CHECK(info.product_name.empty());
    CHECK(info.color_rgb == AMS_DEFAULT_SLOT_COLOR);

    // The same silence on the next frame has nothing left to take back.
    int events = 0;
    backend.set_event_callback([&](const std::string& ev, const std::string&) {
        if (ev == AmsBackend::EVENT_STATE_CHANGED)
            ++events;
    });
    SnapmakerTestAccess::handle_status(backend, untagged);
    CHECK(events == 0);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker tag reader naming no tag falls back to print_task_config",
                 "[ams][snapmaker][rfid][print_task_config]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_type", json::array({"PETG", "PETG", "PETG", "PETG"})},
                            {"filament_vendor", json::array({"eSUN", "eSUN", "eSUN", "eSUN"})},
                            {"filament_color_rgba",
                             json::array({"AA0000FF", "AA0000FF", "AA0000FF", "AA0000FF"})}}}});
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(0).material == "PLA");

    SnapmakerTestAccess::handle_status(backend, untagged_frame());
    auto info = backend.get_slot_info(0);
    CHECK(info.material == "PETG");
    CHECK(info.brand == "eSUN");
    CHECK(info.color_rgb == 0xAA0000u);
    CHECK(info.spool_name.empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker print_task_config type and vendor NONE are unset",
                 "[ams][snapmaker][print_task_config]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_type", json::array({"PLA", "NONE", "PETG", ""})},
                            {"filament_vendor", json::array({"NONE", "eSUN", "", "Sunlu"})}}}});
    CHECK(backend.get_slot_info(0).material == "PLA");
    CHECK(backend.get_slot_info(1).material.empty());
    CHECK(backend.get_slot_info(2).material == "PETG");
    CHECK(backend.get_slot_info(3).material.empty());
    CHECK(backend.get_slot_info(0).brand.empty());
    CHECK(backend.get_slot_info(1).brand == "eSUN");
    CHECK(backend.get_slot_info(3).brand == "Sunlu");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker tag reader naming no tag takes back the tag's temps and weight",
                 "[ams][snapmaker][rfid]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // A loaded slot, so the lane_data auto-mirror keeps an override for it.
    SnapmakerTestAccess::handle_status(backend, all_loaded_frame());
    // The helper's tag states 190-220 nozzle, 60 bed, 1000 g.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(2, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(2).nozzle_temp_max == 220);
    REQUIRE(backend.get_slot_info(2).total_weight_g == 1000.0f);
    REQUIRE(SnapmakerTestAccess::get_override(backend, 2).has_value());

    SnapmakerTestAccess::handle_status(backend, untagged_frame());
    const SlotInfo blank;
    auto info = backend.get_slot_info(2);
    CHECK(info.nozzle_temp_min == blank.nozzle_temp_min);
    CHECK(info.nozzle_temp_max == blank.nozzle_temp_max);
    CHECK(info.bed_temp == blank.bed_temp);
    CHECK(info.total_weight_g == blank.total_weight_g);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker user's edit stands over a reader naming no tag",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    SnapmakerTmpCacheDir tmp("edit_over_untagged");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    // The helper's tag: PLA, Snapmaker, 190-220 nozzle, 60 bed.
    const json tag =
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4}));
    SnapmakerTestAccess::handle_status(backend, all_loaded_frame());
    SnapmakerTestAccess::handle_status(backend, tag);
    SlotInfo edit = backend.get_slot_info(0);
    SECTION("an edit equal to the tag") {}
    SECTION("an edit differing from the tag") {
        edit.material = "PETG";
        edit.brand = "eSUN";
        edit.nozzle_temp_min = 230;
        edit.nozzle_temp_max = 250;
        edit.bed_temp = 80;
    }
    REQUIRE(backend.set_slot_info(0, edit, /*persist=*/true).success());

    // Firmware states back what the edit wrote to the tag record.
    json echo = make_filament_detect_status(0, edit.material, 0xFF202020u, edit.brand,
                                            json::array({1, 2, 3, 4}));
    echo["filament_detect"]["info"][0]["HOTEND_MIN_TEMP"] = edit.nozzle_temp_min;
    echo["filament_detect"]["info"][0]["HOTEND_MAX_TEMP"] = edit.nozzle_temp_max;
    echo["filament_detect"]["info"][0]["BED_TEMP"] = edit.bed_temp;
    SnapmakerTestAccess::handle_status(backend, echo);

    SnapmakerTestAccess::handle_status(backend, untagged_frame());
    auto info = backend.get_slot_info(0);
    CHECK(info.material == edit.material);
    CHECK(info.brand == edit.brand);
    CHECK(info.nozzle_temp_min == edit.nozzle_temp_min);
    CHECK(info.nozzle_temp_max == edit.nozzle_temp_max);
    CHECK(info.bed_temp == edit.bed_temp);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker bound spool stands over a reader naming no tag",
                 "[ams][snapmaker][filament_slot_override][rfid]") {
    SnapmakerTmpCacheDir tmp("spool_over_untagged");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    SnapmakerTestAccess::handle_status(backend, all_loaded_frame());
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));

    // The tag's own values, or a spool of another make.
    SlotInfo bound = backend.get_slot_info(0);
    bound.spoolman_id = 42;
    bool differs = false;
    SECTION("a spool equal to the tag") {}
    SECTION("a spool differing from the tag") {
        differs = true;
    }
    if (differs) {
        bound.brand = "Polymaker";
        bound.material = "PETG";
        bound.nozzle_temp_min = 230;
        bound.nozzle_temp_max = 250;
        bound.bed_temp = 80;
    }
    bool persist = true;
    SECTION("bound by a user edit") {}
    SECTION("bound by a Spoolman reverse sync") {
        persist = false;
    }
    REQUIRE(backend.set_slot_info(0, bound, persist).success());

    SnapmakerTestAccess::handle_status(backend, untagged_frame());
    auto info = backend.get_slot_info(0);
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == bound.material);
    CHECK(info.brand == bound.brand);
    CHECK(info.nozzle_temp_min == bound.nozzle_temp_min);
    CHECK(info.nozzle_temp_max == bound.nozzle_temp_max);
    CHECK(info.bed_temp == bound.bed_temp);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker tag reading at the default colour states no colour",
                 "[ams][snapmaker][rfid][print_task_config]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_color_rgba",
                             json::array({"AA0000FF", "AA0000FF", "AA0000FF", "AA0000FF"})}}}});
    // A reading at the default grey is a tag with no colour, so the colour it
    // leaves on the slot is not the tag's to take back.
    SnapmakerTestAccess::handle_status(
        backend, make_filament_detect_status(0, "PLA", 0xFF000000u | AMS_DEFAULT_SLOT_COLOR,
                                             "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(0).color_rgb == AMS_DEFAULT_SLOT_COLOR);

    SnapmakerTestAccess::handle_status(backend, untagged_frame());
    auto info = backend.get_slot_info(0);
    CHECK(info.color_rgb == AMS_DEFAULT_SLOT_COLOR);
    CHECK(info.material.empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker restart forgets the previous session's tag and task values",
                 "[ams][snapmaker][rfid]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);

    // print_task_config names one colour, then the tag reads another, so the
    // slot shows the tag's while the task colour waits behind it.
    SnapmakerTestAccess::handle_status(
        backend, json{{"print_task_config",
                       json{{"filament_color_rgba",
                             json::array({"AA0000FF", "AA0000FF", "AA0000FF", "AA0000FF"})}}}});
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFF202020u, "Snapmaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(0).color_rgb == 0x202020u);
    REQUIRE(backend.get_slot_info(0).nozzle_temp_max == 220);

    SnapmakerTestAccess::call_on_started(backend);
    SnapmakerTestAccess::handle_status(backend, untagged_frame());

    // Neither value the previous session recorded is judged against: the
    // temperatures are not taken back as that session's tag's, and the colour
    // does not fall back to that session's task colour.
    auto info = backend.get_slot_info(0);
    CHECK(info.nozzle_temp_min == 190);
    CHECK(info.nozzle_temp_max == 220);
    CHECK(info.bed_temp == 60);
    CHECK(info.color_rgb != 0xAA0000u);
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
// Firmware writeback (SET_PRINT_FILAMENT_CONFIG)
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a saved edit writes the record and never POSTs to filament_detect/set",
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
    client.clear_gcode_script_history();
    // The backend's own frames gate record writes; give it an idle print state.
    SnapmakerTestAccess::handle_status(backend, json{{"print_stats", {{"state", "standby"}}}});

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "SnapSpeed"; // known product line: rides FILAMENT_SUBTYPE
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    edit.nozzle_temp_min = 195;
    edit.nozzle_temp_max = 225;
    edit.bed_temp = 60;

    auto err = backend.set_slot_info(2, edit, /*persist=*/true);
    REQUIRE(err.success());

    // The Extended Firmware's filament_detect/set flags the head official,
    // which the firmware then refuses plain writes to; the record write uses
    // the stock screen's command instead.
    CHECK(api.rest_mock().mock_get_post_history().empty());
    const auto scripts = client.gcode_script_history();
    REQUIRE(scripts.size() == 2);
    CHECK(scripts[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 VENDOR=Generic "
                        "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=SnapSpeed");
    CHECK(scripts[1] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 FILAMENT_COLOR_RGBA=FF5500FF");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker weight persist writes the weight and states no identity",
                 "[ams][snapmaker][filament_slot_override][firmware_writeback][1652]") {
    SnapmakerTmpCacheDir tmp("weight_persist_no_identity");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendSnapmaker backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
        &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    SnapmakerTestAccess::inject_override_store(backend, std::move(store));

    // Channel 0 reads a PLA tag. Nobody has edited it.
    SnapmakerTestAccess::handle_status(
        backend,
        make_filament_detect_status(0, "PLA", 0xFFFF5500u, "Polymaker", json::array({1, 2, 3, 4})));
    REQUIRE(backend.get_slot_info(0).material == "PLA");
    if (const auto mirrored = SnapmakerTestAccess::get_override(backend, 0)) {
        REQUIRE_FALSE(mirrored->user_locked_color);
        REQUIRE_FALSE(mirrored->user_locked_material);
    }
    api.rest_mock().mock_clear_post_history();

    // What the consumption meter's minute persist and its pause and completion
    // flushes do.
    backend.update_slot_weight(0, 640.0f, 1000.0f, /*persist=*/true);

    const auto record = SnapmakerTestAccess::get_override(backend, 0);
    REQUIRE(record.has_value());
    CHECK(record->remaining_weight_g == Catch::Approx(640.0f));
    CHECK(record->total_weight_g == Catch::Approx(1000.0f));
    // The mirror skips a locked field, so a lock here would hold the lane to
    // this reading whatever the tag says next.
    CHECK_FALSE(record->user_locked_color);
    CHECK_FALSE(record->user_locked_material);

    const json stored = api.mock_get_db_value("lane_data", "T0");
    REQUIRE_FALSE(stored.is_null());
    CHECK(stored["remaining_weight_g"].get<float>() == Catch::Approx(640.0f));
    CHECK(stored["helix_locked_color"] == false);
    CHECK(stored["helix_locked_material"] == false);

    // Nothing restates the spool's identity to firmware.
    CHECK(api.rest_mock().mock_get_post_history().size() == 0);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a free-form spool name records Basic, not the string",
                 "[ams][snapmaker][firmware_writeback]") {
    // FILAMENT_SUBTYPE is restricted to Snapmaker's known product lines. A
    // free-form spool_name ("My Custom Roll", a Spoolman filament name) is not
    // one of them, so the record names Basic and the free-form string lives in
    // lane_data via the override store.
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
    SnapmakerTestAccess::handle_status(backend, json{{"print_stats", {{"state", "standby"}}}});
    client.clear_gcode_script_history();

    SECTION("a hand-typed string") {
        SlotInfo edit;
        edit.brand = "Generic";
        edit.spool_name = "My Custom Roll"; // NOT a known product line
        edit.material = "PETG";
        edit.color_rgb = 0x00FF00;

        REQUIRE(backend.set_slot_info(0, edit, /*persist=*/true).success());
        const auto scripts = client.gcode_script_history();
        REQUIRE(scripts.size() == 2);
        CHECK(scripts[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR=Generic "
                            "FILAMENT_TYPE=PETG FILAMENT_SUBTYPE=Basic");
    }
    SECTION("a Spoolman filament name") {
        SpoolInfo spool;
        spool.id = 42;
        spool.vendor = "Polymaker";
        spool.material = "PLA";
        spool.filament_name = "Ambrosia Pink"; // parse_spool_info maps filament.name here
        spool.color_hex = "FFB6C1";

        SlotInfo edit;
        apply_spool_to_slot(edit, spool);
        REQUIRE(edit.spool_name == "Ambrosia Pink");

        REQUIRE(backend.set_slot_info(0, edit, /*persist=*/true).success());
        const auto scripts = client.gcode_script_history();
        REQUIRE(scripts.size() == 2);
        CHECK(scripts[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR=Generic "
                            "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic");
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker set_slot_info(persist=false) writes nothing to firmware",
                 "[ams][snapmaker][firmware_writeback]") {
    // Preview edits (persist=false) are in-memory only and must not write the
    // head's firmware record, POST, or the override store.
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
    SnapmakerTestAccess::handle_status(backend, json{{"print_stats", {{"state", "standby"}}}});
    client.clear_gcode_script_history();

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    CHECK(api.rest_mock().mock_get_post_history().empty());
    CHECK(client.gcode_script_history().empty());
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

// ============================================================================
// Resume-refusal classification
// ============================================================================

// The U1 refuses RESUME while a used extruder has no material assigned, and
// says so only on the `!!` broadcast channel: print_stats.exception keeps
// whatever paused the print (a runout), because the refusal is raised
// oneshot. Uncoded `!!` on a paused printer otherwise falls through to
// error_classify::classify(), which offers Resume — the one action the
// firmware is guaranteed to refuse again until the material is set.
TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker claims the not-edit-filament resume refusal",
                 "[ams][snapmaker][error-center]") {
    AmsBackendSnapmaker backend(nullptr, nullptr);
    helix::ClassifyContext paused;
    paused.is_paused = true;

    SECTION("claims the refusal as a Snapmaker fault") {
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE(ev->source == helix::ErrorSource::SNAPMAKER);
        REQUIRE(ev->severity == helix::ErrorSeverity::CRITICAL);
        REQUIRE(ev->sticky);
    }

    SECTION("names the slot the way the machine labels it") {
        // Firmware counts extruders from 0; every slot the user reads is 1-based
        // (ams_state.cpp display_slot). e2 is the third slot.
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE(ev->detail.find('3') != std::string::npos);
        REQUIRE(ev->detail.find("e2") == std::string::npos);
    }

    SECTION("offers no Resume — the firmware refuses it until the material is set") {
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE_FALSE(ev->recovery_actions.empty());
        for (const auto& action : ev->recovery_actions) {
            REQUIRE(action.gcode.find("RESUME") == std::string::npos);
        }
    }

    SECTION("marks the refusal as the answer to the user's command") {
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE(ev->command_refusal);
    }

    SECTION("keeps the firmware wording for cross-channel dedup") {
        auto ev = backend.classify_error("!! e2 not edit filament", paused);
        REQUIRE(ev.has_value());
        REQUIRE(ev->raw_detail == "e2 not edit filament");
    }

    SECTION("every extruder index maps to its own slot") {
        struct Case {
            const char* line;
            char slot;
        };
        for (const auto& c :
             {Case{"!! e0 not edit filament", '1'}, Case{"!! e1 not edit filament", '2'},
              Case{"!! e3 not edit filament", '4'}}) {
            auto ev = backend.classify_error(c.line, paused);
            REQUIRE(ev.has_value());
            REQUIRE(ev->detail.find(c.slot) != std::string::npos);
        }
    }

    SECTION("ignores the refusal when nothing is paused or printing") {
        helix::ClassifyContext idle;
        auto ev = backend.classify_error("!! e2 not edit filament", idle);
        REQUIRE_FALSE(ev.has_value());
    }

    SECTION("leaves unrelated errors to the generic classifier") {
        REQUIRE_FALSE(
            backend.classify_error("!! Probe triggered prior to movement", paused).has_value());
        REQUIRE_FALSE(backend.classify_error("// e2 not edit filament", paused).has_value());
    }
}

// ============================================================================
// print_task_config filament record: SET_PRINT_FILAMENT_CONFIG write + resume repair
// ============================================================================
//
// The U1 refuses RESUME ("e<N> not edit filament") while a head the print uses
// has no filament_type in print_task_config. Only an RFID spool or
// SET_PRINT_FILAMENT_CONFIG fills it, so a material set in HelixScreen has to
// reach that record too.

namespace {

/// print_task_config frame naming each head's filament type and official flag,
/// plus the heads the task uses when @p used is given.
json make_task_filament(const std::array<std::string, 4>& types,
                        const std::array<bool, 4>& official = {false, false, false, false},
                        std::optional<std::vector<bool>> used = std::nullopt) {
    json ptc = {
        {"filament_type", json::array({types[0], types[1], types[2], types[3]})},
        {"filament_official", json::array({official[0], official[1], official[2], official[3]})}};
    if (used) {
        json arr = json::array();
        for (bool b : *used) {
            arr.push_back(b);
        }
        ptc["extruders_used"] = arr;
    }
    return json{{"print_task_config", ptc}};
}

struct FilamentRecordHarness {
    SnapmakerTmpCacheDir tmp;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<AmsBackendSnapmaker> owned;
    AmsBackendSnapmaker& backend;

    /// @p print_state is the print_stats.state the backend's first frame
    /// names; empty sends no print_stats at all.
    explicit FilamentRecordHarness(const std::string& name,
                                   const std::string& print_state = "standby")
        : tmp(name), api(make_api(state, client)),
          owned(std::make_unique<AmsBackendSnapmaker>(api.get(), nullptr)), backend(*owned) {
        // The API mock's halt gate reads its own state.
        state.update_from_status(json{{"webhooks", {{"state", "ready"}}}});
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            api.get(), "snapmaker", helix::ams::LaneKeyStyle::Tool);
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
        SnapmakerTestAccess::inject_override_store(backend, std::move(store));
        if (!print_state.empty()) {
            print(print_state);
        }
    }

    /// A frame naming only print_stats.state.
    void print(const std::string& state) {
        frame(json{{"print_stats", {{"state", state}}}});
    }

    ~FilamentRecordHarness() {
        drain();
        owned.reset();
        drain();
    }

    static std::unique_ptr<MoonrakerAPIMock> make_api(helix::PrinterState& st,
                                                      MoonrakerClientMock& c) {
        st.init_subjects(false);
        return std::make_unique<MoonrakerAPIMock>(c, st);
    }

    /// Stock firmware: the paxx filament_detect/set endpoint does not exist.
    void stock_firmware() {
        RestResponse not_found;
        not_found.success = false;
        not_found.status_code = 404;
        not_found.error = "Not Found";
        api->rest_mock().mock_queue_post_response("/printer/filament_detect/set", not_found);
    }

    void frame(const json& status) {
        SnapmakerTestAccess::handle_status(backend, status);
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    [[nodiscard]] std::vector<std::string> history() const {
        return client.gcode_script_history();
    }

    [[nodiscard]] std::vector<std::string> config_writes() const {
        std::vector<std::string> out;
        for (const auto& s : client.gcode_script_history()) {
            if (s.find("SET_PRINT_FILAMENT_CONFIG") != std::string::npos) {
                out.push_back(s);
            }
        }
        return out;
    }
};

SlotInfo inland_pla() {
    SlotInfo edit;
    edit.brand = "Inland";
    edit.spool_name = "My Custom Spool";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    return edit;
}

/// Global printer state paused on a plain runout: recoverable, not Terminal.
void pause_on_runout() {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    ps.update_from_status(json{{"webhooks", {{"state", "ready"}}},
                               {"print_stats", {{"state", "paused"}}},
                               {"virtual_sdcard", {{"is_active", false}}}});
}

size_t index_of_containing(const std::vector<std::string>& history, const std::string& needle) {
    for (size_t i = 0; i < history.size(); ++i) {
        if (history[i].find(needle) != std::string::npos) {
            return i;
        }
    }
    return history.size();
}

} // namespace

TEST_CASE("Snapmaker filament config gcode maps vendor and subtype to firmware values",
          "[ams][snapmaker][filament_record]") {
    SECTION("an unknown vendor and a free-form spool name take Generic / Basic") {
        SlotInfo s;
        s.brand = "Inland";
        s.spool_name = "My Custom Spool";
        s.material = "PLA";
        s.color_rgb = 0xFF5500;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(1, s) ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic FILAMENT_TYPE=PLA "
              "FILAMENT_SUBTYPE=Basic\n"
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=FF5500FF");
    }
    SECTION("a Snapmaker spool keeps its vendor and a product line with a space is quoted") {
        SlotInfo s;
        s.brand = "snapmaker";
        s.spool_name = "95A HF";
        s.material = "TPU";
        s.color_rgb = 0x00000A;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(3, s) ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 VENDOR=Snapmaker FILAMENT_TYPE=TPU "
              "FILAMENT_SUBTYPE=\"95A HF\" SOFT=1\n"
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 FILAMENT_COLOR_RGBA=00000AFF");
    }
    SECTION("a soft material the firmware's table cannot name is written SOFT=1") {
        // The firmware looks soft up by the exact FILAMENT_TYPE; TPU-85A has no
        // row there, so without SOFT=1 it would store the head as rigid.
        SlotInfo s;
        s.material = "TPU-85A";
        s.color_rgb = 0x1A1A1A;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(1, s, true) ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic FILAMENT_TYPE=TPU-85A "
              "FILAMENT_SUBTYPE=Basic SOFT=1 FORCE=1\n"
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=1A1A1AFF FORCE=1");
        for (const char* soft : {"TPE", "PEBA", "PVA", "tpu 95a", "TPU95A", "TPU+", "PVA+", "Flex",
                                 "flexible", "TPU-Soft"}) {
            s.material = soft;
            INFO(soft);
            CHECK(AmsBackendSnapmaker::build_filament_config_gcode(1, s).find(" SOFT=1") !=
                  std::string::npos);
        }
        // A soft family name only counts as a whole word or a grade of it.
        for (const char* rigid : {"PLA", "PETG", "PA-CF", "TPUX", "PLA+"}) {
            s.material = rigid;
            INFO(rigid);
            CHECK(AmsBackendSnapmaker::build_filament_config_gcode(1, s).find("SOFT") ==
                  std::string::npos);
        }
    }
    SECTION("a material with a space is quoted; one the allowlist refuses writes nothing") {
        SlotInfo s;
        s.material = "Silk PLA";
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).find(
                  "FILAMENT_TYPE=\"Silk PLA\" ") != std::string::npos);
        s.material = "PLA\"; M112";
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).empty());
    }
    SECTION("force appends FORCE=1 to both lines") {
        SlotInfo s;
        s.material = "PLA";
        s.color_rgb = 0x000000;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(2, s, true) ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 VENDOR=Generic FILAMENT_TYPE=PLA "
              "FILAMENT_SUBTYPE=Basic FORCE=1\n"
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 FILAMENT_COLOR_RGBA=000000FF FORCE=1");
    }
    SECTION("no material, or none left after cleaning, writes nothing") {
        SlotInfo s;
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).empty());
        s.material = "NONE";
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).empty());
        s.material = " ;# ";
        CHECK(AmsBackendSnapmaker::build_filament_config_gcode(0, s).empty());
    }
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a material edit on stock firmware writes the head's filament record",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_stock");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic");
    CHECK(writes[1] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=FF5500FF");
    CHECK(writes[0].find("FORCE") == std::string::npos);
    CHECK(h.api->rest_mock().mock_get_post_history().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a material change over a different firmware type rewrites the record",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_changed");
    h.frame(make_task_filament({"PETG", "PETG", "PETG", "PETG"}));
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(2, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].rfind("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 VENDOR=Generic "
                          "FILAMENT_TYPE=PLA",
                          0) == 0);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a re-save naming the firmware's own type leaves the record alone",
                 "[ams][snapmaker][filament_record]") {
    // SET_PRINT_FILAMENT_CONFIG runs FLOW_RESET_K, so a save that does not
    // change the material must not reset the head's flow calibration.
    lv_init_safe();
    FilamentRecordHarness h("record_write_unchanged");
    h.frame(make_task_filament({"NONE", "pla", "NONE", "NONE"}));
    h.stock_firmware();
    h.client.clear_gcode_script_history();

    SlotInfo edit = inland_pla();
    edit.color_rgb = 0x123456; // a colour-only change
    REQUIRE(h.backend.set_slot_info(1, edit, /*persist=*/true).success());
    FilamentRecordHarness::drain();

    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an unseen firmware record is written only for a declared material",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_unseen");
    h.stock_firmware();
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(0, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().size() == 2);

    // The same material again, colour moved: the user declared no material.
    h.client.clear_gcode_script_history();
    SlotInfo recolour = h.backend.get_slot_info(0);
    recolour.color_rgb = 0x00FF00;
    REQUIRE(h.backend.set_slot_info(0, recolour, /*persist=*/true).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());
}

namespace {
/// filament_detect as a U1 with its reader off reports it: every head a scalar
/// CARD_UID, no identity, OFFICIAL as the endpoint last left it.
json make_rig_detect(const std::array<json, 4>& card_uids,
                     const std::array<bool, 4>& official = {false, false, false, false}) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(json{{"MAIN_TYPE", "NONE"},
                            {"CARD_UID", card_uids[i]},
                            {"OFFICIAL", official[i]},
                            {"SKU", 0}});
    }
    return json{{"filament_detect", json{{"info", info}}}};
}
} // namespace

TEST_CASE("Snapmaker CARD_UID shapes decide whether a real card is present",
          "[ams][snapmaker][filament_record]") {
    const auto card = [](const json& uid) {
        return AmsBackendSnapmaker::parse_rfid_info(json{{"MAIN_TYPE", "NONE"}, {"CARD_UID", uid}})
            .has_card;
    };
    CHECK(card(0) == std::optional<bool>(false));
    CHECK(card(2953842434LL) == std::optional<bool>(true));
    CHECK(card(json::array({0, 0, 0, 0})) == std::optional<bool>(false));
    CHECK(card(json::array({144, 32, 196, 2})) == std::optional<bool>(true));
    CHECK_FALSE(AmsBackendSnapmaker::parse_rfid_info(json{{"MAIN_TYPE", "NONE"}}).has_card);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker record writes against a live U1 rig",
                 "[ams][snapmaker][filament_record]") {
    // The rig's frames: reader off, every CARD_UID a scalar 0. Head 1 is
    // official from an endpoint write; head 3 is official in print_task_config
    // with no card behind it.
    lv_init_safe();
    FilamentRecordHarness h("record_write_live_frame");
    h.frame(make_rig_detect({0, 0, 0, 0}, {false, true, false, false}));
    h.frame(make_task_filament({"NONE", "NONE", "ASA", "ASA"}, {false, true, false, true},
                               std::vector<bool>{false, false, false, false}));
    h.stock_firmware();
    h.client.clear_gcode_script_history();

    SlotInfo asa = inland_pla();
    asa.material = "ASA";
    // Head 2 already records ASA: nothing to write.
    REQUIRE(h.backend.set_slot_info(2, asa, /*persist=*/true).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());

    // Head 0, not official: a plain write.
    REQUIRE(h.backend.set_slot_info(0, asa, /*persist=*/true).success());
    FilamentRecordHarness::drain();
    auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 VENDOR=Generic "
                       "FILAMENT_TYPE=ASA FILAMENT_SUBTYPE=Basic");

    // Head 3, official with CARD_UID 0: forced.
    h.client.clear_gcode_script_history();
    REQUIRE(h.backend.set_slot_info(3, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic FORCE=1");

    // A real card on head 3 (a nonzero scalar UID) is the tag's to keep.
    h.frame(make_rig_detect({0, 0, 0, 2953842434LL}, {false, true, false, true}));
    h.client.clear_gcode_script_history();
    SlotInfo petg = inland_pla();
    petg.material = "PETG";
    REQUIRE(h.backend.set_slot_info(3, petg, /*persist=*/true).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an official RFID head's record is never written or forced",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_official");
    h.frame(
        make_rig_detect({0, json::array({144, 32, 196, 2}), 0, 0}, {false, true, false, false}));
    h.frame(make_task_filament({"PETG", "PETG", "PETG", "PETG"}, {false, true, false, false}));
    h.stock_firmware();
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();

    CHECK(h.config_writes().empty());
}

namespace {

/// filament_detect frame whose entry for @p head carries @p card_uid and no
/// filament identity: an untagged channel, or a filament_detect/set write that
/// was later cleared.
json make_detect_card(int head, const json& card_uid) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(i == head ? json{{"MAIN_TYPE", "NONE"}, {"CARD_UID", card_uid}}
                                 : json{{"MAIN_TYPE", "NONE"}});
    }
    return json{{"filament_detect", json{{"info", info}}}};
}

/// A head as an Extended Firmware filament_detect/set write leaves it: flagged
/// official in print_task_config, holding @p type in @p rgba.
json make_official_task(int head, const std::string& type, const std::string& rgba) {
    json ptc = make_task_filament({"NONE", "NONE", "NONE", "NONE"});
    ptc["print_task_config"]["filament_type"][head] = type;
    ptc["print_task_config"]["filament_official"][head] = true;
    ptc["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "FFFFFFFF", "FFFFFFFF", "FFFFFFFF"});
    ptc["print_task_config"]["filament_color_rgba"][head] = rgba;
    return ptc;
}

/// The filament_detect entry that same write leaves: filled, no card behind it.
json make_written_entry(int head, const std::string& type, const json& card_uid = 0) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(i == head ? json{{"MAIN_TYPE", type}, {"CARD_UID", card_uid}}
                                 : json{{"MAIN_TYPE", "NONE"}});
    }
    return json{{"filament_detect", json{{"info", info}}}};
}

} // namespace

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a save sends only the record lines that differ",
                 "[ams][snapmaker][filament_record]") {
    // SET_PRINT_FILAMENT_CONFIG runs FLOW_RESET_K, so a line the record
    // already agrees with must not be sent.
    lv_init_safe();
    FilamentRecordHarness h("record_write_lines");
    json ptc = make_task_filament({"NONE", "pla", "NONE", "NONE"});
    ptc["print_task_config"]["filament_vendor"] = json::array({"NONE", "Generic", "NONE", "NONE"});
    ptc["print_task_config"]["filament_sub_type"] = json::array({"NONE", "Basic", "NONE", "NONE"});
    ptc["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "FF5500FF", "FFFFFFFF", "FFFFFFFF"});
    h.frame(ptc);
    h.client.clear_gcode_script_history();

    SECTION("a re-save of what the record holds sends nothing") {
        REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
        FilamentRecordHarness::drain();
        CHECK(h.config_writes().empty());
    }
    SECTION("a colour change sends the colour line alone") {
        SlotInfo edit = inland_pla();
        edit.color_rgb = 0x123456;
        REQUIRE(h.backend.set_slot_info(1, edit, /*persist=*/true).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] ==
              "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=123456FF");
    }
    SECTION("a subtype-only change sends the identity line alone") {
        SlotInfo edit;
        edit.brand = "Generic";
        edit.material = "PLA";
        edit.spool_name = "Matte";
        edit.color_rgb = 0xFF5500;
        REQUIRE(h.backend.set_slot_info(1, edit, /*persist=*/true).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic "
                           "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Matte");
    }
    SECTION("a product-line change sends the identity line alone") {
        SlotInfo edit = inland_pla();
        edit.brand = "Snapmaker";
        edit.spool_name = "Matte";
        REQUIRE(h.backend.set_slot_info(1, edit, /*persist=*/true).success());
        FilamentRecordHarness::drain();
        const auto writes = h.config_writes();
        REQUIRE(writes.size() == 1);
        CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Snapmaker "
                           "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Matte");
    }
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker an unchanged save on a forced head sends nothing",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_forced_unchanged");
    json ptc = make_official_task(1, "PLA", "FF5500FF");
    ptc["print_task_config"]["filament_vendor"] = json::array({"NONE", "Generic", "NONE", "NONE"});
    ptc["print_task_config"]["filament_sub_type"] = json::array({"NONE", "Basic", "NONE", "NONE"});
    h.frame(make_detect_card(1, 0));
    h.frame(ptc);
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());

    // The control: a change on the same head goes out forced.
    SlotInfo petg = inland_pla();
    petg.material = "PETG";
    REQUIRE(h.backend.set_slot_info(1, petg, /*persist=*/true).success());
    FilamentRecordHarness::drain();
    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].find("FORCE=1") != std::string::npos);
}

// ============================================================================
// Official-flag repair: a card-less head flagged official by an endpoint write
// ============================================================================

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker clears the official flag on a card-less head it wrote",
                 "[ams][snapmaker][filament_record][official_repair]") {
    lv_init_safe();
    FilamentRecordHarness h("official_repair_ours");
    REQUIRE(h.backend.set_slot_info(3, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.client.clear_gcode_script_history();

    h.frame(make_written_entry(3, "PLA"));
    h.frame(make_official_task(3, "PLA", "FF5500FF"));

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic FORCE=1");
    CHECK(writes[1] ==
          "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=3 FILAMENT_COLOR_RGBA=FF5500FF FORCE=1");

    // Once per session: the same frames again send nothing.
    h.client.clear_gcode_script_history();
    h.frame(make_official_task(3, "PLA", "FF5500FF"));
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker leaves an official flag it cannot prove it wrote",
                 "[ams][snapmaker][filament_record][official_repair]") {
    lv_init_safe();
    FilamentRecordHarness h("official_repair_not_ours");
    REQUIRE(h.backend.set_slot_info(3, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.client.clear_gcode_script_history();

    SECTION("a real card behind the head") {
        h.frame(make_written_entry(3, "PLA", json::array({144, 32, 196, 2})));
        h.frame(make_official_task(3, "PLA", "FF5500FF"));
    }
    SECTION("an empty entry, which a reader that is off reports for a real spool") {
        h.frame(make_detect_card(3, 0));
        h.frame(make_official_task(3, "PLA", "FF5500FF"));
    }
    SECTION("a channel that showed a card earlier this session") {
        h.frame(make_written_entry(3, "PLA", json::array({144, 32, 196, 2})));
        h.frame(make_written_entry(3, "PLA"));
        h.frame(make_official_task(3, "PLA", "FF5500FF"));
    }
    SECTION("no card reading seen yet") {
        h.frame(make_official_task(3, "PLA", "FF5500FF"));
    }
    SECTION("a record naming another material") {
        h.frame(make_written_entry(3, "PLA"));
        h.frame(make_official_task(3, "PETG", "FF5500FF"));
    }
    SECTION("a record naming another colour") {
        h.frame(make_written_entry(3, "PLA"));
        h.frame(make_official_task(3, "PLA", "00FF00FF"));
    }
    SECTION("a head with no stored edit") {
        h.frame(make_written_entry(2, "PLA"));
        h.frame(make_official_task(2, "PLA", "FF5500FF"));
    }
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker holds the official-flag repair while a print holds the heads",
                 "[ams][snapmaker][filament_record][official_repair]") {
    lv_init_safe();
    const std::string state = GENERATE(std::string("printing"), std::string("paused"));
    FilamentRecordHarness h("official_repair_mid_print", state);
    REQUIRE(h.backend.set_slot_info(3, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.client.clear_gcode_script_history();

    h.frame(make_written_entry(3, "PLA"));
    h.frame(make_official_task(3, "PLA", "FF5500FF"));
    CHECK(h.config_writes().empty());

    h.print("complete");
    CHECK(h.config_writes().size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker sends no repair before its own frames name a print state",
                 "[ams][snapmaker][filament_record][official_repair]") {
    // The app's print state may not be known when the first frames land; the
    // backend's own print_stats is the only answer it trusts.
    lv_init_safe();
    FilamentRecordHarness h("official_repair_no_state", /*print_state=*/"");
    REQUIRE(h.backend.set_slot_info(3, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.client.clear_gcode_script_history();

    h.frame(make_written_entry(3, "PLA"));
    h.frame(make_official_task(3, "PLA", "FF5500FF"));
    CHECK(h.config_writes().empty());

    h.print("standby");
    CHECK(h.config_writes().size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker holds a save made mid-print until the heads are free",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_mid_print", "printing");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());

    h.print("printing");
    CHECK(h.config_writes().empty());

    // A pause the print raised (a runout, an M600) is no moment for it.
    h.print("paused");
    CHECK(h.config_writes().empty());

    h.print("complete");
    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].rfind("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic "
                          "FILAMENT_TYPE=PLA",
                          0) == 0);

    // Sent once.
    h.client.clear_gcode_script_history();
    h.print("standby");
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker a save made while paused goes straight out",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_while_paused", "paused");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().size() == 2);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker drops a held save with the head's override",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_held_dropped", "printing");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.client.clear_gcode_script_history();

    SECTION("a Clear Spool") {
        h.backend.clear_slot_override(1);
    }
    SECTION("a restart") {
        SnapmakerTestAccess::call_on_started(h.backend);
    }
    SECTION("a detected spool swap") {
        // The first observation only sets the tracker's baseline; a second
        // card with a different UID is the swap signal that clears the
        // override, and with it the save held for the spool that left.
        h.frame(make_filament_detect_status(1, "PETG", 0xFF00FF00u, "Generic",
                                            json::array({9, 9, 9, 9}), true));
        h.frame(make_filament_detect_status(1, "PETG", 0xFF00FF00u, "Generic",
                                            json::array({8, 8, 8, 8}), true));
    }
    h.print("complete");
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker files no tag reading from a filled entry with no card",
                 "[ams][snapmaker][rfid]") {
    lv_init_safe();
    FilamentRecordHarness h("written_entry_not_a_reading");
    h.frame(make_written_entry(2, "PETG", json::array({144, 32, 196, 2})));
    REQUIRE(h.backend.get_slot_info(2).material == "PETG");

    // The same spool's entry rewritten over the API: CARD_UID 0.
    h.frame(make_written_entry(2, "ABS"));
    CHECK(h.backend.get_slot_info(2).material != "ABS");
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume repairs a used head's blank record before resuming",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_present");
    // A material the user set earlier, which never reached the firmware record.
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();

    bool fired = false;
    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART};
    h.backend.prepare_for_resume(1, [&](const AmsError& err) {
        fired = true;
        captured = err;
    });
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].rfind("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic "
                          "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic",
                          0) == 0);
    REQUIRE(fired);
    CHECK(captured.success());
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume after a runout refeeds and repairs nothing",
                 "[ams][snapmaker][filament_record][resume]") {
    // After a runout the user may have loaded a different spool, so the
    // material remembered for the old one is not recorded against it.
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_runout");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_task_filament({"NONE", "", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    SnapmakerTestAccess::set_sensor_present(h.backend, 1, false);
    h.client.clear_gcode_script_history();

    bool fired = false;
    h.backend.prepare_for_resume(1, [&](const AmsError&) { fired = true; });
    FilamentRecordHarness::drain();

    const auto hist = h.history();
    CHECK(index_of_containing(hist, "AUTO_FEEDING EXTRUDER=1 LOAD=1") < hist.size());
    CHECK(h.config_writes().empty());
    CHECK(fired);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume repair sends nothing for a head whose material is unknown",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_unknown");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();

    bool fired = false;
    h.backend.prepare_for_resume(1, [&](const AmsError& err) { fired = err.success(); });
    FilamentRecordHarness::drain();

    CHECK(h.config_writes().empty());
    CHECK(fired);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume repair ignores a head the print does not use",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_unused");
    REQUIRE(h.backend.set_slot_info(2, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_task_filament({"NONE", "PLA", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();

    bool fired = false;
    h.backend.prepare_for_resume(1, [&](const AmsError& err) { fired = err.success(); });
    FilamentRecordHarness::drain();

    CHECK(h.config_writes().empty());
    CHECK(fired);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume repair never runs on a Terminal pause",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    ps.update_from_status(
        json{{"print_stats",
              {{"state", "paused"},
               {"exception", {{"id", 532}, {"code", 1}, {"message", "detected dirty bed"}}}}},
             {"virtual_sdcard", {{"is_active", false}}}});
    FilamentRecordHarness h("resume_repair_terminal");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();

    AmsError captured{AmsResult::SUCCESS};
    h.backend.prepare_for_resume(1, [&](const AmsError& err) { captured = err; });
    FilamentRecordHarness::drain();

    CHECK(captured.result == AmsResult::RESUME_REQUIRES_RESTART);
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume forces the record of a head official only by an endpoint write",
                 "[ams][snapmaker][filament_record][resume]") {
    // The Extended Firmware's filament_detect/set marks the head official and a
    // later clear leaves it so; with no card behind it the firmware refuses a
    // plain write, and RESUME stays refused.
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_forced");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_detect_card(1, 0));
    h.frame(make_task_filament({"NONE", "NONE", "ASA", "ASA"}, {false, true, false, true},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();

    bool fired = false;
    h.backend.prepare_for_resume(1, [&](const AmsError& err) { fired = err.success(); });
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic FORCE=1");
    CHECK(writes[1] ==
          "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 FILAMENT_COLOR_RGBA=FF5500FF FORCE=1");
    CHECK(fired);
}

TEST_CASE_METHOD(SnapmakerFixture, "Snapmaker resume never writes over a real RFID card's record",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_real_card");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_detect_card(1, json::array({144, 32, 196, 2})));
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, true, false, false},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();

    bool fired = false;
    h.backend.prepare_for_resume(1, [&](const AmsError& err) { fired = err.success(); });
    FilamentRecordHarness::drain();

    CHECK(h.config_writes().empty());
    CHECK(fired);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker an edit forces the record of an official head with no card",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_forced");
    h.frame(make_detect_card(2, json::array({0, 0, 0, 0})));
    h.frame(make_task_filament({"NONE", "NONE", "ASA", "NONE"}, {false, false, true, false}));
    h.stock_firmware();
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(2, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0] == "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 VENDOR=Generic "
                       "FILAMENT_TYPE=PLA FILAMENT_SUBTYPE=Basic FORCE=1");

    // The same head with a real card is the tag's to keep.
    h.frame(make_detect_card(2, json::array({144, 32, 196, 2})));
    h.client.clear_gcode_script_history();
    SlotInfo petg = inland_pla();
    petg.material = "PETG";
    REQUIRE(h.backend.set_slot_info(2, petg, /*persist=*/true).success());
    FilamentRecordHarness::drain();
    CHECK(h.config_writes().empty());
}

namespace {
/// print_task_config frame carrying only a routing table and the used-heads flags.
json make_task_routing(std::vector<int> map, std::vector<bool> used) {
    map.resize(32, 0);
    json map_arr = json::array();
    for (int head : map) {
        map_arr.push_back(head);
    }
    json used_arr = json::array();
    for (bool b : used) {
        used_arr.push_back(b);
    }
    return json{
        {"print_task_config", json{{"extruder_map_table", map_arr}, {"extruders_used", used_arr}}}};
}
} // namespace

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume repairs a blank head the task routes to but is not paused on",
                 "[ams][snapmaker][filament_record][resume]") {
    // Two-head print on heads 0 and 2, paused on head 0. extruders_used reads
    // all false mid-print, so head 2 is known only from the task's map.
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_map_heads");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    REQUIRE(h.backend.set_slot_info(2, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_task_routing({0, 2, 0, 0}, {true, false, true, false}));
    h.frame(make_task_routing({0, 2, 0, 0}, {false, false, false, false}));
    h.frame(make_task_filament({"PLA", "NONE", "NONE", "NONE"}));
    SnapmakerTestAccess::set_current_slot(h.backend, 0);
    h.client.clear_gcode_script_history();

    bool fired = false;
    h.backend.prepare_for_resume(0, [&](const AmsError& err) { fired = err.success(); });
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].rfind("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 ", 0) == 0);
    CHECK(fired);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume with all-false used heads repairs the head paused on",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_active_only");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    REQUIRE(h.backend.set_slot_info(2, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, false, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();

    h.backend.prepare_for_resume(1, [](const AmsError&) {});
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].rfind("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 ", 0) == 0);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a refused head repair still repairs the next head and readies once",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_refused");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    REQUIRE(h.backend.set_slot_info(2, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, true, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();
    h.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "refused",
                                    "CONFIG_EXTRUDER=1");

    int ready_calls = 0;
    bool success = false;
    h.backend.prepare_for_resume(1, [&](const AmsError& err) {
        ++ready_calls;
        success = err.success();
    });
    FilamentRecordHarness::drain();

    CHECK(ready_calls == 1);
    CHECK(success);
    const auto hist = h.history();
    CHECK(index_of_containing(hist, "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=2 ") < hist.size());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker resume writes nothing before the firmware record has been seen",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    pause_on_runout();
    FilamentRecordHarness h("resume_repair_unseen");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    SnapmakerTestAccess::set_current_slot(h.backend, 1);
    h.client.clear_gcode_script_history();

    bool fired = false;
    h.backend.prepare_for_resume(1, [&](const AmsError& err) { fired = err.success(); });
    FilamentRecordHarness::drain();

    CHECK(h.config_writes().empty());
    CHECK(fired);
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a Terminal pause with no active tool writes no record",
                 "[ams][snapmaker][filament_record][resume]") {
    lv_init_safe();
    helix::ui::UpdateQueue::instance().init();
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    ps.update_from_status(
        json{{"print_stats",
              {{"state", "paused"},
               {"exception", {{"id", 532}, {"code", 1}, {"message", "detected dirty bed"}}}}},
             {"virtual_sdcard", {{"is_active", false}}}});
    FilamentRecordHarness h("resume_repair_terminal_no_tool");
    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}, {false, false, false, false},
                               std::vector<bool>{false, true, false, false}));
    SnapmakerTestAccess::set_current_slot(h.backend, -1);
    h.client.clear_gcode_script_history();

    AmsError captured{AmsResult::SUCCESS};
    h.backend.prepare_for_resume(-1, [&](const AmsError& err) { captured = err; });
    FilamentRecordHarness::drain();

    CHECK(captured.result == AmsResult::RESUME_REQUIRES_RESTART);
    CHECK(h.config_writes().empty());
}

TEST_CASE_METHOD(SnapmakerFixture,
                 "Snapmaker a filament_detect/set answered state:error falls back to the gcode",
                 "[ams][snapmaker][filament_record]") {
    lv_init_safe();
    FilamentRecordHarness h("record_write_state_error");
    h.frame(make_task_filament({"NONE", "NONE", "NONE", "NONE"}));
    RestResponse refused;
    refused.success = true;
    refused.status_code = 200;
    refused.data = {{"state", "error"}, {"message", "unsupported fields: X"}};
    h.api->rest_mock().mock_queue_post_response("/printer/filament_detect/set", refused);
    h.client.clear_gcode_script_history();

    REQUIRE(h.backend.set_slot_info(1, inland_pla(), /*persist=*/true).success());
    FilamentRecordHarness::drain();

    const auto writes = h.config_writes();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].rfind("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=1 ", 0) == 0);
}
