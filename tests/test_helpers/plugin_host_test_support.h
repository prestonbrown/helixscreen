// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_HAS_PLUGINS

#include "core/update_queue.h"
#include "plugin_host.h"
#include "plugin_test_support.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace helix::plugin::test {

/// A PluginHost over a FakeBackend and an in-memory settings block.
struct HostRig {
    FakeBackend fake;
    json block;
    int writes = 0;
    std::unique_ptr<PluginHost> host;

    explicit HostRig(json initial = json::object(), size_t budget = size_t(64) << 20)
        : block(std::move(initial)) {
        PluginHost::Deps d;
        d.backend = fake.backend();
        d.read_block = [this] { return block; };
        d.write_block = [this](const json& j) {
            block = j;
            ++writes;
        };
        d.settings_path = "/tmp/helix-plugin-host-test/settings.json";
        d.helix_version = "1.1.0";
        d.memory_budget = budget;
        register_plugin_event_callback();
        host = std::make_unique<PluginHost>(std::move(d));
    }

    const PluginInfo* info(const std::string& dir) {
        for (const auto& p : host->plugins()) {
            if (p.dir_name == dir)
                return &p;
        }
        return nullptr;
    }
};

/// The /plugins settings block enabling `id` with `perms`.
inline json enabled(const std::string& id, std::vector<std::string> perms) {
    return json{{"enabled", {{id, {{"version", "1.0.0"}, {"permissions", perms}}}}}};
}

/// The /plugins settings block enabling every given id with its permissions.
inline json enabled_all(std::vector<std::pair<std::string, std::vector<std::string>>> ids) {
    json block = json{{"enabled", json::object()}};
    for (auto& [id, perms] : ids)
        block["enabled"][id] = enabled(id, std::move(perms))["enabled"][id];
    return block;
}

inline void drain() {
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace helix::plugin::test

#endif // HELIX_HAS_PLUGINS
