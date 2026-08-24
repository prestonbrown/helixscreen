// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_cfs.h"
#include "ams_types.h"
#include "config.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_op_dispatch.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "macro_param_cache.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/cfs_test_access.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;

using json = nlohmann::json;

// Friend-class shim for FilamentSlotOverrideStore — same idiom as IFS /
// Snapmaker / ACE tests. Lets us redirect the store's on-disk read-cache to a
// per-test tmp dir so save_async doesn't pollute the developer's real
// helixscreen config.
class FilamentSlotOverrideStoreTestAccess {
  public:
    static void set_cache_directory(helix::ams::FilamentSlotOverrideStore& store,
                                    std::filesystem::path dir) {
        store.cache_dir_ = std::move(dir);
    }
};

// CfsTestAccess (friend shim for AmsBackendCfs) now lives in
// tests/test_helpers/cfs_test_access.h so test_ams_home_confirmation.cpp can
// share it instead of duplicating the class.

namespace {
// Per-test tmp cache dir — same idiom as test_ams_backend_snapmaker.cpp /
// test_ams_backend_ace.cpp.
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
    CfsRemapHelper() : AmsBackendCfs(nullptr, nullptr) {}

    // Capture gcode instead of dispatching to a real Moonraker connection.
    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // Expose protected firmware-writeback helper for direct test calls.
    // push_slot_identity_to_firmware is protected (called from set_slot_info
    // but not part of the public IAmsBackend surface).
    using AmsBackendCfs::push_slot_identity_to_firmware;

    // Capture the assembled load/swap/unload script that change_tool /
    // load_filament / unload_filament hand to dispatch_action_script. Returns
    // success without touching a live Moonraker connection, so the WITH/WITHOUT-
    // material selection (#968 Phase 2) is observable from tests.
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
    CfsK1RemapHelper() {
        CfsTestAccess::set_macro_variant_k1(*this);
    }
};
} // namespace

TEST_CASE("CFS Box profile clear is Fork-only", "[ams][cfs][fork]") {
    CfsRemapHelper backend;

    backend.clear_box_slot_profile(2);
    REQUIRE(backend.captured.empty());

    CfsTestAccess::set_macro_variant_fork(backend);

    backend.clear_box_slot_profile(2);

    REQUIRE(backend.captured == std::vector<std::string>{"_BOX_SLOT_CLEAR SLOT=2"});
}

// =============================================================================
// CFS bypass / external spool
// =============================================================================
//
// Two dialect answers, one per axis from FILAMENT_MANAGEMENT.md § CFS:
//  - Fork (community Kalico box.py): the firmware registers T<external_slot>
//    for the holder and BOX_UNLOAD's external branch ejects it, so bypass is
//    commanded with real gcode.
//  - Stock (K1 BOX_* / K2 CR_BOX_*): no Klipper-side command loads from the
//    holder (Creality's own UI drives the box over RS-485), so enabling is a
//    declaration confirmed by the toolhead filament sensor.

// Wrap a box object in the notify_status_update envelope. Defined with the
// status-parsing fixtures further down; forward-declared so the bypass tests
// above them can share it.
static json make_cfs_notification(const json& box_obj);

TEST_CASE("CFS bypass: fork dialect commands the attended load", "[ams][cfs][bypass]") {
    CfsRemapHelper backend;
    backend.mark_running();
    CfsTestAccess::set_flat_fork(backend, /*external_index=*/4);

    SECTION("enable dispatches T<external>") {
        auto err = backend.enable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        REQUIRE(backend.dispatched[0] == "T4");
    }

    SECTION("no external entry in the payload means no bypass") {
        CfsTestAccess::set_flat_fork(backend, /*external_index=*/-1);
        auto err = backend.enable_bypass();
        REQUIRE(err.result == AmsResult::NOT_SUPPORTED);
        REQUIRE(backend.dispatched.empty());
    }

    SECTION("disable while engaged dispatches BOX_UNLOAD") {
        // Engaged = firmware named the external entry in loaded_slot.
        CfsTestAccess::handle_status(backend, make_cfs_notification(json::parse(
                                                  R"({"api_version": 1, "loaded_slot": 4, "slots": [
                 {"index": 0, "external": false, "present": true},
                 {"index": 1, "external": false, "present": true},
                 {"index": 2, "external": false, "present": true},
                 {"index": 3, "external": false, "present": true},
                 {"index": 4, "external": true,  "present": true}]})")));
        REQUIRE(backend.is_bypass_active());

        auto err = backend.disable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        REQUIRE(backend.dispatched[0] == "BOX_UNLOAD");
    }

    SECTION("disable when not engaged is a no-op success") {
        auto err = backend.disable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.empty());
    }
}

TEST_CASE("CFS bypass: stock dialect declaration + sensor derivation", "[ams][cfs][bypass]") {
    CfsRemapHelper backend;
    backend.mark_running();
    // Stock dialect is the default schema; a full box frame is what flips
    // supports_bypass on.
    CfsTestAccess::handle_status(backend, make_cfs_notification(json::parse(
                                              R"({"state": "connect", "filament": 0, "enable": 1,
            "filament_useup": 0,
            "map": {"T1A": "T1A"},
            "T1": {"state": "connect", "filament": "None",
                   "vender": ["none"], "remain_len": ["-1"],
                   "color_value": ["-1"], "material_type": ["-1"]}})")));

    SECTION("first full box frame flips supports_bypass") {
        auto info = backend.get_system_info();
        REQUIRE(info.supports_bypass);
    }

    SECTION("enable stands the CFS down and declares; sensor feed confirms; "
            "sensor clear retracts") {
        auto err = backend.enable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        // No motion script exists for stock — but the box's print feed is
        // stood down so it cannot drive bay filament into the tube the
        // external spool occupies.
        REQUIRE(backend.captured == std::vector<std::string>{"BOX_ENABLE_CFS_PRINT ENABLE=0"});
        REQUIRE(backend.dispatched.empty());
        REQUIRE(backend.is_bypass_active()); // declaration half

        // Toolhead sensor sees hand-fed filament with no active lane.
        CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/true);
        CfsTestAccess::handle_status(backend, json{{"params", json::array({json::object(), 0})}});
        REQUIRE(backend.get_system_info().current_slot == -2);

        // Filament pulled back out: sentinel clears. The declaration itself
        // persists until the user toggles bypass off — is_bypass_active()
        // keeps reporting the declared half, matching the sidebar toggle.
        CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/false);
        CfsTestAccess::handle_status(backend, json{{"params", json::array({json::object(), 0})}});
        REQUIRE(backend.get_system_info().current_slot == -1);
        REQUIRE(backend.is_bypass_active());
    }

    SECTION("sensor never saw filament: no derivation") {
        CfsTestAccess::set_filament_sensor(backend, /*seen=*/false, /*detected=*/false);
        CfsTestAccess::handle_status(backend, json{{"params", json::array({json::object(), 0})}});
        REQUIRE(backend.get_system_info().current_slot == -1);
    }

    SECTION("disable re-arms the CFS and clears the declaration") {
        backend.enable_bypass();
        backend.captured.clear();

        auto err = backend.disable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.captured == std::vector<std::string>{"BOX_ENABLE_CFS_PRINT ENABLE=1"});
        REQUIRE_FALSE(backend.is_bypass_active());

        // Re-enable works after a disable.
        REQUIRE(backend.enable_bypass().result == AmsResult::SUCCESS);
    }

    SECTION("disable when fully off is a no-op — no spurious re-arm send") {
        auto err = backend.disable_bypass();
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.captured.empty());
        REQUIRE(backend.dispatched.empty());
    }

    SECTION("enable persists the declaration; disable clears it") {
        SettingsManager::instance().set_bypass_declared(false);
        backend.enable_bypass();
        REQUIRE(SettingsManager::instance().get_bypass_declared());

        backend.disable_bypass();
        REQUIRE_FALSE(SettingsManager::instance().get_bypass_declared());
    }

    SECTION("declaration survives a restart via on_started restore") {
        SettingsManager::instance().set_bypass_declared(false);
        backend.enable_bypass();
        REQUIRE(SettingsManager::instance().get_bypass_declared());

        // New backend instance = new process, same persisted settings.
        CfsRemapHelper restarted;
        CfsTestAccess::call_on_started(restarted);
        REQUIRE(restarted.is_bypass_active());

        SettingsManager::instance().set_bypass_declared(false); // test-env cleanup
    }

    SECTION("the box re-arming itself does NOT drop the declaration") {
        // This section used to assert the opposite, on the premise that enable=1
        // meant "someone re-enabled the CFS through Creality's own screen". That
        // premise is false and the frame below is why: it is the literal frame a
        // K2 Plus sent after re-arming with no host involved — no
        // BOX_ENABLE_CFS_PRINT in klippy.log, no disable_bypass() in ours (the
        // only thing that sends ENABLE=1), and the prints in that window were
        // plain Moonraker start_print calls from a third-party client.
        //
        // Because partial frames omit `enable`, the old rule only bit on the
        // first full frame after a restart — so bypass survived all session and
        // died on relaunch, while the external spool was still feeding the
        // nozzle and Unload greyed out.
        SettingsManager::instance().set_bypass_declared(false);
        backend.enable_bypass();
        REQUIRE(backend.is_bypass_active());

        CfsTestAccess::handle_status(
            backend, make_cfs_notification(json::parse(
                         R"({"state":"connect","filament":0,"auto_refill":0,"enable":1,
                "map":{"T1A":"T1A"},
                "T1":{"state":"connect","filament":"None","vender":["none"],
                      "remain_len":["-1"],"color_value":["-1"],
                      "material_type":["-1"]}})")));
        REQUIRE(backend.is_bypass_active());

        SettingsManager::instance().set_bypass_declared(false); // test-env cleanup
    }

    SECTION("bay filament at the toolhead blocks enable") {
        CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/true,
                                        /*current_slot=*/2);
        auto err = backend.enable_bypass();
        REQUIRE(err.result == AmsResult::WRONG_STATE);
        REQUIRE(backend.dispatched.empty());
    }
}

TEST_CASE("CFS auto-refill device action sends an explicit ENABLE", "[ams][cfs]") {
    // BOX_ENABLE_AUTO_REFILL is a setter, not a toggle: the handler reads
    // ENABLE via gcmd.get_int, and Creality's own master-server sends
    // ENABLE=1/0 explicitly on both families. The device action must invert
    // the last box-reported flag and send the spelled-out command.
    CfsRemapHelper backend;
    backend.mark_running();
    backend.captured.clear();

    SECTION("auto-refill currently off → ENABLE=1") {
        CfsTestAccess::set_loaded_state(backend, false, -1);
        {
            // Seed endless_spool_enabled=false via a box frame (auto_refill=0).
            CfsTestAccess::handle_status(
                backend, make_cfs_notification(json::parse(
                             R"({"state":"connect","filament":0,"auto_refill":0,"enable":1,
                    "map":{"T1A":"T1A"},
                    "T1":{"state":"connect","filament":"None","vender":["none"],
                          "remain_len":["-1"],"color_value":["-1"],
                          "material_type":["-1"]}})")));
        }
        backend.captured.clear();
        auto err = backend.execute_device_action("toggle_auto_refill");
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.captured == std::vector<std::string>{"BOX_ENABLE_AUTO_REFILL ENABLE=1"});
    }

    SECTION("auto-refill currently on → ENABLE=0") {
        CfsTestAccess::handle_status(
            backend, make_cfs_notification(json::parse(
                         R"({"state":"connect","filament":0,"auto_refill":1,"enable":1,
                "map":{"T1A":"T1A"},
                "T1":{"state":"connect","filament":"None","vender":["none"],
                      "remain_len":["-1"],"color_value":["-1"],
                      "material_type":["-1"]}})")));
        backend.captured.clear();
        auto err = backend.execute_device_action("toggle_auto_refill");
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(backend.captured == std::vector<std::string>{"BOX_ENABLE_AUTO_REFILL ENABLE=0"});
    }
}

TEST_CASE("CFS type enum", "[ams][cfs]") {
    SECTION("CFS is a valid AmsType") {
        auto t = AmsType::CFS;
        REQUIRE(t != AmsType::NONE);
    }

    SECTION("CFS is a filament system, not a tool changer") {
        REQUIRE(is_filament_system(AmsType::CFS));
        REQUIRE_FALSE(is_tool_changer(AmsType::CFS));
    }

    SECTION("ams_type_to_string returns CFS") {
        REQUIRE(std::string(ams_type_to_string(AmsType::CFS)) == "CFS");
    }

    SECTION("ams_type_from_string parses CFS variants") {
        REQUIRE(ams_type_from_string("cfs") == AmsType::CFS);
        REQUIRE(ams_type_from_string("CFS") == AmsType::CFS);
    }
}

TEST_CASE("CFS data model extensions", "[ams][cfs]") {
    SECTION("EnvironmentData defaults") {
        EnvironmentData env;
        REQUIRE(env.temperature_c == 0.0f);
        REQUIRE(env.humidity_pct == 0.0f);
    }

    SECTION("AmsUnit environment is optional") {
        AmsUnit unit;
        REQUIRE_FALSE(unit.environment.has_value());
        unit.environment = EnvironmentData{27.0f, 48.0f};
        REQUIRE(unit.environment->temperature_c == 27.0f);
        REQUIRE(unit.environment->humidity_pct == 48.0f);
    }

    SECTION("SlotInfo remaining length defaults to zero") {
        SlotInfo slot;
        REQUIRE(slot.remaining_length_m == 0.0f);
    }

    SECTION("SlotInfo environment is optional") {
        SlotInfo slot;
        REQUIRE_FALSE(slot.environment.has_value());
    }

    SECTION("AmsAlert fields") {
        AmsAlert alert;
        alert.message = "Nozzle clog detected";
        alert.hint = "Run a cold pull";
        alert.error_code = "CFS-845";
        alert.level = AmsAlertLevel::SYSTEM;
        REQUIRE(alert.level == AmsAlertLevel::SYSTEM);
        REQUIRE(alert.error_code == "CFS-845");
    }

    SECTION("AmsSystemInfo has alerts vector") {
        AmsSystemInfo info;
        REQUIRE(info.alerts.empty());
        info.alerts.push_back(AmsAlert{.message = "test",
                                       .hint = "fix it",
                                       .error_code = "CFS-831",
                                       .level = AmsAlertLevel::SYSTEM});
        REQUIRE(info.alerts.size() == 1);
    }
}

using helix::printer::CfsMaterialDb;

TEST_CASE("CFS material database", "[ams][cfs]") {
    // Material resolution now goes through FilamentCatalog's transient
    // "cfs"-coded slice (Task 5, filament-catalog merge) rather than the
    // retired CfsMaterialDb::instance()/lookup() material table.
    const auto catalog = FilamentCatalog::load_codes("cfs");

    SECTION("known material lookup") {
        const auto* info = catalog.resolve_code("cfs", "01001");
        REQUIRE(info != nullptr);
        REQUIRE(info->brand == "Creality");
        REQUIRE(info->name == "Hyper PLA");
        REQUIRE(info->type == "PLA");
        REQUIRE(info->nozzle_min == 190);
        REQUIRE(info->nozzle_max == 240);
    }

    SECTION("unknown material returns nullptr") {
        const auto* info = catalog.resolve_code("cfs", "99999");
        REQUIRE(info == nullptr);
    }

    SECTION("code stripping: 101001 to 01001 (K2, Creality prefix)") {
        auto id = CfsMaterialDb::strip_code("101001");
        REQUIRE(id == "01001");
    }

    SECTION("code stripping: K1 Generic prefix (#968)") {
        // K1C reporter tn_data.json: 000001 = PLA, 000003 = PETG — the
        // catalog ids are 00001 / 00003, so the leading '0' is the same
        // brand-prefix digit the K2 carries as '1'.
        REQUIRE(CfsMaterialDb::strip_code("000001") == "00001");
        REQUIRE(CfsMaterialDb::strip_code("000003") == "00003");
        REQUIRE(FilamentCatalog::load_codes("cfs").resolve_code("cfs", "00003") != nullptr);
    }

    SECTION("short code returned as-is") {
        auto id = CfsMaterialDb::strip_code("01001");
        REQUIRE(id == "01001");
    }

    SECTION("sentinel -1 returns empty") {
        auto id = CfsMaterialDb::strip_code("-1");
        REQUIRE(id.empty());
    }
}

TEST_CASE("CFS color parsing", "[ams][cfs]") {
    SECTION("parse 0RRGGBB format") {
        REQUIRE(CfsMaterialDb::parse_color("0C12E1F") == 0xC12E1F);
        REQUIRE(CfsMaterialDb::parse_color("0FFFFFF") == 0xFFFFFF);
        REQUIRE(CfsMaterialDb::parse_color("0000000") == 0x000000);
    }

    SECTION("sentinel values return default") {
        REQUIRE(CfsMaterialDb::parse_color("-1") == 0x808080);
        REQUIRE(CfsMaterialDb::parse_color("None") == 0x808080);
    }
}

TEST_CASE("CFS error decoding", "[ams][cfs]") {
    SECTION("known error code decodes to message and hint") {
        auto alert = CfsErrorDecoder::decode("key845", 0, -1);
        REQUIRE(alert.has_value());
        REQUIRE(alert->message == "Nozzle clog detected");
        REQUIRE_FALSE(alert->hint.empty());
        REQUIRE(alert->level == AmsAlertLevel::SYSTEM);
        REQUIRE(alert->error_code == "CFS-845");
    }

    SECTION("slot-level error includes slot index") {
        auto alert = CfsErrorDecoder::decode("key843", 0, 2);
        REQUIRE(alert.has_value());
        REQUIRE(alert->level == AmsAlertLevel::SLOT);
        REQUIRE(alert->slot_index == 2);
    }

    SECTION("unit-level error includes unit index") {
        auto alert = CfsErrorDecoder::decode("key853", 1, -1);
        REQUIRE(alert.has_value());
        REQUIRE(alert->level == AmsAlertLevel::UNIT);
        REQUIRE(alert->unit_index == 1);
    }

    SECTION("unknown error code returns nullopt") {
        auto alert = CfsErrorDecoder::decode("key999", 0, -1);
        REQUIRE_FALSE(alert.has_value());
    }
}

TEST_CASE("CFS error message+values decoding", "[ams][cfs]") {
    using nlohmann::json;

    SECTION("key849 splices unit/slot locator into message") {
        // Real telemetry shape observed 2026-05-05.
        json values = json::array({1, "B"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key849", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("Retract failed") != std::string::npos);
        REQUIRE(out->first.find("unit 1 slot B") != std::string::npos);
        // Hint passes through unchanged.
        REQUIRE(out->second.find("Manually pull") != std::string::npos);
    }

    SECTION("key851 (slot retract didn't reach) also gets unit/slot locator") {
        json values = json::array({2, "C"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key851", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("unit 2 slot C") != std::string::npos);
    }

    SECTION("key840 (unit-level) gets unit-only locator") {
        json values = json::array({3});
        auto out = CfsErrorDecoder::lookup_message_with_values("key840", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("on unit 3") != std::string::npos);
    }

    SECTION("key111 (cold extruder) surfaces pre-heat guidance") {
        json values = json::array();
        auto out = CfsErrorDecoder::lookup_message_with_values("key111", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("Pre-heat") != std::string::npos);
        REQUIRE(out->second.find("220") != std::string::npos);
        REQUIRE(out->second.find("PETG") != std::string::npos);
    }

    SECTION("key298 (system, no formatter) returns message untouched") {
        json values = json::array();
        auto out = CfsErrorDecoder::lookup_message_with_values("key298", values);
        REQUIRE(out.has_value());
        // No "unit" or "slot" appended.
        REQUIRE(out->first.find("unit") == std::string::npos);
        REQUIRE(out->first.find("slot") == std::string::npos);
    }

    SECTION("malformed values fall back to no locator (no regression)") {
        // Wrong shape — array but slot not a string.
        json values = json::array({1, 2});
        auto out = CfsErrorDecoder::lookup_message_with_values("key849", values);
        REQUIRE(out.has_value());
        REQUIRE(out->first.find("unit") == std::string::npos);
    }

    SECTION("unknown code returns nullopt") {
        json values = json::array({1, "A"});
        auto out = CfsErrorDecoder::lookup_message_with_values("key999", values);
        REQUIRE_FALSE(out.has_value());
    }
}

TEST_CASE("CFS slot addressing", "[ams][cfs]") {
    SECTION("global index to TNN name") {
        REQUIRE(CfsMaterialDb::slot_to_tnn(0) == "T1A");
        REQUIRE(CfsMaterialDb::slot_to_tnn(1) == "T1B");
        REQUIRE(CfsMaterialDb::slot_to_tnn(3) == "T1D");
        REQUIRE(CfsMaterialDb::slot_to_tnn(4) == "T2A");
        REQUIRE(CfsMaterialDb::slot_to_tnn(7) == "T2D");
        REQUIRE(CfsMaterialDb::slot_to_tnn(15) == "T4D");
    }

    SECTION("TNN name to global index") {
        REQUIRE(CfsMaterialDb::tnn_to_slot("T1A") == 0);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T1D") == 3);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T2A") == 4);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T4D") == 15);
    }

    SECTION("invalid TNN returns -1") {
        REQUIRE(CfsMaterialDb::tnn_to_slot("invalid") == -1);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T5A") == -1);
    }
}

// =============================================================================
// CFS backend status parsing tests
// =============================================================================

static nlohmann::json make_cfs_status_json() {
    return nlohmann::json::parse(R"({
        "box": {
            "state": "connect",
            "filament": 1,
            "auto_refill": 1,
            "enable": 1,
            "filament_useup": 1,
            "same_material": [
                ["101001", "0000000", ["T1A"], "PLA"],
                ["101001", "0FFFFFF", ["T1B"], "PLA"]
            ],
            "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
            "T1": {
                "state": "connect",
                "filament": "None",
                "temperature": "27",
                "dry_and_humidity": "48",
                "filament_detected": "None",
                "measuring_wheel": "None",
                "version": "1.1.3",
                "sn": "10000882925L125DBZC",
                "mode": "0",
                "vender": ["unknown", "unknown", "unknown", "unknown"],
                "remain_len": ["35", "57", "52", "52"],
                "color_value": ["0000000", "0FFFFFF", "00A2989", "0C12E1F"],
                "material_type": ["101001", "101001", "101001", "101001"],
                "uuid": [19, 103],
                "change_color_num": ["-1", "-1", "-1", "-1"]
            },
            "T2": {
                "state": "None",
                "filament": "None",
                "temperature": "None",
                "dry_and_humidity": "None",
                "filament_detected": "None",
                "measuring_wheel": "None",
                "version": "-1",
                "sn": "-1",
                "mode": "-1",
                "vender": ["-1", "-1", "-1", "-1"],
                "remain_len": ["-1", "-1", "-1", "-1"],
                "color_value": ["-1", "-1", "-1", "-1"],
                "material_type": ["-1", "-1", "-1", "-1"],
                "uuid": "None",
                "change_color_num": ["-1", "-1", "-1", "-1"]
            }
        }
    })");
}

// Wrap a box object in the notify_status_update envelope the backend expects.
// {"params": [{ "box": {...} }, timestamp]}
static json make_cfs_notification(const json& box_obj) {
    return json{{"params", json::array({json{{"box", box_obj}}, 0})}};
}

// Build a single-unit T1 box object with configurable per-slot material_type
// and color_value arrays. Other fields are held constant across tests so we
// can focus assertions on the RFID fingerprint path. `material_type[i]` and
// `color_value[i]` form the fingerprint for slot i.
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

using helix::printer::AmsBackendCfs;

TEST_CASE("CFS backend status parsing", "[ams][cfs]") {
    auto status = make_cfs_status_json();
    auto info = AmsBackendCfs::parse_box_status(status["box"]);

    SECTION("system-level fields") {
        REQUIRE(info.type == AmsType::CFS);
        // box.auto_refill=1 -> the ENABLE bit, which caps.enabled derives from.
        REQUIRE(info.endless_spool_enabled == true);
    }

    SECTION("connected unit created, disconnected skipped") {
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].name == "T1");
        REQUIRE(info.units[0].slot_count == 4);
        REQUIRE(info.total_slots == 4);
    }

    SECTION("unit environment data") {
        REQUIRE(info.units[0].environment.has_value());
        REQUIRE(info.units[0].environment->temperature_c == 27.0f);
        REQUIRE(info.units[0].environment->humidity_pct == 48.0f);
    }

    SECTION("unit hardware info") {
        REQUIRE(info.units[0].firmware_version == "1.1.3");
        REQUIRE(info.units[0].serial_number == "10000882925L125DBZC");
    }

    SECTION("slot colors parsed") {
        REQUIRE(info.units[0].slots[0].color_rgb == 0x000000);
        REQUIRE(info.units[0].slots[1].color_rgb == 0xFFFFFF);
        REQUIRE(info.units[0].slots[2].color_rgb == 0x0A2989);
        REQUIRE(info.units[0].slots[3].color_rgb == 0xC12E1F);
    }

    SECTION("slot materials resolved from database") {
        // material_type[0] == "101001" -> strip_code -> "01001", which
        // resolves in assets/filaments.json to Creality "Hyper PLA"
        // (type=PLA, nozzle_min=190, nozzle_max=240). Parity test for the
        // CfsMaterialDb -> FilamentCatalog decode-path migration (Task 5).
        REQUIRE(info.units[0].slots[0].material == "PLA");
        REQUIRE(info.units[0].slots[0].brand == "Creality");
        REQUIRE(info.units[0].slots[0].nozzle_temp_min > 0);
        REQUIRE(info.units[0].slots[0].nozzle_temp_max >= info.units[0].slots[0].nozzle_temp_min);
        REQUIRE(info.units[0].slots[0].nozzle_temp_min == 190);
        REQUIRE(info.units[0].slots[0].nozzle_temp_max == 240);
    }

    SECTION("slot remaining length") {
        REQUIRE(info.units[0].slots[0].remaining_length_m == 35.0f);
        REQUIRE(info.units[0].slots[1].remaining_length_m == 57.0f);
    }

    SECTION("slot status derived correctly") {
        REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
    }

    SECTION("topology is HUB") {
        REQUIRE(info.units[0].topology == PathTopology::HUB);
    }
}

