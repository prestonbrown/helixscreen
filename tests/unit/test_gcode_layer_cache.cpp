// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_layer_cache.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;
using Catch::Approx;

namespace {

// Helper to create test segments
std::vector<ToolpathSegment> make_test_segments(size_t count) {
    std::vector<ToolpathSegment> segs;
    segs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ToolpathSegment s;
        s.start = glm::vec3(i * 1.0f, 0, 0);
        s.end = glm::vec3(i * 1.0f + 1.0f, 0, 0);
        s.is_extrusion = true;
        segs.push_back(s);
    }
    return segs;
}

// Loader that creates segments on demand
auto test_loader(size_t segments_per_layer) {
    return
        [segments_per_layer](size_t layer_index) { return make_test_segments(segments_per_layer); };
}

// Loader that tracks which layers were loaded
auto tracking_loader(std::vector<size_t>& loaded_layers, size_t segments_per_layer) {
    return [&loaded_layers, segments_per_layer](size_t layer_index) {
        loaded_layers.push_back(layer_index);
        return make_test_segments(segments_per_layer);
    };
}

} // namespace

TEST_CASE("GCodeLayerCache basic operations", "[gcode][cache]") {
    // Small budget for testing: 10KB
    GCodeLayerCache cache(10 * 1024);

    SECTION("empty cache has no layers") {
        REQUIRE(cache.cached_layer_count() == 0);
        REQUIRE(cache.memory_usage_bytes() == 0);
        REQUIRE_FALSE(cache.is_cached(0));
    }

    SECTION("get_or_load caches and returns data") {
        auto result = cache.get_or_load(0, test_loader(10));

        REQUIRE(result.segments != nullptr);
        REQUIRE(result.was_hit == false); // First access is a miss
        REQUIRE(result.load_failed == false);
        REQUIRE(result.segments->size() == 10);
        REQUIRE(cache.is_cached(0));
        REQUIRE(cache.cached_layer_count() == 1);
    }

    SECTION("second access is a cache hit") {
        cache.get_or_load(0, test_loader(10));
        auto result = cache.get_or_load(0, test_loader(10));

        REQUIRE(result.was_hit == true);
        REQUIRE(result.segments != nullptr);
    }

    SECTION("hit rate tracking works") {
        cache.reset_stats();

        // 1 miss
        cache.get_or_load(0, test_loader(10));
        // 2 hits
        cache.get_or_load(0, test_loader(10));
        cache.get_or_load(0, test_loader(10));

        auto [hits, misses] = cache.hit_stats();
        REQUIRE(hits == 2);
        REQUIRE(misses == 1);
        REQUIRE(cache.hit_rate() == Catch::Approx(2.0f / 3.0f));
    }
}

TEST_CASE("GCodeLayerCache LRU eviction", "[gcode][cache]") {
    // Budget that fits ~2 layers of 50 segments each
    // 50 segments * 40 bytes = 2KB per layer + 64 overhead ≈ 2.1KB
    // Budget of 5KB should fit ~2 layers
    GCodeLayerCache cache(5 * 1024);

    std::vector<size_t> loaded;

    SECTION("evicts oldest layer when over budget") {
        // Load layers 0, 1, 2 - should evict 0 to make room for 2
        cache.get_or_load(0, tracking_loader(loaded, 50));
        cache.get_or_load(1, tracking_loader(loaded, 50));
        cache.get_or_load(2, tracking_loader(loaded, 50));

        // Layer 0 should have been evicted
        REQUIRE_FALSE(cache.is_cached(0));
        // Layers 1 and 2 should still be cached
        REQUIRE(cache.is_cached(1));
        REQUIRE(cache.is_cached(2));
    }

    SECTION("touching a layer prevents eviction") {
        cache.get_or_load(0, tracking_loader(loaded, 50));
        cache.get_or_load(1, tracking_loader(loaded, 50));

        // Touch layer 0 (makes it most recent)
        cache.get_or_load(0, tracking_loader(loaded, 50));

        // Now add layer 2 - should evict 1, not 0
        cache.get_or_load(2, tracking_loader(loaded, 50));

        REQUIRE(cache.is_cached(0));       // Was touched, kept
        REQUIRE_FALSE(cache.is_cached(1)); // Oldest, evicted
        REQUIRE(cache.is_cached(2));       // Newest
    }

    SECTION("explicit eviction works") {
        cache.get_or_load(0, tracking_loader(loaded, 50));
        REQUIRE(cache.is_cached(0));

        bool evicted = cache.evict(0);
        REQUIRE(evicted);
        REQUIRE_FALSE(cache.is_cached(0));

        // Evicting non-existent layer returns false
        REQUIRE_FALSE(cache.evict(999));
    }
}

