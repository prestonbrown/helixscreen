// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_hardware_health_char.cpp
 * @brief HardwareValidationResult aggregation over changed_from_last_session
 *
 * test_hardware_validator.cpp covers has_issues() / has_critical() /
 * total_issue_count() / max_severity() over critical_missing,
 * expected_missing and newly_discovered. It never populates the fourth
 * list, so the branches that read changed_from_last_session are exercised
 * only here.
 *
 * @see include/hardware_validator.h - HardwareIssue, HardwareValidationResult
 */

#include "hardware_validator.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("HardwareValidationResult - changed_from_last_session aggregation",
          "[hardware][validator]") {
    SECTION("has_issues is driven by changed_from_last_session alone") {
        HardwareValidationResult result;
        REQUIRE_FALSE(result.has_issues());

        result.changed_from_last_session.push_back(
            HardwareIssue::warning("fan part", HardwareType::FAN, "Was present last session"));

        REQUIRE(result.has_issues());
    }

    SECTION("changed_from_last_session does not make the result critical") {
        HardwareValidationResult result;
        result.changed_from_last_session.push_back(
            HardwareIssue::warning("fan part", HardwareType::FAN, "Was present last session"));

        REQUIRE_FALSE(result.has_critical());
    }

    SECTION("max_severity is WARNING for changed_from_last_session alone") {
        HardwareValidationResult result;
        result.changed_from_last_session.push_back(
            HardwareIssue::warning("fan part", HardwareType::FAN, "Was present last session"));

        REQUIRE(result.max_severity() == HardwareIssueSeverity::WARNING);
    }

    SECTION("critical still outranks a session change") {
        HardwareValidationResult result;
        result.changed_from_last_session.push_back(
            HardwareIssue::warning("fan part", HardwareType::FAN, "Was present last session"));
        result.critical_missing.push_back(
            HardwareIssue::critical("extruder", HardwareType::HEATER, "Missing"));

        REQUIRE(result.max_severity() == HardwareIssueSeverity::CRITICAL);
    }

    SECTION("total_issue_count sums all four lists") {
        HardwareValidationResult result;
        result.critical_missing.push_back(
            HardwareIssue::critical("extruder", HardwareType::HEATER, "Missing"));
        result.expected_missing.push_back(
            HardwareIssue::warning("fan", HardwareType::FAN, "Missing"));
        result.newly_discovered.push_back(
            HardwareIssue::info("temperature_sensor chamber", HardwareType::SENSOR, "New"));
        result.changed_from_last_session.push_back(
            HardwareIssue::warning("neopixel bar", HardwareType::LED, "Was present last session"));

        REQUIRE(result.total_issue_count() == 4);
    }
}
