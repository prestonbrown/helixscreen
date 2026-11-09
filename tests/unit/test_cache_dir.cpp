// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cache_dir.cpp
 * @brief Unit tests for get_helix_cache_dir() resolution chain
 *
 * Tests the 7-step cache directory resolution: HELIX_CACHE_DIR env,
 * config, platform, XDG, HOME, /var/tmp, /tmp fallbacks.
 */

#include "../test_helpers/scoped_env.h"
#include "app_globals.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

// Helper: create a unique temp directory for test isolation
static std::string make_test_tmpdir(const std::string& label) {
    std::string path = std::string("/tmp/helix_test_cache_") + label + "_" +
                       std::to_string(static_cast<unsigned long>(time(nullptr)));
    std::filesystem::create_directories(path);
    return path;
}

// Helper: clean up a directory tree
static void cleanup_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// ============================================================================
// get_helix_cache_dir() Tests
// ============================================================================

TEST_CASE("get_helix_cache_dir HELIX_CACHE_DIR override", "[cache]") {
    helix::ScopedEnv guard("HELIX_CACHE_DIR");
    std::string tmpdir = make_test_tmpdir("env_override");

    SECTION("Uses HELIX_CACHE_DIR when set") {
        setenv("HELIX_CACHE_DIR", tmpdir.c_str(), 1);

        std::string result = get_helix_cache_dir("test_sub");

        REQUIRE(result.find(tmpdir) == 0);
        REQUIRE(result.find("test_sub") != std::string::npos);
        REQUIRE(std::filesystem::exists(result));
    }

    SECTION("Creates subdirectory inside HELIX_CACHE_DIR") {
        setenv("HELIX_CACHE_DIR", tmpdir.c_str(), 1);

        std::string result = get_helix_cache_dir("my_subdir");

        std::string expected = tmpdir + "/my_subdir";
        REQUIRE(result == expected);
        REQUIRE(std::filesystem::is_directory(result));
    }

    cleanup_dir(tmpdir);
}

TEST_CASE("get_helix_cache_dir falls through on empty env", "[cache]") {
    helix::ScopedEnv helix_guard("HELIX_CACHE_DIR");
    unsetenv("HELIX_CACHE_DIR");

    std::string result = get_helix_cache_dir("fallthrough_test");

    // Should still resolve to something valid (XDG, HOME, /var/tmp, or /tmp)
    REQUIRE(!result.empty());
    REQUIRE(std::filesystem::exists(result));

    cleanup_dir(result);
}

TEST_CASE("get_helix_cache_dir falls through on invalid env path", "[cache]") {
    helix::ScopedEnv guard("HELIX_CACHE_DIR");
    // Set to a path that can't be created (nested under /nonexistent)
    setenv("HELIX_CACHE_DIR", "/nonexistent/readonly/cache", 1);

    std::string result = get_helix_cache_dir("invalid_test");

    // Should gracefully fall through to a working directory
    REQUIRE(!result.empty());
    REQUIRE(std::filesystem::exists(result));

    cleanup_dir(result);
}

TEST_CASE("get_helix_cache_dir result is writable", "[cache]") {
    std::string result = get_helix_cache_dir("writable_test");
    REQUIRE(!result.empty());

    // Verify we can actually write a file there
    std::string test_file = result + "/.write_test";
    {
        std::ofstream ofs(test_file);
        REQUIRE(ofs.good());
        ofs << "test";
    }

    REQUIRE(std::filesystem::exists(test_file));
    std::filesystem::remove(test_file);
    cleanup_dir(result);
}

TEST_CASE("get_helix_cache_dir different subdirs get different paths", "[cache]") {
    std::string dir_a = get_helix_cache_dir("subdir_alpha");
    std::string dir_b = get_helix_cache_dir("subdir_beta");

    REQUIRE(dir_a != dir_b);
    REQUIRE(dir_a.find("subdir_alpha") != std::string::npos);
    REQUIRE(dir_b.find("subdir_beta") != std::string::npos);

    cleanup_dir(dir_a);
    cleanup_dir(dir_b);
}

// ============================================================================
// peek_helix_cache_dir() — resolution without materialization
// ============================================================================
//
// The cascade must not answer "where does the cache go?" by calling ensure_dir()
// and reading success as the verdict: asking the question would create the
// answer, and anything that only wants the path (a sweep classifying a
// directory found on disk as live-or-stale, a diagnostic printing the location)
// would leave a directory tree behind as the cost of asking.

