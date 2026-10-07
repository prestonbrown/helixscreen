// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "../test_helpers/scoped_env.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "plugin_source.h"
#include "plugin_source_app.h"
#include "printer_state.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::plugin;
using helix::plugin::test::read_text;
using helix::plugin::test::TempDir;

namespace {

constexpr const char* kFixtures = "tests/fixtures/plugins";

/// Serves a canned listing, or a failure, in place of the injected config root.
class ScriptedFiles : public MoonrakerFileAPIMock {
  public:
    explicit ScriptedFiles(helix::IMoonrakerClient& client) : MoonrakerFileAPIMock(client) {}

    std::vector<FileInfo> listing;
    bool fail = false;

    void list_files(const std::string& root, const std::string& path, bool recursive,
                    FileListCallback on_success, ErrorCallback on_error) override {
        (void)path;
        (void)recursive;
        if (fail) {
            if (on_error)
                on_error(MoonrakerError::unknown("mock listing failure", "list_files"));
            return;
        }
        (void)root;
        if (on_success)
            on_success(listing);
    }
};

/// Records every download_file_partial() call and serves canned bytes.
class RecordingTransfers : public MoonrakerFileTransferAPIMock {
  public:
    RecordingTransfers(helix::MoonrakerClient& client, const std::string& http_base_url)
        : MoonrakerFileTransferAPIMock(client, http_base_url) {}

    struct PartialCall {
        std::string root;
        std::string path;
        size_t max_bytes = 0;
    };
    std::vector<PartialCall> partials;
    std::string body = "recording-body";
    bool fail_next = false;

