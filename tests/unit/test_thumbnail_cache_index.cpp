// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thumbnail_cache_index.cpp
 * @brief In-memory cache index: cost and correctness (prestonbrown/helixscreen#1207).
 *
 * Defect 2 of #1207. evict_if_needed() runs on every fetch(), fetch_optimized()
 * and save_raw_png(), and each call walks the whole cache directory with a stat
 * per file. Scrolling Print Select over 850 files x 3 thumbnails made that a
 * per-scroll-tick cost on the main thread. Serializing eviction behind a mutex
 * (b0db2b957) removed the duplicate work but not the walk.
 *
 * Nothing measured the walk, which is why it stayed invisible — so these tests
 * assert on ThumbnailCache::get_full_scan_count() / get_stat_count() directly.
 *
 * The hard part is not the index, it is keeping it honest. Three ways it can
 * drift, each pinned below:
 *
 *   - files appear behind the cache's back. ThumbnailProcessor writes the
 *     pre-scaled `.bin` files into ThumbnailCache's own directory and reports to
 *     nobody. An index that only sees ThumbnailCache's writes under-counts, and
 *     an under-counting cache silently stops evicting — the exact failure mode
 *     b0db2b957 fixed for a different reason.
 *   - files vanish behind the cache's back. Over-counting is the safe direction
 *     (it evicts too eagerly) but it must still resynchronise.
 *   - a populated directory at startup, which the index has never observed.
 *
 * A silently drifting index is worse than an honest slow walk, so every one of
 * those has an assertion here rather than a comment.
 */

#include "../../include/thumbnail_cache.h"
#include "../../include/thumbnail_processor.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Smallest valid PNG the cache and the processor will both accept: a 10x10
/// solid-colour square, 75 bytes. Same bytes as tests/unit/test_thumbnail_scaling.cpp.
// clang-format off
const std::vector<uint8_t> TINY_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x02, 0x50, 0x58, 0xEA, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x68, 0x70, 0x50, 0xC0,
    0x83, 0x18, 0x46, 0xA5, 0xB1, 0x21, 0x00, 0x24, 0x51, 0x57, 0x81, 0xF7,
    0xEC, 0xA3, 0x23, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
    0x42, 0x60, 0x82};
// clang-format on

/// Redirects ThumbnailCache's cache dir to a private temp tree for the life of
/// the object, then puts everything back.
///
/// Restoring HELIX_CACHE_DIR alone is NOT enough. Constructing any ThumbnailCache
/// re-points the global ThumbnailProcessor at that cache's directory, so a test
/// that builds one and then deletes its temp tree leaves the process-wide
/// processor aimed at a directory that no longer exists — and the next test to
/// touch it fails for reasons that look nothing like its own subject. Capture and
/// restore the processor's directory as well.
class ScopedCacheDir {
  public:
    explicit ScopedCacheDir(const std::string& tag) {
        const char* orig = std::getenv("HELIX_CACHE_DIR");
        had_orig_ = (orig != nullptr);
        orig_val_ = orig ? orig : "";
        orig_processor_dir_ = helix::ThumbnailProcessor::instance().get_cache_dir();

        root_ = std::filesystem::temp_directory_path() /
                ("helix_thumb_idx_" + tag + "_" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_);
        setenv("HELIX_CACHE_DIR", root_.c_str(), 1);
    }

    ~ScopedCacheDir() {
        if (had_orig_) {
            setenv("HELIX_CACHE_DIR", orig_val_.c_str(), 1);
        } else {
            unsetenv("HELIX_CACHE_DIR");
        }
        helix::ThumbnailProcessor::instance().set_cache_dir(orig_processor_dir_);
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

  private:
    std::filesystem::path root_;
    std::string orig_val_;
    std::string orig_processor_dir_;
    bool had_orig_ = false;
};

/// Write @p count files of @p bytes each into @p dir with staggered mtimes, so
/// the oldest-first eviction order is well defined. These land in the directory
/// without ThumbnailCache observing them — the cold-start case.
void seed_cache_files(const std::string& dir, int count, size_t bytes) {
    const std::string payload(bytes, 'x');
    for (int i = 0; i < count; ++i) {
        const auto path = std::filesystem::path(dir) / ("seed_" + std::to_string(i) + ".png");
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f.good());
        f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        f.close();
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() +
                                                   std::chrono::seconds(i));
    }
}

