// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_shared_callback.h"
#include "i_moonraker_api.h"
#include "thumbnail_load_context.h"
#include "thumbnail_processor.h"
#include "thumbnail_rules.h"
#include "thumbnail_write_journal.h"
#include "zip_reader.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/**
 * @file thumbnail_cache.h
 * @brief Centralized thumbnail caching for print files and history
 *
 * ThumbnailCache provides a unified approach to downloading and caching
 * thumbnail images from Moonraker. It handles:
 * - Hash-based filename generation for cache files
 * - Cache directory creation
 * - Async download with callbacks
 * - LVGL-compatible path formatting ("A:" prefix)
 *
 * ## Usage Example
 * ```cpp
 * ThumbnailCache cache;
 *
 * // Check if already cached (sync)
 * std::string lvgl_path = cache.get_if_cached(relative_path);
 * if (!lvgl_path.empty()) {
 *     lv_image_set_src(img, lvgl_path.c_str());
 *     return;
 * }
 *
 * // Download async
 * cache.fetch(api_, relative_path,
 *     [this](const std::string& lvgl_path) {
 *         // Update UI on main thread
 *         lv_image_set_src(img, lvgl_path.c_str());
 *     },
 *     [](const std::string& error) {
 *         spdlog::warn("Thumbnail download failed: {}", error);
 *     });
 * ```
 *
 * @see IMoonrakerAPI::download_thumbnail
 */

/**
 * @brief One thumbnail fetch request.
 *
 * `key` is the id within `source`: a Moonraker thumbnail path from the gcodes
 * root, a USB gcode path, a timelapse video name. The cache files it under
 * helix::thumbnail_key(source, key, target).
 */
struct ThumbnailRequest {
    /**
     * @brief Which artifact the request resolves to.
     *
     * Pre-scaled is right for almost everything — it renders without runtime
     * scaling. FullPng exists for the two places that genuinely need the
     * original: print-select's fallback when no card .bin has been produced yet
     * (on Snapmaker U1 / AD5M the detail preview is the ONLY render, so
     * answering "not cached" there leaves a black preview), and the history
     * detail overlay, which has always shown the raw PNG.
     */
    enum class ThumbnailFormat {
        Prescaled, ///< pre-scaled .bin at req.target (default; fastest to render)
        FullPng,   ///< full-resolution cached PNG; req.target is ignored
    };

    std::string key;
    helix::ThumbnailSource source = helix::ThumbnailSource::Moonraker;
    helix::ThumbnailTarget target;
    time_t source_modified = 0;
    helix::IMoonrakerAPI* api = nullptr;
    ThumbnailFormat format = ThumbnailFormat::Prescaled;
};

class ThumbnailCache {
    friend class ThumbnailCacheTestAccess;

  public:
    /// Default cache subdirectory name (appended to base cache dir)
    static constexpr const char* kCacheSubdir = "helix_thumbs";

    /// Minimum cache size (5 MB) - floor for very constrained systems
    static constexpr size_t kMinCacheSize = 5 * 1024 * 1024;

    /// Default maximum cache size (20 MB) - conservative for AD5M, override via config
    static constexpr size_t kDefaultMaxCacheSize = 20 * 1024 * 1024;

    /// Default percentage of available disk space to use for cache
    static constexpr double kDefaultDiskPercent = 0.05; // 5%

    /// Default critical disk threshold (5 MB) - conservative for AD5M
    static constexpr size_t kDefaultDiskCritical = 5 * 1024 * 1024;

    /// Default low disk threshold (20 MB) - conservative for AD5M
    static constexpr size_t kDefaultDiskLow = 20 * 1024 * 1024;

    /// Callback for successful thumbnail fetch (receives LVGL-ready path with "A:" prefix)
    ///
    /// `degraded` is true when the path is a graceful fallback rather than the
    /// thing that was asked for: process_and_callback() answers a pre-scaling
    /// FAILURE through this success channel, handing back the raw PNG. It still
    /// renders, just without the pre-scale.
    using SuccessCallback = helix::SharedCallback<void(const std::string& path, bool degraded)>;

