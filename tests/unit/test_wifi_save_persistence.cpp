// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if !defined(__APPLE__) && !defined(__ANDROID__)

#include "../../include/wifi_backend_wpa_supplicant.h"

#include "../catch_amalgamated.hpp"

/**
 * WiFi credential persistence verification.
 *
 * Device-verified on a Snapmaker U1 (2026-07-29): wpa_supplicant answers
 * SAVE_CONFIG with "OK" and never writes the config file. Reply "OK", config
 * mtime unchanged, zero `network={` blocks afterwards, and the path provably
 * writable (a direct append succeeded). The credentials lived in the daemon's
 * memory only, so every power-off silently lost the user's WiFi — matching the
 * field report "WiFi disconnects every time I turn off the printer".
 *
 * The old code took `save_result == "OK\n"` as proof and logged "Configuration
 * saved to disk". These tests pin the rule that replaced it: the config file's
 * contents are the only authority on whether credentials will survive a reboot.
 */

using helix::wifi::detail::classify_removal_result;
using helix::wifi::detail::classify_save_result;
using helix::wifi::detail::classify_scan_reply;
using helix::wifi::detail::RemovalPersistence;
using helix::wifi::detail::SavePersistence;
using helix::wifi::detail::ScanTrigger;
using helix::wifi::detail::wpa_config_has_network;
using helix::wifi::detail::wpa_string_is_valid;

namespace {

// A wpa_supplicant config the way the U1 leaves it: headers, no networks.
constexpr const char* EMPTY_CONFIG = "ctrl_interface=/var/run/wpa_supplicant\n"
                                     "ap_scan=1\n"
                                     "update_config=1\n";

constexpr const char* CONFIG_WITH_NETWORK = "ctrl_interface=/var/run/wpa_supplicant\n"
                                            "ap_scan=1\n"
                                            "update_config=1\n"
                                            "\n"
                                            "network={\n"
                                            "\tssid=\"HomeNet\"\n"
                                            "\tpsk=\"secretpass\"\n"
                                            "}\n";

} // namespace

TEST_CASE("SAVE_CONFIG OK with an unwritten config is not persistence",
          "[network][wpa][persistence][regression]") {
    // THE U1 REGRESSION. If this ever returns Persisted again, HelixScreen is
    // back to telling users their WiFi is saved when it is not.
    REQUIRE(classify_save_result("OK\n", EMPTY_CONFIG, "HomeNet") == SavePersistence::NotPersisted);
}

TEST_CASE("SAVE_CONFIG persistence classification", "[network][wpa][persistence]") {
    SECTION("OK plus the SSID actually on disk is persisted") {
        REQUIRE(classify_save_result("OK\n", CONFIG_WITH_NETWORK, "HomeNet") ==
                SavePersistence::Persisted);
    }

    SECTION("explicit FAIL is not persisted") {
        REQUIRE(classify_save_result("FAIL\n", CONFIG_WITH_NETWORK, "HomeNet") ==
                SavePersistence::NotPersisted);
    }

    SECTION("empty reply is not persisted") {
        REQUIRE(classify_save_result("", CONFIG_WITH_NETWORK, "HomeNet") ==
                SavePersistence::NotPersisted);
    }

    SECTION("a different SSID on disk does not count as ours") {
        REQUIRE(classify_save_result("OK\n", CONFIG_WITH_NETWORK, "SomeOtherNet") ==
                SavePersistence::NotPersisted);
    }

    SECTION("OK without a trailing newline still parses") {
        REQUIRE(classify_save_result("OK", CONFIG_WITH_NETWORK, "HomeNet") ==
                SavePersistence::Persisted);
    }
}

