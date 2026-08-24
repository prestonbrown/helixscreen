// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "gcode_layer_cache.h"

#include "system/crash_handler.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {
namespace gcode {

GCodeLayerCache::GCodeLayerCache(size_t memory_budget_bytes) : memory_budget_(memory_budget_bytes) {
    spdlog::debug("[LayerCache] Created with {:.1f}MB budget",
                  static_cast<double>(memory_budget_) / (1024 * 1024));
}

size_t GCodeLayerCache::estimate_memory(const std::vector<ToolpathSegment>& segments) {
    // Base cost: vector overhead + segment data
    // Each ToolpathSegment is exactly 40 bytes (static_assert in gcode_parser.h):
    // - glm::vec3 start: 12 bytes
    // - glm::vec3 end: 12 bytes
    // - float extrusion_amount: 4 bytes
    // - float width: 4 bytes
    // - int16_t object_name_index: 2 bytes (interned — no string heap)
    // - uint16_t layer_index: 2 bytes
    // - int8_t tool_index, bool is_extrusion, FeatureType, padding: 4 bytes

    size_t base_cost = sizeof(std::vector<ToolpathSegment>) + 64; // Vector overhead + some slack

    size_t per_segment = BYTES_PER_SEGMENT;

    // Charge capacity, not size: a vector grown by repeated push_back (multi-sub-layer
    // parses) can hold up to 2x the live element count.
    return base_cost + (segments.capacity() * per_segment);
}

GCodeLayerCache::CacheResult
GCodeLayerCache::get_or_load(size_t layer_index,
                             const std::function<std::vector<ToolpathSegment>(size_t)>& loader) {
    // Periodically check memory pressure and adapt budget (rate-limited internally)
    check_memory_pressure();

    std::unique_lock<std::mutex> lock(mutex_);

    // Resolve to one of three states: resident (return it), being loaded by
    // someone else (wait for them), or ours to load (fall through).
    for (;;) {
        auto it = cache_.find(layer_index);
        if (it != cache_.end()) {
            hit_count_++;
            touch(layer_index);
            spdlog::trace("[LayerCache] Hit layer {} ({} segments)", layer_index,
                          it->second.segments->size());
            // Return shared_ptr - data stays alive even if entry is evicted
            return CacheResult{it->second.segments, true, false};
        }

        if (loading_.find(layer_index) == loading_.end()) {
            break; // nobody else is on it - we own this load
        }

        // Someone else is already parsing this exact layer. Wait on them rather
        // than duplicating the work; wait() drops the lock, so every OTHER layer
        // stays reachable throughout. Re-check on wake: the load may have
        // landed, failed, or been evicted again.
        load_done_.wait(lock);
    }

    // Cache miss - need to load
    miss_count_++;
    spdlog::debug("[LayerCache] Miss layer {}, loading...", layer_index);

    // Load the layer data WITHOUT the cache lock held.
    //
    // The loader does a file seek and a G-code parse - tens of milliseconds on
    // embedded flash. Holding mutex_ across it serialised every other thread
    // against unrelated layers: the LVGL main thread reaches this from render()
    // and from the touch hit-test, so a background ghost build (which walks all
    // layers with a 1ms yield) made taps take seconds on a 2-core K2. Marking
    // the layer in-flight is what keeps the unlocked window safe.
    loading_.insert(layer_index);
    lock.unlock();

    std::vector<ToolpathSegment> segments;
    bool load_failed = false;
    try {
        segments = loader(layer_index);
    } catch (const std::exception& e) {
        spdlog::error("[LayerCache] Failed to load layer {}: {}", layer_index, e.what());
        load_failed = true;
    }

    lock.lock();

    // Release waiters on the way out of every path below.
    struct LoadGuard {
        GCodeLayerCache* self;
        size_t index;
        ~LoadGuard() {
            self->loading_.erase(index);
            self->load_done_.notify_all();
        }
    } load_guard{this, layer_index};

    if (load_failed) {
        return CacheResult{nullptr, false, true};
    }

    // Another thread may have inserted this layer while we were unlocked (e.g.
    // via insert() from the index builder). Prefer the resident copy over
    // double-charging the budget for identical data.
    if (auto existing = cache_.find(layer_index); existing != cache_.end()) {
        touch(layer_index);
        return CacheResult{existing->second.segments, true, false};
    }

    if (segments.empty()) {
        spdlog::debug("[LayerCache] Layer {} loaded but empty", layer_index);
        // Still cache empty layers to avoid repeated loads
    }

    // Calculate memory needed
    size_t needed = estimate_memory(segments);

    // Check if this single layer exceeds budget
    if (needed > memory_budget_) {
        spdlog::warn("[LayerCache] Layer {} ({} segments, {} bytes) exceeds budget ({} bytes)",
                     layer_index, segments.size(), needed, memory_budget_);
        // Return the data but don't cache it
        // Note: This is a bit tricky - we return a temporary that will be destroyed
        // The caller should check load_failed and handle accordingly
        return CacheResult{nullptr, false, true};
    }

    // Make room if needed
    evict_for_space(needed);

    // Insert into cache - use shared_ptr for thread-safe lifetime management
    CacheEntry entry;
    entry.segments = std::make_shared<std::vector<ToolpathSegment>>(std::move(segments));
    entry.memory_bytes = needed;

    auto [inserted_it, success] = cache_.emplace(layer_index, std::move(entry));
    if (!success) {
        spdlog::error("[LayerCache] Failed to insert layer {} into cache", layer_index);
        return CacheResult{nullptr, false, true};
    }

    // Add to LRU tracking
    lru_order_.push_front(layer_index);
    lru_map_[layer_index] = lru_order_.begin();
    current_memory_ += needed;

    spdlog::debug("[LayerCache] Cached layer {} ({} segments, {} bytes, total {:.1f}MB)",
                  layer_index, inserted_it->second.segments->size(), needed,
                  static_cast<double>(current_memory_) / (1024 * 1024));

    // Return shared_ptr - data stays alive even if entry is evicted
    return CacheResult{inserted_it->second.segments, false, false};
}

std::shared_ptr<const std::vector<ToolpathSegment>> GCodeLayerCache::try_get(size_t layer_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(layer_index);
    if (it == cache_.end()) {
        miss_count_++;
        return nullptr;
    }

    hit_count_++;
    touch(layer_index);
    return it->second.segments;
}

bool GCodeLayerCache::is_cached(size_t layer_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.find(layer_index) != cache_.end();
}

void GCodeLayerCache::prefetch(size_t center_layer, size_t radius,
                               const std::function<std::vector<ToolpathSegment>(size_t)>& loader,
                               size_t max_layer) {
    // Calculate range
    size_t start = (center_layer > radius) ? (center_layer - radius) : 0;
    size_t end = std::min(center_layer + radius, max_layer);

    spdlog::debug("[LayerCache] Prefetching layers [{}, {}] around {}", start, end, center_layer);

    // Load layers - get_or_load handles "already cached" internally, avoiding TOCTOU race
    for (size_t i = start; i <= end; ++i) {
        get_or_load(i, loader);
    }
}

bool GCodeLayerCache::insert(size_t layer_index, std::vector<ToolpathSegment>&& segments) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already cached
    if (cache_.find(layer_index) != cache_.end()) {
        // Already cached, just touch it
        touch(layer_index);
        return true;
    }

