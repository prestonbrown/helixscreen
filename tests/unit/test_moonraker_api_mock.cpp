// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_api_mock.cpp
 * @brief Unit tests for MoonrakerAPIMock - HTTP file transfer mocking
 *
 * Tests the mock API's ability to:
 * - Download files from test assets regardless of working directory
 * - Upload files (mock always succeeds)
 * - Handle missing files with proper error callbacks
 */

#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_performance_source.h"
#include "printer_state.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
// Test Fixture

class MoonrakerAPIMockTestFixture {
  public:
    MoonrakerAPIMockTestFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false); // Don't register XML bindings in tests
        api_ = std::make_unique<MoonrakerAPIMock>(client_, state_);
    }

  protected:
    MoonrakerClientMock client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPIMock> api_;
};

// download_file Tests

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file finds existing test file",
                 "[mock][api][download]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    std::string downloaded_content;

    api_->transfers().download_file(
        "gcodes", "3DBenchy.gcode",
        [&](const std::string& content) {
            downloaded_content = content;
            success_called.store(true);
        },
        [&](const MoonrakerError&) { error_called.store(true); });

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
    REQUIRE(downloaded_content.size() > 100); // Should have substantial content
    REQUIRE(downloaded_content.find("G") != std::string::npos);
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file returns FILE_NOT_FOUND for missing file",
                 "[mock][api][download]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    MoonrakerError captured_error;

    api_->transfers().download_file(
        "gcodes", "nonexistent_file_xyz123.gcode",
        [&](const std::string&) { success_called.store(true); },
        [&](const MoonrakerError& err) {
            captured_error = err;
            error_called.store(true);
        });

    REQUIRE_FALSE(success_called.load());
    REQUIRE(error_called.load());
    REQUIRE(captured_error.type == MoonrakerErrorType::FILE_NOT_FOUND);
    REQUIRE(captured_error.method == "download_file");
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file strips directory from path",
                 "[mock][api][download]") {
    // Test that paths like "subdir/file.gcode" still find "file.gcode" in test assets
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};

    api_->transfers().download_file(
        "gcodes", "some/nested/path/3DBenchy.gcode",
        [&](const std::string& content) {
            success_called.store(true);
            REQUIRE(content.size() > 100);
        },
        [&](const MoonrakerError&) { error_called.store(true); });

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file works regardless of CWD",
                 "[mock][api][download]") {
    // This test verifies the fallback path search works
    // The implementation should try multiple paths:
    // - assets/test_gcodes/
    // - ../assets/test_gcodes/
    // - ../../assets/test_gcodes/

    std::atomic<bool> success_called{false};

    api_->transfers().download_file(
        "gcodes", "3DBenchy.gcode", [&](const std::string&) { success_called.store(true); },
        [&](const MoonrakerError& err) { INFO("download_file error: " << err.message); });

    // Should succeed from project root or build/bin/
    REQUIRE(success_called.load());
}

