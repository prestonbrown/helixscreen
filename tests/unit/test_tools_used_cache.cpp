// SPDX-License-Identifier: GPL-3.0-or-later

#include "tools_used_cache.h"

#include <cstdlib>
#include <filesystem>
#include <set>

#include "../catch_amalgamated.hpp"

namespace {
// Per-TEST_SECTION temp cache dir; HELIX_CACHE_DIR is the documented override
// for get_helix_cache_dir() (src/app_globals.cpp:424). MUST save/restore the
// previous value — leaking it would redirect every later test's cache writes
// in this binary (tests within one Catch2 binary run sequentially, but they
// share the process env).
struct CacheDirGuard {
    std::filesystem::path dir;
    std::string prev_env_;
    bool had_prev_ = false;
    CacheDirGuard()
        : dir(std::filesystem::temp_directory_path() /
              ("tools_used_test_" + std::to_string(::getpid()))) {
        std::filesystem::create_directories(dir);
        if (const char* old = ::getenv("HELIX_CACHE_DIR")) {
            prev_env_ = old;
            had_prev_ = true;
        }
        setenv("HELIX_CACHE_DIR", dir.c_str(), 1);
    }
    ~CacheDirGuard() {
        if (had_prev_) {
            setenv("HELIX_CACHE_DIR", prev_env_.c_str(), 1);
        } else {
            unsetenv("HELIX_CACHE_DIR");
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
} // namespace

TEST_CASE("ToolsUsedCache round trip + staleness", "[tools_used_cache]") {
    CacheDirGuard guard;
    helix::ToolsUsedCache a;

    SECTION("miss then hit") {
        REQUIRE(a.lookup("file.gcode", 100, 1000) == std::nullopt);
        const std::set<int> tools{0, 2};
        a.store("file.gcode", 100, 1000, tools);
        REQUIRE(a.lookup("file.gcode", 100, 1000).value_or(std::set<int>{}) == tools);
    }
    SECTION("empty set is a hit, not a miss") {
        const std::set<int> empty;
        a.store("single.gcode", 50, 5, empty);
        auto got = a.lookup("single.gcode", 50, 5);
        REQUIRE(got.has_value());
        REQUIRE(got->empty());
    }
    SECTION("size or mtime change invalidates") {
        a.store("file.gcode", 100, 1000, {0});
        REQUIRE(a.lookup("file.gcode", 101, 1000) == std::nullopt);
        REQUIRE(a.lookup("file.gcode", 100, 1001) == std::nullopt);
    }
    SECTION("path difference is a different entry") {
        a.store("dir/a.gcode", 100, 1000, {1});
        REQUIRE(a.lookup("dir/b.gcode", 100, 1000) == std::nullopt);
    }
}

TEST_CASE("ToolsUsedCache persistence across instances", "[tools_used_cache]") {
    CacheDirGuard guard;
    helix::ToolsUsedCache a;
    const std::set<int> tools{0, 1, 3};
    a.store("persist.gcode", 10, 20, tools);
    helix::ToolsUsedCache b; // fresh instance reads the same disk file
    REQUIRE(b.lookup("persist.gcode", 10, 20).value_or(std::set<int>{}) == tools);
}

TEST_CASE("ToolsUsedCache LRU bound", "[tools_used_cache]") {
    CacheDirGuard guard;
    helix::ToolsUsedCache a;
    for (size_t i = 0; i < helix::ToolsUsedCache::MAX_ENTRIES + 10; ++i) {
        a.store("f" + std::to_string(i) + ".gcode", i, i, {0});
    }
    // f0 was stored first and never looked up — evicted.
    REQUIRE(a.lookup("f0.gcode", 0, 0) == std::nullopt);
    // The most recent stores survive.
    const size_t last = helix::ToolsUsedCache::MAX_ENTRIES + 9;
    REQUIRE(a.lookup("f" + std::to_string(last) + ".gcode", last, last).has_value());
}
