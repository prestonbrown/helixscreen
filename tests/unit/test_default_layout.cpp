// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/measured_grids.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/scoped_int_subject.h"
#include "ams_state.h"
#include "data_root_resolver.h"
#include "default_layout_resolver.h"
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
#include <functional>
#include <map>
#include <optional>
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

// The runtime breakpoint is Micro (index 0), whose placement key is "tiny", so
// the JSON below places under "tiny" and adds other tiers only to prove they are
// not the ones read.
TEST_CASE("default_layout: the layout file's anchors, and the fallback when it has none",
          "[default_layout]") {
    struct Expect {
        const char* id;
        bool placed;
        int col = -1, row = -1;
        int colspan = 0, rowspan = 0; // 0: not checked; -1: the registry's span
    };
    struct Row {
        const char* what;
        const char* json; // nullptr: no file at all
        std::vector<Expect> expect;
        std::vector<const char*> absent;
        bool only_expected_placed = false;
    };
    // The built-in anchors every unusable file falls back to.
    const std::vector<Expect> fallback = {{"printer_image", true, 0, 0, 4, 4},
                                          {"print_status", true, 0, 4, 4, 4},
                                          {"tips", true, 4, 0, 4, 4}};
    const Row rows[] = {
        {"valid tiny placements",
         R"({"anchors": [
               {"id": "printer_image", "placements": {"tiny": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}},
               {"id": "print_status", "placements": {"tiny": {"col": 0, "row": 2, "colspan": 2, "rowspan": 2}}},
               {"id": "tips", "placements": {"tiny": {"col": 2, "row": 0, "colspan": 4, "rowspan": 2}}}]})",
         {{"printer_image", true, 0, 0, 2, 2},
          {"print_status", true, 0, 2, 2, 2},
          {"tips", true, 2, 0, 4, 2}}},
        {"only the runtime tier's placement is read",
         R"({"anchors": [
               {"id": "printer_image", "placements": {
                   "tiny": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2},
                   "large": {"col": 0, "row": 0, "colspan": 3, "rowspan": 3}}},
               {"id": "tips", "placements": {
                   "tiny": {"col": 2, "row": 0, "colspan": 2, "rowspan": 2},
                   "large": {"col": 3, "row": 0, "colspan": 5, "rowspan": 2}}}]})",
         {{"printer_image", true, 0, 0, 2, 2}, {"tips", true, 2, 0, 2, 2}}},
        {"no file", nullptr, fallback},
        {"malformed JSON", "{ this is not valid json }}}}", fallback},
        {"an empty anchors array", R"({ "anchors": [] })", fallback},
        {"no anchors key", R"({ "something_else": true })", fallback},
        {"no anchor has the runtime tier",
         R"({"anchors": [{"id": "printer_image", "placements": {"large": {"col": 0, "row": 0, "colspan": 3, "rowspan": 3}}}]})",
         fallback},
        {"one anchor with the runtime tier is enough: the other is auto-placed",
         R"({"anchors": [
               {"id": "printer_image", "placements": {"tiny": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}},
               {"id": "tips", "placements": {"large": {"col": 3, "row": 0, "colspan": 5, "rowspan": 2}}}]})",
         {{"printer_image", true, 0, 0, 2, 2}, {"tips", false}}},
        {"an unknown id is ignored",
         R"({"anchors": [
               {"id": "totally_bogus_widget", "placements": {"tiny": {"col": 0, "row": 0, "colspan": 1, "rowspan": 1}}},
               {"id": "printer_image", "placements": {"tiny": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}}]})",
         {{"printer_image", true, 0, 0, 2, 2}},
         {"totally_bogus_widget"}},
        {"an empty id is skipped",
         R"({"anchors": [
               {"id": "", "placements": {"tiny": {"col": 0, "row": 0, "colspan": 1, "rowspan": 1}}},
               {"id": "printer_image", "placements": {"tiny": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}}]})",
         {{"printer_image", true, 0, 0, 2, 2}}},
        {"widgets the file does not anchor are auto-placed",
         R"({"anchors": [{"id": "printer_image", "placements": {"tiny": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}}]})",
         {{"printer_image", true, 0, 0, 2, 2}},
         {},
         true},
        // An omitted span takes the registry span rather than one track: one track
        // is a quarter of the area the widget declares as its minimum.
        {"omitted fields: row 0 and the registry span",
         R"({"anchors": [{"id": "printer_image", "placements": {"tiny": {"col": 1}}}]})",
         {{"printer_image", true, 1, 0, -1, -1}}},
        {"file positions override the built-in ones",
         R"({"anchors": [{"id": "printer_image", "placements": {"tiny": {"col": 5, "row": 3, "colspan": 1, "rowspan": 1}}}]})",
         {{"printer_image", true, 5, 3, 1, 1}}},
    };

    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.what) {
            TempCwdGuard guard;
            if (row.json != nullptr) {
                guard.write_layout(row.json);
            }
            const auto entries = PanelWidgetConfig::build_default_grid();

            // Every registry widget appears once, and some are on.
            REQUIRE(entries.size() == grid_widget_count());
            std::set<std::string> ids;
            for (const auto& e : entries) {
                ids.insert(e.id);
            }
            for (const auto& def : get_all_widget_defs()) {
                if (!def.multi_instance) {
                    CHECK(ids.count(def.id) == 1);
                }
            }
            CHECK(std::any_of(entries.begin(), entries.end(),
                              [](const PanelWidgetEntry& e) { return e.enabled; }));

            for (const auto& x : row.expect) {
                CAPTURE(x.id);
                const auto* e = find_entry(entries, x.id);
                REQUIRE(e != nullptr);
                CHECK(e->has_grid_position() == x.placed);
                if (!x.placed) {
                    continue;
                }
                CHECK(e->enabled);
                CHECK(e->col == x.col);
                CHECK(e->row == x.row);
                const auto* def = find_widget_def(x.id);
                REQUIRE(def != nullptr);
                if (x.colspan != 0) {
                    CHECK(e->colspan == (x.colspan < 0 ? def->colspan : x.colspan));
                    CHECK(e->rowspan == (x.rowspan < 0 ? def->rowspan : x.rowspan));
                }
            }
            for (const char* id : row.absent) {
                CHECK(find_entry(entries, id) == nullptr);
            }
            if (row.only_expected_placed) {
                for (const auto& e : entries) {
                    const bool listed = std::any_of(row.expect.begin(), row.expect.end(),
                                                    [&e](const Expect& x) { return e.id == x.id; });
                    if (!listed) {
                        CAPTURE(e.id);
                        CHECK(e.col == -1);
                        CHECK(e.row == -1);
                    }
                }
            }
        }
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
        // AmsState's own subject, not a private one published under its name: a
        // panel that attaches to the name carries AmsState's lifetime token, so
        // an observer left on a foreign subject outlives the context it points to.
        helix::AmsState::instance().init_subjects(true);
        subject_ = lv_xml_get_subject(nullptr, "ams_slot_count");
        REQUIRE(subject_ != nullptr);
        lv_subject_set_int(subject_, slot_count);
    }

    void set(int val) {
        lv_subject_set_int(subject_, val);
    }

    ~AmsSubjectGuard() {
        // Reset to 0 so subsequent tests see "no AMS" by default
        lv_subject_set_int(subject_, 0);
        // Observers of the subject defer their applies; run them while it is alive.
        helix::ui::UpdateQueue::instance().drain();
    }

    AmsSubjectGuard(const AmsSubjectGuard&) = delete;
    AmsSubjectGuard& operator=(const AmsSubjectGuard&) = delete;

  private:
    lv_subject_t* subject_ = nullptr;
};

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

TEST_CASE("default_layout: bed_temperature is on and last at every tier, with or without an AMS",
          "[default_layout][regression]") {
    struct Row {
        const char* what;
        UiBreakpoint bp;
        int ams_slots; // 0: no AMS subject set
        const char* json;
        int col, row; // -1: not anchored
    };
    // An anchor states where its tier puts the widget, so it answers the "is
    // there room" question an AMS heuristic would guess at; unanchored, the
    // square-cell grid has the cells to seat it beside an AMS widget.
    const Row rows[] = {
        {"micro, no AMS", UiBreakpoint::Micro, 0, R"({ "anchors": [] })", -1, -1},
        {"small, no AMS", UiBreakpoint::Small, 0, R"({ "anchors": [] })", -1, -1},
        {"medium, no AMS", UiBreakpoint::Medium, 0, R"({ "anchors": [] })", -1, -1},
        {"large, AMS", UiBreakpoint::Large, 4, R"({ "anchors": [] })", -1, -1},
        {"xlarge, AMS", UiBreakpoint::XLarge, 4, R"({ "anchors": [] })", -1, -1},
        {"medium, AMS, anchored", UiBreakpoint::Medium, 4,
         R"({"anchors": [{"id": "bed_temperature", "placements": {"medium": {"col": 3, "row": 3, "colspan": 1, "rowspan": 1}}}]})",
         3, 3},
        {"medium, AMS, unanchored", UiBreakpoint::Medium, 4,
         R"({"anchors": [{"id": "printer_image", "placements": {"medium": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}}]})",
         -1, -1},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.what) {
            TempCwdGuard guard;
            BreakpointGuard bp(row.bp);
            std::optional<AmsSubjectGuard> ams;
            if (row.ams_slots > 0) {
                ams.emplace(row.ams_slots);
            }
            guard.write_layout(row.json);

            const auto entries = PanelWidgetConfig::build_default_grid();
            const auto* bed = find_entry(entries, "bed_temperature");
            REQUIRE(bed != nullptr);
            CHECK(bed->enabled);
            if (row.col >= 0) {
                CHECK(bed->col == row.col);
                CHECK(bed->row == row.row);
            } else {
                CHECK(entries.back().id == "bed_temperature");
            }
        }
    }
}

