// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_update_queue.h"

#include "../test_helpers/panel_widget_size_harness.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "printer_discovery.h"
#include "safety_settings_manager.h"
#include "src/ui/panel_widgets/tool_switcher_widget.h"
#include "tool_state.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace helix {

/// A responsive token as this run's breakpoint resolves it, so the boxes the
/// tool switcher tests build come from the same numbers the widget reads.
inline int resolve_px(const char* name, int fallback) {
    const char* s = lv_xml_get_const(nullptr, name);
    return s ? std::atoi(s) : fallback;
}

/// The tile size whose content box (the tile less space_md each side) is
/// content_w x content_h: the widget decides on the content box.
inline std::pair<int, int> tile_for_content(int content_w, int content_h) {
    const int inset = resolve_px("space_md", 10);
    return {content_w + 2 * inset, content_h + 2 * inset};
}

template <typename W> void resize_content(PanelWidgetHarness<W>& h, int content_w, int content_h) {
    const auto [w, hpx] = tile_for_content(content_w, content_h);
    h.resize(1, 1, w, hpx);
}

/// Backend double: the tool map is what makes ToolState delegate to the backend
/// instead of the Tn gcode fallback, and change_tool() answers straight from a
/// flag so the refusal has somewhere to travel FROM. self_homes is settable
/// because it is the input that decides whether PAUSED is allowed.
///
/// change_tool() deliberately does NOT chain to AmsBackendMock: the mock runs
/// the change on its own thread and emits slot events, whose deferred AmsState
/// writes can outlive this fixture's teardown and land in a LATER test's
/// async-lifetime skip window (test_filament_dispatch_surfaces.cpp counts those
/// exactly). Nothing here asserts simulated motion, only the dispatch result.
class GateBackend : public AmsBackendMock {
  public:
    explicit GateBackend(int slots) : AmsBackendMock(slots) {}

    bool self_homes = false;
    bool tool_change_should_fail = false;
    int change_tool_calls = 0;
    int last_tool = -1;
    bool can_park = true;
    int park_calls = 0;
    /// Slot reads, which take the backend's mutex: what a widget refresh costs it.
    mutable int slot_info_calls = 0;

    /// The action get_system_info() reports in place of the mock's own; IDLE
    /// leaves the mock's.
    AmsAction report_action = AmsAction::IDLE;

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        if (report_action != AmsAction::IDLE) {
            info.action = report_action;
        }
        return info;
    }

    SlotInfo get_slot_info(int slot_index) const override {
        ++slot_info_calls;
        return AmsBackendMock::get_slot_info(slot_index);
    }

    BackendTraits traits() const override {
        BackendTraits t = AmsBackendMock::traits();
        t.filament_ops_self_home = self_homes;
        return t;
    }

    bool supports_tool_park() const override {
        return can_park;
    }

    AmsError park_active_tool() override {
        ++park_calls;
        return AmsErrorHelper::success();
    }

    bool has_feeder = false;
    bool feeder_is_open = false;
    /// Every set_feeder_open() request, in order: true opens, false closes.
    std::vector<bool> feeder_requests;

    bool supports_feeder_control() const override {
        return has_feeder;
    }

    bool feeder_open() const override {
        return feeder_is_open;
    }

    AmsError set_feeder_open(bool open) override {
        feeder_requests.push_back(open);
        return AmsErrorHelper::success();
    }

    AmsError change_tool(int tool_number) override {
        ++change_tool_calls;
        last_tool = tool_number;
        if (tool_change_should_fail) {
            return AmsError(AmsResult::TOOL_CHANGE_FAILED, "forced failure",
                            "Tool change did not complete", "Check the filament path and retry");
        }
        return AmsErrorHelper::success();
    }
};

/// A klipper-toolchanger printer with @p heads heads, each its own extruder,
/// driven through a GateBackend, with the warning and error toasts recorded.
/// Head 0 is mounted. "Confirm tool changes" is off, so a tap dispatches at
/// once; a test of the confirmation turns it on. Tears it all down on the way
/// out, so later test files inherit no tools, backend or setting.
struct ToolChangerRig {
    GateBackend* backend = nullptr;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool saved_confirm = SafetySettingsManager::instance().get_tool_change_require_confirmation();

    /// @p slots, where given, is the backend's lane count, for a rig whose
    /// lanes outnumber its heads.
    explicit ToolChangerRig(int heads, int slots = 0) {
        // Registered by name: the rail's repeat and its bindings read
        // tool_count and active_tool from XML.
        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(true);
        PrinterDiscovery hw;
        nlohmann::json objects = nlohmann::json::array({"toolchanger", "heater_bed", "gcode_move"});
        for (int i = 0; i < heads; ++i) {
            objects.push_back("tool T" + std::to_string(i));
            objects.push_back(i == 0 ? std::string("extruder") : "extruder" + std::to_string(i));
        }
        hw.parse_objects(objects);
        ToolState::instance().init_tools(hw);
        REQUIRE(ToolState::instance().tool_count() == heads);

        auto owned = std::make_unique<GateBackend>(slots > 0 ? slots : heads);
        owned->set_tool_changer_mode(true);
        owned->set_operation_delay(0);
        REQUIRE(owned->start());
        backend = owned.get();
        AmsState::instance().deinit_subjects();
        AmsState::instance().init_subjects(true);
        AmsState::instance().set_backend(std::move(owned));

        // The tile's tap callbacks, as the widget registry registers them before
        // any XML is built.
        register_tool_switcher_widget();

        ui::set_test_notification_warning_hook(
            [this](const std::string& msg) { warnings.push_back(msg); });
        ui::set_test_notification_error_hook(
            [this](const std::string& msg) { errors.push_back(msg); });
        SafetySettingsManager::instance().set_tool_change_require_confirmation(false);
    }

    ~ToolChangerRig() {
        SafetySettingsManager::instance().set_tool_change_require_confirmation(saved_confirm);
        ui::set_test_notification_warning_hook(nullptr);
        ui::set_test_notification_error_hook(nullptr);
        if (backend) {
            backend->wait_for_operation_thread();
            backend->stop();
        }
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        ui::UpdateQueue::instance().drain_until_empty();
    }

    ToolChangerRig(const ToolChangerRig&) = delete;
    ToolChangerRig& operator=(const ToolChangerRig&) = delete;

    /// Empty the carriage, as a toolchanger reports it.
    static void unmount() {
        ToolState::instance().update_from_status(
            nlohmann::json{{"toolchanger", {{"tool_number", -1}}}});
    }
};

} // namespace helix
