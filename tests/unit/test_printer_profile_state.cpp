// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "printer_detector.h"
#include "printer_profile_state.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("PrinterProfileState resolves a type once and honours external persistence",
          "[printer_state][profile][1401]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);

    // No database entry: the strategy follows the probe.
    REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
    CHECK(profile.printer_type() == "Unknown Printer");
    CHECK(profile.z_offset_calibration_strategy() == ZOffsetCalibrationStrategy::PROBE_CALIBRATE);
    CHECK(std::string(lv_subject_get_string(profile.get_printer_type_subject())) ==
          "Unknown Printer");
    CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 1);

    // Same type, same strategy: nothing to do.
    CHECK_FALSE(profile.set_printer_type("Unknown Printer", true, false, false));

    // A module that persists the offset forces FIRMWARE_MANAGED on re-resolution.
    REQUIRE(profile.set_external_persistence(true));
    CHECK_FALSE(profile.set_external_persistence(true));
    REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
    CHECK(profile.z_offset_calibration_strategy() == ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
    CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 0);

    // Refuted: the type-derived strategy comes back.
    REQUIRE(profile.set_external_persistence(false));
    REQUIRE(profile.set_printer_type("Unknown Printer", false, false, false));
    CHECK(profile.z_offset_calibration_strategy() == ZOffsetCalibrationStrategy::ENDSTOP);
    CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 1);

    profile.deinit_subjects();
}

TEST_CASE("PrinterProfileState merges firmware option defaults as deltas",
          "[printer_state][profile]") {
    PrinterProfileState profile;
    CHECK(profile.merge_firmware_option_defaults({{"bed_mesh", true}}));
    CHECK_FALSE(profile.merge_firmware_option_defaults({{"bed_mesh", true}}));
    CHECK(profile.merge_firmware_option_defaults({{"priming", false}}));
    CHECK(profile.merge_firmware_option_defaults({{"bed_mesh", false}}));
}

TEST_CASE("PrinterProfileState synthesizes the timelapse option only while available",
          "[printer_state][profile]") {
    PrinterProfileState profile;
    profile.set_timelapse_default_enabled(true);

    profile.apply_dynamic_options(false, true);
    const auto* tl = profile.pre_print_option_set().find("timelapse");
    REQUIRE(tl != nullptr);
    CHECK(tl->default_enabled);

    profile.apply_dynamic_options(false, true);
    CHECK(profile.pre_print_option_set().options.size() == 1);

    profile.apply_dynamic_options(false, false);
    CHECK(profile.pre_print_option_set().find("timelapse") == nullptr);
}

TEST_CASE("PrinterProfileState knows which printers cannot print from a dot-directory",
          "[printer_state][profile][staging]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);

    REQUIRE(profile.set_printer_type("FlashForge Adventurer 5M (ForgeX)", true, false, false));
    CHECK(profile.klipper_hides_dot_dirs());
    REQUIRE(profile.set_printer_type("FlashForge Adventurer 5M Pro (ForgeX)", true, false, false));
    CHECK(profile.klipper_hides_dot_dirs());
    REQUIRE(profile.set_printer_type("FlashForge Adventurer 5X", true, false, false));
    CHECK(profile.klipper_hides_dot_dirs());
    // Stock firmware and everything else print from .helix_temp as before.
    REQUIRE(profile.set_printer_type("FlashForge Adventurer 5M", true, false, false));
    CHECK_FALSE(profile.klipper_hides_dot_dirs());
    REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
    CHECK_FALSE(profile.klipper_hides_dot_dirs());
}

TEST_CASE("Z-offset tune hint claims auto-save only where something persists it",
          "[printer_state][profile][zoffset]") {
    using S = ZOffsetCalibrationStrategy;
    using H = ZOffsetTuneHint;

    // A strategy with its own Save step: the hint points at it, whatever else is known.
    CHECK(resolve_z_offset_tune_hint(S::PROBE_CALIBRATE, false, true, false, true) ==
          H::SAVE_ON_CONTROLS);
    CHECK(resolve_z_offset_tune_hint(S::ENDSTOP, false, false, true, true) == H::SAVE_ON_CONTROLS);

    // Firmware-managed and the firmware persists the offset.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, true, false, true) ==
          H::AUTO_SAVED);
    // An installed persistence provider is proof on its own.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, true, false, false, true) ==
          H::AUTO_SAVED);
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, true, false, true, true) ==
          H::AUTO_SAVED);

    // No Save step and nothing persists: the adjustment is temporary.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, false, false, true) ==
          H::TEMPORARY);
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, false, true, true) ==
          H::TEMPORARY_TOOL_CHANGE);

    // Per-head storage saves for the mounted head only, whatever the save setting.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, true, true, false, true,
                                     /*per_head=*/true) == H::AUTO_SAVED_PER_HEAD);
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, true, true, false, false,
                                     /*per_head=*/true) == H::AUTO_SAVED_PER_HEAD);
}

