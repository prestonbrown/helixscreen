// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../helix_test_fixture.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "data_root_resolver.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "layout_manager.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

extern "C" void lv_xml_component_init(void);

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace fs = std::filesystem;

// ============================================================================
// RAII helper: change CWD to a temp directory, restore on destruction
// ============================================================================
//
// build_default_grid() opens "config/default_layout.json" relative to CWD.
// Tests use this guard to control which file (if any) the function sees.
//
// NOTE: The breakpoint subject is a zero-initialized static lv_subject_t in
// theme_manager.cpp. In tests (no LVGL theme init), lv_subject_get_int()
// returns 0, which maps to breakpoint index 0 = "tiny". All test JSON
// placements must use "tiny" to match the runtime breakpoint.

class TempCwdGuard {
  public:
    TempCwdGuard() {
        char buf[4096];
        auto* r = getcwd(buf, sizeof(buf));
        (void)r;
        original_cwd_ = buf;
        tmp_dir_ = fs::temp_directory_path() / ("helix_test_layout_" + std::to_string(getpid()));
        fs::create_directories(tmp_dir_);
        int rc = chdir(tmp_dir_.c_str());
        (void)rc;

        // Isolate from HELIX_DATA_DIR / HELIX_CONFIG_DIR leaked by other tests
        // (e.g. test_display_manager.cpp::ensure_data_dir sets HELIX_DATA_DIR to
        // the project root and never clears it). Without this, find_readable()
        // would resolve the real assets/config/default_layout.json and the
        // "missing file" fallback tests would see JSON data instead.
        save_and_unset_env("HELIX_DATA_DIR", saved_data_dir_, had_data_dir_);
        save_and_unset_env("HELIX_CONFIG_DIR", saved_config_dir_, had_config_dir_);

        // Breakpoint is pinned to Micro by the bp_ member below, which also puts
        // it back. Setting it here without restoring left every later test in
        // the process building a Micro-cell grid.
    }

    ~TempCwdGuard() {
        (void)chdir(original_cwd_.c_str());
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
        restore_env("HELIX_DATA_DIR", saved_data_dir_, had_data_dir_);
        restore_env("HELIX_CONFIG_DIR", saved_config_dir_, had_config_dir_);
    }

    /// Write config/default_layout.json with given content
    void write_layout(const std::string& content) {
        fs::create_directories(tmp_dir_ / "config");
        std::ofstream out(tmp_dir_ / "config" / "default_layout.json");
        out << content;
        out.close();
    }

    /// Remove config/default_layout.json if it exists
    void remove_layout() {
        std::error_code ec;
        fs::remove(tmp_dir_ / "config" / "default_layout.json", ec);
    }

    TempCwdGuard(const TempCwdGuard&) = delete;
    TempCwdGuard& operator=(const TempCwdGuard&) = delete;

  private:
    /// Micro for the whole guard's life: this file's JSON placements are all
    /// authored under the "tiny" key, which is breakpoint index 0. Restored on
    /// destruction so it does not redefine the grid for the rest of the suite.
    helix::test::ScopedBreakpoint bp_{UiBreakpoint::Micro};

    static void save_and_unset_env(const char* name, std::string& saved, bool& had) {
        const char* v = getenv(name);
        had = (v != nullptr);
        if (had) {
            saved = v;
            unsetenv(name);
        }
    }

    static void restore_env(const char* name, const std::string& saved, bool had) {
        if (had) {
            setenv(name, saved.c_str(), 1);
        } else {
            unsetenv(name);
        }
    }

    std::string original_cwd_;
    fs::path tmp_dir_;
    std::string saved_data_dir_;
    std::string saved_config_dir_;
    bool had_data_dir_ = false;
    bool had_config_dir_ = false;
};

// ============================================================================
// Helper: count of widget defs that build_default_grid() includes
// ============================================================================
//
// build_default_grid() includes every widget definition once (multi-instance
// widgets are included as their base ID; additional instances are user-added).

static size_t grid_widget_count() {
    return get_all_widget_defs().size();
}

// ============================================================================
// Helper: find entry by ID in a vector
// ============================================================================

static const PanelWidgetEntry* find_entry(const std::vector<PanelWidgetEntry>& entries,
                                          const std::string& id) {
    for (const auto& e : entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("default_layout: valid JSON with tiny breakpoint produces correct anchors",
          "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            },
            {
                "id": "print_status",
                "placements": {
                    "tiny": { "col": 0, "row": 2, "colspan": 2, "rowspan": 2 }
                }
            },
            {
                "id": "tips",
                "placements": {
                    "tiny": { "col": 2, "row": 0, "colspan": 4, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 2);
    CHECK(pi->rowspan == 2);
    CHECK(pi->enabled);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->col == 0);
    CHECK(ps->row == 2);
    CHECK(ps->colspan == 2);
    CHECK(ps->rowspan == 2);
    CHECK(ps->enabled);

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->col == 2);
    CHECK(tips->row == 0);
    CHECK(tips->colspan == 4);
    CHECK(tips->rowspan == 2);
    CHECK(tips->enabled);
}

TEST_CASE("default_layout: different breakpoints produce different placements",
          "[default_layout]") {
    // Runtime breakpoint is "tiny" (index 0). Providing both tiny and large
    // placements verifies that only the tiny values are selected.
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny":  { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 },
                    "large": { "col": 0, "row": 0, "colspan": 3, "rowspan": 3 }
                }
            },
            {
                "id": "tips",
                "placements": {
                    "tiny":  { "col": 2, "row": 0, "colspan": 2, "rowspan": 2 },
                    "large": { "col": 3, "row": 0, "colspan": 5, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    // Tiny values selected (not large: 3x3)
    CHECK(pi->colspan == 2);
    CHECK(pi->rowspan == 2);

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    // Tiny values (not large: col=3 5x2)
    CHECK(tips->col == 2);
    CHECK(tips->colspan == 2);
    CHECK(tips->rowspan == 2);
}

TEST_CASE("default_layout: missing file falls back to hardcoded defaults", "[default_layout]") {
    TempCwdGuard guard;
    // No layout file written — config/default_layout.json does not exist

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // Hardcoded fallback anchors: printer_image, print_status, tips
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->enabled);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 4);
    CHECK(pi->rowspan == 4);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->enabled);
    CHECK(ps->has_grid_position());
    CHECK(ps->col == 0);
    CHECK(ps->row == 4);
    CHECK(ps->colspan == 4);
    CHECK(ps->rowspan == 4);

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->enabled);
    CHECK(tips->has_grid_position());
    CHECK(tips->col == 4);
    CHECK(tips->row == 0);
    CHECK(tips->colspan == 4);
    CHECK(tips->rowspan == 4);
}

