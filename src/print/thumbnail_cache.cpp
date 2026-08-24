// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thumbnail_cache.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "system/crash_handler.h"
#include "system/helix_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <vector>

using namespace helix;

// Global singleton using Meyer's Singleton pattern (thread-safe, no leak)
ThumbnailCache& get_thumbnail_cache() {
    static ThumbnailCache instance;
    return instance;
}

// Helper to calculate dynamic cache size based on available disk space
static size_t calculate_dynamic_max_size(const std::string& cache_dir, size_t configured_max) {
    try {
        std::filesystem::space_info space = std::filesystem::space(cache_dir);
        size_t available = space.available;

        // Use 5% of available space
        size_t dynamic_size = static_cast<size_t>(available * ThumbnailCache::DEFAULT_DISK_PERCENT);

        // Clamp to min/configured_max bounds
        size_t clamped = std::clamp(dynamic_size, ThumbnailCache::MIN_CACHE_SIZE, configured_max);

        spdlog::debug("[ThumbnailCache] Available disk: {} MB, cache limit: {} MB (max: {} MB)",
                      available / (1024 * 1024), clamped / (1024 * 1024),
                      configured_max / (1024 * 1024));

        return clamped;
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[ThumbnailCache] Failed to query disk space: {}, using minimum", e.what());
        return ThumbnailCache::MIN_CACHE_SIZE;
    }
}

// Helper to try creating a cache directory and return success
static bool try_create_cache_dir(const std::string& path) {
    return helix::paths::ensure_dir(path) && helix::paths::probe_writable(path);
}

std::string ThumbnailCache::determine_cache_dir() {
    // 1. Check config setting first (explicit override)
    Config* config = Config::get_instance();
    if (config) {
        std::string config_dir = config->get<std::string>("/cache/directory", "");
        if (!config_dir.empty()) {
            std::string full_path = config_dir + "/" + CACHE_SUBDIR;
            if (try_create_cache_dir(full_path)) {
                spdlog::info("[ThumbnailCache] Using configured cache directory: {}", full_path);
                return full_path;
            }
            spdlog::warn("[ThumbnailCache] Cannot use configured directory: {}", full_path);
        }
    }

    // 2. Fall through to centralized cache resolution chain
    return get_helix_cache_dir(CACHE_SUBDIR);
}

ThumbnailCache::ThumbnailCache()
    : cache_dir_(determine_cache_dir()), max_size_(MIN_CACHE_SIZE),
      disk_critical_(DEFAULT_DISK_CRITICAL), disk_low_(DEFAULT_DISK_LOW),
      configured_max_(DEFAULT_MAX_CACHE_SIZE) {
    ensure_cache_dir();
    load_config();
    // Now that directory exists and config is loaded, calculate dynamic size
    max_size_ = calculate_dynamic_max_size(cache_dir_, configured_max_);

    // HELIX_THUMB_CACHE_MAX_MB — force a hard cache ceiling for testing.
    //
    // Applied AFTER the dynamic sizing and deliberately not through it:
    // calculate_dynamic_max_size() clamps its result up to MIN_CACHE_SIZE
    // (5 MB), so feeding a small value in through configured_max_ gets raised
    // straight back and eviction still never fires. Making eviction reachable
    // is the entire point here — without it the decode-vs-evict interaction
    // that debug bundle 6F3QJLFG implicates cannot be exercised under a
    // sanitizer, only in the standalone stress test (#960).
    //
    // Env rather than config so it composes with HELIX_CACHE_DIR in a one-line
    // launch, matching how the rest of the test surface is driven.
    if (const char* env_max = std::getenv("HELIX_THUMB_CACHE_MAX_MB")) {
        char* end = nullptr;
        const long mb = std::strtol(env_max, &end, 10);
        if (end != env_max && mb > 0) {
            configured_max_ = static_cast<size_t>(mb) * 1024 * 1024;
            max_size_ = configured_max_;
            spdlog::info("[ThumbnailCache] HELIX_THUMB_CACHE_MAX_MB={} — cache ceiling forced "
                         "to {} MB (dynamic sizing and the {} MB floor bypassed)",
                         mb, mb, MIN_CACHE_SIZE / (1024 * 1024));
        } else {
            spdlog::warn("[ThumbnailCache] HELIX_THUMB_CACHE_MAX_MB='{}' is not a positive "
                         "integer — ignoring",
                         env_max);
        }
    }

    // Sync ThumbnailProcessor's cache dir with ours, and subscribe to the
    // .bin writes it will make there. Directory first: the journal is only
    // meaningful for writes aimed at cache_dir_, and index_file_locked()
    // discards anything that lands elsewhere.
    helix::ThumbnailProcessor::instance().set_cache_dir(cache_dir_);
    helix::ThumbnailProcessor::instance().set_write_journal(journal_);
}

