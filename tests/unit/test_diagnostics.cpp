// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for helix::diagnostics — the single producer behind the startup log
// block, the About panel and the debug bundle's `diagnostics` section.
//
// The questions that matter are the ones a support case turns on: does an
// override win over the platform default, which cache rung actually won, which
// of the three layouts one platform key is serving, does a board with a partial
// /proc degrade or crash, and does the LAN address leave the machine.

#include "../test_helpers/scoped_env.h"
#include "platform_info.h"
#include "system/debug_bundle_collector.h"
#include "system/diagnostics.h"
#include "test_helpers/flashforge_layout_fixture.h"
#include "test_helpers/scoped_board.h"
#include "test_helpers/unique_temp_dir.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = helix::test::unique_temp_dir("helix-diag-test");
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& p, const std::string& body) {
    std::ofstream(p) << body;
}

} // namespace

// ============================================================================
// Paths: an override must win over the platform default
// ============================================================================

TEST_CASE("diagnostics reports an overridden cache dir, not the platform default",
          "[diagnostics]") {
    TempDir tmp;
    helix::ScopedEnv cache_override("HELIX_CACHE_DIR", tmp.path.c_str());

    const auto diag = helix::diagnostics::collect();

    REQUIRE(diag.paths.cache_dir.rfind(tmp.path.string(), 0) == 0);
}

TEST_CASE("diagnostics names the cache rung that won", "[diagnostics]") {
    SECTION("the env override announces itself") {
        TempDir tmp;
        helix::ScopedEnv cache_override("HELIX_CACHE_DIR", tmp.path.c_str());
        REQUIRE(helix::diagnostics::collect().paths.cache_tier == "HELIX_CACHE_DIR");
    }

    SECTION("a fall-through rung is not attributed to the override") {
        helix::ScopedEnv cache_override("HELIX_CACHE_DIR", nullptr);
        REQUIRE(helix::diagnostics::collect().paths.cache_tier != "HELIX_CACHE_DIR");
    }
}

TEST_CASE("collect() reports the cache without creating it", "[diagnostics]") {
    TempDir tmp;
    const fs::path base = tmp.path / "not-yet-mounted";
    helix::ScopedEnv cache_override("HELIX_CACHE_DIR", base.c_str());

    const auto d = helix::diagnostics::collect();

    REQUIRE(d.paths.cache_tier == "HELIX_CACHE_DIR");
    REQUIRE(d.paths.cache_dir == base.string());
    // Reporting where the cache lives must not decide where it lives. An
    // override naming a stick that is not mounted has a writable parent, so
    // resolving it here would build the tree on the root filesystem instead.
    REQUIRE_FALSE(fs::exists(base));
}

TEST_CASE("the reported cache path and tier name the same rung", "[diagnostics]") {
    TempDir tmp;
    // A regular file where the cache directory would go. The parent is
    // writable so the candidate is viable, and mkdir over a file cannot
    // succeed — the split that lets a resolved path and a peeked tier drift.
    const fs::path blocked = tmp.path / "blocked";
    write_file(blocked, "not a directory");
    helix::ScopedEnv cache_override("HELIX_CACHE_DIR", blocked.c_str());

    const auto d = helix::diagnostics::collect();

    INFO("cache_dir was: " << d.paths.cache_dir);
    REQUIRE(d.paths.cache_tier == "HELIX_CACHE_DIR");
    REQUIRE(d.paths.cache_dir.rfind(blocked.string(), 0) == 0);
}

TEST_CASE("the updater staging dir is a sibling of the install root", "[diagnostics]") {
    // install.sh rm -rf's TMP_DIR and moves INSTALL_DIR, so a staging dir
    // under the install root would be destroyed mid-update.
    REQUIRE(helix::diagnostics::staging_dir_for("/home/pi/helixscreen") ==
            "/home/pi/.helix-update-staging");
    REQUIRE(helix::diagnostics::staging_dir_for("/opt/helixscreen/") ==
            "/opt/.helix-update-staging");
    REQUIRE(helix::diagnostics::staging_dir_for("").empty());
}

// ============================================================================
// Identity: one platform key, three filesystem layouts
// ============================================================================

TEST_CASE("the mod flavor separates the layouts one platform key serves", "[diagnostics]") {
    TempDir tmp;
    auto name_of = [&](const char* layout) {
        return std::string(helix::flashforge_mod_tree_name(helix::flashforge_mod_tree(
            helix::test::make_flashforge_layout(tmp.path, layout).string())));
    };

    CHECK(name_of("zmod-marker") == "ZMOD");
    CHECK(name_of("zmod-prog") == "ZMOD");
    CHECK(name_of("forgex") == "Forge-X");
    CHECK(name_of("forgex-host") == "Forge-X");
    CHECK(name_of("plain") == "none");
}

TEST_CASE("diagnostics names the board", "[diagnostics]") {
    helix::test::ScopedBoard board(helix::board::Board::Creator5Pro);
    CHECK(helix::diagnostics::collect().identity.board == "creator5-pro");
}

TEST_CASE("the startup log line names the board", "[diagnostics]") {
    helix::test::ScopedBoard board(helix::board::Board::Creator5Pro);
    std::ostringstream captured;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
    sink->set_pattern("%v");
    auto capture = std::make_shared<spdlog::logger>("diag_capture", sink);
    capture->set_level(spdlog::level::trace);
    auto original = spdlog::default_logger();
    spdlog::set_default_logger(capture);
    helix::diagnostics::log_diagnostics();
    spdlog::set_default_logger(original);

    CHECK(captured.str().find("creator5-pro") != std::string::npos);
}

