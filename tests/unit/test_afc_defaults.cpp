// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_defaults.h"

#include <algorithm>
#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

// ============================================================================
// Sections
// ============================================================================

TEST_CASE("AFC default sections count", "[defaults][afc]") {
    auto sections = afc_default_sections();
    REQUIRE(sections.size() == 7);
}

TEST_CASE("AFC default sections have required fields", "[defaults][afc]") {
    auto sections = afc_default_sections();
    for (const auto& s : sections) {
        INFO("section id: " << s.id);
        REQUIRE_FALSE(s.id.empty());
        REQUIRE_FALSE(s.label.empty());
        REQUIRE_FALSE(s.description.empty());
        REQUIRE(s.display_order >= 0);
    }
}

TEST_CASE("AFC default sections are in display order", "[defaults][afc]") {
    auto sections = afc_default_sections();
    for (size_t i = 1; i < sections.size(); ++i) {
        INFO("section " << sections[i].id << " (order " << sections[i].display_order
                        << ") should be after " << sections[i - 1].id << " (order "
                        << sections[i - 1].display_order << ")");
        REQUIRE(sections[i].display_order > sections[i - 1].display_order);
    }
}

TEST_CASE("AFC default sections contain known IDs", "[defaults][afc]") {
    auto sections = afc_default_sections();
    std::set<std::string> ids;
    for (const auto& s : sections) {
        ids.insert(s.id);
    }

    REQUIRE(ids.count("setup") == 1);
    REQUIRE(ids.count("speed") == 1);
    REQUIRE(ids.count("maintenance") == 1);
    REQUIRE(ids.count("hub") == 1);
    REQUIRE(ids.count("tip_forming") == 1);
    REQUIRE(ids.count("purge") == 1);
    REQUIRE(ids.count("toolhead") == 1);
}

TEST_CASE("AFC default sections have unique IDs", "[defaults][afc]") {
    auto sections = afc_default_sections();
    std::set<std::string> ids;
    for (const auto& s : sections) {
        REQUIRE(ids.insert(s.id).second); // insert returns false if duplicate
    }
}

// ============================================================================
// Actions
// ============================================================================

TEST_CASE("AFC default actions count", "[defaults][afc]") {
    auto actions = afc_default_actions();
    REQUIRE(actions.size() == 26);
}

TEST_CASE("AFC default actions have required fields", "[defaults][afc]") {
    auto actions = afc_default_actions();
    for (const auto& a : actions) {
        INFO("action id: " << a.id);
        REQUIRE_FALSE(a.id.empty());
        REQUIRE_FALSE(a.label.empty());
        REQUIRE_FALSE(a.icon.empty());
        REQUIRE_FALSE(a.section.empty());
        REQUIRE_FALSE(a.description.empty());
    }
}

TEST_CASE("AFC default actions contain known IDs", "[defaults][afc]") {
    auto actions = afc_default_actions();
    std::set<std::string> ids;
    for (const auto& a : actions) {
        ids.insert(a.id);
    }

    REQUIRE(ids.count("calibration_wizard") == 1);
    REQUIRE(ids.count("bowden_length") == 1);
    REQUIRE(ids.count("speed_fwd") == 1);
    REQUIRE(ids.count("speed_rev") == 1);
    REQUIRE(ids.count("test_lanes") == 1);
    REQUIRE(ids.count("change_blade") == 1);
    REQUIRE(ids.count("park") == 1);
    REQUIRE(ids.count("brush") == 1);
    REQUIRE(ids.count("reset_motor") == 1);
    REQUIRE(ids.count("led_toggle") == 1);
    REQUIRE(ids.count("led_extruder") == 1);
    REQUIRE(ids.count("quiet_mode") == 1);
    REQUIRE(ids.count("hub_cut_enabled") == 1);
    REQUIRE(ids.count("hub_cut_dist") == 1);
    REQUIRE(ids.count("hub_bowden_length") == 1);
    REQUIRE(ids.count("assisted_retract") == 1);
    REQUIRE(ids.count("ramming_volume") == 1);
    REQUIRE(ids.count("unloading_speed_start") == 1);
    REQUIRE(ids.count("cooling_tube_length") == 1);
    REQUIRE(ids.count("cooling_tube_retraction") == 1);
    REQUIRE(ids.count("purge_enabled") == 1);
    REQUIRE(ids.count("purge_length") == 1);
    REQUIRE(ids.count("brush_enabled") == 1);
    REQUIRE(ids.count("tool_stn") == 1);
    REQUIRE(ids.count("tool_stn_unload") == 1);
    REQUIRE(ids.count("tool_sensor_after_extruder") == 1);
}

TEST_CASE("AFC default actions have unique IDs", "[defaults][afc]") {
    auto actions = afc_default_actions();
    std::set<std::string> ids;
    for (const auto& a : actions) {
        REQUIRE(ids.insert(a.id).second);
    }
}

