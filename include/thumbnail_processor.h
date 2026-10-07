// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "thumbnail_write_journal.h"

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>

// Forward declarations
class HThreadPool;

/**
 * @file thumbnail_processor.h
 * @brief Background thumbnail pre-scaling for optimal display performance
 *
 * This class addresses a critical performance issue on embedded displays:
 * LVGL scales large thumbnails (300x300) to display size (~140x150) every frame
 * when using inner_align="contain". On ARM devices without GPU (like AD5M),
 * this causes severe UI lag during scrolling.
 *
 * Solution: Pre-scale thumbnails once at download time, store as raw LVGL binary,
 * display at 1:1 with zero runtime scaling.
 *
 * @see docs/THUMBNAIL_OPTIMIZATION_PLAN.md for full architecture
 */

/// Test-only accessor for the private constructor (tests/unit/). Forward
/// declared so the friend declaration below has something to name.
struct ThumbnailProcessorTestAccess;

namespace helix {

/**
 * @brief Thumbnail use case — determines target dimensions
 */
enum class ThumbnailSize {
    Card,  ///< Small card in file list (120–220px depending on display)
    Detail ///< Larger detail/status view (200–400px depending on display)
};

/**
 * @brief Target dimensions and format for pre-scaled thumbnails
 *
 * Determined by display breakpoint and card layout. Thumbnails are scaled
 * to the smallest size that fully covers the target, preserving aspect ratio.
 */
struct ThumbnailTarget {
    int width = 160;  ///< Target width in pixels
    int height = 160; ///< Target height in pixels

    /**
     * @brief Color format for output — always ARGB8888
     *
     * LVGL handles conversion to display format (e.g., RGB565) at render time.
     */
    uint8_t color_format = 0x10; // LV_COLOR_FORMAT_ARGB8888

    bool operator==(const ThumbnailTarget& other) const {
        return width == other.width && height == other.height && color_format == other.color_format;
    }
};

/**
 * @brief Result of thumbnail processing operation
 */
struct ProcessResult {
    bool success = false;
    std::string output_path; ///< Path to .bin file (empty on failure)
    std::string error;       ///< Error message (empty on success)
    int output_width = 0;    ///< Actual output width (may differ due to aspect ratio)
    int output_height = 0;   ///< Actual output height
};

/**
 * @brief Callback types for async processing
 */
using ProcessSuccessCallback = std::function<void(const std::string& lvbin_path)>;
using ProcessErrorCallback = std::function<void(const std::string& error)>;

/**
 * @brief Background thumbnail processor with thread pool
 *
 * Decodes PNG thumbnails, resizes them to target dimensions, and writes
 * LVGL-native binary files (.bin) for zero-overhead display.
 *
 * Thread-safe: All public methods can be called from any thread.
 *
 * Example usage:
 * @code
 *   auto& processor = ThumbnailProcessor::instance();
 *
 *   // Check if already processed
 *   std::string cached = processor.get_if_processed("/path/thumb.png", target);
 *   if (!cached.empty()) {
 *       lv_image_set_src(img, cached.c_str());
 *       return;
 *   }
 *
 *   // Process in background
 *   processor.process_async(png_data, "/path/thumb.png", target,
 *       [](const std::string& path) { lv_image_set_src(img, path.c_str()); },
 *       [](const std::string& err) { spdlog::warn("Failed: {}", err); });
 * @endcode
 */
class ThumbnailProcessor {
  public:
    /**
     * @brief Get the singleton instance
     *
     * Creates the processor on first call with a 2-thread pool.
     */
    static ThumbnailProcessor& instance();

    // Non-copyable, non-movable (singleton)
    ThumbnailProcessor(const ThumbnailProcessor&) = delete;
    ThumbnailProcessor& operator=(const ThumbnailProcessor&) = delete;

    /**
     * @brief Process PNG data asynchronously
     *
     * Decodes the PNG, resizes to target dimensions, converts to LVGL format,
     * and writes to cache. Callbacks are invoked on worker thread.
     *
     * @param png_data Raw PNG file contents
     * @param source_path Original thumbnail path (used for cache key generation)
     * @param target Target dimensions and format
     * @param on_success Called with path to .bin file on success
     * @param on_error Called with error message on failure
     */
    void process_async(const std::vector<uint8_t>& png_data, const std::string& source_path,
                       const ThumbnailTarget& target, ProcessSuccessCallback on_success,
                       ProcessErrorCallback on_error);

    /**
     * @brief Process a PNG file asynchronously, reading it on the worker thread
     *
     * Same contract as process_async(), but takes a path instead of bytes and
     * performs the read inside the pool task. Prefer this from the main thread:
     * the caller would otherwise slurp the whole PNG synchronously only to hand
     * the bytes straight back to this pool, once per file while a file listing
     * populates.
     *
     * @param png_path    Filesystem path to the PNG (no "A:" LVGL prefix)
     * @param source_path Original thumbnail path (used for cache key generation)
     * @param target      Target dimensions and format
     * @param on_success  Called with path to .bin file on success (main thread)
     * @param on_error    Called with error message on failure (main thread)
     */
    void process_file_async(const std::string& png_path, const std::string& source_path,
                            const ThumbnailTarget& target, ProcessSuccessCallback on_success,
                            ProcessErrorCallback on_error);

