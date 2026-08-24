// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_history_api.cpp
 * @brief Unit tests for Print History API (Stage 1 validation)
 *
 * Tests the Moonraker history API implementation:
 * - get_history_list() returns mock jobs with correct structure
 * - get_history_totals() returns aggregate statistics
 * - delete_history_job() removes job from history
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/print_history_data.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../../src/api/moonraker_api_internal.h"
#include "../ui_test_utils.h"

using moonraker_internal::json_number_or;

#include <spdlog/fmt/fmt.h>

#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Global LVGL Initialization
// ============================================================================

namespace {
struct LVGLInitializerHistory {
    LVGLInitializerHistory() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerHistory lvgl_init;
} // namespace

// ============================================================================
// Test Fixture
// ============================================================================

class PrintHistoryTestFixture {
  public:
    PrintHistoryTestFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        printer_state_.init_subjects(false);
        client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(client_, printer_state_);
    }

    ~PrintHistoryTestFixture() {
        client_.disconnect();
        api_.reset();
    }

  protected:
    MoonrakerClientMock client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPI> api_;
};

// ============================================================================
// get_history_list Tests
// ============================================================================

TEST_CASE_METHOD(PrintHistoryTestFixture, "get_history_list returns mock jobs", "[history][api]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    std::vector<PrintHistoryJob> captured_jobs;
    uint64_t captured_total = 0;

    api_->history().get_history_list(
        50, 0, 0.0, 0.0,
        [&](const std::vector<PrintHistoryJob>& jobs, uint64_t total) {
            captured_jobs = jobs;
            captured_total = total;
            success_called.store(true);
        },
        [&](const MoonrakerError&) { error_called.store(true); });

    // Wait for async callback
    for (int i = 0; i < 50 && !success_called.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
    REQUIRE(captured_jobs.size() > 0);
    REQUIRE(captured_total >= captured_jobs.size());

    // Verify job structure
    const auto& first_job = captured_jobs[0];
    REQUIRE_FALSE(first_job.job_id.empty());
    REQUIRE_FALSE(first_job.filename.empty());
    REQUIRE(first_job.start_time > 0.0);
    REQUIRE_FALSE(first_job.duration_str.empty());
    REQUIRE_FALSE(first_job.date_str.empty());
}

// get_history_list() releases the response DOM as soon as the jobs are built,
// because that document is the largest the app parses (714 KB / ~3.9 MB of heap
// for 500 jobs on a printer with real history). Everything read out of the
// response must therefore be read BEFORE the release — this pins that ordering,
// so moving the release earlier fails here instead of silently zeroing the
// count or truncating the jobs.
TEST_CASE_METHOD(PrintHistoryTestFixture, "get_history_list reads the response before releasing it",
                 "[history][api]") {
    std::atomic<bool> success_called{false};
    std::vector<PrintHistoryJob> captured_jobs;
    uint64_t captured_total = 0;

    api_->history().get_history_list(
        50, 0, 0.0, 0.0,
        [&](const std::vector<PrintHistoryJob>& jobs, uint64_t total) {
            captured_jobs = jobs;
            captured_total = total;
            success_called.store(true);
        },
        [&](const MoonrakerError&) {});

    for (int i = 0; i < 50 && !success_called.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(success_called.load());
    REQUIRE(captured_jobs.size() > 1);

    // The count comes straight off the DOM; releasing before reading it zeroes it.
    CHECK(captured_total > 0);
    CHECK(captured_total >= captured_jobs.size());

    // EVERY job must be fully populated, not just the first — a release that
    // landed mid-loop would leave later entries blank while the early ones pass.
    for (const auto& job : captured_jobs) {
        CHECK_FALSE(job.job_id.empty());
        CHECK_FALSE(job.filename.empty());
        CHECK(job.start_time > 0.0);
        // Derived display strings are built per job during the same walk.
        CHECK_FALSE(job.duration_str.empty());
        CHECK_FALSE(job.date_str.empty());
    }
}

TEST_CASE_METHOD(PrintHistoryTestFixture, "get_history_list jobs have valid status",
                 "[history][api]") {
    std::atomic<bool> done{false};
    std::vector<PrintHistoryJob> captured_jobs;

    api_->history().get_history_list(
        50, 0, 0.0, 0.0,
        [&](const std::vector<PrintHistoryJob>& jobs, uint64_t) {
            captured_jobs = jobs;
            done.store(true);
        },
        [&](const MoonrakerError&) { done.store(true); });

    for (int i = 0; i < 50 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(captured_jobs.size() > 0);

    // Check that all jobs have valid status
    for (const auto& job : captured_jobs) {
        REQUIRE(job.status != PrintJobStatus::UNKNOWN);
        // Status should be one of the expected values
        bool valid_status =
            (job.status == PrintJobStatus::COMPLETED || job.status == PrintJobStatus::CANCELLED ||
             job.status == PrintJobStatus::ERROR || job.status == PrintJobStatus::IN_PROGRESS);
        REQUIRE(valid_status);
    }
}

// ============================================================================
// get_history_totals Tests
// ============================================================================

TEST_CASE_METHOD(PrintHistoryTestFixture, "get_history_totals returns statistics",
                 "[history][api]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    PrintHistoryTotals captured_totals;

    api_->history().get_history_totals(
        [&](const PrintHistoryTotals& totals) {
            captured_totals = totals;
            success_called.store(true);
        },
        [&](const MoonrakerError&) { error_called.store(true); });

    for (int i = 0; i < 50 && !success_called.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());

    // Mock should return reasonable statistics
    REQUIRE(captured_totals.total_jobs > 0);
    REQUIRE(captured_totals.total_time > 0);
    REQUIRE(captured_totals.total_filament_used > 0.0);
    REQUIRE(captured_totals.longest_job > 0.0);

    // Note: Real Moonraker doesn't provide breakdown counts (completed/cancelled/failed)
    // These must be calculated client-side from the job list if needed
    // So we don't test for them here
}

// ============================================================================
// delete_history_job Tests
// ============================================================================

TEST_CASE_METHOD(PrintHistoryTestFixture, "delete_history_job calls success callback",
                 "[history][api]") {
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};

    // First get a job ID to delete
    std::string job_id_to_delete;
    api_->history().get_history_list(
        1, 0, 0.0, 0.0,
        [&](const std::vector<PrintHistoryJob>& jobs, uint64_t) {
            if (!jobs.empty()) {
                job_id_to_delete = jobs[0].job_id;
            }
        },
        [](const MoonrakerError&) {});

    for (int i = 0; i < 50 && job_id_to_delete.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE_FALSE(job_id_to_delete.empty());

    // Now delete it
    api_->history().delete_history_job(
        job_id_to_delete, [&]() { success_called.store(true); },
        [&](const MoonrakerError&) { error_called.store(true); });

    for (int i = 0; i < 50 && !success_called.load() && !error_called.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(success_called.load());
    REQUIRE_FALSE(error_called.load());
}

// ============================================================================
// Real-World JSON Parsing Tests
// ============================================================================

// Real Moonraker response with null values (captured from actual Voron printer)
static const char* REAL_MOONRAKER_HISTORY_RESPONSE = R"({
    "result": {
        "count": 796,
        "jobs": [
            {
                "job_id": "000313",
                "user": "_TRUSTED_USER_",
                "filename": "Body744_ASA_1h40m.gcode",
                "status": "completed",
                "start_time": 1760570869.4063392,
                "end_time": 1760576647.4602716,
                "print_duration": 5481.505679905,
                "total_duration": 5778.059486547,
                "filament_used": 6170.388689999407,
                "metadata": {
                    "size": 13922674,
                    "slicer": "OrcaSlicer",
                    "slicer_version": "2.3.1",
                    "layer_count": 47,
                    "object_height": 9.4,
                    "estimated_time": 6027,
                    "nozzle_diameter": 0.4,
                    "layer_height": 0.2,
                    "first_layer_height": 0.2,
                    "first_layer_extr_temp": 260.0,
                    "first_layer_bed_temp": 90.0,
                    "chamber_temp": 0.0,
                    "filament_name": "Generic ASA @Voron v2",
                    "filament_type": "ASA;ASA;ASA;ASA",
                    "thumbnails": [
                        {"width": 32, "height": 32, "size": 990, "relative_path": ".thumbs/Body744_ASA_1h40m-32x32.png"},
                        {"width": 300, "height": 300, "size": 16304, "relative_path": ".thumbs/Body744_ASA_1h40m-300x300.png"}
                    ]
                },
                "auxiliary_data": [
                    {"provider": "spoolman", "name": "spool_ids", "value": [5, null], "description": "Spool IDs used", "units": null}
                ],
                "exists": true
            },
            {
                "job_id": "000312",
                "user": "_TRUSTED_USER_",
                "filename": "Body744_ASA_1h40m.gcode",
                "status": "cancelled",
                "start_time": 1760569839.3108423,
                "end_time": 1760570661.1919284,
                "print_duration": 293.3458584309999,
                "total_duration": 821.8611410999999,
                "filament_used": 285.66931999999963,
                "metadata": {},
                "auxiliary_data": [
                    {"provider": "spoolman", "name": "spool_ids", "value": [5], "description": "Spool IDs used", "units": null}
                ],
                "exists": true
            },
            {
                "job_id": "000311",
                "user": "_TRUSTED_USER_",
                "filename": "Belt_bracket.gcode",
                "status": "klippy_shutdown",
                "start_time": 1759265379.2184007,
                "end_time": 1759265554.49163,
                "print_duration": 0.0,
                "total_duration": 175.64103475003503,
                "filament_used": 0.0,
                "metadata": {
                    "layer_count": 60,
                    "first_layer_extr_temp": 260.0,
                    "first_layer_bed_temp": 90.0,
                    "filament_type": "ASA"
                },
                "auxiliary_data": [],
                "exists": true
            }
        ]
    }
})";

TEST_CASE("Parse real Moonraker history response with nulls", "[history][parsing]") {
    // This tests that our JSON parsing handles real-world responses
    // including null values in auxiliary_data

    auto j = nlohmann::json::parse(REAL_MOONRAKER_HISTORY_RESPONSE);

    REQUIRE(j.contains("result"));
    REQUIRE(j["result"].contains("count"));
    REQUIRE(j["result"]["count"].get<int>() == 796);
    REQUIRE(j["result"].contains("jobs"));
    REQUIRE(j["result"]["jobs"].is_array());
    REQUIRE(j["result"]["jobs"].size() == 3);
}

TEST_CASE("Parse history job with null auxiliary_data values", "[history][parsing]") {
    auto j = nlohmann::json::parse(REAL_MOONRAKER_HISTORY_RESPONSE);
    const auto& jobs = j["result"]["jobs"];

    // First job has null in auxiliary_data.value array
    const auto& job0 = jobs[0];
    REQUIRE(job0["auxiliary_data"][0]["value"][1].is_null());
    REQUIRE(job0["auxiliary_data"][0]["units"].is_null());

    // But the core fields should all be accessible
    REQUIRE(job0["job_id"].get<std::string>() == "000313");
    REQUIRE(job0["filename"].get<std::string>() == "Body744_ASA_1h40m.gcode");
    REQUIRE(job0["status"].get<std::string>() == "completed");
    REQUIRE(job0["print_duration"].get<double>() > 5000.0);
    REQUIRE(job0["filament_used"].get<double>() > 6000.0);
}

TEST_CASE("Parse history job with empty metadata", "[history][parsing]") {
    auto j = nlohmann::json::parse(REAL_MOONRAKER_HISTORY_RESPONSE);
    const auto& jobs = j["result"]["jobs"];

    // Second job has empty metadata
    const auto& job1 = jobs[1];
    REQUIRE(job1["metadata"].empty());

    // But core fields are still valid
    REQUIRE(job1["status"].get<std::string>() == "cancelled");
    REQUIRE(job1["print_duration"].get<double>() > 200.0);
}

TEST_CASE("Parse history job with klippy_shutdown status", "[history][parsing]") {
    auto j = nlohmann::json::parse(REAL_MOONRAKER_HISTORY_RESPONSE);
    const auto& jobs = j["result"]["jobs"];

    // Third job has klippy_shutdown status and zero print_duration
    const auto& job2 = jobs[2];
    REQUIRE(job2["status"].get<std::string>() == "klippy_shutdown");
    REQUIRE(job2["print_duration"].get<double>() == 0.0);
    REQUIRE(job2["filament_used"].get<double>() == 0.0);
}

TEST_CASE("json::value() handles missing keys with defaults", "[history][parsing]") {
    // Test that nlohmann::json's value() function returns defaults for missing keys
    auto j = nlohmann::json::parse(R"({"name": "test"})");

    REQUIRE(j.value("name", "") == "test");
    REQUIRE(j.value("missing_string", "default") == "default");
    REQUIRE(j.value("missing_int", 42) == 42);
    REQUIRE(j.value("missing_double", 3.14) == 3.14);
    REQUIRE(j.value("missing_bool", true) == true);
}

TEST_CASE("json::value() handles null values", "[history][parsing]") {
    // Test how value() handles explicit null values
    auto j = nlohmann::json::parse(R"({"value": null, "number": 42, "obj": {"nested": "yes"}})");

    // value() works on objects for getting nested keys with defaults
    REQUIRE(j["obj"].value("nested", "no") == "yes");
    REQUIRE(j["obj"].value("missing", "default") == "default");

    // For explicit nulls, we need is_null() check
    REQUIRE(j["value"].is_null());

    // And getting a value from null throws - this is the real issue
    // We should NOT call value() on a json object that IS null
    REQUIRE_THROWS(j["value"].value("anything", 0));
}

// ============================================================================
// PrintHistoryJob Parsing Tests
// ============================================================================

// Helper function to parse a job JSON into PrintHistoryJob (mirrors MoonrakerAPI logic)
static PrintHistoryJob parse_history_job(const nlohmann::json& job_json) {
    PrintHistoryJob job;
    job.job_id = job_json.value("job_id", "");
    job.filename = job_json.value("filename", "");

    // Numeric fields use null-safe accessor (end_time can be null for in-progress jobs)
    job.start_time = json_number_or(job_json, "start_time", 0.0);
    job.end_time = json_number_or(job_json, "end_time", 0.0);
    job.print_duration = json_number_or(job_json, "print_duration", 0.0);
    job.total_duration = json_number_or(job_json, "total_duration", 0.0);
    job.filament_used = json_number_or(job_json, "filament_used", 0.0);
    job.exists = job_json.value("exists", false);

    // Parse status string to enum
    std::string status_str = job_json.value("status", "");
    if (status_str == "completed") {
        job.status = PrintJobStatus::COMPLETED;
    } else if (status_str == "cancelled") {
        job.status = PrintJobStatus::CANCELLED;
    } else if (status_str == "error" || status_str == "klippy_shutdown" ||
               status_str == "klippy_disconnect") {
        job.status = PrintJobStatus::ERROR;
    } else if (status_str == "in_progress" || status_str == "printing") {
        job.status = PrintJobStatus::IN_PROGRESS;
    } else {
        job.status = PrintJobStatus::UNKNOWN;
    }

    // Parse metadata if present (matches PrintHistoryJob struct fields)
    if (job_json.contains("metadata") && job_json["metadata"].is_object()) {
        const auto& meta = job_json["metadata"];
        job.filament_type = meta.value("filament_type", "");
        job.layer_count = json_number_or(meta, "layer_count", 0u);
        job.layer_height = json_number_or(meta, "layer_height", 0.0);
        job.nozzle_temp = json_number_or(meta, "first_layer_extr_temp", 0.0);
        job.bed_temp = json_number_or(meta, "first_layer_bed_temp", 0.0);
    }

    return job;
}

TEST_CASE("Parse completed job correctly", "[history][parsing]") {
    auto job_json = nlohmann::json::parse(R"({
        "job_id": "000313",
        "filename": "Body744_ASA_1h40m.gcode",
        "status": "completed",
        "start_time": 1760570869.4063392,
        "end_time": 1760576647.4602716,
        "print_duration": 5481.505679905,
        "total_duration": 5778.059486547,
        "filament_used": 6170.388689999407,
        "exists": true,
        "metadata": {
            "slicer": "OrcaSlicer",
            "layer_count": 47,
            "layer_height": 0.2,
            "first_layer_extr_temp": 260.0,
            "first_layer_bed_temp": 90.0,
            "filament_type": "ASA;ASA;ASA;ASA"
        }
    })");

    PrintHistoryJob job = parse_history_job(job_json);

    REQUIRE(job.job_id == "000313");
    REQUIRE(job.filename == "Body744_ASA_1h40m.gcode");
    REQUIRE(job.status == PrintJobStatus::COMPLETED);
    REQUIRE(job.print_duration > 5400.0);
    REQUIRE(job.filament_used > 6000.0);
    REQUIRE(job.layer_count == 47);
    REQUIRE(job.layer_height == 0.2);
    REQUIRE(job.nozzle_temp == 260.0);
    REQUIRE(job.bed_temp == 90.0);
    REQUIRE(job.filament_type == "ASA;ASA;ASA;ASA");
    REQUIRE(job.exists == true);
}

TEST_CASE("Parse cancelled job correctly", "[history][parsing]") {
    auto job_json = nlohmann::json::parse(R"({
        "job_id": "000312",
        "filename": "Body744_ASA_1h40m.gcode",
        "status": "cancelled",
        "start_time": 1760569839.3108423,
        "end_time": 1760570661.1919284,
        "print_duration": 293.3458584309999,
        "total_duration": 821.8611410999999,
        "filament_used": 285.66931999999963,
        "metadata": {}
    })");

    PrintHistoryJob job = parse_history_job(job_json);

    REQUIRE(job.job_id == "000312");
    REQUIRE(job.status == PrintJobStatus::CANCELLED);
    REQUIRE(job.print_duration < 300.0);
    // Empty metadata should not cause crash - fields have defaults
    REQUIRE(job.filament_type.empty());
    REQUIRE(job.layer_count == 0);
}

