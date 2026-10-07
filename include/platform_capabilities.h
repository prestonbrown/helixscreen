// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file platform_capabilities.h
 * @brief Hardware capability detection for adaptive UI rendering
 *
 * Detects system hardware metrics (RAM, CPU cores) and classifies the platform
 * into tiers that determine which UI features are available. This enables
 * graceful degradation on resource-constrained embedded systems.
 *
 * Usage:
 * @code
 *   auto caps = PlatformCapabilities::detect();
 *   if (caps.supports_charts) {
 *       // Show frequency response chart
 *   } else {
 *       // Show table fallback
 *   }
 * @endcode
 *
 * @see docs/INPUT_SHAPING_IMPLEMENTATION.md for design rationale
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace helix {

/**
 * @brief Platform capability tiers
 *
 * Tiers determine which UI features are available based on hardware constraints.
 * Classification is based on RAM and CPU core count.
 */
enum class PlatformTier {
    /**
     * EMBEDDED: Very constrained hardware
     * - RAM < 512MB OR single core
     * - No charts, table view only
     * - Examples: AD5M printer, older SBCs
     */
    EMBEDDED,

    /**
     * BASIC: Mid-range embedded hardware
     * - RAM 512-767 MB OR 2-3 cores
     * - Simplified charts (50 points), no animations
     * - Examples: 512 MB boards, 2-3 core boards
     */
    BASIC,

    /**
     * STANDARD: Modern capable hardware
     * - RAM >= 768 MB AND 4+ cores
     * - Full charts (200 points) with animations
     * - Examples: Raspberry Pi 3B (1 GB) and 4/5, desktop
     */
    STANDARD
};

/**
 * @brief CPU information extracted from /proc/cpuinfo
 */
struct CpuInfo {
    int core_count = 0;    ///< Number of logical CPU cores
    float bogomips = 0.0f; ///< BogoMIPS value (approximate speed indicator)
    int cpu_mhz = 0;       ///< CPU frequency in MHz (if available)
    /// Human-readable CPU name. Which key carries it is architecture-specific
    /// ("model name" on x86, "Hardware" on ARM, "cpu model" on MIPS), so the
    /// parser tries each; "" when none of them appears.
    std::string model;
};

/**
 * @brief Platform hardware capabilities
 *
 * Contains detected hardware metrics and derived capability flags.
 * Use detect() for runtime detection or from_metrics() for testing.
 */
struct PlatformCapabilities {
    // ========================================================================
    // Detected hardware metrics
    // ========================================================================

    size_t total_ram_mb = 0; ///< Total RAM in MB from /proc/meminfo
    int cpu_cores = 0;       ///< Number of CPU cores from /proc/cpuinfo
    float bogomips = 0.0f;   ///< BogoMIPS value (speed indicator)

    // ========================================================================
    // Derived capabilities
    // ========================================================================

    PlatformTier tier = PlatformTier::EMBEDDED; ///< Classified tier
    bool supports_charts = false;               ///< Can render LVGL charts
    bool supports_animations = false;           ///< Can render smooth animations
    size_t max_chart_points = 0;                ///< Max data points for charts

    // ========================================================================
    // Tier thresholds (static constexpr for configuration)
    // ========================================================================

    /// RAM below this threshold = EMBEDDED tier
    static constexpr size_t EMBEDDED_RAM_THRESHOLD_MB = 512;

    /// RAM at or above this threshold (with enough cores) = STANDARD tier
    static constexpr size_t STANDARD_RAM_THRESHOLD_MB = 768;

    /// Minimum cores for STANDARD tier (with enough RAM)
    static constexpr int STANDARD_CPU_CORES_MIN = 4;

    /// Max chart points for STANDARD tier
    static constexpr size_t STANDARD_CHART_POINTS = 200;

    /// Max chart points for BASIC tier
    static constexpr size_t BASIC_CHART_POINTS = 50;

    // ========================================================================
    // Factory methods
    // ========================================================================

    /**
     * @brief Detect platform capabilities from system
     *
     * Reads /proc/meminfo and /proc/cpuinfo to detect hardware metrics,
     * then classifies the platform tier.
     *
     * @return PlatformCapabilities with detected values
     * @note On non-Linux systems, returns default (EMBEDDED tier)
     */
    static PlatformCapabilities detect();

    /**
     * @brief Create capabilities from explicit metrics (for testing)
     *
     * @param ram_mb Total RAM in megabytes
     * @param cores Number of CPU cores
     * @param bogomips BogoMIPS value
     * @return PlatformCapabilities with derived capabilities
     */
    static PlatformCapabilities from_metrics(size_t ram_mb, int cores, float bogomips);
};

// ============================================================================
// Parsing functions (exposed for testing)
// ============================================================================

/**
 * @brief Parse total RAM from /proc/meminfo content
 *
 * Extracts the MemTotal value and converts to megabytes.
 *
 * @param content Full content of /proc/meminfo
 * @return Total RAM in MB, or 0 if parsing fails
 */
size_t parse_meminfo_total_mb(const std::string& content);

/**
 * @brief Read one "<Key>: <N> kB" line out of /proc/meminfo content
 *
 * @param content Full content of /proc/meminfo
 * @param key     Field name without the colon, e.g. "MemTotal", "MemAvailable"
 * @return The value in kB, or 0 when the key is absent or unparseable
 */
uint64_t parse_meminfo_kb(const std::string& content, const std::string& key);

/**
 * @brief Parse CPU information from /proc/cpuinfo content
 *
 * Counts processor entries and extracts BogoMIPS/MHz values.
 *
 * @param content Full content of /proc/cpuinfo
 * @return CpuInfo with detected values
 */
CpuInfo parse_cpuinfo(const std::string& content);

/**
 * @brief Whether this tier can afford style effects that cost a redraw per use
 *
 * The one rule for each such effect: a pressed widget's scale-down renders
 * through a TRANSFORM layer on every press, and a scrollbar restyled while
 * scrolling redraws the whole scroller when a drag starts and stops. The
 * limited tiers go without both.
 */
bool full_style_effects_allowed(PlatformTier tier);

/**
 * @brief Convert PlatformTier to string representation
 *
 * @param tier Platform tier
 * @return Lowercase string: "embedded", "basic", or "standard"
 */
std::string platform_tier_to_string(PlatformTier tier);

} // namespace helix
