// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/happy_hare_fixture.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace helix::test {

/// A Happy Hare golden payload from tests/fixtures/, resolved from __FILE__ so
/// the run does not depend on cwd. Each one carries `mmu_machine`,
/// `configfile_settings`, `printer_objects`, `mmu_status` and `entry_sensors`.
inline nlohmann::json load_happy_hare_fixture(const std::string& name) {
    std::string path = __FILE__;
    const auto pos = path.rfind("/tests/test_helpers/");
    path = (pos == std::string::npos ? std::string("tests") : path.substr(0, pos) + "/tests") +
           "/fixtures/" + name;
    std::ifstream f(path);
    INFO("fixture missing or unreadable: " << path);
    REQUIRE(f.is_open());
    return nlohmann::json::parse(f);
}

} // namespace helix::test