TEST_CASE("default_layout: malformed JSON falls back gracefully", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout("{ this is not valid json }}}}");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // Should get hardcoded fallback anchors
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->has_grid_position());

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->has_grid_position());
}

TEST_CASE("default_layout: empty anchors array falls back to hardcoded defaults",
          "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({ "anchors": [] })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // Empty anchors array -> no anchors loaded -> hardcoded fallback triggered
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 4);
    CHECK(pi->rowspan == 4);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->has_grid_position());

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->has_grid_position());
}

TEST_CASE("default_layout: unknown widget ID in JSON is ignored", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "totally_bogus_widget",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 1, "rowspan": 1 }
                }
            },
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // The bogus widget should not appear in entries
    auto* bogus = find_entry(entries, "totally_bogus_widget");
    CHECK(bogus == nullptr);

    // The valid widget should be anchored
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
}

TEST_CASE("default_layout: missing breakpoint in placements causes fallback", "[default_layout]") {
    TempCwdGuard guard;
    // Only define "large" placements — runtime breakpoint is "tiny", so no match.
    // With no anchors matched, the empty vector triggers hardcoded fallback.
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "large": { "col": 0, "row": 0, "colspan": 3, "rowspan": 3 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // No anchors matched for tiny breakpoint -> empty anchors -> hardcoded fallback
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 4);
    CHECK(pi->rowspan == 4);
}

TEST_CASE("default_layout: partial breakpoint match does not trigger fallback",
          "[default_layout]") {
    TempCwdGuard guard;
    // One anchor has "tiny" placement, one only has "large"
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            },
            {
                "id": "tips",
                "placements": {
                    "large": { "col": 3, "row": 0, "colspan": 5, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // printer_image has tiny placement -> anchored from JSON
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->colspan == 2);

    // tips has only large placement -> not matched for "tiny".
    // But since at least one anchor was loaded, fallback is NOT triggered.
    // So tips gets auto-placed (col=-1, row=-1).
    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK_FALSE(tips->has_grid_position());
}

TEST_CASE("default_layout: result always has at least some enabled widgets", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({ "anchors": [] })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE_FALSE(entries.empty());

    bool any_enabled = std::any_of(entries.begin(), entries.end(),
                                   [](const PanelWidgetEntry& e) { return e.enabled; });
    CHECK(any_enabled);
}

TEST_CASE("default_layout: result always has at least some enabled widgets even with missing file",
          "[default_layout]") {
    TempCwdGuard guard;
    // No layout file at all

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE_FALSE(entries.empty());

    bool any_enabled = std::any_of(entries.begin(), entries.end(),
                                   [](const PanelWidgetEntry& e) { return e.enabled; });
    CHECK(any_enabled);
}

TEST_CASE("default_layout: non-anchor widgets get auto-place coordinates", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();

    for (const auto& e : entries) {
        if (e.id == "printer_image")
            continue;
        INFO("Widget " << e.id << " col=" << e.col << " row=" << e.row);
        // All non-anchor widgets should have col=-1, row=-1 (auto-placed)
        CHECK(e.col == -1);
        CHECK(e.row == -1);
    }
}

TEST_CASE("default_layout: anchor with empty id is skipped", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 1, "rowspan": 1 }
                }
            },
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // Should not crash, printer_image should still be anchored
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
}

TEST_CASE("default_layout: JSON with missing anchors key falls back to hardcoded defaults",
          "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({ "something_else": true })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() == grid_widget_count());

    // anchors key missing -> .value("anchors", json::array()) returns empty ->
    // no anchors loaded -> hardcoded fallback
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->has_grid_position());
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 4);
    CHECK(pi->rowspan == 4);
}

TEST_CASE("default_layout: anchor placements default col/row/span values when omitted",
          "[default_layout]") {
    TempCwdGuard guard;
    // Placement exists for "tiny" but is missing some fields
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 1 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    const auto* def = helix::find_widget_def("printer_image");
    REQUIRE(def);
    // col comes from the JSON; row falls back to 0. An omitted span takes the
    // registry span rather than one track — one track is a quarter of the area
    // the widget declares as its minimum, so defaulting to it would render an
    // anchored widget at a size it is not allowed to have.
    CHECK(pi->col == 1);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == def->colspan);
    CHECK(pi->rowspan == def->rowspan);
    CHECK(pi->colspan > 1);
    CHECK(pi->rowspan > 1);
}

TEST_CASE("default_layout: custom anchor positions from JSON override hardcoded defaults",
          "[default_layout]") {
    TempCwdGuard guard;
    // Use non-default positions to verify JSON takes priority over hardcoded values
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 5, "row": 3, "colspan": 1, "rowspan": 1 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    // JSON values should override the hardcoded fallback positions
    CHECK(pi->col == 5);
    CHECK(pi->row == 3);
    CHECK(pi->colspan == 1);
    CHECK(pi->rowspan == 1);
}

TEST_CASE("default_layout: all registry widgets present in result regardless of JSON content",
          "[default_layout]") {
    TempCwdGuard guard;
    // Only anchor one widget — all others should still appear in result
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    const auto& defs = get_all_widget_defs();
    REQUIRE(entries.size() == grid_widget_count());

    // Every non-multi-instance registry widget must appear exactly once
    std::set<std::string> entry_ids;
    for (const auto& e : entries) {
        entry_ids.insert(e.id);
    }
    for (const auto& def : defs) {
        if (def.multi_instance)
            continue;
        CHECK(entry_ids.count(def.id) == 1);
    }
}

// ============================================================================
// RAII helper: register an ams_slot_count subject for testing
// ============================================================================
//
// Ensures the XML "globals" scope exists so lv_xml_register_subject() and
// lv_xml_get_subject() work in the unit-test environment (no full lv_init).

class AmsSubjectGuard {
  public:
    explicit AmsSubjectGuard(int slot_count) {
        if (!lv_xml_component_get_scope("globals"))
            lv_xml_component_init();
        // Use a static subject so the pointer registered with the XML system
        // remains valid after this guard is destroyed (avoids dangling pointer).
        static bool initialized = false;
        if (!initialized) {
            lv_subject_init_int(&subject_, 0);
            lv_xml_register_subject(nullptr, "ams_slot_count", &subject_);
            initialized = true;
        }
        lv_subject_set_int(&subject_, slot_count);
    }

