// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_pre_print_options_renderer.h"
#include "ui_print_preparation_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "macro_param_cache.h"
#include "pre_print_option.h"
#include "printer_detector.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

/// Walk a container's children top-to-bottom and return ids of switch widgets
/// found, in display order. Helper for asserting "the rows came out in the
/// expected sequence" without coupling to LVGL widget pointers.
std::vector<std::string> child_widget_classes(lv_obj_t* container) {
    std::vector<std::string> classes;
    uint32_t n = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(container, i);
        const lv_obj_class_t* cls = lv_obj_get_class(child);
        if (cls == &lv_label_class) {
            classes.emplace_back("label");
        } else {
            // Anything else (rows, switches, etc.) — track as "row" since
            // every non-label child in our renderer output is a row.
            classes.emplace_back("row");
        }
    }
    return classes;
}

/// Build an option set with options across multiple categories. (Categories
/// are sort keys only — the renderer emits a flat row list with no
/// subheaders. This helper just exercises the multi-category sort path.)
/// Mirrors the JSON shape that `parse_pre_print_option_set` accepts but
/// builds it directly to keep the test independent of printer_database.json
/// drift.
PrePrintOptionSet make_multi_category_set() {
    PrePrintOptionSet s;
    s.macro_name = "START_PRINT";

    PrePrintOption mech;
    mech.id = "bed_mesh";
    mech.category = PrePrintCategory::Mechanical;
    mech.order = 10;
    mech.default_enabled = true;
    mech.strategy_kind = PrePrintStrategyKind::MacroParam;
    mech.strategy = PrePrintStrategyMacroParam{"SKIP_BED_MESH", "0", "1", "0"};

    PrePrintOption qual;
    qual.id = "nozzle_clean";
    qual.category = PrePrintCategory::Quality;
    qual.order = 10;
    qual.default_enabled = false;
    qual.strategy_kind = PrePrintStrategyKind::MacroParam;
    qual.strategy = PrePrintStrategyMacroParam{"SKIP_NOZZLE_CLEAN", "0", "1", "0"};

    PrePrintOption mon;
    mon.id = "ai_detect";
    mon.category = PrePrintCategory::Monitoring;
    mon.order = 10;
    mon.default_enabled = false;
    mon.strategy_kind = PrePrintStrategyKind::PreStartGcode;
    mon.strategy = PrePrintStrategyPreStartGcode{"LOAD_AI_RUN SWITCH={value}"};

    s.options = {mech, qual, mon};
    return s;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: empty option set leaves container empty",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    PrePrintOptionSet empty;
    renderer.populate(container, empty, nullptr, nullptr);

    REQUIRE(renderer.row_count() == 0);
    REQUIRE(lv_obj_get_child_count(container) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "PrePrintOptionsRenderer: single-category set has no subheader",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";

    PrePrintOption opt;
    opt.id = "bed_mesh";
    opt.category = PrePrintCategory::Mechanical;
    opt.order = 10;
    opt.default_enabled = true;
    opt.strategy_kind = PrePrintStrategyKind::MacroParam;
    opt.strategy = PrePrintStrategyMacroParam{"SKIP_LEVELING", "0", "1", "0"};
    set.options.push_back(opt);

    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.row_count() == 1);
    auto rendered = renderer.rendered_ids();
    REQUIRE(rendered.size() == 1);
    REQUIRE(rendered[0] == "bed_mesh");

    // Flat list: 1 row container only, no subheader.
    REQUIRE(lv_obj_get_child_count(container) == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: multi-category set emits flat row list",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    auto set = make_multi_category_set();
    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.row_count() == 3);
    REQUIRE(renderer.rendered_ids() ==
            std::vector<std::string>{"bed_mesh", "nozzle_clean", "ai_detect"});

    // 3 rows, no category subheaders — the section title comes from the
    // surrounding XML card, not the renderer.
    REQUIRE(lv_obj_get_child_count(container) == 3);

    // Display order: row, row, row.
    auto classes = child_widget_classes(container);
    REQUIRE(classes == std::vector<std::string>{"row", "row", "row"});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: state subjects initialized from default_enabled",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    auto set = make_multi_category_set();
    renderer.populate(container, set, nullptr, nullptr);

    // bed_mesh: default_enabled=true -> 1
    REQUIRE(renderer.get_state("bed_mesh") == 1);
    // nozzle_clean: default_enabled=false -> 0
    REQUIRE(renderer.get_state("nozzle_clean") == 0);
    // ai_detect: default_enabled=false -> 0
    REQUIRE(renderer.get_state("ai_detect") == 0);

    // Unknown id: returns the supplied default (0 by default).
    REQUIRE(renderer.get_state("does_not_exist") == 0);
    REQUIRE(renderer.get_state("does_not_exist", 42) == 42);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: visibility lookup hides row when subject is 0",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    // Single-option set with a paired visibility subject.
    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    PrePrintOption opt;
    opt.id = "bed_mesh";
    opt.category = PrePrintCategory::Mechanical;
    opt.order = 10;
    opt.default_enabled = true;
    opt.strategy_kind = PrePrintStrategyKind::MacroParam;
    opt.strategy = PrePrintStrategyMacroParam{"SKIP_LEVELING", "0", "1", "0"};
    set.options.push_back(opt);

    lv_subject_t can_show{};
    lv_subject_init_int(&can_show, 1); // start visible

    auto vis_lookup = [&](const std::string& id) -> lv_subject_t* {
        return id == "bed_mesh" ? &can_show : nullptr;
    };

    renderer.populate(container, set, vis_lookup, nullptr);
    REQUIRE(renderer.row_count() == 1);

    lv_obj_t* row = renderer.get_row("bed_mesh");
    REQUIRE(row != nullptr);

    // Initially visible.
    REQUIRE_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // Flipping the visibility subject hides the row.
    lv_subject_set_int(&can_show, 0);
    REQUIRE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // And restoring re-shows it.
    lv_subject_set_int(&can_show, 1);
    REQUIRE_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // Tear down: clear the renderer FIRST so observers are uninstalled
    // before we deinit the local visibility subject (avoids dangling
    // observer pointers).
    renderer.clear();
    lv_subject_deinit(&can_show);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: set_state updates subject and persists across reads",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    auto set = make_multi_category_set();
    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.get_state("nozzle_clean") == 0);
    renderer.set_state("nozzle_clean", 1);
    REQUIRE(renderer.get_state("nozzle_clean") == 1);

    // No-op for unknown id (must not crash).
    renderer.set_state("does_not_exist", 1);
    REQUIRE(renderer.get_state("does_not_exist") == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: AD5M Pro live DB entry produces one row, no subheader",
                 "[print_file_detail][pre_print_options][db]") {
    // Sanity-checks the live printer_database.json: AD5M Pro currently has a
    // single mechanical option (bed_mesh). Only one category present means
    // exactly one subheader (per category) is emitted. If the DB grows new
    // categories for this printer, this test will need adjustment.
    auto set = PrinterDetector::get_pre_print_option_set("FlashForge Adventurer 5M Pro");
    REQUIRE_FALSE(set.empty());

    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.row_count() == set.options.size());
    // Every option must have produced a corresponding row.
    for (const auto& opt : set.options) {
        REQUIRE(renderer.get_row(opt.id) != nullptr);
        REQUIRE(renderer.get_switch(opt.id) != nullptr);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "PrePrintOptionsRenderer: K1C live DB entry produces one row",
                 "[print_file_detail][pre_print_options][db]") {
    auto set = PrinterDetector::get_pre_print_option_set("Creality K1C");
    REQUIRE_FALSE(set.empty());

    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, set, nullptr, nullptr);

    // K1C currently has just bed_mesh (PREPARE param + PRINT_PREPARED
    // pre-start gcode). Verify the row exists with a switch widget.
    REQUIRE(renderer.row_count() == set.options.size());
    REQUIRE(renderer.get_row("bed_mesh") != nullptr);
    REQUIRE(renderer.get_switch("bed_mesh") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: K2 Plus live DB renders bed_mesh and ai_detect",
                 "[print_file_detail][pre_print_options][db][ai_detect]") {
    // K2 Plus advertises bed_mesh (Mechanical) + ai_detect (Monitoring).
    // Renderer emits a flat row list with no subheaders (categories are sort
    // keys only; the section title comes from print_file_detail.xml's
    // PRINT OPTIONS card header).
    auto set = PrinterDetector::get_pre_print_option_set("Creality K2 Plus");
    REQUIRE_FALSE(set.empty());

    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, set, nullptr, nullptr);

    REQUIRE(renderer.row_count() == set.options.size());
    REQUIRE(renderer.get_row("bed_mesh") != nullptr);
    REQUIRE(renderer.get_row("ai_detect") != nullptr);
    REQUIRE(renderer.get_switch("ai_detect") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: DB option labels are English text, not semantic keys",
                 "[print_file_detail][pre_print_options][i18n]") {
    // English loads NO translation pack (translation_loader.cpp skips
    // kIdentityLocale), so lv_tr(key) returns the key unchanged — in tests
    // and in the English UI alike. A label_key holding a semantic key such
    // as "pre_print_option.ai_detect.label" therefore renders the raw dotted
    // identifier to the user (v0.99.114 regression class; the timelapse twin
    // is covered in test_has_any_preprint_options.cpp against the C++
    // synthesis). This reads the REAL printer_database.json entry so a
    // semantic key reintroduced in the DB fails here.
    auto k2set = PrinterDetector::get_pre_print_option_set("Creality K2 Plus");
    const PrePrintOption* ai = k2set.find("ai_detect");
    REQUIRE(ai != nullptr);
    REQUIRE_FALSE(ai->label_key.empty());

    // A semantic key is unmistakable: dotted identifier. English option text
    // ("AI detection") never contains one.
    INFO("label_key: " << ai->label_key);
    REQUIRE(ai->label_key.find('.') == std::string::npos);

    // And through the renderer's own lookup the label is human text.
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, k2set, nullptr, nullptr);
    lv_obj_t* label = lv_obj_find_by_name(renderer.get_row("ai_detect"), "label");
    REQUIRE(label != nullptr);
    REQUIRE(std::string(lv_label_get_text(label)) == "AI detection");
}

