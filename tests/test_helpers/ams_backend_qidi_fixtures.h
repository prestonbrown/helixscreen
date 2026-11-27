// tests/test_helpers/ams_backend_qidi_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What these test files share: their fixtures, helpers and includes.
// test_ams_backend_qidi.cpp, test_ams_backend_qidi_2.cpp, test_ams_backend_qidi_3.cpp.
// Generated from the first of those files by split_v3.py, which keeps every
// case's text and order and moves everything else here.
#pragma once

#include "../lvgl_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_backend_qidi.h"
#include "ams_device_action_value.h"
#include "ams_error.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/temperature_units.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "notice.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "test_helpers/backend_harness.h"
#include "test_helpers/backend_user_edit.h"
#include "test_helpers/filament_slot_override_store_test_access.h"
#include "test_helpers/mock_printer.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/qidi_box_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"
#include "test_helpers/zero_lane_numbering.h"
#include "ui/ams_drawing_utils.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix::test {

using helix::AmsAction;
using helix::AmsBackendQidi;
using helix::AmsType;
using helix::DryerInfo;
using helix::kAmsDefaultSlotColor;
using helix::PathTopology;
using helix::QidiBoxTestAccess;
using helix::SlotInfo;
using helix::SlotStatus;
using json = nlohmann::json;

class RecordingQidiBackend : public AmsBackendQidi {
  public:
    using AmsBackendQidi::AmsBackendQidi;
    helix::AmsError execute_gcode(const std::string& gcode) override {
        if (fail_dispatch) {
            return helix::AmsErrorHelper::not_supported("fixture dispatch failure");
        }
        sent.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    // The completion form records each line of the script; `reject` answers
    // as Klipper does when a command raises, after the send.
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete,
                                  helix::SharedCallback<void(const MoonrakerError&)> on_error,
                                  bool /*silent*/) override {
        if (fail_dispatch) {
            return helix::AmsErrorHelper::not_supported("fixture dispatch failure");
        }
        for (const auto line : helix::text_io::lines(gcode)) {
            sent.emplace_back(line);
        }
        if (hold) {
            held_complete = on_complete;
            held_error = on_error;
        } else if (reject) {
            if (on_error) {
                MoonrakerError err;
                err.message = "rejected";
                on_error(err);
            }
        } else if (on_complete) {
            on_complete();
        }
        return helix::AmsErrorHelper::success();
    }
    bool reject = false;
    // While set, a completion form's answer waits for the test to fire it.
    bool hold = false;
    helix::SharedCallback<void()> held_complete;
    helix::SharedCallback<void(const MoonrakerError&)> held_error;
    // While set, every dispatch fails - for the writes whose echo can never
    // arrive because the command never reached the box.
    bool fail_dispatch = false;
    std::vector<std::string> sent;
};

static void clear_as_the_menu_does(helix::AmsBackend& backend, int slot) {
    SlotInfo cleared = backend.get_slot_info(slot);
    cleared.material.clear();
    cleared.color_rgb = kAmsDefaultSlotColor;
    cleared.brand.clear();
    cleared.clear_spoolman_link();
    (void)backend.commit_user_edit(slot, backend.get_slot_info(slot), cleared);
    backend.clear_slot_override(slot);
}

static const char* kStockFilasExcerpt = R"INI(
[fila1]
filament                       = PLA Rapido
min_temp                       = 190
max_temp                       = 240
box_min_temp                   = 0
box_max_temp                   = 0
type                           = PLA

[fila11]
filament                       = ABS Rapido
min_temp                       = 240
max_temp                       = 280
box_min_temp                   = 0
box_max_temp                   = 45
type                           = ABS

[fila42]
filament                       = PETG-CF
min_temp                       = 240
max_temp                       = 270
box_min_temp                   = 0
box_max_temp                   = 45
type                           = PETG-CF

[colordict]
1                              = #FAFAFA
2                              = #060606
18                             = #FF362D
24                             = #B87F2B

[vendor_list]
0                              = Generic
1                              = QIDI
2                              = eSUN
)INI";

struct PausedPrint {
    PausedPrint() {
        get_printer_state().update_from_status(json{{"pause_resume", {{"is_paused", true}}}});
    }
    ~PausedPrint() {
        get_printer_state().update_from_status(json{{"pause_resume", {{"is_paused", false}}}});
    }
};

inline bool offers_try_again(const std::optional<helix::ErrorEvent>& ev) {
    return ev &&
           std::any_of(ev->recovery_actions.begin(), ev->recovery_actions.end(),
                       [](const helix::RecoveryAction& a) { return a.gcode == "TRY_MOVE_AGAIN"; });
}

class RecordingQidiWithApi : public AmsBackendQidi {
  public:
    RecordingQidiWithApi(MoonrakerAPI* api, helix::MoonrakerClient* client)
        : AmsBackendQidi(api, client) {}
    helix::AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }
    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete) override {
        sent.push_back(gcode);
        (void)on_complete;
        return helix::AmsErrorHelper::success();
    }
    std::vector<std::string> sent;
};

struct QidiHomingGuardFixture : public LVGLTestFixture {
    QidiHomingGuardFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        api = std::make_unique<MoonrakerAPIMock>(mock_client, state);
        backend = std::make_unique<RecordingQidiWithApi>(api.get(), &mock_client);
        // Force the synchronous execute_gcode() path (no CLEAR_NOZZLE wipe →
        // no ensure_homed_then() async query), so a STANDBY load emits gcode
        // synchronously and `sent` is deterministic. M603 keeps unload one-line.
        QidiBoxTestAccess::set_fw_caps(*backend, /*m603=*/true, /*clear_nozzle=*/false);
    }
    void set_print_state(helix::PrintJobState s) {
        helix::test::set_wire_state(state, s);
    }
    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<RecordingQidiWithApi> backend;
};

struct QidiTmpCacheDir {
    std::filesystem::path path;
    explicit QidiTmpCacheDir(const std::string& suffix) {
        path = std::filesystem::temp_directory_path() /
               ("qidi_cache_" + suffix + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~QidiTmpCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

static json make_controller_slot_notification(int box, int slot_in_box, const json& fields) {
    json box_obj = json::object();
    const std::string prefix =
        "box" + std::to_string(box) + "_filament_info_" + std::to_string(slot_in_box) + "_";
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        box_obj[prefix + it.key()] = it.value();
    }
    return json{{"multi_color_controller", json{{"box", box_obj}}}};
}

static const json kControllerGenericPla{
    {"filament_id", "QFA90"},  {"filament_type", "PLA"},
    {"vendor_str", "Generic"}, {"color_rgb_str", json::array({"228332"})},
    {"min_temp", 190},         {"max_temp", 240}};

} // namespace helix::test
