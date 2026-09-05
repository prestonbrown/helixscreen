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
 *
 * TDD: These tests are written BEFORE the implementation is complete.
 */

#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Test Fixture
// ============================================================================

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

// ============================================================================
// download_file Tests
// ============================================================================

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
    // Verify it looks like G-code
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
            // Verify we got actual content
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
        [&](const MoonrakerError& err) {
            // Log the error for debugging if this fails
            INFO("download_file error: " << err.message);
        });

    // Should succeed from project root or build/bin/
    REQUIRE(success_called.load());
}

// ============================================================================
// download_file_partial Tests (Partial/Range Download)
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_partial returns limited content",
                 "[mock][api][download][partial]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    std::string downloaded_content;
    constexpr size_t MAX_BYTES = 1000; // Only first 1KB

    api_->transfers().download_file_partial(
        "gcodes", "3DBenchy.gcode", MAX_BYTES,
        [&](const std::string& content) {
            downloaded_content = content;
            success_called.store(true);
        },
        [&](const MoonrakerError&) { error_called.store(true); });

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
    // Content should be limited to max_bytes
    REQUIRE(downloaded_content.size() <= MAX_BYTES);
    // And should have some content
    REQUIRE(downloaded_content.size() > 0);
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_partial returns full content for small files",
                 "[mock][api][download][partial]") {
    std::atomic<bool> success_called{false};
    std::string downloaded_content;
    constexpr size_t MAX_BYTES = 10 * 1024 * 1024; // 10MB limit (larger than file)

    // First get full file size
    std::string full_content;
    api_->transfers().download_file(
        "gcodes", "3DBenchy.gcode", [&](const std::string& content) { full_content = content; },
        [](const MoonrakerError&) {});

    REQUIRE(full_content.size() > 0);

    // Now get with large limit - should return full content
    api_->transfers().download_file_partial(
        "gcodes", "3DBenchy.gcode", MAX_BYTES,
        [&](const std::string& content) {
            downloaded_content = content;
            success_called.store(true);
        },
        [](const MoonrakerError&) {});

    REQUIRE(success_called.load());
    // If file is smaller than limit, we get the whole thing
    if (full_content.size() < MAX_BYTES) {
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
    constexpr size_t PARTIAL_SIZE = 500;

    // Get full file
    api_->transfers().download_file(
        "gcodes", "3DBenchy.gcode", [&](const std::string& content) { full_content = content; },
        [](const MoonrakerError&) {});

    REQUIRE(full_content.size() > PARTIAL_SIZE);

    // Get partial file
    api_->transfers().download_file_partial(
        "gcodes", "3DBenchy.gcode", PARTIAL_SIZE,
        [&](const std::string& content) { partial_content = content; },
        [](const MoonrakerError&) {});

    // Partial should match the beginning of full content
    REQUIRE(partial_content.size() == PARTIAL_SIZE);
    REQUIRE(full_content.substr(0, PARTIAL_SIZE) == partial_content);
}

// ============================================================================
// upload_file Tests
// ============================================================================

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

// ============================================================================
// download_file_to_path Tests (Streaming Download)
// ============================================================================

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

    // Clean up any existing file
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

    // Verify file exists and has content
    REQUIRE(std::filesystem::exists(dest_path));
    REQUIRE(std::filesystem::file_size(dest_path) > 100);

    // Clean up
    std::remove(dest_path.c_str());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file_to_path file content matches source",
                 "[mock][api][download][streaming]") {
    std::atomic<bool> success_called{false};
    std::string dest_path =
        "/tmp/helix_test_download_content_" + std::to_string(getpid()) + ".gcode";

    // Clean up
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

    // Clean up
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

    // Clean up
    std::remove(dest_path.c_str());

    // Path with nested directories should still find the file
    api_->transfers().download_file_to_path(
        "gcodes", "some/nested/path/3DBenchy.gcode", dest_path,
        [&](const std::string&) { success_called.store(true); }, [](const MoonrakerError&) {});

    REQUIRE(success_called.load());
    REQUIRE(std::filesystem::exists(dest_path));
    REQUIRE(std::filesystem::file_size(dest_path) > 100);

    // Clean up
    std::remove(dest_path.c_str());
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock download_file handles null success callback",
                 "[mock][api][download]") {
    std::atomic<bool> error_called{false};

    // Should not crash when success callback is null, and file should still be found
    REQUIRE_NOTHROW(
        api_->transfers().download_file("gcodes", "3DBenchy.gcode", nullptr,
                                        [&](const MoonrakerError&) { error_called.store(true); }));

    // Verify no error occurred (file exists)
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

    // Verify success was not called (file doesn't exist)
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

    // Verify no error occurred (upload succeeds in mock)
    REQUIRE_FALSE(error_called.load());
}

