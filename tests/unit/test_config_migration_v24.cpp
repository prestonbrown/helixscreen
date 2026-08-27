// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The v23 -> v24 migration TAGS every saved home layout rather than converting
// or clearing it. Saved col/row/span are cell counts against a grid whose track
// count and cell size both changed, and this runs at config load, before any
// screen size is known — so the conversion cannot happen here. It can happen at
// the first grid build, which knows the panel extent and the measured content
// box, so the coordinates are left intact for PanelWidgetManager to port (see
// tests/unit/test_layout_port.cpp for the conversion itself).
//
// What this migration must therefore NOT do is destroy the inputs that port
// needs. These tests pin that: coordinates and spans survive, the array order
// that drives placement order survives, a deliberate hide survives, and the old
// grid's row cache is carried onto the panel instead of being dropped.
//
// The migration is a static function in config.cpp, so it is driven through the
// public Config::init() path exactly as the v21 tests do. A sandboxed
// HELIX_CONFIG_DIR keeps backup-restore search paths inside the temp dir.

#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace {

class MigrationV24Fixture {
  protected:
    Config config;
    std::string temp_dir;
    std::string config_path;
    std::string saved_config_dir_;
    bool had_config_dir_ = false;

    void SetUp() {
        temp_dir = (fs::temp_directory_path() / "helix_migration_v23_test").string();
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);

        if (const char* prev = std::getenv("HELIX_CONFIG_DIR")) {
            saved_config_dir_ = prev;
            had_config_dir_ = true;
        }
        setenv("HELIX_CONFIG_DIR", temp_dir.c_str(), 1);

        config_path = temp_dir + "/settings.json";
    }

    void TearDown() {
        fs::remove_all(temp_dir);
        if (had_config_dir_) {
            setenv("HELIX_CONFIG_DIR", saved_config_dir_.c_str(), 1);
        } else {
            unsetenv("HELIX_CONFIG_DIR");
        }
        config.clear_path();
    }

    /// Write a settings.json, then init Config from it so the real versioned
    /// migrations run.
    void write_and_init(const json& contents) {
        std::ofstream f(config_path);
        f << contents.dump(2);
        f.close();
        config.init(config_path);
    }

    /// The multi-page panel_widgets shape, wrapping one page of widgets.
    static json home(std::initializer_list<json> widgets) {
        return json{{"main_page_index", 0},
                    {"next_page_id", 1},
                    {"pages", json::array({json{{"id", "main"}, {"widgets", json(widgets)}}})}};
    }

    json widgets_of(const char* printer = "default") const {
        return config.get<json>(
            std::string("/printers/") + printer + "/panel_widgets/home/pages/0/widgets", json());
    }

    json panel_of(const char* printer = "default") const {
        return config.get<json>(std::string("/printers/") + printer + "/panel_widgets/home",
                                json());
    }

  public:
    MigrationV24Fixture() {
        SetUp();
    }
    ~MigrationV24Fixture() {
        TearDown();
    }
};

} // namespace

TEST_CASE_METHOD(MigrationV24Fixture, "Config migration v24: preserves every coordinate and span",
                 "[config][migration][24]") {
    write_and_init({{"config_version", 23},
                    {"active_printer_id", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", home({{{"id", "printer_image"},
                                          {"enabled", true},
                                          {"col", 0},
                                          {"row", 0},
                                          {"colspan", 2},
                                          {"rowspan", 2}},
                                         {{"id", "tips"},
                                          {"enabled", true},
                                          {"col", 2},
                                          {"row", 0},
                                          {"colspan", 4},
                                          {"rowspan", 2}}})}}}}}}}});

    auto widgets = widgets_of();
    REQUIRE(widgets.is_array());
    REQUIRE(widgets.size() == 2);
    // The port runs later and needs these numbers; destroying them here is
    // exactly what this migration used to do and must not do again.
    CHECK(widgets[0]["col"] == 0);
    CHECK(widgets[0]["row"] == 0);
    CHECK(widgets[0]["colspan"] == 2);
    CHECK(widgets[0]["rowspan"] == 2);
    CHECK(widgets[1]["col"] == 2);
    CHECK(widgets[1]["row"] == 0);
    CHECK(widgets[1]["colspan"] == 4);
    CHECK(widgets[1]["rowspan"] == 2);

    // And they must be marked as the unit they are, or the first grid build
    // reads cell counts as track counts.
    auto panel = panel_of();
    CHECK(panel["layout_units"] == "cells_v21");
    CHECK(config.get<int>("/config_version", 0) == 24);
}

