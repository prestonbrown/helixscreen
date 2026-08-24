// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thumbnail_pipeline_stress.cpp
 * @brief Concurrent decode/rescale/write against live eviction (#960, #1239).
 *
 * Bundle 6F3QJLFG aborted with `malloc(): unsorted double linked list
 * corrupted` 347 ms after a print started. The file being printed was a fresh
 * upload with no cached thumbnail, and the cache was already over its 20 MB
 * limit — so the cold-fetch pipeline ran three concurrent decode+rescale jobs
 * on a two-worker pool while eviction unlinked files out from under them.
 *
 * test_thumbnail_cache_concurrency.cpp already pins eviction's *accounting*
 * under threads (#1207). What it does not do is run the decoder at the same
 * time. This file covers that half: real PNG bytes through
 * ThumbnailProcessor::process_sync() — stb_image decode, stbir resize, the
 * RGBA→BGRA swap, and the .bin write — on several threads at once, at mixed
 * target sizes, while other threads evict against a limit small enough to keep
 * unlinking continuously.
 *
 * It asserts nothing about eviction arithmetic. Its job is to give ASAN a wide
 * window on the exact allocation traffic the bundle points at; a clean run is
 * evidence, not proof, and a dirty one is the bug.
 *
 * Tagged [slow] — it is a stress loop, not a unit assertion.
 */

#include "../../include/thumbnail_cache.h"
#include "../../include/thumbnail_processor.h"
#include "system/crash_handler.h"

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

/// Same contract as ScopedCacheDir in test_thumbnail_cache_concurrency.cpp:
/// constructing a ThumbnailCache re-points the process-wide ThumbnailProcessor
/// at that cache's directory, so the processor's dir must be restored too or
/// the next test to touch it fails for reasons unrelated to its own subject.
class ScopedCacheDir {
  public:
    explicit ScopedCacheDir(const std::string& tag) {
        const char* orig = std::getenv("HELIX_CACHE_DIR");
        had_orig_ = (orig != nullptr);
        orig_val_ = orig ? orig : "";
        orig_processor_dir_ = helix::ThumbnailProcessor::instance().get_cache_dir();

        root_ = std::filesystem::temp_directory_path() /
                ("helix_thumb_stress_" + tag + "_" + std::to_string(::getpid()));
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

    const std::filesystem::path& path() const {
        return root_;
    }

  private:
    std::filesystem::path root_;
    std::string orig_val_;
    std::string orig_processor_dir_;
    bool had_orig_ = false;
};

/// Read a real PNG off disk. The repo's own assets are used rather than a
/// synthesised image so the decoder sees production-shaped input (interlacing,
/// palette, alpha) instead of the smallest thing that parses.
std::vector<uint8_t> read_file_bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("Concurrent thumbnail decode survives eviction unlinking beneath it",
          "[assets][processor][cache][threading][960][slow]") {
    // Production-shaped PNG input. Skip rather than fail if the asset moves —
    // this test is about the decoder's behaviour under load, and a missing
    // fixture is not a finding about that.
    const auto png_path = std::filesystem::path("assets/images/benchy_thumbnail_white.png");
    const std::vector<uint8_t> png = read_file_bytes(png_path);
    if (png.empty()) {
        WARN("benchy_thumbnail_white.png not readable from CWD - skipping stress run");
        return;
    }

    ScopedCacheDir scoped("decode_evict");

    // Small enough that eviction fires on essentially every pass, which is the
    // state the reporter's device was in (22 MB against a 20 MB limit).
    constexpr size_t CACHE_LIMIT_BYTES = 256 * 1024;
    ThumbnailCache cache(CACHE_LIMIT_BYTES);

    auto& processor = helix::ThumbnailProcessor::instance();
    processor.set_cache_dir(scoped.path().string());

    constexpr int DECODER_THREADS = 4;
    constexpr int EVICTOR_THREADS = 2;
    constexpr int ROUNDS = 40;

    // Mixed targets on purpose: the crash window had 164/200/300 in flight at
    // once, so every worker writes a differently-sized .bin into one directory.
    constexpr uint8_t ARGB8888 = 0x10;
    const std::vector<helix::ThumbnailTarget> targets = {
        {164, 164, ARGB8888}, {200, 200, ARGB8888}, {300, 300, ARGB8888}, {120, 120, ARGB8888}};

    std::atomic<bool> stop{false};
    std::atomic<int> decodes{0};

    std::vector<std::thread> evictors;
    evictors.reserve(EVICTOR_THREADS);
    for (int e = 0; e < EVICTOR_THREADS; ++e) {
        evictors.emplace_back([&] {
            // set_max_size() is the public entry point that runs an eviction
            // pass; same idiom as test_thumbnail_cache_concurrency.cpp. The
            // value never changes, so all churn comes from the pass itself.
            while (!stop.load(std::memory_order_relaxed)) {
                cache.set_max_size(CACHE_LIMIT_BYTES);
            }
        });
    }

    std::vector<std::thread> decoders;
    decoders.reserve(DECODER_THREADS);
    for (int t = 0; t < DECODER_THREADS; ++t) {
        decoders.emplace_back([&, t] {
            for (int r = 0; r < ROUNDS; ++r) {
                const auto& target = targets[static_cast<size_t>((t + r) % targets.size())];
                // Distinct source identifiers so each thread hashes to its own
                // output filename most of the time, and collides sometimes —
                // both paths matter, the colliding one exercises the shared
                // "<path>.tmp" staging name in write_lvgl_bin().
                const std::string source =
                    "stress_" + std::to_string((t + r) % 3) + "_" + std::to_string(r % 5) + ".png";
                auto result = processor.process_sync(png, source, target);
                if (result.success) {
                    decodes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : decoders) {
        th.join();
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : evictors) {
        th.join();
    }

    // The pipeline must actually have run — a run where every decode failed
    // would pass ASAN trivially and prove nothing.
    REQUIRE(decodes.load() > 0);
}

namespace {

/// dump_to_fd() writes with write(2); a pipe is the simplest readable sink.
std::vector<std::string> capture_crumbs() {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    crash_handler::breadcrumb::dump_to_fd(fds[1]);
    ::close(fds[1]);

    std::string buf;
    char chunk[256];
    ssize_t n;
    while ((n = ::read(fds[0], chunk, sizeof(chunk))) > 0) {
        buf.append(chunk, static_cast<size_t>(n));
    }
    ::close(fds[0]);

    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] == '\n') {
            lines.emplace_back(buf.substr(start, i - start));
            start = i + 1;
        }
    }
    return lines;
}

} // namespace

TEST_CASE("Thumbnail decode brackets itself with crumbs", "[assets][processor][crash][960]") {
    // The #960 hypothesis rests on inferring what was in flight at the abort
    // from a POST-RESTART log, which is far too weak to fix against. These
    // crumbs make the next bundle say it outright. Asserted through the real
    // process_sync() path — a hand-rolled note() call would pass even if
    // do_process() had never been instrumented.
    const auto png =
        read_file_bytes(std::filesystem::path("assets/images/benchy_thumbnail_white.png"));
    if (png.empty()) {
        WARN("benchy_thumbnail_white.png not readable from CWD - skipping");
        return;
    }

    ScopedCacheDir scoped("decode_crumbs");
    auto& processor = helix::ThumbnailProcessor::instance();
    processor.set_cache_dir(scoped.path().string());

    for (int i = 0; i < 256; ++i) {
        crash_handler::breadcrumb::note("drain", "drain");
    }

    helix::ThumbnailTarget target{164, 164, 0x10};
    auto result = processor.process_sync(png, "crumbcheck.png", target);
    REQUIRE(result.success);

    auto lines = capture_crumbs();
    bool saw_begin = false, saw_end = false;
    for (const auto& l : lines) {
        if (l.find("thumb decode_begin") != std::string::npos)
            saw_begin = true;
        if (l.find("thumb decode_end") != std::string::npos)
            saw_end = true;
    }
    CHECK(saw_begin);
    CHECK(saw_end);
}
