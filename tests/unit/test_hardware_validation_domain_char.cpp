// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_hardware_validation_domain_char.cpp
 * @brief Characterization tests for PrinterState hardware validation domain
 *
 * These tests capture the CURRENT behavior of hardware validation subjects
 * in PrinterState before extraction to a dedicated state class.
 *
 * Hardware validation subjects (8 total):
 * - hardware_status_level_ (int) - 0=OK, 1=ATTENTION, 2=CRITICAL
 * - hardware_critical_count_ (int) - count of critical issues
 * - hardware_warning_count_ (int) - count of warning (expected_missing) issues
 * - hardware_info_count_ (int) - count of info (newly_discovered) issues
 * - hardware_session_count_ (int) - count of session change issues
 * - hardware_status_title_ (string) - "All Healthy" or "X Issues Detected"
 * - hardware_status_detail_ (string) - e.g., "1 critical, 2 missing, 1 new"
 * - hardware_issues_label_ (string) - "1 Hardware Issue" or "5 Hardware Issues"
 *
 * Update mechanism:
 * - set_hardware_validation_result(HardwareValidationResult) - synchronous
 * - remove_hardware_issue(string) - removes issue and re-applies result
 *
 * Key behaviors:
 * - All subjects initialize to 0/"" or default strings
 * - Version increments on every set_hardware_validation_result call
 * - String formatting respects pluralization
 */

#include "../test_helpers/fresh_printer_state.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "hardware_validator.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
// Helper to get subject by XML name (requires init_subjects(true))
static lv_subject_t* get_subject_by_name(const char* name) {
    return lv_xml_get_subject(NULL, name);
}

// Helper to create a critical issue
static HardwareIssue make_critical(const std::string& name, const std::string& msg = "Missing") {
    return HardwareIssue::critical(name, HardwareType::HEATER, msg);
}

// Helper to create a warning issue
static HardwareIssue make_warning(const std::string& name, const std::string& msg = "Missing") {
    return HardwareIssue::warning(name, HardwareType::SENSOR, msg);
}

// Helper to create an info issue
static HardwareIssue make_info(const std::string& name, const std::string& msg = "New") {
    return HardwareIssue::info(name, HardwareType::FAN, msg);
}

// ============================================================================
// Initial Value Tests - Document default initialization behavior
// ============================================================================

TEST_CASE("Hardware validation characterization: initial values after init",
          "[characterization][hardware-validation][init]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true); // Need XML registration to lookup by name

    SECTION("hardware_status_title initializes to 'Healthy'") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_title");
        REQUIRE(subject != nullptr);
        REQUIRE(std::string(lv_subject_get_string(subject)) == "Healthy");
    }

    SECTION("hardware_issues_label initializes to 'No Hardware Issues'") {
        lv_subject_t* subject = get_subject_by_name("hardware_issues_label");
        REQUIRE(subject != nullptr);
        REQUIRE(std::string(lv_subject_get_string(subject)) == "No Hardware Issues");
    }
}

// ============================================================================
// Empty Result Tests - Verify behavior with no issues
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: empty result (no issues)",
                 "[characterization][hardware-validation][setter]") {
    HardwareValidationResult empty_result;
    state.hardware_validation_state().set_hardware_validation_result(empty_result);

    SECTION("all category counts are 0 for empty result") {
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_critical_count")) == 0);
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_warning_count")) == 0);
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_info_count")) == 0);
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_session_count")) == 0);
    }

    SECTION("status_title is 'All Healthy' for empty result") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_title");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "All Healthy");
    }

    SECTION("status_detail is 'All configured hardware detected' for empty result") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_detail");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "All configured hardware detected");
    }

    SECTION("issues_label is 'No Hardware Issues' for empty result") {
        REQUIRE(std::string(lv_subject_get_string(
                    state.hardware_validation_state().get_hardware_issues_label_subject())) ==
                "No Hardware Issues");
    }
}

// ============================================================================
// Critical Issues Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: critical issues only",
                 "[characterization][hardware-validation][setter]") {
    HardwareValidationResult result;
    result.critical_missing.push_back(make_critical("extruder", "Extruder not responding"));
    result.critical_missing.push_back(make_critical("heater_bed", "Bed heater missing"));
    state.hardware_validation_state().set_hardware_validation_result(result);

    SECTION("critical_count matches number of critical issues") {
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_critical_count")) == 2);
    }

    SECTION("other category counts remain 0") {
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_warning_count")) == 0);
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_info_count")) == 0);
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_session_count")) == 0);
    }

    SECTION("status_title shows '2 Issues Detected'") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_title");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "2 Issues Detected");
    }

    SECTION("status_detail shows '2 critical'") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_detail");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "2 critical");
    }

    SECTION("issues_label shows '2 Hardware Issues'") {
        REQUIRE(std::string(lv_subject_get_string(
                    state.hardware_validation_state().get_hardware_issues_label_subject())) ==
                "2 Hardware Issues");
    }
}

