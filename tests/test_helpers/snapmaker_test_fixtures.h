// tests/test_helpers/snapmaker_test_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What these test files share: their fixtures, helpers and includes.
// test_ams_backend_snapmaker.cpp, test_ams_backend_snapmaker_2.cpp,
// test_ams_backend_snapmaker_3.cpp, test_ams_backend_snapmaker_4.cpp. Generated from the first of
// those files by split_v3.py, which keeps every case's text and order and moves everything else
// here.
#pragma once

#include "ui_batch_filament_modal.h"
#include "ui_insert_notice.h"

#include "../helix_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_backend_snapmaker.h"
#include "ams_bypass_policy.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "filament_insert_watcher.h"
#include "filament_op_execute.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_translation.h"
#include "lvgl.h"
#include "lvgl_ui_test_fixture.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "preflight_validator.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "snapmaker_nozzle_offset.h"
#include "spoolman_types.h" // SpoolInfo + apply_spool_to_slot (the picker-side writer)
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/filament_slot_override_store_test_access.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/printer_state_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/scoped_env.h"
#include "test_helpers/scoped_language.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/snapmaker_test_access.h"
#include "test_helpers/step_operation_helpers.h"
#include "test_helpers/toast_log.h"
#include "tool_state.h"
#include "translation_loader.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix::test {

using namespace helix::printer;
using namespace helix;
using json = nlohmann::json;

struct SnapmakerFixture : public HelixTestFixture {
    ~SnapmakerFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

static json make_feed_status(int extruder_idx, const std::string& channel_state,
                             bool filament_detected = true,
                             const std::string& channel_error = "ok") {
    const char* feed_key = (extruder_idx <= 1) ? "filament_feed left" : "filament_feed right";
    std::string ext_key = "extruder" + std::to_string(extruder_idx);
    return json{{feed_key, json{{ext_key, json{{"filament_detected", filament_detected},
                                               {"channel_state", channel_state},
                                               {"channel_error", channel_error}}}}}};
}

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
inline json make_filament_detect_status(int slot_index, const std::string& main_type,
                                        uint32_t argb_color, const std::string& manufacturer,
                                        const json& card_uid, bool others_none = false) {
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
    using AmsBackendSnapmaker::AmsBackendSnapmaker;

    std::vector<std::string> captured_gcodes;

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>,
                           helix::SharedCallback<void(const MoonrakerError&)>, bool) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    // current_slot is protected on the subscription base; expose a setter so
    // tests can stand in for "filament currently loaded in slot N".
    void set_loaded_slot(int slot) {
        system_info_.current_slot = slot;
    }
};

inline json feed_with_action(int extruder_idx, const std::string& channel_state,
                             const std::string& action_state, bool filament_detected = true) {
    json frame = make_feed_status(extruder_idx, channel_state, filament_detected);
    frame.begin().value().begin().value()["channel_action_state"] = action_state;
    return frame;
}

inline json t3_picked() {
    return json{{"toolhead", json{{"extruder", "extruder3"}}}};
}
inline json t3_feed() {
    return make_feed_status(3, "preload_finish", /*filament_detected=*/true);
}
inline json t3_switch(bool detected) {
    return json{{"filament_motion_sensor e3_filament",
                 json{{"filament_detected", detected}, {"enabled", true}}}};
}
inline json merged(std::initializer_list<json> frames) {
    json out = json::object();
    for (const auto& f : frames) {
        out.update(f);
    }
    return out;
}

struct LoadAnswers {
    bool filament_loaded;
    SlotStatus status;
    bool actively_loaded;
    bool at_nozzle;
    bool parked;
    bool operator==(const LoadAnswers& o) const {
        return filament_loaded == o.filament_loaded && status == o.status &&
               actively_loaded == o.actively_loaded && at_nozzle == o.at_nozzle &&
               parked == o.parked;
    }
};

} // namespace helix::test

template <> struct Catch::StringMaker<helix::test::LoadAnswers> {
    static std::string convert(const helix::test::LoadAnswers& a) {
        return "{filament_loaded=" + std::to_string(a.filament_loaded) +
               " status=" + std::to_string(static_cast<int>(a.status)) +
               " actively=" + std::to_string(a.actively_loaded) +
               " at_nozzle=" + std::to_string(a.at_nozzle) + " parked=" + std::to_string(a.parked) +
               "}";
    }
};