// Unique per run so the /var/tmp and /tmp rungs of the cascade can be asserted
// absent without colliding with a real cache or a previous test run.
static std::string unique_subdir(const std::string& label) {
    return "peek_" + label + "_" + std::to_string(static_cast<unsigned long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long>(time(nullptr)));
}

TEST_CASE("peek_helix_cache_dir resolves without creating", "[cache]") {
    helix::ScopedEnv guard("HELIX_CACHE_DIR");
    std::string tmpdir = make_test_tmpdir("peek_nocreate");
    setenv("HELIX_CACHE_DIR", tmpdir.c_str(), 1);
    const std::string sub = unique_subdir("nocreate");

    std::string peeked = peek_helix_cache_dir(sub);

    REQUIRE(peeked == tmpdir + "/" + sub);
    // The whole point: the answer did not bring the directory into existence.
    REQUIRE_FALSE(std::filesystem::exists(peeked));

    // Repeating the question still creates nothing.
    REQUIRE(peek_helix_cache_dir(sub) == peeked);
    REQUIRE_FALSE(std::filesystem::exists(peeked));

    cleanup_dir(tmpdir);
}

TEST_CASE("peek agrees with get, and only get creates", "[cache]") {
    helix::ScopedEnv guard("HELIX_CACHE_DIR");
    std::string tmpdir = make_test_tmpdir("peek_agrees");
    setenv("HELIX_CACHE_DIR", tmpdir.c_str(), 1);
    const std::string sub = unique_subdir("agrees");

    std::string peeked = peek_helix_cache_dir(sub);
    REQUIRE_FALSE(peeked.empty());
    REQUIRE_FALSE(std::filesystem::exists(peeked));

    std::string got = get_helix_cache_dir(sub);
    REQUIRE(got == peeked);
    REQUIRE(std::filesystem::is_directory(got));

    // And peek still reports the same place now that it exists.
    REQUIRE(peek_helix_cache_dir(sub) == got);

    cleanup_dir(tmpdir);
}

TEST_CASE("get_helix_cache_dir creates only the winning tier", "[cache]") {
    helix::ScopedEnv guard("HELIX_CACHE_DIR");
    std::string tmpdir = make_test_tmpdir("only_winner");
    setenv("HELIX_CACHE_DIR", tmpdir.c_str(), 1);
    const std::string sub = unique_subdir("winner");

    std::string got = get_helix_cache_dir(sub);
    REQUIRE(got == tmpdir + "/" + sub);
    REQUIRE(std::filesystem::is_directory(got));

    // Lower rungs of the cascade must be untouched — the override won, so
    // nothing below it had any reason to be materialized.
    REQUIRE_FALSE(std::filesystem::exists("/var/tmp/helix_" + sub));
    REQUIRE_FALSE(std::filesystem::exists("/tmp/helix_" + sub));

    cleanup_dir(tmpdir);
}

TEST_CASE("a candidate under an unwritable parent is skipped, not created", "[cache]") {
    if (geteuid() == 0) {
        SUCCEED("running as root — mode bits do not deny access");
        return;
    }

    helix::ScopedEnv guard("HELIX_CACHE_DIR");
    std::string locked = make_test_tmpdir("locked_parent");
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec);

    const std::string sub = unique_subdir("locked");
    std::string denied = locked + "/nested";
    setenv("HELIX_CACHE_DIR", denied.c_str(), 1);

    // Resolution must fall past the unusable override to a lower tier...
    std::string got = get_helix_cache_dir(sub);
    REQUIRE_FALSE(got.empty());
    REQUIRE(got.find(locked) == std::string::npos);
    // ...leaving nothing behind under the directory it could not use.
    REQUIRE_FALSE(std::filesystem::exists(denied));

    // peek reaches the same verdict without writing anywhere.
    REQUIRE(peek_helix_cache_dir(sub).find(locked) == std::string::npos);

    std::filesystem::permissions(locked, std::filesystem::perms::owner_all);
    cleanup_dir(locked);
    cleanup_dir(got);
}

// ============================================================================
// Stale-cache reclamation
// ============================================================================

#include "../test_helpers/helix_cache_dir_test_access.h"

namespace {
/// Build <root>/<subdir>/marker so a reclaim has something real to remove.
std::string seed_cache(const std::string& root, const std::string& subdir) {
    const std::string dir = root + "/" + subdir;
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/marker") << "x";
    return dir;
}
} // namespace

