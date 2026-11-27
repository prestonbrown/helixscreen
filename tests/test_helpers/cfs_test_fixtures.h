// tests/test_helpers/cfs_test_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What these test files share: their fixtures, helpers and includes.
// test_ams_backend_cfs.cpp, test_ams_backend_cfs_2.cpp, test_ams_backend_cfs_3.cpp,
// test_ams_backend_cfs_4.cpp, test_ams_backend_cfs_5.cpp, test_ams_backend_cfs_6.cpp. Generated
// from the first of those files by split_v3.py, which keeps every case's text and order and moves
// everything else here.
#pragma once

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_slot.h"
#include "ui_spool_canvas.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_cfs.h"
#include "ams_backend_mock.h"
#include "ams_remap.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_mapper.h"
#include "filament_op_dispatch.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_resolver.h"
#include "lane_translation.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "system/telemetry_manager.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/cfs_box_fixtures.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/filament_slot_override_store_test_access.h"
#include "test_helpers/force_bypass_controls_guard.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/toast_log.h"
#include "test_helpers/update_queue_test_access.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::test {

using namespace helix::printer;
using namespace helix;
using json = nlohmann::json;

inline std::optional<DeviceAction> find_action(AmsBackendCfs& b, const char* id) {
    for (auto& a : b.get_device_actions()) {
        if (a.id == id) {
            return a;
        }
    }
    return std::nullopt;
}

struct CfsTmpCacheDir {
    std::filesystem::path path;
    explicit CfsTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("cfs_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~CfsTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Capturing backend subclass — records execute_gcode / dispatch_action_script
// without a live Moonraker connection. Defined here (above the TEST_CASEs that
// use it) so both the #968 selection/material tests and the later remap tests
// can share it.
class CfsRemapHelper : public AmsBackendCfs {
  public:
    using AmsBackendCfs::AmsBackendCfs;
    CfsRemapHelper() : AmsBackendCfs(nullptr, nullptr) {}

    // Capture gcode instead of dispatching to a real Moonraker connection.
    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // The completion form (the Endless Spool switch) captures into the same list.
    // `reject` answers as Klipper does when a command raises: after the send.
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        captured.push_back(gcode);
        if (hold) {
            held_ack = std::move(on_complete);
        } else if (reject) {
            if (on_error) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::JSON_RPC_ERROR;
                err.message = "Unknown command";
                on_error(err);
            }
        } else if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }

    bool reject = false;
    bool hold = false; // Klipper's answer waits for the test to fire held_ack
    helix::SharedCallback<void()> held_ack;

    // Expose protected firmware-writeback helper for direct test calls.
    // push_slot_identity_to_firmware is protected (called from apply_user_edit
    // but not part of the public IAmsBackend surface).
    AmsError push(int slot, const std::string& material, const std::string& brand,
                  const std::string& catalog_id, uint32_t color_rgb) {
        SlotInfo info;
        info.material = material;
        info.brand = brand;
        info.catalog_id = catalog_id;
        info.color_rgb = color_rgb;
        return push_slot_identity_to_firmware(slot, info);
    }
    AmsError push_info(int slot, const SlotInfo& info) {
        return push_slot_identity_to_firmware(slot, info);
    }

    // Capture the assembled load/swap/unload script that change_tool /
    // load_filament / unload_filament hand to dispatch_action_script. Returns
    // success without touching a live Moonraker connection, so the WITH/WITHOUT-
    // material selection (#968) is observable from tests.
    AmsError dispatch_action_script(std::string gcode) override {
        dispatched.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // Expose the protected started flag so change_tool's check_preconditions()
    // passes without a live subscription.
    void mark_running() {
        running_ = true;
    }

    std::vector<std::string> captured;
    std::vector<std::string> dispatched;
};

// K1-dialect variant of CfsRemapHelper. The macro_variant_ is normally latched
// in the ctor from PrinterDetector; this subclass forces K1 so change_tool
// selection tests exercise the BOX_* (not CR_BOX_*) builders.
class CfsK1RemapHelper : public CfsRemapHelper {
  public:
    template <typename... Args>
    explicit CfsK1RemapHelper(Args&&... args) : CfsRemapHelper(std::forward<Args>(args)...) {
        CfsTestAccess::set_macro_variant_k1(*this);
    }
};

// CfsRemapHelper's gcode capture plus a REAL PrinterState wired behind a mock
// api, so tests can drive the print lifecycle the insert-probe gate reads. The
// plain helper passes a null api, where the lifecycle reads as unknown and
// never holds the machine (mirroring the production null-api path).
// api_ is patched after the base ctor because PrinterState must outlive the
// backend and a base-class init list has nowhere to declare it.
class CfsLifecycleHelper : public CfsRemapHelper {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;