// Regression for a cross-test isolation leak: `ams_slot_count` is a member
// subject of the AmsState *process singleton*, registered into the global XML
// scope. AMS tests (LVGLUITestFixture + AmsState::init_subjects(true)) drive
// slot discovery, setting it >0, and it never returns to 0 on its own — so it
// leaks into later tests. Tests above read this global subject via
// build_default_grid() and assume it is 0/absent; a leaked value silently fills
// the spool slot with the AMS widget instead of the filament widget, failing
// only in the full single-process suite (passes in isolation / sharded runs).
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
// A 480x800 portrait panel has a cramped axis of 480, the same MEDIUM
// breakpoint as an 800x480 landscape one, so a table keyed by breakpoint alone
// would hand it anchors authored for a landscape grid. Anchor tables are keyed
// by layout variant in the same most-specific-first order
// LayoutManager::variant_chain() uses for ui_xml/ overrides.

namespace {

/// A layout file with a distinguishable base anchor and portrait variant.
constexpr const char* kVariantLayout = R"({
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

TEST_CASE("default_layout: the anchor table follows the layout variant chain",
          "[default_layout][portrait]") {
    constexpr const char* kBaseOnly = R"({"anchors": [
        {"id": "printer_image", "placements": {"micro": {"col": 1, "row": 1, "colspan": 2, "rowspan": 2}}}]})";
    struct Expect {
        const char* id;
        int col, row, colspan, rowspan; // -2: not checked
    };
    struct Row {
        const char* what;
        int w, h;
        helix::LayoutType type;
        const char* json;
        std::vector<Expect> expect;
    };
    const Row rows[] = {
        // tips is absent from the portrait variant, so it must not inherit the
        // base 4-wide anchor: a 4-column widget cannot exist on a portrait grid.
        {"portrait uses the portrait variant",
         480,
         800,
         helix::LayoutType::PORTRAIT,
         kVariantLayout,
         {{"printer_image", 0, -2, -2, 3}, {"tips", -1, -1, -2, -2}}},
        {"landscape ignores it",
         800,
         480,
         helix::LayoutType::STANDARD,
         kVariantLayout,
         {{"printer_image", 4, -2, -2, 2}, {"tips", 0, -2, 4, -2}}},
        // TINY_PORTRAIT's chain is {tiny_portrait, portrait}, as for ui_xml/
        // overrides: with no tiny_portrait table it reaches portrait, not the base.
        {"a portrait sub-class inherits the shared portrait variant",
         320,
         480,
         helix::LayoutType::TINY_PORTRAIT,
         kVariantLayout,
         {{"printer_image", 0, -2, -2, 3}}},
        {"portrait with no variant uses the base anchors",
         480,
         800,
         helix::LayoutType::PORTRAIT,
         kBaseOnly,
         {{"printer_image", 1, 1, -2, -2}}},
    };
    const auto check = [](int got, int want) {
        if (want != -2) {
            CHECK(got == want);
        }
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.what) {
            TempCwdGuard guard;
            guard.write_layout(row.json);
            LayoutTypeGuard layout(row.w, row.h);
            REQUIRE(helix::LayoutManager::instance().type() == row.type);

            const auto entries = PanelWidgetConfig::build_default_grid();
            for (const auto& x : row.expect) {
                CAPTURE(x.id);
                const auto* e = find_entry(entries, x.id);
                REQUIRE(e != nullptr);
                check(e->col, x.col);
                check(e->row, x.row);
                check(e->colspan, x.colspan);
                check(e->rowspan, x.rowspan);
            }
            // Every portrait tier has at least 8 tracks, so tips (authored 8 wide,
            // minimum 4) stays on in both orientations.
            if (row.json == kVariantLayout) {
                const auto* tips = find_entry(entries, "tips");
                REQUIRE(tips != nullptr);
                CHECK(tips->enabled);
            }
        }
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

/// The table the app ships, as the app finds it.
nlohmann::json load_shipped_layout() {
    std::ifstream in(helix::find_readable("default_layout.json"));
    REQUIRE(in.is_open());
    return nlohmann::json::parse(in);
}

const std::map<std::string, TrackBudget> kPortraitBudget = {
    {"micro", {8, 12}},
    {"tiny", {8, 10}},
    {"small", {10, 10}},
    {"medium", {8, 12}},
    {"large", {10, 14}},
    {"xlarge", {10, 16}},
    // 1080x1920, measured: content 1056x1756 over the 144px cell of the 72px
    // track edge is 7x12 cells. 1080x2400 measures 14x32, the same width with
    // more rows. Held at scale 1.0 - the UI scale factor multiplies the cell
    // edge, so the same panels quantise to 12 tracks wide at 125%, which this
    // breakpoint-keyed table cannot express. See the scale note above
    // check_anchor_table().
    {"xxlarge", {14, 24}},
};

const std::map<std::string, TrackBudget> kLandscapeBudget = {
    {"micro", {12, 8}},
    {"tiny", {10, 8}},
    {"small", {10, 10}},
    {"medium", {12, 8}},
    {"large", {16, 10}},
    {"xlarge", {16, 10}},
    // 1920x1080, measured: 1720x1056 of content over the 72px track edge is 12x7 cells.
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
/// track counts as it has scales: 1080x2400 is xxlarge portrait at 14x32 tracks
/// unscaled, 12x24 at 125%, and 10x20 at 158%. The budgets below are the scale
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
        // A slot's region is checked like an anchor's rectangle: whatever fills
        // it stays inside it.
        const std::string id = anchor.contains("slot")
                                   ? "slot:" + anchor.value("slot", std::string{})
                                   : anchor.value("id", std::string{});
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
    nlohmann::json layout = load_shipped_layout();

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
        if (anchor.contains("slot")) {
            continue; // a slot's region and cells are checked by the slot cases below
        }
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
TEST_CASE("default_layout: a variant accepts both the array and object shapes, and anchor config",
          "[default_layout][portrait]") {
    // An anchor may carry per-widget config (how portrait ships print_status in
    // its Detailed layout without a C++ branch per widget).
    const char* as_array = R"({
        "anchors": [],
        "variants": { "portrait": [
            { "id": "printer_image",
              "placements": { "tiny": { "col": 1, "row": 2, "colspan": 4, "rowspan": 4 } } },
            { "id": "print_status", "config": { "layout_style": "detailed" },
              "placements": { "tiny": { "col": 0, "row": 6, "colspan": 8, "rowspan": 4 } } }
        ] }
    })";
    const char* as_object = R"({
        "anchors": [],
        "variants": { "portrait": { "anchors": [
            { "id": "printer_image",
              "placements": { "tiny": { "col": 1, "row": 2, "colspan": 4, "rowspan": 4 } } },
            { "id": "print_status", "config": { "layout_style": "detailed" },
              "placements": { "tiny": { "col": 0, "row": 6, "colspan": 8, "rowspan": 4 } } }
        ] } }
    })";

    for (const char* doc : {as_array, as_object}) {
        CAPTURE(doc);
        TempCwdGuard guard;
        guard.write_layout(doc);
        LayoutTypeGuard portrait(480, 800);
        const auto entries = PanelWidgetConfig::build_default_grid();
        const auto* pi = find_entry(entries, "printer_image");
        REQUIRE(pi);
        CHECK(pi->col == 1);
        CHECK(pi->row == 2);
        CHECK(pi->colspan == 4);
        const auto* ps = find_entry(entries, "print_status");
        REQUIRE(ps);
        REQUIRE(ps->config.is_object());
        CHECK(ps->config.value("layout_style", std::string{}) == "detailed");
    }
}

