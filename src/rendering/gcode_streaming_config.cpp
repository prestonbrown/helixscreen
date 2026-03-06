// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_streaming_config.h"

#include "config.h"
#include "memory_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace helix {

GCodeStreamingMode get_gcode_streaming_mode() {
    // Priority 1: Environment variable (highest)
    const char* env = std::getenv("HELIX_GCODE_STREAMING");
    if (env != nullptr) {
        if (std::strcmp(env, "on") == 0 || std::strcmp(env, "ON") == 0) {
            spdlog::debug("[GCodeStreaming] Mode from env var: ON");
            return GCodeStreamingMode::ON;
        }
        if (std::strcmp(env, "off") == 0 || std::strcmp(env, "OFF") == 0) {
            spdlog::debug("[GCodeStreaming] Mode from env var: OFF");
            return GCodeStreamingMode::OFF;
        }
        if (std::strcmp(env, "auto") == 0 || std::strcmp(env, "AUTO") == 0) {
            spdlog::debug("[GCodeStreaming] Mode from env var: AUTO");
            return GCodeStreamingMode::AUTO;
        }
        spdlog::warn("[GCodeStreaming] Unknown HELIX_GCODE_STREAMING value '{}', using AUTO", env);
    }

    // Priority 2: Config file
    Config* config = Config::get_instance();
    if (config != nullptr) {
        std::string mode = config->get<std::string>("/gcode_viewer/streaming_mode", "auto");
        if (mode == "on") {
            spdlog::debug("[GCodeStreaming] Mode from config: ON");
            return GCodeStreamingMode::ON;
        }
        if (mode == "off") {
            spdlog::debug("[GCodeStreaming] Mode from config: OFF");
            return GCodeStreamingMode::OFF;
        }
        // Default to AUTO for any other value
        spdlog::trace("[GCodeStreaming] Mode from config: AUTO");
    }

    return GCodeStreamingMode::AUTO;
}

int get_streaming_threshold_percent() {
    Config* config = Config::get_instance();
    if (config != nullptr) {
        int percent = config->get<int>("/gcode_viewer/streaming_threshold_percent", 40);
        // Clamp to valid range
        return std::clamp(percent, 1, 90);
    }
    return 40; // Default
}

size_t calculate_streaming_threshold(size_t available_memory_kb, int threshold_percent) {
    // Calculate max memory usage allowed
    float threshold_ratio = static_cast<float>(threshold_percent) / 100.0f;
    size_t max_memory_bytes = available_memory_kb * 1024 * threshold_ratio;

    // Work backwards: max_file_size * EXPANSION_FACTOR = max_memory_bytes
    // So max_file_size = max_memory_bytes / EXPANSION_FACTOR
    size_t max_file_size = max_memory_bytes / GCodeMemoryLimits::EXPANSION_FACTOR;

    spdlog::trace("[GCodeStreaming] Threshold calculation: {}MB RAM × {}% / {}x = {}MB max file",
                  available_memory_kb / 1024, threshold_percent,
                  GCodeMemoryLimits::EXPANSION_FACTOR, max_file_size / (1024 * 1024));

    return max_file_size;
}

bool should_use_gcode_streaming(size_t file_size_bytes, const MemoryInfo& mem) {
    // Force streaming on low-RAM devices (<=2GB total)
    if (mem.should_force_streaming()) {
        spdlog::debug("[GCodeStreaming] Low-RAM device ({}MB) - forcing streaming",
                      mem.total_kb / 1024);
        return true;
    }

    // If we can't read memory info, be conservative
    if (mem.available_kb == 0) {
        spdlog::warn("[GCodeStreaming] Cannot read memory info, defaulting to streaming "
                     "for files > 2MB");
        return file_size_bytes > (2 * 1024 * 1024);
    }

    // Calculate threshold based on available memory
    int threshold_pct = get_streaming_threshold_percent();
    size_t threshold_bytes = calculate_streaming_threshold(mem.available_kb, threshold_pct);

    bool should_stream = file_size_bytes > threshold_bytes;

    spdlog::trace("[GCodeStreaming] AUTO decision: {}KB file {} {}KB threshold ({}MB RAM, "
                  "{}%)",
                  file_size_bytes / 1024, should_stream ? ">" : "<=", threshold_bytes / 1024,
                  mem.available_kb / 1024, threshold_pct);

    return should_stream;
}

bool should_use_gcode_streaming(size_t file_size_bytes) {
    GCodeStreamingMode mode = get_gcode_streaming_mode();

    switch (mode) {
    case GCodeStreamingMode::ON:
        spdlog::debug("[GCodeStreaming] Forced ON - streaming for {}KB file",
                      file_size_bytes / 1024);
        return true;

    case GCodeStreamingMode::OFF:
        spdlog::debug("[GCodeStreaming] Forced OFF - full load for {}KB file",
                      file_size_bytes / 1024);
        return false;

    case GCodeStreamingMode::AUTO: {
        MemoryInfo mem = get_system_memory_info();
        return should_use_gcode_streaming(file_size_bytes, mem);
    }
    }

    return false; // Unreachable, but satisfies compiler
}

std::string get_streaming_config_description() {
    GCodeStreamingMode mode = get_gcode_streaming_mode();
    std::string mode_str;

    switch (mode) {
    case GCodeStreamingMode::ON:
        mode_str = "ON (forced)";
        break;
    case GCodeStreamingMode::OFF:
        mode_str = "OFF (forced)";
        break;
    case GCodeStreamingMode::AUTO: {
        MemoryInfo mem = get_system_memory_info();
        int threshold_pct = get_streaming_threshold_percent();

        if (mem.available_kb > 0) {
            size_t threshold_bytes = calculate_streaming_threshold(mem.available_kb, threshold_pct);
            float threshold_mb = static_cast<float>(threshold_bytes) / (1024.0f * 1024.0f);

            char buf[128];
            std::snprintf(buf, sizeof(buf), "AUTO (threshold %.1fMB based on %zuMB RAM, %d%%)",
                          threshold_mb, mem.available_kb / 1024, threshold_pct);
            mode_str = buf;
        } else {
            mode_str = "AUTO (memory info unavailable, fallback to 2MB)";
        }
        break;
    }
    }

    return "streaming=" + mode_str;
}

} // namespace helix