    template <typename... Args>
    explicit CfsLifecycleHelper(Args&&... args) : CfsRemapHelper(std::forward<Args>(args)...) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        api_ = api.get();
    }
};

// The calibration flows ack through the on_complete overloads (a macro's
// completion is their terminal signal), so the capture helper must intercept
// those too, firing on_complete inline where the real path defers it to the
// main thread. CfsLiveAckBackend below covers that queue hop.
class CfsCalibHelper : public CfsLifecycleHelper {
  public:
    template <typename... Args>
    explicit CfsCalibHelper(Args&&... args) : CfsLifecycleHelper(std::forward<Args>(args)...) {
        CfsTestAccess::set_macro_variant_k1(*this);
        lv_subject_set_string(state.motion_state().get_homed_axes_subject(), "xyz");
    }

    // Keep the 1-arg capture visible beside these overrides.
    using CfsRemapHelper::execute_gcode;

    AmsError execute_gcode(const std::string& gcode,
                           helix::SharedCallback<void()> on_complete) override {
        captured.push_back(gcode);
        if (hold.count(gcode)) {
            held.push_back(std::move(on_complete));
            return AmsErrorHelper::success();
        }
        if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }

    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool) override {
        captured.push_back(gcode);
        if (fail.count(gcode)) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::JSON_RPC_ERROR;
            err.message = "Macro error: " + gcode;
            if (on_error) {
                on_error(err);
            }
            return AmsErrorHelper::success();
        }
        if (fail_later.count(gcode)) {
            held_failures.push_back(std::move(on_error));
            return AmsErrorHelper::success();
        }
        if (hold.count(gcode)) {
            held.push_back(std::move(on_complete));
            return AmsErrorHelper::success();
        }
        if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }

    /// Commands whose on_complete is captured unfired, so a test can advance
    /// the world (set a cancel flag, start a second flow) before releasing it.
    std::set<std::string> hold;
    /// Commands that answer on_error instead of on_complete, modelling the
    /// macro failing after the RPC acked.
    std::set<std::string> fail;
    std::vector<helix::SharedCallback<void()>> held;
    /// Commands whose on_error waits for fail_held(), so the world can change
    /// between the send and its failure.
    std::set<std::string> fail_later;
    std::vector<helix::SharedCallback<void(const MoonrakerError&)>> held_failures;

    void fail_held() {
        auto pending = std::move(held_failures);
        held_failures.clear();
        for (auto& cb : pending) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::JSON_RPC_ERROR;
            err.message = "Macro error";
            cb(err);
        }
    }

    void release_held() {
        auto pending = std::move(held);
        held.clear();
        for (auto& cb : pending) {
            if (cb) {
                cb();
            }
        }
    }
};

// The failure legs hop through the queue, and the live backend needs one pass
// per answer; drain_until_empty() repeats until the queue is empty.
inline void drain_calib_queue() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

/// Fills the process-wide MacroParamCache with exactly @p names for one scope,
/// and empties it again on the way out.
struct MacroCacheScope {
    explicit MacroCacheScope(const std::unordered_set<std::string>& names) {
        helix::MacroParamCache::instance().populate_from_configfile(json::object(), names);
    }
    ~MacroCacheScope() {
        helix::MacroParamCache::instance().clear();
    }
    MacroCacheScope(const MacroCacheScope&) = delete;
    MacroCacheScope& operator=(const MacroCacheScope&) = delete;
};

/// A K2, or with @p fork the Kalico port, whose printer defines exactly @p macros
/// and, with @p belt_motors, publishes the K2's motorized belt tensioner objects.
class CfsK2CalibHelper : public CfsCalibHelper {
  public:
    explicit CfsK2CalibHelper(const std::unordered_set<std::string>& macros, bool fork = false,
                              bool belt_motors = true)
        : macros_(macros) {
        fork ? CfsTestAccess::set_macro_variant_fork(*this)
             : CfsTestAccess::set_macro_variant_k2(*this);
        helix::PrinterDiscovery hw;
        hw.parse_objects(belt_motors ? json::array({"box", "belt_mdl mdlx", "belt_mdl mdly"})
                                     : json::array({"box"}));
        set_discovery(hw);
    }