    /// Callback for failed thumbnail fetch (receives error message)
    using ErrorCallback = helix::SharedCallback<void(const std::string& error)>;

    /**
     * @brief Default constructor - auto-sizes based on available disk space
     *
     * Creates cache directory if it doesn't exist.
     * Cache size is calculated as:
     *   clamp(available_space * 5%, kMinCacheSize, MAX_CACHE_SIZE)
     */
    ThumbnailCache();

    /**
     * @brief Constructor with explicit max size (for testing)
     *
     * @param max_size Maximum cache size in bytes
     */
    explicit ThumbnailCache(size_t max_size);

    /**
     * @brief Get the current cache directory path
     *
     * @return Absolute path to cache directory (e.g., "/home/user/.cache/helix_thumbs")
     */
    [[nodiscard]] std::string get_cache_dir() const {
        return cache_dir_; // Return by value for thread safety
    }

    /**
     * @brief The local path of the full PNG cached for a cache id
     *
     * @param cache_id helix::thumbnail_cache_id(source, id); a Moonraker
     *        thumbnail path is its own cache id
     * @return `{cache_dir}/` + helix::thumbnail_file_name(hash, nullptr)
     */
    [[nodiscard]] std::string get_cache_path(const std::string& cache_id) const;

    /**
     * @brief What is already cached for a cache id, without touching it
     *
     * The pre-scaled .bin for @p target, else the full PNG, else empty. Read only: no LRU
     * bump, no freshness check, no invalidation, no fetch, so a caller that must cause no
     * side effect (a screensaver start) can ask.
     *
     * @return LVGL path ("A:...") or empty
     */
    [[nodiscard]] std::string peek(const std::string& cache_id,
                                   const helix::ThumbnailTarget& target) const;

    /**
     * @brief Get LVGL path if thumbnail is already cached
     *
     * Checks if the file exists locally without network request.
     * Useful for instant display when revisiting cached content.
     *
     * @param relative_path Moonraker relative path
     * @param source_modified Optional source file modification time (Unix timestamp).
     *        If provided and the cached file is older than this, the cache is
     *        invalidated and empty string is returned. Use 0 to skip validation.
     * @return LVGL-ready path ("A:{cache_dir}/...") if cached, empty string otherwise
     */
    [[nodiscard]] std::string get_if_cached(const std::string& relative_path,
                                            time_t source_modified = 0) const;

    /**
     * @brief Synchronous cache lookup for a request
     *
     * The request-shaped counterpart to fetch(), and it resolves the same
     * artifact fetch() would: the pre-scaled .bin for req.target, or the
     * full-resolution PNG when req.format is FullPng. Either way
     * req.source_modified governs freshness, so a caller that already built a
     * ThumbnailRequest does not have to unpack it to ask "do I already have
     * this?".
     *
     * @param req The request to look up
     * @return LVGL path ("A:...") to the cached file, or empty if absent/stale
     */
    [[nodiscard]] std::string get_if_cached(const ThumbnailRequest& req) const;

    /**
     * @brief Check if a path is already in LVGL format
     *
     * @param path Path to check
     * @return true if path starts with "A:" (already processed)
     */
    [[nodiscard]] static bool is_lvgl_path(const std::string& path);

    /**
     * @brief Convert a local filesystem path to LVGL format
     *
     * @param local_path Local filesystem path
     * @return LVGL-ready path with "A:" prefix
     */
    [[nodiscard]] static std::string to_lvgl_path(const std::string& local_path);