    void set(int val) {
        lv_subject_set_int(&subject_, val);
    }

    ~AmsSubjectGuard() {
        // Reset to 0 so subsequent tests see "no AMS" by default
        lv_subject_set_int(&subject_, 0);
    }

    AmsSubjectGuard(const AmsSubjectGuard&) = delete;
    AmsSubjectGuard& operator=(const AmsSubjectGuard&) = delete;

  private:
    static lv_subject_t subject_;
};

lv_subject_t AmsSubjectGuard::subject_{};

// ============================================================================
// Helper: set breakpoint and restore on scope exit
// ============================================================================

class BreakpointGuard {
  public:
    explicit BreakpointGuard(UiBreakpoint bp) {
        subj_ = theme_manager_get_breakpoint_subject();
        if (subj_) {
            if (subj_->type != LV_SUBJECT_TYPE_INT) {
                // Zero-initialized subject in test env — init it properly
                lv_subject_init_int(subj_, 0);
            }
            original_ = lv_subject_get_int(subj_);
            lv_subject_set_int(subj_, to_int(bp));
        }
    }

    ~BreakpointGuard() {
        if (subj_)
            lv_subject_set_int(subj_, original_);
    }

    BreakpointGuard(const BreakpointGuard&) = delete;
    BreakpointGuard& operator=(const BreakpointGuard&) = delete;

  private:
    lv_subject_t* subj_ = nullptr;
    int original_ = 0;
};

// ============================================================================
// bed_temperature conditional placement tests
// ============================================================================

TEST_CASE("default_layout: bed_temperature is always last in result", "[default_layout]") {
    TempCwdGuard guard;
    guard.write_layout(R"({ "anchors": [] })");

    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(!entries.empty());
    CHECK(entries.back().id == "bed_temperature");
}

TEST_CASE("default_layout: bed_temperature enabled at small breakpoint without AMS",
          "[default_layout]") {
    TempCwdGuard guard;
    BreakpointGuard bp(UiBreakpoint::Small); // small
    guard.write_layout(R"({ "anchors": [] })");

    // No AMS subject registered → ams_slot_count lookup returns NULL → no AMS
    auto entries = PanelWidgetConfig::build_default_grid();
    auto* bed = find_entry(entries, "bed_temperature");
    REQUIRE(bed);
    CHECK(bed->enabled);
    CHECK(entries.back().id == "bed_temperature");
}

TEST_CASE("default_layout: bed_temperature enabled at large breakpoint even with AMS",
          "[default_layout]") {
    TempCwdGuard guard;
    BreakpointGuard bp(UiBreakpoint::Large); // large
    AmsSubjectGuard ams(4);
    guard.write_layout(R"({ "anchors": [] })");

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* bed = find_entry(entries, "bed_temperature");
    REQUIRE(bed);
    CHECK(bed->enabled);
    CHECK(entries.back().id == "bed_temperature");
}

TEST_CASE("default_layout: bed_temperature enabled at xlarge breakpoint even with AMS",
          "[default_layout]") {
    TempCwdGuard guard;
    BreakpointGuard bp(UiBreakpoint::XLarge); // xlarge
    AmsSubjectGuard ams(4);
    guard.write_layout(R"({ "anchors": [] })");

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* bed = find_entry(entries, "bed_temperature");
    REQUIRE(bed);
    CHECK(bed->enabled);
    CHECK(entries.back().id == "bed_temperature");
}

TEST_CASE("default_layout: bed_temperature enabled at medium breakpoint without AMS",
          "[default_layout]") {
    TempCwdGuard guard;
    BreakpointGuard bp(UiBreakpoint::Medium); // medium
    guard.write_layout(R"({ "anchors": [] })");

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* bed = find_entry(entries, "bed_temperature");
    REQUIRE(bed);
    CHECK(bed->enabled);
    CHECK(entries.back().id == "bed_temperature");
}