TEST_CASE_METHOD(LVGLUITestFixture, "PrePrintOptionsRenderer: label_key wins over humanize_id",
                 "[print_file_detail][pre_print_options][label]") {
    // When `label_key` is present, the renderer must look it up via lv_tr
    // and never fall through to the humanize_id path. We verify by giving
    // the option an id that humanize_id WOULD garble (mixed case, no
    // underscores) — if label_key is honored, the row label is the i18n
    // value (or the key itself, since en.yml lacks this synthetic key);
    // if humanize_id ran, the label would have been title-cased from the id.
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";

    PrePrintOption keyed;
    keyed.id = "weirdId123";                        // humanize_id would emit "WeirdId123"
    keyed.label_key = "pre_print_option.foo.label"; // not in en.yml; lv_tr returns it as-is
    keyed.category = PrePrintCategory::Mechanical;
    keyed.order = 10;
    keyed.default_enabled = false;
    keyed.strategy_kind = PrePrintStrategyKind::MacroParam;
    keyed.strategy = PrePrintStrategyMacroParam{"PARAM", "1", "0", "0"};
    set.options.push_back(keyed);

    PrePrintOption unkeyed;
    unkeyed.id = "ai_detect"; // humanize_id capitalizes after each separator -> "AI Detect"
    unkeyed.category = PrePrintCategory::Mechanical;
    unkeyed.order = 20;
    unkeyed.default_enabled = false;
    unkeyed.strategy_kind = PrePrintStrategyKind::MacroParam;
    unkeyed.strategy = PrePrintStrategyMacroParam{"PARAM2", "1", "0", "0"};
    set.options.push_back(unkeyed);

    renderer.populate(container, set, nullptr, nullptr);
    REQUIRE(renderer.row_count() == 2);

    // Find the label widget inside each row. Each row container's first
    // child is the label (it's added before the switch).
    auto label_text_for = [&](const std::string& id) -> std::string {
        lv_obj_t* row = renderer.get_row(id);
        REQUIRE(row != nullptr);
        REQUIRE(lv_obj_get_child_count(row) >= 1);
        lv_obj_t* label = lv_obj_get_child(row, 0);
        REQUIRE(lv_obj_get_class(label) == &lv_label_class);
        const char* t = lv_label_get_text(label);
        return std::string(t ? t : "");
    };

    // label_key path: lv_tr returns the key when no translation exists.
    // The renderer never invokes humanize_id, so the output is exactly
    // the key string (or its translation). Either way, it is NOT the
    // title-cased id form.
    std::string keyed_text = label_text_for("weirdId123");
    REQUIRE(keyed_text != "WeirdId123");
    REQUIRE_FALSE(keyed_text.empty());

    // No label_key: humanize_id runs, then is run through lv_tr (which
    // returns the humanized form when no translation exists). humanize_id
    // capitalizes after each separator, then a fix_acronym pass uppercases
    // "Ai" -> "AI" so the final output is "AI Detect".
    std::string unkeyed_text = label_text_for("ai_detect");
    REQUIRE(unkeyed_text == "AI Detect");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: clear() drops rows and resets subjects",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    auto set = make_multi_category_set();

    renderer.populate(container, set, nullptr, nullptr);
    REQUIRE(renderer.row_count() == 3);

    renderer.clear();
    REQUIRE(renderer.row_count() == 0);
    REQUIRE(renderer.rendered_ids().empty());
    REQUIRE(renderer.get_row("bed_mesh") == nullptr);
}

