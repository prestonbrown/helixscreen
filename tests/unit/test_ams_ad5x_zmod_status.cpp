// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native ZMOD publishes the IFS state as Klipper objects from ghzserg/z_ad5x#12
// onward, so `zmod_ifs` / `zmod_color` frames arrive by subscription instead of
// being scraped out of IFS_STATUS and GET_ZCOLOR output. These tests pin that
// the pushed frames land through the SAME apply path the macro responses use,
// including Moonraker's diff frames, which carry only what changed.

#include "../lvgl_test_fixture.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_types.h"
#include "printer_discovery.h"
#include "test_helpers/ad5x_ifs_test_access.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

json ifs_frame(int chan, std::vector<bool> ports) {
    json obj = json::object();
    obj["Chan"] = chan;
    obj["State"] = 5;
    obj["Ports"] = ports;
    return json{{"zmod_ifs", obj}};
}

json color_frame() {
    return json{
        {"zmod_color",
         {{"ifs", true},
          {"channel", 1},
          {"color_limit", 4},
          {"slots", json::array({json{{"ID", "1"}, {"Material", "PLA"}, {"HEX", "FF0000"}},
                                 json{{"ID", "2"}, {"Material", "PETG"}, {"HEX", "00FF00"}},
                                 json{{"ID", "3"}, {"Material", "ABS"}, {"HEX", "0000FF"}},
                                 json{{"ID", "4"}, {"Material", "TPU"}, {"HEX", "FFFFFF"}}})}}}};
}

/// A verbatim frame off an AD5X running 1.7.2-37 with the get_status patch:
/// lanes 1 and 4 loaded, 2 and 3 empty. The empty lanes are the point - the
/// firmware fills them with the "?" sentinel and an empty HEX, not with nulls
/// or omitted keys.
json live_color_frame() {
    return json{{"zmod_color",
                 {{"ifs", true},
                  {"display", false},
                  {"channel", 1},
                  {"color_limit", 4},
                  {"extruder_sensor", false},
                  {"slots", json::array({json{{"ID", "1"},
                                              {"Material", "PLA"},
                                              {"Color", "bright purple"},
                                              {"HEX", "A03CF7"},
                                              {"hasFilament", true}},
                                         json{{"ID", "2"},
                                              {"Material", "?"},
                                              {"Color", ""},
                                              {"HEX", ""},
                                              {"hasFilament", false}},
                                         json{{"ID", "3"},
                                              {"Material", "?"},
                                              {"Color", ""},
                                              {"HEX", ""},
                                              {"hasFilament", false}},
                                         json{{"ID", "4"},
                                              {"Material", "PLA"},
                                              {"Color", "white"},
                                              {"HEX", "FFFFFF"},
                                              {"hasFilament", true}}})}}}};
}

} // namespace

TEST_CASE("AD5X IFS subscribes to the zmod objects only where they exist",
          "[ams][ad5x_ifs][zmod_status]") {
    helix::PrinterDiscovery hw;

    // Stock 1.7.2 and older: neither object implements get_status(), so neither
    // appears in objects/list and nothing beyond save_variables is asked for.
    hw.set_printer_objects({"toolhead", "extruder", "save_variables"});
    auto stock = AmsBackendAd5xIfs::required_status_objects(hw);
    CHECK(stock == std::vector<std::string>{"save_variables"});

    hw.set_printer_objects({"toolhead", "save_variables", "zmod_ifs", "zmod_color"});
    auto patched = AmsBackendAd5xIfs::required_status_objects(hw);
    REQUIRE(patched.size() == 3);
    CHECK(std::find(patched.begin(), patched.end(), "zmod_ifs") != patched.end());
    CHECK(std::find(patched.begin(), patched.end(), "zmod_color") != patched.end());
}

