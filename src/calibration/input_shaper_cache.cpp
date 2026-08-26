// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_shaper_cache.h"

#include "system/helix_paths.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <fstream>

namespace helix {
namespace calibration {

// ============================================================================
// Constants
// ============================================================================

/// Cache file name
static constexpr const char* CACHE_FILENAME = "input_shaper_cache.json";

/// Cache format version
static constexpr int CACHE_VERSION = 1;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Try creating a cache directory and verify write access
 * @param path Directory path to create
 * @return true if directory exists and is writable
 */
static bool try_create_cache_dir(const std::filesystem::path& path) {
    const std::string dir = path.string();
    // Viability first, so a candidate we cannot use is rejected without a
    // directory tree left behind as the cost of finding out. ensure_dir() is
    // still the authority — can_create_dir() cannot see a race or a quota.
    return helix::paths::can_create_dir(dir) && helix::paths::ensure_dir(dir) &&
           helix::paths::probe_writable(dir);
}

/**
 * @brief Determine the cache directory following XDG spec
 * @return Path to cache directory for input shaper data
 */
static std::filesystem::path determine_cache_dir() {
    // 1/2. XDG cache base: $XDG_CACHE_HOME then $HOME/.cache (try each in order
    // so an uncreatable XDG dir still falls through to $HOME/.cache).
    for (const std::string& base : helix::paths::xdg_cache_bases()) {
        std::filesystem::path full_path = std::filesystem::path(base) / "helix";
        if (try_create_cache_dir(full_path)) {
            spdlog::debug("[InputShaperCache] Using cache base: {}", full_path.string());
            return full_path;
        }
        spdlog::warn("[InputShaperCache] Cannot use cache base {}", full_path.string());
    }

    // 3. Try /tmp/helix as fallback
    std::filesystem::path fallback = "/tmp/helix";
    if (try_create_cache_dir(fallback)) {
        spdlog::warn("[InputShaperCache] Falling back to /tmp: {}", fallback.string());
        return fallback;
    }

    // 4. Absolute last resort - current directory
    try {
        std::filesystem::path cwd_fallback = std::filesystem::current_path() / "helix_cache";
        spdlog::error("[InputShaperCache] No writable cache directory found, using {}",
                      cwd_fallback.string());
        return cwd_fallback;
    } catch (...) {
        // If even current_path() fails, use relative path
        spdlog::error("[InputShaperCache] No writable cache directory found, using ./helix_cache");
        return "./helix_cache";
    }
}

/**
 * @brief Get current Unix timestamp in seconds
 * @return Current time as seconds since epoch
 */
static int64_t get_current_timestamp() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

// ============================================================================
// JSON Serialization Functions
// ============================================================================

nlohmann::json shaper_option_to_json(const ShaperOption& opt) {
    return nlohmann::json{{"type", opt.type},
                          {"frequency", opt.frequency},
                          {"vibrations", opt.vibrations},
                          {"smoothing", opt.smoothing},
                          {"max_accel", opt.max_accel}};
}

ShaperOption shaper_option_from_json(const nlohmann::json& json) {
    ShaperOption opt;
    opt.type = json.value("type", "");
    opt.frequency = json.value("frequency", 0.0f);
    opt.vibrations = json.value("vibrations", 0.0f);
    opt.smoothing = json.value("smoothing", 0.0f);
    opt.max_accel = json.value("max_accel", 0.0f);
    return opt;
}

nlohmann::json input_shaper_result_to_json(const InputShaperResult& result) {
    // Convert freq_response to array of [freq, amplitude] pairs
    nlohmann::json freq_response_json = nlohmann::json::array();
    for (const auto& [freq, amplitude] : result.freq_response) {
        freq_response_json.push_back(nlohmann::json::array({freq, amplitude}));
    }

    // Convert all_shapers to JSON array
    nlohmann::json all_shapers_json = nlohmann::json::array();
    for (const auto& shaper : result.all_shapers) {
        all_shapers_json.push_back(shaper_option_to_json(shaper));
    }

    return nlohmann::json{
        {"axis", std::string(1, result.axis)}, {"shaper_type", result.shaper_type},
        {"shaper_freq", result.shaper_freq},   {"max_accel", result.max_accel},
        {"smoothing", result.smoothing},       {"vibrations", result.vibrations},
        {"freq_response", freq_response_json}, {"all_shapers", all_shapers_json}};
}

InputShaperResult input_shaper_result_from_json(const nlohmann::json& json) {
    InputShaperResult result;

    // Parse axis - stored as string "X" or "Y"
    std::string axis_str = json.value("axis", "X");
    result.axis = axis_str.empty() ? 'X' : axis_str[0];

    result.shaper_type = json.value("shaper_type", "");
    result.shaper_freq = json.value("shaper_freq", 0.0f);
    result.max_accel = json.value("max_accel", 0.0f);
    result.smoothing = json.value("smoothing", 0.0f);
    result.vibrations = json.value("vibrations", 0.0f);

    // Parse freq_response array
    if (json.contains("freq_response") && json["freq_response"].is_array()) {
        for (const auto& pair : json["freq_response"]) {
            if (pair.is_array() && pair.size() >= 2) {
                float freq = pair[0].get<float>();
                float amplitude = pair[1].get<float>();
                result.freq_response.emplace_back(freq, amplitude);
            }
        }
    }

    // Parse all_shapers array
    if (json.contains("all_shapers") && json["all_shapers"].is_array()) {
        for (const auto& shaper_json : json["all_shapers"]) {
            result.all_shapers.push_back(shaper_option_from_json(shaper_json));
        }
    }

    return result;
}

nlohmann::json
calibration_results_to_json(const InputShaperCalibrator::CalibrationResults& results) {
    nlohmann::json json;
    json["version"] = CACHE_VERSION;
    json["noise_level"] = results.noise_level;

    // Always include both results, even if empty (for test compatibility)
    json["x_result"] = input_shaper_result_to_json(results.x_result);
    json["y_result"] = input_shaper_result_to_json(results.y_result);

    return json;
}

InputShaperCalibrator::CalibrationResults
calibration_results_from_json(const nlohmann::json& json) {
    InputShaperCalibrator::CalibrationResults results;

    results.noise_level = json.value("noise_level", 0.0f);

    if (json.contains("x_result") && json["x_result"].is_object()) {
        results.x_result = input_shaper_result_from_json(json["x_result"]);
    }

    if (json.contains("y_result") && json["y_result"].is_object()) {
        results.y_result = input_shaper_result_from_json(json["y_result"]);
    }

    return results;
}

// ============================================================================
// InputShaperCache Implementation
// ============================================================================

InputShaperCache::InputShaperCache() : cache_dir_(determine_cache_dir()) {
    spdlog::debug("[InputShaperCache] Initialized with default cache dir: {}", cache_dir_.string());
}

InputShaperCache::InputShaperCache(const std::filesystem::path& cache_dir) : cache_dir_(cache_dir) {
    // Ensure the cache directory exists
    try {
        std::filesystem::create_directories(cache_dir_);
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[InputShaperCache] Failed to create cache directory {}: {}",
                     cache_dir_.string(), e.what());
    }
    spdlog::debug("[InputShaperCache] Initialized with cache dir: {}", cache_dir_.string());
}

bool InputShaperCache::save_results(const InputShaperCalibrator::CalibrationResults& results,
                                    const std::string& printer_id) {
    try {
        // Ensure cache directory exists
        std::filesystem::create_directories(cache_dir_);

        // Build the full JSON document
        nlohmann::json json = calibration_results_to_json(results);
        json["printer_id"] = printer_id;
        json["timestamp"] = get_current_timestamp();

        // Write to file
        std::filesystem::path cache_path = get_cache_path();
        std::ofstream file(cache_path);
        if (!file) {
            spdlog::error("[InputShaperCache] Failed to open cache file for writing: {}",
                          cache_path.string());
            return false;
        }

        file << json.dump(2); // Pretty print with 2-space indent
        file.close();

        if (!file) {
            spdlog::error("[InputShaperCache] Failed to write cache file: {}", cache_path.string());
            return false;
        }

        spdlog::info("[InputShaperCache] Saved calibration results to {}", cache_path.string());
        return true;

    } catch (const std::exception& e) {
        spdlog::error("[InputShaperCache] Error saving cache: {}", e.what());
        return false;
    }
}

std::optional<InputShaperCalibrator::CalibrationResults>
InputShaperCache::load_results(const std::string& printer_id) {
    try {
        std::filesystem::path cache_path = get_cache_path();

        // Check if file exists
        if (!std::filesystem::exists(cache_path)) {
            spdlog::debug("[InputShaperCache] Cache file not found: {}", cache_path.string());
            return std::nullopt;
        }

        // Read file
        std::ifstream file(cache_path);
        if (!file) {
            spdlog::warn("[InputShaperCache] Failed to open cache file: {}", cache_path.string());
            return std::nullopt;
        }

        nlohmann::json json;
        try {
            file >> json;
        } catch (const nlohmann::json::parse_error& e) {
            spdlog::warn("[InputShaperCache] Failed to parse cache file: {}", e.what());
            return std::nullopt;
        }

        // Validate version
        int version = json.value("version", 0);
        if (version != CACHE_VERSION) {
            spdlog::warn("[InputShaperCache] Cache version mismatch (got {}, expected {})", version,
                         CACHE_VERSION);
            return std::nullopt;
        }

        // Validate printer_id
        std::string cached_printer_id = json.value("printer_id", "");
        if (cached_printer_id != printer_id) {
            spdlog::debug("[InputShaperCache] Printer ID mismatch (cached: '{}', requested: '{}')",
                          cached_printer_id, printer_id);
            return std::nullopt;
        }

        // Validate timestamp (TTL check)
        int64_t timestamp = json.value("timestamp", static_cast<int64_t>(0));
        if (!is_timestamp_valid(timestamp)) {
            spdlog::info("[InputShaperCache] Cache expired for printer '{}'", printer_id);
            return std::nullopt;
        }

        // Parse results
        InputShaperCalibrator::CalibrationResults results = calibration_results_from_json(json);

        spdlog::info("[InputShaperCache] Loaded cached calibration results for printer '{}'",
                     printer_id);
        return results;

    } catch (const std::exception& e) {
        spdlog::warn("[InputShaperCache] Error loading cache: {}", e.what());
        return std::nullopt;
    }
}

bool InputShaperCache::has_cached_results(const std::string& printer_id) {
    try {
        std::filesystem::path cache_path = get_cache_path();

        // Quick existence check
        if (!std::filesystem::exists(cache_path)) {
            return false;
        }

        // Read just enough to validate printer_id and timestamp without full parse
        std::ifstream file(cache_path);
        if (!file) {
            return false;
        }

        nlohmann::json json;
        try {
            file >> json;
        } catch (const nlohmann::json::parse_error&) {
            return false;
        }

        // Check version
        int version = json.value("version", 0);
        if (version != CACHE_VERSION) {
            return false;
        }

        // Check printer_id
        std::string cached_printer_id = json.value("printer_id", "");
        if (cached_printer_id != printer_id) {
            return false;
        }

        // Check timestamp
        int64_t timestamp = json.value("timestamp", static_cast<int64_t>(0));
        return is_timestamp_valid(timestamp);

    } catch (...) {
        return false;
    }
}

void InputShaperCache::clear_cache() {
    try {
        std::filesystem::path cache_path = get_cache_path();
        if (std::filesystem::exists(cache_path)) {
            std::filesystem::remove(cache_path);
            spdlog::info("[InputShaperCache] Cleared cache file: {}", cache_path.string());
        }
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[InputShaperCache] Failed to clear cache: {}", e.what());
    }
}

std::filesystem::path InputShaperCache::get_cache_path() const {
    return cache_dir_ / CACHE_FILENAME;
}

bool InputShaperCache::is_timestamp_valid(int64_t timestamp) const {
    if (timestamp <= 0) {
        return false;
    }

    int64_t current_time = get_current_timestamp();
    int64_t ttl_seconds = static_cast<int64_t>(DEFAULT_TTL_DAYS) * 24 * 60 * 60;
    int64_t expiration_time = timestamp + ttl_seconds;

    return current_time < expiration_time;
}

} // namespace calibration
} // namespace helix