  private:
    // After the base, whose mock client fills the cache with its own macros.
    MacroCacheScope macros_;
};

static json make_cfs_notification(const json& box_obj);

inline json make_flat_fork_box();

class CfsAutoRefillHelper : public CfsRemapHelper {
  public:
    using CfsRemapHelper::CfsRemapHelper;
    using CfsRemapHelper::execute_gcode;

    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        captured.push_back(gcode);
        if (reject) {
            if (on_error) {
                on_error(MoonrakerError{});
            }
        } else if (on_complete) {
            on_complete();
        }
        return AmsErrorHelper::success();
    }

    bool reject = false;
};

class CfsLiveAckBackend : public AmsBackendCfs {
  public:
    MockPrinter printer;

    CfsLiveAckBackend() : CfsLiveAckBackend(nullptr, nullptr) {
        running_ = true;
    }
    CfsLiveAckBackend(IMoonrakerAPI*, helix::IMoonrakerClient* client)
        : AmsBackendCfs(nullptr, client) {
        api_ = &printer.api;
        CfsTestAccess::set_macro_variant_k1(*this);
        lv_subject_set_string(printer.state.motion_state().get_homed_axes_subject(), "xyz");
    }
    ~CfsLiveAckBackend() override {
        drain_calib_queue();
    }
    void set_client_for_test(helix::IMoonrakerClient* client) {
        client_ = client;
    }
};

using helix::printer::CfsMaterialDb;
static json make_cfs_notification(const json& box_obj) {
    return json{{"params", json::array({json{{"box", box_obj}}, 0})}};
}

static json make_single_unit_box(const std::vector<std::string>& material_types,
                                 const std::vector<std::string>& color_values) {
    json box = json::parse(R"({
        "state": "connect",
        "filament": 0,
        "auto_refill": 1,
        "enable": 1,
        "filament_useup": 1,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {
            "state": "connect",
            "filament": "None",
            "temperature": "27",
            "dry_and_humidity": "48",
            "version": "1.1.3",
            "sn": "SERIAL",
            "change_color_num": ["-1", "-1", "-1", "-1"]
        }
    })");
    box["T1"]["material_type"] = material_types;
    box["T1"]["color_value"] = color_values;

    // Presence on real hardware comes from `vender` OR a positive `remain_len`,
    // NOT from color/material (which stay latched after a spool is removed).
    // Synthesize realistic per-slot vender AND remain_len that match the
    // occupancy each test expresses through its material/color arrays: a spool
    // with any non-sentinel material OR color reports vender "unknown" (present,
    // RFID vendor unresolved) and a real length; an all-sentinel slot reports
    // vender "none" and remain_len "-1" (empty bay).
    auto is_sentinel = [](const std::string& v) {
        return v.empty() || v == "-1" || v == "None" || v == "unknown";
    };
    json vender = json::array();
    json remain_len = json::array();
    for (size_t i = 0; i < 4; ++i) {
        const bool mat_present = i < material_types.size() && !is_sentinel(material_types[i]);
        const bool col_present = i < color_values.size() && !is_sentinel(color_values[i]);
        const bool present = mat_present || col_present;
        vender.push_back(present ? "unknown" : "none");
        remain_len.push_back(present ? "52" : "-1");
    }
    box["T1"]["vender"] = vender;
    box["T1"]["remain_len"] = remain_len;
    return box;
}

static json make_multi_unit_box(int unit_count) {
    json box = json{{"state", "connect"}, {"filament", 0},       {"auto_refill", 1},
                    {"enable", 1},        {"filament_useup", 1}, {"map", json::object()}};
    for (int u = 1; u <= unit_count; ++u) {
        box["T" + std::to_string(u)] =
            json{{"state", "connect"},
                 {"filament", "None"},
                 {"temperature", "27"},
                 {"dry_and_humidity", "48"},
                 {"version", "1.1.3"},
                 {"sn", "SERIAL"},
                 {"vender", json::array({"none", "none", "none", "none"})},
                 {"remain_len", json::array({"-1", "-1", "-1", "-1"})},
                 {"color_value", json::array({"-1", "-1", "-1", "-1"})},
                 {"material_type", json::array({"-1", "-1", "-1", "-1"})},
                 {"change_color_num", json::array({"-1", "-1", "-1", "-1"})}};
    }
    return box;
}

