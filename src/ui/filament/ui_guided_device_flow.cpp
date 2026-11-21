// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_guided_device_flow.h"

#include "ui_resume_dispatch.h"

#include "ams_state.h"
#include "app_globals.h"
#include "error_event.h"
#include "filament_op_execute.h"
#include "src/ui/panel_widgets/filament_ops_widget.h"

#include <map>

namespace helix::ui {

namespace {
std::map<std::string, GuidedDeviceFlow>& registry() {
    static std::map<std::string, GuidedDeviceFlow> flows;
    return flows;
}
} // namespace

void register_guided_device_flow(const std::string& id, GuidedDeviceFlow flow) {
    registry()[id] = std::move(flow);
}

void unregister_guided_device_flow(const std::string& id) {
    registry().erase(id);
}

const GuidedDeviceFlow* find_guided_device_flow(const std::string& id) {
    auto it = registry().find(id);
    return it == registry().end() ? nullptr : &it->second;
}

void register_recovery_flows() {
    register_guided_device_flow(kOpenFilamentFlowId,
                                {nullptr, [](const std::string&) { open_filament_controls(); }});
    register_guided_device_flow(kLoadLaneFlowId,
                                {nullptr, nullptr, [](int lane) {
                                     execute_filament_load(AmsState::instance().get_backend(), lane,
                                                           "[RecoveryFlow]");
                                 }});
    // The fault was an unload, so the lane holds filament to take out.
    register_guided_device_flow(kUnloadLaneFlowId,
                                {nullptr, nullptr, [](int lane) {
                                     execute_filament_unload(AmsState::instance().get_backend(),
                                                             lane, /*target_is_loaded=*/true,
                                                             "[RecoveryFlow]");
                                 }});
    register_guided_device_flow(kCancelPrintFlowId, {nullptr, [](const std::string&) {
                                                         dispatch_cancel_print(get_moonraker_api(),
                                                                               "[RecoveryFlow]");
                                                     }});
}

void register_builtin_guided_device_flows() {
    // A registrar compiled without its vendor's support is an empty stub, so every row
    // is safe to run in every build.
    static constexpr void (*kRegistrars[])() = {
        register_recovery_flows,
        register_cfs_device_flows,
        register_snapmaker_device_flows,
        register_indx_device_flows,
    };
    for (auto* registrar : kRegistrars) {
        registrar();
    }
}

} // namespace helix::ui
