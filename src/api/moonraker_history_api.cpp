// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_history_api.h"

#include "ui_format_utils.h"

#include "display_settings_manager.h"
#include "format_utils.h"
#include "json_utils.h"
#include "locale_formats.h"
#include "moonraker_api_internal.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <iomanip>
#include <sstream>

#ifdef __GLIBC__
#include <malloc.h> // malloc_trim() after dropping the history DOM
#endif

#include "hv/json.hpp"

using namespace helix;

namespace {

/**
 * @brief Format duration in seconds to human-readable string
 * @param seconds Duration in seconds
 * @return Formatted string like "2h 15m" or "45m" or "30s"
 */
std::string format_history_duration(double seconds) {
    return helix::format::duration(static_cast<int>(seconds));
}

/**
 * @brief Format Unix timestamp to human-readable date
 * @param timestamp Unix timestamp (seconds since epoch)
 * @return Formatted string like "Dec 1, 2:30 PM" (12H) or "Dec 1, 14:30" (24H)
 */
std::string format_history_date(double timestamp) {
    time_t t = static_cast<time_t>(timestamp);
    return helix::ui::format_modified_date(t);
}

/**
 * @brief Format filament usage in mm to human-readable string
 * @param mm Filament length in millimeters
 * @return Formatted string like "12.5m" or "1.2km"
 */
std::string format_history_filament(double mm) {
    char buf[32];
    if (mm < 1000) {
        snprintf(buf, sizeof(buf), "%.0fmm", mm);
    } else if (mm < 1000000) {
        snprintf(buf, sizeof(buf), "%.1fm", mm / 1000.0);
    } else {
        snprintf(buf, sizeof(buf), "%.2fkm", mm / 1000000.0);
    }
    return std::string(buf);
}

/**
 * @brief Null-safe numeric value extraction from JSON
 *
 * Unlike json::value(), this handles fields that exist but are null.
 * Returns default_val if key is missing OR if value is null/non-numeric.
 */
template <typename T> T json_number_or(const nlohmann::json& j, const char* key, T default_val) {
    if (j.contains(key) && j[key].is_number()) {
        return j[key].get<T>();
    }
    return default_val;
}

/**
 * @brief Parse a single job from Moonraker history response
 */
PrintHistoryJob parse_history_job(const json& job_json) {
    PrintHistoryJob job;

    // String fields. json::value() is safe for a MISSING key but NOT for a key
    // present with a null value — it calls get<std::string>() on the null and
    // throws type_error.302. Moonraker writes a null "filename" for jobs whose
    // source file has since been deleted, and there is no try/catch on this
    // path: one such row would abort the whole server.history.list parse, so
    // on_success never fires and the history panel spins forever.
    job.job_id = helix::json_util::safe_string(job_json, "job_id", "");
    job.filename = helix::json_util::safe_string(job_json, "filename", "");
    job.status = parse_job_status(helix::json_util::safe_string(job_json, "status", "unknown"));

    // Numeric fields - use json_number_or() for null-safety
    // end_time is notably null for in-progress jobs
    job.start_time = json_number_or(job_json, "start_time", 0.0);
    job.end_time = json_number_or(job_json, "end_time", 0.0);
    job.print_duration = json_number_or(job_json, "print_duration", 0.0);
    job.total_duration = json_number_or(job_json, "total_duration", 0.0);
    job.filament_used = json_number_or(job_json, "filament_used", 0.0);

    // Boolean. There is no existence check here (the old comment claimed one),
    // and value() throws on a present-but-null "exists" just as it does above.
    job.exists = helix::json_util::safe_bool(job_json, "exists", false);

    // Metadata (may be nested or null)
    if (job_json.contains("metadata") && job_json["metadata"].is_object()) {
        const auto& meta = job_json["metadata"];
        job.filament_type = moonraker_internal::json_string_list_or(meta, "filament_type");
        job.layer_count = json_number_or(meta, "layer_count", 0u);
        job.layer_height = json_number_or(meta, "layer_height", 0.0);
        job.nozzle_temp = json_number_or(meta, "first_layer_extr_temp", 0.0);
        job.bed_temp = json_number_or(meta, "first_layer_bed_temp", 0.0);

        // Parse all available thumbnails with dimensions
        if (meta.contains("thumbnails") && meta["thumbnails"].is_array()) {
            int best_pixels = 0;
            for (const auto& t : meta["thumbnails"]) {
                ThumbnailInfo info;
                info.relative_path = helix::json_util::safe_string(t, "relative_path", "");
                // safe_int also coerces the string-encoded dimensions some
                // slicers emit ("400" rather than 400).
                info.width = helix::json_util::safe_int(t, "width", 0);
                info.height = helix::json_util::safe_int(t, "height", 0);
                if (!info.relative_path.empty()) {
                    job.thumbnails.push_back(info);
                    if (info.pixel_count() > best_pixels) {
                        best_pixels = static_cast<int>(info.pixel_count());
                        job.thumbnail_path = info.relative_path;
                    }
                }
            }
        }

        // UUID and file size for precise history matching
        job.uuid = helix::json_util::safe_string(meta, "uuid", "");
        job.size_bytes = json_number_or(meta, "size", static_cast<size_t>(0));

        // Source gcode mtime. Same field server.files.metadata returns, carried
        // in the history snapshot; it is what tells a thumbnail consumer that a
        // re-slice under the same filename outdated the cached render. Absent
        // means 0, which every consumer reads as "skip freshness validation".
        job.modified = json_number_or(meta, "modified", 0.0);
    }

    // Pre-format display strings
    job.duration_str = format_history_duration(job.print_duration);
    job.date_str = format_history_date(job.start_time);
    job.filament_str = format_history_filament(job.filament_used);

    return job;
}

} // anonymous namespace