TEST_CASE("Parse klippy_shutdown as error status", "[history][parsing]") {
    auto job_json = nlohmann::json::parse(R"({
        "job_id": "000311",
        "filename": "Belt_bracket.gcode",
        "status": "klippy_shutdown",
        "start_time": 1759265379.2184007,
        "end_time": 1759265554.49163,
        "print_duration": 0.0,
        "total_duration": 175.64103475003503,
        "filament_used": 0.0,
        "metadata": {
            "layer_count": 60,
            "filament_type": "ASA"
        }
    })");

    PrintHistoryJob job = parse_history_job(job_json);

    REQUIRE(job.job_id == "000311");
    // klippy_shutdown should map to ERROR status
    REQUIRE(job.status == PrintJobStatus::ERROR);
    REQUIRE(job.print_duration == 0.0);
    REQUIRE(job.filament_used == 0.0);
    // Metadata should still parse
    REQUIRE(job.layer_count == 60);
    REQUIRE(job.filament_type == "ASA");
}

TEST_CASE("Parse job with missing optional fields", "[history][parsing]") {
    // Minimal job - only required fields
    auto job_json = nlohmann::json::parse(R"({
        "job_id": "000001",
        "filename": "test.gcode",
        "status": "completed",
        "start_time": 1000000.0,
        "print_duration": 3600.0,
        "filament_used": 1000.0
    })");

    PrintHistoryJob job = parse_history_job(job_json);

    REQUIRE(job.job_id == "000001");
    REQUIRE(job.status == PrintJobStatus::COMPLETED);
    // Optional fields should have safe defaults
    REQUIRE(job.end_time == 0.0);
    REQUIRE(job.total_duration == 0.0);
    REQUIRE(job.filament_type.empty());
    REQUIRE(job.layer_count == 0);
    REQUIRE(job.exists == false); // No metadata means file might not exist
}

