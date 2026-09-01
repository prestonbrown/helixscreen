// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for helix::logs — the unified log-tail helper shared by CrashReporter
// and DebugBundleCollector. Covers file reads, syslog filtering, the cascade
// ordering (file → syslog → journal), and empty-source fallback. Does not
// exercise the real journalctl path — that's validated on-device.

#include "system/log_collector.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

namespace {

struct TempDirGuard {
    fs::path path;
    TempDirGuard() {
        path = fs::temp_directory_path() / ("helix-logs-test-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(std::rand()));
        fs::create_directories(path);
    }
    ~TempDirGuard() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_lines(const fs::path& p, int n, const std::string& prefix = "line") {
    std::ofstream ofs(p);
    for (int i = 1; i <= n; ++i) {
        ofs << prefix << " " << i << "\n";
    }
}

// Builds one fake rootfs layout under `base` for the AD5X mod-tree predicate
// (ad5x_mod_layout_present). Every probe resolves under the returned root, so
// no test ever creates /ZMOD or /usr/prog on the real build host.
//
//   zmod-marker : /ZMOD regular file (the Z-Mod indicator)
//   zmod-prog   : /usr/prog dir (FlashForge's own layout — present on any
//                 AD5X, modded or not; K1 has neither this nor the marker)
//   forgex      : /opt/config/mod/.shell/platform.sh and nothing else — a
//                 Forge-X chroot has no /ZMOD, no /usr/prog, no /usr/data
//   forgex-host : /usr/data/config/mod/.shell/platform.sh — the same mod tree
//                 seen from outside the chroot (the rig binds /usr/data at
//                 /opt, so exactly one spelling is reachable per side)
//   plain       : empty root (Pi / dev box / K1: none of the markers)
fs::path make_ad5x_layout(const fs::path& base, const std::string& layout) {
    fs::path root = base / layout;
    if (layout == "zmod-marker") {
        fs::create_directories(root);
        std::ofstream(root / "ZMOD") << "marker\n";
    } else if (layout == "zmod-prog") {
        fs::create_directories(root / "usr/prog");
    } else if (layout == "forgex") {
        fs::create_directories(root / "opt/config/mod/.shell");
        std::ofstream(root / "opt/config/mod/.shell/platform.sh") << "#!/bin/sh\n";
    } else if (layout == "forgex-host") {
        fs::create_directories(root / "usr/data/config/mod/.shell");
        std::ofstream(root / "usr/data/config/mod/.shell/platform.sh") << "#!/bin/sh\n";
    } else {
        fs::create_directories(root);
    }
    return root;
}

} // namespace

// ============================================================================
// tail_file()
// ============================================================================

TEST_CASE("helix::logs::tail_file returns last N lines", "[log_collector]") {
    TempDirGuard tmp;
    auto log = tmp.path / "helix.log";
    write_lines(log, 100);

    auto result = helix::logs::tail_file({log.string()}, 10);
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("line 100") != std::string::npos);
    REQUIRE(result.find("line 91") != std::string::npos);
    // First-ten should not leak in
    REQUIRE(result.find("line 10\n") == std::string::npos);
}

TEST_CASE("helix::logs::tail_file handles file shorter than N", "[log_collector]") {
    TempDirGuard tmp;
    auto log = tmp.path / "small.log";
    write_lines(log, 3);

    auto result = helix::logs::tail_file({log.string()}, 50);
    REQUIRE(result.find("line 1") != std::string::npos);
    REQUIRE(result.find("line 3") != std::string::npos);
}

TEST_CASE("helix::logs::tail_file tries paths in order", "[log_collector]") {
    TempDirGuard tmp;
    auto fallback = tmp.path / "fallback.log";
    write_lines(fallback, 5, "fb");

    // First path missing, second present — function should use the second.
    auto result =
        helix::logs::tail_file({(tmp.path / "missing.log").string(), fallback.string()}, 10);
    REQUIRE(result.find("fb 1") != std::string::npos);
    REQUIRE(result.find("fb 5") != std::string::npos);
}

TEST_CASE("helix::logs::tail_file returns empty when no path readable", "[log_collector]") {
    auto result = helix::logs::tail_file({"/nonexistent/one.log", "/nonexistent/two.log"}, 10);
    REQUIRE(result.empty());
}

TEST_CASE("helix::logs::tail_file picks newest mtime, not list order", "[log_collector]") {
    // Regression for #981: an AD5X debug bundle shipped a month-old log because
    // a stale leftover sat at a higher-priority path and won by list position.
    // Freshness must decide, not order.
    TempDirGuard tmp;
    auto stale = tmp.path / "stale.log";
    auto fresh = tmp.path / "fresh.log";
    write_lines(stale, 5, "stale");
    write_lines(fresh, 5, "fresh");

    auto now = fs::file_time_type::clock::now();
    fs::last_write_time(stale, now - std::chrono::hours(48));
    fs::last_write_time(fresh, now);

    // `stale` is FIRST in the list but must lose to the fresher file.
    auto result = helix::logs::tail_file({stale.string(), fresh.string()}, 10);
    REQUIRE(result.find("fresh 5") != std::string::npos);
    REQUIRE(result.find("stale") == std::string::npos);
}

TEST_CASE("helix::logs::tail_file falls through unreadable newest to next freshest",
          "[log_collector]") {
    // The freshest path exists but is empty (size 0); selection must skip it and
    // fall back to the next-freshest file with real content rather than return
    // empty.
    TempDirGuard tmp;
    auto older = tmp.path / "older.log";
    auto empty_fresh = tmp.path / "empty.log";
    write_lines(older, 4, "older");
    { std::ofstream ofs(empty_fresh); } // newest mtime, zero bytes

    auto now = fs::file_time_type::clock::now();
    fs::last_write_time(older, now - std::chrono::hours(2));
    fs::last_write_time(empty_fresh, now);

    auto result = helix::logs::tail_file({empty_fresh.string(), older.string()}, 10);
    REQUIRE(result.find("older 4") != std::string::npos);
}

TEST_CASE("helix::logs::tail_file returns empty when file is empty", "[log_collector]") {
    TempDirGuard tmp;
    auto empty = tmp.path / "empty.log";
    { std::ofstream ofs(empty); }
    auto result = helix::logs::tail_file({empty.string()}, 10);
    REQUIRE(result.empty());
}

// ============================================================================
// tail_syslog_from() — the filter logic under test. tail_syslog() itself just
// forwards to tail_syslog_from with /var/log/{messages,syslog} — an integration
// detail we don't try to exercise from unit tests.
// ============================================================================

TEST_CASE("helix::logs::tail_syslog_from keeps only helix-screen lines", "[log_collector]") {
    TempDirGuard tmp;
    auto path = tmp.path / "messages";
    {
        std::ofstream ofs(path);
        ofs << "Apr 18 03:48:30 host kernel: unrelated kernel message\n"
            << "Apr 18 03:48:31 host systemd[1]: Started helix-screen.service\n"
            << "Apr 18 03:48:32 host helix-screen[1234]: [Application] init\n"
            << "Apr 18 03:48:33 host sshd[1200]: accepted login\n"
            << "Apr 18 03:48:34 host helix-watchdog[99]: restart\n"
            << "Apr 18 03:48:35 host helix-splash[42]: shown\n";
    }

    auto result = helix::logs::tail_syslog_from({path.string()}, 100);

    REQUIRE(result.find("helix-screen[1234]") != std::string::npos);
    REQUIRE(result.find("helix-watchdog[99]") != std::string::npos);
    REQUIRE(result.find("helix-splash[42]") != std::string::npos);
    // "helix-screen.service" mentions the identifier via systemd — that's
    // intentional (startup/stop events are useful context).
    REQUIRE(result.find("Started helix-screen.service") != std::string::npos);
    REQUIRE(result.find("unrelated kernel message") == std::string::npos);
    REQUIRE(result.find("sshd") == std::string::npos);
}

TEST_CASE("helix::logs::tail_syslog_from bounds output to num_lines", "[log_collector]") {
    TempDirGuard tmp;
    auto path = tmp.path / "messages";
    {
        std::ofstream ofs(path);
        for (int i = 1; i <= 50; ++i) {
            ofs << "Apr 18 03:48:" << i << " host helix-screen[1]: entry " << i << "\n";
        }
    }

    auto result = helix::logs::tail_syslog_from({path.string()}, 5);

    // Last 5 helix-screen entries: 46, 47, 48, 49, 50
    REQUIRE(result.find("entry 50") != std::string::npos);
    REQUIRE(result.find("entry 46") != std::string::npos);
    REQUIRE(result.find("entry 45") == std::string::npos);
    REQUIRE(result.find("entry 1\n") == std::string::npos);
}

TEST_CASE("helix::logs::tail_syslog_from falls through missing files", "[log_collector]") {
    TempDirGuard tmp;
    auto real = tmp.path / "syslog";
    {
        std::ofstream ofs(real);
        ofs << "Apr 18 10:00:00 host helix-screen[1]: present\n";
    }

    auto result =
        helix::logs::tail_syslog_from({(tmp.path / "missing.log").string(), real.string()}, 10);
    REQUIRE(result.find("present") != std::string::npos);
}

TEST_CASE("helix::logs::tail_syslog_from returns empty when no path readable", "[log_collector]") {
    auto result = helix::logs::tail_syslog_from({"/nonexistent/a", "/nonexistent/b"}, 10);
    REQUIRE(result.empty());
}

TEST_CASE("helix::logs::tail_syslog_from returns empty when no matching lines", "[log_collector]") {
    TempDirGuard tmp;
    auto path = tmp.path / "messages";
    {
        std::ofstream ofs(path);
        ofs << "Apr 18 03:48:31 host kernel: nothing to see\n"
            << "Apr 18 03:48:32 host sshd: login\n";
    }
    auto result = helix::logs::tail_syslog_from({path.string()}, 10);
    REQUIRE(result.empty());
}

// ============================================================================
// tail_best() cascade
// ============================================================================

TEST_CASE("helix::logs::tail_best uses provided paths first", "[log_collector]") {
    TempDirGuard tmp;
    auto log = tmp.path / "helix.log";
    write_lines(log, 20);

    // Provided paths take precedence over default resolution. The syslog /
    // journal fallbacks should not be consulted when a file is present.
    auto result = helix::logs::tail_best(5, {log.string()});
    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("line 20") != std::string::npos);
    REQUIRE(result.find("line 16") != std::string::npos);
}