    void download_file_partial(const std::string& root, const std::string& path, size_t max_bytes,
                               StringCallback on_success, ErrorCallback on_error,
                               CancelFlag = nullptr) override {
        partials.push_back({root, path, max_bytes});
        if (fail_next) {
            if (on_error)
                on_error(MoonrakerError::unknown("mock partial failure", "download_file_partial"));
            return;
        }
        if (on_success)
            on_success(body);
    }
};

/// MoonrakerAPIMock with both file sub-APIs swapped for the fakes above.
class RecordingApi : public MoonrakerAPIMock {
  public:
    RecordingApi(helix::MoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPIMock(client, state), xfers_(client, "http://127.0.0.1:1"), files_(client) {}

    MoonrakerFileTransferAPI& transfers() override {
        return xfers_;
    }
    MoonrakerFileAPI& files() override {
        return files_;
    }

    RecordingTransfers xfers_;
    ScriptedFiles files_;
};

/// One listing round trip through the production deps.
std::vector<RemoteFile> listed(SourceDeps& deps, bool& ok) {
    std::vector<RemoteFile> files;
    bool done = false;
    deps.list([&](bool o, std::vector<RemoteFile> f) {
        ok = o;
        files = std::move(f);
        done = true;
    });
    REQUIRE(done);
    return files;
}

} // namespace

TEST_CASE("is_plugin_filelist_change matches only the plugin folder", "[plugin][source_app]") {
    auto msg = [](const std::string& root, const std::string& path) {
        return json{{"method", "notify_filelist_changed"},
                    {"params", json::array({{{"action", "create_file"},
                                             {"item", {{"root", root}, {"path", path}}}}})}};
    };
    CHECK(is_plugin_filelist_change(msg("config", "helixscreen/plugins/spark/main.lua")));
    CHECK(is_plugin_filelist_change(msg("config", "helixscreen/plugins/spark")));
    CHECK(is_plugin_filelist_change(msg("config", "helixscreen/plugins")));
    CHECK_FALSE(is_plugin_filelist_change(msg("config", "saved_variables.cfg")));
    CHECK_FALSE(is_plugin_filelist_change(msg("config", "helixscreen/settings.json")));
    CHECK_FALSE(is_plugin_filelist_change(msg("config", "helixscreen/plugins_extra/x.lua")));
    CHECK_FALSE(is_plugin_filelist_change(msg("gcodes", "helixscreen/plugins/x/main.lua")));
    CHECK_FALSE(is_plugin_filelist_change(json::object()));

    json moved = {
        {"params",
         json::array({{{"action", "move_file"},
                       {"item", {{"root", "config"}, {"path", "old/x.lua"}}},
                       {"source_item",
                        {{"root", "config"}, {"path", "helixscreen/plugins/spark/x.lua"}}}}})}};
    CHECK(is_plugin_filelist_change(moved));
}

TEST_CASE_METHOD(LVGLTestFixture, "make_moonraker_source_deps lists only the plugin folder",
                 "[plugin][source_app]") {
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({
        {"helixscreen/plugins/spark/main.lua", "-- spark"},
        {"helixscreen/plugins/spark/ui/tile.xml", "<tile/>"},
        {"saved_variables.cfg", "# vars"},
        {"helixscreen/settings.json", "{}"},
    });

    SourceDeps deps = make_moonraker_source_deps(&api);
    bool ok = false;
    const std::vector<RemoteFile> files = listed(deps, ok);

    REQUIRE(ok);
    REQUIRE(files.size() == 2);
    CHECK(files[0].path == "spark/main.lua");
    CHECK(files[0].size == 8); // "-- spark"
    CHECK(files[1].path == "spark/ui/tile.xml");
    CHECK(files[1].size == 7); // "<tile/>"
    for (const auto& f : files) {
        CHECK(f.path.rfind("helixscreen/", 0) != 0); // the prefix is stripped
        CHECK(f.path.find("saved_variables") == std::string::npos);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "source deps list skips directories and non-plugin paths",
                 "[plugin][source_app]") {
    MoonrakerClientMock client;
    helix::PrinterState state;
    RecordingApi api(client, state);

    FileInfo dir_entry;
    dir_entry.path = "helixscreen/plugins/spark";
    dir_entry.is_dir = true;
    FileInfo lua;
    lua.path = "helixscreen/plugins/spark/main.lua";
    lua.size = 8;
    lua.modified = 12.5;
    FileInfo stray;
    stray.path = "saved_variables.cfg";
    stray.size = 3;
    api.files_.listing = {dir_entry, lua, stray};

    SourceDeps deps = make_moonraker_source_deps(&api);
    bool ok = false;
    const std::vector<RemoteFile> files = listed(deps, ok);

    REQUIRE(ok);
    REQUIRE(files.size() == 1);
    CHECK(files[0].path == "spark/main.lua");
    CHECK(files[0].size == 8);
    CHECK(files[0].modified == 12.5);
}

TEST_CASE_METHOD(LVGLTestFixture, "source deps list reports a failed listing",
                 "[plugin][source_app]") {
    MoonrakerClientMock client;
    helix::PrinterState state;
    RecordingApi api(client, state);
    api.files_.fail = true;

    SourceDeps deps = make_moonraker_source_deps(&api);
    bool ok = true;
    const std::vector<RemoteFile> files = listed(deps, ok);

    CHECK_FALSE(ok);
    CHECK(files.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "source deps download passes the byte cap through",
                 "[plugin][source_app]") {
    MoonrakerClientMock client;
    helix::PrinterState state;
    RecordingApi api(client, state);
    SourceDeps deps = make_moonraker_source_deps(&api);

    TempDir tmp;
    const std::string dest = tmp.file("main.lua");
    bool ok = false;
    deps.download("wd/main.lua", dest, 4097, [&](bool o, const std::string&) { ok = o; });

    REQUIRE(api.xfers_.partials.size() == 1);
    CHECK(api.xfers_.partials[0].root == "config");
    CHECK(api.xfers_.partials[0].path == "helixscreen/plugins/wd/main.lua");
    CHECK(api.xfers_.partials[0].max_bytes == 4097);
    REQUIRE(ok);
    CHECK(read_text(dest) == api.xfers_.body);

    api.xfers_.fail_next = true;
    bool ok2 = true;
    std::string error;
    deps.download("wd/ui/x.xml", tmp.file("x.xml"), 100, [&](bool o, const std::string& e) {
        ok2 = o;
        error = e;
    });
    CHECK_FALSE(ok2);
    CHECK_FALSE(error.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "HELIX_MOCK_PLUGINS_DIR serves the plugin fixtures",
                 "[plugin][source_app]") {
    helix::ScopedEnv plugins_dir("HELIX_MOCK_PLUGINS_DIR", kFixtures);
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPIMock api(client, state);
    SourceDeps deps = make_moonraker_source_deps(&api);

    bool ok = false;
    const std::vector<RemoteFile> files = listed(deps, ok);
    REQUIRE(ok);

    const std::string fixture = std::string(kFixtures) + "/widget-demo/manifest.json";
    const auto it = std::find_if(files.begin(), files.end(), [](const RemoteFile& f) {
        return f.path == "widget-demo/manifest.json";
    });
    REQUIRE(it != files.end());
    std::error_code ec;
    CHECK(it->size == std::filesystem::file_size(fixture, ec));

    TempDir tmp;
    const std::string dest = tmp.file("manifest.json");
    bool dl_ok = false;
    std::string error;
    deps.download("widget-demo/manifest.json", dest, 1 << 20, [&](bool o, const std::string& e) {
        dl_ok = o;
        error = e;
    });
    REQUIRE(dl_ok);
    CHECK(error.empty());
    CHECK(read_text(dest) == read_text(fixture));
}

#endif // HELIX_HAS_PLUGINS