// An anchor may carry per-widget config, which is how portrait ships
// print_status in its Detailed layout without a C++ branch per widget.
// The shipped portrait table must actually ship Detailed — the whole reason the
// anchor config plumbing exists. Library clips its last action row at every
// measured geometry.
TEST_CASE("default_layout: the shipped portrait print_status is Detailed",
          "[default_layout][portrait][shipped]") {
    nlohmann::json layout = load_shipped_layout();

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
TEST_CASE("default_layout: the shipped landscape and ultrawide anchors fit their grids",
          "[default_layout][shipped][ultrawide]") {
    nlohmann::json layout = load_shipped_layout();
    REQUIRE(layout.contains("anchors"));
    REQUIRE(layout.contains("variants"));
    REQUIRE(layout["variants"].contains("ultrawide"));

    const std::pair<const nlohmann::json*, const std::map<std::string, TrackBudget>*> tables[] = {
        {&layout, &kLandscapeBudget}, {&layout["variants"]["ultrawide"], &kUltrawideBudget}};
    for (const auto& [table, budgets] : tables) {
        for (int i = 0; i < 7; i++) {
            auto it = budgets->find(kBpNames[i]);
            check_table_at_tier(*table, i, kBpNames[i],
                                it == budgets->end() ? nullptr : &it->second);
        }
    }
}

// The invariant that pure per-tier arithmetic cannot see. Every shipped table,
// every tier: a widget either resolves to the tier's own key along with all its
// siblings, or the whole tier inherits, or it is explicitly disabled. Mixing
// them hands one widget coordinates authored for a wider grid.
TEST_CASE("default_layout: no shipped table mixes authored and inherited placements",
          "[default_layout][shipped]") {
    nlohmann::json layout = load_shipped_layout();

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
// 1080x2400 is xxlarge portrait at 14x32 tracks unscaled, 12x24 at 125%, 10x20 at
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

    // Same tier, same file: only the measured grid differs. A grid with no entry
    // of its own, or no measured grid at all (config load), reads the bare key.
    struct Row {
        const char* what;
        int cols, rows; // 0: no measured grid
        int col, row, colspan;
    };
    const Row rows[] = {
        {"the grid the key names", 6, 14, 2, 4, 4},
        {"another grid", 12, 24, 0, 0, 8},
        {"no measured grid", 0, 0, 0, 0, 8},
    };
    for (const auto& row : rows) {
        CAPTURE(row.what);
        const auto entries = row.cols > 0
                                 ? PanelWidgetConfig::build_default_grid(row.cols, row.rows)
                                 : PanelWidgetConfig::build_default_grid();
        const auto* pi = find_entry(entries, "printer_image");
        REQUIRE(pi);
        CHECK(pi->col == row.col);
        CHECK(pi->row == row.row);
        CHECK(pi->colspan == row.colspan);
    }
}

TEST_CASE("default_layout: an anchor that does not fit the measured grid is not honoured",
          "[default_layout][grid_key]") {
    // clamp_to_grid() would shove the origin back so the span fits, landing the
    // widget on a neighbour and reading as a designed position. Dropping it to
    // auto-place says what happened: this anchor does not describe this grid.
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            { "id": "printer_image",
              "placements": { "tiny": { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 } } },
            { "id": "print_status",
              "placements": { "tiny": { "col": 8, "row": 0, "colspan": 2, "rowspan": 2 } } },
            { "id": "tips",
              "placements": { "tiny": { "col": 0, "row": 4, "colspan": 10, "rowspan": 2 } } }
        ]
    })");

    SECTION("a 6x14 grid holds printer_image only") {
        // print_status's col 8 is off the grid; tips' colspan 10 is wider than 6.
        const auto entries = PanelWidgetConfig::build_default_grid(6, 14);
        const auto* pi = find_entry(entries, "printer_image");
        REQUIRE(pi);
        CHECK(pi->col == 0);
        CHECK(pi->row == 0);
        const auto* ps = find_entry(entries, "print_status");
        REQUIRE(ps);
        CHECK(ps->col == -1);
        CHECK(ps->row == -1);
        const auto* tips = find_entry(entries, "tips");
        REQUIRE(tips);
        CHECK(tips->col == -1);
        CHECK(tips->row == -1);
        // A dropped anchor keeps its registry span, as an entry that never had a
        // position does, not the span the rejected anchor asked for.
        const auto* tips_def = helix::find_widget_def("tips");
        REQUIRE(tips_def);
        CHECK(tips->colspan == tips_def->colspan);
    }
    SECTION("the same anchors fit a 12x24 grid") {
        const auto entries = PanelWidgetConfig::build_default_grid(12, 24);
        const auto* ps = find_entry(entries, "print_status");
        REQUIRE(ps);
        CHECK(ps->col == 8);
        CHECK(ps->row == 0);
    }
}

TEST_CASE("default_layout: a widget the table chose stays on when its anchor does not fit",
          "[default_layout][grid_key]") {
    // clock is off in the registry, so only the table's choice keeps it on
    // once its anchor is dropped for auto-placement.
    const auto* clock_def = helix::find_widget_def("clock");
    REQUIRE(clock_def);
    REQUIRE_FALSE(clock_def->default_enabled);

    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            { "id": "printer_image",
              "placements": { "tiny": { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 } } },
            { "id": "clock",
              "placements": { "tiny": { "col": 8, "row": 0, "colspan": 4, "rowspan": 2 } } }
        ]
    })");

    SECTION("unseated on a grid too narrow") {
        auto entries = PanelWidgetConfig::build_default_grid(6, 14);
        auto* clock = find_entry(entries, "clock");
        REQUIRE(clock);
        CHECK(clock->enabled);
        CHECK_FALSE(clock->has_grid_position());
    }
    SECTION("seated where it fits") {
        auto entries = PanelWidgetConfig::build_default_grid(12, 24);
        auto* clock = find_entry(entries, "clock");
        REQUIRE(clock);
        CHECK(clock->enabled);
        CHECK(clock->col == 8);
    }
}

TEST_CASE("default_layout: an AMS printer keeps the filament widget the table anchors",
          "[default_layout]") {
    // Whether the spool tile is the AMS or the filament sensor is the table's
    // decision (a slot); nothing outside it swaps one for the other.
    TempCwdGuard guard;
    guard.write_layout(R"({
        "anchors": [
            { "id": "printer_image",
              "placements": { "tiny": { "col": 0, "row": 0, "colspan": 4, "rowspan": 4 } } },
            { "id": "filament",
              "placements": { "tiny": { "col": 4, "row": 0, "colspan": 2, "rowspan": 2 } } }
        ]
    })");
    AmsSubjectGuard ams(4);

    auto entries = PanelWidgetConfig::build_default_grid();
    auto* fil = find_entry(entries, "filament");
    REQUIRE(fil);
    CHECK(fil->enabled);
    CHECK(fil->col == 4);
    CHECK(fil->row == 0);

    const auto* ams_def = helix::find_widget_def("ams");
    REQUIRE(ams_def);
    auto* a = find_entry(entries, "ams");
    REQUIRE(a);
    CHECK(a->enabled == ams_def->default_enabled);
}

TEST_CASE("default_layout: every grid-qualified table fits the grid it names",
          "[default_layout][shipped][grid_key]") {
    // A grid-qualified key states the track grid it was authored for, so it
    // checks against that rather than against a tier budget — no assumption
    // about which panel or scale produces the grid, and none needed. This is
    // the check the tier-keyed budgets cannot make: they hold one grid per
    // tier, and the UI scale gives a tier as many grids as it has scales.
    nlohmann::json layout = load_shipped_layout();

    std::vector<std::pair<std::string, const nlohmann::json*>> tables;
    tables.emplace_back("base", &layout);
    if (layout.contains("variants")) {
        for (auto v = layout["variants"].begin(); v != layout["variants"].end(); ++v) {
            if (v->is_object() && v->contains("anchors")) {
                tables.emplace_back(v.key(), &*v);
            }
        }
    }

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
                if (!anchor.contains("placements") || !anchor["placements"].contains(key)) {
                    continue;
                }
                const auto& p = anchor["placements"][key];
                if (anchor.contains("slot")) {
                    // A slot's region holds its place on the grid like an anchor;
                    // what fills it is checked by the slot cases below.
                    const AnchorRect r{"slot:" + anchor.value("slot", std::string{}),
                                       p.value("col", 0), p.value("row", 0), p.value("colspan", 1),
                                       p.value("rowspan", 1)};
                    INFO("slot " << r.id);
                    CHECK(r.col + r.colspan <= pk.cols);
                    CHECK(r.row + r.rowspan <= pk.rows);
                    for (const auto& other : placed) {
                        INFO("overlaps anchor " << other.id);
                        CHECK_FALSE(anchors_overlap(r, other));
                    }
                    placed.push_back(r);
                    continue;
                }
                const std::string id = anchor.value("id", std::string{});
                const auto* def = helix::find_widget_def(id);
                INFO("anchor " << id);
                REQUIRE(def != nullptr);

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
                    INFO("overlaps anchor " << other.id);
                    CHECK_FALSE(anchors_overlap(r, other));
                }
                placed.push_back(r);
            }
        }
    }

    // The shipped file carries no grid-qualified table today, so this case
    // guards the keys a later edit adds; the cases above cover the key itself.
}

// ============================================================================
// Capability slots resolved by build_default_grid()
// ============================================================================

namespace {

using helix::test::ScopedIntSubject;

/// The medium landscape grid of an 800x480 panel: 12x8 tracks.
constexpr int kMediumCols = 12;
constexpr int kMediumRows = 8;

/// An 800x480 landscape panel at the medium tier with no dryer, chamber or
/// Spoolman; @p readouts sets the LED and filament-sensor gates together.
struct MediumSlotScene {
    MediumSlotScene(int ams_slots, int readouts)
        : ams(ams_slots), led("led_controllable", readouts),
          fil("filament_sensor_count", readouts) {}
    helix::test::ScopedBreakpoint bp{UiBreakpoint::Medium};
    LayoutTypeGuard landscape{800, 480};
    AmsSubjectGuard ams;
    ScopedIntSubject dryer{"home_dryer_available", 0};
    ScopedIntSubject chamber{"printer_has_chamber", 0};
    ScopedIntSubject led;
    ScopedIntSubject fil;
    ScopedIntSubject spoolman{"printer_has_spoolman", 0};
};

void require_every_default_widget_placed_or_off(const std::vector<PanelWidgetEntry>& entries) {
    for (const auto& e : entries) {
        const auto* def = helix::find_widget_def(e.id);
        REQUIRE(def != nullptr);
        if (!def->default_enabled) {
            continue;
        }
        INFO("default-enabled widget " << e.id << " at (" << e.col << "," << e.row << ")");
        CHECK((e.is_placed() || !e.enabled));
    }
}

} // namespace

TEST_CASE("default_layout: an AMS printer fills the medium spool slot with the AMS",
          "[default_layout][slot][shipped]") {
    const MediumSlotScene scene(/*ams_slots=*/4, /*readouts=*/1);

    auto entries = PanelWidgetConfig::build_default_grid(kMediumCols, kMediumRows);

    auto* a = find_entry(entries, "ams");
    REQUIRE(a);
    CHECK(a->enabled);
    CHECK(a->col == 4);
    CHECK(a->row == 2);
    CHECK(a->colspan == 4);
    CHECK(a->rowspan == 2);

    // The slot's other alternative lost, so its candidates are switched off
    // rather than left for auto-placement to squeeze into a full grid.
    for (const char* id : {"filament", "led", "dryer", "chamber_temperature"}) {
        auto* off = find_entry(entries, id);
        REQUIRE(off);
        INFO("candidate " << id);
        CHECK_FALSE(off->enabled);
        CHECK_FALSE(off->has_grid_position());
    }
    require_every_default_widget_placed_or_off(entries);
}

