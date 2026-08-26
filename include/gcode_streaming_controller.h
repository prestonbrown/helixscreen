// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_data_source.h"
#include "gcode_layer_cache.h"
#include "gcode_layer_index.h"
#include "gcode_parser.h"
#include "gcode_streaming_config.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace helix {
namespace gcode {

// Forward declaration
class GCodeStreamingController;

/**
 * @brief Builds ghost preview progressively in background for streaming mode
 *
 * Since streaming mode loads layers on-demand with LRU eviction, we can't
 * guarantee all layers are cached simultaneously for ghost rendering.
 * This builder iterates through all layers in the background, rendering
 * each to the ghost buffer via a callback.
 *
 * Features:
 * - Progressive rendering: ghost fills in as layers are processed
 * - UI yielding: pauses when user navigates to avoid lag
 * - Cancellation: stops promptly on file change or destruction
 *
 * Usage:
 * @code
 *   BackgroundGhostBuilder builder;
 *   builder.start(&controller, [](size_t layer, const auto& segments) {
 *       // Render segments to ghost buffer using Bresenham
 *   });
 *   // Query progress: builder.get_progress()
 *   // Check completion: builder.is_complete()
 * @endcode
 */
class BackgroundGhostBuilder {
  public:
    /// Callback type for rendering a layer's segments
    using RenderCallback =
        std::function<void(size_t layer_index, const std::vector<ToolpathSegment>& segments)>;

    BackgroundGhostBuilder() = default;
    ~BackgroundGhostBuilder();

    // Non-copyable, non-moveable
    BackgroundGhostBuilder(const BackgroundGhostBuilder&) = delete;
    BackgroundGhostBuilder& operator=(const BackgroundGhostBuilder&) = delete;
    BackgroundGhostBuilder(BackgroundGhostBuilder&&) = delete;
    BackgroundGhostBuilder& operator=(BackgroundGhostBuilder&&) = delete;

    /**
     * @brief Start building ghost preview in background
     *
     * @param controller Streaming controller to load layers from
     * @param render_callback Called for each layer with its segments
     */
    void start(GCodeStreamingController* controller, RenderCallback render_callback);

    /**
     * @brief Cancel the background build
     *
     * Signals the worker thread to stop. Blocks until thread exits.
     */
    void cancel();

    /**
     * @brief Get build progress as fraction
     * @return Progress from 0.0 (starting) to 1.0 (complete)
     */
    float get_progress() const;

    /**
     * @brief Check if build has completed all layers
     * @return true if all layers have been rendered
     */
    bool is_complete() const;

    /**
     * @brief Check if build is currently running
     * @return true if worker thread is active
     */
    bool is_running() const;

    /**
     * @brief Get number of layers rendered so far
     * @return Count of layers processed
     */
    size_t layers_rendered() const;

    /**
     * @brief Get total number of layers to render
     * @return Total layer count
     */
    size_t total_layers() const;

    /**
     * @brief Signal that UI has a pending layer request
     *
     * Call this when user navigates layers. The ghost builder will
     * pause briefly to let the UI load complete first.
     */
    void notify_user_request();

  private:
    void worker_thread();

    GCodeStreamingController* controller_{nullptr};
    RenderCallback render_callback_;

    std::thread worker_;
    std::atomic<size_t> current_layer_{0};
    std::atomic<size_t> total_layers_{0};
    std::atomic<bool> complete_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> running_{false};

    // UI yielding: timestamp of last user navigation (nanoseconds since steady_clock epoch)
    // Stored as int64_t because GCC 7 doesn't support atomic<time_point>
    std::atomic<int64_t> last_user_request_ns_{0};
    static constexpr std::chrono::milliseconds YIELD_DURATION{50};
};

/**
 * @brief Orchestrates streaming G-code loading for memory-constrained devices
 *
 * The streaming controller provides on-demand layer loading by coordinating:
 * - GCodeLayerIndex: Maps layer numbers to file byte offsets (~24 bytes/layer)
 * - GCodeDataSource: Reads byte ranges from file or network
 * - GCodeLayerCache: LRU cache for parsed segment data
 * - GCodeParser: Converts raw G-code bytes to ToolpathSegments
 *
 * This enables viewing 10MB+ G-code files on devices with limited RAM (e.g.,
 * AD5M with 47MB) by loading only the layers currently being viewed.
 *
 * Usage:
 * @code
 *   GCodeStreamingController controller;
 *   if (controller.open_file("model.gcode")) {
 *       // Get segments for layer 42 (loads if not cached)
 *       auto* segments = controller.get_layer_segments(42);
 *       if (segments) {
 *           for (const auto& seg : *segments) {
 *               // Render segment...
 *           }
 *       }
 *   }
 * @endcode
 *
 * Memory usage: Index (~24 bytes × layers) + Cache (configurable budget)
 */
class GCodeStreamingController {
  public:
    /// Default prefetch radius (layers around current view to preload)
    static constexpr size_t DEFAULT_PREFETCH_RADIUS = 3;