TEST_CASE("the bundle form carries the board", "[diagnostics]") {
    helix::diagnostics::Diagnostics d;
    d.identity.board = "k1";
    CHECK(helix::DebugBundleCollector::build_diagnostics_info(d)["identity"]["board"] == "k1");
}

TEST_CASE("collect() reports a mod flavor for every host", "[diagnostics]") {
    const auto id = helix::diagnostics::collect().identity;
    REQUIRE((id.mod_flavor == "none" || id.mod_flavor == "ZMOD" || id.mod_flavor == "Forge-X"));
    REQUIRE_FALSE(id.platform_key.empty());
}

// ============================================================================
// Machine: a partial /proc degrades, it does not crash
// ============================================================================

TEST_CASE("machine facts degrade when /proc is missing", "[diagnostics]") {
    TempDir tmp;
    const auto m = helix::diagnostics::read_machine((tmp.path / "absent").string());

    REQUIRE(m.cpu_model == "unknown");
    REQUIRE(m.cpu_cores == 0);
    REQUIRE(m.mem_total_kb == 0);
    REQUIRE(m.mem_available_kb == 0);
    REQUIRE(m.loadavg == "unknown");
    // uname answers regardless of /proc, so these stay real.
    REQUIRE_FALSE(m.kernel_release.empty());
    REQUIRE_FALSE(m.kernel_arch.empty());
}

TEST_CASE("machine facts survive a /proc that carries only some sources", "[diagnostics]") {
    TempDir tmp;
    write_file(tmp.path / "meminfo", "MemTotal:       473384 kB\nMemAvailable:   102400 kB\n");
    write_file(tmp.path / "uptime", "12345.67 98765.43\n");

    const auto m = helix::diagnostics::read_machine(tmp.path.string());

    REQUIRE(m.mem_total_kb == 473384);
    REQUIRE(m.mem_available_kb == 102400);
    REQUIRE(m.uptime_seconds == Catch::Approx(12345.67));
    REQUIRE(m.cpu_model == "unknown"); // no cpuinfo in this root
    REQUIRE(m.loadavg == "unknown");   // no loadavg in this root
}

TEST_CASE("machine facts read an ARM-style cpuinfo with no model name line", "[diagnostics]") {
    TempDir tmp;
    write_file(tmp.path / "cpuinfo", "processor\t: 0\nBogoMIPS\t: 48.00\n"
                                     "processor\t: 1\nBogoMIPS\t: 48.00\n"
                                     "Hardware\t: Allwinner sun8i Family\nRevision\t: 0000\n");

    const auto m = helix::diagnostics::read_machine(tmp.path.string());

    REQUIRE(m.cpu_cores == 2);
    REQUIRE(m.cpu_model == "Allwinner sun8i Family");
}

// /proc is the only source here, and macOS has none. The app never ships
// there, so the host case is a Linux case.
#ifndef __APPLE__
TEST_CASE("machine facts read this host", "[diagnostics]") {
    const auto m = helix::diagnostics::read_machine();
    REQUIRE(m.cpu_cores > 0);
    REQUIRE(m.mem_total_kb > 0);
    REQUIRE(m.uptime_seconds > 0.0);
    REQUIRE(m.cpu_model != "unknown");
}
#endif

// ============================================================================
// Bundle form: the address leaves the struct intact and the bundle sanitized
// ============================================================================

TEST_CASE("the bundle form sanitizes the moonraker url, the struct keeps it", "[diagnostics]") {
    // TEST-NET-3 (RFC 5737): routable-shaped, nobody's real printer.
    const std::string url = "http://203.0.113.9:7125";

    helix::diagnostics::Diagnostics d;
    d.moonraker_url = url;
    d.paths.install_root = "/home/pi/helixscreen";

    const auto section = helix::DebugBundleCollector::build_diagnostics_info(d);

    const std::string emitted = section["moonraker_url"].get<std::string>();
    REQUIRE(emitted.find("203.0.113.9") == std::string::npos);
    REQUIRE(emitted != url);
    // The raw struct is what the local consumers render, and it is untouched.
    REQUIRE(d.moonraker_url == url);
}

TEST_CASE("the bundle form carries the resolved paths and the cache tier", "[diagnostics]") {
    helix::diagnostics::Diagnostics d;
    d.paths.config_dir = "/etc/klipper/config/helixscreen";
    d.paths.cache_dir = "/usr/data/helixscreen-state/cache";
    d.paths.cache_tier = "k1";
    d.identity.platform_key = "mips";
    d.identity.mod_flavor = "Forge-X";
    d.machine.cpu_cores = 4;

    const auto section = helix::DebugBundleCollector::build_diagnostics_info(d);

    REQUIRE(section["paths"]["config_dir"] == "/etc/klipper/config/helixscreen");
    REQUIRE(section["paths"]["cache_tier"] == "k1");
    REQUIRE(section["identity"]["platform_key"] == "mips");
    REQUIRE(section["identity"]["mod_flavor"] == "Forge-X");
    REQUIRE(section["machine"]["cpu_cores"] == 4);
}