namespace helix::test {

inline LoadAnswers answers(const AmsBackendSnapmaker& b, int slot) {
    return {b.get_system_info().filament_loaded, b.get_slot_info(slot).status,
            b.slot_is_actively_loaded(slot), b.slot_has_filament_at_toolhead(slot),
            b.slot_filament_parked_in_toolhead(slot)};
}
inline LoadAnswers fed(std::initializer_list<json> frames) {
    helix::test::RegisteredWired<AmsBackendSnapmaker> reg;
    for (const auto& f : frames) {
        reg->fire_status(f);
    }
    return answers(*reg, 3);
}

inline json untagged_frame() {
    return json{
        {"filament_detect",
         json{{"info", json::array({json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}},
                                    json{{"MAIN_TYPE", "NONE"}}, json{{"MAIN_TYPE", "NONE"}}})}}}};
}

struct SnapmakerLaneRig {
    explicit SnapmakerLaneRig(const std::string& name)
        : tmp(name), api(client, state), backend_reg(&api) {
        state.init_subjects(false);
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
        SnapmakerTestAccess::inject_override_store(*backend_reg, std::move(store));
        // An edit writes print_task_config only once a print state is read.
        backend_reg->fire_status(json{{"print_stats", {{"state", "standby"}}}});
    }

    SnapmakerTmpCacheDir tmp;
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api;
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg;
};

/// Slot 0 seated, its tag reading @p argb and @p main_type. The port sensor
/// is what makes the slot present and AVAILABLE, the only status the mirror
/// runs on, and the CARD_UID stays the same across frames so no case reads as
/// a spool swap. The state array rides along as the entrance/tag reading,
/// which backs no presence claim.
inline json seated_tag_frame(uint32_t argb, const std::string& main_type) {
    json status =
        make_filament_detect_status(0, main_type, argb, "OtherBrand", json::array({1, 2, 3, 4}));
    status["filament_detect"]["state"] = json::array({1, 0, 0, 0});
    status["filament_feed left"] = json{{"extruder0", json{{"filament_detected", true}}}};
    return status;
}

/// Slot 0's stored record as a link to spool 42 leaves it: the spool's colour
/// and material rode in with the binding, so neither is declared.
inline helix::ams::FilamentSlotOverride linked_to_spool_42() {
    helix::ams::FilamentSlotOverride ovr;
    ovr.spoolman_id = 42;
    ovr.brand = "Polymaker";
    ovr.color_rgb = 0xFF5500;
    ovr.color_set = true;
    ovr.material = "PLA";
    return ovr;
}

inline SpoolInfo spool_42() {
    SpoolInfo spool;
    spool.id = 42;
    spool.vendor = "Polymaker";
    spool.material = "PLA";
    spool.color_hex = "FF5500";
    return spool;
}

struct ResumeRecoveryHarness {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState api_state;
    MoonrakerAPIMock api;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;