TEST_CASE("Parse job with null end_time (in-progress job)", "[history][parsing]") {
    // In-progress jobs have null end_time - this must not throw
    // Per Moonraker source: end_time is Optional[float] = None, other numeric fields init to 0.
    // This tests the null-safety fix for json::value() gotcha
    auto job_json = nlohmann::json::parse(R"({
        "job_id": "000999",
        "filename": "in_progress.gcode",
        "status": "in_progress",
        "start_time": 1760600000.0,
        "end_time": null,
        "print_duration": 120.5,
        "total_duration": 125.0,
        "filament_used": 500.0,
        "exists": true,
        "metadata": {
            "layer_count": 10,
            "layer_height": 0.2
        }
    })");

    // This should NOT throw - null end_time should become 0.0
    PrintHistoryJob job = parse_history_job(job_json);

    REQUIRE(job.job_id == "000999");
    REQUIRE(job.status == PrintJobStatus::IN_PROGRESS);
    REQUIRE(job.start_time > 0.0);
    // Null end_time should safely default to 0
    REQUIRE(job.end_time == 0.0);
    // Other fields should parse normally
    REQUIRE(job.print_duration == 120.5);
    REQUIRE(job.total_duration == 125.0);
    REQUIRE(job.filament_used == 500.0);
    REQUIRE(job.layer_count == 10);
    REQUIRE(job.layer_height == 0.2);
}

