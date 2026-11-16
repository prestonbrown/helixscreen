// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "../test_helpers/plugin_test_support.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "plugin_source.h"
#include "plugin_source_app.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;
namespace fs = std::filesystem;

namespace {

constexpr const char* kFixtures = "tests/fixtures/plugins";

/// Serves an in-memory plugin folder, counting listings so a test can prove a
/// burst of change requests coalesced into one sync.
struct FakeRemote {
    std::map<std::string, std::pair<std::string, double>> files; ///< path -> (content, mtime)
    int list_calls = 0;

    SourceDeps deps() {
        SourceDeps d;
        d.list = [this](std::function<void(bool, std::vector<RemoteFile>)> cb) {
            ++list_calls;
            std::vector<RemoteFile> out;
            for (const auto& [p, v] : files)
                out.push_back({p, v.first.size(), v.second});
            cb(true, out);
        };
        d.download = [this](const std::string& p, const std::string& dest, size_t,
                            std::function<void(bool, std::string)> cb) {
            fs::create_directories(fs::path(dest).parent_path());
            std::ofstream(dest, std::ios::binary) << files.at(p).first;
            cb(true, {});
        };
        return d;
    }

    /// Copies one fixture plugin's files into the remote map as "<id>/<rest>".
    void serve_fixture(const std::string& id) {
        const fs::path root = fs::path(kFixtures) / id;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file())
                files[id + "/" + fs::relative(entry.path(), root).string()] = {
                    read_text(entry.path()), 100.0};
        }
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a sync that adds a plugin reaches the host",
                 "[plugin][wiring]") {
    TempDir cache;
    // A crash between a swap's two renames leaves .old-<id> behind: the boot
    // scan must not list it as a plugin row.
    fs::create_directories(cache.path / ".old-junk");
    std::ofstream(cache.path / ".old-junk" / "manifest.json", std::ios::binary) << "{}";
    FakeRemote remote;
    remote.serve_fixture("widget-demo");
    HostRig rig; // nothing enabled
    rig.host->load_from(cache.path.string());
    REQUIRE(rig.host->plugins().empty());

    PluginSyncDriver driver(*rig.host, remote.deps(), cache.path.string());
    driver.sync_now();
    drain();

    CHECK(remote.list_calls == 1);
    REQUIRE(rig.host->plugins().size() == 1);
    CHECK(rig.host->plugins()[0].dir_name == "widget-demo");
    CHECK(rig.host->plugins()[0].status == PluginStatus::Disabled);
    CHECK(rig.host->runtime("widget-demo") == nullptr);
    // Widget definitions only change when a plugin loads; Disabled loads none.
    CHECK(read_text(cache.path / "widget-demo" / "manifest.json") ==
          read_text(fs::path(kFixtures) / "widget-demo" / "manifest.json"));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "an update that grows permissions unloads the plugin and needs approval",
                 "[plugin][wiring]") {
    TempDir cache;
    FakeRemote remote;
    remote.serve_fixture("hello");
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from(cache.path.string());

    PluginSyncDriver driver(*rig.host, remote.deps(), cache.path.string());
    driver.sync_now();
    drain();
    REQUIRE(rig.host->runtime("hello") != nullptr);

    json m =
        json::parse(read_text(fs::path(kFixtures) / "hello" / "manifest.json"), nullptr, false);
    REQUIRE(m.is_object());
    m["permissions"] = json::array({"gcode", "http"});
    remote.files["hello/manifest.json"] = {m.dump(), 200.0};
    driver.sync_now();
    drain();

    const PluginInfo* info = rig.info("hello");
    REQUIRE(info != nullptr);
    CHECK(info->status == PluginStatus::NeedsApproval);
    CHECK(rig.host->runtime("hello") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "a burst of change requests runs one sync", "[plugin][wiring]") {
    TempDir cache;
    FakeRemote remote;
    remote.serve_fixture("widget-demo");
    HostRig rig;
    rig.host->load_from(cache.path.string());

    PluginSyncDriver driver(*rig.host, remote.deps(), cache.path.string(), 1500);
    for (int i = 0; i < 5; ++i) {
        driver.request_sync();
        process_lvgl(200);
    }
    CHECK(remote.list_calls == 0); // nothing before the debounce

    process_lvgl(2000);
    CHECK(remote.list_calls == 1);
    drain();
    REQUIRE(rig.host->plugins().size() == 1);
    CHECK(rig.host->plugins()[0].dir_name == "widget-demo");
}

TEST_CASE_METHOD(LVGLTestFixture, "the driver dies before its timer fires", "[plugin][wiring]") {
    TempDir cache;
    FakeRemote remote;
    HostRig rig;
    rig.host->load_from(cache.path.string());
    {
        PluginSyncDriver driver(*rig.host, remote.deps(), cache.path.string());
        driver.request_sync();
    }
    process_lvgl(3000);
    drain();
    CHECK(remote.list_calls == 0);
    CHECK(rig.host->plugins().empty());
}

TEST_CASE("plugin_cache_dir_for keys the cache by printer", "[plugin][wiring]") {
    TempDir tmp;
    helix::ScopedEnv cache_env("HELIX_CACHE_DIR", tmp.path.string().c_str());

    const std::string a = plugin_cache_dir_for("printer-1");
    const std::string b = plugin_cache_dir_for("printer-2");
    const std::string d = plugin_cache_dir_for("");

    const std::string root = get_helix_cache_dir("plugins");
    CHECK(a != b);
    CHECK(a.rfind(root + "/", 0) == 0);
    CHECK(b.rfind(root + "/", 0) == 0);
    CHECK(d == root + "/default");
}

TEST_CASE("plugin_cache_dir_for keeps a hostile printer id inside the cache root",
          "[plugin][wiring]") {
    TempDir tmp;
    helix::ScopedEnv cache_env("HELIX_CACHE_DIR", tmp.path.string().c_str());
    const std::string root = get_helix_cache_dir("plugins");

    CHECK(plugin_cache_dir_for("../../x") == root + "/------x");
    CHECK(plugin_cache_dir_for("/etc/passwd") == root + "/-etc-passwd");
    CHECK(plugin_cache_dir_for("") == root + "/default");
    CHECK(plugin_cache_dir_for("printer-1") == root + "/printer-1");
    // Two ids differing only in a byte the map rewrites share one cache dir.
    // Acceptable degradation: both stay per-printer and inside the cache root.
    CHECK(plugin_cache_dir_for("printer/1") == plugin_cache_dir_for("printer-1"));
}

TEST_CASE_METHOD(LVGLTestFixture, "a synced folder the host cannot load never counts as new",
                 "[plugin][wiring]") {
    TempDir cache;
    FakeRemote remote;
    remote.serve_fixture("hello");
    remote.files["no-manifest/notes.txt"] = {"data", 100.0};
    HostRig rig;
    rig.host->load_from(cache.path.string());

    std::vector<std::string> changed;
    PluginSyncDriver driver(*rig.host, remote.deps(), cache.path.string());
    driver.on_synced = [&changed](const SyncResult& result) { changed = result.changed; };
    driver.sync_now();
    drain();

    REQUIRE(changed.size() == 2);
    // A rescan drops a folder with no manifest from the host's rows, so the
    // pick below only has to look at what the host kept.
    CHECK(rig.info("no-manifest") == nullptr);
    const std::vector<std::string> fresh = loadable_plugin_ids(changed, rig.host->plugins());
    REQUIRE(fresh.size() == 1);
    CHECK(fresh[0] == "hello");
}

TEST_CASE_METHOD(LVGLTestFixture, "no new-plugin pick when every synced folder is unloadable",
                 "[plugin][wiring]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["no-manifest/notes.txt"] = {"data", 100.0};
    HostRig rig;
    rig.host->load_from(cache.path.string());

    std::vector<std::string> changed;
    PluginSyncDriver driver(*rig.host, remote.deps(), cache.path.string());
    driver.on_synced = [&changed](const SyncResult& result) { changed = result.changed; };
    driver.sync_now();
    drain();

    REQUIRE(changed.size() == 1);
    CHECK(loadable_plugin_ids(changed, rig.host->plugins()).empty());
}

#endif // HELIX_HAS_PLUGINS