TEST_CASE("wpa_config_has_network matches whole SSID tokens", "[network][wpa][persistence]") {
    SECTION("exact match") {
        REQUIRE(wpa_config_has_network(CONFIG_WITH_NETWORK, "HomeNet"));
    }

    SECTION("a prefix of a longer SSID must not match") {
        // ssid="HomeNet" on disk must not satisfy a request for "Home".
        REQUIRE_FALSE(wpa_config_has_network(CONFIG_WITH_NETWORK, "Home"));
    }

    SECTION("a longer SSID than the one on disk must not match") {
        REQUIRE_FALSE(wpa_config_has_network(CONFIG_WITH_NETWORK, "HomeNetExtra"));
    }

    SECTION("scan_ssid= must not be mistaken for ssid=") {
        const std::string cfg = "network={\n\tscan_ssid=\"Decoy\"\n\tssid=\"Real\"\n}\n";
        REQUIRE_FALSE(wpa_config_has_network(cfg, "Decoy"));
        REQUIRE(wpa_config_has_network(cfg, "Real"));
    }

    SECTION("empty SSID never matches") {
        REQUIRE_FALSE(wpa_config_has_network(CONFIG_WITH_NETWORK, ""));
        REQUIRE_FALSE(wpa_config_has_network(EMPTY_CONFIG, ""));
    }

    SECTION("empty config never matches") {
        REQUIRE_FALSE(wpa_config_has_network("", "HomeNet"));
    }

    SECTION("multiple networks — finds the one asked for") {
        const std::string cfg = "network={\n\tssid=\"First\"\n}\n"
                                "network={\n\tssid=\"Second\"\n}\n";
        REQUIRE(wpa_config_has_network(cfg, "First"));
        REQUIRE(wpa_config_has_network(cfg, "Second"));
        REQUIRE_FALSE(wpa_config_has_network(cfg, "Third"));
    }

    SECTION("SSID containing spaces") {
        const std::string cfg = "network={\n\tssid=\"My Home Net\"\n}\n";
        REQUIRE(wpa_config_has_network(cfg, "My Home Net"));
    }
}

// wpa_string_is_valid() is the injection barrier between untrusted SSID/PSK
// input and wpa_supplicant's quoted `SET_NETWORK <id> ssid "<value>"` command
// protocol. It and validate_wpa_string() used to be byte-identical, separately
// maintained rule sets with static (file-only) linkage — never reachable from
// a test. validate_wpa_string() is now a thin wrapper over this predicate, so
// pinning the predicate here covers both.
TEST_CASE("wpa_string_is_valid rejects command-injection characters",
          "[network][wpa][validation]") {
    SECTION("an ordinary SSID is valid") {
        REQUIRE(wpa_string_is_valid("HomeNetwork"));
    }

    SECTION("empty is invalid") {
        REQUIRE_FALSE(wpa_string_is_valid(""));
    }

    SECTION("a double quote would close the command's quoted value early") {
        REQUIRE_FALSE(wpa_string_is_valid("Evil\" psk \"x"));
    }

    SECTION("a backslash is rejected") {
        REQUIRE_FALSE(wpa_string_is_valid("back\\slash"));
    }

    SECTION("embedded newline is rejected") {
        REQUIRE_FALSE(wpa_string_is_valid("line\nbreak"));
    }

    SECTION("embedded carriage return is rejected") {
        REQUIRE_FALSE(wpa_string_is_valid("line\rbreak"));
    }

    SECTION("embedded tab is rejected") {
        REQUIRE_FALSE(wpa_string_is_valid("a\tb"));
    }

    SECTION("other control characters are rejected") {
        REQUIRE_FALSE(wpa_string_is_valid(std::string("a") + '\x01' + "b"));
    }

    SECTION("DEL (127) is rejected") {
        REQUIRE_FALSE(wpa_string_is_valid(std::string("a") + '\x7f' + "b"));
    }

    SECTION("exactly 255 bytes is valid (the boundary)") {
        REQUIRE(wpa_string_is_valid(std::string(255, 'x')));
    }

    SECTION("256 bytes exceeds the limit") {
        REQUIRE_FALSE(wpa_string_is_valid(std::string(256, 'x')));
    }

    SECTION("spaces are allowed — SSIDs may contain them") {
        REQUIRE(wpa_string_is_valid("My Home Net"));
    }
}