// ============================================================================
// Statistics Calculation Tests
// ============================================================================

// Helper to calculate statistics from a job list (mirrors UI code)
struct HistoryStats {
    int total_jobs = 0;
    int completed_jobs = 0;
    int cancelled_jobs = 0;
    int error_jobs = 0;
    double total_print_time = 0.0;
    double total_filament = 0.0;
    double success_rate = 0.0;
};

static HistoryStats calculate_stats(const std::vector<PrintHistoryJob>& jobs) {
    HistoryStats stats;
    stats.total_jobs = static_cast<int>(jobs.size());

    for (const auto& job : jobs) {
        switch (job.status) {
        case PrintJobStatus::COMPLETED:
            stats.completed_jobs++;
            break;
        case PrintJobStatus::CANCELLED:
            stats.cancelled_jobs++;
            break;
        case PrintJobStatus::ERROR:
            stats.error_jobs++;
            break;
        default:
            break;
        }
        stats.total_print_time += job.print_duration;
        stats.total_filament += job.filament_used;
    }

    if (stats.total_jobs > 0) {
        stats.success_rate = (static_cast<double>(stats.completed_jobs) / stats.total_jobs) * 100.0;
    }

    return stats;
}

TEST_CASE("Calculate statistics from job list", "[history][stats]") {
    std::vector<PrintHistoryJob> jobs;

    // 2 completed jobs
    PrintHistoryJob job1;
    job1.status = PrintJobStatus::COMPLETED;
    job1.print_duration = 3600.0; // 1 hour
    job1.filament_used = 5000.0;  // 5m
    jobs.push_back(job1);

    PrintHistoryJob job2;
    job2.status = PrintJobStatus::COMPLETED;
    job2.print_duration = 7200.0; // 2 hours
    job2.filament_used = 10000.0; // 10m
    jobs.push_back(job2);

    // 1 cancelled job
    PrintHistoryJob job3;
    job3.status = PrintJobStatus::CANCELLED;
    job3.print_duration = 600.0; // 10 min
    job3.filament_used = 500.0;  // 0.5m
    jobs.push_back(job3);

    // 1 error job
    PrintHistoryJob job4;
    job4.status = PrintJobStatus::ERROR;
    job4.print_duration = 0.0;
    job4.filament_used = 0.0;
    jobs.push_back(job4);

    HistoryStats stats = calculate_stats(jobs);

    REQUIRE(stats.total_jobs == 4);
    REQUIRE(stats.completed_jobs == 2);
    REQUIRE(stats.cancelled_jobs == 1);
    REQUIRE(stats.error_jobs == 1);
    REQUIRE(stats.total_print_time == 11400.0); // 3.17 hours
    REQUIRE(stats.total_filament == 15500.0);   // 15.5m
    REQUIRE(stats.success_rate == 50.0);        // 2/4 = 50%
}

