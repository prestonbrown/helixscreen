// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/unique_temp_dir.h"
#include "core/data_root_resolver.h"
#include "json_utils.h"
#include "printer_event_journal.h"
#include "system/debug_bundle_collector.h"
#include "wifi_saved_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utime.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ConfigDirGuard;
using json = nlohmann::json;

/**
 * Task 11's helix::wifi::store keeps every PSK the user has ever entered in
 * a HelixScreen-owned file, independent of wpa_supplicant's own config.
 * Debug bundles are user-uploaded and land in a shared triage flow, and
 * bundle log_tail already leaks SSIDs (prestonbrown/helixscreen#1191) — a
 * file of cleartext PSKs must not become a second, worse exit.
 *
 * collect_platform_files() uses an explicit allowlist (see
 * platform_diagnostic_files() in debug_bundle_collector.cpp) and
 * collect_sanitized_settings() reads only settings.json — neither names the
 * wifi store today, so this test is expected to PASS immediately. Its job is
 * to keep that true as a regression guard: something has to fail loudly the
 * day a future collector change starts sweeping arbitrary config-dir files
 * (or the store itself) into a bundle.
 */

namespace {

/// True if any key anywhere in the JSON tree (object keys at any depth,
/// including inside arrays) is exactly @p key_name. Recurses into both
/// objects and arrays so a collector that nests the store under some other
/// wrapper key still gets caught.
bool contains_key_anywhere(const json& node, const std::string& key_name) {
    if (node.is_object()) {
        for (const auto& [key, value] : node.items()) {
            if (key == key_name)
                return true;
            if (contains_key_anywhere(value, key_name))
                return true;
        }
    } else if (node.is_array()) {
        for (const auto& value : node) {
            if (contains_key_anywhere(value, key_name))
                return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("A saved WiFi PSK never reaches a debug bundle", "[debug-bundle][wifi][security]") {
    ConfigDirGuard guard("debug_bundle_no_secrets");

    // A unique, long, greppable literal — unambiguous as a substring match,
    // and distinctive enough that it could not plausibly appear in the
    // bundle for any reason other than the store leaking into it.
    const std::string distinctive_psk = "correct-horse-battery-staple-9f3e7ab21c";
    const std::string distinctive_ssid = "TotallyUniqueTestNetworkSSID-9f3e7ab21c";

    REQUIRE(helix::wifi::store::save({distinctive_ssid, distinctive_psk}));
    REQUIRE(helix::wifi::store::load().size() == 1);

    const json bundle = helix::DebugBundleCollector::collect();
    const std::string serialized = helix::json_util::safe_dump(bundle);

    // The PSK itself must never appear, anywhere, in any form.
    CHECK(serialized.find(distinctive_psk) == std::string::npos);

    // The SSID is PII too (the substance of #1191): a bundle that omits the
    // PSK but leaks the network name is still a leak.
    CHECK(serialized.find(distinctive_ssid) == std::string::npos);

    // No key named "wifi_networks" anywhere in the tree — that would be the
    // shape a naive "just dump the store file" collector change would take.
    CHECK_FALSE(contains_key_anywhere(bundle, "wifi_networks"));

    // Nor should the store's own filename appear — neither as a bundled
    // file's logical key nor embedded in some path/error string.
    const std::string store_basename =
        std::filesystem::path(helix::wifi::store::store_path()).filename().string();
    INFO("store basename: " << store_basename);
    CHECK(serialized.find(store_basename) == std::string::npos);
}

/**
 * The bundle captures Moonraker DB namespaces by explicit allowlist so a
 * filament-identity report arrives with the override records that decide what
 * the user actually sees. The same database holds Moonraker's user accounts and
 * API key, and `gcode_metadata` holds base64 thumbnails, so the allowlist is the
 * safety property: sanitize_value() is field-aware and cannot know what is
 * sensitive inside a namespace nobody has looked at.
 */
TEST_CASE("Bundled DB namespaces stay an allowlist of filament overrides",
          "[debug-bundle][security]") {
    const auto namespaces = helix::DebugBundleCollector::filament_override_namespaces();

    SECTION("it carries the three override stores and nothing else") {
        REQUIRE(namespaces.size() == 3);
        CHECK(std::find(namespaces.begin(), namespaces.end(), "lane_data") != namespaces.end());
        CHECK(std::find(namespaces.begin(), namespaces.end(), "helix-screen-afc-overrides") !=
              namespaces.end());
        CHECK(std::find(namespaces.begin(), namespaces.end(), "helix-screen-hh-overrides") !=
              namespaces.end());
    }

    SECTION("no credential-bearing or bulk namespace is ever listed") {
        // A future addition that reaches for one of these trips here rather
        // than shipping it to whoever the reporter forwards the bundle to.
        static const char* forbidden[] = {"authorization", "authorized_users", "users",  "secrets",
                                          "moonraker",     "gcode_metadata",   "history"};
        for (const auto& ns : namespaces) {
            for (const char* bad : forbidden) {
                INFO("namespace: " << ns << " vs forbidden: " << bad);
                CHECK(ns.find(bad) == std::string::npos);
            }
        }
    }
}

/**
 * The host census reports which processes contend for the display. A process's
 * arguments carry paths, device serials and addresses, so only argv[0]'s name
 * leaves, and every census string goes through sanitize_value().
 */
TEST_CASE("The host census ships process names, never their arguments",
          "[debug-bundle][security][1692]") {
    helix::diag::HostCensus census;
    census.os_pretty_name = "Vendor Linux built by builder@example.com";
    census.failed_units = std::vector<std::string>{"lightdm.service"};

    const std::string serial = "SN-9f3e7ab21c";
    const auto root = std::filesystem::temp_directory_path() / "helix_census_no_secrets";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "proc" / "42");
    {
        std::ofstream cmd(root / "proc" / "42" / "cmdline", std::ios::binary);
        cmd << "/usr/bin/guppyscreen" << '\0' << "--camera" << '\0' << "/dev/v4l/by-id/usb-Cam_"
            << serial << '\0';
    }
    census.processes = helix::diag::census_processes((root / "proc").string(), 32);
    std::filesystem::remove_all(root);
    REQUIRE(census.processes.size() == 1);

    const json sys = helix::DebugBundleCollector::collect_system_info({}, census);
    const std::string serialized = sys.dump();

    CHECK(serialized.find(serial) == std::string::npos);
    CHECK(serialized.find("--camera") == std::string::npos);
    CHECK(serialized.find("builder@example.com") == std::string::npos);
    CHECK(sys.at("display_processes").at(0).at("name") == json("guppyscreen"));
    CHECK(sys.at("failed_units") == json::array({"lightdm.service"}));

    SECTION("a box where systemd gave no answer reports no failed-unit list at all") {
        census.failed_units.reset();
        CHECK_FALSE(
            helix::DebugBundleCollector::collect_system_info({}, census).contains("failed_units"));
    }
}

namespace {

/// A printer's log directory on this host, handed to collect() through its
/// LogLocator. Each file's mtime is set explicitly, so the bundle's age and
/// staleness verdicts read from literals.
class LocalLogDir {
  public:
    LocalLogDir() : root_(helix::test::unique_temp_dir("helix_bundle_logs")) {
        std::filesystem::create_directories(root_);
    }
    ~LocalLogDir() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::string write(const std::string& name, const std::string& body, std::time_t mtime) {
        const std::string path = root_ + "/" + name;
        std::ofstream(path) << body;
        const utimbuf times{mtime, mtime};
        REQUIRE(::utime(path.c_str(), &times) == 0);
        paths_[name] = path;
        return path;
    }

    helix::LogLocator locator() const {
        return [paths = paths_](const std::string& name) -> std::vector<std::string> {
            auto it = paths.find(name);
            if (it == paths.end())
                return {};
            return {it->second};
        };
    }

  private:
    std::string root_;
    std::map<std::string, std::string> paths_;
};

/// A klippy.log tail as Klipper writes it: the start banner, a config dump
/// (which the bundle elides whole), then the runtime lines a user's macros and
/// shell commands put secrets into, among the ordinary lines a triager reads.
const char* const kKlippyLog =
    "Start printer at Mon Nov 16 10:00:00 2026 (1763287200.0 12.3)\n"
    "===== Config file =====\n"
    "[gcode_shell_command snapshot]\n"
    "command = curl -s http://192.168.1.40:8080/?action=snapshot\n"
    "=======================\n"
    "Running command {curl -s http://octo:hunter2pass@192.168.1.40:8080/?action=snapshot}\n"
    "Read 1781.2: 'NOTIFY URL=https://discord.com/api/webhooks/112233/AbCdEfGhIjKlMnOp\\n'\n"
    "ghp_aB3dE5fG7hI9jK1lM3nO5pQ7rS9tU1vW3xY5\n"
    "Echo: print started by owner.name@example.com\n"
    "webhooks client 1234: New connection from 8.8.4.4\n"
    "webhooks client 5678: New connection from 192.168.1.50\n"
    "Host network: wlan0 ether dc:a6:32:12:34:56\n"
    "MCU error during connect\n"
    "Stats 1234.5: gcodein=0 mcu: mcu_awake=0.003 mcu_task_avg=0.000010\n";

} // namespace

/**
 * klippy.log read off this host's disk goes through the same line scrubber as
 * every other log in the bundle. The config dump inside it is where a user's
 * webhook URLs, shell-command credentials and pasted tokens end up.
 */
TEST_CASE("A local klippy.log reaches the bundle scrubbed and otherwise intact",
          "[debug-bundle][security]") {
    LocalLogDir dir;
    const std::time_t now = std::time(nullptr);
    const std::string klippy = dir.write("klippy.log", kKlippyLog, now);

    helix::BundleOptions opts;
    opts.include_klipper_logs = true;
    opts.log_locator = dir.locator();
    const json bundle = helix::DebugBundleCollector::collect(opts);

    REQUIRE(bundle.contains("klipper_log"));
    const std::string log = bundle["klipper_log"].get<std::string>();
    INFO(log);
    // Read from the fixture, not from Moonraker or this machine's own Klipper.
    REQUIRE(log.rfind("[read from local disk: " + klippy + ", last written ", 0) == 0);

    SECTION("secrets are redacted") {
        CHECK(log.find("discord.com/api/webhooks") == std::string::npos);
        CHECK(log.find("AbCdEfGhIjKlMnOp") == std::string::npos);
        CHECK(log.find("[REDACTED_WEBHOOK]") != std::string::npos);

        CHECK(log.find("hunter2pass") == std::string::npos);
        CHECK(log.find("http://[REDACTED_CREDENTIALS]@192.168.1.40:8080/?action=snapshot") !=
              std::string::npos);

        CHECK(log.find("ghp_aB3dE5fG7hI9jK1lM3nO5pQ7rS9tU1vW3xY5") == std::string::npos);
        CHECK(log.find("\n[REDACTED_TOKEN]\n") != std::string::npos);

        CHECK(log.find("owner.name@example.com") == std::string::npos);
        CHECK(log.find("Echo: print started by [REDACTED_EMAIL]") != std::string::npos);

        CHECK(log.find("dc:a6:32:12:34:56") == std::string::npos);
        CHECK(log.find("wlan0 ether [REDACTED_MAC]") != std::string::npos);

        CHECK(log.find("8.8.4.4") == std::string::npos);
        CHECK(log.find("webhooks client 1234: New connection from ip#") != std::string::npos);
    }

    SECTION("diagnostic lines survive") {
        CHECK(log.find("Start printer at Mon Nov 16 10:00:00 2026") != std::string::npos);
        CHECK(log.find("New connection from 192.168.1.50") != std::string::npos);
        CHECK(log.find("MCU error during connect") != std::string::npos);
        CHECK(log.find("Stats 1234.5: gcodein=0 mcu: mcu_awake=0.003") != std::string::npos);
    }
}

/**
 * The Klipper log metadata compares the klippy.log's last write with Klippy's
 * latest connection in moonraker.log, both read through the same locator. A
 * copy written before that connection is some earlier session's and says so.
 */
TEST_CASE("klipper_log_meta judges a local klippy.log against moonraker.log",
          "[debug-bundle][klippy]") {
    helix::LocalEventJournal::instance().clear();
    LocalLogDir dir;
    // 2020-01-01T00:00:00Z.
    const std::time_t old_write = 1577836800;
    const std::string moonraker_log =
        "2026-11-16 10:05:00,123 [klippy_connection.py:_on_connection_established()] - "
        "Klippy Connection Established\n";

    helix::BundleOptions opts;
    opts.include_klipper_logs = true;
    opts.include_moonraker_logs = true;

    SECTION("a klippy.log older than the connection is stale and bannered") {
        const std::string klippy = dir.write("klippy.log", kKlippyLog, old_write);
        dir.write("moonraker.log", moonraker_log, std::time(nullptr));
        opts.log_locator = dir.locator();
        const json bundle = helix::DebugBundleCollector::collect(opts);

        REQUIRE(bundle.contains("klipper_log_meta"));
        const json& meta = bundle["klipper_log_meta"];
        INFO(meta.dump());
        CHECK(meta["source"] == "local");
        CHECK(meta["path"] == klippy);
        CHECK(meta["modified"] == "2020-01-01T00:00:00Z");
        CHECK(meta["last_stamp"] == "2026-11-16 10:00:00");
        CHECK(meta["klippy_connected_from"] == "moonraker.log");
        CHECK(meta["stale"] == true);
        CHECK(bundle["klipper_log"].get<std::string>().rfind(
                  "[helix] STALE: this klippy.log was last written 2020-01-01T00:00:00Z", 0) == 0);
    }

    SECTION("a klippy.log written since the connection is current") {
        dir.write("klippy.log", kKlippyLog, std::time(nullptr));
        dir.write("moonraker.log", moonraker_log, std::time(nullptr));
        opts.log_locator = dir.locator();
        const json bundle = helix::DebugBundleCollector::collect(opts);

        REQUIRE(bundle.contains("klipper_log_meta"));
        CHECK(bundle["klipper_log_meta"]["klippy_connected_from"] == "moonraker.log");
        CHECK(bundle["klipper_log_meta"]["stale"] == false);
        CHECK(bundle["klipper_log"].get<std::string>().rfind("[read from local disk: ", 0) == 0);
    }
}

/// The launcher log comes from the locator too, so a bundle built from fixture
/// files carries the fixture's exit records and nothing from this machine.
TEST_CASE("The bundle's launcher_log comes from the located launcher.log",
          "[debug-bundle][security]") {
    LocalLogDir dir;
    const std::string launcher =
        dir.write("launcher.log",
                  "[2026-11-16 10:00:00] [helix-launcher] helix-screen exited with code 139\n"
                  "[2026-11-16 10:00:01] [helix-launcher] restarting as admin@example.com\n",
                  std::time(nullptr));

    helix::BundleOptions opts;
    opts.log_locator = dir.locator();
    const json bundle = helix::DebugBundleCollector::collect(opts);

    REQUIRE(bundle.contains("launcher_log"));
    const std::string log = bundle["launcher_log"].get<std::string>();
    INFO(log);
    CHECK(log.rfind("[read from local disk: " + launcher, 0) == 0);
    CHECK(log.find("helix-screen exited with code 139") != std::string::npos);
    CHECK(log.find("admin@example.com") == std::string::npos);
    // Klipper logs were not asked for, so none ship.
    CHECK_FALSE(bundle.contains("klipper_log"));
}