    /**
     * @brief Fetch thumbnail, downloading if not cached
     *
     * This is the main async entry point. It:
     * 1. Checks if already cached (returns immediately if so)
     * 2. Downloads from Moonraker if not cached
     * 3. Calls success callback with LVGL-ready path
     *
     * @param api IMoonrakerAPI instance for downloading
     * @param relative_path Moonraker relative path (e.g., ".thumbnails/file.png")
     * @param on_success Called with LVGL path on success (may be called synchronously if cached)
     * @param on_error Called with error message on failure
     * @param source_modified Source mtime; a cached PNG older than it is dropped and
     *        refetched. 0 skips the check.
     *
     * @note Callbacks may be invoked from background thread - use ui_queue_update() for UI updates
     */
    void fetch(helix::IMoonrakerAPI* api, const std::string& relative_path,
               SuccessCallback on_success, ErrorCallback on_error, time_t source_modified = 0);

    /**
     * @brief Fetch a thumbnail described by a request, guarded by a load context
     *
     * The single fetch entry point every consumer uses. The
     * request carries what to fetch (key, target, format, freshness, api); the
     * context carries whether the answer is still wanted.
     *
     * on_success is invoked only if ctx.is_valid() — that is, the caller is
     * still alive AND no newer request has bumped the generation counter the
     * context captured. A superseded load is dropped silently, so callbacks do
     * not each need their own staleness check. on_error is NOT guarded.
     *
     * @param req What to fetch
     * @param ctx Async safety context (created via ThumbnailLoadContext::create())
     * @param on_success Called with the LVGL path (only if ctx.is_valid())
     * @param on_error Optional error callback (always called on error)
     *
     * @note Callbacks are marshalled to the LVGL main thread by fetch_optimized(),
     *       and run inline when the caller is already on it.
     * @see ThumbnailLoadContext::create
     */
    void fetch(const ThumbnailRequest& req, ThumbnailLoadContext ctx, SuccessCallback on_success,
               ErrorCallback on_error = nullptr);

    /**
     * @brief Save thumbnail bytes extracted outside Moonraker straight to the cache
     *
     * For thumbnails read out of a gcode header (USB files, metadata with no
     * thumbnails) or a local file, filed under the same key a fetch for
     * (source, id) looks up. A JPEG is re-encoded as PNG; anything else is
     * refused (helix::ensure_png).
     *
     * Overwriting a key drops the pre-scaled .bin variants derived from the
     * previous PNG, so a re-slice under the same name cannot be served
     * through stale prescaled artifacts (callers that pass no
     * source_modified have no other freshness check).
     *
     * @return LVGL path ("A:...") to the saved file, or empty on failure
     * @note Triggers cache eviction if needed after saving
     */
    std::string save_raw_png(helix::ThumbnailSource source, const std::string& id,
                             const std::vector<uint8_t>& png_data);

    /**
     * @brief save_raw_png(), then pre-scale to @p target on the calling thread
     *
     * For sources with no fetch to hang the pre-scale off (a USB scan), so
     * their cards draw a .bin like every other card. A .bin still fresh against
     * @p source_modified is returned as is, with nothing rewritten or decoded.
     * Otherwise blocks on the decode: worker threads only.
     *
     * @return The .bin path, the PNG path when pre-scaling fails, or empty when
     *         nothing was saved
     */
    std::string save_prescaled(helix::ThumbnailSource source, const std::string& id,
                               const std::vector<uint8_t>& image_data,
                               const helix::ThumbnailTarget& target, time_t source_modified);

    /**
     * @brief Clear all cached thumbnails
     *
     * Removes all files from the cache directory.
     * Useful for testing or manual cache invalidation.
     *
     * @return Number of files removed
     */
    size_t clear_cache();

    /**
     * @brief Invalidate cached thumbnails for a specific file
     *
     * Removes PNG and all pre-scaled .bin variants for the given path.
     * Call this when a G-code file is overwritten with new content.
     *
     * @param cache_id helix::thumbnail_cache_id(source, id); a Moonraker
     *        thumbnail path is its own cache id
     * @return Number of files removed
     */
    size_t invalidate(const std::string& cache_id);