// T4: integration — manager reads option state through a provider that
// delegates to the renderer. Mirrors the production wiring in
// PrintSelectDetailView::populate_option_rows() where the renderer
// becomes the source of truth for per-option toggle state.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: provider integration with PrintPreparationManager",
                 "[print_file_detail][pre_print_options][integration]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    auto set = make_multi_category_set();
    renderer.populate(container, set, nullptr, nullptr);
    REQUIRE(renderer.row_count() == 3);

    PrintPreparationManager manager;
    manager.set_option_state_provider(
        [&renderer](const std::string& id) { return renderer.get_state(id, -1); });

    SECTION("Initial provider readouts match default_enabled") {
        // bed_mesh: default_enabled=true → ENABLED
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::ENABLED);
        // nozzle_clean: default_enabled=false → DISABLED
        REQUIRE(manager.get_option_state("nozzle_clean") == PrePrintOptionState::DISABLED);
        // ai_detect: default_enabled=false → DISABLED
        REQUIRE(manager.get_option_state("ai_detect") == PrePrintOptionState::DISABLED);
    }

    SECTION("Programmatic state change is reflected in manager reads") {
        renderer.set_state("bed_mesh", 0);
        REQUIRE(manager.get_option_state("bed_mesh") == PrePrintOptionState::DISABLED);

        renderer.set_state("ai_detect", 1);
        REQUIRE(manager.get_option_state("ai_detect") == PrePrintOptionState::ENABLED);
    }

    SECTION("Unknown id falls through to NOT_APPLICABLE") {
        // The provider returns -1 for unknown ids (renderer.get_state default),
        // and the manager has no cached options without a printer_state, so
        // the result is NOT_APPLICABLE.
        REQUIRE(manager.get_option_state("does_not_exist") == PrePrintOptionState::NOT_APPLICABLE);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: plugin visibility subject is tri-state",
                 "[print_file_detail][pre_print_options][preprint][plugin_gate]") {
    // Mirrors the plugin-gate wiring: the visibility subject is the
    // helix_plugin_installed tri-state (-1 unknown/checking, 0 confirmed-absent,
    // 1 present). Rows stay visible during the -1 startup window and only hide
    // on a confirmed 0.
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());

    PrePrintOptionSet set;
    set.macro_name = "START_PRINT";
    PrePrintOption opt;
    opt.id = "bed_mesh";
    opt.category = PrePrintCategory::Mechanical;
    opt.order = 10;
    opt.default_enabled = true;
    opt.strategy_kind = PrePrintStrategyKind::MacroParam;
    opt.strategy = PrePrintStrategyMacroParam{"SKIP_LEVELING", "0", "1", "0"};
    set.options.push_back(opt);

    lv_subject_t plugin_installed{};
    lv_subject_init_int(&plugin_installed, -1); // unknown / still checking

    auto vis_lookup = [&](const std::string& id) -> lv_subject_t* {
        return id == "bed_mesh" ? &plugin_installed : nullptr;
    };

    renderer.populate(container, set, vis_lookup, nullptr);
    lv_obj_t* row = renderer.get_row("bed_mesh");
    REQUIRE(row != nullptr);

    // -1 (unknown): row visible so we never flash-hide during startup.
    REQUIRE_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // 0 (confirmed absent): hidden.
    lv_subject_set_int(&plugin_installed, 0);
    REQUIRE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    // 1 (present): visible again.
    lv_subject_set_int(&plugin_installed, 1);
    REQUIRE_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    renderer.clear();
    lv_subject_deinit(&plugin_installed);
}