// Presence regression (prestonbrown/helixscreen#1077). Covers the three cases
// CFS presence has to get right, all in one fixture:
//   A = tagged spool present  (vender set, RFID vendor unresolved → "unknown")
//   B = UNTAGGED spool present (vender sentinel, but a real remain_len)
//   C = genuinely empty bay    (vender sentinel, no length, but color/material
//                               still LATCHED from the last spool)
//   D = empty bay, zero length (remain_len "0" must not count as present)
// color_value/material_type latch after removal, so they must NOT drive
// presence (that faked the ghost slots); vender OR a positive remain_len does.
TEST_CASE("CFS presence: vender + remain_len combined signal (#1077)", "[ams][cfs]") {
    json box = json::parse(R"({
        "state": "connect", "filament": 1, "auto_refill": 1, "enable": 1, "filament_useup": 0,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {
            "state": "connect", "filament": "None", "temperature": "27", "dry_and_humidity": "40",
            "version": "1.1.3", "sn": "SERIAL", "mode": "0",
            "vender": ["unknown", "none", "none", "none"],
            "remain_len": ["-1", "42", "-1", "0"],
            "color_value": ["0FFFFFF", "0FF0000", "0C12E1F", "00A2989"],
            "material_type": ["unknown", "-1", "101001", "101001"],
            "change_color_num": ["-1", "-1", "-1", "-1"]
        }
    })");
    auto info = AmsBackendCfs::parse_box_status(box);
    REQUIRE(info.units.size() == 1);
    const auto& slots = info.units[0].slots;

    SECTION("tagged spool present (A)") {
        REQUIRE(slots[0].status == SlotStatus::AVAILABLE);
    }

    SECTION("untagged spool with remaining length is present, not empty (B)") {
        // The key case: vender is a sentinel ("none") but remain_len is real, so
        // an untagged 3rd-party spool must NOT parse EMPTY.
        REQUIRE(slots[1].status == SlotStatus::AVAILABLE);
        REQUIRE(slots[1].remaining_length_m == 42.0f);
    }

    SECTION("genuinely empty bay is EMPTY despite latched color/material (C, D)") {
        REQUIRE(slots[2].status == SlotStatus::EMPTY); // vender none, remain -1
        REQUIRE(slots[3].status == SlotStatus::EMPTY); // vender none, remain "0"
    }

    SECTION("empty bay scrubs latched color/material so no ghost renders (C)") {
        // Slot 2 carries a fully-latched color (0xC12E1F) and a resolvable
        // Creality material code (101001) — both cleared once the bay reads
        // empty (scrubbed color resolves to the 0x808080 sentinel).
        REQUIRE(slots[2].color_rgb == CfsMaterialDb::parse_color("-1"));
        REQUIRE(slots[2].material.empty());
        REQUIRE(slots[2].brand.empty());
    }

    SECTION("present bay with unresolved RFID vendor defaults brand to Creality (A)") {
        // Slot 0: vender "unknown" (tag present, vendor unresolved) + unmapped
        // material code. CFS RFID tags are Creality-only → brand resolves to
        // Creality rather than staying blank.
        REQUIRE(slots[0].brand == "Creality");
    }
}

TEST_CASE("CFS disconnected unit handling", "[ams][cfs]") {
    auto status = make_cfs_status_json();
    auto info = AmsBackendCfs::parse_box_status(status["box"]);

    SECTION("T2 is disconnected — not in units list") {
        for (const auto& unit : info.units) {
            REQUIRE(unit.name != "T2");
        }
    }
}

