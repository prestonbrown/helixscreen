// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_helix_plugin_installer.cpp
 * @brief Unit tests for HelixPluginInstaller
 *
 * Tests cover:
 * 1. Local moonraker detection (is_local_moonraker)
 * 2. Preference management (should_prompt_install, set_install_declined)
 * 3. Install script path resolution
 * 4. Remote install command generation
 *
 * NOTE: These tests are written BEFORE the implementation (test-first development).
 *       The HelixPluginInstaller class will be implemented to pass these tests.
 */

#include "../../include/config.h"
#include "../../include/helix_plugin_installer.h"

#include <filesystem>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Test Helper: URL Parsing (tests the internal URL parsing logic)
// ============================================================================

TEST_CASE("HelixPluginInstaller URL parsing", "[plugin_installer]") {
    SECTION("is_local_moonraker answers from the URL the installer was given") {
        // The predicate itself is tested once, in test_host_identity.cpp. What
        // belongs here is the wiring: that the installer names the host from its
        // own URL and refuses to claim locality when it cannot.
        helix::HelixPluginInstaller installer;

        installer.set_websocket_url("ws://localhost:7125/websocket");
        REQUIRE(installer.is_local_moonraker());
        installer.set_websocket_url("ws://127.0.0.1:7125/websocket");
        REQUIRE(installer.is_local_moonraker());
        installer.set_websocket_url("ws://[::1]:7125/websocket");
        REQUIRE(installer.is_local_moonraker());

        // 203.0.113.0/24 is TEST-NET-3 (RFC 5737) — reserved for documentation
        // and never assigned to a real interface, so this stays remote on any
        // machine. An RFC1918 literal would not: is_moonraker_on_same_host()
        // scans local interfaces, and a dev box or CI runner can legitimately
        // hold 192.168.x.x.
        installer.set_websocket_url("ws://203.0.113.7:7125/websocket");
        REQUIRE_FALSE(installer.is_local_moonraker());

        // No URL at all is not evidence of locality — auto-install must not fire.
        installer.set_websocket_url("");
        REQUIRE_FALSE(installer.is_local_moonraker());
    }

    SECTION("extract_host_from_websocket_url parses URLs correctly") {
        // Standard WebSocket URLs (ws://)
        REQUIRE(helix::extract_host_from_websocket_url("ws://localhost:7125/websocket") ==
                "localhost");
        REQUIRE(helix::extract_host_from_websocket_url("ws://127.0.0.1:7125/websocket") ==
                "127.0.0.1");
        REQUIRE(helix::extract_host_from_websocket_url("ws://192.168.1.100:7125/websocket") ==
                "192.168.1.100");
        REQUIRE(helix::extract_host_from_websocket_url("ws://printer.local:7125/websocket") ==
                "printer.local");

        // Secure WebSocket URLs (wss://)
        REQUIRE(helix::extract_host_from_websocket_url("wss://localhost:7125/websocket") ==
                "localhost");
        REQUIRE(helix::extract_host_from_websocket_url("wss://127.0.0.1:7125/websocket") ==
                "127.0.0.1");
        REQUIRE(helix::extract_host_from_websocket_url("wss://192.168.1.100:7125/websocket") ==
                "192.168.1.100");
        REQUIRE(helix::extract_host_from_websocket_url("wss://printer.local:443/websocket") ==
                "printer.local");

        // With different ports
        REQUIRE(helix::extract_host_from_websocket_url("ws://localhost:80/websocket") ==
                "localhost");
        REQUIRE(helix::extract_host_from_websocket_url("ws://192.168.1.100:8080/websocket") ==
                "192.168.1.100");

        // IPv6 URLs (bracketed format)
        REQUIRE(helix::extract_host_from_websocket_url("ws://[::1]:7125/websocket") == "::1");
        REQUIRE(helix::extract_host_from_websocket_url("wss://[::1]:7125/websocket") == "::1");

        // Edge cases
        REQUIRE(helix::extract_host_from_websocket_url("") == "");
        REQUIRE(helix::extract_host_from_websocket_url("invalid") == "");
        REQUIRE(helix::extract_host_from_websocket_url("http://not-websocket:7125") == "");
    }

    SECTION("is_local_moonraker works with WSS URLs") {
        helix::HelixPluginInstaller installer;

        // WSS localhost should be detected as local
        installer.set_websocket_url("wss://localhost:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        installer.set_websocket_url("wss://127.0.0.1:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        // WSS remote should not be detected as local
        installer.set_websocket_url("wss://192.168.1.100:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == false);
    }
}

// ============================================================================
// Test: HelixPluginInstaller Class
// ============================================================================

TEST_CASE("HelixPluginInstaller", "[plugin_installer]") {
    SECTION("is_local_moonraker detects localhost connections") {
        helix::HelixPluginInstaller installer;

        // Test with various URL patterns
        // Note: is_local_moonraker() uses the API's connected URL

        // When not connected, should return false
        REQUIRE(installer.is_local_moonraker() == false);

        // When set to localhost, should return true
        installer.set_websocket_url("ws://localhost:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        installer.set_websocket_url("ws://127.0.0.1:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        installer.set_websocket_url("ws://[::1]:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == true);

        // When set to remote, should return false
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == false);

        installer.set_websocket_url("ws://printer.local:7125/websocket");
        REQUIRE(installer.is_local_moonraker() == false);
    }

    SECTION("get_remote_install_command returns valid curl command") {
        helix::HelixPluginInstaller installer;

        std::string cmd = installer.get_remote_install_command();

        // Should start with curl
        REQUIRE(cmd.find("curl") == 0);

        // Should use -sSL flags (silent, show errors, follow redirects)
        REQUIRE(cmd.find("-sSL") != std::string::npos);

        // Should pipe to bash
        REQUIRE(cmd.find("| bash") != std::string::npos);

        // Should reference the remote-install.sh script
        REQUIRE(cmd.find("remote-install.sh") != std::string::npos);

        // Should be from GitHub raw
        REQUIRE(cmd.find("raw.githubusercontent.com") != std::string::npos);
    }
}

// ============================================================================
// Test: Preference Management
// ============================================================================

// Note: Preference tests are integration tests that require a Config singleton.
// Since Config is typically initialized once at startup, these tests verify
// the logic but may need to be run in isolation or with a test config setup.

TEST_CASE("HelixPluginInstaller preferences logic", "[plugin_installer][preferences]") {
    SECTION("should_prompt_install is suppressed when config is unavailable") {
        helix::HelixPluginInstaller installer;

        // The implementation returns FALSE when Config::get_instance() is null
        // (the old comment here claimed the opposite). Suppressing is the safe
        // side: with no config we cannot know whether the user already declined,
        // so we must not nag.
        helix::Config* config = helix::Config::get_instance();
        const bool result = installer.should_prompt_install();

        if (!config) {
            REQUIRE_FALSE(result);
        } else {
            // Config present: prompting is gated behind beta features AND a
            // completed wizard. Whatever this environment's config happens to
            // say, a true answer implies both gates were open.
            if (result) {
                CHECK(config->is_beta_features_enabled());
                CHECK_FALSE(config->is_wizard_required());
            }
            // Contrapositive, so the false branch is not a free pass either.
            if (!config->is_beta_features_enabled() || config->is_wizard_required()) {
                CHECK_FALSE(result);
            }
        }
    }
}

// ============================================================================
// Test: Install Script Path Resolution
// ============================================================================

TEST_CASE("HelixPluginInstaller script path", "[plugin_installer]") {
    SECTION("get_install_script_path returns valid path when script exists") {
        helix::HelixPluginInstaller installer;

        std::string path = installer.get_install_script_path();

        // In development environment, should find the script
        if (!path.empty()) {
            REQUIRE(std::filesystem::exists(path));
            REQUIRE(path.find("install.sh") != std::string::npos);
        }
        // In test environment without bundled script, may return empty
        // This is acceptable - we fall back to remote instructions
    }

    SECTION("get_install_script_path never returns a path that isn't there") {
        helix::HelixPluginInstaller installer;

        // The resolver walks a candidate list and returns the first entry that
        // exists, canonicalizes to a file still named install.sh, and is
        // owner-executable. Every other outcome is "". So the invariant that
        // holds regardless of whether this checkout has the script bundled is:
        // empty, or a real executable file called install.sh.
        const std::string path = installer.get_install_script_path();

        CHECK((path.empty() || std::filesystem::exists(path)));
        if (!path.empty()) {
            CHECK(std::filesystem::path(path).filename() == "install.sh");
            const auto perms = std::filesystem::status(path).permissions();
            CHECK((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none);
        }

        // Path resolution is a pure query - it must not move the state machine.
        CHECK(installer.get_state() == helix::PluginInstallState::IDLE);
    }
}

// ============================================================================
// Test: Install State Machine
// ============================================================================

TEST_CASE("HelixPluginInstaller state", "[plugin_installer]") {
    SECTION("initial state is IDLE") {
        helix::HelixPluginInstaller installer;
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("is_installing returns false when IDLE") {
        helix::HelixPluginInstaller installer;
        REQUIRE(installer.is_installing() == false);
    }

    SECTION("state accessors are consistent") {
        helix::HelixPluginInstaller installer;

        // IDLE state
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
        REQUIRE(installer.is_installing() == false);
    }
}

// ============================================================================
// Test: install_local() Error Paths
// ============================================================================

TEST_CASE("HelixPluginInstaller install_local error handling", "[plugin_installer]") {
    SECTION("install_local fails when not connected to local Moonraker") {
        helix::HelixPluginInstaller installer;

        // Set to remote URL
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");

        bool callback_called = false;
        bool callback_success = true;
        std::string callback_message;

        installer.install_local([&](bool success, const std::string& msg) {
            callback_called = true;
            callback_success = success;
            callback_message = msg;
        });

        REQUIRE(callback_called);
        REQUIRE(callback_success == false);
        REQUIRE(callback_message.find("local Moonraker") != std::string::npos);
        // State should remain IDLE since install never started
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("install_local fails when no URL is set") {
        helix::HelixPluginInstaller installer;
        // No URL set - should fail with "local Moonraker" message

        bool callback_called = false;
        bool callback_success = true;

        installer.install_local([&](bool success, const std::string&) {
            callback_called = true;
            callback_success = success;
        });

        REQUIRE(callback_called);
        REQUIRE(callback_success == false);
    }

    SECTION("install_local fails gracefully when script not found (localhost)") {
        helix::HelixPluginInstaller installer;

        // Set localhost URL so is_local_moonraker() returns true
        installer.set_websocket_url("ws://localhost:7125/websocket");

        bool callback_called = false;
        bool callback_success = true;
        std::string callback_message;

        installer.install_local([&](bool success, const std::string& msg) {
            callback_called = true;
            callback_success = success;
            callback_message = msg;
        });

        // In test environment, install.sh likely won't be found
        // Either way, callback should be called
        REQUIRE(callback_called);

        // If script not found, should fail with appropriate message
        if (!callback_success) {
            REQUIRE((callback_message.find("not found") != std::string::npos ||
                     callback_message.find("failed") != std::string::npos ||
                     callback_message.find("Failed") != std::string::npos));
        }
    }

    SECTION("install_local handles nullptr callback safely") {
        helix::HelixPluginInstaller installer;
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");

        // Should not crash with nullptr callback
        installer.install_local(nullptr);

        // State should remain IDLE (install didn't start due to remote URL)
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }
}

// ============================================================================
// Test: Edge Cases in URL Parsing
// ============================================================================

TEST_CASE("HelixPluginInstaller URL edge cases", "[plugin_installer]") {
    SECTION("extract_host handles malformed IPv6 brackets") {
        // Missing closing bracket
        REQUIRE(helix::extract_host_from_websocket_url("ws://[::1:7125/websocket") == "");

        // Empty brackets
        REQUIRE(helix::extract_host_from_websocket_url("ws://[]:7125/websocket") == "");
    }

    SECTION("extract_host handles URLs without port") {
        REQUIRE(helix::extract_host_from_websocket_url("ws://localhost/websocket") == "localhost");
        REQUIRE(helix::extract_host_from_websocket_url("ws://192.168.1.100/path") ==
                "192.168.1.100");
    }

    SECTION("extract_host handles URLs with just hostname") {
        REQUIRE(helix::extract_host_from_websocket_url("ws://localhost") == "localhost");
    }
}

// ============================================================================
// Test: uninstall_local() Error Paths
// ============================================================================

TEST_CASE("HelixPluginInstaller uninstall_local error handling", "[plugin_installer]") {
    SECTION("uninstall_local fails when not connected to local Moonraker") {
        helix::HelixPluginInstaller installer;

        // Set to remote URL
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");

        bool callback_called = false;
        bool callback_success = true;
        std::string callback_message;

        installer.uninstall_local([&](bool success, const std::string& msg) {
            callback_called = true;
            callback_success = success;
            callback_message = msg;
        });

        REQUIRE(callback_called);
        REQUIRE(callback_success == false);
        REQUIRE(callback_message.find("local Moonraker") != std::string::npos);
        // State should remain IDLE since uninstall never started
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("uninstall_local fails when no URL is set") {
        helix::HelixPluginInstaller installer;
        // No URL set - should fail with "local Moonraker" message

        bool callback_called = false;
        bool callback_success = true;

        installer.uninstall_local([&](bool success, const std::string&) {
            callback_called = true;
            callback_success = success;
        });

        REQUIRE(callback_called);
        REQUIRE(callback_success == false);
    }

    SECTION("uninstall_local handles nullptr callback safely") {
        helix::HelixPluginInstaller installer;
        installer.set_websocket_url("ws://192.168.1.100:7125/websocket");

        // Should not crash with nullptr callback
        installer.uninstall_local(nullptr);

        // State should remain IDLE (uninstall didn't start due to remote URL)
        REQUIRE(installer.get_state() == helix::PluginInstallState::IDLE);
    }

    SECTION("uninstall_local fails gracefully when script not found (localhost)") {
        helix::HelixPluginInstaller installer;

        // Set localhost URL so is_local_moonraker() returns true
        installer.set_websocket_url("ws://localhost:7125/websocket");

        bool callback_called = false;
        bool callback_success = true;
        std::string callback_message;

        installer.uninstall_local([&](bool success, const std::string& msg) {
            callback_called = true;
            callback_success = success;
            callback_message = msg;
        });

        // In test environment, install.sh likely won't be found
        REQUIRE(callback_called);

        // If script not found, should fail with appropriate message
        if (!callback_success) {
            REQUIRE((callback_message.find("not found") != std::string::npos ||
                     callback_message.find("failed") != std::string::npos ||
                     callback_message.find("Failed") != std::string::npos));
        }
    }
}