TEST_CASE("Z-offset tune hint is temporary when Z-offset saving is off",
          "[printer_state][profile][zoffset][save-toggle]") {
    using S = ZOffsetCalibrationStrategy;
    using H = ZOffsetTuneHint;

    // The strategy has a Save step, but the user turned it off (#1766).
    CHECK(resolve_z_offset_tune_hint(S::PROBE_CALIBRATE, false, true, false, false) ==
          H::TEMPORARY);
    CHECK(resolve_z_offset_tune_hint(S::ENDSTOP, false, true, true, false) ==
          H::TEMPORARY_TOOL_CHANGE);

    // Firmware-managed printers have no Save step to turn off.
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, true, false, false) ==
          H::AUTO_SAVED);
    CHECK(resolve_z_offset_tune_hint(S::FIRMWARE_MANAGED, false, false, true, false) ==
          H::TEMPORARY_TOOL_CHANGE);
}

TEST_CASE("PrinterProfileState publishes whether each head's own offset is re-applied at pickup",
          "[printer_state][profile][zoffset][per_head]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);
    auto reapplied = [&] {
        return lv_subject_get_int(profile.get_z_offset_reapplied_per_head_subject());
    };
    auto hint = [&] {
        return static_cast<ZOffsetTuneHint>(
            lv_subject_get_int(profile.get_z_offset_tune_hint_subject()));
    };

    // The Creator 5 line hides the paper test by database whatever the firmware.
    CHECK(PrinterDetector::hide_manual_z_calibration("FlashForge Creator 5"));
    CHECK(PrinterDetector::hide_manual_z_calibration("FlashForge Creator 5 Pro"));
    REQUIRE(profile.set_printer_type("FlashForge Creator 5 Pro", true, false, false));
    CHECK(reapplied() == 0);
    CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED);

    // Z-Mod's per-head storage, found by discovery: re-applied, and saved per head.
    CHECK(profile.set_z_offset_per_head(true));
    CHECK(reapplied() == 1);
    REQUIRE(profile.set_printer_type("FlashForge Creator 5 Pro", true, false, false));
    CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED_PER_HEAD);
    CHECK(profile.set_z_offset_per_head(false));
    CHECK(reapplied() == 0);

    // The U1 re-applies the mounted head's stored offset at every tool change and
    // saves nothing: re-applied, but the tune hint keeps saying temporary.
    REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
    CHECK(profile.z_offset_reapplied_per_head());
    CHECK(reapplied() == 1);
    CHECK(hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);

    profile.deinit_subjects();
}

TEST_CASE("PrinterProfileState republishes the tune hint when Z-offset saving changes",
          "[printer_state][profile][zoffset][save-toggle]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);
    auto hint = [&] {
        return static_cast<ZOffsetTuneHint>(
            lv_subject_get_int(profile.get_z_offset_tune_hint_subject()));
    };

    REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
    REQUIRE(hint() == ZOffsetTuneHint::SAVE_ON_CONTROLS);

    CHECK(profile.set_z_offset_save_enabled(false));
    CHECK(hint() == ZOffsetTuneHint::TEMPORARY);
    CHECK_FALSE(profile.set_z_offset_save_enabled(false));

    // A type change keeps honouring the setting.
    REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
    CHECK(hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);
    REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
    CHECK(hint() == ZOffsetTuneHint::TEMPORARY);

    CHECK(profile.set_z_offset_save_enabled(true));
    CHECK(hint() == ZOffsetTuneHint::SAVE_ON_CONTROLS);

    profile.deinit_subjects();
}

