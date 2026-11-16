// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_icon_codepoints.h"

#include "afc_defaults.h"
#include "ams_backend_ace.h"
#include "ams_backend_cfs.h"
#include "ams_device_sections.h"
#include "ams_types.h"
#include "hh_defaults.h"

#include <algorithm>
#include <any>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

namespace {

const DeviceAction& by_id(const std::vector<DeviceAction>& as, const std::string& id) {
    auto it = std::find_if(as.begin(), as.end(), [&](const DeviceAction& a) { return a.id == id; });
    REQUIRE(it != as.end());
    return *it;
}

} // namespace

TEST_CASE("DeviceAction factories set every field the control type uses", "[ams][device_actions]") {
    SECTION("button") {
        auto a = DeviceAction::button("id", "Label", "sec", "icon", "hint");
        CHECK(a.id == "id");
        CHECK(a.label == "Label");
        CHECK(a.section == "sec");
        CHECK(a.icon == "icon");
        CHECK(a.description == "hint");
        CHECK(a.type == ActionType::BUTTON);
        CHECK_FALSE(a.current_value.has_value());
        CHECK(a.slot_index == -1);
        CHECK(a.enabled);
        CHECK(a.disable_reason.empty());
    }
    SECTION("toggle keeps the value's type") {
        auto a = DeviceAction::toggle("id", "L", "s", true);
        CHECK(a.type == ActionType::TOGGLE);
        CHECK(std::any_cast<bool>(a.current_value) == true);
        CHECK(a.icon.empty());
        CHECK_FALSE(DeviceAction::toggle("id", "L", "s").current_value.has_value());
    }
    SECTION("slider") {
        auto a = DeviceAction::slider("id", "L", "s", 450.0f, 100.0f, 2000.0f, "mm", "ruler", "d");
        CHECK(a.type == ActionType::SLIDER);
        CHECK(std::any_cast<float>(a.current_value) == 450.0f);
        CHECK(a.min_value == 100.0f);
        CHECK(a.max_value == 2000.0f);
        CHECK(a.unit == "mm");
        CHECK(a.icon == "ruler");
        CHECK(a.description == "d");
    }
    SECTION("dropdown") {
        auto a = DeviceAction::dropdown("id", "L", "s", {"a", "b"}, std::string("b"));
        CHECK(a.type == ActionType::DROPDOWN);
        CHECK(a.options == std::vector<std::string>{"a", "b"});
        CHECK(std::any_cast<std::string>(a.current_value) == "b");
    }
}

TEST_CASE("DeviceAction::info is a read-only string row", "[ams][device_actions]") {
    auto a = DeviceAction::info("id", "Label", "tool_offsets", "X 0.100 mm", "axis_arrow", "hint");
    CHECK(a.type == ActionType::INFO);
    const auto* value = std::any_cast<std::string>(&a.current_value);
    REQUIRE(value != nullptr);
    CHECK(*value == "X 0.100 mm");
    CHECK(a.label == "Label");
    CHECK(a.section == "tool_offsets");
    CHECK(a.icon == "axis_arrow");
    CHECK(a.description == "hint");
    CHECK_FALSE(a.moves_machine);
    CHECK(a.enabled);
}

TEST_CASE("AFC default actions keep their shipped field values", "[ams][afc][device_actions]") {
    const auto actions = afc_default_actions();
    CHECK(actions.size() == 27);

    const auto& bowden = by_id(actions, "bowden_length");
    CHECK(bowden.label == "Bowden Length");
    CHECK(bowden.icon == "ruler");
    CHECK(bowden.section == "setup");
    CHECK(bowden.description == "Distance from hub to toolhead");
    CHECK(bowden.type == ActionType::SLIDER);
    CHECK(std::any_cast<float>(bowden.current_value) == 450.0f);
    CHECK(bowden.min_value == 100.0f);
    CHECK(bowden.max_value == 2000.0f);
    CHECK(bowden.unit == "mm");

    const auto& cut = by_id(actions, "hub_cut_enabled");
    CHECK(cut.type == ActionType::TOGGLE);
    CHECK(std::any_cast<bool>(cut.current_value) == false);
    CHECK(cut.icon == "tune");
    CHECK(cut.section == "hub");

    CHECK(by_id(actions, "calibration_wizard").moves_machine);
    CHECK_FALSE(bowden.moves_machine);

    const auto& park = by_id(actions, "park");
    CHECK(park.type == ActionType::BUTTON);
    CHECK(park.icon == "home_import");
    CHECK(park.description == "Park the AFC system");
    CHECK_FALSE(park.current_value.has_value());
}

TEST_CASE("Happy Hare default actions keep their shipped field values",
          "[ams][happy_hare][device_actions]") {
    const auto actions = hh_default_actions();
    CHECK(actions.size() == 32);

    const auto& speed = by_id(actions, "gear_from_buffer_speed");
    CHECK(speed.type == ActionType::SLIDER);
    CHECK(std::any_cast<double>(speed.current_value) == 150.0);
    CHECK(speed.min_value == 10.0f);
    CHECK(speed.max_value == 300.0f);
    CHECK(speed.unit == "mm/s");
    CHECK(speed.icon.empty());

    const auto& led = by_id(actions, "led_mode");
    CHECK(led.type == ActionType::DROPDOWN);
    CHECK(led.options == std::vector<std::string>{"off", "gate_status", "filament_color", "on"});
    CHECK(std::any_cast<std::string>(led.current_value) == "off");

    CHECK(std::any_cast<bool>(by_id(actions, "motors_toggle").current_value) == true);
    CHECK(std::any_cast<bool>(by_id(actions, "gear_sync").current_value) == false);
    CHECK(by_id(actions, "servo_buzz").type == ActionType::BUTTON);
}