TEST_CASE(
    "default_layout: a printer without an AMS fills the medium spool slot from the graph side",
    "[default_layout][slot][shipped]") {
    const MediumSlotScene scene(/*ams_slots=*/0, /*readouts=*/1);

    auto entries = PanelWidgetConfig::build_default_grid(kMediumCols, kMediumRows);

    // Both cells draw from ENV_FIL in the order they are written: the one
    // beside the graph first, so a lone readout never strands an empty cell
    // between two tiles.
    auto* f = find_entry(entries, "filament");
    REQUIRE(f);
    CHECK(f->is_placed());
    CHECK(f->col == 6);
    CHECK(f->row == 2);
    auto* l = find_entry(entries, "led");
    REQUIRE(l);
    CHECK(l->is_placed());
    CHECK(l->col == 4);
    CHECK(l->row == 2);

    auto* a = find_entry(entries, "ams");
    REQUIRE(a);
    CHECK_FALSE(a->enabled);
    CHECK_FALSE(a->has_grid_position());
    require_every_default_widget_placed_or_off(entries);
}

TEST_CASE("default_layout: a printer with no readout hardware leaves the medium spool slot empty",
          "[default_layout][slot][shipped]") {
    const MediumSlotScene scene(/*ams_slots=*/0, /*readouts=*/0);

    std::string signature;
    auto entries = PanelWidgetConfig::build_default_grid(kMediumCols, kMediumRows, &signature);

    // No stand-in: the region (4,2,4,2) holds nothing at all.
    for (const auto& e : entries) {
        if (!e.enabled || !e.has_grid_position()) {
            continue;
        }
        INFO("widget " << e.id << " at (" << e.col << "," << e.row << ") " << e.colspan << "x"
                       << e.rowspan);
        const bool overlaps =
            e.col < 8 && e.col + e.colspan > 4 && e.row < 4 && e.row + e.rowspan > 2;
        CHECK_FALSE(overlaps);
    }
    REQUIRE(find_entry(entries, "print_status"));
    CHECK(find_entry(entries, "print_status")->is_placed());
    for (const char* id : {"led", "filament", "clock"}) {
        auto* off = find_entry(entries, id);
        REQUIRE(off);
        INFO("candidate " << id);
        CHECK_FALSE(off->enabled);
    }
    // The signature names what the resolution consulted.
    CHECK(signature.find("medium|12|8|") == 0);
    CHECK(signature.find("led_controllable=0") != std::string::npos);
    CHECK(signature.find("ams_slot_count=0") != std::string::npos);
    require_every_default_widget_placed_or_off(entries);
}

TEST_CASE("default_layout: snapshot reads an absent gate as open and an absent requires as off",
          "[default_layout][slot]") {
    const auto table = nlohmann::json::parse(R"({
        "anchors": [],
        "candidate_lists": {
            "X": ["dryer", {"id": "active_spool", "requires": ["zz_test_absent_capability"]},
                  {"id": "clock", "requires": ["zz_test_present_capability"]}]
        }
    })");
    ScopedIntSubject present("zz_test_present_capability", 7);
    REQUIRE(lv_xml_get_subject(nullptr, "zz_test_absent_capability") == nullptr);

    const bool dryer_registered = lv_xml_get_subject(nullptr, "home_dryer_available") != nullptr;
    const auto snap = PanelWidgetConfig::snapshot_capabilities(table);

    CHECK(snap.subjects.count("zz_test_absent_capability") == 1);
    CHECK(snap.get("zz_test_absent_capability") == 0);
    CHECK(snap.get("zz_test_present_capability") == 7);
    if (!dryer_registered) {
        CHECK(snap.get("home_dryer_available") == 1);
    }
    {
        ScopedIntSubject dryer("home_dryer_available", 0);
        CHECK(PanelWidgetConfig::snapshot_capabilities(table).get("home_dryer_available") == 0);
    }

    // An overlay's subjects are read as well as the table's.
    const auto overlay = nlohmann::json::parse(R"({
        "candidate_lists": { "Y": [{"id": "clock", "requires": ["zz_test_overlay_capability"]}] }
    })");
    ScopedIntSubject ov("zz_test_overlay_capability", 3);
    CHECK(PanelWidgetConfig::snapshot_capabilities(table, &overlay)
              .get("zz_test_overlay_capability") == 3);
    CHECK(PanelWidgetConfig::snapshot_capabilities(table).subjects.count(
              "zz_test_overlay_capability") == 0);
}

TEST_CASE("default_layout: an AMS on a medium grid too narrow for the slot is auto-placed",
          "[default_layout][slot][shipped]") {
    const MediumSlotScene scene(/*ams_slots=*/4, /*readouts=*/1);

    // The shipped region (4,2,4,2) ends at track 8; a 6-column grid cuts it.
    auto entries = PanelWidgetConfig::build_default_grid(6, kMediumRows);

    auto* a = find_entry(entries, "ams");
    REQUIRE(a);
    CHECK(a->enabled);
    CHECK_FALSE(a->has_grid_position());
    auto* f = find_entry(entries, "filament");
    REQUIRE(f);
    CHECK_FALSE(f->enabled);
}

// ============================================================================
// The shipped tables: slot structure and printer personas
// ============================================================================