TEST_CASE("CFS GCode helpers", "[ams][cfs]") {
    // The CR_BOX_* primitives don't park the toolhead — without wrapping,
    // CR_BOX_FLUSH extrudes onto the build plate instead of into the K2's
    // waste port. These assertions enforce the SAVE_GCODE_STATE +
    // BOX_GO_TO_EXTRUDE_POS / BOX_MOVE_TO_SAFE_POS envelope that mirrors
    // the K2 stock screen's LOAD_MATERIAL macro chain.
    SECTION("load gcode uses CR_BOX commands with TNN, wrapped in park envelope") {
        const std::string expected_a = "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
                                       "BOX_SAVE_FAN\n"
                                       "BOX_GO_TO_EXTRUDE_POS\n"
                                       "BOX_MODE_WAIT\n"
                                       "CR_BOX_PRE_OPT\nCR_BOX_EXTRUDE TNN=T1A\n"
                                       "CR_BOX_WASTE\nCR_BOX_FLUSH TNN=T1A\nCR_BOX_END_OPT\n"
                                       "BOX_NOZZLE_CLEAN\n"
                                       "BOX_RESTORE_FAN\n"
                                       "BOX_MOVE_TO_SAFE_POS\n"
                                       "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::load_gcode(0) == expected_a);

        REQUIRE(AmsBackendCfs::load_gcode(1).find("TNN=T1B") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        // Envelope: mode-wait + fan save/restore around the op.
        // No BOX_SET_TEMP — we deliberately don't lower a hotter pre-set
        // extruder target (e.g. PETG @ 240°C); cold extruders surface a
        // friendly key111 modal instead.
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_SET_TEMP") == std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_MODE_WAIT") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_SAVE_FAN") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_RESTORE_FAN") != std::string::npos);
        // Load ends with fresh filament in the nozzle — wipe before parking.
        REQUIRE(AmsBackendCfs::load_gcode(1).find("BOX_NOZZLE_CLEAN") != std::string::npos);

        REQUIRE(AmsBackendCfs::load_gcode(4).find("TNN=T2A") != std::string::npos);
    }

    SECTION("unload gcode uses CR_BOX commands inside park envelope") {
        const std::string g = AmsBackendCfs::unload_gcode();
        REQUIRE(g.find("SAVE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_SET_TEMP") == std::string::npos);
        REQUIRE(g.find("CR_BOX_PRE_OPT\nCR_BOX_CUT\nBOX_MODE_WAIT\n"
                       "CR_BOX_RETRUDE\nCR_BOX_END_OPT") != std::string::npos);
        REQUIRE(g.find("BOX_RESTORE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        REQUIRE(g.find("RESTORE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        // Unload ends with retrude — nozzle is empty; no wipe needed.
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") == std::string::npos);
    }

    SECTION("swap gcode combines unload and load inside park envelope") {
        for (int idx : {0, 1, 3}) {
            const std::string g = AmsBackendCfs::swap_gcode(idx);
            REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
            REQUIRE(g.find("BOX_SET_TEMP") == std::string::npos);
            REQUIRE(g.find("CR_BOX_CUT\nBOX_MODE_WAIT\nCR_BOX_RETRUDE\n"
                           "BOX_MODE_WAIT\nCR_BOX_EXTRUDE TNN=") != std::string::npos);
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
            // Swap ends with flush of the new slot — wipe before parking.
            REQUIRE(g.find("BOX_NOZZLE_CLEAN") != std::string::npos);
        }
        // Invalid index
        REQUIRE(AmsBackendCfs::swap_gcode(-1).empty());
        REQUIRE(AmsBackendCfs::swap_gcode(16).empty());
    }

    SECTION("reset gcode") {
        REQUIRE(AmsBackendCfs::reset_gcode() == "BOX_ERROR_CLEAR");
    }

    SECTION("recover gcode") {
        using V = helix::printer::CfsMacroVariant;
        // K2 keeps the box-specific resume.
        REQUIRE(AmsBackendCfs::recover_gcode(V::K2) == "BOX_ERROR_RESUME_PROCESS");

        // K1 must NOT emit it: the K1 box extension registers no
        // cmd_error_resume_process (symbol-grepped from box_wrapper .so in
        // CR4CU220812S11 v2.3.5.34), so it returns "Unknown command" and the
        // box is never resumed. #1278.
        REQUIRE(AmsBackendCfs::recover_gcode(V::K1) != "BOX_ERROR_RESUME_PROCESS");
        REQUIRE(AmsBackendCfs::recover_gcode(V::K1) == "RESUME");

        // BOX_TNN_RETRY_PROCESS exists on K1 but is NOT a substitute — it
        // retries a specific tool change and needs TNN/LAST_TNN context.
        REQUIRE(AmsBackendCfs::recover_gcode(V::K1) != "BOX_TNN_RETRY_PROCESS");
    }
}

TEST_CASE("CFS K1 macro variant (#968)", "[ams][cfs]") {
    // K1 official CFS upgrade firmware uses plain BOX_* macros — no CR_ prefix,
    // no fan-save/mode-wait helpers. Reporter's K1C gcode/help shows
    // BOX_GO_TO_EXTRUDE_POS and BOX_MOVE_TO_SAFE_POS succeed but every
    // CR_BOX_* command returns key61 "Unknown command".
    using V = helix::printer::CfsMacroVariant;

    SECTION("K1 load gcode mirrors BOX_LOAD_MATERIAL_WITHOUT_MATERIAL (#968)") {
        // Fresh load, nozzle empty. Feed steps follow the firmware
        // BOX_LOAD_MATERIAL_WITHOUT_MATERIAL chain, with explicit TNN= on the
        // two commands that take it, and ADD the missing BOX_EXTRUDER_EXTRUDE
        // (root cause of "no auto-extrude after load"). BOX_MATERIAL_FLUSH is
        // bare: it takes LEN/VELOCITY/TEMP only, never TNN. Homing is handled
        // upstream by dispatch_action_script.
        const std::string expected_a = "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
                                       "BOX_SAVE_FAN\n"
                                       "BOX_ERROR_CLEAR\n"
                                       "BOX_CHECK_MATERIAL\n"
                                       "BOX_GO_TO_EXTRUDE_POS\n"
                                       "BOX_EXTRUDE_MATERIAL TNN=T1A\n"
                                       "BOX_EXTRUDER_EXTRUDE TNN=T1A\n"
                                       "BOX_MATERIAL_FLUSH\n"
                                       "BOX_NOZZLE_CLEAN\n"
                                       "BOX_RESTORE_FAN\n"
                                       "BOX_MOVE_TO_SAFE_POS\n"
                                       "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::load_gcode(0, V::K1) == expected_a);

        // No CR_BOX_* primitives on K1.
        const std::string g = AmsBackendCfs::load_gcode(1, V::K1);
        REQUIRE(g.find("CR_BOX_") == std::string::npos);
        REQUIRE(g.find("TNN=T1B") != std::string::npos);

        // The fix: BOX_EXTRUDER_EXTRUDE must appear between EXTRUDE and FLUSH.
        const auto pos_extrude = g.find("BOX_EXTRUDE_MATERIAL TNN=T1B");
        const auto pos_extruder = g.find("BOX_EXTRUDER_EXTRUDE TNN=T1B");
        const auto pos_flush = g.find("BOX_MATERIAL_FLUSH\n");
        REQUIRE(pos_extrude != std::string::npos);
        REQUIRE(pos_extruder != std::string::npos);
        REQUIRE(pos_flush != std::string::npos);
        REQUIRE(pos_extrude < pos_extruder);
        REQUIRE(pos_extruder < pos_flush);

        // K1 firmware lacks mode-wait — must not be emitted. Fan-save DOES
        // exist here and is required; see the envelope guard section (#1278).
        REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_RESTORE_FAN") != std::string::npos);
        REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);

        // Fresh load mirror must NOT cut (nozzle is empty).
        REQUIRE(g.find("BOX_CUT_MATERIAL") == std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") == std::string::npos);

        // Envelope helpers that K1 DOES support.
        REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") != std::string::npos);
    }

    SECTION("K1 load TNN covers full unit/slot range") {
        // K1 firmware (per K1-Max box.cfg) uses the same TNN encoding as K2.
        // Spot-check first slot of each unit + last slot to lock in the
        // CfsMaterialDb::slot_to_tnn translation under the K1 path.
        struct Case {
            int idx;
            const char* tnn;
        };
        for (const auto& c : {Case{0, "T1A"}, Case{1, "T1B"}, Case{3, "T1D"}, Case{4, "T2A"},
                              Case{8, "T3A"}, Case{12, "T4A"}, Case{15, "T4D"}}) {
            const std::string g = AmsBackendCfs::load_gcode(c.idx, V::K1);
            const std::string needle = std::string("TNN=") + c.tnn;
            INFO("idx=" << c.idx << " expected " << needle);
            REQUIRE(g.find(needle) != std::string::npos);
        }
        // Out-of-range stays empty (same contract as K2).
        REQUIRE(AmsBackendCfs::load_gcode(-1, V::K1).empty());
        REQUIRE(AmsBackendCfs::load_gcode(16, V::K1).empty());
    }

    SECTION("K1 unload gcode mirrors BOX_QUIT_MATERIAL (#968)") {
        // Mirrors the firmware BOX_QUIT_MATERIAL step list:
        // ERROR_CLEAR → CHECK_MATERIAL → CUT → RETRUDE → safe park.
        const std::string expected = "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
                                     "BOX_SAVE_FAN\n"
                                     "BOX_ERROR_CLEAR\n"
                                     "BOX_CHECK_MATERIAL\n"
                                     "BOX_CUT_MATERIAL\n"
                                     "BOX_RETRUDE_MATERIAL\n"
                                     "BOX_RESTORE_FAN\n"
                                     "BOX_MOVE_TO_SAFE_POS\n"
                                     "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::unload_gcode(V::K1) == expected);
        // Sanity: no K2-only macros leaked in.
        const std::string g = AmsBackendCfs::unload_gcode(V::K1);
        REQUIRE(g.find("CR_BOX_") == std::string::npos);
        REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
        // BOX_SAVE_FAN is NOT K2-only — it exists on K1 too (symbol-verified in
        // CR4CU220812S11_ota_img_V2.3.5.34) and the envelope now emits it.
        REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
        // Cut + retrude present.
        REQUIRE(g.find("BOX_CUT_MATERIAL") != std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") != std::string::npos);
        // Nozzle empty post-cut → no wipe.
        REQUIRE(g.find("BOX_NOZZLE_CLEAN") == std::string::npos);
    }

    SECTION("K1 swap gcode mirrors BOX_LOAD_MATERIAL_WITH_MATERIAL for slot 5 (T2B) (#968)") {
        // Nozzle loaded: cut old, retract, position, clean, then load new slot
        // with the missing BOX_EXTRUDER_EXTRUDE between EXTRUDE and FLUSH.
        // BOX_MATERIAL_FLUSH is bare — it has no TNN parameter.
        const std::string expected = "SAVE_GCODE_STATE NAME=helix_cfs_load\n"
                                     "BOX_SAVE_FAN\n"
                                     "BOX_ERROR_CLEAR\n"
                                     "BOX_CHECK_MATERIAL\n"
                                     "BOX_CUT_MATERIAL\n"
                                     "BOX_RETRUDE_MATERIAL\n"
                                     "BOX_GO_TO_EXTRUDE_POS\n"
                                     "BOX_NOZZLE_CLEAN\n"
                                     "BOX_EXTRUDE_MATERIAL TNN=T2B\n"
                                     "BOX_EXTRUDER_EXTRUDE TNN=T2B\n"
                                     "BOX_MATERIAL_FLUSH\n"
                                     "BOX_RESTORE_FAN\n"
                                     "BOX_MOVE_TO_SAFE_POS\n"
                                     "RESTORE_GCODE_STATE NAME=helix_cfs_load";
        REQUIRE(AmsBackendCfs::swap_gcode(5, V::K1) == expected);
        REQUIRE(expected.find("CR_BOX_") == std::string::npos);
    }

    SECTION("K1 swap spot checks across slot range") {
        for (int idx : {0, 1, 3, 5, 11, 15}) {
            const std::string g = AmsBackendCfs::swap_gcode(idx, V::K1);
            REQUIRE(g.find("CR_BOX_") == std::string::npos);
            REQUIRE(g.find("BOX_CUT_MATERIAL") != std::string::npos);
            REQUIRE(g.find("BOX_RETRUDE_MATERIAL") != std::string::npos);
            REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=") != std::string::npos);
            REQUIRE(g.find("BOX_EXTRUDER_EXTRUDE TNN=") != std::string::npos);
            REQUIRE(g.find("BOX_MATERIAL_FLUSH\n") != std::string::npos);
            // The missing extrude primitive must sit between EXTRUDE and FLUSH.
            REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=") < g.find("BOX_EXTRUDER_EXTRUDE TNN="));
            REQUIRE(g.find("BOX_EXTRUDER_EXTRUDE TNN=") < g.find("BOX_MATERIAL_FLUSH\n"));
            // Swap ends with flush of new slot — wipe before parking.
            REQUIRE(g.find("BOX_NOZZLE_CLEAN") != std::string::npos);
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        }
        REQUIRE(AmsBackendCfs::swap_gcode(-1, V::K1).empty());
        REQUIRE(AmsBackendCfs::swap_gcode(16, V::K1).empty());
    }

    SECTION("K1 envelope omits all K2-only helpers across all three operations") {
        // Regression guard. BOX_MODE_WAIT and the CR_BOX_* primitives are
        // genuinely absent from K1 and re-introducing them means key61 (#968).
        //
        // BOX_SAVE_FAN / BOX_RESTORE_FAN are NOT in that set, though this guard
        // once asserted they were. They are C-extension commands registered from
        // box_wrapper.cpython-38-mipsel-linux-gnu.so, never [gcode_macro]s, so
        // the box.cfg dump they were "verified absent" in could not have listed
        // them either way. Symbol grep of the extension in
        // CR4CU220812S11_ota_img_V2.3.5.34 shows cmd_save_fan / cmd_restore_fan
        // present. Omitting them left part-cooling blowing across the nozzle
        // through every K1 load, cut and flush (#1278).
        for (const std::string& g :
             {AmsBackendCfs::load_gcode(0, V::K1), AmsBackendCfs::unload_gcode(V::K1),
              AmsBackendCfs::swap_gcode(0, V::K1)}) {
            REQUIRE(g.find("BOX_SAVE_FAN") != std::string::npos);
            REQUIRE(g.find("BOX_RESTORE_FAN") != std::string::npos);
            // Suppress before the body runs, restore after it — never the
            // reverse, or the op runs with the fan on and ends with it off.
            REQUIRE(g.find("BOX_SAVE_FAN") < g.find("BOX_RESTORE_FAN"));
            // The restore must precede the park so a raise mid-park still
            // leaves the fan correct.
            REQUIRE(g.find("BOX_RESTORE_FAN") < g.find("BOX_MOVE_TO_SAFE_POS"));
            REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
            REQUIRE(g.find("CR_BOX_") == std::string::npos);
            // Save/restore + park are common to all three K1 operations.
            REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
            REQUIRE(g.find("SAVE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
            REQUIRE(g.find("RESTORE_GCODE_STATE NAME=helix_cfs_load") != std::string::npos);
        }
        // BOX_GO_TO_EXTRUDE_POS positions the toolhead over the waste port
        // before feeding fresh filament — load + swap need it, unload does NOT
        // (the BOX_QUIT_MATERIAL mirror only cuts + retracts, then parks).
        REQUIRE(AmsBackendCfs::load_gcode(0, V::K1).find("BOX_GO_TO_EXTRUDE_POS") !=
                std::string::npos);
        REQUIRE(AmsBackendCfs::swap_gcode(0, V::K1).find("BOX_GO_TO_EXTRUDE_POS") !=
                std::string::npos);
        REQUIRE(AmsBackendCfs::unload_gcode(V::K1).find("BOX_GO_TO_EXTRUDE_POS") ==
                std::string::npos);
    }

    SECTION("K1 BOX_MATERIAL_FLUSH is emitted bare — it has no TNN parameter (#968)") {
        // Genuine K1 CFS firmware (Creality OTA V2.3.5.34, board CR4CU220812S11):
        // the shipped box.cfg — byte-identical across all 11 per-model configs —
        // documents the parameter forms as
        //     BOX_EXTRUDE_MATERIAL TNN=T1A
        //     BOX_EXTRUDER_EXTRUDE TNN=T1A
        //     BOX_MATERIAL_FLUSH LEN=100 VELOCITY=360 TEMP=220
        //     BOX_RETRUDE_MATERIAL_WITH_TNN TNN=T1A
        // TNN is on three commands and deliberately not on the flush, and every
        // shipped BOX_LOAD_MATERIAL_* macro invokes the flush bare. Disassembly
        // of box_wrapper...so agrees: cmd_material_flush reads only LEN /
        // VELOCITY / TEMP and never touches TNN or the Tnn_map lookup.
        //
        // Sending TNN= is inert rather than fatal (Klipper's GCodeCommand
        // ignores unread parameters), which is exactly why it needs pinning —
        // nothing at runtime would ever complain if it came back.
        //
        // Extract each emitted flush line and require it to be exactly bare.
        auto flush_lines = [](const std::string& g) {
            std::vector<std::string> out;
            const std::string needle = "BOX_MATERIAL_FLUSH";
            for (size_t p = g.find(needle); p != std::string::npos; p = g.find(needle, p + 1)) {
                const size_t end = g.find('\n', p);
                out.push_back(g.substr(p, end == std::string::npos ? std::string::npos : end - p));
            }
            return out;
        };

        // Load and swap both flush; unload does not (nozzle is empty post-cut).
        for (int idx : {0, 1, 5, 15}) {
            for (const std::string& g :
                 {AmsBackendCfs::load_gcode(idx, V::K1), AmsBackendCfs::swap_gcode(idx, V::K1)}) {
                const auto lines = flush_lines(g);
                INFO("idx=" << idx << " gcode:\n" << g);
                REQUIRE(lines.size() == 1);
                REQUIRE(lines[0] == "BOX_MATERIAL_FLUSH");
            }
        }
        // Unload emits no flush at all.
        REQUIRE(flush_lines(AmsBackendCfs::unload_gcode(V::K1)).empty());

        // No K1 builder may put TNN on the flush, and none may reach for the
        // TNN-aware BOX_MATERIAL_CHANGE_FLUSH — it exists in this firmware but
        // no shipped macro uses it, so adopting it would be a behavior change.
        for (const std::string& g :
             {AmsBackendCfs::load_gcode(0, V::K1), AmsBackendCfs::unload_gcode(V::K1),
              AmsBackendCfs::swap_gcode(0, V::K1)}) {
            REQUIRE(g.find("BOX_MATERIAL_FLUSH TNN=") == std::string::npos);
            REQUIRE(g.find("BOX_MATERIAL_CHANGE_FLUSH") == std::string::npos);
        }

        // The commands that genuinely DO take TNN must keep it — this guard
        // must not be "fixed" by stripping TNN everywhere.
        const std::string load = AmsBackendCfs::load_gcode(5, V::K1);
        REQUIRE(load.find("BOX_EXTRUDE_MATERIAL TNN=T2B") != std::string::npos);
        REQUIRE(load.find("BOX_EXTRUDER_EXTRUDE TNN=T2B") != std::string::npos);
    }

    SECTION("K2 default preserved when variant omitted") {
        // Existing call sites without variant arg must still emit K2 macros.
        REQUIRE(AmsBackendCfs::load_gcode(0).find("CR_BOX_EXTRUDE") != std::string::npos);
        REQUIRE(AmsBackendCfs::unload_gcode().find("CR_BOX_CUT") != std::string::npos);
        REQUIRE(AmsBackendCfs::swap_gcode(0).find("CR_BOX_EXTRUDE") != std::string::npos);
    }
}

// =============================================================================
// #968 Phase 2 — WITH/WITHOUT-material selection keys on filament-at-nozzle,
// NOT a preloaded (cassette) slot. K1 CFS reports a preloaded slot index with
// the nozzle still EMPTY; cutting in that state is the reporter's "hallucinated
// cut on empty nozzle" bug.
// =============================================================================
TEST_CASE("CFS change_tool selects load-vs-swap from filament_loaded (#968)", "[ams][cfs][k1]") {
    CfsK1RemapHelper backend;
    backend.mark_running();

    SECTION("nozzle EMPTY but slot preloaded → fresh load (no cut)") {
        // The exact K1 CFS state from the report: current_slot >= 0 (a cassette
        // is staged) but filament_loaded == false (nothing at the nozzle yet).
        CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/false,
                                        /*current_slot=*/2);
        REQUIRE(backend.change_tool(0).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        const std::string& g = backend.dispatched[0];
        // Fresh load mirror — NO cut/retract.
        REQUIRE(g.find("BOX_CUT_MATERIAL") == std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") == std::string::npos);
        // Load primitives present, including the EXTRUDER_EXTRUDE fix.
        REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=T1A") != std::string::npos);
        REQUIRE(g.find("BOX_EXTRUDER_EXTRUDE TNN=T1A") != std::string::npos);
    }

    SECTION("filament physically at nozzle → swap (cut first)") {
        CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/true,
                                        /*current_slot=*/2);
        REQUIRE(backend.change_tool(0).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        const std::string& g = backend.dispatched[0];
        // Swap mirror — cut + retract the old filament before loading new.
        REQUIRE(g.find("BOX_CUT_MATERIAL") != std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") != std::string::npos);
        REQUIRE(g.find("BOX_EXTRUDE_MATERIAL TNN=T1A") != std::string::npos);
        REQUIRE(g.find("BOX_EXTRUDER_EXTRUDE TNN=T1A") != std::string::npos);
    }
}

// =============================================================================
// push_slot_identity_to_firmware (#968 material-type writeback)
// =============================================================================
//
// The push writes color_value always, and material_type ONLY when a code for
// the user's pick exists in the firmware-observed vocabulary harvested by
// handle_status_update (observed_material_*_). Codes are never synthesized —
// a value the firmware never reported could poison the wrapper's material-DB
// lookups (flush temps, same-material matching) and the stock LCD display.
//
// K1 codes below (000001 = Generic PLA, 000003 = Generic PETG) come from the
// #968 reporter's real tn_data.json / box dumps.
TEST_CASE("CFS push_slot_identity_to_firmware writes color + observed material code",
          "[ams][cfs][firmware_writeback][968]") {
    CfsRemapHelper helper;

    SECTION("no firmware-observed code → color-only + same-material refresh") {
        helper.push_slot_identity_to_firmware(0, "PETG", "Generic", "", 0xFF0000);
        // The follow-up BOX_UPDATE_SAME_MATERIAL_LIST mirrors Creality's own
        // master-server, which refreshes the auto-refill equivalence groups
        // after every BOX_MODIFY_TN_DATA write (group membership requires
        // exact color equality).
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0] ==
                "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0FF0000");
        REQUIRE(helper.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
        for (const auto& c : helper.captured) {
            REQUIRE(c.find("PART=material_type") == std::string::npos);
        }
    }

    SECTION("baseline present but code never observed → still color-only") {
        CfsTestAccess::set_last_rfid_uid(helper, 5, "101001|0FF0000");
        helper.push_slot_identity_to_firmware(5, "PETG", "Generic", "", 0x00FF00);
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0].find("PART=material_type") == std::string::npos);
        REQUIRE(helper.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
    }

    SECTION("K1: brand|type observed on another slot → material + color written") {
        // Slot 1 carries the PETG spool (K1 code 000003); the user edits
        // slot 0. The observed vocabulary is system-wide, not per-slot.
        CfsTestAccess::handle_status(
            helper, make_cfs_notification(make_single_unit_box({"-1", "000003", "-1", "-1"},
                                                               {"-1", "01B04AE", "-1", "-1"})));
        helper.push_slot_identity_to_firmware(0, "PETG", "Generic", "", 0xFF0000);
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0] ==
                "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=material_type DATA=000003\n"
                "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0FF0000");
        REQUIRE(helper.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
    }

    SECTION("type-only fallback when brand is unknown") {
        CfsTestAccess::handle_status(
            helper, make_cfs_notification(make_single_unit_box({"000003", "-1", "-1", "-1"},
                                                               {"01B04AE", "-1", "-1", "-1"})));
        helper.push_slot_identity_to_firmware(1, "PETG", "", "", 0x00FF00);
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0].find("PART=material_type DATA=000003") != std::string::npos);
        REQUIRE(helper.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
    }

    SECTION("catalog product pick resolves through its own cfs code id") {
        CfsTestAccess::handle_status(
            helper, make_cfs_notification(make_single_unit_box({"-1", "-1", "000003", "-1"},
                                                               {"-1", "-1", "0000000", "-1"})));
        helper.push_slot_identity_to_firmware(0, "PETG", "Generic", "generic-petg", 0xFF0000);
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0].find("PART=material_type DATA=000003") != std::string::npos);
        REQUIRE(helper.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
    }

    SECTION("unknown material → color-only even with a rich vocabulary") {
        CfsTestAccess::handle_status(
            helper, make_cfs_notification(make_single_unit_box({"000003", "-1", "-1", "-1"},
                                                               {"01B04AE", "-1", "-1", "-1"})));
        helper.push_slot_identity_to_firmware(2, "Unobtanium", "ACME", "", 0xFF0000);
        REQUIRE(helper.captured.size() == 2);
        REQUIRE(helper.captured[0].find("PART=material_type") == std::string::npos);
        REQUIRE(helper.captured[1] == "BOX_UPDATE_SAME_MATERIAL_LIST");
        REQUIRE(helper.captured[0].find("PART=material_type") == std::string::npos);
    }

    SECTION("invalid slot index skips the write (must not crash klippy)") {
        helper.push_slot_identity_to_firmware(99, "PETG", "Generic", "", 0xFF0000);
        REQUIRE(helper.captured.empty());
    }
}

// The two PART writes land as one script, but a status poll can slip between
// their echoes and observe the intermediate composite (new material + old
// color). expect_any_of must classify BOTH echoes as OwnWriteEcho so the
// fresh override survives its own writeback; a subsequent genuine change must
// still clear it.
TEST_CASE("CFS identity writeback echoes do not self-wipe the override",
          "[ams][cfs][firmware_writeback][968]") {
    CfsRemapHelper helper;

    // Slot 0 holds Generic PLA (K1 code 000001, NilsOF's dump color 09CFF4F);
    // slot 1 holds Generic PETG (000003), which is what puts PETG in the
    // firmware-observed vocabulary the push needs.
    CfsTestAccess::handle_status(
        helper, make_cfs_notification(make_single_unit_box({"000001", "000003", "-1", "-1"},
                                                           {"09CFF4F", "01B04AE", "-1", "-1"})));

    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PETG";
    ovr.brand = "Generic";
    ovr.color_rgb = 0xFF0000;
    ovr.color_set = true;
    ovr.user_locked_color = true;
    ovr.user_locked_material = true;
    CfsTestAccess::seed_override(helper, 0, ovr);

    // User picks PETG + red. Material (000003 via observed vocabulary) and
    // color both differ from the baseline.
    helper.push_slot_identity_to_firmware(0, "PETG", "Generic", "", 0xFF0000);
    REQUIRE(helper.captured.size() == 2);
    REQUIRE(helper.captured[0].find("PART=material_type DATA=000003") != std::string::npos);

    SECTION("intermediate echo (new material, old color) keeps the override") {
        CfsTestAccess::handle_status(
            helper, make_cfs_notification(make_single_unit_box({"000003", "-1", "-1", "-1"},
                                                               {"09CFF4F", "-1", "-1", "-1"})));
        REQUIRE(CfsTestAccess::get_override(helper, 0).has_value());
    }

    SECTION("final echo (new material + new color) keeps the override") {
        // Intermediate first, then final — the realistic poll sequence.
        CfsTestAccess::handle_status(
            helper, make_cfs_notification(make_single_unit_box({"000003", "-1", "-1", "-1"},
                                                               {"09CFF4F", "-1", "-1", "-1"})));
        CfsTestAccess::handle_status(
            helper, make_cfs_notification(make_single_unit_box({"000003", "-1", "-1", "-1"},
                                                               {"0FF0000", "-1", "-1", "-1"})));
        REQUIRE(CfsTestAccess::get_override(helper, 0).has_value());
    }

    SECTION("a value we never wrote still clears the override (real swap)") {
        CfsTestAccess::handle_status(
            helper, make_cfs_notification(make_single_unit_box({"000001", "-1", "-1", "-1"},
                                                               {"0FFFFFF", "-1", "-1", "-1"})));
        REQUIRE_FALSE(CfsTestAccess::get_override(helper, 0).has_value());
    }
}

TEST_CASE("CFS backend ctor latches macro variant from PrinterDetector (#968)", "[ams][cfs]") {
    // The constructor reads PrinterDetector::is_creality_k1() once and caches
    // the result. Verify both routes resolve correctly.
    auto* config = Config::get_instance();
    REQUIRE(config != nullptr);
    const std::string type_path = config->df() + "type";
    const std::string saved = config->get<std::string>(type_path, "");

    SECTION("K1C printer type → K1 dialect") {
        config->set<std::string>(type_path, "Creality K1C");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) == helix::printer::CfsMacroVariant::K1);
    }

    SECTION("K1 Max printer type → K1 dialect") {
        config->set<std::string>(type_path, "Creality K1 Max");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) == helix::printer::CfsMacroVariant::K1);
    }

    SECTION("K2 Plus printer type → K2 dialect") {
        config->set<std::string>(type_path, "Creality K2 Plus");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) == helix::printer::CfsMacroVariant::K2);
    }

    SECTION("Unset printer type → K2 dialect (safe default)") {
        config->set<std::string>(type_path, "");
        AmsBackendCfs backend(nullptr, nullptr);
        REQUIRE(CfsTestAccess::macro_variant(backend) == helix::printer::CfsMacroVariant::K2);
    }

    // Restore prior config so we don't bleed into adjacent tests.
    config->set<std::string>(type_path, saved);
}

