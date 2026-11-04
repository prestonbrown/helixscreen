// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_manager_pins.cpp
 * @brief Pins the observable behaviour of the theme manager: the XML constant
 *        set it registers, the change-generation subject, live recolouring of
 *        inline colours, and the lifetime of the subjects it publishes.
 *
 * The constant snapshot lives in tests/fixtures/theme_const_golden.txt. After an
 * intentional token change, regenerate it with
 * `HELIX_UPDATE_GOLDEN=1 ./build/bin/helix-tests "[theme_pins]"` and review the diff.
 */

#include "../../src/ui/theme/theme_manager_internal.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_loader.h"
#include "theme_manager.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

std::string golden_path() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    const std::string root = pos != std::string::npos ? src.substr(0, pos) + "/" : "";
    return root + "tests/fixtures/theme_const_golden.txt";
}

/// Every token name the theme system is responsible for, read back through the
/// XML constant registry the way a widget would.
std::set<std::string> token_names() {
    std::set<std::string> names;
    for (const char* type : {"color", "px", "string"}) {
        for (const auto& [name, value] : theme_manager_parse_all_xml_for_element("ui_xml", type)) {
            names.insert(name);
        }
    }
    for (const char* suffix : {"_light", "_dark", "_micro", "_tiny", "_small", "_medium", "_large",
                               "_xlarge", "_xxlarge"}) {
        for (const char* type : {"color", "px", "string"}) {
            for (const auto& [name, value] :
                 theme_manager_parse_all_xml_for_suffix("ui_xml", type, suffix)) {
                names.insert(name);
            }
        }
    }
    for (const char* name : helix::ModePalette::color_names()) {
        names.insert(name);
        names.insert(std::string(name) + "_light");
        names.insert(std::string(name) + "_dark");
    }
    for (const char* name :
         {"border_radius", "border_width", "border_opacity", "shadow_intensity", "shadow_opa",
          "shadow_offset_y", "shadow_cast", "overlay_shadow_opa", "overlay_width_transient",
          "overlay_width_destination", "nav_width", "font_body", "font_small", "font_heading",
          "text_on_primary", "text_on_danger", "chart_measured"}) {
        names.insert(name);
    }
    for (int i = 1; i <= 8; ++i) {
        names.insert("object_color_" + std::to_string(i));
    }
    return names;
}

std::string snapshot(int32_t w, int32_t h, bool dark) {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution res(disp, w, h);
    theme_manager_refresh_layout_constants(disp);
    theme_manager_apply_theme(helix::get_builtin_fallback_theme(), dark);

    std::ostringstream out;
    out << "## " << w << "x" << h << (dark ? " dark" : " light") << "\n";
    for (const auto& name : token_names()) {
        const char* value = lv_xml_get_const_silent(nullptr, name.c_str());
        out << name << "=" << (value ? value : "<unset>") << "\n";
    }
    // Mode-aware resolution: the base names themselves are first-wins, so the
    // light/dark difference is only visible through the accessor.
    // Sorted: the parser returns an unordered_map, whose iteration order differs
    // between standard libraries, and the snapshot must be byte-stable.
    const auto light_parsed = theme_manager_parse_all_xml_for_suffix("ui_xml", "color", "_light");
    const std::map<std::string, std::string> light_colors(light_parsed.begin(), light_parsed.end());
    char hex[16];
    for (const auto& [base, v] : light_colors) {
        snprintf(hex, sizeof(hex), "%06x",
                 lv_color_to_u32(theme_manager_get_color(base.c_str())) & 0xFFFFFF);
        out << "color:" << base << "=" << hex << "\n";
    }
    for (const char* name : helix::ModePalette::color_names()) {
        snprintf(hex, sizeof(hex), "%06x",
                 lv_color_to_u32(theme_manager_get_color(name)) & 0xFFFFFF);
        out << "palette:" << name << "=" << hex << "\n";
    }
    return out.str();
}

int generation() {
    return lv_subject_get_int(theme_manager_get_changed_subject());
}

