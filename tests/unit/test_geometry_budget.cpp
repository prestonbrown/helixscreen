// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

#include "geometry_budget_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// Memory parsing tests
TEST_CASE("Budget: parse MemAvailable from /proc/meminfo", "[gcode][budget]") {
    const std::string meminfo = R"(MemTotal:        3884136 kB
MemFree:         1363424 kB
MemAvailable:    3768880 kB
Buffers:          104872 kB
Cached:          2091048 kB)";
    REQUIRE(GeometryBudgetManager::parse_meminfo_available_kb(meminfo) == 3768880);
}

TEST_CASE("Budget: parse MemAvailable from 1GB system", "[gcode][budget]") {
    const std::string meminfo = R"(MemTotal:         999936 kB
MemFree:          102400 kB
MemAvailable:     307200 kB)";
    REQUIRE(GeometryBudgetManager::parse_meminfo_available_kb(meminfo) == 307200);
}

TEST_CASE("Budget: parse MemAvailable returns 0 on missing field", "[gcode][budget]") {
    const std::string meminfo = R"(MemTotal:        3884136 kB
MemFree:         1363424 kB)";
    REQUIRE(GeometryBudgetManager::parse_meminfo_available_kb(meminfo) == 0);
}

TEST_CASE("Budget: parse MemAvailable from AD5M (256MB)", "[gcode][budget]") {
    const std::string meminfo = R"(MemTotal:         253440 kB
MemFree:           12288 kB
MemAvailable:      38912 kB)";
    REQUIRE(GeometryBudgetManager::parse_meminfo_available_kb(meminfo) == 38912);
}

// Budget calculation tests
TEST_CASE("Budget: 25% of available memory", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    size_t budget = mgr.calculate_budget(3768880);
    REQUIRE(budget == 256 * 1024 * 1024); // Capped at 256MB
}

TEST_CASE("Budget: 1GB Pi with 300MB free", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    size_t budget = mgr.calculate_budget(307200);
    REQUIRE(budget == 307200 * 1024 / 4);
}

TEST_CASE("Budget: AD5M with 38MB available", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    size_t budget = mgr.calculate_budget(38912);
    REQUIRE(budget == 38912 * 1024 / 4);
}

TEST_CASE("Budget: hard cap at 256MB even with 8GB free", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    size_t budget = mgr.calculate_budget(6144000);
    REQUIRE(budget == 256 * 1024 * 1024);
}

TEST_CASE("Budget: 0 available memory returns 0", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    size_t budget = mgr.calculate_budget(0);
    REQUIRE(budget == 0);
}

// Tier selection tests
TEST_CASE("Budget: tier selection - small file gets Tier 1", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto config = mgr.select_tier(50000, 256 * 1024 * 1024);
    REQUIRE(config.tier == 1);
    REQUIRE(config.tube_sides == 16);
    REQUIRE(config.include_travels == true);
}

TEST_CASE("Budget: tier selection - medium file gets Tier 2", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    // 150K segs × 1300 = 195MB > 150MB, × 600 = 90MB < 150MB → Tier 2
    auto config = mgr.select_tier(150000, 150 * 1024 * 1024);
    REQUIRE(config.tier == 2);
    REQUIRE(config.tube_sides == 8);
}

TEST_CASE("Budget: dropped auxiliary mass must not cost a tier", "[gcode][budget]") {
    // #1425: the builder drops prime-tower/purge segments, but the tier gate was
    // estimating from total_segments, which still counted them. A 4-color file
    // that is half tower was charged for the half that never becomes geometry.
    GeometryBudgetManager mgr;
    const size_t budget = 100 * 1024 * 1024;

    // 300K total segments, half of them prime tower.
    // Charged in full:   300K x 600 = 171.7MB > 100MB, x 300 = 85.8MB < 100MB -> tier 3
    // Charged drawable:  150K x 600 =  85.8MB < 100MB                         -> tier 2
    auto inflated = mgr.select_tier(300000, budget);
    auto drawable = mgr.select_tier(150000, budget);

    REQUIRE(inflated.tier == 3);
    REQUIRE(drawable.tier == 2);
    REQUIRE(drawable.tube_sides > inflated.tube_sides);
    REQUIRE(drawable.include_travels);
    REQUIRE_FALSE(inflated.include_travels);
}

