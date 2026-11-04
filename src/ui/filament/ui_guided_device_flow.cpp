// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_guided_device_flow.h"

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

void register_builtin_guided_device_flows() {
    // A registrar compiled without its vendor's support is an empty stub, so every row
    // is safe to run in every build.
    static constexpr void (*kRegistrars[])() = {
        register_cfs_device_flows,
        register_snapmaker_device_flows,
        register_indx_device_flows,
    };
    for (auto* registrar : kRegistrars) {
        registrar();
    }
}

} // namespace helix::ui