// download_file_partial Tests (Partial/Range Download)

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_partial returns limited content",
                 "[mock][api][download][partial]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    std::string downloaded_content;
    constexpr size_t kMaxBytes = 1000; // Only first 1KB

    api_->transfers().download_file_partial(
        "gcodes", "3DBenchy.gcode", kMaxBytes,
        [&](const std::string& content) {
            downloaded_content = content;
            success_called.store(true);
        },
        [&](const MoonrakerError&) { error_called.store(true); });

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
    // Content should be limited to max_bytes
    REQUIRE(downloaded_content.size() <= kMaxBytes);
    // And should have some content
    REQUIRE(downloaded_content.size() > 0);
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_partial returns full content for small files",
                 "[mock][api][download][partial]") {
    std::atomic<bool> success_called{false};
    std::string downloaded_content;
    constexpr size_t kMaxBytes = 10 * 1024 * 1024; // 10MB limit (larger than file)

    std::string full_content;
    api_->transfers().download_file(
        "gcodes", "3DBenchy.gcode", [&](const std::string& content) { full_content = content; },
        [](const MoonrakerError&) {});

    REQUIRE(full_content.size() > 0);

    // Now get with large limit - should return full content
    api_->transfers().download_file_partial(
        "gcodes", "3DBenchy.gcode", kMaxBytes,
        [&](const std::string& content) {
            downloaded_content = content;
            success_called.store(true);
        },
        [](const MoonrakerError&) {});

    REQUIRE(success_called.load());
    // If file is smaller than limit, we get the whole thing
    if (full_content.size() < kMaxBytes) {
        REQUIRE(downloaded_content == full_content);
    }
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_partial returns FILE_NOT_FOUND for missing file",
                 "[mock][api][download][partial]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    MoonrakerError captured_error;

    api_->transfers().download_file_partial(
        "gcodes", "nonexistent_file_xyz123.gcode", 1000,
        [&](const std::string&) { success_called.store(true); },
        [&](const MoonrakerError& err) {
            captured_error = err;
            error_called.store(true);
        });

    REQUIRE_FALSE(success_called.load());
    REQUIRE(error_called.load());
    REQUIRE(captured_error.type == MoonrakerErrorType::FILE_NOT_FOUND);
    REQUIRE(captured_error.method == "download_file_partial");
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_partial content matches beginning of full file",
                 "[mock][api][download][partial]") {
    std::string full_content;
    std::string partial_content;
    constexpr size_t kPartialSize = 500;

    api_->transfers().download_file(
        "gcodes", "3DBenchy.gcode", [&](const std::string& content) { full_content = content; },
        [](const MoonrakerError&) {});

    REQUIRE(full_content.size() > kPartialSize);

    api_->transfers().download_file_partial(
        "gcodes", "3DBenchy.gcode", kPartialSize,
        [&](const std::string& content) { partial_content = content; },
        [](const MoonrakerError&) {});

    // Partial should match the beginning of full content
    REQUIRE(partial_content.size() == kPartialSize);
    REQUIRE(full_content.substr(0, kPartialSize) == partial_content);
}

// upload_file Tests

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture, "MoonrakerAPIMock upload_file always succeeds",
                 "[mock][api][upload]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};

    api_->transfers().upload_file(
        "gcodes", "test_upload.gcode", "G28\nG1 X100 Y100 F3000\n",
        [&]() { success_called.store(true); },
        [&](const MoonrakerError&) { error_called.store(true); });

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock upload_file_with_name always succeeds", "[mock][api][upload]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};

    api_->transfers().upload_file_with_name(
        "gcodes", "subdir/test.gcode", "custom_filename.gcode", "G28\nM104 S200\n",
        [&]() { success_called.store(true); },
        [&](const MoonrakerError&) { error_called.store(true); });

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture, "MoonrakerAPIMock upload_file handles large content",
                 "[mock][api][upload]") {
    std::atomic<bool> success_called{false};

    // Generate a large G-code content (simulate realistic file)
    std::string large_content;
    large_content.reserve(1024 * 100); // ~100KB
    for (int i = 0; i < 5000; i++) {
        large_content += "G1 X" + std::to_string(i % 200) + " Y" + std::to_string(i % 200) + " E" +
                         std::to_string(i * 0.1) + "\n";
    }

    api_->transfers().upload_file(
        "gcodes", "large_file.gcode", large_content, [&]() { success_called.store(true); },
        [&](const MoonrakerError&) {});

    REQUIRE(success_called.load());
}

