// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_permissions.h"

#include <array>
#include <utility>

namespace helix::plugin {

namespace {
constexpr std::array<std::pair<Permission, const char*>, 4> kNames{{
    {Permission::Gcode, "gcode"},
    {Permission::MoonrakerWrite, "moonraker_write"},
    {Permission::Http, "http"},
    {Permission::Storage, "storage"},
}};

// Also the only server.helix.* methods a plugin may call besides the guarded
// print_modified: reject_plugin_rpc refuses every other one.
constexpr std::array<std::string_view, 16> kReadonlyMethods{
    "printer.objects.query",
    "printer.objects.list",
    "server.info",
    "server.files.list",
    "server.files.metadata",
    "server.temperature_store",
    "machine.system_info",
    "server.helix.status",
    "server.helix.plr.status",
    "server.helix.3mf_conversions",
    "server.helix.analysis",
    "server.helix.journal",
    "server.helix.config",
    "server.helix.storage",
    "server.helix.filament_usage",
    "server.helix.signal_migration",
};
} // namespace

std::optional<Permission> permission_from_string(std::string_view name) {
    for (const auto& [p, n] : kNames) {
        if (name == n)
            return p;
    }
    return std::nullopt;
}

const char* permission_name(Permission p) {
    for (const auto& [q, n] : kNames) {
        if (p == q)
            return n;
    }
    return "?";
}

std::vector<Permission> permission_growth(const PermissionSet& granted,
                                          const PermissionSet& requested) {
    std::vector<Permission> grown;
    for (Permission p : requested) {
        if (!granted.count(p))
            grown.push_back(p);
    }
    return grown;
}

bool is_readonly_moonraker_method(std::string_view method) {
    for (auto m : kReadonlyMethods) {
        if (method == m)
            return true;
    }
    return false;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