/// Sum the readable regular files in @p dir without going through
/// ThumbnailCache, so an assertion about the index cannot be satisfied by the
/// index's own arithmetic.
size_t true_dir_size(const std::string& dir) {
    size_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code e2;
        if (!std::filesystem::is_regular_file(entry.path(), e2) || e2) {
            continue;
        }
        const auto sz = std::filesystem::file_size(entry.path(), e2);
        if (!e2) {
            total += sz;
        }
    }
    return total;
}

/// Delete @p count of the seeded files directly, bypassing ThumbnailCache.
size_t delete_seed_files_externally(const std::string& dir, int first, int count) {
    size_t freed = 0;
    for (int i = first; i < first + count; ++i) {
        const auto path = std::filesystem::path(dir) / ("seed_" + std::to_string(i) + ".png");
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path, ec);
        if (ec) {
            continue;
        }
        if (std::filesystem::remove(path, ec)) {
            freed += sz;
        }
    }
    return freed;
}

helix::ThumbnailTarget target_120() {
    helix::ThumbnailTarget t;
    t.width = 120;
    t.height = 120;
    return t;
}

constexpr size_t FILE_BYTES = 16 * 1024;

/// Comfortably above anything these tests put on disk, so eviction never fires
/// and the only thing under test is the accounting.
constexpr size_t GENEROUS_LIMIT = 64 * 1024 * 1024;

} // namespace

// ============================================================================
// Cost — the actual subject of defect 2
// ============================================================================

TEST_CASE("Cold start walks the cache directory exactly once", "[assets][cache][thumbnail][1207]") {
    ScopedCacheDir scoped("cold_start");

    constexpr int FILE_COUNT = 24;
    constexpr size_t EXPECTED = FILE_BYTES * FILE_COUNT;

    ThumbnailCache cache(GENEROUS_LIMIT);
    const std::string dir = cache.get_cache_dir();

    // Construction must not walk: the directory is normally populated from a
    // previous run, and paying for it before anyone asks is the cost we are
    // removing, not adding.
    REQUIRE(cache.get_full_scan_count() == 0);

    seed_cache_files(dir, FILE_COUNT, FILE_BYTES);

    // First query has no choice but to walk — nothing has observed this
    // directory. It must report the truth, not a partial sum.
    REQUIRE(cache.get_cache_size() == EXPECTED);
    REQUIRE(cache.get_full_scan_count() == 1);

    // Second query answers from the index. This is the whole point: repeat
    // queries against an unchanged cache cost no syscalls.
    REQUIRE(cache.get_cache_size() == EXPECTED);
    REQUIRE(cache.get_full_scan_count() == 1);
}

TEST_CASE("Steady-state eviction checks do not re-walk the cache directory",
          "[assets][cache][thumbnail][1207]") {
    ScopedCacheDir scoped("steady_state");

    constexpr int SEED_COUNT = 40;

    ThumbnailCache cache(GENEROUS_LIMIT);
    const std::string dir = cache.get_cache_dir();
    seed_cache_files(dir, SEED_COUNT, FILE_BYTES);

    // Prime the index off the pre-existing directory.
    REQUIRE(cache.get_cache_size() == FILE_BYTES * SEED_COUNT);
    const size_t walks_before = cache.get_full_scan_count();
    const size_t stats_before = cache.get_stat_count();

    // save_raw_png() is the cheapest public path that runs eviction checks — it
    // calls evict_if_needed() before and after the write, the same shape
    // fetch()/fetch_optimized() use around a download. Nothing here is anywhere
    // near the limit, so every one of these checks should be pure arithmetic.
    constexpr int WRITES = 30;
    for (int i = 0; i < WRITES; ++i) {
        const std::string lvgl = cache.save_raw_png("index_steady_" + std::to_string(i), TINY_PNG);
        REQUIRE(!lvgl.empty());
    }

    const size_t walks = cache.get_full_scan_count() - walks_before;
    const size_t stats = cache.get_stat_count() - stats_before;

    INFO("full directory walks across " << WRITES << " writes (" << (2 * WRITES)
                                        << " eviction checks): " << walks);
    INFO("files stat'd: " << stats);

    // 2 * WRITES eviction checks. The old shape walked on every one of them;
    // an index-backed cache should need none, and is allowed a small allowance
    // for a periodic reconciliation pass.
    REQUIRE(walks <= 2);

    // Independent of the walk count: the per-file stat cost must scale with the
    // number of files *written*, not with the size of the cache. One stat per
    // new file, plus whatever a reconcile pass costs.
    REQUIRE(stats <= 4 * static_cast<size_t>(WRITES));

    // Cheap must not mean wrong. The index has to have absorbed its own writes.
    REQUIRE(cache.get_cache_size() == true_dir_size(dir));
}