TEST_CASE("Calculate statistics from empty job list", "[history][stats]") {
    std::vector<PrintHistoryJob> jobs;

    HistoryStats stats = calculate_stats(jobs);

    REQUIRE(stats.total_jobs == 0);
    REQUIRE(stats.completed_jobs == 0);
    REQUIRE(stats.success_rate == 0.0);
    REQUIRE(stats.total_print_time == 0.0);
    REQUIRE(stats.total_filament == 0.0);
}

TEST_CASE("Success rate calculation with all completed", "[history][stats]") {
    std::vector<PrintHistoryJob> jobs;

    for (int i = 0; i < 10; i++) {
        PrintHistoryJob job;
        job.status = PrintJobStatus::COMPLETED;
        job.print_duration = 1000.0;
        jobs.push_back(job);
    }

    HistoryStats stats = calculate_stats(jobs);

    REQUIRE(stats.total_jobs == 10);
    REQUIRE(stats.success_rate == 100.0);
}

// ============================================================================
// Filament Type Parsing Tests
// ============================================================================

// Helper to parse filament types from semicolon-separated string
static std::vector<std::string> parse_filament_types(const std::string& filament_str) {
    std::vector<std::string> types;
    if (filament_str.empty())
        return types;

    std::stringstream ss(filament_str);
    std::string item;
    while (std::getline(ss, item, ';')) {
        // Trim whitespace
        size_t start = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (start != std::string::npos) {
            types.push_back(item.substr(start, end - start + 1));
        }
    }
    return types;
}