/// Puts the fixture's theme and mode back so later cases see what they expect.
struct RestoreTheme {
    helix::ThemeData theme = theme_manager_get_active_theme();
    bool dark = theme_manager_is_dark_mode();
    ~RestoreTheme() {
        theme_manager_apply_theme(theme, dark);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "theme constants match the golden snapshot",
                 "[theme_pins][theme]") {
    RestoreTheme restore;

    std::string actual;
    for (const auto& [w, h] : {std::pair<int32_t, int32_t>{480, 272}, {800, 480}, {1280, 720}}) {
        for (bool dark : {true, false}) {
            actual += snapshot(w, h, dark);
        }
    }

    if (std::getenv("HELIX_UPDATE_GOLDEN")) {
        std::ofstream(golden_path()) << actual;
        WARN("golden snapshot rewritten: " << golden_path());
        return;
    }

    std::ifstream f(golden_path());
    REQUIRE(f.good());
    std::stringstream expected;
    expected << f.rdbuf();

    // Compare line by line so a failure names the token, not a 150KB blob.
    std::istringstream a(actual), e(expected.str());
    std::string al, el, section;
    int line = 0;
    while (true) {
        const bool ha = static_cast<bool>(std::getline(a, al));
        const bool he = static_cast<bool>(std::getline(e, el));
        if (!ha && !he)
            break;
        ++line;
        if (ha && al.rfind("## ", 0) == 0)
            section = al;
        INFO("section " << section << ", line " << line);
        REQUIRE(al == el);
    }
}

// theme_manager_init scans every top-level ui_xml file, component-local <consts> included,
// and the global scope is first-registration-wins. A case that inits the theme without
// globals.xml loaded would otherwise let timelapse_video_card's px card_height (220)
// shadow globals.xml's 80% for the rest of the process.
TEST_CASE_METHOD(LVGLTestFixture, "theme init on a bare fixture leaves globals.xml consts winning",
                 "[theme_pins][theme]") {
    const bool original_dark = theme_manager_is_dark_mode();
    theme_manager_init(lv_display_get_default(), original_dark);

    const char* card_height = lv_xml_get_const_silent(nullptr, "card_height");
    REQUIRE(card_height != nullptr);
    CHECK(std::string(card_height) == "80%");
}

TEST_CASE_METHOD(LVGLUITestFixture, "theme_changed generation bumps on apply and toggle",
                 "[theme_pins][theme]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();

    const int g0 = generation();
    theme_manager_apply_theme(theme, true);
    CHECK(generation() == g0 + 1);

    theme_manager_apply_theme(theme, false);
    CHECK(generation() == g0 + 2);

    theme_manager_toggle_dark_mode();
    CHECK(generation() == g0 + 3);
    CHECK(theme_manager_is_dark_mode());

    theme_manager_notify_change();
    CHECK(generation() == g0 + 4);
}

TEST_CASE_METHOD(LVGLUITestFixture, "inline card_bg and border recolor on a theme switch",
                 "[theme_pins][theme]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);

    const lv_color_t dark_card = theme_manager_parse_hex_color(theme.dark.card_bg.c_str());
    const lv_color_t light_card = theme_manager_parse_hex_color(theme.light.card_bg.c_str());
    const lv_color_t dark_border = theme_manager_parse_hex_color(theme.dark.border.c_str());
    const lv_color_t light_border = theme_manager_parse_hex_color(theme.light.border.c_str());
    REQUIRE_FALSE(lv_color_eq(dark_card, light_card));
    REQUIRE_FALSE(lv_color_eq(dark_border, light_border));

    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, dark_card, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, dark_border, LV_PART_MAIN);

    theme_manager_apply_theme(theme, false);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(box, LV_PART_MAIN), light_card));
    CHECK(lv_color_eq(lv_obj_get_style_border_color(box, LV_PART_MAIN), light_border));

    theme_manager_toggle_dark_mode();
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(box, LV_PART_MAIN), dark_card));
    CHECK(lv_color_eq(lv_obj_get_style_border_color(box, LV_PART_MAIN), dark_border));
}

namespace {
void noop_cb(lv_observer_t*, lv_subject_t*) {}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "theme subjects exist once the theme is initialized",
                 "[theme_pins][theme]") {
    std::vector<lv_subject_t*> subjects;
    for (const char* name :
         {"ui_breakpoint", "ui_breakpoint_v", "ui_is_portrait", "ui_is_ultrawide"}) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        subjects.push_back(s);
    }
    CHECK(subjects[0] == theme_manager_get_breakpoint_subject());
    for (int i = 0; i < 16; ++i) {
        lv_subject_t* s =
            lv_xml_get_subject(nullptr, ("swatch_" + std::to_string(i) + "_desc").c_str());
        REQUIRE(s != nullptr);
    }
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "swatch_0_desc"))) ==
          "App background");
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "swatch_15_desc"))) ==
          "Focus ring");
    CHECK(theme_manager_get_changed_subject() != nullptr);
}

