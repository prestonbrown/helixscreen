// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_file_provider_generation.cpp
 * @brief PrintSelectFileProvider per-request generation guard (#912).
 *
 * When a get_directory refresh is reissued (e.g. after PrintSelectPanel's 30s
 * stuck-refresh self-heal presumes the first response lost), a late-arriving
 * original response must NOT fire on_files_ready a second time — that causes an
 * extra metadata pass and grid flicker. refresh_files() stamps each request with
 * an incrementing generation; a response whose captured generation no longer
 * matches the current one is discarded before the callback fires.
 *
 * Test seam: MoonrakerFileAPI::get_directory is virtual, so we inject a
 * CapturingFileAPI (via the protected MoonrakerAPI::file_api_ member on a test
 * subclass of MoonrakerAPIMock) that records the success/error callbacks instead
 * of sending, letting us deliver them out of order deterministically.
 */

#include "ui_print_select_file_provider.h"

#include "../test_helpers/scoped_env.h"
#include "../test_helpers/scoped_try_reserve_failure.h"
#include "core/gcode_filename.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_file_api.h"
#include "moonraker_types.h"
#include "print_file_data.h"
#include "printer_state.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// MoonrakerFileAPI that records get_directory callbacks instead of dispatching,
/// so a test can deliver them in an arbitrary order.
class CapturingFileAPI : public MoonrakerFileAPI {
  public:
    explicit CapturingFileAPI(MoonrakerClient& client) : MoonrakerFileAPI(client) {}

    void get_directory(const std::string& /*root*/, const std::string& /*path*/,
                       FileListCallback on_success, ErrorCallback on_error) override {
        success_cbs.push_back(std::move(on_success));
        error_cbs.push_back(std::move(on_error));
    }

    std::vector<FileListCallback> success_cbs;
    std::vector<ErrorCallback> error_cbs;
};

/// MoonrakerAPIMock whose file API is the capturing double. file_api_ is a
/// protected member of MoonrakerAPI, so a subclass may replace it.
class CapturingMockAPI : public MoonrakerAPIMock {
  public:
    CapturingMockAPI(MoonrakerClient& client, PrinterState& state)
        : MoonrakerAPIMock(client, state) {
        auto cap = std::make_unique<CapturingFileAPI>(client);
        capturing = cap.get();
        file_api_ = std::move(cap);
    }

    CapturingFileAPI* capturing = nullptr;
};

class FileProviderGenFixture {
  public:
    FileProviderGenFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        // Drive the mock to CONNECTED so PrintSelectFileProvider::is_ready() passes.
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<CapturingMockAPI>(client_, state_);
        provider_.set_api(api_.get());
    }

    /// Build a single printable-file directory listing with a distinguishing name.
    static std::vector<FileInfo> one_file(const std::string& name) {
        FileInfo f;
        f.filename = name;
        f.modified = 1000.0;
        f.size = 4096;
        f.is_dir = false;
        return {f};
    }

  protected:
    MoonrakerClientMock client_;
    PrinterState state_;
    std::unique_ptr<CapturingMockAPI> api_;
    helix::ui::PrintSelectFileProvider provider_;
};

} // namespace

TEST_CASE_METHOD(FileProviderGenFixture,
                 "FileProvider discards a superseded (stale-generation) success response",
                 "[fileprovider][print_select]") {
    REQUIRE(provider_.is_ready()); // mock client is CONNECTED

    std::vector<std::string> delivered;
    provider_.set_on_files_ready([&delivered](std::vector<PrintFileData>&& files) {
        for (auto& f : files) {
            delivered.push_back(f.filename);
        }
    });

    // Two refreshes: the second supersedes the first (bumps the generation).
    provider_.refresh_files("");
    provider_.refresh_files("");

    auto* cap = api_->capturing;
    REQUIRE(cap->success_cbs.size() == 2);

    // Deliver the FIRST (now stale) response after the second was issued.
    cap->success_cbs[0](one_file("first.gcode"));
    REQUIRE(delivered.empty()); // discarded — callback must not fire

    // Deliver the SECOND (live) response — only its data reaches on_files_ready.
    cap->success_cbs[1](one_file("second.gcode"));
    REQUIRE_FALSE(delivered.empty());
    REQUIRE(std::find(delivered.begin(), delivered.end(), "second.gcode") != delivered.end());
    REQUIRE(std::find(delivered.begin(), delivered.end(), "first.gcode") == delivered.end());
}

TEST_CASE_METHOD(FileProviderGenFixture,
                 "FileProvider discards a superseded (stale-generation) error response",
                 "[fileprovider][print_select]") {
    REQUIRE(provider_.is_ready());

    int error_calls = 0;
    provider_.set_on_error([&error_calls](const std::string&) { ++error_calls; });

    provider_.refresh_files(""); // gen 1
    provider_.refresh_files(""); // gen 2 supersedes gen 1

    auto* cap = api_->capturing;
    REQUIRE(cap->error_cbs.size() == 2);

    // The stale (gen-1) error must be swallowed.
    cap->error_cbs[0](MoonrakerError{});
    REQUIRE(error_calls == 0);

    // The live (gen-2) error propagates.
    cap->error_cbs[1](MoonrakerError{});
    REQUIRE(error_calls == 1);
}

