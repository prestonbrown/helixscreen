// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_local_include_plan.cpp
 * @brief The local-write fallback decision, against a real Creality K2
 *
 * On stock K2 firmware Moonraker is launched as
 *   moonraker.py -c /usr/share/moonraker/moonraker.conf
 * while the file manager's only writable config root is
 *   /mnt/UDISK/printer_data/config
 * so GET /server/files/config/moonraker.conf is a 404 and no HTTP call exists
 * that can reach the file. HelixScreen runs on that printer as root, and the
 * config it needs to edit is a local file, so the fallback is to write it
 * directly — but only once we know exactly which file and which line.
 */

#include "system/moonraker_local_probe.h"

#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::diag::LocalIncludePlan;
using helix::diag::plan_local_include;
using helix::diag::ProcMatch;

namespace {

/// What /proc looks like on the K2: moonraker pointed at a vendor config, plus
/// klippy, which plan_local_include() must not mistake for moonraker.
std::vector<ProcMatch> k2_procs() {
    return {
        {412, "/usr/bin/python3 /usr/share/klipper/klippy/klippy.py "
              "/mnt/UDISK/printer_data/config/printer.cfg -l "
              "/mnt/UDISK/printer_data/logs/klippy.log -a /tmp/klippy_uds"},
        {455, "/usr/bin/python3 /usr/share/moonraker/moonraker/moonraker.py -c "
              "/usr/share/moonraker/moonraker.conf"},
    };
}

const char* K2_ROOT = "/mnt/UDISK/printer_data/config";

} // namespace

TEST_CASE("K2: plan names the vendor config and an absolute include", "[local_include]") {
    auto plan = plan_local_include(k2_procs(), K2_ROOT);

    REQUIRE(plan.viable);
    CHECK(plan.vendor_config_abs == "/usr/share/moonraker/moonraker.conf");
    CHECK(plan.helix_conf_abs == "/mnt/UDISK/printer_data/config/helixscreen.conf");
    // The file API addresses the same file relative to the config root.
    CHECK(plan.helix_conf_upload == "helixscreen.conf");
    // Absolute, because a bare "helixscreen.conf" would resolve against
    // /usr/share/moonraker/ — a file that does not exist, which makes Moonraker
    // refuse to start.
    CHECK(plan.include_line == "[include /mnt/UDISK/printer_data/config/helixscreen.conf]");
    CHECK(plan.error.empty());
}

TEST_CASE("plan derives the config from -d when there is no -c", "[local_include]") {
    std::vector<ProcMatch> procs = {
        {455, "python3 /opt/moonraker/moonraker.py -d /var/lib/printer_data"}};

    auto plan = plan_local_include(procs, K2_ROOT);

    REQUIRE(plan.viable);
    CHECK(plan.vendor_config_abs == "/var/lib/printer_data/config/moonraker.conf");
}

TEST_CASE("plan prefers -c over -d when both are given", "[local_include]") {
    std::vector<ProcMatch> procs = {
        {455, "python3 moonraker.py -d /var/lib/printer_data -c /etc/moonraker/moonraker.conf"}};

    auto plan = plan_local_include(procs, K2_ROOT);

    REQUIRE(plan.viable);
    CHECK(plan.vendor_config_abs == "/etc/moonraker/moonraker.conf");
}

TEST_CASE("K1: a config already under the writable root is not a local-write case",
          "[local_include]") {
    // The working control. Its moonraker.conf IS reachable through the file API,
    // so an Unreachable verdict here means something else is wrong and writing the
    // file behind Moonraker's back would hide it.
    std::vector<ProcMatch> procs = {
        {455, "python3 /usr/share/moonraker/moonraker/moonraker.py -d /usr/data/printer_data"}};

    auto plan = plan_local_include(procs, "/usr/data/printer_data/config");

    CHECK_FALSE(plan.viable);
    CHECK_FALSE(plan.error.empty());
}

TEST_CASE("a config sitting directly in the writable root is not a local-write case",
          "[local_include]") {
    std::vector<ProcMatch> procs = {
        {455, "python3 moonraker.py -c /mnt/UDISK/printer_data/config/moonraker.conf"}};

    CHECK_FALSE(plan_local_include(procs, K2_ROOT).viable);
}

TEST_CASE("a config in a subdirectory of the writable root is not a local-write case",
          "[local_include]") {
    std::vector<ProcMatch> procs = {
        {455, "python3 moonraker.py -c /mnt/UDISK/printer_data/config/vendor/moonraker.conf"}};

    CHECK_FALSE(plan_local_include(procs, K2_ROOT).viable);
}

TEST_CASE("a sibling directory sharing the root's prefix is still a local-write case",
          "[local_include]") {
    // ".../config_backup" must not be swallowed by the ".../config" prefix.
    std::vector<ProcMatch> procs = {
        {455, "python3 moonraker.py -c /mnt/UDISK/printer_data/config_backup/moonraker.conf"}};

    auto plan = plan_local_include(procs, K2_ROOT);
    REQUIRE(plan.viable);
    CHECK(plan.vendor_config_abs == "/mnt/UDISK/printer_data/config_backup/moonraker.conf");
}

