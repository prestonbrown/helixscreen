// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/gcode_filename.h"
#include "env_knobs.h"
#include "gcode_parser.h"
#include "mock_planted_gcodes.h"
#include "moonraker_client_mock_internal.h"
#include "print_photos.h"
#include "runtime_config.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <sys/stat.h>

namespace {

/**
 * @brief Scan test gcode directory for filenames (same source as file list mock)
 * @return Vector of gcode filenames found in TEST_GCODE_DIR
 */
std::vector<std::string> scan_test_gcode_files() {
    std::vector<std::string> files;

    DIR* dir = opendir(RuntimeConfig::TEST_GCODE_DIR);
    if (!dir) {
        spdlog::warn("[MockHistory] Cannot open test G-code directory: {}",
                     RuntimeConfig::TEST_GCODE_DIR);
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // Skip hidden files and non-gcode files
        if (name[0] == '.' || name.length() < 7) {
            continue;
        }

        // Check for .gcode extension (case insensitive)
        std::string ext = name.substr(name.length() - 6);
        ext = helix::text_io::to_lower(ext);
        if (ext != ".gcode") {
            continue;
        }

        files.push_back(name);
    }

    closedir(dir);
    std::sort(files.begin(), files.end());
    return files;
}

/**
 * @brief Get file size for a test gcode file
 */
size_t get_test_file_size(const std::string& filename) {
    std::string path = std::string(RuntimeConfig::TEST_GCODE_DIR) + "/" + filename;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return static_cast<size_t>(st.st_size);
    }
    return 0;
}

} // namespace