// theme_manager_deinit() delegates to this. It runs on a local instance because
// tearing down the process-wide subjects mid-test would strip observers that
// other modules hold for the life of the fixture.
TEST_CASE_METHOD(LVGLTestFixture,
                 "ThemeSubjects::deinit drops every observer and resets the generation",
                 "[theme_pins][theme]") {
    helix::theme_detail::ThemeSubjects subs;
    lv_subject_init_int(&subs.changed, 0);
    subs.changed_ready = true;
    subs.generation = 7;
    lv_subject_init_int(&subs.breakpoint, 2);
    subs.breakpoint_ready = true;
    lv_subject_init_int(&subs.breakpoint_v, 3);
    subs.breakpoint_v_ready = true;
    lv_subject_init_int(&subs.is_portrait, 0);
    subs.is_portrait_ready = true;
    lv_subject_init_int(&subs.is_ultrawide, 0);
    subs.is_ultrawide_ready = true;
    for (size_t i = 0; i < subs.kSwatchCount; ++i) {
        lv_subject_init_string(&subs.swatch_desc[i], subs.swatch_bufs[i], nullptr,
                               subs.kSwatchBufSize, "x");
    }
    subs.swatch_ready = true;

    std::vector<lv_subject_t*> all = {&subs.changed, &subs.breakpoint, &subs.breakpoint_v,
                                      &subs.is_portrait, &subs.is_ultrawide};
    for (auto& s : subs.swatch_desc)
        all.push_back(&s);
    for (lv_subject_t* s : all) {
        lv_subject_add_observer(s, noop_cb, nullptr);
        REQUIRE(lv_ll_get_len(&s->subs_ll) == 1);
    }

    subs.deinit();

    for (lv_subject_t* s : all) {
        CHECK(lv_ll_get_len(&s->subs_ll) == 0);
    }
    CHECK_FALSE(subs.changed_ready);
    CHECK_FALSE(subs.breakpoint_ready);
    CHECK_FALSE(subs.breakpoint_v_ready);
    CHECK_FALSE(subs.is_portrait_ready);
    CHECK_FALSE(subs.is_ultrawide_ready);
    CHECK_FALSE(subs.swatch_ready);
    CHECK(subs.generation == 0);

    // A second deinit is a no-op rather than a double free.
    subs.deinit();
}

// Every size-suffix site must agree on the ladder: the breakpoint suffix, the
// tier number, and the "has a variant-selected base name" test.
TEST_CASE("size suffix ladder agrees across its users", "[theme_pins][theme]") {
    struct Row {
        const char* suffix;
        int tier;
        int32_t resolution; // a cramped-axis size that classifies into the tier
    };
    const Row rows[] = {{"_micro", 0, 272},   {"_tiny", 1, 390},  {"_small", 2, 460},
                        {"_medium", 3, 550},  {"_large", 4, 700}, {"_xlarge", 5, 1000},
                        {"_xxlarge", 6, 1001}};
    for (const Row& r : rows) {
        INFO(r.suffix);
        CHECK(helix::theme_detail::tier_for_suffix(r.suffix) == r.tier);
        CHECK(std::string(theme_manager_get_breakpoint_suffix(r.resolution)) == r.suffix);
        CHECK(helix::theme_detail::has_dynamic_suffix(std::string("space_md") + r.suffix));
        CHECK_FALSE(helix::theme_detail::has_dynamic_suffix(r.suffix));
    }
    CHECK(helix::theme_detail::has_dynamic_suffix("screen_bg_light"));
    CHECK(helix::theme_detail::has_dynamic_suffix("screen_bg_dark"));
    CHECK_FALSE(helix::theme_detail::has_dynamic_suffix("space_md"));
    CHECK_FALSE(helix::theme_detail::has_dynamic_suffix("space_md_larger"));
    CHECK(helix::theme_detail::tier_for_suffix("_light") == -1);
    CHECK(helix::theme_detail::tier_for_suffix("") == -1);
}

namespace {
int g_changed_fires = 0;
void count_cb(lv_observer_t*, lv_subject_t*) {
    ++g_changed_fires;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "a repeat init keeps the theme_changed generation",
                 "[theme_pins][theme]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);
    theme_manager_toggle_dark_mode();
    const int g = generation();
    REQUIRE(g > 0);

    lv_observer_t* obs =
        lv_subject_add_observer(theme_manager_get_changed_subject(), count_cb, nullptr);
    g_changed_fires = 0; // add_observer notifies once with the current value

    // A different mode forces a full rebuild without a deinit in between.
    const int rebuilds = theme_manager_full_init_count();
    theme_manager_init(lv_display_get_default(), !theme_manager_is_dark_mode());
    REQUIRE(theme_manager_full_init_count() == rebuilds + 1);

    CHECK(generation() == g);
    CHECK(g_changed_fires == 0);
    lv_observer_remove(obs);
}