// download_file_to_path Tests (Streaming Download)

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_to_path creates file at destination",
                 "[mock][api][download][streaming]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    std::string received_path;

    // Per-process path, like the three download cases below it. Hashing a fixed
    // filename yields the same path in every process, so two runs of this shard
    // at once delete and re-create one another's destination and fail on a
    // file_size that races a std::remove.
    std::string dest_path = "/tmp/helix_test_download_" + std::to_string(getpid()) + ".gcode";

    std::remove(dest_path.c_str());

    api_->transfers().download_file_to_path(
        "gcodes", "3DBenchy.gcode", dest_path,
        [&](const std::string& path) {
            received_path = path;
            success_called.store(true);
        },
        [&](const MoonrakerError&) { error_called.store(true); });

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
    REQUIRE(received_path == dest_path);

    REQUIRE(std::filesystem::exists(dest_path));
    REQUIRE(std::filesystem::file_size(dest_path) > 100);

    std::remove(dest_path.c_str());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_to_path file content matches source",
                 "[mock][api][download][streaming]") {
    std::atomic<bool> success_called{false};
    std::string dest_path =
        "/tmp/helix_test_download_content_" + std::to_string(getpid()) + ".gcode";

    std::remove(dest_path.c_str());

    // First, get content via regular download_file
    std::string original_content;
    api_->transfers().download_file(
        "gcodes", "3DBenchy.gcode", [&](const std::string& content) { original_content = content; },
        [](const MoonrakerError&) {});

    REQUIRE(original_content.size() > 100);

    // Now download to path
    api_->transfers().download_file_to_path(
        "gcodes", "3DBenchy.gcode", dest_path,
        [&](const std::string&) { success_called.store(true); }, [](const MoonrakerError&) {});

    REQUIRE(success_called.load());

    // Read the downloaded file and compare
    std::ifstream file(dest_path, std::ios::binary);
    REQUIRE(file.good());
    std::ostringstream content;
    content << file.rdbuf();
    file.close();

    REQUIRE(content.str() == original_content);

    std::remove(dest_path.c_str());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_to_path returns FILE_NOT_FOUND for missing file",
                 "[mock][api][download][streaming]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    MoonrakerError captured_error;
    std::string dest_path =
        "/tmp/helix_test_download_missing_" + std::to_string(getpid()) + ".gcode";

    api_->transfers().download_file_to_path(
        "gcodes", "nonexistent_file_xyz123.gcode", dest_path,
        [&](const std::string&) { success_called.store(true); },
        [&](const MoonrakerError& err) {
            captured_error = err;
            error_called.store(true);
        });

    REQUIRE_FALSE(success_called.load());
    REQUIRE(error_called.load());
    REQUIRE(captured_error.type == MoonrakerErrorType::FILE_NOT_FOUND);
    REQUIRE(captured_error.method == "download_file_to_path");

    // Verify destination file was NOT created
    REQUIRE_FALSE(std::filesystem::exists(dest_path));
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_to_path strips directory from path",
                 "[mock][api][download][streaming]") {
    std::atomic<bool> success_called{false};
    std::string dest_path =
        "/tmp/helix_test_download_nested_" + std::to_string(getpid()) + ".gcode";

    std::remove(dest_path.c_str());

    // Path with nested directories should still find the file
    api_->transfers().download_file_to_path(
        "gcodes", "some/nested/path/3DBenchy.gcode", dest_path,
        [&](const std::string&) { success_called.store(true); }, [](const MoonrakerError&) {});

    REQUIRE(success_called.load());
    REQUIRE(std::filesystem::exists(dest_path));
    REQUIRE(std::filesystem::file_size(dest_path) > 100);

    std::remove(dest_path.c_str());
}

// Edge Cases

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file handles null success callback",
                 "[mock][api][download]") {
    std::atomic<bool> error_called{false};

    // Should not crash when success callback is null, and file should still be found
    REQUIRE_NOTHROW(
        api_->transfers().download_file("gcodes", "3DBenchy.gcode", nullptr,
                                        [&](const MoonrakerError&) { error_called.store(true); }));

    REQUIRE_FALSE(error_called.load());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file handles null error callback",
                 "[mock][api][download]") {
    std::atomic<bool> success_called{false};

    // Should not crash when error callback is null (for missing file)
    REQUIRE_NOTHROW(api_->transfers().download_file(
        "gcodes", "nonexistent.gcode", [&](const std::string&) { success_called.store(true); },
        nullptr));

    REQUIRE_FALSE(success_called.load());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock upload_file handles null success callback",
                 "[mock][api][upload]") {
    std::atomic<bool> error_called{false};

    // Should not crash when success callback is null
    REQUIRE_NOTHROW(
        api_->transfers().upload_file("gcodes", "test.gcode", "G28", nullptr,
                                      [&](const MoonrakerError&) { error_called.store(true); }));

    REQUIRE_FALSE(error_called.load());
}