    size_t needed = estimate_memory(segments);

    // Check if it would fit even with empty cache
    if (needed > memory_budget_) {
        spdlog::warn("[LayerCache] Layer {} ({} bytes) exceeds budget, not caching", layer_index,
                     needed);
        return false;
    }

    // Make room
    evict_for_space(needed);

    // Insert - use shared_ptr for thread-safe lifetime management
    CacheEntry entry;
    entry.segments = std::make_shared<std::vector<ToolpathSegment>>(std::move(segments));
    entry.memory_bytes = needed;

    cache_.emplace(layer_index, std::move(entry));
    lru_order_.push_front(layer_index);
    lru_map_[layer_index] = lru_order_.begin();
    current_memory_ += needed;

    return true;
}

void GCodeLayerCache::clear() {
    std::unique_lock<std::mutex> lock(mutex_);

    // Drain in-flight loads before wiping.
    //
    // Callers tear down the data source immediately after clearing (see
    // GCodeStreamingController::close(), which resets data_source_ next), and a
    // loader closure captures that source. While get_or_load() held mutex_
    // across the load this barrier was implicit; now that the load runs
    // unlocked, waiting here is what keeps a loader from outliving the file it
    // is reading.
    load_done_.wait(lock, [this] { return loading_.empty(); });

    cache_.clear();
    lru_order_.clear();
    lru_map_.clear();
    current_memory_ = 0;

    spdlog::debug("[LayerCache] Cleared");
}

