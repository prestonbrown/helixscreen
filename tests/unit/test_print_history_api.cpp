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
#include "../../include/print_history_parse.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

class PrintHistoryTestFixture : public LVGLTestFixture {
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
        CHECK_FALSE(job.date_str.empty());
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
// PrintHistoryJob Parsing Tests
// ============================================================================

// A QIDI Q2 Moonraker copies slicer settings into metadata as JSON strings
// (prestonbrown/helixscreen#1713). This row is verbatim from a Q2.
TEST_CASE("Parse Q2 history row with string metadata", "[history][parsing][json_coercion]") {
    auto job_json = nlohmann::json::parse(R"({
        "job_id": "000002",
        "filename": "Foo (PETG).gcode.3mf",
        "status": "interrupted",
        "start_time": 1758000000.5,
        "end_time": null,
        "print_duration": 0.0,
        "total_duration": 0.8278,
        "filament_used": 0.0,
        "metadata": {
            "size": 51671,
            "layer_height": "0.2",
            "thumbnails": [
                {"width": 300, "height": 300, "size": 0,
                 "relative_path": ".thumbs/Foo-300x300.png"}
            ]
        },
        "exists": true
    })");

    PrintHistoryJob job = parse_history_job(job_json);

    CHECK(job.status == PrintJobStatus::ERROR);
    CHECK(job.end_time == 0.0);
    CHECK(job.total_duration == Catch::Approx(0.8278));
    CHECK(job.layer_height == Catch::Approx(0.2));
    CHECK(job.size_bytes == 51671);
    REQUIRE(job.thumbnails.size() == 1);
    CHECK(job.thumbnails[0].relative_path == ".thumbs/Foo-300x300.png");
}

TEST_CASE("History numeric fields accept JSON strings", "[history][parsing][json_coercion]") {
    auto job_json = nlohmann::json::parse(R"({
        "job_id": "000003",
        "status": "completed",
        "start_time": "1758000000.5",
        "end_time": "1758003600.5",
        "print_duration": "3500.25",
        "total_duration": "3600.5",
        "filament_used": "1234.5",
        "metadata": {
            "layer_count": "42",
            "layer_height": "0.2",
            "first_layer_extr_temp": "220",
            "first_layer_bed_temp": "60",
            "size": "711288",
            "modified": "1757990000.25"
        }
    })");

    PrintHistoryJob job = parse_history_job(job_json);

    CHECK(job.start_time == Catch::Approx(1758000000.5));
    CHECK(job.end_time == Catch::Approx(1758003600.5));
    CHECK(job.print_duration == Catch::Approx(3500.25));
    CHECK(job.total_duration == Catch::Approx(3600.5));
    CHECK(job.filament_used == Catch::Approx(1234.5));
    CHECK(job.layer_count == 42);
    CHECK(job.layer_height == Catch::Approx(0.2));
    CHECK(job.nozzle_temp == Catch::Approx(220.0));
    CHECK(job.bed_temp == Catch::Approx(60.0));
    CHECK(job.size_bytes == 711288);
    CHECK(job.modified == Catch::Approx(1757990000.25));
}

TEST_CASE("parse_history_job reads the object height", "[history][parsing]") {
    auto job_json = nlohmann::json::parse(
        R"({"job_id": "000010", "status": "completed", "metadata": {"object_height": 12.5}})");
    CHECK(parse_history_job(job_json).object_height == Catch::Approx(12.5));

    auto missing = nlohmann::json::parse(
        R"({"job_id": "000011", "status": "completed", "metadata": {"layer_height": 0.2}})");
    CHECK(parse_history_job(missing).object_height == 0.0);
}

TEST_CASE("History numeric fields default on null, missing, negative and garbage",
          "[history][parsing][json_coercion]") {
    auto job_json = nlohmann::json::parse(R"({
        "job_id": "000004",
        "status": "completed",
        "start_time": null,
        "print_duration": "garbage",
        "metadata": {
            "layer_count": "-1",
            "layer_height": null,
            "first_layer_extr_temp": "hot",
            "size": "-1",
            "modified": null
        }
    })");

    PrintHistoryJob job = parse_history_job(job_json);

    CHECK(job.start_time == 0.0);
    CHECK(job.end_time == 0.0);
    CHECK(job.print_duration == 0.0);
    CHECK(job.layer_count == 0);
    CHECK(job.layer_height == 0.0);
    CHECK(job.nozzle_temp == 0.0);
    CHECK(job.bed_temp == 0.0);
    CHECK(job.size_bytes == 0);
    CHECK(job.modified == 0.0);

    SECTION("a negative or out-of-range layer count does not wrap") {
        job_json["metadata"]["layer_count"] = -1;
        CHECK(parse_history_job(job_json).layer_count == 0);
        job_json["metadata"]["layer_count"] = 5000000000LL;
        CHECK(parse_history_job(job_json).layer_count == 0);
    }
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
// Real Voron Printer Data Tests (from user's 192.168.1.112)
// ============================================================================

// Exact JSON response from curl to real Voron printer
static const char* kRealVoronHistory5Jobs = R"({
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
    auto j = nlohmann::json::parse(kRealVoronHistory5Jobs);

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