TEST_CASE_METHOD(MigrationV24Fixture,
                 "Config migration v24: a user trash keeps its hide, an engine disable does not",
                 "[config][migration][24]") {
    write_and_init({{"config_version", 23},
                    {"active_printer_id", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", home({{{"id", "tips"},
                                          {"enabled", false},
                                          {"col", 2},
                                          {"row", 0},
                                          {"colspan", 4},
                                          {"rowspan", 2}},
                                         {{"id", "fan_stack"},
                                          {"enabled", false},
                                          {"col", -1},
                                          {"row", -1},
                                          {"colspan", 1},
                                          {"rowspan", 1}},
                                         {{"id", "led"},
                                          {"enabled", true},
                                          {"col", 3},
                                          {"row", 2},
                                          {"colspan", 1},
                                          {"rowspan", 1}}})}}}}}}}});

    auto widgets = widgets_of();
    REQUIRE(widgets.size() == 3);
    // tips carried real coordinates, so its disable was a trash press.
    CHECK(widgets[0]["enabled"] == false);
    // fan_stack was already unplaced, so the disable was the engine's; the key
    // goes away and the registry default decides.
    CHECK_FALSE(widgets[1].contains("enabled"));
    CHECK(widgets[2]["enabled"] == true);
    // Array order drives placement order and must not change.
    CHECK(widgets[0]["id"] == "tips");
    CHECK(widgets[1]["id"] == "fan_stack");
    CHECK(widgets[2]["id"] == "led");
}

TEST_CASE_METHOD(MigrationV24Fixture, "Config migration v24: covers every printer profile",
                 "[config][migration][24]") {
    write_and_init(
        {{"config_version", 23},
         {"active_printer_id", "default"},
         {"printers",
          {{"default",
            {{"panel_widgets",
              {{"home", home({{{"id", "led"}, {"enabled", true}, {"col", 1}, {"row", 1}}})}}}}},
           {"printer-2",
            {{"panel_widgets",
              {{"home",
                home({{{"id", "tips"}, {"enabled", true}, {"col", 3}, {"row", 2}}})}}}}}}}});

    for (const char* id : {"default", "printer-2"}) {
        INFO("printer " << id);
        auto widgets = widgets_of(id);
        REQUIRE(widgets.size() == 1);
        CHECK(widgets[0]["col"].get<int>() >= 0);
        CHECK(panel_of(id)["layout_units"] == "cells_v21");
    }
}

TEST_CASE_METHOD(MigrationV24Fixture, "Config migration v24: handles a legacy flat array",
                 "[config][migration][24]") {
    // Configs written before the multi-page format hold a bare array, which
    // PanelWidgetConfig::load() routes down a separate branch that can decide
    // the config is pre-grid and replace it with build_defaults(), discarding
    // every deliberate hide. The migration converts the array to the page shape
    // so that branch is never reached, and tags it like any other panel.
    write_and_init(
        {{"config_version", 23},
         {"active_printer_id", "default"},
         {"printers",
          {{"default",
            {{"panel_widgets",
              {{"home",
                json::array({{{"id", "tips"},
                              {"enabled", false},
                              {"col", 2},
                              {"row", 0},
                              {"colspan", 4},
                              {"rowspan", 2}},
                             {{"id", "led"}, {"enabled", true}, {"col", 3}, {"row", 2}}})}}}}}}}});

    auto homecfg = panel_of();
    REQUIRE(homecfg.is_object());
    REQUIRE(homecfg.contains("pages"));
    CHECK(homecfg["layout_units"] == "cells_v21");
    auto widgets = homecfg["pages"][0]["widgets"];
    REQUIRE(widgets.size() == 2);
    CHECK(widgets[0]["enabled"] == false); // deliberate hide survives
    CHECK(widgets[0]["col"] == 2);         // and so does its position
    CHECK(widgets[0]["colspan"] == 4);
}