TEST_CASE("helix::logs::tail_best steps over a missing path to the next one", "[log_collector]") {
    // Asserting "empty" for an all-missing list is not safe (a dev box could
    // have journalctl entries), so pin the property that actually matters:
    // the file cascade must SKIP an unreadable path and keep going, not abort
    // at the first miss. Put a real file second and require its content back.
    TempDirGuard tmp;
    auto log = tmp.path / "second.log";
    write_lines(log, 12, "cascade"); // TempDirGuard removes the file on scope exit

    auto result = helix::logs::tail_best(10, {"/nonexistent/a.log", log.string()});

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("cascade 12") != std::string::npos);
    REQUIRE(result.find("cascade 3") != std::string::npos);
    // Tail semantics still apply: only the last 10 of 12 lines survive.
    REQUIRE(result.find("cascade 1\n") == std::string::npos);
}

// ============================================================================
// default_file_paths() composition
// ============================================================================

TEST_CASE("helix::logs::default_file_paths always includes /var/log and /tmp", "[log_collector]") {
    auto paths = helix::logs::default_file_paths();
    REQUIRE_FALSE(paths.empty());

    // First entry is the legacy system-wide file
    REQUIRE(paths.front() == "/var/log/helix-screen.log");
    // Last entry is the tmp fallback
    REQUIRE(paths.back() == "/tmp/helixscreen.log");
}