TEST_CASE("ACE and CFS device actions keep their shipped field values",
          "[ams][ace][cfs][device_actions]") {
    const auto ace = helix::AmsBackendAce(nullptr, nullptr).get_device_actions();
    REQUIRE(ace.size() == 3);
    CHECK(ace[0].id == "ace_manual_feed");
    CHECK(ace[0].section == "filament_control");
    CHECK(ace[0].description == "Feed filament from current slot");
    CHECK(ace[0].type == ActionType::BUTTON);
    // Manual feed and retract push filament through the toolhead path.
    CHECK(ace[0].moves_machine);
    CHECK(ace[1].moves_machine);
    CHECK_FALSE(ace[0].allowed_while_paused);
    CHECK(ace[2].id == "ace_feed_assist_toggle");
    CHECK(ace[2].type == ActionType::TOGGLE);
    CHECK_FALSE(ace[2].current_value.has_value());

    const auto cfs = AmsBackendCfs(nullptr, nullptr).get_device_actions();
    REQUIRE(cfs.size() == 3);
    CHECK(cfs[1].id == "nozzle_clean");
    CHECK(cfs[1].section == "maintenance");
    CHECK(cfs[2].label == "Communication Test");
    CHECK(cfs[2].enabled);
}

TEST_CASE("every default device action names an icon the font has",
          "[ams][device_actions][icons]") {
    for (const auto& actions : {afc_default_actions(), hh_default_actions()}) {
        for (const auto& a : actions) {
            if (!a.icon.empty()) {
                INFO(a.id << " icon " << a.icon);
                CHECK(helix::ui::icon::lookup_codepoint(a.icon.c_str()) != nullptr);
            }
        }
    }
}

TEST_CASE("every backend's sections come from the shared vocabulary with a real icon",
          "[ams][device_actions][device_sections]") {
    for (const auto& kind : helix::printer::kDeviceSectionKinds) {
        INFO(kind.id);
        CHECK(helix::ui::icon::lookup_codepoint(kind.icon) != nullptr);
    }

    std::vector<std::vector<DeviceSection>> all = {
        afc_default_sections(),
        hh_default_sections(),
        helix::AmsBackendAce(nullptr, nullptr).get_device_sections(),
        AmsBackendCfs(nullptr, nullptr).get_device_sections(),
    };
    for (const auto& sections : all) {
        REQUIRE_FALSE(sections.empty());
        for (const auto& section : sections) {
            INFO(section.id);
            const auto* kind = helix::printer::find_device_section_kind(section.id);
            REQUIRE(kind != nullptr);
            CHECK(section.label == kind->label);
            CHECK(section.icon == kind->icon);
        }
    }
    // One word for one kind of section, whichever system declares it.
    CHECK(afc_default_sections()[1].label == hh_default_sections()[1].label);
}

TEST_CASE("tool_offsets is a section kind", "[ams][sections]") {
    const auto* kind = helix::printer::find_device_section_kind("tool_offsets");
    REQUIRE(kind != nullptr);
    CHECK(std::string(kind->label) == "Tool Offsets");
    CHECK(std::string(kind->icon) == "axis_arrow");
}

TEST_CASE("an id outside the section vocabulary still renders",
          "[ams][device_actions][device_sections]") {
    const auto section = helix::printer::device_section("not_a_kind", 3, "d");
    CHECK(section.label == "not_a_kind");
    CHECK(section.icon == "cog");
    CHECK(section.display_order == 3);
}

TEST_CASE("Happy Hare default sections and actions are well-formed", "[ams][hh_defaults]") {
    const auto sections = hh_default_sections();
    const auto actions = hh_default_actions();
    REQUIRE_FALSE(sections.empty());
    REQUIRE_FALSE(actions.empty());

    std::set<std::string> section_ids;
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        INFO("section " << s.id);
        CHECK_FALSE(s.id.empty());
        CHECK_FALSE(s.label.empty());
        CHECK_FALSE(s.description.empty());
        CHECK(s.display_order >= 0);
        if (i > 0) {
            CHECK(s.display_order > sections[i - 1].display_order);
        }
        CHECK(section_ids.insert(s.id).second);
    }

    std::set<std::string> action_ids;
    for (const auto& a : actions) {
        INFO("action " << a.id);
        CHECK_FALSE(a.id.empty());
        CHECK_FALSE(a.label.empty());
        CHECK(section_ids.count(a.section) == 1);
        CHECK(action_ids.insert(a.id).second);
        if (a.type == ActionType::SLIDER) {
            CHECK(a.min_value < a.max_value);
            CHECK_FALSE(a.unit.empty());
        }
        if (a.type == ActionType::DROPDOWN) {
            CHECK(a.options.size() >= 2);
        }
    }
}

TEST_CASE("HH default actions: extruder load and unload share the first button row",
          "[ams][hh_defaults]") {
    // The section overlay packs consecutive buttons two to a row.
    std::vector<std::string> maintenance_buttons;
    for (const auto& a : hh_default_actions()) {
        if (a.section == "maintenance" && a.type == ActionType::BUTTON) {
            maintenance_buttons.push_back(a.id);
        }
    }
    REQUIRE(maintenance_buttons.size() >= 2);
    CHECK(maintenance_buttons[0] == "load_extruder");
    CHECK(maintenance_buttons[1] == "unload_extruder");
}
