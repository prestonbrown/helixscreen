// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thumbnail_cache_concurrency.cpp
 * @brief Eviction accounting and concurrency tests (prestonbrown/helixscreen#1207).
 *
 * evict_if_needed() is reached from the main thread and from every HttpExecutor
 * worker, because MoonrakerFileTransferAPI invokes download_thumbnail's success
 * callback on the pool thread. Measured on real hardware: 104 eviction events
 * across 5 threads in 90 seconds, up to 4 in the same millisecond.
 *
 * The failure that produces is an accounting one. Both directory walks used a
 * single try/catch wrapped around the whole loop, so ONE entry that cannot be
 * stat'd discards the entire result:
 *
 *   - get_cache_size() returns the partial sum accumulated before the throw,
 *     under-reporting the cache — so eviction concludes there is nothing to do.
 *   - evict_if_needed()'s scan returns outright, skipping the pass entirely.
 *
 * A concurrent unlink is exactly what makes an entry un-stat'able mid-walk, which
 * is why this only showed up under load. The tests below reproduce that
 * deterministically with a symlink loop (ELOOP is a genuine stat error, unlike a
 * dangling link, which reports as not_found and is skipped cleanly), then a
 * threaded case pins the invariant that eviction is not best-effort.
 */

#include "../../include/thumbnail_cache.h"
#include "../../include/thumbnail_processor.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

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
                ("helix_thumb_conc_" + tag + "_" + std::to_string(::getpid()));
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
/// the oldest-first eviction order is well defined.
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
/// ThumbnailCache, so an assertion about eviction cannot be satisfied by the
/// same accounting bug it is meant to catch.
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

/// Create a pair of symlinks pointing at each other. Resolving either yields
/// ELOOP, which std::filesystem reports as a filesystem_error rather than as
/// not_found — the same shape of failure a concurrent unlink causes mid-walk.
void make_unstattable_entry(const std::string& dir, const std::string& stem) {
    const auto a = std::filesystem::path(dir) / (stem + "_a.png");
    const auto b = std::filesystem::path(dir) / (stem + "_b.png");
    std::error_code ec;
    std::filesystem::remove(a, ec);
    std::filesystem::remove(b, ec);
    std::filesystem::create_symlink(b, a);
    std::filesystem::create_symlink(a, b);
}

} // namespace

TEST_CASE("Cache size accounting survives an entry that cannot be stat'd",
          "[assets][cache][1207]") {
    ScopedCacheDir scoped("size_partial");

    constexpr size_t FILE_BYTES = 16 * 1024;
    constexpr int FILE_COUNT = 32;
    constexpr size_t EXPECTED = FILE_BYTES * FILE_COUNT;

    ThumbnailCache cache(4 * EXPECTED); // limit high enough that nothing evicts
    const std::string dir = cache.get_cache_dir();
    seed_cache_files(dir, FILE_COUNT, FILE_BYTES);

    // Several interleaved bad entries: directory_iterator order is unspecified,
    // so one alone could happen to sort last and hide the truncation.
    make_unstattable_entry(dir, "bad0");
    make_unstattable_entry(dir, "bad1");
    make_unstattable_entry(dir, "bad2");

    // A single un-stat'able entry must cost only that entry, not every byte
    // counted after it. Wrapping the whole walk in one try/catch returns the
    // partial sum, which reads as "cache is smaller than it is" and silently
    // disables eviction.
    REQUIRE(cache.get_cache_size() == EXPECTED);
}

TEST_CASE("Eviction still runs when the cache holds an entry that cannot be stat'd",
          "[assets][cache][1207]") {
    ScopedCacheDir scoped("evict_partial");

    constexpr size_t FILE_BYTES = 16 * 1024;
    constexpr int FILE_COUNT = 64;
    constexpr size_t LIMIT = 16 * FILE_BYTES; // 4x oversubscribed

    ThumbnailCache cache(LIMIT);
    const std::string dir = cache.get_cache_dir();
    seed_cache_files(dir, FILE_COUNT, FILE_BYTES);
    make_unstattable_entry(dir, "bad0");
    make_unstattable_entry(dir, "bad1");

    cache.set_max_size(LIMIT);

    // Measured off the filesystem, not via get_cache_size(): the bug under test
    // makes that method under-report, which would satisfy this assertion for the
    // wrong reason. The scan abandoning the pass on the first bad entry means the
    // cache grows without bound while the log shows a single warning.
    const size_t final_size = true_dir_size(dir);
    INFO("true on-disk size: " << final_size << " limit: " << LIMIT);
    REQUIRE(final_size <= LIMIT);
}

TEST_CASE("Concurrent eviction leaves the cache at or below its limit",
          "[assets][cache][threading][1207][slow]") {
    ScopedCacheDir scoped("evict_threads");

    constexpr size_t FILE_BYTES = 16 * 1024;
    constexpr int FILE_COUNT = 64;
    constexpr size_t LIMIT = 16 * FILE_BYTES;

    ThumbnailCache cache(LIMIT);
    seed_cache_files(cache.get_cache_dir(), FILE_COUNT, FILE_BYTES);
    REQUIRE(cache.get_cache_size() > LIMIT);

    // set_max_size() is the public entry point that runs an eviction pass. Every
    // thread sets the SAME value, so any difference in outcome comes from the
    // interleaving rather than from disagreeing limits. This also exercises the
    // unsynchronized max_size_ write the issue calls out as a plain data race.
    constexpr int THREADS = 6;
    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
            ready.fetch_add(1);
            while (ready.load() < THREADS) {
            }
            for (int i = 0; i < 10; ++i) {
                cache.set_max_size(LIMIT);
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    REQUIRE(cache.get_cache_size() <= LIMIT);
}

TEST_CASE("Cache size query is safe while eviction runs on other threads",
          "[assets][cache][threading][1207][slow]") {
    ScopedCacheDir scoped("readers");

    constexpr size_t FILE_BYTES = 8 * 1024;
    constexpr int FILE_COUNT = 96;
    constexpr size_t LIMIT = 24 * FILE_BYTES;

    ThumbnailCache cache(LIMIT);
    seed_cache_files(cache.get_cache_dir(), FILE_COUNT, FILE_BYTES);

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                (void)cache.get_cache_size();
                reads.fetch_add(1);
            }
        });
    }

    std::vector<std::thread> evictors;
    for (int e = 0; e < 3; ++e) {
        evictors.emplace_back([&] {
            for (int i = 0; i < 20; ++i) {
                cache.set_max_size(LIMIT);
            }
        });
    }
    for (auto& th : evictors) {
        th.join();
    }
    stop.store(true);
    for (auto& th : readers) {
        th.join();
    }

    REQUIRE(reads.load() > 0);
    REQUIRE(cache.get_cache_size() <= LIMIT);
}