// ============================================================================
// Slot-Spool Mapping Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock slot-spool mapping is empty initially", "[mock][filament]") {
    // No spools should be assigned to slots initially
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 0);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(1) == 0);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(7) == 0);
    REQUIRE_FALSE(api_->spoolman_mock().get_spool_info_for_slot(0).has_value());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture, "MoonrakerAPIMock can assign spool to slot",
                 "[mock][filament]") {
    // Spool 1 exists in mock data (Polymaker Jet Black PLA)
    api_->spoolman_mock().assign_spool_to_slot(0, 1);

    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 1);

    auto spool_info = api_->spoolman_mock().get_spool_info_for_slot(0);
    REQUIRE(spool_info.has_value());
    REQUIRE(spool_info->id == 1);
    REQUIRE(spool_info->vendor == "Polymaker");
    REQUIRE(spool_info->material == "PLA");
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock can assign multiple spools to different slots",
                 "[mock][filament]") {
    api_->spoolman_mock().assign_spool_to_slot(0, 1); // Polymaker PLA
    api_->spoolman_mock().assign_spool_to_slot(1, 3); // Elegoo ASA
    api_->spoolman_mock().assign_spool_to_slot(2, 6); // Overture TPU

    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 1);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(1) == 3);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(2) == 6);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(3) == 0); // Not assigned

    auto slot0 = api_->spoolman_mock().get_spool_info_for_slot(0);
    auto slot1 = api_->spoolman_mock().get_spool_info_for_slot(1);
    auto slot2 = api_->spoolman_mock().get_spool_info_for_slot(2);

    REQUIRE(slot0->material == "PLA");
    REQUIRE(slot1->material == "ASA");
    REQUIRE(slot2->material == "TPU");
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture, "MoonrakerAPIMock can unassign spool from slot",
                 "[mock][filament]") {
    api_->spoolman_mock().assign_spool_to_slot(0, 1);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 1);

    api_->spoolman_mock().unassign_spool_from_slot(0);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 0);
    REQUIRE_FALSE(api_->spoolman_mock().get_spool_info_for_slot(0).has_value());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock reassigning spool replaces previous", "[mock][filament]") {
    api_->spoolman_mock().assign_spool_to_slot(0, 1);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 1);

    api_->spoolman_mock().assign_spool_to_slot(0, 5); // Replace with different spool
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 5);

    auto spool_info = api_->spoolman_mock().get_spool_info_for_slot(0);
    REQUIRE(spool_info->vendor == "Kingroon");
    REQUIRE(spool_info->material == "PETG");
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture, "MoonrakerAPIMock assigning spool_id 0 unassigns",
                 "[mock][filament]") {
    api_->spoolman_mock().assign_spool_to_slot(0, 1);
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 1);

    api_->spoolman_mock().assign_spool_to_slot(0, 0); // Assign 0 = unassign
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 0);
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock ignores assignment of nonexistent spool", "[mock][filament]") {
    api_->spoolman_mock().assign_spool_to_slot(0, 9999); // Doesn't exist
    REQUIRE(api_->spoolman_mock().get_spool_for_slot(0) == 0);
}