TEST_CASE("GCodeLayerCache memory tracking", "[gcode][cache]") {
    GCodeLayerCache cache(100 * 1024); // 100KB

    SECTION("memory usage increases with cached layers") {
        size_t initial = cache.memory_usage_bytes();
        REQUIRE(initial == 0);

        cache.get_or_load(0, test_loader(100));
        size_t after_one = cache.memory_usage_bytes();
        REQUIRE(after_one > initial);

        cache.get_or_load(1, test_loader(100));
        size_t after_two = cache.memory_usage_bytes();
        REQUIRE(after_two > after_one);
    }

    SECTION("clear resets memory usage") {
        cache.get_or_load(0, test_loader(100));
        cache.get_or_load(1, test_loader(100));
        REQUIRE(cache.memory_usage_bytes() > 0);

        cache.clear();
        REQUIRE(cache.memory_usage_bytes() == 0);
        REQUIRE(cache.cached_layer_count() == 0);
    }

    SECTION("set_memory_budget evicts excess") {
        // Start with generous budget
        cache.get_or_load(0, test_loader(100));
        cache.get_or_load(1, test_loader(100));
        cache.get_or_load(2, test_loader(100));
        REQUIRE(cache.cached_layer_count() == 3);

        // Reduce budget to fit only 1 layer
        cache.set_memory_budget(10 * 1024);

        // Should have evicted down to fit
        REQUIRE(cache.cached_layer_count() <= 2);
        REQUIRE(cache.memory_usage_bytes() <= 10 * 1024);
    }
}

TEST_CASE("GCodeLayerCache insert", "[gcode][cache]") {
    GCodeLayerCache cache(10 * 1024);

    SECTION("insert adds layer to cache") {
        auto segments = make_test_segments(20);
        bool success = cache.insert(5, std::move(segments));

        REQUIRE(success);
        REQUIRE(cache.is_cached(5));

        auto result = cache.get_or_load(5, test_loader(0));
        REQUIRE(result.was_hit == true);
        REQUIRE(result.segments->size() == 20);
    }

    SECTION("insert rejects oversized layer") {
        // Try to insert a huge layer
        auto segments = make_test_segments(10000); // Way over 10KB budget

        bool success = cache.insert(0, std::move(segments));
        REQUIRE_FALSE(success);
        REQUIRE_FALSE(cache.is_cached(0));
    }
}

TEST_CASE("GCodeLayerCache prefetch", "[gcode][cache]") {
    GCodeLayerCache cache(100 * 1024);

    std::vector<size_t> loaded;

    SECTION("prefetch loads layers around center") {
        cache.prefetch(5, 2, tracking_loader(loaded, 20), 100);

        // Should have loaded layers 3, 4, 5, 6, 7
        REQUIRE(loaded.size() == 5);
        REQUIRE(cache.is_cached(3));
        REQUIRE(cache.is_cached(4));
        REQUIRE(cache.is_cached(5));
        REQUIRE(cache.is_cached(6));
        REQUIRE(cache.is_cached(7));
    }

    SECTION("prefetch respects max_layer") {
        cache.prefetch(2, 5, tracking_loader(loaded, 20), 4);

        // Should load 0, 1, 2, 3, 4 (not beyond max_layer=4)
        REQUIRE(loaded.size() == 5);
        for (size_t i = 0; i <= 4; ++i) {
            REQUIRE(cache.is_cached(i));
        }
    }

    SECTION("prefetch handles already cached layers efficiently") {
        // Pre-cache layer 5
        cache.get_or_load(5, tracking_loader(loaded, 20));
        loaded.clear();

        cache.prefetch(5, 1, tracking_loader(loaded, 20), 100);

        // Should load 4, 6; layer 5 was already cached so loader won't be called for it
        // (get_or_load returns cached data without calling loader)
        REQUIRE(loaded.size() == 2);
        REQUIRE(std::find(loaded.begin(), loaded.end(), 5) == loaded.end());
    }
}