TEST_CASE("AFC default actions have correct section assignments", "[defaults][afc]") {
    auto actions = afc_default_actions();

    // Build section lookup
    auto sections = afc_default_sections();
    std::set<std::string> valid_sections;
    for (const auto& s : sections) {
        valid_sections.insert(s.id);
    }

    // Every action must reference a valid section
    for (const auto& a : actions) {
        INFO("action " << a.id << " references section " << a.section);
        REQUIRE(valid_sections.count(a.section) == 1);
    }

    // Spot-check specific assignments
    auto find = [&](const std::string& id) -> const DeviceAction* {
        for (const auto& a : actions) {
            if (a.id == id)
                return &a;
        }
        return nullptr;
    };

    REQUIRE(find("calibration_wizard")->section == "setup");
    REQUIRE(find("bowden_length")->section == "setup");
    REQUIRE(find("speed_fwd")->section == "speed");
    REQUIRE(find("speed_rev")->section == "speed");
    REQUIRE(find("test_lanes")->section == "maintenance");
    REQUIRE(find("change_blade")->section == "maintenance");
    REQUIRE(find("park")->section == "maintenance");
    REQUIRE(find("brush")->section == "maintenance");
    REQUIRE(find("reset_motor")->section == "maintenance");
    REQUIRE(find("led_toggle")->section == "setup");
    REQUIRE(find("led_extruder")->section == "setup");
    REQUIRE(find("quiet_mode")->section == "setup");
    REQUIRE(find("hub_cut_enabled")->section == "hub");
    REQUIRE(find("hub_cut_dist")->section == "hub");
    REQUIRE(find("hub_bowden_length")->section == "hub");
    REQUIRE(find("assisted_retract")->section == "hub");
    REQUIRE(find("ramming_volume")->section == "tip_forming");
    REQUIRE(find("unloading_speed_start")->section == "tip_forming");
    REQUIRE(find("cooling_tube_length")->section == "tip_forming");
    REQUIRE(find("cooling_tube_retraction")->section == "tip_forming");
    REQUIRE(find("purge_enabled")->section == "purge");
    REQUIRE(find("purge_length")->section == "purge");
    REQUIRE(find("brush_enabled")->section == "purge");
    REQUIRE(find("tool_stn")->section == "toolhead");
    REQUIRE(find("tool_stn_unload")->section == "toolhead");
    REQUIRE(find("tool_sensor_after_extruder")->section == "toolhead");
}

TEST_CASE("AFC default BUTTON actions have correct defaults", "[defaults][afc]") {
    auto actions = afc_default_actions();
    for (const auto& a : actions) {
        if (a.type != ActionType::BUTTON)
            continue;
        INFO("button action: " << a.id);
        REQUIRE_FALSE(a.current_value.has_value());
        REQUIRE(a.options.empty());
        REQUIRE(a.min_value == 0);
        REQUIRE(a.max_value == 0);
        REQUIRE(a.unit.empty());
        REQUIRE(a.slot_index == -1);
        REQUIRE(a.enabled);
        REQUIRE(a.disable_reason.empty());
    }
}

TEST_CASE("AFC default SLIDER actions have valid ranges", "[defaults][afc]") {
    auto actions = afc_default_actions();
    for (const auto& a : actions) {
        if (a.type != ActionType::SLIDER)
            continue;
        INFO("slider action: " << a.id);
        REQUIRE(a.min_value < a.max_value);
        REQUIRE_FALSE(a.unit.empty());
        REQUIRE(a.current_value.has_value());
        REQUIRE(a.enabled);
    }
}

TEST_CASE("AFC bowden_length slider has correct range", "[defaults][afc]") {
    auto actions = afc_default_actions();
    const DeviceAction* bowden = nullptr;
    for (const auto& a : actions) {
        if (a.id == "bowden_length") {
            bowden = &a;
            break;
        }
    }
    REQUIRE(bowden != nullptr);
    REQUIRE(bowden->type == ActionType::SLIDER);
    REQUIRE(bowden->min_value == 100);
    REQUIRE(bowden->max_value == 2000);
    REQUIRE(bowden->unit == "mm");
    REQUIRE(std::any_cast<float>(bowden->current_value) == 450.0f);
}

TEST_CASE("AFC speed sliders have correct range", "[defaults][afc]") {
    auto actions = afc_default_actions();
    for (const auto& a : actions) {
        if (a.id == "speed_fwd" || a.id == "speed_rev") {
            INFO("speed action: " << a.id);
            REQUIRE(a.type == ActionType::SLIDER);
            REQUIRE(a.min_value == 0.5f);
            REQUIRE(a.max_value == 2.0f);
            REQUIRE(a.unit == "x");
            REQUIRE(std::any_cast<float>(a.current_value) == 1.0f);
        }
    }
}

// ============================================================================
// Capabilities
// ============================================================================

TEST_CASE("AFC default capabilities are correct", "[defaults][afc]") {
    auto caps = afc_default_capabilities();
    REQUIRE(caps.supports_endless_spool);
    REQUIRE(caps.supports_bypass);
    REQUIRE(caps.supports_purge);
    REQUIRE(caps.tip_method == TipMethod::CUT);
}
