// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace helix {

/**
 * @brief Read current memory stats (cross-platform: Linux + macOS)
 * @param rss_kb Output: Resident Set Size in KB
 * @param hwm_kb Output: High Water Mark (peak RSS) in KB
 * @return true if successful
 */
bool read_memory_stats(int64_t& rss_kb, int64_t& hwm_kb);

/**
 * @brief Read private dirty memory (Linux only)
 * @param private_dirty_kb Output: Private dirty pages in KB
 * @return true if successful (always false on macOS)
 */
bool read_private_dirty(int64_t& private_dirty_kb);

/**
 * @brief Aggregated memory breakdown from /proc/self/smaps_rollup
 *
 * Provides cheap per-process memory categorization without per-VMA cost.
 * Useful for understanding what's consuming memory in production.
 */
struct SmapsRollup {
    int64_t rss_kb = 0;
    int64_t pss_kb = 0;
    int64_t private_dirty_kb = 0;
    int64_t private_clean_kb = 0;
    int64_t shared_clean_kb = 0;
    int64_t shared_dirty_kb = 0;
    int64_t swap_kb = 0;
    int64_t swap_pss_kb = 0;
};

/**
 * @brief Read aggregated smaps data from /proc/self/smaps_rollup (Linux only)
 * @param rollup Output: all memory breakdown fields
 * @return true if successful (always false on macOS)
 */
bool read_smaps_rollup(SmapsRollup& rollup);

// ============================================================================
// System memory info (for resource management decisions)
// ============================================================================

/**
 * @brief System memory information
 */
struct MemoryInfo {
    size_t total_kb = 0;     ///< Total system memory in KB
    size_t available_kb = 0; ///< Available memory in KB (free + buffers/cache)
    size_t free_kb = 0;      ///< Strictly free memory in KB

    // RAM tier thresholds (total system RAM).
    //
    // Boundaries are set BELOW the nominal DRAM size to account for firmware-
    // reserved memory (kernel, CMA, framebuffer): a board with N MB physical
    // reports ~5-15% less as usable total. Without this headroom a 512MB device
    // (AD5X reports ~470MB) would mis-classify as "normal" and inherit the tight
    // 3MB/5min growth threshold meant for 256-512MB boards, spuriously warning
    // on the one-time 3D viewer load at print start.
    static constexpr size_t TIER_CONSTRAINED_KB =
        256 * 1024; ///< < 256MB usable = constrained (AD5M, K1C)
    static constexpr size_t TIER_NORMAL_KB =
        448 * 1024; ///< 256-448MB usable = normal; >=448MB (512MB-class, e.g. AD5X) = good
    static constexpr size_t TIER_FORCE_STREAMING_KB =
        2ULL * 1024 * 1024; ///< <= 2GB = force streaming

    /// Check if available memory is low (< 64MB available right now)
    bool is_low_memory() const {
        return available_kb < 64 * 1024;
    }

    /// Device tier: constrained (< 256MB total) - AD5M, embedded
    bool is_constrained_device() const {
        return total_kb < TIER_CONSTRAINED_KB;
    }

    /// Device tier: normal (256-448MB usable) - Pi 3, low-end Pi 4
    bool is_normal_device() const {
        return total_kb >= TIER_CONSTRAINED_KB && total_kb < TIER_NORMAL_KB;
    }

    /// Device tier: good (>= 448MB usable) - 512MB-class (AD5X), Desktop, Pi 4 2GB+
    bool is_good_device() const {
        return total_kb >= TIER_NORMAL_KB;
    }

    /// Should force G-code streaming mode (total RAM <= 2GB)
    bool should_force_streaming() const {
        return total_kb > 0 && total_kb <= TIER_FORCE_STREAMING_KB;
    }

    /// Get total memory in MB
    size_t total_mb() const {
        return total_kb / 1024;
    }

    /// Get available memory in MB
    size_t available_mb() const {
        return available_kb / 1024;
    }
};

/**
 * @brief Get current system memory information
 *
 * On Linux, reads from /proc/meminfo.
 * On macOS, uses mach APIs (returns zeros for available - use RSS instead).
 *
 * @return MemoryInfo struct with current memory status
 */
MemoryInfo get_system_memory_info();

// Below this total RAM, warn before resonance/input-shaper calibration: klippy
// buffers accelerometer samples and runs an FFT that overcommits tiny hosts
// (e.g. the 128 MB Elegoo CC1) into swap thrash, surfacing as klippy
// "Timer Too Close" errors. Threshold chosen by the maintainer (200 MB).
inline constexpr size_t RESONANCE_LOW_RAM_WARN_MB = 200;