TEST_CASE("PrinterProfileState publishes the z-offset tune hint from the database",
          "[printer_state][profile][zoffset]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);
    auto hint = [&] {
        return static_cast<ZOffsetTuneHint>(
            lv_subject_get_int(profile.get_z_offset_tune_hint_subject()));
    };

    SECTION("Snapmaker U1: no save path, and a tool change re-applies the head's offset") {
        REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
        CHECK(profile.z_offset_calibration_strategy() ==
              ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
        CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 0);
        CHECK(hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);

        // A provider latching later is a change even though the strategy is not.
        REQUIRE(profile.set_external_persistence(true));
        REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
        CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED);
        REQUIRE(profile.set_external_persistence(false));
        REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
        CHECK(hint() == ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE);
    }

    SECTION("the other firmware-managed printers keep the auto-saved claim") {
        for (const char* type :
             {"FlashForge Adventurer 5M (ForgeX)", "FlashForge Adventurer 5M Pro (ForgeX)",
              "FlashForge Adventurer 5X", "FlashForge Creator 5", "FlashForge Creator 5 Pro",
              "Artillery M1 Pro"}) {
            INFO(type);
            REQUIRE(profile.set_printer_type(type, true, false, false));
            CHECK(profile.z_offset_calibration_strategy() ==
                  ZOffsetCalibrationStrategy::FIRMWARE_MANAGED);
            CHECK(hint() == ZOffsetTuneHint::AUTO_SAVED);
        }
    }

    SECTION("a printer with a Save step points at it") {
        REQUIRE(profile.set_printer_type("Unknown Printer", true, false, false));
        CHECK(lv_subject_get_int(profile.get_z_offset_can_save_subject()) == 1);
        CHECK(hint() == ZOffsetTuneHint::SAVE_ON_CONTROLS);
    }

    profile.deinit_subjects();
}

namespace {

/// The z_offset_tune_hint value each z_offset_help_text_* label in @p path is
/// shown for: its bind_flag_if_not_eq ref_value (hidden for every other value).
std::map<std::string, int> tune_hint_label_values(const std::string& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string xml = buf.str();

    std::map<std::string, int> values;
    const std::string name_attr = "name=\"z_offset_help_text_";
    for (size_t pos = xml.find(name_attr); pos != std::string::npos;
         pos = xml.find(name_attr, pos + 1)) {
        const size_t name_start = pos + std::string("name=\"").size();
        const std::string name = xml.substr(name_start, xml.find('"', name_start) - name_start);
        const size_t end = xml.find("</text_small>", pos);
        REQUIRE(end != std::string::npos);
        const std::string element = xml.substr(pos, end - pos);
        INFO(path << " " << name);
        CHECK(element.find("hidden=\"true\"") != std::string::npos);
        const std::string bind = "<bind_flag_if_not_eq subject=\"z_offset_tune_hint\" "
                                 "flag=\"hidden\" ref_value=\"";
        const size_t b = element.find(bind);
        REQUIRE(b != std::string::npos);
        CHECK(element.find("bind_flag_if_not_eq", b + bind.size()) == std::string::npos);
        values[name] = std::stoi(element.substr(b + bind.size()));
    }
    return values;
}

} // namespace

TEST_CASE("Print Tune shows exactly the label for the resolved z-offset tune hint",
          "[printer_state][profile][zoffset][xml]") {
    LVGLTestFixture fixture;
    PrinterProfileState profile;
    profile.init_subjects(false);
    REQUIRE(profile.set_printer_type("Snapmaker U1", true, false, false));
    const int u1_hint = lv_subject_get_int(profile.get_z_offset_tune_hint_subject());
    profile.deinit_subjects();

    const std::map<std::string, int> expected = {
        {"z_offset_help_text_manual", static_cast<int>(ZOffsetTuneHint::SAVE_ON_CONTROLS)},
        {"z_offset_help_text_auto", static_cast<int>(ZOffsetTuneHint::AUTO_SAVED)},
        {"z_offset_help_text_temporary", static_cast<int>(ZOffsetTuneHint::TEMPORARY)},
        {"z_offset_help_text_tool_change",
         static_cast<int>(ZOffsetTuneHint::TEMPORARY_TOOL_CHANGE)},
        {"z_offset_help_text_per_head", static_cast<int>(ZOffsetTuneHint::AUTO_SAVED_PER_HEAD)},
    };

    for (const char* path :
         {"ui_xml/print_tune_panel.xml", "ui_xml/portrait/print_tune_panel.xml"}) {
        INFO(path);
        const auto values = tune_hint_label_values(path);
        CHECK(values == expected);

        // On the U1 the tool-change label is the only one left visible.
        std::vector<std::string> visible;
        for (const auto& [name, value] : values) {
            if (value == u1_hint) {
                visible.push_back(name);
            }
        }
        CHECK(visible == std::vector<std::string>{"z_offset_help_text_tool_change"});
    }
}