/**
 * Forget-network removal verification.
 *
 * AD5X bundles TAU4PW4H / 865DXBQ7 (v0.99.107): the app cannot read the vendor
 * config at /usr/prog/wifi/wpa_supplicant.conf, so the post-save re-read handed
 * the old check an empty string. "SSID not in these contents" then read as a
 * confirmed removal, and the same session logged both "Removal verified on disk"
 * and "did not record this network" about that one file. The user's forgotten
 * 5 GHz AP was the associated network again after the next reboot.
 *
 * The rule these pin: absence only counts as removal when we actually read the
 * file.
 */
TEST_CASE("classify_removal_result separates verified absence from an unread config",
          "[wifi][unit][persistence]") {
    SECTION("read the config and the SSID is gone — verified") {
        REQUIRE(classify_removal_result(true, true, EMPTY_CONFIG, "HomeNet") ==
                RemovalPersistence::Verified);
    }

    SECTION("read the config and the SSID is still there — it comes back at boot") {
        REQUIRE(classify_removal_result(true, true, CONFIG_WITH_NETWORK, "HomeNet") ==
                RemovalPersistence::StillListed);
    }

    SECTION("config unreadable — unverifiable, NOT verified") {
        // The AD5X case: path resolved, open failed, contents empty.
        REQUIRE(classify_removal_result(true, false, "", "HomeNet") ==
                RemovalPersistence::Unverifiable);
    }

    SECTION("unreadable is unverifiable even for an SSID that was never there") {
        // Guards the specific inversion: with no readability check, ANY ssid
        // "verifies" against empty contents.
        REQUIRE(classify_removal_result(true, false, "", "NeverConfigured") ==
                RemovalPersistence::Unverifiable);
    }

    SECTION("no -c path for the daemon — unverifiable") {
        REQUIRE(classify_removal_result(false, false, "", "HomeNet") ==
                RemovalPersistence::Unverifiable);
    }

    SECTION("a genuinely empty-but-read config verifies (not conflated with unreadable)") {
        // readable=true with zero-length contents is a real state — a config
        // wpa_supplicant truncated to nothing. Absence there IS observed.
        REQUIRE(classify_removal_result(true, true, "", "HomeNet") == RemovalPersistence::Verified);
    }
}

/**
 * SCAN reply classification.
 *
 * Bundle TAU4PW4H: forgetting the connected AP disassociates, the overlay
 * restarts scanning immediately, and the user got "WiFi scan failed. Try again."
 * 48 ms after their own Forget tap. FAIL-BUSY means a scan is already running
 * and its SCAN_COMPLETE still arrives, so there is nothing to report.
 */
TEST_CASE("classify_scan_reply treats FAIL-BUSY as a scan in flight, not a failure",
          "[wifi][unit][ad5x]") {
    REQUIRE(classify_scan_reply("OK\n") == ScanTrigger::Started);
    REQUIRE(classify_scan_reply("FAIL-BUSY\n") == ScanTrigger::AlreadyBusy);
    REQUIRE(classify_scan_reply("FAIL\n") == ScanTrigger::Failed);
    REQUIRE(classify_scan_reply("") == ScanTrigger::NoReply);

    SECTION("FAIL-BUSY is not matched by a prefix of a different FAIL reply") {
        REQUIRE(classify_scan_reply("FAIL-BUSYNESS\n") == ScanTrigger::AlreadyBusy);
        REQUIRE(classify_scan_reply("FAIL-B\n") == ScanTrigger::Failed);
    }

    SECTION("an unrecognised reply is a failure, not silently a success") {
        REQUIRE(classify_scan_reply("UNKNOWN COMMAND\n") == ScanTrigger::Failed);
    }
}

#endif // !__APPLE__ && !__ANDROID__