    /**
     * @brief Get the total size of cached thumbnails
     *
     * @return Total size in bytes
     */
    [[nodiscard]] size_t get_cache_size() const;

    /**
     * @brief Get the maximum cache size
     *
     * @return Maximum cache size in bytes
     */
    [[nodiscard]] size_t get_max_size() const;

    /**
     * @brief Set maximum cache size
     *
     * If new size is smaller than current cache, eviction will occur.
     *
     * @param max_size New maximum size in bytes
     */
    void set_max_size(size_t max_size);

    /**
     * @brief Disk pressure levels for adaptive cache management
     */
    enum class DiskPressure {
        Normal,  ///< Plenty of space - normal caching behavior
        Low,     ///< Below DISK_LOW_THRESHOLD - evict aggressively
        Critical ///< Below DISK_CRITICAL_THRESHOLD - skip caching entirely
    };

    /**
     * @brief Check current disk pressure level
     *
     * Queries available disk space and returns appropriate pressure level.
     * Used to adapt caching behavior to real-time disk conditions.
     *
     * @return Current disk pressure level
     */
    [[nodiscard]] DiskPressure get_disk_pressure() const;

    /// The pressure level a free-space reading means. An unknown reading (the
    /// platform cannot report free space) is Normal: not knowing how much room
    /// is left is no evidence that it is running out.
    [[nodiscard]] static DiskPressure classify_disk_pressure(std::optional<size_t> available,
                                                             size_t critical, size_t low);

    /**
     * @brief Get available disk space in bytes
     *
     * @return Available bytes, or 0 on error
     */
    [[nodiscard]] size_t get_available_disk_space() const;

    /// Free bytes, nullopt when the platform cannot report them. Rate-limited.
    [[nodiscard]] std::optional<size_t> probe_disk_space() const;

    /// Drop the cached free-space reading so the next query re-probes.
    /// Call after any operation that materially changes cache size on disk.
    void invalidate_disk_probe();

    /**
     * @brief Check if caching is currently allowed
     *
     * Returns false when disk is critically low to prevent filling up the filesystem.
     *
     * @return true if caching is allowed, false if disk is critical
     */
    [[nodiscard]] bool is_caching_allowed() const;