TEST_CASE("klippy alone is not evidence of where Moonraker's config lives", "[local_include]") {
    std::vector<ProcMatch> procs = {{412, "/usr/bin/python3 /usr/share/klipper/klippy/klippy.py "
                                          "/mnt/UDISK/printer_data/config/printer.cfg -l "
                                          "/mnt/UDISK/printer_data/logs/klippy.log"}};

    auto plan = plan_local_include(procs, K2_ROOT);

    CHECK_FALSE(plan.viable);
    CHECK_FALSE(plan.error.empty());
}

TEST_CASE("no local processes at all is not viable", "[local_include]") {
    CHECK_FALSE(plan_local_include({}, K2_ROOT).viable);
}

TEST_CASE("a moonraker process with no path flags is not viable", "[local_include]") {
    std::vector<ProcMatch> procs = {{455, "python3 /usr/share/moonraker/moonraker/moonraker.py"}};
    auto plan = plan_local_include(procs, K2_ROOT);
    CHECK_FALSE(plan.viable);
    CHECK_FALSE(plan.error.empty());
}

TEST_CASE("no writable config root means there is nowhere to put helixscreen.conf",
          "[local_include]") {
    // Without a writable root the include target could not be created through the
    // file API, and an include pointing at a nonexistent file stops Moonraker dead.
    auto plan = plan_local_include(k2_procs(), "");
    CHECK_FALSE(plan.viable);
    CHECK_FALSE(plan.error.empty());
}

TEST_CASE("a relative config root is rejected", "[local_include]") {
    // A relative path would resolve against HelixScreen's cwd, not Moonraker's.
    CHECK_FALSE(plan_local_include(k2_procs(), "printer_data/config").viable);
}

TEST_CASE("a relative -c value is rejected", "[local_include]") {
    // Same reason in the other direction: we would write to the wrong file.
    std::vector<ProcMatch> procs = {{455, "python3 moonraker.py -c moonraker.conf"}};
    CHECK_FALSE(plan_local_include(procs, K2_ROOT).viable);
}

TEST_CASE("a trailing slash on the config root does not double up", "[local_include]") {
    auto plan = plan_local_include(k2_procs(), "/mnt/UDISK/printer_data/config/");

    REQUIRE(plan.viable);
    CHECK(plan.helix_conf_abs == "/mnt/UDISK/printer_data/config/helixscreen.conf");
    CHECK(plan.include_line == "[include /mnt/UDISK/printer_data/config/helixscreen.conf]");
}

// ============================================================================
// Symlinked trees — the AD5M
//
// /root/printer_data/config is a symlink to /opt/config (verified on the device,
// 2026-08-15), so Moonraker's -c and the file manager's root name ONE directory
// two ways. A literal prefix test calls that a local-write case and would edit a
// vendor file behind Moonraker's back on a printer whose config the file API
// addresses perfectly well.
// ============================================================================

TEST_CASE("AD5M: a config reached through a symlinked root is not a local-write case",
          "[local_include]") {
    namespace fs = std::filesystem;

    // Build the real shape rather than assert on strings: plan_local_include()
    // resolves through the filesystem, so the link has to exist.
    const fs::path base =
        fs::temp_directory_path() / ("helix-linkplan-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "opt" / "config", ec);
    fs::create_directories(base / "root" / "printer_data", ec);
    fs::create_symlink(base / "opt" / "config", base / "root" / "printer_data" / "config", ec);
    if (ec) {
        fs::remove_all(base, ec);
        SKIP("cannot create symlinks here: " + ec.message());
    }

    const std::string root = (base / "opt" / "config").string();
    const std::string reported =
        (base / "root" / "printer_data" / "config" / "moonraker.conf").string();
    std::vector<ProcMatch> procs = {
        {455, "/usr/bin/python3 /opt/moonraker/moonraker.py -c " + reported}};

    auto plan = plan_local_include(procs, root);

    CHECK_FALSE(plan.viable);
    CHECK(plan.error.find("already inside the writable config root") != std::string::npos);

    fs::remove_all(base, ec);
}

TEST_CASE("a genuinely foreign tree stays a local-write case even with links around",
          "[local_include]") {
    // The mirror of the above: resolving symlinks must not swallow the K2, where
    // the vendor config really does live outside anything the file API serves.
    namespace fs = std::filesystem;

    const fs::path base =
        fs::temp_directory_path() / ("helix-linkplan-foreign-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "mnt" / "UDISK" / "printer_data" / "config", ec);
    fs::create_directories(base / "usr" / "share" / "moonraker", ec);

    const std::string root = (base / "mnt" / "UDISK" / "printer_data" / "config").string();
    const std::string vendor = (base / "usr" / "share" / "moonraker" / "moonraker.conf").string();
    std::vector<ProcMatch> procs = {
        {455, "/usr/bin/python3 /usr/share/moonraker/moonraker.py -c " + vendor}};

    auto plan = plan_local_include(procs, root);

    REQUIRE(plan.viable);
    CHECK(plan.vendor_config_abs == vendor);
    CHECK(plan.helix_conf_abs == root + "/helixscreen.conf");

    fs::remove_all(base, ec);
}
