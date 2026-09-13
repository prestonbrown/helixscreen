// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_parser.h"
#include "memory_utils.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace helix {
namespace gcode {

// Forward declaration
class GCodeDataSource;

/**
 * @brief Memory-budgeted LRU cache for G-code layers
 *
 * Stores parsed segment data for on-demand layer access. When the memory
 * budget is exceeded, least-recently-used layers are evicted. This enables
 * viewing large G-code files (10MB+) on memory-constrained devices.
 *
 * Thread-safe for concurrent access from UI and background loading threads.
 *
 * Usage:
 * @code
 *   GCodeLayerCache cache(8 * 1024 * 1024); // 8MB budget
 *   auto layer_data = cache.get_or_load(50, data_source, index);
 *   if (layer_data) {
 *       // Render segments...
 *   }
 * @endcode
 *
 * Memory usage: 40 bytes per segment (BYTES_PER_SEGMENT) + cache bookkeeping
 */
class GCodeLayerCache {
  public:
    /// Memory budget for constrained devices like AD5M (2MB) - <256MB total RAM
    static constexpr size_t DEFAULT_BUDGET_CONSTRAINED = 2 * 1024 * 1024;

    /// Memory budget for normal devices (16MB) - 256MB-512MB total RAM
    static constexpr size_t DEFAULT_BUDGET_NORMAL = 16 * 1024 * 1024;

    /// Memory budget for well-equipped devices (32MB) - >512MB total RAM
    static constexpr size_t DEFAULT_BUDGET_GOOD = 32 * 1024 * 1024;

    /// Approximate bytes per segment (for estimation)
    static constexpr size_t BYTES_PER_SEGMENT = 40;

    /// A RAM tier's cache budget, with the label logs use for it.
    struct BudgetTier {
        size_t budget_bytes;
        const char* name;
    };

    /**
     * @brief The budget tier a device with @p total_ram_kb of total RAM gets.
     *
     * GCodeStreamingController sizes its cache from this, and the 2D streaming
     * memory gate (is_gcode_2d_streaming_safe_impl) prices those same bytes
     * before authorizing a render, so both read one rule. A total of 0 means
     * the system total could not be read and lands in the smallest tier.
     */
    static BudgetTier budget_tier_for_total_ram_kb(size_t total_ram_kb) {
        MemoryInfo mem;
        mem.total_kb = total_ram_kb;
        if (mem.is_constrained_device())
            return {DEFAULT_BUDGET_CONSTRAINED, "constrained"};
        if (mem.is_normal_device())
            return {DEFAULT_BUDGET_NORMAL, "normal"};
        return {DEFAULT_BUDGET_GOOD, "good"};
    }

    /**
     * @brief Construct cache with memory budget
     * @param memory_budget_bytes Maximum memory usage in bytes
     */
    explicit GCodeLayerCache(size_t memory_budget_bytes = DEFAULT_BUDGET_NORMAL);

    ~GCodeLayerCache() = default;

    // Non-copyable, non-moveable (mutex prevents move)
    GCodeLayerCache(const GCodeLayerCache&) = delete;
    GCodeLayerCache& operator=(const GCodeLayerCache&) = delete;
    GCodeLayerCache(GCodeLayerCache&&) = delete;
    GCodeLayerCache& operator=(GCodeLayerCache&&) = delete;

    /**
     * @brief Result of a cache lookup
     *
     * Uses shared_ptr to ensure the segment data stays valid even if the cache
     * entry is evicted while the caller is still using the data. This is critical
     * for thread safety when the background ghost render thread iterates over
     * segments while other threads may trigger cache eviction.
     */
    struct CacheResult {
        std::shared_ptr<const std::vector<ToolpathSegment>>
            segments;            ///< Shared pointer to segments (thread-safe lifetime)
        bool was_hit{false};     ///< True if found in cache
        bool load_failed{false}; ///< True if load attempted but failed
    };