TEST_CASE("Parse multi-extruder filament types", "[history][parsing]") {
    std::string filament_str = "ASA;ASA;ASA;ASA";
    auto types = parse_filament_types(filament_str);

    REQUIRE(types.size() == 4);
    for (const auto& t : types) {
        REQUIRE(t == "ASA");
    }
}

TEST_CASE("Parse single filament type", "[history][parsing]") {
    std::string filament_str = "PLA";
    auto types = parse_filament_types(filament_str);

    REQUIRE(types.size() == 1);
    REQUIRE(types[0] == "PLA");
}

TEST_CASE("Parse empty filament type", "[history][parsing]") {
    std::string filament_str = "";
    auto types = parse_filament_types(filament_str);

    REQUIRE(types.empty());
}

TEST_CASE("Parse mixed filament types with whitespace", "[history][parsing]") {
    std::string filament_str = "PLA ; PETG ; ABS";
    auto types = parse_filament_types(filament_str);

    REQUIRE(types.size() == 3);
    REQUIRE(types[0] == "PLA");
    REQUIRE(types[1] == "PETG");
    REQUIRE(types[2] == "ABS");
}

// ============================================================================
// Real Voron Printer Data Tests (from user's 192.168.1.112)
// ============================================================================

// Exact JSON response from curl to real Voron printer
static const char* REAL_VORON_HISTORY_5_JOBS = R"({
    "result": {
        "count": 5,
        "jobs": [
            {
                "job_id": "000313",
                "user": "_TRUSTED_USER_",
                "filename": "Body744_ASA_1h40m.gcode",
                "status": "completed",
                "start_time": 1760570869.4063392,
                "end_time": 1760576647.4602716,
                "print_duration": 5481.505679905,
                "total_duration": 5778.059486547,
                "filament_used": 6170.388689999407,
                "metadata": {
                    "slicer": "OrcaSlicer",
                    "slicer_version": "2.3.1",
                    "layer_count": 47,
                    "layer_height": 0.2,
                    "first_layer_extr_temp": 260.0,
                    "first_layer_bed_temp": 90.0,
                    "filament_type": "ASA;ASA;ASA;ASA",
                    "mmu_print": 1
                },
                "auxiliary_data": [
                    {"provider": "spoolman", "name": "spool_ids", "value": [5, null], "units": null}
                ],
                "exists": true
            },
            {
                "job_id": "000312",
                "status": "cancelled",
                "filename": "Body744_ASA_1h40m.gcode",
                "print_duration": 293.3458584309999,
                "filament_used": 285.66931999999963,
                "exists": true
            },
            {
                "job_id": "000311",
                "filename": "Belt_bracket_v6recovered_ASA_5h0m.gcode",
                "status": "klippy_shutdown",
                "print_duration": 0.0,
                "filament_used": 0.0,
                "metadata": {
                    "layer_count": 60,
                    "filament_type": "ASA;ASA;ASA;PLA"
                },
                "exists": true
            },
            {
                "job_id": "000310",
                "status": "klippy_shutdown",
                "filename": "lead screw cleaner handle remix.gcode",
                "print_duration": 0.0,
                "filament_used": 0.0,
                "exists": true
            },
            {
                "job_id": "00030F",
                "status": "completed",
                "filename": "Belt_bracket_v6recovered_ASA_5h0m.gcode",
                "print_duration": 17420.564048016007,
                "filament_used": 63183.214590007825,
                "auxiliary_data": [
                    {"provider": "spoolman", "name": "spool_ids", "value": [127, null, 86], "units": null}
                ],
                "exists": true
            }
        ]
    }
})";