// ============================================================================
// Warning Issues Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: warning issues only",
                 "[characterization][hardware-validation][setter]") {
    HardwareValidationResult result;
    result.expected_missing.push_back(make_warning("temperature_sensor chamber"));
    state.hardware_validation_state().set_hardware_validation_result(result);

    SECTION("warning_count matches number of expected_missing issues") {
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_warning_count")) == 1);
    }

    SECTION("status_title shows '1 Issue Detected' (singular)") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_title");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "1 Issue Detected");
    }

    SECTION("status_detail shows '1 missing'") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_detail");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "1 missing");
    }

    SECTION("issues_label shows '1 Hardware Issue' (singular)") {
        REQUIRE(std::string(lv_subject_get_string(
                    state.hardware_validation_state().get_hardware_issues_label_subject())) ==
                "1 Hardware Issue");
    }
}

// ============================================================================
// Info Issues Tests (newly discovered)
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: info issues only (newly discovered)",
                 "[characterization][hardware-validation][setter]") {
    HardwareValidationResult result;
    result.newly_discovered.push_back(make_info("neopixel toolhead_lights"));
    result.newly_discovered.push_back(make_info("fan_generic exhaust_fan"));
    result.newly_discovered.push_back(make_info("filament_switch_sensor runout"));
    state.hardware_validation_state().set_hardware_validation_result(result);

    SECTION("info_count matches number of newly_discovered issues") {
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_info_count")) == 3);
    }

    SECTION("status_detail shows '3 new'") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_detail");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "3 new");
    }

    SECTION("issues_label shows '3 Hardware Issues'") {
        REQUIRE(std::string(lv_subject_get_string(
                    state.hardware_validation_state().get_hardware_issues_label_subject())) ==
                "3 Hardware Issues");
    }
}

// ============================================================================
// Session Changed Issues Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: session changed issues only",
                 "[characterization][hardware-validation][setter]") {
    HardwareValidationResult result;
    // Session changes are warnings that hardware was present last session but is now missing
    HardwareIssue session_issue;
    session_issue.hardware_name = "temperature_sensor enclosure";
    session_issue.hardware_type = HardwareType::SENSOR;
    session_issue.severity = HardwareIssueSeverity::WARNING;
    session_issue.message = "Was present last session";
    result.changed_from_last_session.push_back(session_issue);
    state.hardware_validation_state().set_hardware_validation_result(result);

    SECTION("session_count matches number of changed_from_last_session issues") {
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_session_count")) == 1);
    }

    SECTION("status_detail shows '1 changed'") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_detail");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "1 changed");
    }
}

// ============================================================================
// Mixed Issues Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState, "Hardware validation characterization: mixed issues",
                 "[characterization][hardware-validation][setter]") {
    HardwareValidationResult result;
    result.critical_missing.push_back(make_critical("extruder"));
    result.expected_missing.push_back(make_warning("probe"));
    result.expected_missing.push_back(make_warning("bltouch"));
    result.newly_discovered.push_back(make_info("neopixel case_lights"));
    state.hardware_validation_state().set_hardware_validation_result(result);

    SECTION("each category count is correct") {
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_critical_count")) == 1);
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_warning_count")) == 2);
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_info_count")) == 1);
        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_session_count")) == 0);
    }

    SECTION("status_title shows total count") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_title");
        REQUIRE(std::string(lv_subject_get_string(subject)) == "4 Issues Detected");
    }

    SECTION("status_detail lists all non-empty categories with comma separation") {
        lv_subject_t* subject = get_subject_by_name("hardware_status_detail");
        std::string detail = lv_subject_get_string(subject);
        // Should contain: "1 critical, 2 missing, 1 new"
        REQUIRE(detail.find("1 critical") != std::string::npos);
        REQUIRE(detail.find("2 missing") != std::string::npos);
        REQUIRE(detail.find("1 new") != std::string::npos);
        REQUIRE(detail.find(", ") != std::string::npos);
    }
}

// ============================================================================
// Version Increment Tests
// ============================================================================