bool GCodeLayerCache::evict(size_t layer_index) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(layer_index);
    if (it == cache_.end()) {
        return false;
    }

    // Remove from LRU tracking
    auto lru_it = lru_map_.find(layer_index);
    if (lru_it != lru_map_.end()) {
        lru_order_.erase(lru_it->second);
        lru_map_.erase(lru_it);
    }

    // Update memory tracking (safe subtract prevents underflow)
    subtract_memory(it->second.memory_bytes);

    // Remove from cache
    cache_.erase(it);

    spdlog::debug("[LayerCache] Evicted layer {}", layer_index);
    return true;
}

size_t GCodeLayerCache::memory_usage_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_memory_;
}

size_t GCodeLayerCache::cached_layer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

std::pair<size_t, size_t> GCodeLayerCache::hit_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {hit_count_, miss_count_};
}

float GCodeLayerCache::hit_rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = hit_count_ + miss_count_;
    if (total == 0) {
        return 0.0f;
    }
    return static_cast<float>(hit_count_) / static_cast<float>(total);
}

void GCodeLayerCache::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    hit_count_ = 0;
    miss_count_ = 0;
}

void GCodeLayerCache::set_memory_budget(size_t budget_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    memory_budget_ = budget_bytes;

    // Evict if we're now over budget
    while (current_memory_ > memory_budget_ && !lru_order_.empty()) {
        size_t oldest = lru_order_.back();
        lru_order_.pop_back();
        lru_map_.erase(oldest);

        auto it = cache_.find(oldest);
        if (it != cache_.end()) {
            subtract_memory(it->second.memory_bytes);
            cache_.erase(it);
            spdlog::debug("[LayerCache] Budget reduced, evicted layer {}", oldest);
        }
    }
}

void GCodeLayerCache::evict_for_space(size_t required_bytes) {
    // Already holding lock when called

    while (current_memory_ + required_bytes > memory_budget_ && !lru_order_.empty()) {
        // Evict oldest (back of list)
        size_t oldest = lru_order_.back();
        lru_order_.pop_back();
        lru_map_.erase(oldest);

        auto it = cache_.find(oldest);
        if (it != cache_.end()) {
            size_t freed = it->second.memory_bytes;
            cache_.erase(it);
            subtract_memory(freed);
            spdlog::debug("[LayerCache] Evicted layer {} to make room ({} bytes freed)", oldest,
                          freed);
        }
    }
}

void GCodeLayerCache::touch(size_t layer_index) {
    // Already holding lock when called

    auto lru_it = lru_map_.find(layer_index);
    if (lru_it != lru_map_.end()) {
        // Move to front of LRU list — splice is noexcept and preserves
        // the iterator in lru_map_ (no erase+reinsert needed)
        lru_order_.splice(lru_order_.begin(), lru_order_, lru_it->second);
    }
}

void GCodeLayerCache::subtract_memory(size_t bytes) {
    // Already holding lock when called
    // Defensive: prevent underflow in memory tracking
    if (bytes <= current_memory_) {
        current_memory_ -= bytes;
    } else {
        spdlog::error("[LayerCache] Memory tracking underflow! tracked={}, subtracting={}",
                      current_memory_, bytes);
        current_memory_ = 0;
    }
}

// =============================================================================
// Adaptive Memory Management
// =============================================================================

void GCodeLayerCache::set_adaptive_mode(bool enabled, int target_percent, size_t min_budget_bytes,
                                        size_t max_budget_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    adaptive_enabled_ = enabled;
    adaptive_target_percent_ = std::clamp(target_percent, 1, 50);
    adaptive_min_budget_ = min_budget_bytes;
    adaptive_max_budget_ = max_budget_bytes;
    last_pressure_check_ = std::chrono::steady_clock::now();

    if (enabled) {
        spdlog::info("[LayerCache] Adaptive mode enabled: target {}% of available RAM, "
                     "budget range [{:.1f}MB, {:.1f}MB]",
                     adaptive_target_percent_,
                     static_cast<double>(adaptive_min_budget_) / (1024 * 1024),
                     static_cast<double>(adaptive_max_budget_) / (1024 * 1024));

        // Immediately check and adjust
        // (unlock to avoid recursive lock in check_memory_pressure)
    }
}

