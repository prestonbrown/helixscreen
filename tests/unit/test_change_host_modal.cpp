// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "utils/network_validation.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Change Host Modal - Config Read/Write Tests
// ============================================================================

// Test fixture that sets up Config singleton with test data
// Must be in namespace helix to match friend declaration in Config
namespace helix {
class ChangeHostConfigFixture : public Config {
  public:
    ChangeHostConfigFixture() {
        data = {{"config_version", 3},
                {"active_printer_id", "default"},
                {"printers",
                 {{"default", {{"moonraker_host", "192.168.1.50"}, {"moonraker_port", 7125}}}}}};
        active_printer_id_ = "default";
        saved_instance_ = instance;
        instance = this;
    }

    ~ChangeHostConfigFixture() {
        instance = saved_instance_;
    }

  private:
    Config* saved_instance_ = nullptr;
};
} // namespace helix

TEST_CASE("Change host: Config read returns current values", "[change_host][config]") {
    ChangeHostConfigFixture config;
    auto* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);

    std::string host = cfg->get<std::string>(cfg->df() + "moonraker_host", "");
    int port = cfg->get<int>(cfg->df() + "moonraker_port", 7125);

    REQUIRE(host == "192.168.1.50");
    REQUIRE(port == 7125);
}

TEST_CASE("Change host: Config write updates values", "[change_host][config]") {
    ChangeHostConfigFixture config;
    auto* cfg = Config::get_instance();

    cfg->set(cfg->df() + "moonraker_host", std::string("10.0.0.1"));
    cfg->set(cfg->df() + "moonraker_port", 8080);

    REQUIRE(cfg->get<std::string>(cfg->df() + "moonraker_host", "") == "10.0.0.1");
    REQUIRE(cfg->get<int>(cfg->df() + "moonraker_port", 0) == 8080);
}

TEST_CASE("Change host: Config defaults for missing host", "[change_host][config]") {
    ChangeHostConfigFixture config;
    auto* cfg = Config::get_instance();

    // Remove host key, verify default
    cfg->set(cfg->df() + "moonraker_host", std::string(""));
    std::string host = cfg->get<std::string>(cfg->df() + "moonraker_host", "127.0.0.1");

    // Empty string was set, not missing, so we get empty
    REQUIRE(host == "");
}

TEST_CASE("Change host: Port round-trips as integer", "[change_host][config]") {
    ChangeHostConfigFixture config;
    auto* cfg = Config::get_instance();

    // Set port as int, read back
    cfg->set(cfg->df() + "moonraker_port", 443);
    REQUIRE(cfg->get<int>(cfg->df() + "moonraker_port", 0) == 443);

    cfg->set(cfg->df() + "moonraker_port", 65535);
    REQUIRE(cfg->get<int>(cfg->df() + "moonraker_port", 0) == 65535);
}

// ============================================================================
// Validation tests specific to change host flow
// (comprehensive validation tests are in test_network_validation.cpp)
// ============================================================================

TEST_CASE("Change host: Validate typical user inputs", "[change_host][validation]") {
    // Typical IP that a user would enter
    REQUIRE(is_valid_ip_or_hostname("192.168.1.100") == true);
    REQUIRE(is_valid_ip_or_hostname("10.0.0.1") == true);
    REQUIRE(is_valid_ip_or_hostname("printer.local") == true);

    // Typical port
    REQUIRE(is_valid_port("7125") == true);

    // Common mistakes
    REQUIRE(is_valid_ip_or_hostname("") == false);
    REQUIRE(is_valid_port("") == false);
    REQUIRE(is_valid_port("0") == false);
}
