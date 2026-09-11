// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "data_root_resolver.h"
#include "system/debug_bundle_collector.h"
#include "wifi_saved_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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

/// Point HELIX_CONFIG_DIR at an isolated temp directory for the duration of
/// a test, restoring whatever was there before on scope exit. Mirrors
/// tests/unit/test_wifi_saved_config_store.cpp's ConfigDirGuard — both the
/// wifi store and collect_sanitized_settings() resolve paths through
/// helix::writable_path(), which honors this variable.
class ConfigDirGuard {
  public:
    explicit ConfigDirGuard(const std::string& dir) {
        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            had_prev_ = true;
            prev_ = prev;
        }
        setenv("HELIX_CONFIG_DIR", dir.c_str(), 1);
    }

    ~ConfigDirGuard() {
        if (had_prev_)
            setenv("HELIX_CONFIG_DIR", prev_.c_str(), 1);
        else
            unsetenv("HELIX_CONFIG_DIR");
    }

    ConfigDirGuard(const ConfigDirGuard&) = delete;
    ConfigDirGuard& operator=(const ConfigDirGuard&) = delete;

  private:
    bool had_prev_ = false;
    std::string prev_;
};

/// Fresh, empty temp dir per test so runs never see a previous test's store.
std::string make_temp_dir(const std::string& name) {
    const std::string dir = "/tmp/" + name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

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
    ConfigDirGuard guard(make_temp_dir("helix_debug_bundle_no_secrets"));

    // A unique, long, greppable literal — unambiguous as a substring match,
    // and distinctive enough that it could not plausibly appear in the
    // bundle for any reason other than the store leaking into it.
    const std::string distinctive_psk = "correct-horse-battery-staple-9f3e7ab21c";
    const std::string distinctive_ssid = "TotallyUniqueTestNetworkSSID-9f3e7ab21c";

    REQUIRE(helix::wifi::store::save({distinctive_ssid, distinctive_psk}));
    REQUIRE(helix::wifi::store::load().size() == 1);

    const json bundle = helix::DebugBundleCollector::collect();
    const std::string serialized = bundle.dump();

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