ThumbnailCache::ThumbnailCache(size_t max_size)
    : cache_dir_(determine_cache_dir()), max_size_(max_size), disk_critical_(DEFAULT_DISK_CRITICAL),
      disk_low_(DEFAULT_DISK_LOW), configured_max_(max_size) {
    ensure_cache_dir();
    spdlog::debug("[ThumbnailCache] Using explicit max size: {} MB", max_size_ / (1024 * 1024));

    // Sync ThumbnailProcessor's cache dir with ours, and subscribe to its
    // .bin writes — see the default constructor.
    helix::ThumbnailProcessor::instance().set_cache_dir(cache_dir_);
    helix::ThumbnailProcessor::instance().set_write_journal(journal_);
}

void ThumbnailCache::ensure_cache_dir() const {
    try {
        std::filesystem::create_directories(cache_dir_);
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[ThumbnailCache] Failed to create cache directory {}: {}", cache_dir_,
                     e.what());
    }
}

void ThumbnailCache::load_config() {
    Config* config = Config::get_instance();
    if (!config) {
        spdlog::debug("[ThumbnailCache] Config not available, using defaults");
        return;
    }

    // Read cache settings from config (values are in MB, convert to bytes)
    int max_mb = config->get<int>("/cache/thumbnail_max_mb",
                                  static_cast<int>(DEFAULT_MAX_CACHE_SIZE / (1024 * 1024)));
    int critical_mb = config->get<int>("/cache/disk_critical_mb",
                                       static_cast<int>(DEFAULT_DISK_CRITICAL / (1024 * 1024)));
    int low_mb =
        config->get<int>("/cache/disk_low_mb", static_cast<int>(DEFAULT_DISK_LOW / (1024 * 1024)));

    // Convert to bytes and store
    configured_max_ = static_cast<size_t>(max_mb) * 1024 * 1024;
    disk_critical_ = static_cast<size_t>(critical_mb) * 1024 * 1024;
    disk_low_ = static_cast<size_t>(low_mb) * 1024 * 1024;

    // Sanity check: critical should be less than low
    if (disk_critical_ >= disk_low_) {
        spdlog::warn("[ThumbnailCache] disk_critical_mb ({}) >= disk_low_mb ({}), adjusting",
                     critical_mb, low_mb);
        disk_critical_ = disk_low_ / 2;
    }

    spdlog::debug("[ThumbnailCache] Config loaded: max={} MB, critical={} MB, low={} MB",
                  configured_max_ / (1024 * 1024), disk_critical_ / (1024 * 1024),
                  disk_low_ / (1024 * 1024));
}

std::string ThumbnailCache::compute_hash(const std::string& path) {
    std::hash<std::string> hasher;
    return std::to_string(hasher(path));
}

std::string ThumbnailCache::get_cache_path(const std::string& relative_path) const {
    return cache_dir_ + "/" + compute_hash(relative_path) + ".png";
}

bool ThumbnailCache::is_lvgl_path(const std::string& path) {
    return path.size() >= 2 && path[0] == 'A' && path[1] == ':';
}

std::string ThumbnailCache::to_lvgl_path(const std::string& local_path) {
    if (is_lvgl_path(local_path)) {
        return local_path; // Already in LVGL format
    }
    return "A:" + local_path;
}

std::string ThumbnailCache::get_if_cached(const std::string& relative_path,
                                          time_t source_modified) const {
    if (relative_path.empty()) {
        return "";
    }

    // If already an LVGL path, check if the file exists
    if (is_lvgl_path(relative_path)) {
        std::string local_path = relative_path.substr(2); // Remove "A:" prefix
        if (std::filesystem::exists(local_path)) {
            return relative_path;
        }
        return "";
    }

    // Check if cached locally
    std::string cache_path = get_cache_path(relative_path);
    if (!std::filesystem::exists(cache_path)) {
        return "";
    }

    // If source_modified provided, validate cache freshness
    if (source_modified > 0) {
        try {
            auto cache_time = std::filesystem::last_write_time(cache_path);
            // Convert file_time_type to time_t for comparison
            // C++20 provides a cleaner way, but this works for C++17
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                cache_time - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            time_t cache_epoch = std::chrono::system_clock::to_time_t(sctp);

            if (cache_epoch < source_modified) {
                spdlog::debug("[ThumbnailCache] Cache stale for {} (cached: {}, source: {})",
                              relative_path, cache_epoch, source_modified);
                // Invalidate by removing the file (const_cast needed for invalidation)
                const_cast<ThumbnailCache*>(this)->invalidate(relative_path);
                return "";
            }
        } catch (const std::filesystem::filesystem_error& e) {
            spdlog::warn("[ThumbnailCache] Failed to check cache age: {}", e.what());
            // On error, assume cache is valid (don't break existing behavior)
        }
    }

    spdlog::debug("[ThumbnailCache] Cache hit for {}", relative_path);
    return to_lvgl_path(cache_path);
}

