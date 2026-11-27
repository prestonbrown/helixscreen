// tests/test_helpers/ad5x_ifs_test_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What these test files share: their fixtures, helpers and includes.
// test_ams_backend_ad5x_ifs.cpp, test_ams_backend_ad5x_ifs_2.cpp, test_ams_backend_ad5x_ifs_3.cpp,
// test_ams_backend_ad5x_ifs_4.cpp, test_ams_backend_ad5x_ifs_5.cpp,
// test_ams_backend_ad5x_ifs_6.cpp, test_ams_backend_ad5x_ifs_7.cpp,
// test_ams_backend_ad5x_ifs_8.cpp. Generated from the first of those files by split_v3.py, which
// keeps every case's text and order and moves everything else here.
#pragma once

#include "ui_insert_notice.h"

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/http_executor.h"
#include "filament_database.h"
#include "filament_op_router.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "filament_variants.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/filament_slot_override_store_test_access.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/unique_temp_dir.h"
#include "test_helpers/update_queue_test_access.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::test {

using helix::Ad5xIfsTestAccess;
using helix::AmsAction;
using helix::AmsBackend;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendAfc;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;
using helix::AmsState;
using helix::AmsSystemInfo;
using helix::AmsType;
using helix::kAmsDefaultSlotColor;
using helix::PathSegment;
using helix::PathTopology;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::StepOperationType;
struct Ad5xIfsTmpCacheDir {
    std::filesystem::path path;
    explicit Ad5xIfsTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("ad5x_ifs_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~Ad5xIfsTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Backend subclass that records every dispatched gcode instead of reaching a
// printer. Used by the _IFS_VARS mirror tests and the IFS_MAP_TOOL verb tests.
class GcodeCapturingBackend : public AmsBackendAd5xIfs {
  public:
    using AmsBackendAd5xIfs::AmsBackendAd5xIfs;
    std::vector<std::string> captured_gcodes;
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> /*on_complete*/) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> /*on_complete*/,
                           helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                           bool /*silent*/) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    /// Deliver one status frame the way the subscription does.
    void feed(const nlohmann::json& frame) {
        handle_status_update(
            nlohmann::json{{"method", "notify_status_update"}, {"params", {frame, 0.0}}});
    }
    bool any_gcode_starts_with(const std::string& prefix) const {
        for (const auto& g : captured_gcodes) {
            if (g.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }
};

using json = nlohmann::json;

static json make_extruder(double temperature, double target) {
    return json{{"extruder", json{{"temperature", temperature}, {"target", target}}}};
}

static json make_save_variables(const json& variables) {
    return json{{"save_variables", json{{"variables", variables}}}};
}

static json make_ifs_vars_macro(const json& dict) {
    return json{{"gcode_macro _ifs_vars", dict}};
}

static json make_ifs_frame(const json& tool_map) {
    json ifs = json{{"connected", true}, {"channel_count", 4}};
    if (!tool_map.is_null()) {
        ifs["tool_map"] = tool_map;
    }
    return json{{"ifs", ifs}};
}

static json identity_tool_map() {
    return json{{"0", 1}, {"1", 2}, {"2", 3}, {"3", 4}};
}

static json make_port_sensor(int port_1based, bool detected) {
    std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port_1based);
    return json{{key, json{{"filament_detected", detected}}}};
}

static json make_head_sensor(bool detected) {
    return json{
        {"filament_switch_sensor head_switch_sensor", json{{"filament_detected", detected}}}};
}

static json make_motion_sensor(bool detected) {
    return json{
        {"filament_motion_sensor ifs_motion_sensor", json{{"filament_detected", detected}}}};
}

static json make_zmod_head_sensor(bool detected) {
    return json{
        {"zmod_ifs_switch_sensor head_switch_sensor", json{{"filament_detected", detected}}}};
}

static json make_zmod_motion_sensor(bool detected) {
    return json{
        {"zmod_ifs_motion_sensor ifs_motion_sensor", json{{"filament_detected", detected}}}};
}

static json standard_variables() {
    return json{{"less_waste_colors", json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"})},
                {"less_waste_types", json::array({"PLA", "PETG", "ABS", "TPU"})},
                {"less_waste_tools", json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
                {"less_waste_current_tool", 0},
                {"less_waste_external", 0}};
}

static void seed_standard_colors(AmsBackendAd5xIfs& b) {
    // Presence FIRST, then color/material — mirroring the real atomic parse
    // (apply_zcolor_result fills presence + color + material together, so the
    // detectors' first observation of a slot always sees it present). Setting
    // material while the slot still reads absent would create an artificial
    // "material-before-presence" window that the real path never produces: with
    // the #1065 presence-lag baseline-hold, that window turns the later presence
    // flip into a "" -> MATERIAL insert delta and fabricates an auto-mirror
    // override. Presence-first keeps the post-seed state override-free, matching
    // production and every test written against this helper.
    Ad5xIfsTestAccess::set_port_presence(b, 0, true);
    Ad5xIfsTestAccess::set_port_presence(b, 1, true);
    Ad5xIfsTestAccess::set_port_presence(b, 2, true);
    Ad5xIfsTestAccess::set_port_presence(b, 3, true);
    Ad5xIfsTestAccess::set_color(b, 0, "FF0000");
    Ad5xIfsTestAccess::set_color(b, 1, "00FF00");
    Ad5xIfsTestAccess::set_color(b, 2, "0000FF");
    Ad5xIfsTestAccess::set_color(b, 3, "FFFFFF");
    Ad5xIfsTestAccess::set_material(b, 0, "PLA");
    Ad5xIfsTestAccess::set_material(b, 1, "PETG");
    Ad5xIfsTestAccess::set_material(b, 2, "ABS");
    Ad5xIfsTestAccess::set_material(b, 3, "TPU");
}

struct RegisteredAd5xIfs {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend_reg{nullptr, nullptr};
    AmsBackendAd5xIfs& backend = *backend_reg;
};

struct WiredAd5xIfs {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> backend_reg;
    helix::test::WiredBackend<AmsBackendAd5xIfs>& backend = *backend_reg;
};

inline json plugin_rig_frame() {
    return json::parse(R"({
        "webhooks": {"state": "ready"},
        "save_variables": {"variables": {
            "less_waste_tools": [2, 1, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5],
            "less_waste_current_tool": 1,
            "less_waste_external": 0,
            "bambufy_custom_types": ["PLA+"]}},
        "gcode_macro _ifs_vars": {"variable_backup": 1, "variable_ifs_unlock_after_boot": 1},
        "filament_switch_sensor _ifs_port_sensor_1": {"filament_detected": true},
        "filament_switch_sensor _ifs_port_sensor_2": {"filament_detected": false},
        "filament_switch_sensor _ifs_port_sensor_3": {"filament_detected": true},
        "filament_switch_sensor _ifs_port_sensor_4": {"filament_detected": false},
        "zmod_ifs_motion_sensor ifs_motion_sensor": {"filament_detected": false},
        "zmod_ifs_switch_sensor head_switch_sensor": {"filament_detected": true},
        "extruder": {"temperature": 219.5, "target": 220.0}
    })");
}

/// Delivers frames to a plugin rig and counts the state events they raise.
struct FrameRig {
    helix::test::RegisteredWired<AmsBackendAd5xIfs> reg;
    helix::test::WiredBackend<AmsBackendAd5xIfs>& backend = *reg;
    int events = 0;
    FrameRig() {
        Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(backend, false);
        backend.set_event_callback([this](const std::string& event, const std::string&) {
            events += event == AmsBackend::EVENT_STATE_CHANGED ? 1 : 0;
        });
    }
    void deliver(const json& frame) {
        backend.fire_status(frame);
    }
};

static json wrap_notification(const json& status) {
    return json{{"method", "notify_status_update"}, {"params", json::array({status, 12345.678})}};
}

static AmsBackendAd5xIfs::ZColorSilentResult make_zcolor(std::optional<int> extruder_slot,
                                                         bool saw_valid) {
    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = saw_valid;
    r.extruder_slot = extruder_slot;
    // slots[] left default-empty (std::array of std::optional) — the per-port
    // loop in apply_zcolor_result handles empty entries as "not loaded".
    return r;
}

struct Ad5xIfsTmpJsonFile {
    std::filesystem::path path;
    explicit Ad5xIfsTmpJsonFile(const std::string& suffix, const std::string& seed_content) {
        path = std::filesystem::temp_directory_path() /
               ("ad5x_ifs_advjson_" + suffix + "_" + std::to_string(::getpid()) + ".json");
        std::filesystem::remove(path);
        if (!seed_content.empty()) {
            std::ofstream f(path);
            f << seed_content;
        }
    }
    ~Ad5xIfsTmpJsonFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

class TestableAd5xIfsBackend : public AmsBackendAd5xIfs {
  public:
    using AmsBackendAd5xIfs::AmsBackendAd5xIfs;
    TestableAd5xIfsBackend() : AmsBackendAd5xIfs(nullptr, nullptr) {}

    std::vector<std::string> captured_gcodes;
    helix::SharedCallback<void()> captured_completion; // on_complete of the last dispatch
    helix::SharedCallback<void(const MoonrakerError&)> captured_error; // on_error, 4-arg form only

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        captured_gcodes.push_back(gcode);
        captured_completion = std::move(on_complete);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        captured_gcodes.push_back(gcode);
        captured_completion = std::move(on_complete);
        captured_error = std::move(on_error);
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return homed;
    }

    bool homed = true;

    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }
    bool has_gcode_containing(const std::string& needle) const {
        for (const auto& g : captured_gcodes) {
            if (g.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
};

/// A running TestableAd5xIfsBackend, the starting point of most eject and lane-change cases.
struct RunningAd5xIfs {
    TestableAd5xIfsBackend backend;
    RunningAd5xIfs() {
        Ad5xIfsTestAccess::set_running(backend, true);
    }
};

/// A running backend with no Z-color support, so no confirming poll or async
/// debounce task runs under the case.
struct QuietRunningAd5xIfs : RunningAd5xIfs {
    QuietRunningAd5xIfs() {
        Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    }
};

inline void seat_unrecorded_zmod(TestableAd5xIfsBackend& backend, std::array<bool, 4> present,
                                 std::optional<int> file_channel = 0) {
    Ad5xIfsTestAccess::set_running(backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(backend, false);
    Ad5xIfsTestAccess::set_current_slot(backend, -1, /*filament_loaded=*/false);
    Ad5xIfsTestAccess::set_head_switch(backend, true, true);
    Ad5xIfsTestAccess::set_head_filament(backend, true);
    Ad5xIfsTestAccess::set_ffm_channel(backend, file_channel.value_or(0));
    Ad5xIfsTestAccess::set_ffm_file_channel(backend, file_channel);
    Ad5xIfsTestAccess::set_persisted_seated_slot(backend, std::nullopt);
    for (size_t i = 0; i < present.size(); ++i) {
        Ad5xIfsTestAccess::set_port_presence(backend, i, present[i]);
    }
    REQUIRE(Ad5xIfsTestAccess::seated_chan(backend) == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::module_live(backend));
}

class LiveAckAd5xIfsBackend : public AmsBackendAd5xIfs {
  public:
    MockPrinter printer;

    LiveAckAd5xIfsBackend() : AmsBackendAd5xIfs(nullptr, nullptr) {
        api_ = &printer.api;
        Ad5xIfsTestAccess::set_running(*this, true);
        Ad5xIfsTestAccess::set_zcolor_supported(*this, false);
    }
    ~LiveAckAd5xIfsBackend() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    bool toolhead_homed() const override {
        return true;
    }
};

inline MoonrakerError macro_error(MoonrakerErrorType type, const std::string& message) {
    MoonrakerError err;
    err.type = type;
    err.message = message;
    return err;
}

/// The failure reaches the backend through two main-thread hops: the dispatch
/// marshals it off the response thread, and the backend's own callback defers
/// again before taking the lock.
inline void drain_queue() {
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::UpdateQueue::instance().drain();
}

inline helix::ams::FilamentSlotOverride make_locked_override(uint32_t color_rgb,
                                                             const std::string& material) {
    helix::ams::FilamentSlotOverride ovr;
    ovr.color_rgb = color_rgb;
    ovr.color_set = true;
    ovr.material = material;
    return ovr;
}

// An auto-mirror override: color/material set from a prior external edit but
// NEITHER field user-locked. This is what sync_override_to_firmware_locked
// leaves behind after an external CHANGE_ZCOLOR — it masks firmware truth in
// apply_overrides exactly like a locked one, but the OverwriteAlways mirror is
// free to refresh it. Reproduces the state where an earlier edit had
// already staged an override before the failing type-then-color sequence.
inline helix::ams::FilamentSlotOverride make_auto_mirror_override(uint32_t color_rgb,
                                                                  const std::string& material) {
    helix::ams::FilamentSlotOverride ovr;
    ovr.color_rgb = color_rgb;
    ovr.color_set = true;
    ovr.material = material;
    return ovr;
}

const std::vector<std::string> kZmodPalette = {
    "ffffff", "fef043", "dcf478", "0acc38", "067749", "0c6283", "0de2a0", "75d9f3",
    "45a8f9", "2750e0", "46328e", "a03cf7", "f330f9", "d4b0dc", "f95d73", "f72224",
    "7c4b00", "f98d33", "fdebd5", "d3c4a3", "af7836", "898989", "bcbcbc", "161616"};

// zmod's stock material whitelist, in render order (17:34:35.849-35.886).
// PETG-CF is last, so that is the type the slot ended up displaying.
const std::vector<std::string> kZmodMaterials = {"PLA", "PLA-CF", "SILK",   "TPU",
                                                 "ABS", "PETG",   "PETG-CF"};

inline std::string palette_button(int slot1, const std::string& type, const std::string& hex) {
    return "// action:prompt_button _ |CHANGE_ZCOLOR SLOT=" + std::to_string(slot1) +
           " TYPE=" + type + " HEX=" + hex + "|primary|" + hex;
}

inline std::string material_button(int slot1, const std::string& type, const std::string& hex) {
    return "// action:prompt_button " + type + "|CHANGE_ZCOLOR SLOT=" + std::to_string(slot1) +
           " TYPE=" + type + " HEX=" + hex + "|primary|" + hex;
}

inline std::string slot_row(int slot1, const std::string& type, const std::string& hex) {
    return "// action:prompt_button " + std::to_string(slot1) + ": " + type +
           "|RUN_ZCOLOR SLOT=" + std::to_string(slot1) + " HEX=" + hex + " TYPE=" + type +
           "|primary|" + hex;
}

inline std::unique_ptr<TestableAd5xIfsBackend>
make_routing_backend(int current_slot, int seated_chan, bool head_loaded) {
    auto backend = std::make_unique<TestableAd5xIfsBackend>();
    Ad5xIfsTestAccess::set_running(*backend, true);
    Ad5xIfsTestAccess::set_zcolor_supported(*backend, false); // no async debounce task
    // Plugin path so IFS_STATUS Chan sets seated_chan_ WITHOUT driving current_slot,
    // letting us pin the two authorities independently.
    Ad5xIfsTestAccess::set_has_ifs_vars(*backend, true);
    if (seated_chan > 0) {
        AmsBackendAd5xIfs::ZColorSilentResult r;
        r.saw_valid_response = true;
        r.ifs_active = true;
        r.ifs_chan = seated_chan;
        Ad5xIfsTestAccess::apply_zcolor_result(*backend, r);
    }
    Ad5xIfsTestAccess::set_current_slot(*backend, current_slot, /*filament_loaded=*/head_loaded);
    Ad5xIfsTestAccess::set_head_filament(*backend, head_loaded);
    // Adventurer5M.json read and recording no lane, so a toolhead unload of an
    // explicit lane may declare it.
    Ad5xIfsTestAccess::set_ffm_file_channel(*backend, 0);
    return backend;
}

// Runs the real unload and reports which route it took. Asserts exactly one route.
inline bool unload_routed_to_toolhead(TestableAd5xIfsBackend& b, int slot) {
    REQUIRE(b.unload_filament(slot).success());
    const bool cut = b.has_gcode_containing("REMOVE_CURRENT_PRUTOK");
    const bool eject = b.has_gcode_containing("IFS_F11") || b.has_gcode_containing("IFS_F24");
    REQUIRE(cut != eject); // exactly one route, never both / neither
    return cut;
}

class Ad5xHomingGuardBackend : public AmsBackendAd5xIfs {
  public:
    Ad5xHomingGuardBackend(MoonrakerAPI* api, helix::MoonrakerClient* client)
        : AmsBackendAd5xIfs(api, client) {}

    std::vector<std::string> captured_gcodes;
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        captured_gcodes.push_back(gcode);
        (void)on_complete;
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> /*on_complete*/,
                           helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                           bool /*silent*/) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
};

struct Ad5xHomingGuardFixture : public LVGLTestFixture {
    Ad5xHomingGuardFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
        backend = std::make_unique<Ad5xHomingGuardBackend>(api.get(), &mock_client);
        // check_preconditions() short-circuits on a stopped backend; flip running_
        // so the print-active gate is actually reached.
        Ad5xIfsTestAccess::set_running(*backend, true);
    }
    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }
    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<Ad5xHomingGuardBackend> backend;
};

struct Ad5xRunoutFixture : public LVGLTestFixture {
    Ad5xRunoutFixture() {
        get_printer_state().init_subjects(false);
        set_print_state(helix::PrintJobState::STANDBY);
        set_homed_axes("");
    }
    ~Ad5xRunoutFixture() override {
        set_print_state(helix::PrintJobState::STANDBY);
        set_homed_axes("");
    }

    static void set_homed_axes(const char* axes) {
        get_printer_state().update_from_status({{"toolhead", {{"homed_axes", axes}}}});
    }

    // Plain null check, not REQUIRE: this also runs from the destructor, where a
    // Catch2 assertion would throw during unwinding.
    static void set_print_state(helix::PrintJobState s) {
        if (get_printer_state().print_state().get_print_state_enum_subject()) {
            helix::test::set_wire_state(get_printer_state(), s);
        }
    }

    /// Seat filament at the toolhead SWITCH, then drop it - the present->absent
    /// edge the detector arms on. Two frames, because an edge needs a previous
    /// value and a fresh backend has never seen the switch.
    template <typename Backend> static void seat_then_drop_head(Backend& b) {
        b.fire_status(make_head_sensor(true));
        b.fire_status(make_head_sensor(false));
    }

    /// Evaluate the detector with the dwell long past.
    static bool runs_out_after_dwell(AmsBackendAd5xIfs& b) {
        Ad5xIfsTestAccess::age_head_empty(b, std::chrono::seconds(600));
        return Ad5xIfsTestAccess::evaluate_runout(b);
    }
};

/// Gcode-capturing backend wired to a MoonrakerAPIMock, so the real
/// change_tool() / unload_filament() dispatch paths run without a printer.
class Ad5xRunoutOpBackend : public AmsBackendAd5xIfs {
  public:
    using AmsBackendAd5xIfs::AmsBackendAd5xIfs;

    std::vector<std::string> captured_gcodes;

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        captured_gcodes.push_back(gcode);
        (void)on_complete;
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> /*on_complete*/,
                           helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                           bool /*silent*/) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return true;
    }
    bool has_gcode_containing(const std::string& needle) const {
        for (const auto& g : captured_gcodes) {
            if (g.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
};

/// Adds the mock API the operation-dispatch tests need.
///
/// Two PrinterStates are deliberately in play: `local_state` backs the mock API
/// and is what refuse_if_printing() consults (kept STANDBY so a dispatch is
/// allowed at all - this backend refuses filament ops while PAUSED, see
/// filament_ops_self_home()), while the inherited global is what the runout
/// detector reads and is what the tests drive to PAUSED.
struct Ad5xRunoutOpFixture : public Ad5xRunoutFixture {
    Ad5xRunoutOpFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        local_state.init_subjects(false);
        helix::test::set_wire_state(local_state, helix::PrintJobState::STANDBY);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, local_state);
        backend = std::make_unique<helix::test::StartedBackend<Ad5xRunoutOpBackend>>(api.get());
        Ad5xIfsTestAccess::set_zcolor_supported(*backend, false);
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState local_state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<helix::test::StartedBackend<Ad5xRunoutOpBackend>> backend;
};

template <typename Backend> static bool macro_changes_state(Backend& backend, const json& dict) {
    int events = 0;
    backend.set_event_callback([&events](const std::string& event, const std::string&) {
        events += event == AmsBackend::EVENT_STATE_CHANGED ? 1 : 0;
    });
    backend.fire_status(make_ifs_vars_macro(dict));
    backend.set_event_callback(nullptr);
    return events > 0;
}

inline json zmod_change_frame(int channel) {
    return json{{"gcode_macro END_CHANGE_FILAMENT", {{"last_data", {{"channel", channel}}}}}};
}

inline nlohmann::json outside_lane_record(int slot, const std::string& color,
                                          const std::string& material, const std::string& name,
                                          int spool_id) {
    nlohmann::json rec{{"lane", std::to_string(slot)},
                       {"color", color},
                       {"material", material},
                       {"name", name},
                       {"vendor_name", "Eryone"},
                       {"nozzle_temp", 210},
                       {"bed_temp", 60}};
    if (spool_id > 0) {
        rec["spool_id"] = spool_id;
    }
    return rec;
}

// A read of the lane records, landed.
inline void lane_record_reread_lands(AmsBackendAd5xIfs& backend) {
    Ad5xIfsTestAccess::refresh_lane_records(backend);
    helix::ui::UpdateQueue::instance().drain();
}

// on_started()'s store and its first read, against whatever the mock
// namespace holds.
inline void boot_from_lane_data(AmsBackendAd5xIfs& backend, MockPrinter& printer,
                                const std::filesystem::path& /*cache_dir*/) {
    Ad5xIfsTestAccess::inject_override_store(
        backend, std::make_unique<helix::ams::FilamentSlotOverrideStore>(&printer.api, "ifs"));
    lane_record_reread_lands(backend);
}

// Firmware's own reading of slot 0, as a GET_ZCOLOR / Adventurer5M.json parse
// leaves it: presence first, then colour and type.
inline void firmware_states_slot0(AmsBackendAd5xIfs& backend, const char* hex, const char* type) {
    Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
    Ad5xIfsTestAccess::set_color(backend, 0, hex);
    Ad5xIfsTestAccess::set_material(backend, 0, type);
}

// The outside tool's firmware write, as the backend hears it: the
// CHANGE_ZCOLOR echo on the gcode stream, then the Adventurer5M.json re-read
// it schedules.
inline void outside_tool_change_zcolor(AmsBackendAd5xIfs& backend) {
    REQUIRE_FALSE(Ad5xIfsTestAccess::on_gcode_response_line(
        backend, "CHANGE_ZCOLOR SLOT=1 HEX=0055FF TYPE=PETG"));
    Ad5xIfsTestAccess::parse_adventurer_json(
        backend, R"({"FFMInfo": {"ffmColor1": "#0055FF", "ffmType1": "PETG"}})");
}

inline void lesswaste_ready(AmsBackendAd5xIfs& ifs) {
    Ad5xIfsTestAccess::set_ifs_macro_confirmed_missing(ifs, false);
}

/// lessWaste's private save_variables namespace: a 1:1 tool->port map and the
/// active tool. less_waste_tools sets has_ifs_vars_, so current_slot resolves
/// through the tool map rather than the native seated-channel path.
inline json less_waste_vars(int current_tool) {
    return json{{"less_waste_tools", json::array({1, 2, 3, 4})},
                {"less_waste_current_tool", current_tool}};
}

inline json lesswaste_frame(const json& vars) {
    return json{{"save_variables", json{{"variables", vars}}}};
}

/// Slot 0 seated and printing: T0 active, its port sensor sees the spool, the
/// head switch sees filament at the extruder.
template <typename Backend> void seat_slot0(Backend& ifs) {
    json frame = lesswaste_frame(less_waste_vars(0));
    frame.update(make_port_sensor(1, true));
    frame.update(make_port_sensor(3, true));
    frame.update(make_head_sensor(true));
    ifs.fire_status(frame);
}

struct IfsLaneTruth {
    Ad5xIfsTmpCacheDir tmp;
    MockPrinter printer;
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> reg;
    AmsBackendAd5xIfs& backend;
    std::string json_path;

    /// @p stored, when an object, is lane 1's record in Moonraker before the
    /// store opens. @p settle lets the store's first read land before the
    /// firmware's first frame.
    explicit IfsLaneTruth(const std::string& name, const nlohmann::json& stored = nullptr,
                          bool settle = true)
        : tmp(name), reg(&printer.api, nullptr), backend(*reg) {
        if (stored.is_object()) {
            printer.client.mock_db_set("lane_data", "lane1", stored);
        }
        Ad5xIfsTestAccess::open_lane_record_store(backend);
        if (settle) {
            drain();
        }
        json_path = (tmp.path / "Adventurer5M.json").string();
        Ad5xIfsTestAccess::set_local_adventurer_json_path(backend, json_path);
        Ad5xIfsTestAccess::set_port_presence(backend, 0, true);
        firmware("#FF0000", "PLA");
    }

    /// The printer's file states @p hex / @p type for lane 1, and the backend reads it.
    void firmware(const std::string& hex, const std::string& type) {
        const std::string content =
            R"({"FFMInfo":{"ffmColor1":")" + hex + R"(","ffmType1":")" + type + R"("}})";
        { std::ofstream(json_path) << content; }
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);
    }

    [[nodiscard]] nlohmann::json record() {
        return printer.client.mock_db_get("lane_data", "lane1");
    }

    [[nodiscard]] nlohmann::json file() const {
        std::ifstream f(json_path);
        return nlohmann::json::parse(f, nullptr, false);
    }

    [[nodiscard]] bool pending() const {
        return helix::ams::lane_sources(backend.lane_id(0)).pending.has_value();
    }

    static void drain() {
        for (int i = 0; i < 4; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }
};

} // namespace helix::test
