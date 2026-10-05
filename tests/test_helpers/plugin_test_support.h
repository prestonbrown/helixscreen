// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"
#include "lua_runtime.h"
#include "plugin_backend.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace helix::plugin::test {

/// A runtime whose faults are recorded instead of acted on.
struct TestRuntime {
    std::string fault;
    std::unique_ptr<LuaRuntime> rt;

    explicit TestRuntime(LuaRuntime::Limits limits = {},
                         std::string dir = "tests/fixtures/plugins/require-test",
                         std::string id = "test-plugin") {
        rt = std::make_unique<LuaRuntime>(std::move(id), std::move(dir), limits,
                                          [this](const std::string& r) { fault = r; });
    }

    bool run(const std::string& code) {
        return rt->run_string(code, "test");
    }

    /// A global's value as Lua's tostring() prints it.
    std::string global(const char* name) {
        lua_State* L = rt->state();
        lua_getglobal(L, name);
        std::string s = luaL_tolstring(L, -1, nullptr);
        lua_pop(L, 2);
        return s;
    }
};

/// Records every backend request so a test can inspect it and answer it later.
struct FakeBackend {
    struct Request {
        std::string kind; ///< gcode, call, upload, download, http
        std::string a;    ///< script, method, root, or HTTP method
        std::string b;    ///< path or URL
        std::string c;    ///< upload content or HTTP body
        json params;      ///< call params or HTTP headers
        RpcCallback reply;
        size_t cap = 0; ///< byte cap the binding asked for; downloads and http only
    };
    std::vector<Request> requests;
    std::vector<std::pair<std::string, std::function<void(const json&)>>> notify;
    /// Every set_plugin_objects call: (plugin id, its objects map).
    std::vector<std::pair<std::string, json>> object_sets;
    int notify_unregistered = 0;

    PluginBackend backend() {
        PluginBackend b;
        b.gcode = [this](const std::string& s, RpcCallback cb) {
            requests.push_back({"gcode", s, {}, {}, {}, std::move(cb), 0});
        };
        b.call = [this](const std::string& m, const json& p, RpcCallback cb) {
            requests.push_back({"call", m, {}, {}, p, std::move(cb), 0});
        };
        b.upload = [this](const std::string& r, const std::string& p, const std::string& c,
                          RpcCallback cb) {
            requests.push_back({"upload", r, p, c, {}, std::move(cb), 0});
        };
        b.download = [this](const std::string& r, const std::string& p, size_t cap,
                            RpcCallback cb) {
            requests.push_back({"download", r, p, {}, {}, std::move(cb), cap});
        };
        b.http = [this](const std::string& m, const std::string& u, const std::string& body,
                        const json& h, uint32_t, size_t cap, RpcCallback cb) {
            requests.push_back({"http", m, u, body, h, std::move(cb), cap});
        };
        b.on_notify = [this](const std::string& m, std::function<void(const json&)> h) {
            notify.emplace_back(m, std::move(h));
            return std::function<void()>([this] { ++notify_unregistered; });
        };
        b.set_plugin_objects = [this](const std::string& id, const json& objects) {
            object_sets.emplace_back(id, objects);
        };
        return b;
    }
};

/// A TestRuntime with a PluginContext and a fake backend, with core bindings installed plus
/// whichever `installers` a test asks for (install_widget_bindings included: PluginHost's
/// load path installs it, so a test driving helix.widget gets the same binding set).
struct BoundRuntime {
    FakeBackend fake;
    PluginBackend backend = fake.backend();
    Manifest manifest;
    json settings = json::object();
    int saves = 0;
    std::string storage_path;
    std::unique_ptr<PluginContext> ctx;
    TestRuntime t; // last member: destroyed first, while ctx and fake still exist

    explicit BoundRuntime(std::vector<Installer> installers = {}, PermissionSet perms = {},
                          std::vector<SettingDecl> decls = {}, std::string storage = {},
                          LuaRuntime::Limits limits = {})
        : storage_path(std::move(storage)), t(std::move(limits)) {
        manifest.id = "test-plugin";
        manifest.name = "Test Plugin";
        manifest.version = "1.0.0";
        manifest.permissions = std::move(perms);
        manifest.settings = std::move(decls);
        ctx = std::make_unique<PluginContext>(
            PluginContext{*t.rt, backend, manifest, &settings, [this] { ++saves; }, storage_path});
        install_core_bindings(*ctx);
        for (Installer install : installers)
            install(*ctx);
    }
};

/// A fresh directory under the system temp dir, removed with its contents on destruction.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device rd;
        path =
            std::filesystem::temp_directory_path() / ("helix-plugin-test-" + std::to_string(rd()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string file(const std::string& name) const {
        return (path / name).string();
    }
};

/// A whole file as one string; empty when unreadable.
inline std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace helix::plugin::test

#endif // HELIX_HAS_PLUGINS