TEST_CASE("default_layout: anchored bed_temperature survives AMS at medium breakpoint",
          "[default_layout][regression]") {
    TempCwdGuard guard;
    BreakpointGuard bp(UiBreakpoint::Medium);
    AmsSubjectGuard ams(4);
    // The shipped medium table anchors bed_temperature. An anchor states where
    // this tier puts the widget, so it answers the "is there room" question the
    // AMS heuristic is guessing at. Before the fix the heuristic won: the K2 and
    // every other AMS printer at or below 800x480 shipped with no bed readout and
    // an empty cell where the anchor had reserved one.
    guard.write_layout(R"({
      "anchors": [
        { "id": "bed_temperature",
          "placements": { "medium": { "col": 3, "row": 3, "colspan": 1, "rowspan": 1 } } }
      ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* bed = find_entry(entries, "bed_temperature");
    REQUIRE(bed);
    CHECK(bed->enabled);
    CHECK(bed->col == 3);
    CHECK(bed->row == 3);
}

TEST_CASE("default_layout: unanchored bed_temperature survives AMS at medium breakpoint",
          "[default_layout]") {
    TempCwdGuard guard;
    BreakpointGuard bp(UiBreakpoint::Medium);
    AmsSubjectGuard ams(4);
    // bed_temperature ships enabled on every tier, anchored or not, and the
    // placement engine decides where it lands. The square-cell grid has the cells
    // to seat it alongside an AMS widget, so no heuristic disables it.
    guard.write_layout(R"({
      "anchors": [
        { "id": "printer_image",
          "placements": { "medium": { "col": 0, "row": 0, "colspan": 2, "rowspan": 2 } } }
      ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* bed = find_entry(entries, "bed_temperature");
    REQUIRE(bed);
    CHECK(bed->enabled);
}

// Regression for a cross-test isolation leak: `ams_slot_count` is a member
// subject of the AmsState *process singleton*, registered into the global XML
// scope. AMS tests (LVGLUITestFixture + AmsState::init_subjects(true)) drive
// slot discovery, setting it >0, and it never returns to 0 on its own — so it
// leaks into later tests. Tests above read this global subject via
// build_default_grid() and assume it is 0/absent; a leaked value silently swaps
// the filament widget for the AMS widget, failing only in the full
// single-process suite (passes in isolation / sharded runs). bed_temperature
// used to be gated on the same subject and is no longer, but the filament/AMS
// swap keeps the leak load-bearing.
//
// HelixTestFixture::reset_all() — run on every fixture test's ctor + dtor — must
// clear it so leakers clean up after themselves. FAILS before the reset_all fix
// (value stays >0), PASSES after.
// TEST_CASE_METHOD(HelixTestFixture) both initializes LVGL (ctor → lv_init_safe)
// and grants access to the protected static reset_all().
TEST_CASE_METHOD(HelixTestFixture, "default_layout: reset_all clears leaked ams_slot_count subject",
                 "[default_layout][regression]") {
    // Mimic an AMS test leaving the singleton's gate subject >0. If AmsState
    // already registered the real subject this finds it; otherwise stand one in.
    lv_subject_t* ams = lv_xml_get_subject(nullptr, "ams_slot_count");
    static lv_subject_t stand_in;
    if (!ams) {
        lv_subject_init_int(&stand_in, 0);
        lv_xml_register_subject(nullptr, "ams_slot_count", &stand_in);
        ams = lv_xml_get_subject(nullptr, "ams_slot_count");
    }
    REQUIRE(ams != nullptr);
    lv_subject_set_int(ams, 4); // 4 slots "discovered"
    REQUIRE(lv_subject_get_int(ams) == 4);

    reset_all();

    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "ams_slot_count")) == 0);
}

// ============================================================================
// Portrait anchor variants (#1216)
// ============================================================================
//
// default_layout.json used to key placements by BREAKPOINT ONLY. A 480x800
// portrait panel has a cramped axis of 480, i.e. breakpoint MEDIUM, so it got
// the LANDSCAPE medium anchors — authored against a 6-column grid. `tips` at
// col 2 colspan 4 and `temperature`/`bed_temperature` at col 3 simply do not
// exist on a 3-column portrait grid, so three of five anchors were unreachable
// and fell through to auto-place.
//
// Anchor tables are now keyed by layout variant in the same most-specific-first
// order LayoutManager::variant_chain() uses for ui_xml/ overrides.

namespace {

/// A layout file with a distinguishable base anchor and portrait variant.
constexpr const char* VARIANT_LAYOUT = R"({
    "anchors": [
        {
            "id": "printer_image",
            "placements": { "micro": { "col": 4, "row": 0, "colspan": 2, "rowspan": 2 } }
        },
        {
            "id": "tips",
            "placements": { "micro": { "col": 0, "row": 0, "colspan": 4, "rowspan": 2 } }
        }
    ],
    "variants": {
        "portrait": [
            {
                "id": "printer_image",
                "placements": { "micro": { "col": 0, "row": 0, "colspan": 2, "rowspan": 3 } }
            }
        ]
    }
})";

class LayoutTypeGuard {
  public:
    LayoutTypeGuard(int w, int h) {
        helix::LayoutManager::instance().init(w, h);
    }
    ~LayoutTypeGuard() {
        helix::LayoutManager::instance().init(800, 480); // back to STANDARD
    }
};

} // namespace

TEST_CASE("default_layout: portrait uses the portrait anchor variant",
          "[default_layout][portrait]") {
    TempCwdGuard guard;
    guard.write_layout(VARIANT_LAYOUT);
    LayoutTypeGuard portrait(480, 800);
    REQUIRE(helix::LayoutManager::instance().type() == helix::LayoutType::PORTRAIT);

    auto entries = PanelWidgetConfig::build_default_grid();

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->col == 0); // portrait variant, not the base anchor's col 4
    CHECK(pi->rowspan == 3);

    // tips is absent from the portrait variant, so it must NOT inherit the
    // base 4-wide anchor — a 4-column widget cannot exist on a portrait grid.
    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->col == -1);
    CHECK(tips->row == -1);
}

TEST_CASE("default_layout: landscape ignores the portrait anchor variant",
          "[default_layout][portrait]") {
    TempCwdGuard guard;
    guard.write_layout(VARIANT_LAYOUT);
    LayoutTypeGuard landscape(800, 480);
    REQUIRE(helix::LayoutManager::instance().type() == helix::LayoutType::STANDARD);

    auto entries = PanelWidgetConfig::build_default_grid();

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->col == 4); // base anchor
    CHECK(pi->rowspan == 2);

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->col == 0);
    CHECK(tips->colspan == 4);
}

TEST_CASE("default_layout: portrait sub-classes inherit the shared portrait variant",
          "[default_layout][portrait]") {
    TempCwdGuard guard;
    guard.write_layout(VARIANT_LAYOUT);
    // 320x480 is TINY_PORTRAIT, whose variant chain is {tiny_portrait, portrait}
    // — exactly like ui_xml/ overrides. With no tiny_portrait table it must fall
    // through to portrait, not to the landscape base.
    LayoutTypeGuard tiny_portrait(320, 480);
    REQUIRE(helix::LayoutManager::instance().type() == helix::LayoutType::TINY_PORTRAIT);

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->col == 0);
    CHECK(pi->rowspan == 3);
}

TEST_CASE("default_layout: portrait falls back to the base anchors when no variant exists",
          "[default_layout][portrait]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": { "micro": { "col": 1, "row": 1, "colspan": 2, "rowspan": 2 } }
            }
        ]
    })");
    LayoutTypeGuard portrait(480, 800);

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->col == 1);
    CHECK(pi->row == 1);
}

TEST_CASE("default_layout: portrait keeps tips enabled", "[default_layout][portrait]") {
    TempCwdGuard guard;
    guard.write_layout(VARIANT_LAYOUT);

    {
        LayoutTypeGuard portrait(480, 800);
        auto entries = PanelWidgetConfig::build_default_grid();
        auto* tips = find_entry(entries, "tips");
        REQUIRE(tips);
        // The square-cell grid gives every portrait tier at least 8 tracks,
        // so tips (authored 8 wide, minimum 4) fits without shrinking and
        // the landscape-only suppression is gone.
        CHECK(tips->enabled);
    }
    {
        LayoutTypeGuard landscape(800, 480);
        auto entries = PanelWidgetConfig::build_default_grid();
        auto* tips = find_entry(entries, "tips");
        REQUIRE(tips);
        CHECK(tips->enabled);
    }
}