    /**
     * @brief Get layer data, loading from source if not cached
     *
     * If the layer is cached, returns immediately and updates LRU order.
     * If not cached, loads from data source, caches result, and returns.
     * May evict other layers to stay within budget.
     *
     * @param layer_index Zero-based layer index
     * @param loader Function to load layer data: (layer_index) -> vector<ToolpathSegment>
     * @return CacheResult with pointer to segments (valid until next cache operation)
     *
     * @note The returned pointer is only valid until the next cache-modifying operation.
     *       Copy the data if you need to keep it longer.
     */
    CacheResult get_or_load(size_t layer_index,
                            const std::function<std::vector<ToolpathSegment>(size_t)>& loader);

    /**
     * @brief Get a layer only if it is already resident — never loads, never blocks on I/O
     *
     * The non-blocking counterpart to get_or_load(). Takes the cache lock only
     * long enough for a map lookup and an LRU touch, and returns nullptr on a
     * miss rather than reaching for the data source.
     *
     * Use this from any main-thread path where a stall is worse than a miss —
     * above all the touch hit-test, which picks against the layer already on
     * screen and so is served by the cache in practice. A miss there costs one
     * unrecognised tap; a load costs a frozen UI (bundle C2CP6ZAW).
     *
     * @param layer_index Zero-based layer index
     * @return Segments if resident, nullptr on a miss
     */
    std::shared_ptr<const std::vector<ToolpathSegment>> try_get(size_t layer_index);

    /**
     * @brief Check if a layer is currently cached
     * @param layer_index Zero-based layer index
     * @return true if layer is in cache
     */
    bool is_cached(size_t layer_index) const;

    /**
     * @brief Prefetch layers around a center layer
     *
     * Loads layers in range [center - radius, center + radius] in the background.
     * Useful for preloading layers the user is likely to view next.
     *
     * @param center_layer Center layer index
     * @param radius Number of layers on each side to prefetch
     * @param loader Function to load layer data
     * @param max_layer Maximum valid layer index (to avoid out-of-bounds)
     */
    void prefetch(size_t center_layer, size_t radius,
                  const std::function<std::vector<ToolpathSegment>(size_t)>& loader,
                  size_t max_layer);

    /**
     * @brief Insert pre-loaded layer data into cache
     *
     * Used when layer data was loaded externally (e.g., during index building).
     *
     * @param layer_index Layer index
     * @param segments Segment data to cache (moved into cache)
     * @return true if inserted, false if would exceed budget even after eviction
     */
    bool insert(size_t layer_index, std::vector<ToolpathSegment>&& segments);

    /**
     * @brief Clear all cached layers
     */
    void clear();

    /**
     * @brief Evict a specific layer from cache
     * @param layer_index Layer to evict
     * @return true if layer was evicted, false if not in cache
     */
    bool evict(size_t layer_index);

    // Statistics

    /**
     * @brief Get current memory usage
     * @return Bytes used by cached segments
     */
    size_t memory_usage_bytes() const;

    /**
     * @brief Get memory budget
     * @return Maximum bytes allowed
     */
    size_t memory_budget_bytes() const {
        return memory_budget_;
    }

    /**
     * @brief Get number of cached layers
     * @return Layer count
     */
    size_t cached_layer_count() const;

    /**
     * @brief Get cache hit statistics
     * @return Pair of (hits, misses)
     */
    std::pair<size_t, size_t> hit_stats() const;

    /**
     * @brief Get cache hit rate
     * @return Hit rate as fraction [0.0, 1.0]
     */
    float hit_rate() const;

    /**
     * @brief Reset hit/miss counters
     */
    void reset_stats();

    /**
     * @brief Set new memory budget
     *
     * If new budget is smaller, may trigger evictions.
     *
     * @param budget_bytes New budget in bytes
     */
    void set_memory_budget(size_t budget_bytes);

    // =========================================================================
    // Adaptive Memory Management
    // =========================================================================

    /**
     * @brief Enable/disable adaptive memory management
     *
     * When enabled, the cache periodically checks system memory pressure
     * and adjusts its budget accordingly. This is crucial for embedded
     * devices where memory availability can fluctuate.
     *
     * @param enabled true to enable adaptive mode
     * @param target_percent Target percentage of available RAM to use (1-50)
     * @param min_budget_bytes Minimum budget even under pressure
     * @param max_budget_bytes Maximum budget even when RAM is plentiful
     */
    void set_adaptive_mode(bool enabled, int target_percent = 15,
                           size_t min_budget_bytes = 1 * 1024 * 1024,
                           size_t max_budget_bytes = DEFAULT_BUDGET_NORMAL);