namespace mock_internal {

void register_history_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    // server.history.list - Get print history
    registry["server.history.list"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        (void)error_cb;

        // Dynamically scan test gcode directory - same source as file list mock
        // This ensures history filenames always match the file list
        auto gcode_files = scan_test_gcode_files();

        json jobs = json::array();
        auto now = std::chrono::system_clock::now();

        // Status distribution: ~70% completed, ~15% cancelled, ~15% error
        // Using simple pattern: every 7th is cancelled, every 8th is error
        // A power-loss snapshot on record makes the newest job the
        // interrupted one, as Moonraker marks it after the restart.
        const bool interrupted = self->mock_plr_interrupted();
        auto get_status = [interrupted](size_t idx) -> const char* {
            if (idx == 0 && interrupted)
                return "interrupted";
            if (idx % 8 == 7)
                return "error";
            if (idx % 7 == 6)
                return "cancelled";
            return "completed";
        };

        // Generate mock durations based on file index
        auto get_duration_minutes = [](size_t idx) -> int {
            // Varied durations: 5-300 minutes based on hash of index
            return 5 + static_cast<int>((idx * 37 + 13) % 296);
        };

        int limit = params.value("limit", 50);
        int start = params.value("start", 0);
        double since = params.value("since", 0.0);
        double before = params.value("before", 0.0);

        int count = 0;
        for (size_t i = 0; i < gcode_files.size() && count < limit; i++) {
            if (static_cast<int>(i) < start)
                continue;

            const std::string& filename = gcode_files[i];
            const char* status = get_status(i);
            int duration_minutes = get_duration_minutes(i);

            // Spread jobs across last 30 days
            auto job_time =
                now - std::chrono::hours(24 * (i + 1)) - std::chrono::minutes(i * 37 % 60);
            double start_time = std::chrono::duration<double>(job_time.time_since_epoch()).count();
            double end_time = start_time + duration_minutes * 60;

            // Moonraker's window: start_time > since and start_time < before
            if (since > 0 && start_time < since)
                continue;
            if (before > 0 && start_time >= before)
                continue;

            // Generate thumbnail path from filename (strip the gcode extension,
            // add thumbnail suffix). Path is preserved — Moonraker thumbnail
            // paths are relative to the gcode's own directory — so this strips
            // the extension only, not the basename.
            std::string base_name = helix::gcode::strip_gcode_extension(filename);
            std::string thumb_path = ".thumbnails/" + base_name + "-300x300.png";

            // Generate mock UUID and get real file size for history matching
            std::string mock_uuid =
                fmt::format("mock-uuid-{:08x}", static_cast<unsigned int>(i * 12345 + 67890));
            size_t file_size = get_test_file_size(filename);
            const double object_height =
                helix::gcode::extract_header_metadata(helix::mock::gcode_disk_path(filename))
                    .object_height;

            // Generate mock filament usage based on duration
            int filament_mm = duration_minutes * 50; // ~50mm/minute

            json job = {{"job_id", fmt::format("mock_job_{:03d}", i)},
                        {"filename", filename},
                        {"status", status},
                        {"start_time", start_time},
                        {"end_time", end_time},
                        {"print_duration", duration_minutes * 60.0},
                        {"total_duration", duration_minutes * 60.0 + 120},
                        {"filament_used", static_cast<double>(filament_mm)},
                        {"exists", true},
                        {"metadata",
                         {{"filament_type", "PLA"},
                          {"layer_count", duration_minutes * 2},
                          {"layer_height", 0.2},
                          {"object_height", object_height},
                          {"first_layer_extr_temp", 210.0},
                          {"first_layer_bed_temp", 60.0},
                          {"uuid", mock_uuid},
                          {"size", file_size},
                          // Source gcode mtime, as Moonraker reports it. Set to
                          // the job's start time: the file existed when it was
                          // printed, so anything cached from it is at least that
                          // old.
                          {"modified", start_time},
                          {"thumbnails", json::array({{{"relative_path", thumb_path},
                                                       {"width", 300},
                                                       {"height", 300},
                                                       {"size", 25000}}})}}}};
            // HELIX_MOCK_PRINT_PHOTOS=1: the newest job has its camera photos,
            // linked from auxiliary_data as the HelixPrint plugin links them.
            if (i == 0 && helix::env_flag("HELIX_MOCK_PRINT_PHOTOS")) {
                const std::string job_id = job["job_id"].get<std::string>();
                job["auxiliary_data"] = json::array(
                    {{{"provider", helix::kPhotosAuxProvider},
                      {"name", helix::kPhotosAuxName},
                      {"value",
                       json::array({helix::photo_path(job_id, start_time, helix::kPhotoKindLayer),
                                    helix::photo_path(job_id, start_time, helix::kPhotoKindEnd)})},
                      {"description", "Camera photos"},
                      {"units", nullptr}}});
            }
            jobs.push_back(job);
            count++;
        }

        spdlog::debug("[MockHistory] Generated {} history jobs from {} test files", jobs.size(),
                      gcode_files.size());

        if (success_cb) {
            json response = {{"result", {{"count", gcode_files.size()}, {"jobs", jobs}}}};
            success_cb(response);
        }
        return true;
    };

    // server.history.totals - Get aggregate statistics
    // Computed from the same mock job data so totals match the job list
    registry["server.history.totals"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        (void)params;
        (void)error_cb;

        auto gcode_files = scan_test_gcode_files();

        // Same duration formula as server.history.list
        auto get_duration_minutes = [](size_t idx) -> int {
            return 5 + static_cast<int>((idx * 37 + 13) % 296);
        };

        double total_time = 0.0;
        double total_filament = 0.0;
        double longest_job = 0.0;
        for (size_t i = 0; i < gcode_files.size(); i++) {
            double duration_sec = get_duration_minutes(i) * 60.0;
            double filament_mm = get_duration_minutes(i) * 50.0;
            total_time += duration_sec;
            total_filament += filament_mm;
            if (duration_sec > longest_job)
                longest_job = duration_sec;
        }

        json response = {{"result",
                          {{"job_totals",
                            {{"total_jobs", gcode_files.size()},
                             {"total_time", total_time},
                             {"total_filament_used", total_filament},
                             {"longest_job", longest_job}}}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.history.delete_job - Delete a job from history
    registry["server.history.delete_job"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        (void)self;
        (void)error_cb;
        std::string job_id = params.value("uid", "");
        spdlog::info("[MoonrakerClientMock] Mock delete history job: {}", job_id);

        if (success_cb) {
            json response = {{"result", {{"deleted_jobs", json::array({job_id})}}}};
            success_cb(response);
        }
        return true;
    };
}

} // namespace mock_internal