TEST_CASE("PrinterDiscovery enables CFS for K1 box object (#968 gate)", "[ams][cfs][discovery]") {
    // Before 6ebe7417b: K1 + `box` → no CFS, warn log.
    // After the #968 fix flip: K1 + `box` → CFS enabled, K1 dialect chosen
    // downstream by AmsBackendCfs ctor.
    auto* config = Config::get_instance();
    REQUIRE(config != nullptr);
    const std::string type_path = config->df() + "type";
    const std::string saved = config->get<std::string>(type_path, "");

    SECTION("K1C + box object enables CFS") {
        config->set<std::string>(type_path, "Creality K1C");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "box", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_mmu());
        REQUIRE(discovery.get_mmu_type() == AmsType::CFS);
    }

    SECTION("K2 Plus + box object enables CFS (regression check)") {
        config->set<std::string>(type_path, "Creality K2 Plus");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "box", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_mmu());
        REQUIRE(discovery.get_mmu_type() == AmsType::CFS);
    }

    SECTION("No box object → no CFS regardless of printer type") {
        config->set<std::string>(type_path, "Creality K1C");
        helix::PrinterDiscovery discovery;
        nlohmann::json objects = {"configfile", "extruder", "heater_bed"};
        discovery.parse_objects(objects);
        REQUIRE_FALSE(discovery.has_mmu());
    }

    config->set<std::string>(type_path, saved);
}

TEST_CASE("Material comfort ranges", "[filament]") {
    auto range = filament::get_comfort_range("PLA");
    REQUIRE(range.has_value());
    REQUIRE(range->max_humidity_good == Catch::Approx(50.0f));
    REQUIRE(range->max_humidity_warn == Catch::Approx(65.0f));

    auto petg = filament::get_comfort_range("PETG");
    REQUIRE(petg.has_value());
    REQUIRE(petg->max_humidity_good == Catch::Approx(40.0f));

    REQUIRE_FALSE(filament::get_comfort_range("UNKNOWN_MATERIAL").has_value());
}

TEST_CASE("CFS backend has environment sensors", "[ams][cfs]") {
    // CFS units have built-in temperature and humidity sensors; every other
    // backend inherits AmsBackend's default of false, and ui_ams_detail.cpp
    // gates the whole environment card on this returning true.
    //
    // (The old body was REQUIRE(true) with a comment claiming a compile-time
    // check. AmsBackendCfs is perfectly constructible with a null API - the
    // rest of this file does it all over - so just ask the object.)
    AmsBackendCfs backend(nullptr, nullptr);

    static_assert(
        std::is_same_v<decltype(std::declval<const AmsBackendCfs&>().has_environment_sensors()),
                       bool>,
        "ui_ams_detail.cpp branches on a bool from has_environment_sensors()");

    REQUIRE(backend.has_environment_sensors());
}

// =============================================================================
// CFS active slot inference from box status
// =============================================================================

TEST_CASE("CFS parse_box_status infers active slot from tool map", "[ams][cfs]") {
    auto status = make_cfs_status_json();

    SECTION("box.filament is NOT treated as a loaded flag; tool map still parsed") {
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        // box.filament = 1 is a stale active-lane SELECTION index, not a
        // "filament loaded" truth. parse_box_status must NOT infer loaded-ness
        // from it — the toolhead sensor is the sole authority (Fix 1).
        REQUIRE(info.filament_loaded == false);
        REQUIRE(info.tool_to_slot_map.size() >= 1);
        REQUIRE(info.tool_to_slot_map[0] == 0); // T1A = slot 0
    }

    SECTION("box.filament = 0 also yields filament_loaded == false") {
        status["box"]["filament"] = 0;
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        REQUIRE(info.filament_loaded == false);
    }

    SECTION("tool map preserved across multiple slots") {
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        // Map: T1A→T1A(0), T1B→T1B(1), T1C→T1C(2), T1D→T1D(3)
        REQUIRE(info.tool_to_slot_map.size() == 4);
        REQUIRE(info.tool_to_slot_map[0] == 0);
        REQUIRE(info.tool_to_slot_map[1] == 1);
        REQUIRE(info.tool_to_slot_map[2] == 2);
        REQUIRE(info.tool_to_slot_map[3] == 3);
    }

    SECTION("active slot B → current_slot=1 and current_tool mirrors current_slot") {
        // CFS slots map 1:1 to tools; the print-status color dot reads
        // current_tool to label "T<n>". A hardcoded current_tool=0 would
        // mislabel the loaded slot.
        status["box"]["T1"]["filament"] = "B";
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        REQUIRE(info.current_slot == 1);
        REQUIRE(info.current_tool == info.current_slot);
    }

    SECTION("active slot D → current_tool follows current_slot (slot 3)") {
        status["box"]["T1"]["filament"] = "D";
        auto info = AmsBackendCfs::parse_box_status(status["box"]);
        REQUIRE(info.current_slot == 3);
        REQUIRE(info.current_tool == 3);
    }
}

// =============================================================================
// CFS BOX_MODIFY_TN tool remap (set_tool_mapping)
// =============================================================================

TEST_CASE("CFS set_tool_mapping emits BOX_MODIFY_TN with TNN keys/values", "[ams][cfs][remap]") {
    CfsRemapHelper helper;

    SECTION("Identity within unit 1: T1A → T1A") {
        auto err = helper.set_tool_mapping(0, 0);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured.size() == 1);
        REQUIRE(helper.captured[0] == "BOX_MODIFY_TN T1A=T1A");
    }

    SECTION("Cross-unit remap: tool 0 (T1A) → slot 5 (T2B)") {
        auto err = helper.set_tool_mapping(0, 5);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured == std::vector<std::string>{"BOX_MODIFY_TN T1A=T2B"});
    }

    SECTION("Last slot: tool 15 (T4D) → slot 0 (T1A)") {
        auto err = helper.set_tool_mapping(15, 0);
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(helper.captured == std::vector<std::string>{"BOX_MODIFY_TN T4D=T1A"});
    }

    SECTION("Tool number out of range rejected, no gcode sent") {
        auto err = helper.set_tool_mapping(16, 0);
        REQUIRE(err.result == AmsResult::INVALID_TOOL);
        REQUIRE(helper.captured.empty());

        err = helper.set_tool_mapping(-1, 0);
        REQUIRE(err.result == AmsResult::INVALID_TOOL);
        REQUIRE(helper.captured.empty());
    }

    SECTION("Slot index out of range rejected, no gcode sent") {
        auto err = helper.set_tool_mapping(0, 16);
        REQUIRE(err.result != AmsResult::SUCCESS);
        REQUIRE(helper.captured.empty());

        err = helper.set_tool_mapping(0, -1);
        REQUIRE(err.result != AmsResult::SUCCESS);
        REQUIRE(helper.captured.empty());
    }

    SECTION("Multiple calls send gcode in order") {
        REQUIRE(helper.set_tool_mapping(0, 1).result == AmsResult::SUCCESS);
        REQUIRE(helper.set_tool_mapping(2, 7).result == AmsResult::SUCCESS);
        REQUIRE(helper.captured ==
                std::vector<std::string>{"BOX_MODIFY_TN T1A=T1B", "BOX_MODIFY_TN T1C=T2D"});
    }
}

// =============================================================================
// CFS refresh_rfid → BOX_INFO_REFRESH RFID probe (prestonbrown/helixscreen#1077)
// =============================================================================
//
// Inserting a spool does not auto-read its RFID tag; the box reports sentinel
// vender/color/material until BOX_INFO_REFRESH scans it. The "Refresh RFID"
// device action probes every connected unit: ADDR = 1-based unit index, NUM=15
// (0b1111) = all four slots. Verified on K2 Plus.
TEST_CASE("CFS refresh_rfid probes each connected unit via BOX_INFO_REFRESH",
          "[ams][cfs][refresh]") {
    // Neutralize on_started() — its printer.objects.query needs a live client.
    struct Helper : CfsRemapHelper {
        void on_started() override {}
    };
    Helper h;

    SECTION("single connected unit → ADDR=1 NUM=15") {
        CfsTestAccess::set_connected_units(h, 1);
        auto err = h.execute_device_action("refresh_rfid", std::any{});
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(h.captured == std::vector<std::string>{"BOX_INFO_REFRESH ADDR=1 NUM=15"});
    }

    SECTION("two connected units → one probe each, ADDR follows unit index") {
        CfsTestAccess::set_connected_units(h, 2);
        auto err = h.execute_device_action("refresh_rfid", std::any{});
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(h.captured == std::vector<std::string>{"BOX_INFO_REFRESH ADDR=1 NUM=15",
                                                       "BOX_INFO_REFRESH ADDR=2 NUM=15"});
    }

    SECTION("no connected units → no gcode emitted") {
        CfsTestAccess::set_connected_units(h, 0);
        auto err = h.execute_device_action("refresh_rfid", std::any{});
        REQUIRE(err.result == AmsResult::SUCCESS);
        REQUIRE(h.captured.empty());
    }
}

// =============================================================================
// CFS BOX_MODIFY_TN_DATA color firmware-writeback (push_slot_identity_to_firmware)
// =============================================================================
//
// Format reverse-engineered from K2's master-server binary
// (`strings /usr/bin/master-server | grep BOX_MODIFY_TN_DATA`):
//   BOX_MODIFY_TN_DATA ADDR=<1..4> NUM=<A|B|C|D> PART=color_value DATA=0RRGGBB
//
// Validated round-trip on K2 Plus 2026-05-10 — see
// .claude/scratchpad/research/k2-box-firmware-writeback.md.
//
// CRITICAL: invalid args trigger box_wrapper TypeError → klippy invoke_shutdown.
// These tests cover the validation guards in push_slot_identity_to_firmware that
// prevent any out-of-range slot index or zero-color sentinel from reaching the
// firmware as a malformed gcode.