// ============================================================================
// PrePrintOption::requires_macro — is_macro_gate_closed + filter_macro_gated_options
//
// Issue #1122 regression: a `requires_macro` option whose macro isn't
// registered with Klipper must NOT render a toggle row. The toggle would be
// inert (collect_pre_start_gcode_lines drops its gcode), so showing it
// contradicts PrePrintOption::requires_macro's doc ("Hide and skip this
// option when the named macro is not present on the printer").
//
// populate_option_rows() now applies filter_macro_gated_options() before
// handing the set to the renderer. These tests lock in the helper contract
// AND an end-to-end "K2 Plus + no LOAD_AI_RUN -> zero ai_detect rows"
// regression using the live printer DB entry.
// ============================================================================

TEST_CASE("is_macro_gate_closed: empty requires_macro is never gated",
          "[print_file_detail][pre_print_options][requires_macro]") {
    helix::MacroParamCache::instance().clear();

    PrePrintOption opt;
    opt.id = "bed_mesh";
    opt.requires_macro = ""; // No gate declared.
    REQUIRE_FALSE(is_macro_gate_closed(opt));
}

TEST_CASE("is_macro_gate_closed: declared macro absent -> gated",
          "[print_file_detail][pre_print_options][requires_macro]") {
    helix::MacroParamCache::instance().clear();

    PrePrintOption opt;
    opt.id = "ai_detect";
    opt.requires_macro = "LOAD_AI_RUN"; // Not in the (cleared) cache.
    REQUIRE(is_macro_gate_closed(opt));
}