// ============================================================================
// Drift — files appearing behind the cache's back
// ============================================================================

TEST_CASE("A .bin written by ThumbnailProcessor is accounted for without a rescan",
          "[assets][cache][thumbnail][1207]") {
    ScopedCacheDir scoped("bin_drift");

    constexpr int SEED_COUNT = 8;
    constexpr size_t SEEDED = FILE_BYTES * SEED_COUNT;

    ThumbnailCache cache(GENEROUS_LIMIT);
    const std::string dir = cache.get_cache_dir();
    seed_cache_files(dir, SEED_COUNT, FILE_BYTES);

    REQUIRE(cache.get_cache_size() == SEEDED);
    const size_t walks_before = cache.get_full_scan_count();

    // Constructing a ThumbnailCache re-points the processor at its directory,
    // which is precisely why the .bin files land in the cache's accounting
    // domain while being written by code the cache never calls.
    auto& processor = helix::ThumbnailProcessor::instance();
    REQUIRE(processor.get_cache_dir() == dir);

    const auto result = processor.process_sync(TINY_PNG, "index_bin_source.png", target_120());
    REQUIRE(result.success);
    REQUIRE(ThumbnailCache::is_lvgl_path(result.output_path));

    const std::string bin_fs_path = result.output_path.substr(2);
    std::error_code ec;
    const size_t bin_bytes = std::filesystem::file_size(bin_fs_path, ec);
    REQUIRE(!ec);
    REQUIRE(bin_bytes > 0);

    // Correctness: the cache must see bytes it did not write itself. An index
    // that misses these under-counts, and an under-counting cache stops
    // evicting entirely.
    REQUIRE(cache.get_cache_size() == SEEDED + bin_bytes);

    // Cost: and it must learn about them without re-walking the directory,
    // otherwise the index has bought nothing on the pre-scaling path — which is
    // the path every card view takes.
    REQUIRE(cache.get_full_scan_count() == walks_before);
}

TEST_CASE("Eviction honours the limit when pre-scaled .bin files dominate the cache",
          "[assets][cache][thumbnail][1207]") {
    ScopedCacheDir scoped("bin_evict");

    ThumbnailCache cache(GENEROUS_LIMIT);
    const std::string dir = cache.get_cache_dir();

    auto& processor = helix::ThumbnailProcessor::instance();
    REQUIRE(processor.get_cache_dir() == dir);

    constexpr int BINS = 6;
    for (int i = 0; i < BINS; ++i) {
        const auto result = processor.process_sync(
            TINY_PNG, "index_evict_" + std::to_string(i) + ".png", target_120());
        REQUIRE(result.success);
    }

    const size_t on_disk = true_dir_size(dir);
    REQUIRE(on_disk > 0);

    // Half of what the processor actually wrote. If the index does not know
    // about .bin files, it reports a cache of zero bytes and evicts nothing.
    const size_t limit = on_disk / 2;
    cache.set_max_size(limit);

    INFO("true on-disk size after eviction: " << true_dir_size(dir) << " limit: " << limit);
    REQUIRE(true_dir_size(dir) <= limit);
}

// ============================================================================
// Drift — files vanishing behind the cache's back
// ============================================================================