namespace {

/// Track budget for one breakpoint, from GridLayout's square-cell sizing on the
/// measured content boxes pinned in test_grid_square_cells.cpp kMeasured. Both
/// axes matter: the square-cell model derives rows and columns independently,
/// so a table authored by scaling one axis does not automatically fit the other.
struct TrackBudget {
    int cols;
    int rows;
};

const std::map<std::string, TrackBudget> kPortraitBudget = {
    {"micro", {8, 12}},
    {"tiny", {8, 10}},
    {"small", {10, 10}},
    {"medium", {8, 12}},
    {"large", {10, 14}},
    {"xlarge", {10, 16}},
    // 1080x2400, measured: content 1056x2236 over a 192px cell is 6x12 cells.
    // Held at scale 1.0 - the UI scale factor multiplies the cell edge, so the
    // same panel quantises to 8x18 tracks at 125% and 6x14 at 158%, and the
    // shipped anchors do not fit either. That gap is real and untracked here:
    // this table is keyed by breakpoint alone, which cannot express it. See
    // the scale note above check_anchor_table().
    {"xxlarge", {12, 24}},
};

const std::map<std::string, TrackBudget> kLandscapeBudget = {
    {"micro", {12, 8}},
    {"tiny", {10, 8}},
    {"small", {10, 10}},
    {"medium", {12, 8}},
    {"large", {16, 10}},
    {"xlarge", {16, 10}},
    // 1920x1080, measured. Not 26x16: the track formula adds half a cell and
    // then truncates, so 1764px of content over a 144px cell is 12 cells.
    {"xxlarge", {24, 14}},
};

/// The two shipping ultrawide panels, measured the same way.
const std::map<std::string, TrackBudget> kUltrawideBudget = {
    {"tiny", {36, 8}},  // 1480x320
    {"small", {46, 10}} // 1920x440
};

struct AnchorRect {
    std::string id;
    int col;
    int row;
    int colspan;
    int rowspan;
};

bool anchors_overlap(const AnchorRect& a, const AnchorRect& b) {
    return a.col < b.col + b.colspan && b.col < a.col + a.colspan && a.row < b.row + b.rowspan &&
           b.row < a.row + a.rowspan;
}

/// Every anchor must fit the breakpoint on BOTH axes and must not overlap a
/// sibling. Running off either axis is not cosmetic: panel_widget_manager
/// clamps the span, pushes the origin back to fit, and the widget then collides
/// with the neighbour it was authored beside. grid.place() fails and the widget
/// falls through to auto-place at the registry span, so the anchor is silently
/// decoration and the log carries only a warning.
///
/// A breakpoint is not the whole story any more. The high-DPI UI scale factor
/// multiplies the grid's cell edge, so one panel at one breakpoint has as many
/// track counts as it has scales: 1080x2400 is xxlarge portrait at 12x24 tracks
/// unscaled, 8x18 at 125%, and 6x14 at 158%. The budgets below are the scale
/// 1.0 grids, which is what every shipping printer runs (they all sit inside
/// the DPI deadband). Anchors authored for a tier are NOT checked against that
/// tier's scaled grids, because the table cannot name one — keying the shipped
/// layout by (tier, cols, rows) is what would close that, and until then a
/// scaled panel's anchors collapse through clamp_to_grid unchecked.
void check_anchor_table(const nlohmann::json& anchors, const std::string& bp_name,
                        const TrackBudget& budget, bool require_bp) {
    std::vector<AnchorRect> placed;
    for (const auto& anchor : anchors) {
        const std::string id = anchor.value("id", std::string{});
        INFO("anchor " << id << " bp " << bp_name);
        REQUIRE(anchor.contains("placements"));
        const auto& placements = anchor["placements"];
        if (!placements.contains(bp_name)) {
            REQUIRE_FALSE(require_bp);
            continue;
        }
        const auto& p = placements[bp_name];
        const AnchorRect r{id, p.value("col", 0), p.value("row", 0), p.value("colspan", 1),
                           p.value("rowspan", 1)};
        CHECK(r.col >= 0);
        CHECK(r.row >= 0);
        CHECK(r.col + r.colspan <= budget.cols);
        CHECK(r.row + r.rowspan <= budget.rows);
        for (const auto& other : placed) {
            INFO("overlaps anchor " << other.id);
            CHECK_FALSE(anchors_overlap(r, other));
        }
        placed.push_back(r);
    }
}

/// Split a placement key into the tier it names and, when the key is
/// grid-qualified, the track grid it was authored for. "xxlarge@6x14" gives
/// {"xxlarge", 6, 14}; a bare "xxlarge" gives {"xxlarge", 0, 0}.
struct PlacementKey {
    std::string tier;
    int cols = 0;
    int rows = 0;