using helix::printer::AmsBackendCfs;
inline json make_flat_box(const std::string& material, const std::string& brand,
                          const std::string& name, const std::string& color) {
    json box;
    box["api_version"] = 1;
    box["driver_ready"] = true;
    box["loaded_slot"] = -1;
    box["slots"] = json::array({json{{"index", 0},
                                     {"external", false},
                                     {"present", true},
                                     {"material", material},
                                     {"brand", brand},
                                     {"name", name},
                                     {"color", color}},
                                json{{"index", 1}, {"external", false}, {"present", false}},
                                json{{"index", 2}, {"external", false}, {"present", false}},
                                json{{"index", 3}, {"external", false}, {"present", false}}});
    return box;
}

// Backend + store + tmp cache wired together — every override test repeats this.
// Flat-schema rig backend whose every gcode dispatch fails: an edit the box
// never receives.
class SyncCfs : public AmsBackendCfs {
  public:
    using AmsBackendCfs::AmsBackendCfs;
    using AmsBackendCfs::execute_gcode;
    // Klipper's answer, given at once: the send itself is the 1-arg form,
    // which the mock API runs.
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        AmsError err = execute_gcode(gcode);
        if (err.success()) {
            if (on_complete) {
                on_complete();
            }
        } else if (on_error) {
            MoonrakerError failed;
            failed.message = err.technical_msg;
            on_error(failed);
        }
        return err;
    }
};

class FlatFailDispatchCfs : public SyncCfs {
  public:
    FlatFailDispatchCfs(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
        : SyncCfs(api, client) {}

    using SyncCfs::execute_gcode;
    AmsError execute_gcode(const std::string&) override {
        return AmsErrorHelper::not_supported("fixture dispatch failure");
    }
};

template <typename BackendT = SyncCfs> struct CfsOverrideRig {
    CfsTmpCacheDir tmp;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    // Registered, because lane_id() answers kInvalidLaneId for a backend
    // AmsState does not know, and every lane funnel drops a record addressed
    // to that. Declared after api_ so it is torn down first: clear_backends()
    // destroys the backend, which still holds the api pointer.
    std::optional<helix::test::RegisteredWired<BackendT>> registration;
    helix::test::WiredBackend<BackendT>* backend = nullptr;

    explicit CfsOverrideRig(const std::string& name) : tmp(name) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        registration.emplace(api.get());
        backend = &**registration;
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(api.get(), "cfs");
        CfsTestAccess::inject_override_store(*backend, std::move(store));
    }

    void poll(const json& box) {
        backend->fire_status(make_cfs_notification(box));
    }

    /// The lane-record read that follows a save: each staged record is read
    /// back as the lane's Record, which echoes what the box holds no field for.
    void record_lands() {
        for (int slot = 0; slot < 4; ++slot) {
            if (const auto ovr = CfsTestAccess::get_override(*backend, slot)) {
                helix::test::file_override_as_lane_records(*backend, slot, *ovr);
            }
        }
    }
};