TEST_CASE("Parse real Voron printer history data", "[history][parsing][voron]") {
    auto j = nlohmann::json::parse(REAL_VORON_HISTORY_5_JOBS);

    REQUIRE(j["result"]["count"].get<int>() == 5);
    REQUIRE(j["result"]["jobs"].size() == 5);

    // Parse all jobs
    std::vector<PrintHistoryJob> jobs;
    for (const auto& job_json : j["result"]["jobs"]) {
        jobs.push_back(parse_history_job(job_json));
    }

    REQUIRE(jobs.size() == 5);

    // Job 0: completed, has full metadata
    REQUIRE(jobs[0].job_id == "000313");
    REQUIRE(jobs[0].status == PrintJobStatus::COMPLETED);
    REQUIRE(jobs[0].print_duration > 5400.0);
    REQUIRE(jobs[0].filament_used > 6000.0);
    REQUIRE(jobs[0].layer_count == 47);
    REQUIRE(jobs[0].filament_type == "ASA;ASA;ASA;ASA");

    // Job 1: cancelled
    REQUIRE(jobs[1].job_id == "000312");
    REQUIRE(jobs[1].status == PrintJobStatus::CANCELLED);

    // Jobs 2 & 3: klippy_shutdown should map to ERROR
    REQUIRE(jobs[2].job_id == "000311");
    REQUIRE(jobs[2].status == PrintJobStatus::ERROR);
    REQUIRE(jobs[2].print_duration == 0.0);
    REQUIRE(jobs[2].filament_type == "ASA;ASA;ASA;PLA"); // Mixed filament types

    REQUIRE(jobs[3].status == PrintJobStatus::ERROR);

    // Job 4: completed with null values in auxiliary_data
    REQUIRE(jobs[4].job_id == "00030F");
    REQUIRE(jobs[4].status == PrintJobStatus::COMPLETED);
    REQUIRE(jobs[4].filament_used > 63000.0); // 63m of filament!
}

TEST_CASE("Calculate stats from real Voron data", "[history][stats][voron]") {
    auto j = nlohmann::json::parse(REAL_VORON_HISTORY_5_JOBS);

    std::vector<PrintHistoryJob> jobs;
    for (const auto& job_json : j["result"]["jobs"]) {
        jobs.push_back(parse_history_job(job_json));
    }

    HistoryStats stats = calculate_stats(jobs);

    REQUIRE(stats.total_jobs == 5);
    REQUIRE(stats.completed_jobs == 2);  // 000313, 00030F
    REQUIRE(stats.cancelled_jobs == 1);  // 000312
    REQUIRE(stats.error_jobs == 2);      // 000311, 000310 (klippy_shutdown)
    REQUIRE(stats.success_rate == 40.0); // 2/5 = 40%

    // Print time: 5481.5 + 293.3 + 0 + 0 + 17420.6 = ~23195 seconds
    REQUIRE(stats.total_print_time > 23000.0);

    // Filament: 6170 + 285 + 0 + 0 + 63183 = ~69638mm
    REQUIRE(stats.total_filament > 69000.0);
}

// ============================================================================
// Large Response Handling Tests
// ============================================================================

// ============================================================================
// Status String Parsing Tests (production parse_job_status function)
// ============================================================================

TEST_CASE("Parse all Moonraker status strings", "[history][parsing]") {
    // Test the production parse_job_status() function from print_history_data.h
    // which handles all known Moonraker job status strings

    SECTION("completed maps to COMPLETED") {
        REQUIRE(parse_job_status("completed") == PrintJobStatus::COMPLETED);
    }

    SECTION("cancelled maps to CANCELLED") {
        REQUIRE(parse_job_status("cancelled") == PrintJobStatus::CANCELLED);
    }

    SECTION("error states map to ERROR") {
        // Direct error
        REQUIRE(parse_job_status("error") == PrintJobStatus::ERROR);
        // Klipper shutdown mid-print
        REQUIRE(parse_job_status("klippy_shutdown") == PrintJobStatus::ERROR);
        // Klipper connection lost
        REQUIRE(parse_job_status("klippy_disconnect") == PrintJobStatus::ERROR);
        // Moonraker server exit
        REQUIRE(parse_job_status("server_exit") == PrintJobStatus::ERROR);
        // Job interrupted (detected on startup)
        REQUIRE(parse_job_status("interrupted") == PrintJobStatus::ERROR);
    }

    SECTION("active states map to IN_PROGRESS") {
        REQUIRE(parse_job_status("in_progress") == PrintJobStatus::IN_PROGRESS);
        REQUIRE(parse_job_status("printing") == PrintJobStatus::IN_PROGRESS);
    }

    SECTION("unknown strings map to UNKNOWN") {
        REQUIRE(parse_job_status("") == PrintJobStatus::UNKNOWN);
        REQUIRE(parse_job_status("unknown_status") == PrintJobStatus::UNKNOWN);
        REQUIRE(parse_job_status("paused") == PrintJobStatus::UNKNOWN);
        REQUIRE(parse_job_status("COMPLETED") == PrintJobStatus::UNKNOWN); // Case-sensitive
    }
}

