// tests/test_helpers/toolchanger_mount_feed_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What these test files share: their fixtures, helpers and includes.
// test_toolchanger_mount_feed.cpp, test_toolchanger_mount_feed_2.cpp.
// Generated from the first of those files by split_v3.py, which keeps every
// case's text and order and moves everything else here.
#pragma once

#include "ui_ams_device_operations_overlay.h"
#include "ui_batch_filament_modal.h"

#include "../busy_guard_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "app_globals.h"
#include "cleanup_hold.h"
#include "config.h"
#include "connection_state.h"
#include "core/update_queue.h"
#include "filament_op_slot_resolver.h"
#include "macro_param_cache.h"
#include "mount_feed_sequence.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "test_helpers/print_state_test_drivers.h"
#include "test_helpers/step_operation_helpers.h"
#include "test_helpers/toolchanger_test_access.h"
#include "test_helpers/toolchanger_test_helper.h"
#include "test_helpers/zero_lane_numbering.h"
#include "toolchanger_addon.h"
#include "wizard_config_paths.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix::test {

using namespace helix;
namespace mf = helix::mount_feed;
using json = nlohmann::json;

inline mf::Request load_request() {
    mf::Request r;
    r.load = true;
    r.heads = {{/*head=*/2, /*temp_c=*/215, /*mount_gcode=*/"SELECT_TOOL TOOL=\"tool T2\""}};
    r.restore_gcode = "SELECT_TOOL TOOL=\"tool T0\"";
    r.prompt_title = "Load filament";
    r.prompt_text = "Insert filament into Tool 3, then tap Continue";
    r.continue_label = "Continue";
    r.cancel_label = "Cancel";
    return r;
}

/// A Load of tools 1 and 3 (0-based) with tool 0 on the carriage at the start.
inline mf::Request multi_load_request() {
    mf::Request r;
    r.load = true;
    r.heads = {{1, 210, "SELECT_TOOL TOOL=\"tool T1\""}, {3, 220, "SELECT_TOOL TOOL=\"tool T3\""}};
    r.restore_gcode = "SELECT_TOOL TOOL=\"tool T0\"";
    r.prompt_title = "Load filament";
    r.prompt_text = "Insert filament into Tools 2, 4 as far as it goes, then tap Continue";
    r.continue_label = "Continue";
    r.cancel_label = "Cancel";
    return r;
}

inline std::vector<std::string> gcodes(const std::vector<mf::Stage>& stages) {
    std::vector<std::string> out;
    for (const auto& s : stages) {
        out.push_back(s.gcode);
    }
    return out;
}

/// A MedusaHC-shaped changer whose acks the test hands out one at a time.
class AckingChanger : public ToolChangerHelper {
  public:
    explicit AckingChanger(int tools) : ToolChangerHelper(tools) {
        set_mounts_to_feed(true);
    }

    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()> on_complete,
                           helix::SharedCallback<void(const MoonrakerError&)> on_error,
                           bool /*silent*/) override {
        sent_.push_back(gcode);
        acks_.push_back(std::move(on_complete));
        errors_.push_back(std::move(on_error));
        return AmsErrorHelper::success();
    }

    /// Ack the newest stage and let the deferred handlers run.
    void ack() {
        REQUIRE_FALSE(acks_.empty());
        auto cb = acks_.back();
        cb();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The newest stage dropped with the connection, as the request tracker
    /// fails everything in flight when the socket or Klippy goes away.
    void lose() {
        REQUIRE_FALSE(errors_.empty());
        errors_.back()(MoonrakerError::connection_lost("printer.gcode.script"));
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The stage sent @p index-th dropped with the connection, after newer
    /// ones went out.
    void lose_at(size_t index) {
        REQUIRE(index < errors_.size());
        errors_[index](MoonrakerError::connection_lost("printer.gcode.script"));
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    void fail(const std::string& message = "simulated") {
        REQUIRE_FALSE(errors_.empty());
        MoonrakerError err;
        err.message = message;
        errors_.back()(err);
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

  private:
    std::vector<helix::SharedCallback<void()>> acks_;
    std::vector<helix::SharedCallback<void(const MoonrakerError&)>> errors_;
};

struct RestorePurgeSetting {
    ~RestorePurgeSetting() {
        helix::SettingsManager::instance().set_mount_feed_purge_mm(mf::kDefaultPurgeMm);
    }
};

inline PrinterDiscovery pin_watch_medusahc_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array(
        {"medusahc", "pin_watch io", "extruder", "extruder1", "extruder2", "extruder3"}));
    return hw;
}

inline json pin_watch_frame(int current_tool) {
    return json{{"pin_watch io", {{"current_tool", current_tool}}}};
}

class LiveApiChanger : public AmsBackendToolChanger {
  public:
    LiveApiChanger(IMoonrakerAPI* api, IMoonrakerClient* client)
        : AmsBackendToolChanger(api, client) {
        set_discovered_tools({"T0", "T1", "T2", "T3"});
        set_mounts_to_feed(true);
        running_ = true;
    }
    ~LiveApiChanger() override {
        helix::ui::UpdateQueue::instance().drain();
    }
    [[nodiscard]] bool toolhead_homed() const override {
        return homed;
    }
    void feed(const json& status) {
        handle_status_update(
            json{{"method", "notify_status_update"}, {"params", json::array({status, 0.0})}});
    }
    /// Whether every stage may skip the home first.
    bool homed = true;
};

inline PrinterDiscovery irbis_medusahc_discovery() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"toolchanger", "tool T0", "tool T1", "tool T2", "tool T3",
                                  "medusahc", "pin_watch io", "extruder", "extruder1"}));
    return hw;
}

/// A sensed MedusaHC on tool 0, with a Load of tool 1 at its Mount stage.
inline void start_load_to_mount(AckingChanger& tc) {
    tc.set_tool_sensor(toolchanger_addon::resolve_tool_sensor(pin_watch_medusahc_discovery()));
    tc.feed(pin_watch_frame(0));
    REQUIRE(tc.load_filament(1).success());
    tc.ack(); // the prompt is up
    ToolChangerTestAccess::feed_operator_reply(tc, mf::Reply::Continue);
    REQUIRE(tc.sent_.back() == mf::prompt_end_gcode() + "\nSELECT_TOOL TOOL=\"tool T1\"");
}

struct ScopedGcodeToolStyle {
    ScopedGcodeToolStyle() {
        auto& settings = helix::SettingsManager::instance();
        settings.init_subjects();
        settings.set_tool_number_style_setting(helix::ui::ToolNumberStyleSetting::Gcode);
    }
    ~ScopedGcodeToolStyle() {
        helix::SettingsManager::instance().set_tool_number_style_setting(
            helix::ui::ToolNumberStyleSetting::Auto);
    }
};

struct PrintableChanger : AckingChanger {
    explicit PrintableChanger(int tools) : AckingChanger(tools) {}
    bool printing = false;
    bool print_holds_printer() const override {
        return printing;
    }
};

inline void start_printing(BusyGuardFixture& f) {
    helix::test::set_wire_state(f.state, PrintJobState::PRINTING);
}

class ConnectionRecordingBackend : public AmsBackendMock {
  public:
    ConnectionRecordingBackend() : AmsBackendMock(4) {}
    int lost = 0;
    void on_connection_lost() override {
        ++lost;
    }
};

} // namespace helix::test
