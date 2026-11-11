// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Real-machine captures in tests/fixtures/printers/<slug>.json, loaded as
// nlohmann::json or as the PrinterHardwareData detection sees.

#include "printer_detector.h"
#include "printer_discovery.h"

#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix::test {

inline std::string printers_fixture_path(const std::string& slug) {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/test_helpers/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/printers/" + slug + ".json";
    }
    return "tests/fixtures/printers/" + slug + ".json";
}

inline nlohmann::json load_printer_capture(const std::string& slug) {
    const std::string path = printers_fixture_path(slug);
    std::ifstream f(path);
    INFO("capture fixture missing or unreadable: " << path);
    REQUIRE(f.is_open());
    nlohmann::json j;
    f >> j;
    return j;
}

/// PrinterHardwareData from one hardware JSON object: the shape of a machine
/// capture and of a corpus row's "hardware". Fields it does not carry keep
/// their defaults, exactly as a discovery that never fetched them would leave
/// them.
inline PrinterHardwareData hardware_from_json(const nlohmann::json& j, const std::string& label) {
    // Every key must be one this loader reads, so a misspelled or unread key
    // fails here instead of leaving the capture silently weaker.
    static const std::set<std::string> kKnownKeys = {"provenance",
                                                     "notes",
                                                     "heaters",
                                                     "sensors",
                                                     "fans",
                                                     "leds",
                                                     "hostname",
                                                     "printer_objects",
                                                     "steppers",
                                                     "kinematics",
                                                     "mcu",
                                                     "mcu_list",
                                                     "cpu_arch",
                                                     "build_volume",
                                                     "configfile_settings",
                                                     "objects_reported"};
    for (const auto& item : j.items()) {
        INFO("'" << label << "' has a key the loader does not read: " << item.key());
        CHECK(kKnownKeys.count(item.key()) == 1);
    }

    auto strings = [&j](const char* key) {
        std::vector<std::string> out;
        if (j.contains(key)) {
            for (const auto& item : j.at(key)) {
                out.push_back(item.get<std::string>());
            }
        }
        return out;
    };

    PrinterHardwareData hardware;
    hardware.heaters = strings("heaters");
    hardware.sensors = strings("sensors");
    hardware.fans = strings("fans");
    hardware.leds = strings("leds");
    hardware.hostname = j.value("hostname", std::string{});
    hardware.printer_objects = strings("printer_objects");
    hardware.steppers = strings("steppers");
    hardware.kinematics = j.value("kinematics", std::string{});
    hardware.mcu = j.value("mcu", std::string{});
    hardware.mcu_list = strings("mcu_list");
    hardware.cpu_arch = j.value("cpu_arch", std::string{});
    hardware.objects_reported = j.value("objects_reported", false);
    if (j.contains("build_volume")) {
        const auto& v = j.at("build_volume");
        hardware.build_volume =
            BuildVolume{v.value("x_min", 0.0f), v.value("x_max", 0.0f), v.value("y_min", 0.0f),
                        v.value("y_max", 0.0f), v.value("z_max", 0.0f)};
    }
    // A configfile.settings subset goes through discovery's own parse, so the
    // capture takes the same reading a live connection does.
    if (j.contains("configfile_settings")) {
        helix::PrinterDiscovery discovery;
        INFO("'" << label << "' configfile_settings carries no stepper extent");
        REQUIRE(discovery.parse_build_volume(j.at("configfile_settings")));
        hardware.build_volume = discovery.build_volume();
    }
    return hardware;
}

/// PrinterHardwareData aggregated from a real machine's captures: the object
/// list, /printer/info hostname, and whatever other endpoints the snapshot
/// recorded.
inline PrinterHardwareData printer_capture(const std::string& slug) {
    return hardware_from_json(load_printer_capture(slug), "capture " + slug);
}

} // namespace helix::test