void ThumbnailCache::set_max_size(size_t max_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_size_ = max_size;
    evict_locked();
}

size_t ThumbnailCache::get_max_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_size_;
}

size_t ThumbnailCache::get_available_disk_space() const {
    // Rate-limited: statfs is a syscall against the backing store, and this is
    // reached twice per fetch_optimized() — once via is_caching_allowed() and
    // once inside evict_locked() — on the main thread while a file listing
    // populates. Free space does not move fast enough to justify probing it per
    // thumbnail; DISK_PROBE_INTERVAL_MS bounds how stale the answer can be.
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(disk_probe_mutex_);
        if (disk_probe_valid_) {
            const auto age =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_disk_probe_);
            if (age.count() < DISK_PROBE_INTERVAL_MS) {
                return cached_available_bytes_;
            }
        }
    }

    size_t available = 0;
    try {
        std::filesystem::space_info space = std::filesystem::space(cache_dir_);
        available = space.available;
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[ThumbnailCache] Failed to query disk space: {}", e.what());
        available = 0;
    }

    std::lock_guard<std::mutex> lock(disk_probe_mutex_);
    cached_available_bytes_ = available;
    last_disk_probe_ = now;
    disk_probe_valid_ = true;
    return available;
}

void ThumbnailCache::invalidate_disk_probe() {
    std::lock_guard<std::mutex> lock(disk_probe_mutex_);
    disk_probe_valid_ = false;
}

ThumbnailCache::DiskPressure ThumbnailCache::get_disk_pressure() const {
    size_t available = get_available_disk_space();

    if (available < disk_critical_) {
        return DiskPressure::Critical;
    } else if (available < disk_low_) {
        return DiskPressure::Low;
    }
    return DiskPressure::Normal;
}

bool ThumbnailCache::is_caching_allowed() const {
    return get_disk_pressure() != DiskPressure::Critical;
}

std::vector<ThumbnailCache::CacheEntry> ThumbnailCache::scan_locked(size_t* total_out) const {
    std::vector<CacheEntry> entries;
    size_t total = 0;

    full_scans_.fetch_add(1, std::memory_order_relaxed);

    std::error_code ec;
    std::filesystem::directory_iterator it(cache_dir_, ec);
    if (ec) {
        spdlog::warn("[ThumbnailCache] Error opening cache dir {}: {}", cache_dir_, ec.message());
        if (total_out) {
            *total_out = 0;
        }
        return entries;
    }

    // Advance with increment(ec), not a range-for: the throwing operator++ would
    // escape this function entirely now that the blanket try/catch is gone, and
    // this runs on HttpExecutor worker threads where an exception crossing back
    // into libhv's callback is not survivable.
    //
    // Every per-entry query likewise uses the error_code overload. A file that
    // vanishes between readdir and stat — the normal case when another thread is
    // evicting, and the reason one try/catch around the whole loop discarded the
    // entire result — costs only its own entry.
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            spdlog::warn("[ThumbnailCache] Stopping cache scan after read error: {}", ec.message());
            break;
        }
        const auto& path = it->path();

        stat_calls_.fetch_add(1, std::memory_order_relaxed);

        std::error_code entry_ec;
        if (!std::filesystem::is_regular_file(path, entry_ec) || entry_ec) {
            if (entry_ec) {
                spdlog::debug("[ThumbnailCache] Skipping unreadable cache entry {}: {}",
                              path.string(), entry_ec.message());
            }
            continue;
        }

        const auto mtime = std::filesystem::last_write_time(path, entry_ec);
        if (entry_ec) {
            spdlog::debug("[ThumbnailCache] Skipping entry with no mtime {}: {}", path.string(),
                          entry_ec.message());
            continue;
        }

        const auto size = std::filesystem::file_size(path, entry_ec);
        if (entry_ec) {
            spdlog::debug("[ThumbnailCache] Skipping entry with no size {}: {}", path.string(),
                          entry_ec.message());
            continue;
        }

        entries.push_back({path, mtime, size});
        total += size;
    }

    if (total_out) {
        *total_out = total;
    }
    return entries;
}

void ThumbnailCache::rescan_locked() const {
    size_t total = 0;
    std::vector<CacheEntry> entries = scan_locked(&total);

    index_.clear();
    for (const auto& entry : entries) {
        // Normalised on the way in, as in index_file_locked() / forget_file_locked().
        // The index is keyed by path, and "dir/x.png" vs "dir//x.png" arriving from
        // two different call sites would read as two files and double-count.
        index_.emplace(entry.path.lexically_normal(), IndexEntry{entry.mtime, entry.size});
    }
    index_total_ = total;
    index_primed_ = true;
    checks_since_scan_ = 0;

    // Anything queued while we were walking is already reflected in what we
    // just read off the filesystem. Keeping it would double-count.
    journal_->reset();
}