    /// Minimum cache budget (1MB)
    static constexpr size_t MIN_CACHE_BUDGET = 1 * 1024 * 1024;

    /**
     * @brief Construct controller with default settings
     *
     * Uses adaptive cache budget based on available system memory.
     */
    GCodeStreamingController();

    /**
     * @brief Construct controller with explicit cache budget
     * @param cache_budget_bytes Maximum memory for layer cache
     */
    explicit GCodeStreamingController(size_t cache_budget_bytes);

    ~GCodeStreamingController();

    // Non-copyable, non-moveable
    GCodeStreamingController(const GCodeStreamingController&) = delete;
    GCodeStreamingController& operator=(const GCodeStreamingController&) = delete;
    GCodeStreamingController(GCodeStreamingController&&) = delete;
    GCodeStreamingController& operator=(GCodeStreamingController&&) = delete;

    // =========================================================================
    // File Operations
    // =========================================================================

    /**
     * @brief Open a local G-code file for streaming
     *
     * Builds the layer index (single-pass scan) and prepares for streaming.
     * For large files, consider using open_file_async() to avoid blocking UI.
     *
     * @param filepath Path to G-code file
     * @return true if successful
     */
    bool open_file(const std::string& filepath);

    /**
     * @brief Open a local file asynchronously (background index building)
     *
     * Returns immediately. Use is_ready() to check when indexing is complete.
     * Progress can be monitored via get_index_progress().
     *
     * @param filepath Path to G-code file
     * @param on_complete Optional callback when indexing completes (bool success)
     */
    void open_file_async(const std::string& filepath,
                         std::function<void(bool)> on_complete = nullptr);

    /**
     * @brief Open from an existing data source
     *
     * Takes ownership of the data source. Useful for custom sources.
     *
     * @param source Data source (ownership transferred)
     * @return true if successful
     */
    bool open_source(std::unique_ptr<GCodeDataSource> source);

    /**
     * @brief Close current file and release resources
     */
    void close();

    /**
     * @brief Check if a file is open and ready
     * @return true if ready for layer access
     */
    bool is_open() const;

    /**
     * @brief Check if async open is still in progress
     * @return true if indexing is running
     */
    bool is_indexing() const;

    /**
     * @brief Get indexing progress (0.0 to 1.0)
     * @return Progress fraction, or 1.0 if complete
     */
    float get_index_progress() const;

    /**
     * @brief Get source file/URL name
     * @return Source identifier
     */
    std::string get_source_name() const;

    // =========================================================================
    // Layer Access
    // =========================================================================

    /**
     * @brief Get parsed segments for a layer
     *
     * Returns cached data if available, otherwise loads from source.
     * Thread-safe but blocks if loading is needed.
     *
     * @param layer_index Zero-based layer index
     * @return Shared pointer to segment vector, or nullptr if layer doesn't exist.
     *         Data stays valid as long as the shared_ptr is held, even if the
     *         cache entry is evicted. This is critical for thread safety.
     *
     * @note For background loading, use request_layer() + is_layer_ready()
     */
    std::shared_ptr<const std::vector<ToolpathSegment>> get_layer_segments(size_t layer_index);

    /**
     * @brief Get a layer only if it is already resident - never loads, never blocks
     *
     * Returns nullptr instead of touching the data source. Intended for
     * main-thread callers that must not stall: the touch hit-test picks against
     * the layer currently on screen, which render() has already faulted in, so a
     * miss here is rare and costs one unrecognised tap rather than a frozen UI.
     *
     * @param layer_index Zero-based layer index
     * @return Segments if resident, nullptr on a miss
     */
    std::shared_ptr<const std::vector<ToolpathSegment>> try_get_layer_segments(size_t layer_index);