    /**
     * @brief Process PNG data synchronously
     *
     * Blocks until processing is complete. Prefer process_async() for UI code.
     *
     * @param png_data Raw PNG file contents
     * @param source_path Original thumbnail path (used for cache key generation)
     * @param target Target dimensions and format
     * @return ProcessResult with success/failure and output path
     */
    ProcessResult process_sync(const std::vector<uint8_t>& png_data, const std::string& source_path,
                               const ThumbnailTarget& target);

    /**
     * @brief Check if a pre-scaled version exists in cache
     *
     * Fast synchronous lookup - does not trigger processing.
     *
     * @param source_path Original thumbnail path
     * @param target Target dimensions and format
     * @return LVGL path (A:/...) to .bin if cached, empty string otherwise
     */
    std::string get_if_processed(const std::string& source_path,
                                 const ThumbnailTarget& target) const;

    /**
     * @brief Get optimal thumbnail target for current display
     *
     * Queries the active LVGL display and returns target dimensions based on
     * display height breakpoint (5-tier: TINY/SMALL/MEDIUM/LARGE/XLARGE):
     *
     * Card sizes:   SMALL (≤460): 120x120, MEDIUM (≤550): 160x160, LARGE/XLARGE (>550): 220x220
     * Detail sizes: SMALL (≤460): 200x200, MEDIUM (≤550): 300x300, LARGE/XLARGE (>550): 400x400
     *
     * Always uses ARGB8888 — LVGL converts to display format at render time.
     *
     * @param size Use case: Card (file list) or Detail (status/detail views)
     * @note MUST be called from main thread only (LVGL is not thread-safe).
     *       For background threads, cache the result at initialization.
     *
     * @return ThumbnailTarget for current display configuration
     */
    static ThumbnailTarget get_target_for_display(ThumbnailSize size = ThumbnailSize::Card);

    /**
     * @brief Get thumbnail target for specific display dimensions
     *
     * Pure function version for testing. Uses the same breakpoint logic
     * as get_target_for_display(). Always uses ARGB8888.
     *
     * @param width Display width in pixels
     * @param height Display height in pixels
     * @param size Use case: Card (file list) or Detail (status/detail views)
     * @return ThumbnailTarget for the given dimensions
     */
    static ThumbnailTarget get_target_for_resolution(int width, int height,
                                                     ThumbnailSize size = ThumbnailSize::Card);

    /**
     * @brief Get the card thumbnail target that fits a card of the given size
     *
     * The resolution ladder in get_target_for_resolution() infers a card size from
     * the display, and those guesses drifted from what the grid actually lays out —
     * a 480x272 screen builds 138x115 cards, not the ~107px the ladder assumed, so a
     * 120x120 .bin overhung the card and LVGL cropped the top off every model.
     * This derives the target from the real card box instead.
     *
     * The card art is centred and then lifted by `preview_offset_y` (-12% of its own
     * height) to clear the metadata overlay, so a square of side N needs
     * (H - N)/2 - 0.12N >= 0 to stay inside a card of height H. Width is the other
     * bound. The result is snapped down to a multiple of 4 to keep the number of
     * distinct `{hash}_{w}x{h}_ARGB8888.bin` cache entries small.
     *
     * Pure function — no LVGL access, safe from any thread.
     *
     * @param card_width  Card width in pixels
     * @param card_height Card height in pixels
     * @return ThumbnailTarget that fits inside the card, clamped to [64, 220]
     */
    static ThumbnailTarget get_target_for_card(int card_width, int card_height);

    /**
     * @brief Publish the card grid's measured card size for Card-size lookups
     *
     * PrintSelectPanel calls this whenever it recomputes CardDimensions. Once set,
     * get_target_for_display(ThumbnailSize::Card) returns get_target_for_card() for
     * that size instead of the resolution ladder, so the pre-scaled .bin and the
     * image widget agree on a size the card can actually hold. Passing 0,0 restores
     * the ladder (used by callers with no card grid, e.g. the timelapse overlay).
     *
     * Thread-safe: the thumbnail worker threads read the hint while fetching.
     *
     * @param card_width  Card width in pixels, or 0 to clear the hint
     * @param card_height Card height in pixels, or 0 to clear the hint
     */
    static void set_card_size_hint(int card_width, int card_height);

    /**
     * @brief Get the cache directory path (thread-safe)
     * @return Path to thumbnail cache directory (e.g., /tmp/helix_thumbs)
     */
    std::string get_cache_dir() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_dir_;
    }

    /**
     * @brief Set the cache directory path
     *
     * Must be called before any processing. Creates directory if needed.
     *
     * @param path Directory path for cached .bin files
     */
    void set_cache_dir(const std::string& path);