TEST_CASE("Budget: dropped auxiliary mass must not cost 3D entirely", "[gcode][budget]") {
    // The worst case the issue names: the inflated count crosses out of tier 3
    // into tier 4, where ui_gcode_viewer bails (`if (tier > 3) return nullptr`)
    // and the file gets no 3D preview at all.
    GeometryBudgetManager mgr;
    const size_t budget = 100 * 1024 * 1024;

    // Charged in full:  800K x 300 = 240MB, above 2x budget -> tier 4, no 3D
    // Charged drawable: 300K x 300 =  90MB < 100MB          -> tier 3, still 3D
    REQUIRE(mgr.select_tier(800000, budget).tier == 4);
    REQUIRE(mgr.select_tier(300000, budget).tier == 3);
}

TEST_CASE("Budget: tier selection - large file gets Tier 3", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    // 300K segs × 600 = 180MB > 100MB, × 300 = 90MB < 100MB → Tier 3
    auto config = mgr.select_tier(300000, 100 * 1024 * 1024);
    REQUIRE(config.tier == 3);
    REQUIRE(config.tube_sides == 4);
    REQUIRE(config.include_travels == false);
    REQUIRE(config.simplification_tolerance > 0.1f);
}

TEST_CASE("Budget: tier selection - massive file gets Tier 4", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    // 2M segs × 300 = 600MB >> 75MB even at N=4 → Tier 4
    auto config = mgr.select_tier(2000000, 75 * 1024 * 1024);
    REQUIRE(config.tier == 4);
    REQUIRE(config.tube_sides == 0);
}

TEST_CASE("Budget: tier selection - tiny budget forces high tier", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    // 50K segs × 300 = 15MB > 10MB → aggressive Tier 3 or Tier 4
    auto config = mgr.select_tier(50000, 10 * 1024 * 1024);
    REQUIRE(config.tier >= 3);
}

TEST_CASE("Budget: tier selection - 0 segments gets Tier 1", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto config = mgr.select_tier(0, 256 * 1024 * 1024);
    REQUIRE(config.tier == 1);
}

TEST_CASE("Budget: tier 5 for zero budget", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto config = mgr.select_tier(100000, 0);
    REQUIRE(config.tier == 5);
}

// Progressive budget checking tests
TEST_CASE("Budget: check_budget returns CONTINUE when under budget", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto action = mgr.check_budget(50 * 1024 * 1024, 256 * 1024 * 1024, 1);
    REQUIRE(action == GeometryBudgetManager::BudgetAction::CONTINUE);
}

TEST_CASE("Budget: check_budget returns DEGRADE at 90% for tier 1", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto action = mgr.check_budget(91 * 1024 * 1024, 100 * 1024 * 1024, 1);
    REQUIRE(action == GeometryBudgetManager::BudgetAction::DEGRADE);
}

TEST_CASE("Budget: check_budget returns DEGRADE at 90% for tier 2", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto action = mgr.check_budget(91 * 1024 * 1024, 100 * 1024 * 1024, 2);
    REQUIRE(action == GeometryBudgetManager::BudgetAction::DEGRADE);
}

TEST_CASE("Budget: check_budget returns ABORT at 90% for tier 3", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto action = mgr.check_budget(91 * 1024 * 1024, 100 * 1024 * 1024, 3);
    REQUIRE(action == GeometryBudgetManager::BudgetAction::ABORT);
}

TEST_CASE("Budget: check_budget CONTINUE at 89%", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto action = mgr.check_budget(89 * 1024 * 1024, 100 * 1024 * 1024, 1);
    REQUIRE(action == GeometryBudgetManager::BudgetAction::CONTINUE);
}

TEST_CASE("Budget: check_budget handles 0 budget", "[gcode][budget]") {
    GeometryBudgetManager mgr;
    auto action = mgr.check_budget(1024, 0, 1);
    REQUIRE(action == GeometryBudgetManager::BudgetAction::ABORT);
}