inline json make_runout_seated_box(int useup, const std::string& active) {
    return make_unit_box_explicit(
        /*material_types=*/{"unknown", "unknown", "101001", "101001"},
        /*color_values=*/{"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        /*venders=*/{"unknown", "unknown", "unknown", "unknown"},
        /*remain_lens=*/{"100", "0", "46", "50"}, useup, active);
}

// Bay D emptied: vender drops to the sentinel, RFID payload stays latched (so
// no fingerprint change ever fires check_hardware_event_clear - the removal
// path these tests exercise is exactly the one that must NOT clear identity).
inline json make_runout_removed_box(int useup, const std::string& active) {
    json box = make_runout_seated_box(useup, active);
    box["T1"]["vender"] = json::array({"unknown", "unknown", "unknown", "none"});
    return box;
}

// Link bay D's lane to Spoolman spool 137 the way the application does: the
// user states the binding, and the server states what spool 137 is. Binding
// and identity are separate statements - a commit that changes spoolman_id
// declares the binding alone, because the brand, colour and material that ride
// in with a link are the server's word and not a value anyone chose.
inline void link_lane_four_to_spool_137(AmsBackendCfs& backend) {
    SlotInfo edit = backend.get_slot_info(3);
    edit.material = "ASA-CF";
    edit.brand = "Elegoo";
    edit.spool_name = "Black ASA";
    edit.color_rgb = 0x1A1A1A;
    edit.spoolman_id = 137;
    edit.spoolman_filament_id = 99;
    edit.spoolman_vendor_id = 21;
    helix::test::edit_slot_as_user(backend, 3, edit);

    SpoolInfo spool;
    spool.id = 137;
    spool.filament_id = 99;
    spool.vendor_id = 21;
    spool.vendor = "Elegoo";
    spool.filament_name = "Black ASA";
    spool.material = "ASA-CF";
    spool.color_hex = "1A1A1A";
    helix::test::spool_states(backend, 3, spool);
}

inline json make_unbranded_bay_box(const std::string& bay_a_remain_len) {
    return make_unit_box_explicit({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"},
                                  {"none", "none", "none", "none"},
                                  {bay_a_remain_len, "-1", "-1", "-1"});
}

constexpr float kEditedRemainingG = 750.0f;

inline json make_runout_box(int filament_useup) {
    json box = json::parse(R"({
        "state": "connect",
        "filament": 1,
        "auto_refill": 1,
        "enable": 1,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {
            "state": "connect",
            "filament": "None",
            "temperature": "27",
            "dry_and_humidity": "35",
            "version": "1.1.3",
            "sn": "SERIAL",
            "mode": "0",
            "vender": ["unknown", "unknown", "unknown", "unknown"],
            "remain_len": ["100", "0", "46", "50"],
            "color_value": ["0FFFFFF", "01A1A1A", "01A1A1A", "01A1A1A"],
            "material_type": ["101001", "101001", "101001", "101001"],
            "change_color_num": ["0", "0", "-1", "-1"]
        }
    })");
    box["filament_useup"] = filament_useup;
    return box;
}

inline helix::ClassifyContext paused_ctx() {
    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    return ctx;
}

inline bool script_has_command(const std::string& script, const std::string& cmd) {
    std::istringstream lines(script);
    for (std::string line; std::getline(lines, line);) {
        const std::string token = line.substr(0, line.find_first_of(" \t"));
        if (token == cmd) {
            return true;
        }
    }
    return false;
}

class HeldAckCfs : public AmsBackendCfs {
  public:
    using AmsBackendCfs::AmsBackendCfs;
    using AmsBackendCfs::execute_gcode;
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        sent.push_back(gcode);
        held_ack = std::move(on_complete);
        held_error = std::move(on_error);
        return AmsErrorHelper::success();
    }
    void ack() {
        REQUIRE(held_ack);
        std::exchange(held_ack, nullptr)();
        helix::ui::UpdateQueue::instance().drain();
    }
    /// Klipper raises on the script instead of answering OK.
    void reject() {
        REQUIRE(held_error);
        MoonrakerError err;
        err.type = MoonrakerErrorType::JSON_RPC_ERROR;
        err.message = "key849";
        std::exchange(held_error, nullptr)(err);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    std::vector<std::string> sent;
    helix::SharedCallback<void()> held_ack;
    helix::SharedCallback<void(const MoonrakerError&)> held_error;
};

inline nlohmann::json toolhead_sensor_frame(bool detected) {
    return {{"params", nlohmann::json::array({{{"filament_switch_sensor filament_sensor",
                                                {{"filament_detected", detected}}}}})}};
}

inline json make_boot_box_all_units_down(const json& full) {
    json box = full;
    box["T1"]["state"] = "None";
    return box;
}

inline json make_unit_up_delta(const json& full) {
    return json{{"T1", full["T1"]}};
}

inline json make_empty_bus_box() {
    json box = json::parse(R"({"state": "disconnect", "filament": 0, "enable": 1,
                               "auto_refill": 0, "filament_useup": 1, "map": {}})");
    for (const char* unit : {"T1", "T2", "T3", "T4"}) {
        box[unit] = {{"state", "None"},
                     {"filament", "None"},
                     {"version", "-1"},
                     {"vender", {"-1", "-1", "-1", "-1"}},
                     {"remain_len", {"-1", "-1", "-1", "-1"}},
                     {"color_value", {"-1", "-1", "-1", "-1"}},
                     {"material_type", {"-1", "-1", "-1", "-1"}}};
    }
    return box;
}

inline json registered_unit_settings() {
    return json::parse(R"({"auto_addr": {"mb_addr_table_uniids":
        [[19, 103, 59, 72, 36, 34, 176, 3, 54, 53, 52, 52], [0], [0], [0]]}})");
}