    bool grid_qualified() const {
        return cols > 0 && rows > 0;
    }
};

/// The one pair of widgets a table may seat on top of each other, because
/// build_default_grid() enables exactly one of them per printer.
bool mutually_exclusive(const std::string& a, const std::string& b) {
    return (a == "ams" && b == "filament") || (a == "filament" && b == "ams");
}

PlacementKey parse_placement_key(const std::string& key) {
    const auto at = key.find('@');
    if (at == std::string::npos) {
        return {key, 0, 0};
    }
    PlacementKey out{key.substr(0, at), 0, 0};
    const std::string grid = key.substr(at + 1);
    const auto x = grid.find('x');
    if (x == std::string::npos) {
        return out;
    }
    out.cols = std::atoi(grid.substr(0, x).c_str());
    out.rows = std::atoi(grid.substr(x + 1).c_str());
    return out;
}

/// Resolve through the shipped loader's own fallback chain, so a change to it
/// moves these assertions instead of leaving them green against a stale copy.
const char* resolve_key(const nlohmann::json& by_bp, int bp_idx) {
    return helix::choose_breakpoint_key(by_bp, static_cast<UiBreakpoint>(bp_idx));
}

/// Widget ids the table switches off at this tier.
std::set<std::string> disabled_at(const nlohmann::json& table, int bp_idx) {
    std::set<std::string> off;
    auto d = table.find("disabled");
    if (d == table.end() || !d->is_object()) {
        return off;
    }
    if (const char* key = resolve_key(*d, bp_idx)) {
        for (const auto& id : (*d)[key]) {
            off.insert(id.get<std::string>());
        }
    }
    return off;
}

/// Resolve one table at one tier the way the loader does, and hold it to two
/// invariants.
///
/// The first is geometric and only runs when `budget` is given: every effective
/// placement fits both axes and overlaps no sibling.
///
/// The second is the one arithmetic on a single tier cannot see. A tier may
/// author no placements at all and let every widget inherit from a wider key —
/// that is how ultrawide micro works. What it must never do is author
/// placements for SOME widgets while others silently inherit a wider key's
/// coordinates, because those coordinates were written for a wider grid: the
/// widget lands off the edge, gets slid back into a collision, and is evicted
/// with a "grid full" toast, taking whatever would have auto-placed with it.
/// Shipped twice during this rework — temp_graph at micro, then macros and
/// active_spool at ultrawide tiny.
void check_table_at_tier(const nlohmann::json& table, int bp_idx, const std::string& bp_name,
                         const TrackBudget* budget) {
    const auto& anchors = table.contains("anchors") ? table["anchors"] : table;
    const std::set<std::string> off = disabled_at(table, bp_idx);

    std::vector<AnchorRect> placed;
    std::set<std::string> keys_used;
    for (const auto& anchor : anchors) {
        const std::string id = anchor.value("id", std::string{});
        if (off.count(id)) {
            continue;
        }
        INFO("anchor " << id << " bp " << bp_name);
        REQUIRE(anchor.contains("placements"));
        const char* key = resolve_key(anchor["placements"], bp_idx);
        if (!key) {
            continue;
        }
        keys_used.insert(key);
        if (!budget) {
            continue;
        }
        const auto& p = anchor["placements"][key];
        const AnchorRect r{id, p.value("col", 0), p.value("row", 0), p.value("colspan", 1),
                           p.value("rowspan", 1)};
        CHECK(r.col >= 0);
        CHECK(r.row >= 0);
        CHECK(r.col + r.colspan <= budget->cols);
        CHECK(r.row + r.rowspan <= budget->rows);
        for (const auto& other : placed) {
            INFO("overlaps anchor " << other.id);
            CHECK_FALSE(anchors_overlap(r, other));
        }
        placed.push_back(r);
    }

    std::string joined;
    for (const auto& k : keys_used) {
        joined += (joined.empty() ? "" : ", ") + k;
    }
    INFO("tier " << bp_name << " resolves placements from key(s): " << joined);
    CHECK(keys_used.size() <= 1);
}

const char* kBpNames[] = {"micro", "tiny", "small", "medium", "large", "xlarge", "xxlarge"};

} // namespace

// The shipped table itself, not a synthetic one: every portrait anchor has to
// fit the narrowest grid its breakpoint can produce, or it silently falls
// through to auto-place and the anchor is decoration.
TEST_CASE("default_layout: the shipped portrait anchors fit a portrait grid",
          "[default_layout][portrait][shipped]") {
    std::string path = helix::find_readable("default_layout.json");
    std::ifstream in(path);
    REQUIRE(in.is_open());
    nlohmann::json layout = nlohmann::json::parse(in);

    REQUIRE(layout.contains("variants"));
    REQUIRE(layout["variants"].contains("portrait"));
    // A variant is either a bare anchor array or an object carrying "anchors"
    // plus optional "disabled". The loader accepts both, so this does too.
    const auto& portrait_node = layout["variants"]["portrait"];
    const nlohmann::json& portrait =
        portrait_node.is_object() ? portrait_node.at("anchors") : portrait_node;
    REQUIRE(portrait.is_array());
    REQUIRE_FALSE(portrait.empty());

    // Every anchor names a real widget, and every tier it mentions is one the
    // budget table knows — a typo'd tier would otherwise go unchecked.
    for (const auto& anchor : portrait) {
        const std::string id = anchor.value("id", std::string{});
        INFO("anchor " << id);
        const auto* def = helix::find_widget_def(id);
        REQUIRE(def != nullptr);
        REQUIRE(anchor.contains("placements"));
        for (auto it = anchor["placements"].begin(); it != anchor["placements"].end(); ++it) {
            INFO("anchor " << id << " breakpoint " << it.key());
            CHECK(kPortraitBudget.count(parse_placement_key(it.key()).tier) == 1);
            // A span past the widget's registry maximum is not a layout the grid
            // can honour — grid_edit_mode would clamp it the moment the user
            // touched it, and nothing else checks the shipped table against the
            // registry. A print_status colspan of 10 against a max of 8 reached a
            // draft of this table by exactly this gap.
            CHECK(it.value().value("colspan", def->colspan) <= def->effective_max_colspan());
            CHECK(it.value().value("rowspan", def->rowspan) <= def->effective_max_rowspan());
        }
    }

    for (const auto& [bp_name, budget] : kPortraitBudget) {
        check_anchor_table(portrait, bp_name, budget, /*require_bp=*/false);
    }
}

// A variant's "disabled" list is the only way to say "not on this tier". Leaving
// a widget out of the anchors does NOT switch it off: parse_widget_array()
// appends every registry widget that is absent, at its default_enabled, and the
// placement engine then seats it wherever it fits.
TEST_CASE("default_layout: a variant disables a widget per breakpoint",
          "[default_layout][portrait]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [],
        "variants": {
            "portrait": {
                "anchors": [
                    { "id": "printer_image",
                      "placements": { "tiny": { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 } } }
                ],
                "disabled": { "tiny": ["tips"] }
            }
        }
    })");

    LayoutTypeGuard portrait(480, 800);
    auto entries = PanelWidgetConfig::build_default_grid();

    // tips is default_enabled in the registry, so only the disabled list can
    // switch it off — and it must be unplaced, not merely hidden in place.
    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK_FALSE(tips->enabled);
    CHECK(tips->col == -1);
    CHECK(tips->row == -1);

    // Everything else the variant did not name is untouched.
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->enabled);
}

// The legacy bare-array variant shape must keep working — shipped copies of
// default_layout.json predate the object form and the file is runtime-editable.
TEST_CASE("default_layout: a variant accepts both the array and object shapes",
          "[default_layout][portrait]") {
    const char* as_array = R"({
        "anchors": [],
        "variants": { "portrait": [
            { "id": "printer_image",
              "placements": { "tiny": { "col": 1, "row": 2, "colspan": 4, "rowspan": 4 } } }
        ] }
    })";
    const char* as_object = R"({
        "anchors": [],
        "variants": { "portrait": { "anchors": [
            { "id": "printer_image",
              "placements": { "tiny": { "col": 1, "row": 2, "colspan": 4, "rowspan": 4 } } }
        ] } }
    })";

    for (const char* doc : {as_array, as_object}) {
        TempCwdGuard guard;
        guard.write_layout(doc);
        LayoutTypeGuard portrait(480, 800);
        auto entries = PanelWidgetConfig::build_default_grid();
        auto* pi = find_entry(entries, "printer_image");
        REQUIRE(pi);
        CHECK(pi->col == 1);
        CHECK(pi->row == 2);
        CHECK(pi->colspan == 4);
    }
}