    /**
     * @brief Register the listener told about every .bin this processor writes
     *
     * The processor writes its pre-scaled `.bin` files into ThumbnailCache's
     * directory but has no reference to the cache, so the cache's in-memory
     * eviction index cannot see them. Left unreported the index under-counts by
     * most of the cache and eviction silently stops
     * (prestonbrown/helixscreen#1207).
     *
     * Held weakly: a destroyed cache leaves nothing dangling, and an in-flight
     * write simply finds no listener. Last registration wins, mirroring
     * set_cache_dir() — the two are set together by ThumbnailCache's
     * constructor and only ever describe the most recently constructed cache.
     *
     * Thread-safe. Pass an empty weak_ptr to unregister.
     *
     * @param journal Listener to notify after each successful write
     */
    void set_write_journal(std::weak_ptr<ThumbnailWriteJournal> journal);

    /**
     * @brief Clear all cached pre-scaled thumbnails
     *
     * Removes all .bin files from cache directory.
     * Thread-safe but may block briefly.
     */
    void clear_cache();

    /**
     * @brief Get number of tasks still queued or executing
     *
     * Counts both: a task a worker has already picked up but not yet
     * finished is still "pending" for any caller polling this to learn when
     * the pool has genuinely gone quiet.
     */
    size_t pending_tasks() const;

    /**
     * @brief Wait for all pending tasks to complete
     *
     * Useful for testing or graceful shutdown.
     */
    void wait_for_completion();

    /**
     * @brief Submit an arbitrary task to the worker pool, bypassing PNG processing
     *
     * Test/mock-scenario hook: gives pending_tasks() a deterministic nonzero
     * window without needing a real thumbnail to decode, mirroring
     * HttpExecutor::submit() for the same purpose. See mock_scenarios.cpp's
     * "thumbnail_busy" scenario.
     */
    void submit_test_task(std::function<void()> task);

    /**
     * @brief Shutdown the processor
     *
     * Stops the thread pool and waits for pending tasks.
     * Called automatically on destruction.
     */
    void shutdown();

  private:
    ThumbnailProcessor();
    ~ThumbnailProcessor();

    /**
     * @brief Generate cache filename for a source/target combination
     *
     * Format: {cache_key}_{w}x{h}_{format}.bin
     * Example: a1b2c3d4_160x160_ARGB8888.bin
     *
     * @param cache_key ThumbnailCache::compute_hash() of the source, taken when the work is
     *        requested: it names the printer, which can change before a queued job runs.
     */
    std::string generate_cache_filename(const std::string& cache_key,
                                        const ThumbnailTarget& target) const;

    /**
     * @brief Core processing implementation
     *
     * 1. Decode PNG with stb_image
     * 2. Calculate output dimensions (preserve aspect, cover target)
     * 3. Resize with stb_image_resize (high-quality Mitchell filter)
     * 4. Convert to ARGB8888 if needed
     * 5. Write LVGL binary header + pixel data
     *
     * @param cache_key The source's cache key, taken when the work was requested
     * @param cache_dir Cache directory path (passed explicitly for thread safety)
     * @param journal Write listener, or nullptr. Snapshotted by the caller
     *        alongside @p cache_dir for the same reason: the pair must describe
     *        one consistent destination even if set_cache_dir() /
     *        set_write_journal() run before a queued task gets to execute.
     *        Passing it in also keeps do_process() free of mutex_, so a pool
     *        task can never contend with a shutdown() that is waiting on it.
     */
    ProcessResult do_process(const std::vector<uint8_t>& png_data, const std::string& source_path,
                             const std::string& cache_key, const ThumbnailTarget& target,
                             const std::string& cache_dir,
                             const std::shared_ptr<ThumbnailWriteJournal>& journal);

    /// Marshal a finished ProcessResult back to the main thread and fire the
    /// caller's callback. Shared by process_async() and process_file_async() so
    /// both keep identical main-thread-dispatch semantics.
    static void deliver_result(const ProcessResult& result, const std::string& source,
                               ProcessSuccessCallback on_success, ProcessErrorCallback on_error);

    /**
     * @brief Write LVGL binary file
     *
     * Format: 12-byte lv_image_header_t followed by raw pixel data
     */
    bool write_lvbin(const std::string& path, int width, int height, uint8_t color_format,
                     const uint8_t* pixel_data, size_t data_size);

    /// shared_ptr, not unique_ptr: wait_for_completion() must keep the pool
    /// alive while it blocks OUTSIDE the lock, because shutdown() resets this
    /// member under that same lock. Everything else uses it only while holding
    /// mutex_ — see process_async() for why a strong reference alone is not
    /// sufficient there (#1202).
    std::shared_ptr<HThreadPool> thread_pool_;
    std::string cache_dir_;

    /// Weak by design — see set_write_journal(). Guarded by mutex_ and
    /// snapshotted into each task alongside cache_dir_.
    std::weak_ptr<ThumbnailWriteJournal> write_journal_;

    mutable std::mutex mutex_;
    bool shutdown_ = false;

    /// Lets tests construct a private, non-singleton instance so a
    /// shutdown-race test does not tear down the process-wide one out from
    /// under every other test. Lives in the global namespace (tests/), hence
    /// the leading `::` — same convention as GcodeErrorRouterTestAccess.
    friend struct ::ThumbnailProcessorTestAccess;
};

} // namespace helix
