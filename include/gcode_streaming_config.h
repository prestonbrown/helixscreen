// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>

namespace helix {

/**
 * @brief G-code streaming mode configuration
 *
 * Determines whether to use streaming (layer-by-layer) or full-load mode
 * for G-code visualization. Configuration hierarchy:
 *
 * 1. Environment variable HELIX_GCODE_STREAMING (highest priority)
 * 2. Config file gcode_viewer.streaming_mode
 * 3. Auto-detection based on available memory (lowest priority)
 */
enum class GCodeStreamingMode {
    AUTO, ///< Calculate based on available memory
    ON,   ///< Always use streaming mode
    OFF   ///< Always use full load mode
};

/**
 * @brief Get current streaming mode from environment and config
 *
 * Checks in order:
 * 1. HELIX_GCODE_STREAMING env var ("on", "off", "auto")
 * 2. Config file gcode_viewer.streaming_mode
 * 3. Returns AUTO as default
 *
 * @return Configured streaming mode
 */
GCodeStreamingMode get_gcode_streaming_mode();

/**
 * @brief Get streaming threshold percentage from config
 *
 * @return Threshold percentage (1-90, default 40)
 */
int get_streaming_threshold_percent();

/**
 * @brief Calculate max file size before streaming kicks in
 *
 * When streaming_mode is AUTO, this calculates the threshold based on:
 * - Available system memory
 * - Configured threshold percentage
 * - G-code expansion factor (file size → in-memory size)
 *
 * @param available_memory_kb Available system memory in KB
 * @param threshold_percent Percentage of RAM to use (1-90)
 * @return Max file size in bytes before streaming is used
 */
size_t calculate_streaming_threshold(size_t available_memory_kb, int threshold_percent);

/**
 * @brief Determine if streaming should be used for a given file
 *
 * Applies the full decision logic:
 * 1. If mode is ON → always stream
 * 2. If mode is OFF → never stream
 * 3. If mode is AUTO → compare file size to calculated threshold
 *
 * @param file_size_bytes Size of the G-code file
 * @return true if streaming should be used
 */
bool should_use_gcode_streaming(size_t file_size_bytes);

struct MemoryInfo; // forward declaration

/**
 * @brief Testable overload that accepts explicit memory info
 *
 * Used by unit tests to inject specific memory configurations without
 * depending on the real system memory state.
 *
 * @param file_size_bytes Size of the G-code file
 * @param mem System memory information
 * @return true if streaming should be used
 */
bool should_use_gcode_streaming(size_t file_size_bytes, const MemoryInfo& mem);

/**
 * @brief Get human-readable description of current streaming config
 *
 * Useful for debug logging.
 *
 * @return Description like "streaming=auto (threshold 1.2MB based on 47MB RAM)"
 */
std::string get_streaming_config_description();

/**
 * @brief Final say on whether a viewer streams, given what the screen asked for.
 *
 * A screen may ask to opt out of streaming when it would rather have the
 * full-load path feeding a 3D render (PrintSelectDetailView does). That request
 * is only honourable when the build actually HAS a 3D renderer: without one the
 * viewer can only draw 2D, and the full-load path it would fall back to parses
 * the whole file into RAM with no budget of its own.
 *
 * On a K2 Plus (488 MB, built with ENABLE_GLES_3D=no) that combination opened a
 * 130 MB gcode with "streaming mode: OFF", grew helix-screen to 387 MB RSS and
 * got it OOM-killed by the kernel; the same file on the same device renders
 * fine streaming. The size gate in front of it cannot catch this either - it
 * prices the STREAMING renderer, which is not the path that ends up running.
 *
 * Kept as a pure function of its three inputs so both answers are testable in a
 * binary that is itself compiled with 3D enabled.
 *
 * @param screen_opted_out    The screen called ui_gcode_viewer_disable_streaming().
 * @param build_has_3d        This build can render 3D (ENABLE_3D_RENDERER).
 * @param streaming_for_size  should_use_gcode_streaming() for this file.
 * @return true if the viewer should stream.
 */
[[nodiscard]] inline bool gcode_viewer_should_stream(bool screen_opted_out, bool build_has_3d,
                                                     bool streaming_for_size) {
    if (screen_opted_out && build_has_3d) {
        return false;
    }
    return streaming_for_size;
}

} // namespace helix