TEST_CASE("CFS push_slot_identity_to_firmware emits BOX_MODIFY_TN_DATA",
          "[ams][cfs][firmware_writeback]") {
    CfsRemapHelper helper;

    SECTION("Slot 0 (T1A) red 0xFF0000 → ADDR=1 NUM=A DATA=0FF0000") {
        helper.push_slot_identity_to_firmware(0, "", "", "", 0xFF0000);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0FF0000",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("Slot 5 (T2B) green 0x00FF00 → ADDR=2 NUM=B DATA=000FF00") {
        helper.push_slot_identity_to_firmware(5, "", "", "", 0x00FF00);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=2 NUM=B PART=color_value DATA=000FF00",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("Slot 15 (T4D) white 0xFFFFFF → ADDR=4 NUM=D DATA=0FFFFFF") {
        helper.push_slot_identity_to_firmware(15, "", "", "", 0xFFFFFF);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=4 NUM=D PART=color_value DATA=0FFFFFF",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("Color masks high bits — alpha byte from caller is ignored") {
        // If a caller passes 0xAA112233, the alpha byte is dropped. The
        // firmware's leading nibble is always our own '0'.
        helper.push_slot_identity_to_firmware(0, "", "", "", 0xAA112233);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0112233",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }
}

TEST_CASE("CFS push_slot_identity_to_firmware skips invalid inputs (must NOT crash klippy)",
          "[ams][cfs][firmware_writeback]") {
    CfsRemapHelper helper;

    SECTION("color_rgb == 0 (pure black) is a valid color and DOES dispatch") {
        // Pure black is a legitimate user choice and a real firmware-detected
        // color (K2 reports loaded black PLA as 0x000000). The push helper
        // must NOT skip it — that's the bug fixed by the color_set boolean.
        // Caller (set_slot_info) is responsible for not invoking when color
        // wasn't actually set (color_set=false on the override).
        helper.push_slot_identity_to_firmware(0, "", "", "", 0);
        REQUIRE(helper.captured ==
                std::vector<std::string>{
                    "BOX_MODIFY_TN_DATA ADDR=1 NUM=A PART=color_value DATA=0000000",
                    "BOX_UPDATE_SAME_MATERIAL_LIST"});
    }

    SECTION("Negative slot index is skipped") {
        helper.push_slot_identity_to_firmware(-1, "", "", "", 0xFF0000);
        REQUIRE(helper.captured.empty());
    }

    SECTION("slot_index >= 16 is skipped") {
        helper.push_slot_identity_to_firmware(16, "", "", "", 0xFF0000);
        REQUIRE(helper.captured.empty());
        helper.push_slot_identity_to_firmware(99, "", "", "", 0xFF0000);
        REQUIRE(helper.captured.empty());
    }
}

TEST_CASE("CFS set_tool_mapping updates local tool_to_slot_map", "[ams][cfs][remap]") {
    // PrintStartController::saved_tool_mapping_ snapshots get_tool_mapping()
    // before sending remaps and replays it on print end to restore the prior
    // configuration. If get_tool_mapping() returned an empty vector after a
    // successful set_tool_mapping, the restore path would be a no-op.
    CfsRemapHelper helper;
    auto status = make_cfs_status_json();
    json notification = {{"method", "notify_status_update"}, {"params", json::array({status, 0})}};
    CfsTestAccess::handle_status(helper, notification);

    auto baseline = helper.get_tool_mapping();
    REQUIRE_FALSE(baseline.empty()); // box.map populated by handle_status

    SECTION("a remap onto a connected lane moves BOTH directions") {
        // This payload connects T1 only — T2 reports state "None" — so the box
        // has four lanes, 0..3, and lane 3 is one of them.
        REQUIRE(helper.set_tool_mapping(1, 3).result == AmsResult::SUCCESS);

        auto after = helper.get_tool_mapping();
        REQUIRE(after.size() == baseline.size());
        REQUIRE(after[1] == 3);

        // The reverse direction has to follow, or the AMS panel badges one lane
        // while the filament panel's Load/Unload buttons act on another.
        auto info = helper.get_system_info();
        const auto* lane3 = info.get_slot_global(3);
        REQUIRE(lane3 != nullptr);
        CHECK(lane3->mapped_tool == 1);

        // Both losing sides give up their claim: lane 1 no longer answers to
        // T1, and T3 no longer resolves to the lane T1 just took.
        const auto* lane1 = info.get_slot_global(1);
        REQUIRE(lane1 != nullptr);
        CHECK(lane1->mapped_tool == -1);
        CHECK(after[3] == -1);
    }

    SECTION("a remap onto a lane the box has not reported is sent but not recorded") {
        // Slot 5 lives in unit T2, which this payload reports as state "None" —
        // not connected. The command still goes out, because firmware is the
        // authority and a unit we have not yet parsed a frame for may well be
        // there; the frame that describes it is what makes the mapping real.
        //
        // What must NOT happen is recording it locally. A forward entry naming
        // a lane with no SlotInfo has no mapped_tool to pair with, so the two
        // directions could never agree — and resolve_op_button_slot() would
        // hand the filament panel slot 5, which get_slot_global() answers with
        // nullptr. This assertion used to read `after[1] == 5`, from when the
        // forward map was written on its own and nothing had to match it.
        const size_t sent_before = helper.captured.size();
        REQUIRE(helper.set_tool_mapping(1, 5).result == AmsResult::SUCCESS);

        REQUIRE(helper.captured.size() == sent_before + 1);
        CHECK(helper.captured.back() == "BOX_MODIFY_TN T1B=T2B");

        auto after = helper.get_tool_mapping();
        CHECK(after == baseline);

        // The invariant, stated directly: every forward entry names a lane that
        // exists and claims that tool back.
        auto info = helper.get_system_info();
        for (int t = 0; t < static_cast<int>(after.size()); ++t) {
            int slot = after[static_cast<size_t>(t)];
            if (slot < 0) {
                continue;
            }
            INFO("T" << t << " resolves to slot " << slot);
            const auto* lane = info.get_slot_global(slot);
            REQUIRE(lane != nullptr);
            CHECK(lane->mapped_tool == t);
        }
    }
}

TEST_CASE("CFS get_tool_mapping_capabilities advertises editable", "[ams][cfs][remap]") {
    CfsRemapHelper helper;
    auto caps = helper.get_tool_mapping_capabilities();
    REQUIRE(caps.supported);
    REQUIRE(caps.editable);
    // Description is informational; non-empty so UI can show backend-specific copy
    REQUIRE_FALSE(caps.description.empty());
}

// =============================================================================
// CFS filament segment logic
// =============================================================================

TEST_CASE("CFS segment returns HUB for available slots", "[ams][cfs]") {
    auto status = make_cfs_status_json();
    auto info = AmsBackendCfs::parse_box_status(status["box"]);

    SECTION("available slots have HUB segment") {
        // All slots with filament (remain_len > 0) should be AVAILABLE
        REQUIRE(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        // get_slot_filament_segment returns HUB for AVAILABLE slots (tested via backend)
        // We can only test parse_box_status here since backend needs API
    }

    SECTION("empty slots have EMPTY status") {
        // Presence tracks `vender` + `remain_len`, not color: clear both signals
        // for the bay. The latched color_value stays populated, proving it is
        // not consulted.
        status["box"]["T1"]["vender"][0] = "none";
        status["box"]["T1"]["remain_len"][0] = "-1";
        auto info2 = AmsBackendCfs::parse_box_status(status["box"]);
        REQUIRE(info2.units[0].slots[0].status == SlotStatus::EMPTY);
    }
}

// =============================================================================
// CFS action state in operations
// =============================================================================

TEST_CASE("CFS GCode generation for all operations", "[ams][cfs]") {
    SECTION("load gcode uses TNN for multi-unit addressing") {
        // Unit 1 slots
        REQUIRE(AmsBackendCfs::load_gcode(0).find("TNN=T1A") != std::string::npos);
        REQUIRE(AmsBackendCfs::load_gcode(3).find("TNN=T1D") != std::string::npos);
        // Unit 4 last slot
        REQUIRE(AmsBackendCfs::load_gcode(15).find("TNN=T4D") != std::string::npos);
    }

    SECTION("load gcode rejects out of range") {
        REQUIRE(AmsBackendCfs::load_gcode(-1).empty());
        REQUIRE(AmsBackendCfs::load_gcode(16).empty());
    }
}

TEST_CASE("CFS has no per-slot prep sensors", "[ams][cfs]") {
    // CFS tracks slot inventory via material database (RFID/software), not
    // per-gate optical sensors. slot_has_prep_sensor must return false so the
    // filament path canvas draws continuous lines without sensor dot gaps.
    AmsBackendCfs backend(nullptr, nullptr);

    SECTION("all slots report no prep sensor") {
        for (int i = 0; i < 16; i++) {
            REQUIRE_FALSE(backend.slot_has_prep_sensor(i));
        }
    }
}

// ============================================================================
// Task 14: filament slot override integration
// ============================================================================

TEST_CASE("CFS override loaded at init is applied over firmware data",
          "[ams][cfs][filament_slot_override]") {
    // Seed an override in-memory and verify apply_overrides layers it over
    // firmware-parsed slot data. CFS's firmware populates brand /
    // color_name / total_weight_g from the RFID material DB, but the
    // override wins for every non-default field per the merge policy.
    CfsTmpCacheDir tmp("task14_override_applied");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite PLA Orange";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;
    ovr.material = "PLA";
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports slot 0 with DIFFERENT color and material code (firmware
    // material db lookup resolves code "101001" -> PLA/Creality).
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    auto info = backend.get_slot_info(0);
    // Override-eligible fields win.
    CHECK(info.brand == "Polymaker");
    CHECK(info.spool_name == "PolyLite PLA Orange");
    CHECK(info.spoolman_id == 42);
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0xFF5500u);
}

TEST_CASE("CFS migrates from helix-screen:cfs_slot_overrides on first startup",
          "[ams][cfs][filament_slot_override][migration]") {
    // Pre-Task-8 CFS wrote per-slot overrides to
    // helix-screen:cfs_slot_overrides. On first startup post-upgrade, the
    // store's load_blocking() migrates that data into lane_data and deletes
    // the legacy namespace. Tests through the store + MoonrakerAPIMock
    // directly so we don't need to drive on_started() (which requires a
    // started subscription backend).
    CfsTmpCacheDir tmp("task14_migration");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    // Seed legacy namespace with a PLA Orange override on slot 0. lane_data is
    // untouched -> forces migration.
    json legacy = {
        {"0",
         {
             {"brand", "Polymaker"},
             {"material", "PLA"},
             {"color_rgb", 0xFF5500},
             {"spoolman_id", 42},
             {"spool_name", "PolyLite Orange"},
         }},
    };
    api.mock_set_db_value("helix-screen", "cfs_slot_overrides", legacy);

    helix::ams::FilamentSlotOverrideStore store(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(store, tmp.path);
    auto loaded = store.load_blocking();

    // Migrated slot is returned from load_blocking as if it came from lane_data.
    REQUIRE(loaded.count(0) == 1);
    CHECK(loaded[0].brand == "Polymaker");
    CHECK(loaded[0].material == "PLA");
    CHECK(loaded[0].color_rgb == 0xFF5500u);
    CHECK(loaded[0].spoolman_id == 42);
    CHECK(loaded[0].spool_name == "PolyLite Orange");

    // lane_data now holds the AFC-shaped record.
    auto lane1 = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!lane1.is_null());
    CHECK(lane1["vendor"] == "Polymaker");
    CHECK(lane1["lane"] == "0");

    // Legacy namespace deleted post-migration.
    CHECK(api.mock_get_db_value("helix-screen", "cfs_slot_overrides").is_null());
}

TEST_CASE("CFS set_slot_info(persist=true) writes to store", "[ams][cfs][filament_slot_override]") {
    CfsTmpCacheDir tmp("task14_persist_true");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // Prime the backend with 4 slots so set_slot_info's index check passes.
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.spool_name = "PolyLite PLA Orange";
    edit.spoolman_id = 42;
    edit.remaining_weight_g = 850.0f;
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;

    auto err = backend.set_slot_info(0, edit, /*persist=*/true);
    REQUIRE(err.success());

    // In-memory map carries the override.
    auto staged = CfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(staged->color_rgb == 0xFF5500u);

    // Moonraker DB received the AFC-shaped record via save_async.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["vendor"] == "Polymaker");
    CHECK(stored["spool_id"] == 42);
    CHECK(stored["material"] == "PLA");
    CHECK(stored["color"] == "#FF5500");

    // Legacy namespace NOT touched — CFS no longer writes there.
    CHECK(api.mock_get_db_value("helix-screen", "cfs_slot_overrides").is_null());
}

TEST_CASE("CFS set_slot_info(persist=false) does NOT write to store",
          "[ams][cfs][filament_slot_override]") {
    CfsTmpCacheDir tmp("task14_persist_false");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    SlotInfo edit;
    edit.brand = "Draft";
    edit.material = "PLA";
    edit.color_rgb = 0x123456;

    auto err = backend.set_slot_info(0, edit, /*persist=*/false);
    REQUIRE(err.success());

    // The user's edit (brand="Draft", color=0x123456) was NOT persisted —
    // any override entry that exists came from the auto-mirror path firing
    // on the firmware-detected color/material in the prior status update,
    // which is independent of this preview edit. Verify by field:
    //   - brand: must NOT be "Draft" (auto-mirror only fills color/material)
    //   - color_rgb: must NOT be 0x123456 (the preview color)
    //   - color_set may be true if firmware reported a color (slot 0 is
    //     black, 0x000000); the auto-mirror records that as the firmware
    //     baseline, NOT as the user's preview.
    auto stored = CfsTestAccess::get_override(backend, 0);
    if (stored.has_value()) {
        CHECK(stored->brand != "Draft");
        CHECK(stored->color_rgb != 0x123456u);
    }
    auto db_record = api.mock_get_db_value("lane_data", "lane1");
    if (!db_record.is_null()) {
        // Auto-mirror's record does not contain the preview's brand/color.
        CHECK(db_record.value("vendor", "") != "Draft");
        CHECK(db_record.value("color", "") != "#123456");
    }

    // Preview edit still visible via get_slot_info (in-memory only).
    auto info = backend.get_slot_info(0);
    CHECK(info.brand == "Draft");
    CHECK(info.material == "PLA");
    CHECK(info.color_rgb == 0x123456u);
}

TEST_CASE("CFS RFID fingerprint change clears override (hardware swap detected)",
          "[ams][cfs][filament_slot_override]") {
    CfsTmpCacheDir tmp("task14_uid_swap_clears");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // Seed override AND the corresponding DB entry so we can verify
    // clear_async deletes it on swap.
    api.mock_set_db_value(
        "lane_data", "lane1",
        json{{"vendor", "Polymaker"}, {"spool_id", 42}, {"material", "PLA"}, {"color", "#FF5500"}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.material = "PLA";
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: material=101001, color=0FF5500 establishes the baseline.
    // No clear.
    json box1 = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                     {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box1));

    REQUIRE(CfsTestAccess::get_override(backend, 0).has_value());
    REQUIRE(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500");
    REQUIRE(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse: DIFFERENT fingerprint on slot 0 (material=102001, new
    // color) — physical swap detected.
    //
    // Sequence inside handle_status_update for this slot:
    //   1. check_hardware_event_clear fires clear_override_locked, which
    //      erases the user-set override (brand, spool_name, spoolman_id,
    //      material, color) AND deletes the lane_data record.
    //   2. mirror_firmware_to_lane_data is SKIPPED on this parse. A clear_async
    //      DELETE and a save_async POST against the same lane_data key are
    //      independently ordered, so firing both in one pass races — a DELETE
    //      landing second would drop the record we just published. The
    //      republish happens on the next parse instead.
    json box2 = make_single_unit_box({"102001", "101001", "101001", "101001"},
                                     {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box2));

    CHECK(CfsTestAccess::last_rfid_uid(backend, 0) == "102001|000FF00");

    // The swap parse leaves nothing behind: override erased, record deleted.
    CHECK_FALSE(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(api.mock_get_db_value("lane_data", "lane1").is_null());

    // Next parse (same new spool, unchanged fingerprint) republishes firmware
    // truth for the NEW spool.
    CfsTestAccess::handle_status(backend, make_cfs_notification(box2));

    // Override now holds the auto-mirrored firmware values (no user metadata):
    // new color, no spool_name / spoolman_id / brand from the wiped override.
    auto post_swap = CfsTestAccess::get_override(backend, 0);
    REQUIRE(post_swap.has_value());
    CHECK(post_swap->color_rgb == 0x00FF00u);
    CHECK(post_swap->spool_name.empty());
    CHECK(post_swap->spoolman_id == 0);
    CHECK(post_swap->brand.empty());
    // Auto-mirror writes must NOT claim to be user edits.
    CHECK_FALSE(post_swap->user_locked_color);
    CHECK_FALSE(post_swap->user_locked_material);

    // Orca sees the new spool's color, not stale user data.
    auto stored = api.mock_get_db_value("lane_data", "lane1");
    REQUIRE(!stored.is_null());
    CHECK(stored["color"] == "#00FF00");
    CHECK_FALSE(stored.contains("spool_name"));
    CHECK_FALSE(stored.contains("spool_id"));
    CHECK_FALSE(stored.contains("vendor"));

    // Override-exclusive fields reset on the live slot. Firmware-populated
    // fields (brand from material DB, color_rgb) stay — CFS firmware owns
    // those and they should reflect the new spool.
    auto info = backend.get_slot_info(0);
    CHECK(info.spool_name.empty());
    CHECK(info.spoolman_id == 0);
    CHECK(info.remaining_weight_g == -1.0f);
    // color_rgb was re-parsed from firmware this pass — should reflect new
    // spool's color (0x00FF00), not the old override (0xFF5500).
    CHECK(info.color_rgb == 0x00FF00u);
}

TEST_CASE("CFS first RFID observation does NOT clear override",
          "[ams][cfs][filament_slot_override]") {
    // Even when the override was saved against a different (now-stale)
    // fingerprint, the very first observation is a BASELINE and must never
    // fire a clear. Matches Snapmaker semantics.
    CfsTmpCacheDir tmp("task14_first_uid_baseline");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane1", json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports a fingerprint on the FIRST observation — no prior
    // baseline, so this must NOT trigger a clear. Override survives.
    json box = make_single_unit_box({"999001", "101001", "101001", "101001"},
                                    {"0123456", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    auto staged = CfsTestAccess::get_override(backend, 0);
    REQUIRE(staged.has_value());
    CHECK(staged->brand == "Polymaker");
    CHECK(staged->spoolman_id == 42);
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Second parse of the SAME fingerprint stays the baseline — no clear.
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("CFS empty RFID fingerprint does not update baseline or clear",
          "[ams][cfs][filament_slot_override]") {
    // Sentinel material_type "-1" / color_value "-1" = no tag / reader
    // disabled / unreadable. Must not update the baseline and must not clear.
    // This is the contract that keeps transient tag-read failures from
    // masking a genuine hardware swap on the next good read.
    CfsTmpCacheDir tmp("task14_empty_uid_noop");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    api.mock_set_db_value("lane_data", "lane1", json{{"vendor", "Polymaker"}, {"spool_id", 42}});

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spoolman_id = 42;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // First parse: valid fingerprint — baseline established.
    json box1 = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                     {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box1));
    REQUIRE(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500");

    // Second parse: slot 0 has SENTINEL material_type and color — empty
    // fingerprint. Must NOT update baseline and must NOT clear the override.
    json box2 = make_single_unit_box({"-1", "101001", "101001", "101001"},
                                     {"-1", "0FFFFFF", "00A2989", "0C12E1F"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box2));
    CHECK(CfsTestAccess::last_rfid_uid(backend, 0) == "101001|0FF5500"); // unchanged
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());

    // Third parse: same original fingerprint — matches baseline, no clear.
    // Proves the sentinel-UID pass didn't corrupt state.
    CfsTestAccess::handle_status(backend, make_cfs_notification(box1));
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
    CHECK(!api.mock_get_db_value("lane_data", "lane1").is_null());
}

TEST_CASE("CFS override preserved across unchanged parses", "[ams][cfs][filament_slot_override]") {
    // When the RFID fingerprint is unchanged (same spool re-observed), the
    // override must be re-applied on every parse. This is the core behavior
    // that was broken pre-Task-14: firmware data overwrote user edits on
    // every status notification.
    CfsTmpCacheDir tmp("task14_preserved_unchanged");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    helix::ams::FilamentSlotOverride ovr;
    ovr.brand = "Polymaker";
    ovr.spool_name = "PolyLite Orange";
    ovr.spoolman_id = 42;
    ovr.color_rgb = 0xFF5500;
    ovr.material = "PLA";
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Multiple parses with the SAME fingerprint — override must persist.
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});

    for (int i = 0; i < 3; ++i) {
        CfsTestAccess::handle_status(backend, make_cfs_notification(box));
        auto info = backend.get_slot_info(0);
        CHECK(info.brand == "Polymaker");
        CHECK(info.spool_name == "PolyLite Orange");
        CHECK(info.spoolman_id == 42);
        CHECK(info.material == "PLA");
        CHECK(info.color_rgb == 0xFF5500u);
    }

    // Override map itself survived.
    CHECK(CfsTestAccess::get_override(backend, 0).has_value());
}

// =============================================================================
// Presence/loaded-truth fixes. The firmware `box` object's RFID-derived fields
// (color_value/material_type) latch stale data and read sentinels for untagged
// 3rd-party spools, and box.filament is a SELECTION index — not a loaded flag.
// =============================================================================

// Fix 2: a latched/untagged color_value == "unknown" (with no remaining length)
// must NOT be treated as a present spool. A real hex color still reads AVAILABLE.
TEST_CASE("CFS parse: color_value 'unknown' is EMPTY, real hex is AVAILABLE", "[ams][cfs]") {
    // Slot 0: untagged spool — RFID reports sentinel "unknown" / "-1" with no
    // remaining length. Slot 1: a genuine hex color with remaining length.
    json box =
        make_single_unit_box({"-1", "101001", "-1", "-1"}, {"unknown", "0FFFFFF", "-1", "-1"});
    box["T1"]["remain_len"] = json::array({"-1", "57", "-1", "-1"});

    auto info = AmsBackendCfs::parse_box_status(box);
    REQUIRE(info.units.size() == 1);

    SECTION("'unknown' color → EMPTY") {
        REQUIRE(info.units[0].slots[0].status == SlotStatus::EMPTY);
    }
    SECTION("real hex color + length → AVAILABLE") {
        REQUIRE(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    }
}

// Fix 1: box.filament is a stale active-lane SELECTION pointer, NOT a loaded
// flag. With box.filament=1 but T1.filament="None" and no toolhead sensor, the
// system must report current_slot == -1 and filament_loaded == false.
TEST_CASE("CFS: box.filament selection index does not fake a loaded slot", "[ams][cfs]") {
    CfsTmpCacheDir tmp("presence_box_filament");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // box.filament = 1 (a lane is "selected"), but no lane is active
    // (T1.filament = "None") and there is no toolhead sensor in this update.
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    box["filament"] = 1;
    box["T1"]["filament"] = "None";
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_slot == -1);
    REQUIRE(sys.filament_loaded == false);
}

// Fix 1c regression guard: a partial top-level-only box update (e.g.
// box:{filament:N} when a lane is selected during a tool change) carries no
// per-unit lane data, so it must NOT clear a still-valid active slot. The clear
// is gated on has_unit_data.
TEST_CASE("CFS: partial box.filament update does not clear active slot", "[ams][cfs]") {
    CfsTmpCacheDir tmp("presence_partial_no_clobber");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // 1) Full update with lane A active → current_slot = 0.
    json full = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                     {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    full["T1"]["filament"] = "A";
    CfsTestAccess::handle_status(backend, make_cfs_notification(full));
    REQUIRE(backend.get_system_info().current_slot == 0);

    // 2) Partial top-level-only update (selection index changes, no T-unit
    //    data). Must leave the active slot untouched.
    json partial = json::object();
    partial["filament"] = 2;
    CfsTestAccess::handle_status(backend, make_cfs_notification(partial));

    REQUIRE(backend.get_system_info().current_slot == 0);
}

// Fix 1 regression guard: the toolhead sensor is the SOLE writer of
// filament_loaded. A sensor update sets it true; a SUBSEQUENT box-only update
// (carrying box.filament but no sensor param) must NOT clobber it back to false.
TEST_CASE("CFS: box-only update does not clobber sensor-derived filament_loaded", "[ams][cfs]") {
    CfsTmpCacheDir tmp("presence_sensor_authority");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // 1) Toolhead sensor trips: filament present at the nozzle.
    json sensor_update =
        json{{"params", json::array({json{{"filament_switch_sensor filament_sensor",
                                           {{"filament_detected", true}}}},
                                     0})}};
    CfsTestAccess::handle_status(backend, sensor_update);
    REQUIRE(backend.get_system_info().filament_loaded == true);

    // 2) A box-only update (no sensor param) arrives. box.filament is present
    //    but is a selection index, not a loaded flag — it must NOT reset
    //    filament_loaded.
    json box = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                    {"0000000", "0FFFFFF", "00A2989", "0C12E1F"});
    box["filament"] = 0;
    box["T1"]["filament"] = "None";
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    REQUIRE(backend.get_system_info().filament_loaded == true);
}

// Fix 3: trust the user's assignment. An untagged spool always reads RFID -1,
// so firmware reports the bay EMPTY. When the user has assigned filament to
// that bay (override carries real data), the bay is PRESENT (AVAILABLE).
TEST_CASE("CFS: user override promotes an RFID-empty bay to AVAILABLE", "[ams][cfs]") {
    CfsTmpCacheDir tmp("presence_override_trust");
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    AmsBackendCfs backend(&api, nullptr);
    auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(&api, "cfs");
    FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
    CfsTestAccess::inject_override_store(backend, std::move(store));

    // User assigned PLA to slot 0 (untagged spool). Locked, exactly as
    // set_slot_info(persist=true) records a user edit — an UNLOCKED record
    // would be an auto-mirror leftover, which the empty bay is entitled to
    // clear (clear_stale_override_on_removal_locked).
    helix::ams::FilamentSlotOverride ovr;
    ovr.material = "PLA";
    ovr.user_locked_material = true;
    CfsTestAccess::seed_override(backend, 0, ovr);

    // Firmware reports slot 0 EMPTY (RFID -1 / no length), slots 1-3 EMPTY too.
    json box = make_single_unit_box({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"});
    box["T1"]["remain_len"] = json::array({"-1", "-1", "-1", "-1"});
    CfsTestAccess::handle_status(backend, make_cfs_notification(box));

    SECTION("assigned bay is promoted to AVAILABLE") {
        REQUIRE(backend.get_slot_info(0).status == SlotStatus::AVAILABLE);
    }
    SECTION("control: unassigned empty bay stays EMPTY") {
        REQUIRE(backend.get_slot_info(1).status == SlotStatus::EMPTY);
    }
}

// =============================================================================
// Self-wipe guard: firmware echoing back our own BOX_MODIFY_TN_DATA color push
// must not read as a physical spool swap.
//
// set_slot_info(persist=true) both stages the user's override AND rewrites
// firmware's color_value so the stock LCD agrees. color_value is half of the
// RFID fingerprint check_hardware_event_clear watches, so without a guard the
// echo of our own write cleared the override the user had just created — the
// user's material silently vanished on the next restart. Because lane_data IS
// the override store, that also fed OrcaSlicer stale filament info.
// =============================================================================

namespace {

// Box builder with explicit control over `vender` and `remain_len`. The shared
// make_single_unit_box() synthesizes those from material/color presence, which
// is exactly the coupling these tests need to break: a REMOVED tagged spool has
// sentinel vender alongside latched material/color/remain_len.
json make_unit_box_explicit(const std::vector<std::string>& material_types,
                            const std::vector<std::string>& color_values,
                            const std::vector<std::string>& venders,
                            const std::vector<std::string>& remain_lens) {
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
    box["T1"]["vender"] = venders;
    box["T1"]["remain_len"] = remain_lens;
    return box;
}

// Backend + store + tmp cache wired together — every override test repeats this.
struct CfsOverrideRig {
    CfsTmpCacheDir tmp;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<AmsBackendCfs> backend;

    explicit CfsOverrideRig(const std::string& name) : tmp(name) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        backend = std::make_unique<AmsBackendCfs>(api.get(), nullptr);
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(api.get(), "cfs");
        FilamentSlotOverrideStoreTestAccess::set_cache_directory(*store, tmp.path);
        CfsTestAccess::inject_override_store(*backend, std::move(store));
    }

    void poll(const json& box) {
        CfsTestAccess::handle_status(*backend, make_cfs_notification(box));
    }
};

} // namespace

TEST_CASE("CFS user edit survives the firmware echo of our own color push",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    CfsOverrideRig rig("cfs_echo_survives");

    // Slot 0 holds a tagged spool: material 101001, color 0FF5500. First poll
    // establishes the fingerprint baseline "101001|0FF5500".
    json box_before = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                           {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    rig.poll(box_before);
    REQUIRE(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "101001|0FF5500");

    // User assigns ASA-CF / dark gray. This stages the override AND pushes
    // BOX_MODIFY_TN_DATA ... DATA=01A1A1A to the box.
    SlotInfo edit;
    edit.material = "ASA-CF";
    edit.color_name = "Dark Gray";
    edit.color_rgb = 0x1A1A1A;
    REQUIRE(rig.backend->set_slot_info(0, edit, /*persist=*/true).success());

    auto staged = CfsTestAccess::get_override(*rig.backend, 0);
    REQUIRE(staged.has_value());
    REQUIRE(staged->material == "ASA-CF");
    REQUIRE(staged->user_locked_color);
    REQUIRE(staged->user_locked_material);

    SECTION("polls before the echo lands leave the override untouched") {
        // The gcode is queued asynchronously, so firmware keeps reporting the
        // OLD color for an unknown number of polls. Those must read Unchanged —
        // a guard that overwrote the baseline outright would instead see a
        // change here and clear on the very first poll.
        for (int i = 0; i < 3; ++i) {
            rig.poll(box_before);
            auto ovr = CfsTestAccess::get_override(*rig.backend, 0);
            REQUIRE(ovr.has_value());
            CHECK(ovr->material == "ASA-CF");
            CHECK(ovr->color_rgb == 0x1A1A1Au);
        }
        CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "101001|0FF5500");
    }

    SECTION("the echo itself is not a swap and the edit survives every later poll") {
        // Firmware applies the write: color_value becomes our pushed 01A1A1A.
        // material_type is untouched — nothing writes it.
        json box_echo = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                             {"01A1A1A", "0FFFFFF", "00A2989", "0C12E1F"});

        // Survival across MANY polls, not just the first: the override backs
        // lane_data, which is what OrcaSlicer reads.
        for (int i = 0; i < 5; ++i) {
            rig.poll(box_echo);

            auto ovr = CfsTestAccess::get_override(*rig.backend, 0);
            REQUIRE(ovr.has_value());
            CHECK(ovr->material == "ASA-CF");
            CHECK(ovr->color_rgb == 0x1A1A1Au);
            CHECK(ovr->user_locked_color);
            CHECK(ovr->user_locked_material);

            auto info = rig.backend->get_slot_info(0);
            CHECK(info.material == "ASA-CF");
            CHECK(info.color_rgb == 0x1A1A1Au);
        }

        // The baseline advanced to the echoed fingerprint, so a LATER genuine
        // swap is still measured against firmware truth.
        CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "101001|01A1A1A");

        // lane_data still carries the user's material + color for Orca.
        auto stored = rig.api->mock_get_db_value("lane_data", "lane1");
        REQUIRE(!stored.is_null());
        CHECK(stored["material"] == "ASA-CF");
        CHECK(stored["color"] == "#1A1A1A");
    }

    SECTION("a genuine swap after the echo still clears the override") {
        json box_echo = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                             {"01A1A1A", "0FFFFFF", "00A2989", "0C12E1F"});
        rig.poll(box_echo);
        REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

        // Different physical spool: different material code AND color.
        json box_swap = make_single_unit_box({"102001", "101001", "101001", "101001"},
                                             {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"});
        rig.poll(box_swap);
        CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    }
}

TEST_CASE("CFS genuine swap while a color push is in flight still clears the override",
          "[ams][cfs][filament_slot_override][firmware_writeback]") {
    // The echo guard must not blind swap detection during the window between
    // dispatching BOX_MODIFY_TN_DATA and firmware acknowledging it.
    CfsOverrideRig rig("cfs_swap_during_push");

    json box_before = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                           {"0FF5500", "0FFFFFF", "00A2989", "0C12E1F"});
    rig.poll(box_before);

    SlotInfo edit;
    edit.material = "ASA-CF";
    edit.color_rgb = 0x1A1A1A;
    REQUIRE(rig.backend->set_slot_info(0, edit, /*persist=*/true).success());
    REQUIRE(CfsTestAccess::get_override(*rig.backend, 0).has_value());

    // Before the echo arrives the user yanks the spool and inserts another one.
    // The fingerprint changes to something we never asked for.
    json box_swap = make_single_unit_box({"102001", "101001", "101001", "101001"},
                                         {"000FF00", "0FFFFFF", "00A2989", "0C12E1F"});
    rig.poll(box_swap);

    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
    CHECK(CfsTestAccess::last_rfid_uid(*rig.backend, 0) == "102001|000FF00");

    // The pending expectation was consumed by that swap — it must not linger
    // and swallow a subsequent change that happens to match the pushed color.
    helix::ams::FilamentSlotOverride fresh;
    fresh.material = "PETG";
    fresh.color_rgb = 0x00FF00;
    fresh.color_set = true;
    fresh.user_locked_material = true;
    CfsTestAccess::seed_override(*rig.backend, 0, fresh);

    json box_late_echo = make_single_unit_box({"101001", "101001", "101001", "101001"},
                                              {"01A1A1A", "0FFFFFF", "00A2989", "0C12E1F"});
    rig.poll(box_late_echo);
    CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 0).has_value());
}

// =============================================================================
// Presence: `remain_len` LATCHES after a tagged spool is pulled, so it cannot
// be an unconditional presence signal. It exists only to cover UNTAGGED spools,
// which report no RFID payload at all — see parse_box_status.
// =============================================================================

TEST_CASE("CFS parse: tagged spool removal reads EMPTY despite latched remain_len",
          "[ams][cfs][presence]") {
    // Live K2 T1 slot 4 (index 3), physically empty: vender correctly reads the
    // "none" sentinel while remain_len/color_value/material_type all stay
    // latched at the removed spool's values.
    json box = make_unit_box_explicit(
        /*material_types=*/{"unknown", "unknown", "101001", "101001"},
        /*color_values=*/{"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        /*venders=*/{"unknown", "unknown", "unknown", "none"},
        /*remain_lens=*/{"100", "0", "46", "50"});

    auto info = AmsBackendCfs::parse_box_status(box);
    REQUIRE(info.units.size() == 1);

    SECTION("removed tagged bay is EMPTY, not pinned AVAILABLE by latched length") {
        CHECK(info.units[0].slots[3].status == SlotStatus::EMPTY);
        // Latched display fields are scrubbed so the empty bay renders blank.
        CHECK(info.units[0].slots[3].material.empty());
        CHECK(info.units[0].slots[3].remaining_length_m == 0.0f);
    }
    SECTION("seated bays are unaffected") {
        CHECK(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        CHECK(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
        CHECK(info.units[0].slots[2].status == SlotStatus::AVAILABLE);
    }
    SECTION("a seated bay reporting remain_len 0 stays AVAILABLE on its vender") {
        // Slot 1 is spooled out but still physically seated — vender says so.
        CHECK(info.units[0].slots[1].status == SlotStatus::AVAILABLE);
    }
}

TEST_CASE("CFS parse: untagged spool with remaining length stays PRESENT", "[ams][cfs][presence]") {
    // Regression guard for 65b3a1b8d. An untagged 3rd-party spool has no RFID
    // vendor and no RFID payload, but the measuring wheel reports real length.
    // Keying presence on `vender` alone — or dropping remain_len outright —
    // would wrongly parse these EMPTY.
    SECTION("hard sentinels (-1) in material/color") {
        json box =
            make_unit_box_explicit({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"},
                                   {"none", "none", "none", "none"}, {"46", "-1", "-1", "-1"});
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units.size() == 1);
        CHECK(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        CHECK(info.units[0].slots[1].status == SlotStatus::EMPTY);
    }

    SECTION("'unknown' in material/color is also a no-payload sentinel") {
        json box = make_unit_box_explicit(
            {"unknown", "unknown", "-1", "-1"}, {"unknown", "unknown", "-1", "-1"},
            {"none", "none", "none", "none"}, {"46", "0", "-1", "-1"});
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units.size() == 1);
        CHECK(info.units[0].slots[0].status == SlotStatus::AVAILABLE);
        CHECK(info.units[0].slots[1].status == SlotStatus::EMPTY);
    }

    SECTION("a genuinely empty never-tagged bay is EMPTY") {
        json box =
            make_unit_box_explicit({"-1", "-1", "-1", "-1"}, {"-1", "-1", "-1", "-1"},
                                   {"none", "none", "none", "none"}, {"-1", "-1", "-1", "-1"});
        auto info = AmsBackendCfs::parse_box_status(box);
        REQUIRE(info.units.size() == 1);
        CHECK(info.units[0].slots[0].status == SlotStatus::EMPTY);
    }
}

TEST_CASE("CFS clears the stale lane_data record when a tagged spool is removed",
          "[ams][cfs][presence][filament_slot_override]") {
    CfsOverrideRig rig("cfs_removal_clears");

    // Slot 3 seated with a tagged spool. The auto-mirror publishes firmware
    // truth to lane4 so OrcaSlicer can see it.
    json box_seated = make_unit_box_explicit(
        {"unknown", "unknown", "101001", "101001"}, {"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        {"unknown", "unknown", "unknown", "unknown"}, {"100", "0", "46", "50"});
    rig.poll(box_seated);

    REQUIRE(rig.backend->get_slot_info(3).status == SlotStatus::AVAILABLE);
    auto seated_record = rig.api->mock_get_db_value("lane_data", "lane4");
    REQUIRE(!seated_record.is_null());

    // Spool pulled. `vender` drops to the sentinel; everything else latches.
    // The RFID fingerprint is therefore UNCHANGED — check_hardware_event_clear
    // can never fire here, which is exactly why removal needs its own path.
    json box_removed = make_unit_box_explicit(
        {"unknown", "unknown", "101001", "101001"}, {"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        {"unknown", "unknown", "unknown", "none"}, {"100", "0", "46", "50"});
    rig.poll(box_removed);

    SECTION("slot reads EMPTY and is not promoted back by the stale override") {
        CHECK(rig.backend->get_slot_info(3).status == SlotStatus::EMPTY);
    }
    SECTION("the auto-mirrored override is erased") {
        CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 3).has_value());
    }
    SECTION("the lane_data record is deleted, and stays deleted across polls") {
        CHECK(rig.api->mock_get_db_value("lane_data", "lane4").is_null());
        for (int i = 0; i < 3; ++i) {
            rig.poll(box_removed);
            CHECK(rig.api->mock_get_db_value("lane_data", "lane4").is_null());
            CHECK_FALSE(CfsTestAccess::get_override(*rig.backend, 3).has_value());
        }
    }
    SECTION("neighbouring seated slots keep their records") {
        CHECK_FALSE(rig.api->mock_get_db_value("lane_data", "lane3").is_null());
    }
}

TEST_CASE("CFS removal keeps a user-locked assignment for an unloaded slot",
          "[ams][cfs][presence][filament_slot_override]") {
    // A deliberate user assignment means "this is what belongs in this slot".
    // A slot that is merely unloaded must not lose it — only auto-mirrored
    // records describe a spool that is definitionally gone. Matches the AD5X
    // IFS policy of retaining the lane->Spoolman override across empty (#1071).
    CfsOverrideRig rig("cfs_removal_keeps_locked");

    json box_seated = make_unit_box_explicit(
        {"unknown", "unknown", "101001", "101001"}, {"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        {"unknown", "unknown", "unknown", "unknown"}, {"100", "0", "46", "50"});
    rig.poll(box_seated);

    SlotInfo edit;
    edit.material = "ASA-CF";
    edit.color_name = "Dark Gray";
    edit.color_rgb = 0x1A1A1A;
    edit.spool_name = "My ASA";
    REQUIRE(rig.backend->set_slot_info(3, edit, /*persist=*/true).success());

    json box_removed = make_unit_box_explicit(
        {"unknown", "unknown", "101001", "101001"}, {"0FFFFFF", "01A1A1A", "01A1A1A", "0C12E1F"},
        {"unknown", "unknown", "unknown", "none"}, {"100", "0", "46", "50"});
    for (int i = 0; i < 3; ++i) {
        rig.poll(box_removed);
    }

    auto ovr = CfsTestAccess::get_override(*rig.backend, 3);
    REQUIRE(ovr.has_value());
    CHECK(ovr->material == "ASA-CF");
    CHECK(ovr->spool_name == "My ASA");
    CHECK(ovr->user_locked_material);

    auto stored = rig.api->mock_get_db_value("lane_data", "lane4");
    REQUIRE(!stored.is_null());
    CHECK(stored["material"] == "ASA-CF");
}

TEST_CASE("FillUnsetOnly mirror does not overwrite user-locked fields",
          "[ams][filament_slot_override]") {
    std::unordered_map<int, helix::ams::FilamentSlotOverride> overrides;

    SECTION("locked fields are left alone even when the value flags disagree") {
        // color_set/material carry the locks' normal companions, but a legacy
        // or third-party record can present a lock without them. The lock is
        // the authoritative signal and must win on its own.
        helix::ams::FilamentSlotOverride ovr;
        ovr.user_locked_color = true;
        ovr.user_locked_material = true;
        ovr.color_set = false;
        ovr.material.clear();
        overrides[0] = ovr;

        bool changed = helix::ams::mirror_firmware_to_lane_data(
            /*store=*/nullptr, overrides, 0, 0x00FF00, "PETG", /*slot_has_filament=*/true,
            helix::ams::MirrorPolicy::FillUnsetOnly, "[test]");

        CHECK_FALSE(changed);
        CHECK_FALSE(overrides[0].color_set);
        CHECK(overrides[0].material.empty());
    }

    SECTION("unlocked unset fields are still bootstrapped from firmware") {
        overrides[1] = helix::ams::FilamentSlotOverride{};

        bool changed = helix::ams::mirror_firmware_to_lane_data(
            nullptr, overrides, 1, 0x00FF00, "PETG", true, helix::ams::MirrorPolicy::FillUnsetOnly,
            "[test]");

        CHECK(changed);
        CHECK(overrides[1].color_set);
        CHECK(overrides[1].color_rgb == 0x00FF00u);
        CHECK(overrides[1].material == "PETG");
        // Auto-mirror writes never claim to be user edits.
        CHECK_FALSE(overrides[1].user_locked_color);
        CHECK_FALSE(overrides[1].user_locked_material);
    }
}

// ============================================================================
// #1250 — CFS runout surface (auto-refill give-up messages)
//
// The K2's runout path always pauses first (pause_on_runout on
// [filament_switch_sensor filament_sensor]) and then runs
// BOX_CHECK_MATERIAL_REFILL, which either swaps and resumes or gives up with a
// respond_info line. Those give-up lines are the only runout signal HelixScreen
// gets, and they arrive as `// `-prefixed responses, not `!!`.
//
// See docs/devel/printers/CREALITY_K2_SUPPORT.md § "Runout and auto-refill".
// ============================================================================

namespace {
// Box payload with the runout latch (filament_useup) in a chosen state, so the
// weak-hint fallback tier has real backend state to corroborate against.
json make_runout_box(int filament_useup) {
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

helix::ClassifyContext paused_ctx() {
    helix::ClassifyContext ctx;
    ctx.is_paused = true;
    return ctx;
}
} // namespace

TEST_CASE("CFS runout: 'no identical supplies' raises one runout fault", "[ams][cfs][1250]") {
    CfsRemapHelper backend;

    auto ev = backend.classify_error("// no identical supplies", paused_ctx());
    REQUIRE(ev.has_value());
    CHECK(ev->source == helix::ErrorSource::CFS);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    CHECK(ev->sticky);
    // Titled, so modal_title_for() does not relabel it "Filament System Error".
    CHECK(ev->title == std::string("Filament runout"));
    // The user is told the CFS found nothing to switch to, not just "error".
    CHECK(ev->detail.find("no matching spool") != std::string::npos);
    // Raw firmware wording is preserved for cross-channel dedup, `//` stripped.
    CHECK(ev->raw_detail == "no identical supplies");

    // Exactly the two vetted actions, in order.
    REQUIRE(ev->recovery_actions.size() == 2);
    CHECK(ev->recovery_actions[0].gcode == "RESUME");
    CHECK(ev->recovery_actions[0].style == "primary");
    // Resuming extrudes on the next move — a cold nozzle fails the same way the
    // print did.
    CHECK(ev->recovery_actions[0].needs_hot_nozzle);
    // NOT BOX_ERROR_RESUME_PROCESS: that only drives the box half and leaves the
    // job paused (it is reached FROM RESUME via RESUME_EXTERNAL_PROCESS).
    CHECK(ev->recovery_actions[0].gcode != AmsBackendCfs::recover_gcode());

    CHECK(ev->recovery_actions[1].gcode == AmsBackendCfs::reset_gcode());
    CHECK(ev->recovery_actions[1].gcode == "BOX_ERROR_CLEAR");
    // State only — must stay tappable on a cold nozzle.
    CHECK_FALSE(ev->recovery_actions[1].needs_hot_nozzle);
}

TEST_CASE("CFS runout: 'disable material automatic refill' raises one runout fault",
          "[ams][cfs][1250]") {
    CfsRemapHelper backend;

    auto ev = backend.classify_error("// disable material automatic refill", paused_ctx());
    REQUIRE(ev.has_value());
    CHECK(ev->source == helix::ErrorSource::CFS);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    CHECK(ev->title == std::string("Filament runout"));
    // This branch must say WHY nothing happened — auto-refill is switched off.
    CHECK(ev->detail.find("Auto-refill is off") != std::string::npos);
    CHECK(ev->raw_detail == "disable material automatic refill");
    REQUIRE(ev->recovery_actions.size() == 2);
    CHECK(ev->recovery_actions[0].gcode == "RESUME");
    CHECK(ev->recovery_actions[1].gcode == "BOX_ERROR_CLEAR");
}

// ---------------------------------------------------------------------------
// Post-operation phase verification (#968)
//
// The BOX_* primitives record and queue failures instead of raising at the
// failing command, so a load that never fed filament still drains the script
// and reports success at every RPC layer. These pin the rule that reads the
// one independent physical witness we have: the toolhead filament switch.
// ---------------------------------------------------------------------------

TEST_CASE("CFS phase verify: load that never reached the nozzle is caught", "[ams][cfs][968]") {
    using V = AmsBackendCfs::PhaseVerdict;

    // The #968 failure: script drained clean, nozzle still empty.
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::LOADING, /*sensor_ever_read=*/true,
                                              /*filament_at_end=*/false) ==
          V::LoadDidNotReachNozzle);
    // Filament present at the end is the success case.
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::LOADING, true, true) == V::Ok);
}

TEST_CASE("CFS phase verify: unload that left filament behind is caught", "[ams][cfs][968]") {
    using V = AmsBackendCfs::PhaseVerdict;

    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::UNLOADING, true, /*filament_at_end=*/
                                              true) == V::UnloadLeftFilament);
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::UNLOADING, true, false) == V::Ok);
}

TEST_CASE("CFS phase verify: a bypass unload is not judged by the toolhead switch",
          "[ams][cfs][bypass]") {
    using V = AmsBackendCfs::PhaseVerdict;

    // A bay unload reels filament back down its lane, so filament still at the
    // switch means the cut or retract failed. A bypass unload has no lane: both
    // QUIT_MATERIAL and our fallback pull ~10 mm to clear the melt zone and
    // stop, leaving the user to pull the rest. Judging it by the bay rule marked
    // every bypass unload failed and disarmed the manual-pull prompt with it.
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::UNLOADING, /*sensor_ever_read=*/true,
                                              /*filament_at_end=*/true,
                                              /*bypass_unload=*/true) == V::Ok);
    // The bay rule is untouched: same inputs, bypass off, still a failure.
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::UNLOADING, true, true,
                                              /*bypass_unload=*/false) == V::UnloadLeftFilament);
    // The exemption is scoped to unload. A bypass flag must not launder a load
    // that never reached the nozzle.
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::LOADING, true, /*filament_at_end=*/false,
                                              /*bypass_unload=*/true) == V::LoadDidNotReachNozzle);
}