void ThumbnailCache::index_file_locked(const std::filesystem::path& raw_path) const {
    const std::filesystem::path path = raw_path.lexically_normal();

    // A journal entry can name a file in a directory this cache no longer owns:
    // ThumbnailProcessor::set_cache_dir() is allowed to move while a write is in
    // flight, and each task reports to the journal that matched its destination.
    // Counting a foreign file here would corrupt the total in the one direction
    // that matters.
    if (path.parent_path() != std::filesystem::path(cache_dir_).lexically_normal()) {
        spdlog::debug("[ThumbnailCache] Ignoring write outside cache dir: {}", path.string());
        return;
    }

    stat_calls_.fetch_add(1, std::memory_order_relaxed);

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        // Written then immediately removed, or never really there. Drop any
        // stale belief rather than inventing a size.
        forget_file_locked(path);
        return;
    }

    std::error_code mtime_ec;
    const auto mtime = std::filesystem::last_write_time(path, mtime_ec);
    if (mtime_ec) {
        forget_file_locked(path);
        return;
    }

    const auto [it, inserted] = index_.try_emplace(path, IndexEntry{mtime, size});
    if (!inserted) {
        // Overwrite of a file we already index — a re-download, or the same
        // pre-scale target regenerated. Replace its contribution, don't add to it.
        index_total_ -= it->second.size;
        it->second = IndexEntry{mtime, size};
    }
    index_total_ += size;
}

void ThumbnailCache::forget_file_locked(const std::filesystem::path& path) const {
    const auto it = index_.find(path.lexically_normal());
    if (it == index_.end()) {
        return;
    }
    index_total_ -= it->second.size;
    index_.erase(it);
}

void ThumbnailCache::refresh_index_locked() const {
    if (!index_primed_) {
        // Cold start. The directory is normally populated by a previous run and
        // nothing in memory has ever seen it.
        rescan_locked();
        return;
    }

    if (++checks_since_scan_ >= INDEX_RECONCILE_INTERVAL) {
        rescan_locked();
        return;
    }

    bool overflowed = false;
    const std::vector<std::string> written = journal_->drain(&overflowed);
    if (overflowed) {
        // More unread writes than the journal will hold, so the list is
        // incomplete and folding it in would leave a permanent shortfall.
        rescan_locked();
        return;
    }

    for (const auto& path : written) {
        index_file_locked(path);
    }
}

void ThumbnailCache::evict_if_needed() {
    std::lock_guard<std::mutex> lock(mutex_);
    evict_locked();
}

void ThumbnailCache::note_write_and_evict(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Index first: a file that reaches the eviction check unindexed is a file
    // the cache is over its limit by without knowing it.
    index_file_locked(path);
    evict_locked();
}

void ThumbnailCache::evict_locked() {
    // No directory walk in the steady state. This used to scan the whole cache
    // — a stat per file — on every fetch(), fetch_optimized() and
    // save_raw_png(), whether or not anything was evicted
    // (prestonbrown/helixscreen#1207). It is now arithmetic over an index kept
    // current by the write sites, plus a reconcile every
    // INDEX_RECONCILE_INTERVAL checks.
    refresh_index_locked();
    size_t current_size = index_total_;

    // Reads only construction-time state (cache_dir_, disk_*), so it does not
    // re-enter the lock.
    DiskPressure pressure = get_disk_pressure();

    // Determine effective limit based on disk pressure
    size_t effective_limit = max_size_;
    const char* reason = nullptr;

    if (pressure == DiskPressure::Critical) {
        // Critical: evict everything possible to free disk space
        effective_limit = 0;
        reason = "disk critically low";
    } else if (pressure == DiskPressure::Low) {
        // Low: reduce cache to half of normal limit
        effective_limit = max_size_ / 2;
        reason = "disk space low";
    }

    if (current_size <= effective_limit) {
        return;
    }

    if (reason) {
        spdlog::warn("[ThumbnailCache] {} (available: {} MB), reducing cache from {} MB to {} MB",
                     reason, get_available_disk_space() / (1024 * 1024),
                     current_size / (1024 * 1024), effective_limit / (1024 * 1024));
    } else {
        spdlog::debug(
            "[ThumbnailCache] Cache size {} MB exceeds limit {} MB, evicting oldest files",
            current_size / (1024 * 1024), effective_limit / (1024 * 1024));
    }

    // Oldest first, off the index rather than off a fresh directory listing.
    // Sorting is the only O(n log n) left, and unlike the walk it is memory,
    // not syscalls — and it happens only when eviction actually fires.
    std::vector<std::pair<std::filesystem::path, IndexEntry>> victims(index_.begin(), index_.end());
    std::sort(victims.begin(), victims.end(),
              [](const auto& a, const auto& b) { return a.second.mtime < b.second.mtime; });

    // Remove oldest files until under limit
    size_t evicted_count = 0;
    size_t evicted_bytes = 0;
    bool saw_ghost = false;
    for (const auto& [path, entry] : victims) {
        if (current_size <= effective_limit) {
            break;
        }

        // Count bytes only when the file actually went away. remove() reports
        // false (not an error) for a path that is already gone, and crediting
        // ourselves for that would stop the loop believing it freed space it
        // did not — leaving the cache over its limit.
        std::error_code rm_ec;
        const bool removed = std::filesystem::remove(path, rm_ec);
        if (rm_ec) {
            spdlog::warn("[ThumbnailCache] Failed to evict {}: {}", path.string(), rm_ec.message());
            continue;
        }

        // Either way the file is not on disk, so it must leave the index. The
        // difference is whether we may credit its bytes as freed: a ghost —
        // something deleted behind the cache's back — frees nothing, it only
        // corrects an over-count. Treating the two the same is what would turn
        // an over-count into over-eviction, and over-eviction is
        // self-reinforcing: everything discarded past the limit gets
        // re-downloaded on the next scroll.
        forget_file_locked(path);
        current_size -= entry.size;
        if (!removed) {
            saw_ghost = true;
            continue;
        }
        evicted_bytes += entry.size;
        ++evicted_count;
    }

    if (evicted_count > 0) {
        spdlog::info("[ThumbnailCache] Evicted {} files ({} KB) to stay under limit", evicted_count,
                     evicted_bytes / 1024);
        // Reached from the main thread AND from HttpExecutor workers, so a
        // crumb here also witnesses eviction overlapping an in-flight decode.
        crash_handler::breadcrumb::note("thumb", "evict", static_cast<long>(evicted_count));
    }

    if (saw_ghost) {
        // Something is deleting from this directory that we do not know about.
        // The arithmetic above already removed the ghosts, but a source of
        // surprise deletes is also a plausible source of surprise writes, so
        // re-establish the total from the filesystem rather than trusting it.
        spdlog::debug("[ThumbnailCache] Index held entries already gone from disk, resyncing");
        rescan_locked();
    }
}