TEST_CASE("is_macro_gate_closed: declared macro present -> not gated",
          "[print_file_detail][pre_print_options][requires_macro]") {
    helix::MacroParamCache::instance().clear();
    nlohmann::json config = nlohmann::json::object();
    config["gcode_macro LOAD_AI_RUN"] = {{"gcode", "{action_respond_info('ai run')}"}};
    helix::MacroParamCache::instance().populate_from_configfile(config, {"LOAD_AI_RUN"});

    PrePrintOption opt;
    opt.id = "ai_detect";
    opt.requires_macro = "LOAD_AI_RUN";
    REQUIRE_FALSE(is_macro_gate_closed(opt));

    helix::MacroParamCache::instance().clear();
}

TEST_CASE("is_macro_gate_closed: case-insensitive macro lookup",
          "[print_file_detail][pre_print_options][requires_macro]") {
    // MacroParamCache lowercases keys; requires_macro values in
    // printer_database.json use uppercase ("LOAD_AI_RUN"). Confirms the
    // predicate survives the case mismatch the same way has_macro() does.
    helix::MacroParamCache::instance().clear();
    nlohmann::json config = nlohmann::json::object();
    config["gcode_macro LOAD_AI_RUN"] = {{"gcode", "{action_respond_info('ai run')}"}};
    helix::MacroParamCache::instance().populate_from_configfile(config, {"LOAD_AI_RUN"});

    PrePrintOption opt;
    opt.id = "ai_detect";
    opt.requires_macro = "load_ai_run"; // lowercase, should still match.
    REQUIRE_FALSE(is_macro_gate_closed(opt));

    helix::MacroParamCache::instance().clear();
}