    ResumeRecoveryHarness() : api(client, api_state), backend(&api) {
        api_state.init_subjects(false);
        // The MoonrakerAPIMock's execute_gcode halt gate reads ITS OWN state
        // (api_state). Mark Klipper ready there so recovery gcode isn't refused.
        api_state.update_from_status(json{{"webhooks", {{"state", "ready"}}}});

        // Global printer state (read by prepare_for_resume's classify_pause):
        // paused, SD inactive (no-op-shaped), no terminal exception
        // (empty message + id -1 => classify Unknown => recoverable).
        PrinterState& ps = get_printer_state();
        ps.init_subjects(false);
        ps.update_from_status(json{{"webhooks", {{"state", "ready"}}},
                                   {"print_stats", {{"state", "paused"}}},
                                   {"virtual_sdcard", {{"is_active", false}}}});

        // Populate slot 0: MAIN_TYPE=PLA, MANUFACTURER=Polymaker, SUB_TYPE=Basic
        // (a recognized product line, so it round-trips into the config gcode).
        backend.fire_status(make_filament_detect_status(0, "PLA", 0xFF112233u, "Polymaker",
                                                        json::array({1, 2, 3, 4})));

        SnapmakerTestAccess::set_sensor_present(backend, 0, false); // runout latched
        SnapmakerTestAccess::set_current_slot(backend, 0);
        client.clear_gcode_script_history();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

// True if any recorded gcode script contains `needle`.
inline bool any_script_contains(const std::vector<std::string>& history,
                                const std::string& needle) {
    for (const auto& s : history) {
        if (s.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

inline AmsError resume_once(ResumeRecoveryHarness& h, int slot) {
    AmsError captured{AmsResult::RESUME_REQUIRES_RESTART}; // poison
    bool fired = false;
    h.backend.prepare_for_resume(slot, [&](const AmsError& err) {
        fired = true;
        captured = err;
    });
    ResumeRecoveryHarness::drain();
    if (!fired) {
        // A refeed's OK waits for its head's frame; none is coming here.
        SnapmakerTestAccess::expire_refeed_wait(h.backend);
        ResumeRecoveryHarness::drain();
    }
    REQUIRE(fired);
    return captured;
}

inline size_t count_containing(const std::vector<std::string>& history, const std::string& needle) {
    size_t n = 0;
    for (const auto& s : history) {
        if (s.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

/// The runout refeed on head 0 fails, then the head switch reads present.
inline void fail_refeed_then_switch_present(ResumeRecoveryHarness& h) {
    h.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR,
                                    "extruder[0]: state: none, error: ok!", "AUTO_FEEDING");
    REQUIRE_FALSE(resume_once(h, 0).success());
    SnapmakerTestAccess::set_sensor_present(h.backend, 0, true);
    h.client.clear_gcode_script_history();
}

inline json fed_head0(const std::string& state, const std::string& error = "ok") {
    return json{{"filament_feed left",
                 {{"extruder0",
                   {{"filament_detected", true},
                    {"channel_state", state},
                    {"channel_error", error},
                    {"module_exist", true},
                    {"disable_auto", false}}}}}};
}

inline json make_task_config(const std::vector<int>& extruder_map, const std::vector<bool>& used) {
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
inline std::vector<int> identity_map() {
    std::vector<int> m(32, 0);
    for (int i = 0; i < 4; ++i) {
        m[static_cast<size_t>(i)] = i;
    }
    return m;
}

struct SnapmakerInsertFixture : public LVGLUITestFixture {
    SnapmakerInsertFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state) {
        state.init_subjects(false);

        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
        SnapmakerTestAccess::inject_override_store(backend, std::move(store));

        helix::ams::FilamentSlotOverride ovr;
        ovr.brand = "Polymaker";
        ovr.spool_name = "PolyLite Orange";
        ovr.material = "PLA";
        ovr.color_rgb = 0xFF5500;
        SnapmakerTestAccess::seed_override(backend, 0, ovr);

        helix::ui::set_test_toast_hook([this](ToastSeverity severity, const std::string& msg,
                                              uint32_t) { toasts.emplace_back(severity, msg); });
    }

    ~SnapmakerInsertFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    void drain() {
        helix::ui::UpdateQueue::instance().drain();
    }

    SnapmakerTmpCacheDir tmp{"task1710_pending_insert"};
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPIMock api;
    helix::test::RegisteredWired<AmsBackendSnapmaker> backend_reg{&api};
    helix::test::WiredBackend<AmsBackendSnapmaker>& backend = *backend_reg;
    std::vector<std::pair<ToastSeverity, std::string>> toasts;
};

inline int judge(FilamentInsertWatcher& watcher, const AmsBackend& backend) {
    FilamentInsertContext ctx;
    ctx.setting_enabled = true;
    return watcher.judge_lanes(backend, ctx);
}

struct SnapmakerGoldenFixture : public LVGLUITestFixture {
    SnapmakerGoldenFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state) {
        state.init_subjects(false);

        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            &api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
        SnapmakerTestAccess::inject_override_store(backend, std::move(store));

        helix::ams::FilamentSlotOverride ovr;
        ovr.brand = "Polymaker";
        ovr.spool_name = "PolyLite Orange";
        ovr.material = "PLA";
        ovr.color_rgb = 0xFF5500;
        SnapmakerTestAccess::seed_override(backend, 0, ovr);

        SnapmakerTestAccess::set_use_batch_macro(backend, true);
        SnapmakerTestAccess::set_batch_macro_object(backend, "gcode_macro AUTO_FEEDING_BATCH");
        backend.set_event_callback(
            [this](const std::string& name, const std::string&) { events.push_back(name); });
        helix::ui::set_test_toast_hook(
            [this](ToastSeverity, const std::string&, uint32_t) { ++toasts; });
    }

    ~SnapmakerGoldenFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Feeds one frame and returns the snapshot line block for it.
    std::string feed(const json& frame) {
        events.clear();
        backend.captured_gcodes.clear();
        toasts = 0;
        backend.fire_status(frame);
        helix::ui::UpdateQueue::instance().drain();
        return snapshot();
    }

    std::string snapshot() {
        const AmsSystemInfo info = backend.get_system_info();
        std::string out =
            fmt::format("action={} phase={} detail='{}' working={} tool={} slot={} loaded={}\n",
                        ams_action_to_string(info.action), info.operation_phase,
                        info.operation_detail, info.operation_working_slot, info.current_tool,
                        info.current_slot, info.filament_loaded);
        for (int i = 0; i < 4; ++i) {
            const SlotInfo slot = backend.get_slot_info(i);
            const auto snap = backend.channel_snapshot(i);
            out += fmt::format(
                "  s{} {} {}/{}/{} #{:06X} n{}-{} b{} w{:.0f} latch={} pend={} state={} "
                "detected={}\n",
                i, slot_status_to_string(slot.status), slot.material, slot.brand, slot.spool_name,
                slot.color_rgb, slot.nozzle_temp_min, slot.nozzle_temp_max, slot.bed_temp,
                slot.total_weight_g, SnapmakerTestAccess::loaded_at_toolhead(backend, i),
                SnapmakerTestAccess::pending_insert_passes(backend, i), snap.state,
                snap.filament_detected);
        }
        std::string gcodes;
        for (const auto& g : backend.captured_gcodes) {
            gcodes += (gcodes.empty() ? "" : "|") + g;
        }
        std::string unloaded;
        for (int i = 0; i < 4; ++i) {
            if (AmsState::instance().was_slot_recently_unloaded(i)) {
                unloaded += std::to_string(i);
            }
        }
        out += fmt::format(
            "  batch_active={} port_present={} events={} gcodes={} toasts={} unloaded={}",
            backend.batch_plan().active, SnapmakerTestAccess::last_published_port_present(backend),
            events.size(), gcodes, toasts, unloaded);
        return out;
    }

    SnapmakerTmpCacheDir tmp{"golden_sequence"};
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPIMock api;
    helix::test::RegisteredWired<CapturingSnapmakerBackend> backend_reg;
    helix::test::WiredBackend<CapturingSnapmakerBackend>& backend = *backend_reg;
    std::vector<std::string> events;
    int toasts = 0;
};

inline json golden_extruder(const char* state, bool park, bool active) {
    return json{{"state", state},
                {"park_pin", park},
                {"active_pin", active},
                {"activating_move", false},
                {"extruder_offset", json::array({0.0, 0.0, 0.0})},
                {"switch_count", 3},
                {"retry_count", 0},
                {"error_count", 0}};
}

inline json golden_full_frame() {
    json info = json::array();
    info.push_back(json{{"MAIN_TYPE", "PLA"},
                        {"SUB_TYPE", "SnapSpeed"},
                        {"MANUFACTURER", "Snapmaker"},
                        {"VENDOR", "Snapmaker"},
                        {"ARGB_COLOR", 0xFF112233u},
                        {"HOTEND_MIN_TEMP", 190},
                        {"HOTEND_MAX_TEMP", 230},
                        {"BED_TEMP", 60},
                        {"WEIGHT", 1000},
                        {"CARD_UID", json::array({1, 2, 3, 4})}});
    for (int i = 1; i < 4; ++i) {
        info.push_back(json{{"MAIN_TYPE", "NONE"}});
    }
    return json{
        {"toolhead", json{{"extruder", "extruder"}}},
        {"extruder", golden_extruder("ACTIVE", false, true)},
        {"extruder1", golden_extruder("PARKED", true, false)},
        {"extruder2", golden_extruder("PARKED", true, false)},
        {"extruder3", golden_extruder("PARKED", true, false)},
        {"filament_detect", json{{"info", info}, {"state", json::array({1, 1, 1, 0})}}},
        {"filament_feed left", json{{"extruder0", json{{"filament_detected", true},
                                                       {"channel_state", "load_finish"},
                                                       {"channel_error", "ok"},
                                                       {"module_exist", true}}},
                                    {"extruder1", json{{"filament_detected", true},
                                                       {"channel_state", "preload_finish"}}}}},
        {"filament_feed right",
         json{{"extruder2", json{{"filament_detected", true}, {"channel_state", "preload_finish"}}},
              {"extruder3", json{{"filament_detected", false}, {"channel_state", "wait_insert"}}}}},
        {"print_task_config",
         json{
             {"filament_exist", json::array({true, true, true, false})},
             {"filament_type", json::array({"PLA", "PETG", "ABS", ""})},
             {"filament_vendor", json::array({"Snapmaker", "Generic", "Generic", ""})},
             {"filament_color_rgba", json::array({"112233FF", "AA0000FF", "00BB00FF", "FFFFFFFF"})},
             {"extruder_map_table", json::array({0, 1, 2, 3})},
             {"extruders_used", json::array({true, false, false, false})},
             {"auto_replenish_filament", true},
             {"filament_entangle_sen", "medium"},
             {"end_unload_filament", json::array({false, true})}}},
        {"filament_motion_sensor e0_filament",
         json{{"enabled", true}, {"filament_detected", true}}},
        {"filament_motion_sensor e1_filament",
         json{{"enabled", true}, {"filament_detected", true}}},
        {"filament_motion_sensor e2_filament",
         json{{"enabled", true}, {"filament_detected", true}}},
        {"filament_motion_sensor e3_filament",
         json{{"enabled", true}, {"filament_detected", false}}},
        {"gcode_macro AUTO_FEEDING_BATCH", json{{"doing", false}}}};
}

// Pulling a spool from its feeder takes its filament out of the toolhead too.
const json t0_switch_off{
    {"filament_motion_sensor e0_filament", json{{"filament_detected", false}}}};
inline json golden_feed(int ext, const char* state, std::optional<bool> detected = std::nullopt) {
    json ch{{"channel_state", state}};
    if (detected) {
        ch["filament_detected"] = *detected;
    }
    return json{{ext <= 1 ? "filament_feed left" : "filament_feed right",
                 json{{"extruder" + std::to_string(ext), ch}}}};
}

const std::string kGolden = R"GOLD(full frame
action=Idle phase=-1 detail='' working=-1 tool=0 slot=0 loaded=true
  s0 Loaded PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
load: pick
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
load: feeding
action=Loading phase=2 detail='' working=1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=load_feeding detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
load: heating
action=Heating phase=3 detail='' working=1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=load_heating detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
load: finish
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Loaded PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=true pend=0 state=load_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
unload: prepare
action=Unloading phase=0 detail='' working=1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Loaded PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=true pend=0 state=unload_prepare detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=
unload: finish
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=true pend=0 state=load_finish detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
insert: port drops
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Empty PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=false pend=0 state=wait_insert detected=false
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
insert: port rises
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Snapmaker/PolyLite Orange #112233 n190-230 b60 w1000 latch=false pend=2 state=wait_insert detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
insert: reader answers NONE
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Empty // #FFFFFF n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=false
  batch_active=false port_present=1 events=1 gcodes= toasts=1 unloaded=1
insert: head 3 rises
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available // #FFFFFF n0-0 b0 w-1 latch=false pend=2 state=wait_insert detected=true
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
insert: head 3 tag read
action=Idle phase=-1 detail='' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=wait_insert detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=false port_present=1 events=1 gcodes= toasts=0 unloaded=1
batch: head 0 mid-load
action=Loading phase=2 detail='' working=0 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=load_feeding detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=true port_present=1 events=1 gcodes= toasts=0 unloaded=1
batch: head 0 fails
action=Error phase=-1 detail='Feeder 1: Load failed' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=load_fail detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=false port_present=1 events=1 gcodes=AUTO_FEEDING_BATCH ACTION=END toasts=0 unloaded=1
extruder: temperature only
action=Error phase=-1 detail='Feeder 1: Load failed' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=load_fail detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=false port_present=1 events=0 gcodes= toasts=0 unloaded=1
extruder: idle frame
action=Error phase=-1 detail='Feeder 1: Load failed' working=-1 tool=1 slot=1 loaded=true
  s0 Available PLA/Polymaker/PolyLite Orange #112233 n0-0 b0 w-1 latch=false pend=0 state=load_fail detected=true
  s1 Available PETG/Generic/ #AA0000 n0-0 b0 w-1 latch=false pend=0 state=unload_finish detected=true
  s2 Available ABS/Generic/ #00BB00 n0-0 b0 w-1 latch=false pend=0 state=preload_finish detected=true
  s3 Available PETG/Snapmaker/Basic #445566 n0-0 b0 w500 latch=false pend=0 state=wait_insert detected=true
  batch_active=false port_present=1 events=0 gcodes= toasts=0 unloaded=1
)GOLD";

inline json u1_all_parked_frame() {
    return json{{"extruder", json{{"state", "PARKED"}}},
                {"extruder1", json{{"state", "PARKED"}}},
                {"extruder2", json{{"state", "PARKED"}}},
                {"extruder3", json{{"state", "PARKED"}}},
                {"toolhead", json{{"extruder", "extruder2"}}}};
}

inline json make_task_filament(const std::array<std::string, 4>& types,
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
    MockPrinter printer;
    helix::test::RegisteredWired<AmsBackendSnapmaker> reg;
    helix::test::WiredBackend<AmsBackendSnapmaker>& backend;

    /// @p print_state is the print_stats.state the backend's first frame
    /// names; empty sends no print_stats at all.
    explicit FilamentRecordHarness(const std::string& name,
                                   const std::string& print_state = "standby")
        : tmp(name), reg(&printer.api), backend(*reg) {
        // The API mock's halt gate reads its own state.
        printer.state.update_from_status(json{{"webhooks", {{"state", "ready"}}}});
        auto store = std::make_unique<helix::ams::FilamentSlotOverrideStore>(
            &printer.api, "snapmaker", helix::ams::LaneKeyStyle::Tool);
        SnapmakerTestAccess::inject_override_store(backend, std::move(store));
        if (!print_state.empty()) {
            print(print_state);
        }
    }

    /// A frame naming only print_stats.state.
    void print(const std::string& state) {
        frame(json{{"print_stats", {{"state", state}}}});
    }

    void frame(const json& status) {
        backend.fire_status(status);
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    [[nodiscard]] std::vector<std::string> history() const {
        return printer.client.gcode_script_history();
    }

    [[nodiscard]] std::vector<std::string> config_writes() const {
        std::vector<std::string> out;
        for (const auto& s : printer.client.gcode_script_history()) {
            if (s.find("SET_PRINT_FILAMENT_CONFIG") != std::string::npos) {
                out.push_back(s);
            }
        }
        return out;
    }
};

inline SlotInfo inland_pla() {
    SlotInfo edit;
    edit.brand = "Inland";
    edit.spool_name = "My Custom Spool";
    edit.material = "PLA";
    edit.color_rgb = 0xFF5500;
    return edit;
}

/// Global printer state paused on a plain runout: recoverable, not Terminal.
inline void pause_on_runout() {
    PrinterState& ps = get_printer_state();
    ps.init_subjects(false);
    ps.update_from_status(json{{"webhooks", {{"state", "ready"}}},
                               {"print_stats", {{"state", "paused"}}},
                               {"virtual_sdcard", {{"is_active", false}}}});
}

inline size_t index_of_containing(const std::vector<std::string>& history,
                                  const std::string& needle) {
    for (size_t i = 0; i < history.size(); ++i) {
        if (history[i].find(needle) != std::string::npos) {
            return i;
        }
    }
    return history.size();
}

inline json make_rig_detect(const std::array<json, 4>& card_uids,
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

inline json make_detect_card(int head, const json& card_uid) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(i == head ? json{{"MAIN_TYPE", "NONE"}, {"CARD_UID", card_uid}}
                                 : json{{"MAIN_TYPE", "NONE"}});
    }
    return json{{"filament_detect", json{{"info", info}}}};
}

inline json make_task_routing(std::vector<int> map, std::vector<bool> used) {
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

inline void translucent_head_echoed(FilamentRecordHarness& h) {
    h.frame(make_feed_status(1, "load_finish"));
    REQUIRE(h.backend.get_slot_info(1).is_present());
    SlotInfo clear;
    clear.brand = "Inland";
    clear.material = "PETG";
    clear.color_rgb = 0xE8E8FF;
    clear.translucent = true;
    helix::test::edit_slot_as_user(h.backend, 1, clear);
    json echo = make_task_filament({"NONE", "PETG", "NONE", "NONE"});
    echo["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "E8E8FF80", "FFFFFFFF", "FFFFFFFF"});
    h.frame(echo);
}

inline json make_official_task(int head, const std::string& type, const std::string& rgba) {
    json ptc = make_task_filament({"NONE", "NONE", "NONE", "NONE"});
    ptc["print_task_config"]["filament_type"][head] = type;
    ptc["print_task_config"]["filament_official"][head] = true;
    ptc["print_task_config"]["filament_color_rgba"] =
        json::array({"FFFFFFFF", "FFFFFFFF", "FFFFFFFF", "FFFFFFFF"});
    ptc["print_task_config"]["filament_color_rgba"][head] = rgba;
    return ptc;
}

/// The filament_detect entry that same write leaves: filled, no card behind it.
inline json make_written_entry(int head, const std::string& type, const json& card_uid = 0) {
    json info = json::array();
    for (int i = 0; i < 4; ++i) {
        info.push_back(i == head ? json{{"MAIN_TYPE", type},
                                        {"VENDOR", "Generic"},
                                        {"ARGB_COLOR", 0xFFFF5500},
                                        {"CARD_UID", card_uid}}
                                 : json{{"MAIN_TYPE", "NONE"}});
    }
    return json{{"filament_detect", json{{"info", info}}}};
}

inline json make_head_record(int head, const std::string& type, const std::string& rgba) {
    json types = json::array();
    json colours = json::array();
    for (int i = 0; i < 4; ++i) {
        types.push_back(i == head ? type : "PLA");
        colours.push_back(i == head ? rgba : "FFFFFFFF");
    }
    return json{{"print_task_config",
                 {{"filament_type", types},
                  {"filament_color_rgba", colours},
                  {"filament_official", json::array({false, false, false, false})}}}};
}

inline SlotInfo polymaker_asa() {
    SlotInfo edit;
    edit.brand = "Polymaker";
    edit.material = "ASA";
    edit.color_rgb = 0xFF9800;
    return edit;
}

/// A harness whose head 0 holds a declared Polymaker ASA, written to the
/// firmware and echoed back, with its spool resting in the feeder port.
inline void declare_head0_asa(FilamentRecordHarness& h) {
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    helix::test::edit_slot_as_user(h.backend, 0, polymaker_asa());
    FilamentRecordHarness::drain();
    h.frame(make_head_record(0, "ASA", "FF9800FF"));
    h.printer.client.clear_gcode_script_history();
}

inline json heads_0_and_2_cleared() {
    return json{
        {"print_task_config",
         {{"filament_type", json::array({"NONE", "HIPS", "NONE", "ASA"})},
          {"filament_color_rgba", json::array({"FFFFFFFF", "E8E8E8FF", "FFFFFFFF", "FFFFFFFF"})},
          {"filament_official", json::array({false, false, false, false})}}}};
}

inline size_t writes_for_head(const FilamentRecordHarness& h, int head) {
    const std::string key = "CONFIG_EXTRUDER=" + std::to_string(head);
    size_t n = 0;
    for (const auto& w : h.config_writes()) {
        if (w.find(key) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

inline SlotInfo petg_blue() {
    SlotInfo edit = polymaker_asa();
    edit.material = "PETG";
    edit.color_rgb = 0x0000FF;
    return edit;
}

/// Every record write to @p head names @p type and @p rgba, and at least one
/// names a type.
inline void check_head_writes(const FilamentRecordHarness& h, int head, const std::string& type,
                              const std::string& rgba) {
    const std::string key = "CONFIG_EXTRUDER=" + std::to_string(head);
    size_t typed = 0;
    for (const auto& w : h.config_writes()) {
        if (w.find(key) == std::string::npos) {
            continue;
        }
        INFO(w);
        if (w.find("FILAMENT_TYPE=") != std::string::npos) {
            ++typed;
            CHECK(w.find("FILAMENT_TYPE=" + type + " ") != std::string::npos);
        }
        if (w.find("FILAMENT_COLOR_RGBA=") != std::string::npos) {
            CHECK(w.find("FILAMENT_COLOR_RGBA=" + rgba) != std::string::npos);
        }
    }
    CHECK(typed > 0);
}

/// Head 0 holds a declared Polymaker PLA in red, echoed, with its spool in
/// the feeder port. The user then edits it to @p edit (PETG in blue), and the
/// firmware has not stated the edit yet.
inline void edit_head0_unechoed(FilamentRecordHarness& h, const SlotInfo& edit = petg_blue()) {
    h.frame(make_feed_status(0, "unload_finish", /*filament_detected=*/true));
    SlotInfo pla = polymaker_asa();
    pla.material = "PLA";
    pla.color_rgb = 0xFF0000;
    helix::test::edit_slot_as_user(h.backend, 0, pla);
    FilamentRecordHarness::drain();
    h.frame(make_head_record(0, "PLA", "FF0000FF"));
    h.printer.client.clear_gcode_script_history();

    helix::test::edit_slot_as_user(h.backend, 0, edit);
    FilamentRecordHarness::drain();
    REQUIRE(h.config_writes().size() == 2);
    h.printer.client.clear_gcode_script_history();
}

inline SlotInfo petg_blue_translucent() {
    SlotInfo edit = petg_blue();
    edit.translucent = true;
    return edit;
}

/// Blue at the alpha a translucent filament is written with.
inline std::string blue_translucent_rgba() {
    return fmt::format("0000FF{:02X}", helix::filament_alpha(true));
}

/// Heads 0 and 2 each hold a declared spool resting in the feeder port.
inline void declare_heads_0_and_2(FilamentRecordHarness& h) {
    declare_head0_asa(h);
    h.frame(make_feed_status(2, "preload_finish", /*filament_detected=*/true));
    SlotInfo generic = polymaker_asa();
    generic.brand = "Generic";
    generic.color_rgb = 0x1A1A1A;
    helix::test::edit_slot_as_user(h.backend, 2, generic);
    FilamentRecordHarness::drain();
    h.frame(make_head_record(2, "ASA", "1A1A1AFF"));
    h.printer.client.clear_gcode_script_history();
}

inline json used_heads(const std::vector<bool>& used) {
    json arr = json::array();
    for (bool b : used) {
        arr.push_back(b);
    }
    return json{{"print_task_config", {{"extruders_used", arr}}}};
}

/// Head @p head's feeder, served automatically, reading @p state with no error.
inline json feeder(int head, const std::string& state) {
    const char* side = head <= 1 ? "filament_feed left" : "filament_feed right";
    return json{{side,
                 {{"extruder" + std::to_string(head),
                   {{"filament_detected", true},
                    {"channel_state", state},
                    {"channel_error", "ok"},
                    {"module_exist", true},
                    {"disable_auto", false}}}}}};
}

struct Warnings {
    std::vector<std::string> seen;
    Warnings() {
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& m) { seen.push_back(m); });
    }
    ~Warnings() {
        helix::ui::set_test_notification_warning_hook(nullptr);
    }
    Warnings(const Warnings&) = delete;
    Warnings& operator=(const Warnings&) = delete;
};

/// The file read answers @p tools and records the files it was asked about.
struct FileToolsProbe {
    std::vector<std::string> asked;
    FileToolsProbe(AmsBackendSnapmaker& b, std::set<int> tools) {
        SnapmakerTestAccess::set_file_tools_probe(
            b, [this, tools](const std::string& file, std::function<void(std::set<int>)> answer) {
                asked.push_back(file);
                answer(tools);
            });
    }
};

/// A slicer-started print: no task, heads 0 and 1 loaded, 2 and 3 not.
inline void slicer_print_ready(FilamentRecordHarness& h) {
    h.frame(used_heads({false, false, false, false}));
    h.frame(feeder(0, "load_finish"));
    h.frame(feeder(1, "manual_sta_finish"));
    h.frame(feeder(2, "unload_finish"));
    h.frame(feeder(3, "wait_insert"));
}

} // namespace helix::test