TEST_CASE("AD5X IFS applies a pushed zmod_ifs frame", "[ams][ad5x_ifs][zmod_status]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    Ad5xIfsTestAccess::handle_status(backend, ifs_frame(2, {true, true, false, false}));

    CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));
    CHECK(Ad5xIfsTestAccess::port_presence(backend, 1));
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));

    // Chan is 1-based and is the seated-channel authority on native ZMOD, where
    // no tool map exists to launder it through (#1065).
    CHECK(backend.get_current_slot() == 1);
}

TEST_CASE("AD5X IFS honors a Ports-only diff frame", "[ams][ad5x_ifs][zmod_status]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::handle_status(backend, ifs_frame(1, {true, false, false, false}));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));

    // THE GATE. Moonraker sends only what changed, so a spool going into lane 4
    // arrives as Ports alone with no Chan. The gcode path only ever saw this
    // object on a line that already matched "Chan"; requiring it here would
    // silently drop every presence-only update.
    Ad5xIfsTestAccess::handle_status(
        backend, json{{"zmod_ifs", {{"Ports", json::array({true, false, false, true})}}}});

    CHECK(Ad5xIfsTestAccess::port_presence(backend, 0));
    CHECK(Ad5xIfsTestAccess::port_presence(backend, 3));
}

TEST_CASE("AD5X IFS applies pushed zmod_color slots", "[ams][ad5x_ifs][zmod_status]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Presence first: colors are only trusted for lanes the silk sensors see.
    Ad5xIfsTestAccess::handle_status(backend, ifs_frame(1, {true, true, true, true}));
    Ad5xIfsTestAccess::handle_status(backend, color_frame());

    CHECK(backend.get_slot_info(0).material == "PLA");
    CHECK(backend.get_slot_info(0).color_rgb == 0xFF0000);
    CHECK(backend.get_slot_info(1).material == "PETG");
    CHECK(backend.get_slot_info(1).color_rgb == 0x00FF00);
    CHECK(backend.get_slot_info(3).material == "TPU");
    CHECK(backend.get_slot_info(3).color_rgb == 0xFFFFFF);
}

TEST_CASE("AD5X IFS does not surface the firmware's unset-material sentinel",
          "[ams][ad5x_ifs][zmod_status]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    Ad5xIfsTestAccess::handle_status(backend, ifs_frame(1, {true, false, false, true}));
    Ad5xIfsTestAccess::handle_status(backend, live_color_frame());

    CHECK(backend.get_slot_info(0).material == "PLA");
    CHECK(backend.get_slot_info(0).color_rgb == 0xA03CF7);
    CHECK(backend.get_slot_info(3).material == "PLA");

    // THE GATE. "?" is the firmware's own "nothing assigned" marker, and the
    // Adventurer5M.json path already maps it to empty so the UI renders "--".
    // Passed through, it prints a literal "?" on every empty lane.
    CHECK(backend.get_slot_info(1).material.empty());
    CHECK(backend.get_slot_info(2).material.empty());
}

TEST_CASE("AD5X IFS survives malformed zmod frames", "[ams][ad5x_ifs][zmod_status]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::handle_status(backend, ifs_frame(3, {false, false, true, false}));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 2));

    // get_status() is exception-free by construction upstream, but a truncated
    // or type-shifted frame must not take the presence view with it.
    Ad5xIfsTestAccess::handle_status(backend, json{{"zmod_ifs", "not an object"}});
    Ad5xIfsTestAccess::handle_status(backend, json{{"zmod_ifs", {{"Ports", "nope"}}}});
    Ad5xIfsTestAccess::handle_status(
        backend, json{{"zmod_ifs", {{"Ports", json::array({true, false, 7, false})}}}});
    Ad5xIfsTestAccess::handle_status(backend, json{{"zmod_color", {{"slots", 42}}}});
    Ad5xIfsTestAccess::handle_status(
        backend, json{{"zmod_color", {{"slots", json::array({json{{"ID", "9"}}})}}}});

    CHECK(Ad5xIfsTestAccess::port_presence(backend, 2));
    CHECK_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
}