TEST_CASE("reclaim_cache_paths removes a stale cache subdir", "[cache]") {
    std::string root = make_test_tmpdir("reclaim_hit");
    const std::string dir = seed_cache(root, "helix_thumbs");
    REQUIRE(std::filesystem::exists(dir + "/marker"));

    REQUIRE(reclaim_cache_paths({dir}, "helix_thumbs") == 1);
    REQUIRE_FALSE(std::filesystem::exists(dir));

    cleanup_dir(root);
}

TEST_CASE("reclaim_cache_paths drops the parent once it is empty", "[cache]") {
    std::string root = make_test_tmpdir("reclaim_parent");
    const std::string base = root + "/helix";
    const std::string dir = seed_cache(base, "gcode_mod");

    REQUIRE(reclaim_cache_paths({dir}, "gcode_mod") == 1);
    // ~/.cache/helix should not survive as an empty shell.
    REQUIRE_FALSE(std::filesystem::exists(base));

    cleanup_dir(root);
}

TEST_CASE("reclaim_cache_paths keeps a parent that still has siblings", "[cache]") {
    std::string root = make_test_tmpdir("reclaim_sibling");
    const std::string base = root + "/helix";
    const std::string doomed = seed_cache(base, "gcode_mod");
    const std::string keep = seed_cache(base, "helix_thumbs");

    REQUIRE(reclaim_cache_paths({doomed}, "gcode_mod") == 1);
    REQUIRE_FALSE(std::filesystem::exists(doomed));
    REQUIRE(std::filesystem::exists(keep + "/marker"));
    REQUIRE(std::filesystem::exists(base));

    cleanup_dir(root);
}

TEST_CASE("reclaim_cache_paths refuses a path that is not the named subdir", "[cache]") {
    std::string root = make_test_tmpdir("reclaim_mismatch");
    const std::string dir = seed_cache(root, "something_else");

    // The suffix guard is what stands between remove_all and a path nobody checked.
    REQUIRE(reclaim_cache_paths({dir}, "helix_thumbs") == 0);
    REQUIRE(std::filesystem::exists(dir + "/marker"));

    cleanup_dir(root);
}

TEST_CASE("reclaim_cache_paths reclaims the flattened helix_<subdir> form", "[cache]") {
    // Rungs 6-7 spell the leaf "/var/tmp/helix_gcode_temp", not
    // ".../helix/gcode_temp"; the nested form alone leaves them unreclaimable.
    std::string root = make_test_tmpdir("reclaim_flat");
    const std::string dir = seed_cache(root, "helix_helix_thumbs");

    REQUIRE(reclaim_cache_paths({dir}, "helix_thumbs") == 1);
    REQUIRE_FALSE(std::filesystem::exists(dir));

    cleanup_dir(root);
}

TEST_CASE("reclaim_cache_paths still refuses a foreign helix_ prefixed dir", "[cache]") {
    // Widening to the flattened form must not become "anything helix_-prefixed".
    std::string root = make_test_tmpdir("reclaim_flat_wrong");
    const std::string dir = seed_cache(root, "helix_something_else");

    REQUIRE(reclaim_cache_paths({dir}, "helix_thumbs") == 0);
    REQUIRE(std::filesystem::exists(dir + "/marker"));

    cleanup_dir(root);
}

TEST_CASE("reclaim_cache_paths tolerates a path that does not exist", "[cache]") {
    REQUIRE(reclaim_cache_paths({"/nonexistent/helix_thumbs"}, "helix_thumbs") == 0);
}

TEST_CASE("the sweep reclaims NOTHING on a host build", "[cache]") {
    // The platform gate is the safety property: with no compile-time platform
    // rung defined, the XDG/HOME rung IS the live cache, so a sweep that ran
    // here would treat a developer's real ~/.cache/helix as stale. Seed every
    // lower rung and require all of them to survive.
    helix::ScopedEnv cache_guard("HELIX_CACHE_DIR");
    helix::ScopedEnv xdg_guard("XDG_CACHE_HOME");

    std::string root = make_test_tmpdir("sweep_noop");
    setenv("HELIX_CACHE_DIR", (root + "/pinned").c_str(), 1);
    setenv("XDG_CACHE_HOME", (root + "/xdg").c_str(), 1);

    const std::string xdg_cache = seed_cache(root + "/xdg/helix", "helix_thumbs");
    const std::string pinned = seed_cache(root + "/pinned", "helix_thumbs");

    REQUIRE(sweep_stale_helix_cache_dirs() == 0);
    REQUIRE(std::filesystem::exists(xdg_cache + "/marker"));
    REQUIRE(std::filesystem::exists(pinned + "/marker"));

    cleanup_dir(root);
}