// ============================================================================
// Filament Consumption Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock consume_filament decrements active spool weight",
                 "[mock][filament]") {
    auto& spools = api_->spoolman_mock().get_mock_spools();

    // Find the currently active spool (don't assume index 0 is active)
    SpoolInfo* active_spool = nullptr;
    for (auto& s : spools) {
        if (s.is_active) {
            active_spool = &s;
            break;
        }
    }
    REQUIRE(active_spool != nullptr);

    // Record initial weight before consumption
    float initial_weight = active_spool->remaining_weight_g;
    REQUIRE(initial_weight > 50.0f); // Sanity check: enough to consume

    api_->spoolman_mock().consume_filament(50.0f); // Consume 50 grams

    // Verify weight decreased by exactly 50 grams
    REQUIRE(active_spool->remaining_weight_g == Catch::Approx(initial_weight - 50.0f));
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock consume_filament uses slot's assigned spool",
                 "[mock][filament]") {
    auto& spools = api_->spoolman_mock().get_mock_spools();

    // Find spool 5 by ID (don't assume array index)
    SpoolInfo* spool5 = nullptr;
    for (auto& s : spools) {
        if (s.id == 5) {
            spool5 = &s;
            break;
        }
    }
    REQUIRE(spool5 != nullptr);

    // Assign spool 5 to slot 2
    api_->spoolman_mock().assign_spool_to_slot(2, 5);

    // Record initial weight before consumption
    float initial_weight = spool5->remaining_weight_g;
    REQUIRE(initial_weight >= 75.0f); // Sanity check: enough to consume

    api_->spoolman_mock().consume_filament(75.0f, 2); // Consume from slot 2

    // Verify spool 5's weight decreased by exactly 75 grams
    REQUIRE(spool5->remaining_weight_g == Catch::Approx(initial_weight - 75.0f));
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock consume_filament doesn't go negative", "[mock][filament]") {
    auto& spools = api_->spoolman_mock().get_mock_spools();

    // Find a spool with limited remaining weight (find by ID, not index)
    SpoolInfo* test_spool = nullptr;
    for (auto& s : spools) {
        if (s.id == 4) {
            test_spool = &s;
            break;
        }
    }
    REQUIRE(test_spool != nullptr);

    // Set this spool as active
    api_->spoolman_mock().set_active_spool(4, []() {}, nullptr);

    // Verify it's now active
    REQUIRE(test_spool->is_active);

    // Record initial weight
    float initial_weight = test_spool->remaining_weight_g;
    REQUIRE(initial_weight > 0); // Sanity check: spool has some filament

    // Try to consume more than available
    float excess_consumption = initial_weight + 100.0f;
    api_->spoolman_mock().consume_filament(excess_consumption);

    // Should clamp to 0, not go negative
    REQUIRE(test_spool->remaining_weight_g >= 0.0f);
    REQUIRE(test_spool->remaining_weight_g == 0.0f);
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock consume_filament updates remaining length", "[mock][filament]") {
    auto& spools = api_->spoolman_mock().get_mock_spools();

    // Find the currently active spool
    SpoolInfo* active_spool = nullptr;
    for (auto& s : spools) {
        if (s.is_active) {
            active_spool = &s;
            break;
        }
    }
    REQUIRE(active_spool != nullptr);

    float initial_length = active_spool->remaining_length_m;
    float initial_weight = active_spool->remaining_weight_g;
    REQUIRE(initial_length > 0);
    REQUIRE(initial_weight > 0);

    // Consume some filament
    float consumption_grams = 100.0f;
    REQUIRE(initial_weight >= consumption_grams); // Sanity check
    api_->spoolman_mock().consume_filament(consumption_grams);

    // Length should decrease
    REQUIRE(active_spool->remaining_length_m < initial_length);

    // Verify proportional relationship:
    // length reduction should be approximately proportional to weight reduction
    float weight_ratio = (initial_weight - consumption_grams) / initial_weight;
    float expected_length = initial_length * weight_ratio;
    // Allow 10% tolerance for calculation differences
    REQUIRE(active_spool->remaining_length_m == Catch::Approx(expected_length).epsilon(0.1));
}

// ============================================================================
// JSON-RPC Handler Tests
// ============================================================================

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
    // Result should be an array of files
    REQUIRE(received_response["result"].is_array());
}

TEST_CASE_METHOD(MoonrakerAPIMockTestFixture,
                 "MoonrakerAPIMock set_active_spool updates is_active flag", "[mock][filament]") {
    auto& spools = api_->spoolman_mock().get_mock_spools();
    REQUIRE(spools.size() >= 2); // Need at least 2 spools for this test

    // Find spool with ID 1 and spool with ID 2 (don't assume array index)
    SpoolInfo* spool1 = nullptr;
    SpoolInfo* spool2 = nullptr;
    for (auto& s : spools) {
        if (s.id == 1)
            spool1 = &s;
        if (s.id == 2)
            spool2 = &s;
    }
    REQUIRE(spool1 != nullptr);
    REQUIRE(spool2 != nullptr);

    // First set spool 1 as active to establish known state
    api_->spoolman_mock().set_active_spool(1, []() {}, nullptr);
    REQUIRE(spool1->is_active == true);
    REQUIRE(spool2->is_active == false);

    // Now set spool 2 as active
    api_->spoolman_mock().set_active_spool(2, []() {}, nullptr);

    // Verify spool 2 is now active and spool 1 is not
    REQUIRE(spool1->is_active == false);
    REQUIRE(spool2->is_active == true);
}