// An anchor may carry per-widget config, which is how portrait ships
// print_status in its Detailed layout without a C++ branch per widget.
TEST_CASE("default_layout: an anchor carries per-widget config", "[default_layout][portrait]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [],
        "variants": { "portrait": { "anchors": [
            { "id": "print_status",
              "config": { "layout_style": "detailed" },
              "placements": { "tiny": { "col": 0, "row": 0, "colspan": 8, "rowspan": 4 } } }
        ] } }
    })");

    LayoutTypeGuard portrait(480, 800);
    auto entries = PanelWidgetConfig::build_default_grid();
    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    REQUIRE(ps->config.is_object());
    CHECK(ps->config.value("layout_style", std::string{}) == "detailed");
}

// The shipped portrait table must actually ship Detailed — the whole reason the
// anchor config plumbing exists. Library clips its last action row at every
// measured geometry.
TEST_CASE("default_layout: the shipped portrait print_status is Detailed",
          "[default_layout][portrait][shipped]") {
    std::string path = helix::find_readable("default_layout.json");
    std::ifstream in(path);
    REQUIRE(in.is_open());
    nlohmann::json layout = nlohmann::json::parse(in);

    const auto& node = layout["variants"]["portrait"];
    const nlohmann::json& anchors = node.is_object() ? node.at("anchors") : node;
    bool seen = false;
    for (const auto& a : anchors) {
        if (a.value("id", std::string{}) != "print_status") {
            continue;
        }
        seen = true;
        REQUIRE(a.contains("config"));
        CHECK(a["config"].value("layout_style", std::string{}) == "detailed");
    }
    CHECK(seen);
}

// The shipped landscape anchors must fit each breakpoint's grid on both axes
// without overlapping. Track counts are measured from the real content box per
// geometry — see test_grid_square_cells.cpp kMeasured. micro/tiny/small/medium
// are all distinct and must each have their own key.
//
// Note: this does NOT assert that the anchors tile the grid edge to edge. They
// currently do not (micro reaches 8 of 12 columns), and whether the shipped
// tables should fill the width is an open layout question, not a correctness
// one — see the ledger's default-layout discussion item.
TEST_CASE("default_layout: the shipped landscape anchors fit their grid",
          "[default_layout][shipped]") {
    std::string path = helix::find_readable("default_layout.json");
    std::ifstream in(path);
    REQUIRE(in.is_open());
    nlohmann::json layout = nlohmann::json::parse(in);
    REQUIRE(layout.contains("anchors"));

    for (int i = 0; i < 7; i++) {
        auto it = kLandscapeBudget.find(kBpNames[i]);
        check_table_at_tier(layout, i, kBpNames[i],
                            it == kLandscapeBudget.end() ? nullptr : &it->second);
    }
}

TEST_CASE("default_layout: the shipped ultrawide anchors fit their grid",
          "[default_layout][shipped][ultrawide]") {
    std::string path = helix::find_readable("default_layout.json");
    std::ifstream in(path);
    REQUIRE(in.is_open());
    nlohmann::json layout = nlohmann::json::parse(in);
    REQUIRE(layout.contains("variants"));
    REQUIRE(layout["variants"].contains("ultrawide"));

    for (int i = 0; i < 7; i++) {
        auto it = kUltrawideBudget.find(kBpNames[i]);
        check_table_at_tier(layout["variants"]["ultrawide"], i, kBpNames[i],
                            it == kUltrawideBudget.end() ? nullptr : &it->second);
    }
}

// The invariant that pure per-tier arithmetic cannot see. Every shipped table,
// every tier: a widget either resolves to the tier's own key along with all its
// siblings, or the whole tier inherits, or it is explicitly disabled. Mixing
// them hands one widget coordinates authored for a wider grid.
TEST_CASE("default_layout: no shipped table mixes authored and inherited placements",
          "[default_layout][shipped]") {
    std::string path = helix::find_readable("default_layout.json");
    std::ifstream in(path);
    REQUIRE(in.is_open());
    nlohmann::json layout = nlohmann::json::parse(in);

    for (int i = 0; i < 7; i++) {
        INFO("base (landscape) table");
        check_table_at_tier(layout, i, kBpNames[i], nullptr);
    }
    REQUIRE(layout.contains("variants"));
    for (auto v = layout["variants"].begin(); v != layout["variants"].end(); ++v) {
        if (!v->is_object() || !v->contains("anchors")) {
            continue; // "_comment", or a bare-array variant
        }
        for (int i = 0; i < 7; i++) {
            INFO("variant " << v.key());
            check_table_at_tier(*v, i, kBpNames[i], nullptr);
        }
    }
}

// ============================================================================
// Grid-qualified placements
// ============================================================================
//
// A breakpoint names a panel, not a grid. The high-DPI UI scale multiplies the
// cell edge, so one panel at one tier has a different track count per scale --
// 1080x2400 is xxlarge portrait at 12x24 tracks unscaled, 8x18 at 125%, 6x14 at
// 158%. Anchors authored against one of those collapse through clamp_to_grid on
// the others. A placement may therefore name the grid it was authored for.

TEST_CASE("default_layout: a grid-qualified placement wins over the bare tier key",
          "[default_layout][grid_key]") {
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny":      { "col": 0, "row": 0, "colspan": 8, "rowspan": 2 },
                    "tiny@6x14": { "col": 2, "row": 4, "colspan": 4, "rowspan": 2 }
                }
            }
        ]
    })");

    // Same tier, same file — only the measured grid differs.
    auto on_6x14 = PanelWidgetConfig::build_default_grid(6, 14);
    auto* qualified = find_entry(on_6x14, "printer_image");
    REQUIRE(qualified);
    CHECK(qualified->col == 2);
    CHECK(qualified->row == 4);
    CHECK(qualified->colspan == 4);

    // A grid with no entry of its own falls back to the bare tier key.
    auto on_12x24 = PanelWidgetConfig::build_default_grid(12, 24);
    auto* bare = find_entry(on_12x24, "printer_image");
    REQUIRE(bare);
    CHECK(bare->col == 0);
    CHECK(bare->row == 0);
    CHECK(bare->colspan == 8);
}