// ============================================================================
// get_hardware_validation_result Tests
// ============================================================================

TEST_CASE_METHOD(
    FreshRegisteredPrinterState,
    "Hardware validation characterization: get_hardware_validation_result returns stored result",
    "[characterization][hardware-validation][getter]") {
    SECTION("after setting empty result, returns empty result") {
        // Note: The stored HardwareValidationResult is NOT cleared by reset_for_testing()
        // (unlike subjects which are reset to defaults). To ensure empty state, we must
        // explicitly set an empty result.
        HardwareValidationResult empty_result;
        state.hardware_validation_state().set_hardware_validation_result(empty_result);

        const HardwareValidationResult& result =
            state.hardware_validation_state().get_hardware_validation_result();
        REQUIRE(result.critical_missing.empty());
        REQUIRE(result.expected_missing.empty());
        REQUIRE(result.newly_discovered.empty());
        REQUIRE(result.changed_from_last_session.empty());
        REQUIRE(result.has_issues() == false);
    }

    SECTION("returns stored result after set") {
        HardwareValidationResult input;
        input.critical_missing.push_back(make_critical("extruder"));
        input.expected_missing.push_back(make_warning("probe"));
        state.hardware_validation_state().set_hardware_validation_result(input);

        const HardwareValidationResult& stored =
            state.hardware_validation_state().get_hardware_validation_result();
        REQUIRE(stored.critical_missing.size() == 1);
        REQUIRE(stored.critical_missing[0].hardware_name == "extruder");
        REQUIRE(stored.expected_missing.size() == 1);
        REQUIRE(stored.expected_missing[0].hardware_name == "probe");
    }
}

