// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A saved home layout is a set of coordinates in TRACKS, and a track only means
// something against a particular grid. The grid is not fixed per device any
// more: the high-DPI UI scale multiplies the cell edge, so one panel produces a
// different track count per scale (1080x2400 is 12x24 unscaled, 8x18 at 125%,
// 6x14 at 158%). Restoring a config onto other hardware moves it too.
//
// Before this, a layout carried no record of the grid it meant, and
// populate_widgets() wrote its computed positions straight back over the saved
// entries. So the first populate on a different grid replaced the user's
// arrangement with that grid's clamped-and-auto-placed fallback, permanently -
// switching back did not bring it back, because there was nothing left to come
// back to. Spans were already protected from exactly this (#1216, "a property
// of the current screen, not of the user's layout"); positions were not.
//
// The layout is keyed by grid signature. The active grid's arrangement sits
// where readers expect it, so every existing reader is unchanged; the others are
// parked alongside it. These tests pin the property that makes it worth doing:
// an arrangement survives a round trip through another grid untouched.

#include "../test_helpers/config_dir_guard.h"
#include "config.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

namespace {

class PerGridFixture {
  public:
    PerGridFixture() {
        config_path = (config_dir_.dir / "settings.json").string();
    }

    ~PerGridFixture() {
        config.clear_path();
    }

  protected:
    Config config;
    std::string config_path;

  private:
    // Concurrent Catch2 shards run this file's cases in separate processes, so
    // each needs its own config dir — ConfigDirGuard's pid suffix provides it.
    ConfigDirGuard config_dir_{"per_grid"};

  public:
    /// Seed settings.json with one home page and init Config from it.
    void write_and_init(std::initializer_list<json> widgets, const json& extra = json::object()) {
        json home{{"main_page_index", 0},
                  {"next_page_id", 1},
                  {"pages", json::array({json{{"id", "main"}, {"widgets", json(widgets)}}})}};
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            home[it.key()] = it.value();
        }
        json root{{"config_version", 22},
                  {"active_printer_id", "default"},
                  {"printers", {{"default", {{"panel_widgets", {{"home", home}}}}}}}};
        std::ofstream f(config_path);
        f << root.dump(2);
        f.close();
        config.init(config_path);
    }

    static json widget(const char* id, int col, int row, int colspan, int rowspan) {
        return json{{"id", id},   {"enabled", true},    {"col", col},
                    {"row", row}, {"colspan", colspan}, {"rowspan", rowspan}};
    }

    /// Position of one widget in a loaded config, or {-1,-1} when absent.
    static std::pair<int, int> pos_of(const PanelWidgetConfig& cfg, const std::string& id) {
        for (const auto& e : cfg.entries()) {
            if (e.id == id) {
                return {e.col, e.row};
            }
        }
        return {-1, -1};
    }
};

} // namespace

TEST_CASE_METHOD(PerGridFixture,
                 "per-grid layout: an arrangement survives a trip through "
                 "another grid",
                 "[layout][per_grid]") {
    // The property the whole design exists for. Arrange on one grid, visit
    // another, come back: the arrangement is exactly as it was left, not a
    // remap of a remap. A single stored layout cannot do this - the return trip
    // has only the degraded copy to work from.
    write_and_init({widget("printer_image", 0, 0, 4, 4), widget("print_status", 0, 6, 6, 4)},
                   json{{"grid", "6x14"}});

    PanelWidgetConfig cfg("home", config);
    cfg.load();
    REQUIRE(cfg.grid_signature() == "6x14");
    REQUIRE(pos_of(cfg, "printer_image") == std::pair<int, int>{0, 0});
    REQUIRE(pos_of(cfg, "print_status") == std::pair<int, int>{0, 6});

    // Away to a wider grid, where the arrangement is reseated to fit...
    cfg.switch_to_grid(12, 24);
    CHECK(cfg.grid_signature() == "12x24");

    // ...and something happens there, as it would on a real populate.
    for (auto& e : cfg.mutable_entries()) {
        if (e.id == "printer_image") {
            e.col = 8;
            e.row = 20;
        }
    }
    cfg.save();

    // Back again. The 6x14 arrangement is the one that was left, untouched by
    // anything that happened on 12x24.
    cfg.switch_to_grid(6, 14);
    CHECK(cfg.grid_signature() == "6x14");
    CHECK(pos_of(cfg, "printer_image") == std::pair<int, int>{0, 0});
    CHECK(pos_of(cfg, "print_status") == std::pair<int, int>{0, 6});

    // And the 12x24 edit is still there on its own grid.
    cfg.switch_to_grid(12, 24);
    CHECK(pos_of(cfg, "printer_image") == std::pair<int, int>{8, 20});
}