TEST_CASE("CFS bypass load gcode: LOAD_MATERIAL when the printer defines it",
          "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    // Creality's own external-spool load, the mirror of QUIT_MATERIAL. Unlike
    // the unload it needs no tail of ours: its FILAMENT_RACK_FLUSH already
    // drives the feed, gated on the toolhead switch the user fed to.
    CHECK(AmsBackendCfs::bypass_load_gcode(V::K2, /*has_load_material=*/true) == "LOAD_MATERIAL");
    CHECK(AmsBackendCfs::bypass_load_gcode(V::K1, true) == "LOAD_MATERIAL");
}

TEST_CASE("CFS bypass load gcode: fallback feeds the same path the unload backs out",
          "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    const std::string load = AmsBackendCfs::bypass_load_gcode(V::K2, /*has_load_material=*/false);
    const std::string unload = AmsBackendCfs::bypass_unload_gcode(V::K2, false);

    // Same distance and rate, opposite sign — the load pushes back down exactly
    // the path the unload backs out of.
    REQUIRE(load.find("G0 E80 F600") != std::string::npos);
    REQUIRE(load.find("G0 E-") == std::string::npos);
    REQUIRE(unload.find("G0 E-80 F600") != std::string::npos);

    // Positioning and state bracketing, same as the unload.
    REQUIRE(load.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
    REQUIRE(load.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
    REQUIRE(load.find("SAVE_GCODE_STATE") != std::string::npos);
    REQUIRE(load.find("RESTORE_GCODE_STATE") != std::string::npos);

    // A load must not cut. The unload's cut primitive has no business here.
    REQUIRE(load.find("CR_BOX_CUT") == std::string::npos);
    REQUIRE(load.find("BOX_CUT_MATERIAL") == std::string::npos);
}

TEST_CASE("CFS load routes the bypass sentinel instead of refusing it", "[ams][cfs][bypass]") {
    helix::MacroParamCache::instance().clear(); // no LOAD_MATERIAL — exercise the fallback

    SECTION("stock K2: -2 dispatches a load instead of invalid_slot") {
        CfsRemapHelper backend;
        backend.mark_running();

        // The regression: slot_to_tnn(-2) has no answer, so this refused
        // outright and an external spool could never be loaded from the app —
        // which also left no way to reach the state the bypass UNLOAD needs.
        REQUIRE(backend.load_filament(helix::ui::EXTERNAL_SPOOL_SLOT).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0].find("G0 E80 F600") != std::string::npos);
    }

    SECTION("stock K2: a real bay still gets the bay script") {
        CfsRemapHelper backend;
        backend.mark_running();

        REQUIRE(backend.load_filament(0).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0].find("CR_BOX_EXTRUDE") != std::string::npos);
    }

    SECTION("Fork keeps its own T<external> attended load") {
        CfsRemapHelper backend;
        backend.mark_running();
        CfsTestAccess::set_macro_variant_fork(backend);

        // Fork resolves the external bay through its own T command, so the
        // sentinel must NOT be diverted into our stock-dialect script.
        REQUIRE(backend.load_filament(3).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0] == "T3");
    }
}