// ============================================================================
// Main-thread callback marshalling
// ============================================================================

ThumbnailCache::SuccessCallback ThumbnailCache::on_main(SuccessCallback cb) {
    if (!cb) {
        return cb;
    }
    return [cb = std::move(cb)](const std::string& path, bool degraded) {
        helix::ui::run_on_main("ThumbnailCache::on_success",
                               [cb, path, degraded]() { cb(path, degraded); });
    };
}

ThumbnailCache::ErrorCallback ThumbnailCache::on_main_err(ErrorCallback cb) {
    if (!cb) {
        return cb;
    }
    return [cb = std::move(cb)](const std::string& error) {
        helix::ui::run_on_main("ThumbnailCache::on_error", [cb, error]() { cb(error); });
    };
}

void ThumbnailCache::fetch(IMoonrakerAPI* api, const std::string& relative_path,
                           SuccessCallback on_success, ErrorCallback on_error) {
    // Marshal once, at the boundary — see on_main() for why per-path wrapping
    // does not hold. Everything below may now deliver from any thread.
    on_success = on_main(std::move(on_success));
    on_error = on_main_err(std::move(on_error));

    if (relative_path.empty()) {
        if (on_error) {
            on_error("Empty thumbnail path");
        }
        return;
    }

    // If already an LVGL path, validate and return immediately
    if (is_lvgl_path(relative_path)) {
        std::string local_path = relative_path.substr(2);
        if (std::filesystem::exists(local_path)) {
            spdlog::debug("[ThumbnailCache] Already LVGL path: {}", relative_path);
            if (on_success) {
                on_success(relative_path, /*degraded=*/false);
            }
        } else if (on_error) {
            on_error("LVGL path file not found: " + local_path);
        }
        return;
    }

    // Check local filesystem first (might be a local file path in mock mode)
    if (std::filesystem::exists(relative_path)) {
        spdlog::debug("[ThumbnailCache] Local file exists: {}", relative_path);
        if (on_success) {
            on_success(to_lvgl_path(relative_path), /*degraded=*/false);
        }
        return;
    }

    // Check cache
    std::string cached = get_if_cached(relative_path);
    if (!cached.empty()) {
        if (on_success) {
            on_success(cached, /*degraded=*/false);
        }
        return;
    }

    // Need to download
    if (!api) {
        if (on_error) {
            on_error("No API available for thumbnail download");
        }
        return;
    }

    // Check disk pressure before downloading
    if (!is_caching_allowed()) {
        spdlog::warn("[ThumbnailCache] Disk critically low, skipping download of {}",
                     relative_path);
        if (on_error) {
            on_error("Disk space critically low - caching disabled");
        }
        return;
    }

    // Evict old files before downloading new one
    evict_if_needed();

    std::string cache_path = get_cache_path(relative_path);
    spdlog::debug("[ThumbnailCache] Downloading {} -> {}", relative_path, cache_path);

    api->transfers().download_thumbnail(
        relative_path, cache_path,
        // Success callback
        [this, on_success, relative_path](const std::string& local_path) {
            spdlog::debug("[ThumbnailCache] Downloaded {} to {}", relative_path, local_path);
            // Runs on an HttpExecutor worker — MoonrakerFileTransferAPI invokes
            // this unmarshalled. Index the byte we just added before deciding
            // whether to evict, or the cache is over its limit by a file it
            // does not know it has.
            note_write_and_evict(local_path);
            if (on_success) {
                on_success(to_lvgl_path(local_path), /*degraded=*/false);
            }
        },
        // Error callback
        [on_error, relative_path](const MoonrakerError& error) {
            spdlog::warn("[ThumbnailCache] Failed to download {}: {}", relative_path,
                         error.message);
            if (on_error) {
                on_error(error.message);
            }
        });
}