    /**
     * @brief Load a layer without warming its neighbours
     *
     * Same blocking load as get_layer_segments(), minus the schedule_prefetch()
     * that call ends with. For a caller that strides through the file rather
     * than moving between adjacent layers - the background ghost pass samples
     * every Nth - the warmed neighbours are never read, so the prefetch worker
     * spends a batch of seek-and-parses per visited layer on data nothing wants.
     * On the two-core boards streaming exists for, that worker is competing for
     * the core the UI thread runs on, so the waste is not merely wasted.
     *
     * @param layer_index Zero-based layer index
     * @return Segments, or nullptr if the layer doesn't exist or failed to load.
     *         Same shared_ptr lifetime contract as get_layer_segments().
     */
    std::shared_ptr<const std::vector<ToolpathSegment>>
    load_layer_segments_no_prefetch(size_t layer_index);

    /**
     * @brief Request a layer to be loaded (non-blocking)
     *
     * Hands the layer to the prefetch worker and returns immediately. Check
     * is_layer_cached() or call get_layer_segments() later to consume it.
     *
     * @param layer_index Zero-based layer index
     */
    void request_layer(size_t layer_index);

    /**
     * @brief Check if a layer is cached and ready
     * @param layer_index Zero-based layer index
     * @return true if layer can be accessed immediately
     */
    bool is_layer_cached(size_t layer_index) const;

    /**
     * @brief Prefetch layers around current view
     *
     * Loads layers in range [center - radius, center + radius].
     * Called automatically by get_layer_segments() but can be
     * called explicitly for more control.
     *
     * @param center_layer Center layer index
     * @param radius Number of layers on each side (default: 3)
     */
    void prefetch_around(size_t center_layer, size_t radius = DEFAULT_PREFETCH_RADIUS);

    /**
     * @brief Block until the prefetch worker has no queued or in-flight work
     *
     * Prefetch is asynchronous, so the cache reaches its post-prefetch state
     * some time after get_layer_segments() / prefetch_around() return. Callers
     * that need a deterministic cache state - chiefly tests - use this as the
     * sync point instead of assuming the fetch already happened.
     *
     * Returns immediately if no worker is running.
     */
    void wait_for_prefetch_idle();

    // =========================================================================
    // Layer Information
    // =========================================================================

    /**
     * @brief Get total number of layers
     * @return Layer count, or 0 if not open
     */
    size_t get_layer_count() const;

    /**
     * @brief Get Z height for a layer
     * @param layer_index Zero-based layer index
     * @return Z height in mm, or 0.0 if invalid
     */
    float get_layer_z(size_t layer_index) const;

    /**
     * @brief Find layer closest to Z height
     * @param z Z coordinate
     * @return Layer index, or -1 if no layers
     */
    int find_layer_at_z(float z) const;

    /**
     * @brief Get layer index statistics
     * @return Statistics from index building
     */
    const LayerIndexStats& get_index_stats() const;

    /**
     * @brief Get file size
     * @return File size in bytes, or 0 if not open
     */
    size_t get_file_size() const;

    // =========================================================================
    // Cache Management
    // =========================================================================

    /**
     * @brief Get cache hit rate
     * @return Hit rate as fraction [0.0, 1.0]
     */
    float get_cache_hit_rate() const;

    /**
     * @brief Get current cache memory usage
     * @return Bytes used
     */
    size_t get_cache_memory_usage() const;

    /**
     * @brief Get cache memory budget
     * @return Maximum bytes allowed
     */
    size_t get_cache_budget() const;

    /**
     * @brief Set new cache budget
     * @param budget_bytes New budget in bytes
     */
    void set_cache_budget(size_t budget_bytes);

    /**
     * @brief Enable adaptive cache budget based on system memory
     * @param enable true to enable
     */
    void set_adaptive_cache(bool enable);

    /**
     * @brief Clear the layer cache
     */
    void clear_cache();

    /**
     * @brief Trigger memory pressure response
     *
     * Call when system memory is low. Reduces cache and evicts entries.
     */
    void respond_to_memory_pressure();

    // =========================================================================
    // Metadata Access
    // =========================================================================