TEST_CASE("CFS bypass unload gcode: QUIT_MATERIAL when the printer defines it",
          "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    // Creality's own external-spool unload owns the heat, the cut and the park.
    const std::string k2 = AmsBackendCfs::bypass_unload_gcode(V::K2, /*has_quit_material=*/true);
    REQUIRE(k2.rfind("QUIT_MATERIAL", 0) == 0);
    CHECK(AmsBackendCfs::bypass_unload_gcode(V::K1, true) == k2);

    // But QUIT_MATERIAL does not finish the pull. Its built-in retract is [box]
    // tn_retrude = -10 against tn_extrude = 140: the extruder only breaks the
    // grip and the box's feeder reels the rest back down the tube. A bypass
    // spool has no feeder. Measured on a K2 Plus 2026-08-18 — QUIT_MATERIAL
    // alone moved E by -13.99 mm, and another 50 mm by hand freed it.
    CHECK(k2.find("G0 E-80 F600") != std::string::npos);
    CHECK(k2.find("G91") != std::string::npos);
    CHECK(k2.find("G90") != std::string::npos);
}

TEST_CASE("CFS bypass unload gcode: fallback cuts and retracts with the extruder",
          "[ams][cfs][bypass]") {
    using V = helix::printer::CfsMacroVariant;

    const std::string k2 = AmsBackendCfs::bypass_unload_gcode(V::K2, /*has_quit_material=*/false);
    const std::string k1 = AmsBackendCfs::bypass_unload_gcode(V::K1, false);

    for (const std::string& g : {k2, k1}) {
        // The retract is the whole point: the box primitive is TNN-keyed and
        // no-ops under bypass, so the extruder has to do it.
        REQUIRE(g.find("G91") != std::string::npos);
        REQUIRE(g.find("G0 E-80 F600") != std::string::npos);
        REQUIRE(g.find("G90") != std::string::npos);
        REQUIRE(g.find("CR_BOX_RETRUDE") == std::string::npos);
        REQUIRE(g.find("BOX_RETRUDE_MATERIAL") == std::string::npos);

        // No bay handshake: a stood-down box cannot answer any of these.
        REQUIRE(g.find("BOX_MODE_WAIT") == std::string::npos);
        REQUIRE(g.find("CR_BOX_PRE_OPT") == std::string::npos);
        REQUIRE(g.find("CR_BOX_END_OPT") == std::string::npos);
        REQUIRE(g.find("BOX_CHECK_MATERIAL") == std::string::npos);

        // Positioning and state bracketing still happen.
        REQUIRE(g.find("BOX_GO_TO_EXTRUDE_POS") != std::string::npos);
        REQUIRE(g.find("BOX_MOVE_TO_SAFE_POS") != std::string::npos);
        REQUIRE(g.find("SAVE_GCODE_STATE") != std::string::npos);
        REQUIRE(g.find("RESTORE_GCODE_STATE") != std::string::npos);
    }

    // Each dialect cuts with the primitive its own unload already emits.
    REQUIRE(k2.find("CR_BOX_CUT") != std::string::npos);
    REQUIRE(k1.find("BOX_CUT_MATERIAL") != std::string::npos);
    REQUIRE(k1.find("CR_BOX_CUT") == std::string::npos);
}

TEST_CASE("CFS unload routes the bypass sentinel away from the bay script", "[ams][cfs][bypass]") {
    helix::MacroParamCache::instance().clear(); // no QUIT_MATERIAL — exercise the fallback

    SECTION("stock K2: -2 gets the bypass script, a real bay still gets the bay script") {
        CfsRemapHelper backend;
        backend.mark_running();
        CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/true, /*current_slot=*/-2);

        REQUIRE(backend.unload_filament(helix::ui::EXTERNAL_SPOOL_SLOT).result ==
                AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        // The regression: CR_BOX_RETRUDE is keyed on a TNN and silently no-ops
        // with the box stood down, so the cut ran and nothing came out.
        CHECK(backend.dispatched[0].find("CR_BOX_RETRUDE") == std::string::npos);
        CHECK(backend.dispatched[0].find("G0 E-80 F600") != std::string::npos);
        CHECK(CfsTestAccess::phase_bypass_unload(backend));
    }

    SECTION("stock K2: a real bay keeps the box retract") {
        CfsRemapHelper backend;
        backend.mark_running();
        CfsTestAccess::set_loaded_state(backend, true, /*current_slot=*/0);

        REQUIRE(backend.unload_filament(0).result == AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0].find("CR_BOX_RETRUDE") != std::string::npos);
        CHECK_FALSE(CfsTestAccess::phase_bypass_unload(backend));
    }

    SECTION("Fork keeps BOX_UNLOAD — its own external branch handles the holder") {
        CfsRemapHelper backend;
        backend.mark_running();
        CfsTestAccess::set_macro_variant_fork(backend);
        CfsTestAccess::set_loaded_state(backend, true, -2);

        REQUIRE(backend.unload_filament(helix::ui::EXTERNAL_SPOOL_SLOT).result ==
                AmsResult::SUCCESS);
        REQUIRE(backend.dispatched.size() == 1);
        CHECK(backend.dispatched[0] == "BOX_UNLOAD");
        CHECK_FALSE(CfsTestAccess::phase_bypass_unload(backend));
    }
}