    /**
     * @brief Diagnostics: full cache-directory walks performed by this instance
     *
     * Every walk is O(files in cache) `readdir` + `stat` syscalls, so this is the
     * quantity the in-memory index exists to keep flat. Without this count the
     * per-fetch cost of a walk is invisible (prestonbrown/helixscreen#1207).
     *
     * @return Monotonic count of directory walks since construction
     */
    [[nodiscard]] size_t get_full_scan_count() const {
        return full_scans_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Diagnostics: per-file metadata queries issued by this instance
     *
     * One per file examined, whether during a full walk or a single-file index
     * update. (Each examined file costs up to three `stat`-family calls —
     * `is_regular_file`, `last_write_time`, `file_size` — so this is a lower
     * bound on syscalls, not an exact one.)
     *
     * @return Monotonic count of files stat'd since construction
     */
    [[nodiscard]] size_t get_stat_count() const {
        return stat_calls_.load(std::memory_order_relaxed);
    }

  private:
    /**
     * @brief Fetch thumbnail with pre-scaling optimization
     *
     * The engine under fetch(ThumbnailRequest, ...): produces pre-scaled LVGL
     * binary files (.bin) that render without runtime scaling.
     *
     * Flow:
     * 1. Check for pre-scaled .bin (instant return if found)
     * 2. Check for cached PNG (queue for background pre-scaling)
     * 3. Download PNG if needed, then pre-scale
     * 4. Return .bin path on success
     *
     * Private because every consumer goes through fetch(): reaching this
     * directly skips the ThumbnailLoadContext guard and with it all staleness
     * protection.
     *
     * @param source_modified Optional source file modification time (Unix timestamp).
     *        If provided and the cached file is older than this, the cache is
     *        invalidated and a fresh download is triggered. Use 0 to skip validation.
     *
     * @note Falls back to PNG on pre-scaling failure - display still works, just slower
     */
    void fetch_optimized(helix::IMoonrakerAPI* api, const std::string& relative_path,
                         const helix::ThumbnailTarget& target, SuccessCallback on_success,
                         ErrorCallback on_error, time_t source_modified = 0);

    /**
     * @brief Check if a pre-scaled version exists in cache
     *
     * Fast synchronous lookup for pre-scaled .bin files, and what
     * get_if_cached(ThumbnailRequest) resolves to for Prescaled requests.
     * Private for the same reason as fetch_optimized(): callers that build the
     * target by hand drift from the request the fetch will use.
     *
     * @param source_modified Optional source file modification time (Unix timestamp).
     *        If provided and the cached file is older than this, the cache is
     *        invalidated and empty string is returned. Use 0 to skip validation.
     * @return LVGL path (A:/...) to .bin if cached and fresh, empty string otherwise
     */
    [[nodiscard]] std::string get_if_optimized(const std::string& relative_path,
                                               const helix::ThumbnailTarget& target,
                                               time_t source_modified = 0) const;

    /// "{hash}.unsupported": the server sent this cache id in a format the
    /// cache refuses (QOI and the like). A cached miss until the source is newer.
    [[nodiscard]] std::string unsupported_marker_path(const std::string& cache_id) const;

    /// A refusal recorded for @p cache_id that is still fresh against
    /// @p source_modified. A stale marker is removed.
    [[nodiscard]] bool is_refused(const std::string& cache_id, time_t source_modified);

    /// Record a refusal, or drop the reply for any other download error.
    void note_download_error(const std::string& cache_id, const MoonrakerError& error);

    /**
     * @brief Wrap a caller callback so it always fires on the LVGL main thread
     *
     * Callers hand us callbacks that end up setting LVGL image sources, but the
     * thread a result arrives on depends on which internal path produced it:
     * a pre-scaled hit answers inline on the caller's thread, a download error
     * answers on an HttpExecutor worker, and a processor-shutdown error answers
     * on whichever thread reached the processor — which, on the download-success
     * path, is a worker. `process_and_callback` then converts that error into a
     * *success* via the PNG fallback, so even fetch_optimized's success could be
     * delivered off-thread.
     *
     * Rather than ask each of those paths to remember, both public entry points
     * wrap the caller's callbacks once, here. run_on_main() runs inline when
     * already on the main thread, so a cache hit still answers synchronously.
     * See prestonbrown/helixscreen#960, #1202.
     */
    static SuccessCallback on_main(SuccessCallback cb);
    static ErrorCallback on_main_err(ErrorCallback cb);

    std::string cache_dir_; ///< Absolute path to cache directory (const after construction)
    size_t max_size_;       ///< Maximum cache size before LRU eviction — guarded by mutex_
    size_t disk_critical_;  ///< Stop caching below this available space (const after construction)
    size_t disk_low_;       ///< Evict aggressively below this available space (const after ctor)

    /// Rate limit for the free-space syscall. Reached twice per fetch_optimized()
    /// on the main thread; free space does not change fast enough to warrant a
    /// statfs per thumbnail.
    static constexpr int64_t kDiskProbeIntervalMs = 2000;

    /// Guards the disk-probe cache only. Deliberately NOT mutex_: get_disk_pressure()
    /// is called from inside evict_locked(), which already holds mutex_.
    mutable std::mutex disk_probe_mutex_;
    mutable std::optional<size_t> cached_available_bytes_;
    mutable std::chrono::steady_clock::time_point last_disk_probe_;
    mutable bool disk_probe_valid_{false};
    size_t configured_max_; ///< Max size from config, before dynamic sizing (const after ctor)

    /// Serializes cache accounting: the directory scan, the eviction pass, and
    /// max_size_.
    ///
    /// evict_if_needed() runs on the main thread AND on every HttpExecutor worker
    /// — MoonrakerFileTransferAPI invokes download_thumbnail's success callback on
    /// the pool thread, unmarshalled. Measured: 104 eviction events across 5
    /// threads in 90s, up to 4 in the same millisecond, each independently
    /// stat'ing every file and selecting the same victims
    /// (prestonbrown/helixscreen#1207).
    ///
    /// Held across scan-sort-remove, so a scan cannot observe a directory another
    /// thread is halfway through unlinking. A walk that does throws mid-iteration
    /// and silently abandons the pass.
    mutable std::mutex mutex_;

    /// One cached file, as seen by a directory scan.
    struct CacheEntry {
        std::string path;
        std::int64_t mtime_ns; ///< helix::fs::mtime_ns()
        std::uint64_t size;
    };

    /**
     * @brief Walk the cache directory once, collecting every readable entry.
     *
     * Per-entry failures are skipped rather than fatal. A single un-stat'able
     * file (concurrently unlinked, a broken link, a permission change) must cost
     * only that file — one try/catch around the whole loop would return the
     * partial sum accumulated before the throw, which reads as "the cache is
     * smaller than it is" and silently disables eviction.
     *
     * @param total_out Receives the summed size of all readable entries.
     * @return Entries in directory order (unsorted).
     * @pre mutex_ is held.
     */
    [[nodiscard]] std::vector<CacheEntry> scan_locked(size_t* total_out) const;

    // =========================================================================
    // In-memory index (prestonbrown/helixscreen#1207)
    // =========================================================================
    //
    // Without it, every fetch()/fetch_optimized()/save_raw_png() would walk the
    // whole cache directory with a stat per file, whether or not anything gets
    // evicted. Scrolling 850 files x 3 thumbnails makes that a per-scroll-tick
    // cost, much of it on the main thread.
    //
    // The index turns the steady-state check into arithmetic. What makes it
    // *honest* rather than merely fast is having an answer for each way it can
    // drift from the directory it describes:
    //
    //   1. Files this cache writes itself (downloaded PNGs, save_raw_png) —
    //      recorded at the point of writing, one stat each.
    //   2. Files ThumbnailProcessor writes (the pre-scaled .bin variants) —
    //      reported through journal_, one stat each. This is the case that made
    //      an index non-trivial: the processor writes into this directory and
    //      holds no reference to this class.
    //   3. Files that vanish behind our back — the eviction loop already
    //      distinguishes "removed" from "was already gone", so a ghost costs no
    //      credited bytes, and seeing one triggers a resync.
    //   4. Anything else, including a directory populated by a previous run —
    //      covered by the cold-start scan and by a periodic reconcile.
    //
    // The index is allowed to over-count (it evicts too eagerly, which is
    // survivable) but must never under-count, because an under-counting cache
    // reads as "well under the limit" and stops evicting altogether.

    /// One indexed file. Same fields a directory walk would produce.
    struct IndexEntry {
        std::int64_t mtime_ns;
        std::uint64_t size;
    };

    /// Eviction checks between forced full rescans.
    ///
    /// The backstop for drift this class cannot observe — a future writer that
    /// forgets to report, an external delete, a shell. Bounds how long the index
    /// may disagree with the directory, at a cost of one walk per this many
    /// checks instead of one per check. Ordered map, so a rescan is also what
    /// re-establishes exact totals after any anomaly.
    static constexpr size_t kIndexReconcileInterval = 64;

    /// Rebuild the index from the filesystem. The only O(cache) path left.
    /// @pre mutex_ is held.
    void rescan_locked() const;

    /// Bring the index up to date cheaply: absorb journalled writes, or rescan
    /// if the index is cold, overflowed, or due for reconciliation.
    /// @pre mutex_ is held.
    void refresh_index_locked() const;

    /// Stat one file and fold it into the index. Rejects paths outside
    /// cache_dir_ — set_cache_dir() on the processor can retarget it while a
    /// write is in flight, and a stale journal entry must not add a foreign
    /// file to this cache's accounting.
    /// @pre mutex_ is held.
    void index_file_locked(const std::string& raw_path) const;

    /// Drop a path from the index, crediting its bytes back.
    /// @pre mutex_ is held.
    void forget_file_locked(const std::string& path) const;

    /// Record that a lookup just served @p path (LVGL or filesystem form).
    /// Eviction orders by the later of a file's write and its last use.
    void note_use(const std::string& path) const;

    /// Record a file this cache just wrote, then run an eviction check. The
    /// shape every write site uses so no write can reach eviction unindexed.
    void note_write_and_evict(const std::string& path);

    /// Remove the pre-scaled .bin variants ({hash}_{w}x{h}_{format}.bin) for
    /// one key. What both invalidate() and save_raw_png() use to keep a
    /// freshly written PNG from being served through bins prescaled from the
    /// previous one.
    /// @return Variants removed, or nullopt (errno set) when the directory could
    ///         not be listed or a variant could not be removed.
    /// @pre mutex_ is held.
    std::optional<size_t> remove_bin_variants_locked(const std::string& hash);

    /**
     * @brief Eviction pass proper.
     * @pre mutex_ is held.
     */
    void evict_locked();

    /// Diagnostics — see get_full_scan_count() / get_stat_count(). Atomic rather
    /// than mutex_-guarded so the accessors stay lock-free and cannot deadlock a
    /// caller that already holds the lock.
    mutable std::atomic<size_t> full_scans_{0};
    mutable std::atomic<size_t> stat_calls_{0};

    /// The index and its bookkeeping. All guarded by mutex_; mutable because
    /// get_cache_size() is const and still has to prime and reconcile.
    mutable std::map<std::string, IndexEntry> index_;
    /// Wall-clock ns (the same Unix-epoch CLOCK_REALTIME domain as
    /// helix::fs::mtime_ns) of each indexed file's last cache hit. Kept apart
    /// from IndexEntry because a rescan rebuilds that from the directory, and a
    /// use is nothing the directory records: mtime is the freshness stamp, so a
    /// hit must never move it. Guarded by usage_mutex_, NOT mutex_: hits are
    /// recorded on the UI thread, and mutex_ is held across whole directory
    /// walks and unlinks on the workers. Lock order: mutex_, then usage_mutex_.
    mutable std::map<std::string, std::int64_t> last_used_ns_;
    mutable std::mutex usage_mutex_;
    mutable size_t index_total_ = 0;
    mutable bool index_primed_ = false;
    mutable size_t checks_since_scan_ = 0;

    /// Owned here, observed weakly by ThumbnailProcessor, so destroying this
    /// cache cannot leave the processor holding a dangling pointer. Created
    /// eagerly: refresh_index_locked() must never have to null-check it.
    std::shared_ptr<helix::ThumbnailWriteJournal> journal_{
        std::make_shared<helix::ThumbnailWriteJournal>()};

    /**
     * @brief Determine the optimal cache base directory
     *
     * Selection order:
     * 1. Config setting /cache/directory if specified
     * 2. XDG_CACHE_HOME environment variable + "/helix"
     * 3. $HOME/.cache/helix (if HOME is set and writable)
     * 4. /tmp/helix_cache (fallback for systems without home dir)
     *
     * @return Absolute path to the cache base directory (not including kCacheSubdir)
     */
    static std::string determine_cache_dir();

    /**
     * @brief Load cache settings from settings.json
     *
     * Reads cache/thumbnail_max_mb, cache/disk_critical_mb, cache/disk_low_mb.
     * Falls back to defaults if config not available.
     */
    void load_config();

    /**
     * @brief Ensure cache directory exists
     */
    void ensure_cache_dir() const;

    /**
     * @brief Evict oldest files if cache exceeds max size
     *
     * Uses file modification time (mtime) as LRU approximation.
     * Removes oldest files until cache is under max_size_.
     */
    void evict_if_needed();

    /**
     * @brief Process PNG and invoke callback with result
     *
     * Helper for fetch_optimized(). Reads PNG, queues for pre-scaling,
     * and invokes callback with .bin path on success or PNG fallback on error.
     *
     * @param png_lvgl_path LVGL path to the cached PNG
     * @param source_path Original Moonraker relative path (for cache key)
     * @param target Target dimensions for pre-scaling
     * @param on_success Success callback
     * @param on_error Error callback (not currently used - fallback to PNG instead)
     */
    void process_and_callback(const std::string& png_lvgl_path, const std::string& source_path,
                              const helix::ThumbnailTarget& target, SuccessCallback on_success,
                              ErrorCallback on_error);
};

/**
 * @brief Global singleton accessor
 *
 * Provides a single shared cache instance for the application.
 *
 * @return Reference to the global ThumbnailCache
 */
ThumbnailCache& get_thumbnail_cache();

namespace helix {

/// Header size that covers every slicer's embedded-thumbnail comment block.
constexpr size_t kGcodeThumbnailHeaderBytes = 100 * 1024;

/// Error-text prefix marking a PERMANENT extraction verdict: the gcode header
/// was read and contains no embedded thumbnails. Every other failure flavor
/// (download error, cache write error) is transient and must stay retryable,
/// so consumers keying a negative cache off extraction errors match THIS
/// prefix, nothing else. Compared by prefix so the message still carries the
/// path.
inline constexpr const char* kGcodeThumbnailNoneEmbedded = "no embedded thumbnails in ";

/// Whether a gcode-header extraction can deliver a thumbnail on this platform.
/// The ESP32 keeps no disk thumbnail cache (save_raw_png() refuses to write), so
/// an extraction there would download the header only to throw it away.
constexpr bool gcode_thumbnail_extraction_available() {
#if defined(HELIX_PLATFORM_ESP32)
    return false;
#else
    return true;
#endif
}

/**
 * @brief Self-serve a thumbnail from the gcode file header when metadata has none
 *
 * For files whose metadata record carries no thumbnails — a slicer run without
 * them, a USB mount Moonraker can't write .thumbs to, or a customized Moonraker
 * whose metadata path drops them — downloads the first @p max_header_bytes of
 * @p gcode_path, extracts the largest embedded PNG, caches it as
 * ThumbnailSource::GcodeExtract, and feeds it through the prescale pipeline via
 * ThumbnailCache::fetch.
 *
 * The parse and the cache write run on the HttpExecutor worker thread; nothing
 * there touches LVGL or the caller. Callbacks have the same contract as
 * ThumbnailCache::fetch's: marshalled to the main thread, with @p on_success
 * dropped when @p ctx has been superseded. @p on_error receives any failure
 * along the way (download, no embedded thumbnails, cache write, prescale).
 */
void fetch_thumbnail_from_gcode(const std::string& gcode_path, size_t max_header_bytes,
                                IMoonrakerAPI* api, const ThumbnailTarget& target,
                                ThumbnailLoadContext ctx,
                                ThumbnailCache::SuccessCallback on_success,
                                ThumbnailCache::ErrorCallback on_error);

/**
 * @brief A thumbnail stored inside an archive on the printer: a .3mf's plate render
 *
 * The sibling of fetch_thumbnail_from_gcode() for an image that is an entry of
 * @p archive_path: served from the cache when it holds this entry for an
 * archive no newer than @p source_modified, else read with one ranged read,
 * cached as ThumbnailSource::ArchiveEntry and prescaled. Same callback contract.
 */
void fetch_thumbnail_from_archive(const std::string& archive_path, const zip::ZipEntry& entry,
                                  time_t source_modified, IMoonrakerAPI* api,
                                  const ThumbnailTarget& target, ThumbnailLoadContext ctx,
                                  ThumbnailCache::SuccessCallback on_success,
                                  ThumbnailCache::ErrorCallback on_error);

} // namespace helix