namespace {

using helix::layout::CapabilitySnapshot;
using helix::layout::ResolvedDefault;

/// One table of the shipped file: the base (landscape) anchors or a variant,
/// with the variant chain that selects it and the measured grid of each tier.
struct ShippedTable {
    std::string name;
    const nlohmann::json* table;
    std::vector<std::string> chain;
    const std::map<std::string, TrackBudget>* budgets;
};

std::vector<ShippedTable> shipped_tables(const nlohmann::json& layout) {
    std::vector<ShippedTable> out;
    out.push_back({"base", &layout, {}, &kLandscapeBudget});
    out.push_back({"portrait", &layout["variants"]["portrait"], {"portrait"}, &kPortraitBudget});
    out.push_back(
        {"ultrawide", &layout["variants"]["ultrawide"], {"ultrawide"}, &kUltrawideBudget});
    return out;
}

/// A cell's candidates: its inline array, or the named `candidate_lists` entry.
const nlohmann::json& cell_candidate_list(const nlohmann::json& layout,
                                          const nlohmann::json& cell) {
    const auto& c = cell.at("candidates");
    if (c.is_array()) {
        return c;
    }
    REQUIRE(c.is_string());
    const std::string name = c.get<std::string>();
    INFO("candidate list " << name);
    REQUIRE(layout["candidate_lists"].contains(name));
    return layout["candidate_lists"][name];
}

std::string candidate_id(const nlohmann::json& c) {
    return c.is_string() ? c.get<std::string>() : c.value("id", std::string{});
}

AnchorRect rect_of(const std::string& id, const nlohmann::json& p) {
    return {id, p.value("col", 0), p.value("row", 0), p.value("colspan", 1), p.value("rowspan", 1)};
}

bool cells_fit(const nlohmann::json& alternative, int w, int h) {
    for (const auto& cell : alternative["cells"]) {
        const auto& at = cell["at"];
        if (at[0].get<int>() + at[2].get<int>() > w || at[1].get<int>() + at[3].get<int>() > h) {
            return false;
        }
    }
    return true;
}

/// Every gate and `requires` subject the file names, all closed.
CapabilitySnapshot all_closed(const nlohmann::json& layout) {
    CapabilitySnapshot snap;
    for (const auto& s : helix::layout::subjects_named_by(layout)) {
        snap.subjects[s] = 0;
    }
    return snap;
}

const helix::layout::ResolvedPlacement* placed_widget(const ResolvedDefault& r,
                                                      const std::string& id) {
    for (const auto& p : r.placed) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

bool resolved_disabled(const ResolvedDefault& r, const std::string& id) {
    return std::find(r.disabled.begin(), r.disabled.end(), id) != r.disabled.end();
}

} // namespace

TEST_CASE("default_layout: every shipped slot alternative stays inside its region",
          "[default_layout][shipped][slot]") {
    const auto layout = load_shipped_layout();
    int alternatives_checked = 0;
    for (const auto& t : shipped_tables(layout)) {
        for (const auto& entry : (*t.table)["anchors"]) {
            if (!entry.contains("slot")) {
                continue;
            }
            const std::string slot = entry["slot"].get<std::string>();
            REQUIRE(entry.contains("alternatives"));
            REQUIRE_FALSE(entry["alternatives"].empty());
            for (auto key = entry["placements"].begin(); key != entry["placements"].end(); ++key) {
                const AnchorRect region = rect_of("slot:" + slot, key.value());
                bool any_fits = false;
                for (const auto& alt : entry["alternatives"]) {
                    REQUIRE_FALSE(alt["cells"].empty());
                    if (!cells_fit(alt, region.colspan, region.rowspan)) {
                        continue; // the resolver passes over it for this region
                    }
                    any_fits = true;
                    ++alternatives_checked;
                    std::vector<AnchorRect> cells;
                    for (const auto& cell : alt["cells"]) {
                        const auto& at = cell["at"];
                        const AnchorRect r{"cell", at[0], at[1], at[2], at[3]};
                        INFO("table " << t.name << " slot " << slot << " key " << key.key()
                                      << " cell at " << at.dump());
                        for (const auto& other : cells) {
                            CHECK_FALSE(anchors_overlap(r, other));
                        }
                        cells.push_back(r);
                    }
                }
                INFO("table " << t.name << " slot " << slot << " key " << key.key());
                CHECK(any_fits);
            }
        }
    }
    CHECK(alternatives_checked > 0);
}

TEST_CASE("default_layout: every shipped span is one its widget can take",
          "[default_layout][shipped][slot]") {
    // Within the registry's min and max, and an odd track count only on an axis
    // the widget resolves to half a cell on.
    const auto layout = load_shipped_layout();
    auto check_span = [](const std::string& id, int colspan, int rowspan) {
        const auto* def = helix::find_widget_def(id);
        INFO("widget " << id << " at " << colspan << "x" << rowspan);
        REQUIRE(def != nullptr);
        CHECK(colspan >= def->effective_min_colspan());
        CHECK(colspan <= def->effective_max_colspan());
        CHECK(rowspan >= def->effective_min_rowspan());
        CHECK(rowspan <= def->effective_max_rowspan());
        if (colspan % 2 != 0) {
            CHECK(def->supports_half_col);
        }
        if (rowspan % 2 != 0) {
            CHECK(def->supports_half_row);
        }
    };

    int spans = 0;
    for (const auto& t : shipped_tables(layout)) {
        INFO("table " << t.name);
        for (const auto& entry : (*t.table)["anchors"]) {
            if (!entry.contains("slot")) {
                const std::string id = entry["id"].get<std::string>();
                for (const auto& p : entry["placements"]) {
                    check_span(id, p["colspan"].get<int>(), p["rowspan"].get<int>());
                    ++spans;
                }
                continue;
            }
            INFO("slot " << entry["slot"].get<std::string>());
            for (const auto& alt : entry["alternatives"]) {
                for (const auto& cell : alt["cells"]) {
                    for (const auto& c : cell_candidate_list(layout, cell)) {
                        check_span(candidate_id(c), cell["at"][2].get<int>(),
                                   cell["at"][3].get<int>());
                        ++spans;
                    }
                }
            }
        }
    }
    CHECK(spans > 0);
}

TEST_CASE("default_layout: every shipped candidate is gated on hardware or a capability",
          "[default_layout][shipped][slot]") {
    // A slot holds a readout the printer actually has, or nothing: no candidate
    // is a stand-in that every printer qualifies for.
    const auto layout = load_shipped_layout();
    int candidates = 0;
    auto check_list = [&](const std::string& what, const nlohmann::json& list) {
        INFO(what);
        REQUIRE(list.is_array());
        REQUIRE_FALSE(list.empty());
        for (const auto& c : list) {
            const std::string id = candidate_id(c);
            const auto* def = helix::find_widget_def(id);
            INFO("candidate " << id);
            REQUIRE(def != nullptr);
            const bool has_requires =
                c.is_object() && c.contains("requires") && !c["requires"].empty();
            CHECK((def->hardware_gate_subject != nullptr || has_requires));
            if (id == "chamber_temperature") {
                // A chamber sensor on an open frame reads the room.
                REQUIRE(has_requires);
                const auto& req = c["requires"];
                CHECK(std::find(req.begin(), req.end(), "printer_is_enclosed") != req.end());
            }
            ++candidates;
        }
    };

    for (auto it = layout["candidate_lists"].begin(); it != layout["candidate_lists"].end(); ++it) {
        check_list("named list " + it.key(), it.value());
    }
    for (const auto& t : shipped_tables(layout)) {
        for (const auto& entry : (*t.table)["anchors"]) {
            if (!entry.contains("slot")) {
                continue;
            }
            for (const auto& alt : entry["alternatives"]) {
                for (const auto& cell : alt["cells"]) {
                    if (cell["candidates"].is_array()) {
                        check_list(t.name + " slot " + entry["slot"].get<std::string>(),
                                   cell["candidates"]);
                    }
                }
            }
        }
    }
    CHECK(candidates > 0);
}

TEST_CASE("default_layout: no shipped table places a generic filler or an excluded widget",
          "[default_layout][shipped]") {
    const auto layout = load_shipped_layout();
    std::function<void(const nlohmann::json&, std::set<std::string>&)> walk =
        [&](const nlohmann::json& j, std::set<std::string>& out) {
            if (j.is_string()) {
                out.insert(j.get<std::string>());
            } else if (j.is_structured()) {
                for (const auto& v : j) {
                    walk(v, out);
                }
            }
        };
    // What the tables place: no filler that would stand in for a real readout.
    std::set<std::string> placed;
    walk(layout["anchors"], placed);
    walk(layout["candidate_lists"], placed);
    walk(layout["variants"], placed);
    REQUIRE(placed.count("print_status") == 1);
    for (const char* id :
         {"z_offset", "speed_flow", "print_stats", "job_queue", "gcode_console", "control_buttons",
          "preheat", "clock", "network", "lock", "motion", "macros", "favorite_macro"}) {
        INFO("widget " << id);
        CHECK(placed.count(id) == 0);
    }
    // Anywhere in the file: no widget a default may never place.
    std::set<std::string> anywhere;
    walk(layout, anywhere);
    REQUIRE(anywhere.count("printer_image") > 0);
    for (const char* id : {"toolhead", "host_health", "exclude_map"}) {
        INFO("widget " << id);
        CHECK(anywhere.count(id) == 0);
    }
}

TEST_CASE("default_layout: every shipped slot's last alternative fits every region it inherits",
          "[default_layout][shipped][slot]") {
    // The last alternative has no lead requirement, so it is what a slot falls
    // back to; a region it does not fit leaves the slot empty on that tier.
    const auto layout = load_shipped_layout();
    int checked = 0;
    for (const auto& t : shipped_tables(layout)) {
        for (const auto& entry : (*t.table)["anchors"]) {
            if (!entry.contains("slot")) {
                continue;
            }
            const auto& last = entry["alternatives"].back();
            for (int bp = 0; bp < 7; ++bp) {
                const auto budget = t.budgets->find(kBpNames[bp]);
                const int cols = budget == t.budgets->end() ? 0 : budget->second.cols;
                const int rows = budget == t.budgets->end() ? 0 : budget->second.rows;
                for (const auto& [c, r] : {std::pair{0, 0}, std::pair{cols, rows}}) {
                    const std::string key = helix::layout::choose_placement_key(
                        entry["placements"], static_cast<UiBreakpoint>(bp), c, r);
                    if (key.empty()) {
                        continue;
                    }
                    const AnchorRect region = rect_of("region", entry["placements"][key]);
                    INFO("table " << t.name << " slot " << entry["slot"].get<std::string>()
                                  << " tier " << kBpNames[bp] << " key " << key);
                    CHECK(cells_fit(last, region.colspan, region.rowspan));
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("default_layout: every shipped slot stays empty with every gate closed",
          "[default_layout][shipped][slot]") {
    // A printer that opens no gate and no `requires` subject gets nothing in
    // any slot region: the cells stay empty rather than taking a stand-in.
    const auto layout = load_shipped_layout();
    const CapabilitySnapshot closed = all_closed(layout);
    REQUIRE(closed.subjects.count("ams_slot_count") == 1);
    REQUIRE(closed.subjects.count("printer_has_spoolman") == 1);
    REQUIRE(closed.subjects.count("printer_is_enclosed") == 1);

    struct Grid {
        std::string tier;
        int cols;
        int rows;
    };
    int regions = 0;
    for (const auto& t : shipped_tables(layout)) {
        std::vector<Grid> grids;
        for (const auto& [tier, b] : *t.budgets) {
            grids.push_back({tier, b.cols, b.rows});
        }
        for (const auto& entry : (*t.table)["anchors"]) {
            for (auto key = entry["placements"].begin(); key != entry["placements"].end(); ++key) {
                const auto pk = parse_placement_key(key.key());
                if (pk.grid_qualified()) {
                    grids.push_back({pk.tier, pk.cols, pk.rows});
                }
            }
        }

        for (const auto& g : grids) {
            const int bp = breakpoint_from_name(g.tier.c_str());
            REQUIRE(bp >= 0);
            const ResolvedDefault r = helix::layout::resolve_default_layout(
                layout, nullptr, g.tier, g.cols, g.rows, t.chain, "", closed);
            INFO("table " << t.name << " grid " << g.tier << "@" << g.cols << "x" << g.rows);
            REQUIRE(r.ok);

            for (const auto& entry : (*t.table)["anchors"]) {
                if (!entry.contains("slot")) {
                    continue;
                }
                const std::string key = helix::layout::choose_placement_key(
                    entry["placements"], static_cast<UiBreakpoint>(bp), g.cols, g.rows);
                if (key.empty()) {
                    continue;
                }
                const AnchorRect region = rect_of("region", entry["placements"][key]);
                ++regions;
                for (const auto& p : r.placed) {
                    const AnchorRect placed{p.id, p.col, p.row, p.colspan, p.rowspan};
                    INFO("slot " << entry["slot"].get<std::string>() << " holds " << p.id);
                    CHECK_FALSE(anchors_overlap(region, placed));
                }
            }
        }
    }
    CHECK(regions > 0);
}

TEST_CASE("default_layout: the shipped table repeats no key within an object",
          "[default_layout][shipped]") {
    // The parser keeps the last of two equal keys, so a repeated tier silently
    // discards the placement written first.
    std::ifstream in(helix::find_readable("default_layout.json"));
    REQUIRE(in.is_open());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::vector<std::set<std::string>> open_objects;
    std::vector<std::string> repeated;
    const auto table = nlohmann::json::parse(
        text, [&](int /*depth*/, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            switch (event) {
            case nlohmann::json::parse_event_t::object_start:
                open_objects.emplace_back();
                break;
            case nlohmann::json::parse_event_t::object_end:
                open_objects.pop_back();
                break;
            case nlohmann::json::parse_event_t::key:
                if (!open_objects.back().insert(parsed.get<std::string>()).second) {
                    repeated.push_back(parsed.get<std::string>());
                }
                break;
            default:
                break;
            }
            return true;
        });
    REQUIRE(table.is_object());
    INFO("repeated keys: " << nlohmann::json(repeated).dump());
    CHECK(repeated.empty());
}

namespace {

struct ExpectedRect {
    const char* id;
    int col;
    int row;
    int colspan;
    int rowspan;
};

struct Persona {
    const char* name;
    const char* tier;
    int cols;
    int rows;
    std::vector<std::string> chain;
    std::map<std::string, int> open;  // every subject not named here reads 0
    std::vector<ExpectedRect> placed; // the whole page: nothing else is placed
    std::vector<const char*> not_placed;
    std::map<std::string, int> unnamed = {}; // published subjects the table never consults
};

// Capability subjects as each printer publishes them, tracks as the shipped
// table places them at that printer's own measured grid.
// clang-format off
const std::vector<Persona> kPersonas = {
    {"k2 medium: the CFS takes the spool slot as one 2x1 tile", "medium", 12, 8, {},
     {{"ams_slot_count", 4}, {"led_controllable", 1}, {"printer_has_chamber", 1},
      {"printer_is_enclosed", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"fan_stack", 8, 0, 2, 2}, {"notifications", 10, 0, 2, 2}, {"ams", 4, 2, 4, 2},
      {"temp_graph", 8, 2, 4, 6}, {"print_status", 0, 4, 8, 4}},
     {"filament", "led", "chamber_temperature", "tips"}},
    {"ad5m medium: an open frame's chamber sensor gets no tile", "medium", 12, 8, {},
     {{"printer_has_chamber", 1}, {"led_controllable", 1}, {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"fan_stack", 8, 0, 2, 2}, {"notifications", 10, 0, 2, 2}, {"filament", 6, 2, 2, 2},
      {"led", 4, 2, 2, 2}, {"temp_graph", 8, 2, 4, 6}, {"print_status", 0, 4, 8, 4}},
     {"ams", "chamber_temperature", "active_spool"}},
    {"ad5m pro medium: the LED leads an enclosed chamber", "medium", 12, 8, {},
     {{"printer_has_chamber", 1}, {"printer_is_enclosed", 1}, {"led_controllable", 1},
      {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"fan_stack", 8, 0, 2, 2}, {"notifications", 10, 0, 2, 2}, {"filament", 6, 2, 2, 2},
      {"led", 4, 2, 2, 2}, {"temp_graph", 8, 2, 4, 6},
      {"print_status", 0, 4, 8, 4}},
     {"ams", "chamber_temperature"}},
    {"ad5m with Spoolman: filament and LED keep the cells", "medium", 12, 8, {},
     {{"printer_has_chamber", 1}, {"led_controllable", 1}, {"filament_sensor_count", 1},
      {"printer_has_spoolman", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"fan_stack", 8, 0, 2, 2}, {"notifications", 10, 0, 2, 2}, {"filament", 6, 2, 2, 2},
      {"led", 4, 2, 2, 2}, {"temp_graph", 8, 2, 4, 6}, {"print_status", 0, 4, 8, 4}},
     {"active_spool", "chamber_temperature"}},
    {"Spoolman alone: active_spool beside the graph, the image-side cell empty", "medium", 12, 8, {},
     {{"printer_has_chamber", 1}, {"printer_has_spoolman", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"fan_stack", 8, 0, 2, 2}, {"notifications", 10, 0, 2, 2}, {"active_spool", 6, 2, 2, 2},
      {"temp_graph", 8, 2, 4, 6}, {"print_status", 0, 4, 8, 4}},
     {"chamber_temperature", "led", "filament"}},
    {"cc1 micro: print status beside the graph, filament and LED beside the fans", "micro", 12, 8, {},
     {{"led_controllable", 1}, {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 6, 4}, {"temperature", 6, 0, 2, 2}, {"bed_temperature", 8, 0, 2, 2},
      {"notifications", 10, 0, 2, 2}, {"fan_stack", 6, 2, 2, 2}, {"filament", 8, 2, 2, 2},
      {"led", 10, 2, 2, 2}, {"print_status", 0, 4, 8, 4}, {"temp_graph", 8, 4, 4, 4}},
     {"ams", "tips"}},
    {"qidi_q2 micro: the AMS takes the spool slot", "micro", 12, 8, {},
     {{"ams_slot_count", 4}, {"printer_has_chamber", 1}, {"printer_is_enclosed", 1},
      {"led_controllable", 1}, {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 6, 4}, {"temperature", 6, 0, 2, 2}, {"bed_temperature", 8, 0, 2, 2},
      {"notifications", 10, 0, 2, 2}, {"fan_stack", 6, 2, 2, 2}, {"ams", 8, 2, 4, 2},
      {"print_status", 0, 4, 8, 4}, {"temp_graph", 8, 4, 4, 4}},
     {"chamber_temperature", "led", "filament"}},
    {"creality hi micro@8x6: one readout beside the temperatures, notifications in the corner",
     "micro", 8, 6, {},
     {{"led_controllable", 1}, {"filament_sensor_count", 1}},
     {{"temperature", 0, 0, 2, 2}, {"bed_temperature", 2, 0, 2, 2}, {"filament", 4, 0, 2, 2},
      {"notifications", 6, 0, 2, 2}, {"print_status", 0, 2, 8, 4}},
     {"printer_image", "fan_stack", "led", "temp_graph", "ams"}},
    {"creality hi combo micro@8x6: the CFS takes the readout cell", "micro", 8, 6, {},
     {{"ams_slot_count", 4}, {"led_controllable", 1}, {"filament_sensor_count", 1}},
     {{"temperature", 0, 0, 2, 2}, {"bed_temperature", 2, 0, 2, 2}, {"ams", 4, 0, 2, 2},
      {"notifications", 6, 0, 2, 2}, {"print_status", 0, 2, 8, 4}},
     {"printer_image", "fan_stack", "filament", "led", "temp_graph"}},
    {"u1 tiny: the tool switcher takes the spool slot", "tiny", 10, 8, {},
     {{"ams_slot_count", 4}, {"ams_is_tool_changer", 1}, {"led_controllable", 1},
      {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"notifications", 8, 0, 2, 2}, {"fan_stack", 4, 2, 2, 2}, {"tool_switcher", 6, 2, 4, 2},
      {"print_status", 0, 4, 10, 4}},
     {"ams", "led", "filament"}},
    {"voron toolchanger medium: the tool switcher takes the spool slot, no toolhead", "medium", 12,
     8, {},
     {{"ams_slot_count", 4}, {"ams_is_tool_changer", 1}, {"printer_has_chamber", 1},
      {"printer_is_enclosed", 1}, {"led_controllable", 1}, {"filament_sensor_count", 1},
      {"printer_has_bed_mesh", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"fan_stack", 8, 0, 2, 2}, {"notifications", 10, 0, 2, 2}, {"tool_switcher", 4, 2, 4, 2},
      {"temp_graph", 8, 2, 4, 6}, {"print_status", 0, 4, 8, 4}},
     {"ams", "toolhead", "chamber_temperature", "led", "filament", "bed_mesh_tile"}},
    {"voron toolchanger micro: the tool switcher takes the spool slot", "micro", 12, 8, {},
     {{"ams_slot_count", 4}, {"ams_is_tool_changer", 1}, {"led_controllable", 1},
      {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 6, 4}, {"temperature", 6, 0, 2, 2}, {"bed_temperature", 8, 0, 2, 2},
      {"notifications", 10, 0, 2, 2}, {"fan_stack", 6, 2, 2, 2}, {"tool_switcher", 8, 2, 4, 2},
      {"print_status", 0, 4, 8, 4}, {"temp_graph", 8, 4, 4, 4}},
     {"ams", "led", "filament"}},
    {"voron toolchanger large: the eight-track strip under the readout band, the AMS kept",
     "large", 16, 10, {},
     {{"ams_slot_count", 4}, {"ams_is_tool_changer", 1}, {"printer_has_chamber", 1},
      {"printer_is_enclosed", 1}, {"led_controllable", 1}, {"filament_sensor_count", 1},
      {"printer_has_bed_mesh", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"ams", 8, 0, 4, 2}, {"fan_stack", 12, 0, 2, 2}, {"notifications", 14, 0, 2, 2},
      {"tool_switcher", 8, 2, 8, 2}, {"print_status", 0, 4, 8, 4},
      {"bed_mesh_tile", 8, 6, 4, 4}, {"temp_graph", 12, 6, 4, 4}, {"tips", 0, 8, 8, 2}},
     {"toolhead", "chamber_temperature", "led", "filament"}},
    {"voron toolchanger xxlarge: the eight-track strip above the declared block", "xxlarge", 24,
     14, {},
     {{"ams_slot_count", 4}, {"ams_is_tool_changer", 1}, {"led_controllable", 1},
      {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 6, 6}, {"print_status", 6, 0, 10, 6}, {"temperature", 16, 0, 2, 2},
      {"bed_temperature", 18, 0, 2, 2}, {"fan_stack", 20, 0, 2, 2},
      {"notifications", 22, 0, 2, 2}, {"ams", 16, 2, 4, 2}, {"filament", 20, 2, 2, 2},
      {"led", 22, 2, 2, 2}, {"tool_switcher", 16, 8, 8, 2}, {"temp_graph", 0, 6, 16, 6},
      {"tips", 0, 12, 16, 2}},
     {"toolhead", "bed_mesh_tile"}},
    {"voron_24 large: one readout band on top, the bed mesh beside the graph", "large", 16, 10, {},
     {{"printer_has_chamber", 1}, {"printer_is_enclosed", 1}, {"led_controllable", 1},
      {"filament_sensor_count", 1}, {"printer_has_bed_mesh", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"filament", 8, 0, 2, 2}, {"led", 10, 0, 2, 2}, {"fan_stack", 12, 0, 2, 2},
      {"notifications", 14, 0, 2, 2}, {"print_status", 0, 4, 8, 4},
      {"bed_mesh_tile", 8, 6, 4, 4}, {"temp_graph", 12, 6, 4, 4}, {"tips", 0, 8, 8, 2}},
     {"ams", "chamber_temperature", "toolhead", "host_health", "exclude_map", "job_queue",
      "print_stats"}},
    {"voron_24 large without a bed mesh: the mesh cell stays empty", "large", 16, 10, {},
     {{"printer_has_chamber", 1}, {"printer_is_enclosed", 1}, {"led_controllable", 1},
      {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"bed_temperature", 6, 0, 2, 2},
      {"filament", 8, 0, 2, 2}, {"led", 10, 0, 2, 2}, {"fan_stack", 12, 0, 2, 2},
      {"notifications", 14, 0, 2, 2}, {"print_status", 0, 4, 8, 4},
      {"temp_graph", 12, 6, 4, 4}, {"tips", 0, 8, 8, 2}},
     {"bed_mesh_tile", "job_queue"}},
    {"voron_24 xxlarge: the 12x7-cell table keeps the 72px cell", "xxlarge", 24, 14, {},
     {{"printer_has_chamber", 1}, {"printer_is_enclosed", 1}, {"led_controllable", 1},
      {"filament_sensor_count", 1}, {"printer_has_bed_mesh", 1}},
     {{"printer_image", 0, 0, 6, 6}, {"print_status", 6, 0, 10, 6}, {"temperature", 16, 0, 2, 2},
      {"bed_temperature", 18, 0, 2, 2}, {"fan_stack", 20, 0, 2, 2},
      {"notifications", 22, 0, 2, 2}, {"filament", 16, 2, 2, 2},
      {"led", 18, 2, 2, 2}, {"chamber_temperature", 20, 2, 2, 2},
      {"bed_mesh_tile", 16, 4, 8, 4}, {"temp_graph", 0, 6, 16, 6}, {"tips", 0, 12, 16, 2}},
     {"ams", "clock", "print_stats", "z_offset", "toolhead", "host_health", "exclude_map"}},
    {"voron_24 xxlarge without a bed mesh: the mesh region stays empty", "xxlarge", 24, 14, {},
     {{"printer_has_chamber", 1}, {"printer_is_enclosed", 1}, {"led_controllable", 1},
      {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 6, 6}, {"print_status", 6, 0, 10, 6}, {"temperature", 16, 0, 2, 2},
      {"bed_temperature", 18, 0, 2, 2}, {"fan_stack", 20, 0, 2, 2},
      {"notifications", 22, 0, 2, 2}, {"filament", 16, 2, 2, 2},
      {"led", 18, 2, 2, 2}, {"chamber_temperature", 20, 2, 2, 2},
      {"temp_graph", 0, 6, 16, 6}, {"tips", 0, 12, 16, 2}},
     {"bed_mesh_tile", "job_queue"}},
    {"k1 portrait medium: filament and LED beside the graph", "medium", 8, 12, {"portrait"},
     {{"led_controllable", 1}, {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 4, 4}, {"temperature", 4, 0, 2, 2}, {"notifications", 6, 0, 2, 2},
      {"bed_temperature", 4, 2, 2, 2}, {"fan_stack", 6, 2, 2, 2}, {"tips", 0, 4, 8, 2},
      {"print_status", 0, 6, 8, 4}, {"temp_graph", 0, 10, 4, 2}, {"filament", 4, 10, 2, 2},
      {"led", 6, 10, 2, 2}},
     {"ams", "clock"}},
    {"ultrawide 1920x440: image, empty span, readout cluster; print status beside the graph",
     "small", 46, 10, {"ultrawide"},
     {{"led_controllable", 1}, {"filament_sensor_count", 1}},
     {{"printer_image", 0, 0, 8, 4}, {"temperature", 38, 0, 2, 2}, {"bed_temperature", 40, 0, 2, 2},
      {"fan_stack", 42, 0, 2, 2}, {"notifications", 44, 0, 2, 2}, {"filament", 44, 2, 2, 2},
      {"led", 42, 2, 2, 2}, {"print_status", 0, 4, 23, 6}, {"temp_graph", 23, 4, 23, 6}},
     {"ams", "tips"}},
    {"ultratall 440x1920: notifications in the corner, the bed mesh below the graph",
     "small", 10, 44, {"portrait"},
     {{"led_controllable", 1}, {"filament_sensor_count", 1}, {"printer_has_bed_mesh", 1}},
     {{"printer_image", 0, 0, 6, 6}, {"temperature", 6, 0, 2, 2}, {"notifications", 8, 0, 2, 2},
      {"bed_temperature", 6, 2, 2, 2}, {"fan_stack", 8, 2, 2, 2}, {"filament", 6, 4, 2, 2},
      {"led", 8, 4, 2, 2}, {"tips", 0, 6, 10, 2}, {"print_status", 0, 8, 10, 6},
      {"temp_graph", 0, 14, 10, 8}, {"bed_mesh_tile", 0, 22, 8, 6}},
     {"ams", "gcode_console", "clock"}},
};
// clang-format on

} // namespace

TEST_CASE("default_layout: shipped tables resolve each printer persona",
          "[default_layout][shipped][persona]") {
    const auto layout = load_shipped_layout();
    for (const auto& persona : kPersonas) {
        INFO("persona: " << persona.name);
        CapabilitySnapshot caps = all_closed(layout);
        for (const auto& [subject, value] : persona.open) {
            REQUIRE(caps.subjects.count(subject) == 1);
            caps.subjects[subject] = value;
        }
        for (const auto& [subject, value] : persona.unnamed) {
            REQUIRE(caps.subjects.count(subject) == 0);
            caps.subjects[subject] = value;
        }

        const ResolvedDefault r = helix::layout::resolve_default_layout(
            layout, nullptr, persona.tier, persona.cols, persona.rows, persona.chain, "", caps);
        REQUIRE(r.ok);

        for (const auto& e : persona.placed) {
            INFO("widget " << e.id);
            const auto* p = placed_widget(r, e.id);
            REQUIRE(p != nullptr);
            CHECK(p->col == e.col);
            CHECK(p->row == e.row);
            CHECK(p->colspan == e.colspan);
            CHECK(p->rowspan == e.rowspan);
        }
        for (const char* id : persona.not_placed) {
            INFO("widget " << id << " is placed");
            CHECK(placed_widget(r, id) == nullptr);
        }

        CHECK(r.placed.size() == persona.placed.size());
        std::set<std::string> ids;
        for (const auto& p : r.placed) {
            INFO("placed twice: " << p.id);
            CHECK(ids.insert(p.id).second);
        }
        // Nothing default-enabled is left for auto-placement to squeeze in.
        for (const auto& def : helix::get_all_widget_defs()) {
            if (!def.default_enabled) {
                continue;
            }
            INFO("default-enabled widget " << def.id << " neither placed nor disabled");
            CHECK((placed_widget(r, def.id) != nullptr || resolved_disabled(r, def.id)));
        }
    }
}

namespace {

/// The table the resolver reads for @p chain: the first variant in it with
/// anchors, else the base.
const nlohmann::json& active_table(const nlohmann::json& layout,
                                   const std::vector<std::string>& chain) {
    for (const auto& name : chain) {
        if (layout["variants"].contains(name) && layout["variants"][name].is_object() &&
            layout["variants"][name].contains("anchors")) {
            return layout["variants"][name];
        }
    }
    return layout;
}

using TrackSet = std::set<std::pair<int, int>>;

void add_rect(TrackSet& out, int col, int row, int colspan, int rowspan) {
    for (int r = row; r < row + rowspan; ++r) {
        for (int c = col; c < col + colspan; ++c) {
            out.insert({c, r});
        }
    }
}

/// The tracks @p table declares empty on purpose on this grid.
TrackSet declared_gaps(const nlohmann::json& table, int bp, int cols, int rows) {
    TrackSet out;
    if (!table.contains("gaps")) {
        return out;
    }
    const std::string key = helix::layout::choose_placement_key(
        table["gaps"], static_cast<UiBreakpoint>(bp), cols, rows);
    if (key.empty()) {
        return out;
    }
    for (const auto& g : table["gaps"][key]) {
        add_rect(out, g[0].get<int>(), g[1].get<int>(), g[2].get<int>(), g[3].get<int>());
    }
    return out;
}

/// The tracks of every slot region @p table resolves on this grid.
TrackSet slot_regions(const nlohmann::json& table, int bp, int cols, int rows) {
    TrackSet out;
    for (const auto& entry : table["anchors"]) {
        if (!entry.contains("slot")) {
            continue;
        }
        const std::string key = helix::layout::choose_placement_key(
            entry["placements"], static_cast<UiBreakpoint>(bp), cols, rows);
        if (key.empty()) {
            continue;
        }
        const auto& p = entry["placements"][key];
        add_rect(out, p["col"].get<int>(), p["row"].get<int>(), p["colspan"].get<int>(),
                 p["rowspan"].get<int>());
    }
    return out;
}

std::string describe(const TrackSet& tracks) {
    std::string out;
    for (const auto& [c, r] : tracks) {
        out += " (" + std::to_string(c) + "," + std::to_string(r) + ")";
    }
    return out;
}

} // namespace

TEST_CASE("default_layout: every measured grid is covered or declared empty",
          "[default_layout][shipped][coverage]") {
    // The resolver's own output, with nothing left to auto-placement. Every
    // track belongs to at most one tile, at a span its widget can take, and
    // print_status is always placed because it is the only progress readout on
    // home. An empty track is either a gap the table declares for this grid or,
    // when the printer lacks the hardware, a slot cell nothing qualified for.
    // With every gate open each slot fills, so the empty tracks are exactly the
    // declared gaps, with or without an AMS taking its spool slot.
    const auto layout = load_shipped_layout();
    CapabilitySnapshot closed = all_closed(layout);
    CapabilitySnapshot open = closed;
    for (auto& [subject, value] : open.subjects) {
        value = subject == "ams_slot_count" ? 4 : 1;
    }
    CapabilitySnapshot open_no_ams = open;
    open_no_ams.subjects["ams_slot_count"] = 0;

    for (const auto& g : helix::test::kMeasuredGrids) {
        LayoutTypeGuard layout_type(g.panel_w, g.panel_h);
        const std::vector<std::string> chain = helix::LayoutManager::instance().variant_chain();
        const int bp = static_cast<int>(breakpoint_for(std::min(g.panel_w, g.panel_h)));
        const auto& table = active_table(layout, chain);
        const TrackSet gaps = declared_gaps(table, bp, g.cols, g.rows);
        const TrackSet slots = slot_regions(table, bp, g.cols, g.rows);

        for (const CapabilitySnapshot* caps : {&open, &open_no_ams, &closed}) {
            const char* gates = caps == &open          ? "open"
                                : caps == &open_no_ams ? "open, no AMS"
                                                       : "closed";
            INFO(g.name << " (" << kBpNames[bp] << " " << g.cols << "x" << g.rows
                        << " tracks), gates " << gates);
            const ResolvedDefault r = helix::layout::resolve_default_layout(
                layout, nullptr, kBpNames[bp], g.cols, g.rows, chain, "", *caps);
            REQUIRE(r.ok);

            std::vector<std::vector<std::string>> owner(
                static_cast<size_t>(g.rows), std::vector<std::string>(static_cast<size_t>(g.cols)));
            for (const auto& p : r.placed) {
                const auto* def = helix::find_widget_def(p.id);
                INFO("widget " << p.id << " at (" << p.col << "," << p.row << ") " << p.colspan
                               << "x" << p.rowspan);
                REQUIRE(def != nullptr);
                CHECK(p.colspan >= def->effective_min_colspan());
                CHECK(p.colspan <= def->effective_max_colspan());
                CHECK(p.rowspan >= def->effective_min_rowspan());
                CHECK(p.rowspan <= def->effective_max_rowspan());
                CHECK((p.colspan % 2 == 0 || def->supports_half_col));
                CHECK((p.rowspan % 2 == 0 || def->supports_half_row));
                const bool on_grid = p.col >= 0 && p.row >= 0 && p.col + p.colspan <= g.cols &&
                                     p.row + p.rowspan <= g.rows;
                CHECK(on_grid);
                if (!on_grid) {
                    continue;
                }
                for (int row = p.row; row < p.row + p.rowspan; ++row) {
                    for (int col = p.col; col < p.col + p.colspan; ++col) {
                        auto& cell = owner[static_cast<size_t>(row)][static_cast<size_t>(col)];
                        INFO("track (" << col << "," << row << ") already holds " << cell);
                        CHECK(cell.empty());
                        cell = p.id;
                    }
                }
            }

            TrackSet holes;
            for (int row = 0; row < g.rows; ++row) {
                for (int col = 0; col < g.cols; ++col) {
                    if (owner[static_cast<size_t>(row)][static_cast<size_t>(col)].empty()) {
                        holes.insert({col, row});
                    }
                }
            }
            if (caps == &closed) {
                TrackSet undeclared;
                for (const auto& t : holes) {
                    if (!gaps.count(t) && !slots.count(t)) {
                        undeclared.insert(t);
                    }
                }
                INFO(
                    "empty tracks neither a declared gap nor a slot cell:" << describe(undeclared));
                CHECK(undeclared.empty());
            } else {
                INFO("empty tracks:" << describe(holes));
                INFO("declared gaps:" << describe(gaps));
                CHECK(holes == gaps);
            }

            CHECK(placed_widget(r, "print_status") != nullptr);
            for (const auto& def : helix::get_all_widget_defs()) {
                if (def.default_enabled) {
                    INFO("default-enabled widget " << def.id << " neither placed nor disabled");
                    CHECK((placed_widget(r, def.id) != nullptr || resolved_disabled(r, def.id)));
                }
            }
        }
    }
}

TEST_CASE("default_layout: every declared gap is whole cells on its grid",
          "[default_layout][shipped][coverage]") {
    // A gap is breathing room a user can drop a widget into, so it is at least
    // one whole cell and never straddles one.
    const auto layout = load_shipped_layout();
    int gaps = 0;
    for (const auto& t : shipped_tables(layout)) {
        if (!t.table->contains("gaps")) {
            continue;
        }
        for (auto key = (*t.table)["gaps"].begin(); key != (*t.table)["gaps"].end(); ++key) {
            // Each track is declared at most once: a duplicated or overlapping
            // rectangle would otherwise merge silently into the coverage set.
            TrackSet declared;
            const auto pk = parse_placement_key(key.key());
            int cols = pk.cols;
            int rows = pk.rows;
            if (!pk.grid_qualified()) {
                const auto b = t.budgets->find(pk.tier);
                REQUIRE(b != t.budgets->end());
                cols = b->second.cols;
                rows = b->second.rows;
            }
            for (const auto& g : key.value()) {
                INFO("table " << t.name << " key " << key.key() << " gap " << g.dump());
                REQUIRE(g.is_array());
                REQUIRE(g.size() == 4);
                const int c = g[0].get<int>(), r = g[1].get<int>(), w = g[2].get<int>(),
                          h = g[3].get<int>();
                CHECK(c % 2 == 0);
                CHECK(r % 2 == 0);
                CHECK(w >= 2);
                CHECK(h >= 2);
                CHECK(w % 2 == 0);
                CHECK(h % 2 == 0);
                CHECK(c + w <= cols);
                CHECK(r + h <= rows);
                for (int y = r; y < r + h; ++y) {
                    for (int x = c; x < c + w; ++x) {
                        INFO("track (" << x << "," << y << ")");
                        CHECK(declared.insert({x, y}).second);
                    }
                }
                ++gaps;
            }
        }
    }
    CHECK(gaps > 0);
}

namespace {

/// @p layout with every tool_switcher candidate taken out: the slot that holds
/// only the switcher, and each alternative whose lead cell names it.
nlohmann::json without_tool_switcher(nlohmann::json layout) {
    auto names_switcher = [](const nlohmann::json& cell) {
        const auto it = cell.find("candidates");
        if (it == cell.end() || !it->is_array())
            return false;
        for (const auto& c : *it) {
            if ((c.is_string() && c == "tool_switcher") ||
                (c.is_object() && c.value("id", "") == "tool_switcher"))
                return true;
        }
        return false;
    };
    auto& anchors = layout["anchors"];
    nlohmann::json kept = nlohmann::json::array();
    for (auto anchor : anchors) {
        if (anchor.contains("alternatives")) {
            nlohmann::json alts = nlohmann::json::array();
            for (const auto& alt : anchor["alternatives"]) {
                if (!names_switcher(alt["cells"][0]))
                    alts.push_back(alt);
            }
            if (alts.empty())
                continue;
            anchor["alternatives"] = alts;
        }
        kept.push_back(anchor);
    }
    anchors = kept;
    return layout;
}

} // namespace

TEST_CASE("default_layout: a printer that is not a tool changer gets the layout it had",
          "[default_layout][shipped][tool_switcher]") {
    // The tool switcher only ever takes space a tool changer has: every other
    // printer resolves to the same tiles, in the same order, as the table
    // with no switcher in it.
    const auto layout = load_shipped_layout();
    const auto baseline = without_tool_switcher(layout);
    REQUIRE(baseline.dump() != layout.dump());
    CapabilitySnapshot closed = all_closed(layout);
    CapabilitySnapshot open = closed;
    for (auto& [subject, value] : open.subjects) {
        value = subject == "ams_slot_count" ? 4 : 1;
    }
    open.subjects["ams_is_tool_changer"] = 0;
    CapabilitySnapshot open_no_ams = open;
    open_no_ams.subjects["ams_slot_count"] = 0;

    for (const auto& g : helix::test::kMeasuredGrids) {
        LayoutTypeGuard layout_type(g.panel_w, g.panel_h);
        const std::vector<std::string> chain = helix::LayoutManager::instance().variant_chain();
        const int bp = static_cast<int>(breakpoint_for(std::min(g.panel_w, g.panel_h)));
        for (const CapabilitySnapshot* caps : {&open, &open_no_ams, &closed}) {
            INFO(g.name << ", gates " << (caps == &closed ? "closed" : "open"));
            const ResolvedDefault now = helix::layout::resolve_default_layout(
                layout, nullptr, kBpNames[bp], g.cols, g.rows, chain, "", *caps);
            const ResolvedDefault before = helix::layout::resolve_default_layout(
                baseline, nullptr, kBpNames[bp], g.cols, g.rows, chain, "", *caps);
            REQUIRE(now.ok);
            REQUIRE(now.placed.size() == before.placed.size());
            for (size_t i = 0; i < now.placed.size(); ++i) {
                const auto& a = now.placed[i];
                const auto& b = before.placed[i];
                INFO("entry " << i << ": " << a.id << " vs " << b.id);
                CHECK(a.id == b.id);
                CHECK(a.col == b.col);
                CHECK(a.row == b.row);
                CHECK(a.colspan == b.colspan);
                CHECK(a.rowspan == b.rowspan);
                CHECK(a.config == b.config);
            }
        }
    }
}