TEST_CASE("Handle large history response (simulating 200+ jobs)", "[history][parsing][large]") {
    // Build a large JSON response similar to what Moonraker returns for printers with lots of
    // history This tests that our parsing can handle responses in the 300KB+ range

    nlohmann::json response;
    response["result"]["count"] = 500;
    auto& jobs = response["result"]["jobs"];
    jobs = nlohmann::json::array();

    // Generate 200 synthetic jobs (similar to real Moonraker response structure)
    for (int i = 0; i < 200; i++) {
        nlohmann::json job;
        job["job_id"] = fmt::format("{:06d}", i);
        job["filename"] = fmt::format("Test_Model_{}_PLA_2h30m.gcode", i);
        job["status"] = (i % 10 == 0) ? "cancelled" : "completed";
        job["start_time"] = 1760000000.0 + (i * 10000);
        job["end_time"] = 1760000000.0 + (i * 10000) + 9000;
        job["print_duration"] = 8500.0 + (i % 1000);
        job["total_duration"] = 9000.0 + (i % 1000);
        job["filament_used"] = 5000.0 + (i * 100);
        job["exists"] = true;

        // Add metadata (makes response larger, like real Moonraker)
        job["metadata"]["slicer"] = "OrcaSlicer";
        job["metadata"]["slicer_version"] = "2.3.1";
        job["metadata"]["layer_count"] = 100 + (i % 50);
        job["metadata"]["layer_height"] = 0.2;
        job["metadata"]["first_layer_height"] = 0.25;
        job["metadata"]["first_layer_extr_temp"] = 210.0;
        job["metadata"]["first_layer_bed_temp"] = 60.0;
        job["metadata"]["filament_type"] = "PLA";
        job["metadata"]["filament_name"] = "Generic PLA @Voron v2";
        job["metadata"]["estimated_time"] = 9000;
        job["metadata"]["object_height"] = 50.0 + (i % 20);
        job["metadata"]["nozzle_diameter"] = 0.4;

        // Add thumbnails array (common in real responses)
        job["metadata"]["thumbnails"] = nlohmann::json::array();
        job["metadata"]["thumbnails"].push_back(
            {{"width", 32},
             {"height", 32},
             {"size", 990},
             {"relative_path", fmt::format(".thumbs/Test_Model_{}-32x32.png", i)}});
        job["metadata"]["thumbnails"].push_back(
            {{"width", 300},
             {"height", 300},
             {"size", 16304},
             {"relative_path", fmt::format(".thumbs/Test_Model_{}-300x300.png", i)}});

        jobs.push_back(job);
    }

    // Serialize and verify size
    std::string json_str = response.dump();
    size_t response_size = json_str.size();

    // Should be > 100KB for realistic testing (real 50 jobs ~86KB)
    REQUIRE(response_size > 100 * 1024);
    INFO("Generated response size: " << response_size << " bytes (" << response_size / 1024
                                     << " KB)");

    // Now parse it back (this is what our MoonrakerAPI does)
    auto parsed = nlohmann::json::parse(json_str);
    REQUIRE(parsed.contains("result"));
    REQUIRE(parsed["result"]["count"].get<int>() == 500);
    REQUIRE(parsed["result"]["jobs"].size() == 200);

    // Parse all jobs into PrintHistoryJob structs
    std::vector<PrintHistoryJob> parsed_jobs;
    for (const auto& job_json : parsed["result"]["jobs"]) {
        parsed_jobs.push_back(parse_history_job(job_json));
    }

    REQUIRE(parsed_jobs.size() == 200);

    // Verify first and last jobs parsed correctly
    REQUIRE(parsed_jobs[0].job_id == "000000");
    REQUIRE(parsed_jobs[199].job_id == "000199");

    // Calculate stats from parsed jobs
    HistoryStats stats = calculate_stats(parsed_jobs);
    REQUIRE(stats.total_jobs == 200);
    // 20 cancelled (every 10th), 180 completed
    REQUIRE(stats.cancelled_jobs == 20);
    REQUIRE(stats.completed_jobs == 180);
    REQUIRE(stats.success_rate == 90.0);
}