    /**
     * @brief Check memory pressure and adjust budget if adaptive mode enabled
     *
     * Call this periodically (e.g., every few seconds or before heavy operations).
     * In adaptive mode, this reads system memory info and may shrink/grow the cache.
     *
     * @return true if budget was adjusted
     */
    bool check_memory_pressure();

    /**
     * @brief Force immediate memory pressure response
     *
     * Call this when you know memory is tight (e.g., before loading a new file).
     * Evicts entries to get under a reduced budget based on current system memory.
     *
     * @param emergency_factor Factor to reduce budget by (0.5 = halve the budget)
     */
    void respond_to_pressure(float emergency_factor = 0.5f);

    /**
     * @brief Calculate appropriate budget based on current system memory
     *
     * @param mem Current system memory info
     * @return Recommended budget in bytes
     */
    size_t calculate_adaptive_budget(const MemoryInfo& mem) const;

    /**
     * @brief Check if adaptive mode is enabled
     * @return true if adaptive mode is active
     */
    bool is_adaptive_mode() const {
        return adaptive_enabled_;
    }

    /**
     * @brief Get time since last memory pressure check
     * @return Milliseconds since last check
     */
    int64_t ms_since_last_pressure_check() const;

  private:
    /**
     * @brief Entry in the cache
     *
     * Uses shared_ptr for segments to allow safe concurrent access. When a caller
     * gets segments via get_or_load(), they receive a shared_ptr that keeps the
     * data alive even if this entry is evicted from the cache.
     */
    struct CacheEntry {
        std::shared_ptr<std::vector<ToolpathSegment>> segments;
        size_t memory_bytes{0}; ///< Estimated memory usage
    };

    /**
     * @brief Estimate memory usage for segment vector
     * @param segments Vector of segments
     * @return Estimated bytes
     */
    static size_t estimate_memory(const std::vector<ToolpathSegment>& segments);

    /**
     * @brief Evict oldest entries until under budget
     * @param required_bytes Additional bytes needed
     */
    void evict_for_space(size_t required_bytes);

    /**
     * @brief Move layer to front of LRU list (mark as recently used)
     * @param layer_index Layer to mark
     */
    void touch(size_t layer_index);

    /**
     * @brief Safely subtract from memory tracking (prevents underflow)
     * @param bytes Bytes to subtract
     */
    void subtract_memory(size_t bytes);

    // Cache storage
    std::unordered_map<size_t, CacheEntry> cache_;
    std::list<size_t> lru_order_; ///< Front = most recent, back = least recent
    std::unordered_map<size_t, std::list<size_t>::iterator>
        lru_map_; ///< Layer -> iterator into lru_order_

    // Configuration
    size_t memory_budget_;
    size_t current_memory_{0};

    // Statistics
    mutable size_t hit_count_{0};
    mutable size_t miss_count_{0};

    // Thread safety
    mutable std::mutex mutex_;

    /// Layers with a load in flight right now. get_or_load() drops mutex_ across
    /// the loader call so unrelated layers stay reachable; this set is what stops
    /// concurrent requests for the SAME layer from each doing the parse.
    std::unordered_set<size_t> loading_;

    /// Signalled whenever a layer leaves loading_ (loaded, failed, or superseded).
    std::condition_variable load_done_;

    // Adaptive memory management
    bool adaptive_enabled_{false};
    int adaptive_target_percent_{15};         ///< Target % of available RAM
    size_t adaptive_min_budget_{1024 * 1024}; ///< 1MB minimum
    size_t adaptive_max_budget_{DEFAULT_BUDGET_NORMAL};
    std::chrono::steady_clock::time_point last_pressure_check_;
    static constexpr int64_t PRESSURE_CHECK_INTERVAL_MS = 2000; ///< Check every 2 seconds max
};

} // namespace gcode
} // namespace helix