std::string ThumbnailCache::save_raw_png(const std::string& source_identifier,
                                         const std::vector<uint8_t>& png_data) {
    if (source_identifier.empty()) {
        spdlog::warn("[ThumbnailCache] Empty source identifier for save_raw_png");
        return "";
    }

    if (png_data.size() < 8) {
        spdlog::warn("[ThumbnailCache] PNG data too small ({} bytes)", png_data.size());
        return "";
    }

    // Validate PNG magic bytes: 89 50 4E 47 0D 0A 1A 0A
    static const uint8_t png_magic[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(png_data.data(), png_magic, sizeof(png_magic)) != 0) {
        spdlog::warn("[ThumbnailCache] Invalid PNG magic bytes in save_raw_png");
        return "";
    }

#if defined(HELIX_PLATFORM_ESP32)
    // No disk-cache materialization on ESP32 (Task 10 R3 hard constraint).
    // This path is reachable ONLY via the gcode-header thumbnail-extraction
    // fallback (ui_panel_print_select.cpp), which is itself desktop-shaped
    // and awaiting Task 11's PSRAM-based redesign — until then, refuse to
    // write and let the existing empty-return handling there degrade
    // gracefully ("Failed to cache extracted thumbnail for ..." + skip), the
    // same fallback contract every other failure branch in this function
    // already uses.
    spdlog::debug("[ThumbnailCache] save_raw_png: no local cache on this platform ({})",
                  source_identifier);
    return "";
#endif

    // Check disk pressure before saving
    if (!is_caching_allowed()) {
        spdlog::warn("[ThumbnailCache] Disk critically low, skipping save of {}",
                     source_identifier);
        return "";
    }

    // Evict old files before saving new one
    evict_if_needed();

    // Generate cache path using same hash scheme as downloaded thumbnails
    std::string cache_path = get_cache_path(source_identifier);

    // Write PNG data to cache file
    std::ofstream file(cache_path, std::ios::binary);
    if (!file) {
        spdlog::error("[ThumbnailCache] Failed to create cache file: {}", cache_path);
        return "";
    }

    file.write(reinterpret_cast<const char*>(png_data.data()),
               static_cast<std::streamsize>(png_data.size()));
    file.close();

    if (!file) {
        spdlog::error("[ThumbnailCache] Failed to write PNG data to {}", cache_path);
        return "";
    }

    spdlog::debug("[ThumbnailCache] Saved {} bytes from gcode extraction: {}", png_data.size(),
                  cache_path);

    // Index the file we just wrote, then check whether it pushed us over.
    note_write_and_evict(cache_path);

    return to_lvgl_path(cache_path);
}

size_t ThumbnailCache::clear_cache() {
    // Shares mutex_ with eviction: both unlink from the same directory, and a
    // concurrent eviction scan would otherwise trip over files this removes.
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
            if (std::filesystem::is_regular_file(entry.path())) {
                std::filesystem::remove(entry.path());
                ++count;
            }
        }
        spdlog::info("[ThumbnailCache] Cleared {} cached thumbnails", count);

        // We just enumerated and emptied the directory, so an empty index is
        // exact — no rescan needed to establish it.
        index_.clear();
        index_total_ = 0;
        index_primed_ = true;
        checks_since_scan_ = 0;
        journal_->reset();
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[ThumbnailCache] Error clearing cache: {}", e.what());
        // Enumeration stopped partway, so we do not know what survived. Force
        // the next accounting pass to find out rather than assert an empty cache
        // it cannot back up.
        index_primed_ = false;
    }
    return count;
}

