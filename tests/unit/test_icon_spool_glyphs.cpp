// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The filament icons are the custom vented-reel glyphs; the nozzle keeps MDI's
// printer-3d-nozzle under its own names, and every site that means the nozzle
// or extruder (not filament) asks for it by those names. Filament sensors have
// names of their own, so a sensor state never reads as a spool of material.

#include "../../include/ams_device_sections.h"
#include "../../include/ui_icon_codepoints.h"
#include "core/data_root_resolver.h"

#include <fstream>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

std::string read_xml(const std::string& file) {
    std::ifstream in(helix::asset_path("ui_xml/" + file));
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The icon attribute (icon="..." or src="...") nearest @p anchor: the first one
// after it, or with @p before the last one ahead of it.
std::string icon_near(const std::string& xml, const std::string& anchor, bool before = false) {
    const size_t at = xml.find(anchor);
    REQUIRE(at != std::string::npos);
    size_t best = std::string::npos;
    for (const char* key : {"icon=\"", "src=\""}) {
        const size_t pos = before ? xml.rfind(key, at) : xml.find(key, at);
        if (pos == std::string::npos)
            continue;
        if (best == std::string::npos || (before ? pos > best : pos < best))
            best = pos;
    }
    REQUIRE(best != std::string::npos);
    const size_t open = xml.find('"', best) + 1;
    return xml.substr(open, xml.find('"', open) - open);
}

std::string codepoint(const char* name) {
    const char* cp = helix::ui::icon::lookup_codepoint(name);
    return cp ? cp : "";
}

} // namespace

TEST_CASE("icon: the filament names are the spool glyphs, the nozzle names the nozzle",
          "[ui_icon][icon]") {
    CHECK(codepoint("filament") == "\xF3\xBE\x80\x81");
    CHECK(codepoint("filament_outline") == "\xF3\xBE\x80\x82");
    CHECK(codepoint("filament_alert") == "\xF3\xBE\x80\x83");
    CHECK(codepoint("nozzle") == "\xF3\xB0\xB9\x9B");
    CHECK(codepoint("nozzle_outline") == "\xF3\xB0\xB9\x9C");
    CHECK(codepoint("nozzle_alert") == "\xF3\xB1\x87\x80");
}

TEST_CASE("icon: sites about the nozzle or extruder keep the nozzle", "[ui_icon][icon]") {
    CHECK(icon_near(read_xml("maintenance_overlay.xml"), "name=\"row_interval_1\"") == "nozzle");
    CHECK(icon_near(read_xml("runout_guidance_modal.xml"), "name=\"btn_purge\"") == "nozzle");
    const std::string pm = read_xml("printer_manager_overlay.xml");
    CHECK(icon_near(pm, "name=\"pm_chip_nozzle_clean\"") == "nozzle");
    CHECK(icon_near(pm, "name=\"pm_chip_purge_line\"") == "nozzle_outline");
    CHECK(icon_near(read_xml("machine_limits_overlay.xml"), "text=\"Extrude Speed\"", true) ==
          "nozzle");
}

TEST_CASE("icon: the globals' filament glyph strings match the icon table", "[ui_icon][icon]") {
    // Text-glyph sites (the clog bar) read these strings, not the icon table.
    const std::string globals = read_xml("globals.xml");
    auto value_of = [&](const std::string& name) {
        const std::string key = "name=\"" + name + "\" value=\"";
        const size_t at = globals.find(key);
        REQUIRE(at != std::string::npos);
        const size_t open = at + key.size();
        return globals.substr(open, globals.find('"', open) - open);
    };
    CHECK(value_of("icon_filament") == codepoint("filament"));
    CHECK(value_of("icon_filament_outline") == codepoint("filament_outline"));
    CHECK(value_of("icon_filament_alert") == codepoint("filament_alert"));
    CHECK(value_of("icon_filament_sensor_alert") == codepoint("filament_sensor_alert"));
}

TEST_CASE("icon: filament sensor sites use the sensor names, not the spool", "[ui_icon][icon]") {
    CHECK(codepoint("filament_sensor") == codepoint("nozzle"));
    CHECK(codepoint("filament_sensor_alert") == codepoint("nozzle_alert"));

    const std::string indicator = read_xml("filament_sensor_indicator.xml");
    CHECK(icon_near(indicator, "variant=\"success\"", true) == "filament_sensor");
    CHECK(icon_near(indicator, "variant=\"danger\"", true) == "filament_sensor_alert");
    CHECK(icon_near(indicator, "variant=\"disabled\"", true) == "filament_sensor");
    CHECK(icon_near(read_xml("sensors_overlay.xml"), "section=\"filament\"") == "filament_sensor");
    CHECK(icon_near(read_xml("printer_manager_overlay.xml"), "name=\"pm_chip_filament_sensors\"") ==
          "filament_sensor_alert");
    CHECK(read_xml("components/clog_bar_body.xml").find("text=\"#icon_filament_sensor_alert\"") !=
          std::string::npos);
    // The Sensors row opens every sensor type, not only filament ones.
    CHECK(icon_near(read_xml("settings_hardware_overlay.xml"), "name=\"row_filament_sensors\"") ==
          "sensor");
}

TEST_CASE("icon: the AMS Toolhead section shows the nozzle", "[ui_icon][icon]") {
    const auto* kind = helix::printer::find_device_section_kind("toolhead");
    REQUIRE(kind != nullptr);
    CHECK(std::string(kind->icon) == "nozzle");
}