TEST_CASE_METHOD(MigrationV24Fixture,
                 "Config migration v24: carries the cached row count onto the panel",
                 "[config][migration][24]") {
    // /ui/cached_grid/<panel>/rows is a row count in cells of the old grid. It
    // is the one thing about that grid a saved layout cannot re-derive on its
    // own — the old row axis was sized from the widgets in use, with this as a
    // floor for widgets whose hardware gate had not yet fired. So it moves onto
    // the panel beside the tag rather than being dropped, and /ui loses the key.
    write_and_init({{"config_version", 23},
                    {"ui", {{"cached_grid", {{"home", {{"rows", 4}}}}}}},
                    {"active_printer_id", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", home({{{"id", "led"},
                                          {"enabled", true},
                                          {"col", 1},
                                          {"row", 1},
                                          {"colspan", 1},
                                          {"rowspan", 1}}})}}}}}}}});

    auto ui = config.get<json>("/ui", json());
    CHECK_FALSE(ui.contains("cached_grid"));
    CHECK(panel_of()["legacy_rows"] == 4);
}

TEST_CASE_METHOD(MigrationV24Fixture, "Config migration v24: a panel with no cache records zero",
                 "[config][migration][24]") {
    // Absent cache is not the same as a cache of zero rows, but the port treats
    // both as "unknown" and reads the row count off the layout, so the tag can
    // carry a plain 0 rather than an optional.
    write_and_init({{"config_version", 23},
                    {"active_printer_id", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", home({{{"id", "led"},
                                          {"enabled", true},
                                          {"col", 1},
                                          {"row", 1},
                                          {"colspan", 1},
                                          {"rowspan", 1}}})}}}}}}}});

    CHECK(panel_of()["legacy_rows"] == 0);
    CHECK(panel_of()["layout_units"] == "cells_v21");
}

TEST_CASE_METHOD(MigrationV24Fixture, "Config migration v24: is idempotent",
                 "[config][migration][24]") {
    write_and_init({{"config_version", 24},
                    {"active_printer_id", "default"},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home", home({{{"id", "led"},
                                          {"enabled", true},
                                          {"col", 3},
                                          {"row", 2},
                                          {"colspan", 1},
                                          {"rowspan", 1}}})}}}}}}}});

    // Already stamped 24 — the migration must not run and must not touch a
    // layout the user arranged after upgrading.
    auto widgets = widgets_of();
    REQUIRE(widgets.size() == 1);
    CHECK(widgets[0]["col"] == 3);
    CHECK(widgets[0]["row"] == 2);
    CHECK(widgets[0]["colspan"] == 1);
    CHECK(widgets[0]["rowspan"] == 1);
}

TEST_CASE_METHOD(MigrationV24Fixture, "Config migration v24: survives every missing node",
                 "[config][migration][24]") {
    // No printers at all; a printers value that is not an object; a page that is
    // a string; a widgets value that is an object. Each must migrate and stamp
    // rather than throw.
    write_and_init({{"config_version", 23}});
    CHECK(config.get<int>("/config_version", 0) == 24);

    TearDown();
    SetUp();
    write_and_init(
        {{"config_version", 23},
         {"printers",
          {{"default", {{"panel_widgets", {{"home", {{"pages", json::array({"main"})}}}}}}}}}});
    CHECK(config.get<int>("/config_version", 0) == 24);

    TearDown();
    SetUp();
    write_and_init({{"config_version", 23},
                    {"printers",
                     {{"default",
                       {{"panel_widgets",
                         {{"home",
                           {{"pages", json::array({json{{"id", "main"},
                                                        {"widgets", json::object()}}})}}}}}}}}}});
    CHECK(config.get<int>("/config_version", 0) == 24);

    TearDown();
    SetUp();
    write_and_init({{"config_version", 23}, {"printers", "not-an-object"}});
    CHECK(config.get<int>("/config_version", 0) == 24);
}