// ============================================================================
// ad5x_mod_layout_present() — AD5X mod-tree recognition (ZMOD or Forge-X).
// Pinned to the SAME truth table as the launcher's heap-diag gate
// (tests/shell/test_helix_launcher_env.bats, "heap diag:" cases): a Forge-X
// chroot carries none of the ZMOD markers, so the reachable mod git tree
// (.shell/platform.sh) is the recognition evidence there.
// ============================================================================

TEST_CASE("helix::logs::ad5x_mod_layout_present detects ZMOD by marker file", "[log_collector]") {
    TempDirGuard tmp;
    REQUIRE(
        helix::logs::ad5x_mod_layout_present(make_ad5x_layout(tmp.path, "zmod-marker").string()));
}

TEST_CASE("helix::logs::ad5x_mod_layout_present detects ZMOD by FlashForge /usr/prog dir",
          "[log_collector]") {
    TempDirGuard tmp;
    REQUIRE(helix::logs::ad5x_mod_layout_present(make_ad5x_layout(tmp.path, "zmod-prog").string()));
}

TEST_CASE("helix::logs::ad5x_mod_layout_present detects a Forge-X chroot via the mod tree",
          "[log_collector]") {
    TempDirGuard tmp;
    // The rig's chroot binds /usr/data at /opt, so the mod tree is reachable
    // under exactly one of the two spellings depending on which side of the
    // chroot the process runs. Both must count.
    REQUIRE(helix::logs::ad5x_mod_layout_present(make_ad5x_layout(tmp.path, "forgex").string()));
    REQUIRE(
        helix::logs::ad5x_mod_layout_present(make_ad5x_layout(tmp.path, "forgex-host").string()));
}