    /**
     * @brief Get header metadata (slicer info, print time, etc.)
     *
     * Only populated after first layer is parsed.
     *
     * @return Pointer to metadata, or nullptr if not available
     */
    const GCodeHeaderMetadata* get_header_metadata() const;

    /**
     * @brief Resolve an object name index to a string
     *
     * Uses the merged string table built from all parsed layers.
     *
     * @param index Object name index from ToolpathSegment
     * @return Resolved object name, or empty string if invalid
     */
    std::string get_object_name(int16_t index) const;

  private:
    /**
     * @brief Load a layer from source and parse to segments
     * @param layer_index Layer to load
     * @return Parsed segments
     */
    std::vector<ToolpathSegment> load_layer(size_t layer_index);

    /**
     * @brief Build index from current data source
     * @return true if successful
     */
    bool build_index();

    /**
     * @brief Create loader function for cache
     * @return Loader lambda
     */
    std::function<std::vector<ToolpathSegment>(size_t)> make_loader();

    /**
     * @brief Hand a prefetch centre to the worker thread and return immediately
     *
     * Only the most recent centre matters, so a pending request is overwritten
     * rather than queued - scrubbing layers cannot build a backlog.
     */
    void schedule_prefetch(size_t center_layer);

    /// Prefetch worker body: drains prefetch requests until stop_prefetch_ is set.
    void prefetch_worker();

    /// Start the prefetch worker if it is not already running.
    void start_prefetch_worker();

    /**
     * @brief Signal and join the prefetch worker. Safe to call when not running.
     * @param permanent Leave the stop flag set so no new worker can start.
     *                  True only from the destructor.
     */
    void stop_prefetch_worker(bool permanent = false);

    // Background prefetch. get_layer_segments() used to run the prefetch loop
    // inline, so a single main-thread call did up to 2*radius+1 seek-and-parses
    // inside an LVGL draw or touch callback. The worker moves that off the main
    // thread; the cache's in-flight tracking dedups against foreground loads.
    std::thread prefetch_thread_;
    std::mutex prefetch_mutex_;
    std::condition_variable prefetch_cv_;
    std::optional<size_t> pending_prefetch_center_;
    bool stop_prefetch_{false};

    /// True while the worker is mid-batch. Together with an empty
    /// pending_prefetch_center_ this is what "idle" means.
    bool prefetch_running_{false};
    std::condition_variable prefetch_idle_cv_;

    // Components (order matters for destruction)
    std::unique_ptr<GCodeDataSource> data_source_;
    GCodeLayerIndex index_;
    GCodeLayerCache cache_;

    // Async indexing
    std::future<bool> index_future_;
    std::atomic<bool> indexing_{false};
    std::atomic<float> index_progress_{0.0f};
    mutable std::mutex callback_mutex_; // Protects index_complete_callback_
    std::function<void(bool)> index_complete_callback_;

    // Metadata (populated lazily)
    mutable std::mutex metadata_mutex_;
    std::unique_ptr<GCodeHeaderMetadata> header_metadata_;
    bool metadata_extracted_{false};

    // State
    std::atomic<bool> is_open_{false};
    size_t prefetch_radius_{DEFAULT_PREFETCH_RADIUS};

    // Merged object name string table (accumulated from all parsed layers)
    mutable std::mutex name_table_mutex_;
    std::vector<std::string> merged_object_name_table_;
    std::unordered_map<std::string, int16_t> merged_object_name_lookup_;

    /**
     * @brief Remap object name indices from a local parse to the merged table
     *
     * After parsing a layer, its local string table indices need remapping
     * to the merged table. This ensures all segments use consistent indices.
     *
     * @param segments Segments to remap (modified in place)
     * @param local_table Local string table from the parser
     */
    void remap_object_name_indices(std::vector<ToolpathSegment>& segments,
                                   const std::vector<std::string>& local_table);

    // Empty stats for when not open
    static const LayerIndexStats empty_stats_;

    void register_memory_responder();
    uint32_t memory_responder_id_{0};
    // prevent use-after-free when MemoryMonitor invokes a copied callback
    // after this object is destroyed (prestonbrown/helixscreen#733)
    std::shared_ptr<bool> prevent_uaf_sentinel_{std::make_shared<bool>(true)};
};

} // namespace gcode
} // namespace helix
