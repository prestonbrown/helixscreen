// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/scoped_test_mode.h"
#include "runtime_config.h"

#include <cstring>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

// Helper to reset global config between tests
static void reset_runtime_config() {
    *get_runtime_config() = RuntimeConfig{};
}

// Helper function to simulate command-line parsing
bool parse_test_args(const std::vector<std::string>& args) {
    // Reset config before each test
    reset_runtime_config();
    RuntimeConfig* cfg = get_runtime_config();

    // Parse arguments
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--test") {
            cfg->test_mode = true;
        } else if (args[i] == "--real-wifi") {
            cfg->use_real_wifi = true;
        } else if (args[i] == "--real-ethernet") {
            cfg->use_real_ethernet = true;
        } else if (args[i] == "--real-moonraker") {
            cfg->use_real_moonraker = true;
        } else if (args[i] == "--real-files") {
            cfg->use_real_files = true;
        } else if (args[i] == "--real-ams") {
            cfg->use_real_ams = true;
        } else if (args[i] == "--real-sensors") {
            cfg->use_real_sensors = true;
        } else {
            return false; // Unknown argument
        }
    }

    // Validate: --real-* flags require --test mode
    if ((cfg->use_real_wifi || cfg->use_real_ethernet || cfg->use_real_moonraker ||
         cfg->use_real_files || cfg->use_real_ams || cfg->use_real_sensors) &&
        !cfg->test_mode) {
        return false; // Invalid configuration
    }

    return true;
}

TEST_CASE("TestConfig default initialization", "[test_config]") {
    RuntimeConfig config;

    SECTION("All flags are false by default") {
        REQUIRE(config.test_mode == false);
        REQUIRE(config.use_real_wifi == false);
        REQUIRE(config.use_real_ethernet == false);
        REQUIRE(config.use_real_moonraker == false);
        REQUIRE(config.use_real_files == false);
    }

    SECTION("Helper methods return false in production mode") {
        REQUIRE(config.should_mock_wifi() == false);
        REQUIRE(config.should_mock_ethernet() == false);
        REQUIRE(config.should_mock_moonraker() == false);
        REQUIRE(config.should_use_test_files() == false);
        REQUIRE(config.is_test_mode() == false);
    }
}

TEST_CASE("TestConfig test mode without real components", "[test_config]") {
    RuntimeConfig config;
    config.test_mode = true;

    SECTION("All components use mocks by default in test mode") {
        REQUIRE(config.should_mock_wifi() == true);
        REQUIRE(config.should_mock_ethernet() == true);
        REQUIRE(config.should_mock_moonraker() == true);
        REQUIRE(config.should_use_test_files() == true);
        REQUIRE(config.is_test_mode() == true);
    }
}

TEST_CASE("TestConfig test mode with selective real components", "[test_config]") {
    RuntimeConfig config;
    config.test_mode = true;

    SECTION("Real WiFi overrides mock") {
        config.use_real_wifi = true;
        REQUIRE(config.should_mock_wifi() == false);
        REQUIRE(config.should_mock_ethernet() == true);
        REQUIRE(config.should_mock_moonraker() == true);
        REQUIRE(config.should_use_test_files() == true);
    }

    SECTION("Real Ethernet overrides mock") {
        config.use_real_ethernet = true;
        REQUIRE(config.should_mock_wifi() == true);
        REQUIRE(config.should_mock_ethernet() == false);
        REQUIRE(config.should_mock_moonraker() == true);
        REQUIRE(config.should_use_test_files() == true);
    }

    SECTION("Real Moonraker overrides mock") {
        config.use_real_moonraker = true;
        REQUIRE(config.should_mock_wifi() == true);
        REQUIRE(config.should_mock_ethernet() == true);
        REQUIRE(config.should_mock_moonraker() == false);
        REQUIRE(config.should_use_test_files() == true);
    }

    SECTION("Real files override test data") {
        config.use_real_files = true;
        REQUIRE(config.should_mock_wifi() == true);
        REQUIRE(config.should_mock_ethernet() == true);
        REQUIRE(config.should_mock_moonraker() == true);
        REQUIRE(config.should_use_test_files() == false);
    }

    SECTION("Multiple real components") {
        config.use_real_wifi = true;
        config.use_real_moonraker = true;
        REQUIRE(config.should_mock_wifi() == false);
        REQUIRE(config.should_mock_ethernet() == true);
        REQUIRE(config.should_mock_moonraker() == false);
        REQUIRE(config.should_use_test_files() == true);
    }

    SECTION("All real components in test mode") {
        config.use_real_wifi = true;
        config.use_real_ethernet = true;
        config.use_real_moonraker = true;
        config.use_real_files = true;
        REQUIRE(config.should_mock_wifi() == false);
        REQUIRE(config.should_mock_ethernet() == false);
        REQUIRE(config.should_mock_moonraker() == false);
        REQUIRE(config.should_use_test_files() == false);
    }
}