size_t ThumbnailCache::invalidate(const std::string& relative_path) {
    if (relative_path.empty()) {
        return 0;
    }

    // Same directory, same reason as clear_cache().
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    std::string hash = compute_hash(relative_path);

    try {
        // Delete the PNG file
        std::string png_path = cache_dir_ + "/" + hash + ".png";
        if (std::filesystem::exists(png_path)) {
            std::filesystem::remove(png_path);
            forget_file_locked(png_path);
            ++count;
            spdlog::debug("[ThumbnailCache] Invalidated PNG: {}", png_path);
        }

        // Delete all pre-scaled .bin variants (e.g., {hash}_120x120_RGB565.bin)
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
            if (!std::filesystem::is_regular_file(entry.path())) {
                continue;
            }
            std::string filename = entry.path().filename().string();
            // .bin files are named: {hash}_{w}x{h}_{format}.bin
            std::string prefix = hash + "_";
            bool has_prefix =
                filename.size() >= prefix.size() && filename.compare(0, prefix.size(), prefix) == 0;
            bool has_suffix =
                filename.size() >= 4 && filename.compare(filename.size() - 4, 4, ".bin") == 0;
            if (has_prefix && has_suffix) {
                std::filesystem::remove(entry.path());
                forget_file_locked(entry.path());
                ++count;
                spdlog::debug("[ThumbnailCache] Invalidated BIN: {}", entry.path().string());
            }
        }

        if (count > 0) {
            spdlog::info("[ThumbnailCache] Invalidated {} cached files for {}", count,
                         relative_path);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        spdlog::warn("[ThumbnailCache] Error invalidating cache for {}: {}", relative_path,
                     e.what());
        // Removed an unknown subset before throwing. Over-counting until the
        // next reconcile is safe; asserting a total we cannot justify is not.
        index_primed_ = false;
    }

    return count;
}

size_t ThumbnailCache::get_cache_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    refresh_index_locked();
    return index_total_;
}

// ============================================================================
// Optimized Thumbnail Fetching (Pre-scaling)
// ============================================================================

std::string ThumbnailCache::get_if_optimized(const std::string& relative_path,
                                             const helix::ThumbnailTarget& target,
                                             time_t source_modified) const {
    if (relative_path.empty()) {
        return "";
    }

    // Check for pre-scaled .bin via ThumbnailProcessor
    std::string bin_path =
        helix::ThumbnailProcessor::instance().get_if_processed(relative_path, target);
    if (bin_path.empty()) {
        return "";
    }

    // Validate cache freshness if source_modified provided
    if (source_modified > 0) {
        try {
            // Strip "A:" prefix to get filesystem path
            std::string fs_path = bin_path.substr(2);
            if (!std::filesystem::exists(fs_path)) {
                return "";
            }

            auto cache_time = std::filesystem::last_write_time(fs_path);
            // Convert file_time_type to time_t for comparison
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                cache_time - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            time_t cache_epoch = std::chrono::system_clock::to_time_t(sctp);

            if (cache_epoch < source_modified) {
                spdlog::debug(
                    "[ThumbnailCache] Optimized cache stale for {} (cached: {}, source: {})",
                    relative_path, cache_epoch, source_modified);
                // Invalidate all cached variants (PNG + .bin files)
                const_cast<ThumbnailCache*>(this)->invalidate(relative_path);
                return "";
            }
        } catch (const std::filesystem::filesystem_error& e) {
            spdlog::warn("[ThumbnailCache] Failed to check optimized cache age: {}", e.what());
            // On error, assume cache is valid (don't break existing behavior)
        }
    }

    return bin_path;
}

void ThumbnailCache::fetch_optimized(IMoonrakerAPI* api, const std::string& relative_path,
                                     const helix::ThumbnailTarget& target,
                                     SuccessCallback on_success, ErrorCallback on_error,
                                     time_t source_modified) {
    // Marshal once, at the boundary — see on_main(). This covers the download
    // error path, and the processor-shutdown error that process_and_callback
    // turns back into a success, neither of which is obvious at the call sites.
    on_success = on_main(std::move(on_success));
    on_error = on_main_err(std::move(on_error));

    if (relative_path.empty()) {
        if (on_error) {
            on_error("Empty thumbnail path");
        }
        return;
    }

    // Step 1: Check for pre-scaled .bin (instant return if fresh)
    std::string optimized = get_if_optimized(relative_path, target, source_modified);
    if (!optimized.empty()) {
        spdlog::debug("[ThumbnailCache] Pre-scaled cache hit: {}", optimized);
        if (on_success) {
            on_success(optimized, /*degraded=*/false);
        }
        return;
    }

    // Step 2: Check for cached PNG (with age validation)
    std::string cached_png = get_if_cached(relative_path, source_modified);
    if (!cached_png.empty()) {
        // PNG exists and is fresh, queue for pre-scaling
        spdlog::debug("[ThumbnailCache] PNG cached, queuing pre-scale: {}", relative_path);
        process_and_callback(cached_png, relative_path, target, on_success, on_error);
        return;
    }

    // Step 3: Download PNG, then pre-scale
    if (!api) {
        if (on_error) {
            on_error("No API available for thumbnail download");
        }
        return;
    }

    // Check disk pressure before downloading
    if (!is_caching_allowed()) {
        spdlog::warn("[ThumbnailCache] Disk critically low, skipping optimized fetch of {}",
                     relative_path);
        if (on_error) {
            on_error("Disk space critically low - caching disabled");
        }
        return;
    }

    evict_if_needed();

    std::string cache_path = get_cache_path(relative_path);
    spdlog::debug("[ThumbnailCache] Downloading for optimization: {} -> {}", relative_path,
                  cache_path);
    // Cold fetch — the state the reporter's device was in when it aborted.
    crash_handler::breadcrumb::note("thumb", "fetch_cold", 0);

    // Capture target and callbacks for the download completion handler
    api->transfers().download_thumbnail(
        relative_path, cache_path,
        // Success callback - PNG downloaded, now pre-scale it
        [this, on_success, on_error, relative_path, target](const std::string& local_path) {
            spdlog::debug("[ThumbnailCache] Downloaded, now pre-scaling: {}", local_path);
            // HttpExecutor worker thread — see fetch() for why the write is
            // indexed before the eviction check rather than after it.
            note_write_and_evict(local_path);

            // Process the downloaded PNG
            std::string lvgl_path = to_lvgl_path(local_path);
            process_and_callback(lvgl_path, relative_path, target, on_success, on_error);
        },
        // Error callback - download failed
        [on_error, relative_path](const MoonrakerError& error) {
            spdlog::warn("[ThumbnailCache] Optimized fetch failed for {}: {}", relative_path,
                         error.message);
            if (on_error) {
                on_error(error.message);
            }
        });
}