// ---------------------------------------------------------------------------
// select_stale_paths() — the gate's decision over candidate shapes a host build
// never produces. The public sweep can only demonstrate the desktop case here.
// ---------------------------------------------------------------------------

#include "system/helix_cache_dir_internal.h"

using helix::cache_internal::CacheCandidate;
using helix::cache_internal::select_stale_paths;

namespace {
/// Every path is usable — the common case on a device.
bool all_viable(const std::string&) {
    return true;
}

/// The shape every device presents: a platform hook exported HELIX_CACHE_DIR,
/// so rung 1 wins and the platform rung does not.
std::vector<CacheCandidate> device_shape() {
    return {
        {"/usr/data/helixscreen/cache/helix_thumbs", "HELIX_CACHE_DIR", false},
        {"/usr/data/helixscreen-state/cache/helix_thumbs", "k1", true},
        {"/root/.cache/helix/helix_thumbs", nullptr, false},
        {"/var/tmp/helix_helix_thumbs", nullptr, false},
        {"/tmp/helix_helix_thumbs", nullptr, false},
    };
}
} // namespace

TEST_CASE("sweep runs when the env rung wins but a platform rung exists", "[cache]") {
    // Gating on the platform rung *winning* makes the sweep dead code on every
    // shipped device, since each platform hook exports HELIX_CACHE_DIR.
    const std::vector<std::string> stale = select_stale_paths(device_shape(), all_viable);

    REQUIRE(stale.size() == 3);
    CHECK(stale[0] == "/root/.cache/helix/helix_thumbs");
    CHECK(stale[1] == "/var/tmp/helix_helix_thumbs");
    CHECK(stale[2] == "/tmp/helix_helix_thumbs");
}

TEST_CASE("select_stale_paths never reclaims a deliberate rung", "[cache]") {
    // env wins, config sits below it. A config base_directory is stated intent.
    std::vector<CacheCandidate> c = {
        {"/pinned/helix_thumbs", "HELIX_CACHE_DIR", false},
        {"/configured/helix_thumbs", "config", false},
        {"/platform/helix_thumbs", "k1", true},
        {"/home/dev/.cache/helix/helix_thumbs", nullptr, false},
    };
    const std::vector<std::string> stale = select_stale_paths(c, all_viable);

    REQUIRE(stale.size() == 1);
    CHECK(stale[0] == "/home/dev/.cache/helix/helix_thumbs");
}

TEST_CASE("select_stale_paths returns nothing without a platform rung", "[cache]") {
    // Desktop build: even with HELIX_CACHE_DIR redirected, ~/.cache/helix lives.
    std::vector<CacheCandidate> c = {
        {"/pinned/helix_thumbs", "HELIX_CACHE_DIR", false},
        {"/home/dev/.cache/helix/helix_thumbs", nullptr, false},
        {"/tmp/helix_helix_thumbs", nullptr, false},
    };
    CHECK(select_stale_paths(c, all_viable).empty());
}

TEST_CASE("select_stale_paths ignores rungs above the winner", "[cache]") {
    // Rungs above the winner were rejected as unusable — not ours to delete.
    auto only_root_cache_viable = [](const std::string& p) { return p.rfind("/root/", 0) == 0; };
    const std::vector<std::string> stale =
        select_stale_paths(device_shape(), only_root_cache_viable);

    // /root/.cache wins; the two rungs below it are reclaimable, the two
    // unusable rungs above it are not.
    REQUIRE(stale.size() == 2);
    CHECK(stale[0] == "/var/tmp/helix_helix_thumbs");
    CHECK(stale[1] == "/tmp/helix_helix_thumbs");
}

TEST_CASE("select_stale_paths returns nothing when no rung is usable", "[cache]") {
    auto none_viable = [](const std::string&) { return false; };
    CHECK(select_stale_paths(device_shape(), none_viable).empty());
}

// ---------------------------------------------------------------------------
// migrate_state_root() — the state-root rename over a real temp tree. Which
// roots a platform uses is a platform question; this is the filesystem half,
// so any two paths drive it.
// ---------------------------------------------------------------------------

using helix::cache_internal::migrate_state_root;