inline json no_unit_settings() {
    return json::parse(R"({"auto_addr": {"mb_addr_table_uniids": [[0], [0], [0], [0]]}})");
}

/// T1 as a K2 Plus reports it once bus discovery reaches the unit.
inline json k2_plus_t1_connected() {
    return json::parse(R"({"state": "connect", "filament": "None", "filament_detected": "None",
        "mode": "0", "version": "1.1.3", "sn": "10000882925L125DBZC",
        "uuid": [19, 103, 59, 72, 36, 34, 176, 3, 54, 53, 52, 52],
        "vender": ["unknown", "unknown", "unknown", "unknown"],
        "remain_len": ["100", "100", "100", "100"],
        "color_value": ["0FFFFFF", "0C12E1F", "0101820", "0000000"],
        "material_type": ["101001", "101001", "101001", "101001"],
        "change_color_num": ["2", "1", "0", "0"]})");
}

/// The box a K2 Plus with its CFS attached publishes from Klipper startup: the
/// box state "None" and every address "None".
inline json k2_plus_startup_box() {
    json box = make_empty_bus_box();
    box["state"] = "None";
    return box;
}

/// A K2 that is never single-spool: no external_spool_only, no bypass, and the
/// toolhead filament claimed by no external spool.
inline void check_not_single_spool(const AmsBackendCfs& cfs) {
    const auto info = cfs.get_system_info();
    CHECK_FALSE(info.external_spool_only);
    CHECK(info.presents_filament_system());
    CHECK_FALSE(cfs.is_bypass_active());
    CHECK(info.current_slot != helix::ui::kExternalSpoolSlot);
}

/// The toolhead sensor reads filament, so a premature verdict would engage -2.
template <typename Backend> void feed_toolhead_filament(Backend& cfs) {
    CfsTestAccess::set_filament_sensor(cfs, /*seen=*/true, /*detected=*/true);
    cfs.fire_status(json{{"params", json::array({json::object(), 0})}});
}

struct UnansweredClientCfs : AmsBackendCfs {
    using AmsBackendCfs::AmsBackendCfs;
    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }
    void mark_running() {
        running_ = true;
    }
    std::vector<std::string> captured;
};

inline long configfile_queries(const helix::test::FakeMoonrakerClient& client) {
    return std::count_if(client.rpc_calls.begin(), client.rpc_calls.end(), [](const auto& c) {
        return c.params.contains("objects") && c.params["objects"].contains("configfile");
    });
}

inline json make_flat_fork_box() {
    return json::parse(R"({
        "api_version": 1, "data_ready": true, "driver_ready": true,
        "filament_detected": true, "filament_sensor_error": null,
        "humidity_pct": 34, "loaded_mask": 1, "loaded_slot": 0,
        "runout": {"chain": [1], "loaded_slot": 0},
        "runout_swap_enabled": true, "slot_filament_mask": 15,
        "state": "PRINT", "state_code": 2, "status": "OK", "status_code": 0,
        "temp_c": 23, "tracking_active": true, "unload_after_print_enabled": false,
        "slots": [
            {"index": 0, "external": false, "present": true, "loaded": true,
             "material": "PLA", "color": "#111111", "brand": "eSUN", "name": "Black PLA"},
            {"index": 1, "external": false, "present": true, "loaded": false,
             "material": "PLA", "color": "#F2F2F2", "brand": "eSUN", "name": "White PLA"},
            {"index": 2, "external": false, "present": true, "loaded": false,
             "material": "PETG", "color": "#0A2989", "brand": "Jayo", "name": "Blue PETG"},
            {"index": 3, "external": false, "present": true, "loaded": false,
             "material": "ABS", "color": "#C12E1F", "brand": "Jayo", "name": "Red ABS"}
        ]
    })");
}

inline json make_gapped_fork_box() {
    json box = make_flat_fork_box();
    json slots = json::array();
    for (int i = 4; i < 12; ++i) {
        slots.push_back({{"index", i},
                         {"external", false},
                         {"present", true},
                         {"loaded", i == 6},
                         {"material", "PLA"},
                         {"color", i == 9 ? "#111111" : "#F2F2F2"}});
    }
    slots.push_back({{"index", 12},
                     {"external", true},
                     {"present", true},
                     {"loaded", false},
                     {"material", ""},
                     {"color", ""}});
    box["slots"] = slots;
    box["loaded_slot"] = 6;
    box["runout"] = json{{"chain", json::array({8, 10})}, {"loaded_slot", 6}};
    return box;
}