TEST_CASE("CFS bypass unload completes instead of erroring with filament still detected",
          "[ams][cfs][bypass]") {
    helix::MacroParamCache::instance().clear();

    CfsRemapHelper backend;
    backend.mark_running();
    CfsTestAccess::set_loaded_state(backend, /*filament_loaded=*/true, /*current_slot=*/-2);
    REQUIRE(backend.unload_filament(helix::ui::EXTERNAL_SPOOL_SLOT).result == AmsResult::SUCCESS);

    // The end state a bypass unload actually leaves: tip clear of the melt zone,
    // filament still across the toolhead switch, waiting on the user's hand.
    CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/true);
    CfsTestAccess::complete_action(backend);

    // ERROR here is what killed the manual-pull prompt on the K2: op_failed()
    // runs disarm_manual_pull_prompt().
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
    CHECK(backend.get_system_info().operation_detail.empty());
}

TEST_CASE("CFS phase verify: stays silent without a sensor reading", "[ams][cfs][968]") {
    using V = AmsBackendCfs::PhaseVerdict;

    // Klipper publishes filament_detected as null until the switch takes its
    // first reading, and a printer without the switch never publishes at all.
    // last_filament_detected_ is then a DEFAULT, not an observation. Concluding
    // "load failed" from it would put a modal on every successful load on such
    // a machine — strictly worse than saying nothing.
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::LOADING, /*sensor_ever_read=*/false,
                                              false) == V::Unverifiable);
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::UNLOADING, false, true) ==
          V::Unverifiable);
    // Even when the default happens to look like success, refuse to claim it.
    CHECK(AmsBackendCfs::verify_phase_outcome(AmsAction::LOADING, false, true) == V::Unverifiable);
}

TEST_CASE("CFS phase verify: only load and unload are judged", "[ams][cfs][968]") {
    using V = AmsBackendCfs::PhaseVerdict;

    // The synthesized sub-phases (CUTTING/PURGING) and every non-material
    // action must never produce a verdict — they are not operations with a
    // filament end-state contract, and judging them would fire on the
    // synthesized action rather than the user's actual intent.
    for (AmsAction op : {AmsAction::IDLE, AmsAction::CUTTING, AmsAction::PURGING,
                         AmsAction::SELECTING, AmsAction::RESETTING, AmsAction::HEATING}) {
        CHECK(AmsBackendCfs::verify_phase_outcome(op, true, false) == V::Ok);
        CHECK(AmsBackendCfs::verify_phase_outcome(op, true, true) == V::Ok);
    }
}

TEST_CASE("CFS phase verify: failure verdicts carry actionable wording", "[ams][cfs][968]") {
    using V = AmsBackendCfs::PhaseVerdict;

    const std::string load_msg = AmsBackendCfs::phase_verdict_message(V::LoadDidNotReachNozzle);
    const std::string unload_msg = AmsBackendCfs::phase_verdict_message(V::UnloadLeftFilament);

    REQUIRE_FALSE(load_msg.empty());
    REQUIRE_FALSE(unload_msg.empty());
    // The two must not read the same — they need opposite user responses.
    CHECK(load_msg != unload_msg);
    // Non-failures say nothing, so a caller can treat empty as "no fault".
    CHECK(AmsBackendCfs::phase_verdict_message(V::Ok).empty());
    CHECK(AmsBackendCfs::phase_verdict_message(V::Unverifiable).empty());
}

TEST_CASE("CFS phase verify: a failed load raises a fault through current_error",
          "[ams][cfs][968]") {
    CfsRemapHelper backend;

    // No error while nothing has gone wrong.
    CHECK_FALSE(backend.current_error().has_value());

    // Drive the real completion path: intent latched at dispatch, sensor says
    // the nozzle is still empty when the script drains.
    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/false);
    CfsTestAccess::complete_action(backend);

    auto ev = backend.current_error();
    REQUIRE(ev.has_value());
    CHECK(ev->source == helix::ErrorSource::CFS);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    // Must name the real problem rather than a generic failure.
    CHECK(ev->detail.find("did not reach") != std::string::npos);
    // NOT the runout action set. "Resume" is meaningless here — a manual load
    // that failed has no paused job to restart, and offering it would send
    // RESUME to an idle printer.
    for (const auto& a : ev->recovery_actions) {
        CHECK(a.gcode != "RESUME");
    }
    // Clearing the latched box error is the one safe lever, and it must stay
    // tappable on a cold nozzle since it moves no filament.
    REQUIRE(ev->recovery_actions.size() == 1);
    CHECK(ev->recovery_actions[0].gcode == AmsBackendCfs::reset_gcode());
    CHECK_FALSE(ev->recovery_actions[0].needs_hot_nozzle);
}

TEST_CASE("CFS phase verify: a raised fault survives later status frames", "[ams][cfs][968]") {
    CfsRemapHelper backend;

    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, true, false);
    CfsTestAccess::complete_action(backend);
    REQUIRE(backend.current_error().has_value());

    // The box keeps streaming after the failure. Phase synthesis runs off those
    // frames and used to be free to overwrite `action` with LOADING/CUTTING —
    // which would erase the fault and dismiss the modal the user is reading.
    nlohmann::json n = {
        {"params", nlohmann::json::array(
                       {{{"filament_switch_sensor filament_sensor", {{"filament_detected", false}}},
                         {"extruder", {{"temperature", 210.0}, {"target", 220.0}}}}})}};
    CfsTestAccess::handle_status(backend, n["params"][0]);

    CHECK(backend.get_system_info().action == AmsAction::ERROR);
    CHECK(backend.current_error().has_value());
}

TEST_CASE("CFS phase verify: starting a new operation clears the fault", "[ams][cfs][968]") {
    CfsRemapHelper backend;

    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, true, false);
    CfsTestAccess::complete_action(backend);
    REQUIRE(backend.current_error().has_value());

    // A retry must not inherit the previous failure — otherwise the modal can
    // never be dismissed by doing the obvious thing.
    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CHECK_FALSE(backend.current_error().has_value());
    CHECK(backend.get_system_info().action == AmsAction::LOADING);
}

TEST_CASE("CFS phase verify: a good load completes to IDLE with no fault", "[ams][cfs][968]") {
    CfsRemapHelper backend;

    CfsTestAccess::force_phase_intent(backend, AmsAction::LOADING);
    CfsTestAccess::set_filament_sensor(backend, /*seen=*/true, /*detected=*/true);
    CfsTestAccess::complete_action(backend);

    CHECK_FALSE(backend.current_error().has_value());
    CHECK(backend.get_system_info().action == AmsAction::IDLE);
}

TEST_CASE("CFS runout: terse 'no auto refill' wording is recognized", "[ams][cfs][968]") {
    CfsRemapHelper backend;

    // Fourth give-up literal, read out of the 2.3.5.34 box_wrapper extension.
    // It shares nothing with the long-form "disable material automatic refill":
    // "auto refill" != "automatic refill", and there is no "disab". Before this
    // it could only ever reach the weak tier, and only with the box latch set.
    auto ev = backend.classify_error("// no auto refill", paused_ctx());
    REQUIRE(ev.has_value());
    CHECK(ev->title == std::string("Filament runout"));
    CHECK(ev->detail.find("Auto-refill is off") != std::string::npos);
    CHECK(ev->raw_detail == "no auto refill");
    REQUIRE(ev->recovery_actions.size() == 2);
}

TEST_CASE("CFS runout: 'no tray with ingredients found' raises one runout fault",
          "[ams][cfs][968]") {
    CfsRemapHelper backend;

    // The third give-up path, documented in the K1 wrapper RE notes
    // (docs/devel/CREALITY_CFS_INTERNALS.md): a same_material group DOES exist
    // for the exhausted slot, but none of its members currently has material
    // sensor presence. Distinct cause from "no identical supplies", which means
    // no compatible group exists at all.
    auto ev = backend.classify_error("// no tray with ingredients found", paused_ctx());
    REQUIRE(ev.has_value());
    CHECK(ev->source == helix::ErrorSource::CFS);
    CHECK(ev->severity == helix::ErrorSeverity::CRITICAL);
    CHECK(ev->sticky);
    CHECK(ev->title == std::string("Filament runout"));
    // Must name the actual situation: the matching slots are empty, so the fix
    // is loading one of them — not "buy matching filament".
    CHECK(ev->detail.find("empty") != std::string::npos);
    // Must NOT be misreported as the no-compatible-material branch.
    CHECK(ev->detail.find("no matching spool") == std::string::npos);
    CHECK(ev->raw_detail == "no tray with ingredients found");

    REQUIRE(ev->recovery_actions.size() == 2);
    CHECK(ev->recovery_actions[0].gcode == "RESUME");
    CHECK(ev->recovery_actions[1].gcode == AmsBackendCfs::reset_gcode());
}

TEST_CASE("CFS runout: 'no tray' branch does not need the box latch", "[ams][cfs][968]") {
    CfsRemapHelper backend;

    // Distinguishes this from the weak-hint tier: that one requires
    // system_info_.filament_runout to be set. This wording is specific enough
    // to stand on its own, exactly like the other two strong matches, so a
    // fresh backend with no box frame parsed yet must still classify it.
    auto ev = backend.classify_error("// No tray with ingredients found!", paused_ctx());
    REQUIRE(ev.has_value());
    CHECK(ev->detail.find("empty") != std::string::npos);
}

TEST_CASE("CFS runout: matcher survives the sentence being reworded", "[ams][cfs][1250]") {
    CfsRemapHelper backend;

    // Fragment match, not whole-sentence: these are untranslated literals from
    // one firmware build and the surrounding words are the least durable part.
    SECTION("no-match branch, alternate phrasing") {
        auto ev =
            backend.classify_error("// There are no identical supplies available!", paused_ctx());
        REQUIRE(ev.has_value());
        CHECK(ev->detail.find("no matching spool") != std::string::npos);
    }
    SECTION("refill-off branch, alternate phrasing") {
        auto ev = backend.classify_error("// Material automatic refill is disabled", paused_ctx());
        REQUIRE(ev.has_value());
        CHECK(ev->detail.find("Auto-refill is off") != std::string::npos);
    }
    SECTION("bare line with no // prefix still matches") {
        auto ev = backend.classify_error("no identical supplies", paused_ctx());
        REQUIRE(ev.has_value());
        CHECK(ev->raw_detail == "no identical supplies");
    }
}

TEST_CASE("CFS runout: weak-hint fallback needs the box latch AND a pause", "[ams][cfs][1250]") {
    CfsRemapHelper backend;

    // Wording neither matcher recognizes, but the line is about refilling.
    const std::string line = "// material refill aborted";

    SECTION("latch clear: not enough evidence, defer to the generic classifier") {
        CfsTestAccess::handle_status(backend, make_cfs_notification(make_runout_box(0)));
        CHECK_FALSE(backend.classify_error(line, paused_ctx()).has_value());
    }

    SECTION("latch set + paused: fires, surfacing the firmware's own words") {
        CfsTestAccess::handle_status(backend, make_cfs_notification(make_runout_box(1)));
        auto ev = backend.classify_error(line, paused_ctx());
        REQUIRE(ev.has_value());
        // Weaker evidence, so it must NOT claim which give-up path ran.
        CHECK(ev->detail == "material refill aborted");
        CHECK(ev->recovery_actions.size() == 2);
    }

    SECTION("latch set but not paused: still nothing") {
        CfsTestAccess::handle_status(backend, make_cfs_notification(make_runout_box(1)));
        helix::ClassifyContext idle_ctx;
        idle_ctx.is_paused = false;
        CHECK_FALSE(backend.classify_error(line, idle_ctx).has_value());
    }
}

TEST_CASE("CFS runout: an unpaused give-up line is not a runout", "[ams][cfs][1250]") {
    CfsRemapHelper backend;
    helix::ClassifyContext idle_ctx; // is_paused = false

    // The firmware pauses BEFORE running BOX_CHECK_MATERIAL_REFILL, so an
    // unpaused occurrence is a human echoing the words or poking the macro.
    CHECK_FALSE(backend.classify_error("// no identical supplies", idle_ctx).has_value());
    CHECK_FALSE(
        backend.classify_error("// disable material automatic refill", idle_ctx).has_value());
}

TEST_CASE("CFS runout: `!!` lines stay with the generic key8xx classifier", "[ams][cfs][1250]") {
    CfsRemapHelper backend;
    // The box latch is set, so nothing but the `!!` gate can be refusing these.
    CfsTestAccess::handle_status(backend, make_cfs_notification(make_runout_box(1)));

    // key840 keeps its existing generic decode + hardcoded "Reset CFS" action.
    // Claiming it here would double-surface (a backend modal AND the generic one)
    // or silently replace the coded decode.
    CHECK_FALSE(
        backend.classify_error(R"(!! {"code":"key840","values":[]})", paused_ctx()).has_value());

    // Even a `!!` that happens to carry the give-up wording is refused — the
    // inverted gate is unconditional, so the two channels cannot both claim it.
    CHECK_FALSE(backend.classify_error("!! no identical supplies", paused_ctx()).has_value());
    CHECK_FALSE(backend.classify_error("!! material refill aborted", paused_ctx()).has_value());
}

TEST_CASE("CFS runout: ordinary console chatter is ignored", "[ams][cfs][1250]") {
    CfsRemapHelper backend;
    CfsTestAccess::handle_status(backend, make_cfs_notification(make_runout_box(1)));

    // A paused print with the latch set is the state most likely to produce a
    // false positive, so that is the state these are checked in.
    for (const char* line : {"ok", "// echo: busy", "// Klipper state: Ready",
                             "// probe at 10.000,10.000 is z=0.100", ""}) {
        CAPTURE(line);
        CHECK_FALSE(backend.classify_error(line, paused_ctx()).has_value());
    }
}

// ============================================================================
// Endless spool capabilities
// ============================================================================
//
// CFS had no capability test at all, and its old answer was
// {supported=true, editable=false, description="Auto-refill enabled"|"disabled"} —
// so auto-refill on and off were indistinguishable to any UI, and the only place
// the real state lived was an untranslated English string.

TEST_CASE("CFS endless spool: auto-refill on and off are distinguishable",
          "[ams][cfs][endless_spool]") {
    SECTION("auto_refill=1 reads as available and ON") {
        CfsRemapHelper backend;
        CfsTestAccess::handle_status(backend, make_cfs_notification(make_runout_box(0)));

        auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.availability == EndlessSpoolAvailability::Available);
        CHECK(caps.enabled == EndlessSpoolEnabled::On);
        // The box picks the refill spool from its own same_material groups.
        CHECK(caps.editability == EndlessSpoolEditability::ReadOnly);
        CHECK(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);
        CHECK_FALSE(caps.editable());
    }

    SECTION("auto_refill=0 reads as available and OFF") {
        CfsRemapHelper backend;
        json box = make_runout_box(0);
        box["auto_refill"] = 0;
        CfsTestAccess::handle_status(backend, make_cfs_notification(box));

        auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.availability == EndlessSpoolAvailability::Available);
        CHECK(caps.enabled == EndlessSpoolEnabled::Off);
        CHECK(caps.restriction == EndlessSpoolRestriction::FirmwareManaged);
    }

    SECTION("no per-slot relation is reported, so the UI has no dropdown to draw") {
        // The old shape (available + an empty per-slot config) made the context
        // menu render a backup dropdown that could only ever read "None", with no
        // way to tell that apart from "no backup configured".
        CfsRemapHelper backend;
        CfsTestAccess::handle_status(backend, make_cfs_notification(make_runout_box(0)));

        CHECK(backend.get_endless_spool_config().empty());
        CHECK(endless_spool_backup_for(backend.get_endless_spool_config(), 0) == -1);
    }

    SECTION("writes are refused with the firmware-managed reason") {
        CfsRemapHelper backend;
        CfsTestAccess::handle_status(backend, make_cfs_notification(make_runout_box(0)));

        auto result = backend.set_endless_spool_backup(0, 1);
        CHECK_FALSE(result.success());
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
        CHECK(result.user_msg ==
              endless_spool_restriction_text(EndlessSpoolRestriction::FirmwareManaged));

        CHECK_FALSE(backend.reset_endless_spool().success());
    }
}

// ===========================================================================
// Bypass declaration lifetime — what actually means "the CFS took the feed back"
// ===========================================================================
//
// Field evidence, K2 Plus 2026-08-18. Bypass was declared (BOX_ENABLE_CFS_PRINT
// ENABLE=0) at 23:13 and restored cleanly across a restart at 00:34 with the box
// still reporting enable=0. By the next restart `enable` had returned to 1 with
// no host command in between: no BOX_ENABLE_CFS_PRINT anywhere in klippy.log, no
// disable_bypass() in ours, and the prints in that window were plain Moonraker
// start_print calls from Fluidd, which runs nothing vendor-specific. The box
// re-arms itself.
//
// Dropping the declaration on `enable` alone therefore threw bypass away on the
// first full frame after every restart (partial frames omit the field, so it only
// ever bit at startup) while the external spool was still feeding the nozzle.

TEST_CASE("CFS drops the bypass declaration once a bay is actually loaded", "[ams][cfs][bypass]") {
    // The real drift case the guard exists for: the CFS is feeding again, so the
    // declaration is stale and must not permit a later re-derivation.
    CfsRemapHelper cfs;
    CfsTestAccess::set_bypass_declared(cfs, true);

    json box = make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
    box["enable"] = 1;
    box["T1"]["filament"] = "A"; // bay 1 slot A is the active lane
    CfsTestAccess::handle_status(cfs, make_cfs_notification(box));

    CHECK_FALSE(CfsTestAccess::bypass_declared(cfs));
}

TEST_CASE("CFS drops the bypass declaration for a loaded bay even while stood down",
          "[ams][cfs][bypass]") {
    // enable=0 with a bay loaded: the box is not participating in prints but a
    // lane is threaded and named active. Gating the drop on enable==1 as well
    // would leave the declaration latched with the CFS holding the path.
    CfsRemapHelper cfs;
    CfsTestAccess::set_bypass_declared(cfs, true);

    json box = make_single_unit_box({"101001", "-1", "-1", "-1"}, {"01A1A1A", "-1", "-1", "-1"});
    box["enable"] = 0;
    box["T1"]["filament"] = "A";
    CfsTestAccess::handle_status(cfs, make_cfs_notification(box));

    CHECK_FALSE(CfsTestAccess::bypass_declared(cfs));
}