namespace {
/// <root>/<sub>/marker, so a move has real content to carry.
std::string seed_state(const std::string& root, const std::string& sub) {
    const std::string dir = root + "/" + sub;
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/marker") << "x";
    return dir;
}
} // namespace

TEST_CASE("migrate_state_root renames the whole root when the new one is absent", "[cache]") {
    std::string root = make_test_tmpdir("migrate_whole");
    const std::string legacy = root + "/helixscreen";
    const std::string current = root + "/.helixscreen";
    seed_state(legacy, "cache");
    seed_state(legacy, "logs");

    REQUIRE(migrate_state_root(legacy, current) == 1);
    REQUIRE_FALSE(std::filesystem::exists(legacy));
    REQUIRE(std::filesystem::exists(current + "/cache/marker"));
    REQUIRE(std::filesystem::exists(current + "/logs/marker"));

    cleanup_dir(root);
}

TEST_CASE("migrate_state_root is a no-op without a legacy root", "[cache]") {
    std::string root = make_test_tmpdir("migrate_absent");
    const std::string legacy = root + "/helixscreen";
    const std::string current = root + "/.helixscreen";
    seed_state(current, "logs");

    REQUIRE(migrate_state_root(legacy, current) == 0);
    // A missing legacy root must not conjure or disturb anything.
    REQUIRE(std::filesystem::exists(current + "/logs/marker"));

    cleanup_dir(root);
}

TEST_CASE("migrate_state_root carries a subtree across but never clobbers", "[cache]") {
    std::string root = make_test_tmpdir("migrate_merge");
    const std::string legacy = root + "/helixscreen";
    const std::string current = root + "/.helixscreen";
    // Both sides hold a cache — the new side's EMPTY, which a bare rename()
    // would replace without complaint — and only the legacy side holds logs.
    seed_state(legacy, "cache");
    seed_state(legacy, "logs");
    std::filesystem::create_directories(current + "/cache");

    REQUIRE(migrate_state_root(legacy, current) >= 1);
    // logs existed only on the legacy side: carried across.
    REQUIRE(std::filesystem::exists(current + "/logs/marker"));
    // cache existed on both: the new side keeps its own (empty) copy, and the
    // legacy twin stays for the operator to inspect.
    REQUIRE_FALSE(std::filesystem::exists(current + "/cache/marker"));
    REQUIRE(std::filesystem::exists(legacy + "/cache/marker"));
    // The non-empty legacy cache keeps the legacy root alive.
    REQUIRE(std::filesystem::exists(legacy));

    cleanup_dir(root);
}

TEST_CASE("migrate_state_root drops empty legacy scaffolding", "[cache]") {
    std::string root = make_test_tmpdir("migrate_scaffold");
    const std::string legacy = root + "/helixscreen";
    const std::string current = root + "/.helixscreen";
    seed_state(current, "cache");
    seed_state(current, "logs");
    // The hook's pre-start recreates <legacy>/logs even after a completed
    // migration; an empty one is scaffolding, not data.
    std::filesystem::create_directories(legacy + "/logs");

    REQUIRE(migrate_state_root(legacy, current) >= 1);
    REQUIRE_FALSE(std::filesystem::exists(legacy));
    REQUIRE(std::filesystem::exists(current + "/logs/marker"));

    cleanup_dir(root);
}

// ---------------------------------------------------------------------------
// board_cache_candidates() - the platform rung is a per-board table
// ---------------------------------------------------------------------------

#include "board_identity.h"
#include "platform_info.h"

using helix::FlashForgeModTree;
using helix::board::Board;
using helix::cache_internal::board_cache_candidates;

namespace {
/// The paths of @p rungs, in order, after checking each is a labelled platform rung.
std::vector<std::string> rung_paths(const std::vector<CacheCandidate>& rungs, Board board) {
    std::vector<std::string> out;
    for (const auto& c : rungs) {
        CHECK(c.platform);
        CHECK(std::string(c.tier) == helix::board::info(board).key);
        out.push_back(c.path);
    }
    return out;
}
} // namespace