TEST_CASE("An external delete never makes the cache under-report",
          "[assets][cache][thumbnail][1207]") {
    ScopedCacheDir scoped("ext_delete");

    constexpr int FILE_COUNT = 32;

    ThumbnailCache cache(GENEROUS_LIMIT);
    const std::string dir = cache.get_cache_dir();
    seed_cache_files(dir, FILE_COUNT, FILE_BYTES);
    REQUIRE(cache.get_cache_size() == FILE_BYTES * FILE_COUNT);

    const size_t freed = delete_seed_files_externally(dir, 0, 8);
    REQUIRE(freed == 8 * FILE_BYTES);

    // Over-counting is the survivable direction — it evicts too eagerly.
    // Under-counting is not: it reads as "well under the limit" and disables
    // eviction, which is the failure b0db2b957 fixed via a different route.
    REQUIRE(cache.get_cache_size() >= true_dir_size(dir));

    // And it must not stay wrong forever. Any reconciliation period the
    // implementation picks has to be well inside this many checks.
    for (int i = 0; i < 512; ++i) {
        cache.set_max_size(GENEROUS_LIMIT);
    }
    REQUIRE(cache.get_cache_size() == true_dir_size(dir));
}

TEST_CASE("Entries deleted externally do not cause over-eviction below the limit",
          "[assets][cache][thumbnail][1207]") {
    ScopedCacheDir scoped("ghost_evict");

    constexpr int FILE_COUNT = 64;
    constexpr size_t LIMIT = 32 * FILE_BYTES;

    ThumbnailCache cache(GENEROUS_LIMIT);
    const std::string dir = cache.get_cache_dir();
    seed_cache_files(dir, FILE_COUNT, FILE_BYTES);
    REQUIRE(cache.get_cache_size() == FILE_BYTES * FILE_COUNT);

    // 20 ghosts: entries the index believes in that are no longer on disk.
    // Oldest-first eviction hits them before anything real, so an index that
    // credits itself for removing them frees nothing and keeps going.
    REQUIRE(delete_seed_files_externally(dir, 0, 20) == 20 * FILE_BYTES);

    cache.set_max_size(LIMIT);

    const size_t after = true_dir_size(dir);
    INFO("true on-disk size: " << after << " limit: " << LIMIT);
    REQUIRE(after <= LIMIT);

    // Over-eviction is self-reinforcing — everything discarded past the limit
    // gets re-downloaded on the next scroll. 44 real files remained and the
    // limit holds 32 of them, so a correct pass leaves the cache full, not empty.
    REQUIRE(after > LIMIT / 2);

    // Having tripped over ghosts, the index must resynchronise rather than
    // carry a permanent offset.
    REQUIRE(cache.get_cache_size() == true_dir_size(dir));
}

TEST_CASE("A writer with no hook at all still converges within the reconcile bound",
          "[assets][cache][thumbnail][1207]") {
    ScopedCacheDir scoped("foreign_writer");

    constexpr int SEED_COUNT = 8;

    ThumbnailCache cache(GENEROUS_LIMIT);
    const std::string dir = cache.get_cache_dir();
    seed_cache_files(dir, SEED_COUNT, FILE_BYTES);
    REQUIRE(cache.get_cache_size() == FILE_BYTES * SEED_COUNT);

    // Not the cache, not the processor — a hypothetical future writer, or a
    // human with a shell. The index cannot know about this synchronously, and
    // claiming otherwise would be the dishonest version of this feature. What
    // it must do is notice within a bounded number of checks.
    const auto foreign = std::filesystem::path(dir) / "foreign_writer_artifact.png";
    {
        const std::string payload(64 * 1024, 'z');
        std::ofstream f(foreign, std::ios::binary);
        REQUIRE(f.good());
        f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    for (int i = 0; i < 512; ++i) {
        cache.set_max_size(GENEROUS_LIMIT);
    }

    REQUIRE(cache.get_cache_size() == true_dir_size(dir));
    REQUIRE(cache.get_cache_size() == FILE_BYTES * SEED_COUNT + 64 * 1024);
}
