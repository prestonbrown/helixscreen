// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/cfs_box_fixtures.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "hv/json.hpp"

namespace helix::test {

inline nlohmann::json make_cfs_status_json() {
    return nlohmann::json::parse(R"({
        "box": {
            "state": "connect",
            "filament": 1,
            "auto_refill": 1,
            "enable": 1,
            "filament_useup": 1,
            "same_material": [
                ["101001", "0000000", ["T1A"], "PLA"],
                ["101001", "0FFFFFF", ["T1B"], "PLA"]
            ],
            "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
            "T1": {
                "state": "connect",
                "filament": "None",
                "temperature": "27",
                "dry_and_humidity": "48",
                "filament_detected": "None",
                "measuring_wheel": "None",
                "version": "1.1.3",
                "sn": "10000882925L125DBZC",
                "mode": "0",
                "vender": ["unknown", "unknown", "unknown", "unknown"],
                "remain_len": ["35", "57", "52", "52"],
                "color_value": ["0000000", "0FFFFFF", "00A2989", "0C12E1F"],
                "material_type": ["101001", "101001", "101001", "101001"],
                "uuid": [19, 103],
                "change_color_num": ["-1", "-1", "-1", "-1"]
            },
            "T2": {
                "state": "None",
                "filament": "None",
                "temperature": "None",
                "dry_and_humidity": "None",
                "filament_detected": "None",
                "measuring_wheel": "None",
                "version": "-1",
                "sn": "-1",
                "mode": "-1",
                "vender": ["-1", "-1", "-1", "-1"],
                "remain_len": ["-1", "-1", "-1", "-1"],
                "color_value": ["-1", "-1", "-1", "-1"],
                "material_type": ["-1", "-1", "-1", "-1"],
                "uuid": "None",
                "change_color_num": ["-1", "-1", "-1", "-1"]
            }
        }
    })");
}

// Box builder with explicit control over `vender` and `remain_len`.
// test_ams_backend_cfs.cpp's make_single_unit_box() synthesizes those from
// material/color presence, which
// is exactly the coupling these tests need to break: a REMOVED tagged spool has
// sentinel vender alongside latched material/color/remain_len.
//
// filament_useup_value / active_filament drive the runout-episode inputs:
// the latch (top-level box field) and the feeding lane (T1.filament letter).
// filament_useup_value defaults to 0, an idle box.
inline nlohmann::json make_unit_box_explicit(const std::vector<std::string>& material_types,
                                             const std::vector<std::string>& color_values,
                                             const std::vector<std::string>& venders,
                                             const std::vector<std::string>& remain_lens,
                                             int filament_useup_value = 0,
                                             const std::string& active_filament = "None") {
    nlohmann::json box = nlohmann::json::parse(R"({
        "state": "connect",
        "filament": 0,
        "auto_refill": 1,
        "enable": 1,
        "filament_useup": 0,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {
            "state": "connect",
            "filament": "None",
            "temperature": "27",
            "dry_and_humidity": "48",
            "version": "1.1.3",
            "sn": "SERIAL",
            "change_color_num": ["-1", "-1", "-1", "-1"]
        }
    })");
    box["T1"]["material_type"] = material_types;
    box["T1"]["color_value"] = color_values;
    box["T1"]["vender"] = venders;
    box["T1"]["remain_len"] = remain_lens;
    box["filament_useup"] = filament_useup_value;
    box["T1"]["filament"] = active_filament;
    return box;
}

} // namespace helix::test