// ============================================================================
// MoonrakerHistoryAPI Implementation
// ============================================================================

MoonrakerHistoryAPI::MoonrakerHistoryAPI(IMoonrakerClient& client) : client_(client) {}

void MoonrakerHistoryAPI::get_history_list(int limit, int start, double since, double before,
                                           HistoryListCallback on_success, ErrorCallback on_error) {
    json params = json::object();
    params["limit"] = limit;
    params["start"] = start;

    // Only add time filters if non-zero
    if (since > 0) {
        params["since"] = since;
    }
    if (before > 0) {
        params["before"] = before;
    }

    spdlog::debug("[HistoryAPI] get_history_list(limit={}, start={}, since={}, before={})", limit,
                  start, since, before);

    client_.send_jsonrpc(
        "server.history.list", params,
        [on_success](json response) {
            std::vector<PrintHistoryJob> jobs;
            uint64_t total_count = 0;

            if (response.contains("result")) {
                const auto& result = response["result"];
                // Use null-safe access - count might be null in edge cases
                if (result.contains("count") && result["count"].is_number()) {
                    total_count = result["count"].get<uint64_t>();
                }

                if (result.contains("jobs") && result["jobs"].is_array()) {
                    for (const auto& job_json : result["jobs"]) {
                        jobs.push_back(parse_history_job(job_json));
                    }
                }
            }

            spdlog::debug("[HistoryAPI] get_history_list returned {} jobs (total: {})", jobs.size(),
                          total_count);

            // This is the largest response the app parses: 500 jobs measured at
            // 714 KB of JSON on a printer with real history, which costs ~3.9 MB
            // of heap as a DOM. Everything worth keeping is already in `jobs`,
            // so drop the DOM here rather than holding it across the whole
            // callback chain — on_success() copies the job vector and hands it
            // to the main thread, so the peak would otherwise stack the DOM, the
            // vector, and its copy at once.
            response = json();
#ifdef __GLIBC__
            // Freeing the DOM returns its pages to glibc's free lists, not to
            // the OS — measured at 0 kB of RSS recovered without this call, and
            // ~3.4 MB with it. Worth the arena walk here because a history fetch
            // happens at boot and once per finished print, not per message.
            malloc_trim(0);
#endif

            if (on_success) {
                on_success(jobs, total_count);
            }
        },
        on_error);
}

void MoonrakerHistoryAPI::get_history_totals(HistoryTotalsCallback on_success,
                                             ErrorCallback on_error) {
    spdlog::debug("[HistoryAPI] get_history_totals()");

    client_.send_jsonrpc(
        "server.history.totals", json::object(),
        [on_success](json response) {
            PrintHistoryTotals totals;

            if (response.contains("result") && response["result"].contains("job_totals") &&
                response["result"]["job_totals"].is_object()) {
                const auto& jt = response["result"]["job_totals"];
                // Null-safe numeric access for all fields
                if (jt.contains("total_jobs") && jt["total_jobs"].is_number()) {
                    totals.total_jobs = jt["total_jobs"].get<uint64_t>();
                }
                if (jt.contains("total_time") && jt["total_time"].is_number()) {
                    totals.total_time = static_cast<uint64_t>(jt["total_time"].get<double>());
                }
                if (jt.contains("total_filament_used") && jt["total_filament_used"].is_number()) {
                    totals.total_filament_used = jt["total_filament_used"].get<double>();
                }
                if (jt.contains("longest_job") && jt["longest_job"].is_number()) {
                    totals.longest_job = jt["longest_job"].get<double>();
                }
                // Note: Moonraker doesn't provide breakdown counts (completed/cancelled/failed)
                // These must be calculated client-side from the job list if needed
            }

            spdlog::debug("[HistoryAPI] get_history_totals: {} jobs, {}s total time",
                          totals.total_jobs, totals.total_time);

            if (on_success) {
                on_success(totals);
            }
        },
        on_error);
}

void MoonrakerHistoryAPI::delete_history_job(const std::string& job_id, SuccessCallback on_success,
                                             ErrorCallback on_error) {
    json params = json::object();
    params["uid"] = job_id;

    spdlog::debug("[HistoryAPI] delete_history_job(uid={})", job_id);

    client_.send_jsonrpc(
        "server.history.delete_job", params,
        [on_success, job_id](json /*response*/) {
            spdlog::info("[HistoryAPI] Deleted history job: {}", job_id);
            if (on_success) {
                on_success();
            }
        },
        on_error);
}
