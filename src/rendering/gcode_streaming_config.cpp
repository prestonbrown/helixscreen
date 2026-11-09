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

namespace {

// Boards at or under 2GB total usually co-host Klipper and Moonraker, and the
// available-KB figure read when the viewer opens keeps shrinking once the
// print is running. They get a smaller share of it than the 40% default: on a
// Pi 3B (856MB total, ~600MB available) that is still a ~6MB file ceiling, so
// a Benchy full-parses and the 3D preview can follow the print, while anything
// slice-farm sized streams. A configured percentage below this stays honoured;
// one above it is capped.
constexpr int kLowRamThresholdPercent = 15;

int effective_threshold_percent(const MemoryInfo& mem) {
    const int configured = get_streaming_threshold_percent();
    return mem.is_low_ram_device() ? std::min(configured, kLowRamThresholdPercent) : configured;
}

} // namespace

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
    std::string mode = config->get<std::string>("/gcode_viewer/streaming_mode", "auto");
    if (mode == "on") {
        spdlog::debug("[GCodeStreaming] Mode from config: ON");
        return GCodeStreamingMode::ON;
    }
    if (mode == "off") {
        spdlog::debug("[GCodeStreaming] Mode from config: OFF");
        return GCodeStreamingMode::OFF;
    }
    // The streaming section's force_streaming is another spelling of "on"
    if (config->get<bool>("/streaming/force_streaming", false)) {
        spdlog::debug("[GCodeStreaming] Mode from config: ON (streaming.force_streaming)");
        return GCodeStreamingMode::ON;
    }
    spdlog::trace("[GCodeStreaming] Mode from config: AUTO");

    return GCodeStreamingMode::AUTO;
}

int get_streaming_threshold_percent() {
    Config* config = Config::get_instance();
    int percent = config->get<int>("/gcode_viewer/streaming_threshold_percent", 40);
    return std::clamp(percent, 1, 90);

    return 40; // Default
}

size_t calculate_streaming_threshold(size_t available_memory_kb, int threshold_percent) {
    // Calculate max memory usage allowed
    float threshold_ratio = static_cast<float>(threshold_percent) / 100.0f;
    size_t max_memory_bytes = available_memory_kb * 1024 * threshold_ratio;

    // Work backwards: max_file_size * kExpansionFactor = max_memory_bytes
    // So max_file_size = max_memory_bytes / kExpansionFactor
    size_t max_file_size = max_memory_bytes / GCodeMemoryLimits::kExpansionFactor;

    spdlog::trace("[GCodeStreaming] Threshold calculation: {}MB RAM × {}% / {}x = {}MB max file",
                  available_memory_kb / 1024, threshold_percent,
                  GCodeMemoryLimits::kExpansionFactor, max_file_size / (1024 * 1024));

    return max_file_size;
}

bool should_use_gcode_streaming(size_t file_size_bytes, const MemoryInfo& mem) {
    // A configured size ceiling only ever adds streaming: it never lets a file
    // the RAM rule below would stream load whole.
    const int threshold_mb = Config::get_instance()->get<int>("/streaming/threshold_mb", 0);
    if (threshold_mb > 0 && file_size_bytes > static_cast<size_t>(threshold_mb) * 1024 * 1024) {
        return true;
    }

    // If we can't read memory info, be conservative
    if (mem.available_kb == 0) {
        spdlog::warn("[GCodeStreaming] Cannot read memory info, defaulting to streaming "
                     "for files > 2MB");
        return file_size_bytes > (2 * 1024 * 1024);
    }

    // Calculate threshold based on available memory; low-RAM boards take a
    // smaller share of it (kLowRamThresholdPercent)
    int threshold_pct = effective_threshold_percent(mem);
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
        int threshold_pct = effective_threshold_percent(mem);

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