/**
 * @brief Memory thresholds for G-code 3D rendering decisions
 */
struct GCodeMemoryLimits {
    /// Minimum available RAM (KB) to even attempt 3D rendering
    static constexpr size_t MIN_AVAILABLE_KB = 48 * 1024; // 48MB

    /// Maximum G-code file size (bytes) for 3D rendering on constrained devices
    static constexpr size_t MAX_FILE_SIZE_CONSTRAINED = 2 * 1024 * 1024; // 2MB

    /// Maximum G-code file size (bytes) for 3D rendering on normal devices
    static constexpr size_t MAX_FILE_SIZE_NORMAL = 20 * 1024 * 1024; // 20MB

    /// Memory expansion factor (file size -> parsed geometry size estimate)
    static constexpr size_t EXPANSION_FACTOR = 15;
};

/**
 * @brief Check if G-code 3D rendering is safe for a given file
 *
 * Uses heuristics based on file size and available RAM.
 * G-code parsing expands ~10-20x in memory for geometry data.
 *
 * @param file_size_bytes Size of the G-code file in bytes
 * @return true if rendering is likely safe, false if thumbnail-only recommended
 */
bool is_gcode_3d_render_safe(size_t file_size_bytes);

/**
 * @brief Check if G-code 2D streaming rendering is safe for a given file
 *
 * 2D streaming mode uses layer-on-demand loading with LRU cache, so memory
 * requirements are much lower than 3D mode. File is streamed directly to disk
 * (no memory spike during download). Only needs RAM for:
 * - Layer index: ~24 bytes per layer (estimate 1 layer per 500 bytes of G-code)
 * - LRU cache: 1MB fixed budget for parsed layer segments
 * - Ghost preview buffer: display_width * display_height * 4 bytes (ARGB8888)
 * - Safety margin: 3MB for other allocations
 *
 * This is safe for much larger files than is_gcode_3d_render_safe().
 * Reads display dimensions from LVGL at runtime.
 *
 * @param file_size_bytes Size of the G-code file in bytes
 * @return true if 2D streaming rendering is safe, false if thumbnail-only recommended
 */
bool is_gcode_2d_streaming_safe(size_t file_size_bytes);

/**
 * @brief Implementation of 2D streaming memory check (for unit testing)
 *
 * This is the testable implementation that takes all dependencies as parameters.
 * The public is_gcode_2d_streaming_safe() calls this with real values.
 *
 * @param file_size_bytes Size of the G-code file in bytes
 * @param available_kb Available system memory in KB
 * @param display_width Display width in pixels (for ghost buffer calculation)
 * @param display_height Display height in pixels (for ghost buffer calculation)
 * @return true if 2D streaming rendering is safe
 */
bool is_gcode_2d_streaming_safe_impl(size_t file_size_bytes, size_t available_kb, int display_width,
                                     int display_height);

// ============================================================================
// OOM priority
// ============================================================================

/**
 * @brief Parse the HELIX_OOM_SCORE_ADJ contract exported by helix-launcher.sh.
 *
 * @param value Raw environment value; may be nullptr.
 * @param out   Receives the adjustment, clamped to the kernel's [-1000, 1000].
 * @return true when @p out was set, false when the value is absent, empty,
 *         non-numeric, has trailing garbage, or is "0" — the documented
 *         spelling for "leave this process alone".
 */
bool parse_oom_score_adj(const char* value, int& out);

/**
 * @brief Write an oom_score_adj value to @p path (for unit testing).
 * @return true when the write succeeded.
 *
 * Raising the value is unprivileged. Lowering it below the inherited value
 * requires CAP_SYS_RESOURCE and fails as a normal user, which is why the
 * launcher only ever hands us a positive number.
 */
bool write_oom_score_adj(int adj, const char* path);

/**
 * @brief Apply HELIX_OOM_SCORE_ADJ to this process.
 * @return true when an adjustment was written.
 *
 * No-op when the variable is unset or "0". helix-launcher.sh exports it only
 * when Klipper is co-hosted, and exports rather than applies it because
 * oom_score_adj is inherited across fork and preserved across exec: setting it
 * in the launcher would also mark helix-watchdog, and killing the watchdog is
 * what stops helix-screen from coming back.
 */
bool apply_oom_score_adj_from_env();

} // namespace helix
