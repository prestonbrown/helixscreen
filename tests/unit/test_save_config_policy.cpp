// SPDX-License-Identifier: GPL-3.0-or-later

// The command a printer's config is saved with, and whether it restarts
// Klipper. Kalico's SAVE_CONFIG takes RESTART=0 and answers without
// restarting; mainline Klipper has no RESTART parameter and always restarts.

#include "printer_discovery.h"

#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;

TEST_CASE("save config: mainline Klipper and an undiscovered printer get a bare SAVE_CONFIG",
          "[save_config_policy]") {
    // Never send RESTART=0 to a firmware not known to take it.
    PrinterDiscovery undiscovered;
    CHECK(std::string(undiscovered.save_config_gcode()) == "SAVE_CONFIG");
    CHECK(undiscovered.save_config_restarts_klipper());

    PrinterDiscovery hw;
    hw.apply_printer_info(nlohmann::json{{"app", "Klipper"}, {"software_version", "v0.13.0"}});
    CHECK(std::string(hw.save_config_gcode()) == "SAVE_CONFIG");
    CHECK(hw.save_config_restarts_klipper());
}

TEST_CASE("save config: Kalico saves with RESTART=0 and keeps Klipper up, until cleared",
          "[save_config_policy]") {
    PrinterDiscovery hw;
    hw.apply_printer_info(nlohmann::json{{"app", "Kalico"}, {"software_version", "v2025.1"}});
    CHECK(std::string(hw.save_config_gcode()) == "SAVE_CONFIG RESTART=0");
    CHECK_FALSE(hw.save_config_restarts_klipper());

    // A reconnect to another printer starts from a clean discovery.
    hw.clear();
    CHECK(std::string(hw.save_config_gcode()) == "SAVE_CONFIG");
    CHECK(hw.save_config_restarts_klipper());
}