TEST_CASE("board_cache_candidates names each board's rung", "[cache]") {
    using V = std::vector<std::string>;
    auto paths = [](Board b, FlashForgeModTree t) {
        return rung_paths(board_cache_candidates(b, t, "x"), b);
    };

    CHECK(paths(Board::Ad5m, FlashForgeModTree::None) == V{"/data/.helixscreen/cache/x"});
    CHECK(paths(Board::Cc1, FlashForgeModTree::None) ==
          V{"/user-resource/helixscreen-state/cache/x"});
    CHECK(paths(Board::K2, FlashForgeModTree::None) ==
          V{"/mnt/UDISK/helixscreen-state/cache/x", "/usr/data/helixscreen-state/cache/x"});
    CHECK(paths(Board::K1, FlashForgeModTree::None) == V{"/usr/data/helixscreen-state/cache/x"});
    CHECK(paths(Board::SnapmakerU1, FlashForgeModTree::None) ==
          V{"/userdata/helixscreen-state/cache/x"});
    CHECK(paths(Board::Generic, FlashForgeModTree::None).empty());

    // The FlashForge MIPS boards split on the mod tree: Z-Mod keeps state under /srv,
    // Forge-X under the mod's own durable config directory.
    for (Board b : {Board::Ad5x, Board::Creator5, Board::Creator5Pro}) {
        INFO("board: " << helix::board::info(b).key);
        CHECK(paths(b, FlashForgeModTree::Zmod) == V{"/srv/helixscreen-state/cache/x"});
        CHECK(paths(b, FlashForgeModTree::None) == V{"/srv/helixscreen-state/cache/x"});
        CHECK(paths(b, FlashForgeModTree::ForgeX) ==
              V{"/opt/config/mod_data/helixscreen-state/cache/x"});
    }
}

#include "helix_install_roots.h"

TEST_CASE("every board's cache rung lives under a known state root", "[cache]") {
    // A state root missing from kStateRoots is logs and crash files a debug bundle cannot
    // find, and a tree the uninstaller never sweeps.
    for (int i = 0; i < helix::board::kBoardCount; ++i) {
        for (FlashForgeModTree tree :
             {FlashForgeModTree::None, FlashForgeModTree::Zmod, FlashForgeModTree::ForgeX}) {
            const Board board = static_cast<Board>(i);
            for (const auto& rung : board_cache_candidates(board, tree, "x")) {
                bool known = false;
                for (const char* root : helix::kStateRoots) {
                    known = known || rung.path.rfind(std::string(root) + "/", 0) == 0;
                }
                INFO("board " << helix::board::info(board).key << " rung " << rung.path);
                CHECK(known);
            }
        }
    }
}

TEST_CASE("the Forge-X state root is a known state root", "[cache]") {
    bool found = false;
    for (const char* root : helix::kStateRoots) {
        found = found || std::string(root) == "/opt/config/mod_data/helixscreen-state";
    }
    CHECK(found);
}

TEST_CASE("state_root_candidates puts the board's state roots before the host fallbacks",
          "[cache][2085]") {
    using helix::cache_internal::state_root_candidates;
    using V = std::vector<std::string>;

    // The telemetry identity resolves here; the board's root is the one beside
    // the payload, never inside it and never under printer_data.
    CHECK(state_root_candidates(Board::K1, FlashForgeModTree::None, "/var/lib/hs", "/h/.hs") ==
          V{"/usr/data/helixscreen-state", "/var/lib/hs", "/h/.hs"});
    CHECK(state_root_candidates(Board::Ad5x, FlashForgeModTree::Zmod, "/var/lib/hs", "/h/.hs") ==
          V{"/srv/helixscreen-state", "/var/lib/hs", "/h/.hs"});
    CHECK(
        state_root_candidates(Board::K2, FlashForgeModTree::None, "/var/lib/hs", "/h/.hs") ==
        V{"/mnt/UDISK/helixscreen-state", "/usr/data/helixscreen-state", "/var/lib/hs", "/h/.hs"});
    CHECK(state_root_candidates(Board::Generic, FlashForgeModTree::None, "/var/lib/hs", "/h/.hs") ==
          V{"/var/lib/hs", "/h/.hs"});

    // The cache rungs are the same roots, so the two can never name different trees.
    for (int i = 0; i < helix::board::kBoardCount; ++i) {
        const Board board = static_cast<Board>(i);
        V from_rungs;
        for (const auto& rung : board_cache_candidates(board, FlashForgeModTree::Zmod, "x"))
            from_rungs.push_back(
                rung.path.substr(0, rung.path.size() - std::string("/cache/x").size()));
        V roots = state_root_candidates(board, FlashForgeModTree::Zmod, "a", "b");
        roots.resize(roots.size() - 2);
        CHECK(roots == from_rungs);
    }
}