inline json make_fork_box_with_boxes(const std::vector<int>& addresses, int loaded_slot = -1) {
    json box = make_flat_fork_box();
    json slots = json::array();
    int highest = -1;
    for (int address : addresses) {
        for (int local = 0; local < 4; ++local) {
            const int index = (address - 1) * 4 + local;
            highest = std::max(highest, index);
            slots.push_back({{"index", index},
                             {"external", false},
                             {"present", true},
                             {"loaded", index == loaded_slot},
                             {"material", "PLA"},
                             {"color", fmt::format("#{:06X}", 0x101010 * (index + 1))},
                             {"name", fmt::format("Box {} {}", address, char('A' + local))},
                             {"spoolman_id", 100 + index}});
        }
    }
    slots.push_back({{"index", highest + 1},
                     {"external", true},
                     {"present", false},
                     {"loaded", false},
                     {"material", ""},
                     {"color", ""}});
    box["slots"] = slots;
    box["loaded_slot"] = loaded_slot;
    box["runout"] = nullptr;
    return box;
}

using helix::test::GcodeRecordingApi;

/// CFS backend that reports the toolhead unhomed so ensure_homed_then() sends
/// its G28, and that skips the confirmation prompt the UI would normally raise.
class UnhomedCfsBackend : public AmsBackendCfs {
  public:
    UnhomedCfsBackend(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
        : AmsBackendCfs(api, client) {}

    bool toolhead_homed() const override {
        return false;
    }
};

struct CfsInsertRuleRig {
    CfsInsertRuleRig() : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state) {
        state.init_subjects(false);

        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
        CfsTestAccess::inject_override_store(backend, std::move(store));

        helix::ui::reset_insert_offers_for_test();
        helix::ui::set_test_toast_hook([this](ToastSeverity severity, const std::string& msg,
                                              uint32_t) { toasts.emplace_back(severity, msg); });

        // Boot with a tagged PETG spool seated and the box idle: the first
        // occupancy observation is the session's baseline, never an edge.
        json seated =
            make_single_unit_box({"100003", "-1", "-1", "-1"}, {"0FF5500", "-1", "-1", "-1"});
        seated["filament_useup"] = 0;
        backend.fire_status(make_cfs_notification(seated));
        drain();
        REQUIRE(toasts.empty());

        // The lane carries a user assignment for the spool; that is what the
        // notice asks about.
        SlotInfo edit = backend.get_slot_info(0);
        edit.color_rgb = 0x7EC8E3;
        helix::test::edit_slot_as_user(backend, 0, edit);

        // Pulling the spool is not an insert: the tag's material and colour
        // stay latched, only the occupancy fields drop.
        json pulled = json(seated);
        pulled["T1"]["vender"][0] = "none";
        pulled["T1"]["remain_len"][0] = "-1";
        backend.fire_status(make_cfs_notification(pulled));
        drain();
        REQUIRE(toasts.empty());

        // The frames every insert scenario starts from: the latched PETG
        // reading, an idle box, and the bay reporting occupied again.
        reinserted = json(seated);
        reinserted["filament_useup"] = 0;
    }

    ~CfsInsertRuleRig() {
        helix::ui::set_test_toast_hook(nullptr);
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    CfsTmpCacheDir tmp{"cfs_insert_rule"};
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPIMock api;
    helix::test::RegisteredWired<AmsBackendCfs> backend_reg{&api};
    helix::test::WiredBackend<AmsBackendCfs>& backend = *backend_reg;
    std::vector<std::pair<ToastSeverity, std::string>> toasts;
    json reinserted;
};

inline json per_slot_unit(const std::string& active_letter) {
    return json{
        {"state", "1"},
        {"version", "1.0.0"},
        {"sn", "CFS-TEST"},
        {"vender", json::array({"Creality", "Creality", "none", "none"})},
        {"remain_len", json::array({"240", "240", "-1", "-1"})},
        {"material_type", json::array({"101001", "101001", "-1", "-1"})},
        {"color_value", json::array({"FF0000", "00FF00", "-1", "-1"})},
        {"filament", active_letter},
    };
}

/// Single-unit box frame with T1 engaged on `active_letter`.
inline json per_slot_box(const std::string& active_letter) {
    return json{{"filament", 0}, {"T1", per_slot_unit(active_letter)}};
}

} // namespace helix::test