// JSON-RPC Handler Tests

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerClientMock handles server.files.get_directory", "[mock][api][files]") {
    std::atomic<bool> success_called{false};
    json received_response;

    client_.send_jsonrpc(
        "server.files.get_directory", {{"path", "gcodes"}},
        [&](json response) {
            received_response = response;
            success_called.store(true);
        },
        [](const MoonrakerError&) {});

    REQUIRE(success_called.load());
    REQUIRE(received_response.contains("result"));
    // Moonraker's own shape: entries beside the disk's usage in bytes
    const json& result = received_response["result"];
    REQUIRE(result.is_object());
    REQUIRE(result["files"].is_array());
    REQUIRE(result["dirs"].is_array());
    REQUIRE(result["disk_usage"]["total"].get<uint64_t>() > 0);
    REQUIRE(result["disk_usage"]["free"].get<uint64_t>() <=
            result["disk_usage"]["total"].get<uint64_t>());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerClientMock disk usage follows HELIX_MOCK_DISK_FREE_MB and parses",
                 "[mock][api][files][performance]") {
    setenv("HELIX_MOCK_DISK_FREE_MB", "300", 1);
    json response;
    client_.send_jsonrpc(
        "server.files.get_directory", {{"path", "gcodes"}}, [&](json r) { response = r; },
        [](const MoonrakerError&) {});
    unsetenv("HELIX_MOCK_DISK_FREE_MB");

    const auto usage = helix::perf::MoonrakerPerformanceSource::parse_disk_usage(response);
    REQUIRE(usage.has_value());
    CHECK(usage->free_mb == 300);
    CHECK(usage->total_mb == 29 * 1024);
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture, "MoonrakerClientMock file list keeps its entries",
                 "[mock][api][files]") {
    // The gcodes listing the file browser reads must survive the object shape.
    std::vector<FileInfo> files;
    bool done = false;
    api_->files().get_directory(
        "gcodes", "",
        [&](const std::vector<FileInfo>& f) {
            files = f;
            done = true;
        },
        [](const MoonrakerError&) {});
    REQUIRE(done);
    for (const auto& f : files) {
        CHECK_FALSE(f.filename.empty());
    }
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "set_active_spool reaches the mock server and status reports it",
                 "[mock][filament][spoolman]") {
    bool set = false;
    api_->spoolman().set_active_spool(2, [&]() { set = true; }, nullptr);
    REQUIRE(set);
    CHECK(client_.spoolman_mock().get_mock_active_spool_id() == 2);

    int reported = -1;
    api_->spoolman().get_spoolman_status([&](bool, int active) { reported = active; }, nullptr);
    CHECK(reported == 2);
}

// Simulation Frames as Method Callbacks

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerClientMock simulation delivers status frames to method callbacks",
                 "[mock][api][moonraker]") {
    std::mutex mu;
    std::condition_variable cv;
    int frames = 0;

    // The live WebSocket path reaches notify_status_update registrants (plugin
    // subscriptions among them); the simulated frames the mock pushes each tick
    // must arrive through that same door, not only through register_notify_update.
    client_.register_method_callback(
        "notify_status_update", "sim_status_probe", [&](const json& msg) {
            std::lock_guard<std::mutex> lk(mu);
            if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
                msg["params"][0].is_object()) {
                ++frames;
            }
            cv.notify_all();
        });

    client_.start_temperature_simulation();
    bool delivered = false;
    {
        std::unique_lock<std::mutex> lk(mu);
        delivered = cv.wait_for(lk, std::chrono::seconds(10), [&] { return frames >= 2; });
    }
    client_.stop_temperature_simulation();

    REQUIRE(delivered);
    CHECK(frames >= 2);
}

// Config-Root Downloads

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_to_path serves the injected config root",
                 "[mock][api][download][moonraker]") {
    const std::string body = R"({"id": "union-probe"})";
    api_->set_config_files({{"plugins/union-probe/manifest.json", body}});

    const std::string dest_path =
        "/tmp/helix_test_config_download_" + std::to_string(getpid()) + ".json";
    std::remove(dest_path.c_str());

    bool success_called = false;
    std::string received_path;
    bool error_called = false;
    MoonrakerError received_error;
    api_->transfers().download_file_to_path(
        "config", "plugins/union-probe/manifest.json", dest_path,
        [&](const std::string& path) {
            received_path = path;
            success_called = true;
        },
        [&](const MoonrakerError& err) {
            received_error = err;
            error_called = true;
        });

    REQUIRE(success_called);
    CHECK_FALSE(error_called);
    CHECK(received_path == dest_path);

    std::ifstream file(dest_path, std::ios::binary);
    std::ostringstream streamed;
    streamed << file.rdbuf();
    file.close();
    CHECK(streamed.str() == body);
    std::remove(dest_path.c_str());

    // A path the config root does not hold is a not-found, never a fall-through
    // that could resolve some same-basename file from the test asset dirs.
    bool absent_ok = false;
    MoonrakerError absent_error;
    bool absent_error_called = false;
    api_->transfers().download_file_to_path(
        "config", "plugins/union-probe/nope.json", dest_path,
        [&](const std::string&) { absent_ok = true; },
        [&](const MoonrakerError& err) {
            absent_error = err;
            absent_error_called = true;
        });
    CHECK_FALSE(absent_ok);
    REQUIRE(absent_error_called);
    CHECK(absent_error.method == "download_file_to_path");
}