TEST_CASE_METHOD(FileProviderGenFixture,
                 "FileProvider delivers a single non-superseded response normally",
                 "[fileprovider][print_select]") {
    REQUIRE(provider_.is_ready());

    std::vector<std::string> delivered;
    provider_.set_on_files_ready([&delivered](std::vector<PrintFileData>&& files) {
        for (auto& f : files) {
            delivered.push_back(f.filename);
        }
    });

    provider_.refresh_files("");
    auto* cap = api_->capturing;
    REQUIRE(cap->success_cbs.size() == 1);

    cap->success_cbs[0](one_file("only.gcode"));
    REQUIRE(std::find(delivered.begin(), delivered.end(), "only.gcode") != delivered.end());
}

TEST_CASE_METHOD(FileProviderGenFixture,
                 "FileProvider keeps the shown list when the new one cannot be allocated",
                 "[fileprovider][print_select][oom]") {
    REQUIRE(provider_.is_ready());

    int ready_calls = 0;
    std::string error;
    provider_.set_on_files_ready([&ready_calls](std::vector<PrintFileData>&&) { ++ready_calls; });
    provider_.set_on_error([&error](const std::string& e) { error = e; });

    provider_.refresh_files("");
    auto* cap = api_->capturing;
    REQUIRE(cap->success_cbs.size() == 1);

    // PSRAM too fragmented for the list: the build fails instead of aborting,
    // and nothing replaces the list the panel is showing.
    {
        helix::ScopedTryReserveFailure no_memory;
        cap->success_cbs[0](one_file("big.gcode"));
    }

    CHECK(ready_calls == 0);
    CHECK_FALSE(error.empty());
}

namespace {

FileInfo entry(const std::string& name, bool dir, uint64_t size = 4096, double modified = 1000.0) {
    FileInfo f;
    f.filename = name;
    f.is_dir = dir;
    f.size = size;
    f.modified = modified;
    return f;
}

std::vector<std::string> names(const std::vector<PrintFileData>& files) {
    std::vector<std::string> out;
    for (const auto& f : files) {
        out.push_back(f.filename);
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(FileProviderGenFixture,
                 "FileProvider leaves the app's staging directories out of the gcodes root",
                 "[fileprovider][print_select][staging]") {
    // The plugin's status named a visible link directory.
    helix::gcode::set_plugin_link_dir("helix_print");
    std::vector<std::string> delivered;
    provider_.set_on_files_ready(
        [&delivered](std::vector<PrintFileData>&& files) { delivered = names(files); });
    const std::vector<FileInfo> listing = {entry("helix_staging", true), entry("helix_print", true),
                                           entry(".helix_temp", true), entry("parts", true),
                                           entry("a.gcode", false)};

    provider_.refresh_files("");
    api_->capturing->success_cbs.back()(listing);
    CHECK(delivered == std::vector<std::string>{"parts", "a.gcode"});

    // Deeper down, a folder of that name is the user's own.
    provider_.refresh_files("parts");
    api_->capturing->success_cbs.back()(listing);
    CHECK(std::find(delivered.begin(), delivered.end(), "helix_staging") != delivered.end());
    CHECK(std::find(delivered.begin(), delivered.end(), "helix_print") != delivered.end());
    helix::gcode::set_plugin_link_dir("");
}

TEST_CASE_METHOD(FileProviderGenFixture,
                 "FileProvider hands each .3mf the plates the printer has already converted",
                 "[fileprovider][print_select][three_mf][conversions]") {
    const std::string json_path = std::filesystem::temp_directory_path().string() +
                                  "/helix_provider_conversions_" + std::to_string(::getpid()) +
                                  ".json";
    {
        std::ofstream out(json_path);
        out << R"({"conversions":[)"
            << R"({"source":"X.gcode.3mf","source_mtime":1000.0,"source_size":4096,"plate":2,)"
            << R"("output":"X_plate2.gcode","state":"pending"},)"
            << R"({"source":"X.gcode.3mf","source_mtime":1000.0,"source_size":4096,"plate":5,)"
            << R"("output":"X_plate5.gcode","state":"done"}]})";
    }
    ScopedEnv conversions("HELIX_MOCK_3MF_CONVERSIONS", json_path.c_str());

    std::vector<PrintFileData> delivered;
    provider_.set_on_files_ready(
        [&delivered](std::vector<PrintFileData>&& files) { delivered = std::move(files); });
    provider_.refresh_files("");
    api_->capturing->success_cbs.back()(
        {entry("X.gcode.3mf", false), entry("X_plate5.gcode", false)});

    // One plate still pending: the archive stays listed, and its finished
    // plate prints from the output.
    const auto it = std::find_if(delivered.begin(), delivered.end(),
                                 [](const auto& f) { return f.filename == "X.gcode.3mf"; });
    REQUIRE(it != delivered.end());
    CHECK(it->archive_outputs == std::map<int, std::string>{{5, "X_plate5.gcode"}});
    std::remove(json_path.c_str());
}