TEST_CASE("TestConfig production mode ignores real flags", "[test_config]") {
    RuntimeConfig config;
    config.test_mode = false; // Production mode

    SECTION("Real flags have no effect without test mode") {
        config.use_real_wifi = true;
        config.use_real_ethernet = true;
        config.use_real_moonraker = true;
        config.use_real_files = true;

        // In production, we never use mocks regardless of flags
        REQUIRE(config.should_mock_wifi() == false);
        REQUIRE(config.should_mock_ethernet() == false);
        REQUIRE(config.should_mock_moonraker() == false);
        REQUIRE(config.should_use_test_files() == false);
        REQUIRE(config.is_test_mode() == false);
    }
}

TEST_CASE("Command-line argument parsing", "[test_config]") {
    // These cases parse CLI flags straight onto the process-global RuntimeConfig.
    // Constructed with the current value, the guard changes nothing on entry and
    // puts the flag back on exit, including when a REQUIRE throws (#1287).
    ScopedTestMode restore_test_mode(get_runtime_config()->test_mode);

    SECTION("No arguments - production mode") {
        REQUIRE(parse_test_args({}) == true);
        RuntimeConfig* cfg = get_runtime_config();
        REQUIRE(cfg->test_mode == false);
        REQUIRE(cfg->should_mock_wifi() == false);
    }

    SECTION("Test mode only") {
        REQUIRE(parse_test_args({"--test"}) == true);
        RuntimeConfig* cfg = get_runtime_config();
        REQUIRE(cfg->test_mode == true);
        REQUIRE(cfg->should_mock_wifi() == true);
        REQUIRE(cfg->should_mock_ethernet() == true);
        REQUIRE(cfg->should_mock_moonraker() == true);
        REQUIRE(cfg->should_use_test_files() == true);
    }

    SECTION("Test mode with real WiFi") {
        REQUIRE(parse_test_args({"--test", "--real-wifi"}) == true);
        RuntimeConfig* cfg = get_runtime_config();
        REQUIRE(cfg->test_mode == true);
        REQUIRE(cfg->should_mock_wifi() == false);
        REQUIRE(cfg->should_mock_ethernet() == true);
    }

    SECTION("Test mode with multiple real components") {
        REQUIRE(parse_test_args({"--test", "--real-wifi", "--real-moonraker"}) == true);
        RuntimeConfig* cfg = get_runtime_config();
        REQUIRE(cfg->test_mode == true);
        REQUIRE(cfg->should_mock_wifi() == false);
        REQUIRE(cfg->should_mock_moonraker() == false);
        REQUIRE(cfg->should_mock_ethernet() == true);
        REQUIRE(cfg->should_use_test_files() == true);
    }

    SECTION("Test mode with real AMS") {
        REQUIRE(parse_test_args({"--test", "--real-ams"}) == true);
        RuntimeConfig* cfg = get_runtime_config();
        REQUIRE(cfg->test_mode == true);
        REQUIRE(cfg->should_mock_ams() == false);
        REQUIRE(cfg->should_mock_wifi() == true);
    }

    SECTION("Test mode with real sensors") {
        REQUIRE(parse_test_args({"--test", "--real-sensors"}) == true);
        RuntimeConfig* cfg = get_runtime_config();
        REQUIRE(cfg->test_mode == true);
        REQUIRE(cfg->should_mock_sensors() == false);
        REQUIRE(cfg->should_mock_wifi() == true);
    }

    SECTION("Real flags without test mode should fail") {
        REQUIRE(parse_test_args({"--real-wifi"}) == false);
        REQUIRE(parse_test_args({"--real-ethernet"}) == false);
        REQUIRE(parse_test_args({"--real-moonraker"}) == false);
        REQUIRE(parse_test_args({"--real-files"}) == false);
        REQUIRE(parse_test_args({"--real-ams"}) == false);
        REQUIRE(parse_test_args({"--real-sensors"}) == false);
    }

    SECTION("Unknown arguments should fail") {
        REQUIRE(parse_test_args({"--unknown"}) == false);
        REQUIRE(parse_test_args({"--test", "--unknown"}) == false);
    }

    SECTION("Order independence") {
        // --test can come after --real-* flags
        REQUIRE(parse_test_args({"--real-wifi", "--test"}) == true);
        RuntimeConfig* cfg = get_runtime_config();
        REQUIRE(cfg->test_mode == true);
        REQUIRE(cfg->should_mock_wifi() == false);
    }
}

TEST_CASE("TestConfig accessor functions", "[test_config]") {
    // These cases parse CLI flags straight onto the process-global RuntimeConfig.
    // Constructed with the current value, the guard changes nothing on entry and
    // puts the flag back on exit, including when a REQUIRE throws (#1287).
    ScopedTestMode restore_test_mode(get_runtime_config()->test_mode);

    SECTION("get_runtime_config returns pointer") {
        RuntimeConfig* cfg = get_runtime_config();
        REQUIRE(cfg != nullptr);
    }

    SECTION("Pointer can be modified") {
        RuntimeConfig* cfg = get_runtime_config();
        cfg->test_mode = true;
        REQUIRE(get_runtime_config()->test_mode == true);
        // Reset for other tests
        cfg->test_mode = false;
    }
}