bool GCodeLayerCache::check_memory_pressure() {
    auto now = std::chrono::steady_clock::now();

    // Phase 1: decide whether this call owns the check. Stamping the timestamp
    // here claims it, so concurrent callers bail out instead of piling up on the
    // procfs read below.
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!adaptive_enabled_) {
            return false;
        }

        // Rate limit checks to avoid overhead
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pressure_check_);
        if (elapsed.count() < PRESSURE_CHECK_INTERVAL_MS) {
            return false;
        }

        last_pressure_check_ = now;
    }

    // Phase 2: query system memory WITHOUT the lock. get_system_memory_info()
    // opens and parses /proc/meminfo - filesystem work, and this is the first
    // statement of get_or_load(), so the mutex it would otherwise hold is the
    // one on the renderer's draw and touch-hit-test paths.
    MemoryInfo mem = get_system_memory_info();

    // Phase 3: apply the result.
    std::lock_guard<std::mutex> lock(mutex_);

    size_t new_budget = calculate_adaptive_budget(mem);

    // Only adjust if meaningful change (>10% difference)
    float change_ratio = static_cast<float>(new_budget) / static_cast<float>(memory_budget_);
    if (change_ratio > 0.9f && change_ratio < 1.1f) {
        return false;
    }

    size_t old_budget = memory_budget_;
    memory_budget_ = new_budget;

    // If budget shrank, evict excess
    while (current_memory_ > memory_budget_ && !lru_order_.empty()) {
        size_t oldest = lru_order_.back();
        lru_order_.pop_back();
        lru_map_.erase(oldest);

        auto it = cache_.find(oldest);
        if (it != cache_.end()) {
            subtract_memory(it->second.memory_bytes);
            cache_.erase(it);
        }
    }

    spdlog::info("[LayerCache] Adaptive adjustment: {:.1f}MB -> {:.1f}MB "
                 "(available RAM: {:.0f}MB, {} layers cached)",
                 static_cast<double>(old_budget) / (1024 * 1024),
                 static_cast<double>(new_budget) / (1024 * 1024),
                 static_cast<double>(mem.available_kb) / 1024, cache_.size());

    return true;
}

void GCodeLayerCache::respond_to_pressure(float emergency_factor) {
    std::lock_guard<std::mutex> lock(mutex_);

    emergency_factor = std::clamp(emergency_factor, 0.1f, 1.0f);
    size_t emergency_budget = static_cast<size_t>(memory_budget_ * emergency_factor);

    // Only apply min_budget constraint if adaptive mode is enabled
    if (adaptive_enabled_) {
        emergency_budget = std::max(emergency_budget, adaptive_min_budget_);
    }

    spdlog::warn("[LayerCache] Emergency pressure response: reducing to {:.1f}MB",
                 static_cast<double>(emergency_budget) / (1024 * 1024));

    // Evict until under emergency budget
    size_t freed_total = 0;
    size_t evicted_count = 0;
    while (current_memory_ > emergency_budget && !lru_order_.empty()) {
        size_t oldest = lru_order_.back();
        lru_order_.pop_back();
        lru_map_.erase(oldest);

        auto it = cache_.find(oldest);
        if (it != cache_.end()) {
            size_t freed = it->second.memory_bytes;
            cache_.erase(it);
            subtract_memory(freed);
            freed_total += freed;
            ++evicted_count;
            spdlog::debug("[LayerCache] Emergency evict layer {} ({} bytes)", oldest, freed);
        }
    }
    if (evicted_count > 0) {
        // Correlate eviction with subsequent dangling-pointer SEGVs (#851 family).
        // Detail is bytes freed; layer count goes in the subject.
        char layers[24];
        std::snprintf(layers, sizeof(layers), "%luL", static_cast<unsigned long>(evicted_count));
        crash_handler::breadcrumb::note("layercache_evict", layers, static_cast<long>(freed_total));
    }
}

size_t GCodeLayerCache::calculate_adaptive_budget(const MemoryInfo& mem) const {
    // Already holding lock when called

    if (mem.available_kb == 0) {
        // Can't get memory info, use conservative default
        return adaptive_min_budget_;
    }

    // Calculate budget as percentage of available RAM
    size_t available_bytes = mem.available_kb * 1024;
    size_t target_budget = (available_bytes * adaptive_target_percent_) / 100;

    // Clamp to configured range
    target_budget = std::max(target_budget, adaptive_min_budget_);
    target_budget = std::min(target_budget, adaptive_max_budget_);

    // When available memory is low, be more aggressive
    if (mem.is_low_memory()) {
        // If < 64MB available, use at most 10% for cache
        target_budget = std::min(target_budget, available_bytes / 10);
        target_budget = std::max(target_budget, adaptive_min_budget_);
    }

    return target_budget;
}

int64_t GCodeLayerCache::ms_since_last_pressure_check() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - last_pressure_check_)
        .count();
}

} // namespace gcode
} // namespace helix

#endif // HELIX_HAS_GCODE_VIEWER
