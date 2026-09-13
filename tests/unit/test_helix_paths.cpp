// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_helix_paths.cpp
 * @brief Unit tests for helix::paths filesystem-path primitives.
 *
 * These primitives centralize behavior currently reimplemented across
 * update_checker.cpp, log_path_probe.cpp, input_shaper_cache.cpp,
 * thumbnail_cache.cpp, app_globals.cpp, app_constants.h, logging_init.cpp,
 * and data_root_resolver.cpp. Written TDD-style: the cases below encode the
 * exact edge-case semantics converged from those sources.
 */

#include "system/helix_paths.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix::paths;

namespace {

// RAII save/restore of an environment variable so tests don't leak state.
class EnvVarGuard {
  public:
    explicit EnvVarGuard(const char* name) : name_(name) {
        const char* v = std::getenv(name);
        had_ = v != nullptr;
        if (had_) {
            saved_ = v;
        }
    }
    ~EnvVarGuard() {
        if (had_) {
            ::setenv(name_.c_str(), saved_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }
    void set(const std::string& value) {
        ::setenv(name_.c_str(), value.c_str(), 1);
    }
    void unset() {
        ::unsetenv(name_.c_str());
    }

  private:
    std::string name_;
    bool had_ = false;
    std::string saved_;
};

// Create a unique, writable temp directory for the duration of a test.
fs::path make_tmp_dir(const std::string& tag) {
    fs::path base =
        fs::temp_directory_path() / ("helix_paths_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

} // namespace

TEST_CASE("dirname edge cases", "[helix_paths]") {
    CHECK(dirname("/a/b/c.tar.gz") == "/a/b");
    CHECK(dirname("/a/b/") == "/a");
    CHECK(dirname("file") == ".");
    CHECK(dirname("/file") == "/");
    CHECK(dirname("/") == "/");
    CHECK(dirname("") == ".");
}

TEST_CASE("strip_trailing_slash edge cases", "[helix_paths]") {
    CHECK(strip_trailing_slash("/a/b/") == "/a/b");
    CHECK(strip_trailing_slash("/") == "/");
    CHECK(strip_trailing_slash("/a///") == "/a");
    CHECK(strip_trailing_slash("") == "");
    CHECK(strip_trailing_slash("/a/b") == "/a/b"); // no trailing slash: unchanged
}

TEST_CASE("home honors absolute HOME only", "[helix_paths]") {
    EnvVarGuard home_guard("HOME");

    home_guard.set("/home/tester");
    CHECK(home() == "/home/tester");

    home_guard.set("");
    CHECK(home() == "");

    home_guard.set("relative/path");
    CHECK(home() == "");

    home_guard.unset();
    CHECK(home() == "");
}

TEST_CASE("xdg_cache_bases ordered candidates", "[helix_paths]") {
    EnvVarGuard xdg_guard("XDG_CACHE_HOME");
    EnvVarGuard home_guard("HOME");

    // XDG set + HOME set -> both candidates, XDG first (the fallback chain).
    xdg_guard.set("/xdg/cache");
    home_guard.set("/home/tester");
    CHECK(xdg_cache_bases() == std::vector<std::string>{"/xdg/cache", "/home/tester/.cache"});

    // XDG unset -> only HOME/.cache.
    xdg_guard.unset();
    CHECK(xdg_cache_bases() == std::vector<std::string>{"/home/tester/.cache"});

    // Neither -> empty.
    home_guard.unset();
    CHECK(xdg_cache_bases().empty());
}

TEST_CASE("xdg_data_home chain", "[helix_paths]") {
    EnvVarGuard xdg_guard("XDG_DATA_HOME");
    EnvVarGuard home_guard("HOME");

    // XDG set wins.
    xdg_guard.set("/xdg/data");
    home_guard.set("/home/tester");
    CHECK(xdg_data_home() == "/xdg/data");

    // XDG unset -> HOME/.local/share.
    xdg_guard.unset();
    CHECK(xdg_data_home() == "/home/tester/.local/share");

    // Neither -> "" (no /tmp hard-fallback here).
    home_guard.unset();
    CHECK(xdg_data_home() == "");
}

TEST_CASE("available_space on real vs bogus dir", "[helix_paths]") {
    fs::path dir = make_tmp_dir("avail");
    CHECK(available_space(dir.string()) > 0);
    CHECK(available_space("/no/such/path/really/unlikely/xyz") == 0);
    fs::remove_all(dir);
}

TEST_CASE("is_writable_dir true/false", "[helix_paths]") {
    fs::path dir = make_tmp_dir("writable");
    CHECK(is_writable_dir(dir.string()));
    CHECK_FALSE(is_writable_dir("/no/such/path/really/unlikely/xyz"));
    CHECK_FALSE(is_writable_dir(""));

    // chmod-000 dir is unwritable, but root ignores permission bits.
    if (::geteuid() != 0) {
        fs::path locked = dir / "locked";
        fs::create_directories(locked);
        fs::permissions(locked, fs::perms::none);
        CHECK_FALSE(is_writable_dir(locked.string()));
        fs::permissions(locked, fs::perms::owner_all); // restore so remove_all works
    }
    fs::remove_all(dir);
}

TEST_CASE("ensure_dir creates nested, idempotent", "[helix_paths]") {
    fs::path base = make_tmp_dir("ensure");
    fs::path nested = base / "a" / "b" / "c";

    CHECK(ensure_dir(nested.string()));
    CHECK(fs::exists(nested));
    CHECK(fs::is_directory(nested));

    // Second call is idempotent.
    CHECK(ensure_dir(nested.string()));

    fs::remove_all(base);
}

TEST_CASE("probe_writable success leaves no residue", "[helix_paths]") {
    fs::path dir = make_tmp_dir("probe");

    CHECK(probe_writable(dir.string(), 0));

    // No leftover test file.
    bool leftover = false;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        leftover = true;
    }
    CHECK_FALSE(leftover);

    fs::remove_all(dir);
}

TEST_CASE("probe_writable fails on nonexistent dir", "[helix_paths]") {
    CHECK_FALSE(probe_writable("/no/such/path/really/unlikely/xyz", 0));
}

// GAP 3 (L093 shape): the two cache sites (input_shaper_cache, thumbnail_cache)
// depend on probe_writable correctly REJECTING an existing-but-unwritable dir —
// a plain is_writable_dir/exists check would wrongly accept it. This proves the
// create+write+remove probe actually fails when the dir denies writes.
TEST_CASE("probe_writable rejects read-only existing dir", "[helix_paths]") {
    if (::geteuid() == 0) {
        return; // root bypasses permission bits — the probe would spuriously succeed
    }
    fs::path dir = make_tmp_dir("probe_ro");
    fs::path locked = dir / "locked";
    fs::create_directories(locked);
    fs::permissions(locked, fs::perms::none); // no read/write/exec for anyone

    CHECK_FALSE(probe_writable(locked.string(), 0));

    fs::permissions(locked, fs::perms::owner_all); // restore so cleanup works
    fs::remove_all(dir);
}

// GAP 4: home() rejects any value containing a control character (heap-corruption
// guard inherited from sanitize_home). A regression that dropped the control-char
// scan would return the raw junk path instead of "".
TEST_CASE("home rejects control characters", "[helix_paths]") {
    EnvVarGuard home_guard("HOME");

    // Absolute path with an embedded control char (0x01) is rejected.
    home_guard.set("/home/\x01"
                   "bad");
    CHECK(home() == "");

    // A clean absolute path is still accepted (guards against over-rejection).
    home_guard.set("/home/tester");
    CHECK(home() == "/home/tester");
}

TEST_CASE("probe_writable fails when space short", "[helix_paths]") {
    fs::path dir = make_tmp_dir("probe_space");
    CHECK_FALSE(probe_writable(dir.string(), UINT64_MAX));
    fs::remove_all(dir);
}

// first_writable_dir returns the FIRST writable candidate in preference order,
// skipping nonexistent / read-only / short-on-space entries. Backs
// app_get_runtime_dir()'s /tmp-then-cache fallback on ProtectSystem=strict devices.
TEST_CASE("first_writable_dir returns first writable candidate in order", "[helix_paths]") {
    fs::path a = make_tmp_dir("fwd_a");
    fs::path b = make_tmp_dir("fwd_b");
    CHECK(first_writable_dir({a.string(), b.string()}) == a.string());
    fs::remove_all(a);
    fs::remove_all(b);
}

TEST_CASE("first_writable_dir skips a nonexistent candidate", "[helix_paths]") {
    fs::path b = make_tmp_dir("fwd_skip_missing");
    std::vector<std::string> cands = {"/no/such/path/really/unlikely/xyz", b.string()};
    CHECK(first_writable_dir(cands) == b.string());
    fs::remove_all(b);
}

TEST_CASE("first_writable_dir skips a read-only candidate", "[helix_paths]") {
    if (::geteuid() == 0) {
        return; // root bypasses permission bits — the read-only probe would succeed
    }
    fs::path dir = make_tmp_dir("fwd_ro");
    fs::path locked = dir / "locked";
    fs::create_directories(locked);
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec); // 0500, no write
    fs::path good = make_tmp_dir("fwd_ro_good");

    CHECK(first_writable_dir({locked.string(), good.string()}) == good.string());

    fs::permissions(locked, fs::perms::owner_all); // restore so cleanup works
    fs::remove_all(dir);
    fs::remove_all(good);
}

TEST_CASE("first_writable_dir returns empty when nothing qualifies", "[helix_paths]") {
    CHECK(first_writable_dir({}) == "");
    CHECK(first_writable_dir({"/no/such/a", "/no/such/b"}) == "");
}

TEST_CASE("first_writable_dir honors min_free_bytes", "[helix_paths]") {
    fs::path dir = make_tmp_dir("fwd_space");
    // An impossibly large free-space gate forces the (otherwise writable) dir to
    // be skipped, so no candidate qualifies.
    CHECK(first_writable_dir({dir.string()}, UINT64_MAX) == "");
    fs::remove_all(dir);
}

TEST_CASE("is_ram_backed separates tmpfs from storage", "[helix_paths]") {
    // A path we cannot stat answers "not RAM" — the predicate gates a warning,
    // and a warning invented from a failed syscall is noise.
    CHECK_FALSE(is_ram_backed(""));
    CHECK_FALSE(is_ram_backed("/no/such/path/really/unlikely/xyz"));

#if defined(__linux__)
    // /dev/shm is tmpfs on every Linux that has it; a build host without one
    // cannot exercise the positive case.
    if (fs::exists("/dev/shm")) {
        CHECK(is_ram_backed("/dev/shm"));
    }
    // The tree the tests are running from is real storage.
    CHECK_FALSE(is_ram_backed(fs::current_path().string()));

    // Resolves through symlinks: the CC1 reaches tmpfs as /tmp -> /var/tmp ->
    // /var/volatile/tmp, and a predicate that stopped at the link would miss it.
    fs::path link = fs::temp_directory_path() / "helix_ram_link_test";
    fs::remove(link);
    if (fs::exists("/dev/shm")) {
        std::error_code ec;
        fs::create_directory_symlink("/dev/shm", link, ec);
        if (!ec) {
            CHECK(is_ram_backed(link.string()));
            fs::remove(link);
        }
    }
#endif
}