// ============================================================================
// remove_hardware_issue Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: remove_hardware_issue",
                 "[characterization][hardware-validation][remove]") {
    SECTION("removes issue from expected_missing") {
        HardwareValidationResult result;
        result.expected_missing.push_back(make_warning("probe"));
        result.expected_missing.push_back(make_warning("bltouch"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        state.hardware_validation_state().remove_hardware_issue("probe");

        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_warning_count")) == 1);
        REQUIRE(state.hardware_validation_state()
                    .get_hardware_validation_result()
                    .expected_missing.size() == 1);
    }

    SECTION("removing last issue sets has_issues to 0") {
        HardwareValidationResult result;
        result.critical_missing.push_back(make_critical("extruder"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(state.hardware_validation_state().has_hardware_issues() == true);

        state.hardware_validation_state().remove_hardware_issue("extruder");

        REQUIRE(state.hardware_validation_state().has_hardware_issues() == false);
        REQUIRE(std::string(lv_subject_get_string(get_subject_by_name("hardware_status_title"))) ==
                "All Healthy");
    }

    SECTION("removes issue from critical_missing") {
        HardwareValidationResult result;
        result.critical_missing.push_back(make_critical("extruder"));
        result.critical_missing.push_back(make_critical("heater_bed"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        state.hardware_validation_state().remove_hardware_issue("extruder");

        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_critical_count")) == 1);
        REQUIRE(state.hardware_validation_state()
                    .get_hardware_validation_result()
                    .critical_missing.size() == 1);
    }

    SECTION("removes issue from newly_discovered") {
        HardwareValidationResult result;
        result.newly_discovered.push_back(make_info("neopixel toolhead"));
        result.newly_discovered.push_back(make_info("fan_generic exhaust"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        state.hardware_validation_state().remove_hardware_issue("neopixel toolhead");

        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_info_count")) == 1);
        REQUIRE(state.hardware_validation_state()
                    .get_hardware_validation_result()
                    .newly_discovered.size() == 1);
    }

    SECTION("removing a name that is not present leaves every list intact") {
        HardwareValidationResult result;
        result.critical_missing.push_back(make_critical("extruder"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        state.hardware_validation_state().remove_hardware_issue("no_such_hardware");

        REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_critical_count")) == 1);
        REQUIRE(state.hardware_validation_state().has_hardware_issues() == true);
    }
}

// ============================================================================
// String Formatting Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: string formatting and pluralization",
                 "[characterization][hardware-validation][format]") {
    SECTION("singular issue label: '1 Hardware Issue'") {
        HardwareValidationResult result;
        result.critical_missing.push_back(make_critical("extruder"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(std::string(lv_subject_get_string(
                    state.hardware_validation_state().get_hardware_issues_label_subject())) ==
                "1 Hardware Issue");
    }

    SECTION("plural issue label: '5 Hardware Issues'") {
        HardwareValidationResult result;
        result.critical_missing.push_back(make_critical("extruder"));
        result.expected_missing.push_back(make_warning("probe"));
        result.expected_missing.push_back(make_warning("bltouch"));
        result.newly_discovered.push_back(make_info("neopixel led"));
        result.newly_discovered.push_back(make_info("fan_generic exhaust"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(std::string(lv_subject_get_string(
                    state.hardware_validation_state().get_hardware_issues_label_subject())) ==
                "5 Hardware Issues");
    }

    SECTION("singular title: '1 Issue Detected'") {
        HardwareValidationResult result;
        result.expected_missing.push_back(make_warning("probe"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(std::string(lv_subject_get_string(get_subject_by_name("hardware_status_title"))) ==
                "1 Issue Detected");
    }

    SECTION("plural title: '3 Issues Detected'") {
        HardwareValidationResult result;
        result.critical_missing.push_back(make_critical("extruder"));
        result.expected_missing.push_back(make_warning("probe"));
        result.newly_discovered.push_back(make_info("neopixel led"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(std::string(lv_subject_get_string(get_subject_by_name("hardware_status_title"))) ==
                "3 Issues Detected");
    }
}

// ============================================================================
// has_hardware_issues() Convenience Method Test
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: has_hardware_issues() method",
                 "[characterization][hardware-validation][convenience]") {
    SECTION("has_hardware_issues() returns false initially") {
        REQUIRE(state.hardware_validation_state().has_hardware_issues() == false);
    }

    SECTION("has_hardware_issues() returns true when issues present") {
        HardwareValidationResult result;
        result.newly_discovered.push_back(make_info("neopixel led"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(state.hardware_validation_state().has_hardware_issues() == true);
    }
}

// ============================================================================
// Observer Notification Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: observer fires when validation changes",
                 "[characterization][hardware-validation][observer]") {
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* /*subject*/) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count_ptr)++;
    };

    SECTION("observer fires on hardware_status_level changes") {
        int fire_count = 0;
        lv_subject_t* level = get_subject_by_name("hardware_status_level");
        REQUIRE(level != nullptr);
        lv_subject_add_observer(level, observer_cb, &fire_count);
        const int after_attach = fire_count;

        HardwareValidationResult result;
        result.critical_missing.push_back(make_critical("extruder"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(fire_count > after_attach);
    }
}

// ============================================================================
// Reset Cycle Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: subjects survive a reset cycle",
                 "[characterization][hardware-validation]") {
    HardwareValidationResult result;
    result.critical_missing.push_back(make_critical("extruder"));
    state.hardware_validation_state().set_hardware_validation_result(result);
    REQUIRE(state.hardware_validation_state().has_hardware_issues() == true);
    REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_status_level")) == 2);

    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    REQUIRE(state.hardware_validation_state().has_hardware_issues() == false);
    REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_status_level")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("hardware_critical_count")) == 0);
}

// ============================================================================
// Headline Badge Level Tests
// ============================================================================

TEST_CASE_METHOD(FreshRegisteredPrinterState,
                 "Hardware validation characterization: status_level drives the headline badge",
                 "[characterization][hardware-validation][setter]") {
    lv_subject_t* level = get_subject_by_name("hardware_status_level");
    REQUIRE(level != nullptr);

    SECTION("a clean result selects the OK badge") {
        state.hardware_validation_state().set_hardware_validation_result(
            HardwareValidationResult{});

        REQUIRE(lv_subject_get_int(level) == 0);
    }

    SECTION("info-only issues select the attention badge, not the OK badge") {
        HardwareValidationResult result;
        result.newly_discovered.push_back(make_info("neopixel toolhead_lights"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(lv_subject_get_int(level) == 1);
    }

    SECTION("warning issues select the attention badge") {
        HardwareValidationResult result;
        result.expected_missing.push_back(make_warning("temperature_sensor chamber"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(lv_subject_get_int(level) == 1);
    }

    SECTION("critical issues select the critical badge") {
        HardwareValidationResult result;
        result.critical_missing.push_back(make_critical("extruder"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(lv_subject_get_int(level) == 2);
    }

    SECTION("critical alongside info still selects the critical badge") {
        HardwareValidationResult result;
        result.newly_discovered.push_back(make_info("fan_generic exhaust_fan"));
        result.critical_missing.push_back(make_critical("heater_bed"));
        state.hardware_validation_state().set_hardware_validation_result(result);

        REQUIRE(lv_subject_get_int(level) == 2);
    }
}