TEST_CASE("helix::logs::ad5x_mod_layout_present rejects a plain host", "[log_collector]") {
    TempDirGuard tmp;
    REQUIRE_FALSE(
        helix::logs::ad5x_mod_layout_present(make_ad5x_layout(tmp.path, "plain").string()));
}

TEST_CASE("helix::logs::ad5x_mod_layout_present requires the marker kinds, not just names",
          "[log_collector]") {
    // /ZMOD as a DIRECTORY is not the Z-Mod marker file. The predicate checks
    // S_ISREG here and the launcher uses `-f` — the two must stay in step or
    // a stray directory arms AD5X handling on the wrong host.
    TempDirGuard tmp;
    auto root = tmp.path / "zmod-dir";
    fs::create_directories(root / "ZMOD");
    REQUIRE_FALSE(helix::logs::ad5x_mod_layout_present(root.string()));
}

TEST_CASE("helix::logs::default_file_paths gates the AD5X mod_data logs on the layout",
          "[log_collector]") {
    // The mod_data cascade entries (app log + init-script stdout redirect, in
    // both bind-mount spellings) belong on hosts that carry an AD5X mod tree —
    // ZMOD or Forge-X — and nowhere else. On a plain host they were dead stat
    // probes pointing at paths no helix install ever wrote.
    constexpr std::array<const char*, 4> mod_data_paths = {
        "/opt/config/mod_data/log/helix.log",
        "/usr/data/config/mod_data/log/helix.log",
        "/opt/config/mod_data/log/helixscreen.log",
        "/usr/data/config/mod_data/log/helixscreen.log",
    };
    auto has_all = [&](const std::vector<std::string>& paths) {
        for (const char* want : mod_data_paths) {
            if (std::find(paths.begin(), paths.end(), want) == paths.end())
                return false;
        }
        return true;
    };

    TempDirGuard tmp;
    REQUIRE(
        has_all(helix::logs::default_file_paths(make_ad5x_layout(tmp.path, "zmod-prog").string())));
    REQUIRE(has_all(
        helix::logs::default_file_paths(make_ad5x_layout(tmp.path, "zmod-marker").string())));
    REQUIRE(
        has_all(helix::logs::default_file_paths(make_ad5x_layout(tmp.path, "forgex").string())));

    auto plain = helix::logs::default_file_paths(make_ad5x_layout(tmp.path, "plain").string());
    for (const char* absent : mod_data_paths) {
        REQUIRE(std::find(plain.begin(), plain.end(), absent) == plain.end());
    }
    // Everything a plain host legitimately has is still there.
    REQUIRE(std::find(plain.begin(), plain.end(), "/var/log/helix-screen.log") != plain.end());
    REQUIRE(std::find(plain.begin(), plain.end(), "/tmp/helixscreen.log") != plain.end());
}

TEST_CASE("helix::logs::default_file_paths honors XDG_DATA_HOME when set", "[log_collector]") {
    setenv("XDG_DATA_HOME", "/custom/xdg", 1);
    auto paths = helix::logs::default_file_paths();
    unsetenv("XDG_DATA_HOME");

    bool found = false;
    for (const auto& p : paths) {
        if (p == "/custom/xdg/helix-screen/helix.log") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}