TEST_CASE("GCodeLayerCache adaptive mode", "[gcode][cache]") {
    GCodeLayerCache cache(100 * 1024);

    SECTION("adaptive mode can be enabled") {
        REQUIRE_FALSE(cache.is_adaptive_mode());

        cache.set_adaptive_mode(true, 15, 1024, 50 * 1024);

        REQUIRE(cache.is_adaptive_mode());
    }

    SECTION("respond_to_pressure evicts entries") {
        // Fill the cache
        for (size_t i = 0; i < 10; ++i) {
            cache.get_or_load(i, test_loader(50));
        }
        size_t before = cache.cached_layer_count();
        REQUIRE(before > 0);

        // Trigger emergency pressure response
        cache.respond_to_pressure(0.1f); // Reduce to 10% of budget

        size_t after = cache.cached_layer_count();
        REQUIRE(after < before);
    }

    SECTION("check_memory_pressure rate limits") {
        cache.set_adaptive_mode(true);

        // First check should work
        // (Note: actual adjustment depends on system memory, so we just check it doesn't crash)
        cache.check_memory_pressure();

        // Immediate second check should be skipped (rate limited)
        int64_t ms = cache.ms_since_last_pressure_check();
        REQUIRE(ms < 100); // Should be very recent
    }
}

TEST_CASE("GCodeLayerCache thread safety", "[gcode][cache][thread][slow]") {
    GCodeLayerCache cache(100 * 1024);

    SECTION("concurrent reads don't crash") {
        // Pre-populate
        cache.get_or_load(0, test_loader(50));

        // Catch2's assertion macros are not thread-safe: they mutate shared
        // per-assertion state, so calling REQUIRE from these worker threads is
        // itself a data race (TSan reported it as concurrent writes from two
        // worker threads at the REQUIRE). Record the outcome atomically and
        // assert once, back on the main thread.
        std::atomic<bool> all_loaded{true};
        std::vector<std::thread> threads;
        for (int i = 0; i < 10; ++i) {
            threads.emplace_back([&cache, &all_loaded]() {
                for (int j = 0; j < 100; ++j) {
                    auto result = cache.get_or_load(0, test_loader(50));
                    if (result.segments == nullptr) {
                        all_loaded.store(false, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }
        REQUIRE(all_loaded.load(std::memory_order_relaxed));

        REQUIRE(cache.is_cached(0));
    }

    SECTION("concurrent reads and writes don't crash") {
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;

        // Reader threads
        for (int i = 0; i < 5; ++i) {
            threads.emplace_back([&cache, &stop]() {
                while (!stop.load()) {
                    cache.get_or_load(rand() % 20, test_loader(20));
                }
            });
        }

        // Writer thread (evictions)
        threads.emplace_back([&cache, &stop]() {
            while (!stop.load()) {
                cache.evict(rand() % 20);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        // Let it run briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop.store(true);

        for (auto& t : threads) {
            t.join();
        }
    }
}

// =============================================================================
// Main-thread stall regression (bundle C2CP6ZAW)
//
// get_or_load() used to hold mutex_ across the loader() call, which does a file
// seek plus a G-code parse. The background ghost builder walks every layer
// through that same path with only a 1ms yield, so the LVGL main thread — which
// reaches the cache from render() AND from the touch hit-test pick_object_at() —
// serialised behind an unrelated layer's disk read. On a 2-core armv7 K2 that
// showed up as multi-second touch latency until the reporter forced
// thumbnail-only rendering.
//
// The contract these pin: work on layer A must never delay work on layer B.
// =============================================================================

namespace {

// Loader that blocks for a fixed duration, simulating a slow seek + parse.
auto slow_loader(std::chrono::milliseconds delay, size_t segments_per_layer,
                 std::atomic<bool>* entered = nullptr) {
    return [delay, segments_per_layer, entered](size_t /*layer_index*/) {
        if (entered) {
            entered->store(true);
        }
        std::this_thread::sleep_for(delay);
        return make_test_segments(segments_per_layer);
    };
}

constexpr auto SLOW_LOAD = std::chrono::milliseconds(400);

// Generous enough to absorb CI scheduling noise while still being far below
// SLOW_LOAD — the bug produces ~SLOW_LOAD, the fix produces ~0.
constexpr auto MUST_NOT_BLOCK = std::chrono::milliseconds(150);

} // namespace

TEST_CASE("GCodeLayerCache does not serialise independent layers",
          "[gcode][cache][threading][slow]") {
    GCodeLayerCache cache(1024 * 1024);

    SECTION("a cached layer stays reachable while another layer is loading") {
        // Layer 1 is already resident, so this lookup does zero I/O and its only
        // possible source of delay is lock contention.
        REQUIRE(cache.insert(1, make_test_segments(10)));

        std::atomic<bool> slow_started{false};
        std::thread loader_thread(
            [&] { cache.get_or_load(0, slow_loader(SLOW_LOAD, 10, &slow_started)); });

        while (!slow_started.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const auto start = std::chrono::steady_clock::now();
        auto hit = cache.get_or_load(1, test_loader(10));
        const auto elapsed = std::chrono::steady_clock::now() - start;

        loader_thread.join();

        REQUIRE(hit.was_hit);
        REQUIRE(hit.segments != nullptr);
        CHECK(elapsed < MUST_NOT_BLOCK);
    }

    SECTION("two different layers load concurrently, not back to back") {
        std::atomic<bool> first_started{false};

        const auto start = std::chrono::steady_clock::now();
        std::thread a([&] { cache.get_or_load(0, slow_loader(SLOW_LOAD, 10, &first_started)); });

        while (!first_started.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::thread b([&] { cache.get_or_load(1, slow_loader(SLOW_LOAD, 10)); });
        a.join();
        b.join();
        const auto elapsed = std::chrono::steady_clock::now() - start;

        // Serialised: ~2x SLOW_LOAD. Overlapped: ~1x. Anything under 1.5x proves
        // the second load was not waiting on the first one's lock.
        CHECK(elapsed < SLOW_LOAD * 3 / 2);
        REQUIRE(cache.is_cached(0));
        REQUIRE(cache.is_cached(1));
    }
}

TEST_CASE("GCodeLayerCache try_get never loads", "[gcode][cache][threading][slow]") {
    GCodeLayerCache cache(1024 * 1024);

    SECTION("miss returns null without invoking a loader") {
        // try_get takes no loader at all, so a miss cannot do I/O by construction.
        // The assertion is that a miss is reported rather than filled.
        REQUIRE(cache.try_get(7) == nullptr);
        REQUIRE_FALSE(cache.is_cached(7));
    }

    SECTION("hit returns the cached segments") {
        REQUIRE(cache.insert(3, make_test_segments(12)));

        auto segments = cache.try_get(3);
        REQUIRE(segments != nullptr);
        CHECK(segments->size() == 12);
    }

    SECTION("does not block behind an in-flight load of another layer") {
        REQUIRE(cache.insert(1, make_test_segments(10)));

        std::atomic<bool> slow_started{false};
        std::thread loader_thread(
            [&] { cache.get_or_load(0, slow_loader(SLOW_LOAD, 10, &slow_started)); });

        while (!slow_started.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const auto start = std::chrono::steady_clock::now();
        auto segments = cache.try_get(1);
        const auto elapsed = std::chrono::steady_clock::now() - start;

        loader_thread.join();

        REQUIRE(segments != nullptr);
        CHECK(elapsed < MUST_NOT_BLOCK);
    }
}

TEST_CASE("GCodeLayerCache clear waits for in-flight loads", "[gcode][cache][threading][slow]") {
    // Callers destroy the data source right after clear() (see
    // GCodeStreamingController::close()). A loader closure captures that source,
    // so clear() returning while a load is still running would leave the loader
    // reading a freed file. Dropping the lock around loader() removed the
    // implicit barrier that used to guarantee this; the explicit drain replaces
    // it, and this pins it.
    GCodeLayerCache cache(1024 * 1024);

    std::atomic<bool> load_running{false};
    std::atomic<bool> load_finished{false};

    std::thread loader_thread([&] {
        cache.get_or_load(0, [&](size_t) {
            load_running.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            load_finished.store(true);
            return make_test_segments(10);
        });
    });

    while (!load_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    cache.clear();

    // The load must have completed before clear() returned.
    CHECK(load_finished.load());

    loader_thread.join();
}

TEST_CASE("GCodeLayerCache coalesces concurrent loads of one layer",
          "[gcode][cache][threading][slow]") {
    GCodeLayerCache cache(1024 * 1024);

    // Dropping the lock around loader() must not turn N concurrent requests for
    // the same layer into N duplicate parses — that would trade a stall for
    // wasted I/O on exactly the devices this is meant to help.
    std::atomic<int> load_count{0};
    auto counting_slow_loader = [&load_count](size_t /*layer_index*/) {
        load_count.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return make_test_segments(10);
    };

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            auto result = cache.get_or_load(0, counting_slow_loader);
            REQUIRE(result.segments != nullptr);
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    CHECK(load_count.load() == 1);
    REQUIRE(cache.is_cached(0));
}