void ThumbnailCache::process_and_callback(const std::string& png_lvgl_path,
                                          const std::string& source_path,
                                          const helix::ThumbnailTarget& target,
                                          SuccessCallback on_success, ErrorCallback on_error) {
    // This function uses graceful fallback - on failure, it calls on_success with
    // the PNG path instead of calling on_error. The PNG still works, just slower.
    (void)on_error;

    std::string local_path = png_lvgl_path;
    if (is_lvgl_path(local_path)) {
        local_path = local_path.substr(2); // Remove "A:" prefix
    }

    // Hand the PATH to the processor, not the bytes. This runs on the main
    // thread once per file while a listing populates; reading the whole PNG here
    // only to pass it straight to the pool put a synchronous file read on the
    // LVGL loop for every card. process_file_async() does the read on the worker
    // and reports a read failure through on_error, which the lambda below turns
    // back into the same PNG fallback this code always used.
    helix::ThumbnailProcessor::instance().process_file_async(
        local_path, source_path, target,
        // Success - return optimized path
        [on_success](const std::string& lvbin_path) {
            spdlog::debug("[ThumbnailCache] Pre-scaling complete: {}", lvbin_path);
            if (on_success) {
                on_success(lvbin_path, /*degraded=*/false);
            }
        },
        // Error - fallback to PNG. Reported through on_success because the PNG
        // still renders, but flagged degraded so the caller can tell it did not
        // get the pre-scaled .bin it asked for.
        [on_success, png_lvgl_path](const std::string& error) {
            spdlog::warn("[ThumbnailCache] Pre-scaling failed ({}), using PNG fallback", error);
            // Fallback: return PNG path (still works, just slower)
            if (on_success) {
                on_success(png_lvgl_path, /*degraded=*/true);
            }
        });
}

// ============================================================================
// High-Level Semantic Methods
// ============================================================================

void ThumbnailCache::fetch(const ThumbnailRequest& req, ThumbnailLoadContext ctx,
                           SuccessCallback on_success, ErrorCallback on_error) {
    // The caller's success callback runs only if no newer request superseded
    // this one. The target comes from the request rather than being chosen
    // here, which is what lets one method serve every call site — the per-view
    // wrappers this replaced each picked their own and could not be told
    // otherwise.
    auto guarded_success = [ctx, on_success = std::move(on_success)](const std::string& path,
                                                                     bool degraded) {
        if (!ctx.is_valid()) {
            spdlog::debug("[ThumbnailCache] Dropping stale fetch result: {}", path);
            return;
        }
        if (on_success) {
            on_success(path, degraded);
        }
    };

    if (req.format == ThumbnailRequest::ThumbnailFormat::FullPng) {
        // Full-resolution PNG: req.target does not apply, so this bypasses the
        // pre-scaler entirely. A PNG handed back because a PNG was requested is
        // the thing that was asked for, never the degraded fallback that
        // process_and_callback reports when a pre-scale FAILS.
        fetch(
            req.api, req.key,
            [guarded_success](const std::string& path, bool /*degraded*/) {
                guarded_success(path, /*degraded=*/false);
            },
            std::move(on_error));
        return;
    }

    fetch_optimized(req.api, req.key, req.target, std::move(guarded_success), std::move(on_error),
                    req.source_modified);
}

std::string ThumbnailCache::get_if_cached(const ThumbnailRequest& req) const {
    if (req.format == ThumbnailRequest::ThumbnailFormat::FullPng) {
        return get_if_cached(req.key, req.source_modified);
    }
    return get_if_optimized(req.key, req.target, req.source_modified);
}