TEST_CASE("filter_macro_gated_options: removes only macro-gated options",
          "[print_file_detail][pre_print_options][requires_macro]") {
    helix::MacroParamCache::instance().clear();

    PrePrintOptionSet input;
    input.macro_name = "START_PRINT";
    input.setup_gcode = "PRINT_PREPARED";

    PrePrintOption bed_mesh;
    bed_mesh.id = "bed_mesh";
    bed_mesh.requires_macro = ""; // No gate.
    bed_mesh.strategy_kind = PrePrintStrategyKind::MacroParam;
    bed_mesh.strategy = PrePrintStrategyMacroParam{"SKIP_BED_MESH", "0", "1", "0"};

    PrePrintOption ai_detect;
    ai_detect.id = "ai_detect";
    ai_detect.requires_macro = "LOAD_AI_RUN"; // Gated, absent from cache.
    ai_detect.strategy_kind = PrePrintStrategyKind::PreStartGcode;
    ai_detect.strategy = PrePrintStrategyPreStartGcode{"LOAD_AI_RUN SWITCH={value}"};

    input.options = {bed_mesh, ai_detect};

    PrePrintOptionSet out = filter_macro_gated_options(input);

    REQUIRE(out.options.size() == 1);
    REQUIRE(out.options[0].id == "bed_mesh");
    // macro_name + setup_gcode pass through untouched.
    REQUIRE(out.macro_name == "START_PRINT");
    REQUIRE(out.setup_gcode == "PRINT_PREPARED");
}

TEST_CASE("filter_macro_gated_options: preserves all options when no macros are gated",
          "[print_file_detail][pre_print_options][requires_macro]") {
    helix::MacroParamCache::instance().clear();
    nlohmann::json config = nlohmann::json::object();
    config["gcode_macro LOAD_AI_RUN"] = {{"gcode", "{action_respond_info('ai run')}"}};
    helix::MacroParamCache::instance().populate_from_configfile(config, {"LOAD_AI_RUN"});

    auto set = make_multi_category_set(); // bed_mesh, nozzle_clean, ai_detect
    // make_multi_category_set's ai_detect has empty requires_macro by default;
    // assign one to exercise the "macro present" path.
    set.options[2].requires_macro = "LOAD_AI_RUN";

    PrePrintOptionSet out = filter_macro_gated_options(set);
    REQUIRE(out.options.size() == set.options.size());

    helix::MacroParamCache::instance().clear();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: K2 Plus without LOAD_AI_RUN renders no ai_detect row "
                 "(issue #1122 regression)",
                 "[print_file_detail][pre_print_options][requires_macro][db][ai_detect]") {
    // Issue #1122: K2 Plus declares ai_detect with requires_macro=LOAD_AI_RUN.
    // Stock firmware that doesn't register LOAD_AI_RUN must NOT render the
    // ai_detect toggle — it would be inert. populate_option_rows() now applies
    // filter_macro_gated_options() before handing the set to the renderer; this
    // test reproduces that pipeline against the live DB entry.
    helix::MacroParamCache::instance().clear(); // Stock K2 Plus, no LOAD_AI_RUN.

    auto set = PrinterDetector::get_pre_print_option_set("Creality K2 Plus");
    REQUIRE_FALSE(set.empty());

    PrePrintOptionSet filtered = filter_macro_gated_options(set);

    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, filtered, nullptr, nullptr);

    // bed_mesh still renders (no requires_macro).
    REQUIRE(renderer.get_row("bed_mesh") != nullptr);
    // ai_detect must be filtered out — its required macro isn't registered.
    REQUIRE(renderer.get_row("ai_detect") == nullptr);
    REQUIRE(renderer.row_count() == filtered.options.size());
    REQUIRE(filtered.options.size() < set.options.size());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: K2 Plus with LOAD_AI_RUN still renders ai_detect row",
                 "[print_file_detail][pre_print_options][requires_macro][db][ai_detect]") {
    // Positive-path companion to the regression above: when LOAD_AI_RUN IS
    // registered (Creality OS variants), ai_detect renders normally.
    helix::MacroParamCache::instance().clear();
    nlohmann::json config = nlohmann::json::object();
    config["gcode_macro LOAD_AI_RUN"] = {{"gcode", "{action_respond_info('ai run')}"}};
    helix::MacroParamCache::instance().populate_from_configfile(config, {"LOAD_AI_RUN"});

    auto set = PrinterDetector::get_pre_print_option_set("Creality K2 Plus");
    PrePrintOptionSet filtered = filter_macro_gated_options(set);

    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, filtered, nullptr, nullptr);

    REQUIRE(renderer.get_row("bed_mesh") != nullptr);
    REQUIRE(renderer.get_row("ai_detect") != nullptr);
    REQUIRE(renderer.row_count() == set.options.size());

    helix::MacroParamCache::instance().clear();
}