TEST_CASE_METHOD(PerGridFixture,
                 "per-grid layout: a grid never visited is seeded from the one "
                 "being left",
                 "[layout][per_grid]") {
    // A first visit has nothing stored, so it starts from the arrangement the
    // user was just looking at rather than from the shipped defaults - that is
    // the closest thing to their intent we have. Seeded through the same
    // remapper the v22 port uses, so it fits by construction.
    write_and_init({widget("printer_image", 0, 0, 4, 4), widget("print_status", 0, 6, 6, 4)},
                   json{{"grid", "6x14"}});

    PanelWidgetConfig cfg("home", config);
    cfg.load();
    cfg.switch_to_grid(12, 24);

    // Everything that was placed is still placed, and inside the new grid.
    int placed = 0;
    for (const auto& e : cfg.entries()) {
        if (e.id != "printer_image" && e.id != "print_status") {
            continue;
        }
        INFO("widget " << e.id);
        if (!e.has_grid_position()) {
            continue; // the remapper drops to auto-place rather than lying
        }
        CHECK(e.col + e.colspan <= 12);
        CHECK(e.row + e.rowspan <= 24);
        ++placed;
    }
    CHECK(placed > 0);
}

TEST_CASE_METHOD(PerGridFixture,
                 "per-grid layout: a layout with no recorded grid is stamped, "
                 "not moved",
                 "[layout][per_grid]") {
    // Every layout saved before this existed. There is no way to recover which
    // grid it was arranged on, so the first measured populate stamps the grid it
    // is on now and changes nothing else. Moving or reseating it would discard
    // an arrangement on a guess.
    write_and_init({widget("printer_image", 2, 4, 4, 4)});

    PanelWidgetConfig cfg("home", config);
    cfg.load();
    CHECK(cfg.grid_signature().empty());

    cfg.switch_to_grid(12, 24);
    CHECK(cfg.grid_signature() == "12x24");
    CHECK(pos_of(cfg, "printer_image") == std::pair<int, int>{2, 4});
}

TEST_CASE_METHOD(PerGridFixture, "per-grid layout: switching to the active grid is a no-op",
                 "[layout][per_grid]") {
    // populate_widgets() runs on every rebuild, so the common case by far is
    // "already on this grid". It must not churn storage or reseat anything.
    write_and_init({widget("printer_image", 0, 0, 4, 4)}, json{{"grid", "6x14"}});

    PanelWidgetConfig cfg("home", config);
    cfg.load();
    cfg.switch_to_grid(6, 14);

    CHECK(cfg.grid_signature() == "6x14");
    CHECK(pos_of(cfg, "printer_image") == std::pair<int, int>{0, 0});
}

TEST_CASE_METHOD(PerGridFixture, "per-grid layout: a parked arrangement survives reload from disk",
                 "[layout][per_grid]") {
    // The parked grids are only worth anything if they outlive the process. A
    // fresh PanelWidgetConfig over the same Config must see them.
    write_and_init({widget("printer_image", 0, 0, 4, 4)}, json{{"grid", "6x14"}});

    {
        PanelWidgetConfig cfg("home", config);
        cfg.load();
        cfg.switch_to_grid(12, 24);
        for (auto& e : cfg.mutable_entries()) {
            if (e.id == "printer_image") {
                e.col = 6;
                e.row = 10;
            }
        }
        cfg.save();
    }

    PanelWidgetConfig reloaded("home", config);
    reloaded.load();
    CHECK(reloaded.grid_signature() == "12x24");
    CHECK(pos_of(reloaded, "printer_image") == std::pair<int, int>{6, 10});

    reloaded.switch_to_grid(6, 14);
    CHECK(pos_of(reloaded, "printer_image") == std::pair<int, int>{0, 0});
}