TEST_CASE("default_layout: an unknown grid leaves tier-keyed behaviour untouched",
          "[default_layout][grid_key]") {
    // Every caller that cannot measure a grid — config load, and every existing
    // test — must get exactly what it got before grids entered the key.
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny":      { "col": 0, "row": 0, "colspan": 8, "rowspan": 2 },
                    "tiny@6x14": { "col": 2, "row": 4, "colspan": 4, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);
    CHECK(pi->colspan == 8);
}

TEST_CASE("default_layout: an anchor that does not fit the measured grid is not honoured",
          "[default_layout][grid_key]") {
    // The failure this exists to stop. clamp_to_grid() would shove the origin
    // back so the span fits, landing the widget on top of a neighbour and
    // reading as a designed position. Dropping it to auto-place says what
    // actually happened: this anchor does not describe this grid.
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "printer_image",
                "placements": {
                    "tiny": { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 }
                }
            },
            {
                "id": "print_status",
                "placements": {
                    "tiny": { "col": 8, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            },
            {
                "id": "tips",
                "placements": {
                    "tiny": { "col": 0, "row": 4, "colspan": 10, "rowspan": 2 }
                }
            }
        ]
    })");

    // A 6x14 grid holds printer_image (0+4 <= 6, 0+4 <= 14) but not
    // print_status (col 8 is off the grid) nor tips (colspan 10 > 6).
    auto entries = PanelWidgetConfig::build_default_grid(6, 14);

    auto* pi = find_entry(entries, "printer_image");
    REQUIRE(pi);
    CHECK(pi->col == 0);
    CHECK(pi->row == 0);

    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->col == -1);
    CHECK(ps->row == -1);

    auto* tips = find_entry(entries, "tips");
    REQUIRE(tips);
    CHECK(tips->col == -1);
    CHECK(tips->row == -1);

    // Dropped anchors keep their registry span, the same answer an entry that
    // never had a position gets — not the span the rejected anchor asked for.
    const auto* tips_def = helix::find_widget_def("tips");
    REQUIRE(tips_def);
    CHECK(tips->colspan == tips_def->colspan);
}

TEST_CASE("default_layout: the same anchor fits a grid that is big enough",
          "[default_layout][grid_key]") {
    // Guards the test above against passing for the wrong reason: if the
    // fitting check were simply rejecting everything, this would fail too.
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            {
                "id": "print_status",
                "placements": {
                    "tiny": { "col": 8, "row": 0, "colspan": 2, "rowspan": 2 }
                }
            }
        ]
    })");

    auto entries = PanelWidgetConfig::build_default_grid(12, 24);
    auto* ps = find_entry(entries, "print_status");
    REQUIRE(ps);
    CHECK(ps->col == 8);
    CHECK(ps->row == 0);
}

TEST_CASE("default_layout: every grid-qualified table fits the grid it names",
          "[default_layout][shipped][grid_key]") {
    // A grid-qualified key states the track grid it was authored for, so it
    // checks against that rather than against a tier budget — no assumption
    // about which panel or scale produces the grid, and none needed. This is
    // the check the tier-keyed budgets cannot make: they hold one grid per
    // tier, and the UI scale gives a tier as many grids as it has scales.
    std::string path = helix::find_readable("default_layout.json");
    std::ifstream in(path);
    REQUIRE(in.is_open());
    nlohmann::json layout = nlohmann::json::parse(in);

    std::vector<std::pair<std::string, const nlohmann::json*>> tables;
    tables.emplace_back("base", &layout);
    if (layout.contains("variants")) {
        for (auto v = layout["variants"].begin(); v != layout["variants"].end(); ++v) {
            if (v->is_object() && v->contains("anchors")) {
                tables.emplace_back(v.key(), &*v);
            }
        }
    }

    int checked = 0;
    for (const auto& [table_name, table] : tables) {
        // Collect every grid-qualified key this table uses, then check each
        // key's anchors as the one layout they are: all of them together, not
        // one anchor at a time. Overlap is a property of the set.
        std::set<std::string> qualified;
        for (const auto& anchor : (*table)["anchors"]) {
            if (!anchor.contains("placements")) {
                continue;
            }
            for (auto it = anchor["placements"].begin(); it != anchor["placements"].end(); ++it) {
                if (parse_placement_key(it.key()).grid_qualified()) {
                    qualified.insert(it.key());
                }
            }
        }

        for (const auto& key : qualified) {
            const PlacementKey pk = parse_placement_key(key);
            INFO("table " << table_name << " key " << key);

            std::vector<AnchorRect> placed;
            for (const auto& anchor : (*table)["anchors"]) {
                const std::string id = anchor.value("id", std::string{});
                if (!anchor.contains("placements") || !anchor["placements"].contains(key)) {
                    continue;
                }
                const auto* def = helix::find_widget_def(id);
                INFO("anchor " << id);
                REQUIRE(def != nullptr);

                const auto& p = anchor["placements"][key];
                const AnchorRect r{id, p.value("col", 0), p.value("row", 0),
                                   p.value("colspan", def->colspan),
                                   p.value("rowspan", def->rowspan)};

                // Inside the grid the key names. An anchor that fails this is
                // exactly what build_default_grid() now drops to auto-place, so
                // shipping one would be shipping a decoration.
                CHECK(r.col >= 0);
                CHECK(r.row >= 0);
                CHECK(r.col + r.colspan <= pk.cols);
                CHECK(r.row + r.rowspan <= pk.rows);

                // Within what the widget can actually be stretched to. A span
                // past the registry maximum is one grid_edit_mode would refuse
                // to give back the moment the user touched it.
                CHECK(r.colspan <= def->effective_max_colspan());
                CHECK(r.rowspan <= def->effective_max_rowspan());

                for (const auto& other : placed) {
                    // ams and filament are mutually exclusive: the swap at the
                    // end of build_default_grid() enables exactly one, on the
                    // hardware it found. Sharing a slot is how a table gives
                    // both the same spot without spending it twice, so an
                    // overlap between those two is the intent, not a clash.
                    if (mutually_exclusive(r.id, other.id)) {
                        continue;
                    }
                    INFO("overlaps anchor " << other.id);
                    CHECK_FALSE(anchors_overlap(r, other));
                }
                placed.push_back(r);
                ++checked;
            }
        }
    }

    // Guards the loop against passing by finding nothing: the shipped file
    // carries at least one grid-qualified table.
    CHECK(checked > 0);
}
