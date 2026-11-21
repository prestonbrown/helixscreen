// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_color_picker_modal.h"
#include "ui_swatch.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_geometry.h"
#include "../ui_test_utils.h"
#include "color_picker_model.h"
#include "display_settings_manager.h"
#include "lvgl/src/misc/lv_area_private.h" // lv_area_is_in

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

extern "C" {
#include "helix-xml/src/xml/lv_xml.h"
}

TEST_CASE("swatch_needs_edge: a swatch too close to its surface gets an edge, in either theme",
          "[color_swatch]") {
    constexpr uint32_t kDarkScreen = 0x2E3440;
    constexpr uint32_t kDarkDialog = 0x4C566A;
    constexpr uint32_t kLightScreen = 0xECEFF4;
    constexpr uint32_t kLightDialog = 0xEDEFF6;
    // Dark-on-dark.
    CHECK(helix::ui::swatch_needs_edge(0x000000, kDarkScreen));
    CHECK(helix::ui::swatch_needs_edge(0x1A1A1A, kDarkScreen));
    CHECK(helix::ui::swatch_needs_edge(0x4A4A4A, kDarkScreen));
    CHECK(helix::ui::swatch_needs_edge(0x4A4A4A, kDarkDialog));
    // Light-on-light.
    CHECK(helix::ui::swatch_needs_edge(0xFFFFFF, kLightScreen));
    CHECK(helix::ui::swatch_needs_edge(0xE8E8E8, kLightDialog));
    CHECK(helix::ui::swatch_needs_edge(0xE0D5C7, kLightScreen));
    // Plenty of contrast: no edge.
    CHECK_FALSE(helix::ui::swatch_needs_edge(0xFFFFFF, kDarkScreen));
    CHECK_FALSE(helix::ui::swatch_needs_edge(0x808080, kDarkScreen));
    CHECK_FALSE(helix::ui::swatch_needs_edge(0x1A1A1A, kLightScreen));
    CHECK_FALSE(helix::ui::swatch_needs_edge(0xE53935, kLightDialog));
}

namespace {

/// Integer <px> tokens of one ui_xml file, keyed by full suffixed name. Values
/// that are not bare integers (token references) are skipped: no ladder this
/// test reads uses them.
std::map<std::string, int> read_px_tokens(const char* xml_path) {
    std::ifstream f(xml_path);
    REQUIRE(f.good()); // a missing token file must fail loudly, not vacuously
    std::map<std::string, int> tokens;
    std::string line;
    while (std::getline(f, line)) {
        const auto name_at = line.find("<px name=\"");
        if (name_at == std::string::npos)
            continue;
        const auto name_start = name_at + 10;
        const auto name_end = line.find('"', name_start);
        if (name_end == std::string::npos)
            continue;
        const auto value_at = line.find("value=\"", name_end);
        if (value_at == std::string::npos)
            continue;
        const auto value_start = value_at + 7;
        const auto value_end = line.find('"', value_start);
        try {
            tokens[line.substr(name_start, name_end - name_start)] =
                std::stoi(line.substr(value_start, value_end - value_start));
        } catch (const std::exception&) {
            // Not a bare integer (a token reference): not one of ours.
        }
    }
    return tokens;
}

/// The tier fallback chain the runtime resolver applies
/// (src/ui/theme_responsive.cpp theme_manager_pick_px_tier): micro falls to
/// tiny then small, tiny to small, and xlarge/xxlarge walk back down to large.
/// Mirrored here so the test asserts the values a screen of each class
/// actually resolves, not just the tiers that define their own entries.
int tier_value(const std::map<std::string, int>& tokens, const std::string& base,
               const char* suffix) {
    std::vector<std::string> chain{base + suffix};
    if (strcmp(suffix, "_micro") == 0)
        chain.push_back(base + "_tiny");
    if (strcmp(suffix, "_micro") == 0 || strcmp(suffix, "_tiny") == 0)
        chain.push_back(base + "_small");
    if (strcmp(suffix, "_xxlarge") == 0)
        chain.push_back(base + "_xlarge");
    if (strcmp(suffix, "_xxlarge") == 0 || strcmp(suffix, "_xlarge") == 0)
        chain.push_back(base + "_large");
    for (const auto& key : chain) {
        const auto it = tokens.find(key);
        if (it != tokens.end())
            return it->second;
    }
    FAIL("no tier of " << base << " resolves for class " << suffix);
    return 0;
}

} // namespace

TEST_CASE("swatch token ladders keep every touch target at the floor, per size class",
          "[color_swatch][swatch]") {
    const auto globals = read_px_tokens("ui_xml/globals.xml");
    const auto led = read_px_tokens("ui_xml/led_tokens.xml");

    // The swatch floor the AMS edit overlay's grid landed: a swatch's whole
    // target (disc + hit reach) clears 44px. Tappable controls have their own
    // taller floor (#touch_target_min, 48) - which must never drop below the
    // swatch floor, or the grids would outrank the buttons.
    constexpr int kSwatchFloor = 44;
    REQUIRE(globals.count("touch_target_min") == 1);
    REQUIRE(globals.at("touch_target_min") >= kSwatchFloor);

    struct Ladder {
        const char* label;
        const std::map<std::string, int>* tokens;
        const char *size, *gap, *half;
    };
    const Ladder ladders[] = {
        {"swatch_grid", &globals, "swatch_grid_size", "swatch_grid_gap", "swatch_grid_gap_half"},
        {"led_swatch", &led, "led_swatch_size", "led_swatch_gap", "led_swatch_gap_half"},
    };
    const char* classes[] = {"_micro", "_tiny",   "_small",  "_medium",
                             "_large", "_xlarge", "_xxlarge"};

    for (const auto& ladder : ladders) {
        for (const char* cls : classes) {
            INFO(ladder.label << cls);
            const int size = tier_value(*ladder.tokens, ladder.size, cls);
            const int gap = tier_value(*ladder.tokens, ladder.gap, cls);
            const int half = tier_value(*ladder.tokens, ladder.half, cls);
            // The whole target - disc plus its hit reach - clears the floor...
            CHECK(size + 2 * half >= kSwatchFloor);
            // ...and the gap swallows both neighbours' reach, so adjacent
            // targets abut without overlapping.
            CHECK(gap >= 2 * half);
        }
    }

    // The shared grid keeps its 6-column shape: 6 swatches and 5 gaps.
    for (const char* cls : classes) {
        INFO("swatch_grid_width" << cls);
        CHECK(tier_value(globals, "swatch_grid_width", cls) ==
              6 * tier_value(globals, "swatch_grid_size", cls) +
                  5 * tier_value(globals, "swatch_grid_gap", cls));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "ColorPickerModal: the picker fits its card at every size",
                 "[color_swatch][color_picker][layout]") {
    auto& display = helix::DisplaySettingsManager::instance();
    const bool animations = display.get_animations_enabled();
    display.set_animations_enabled(false);
    struct Size {
        int32_t w, h;
    };
    for (const Size sz :
         {Size{480, 320}, Size{320, 480}, Size{480, 272}, Size{480, 400}, Size{800, 480}}) {
        for (const auto palette : {helix::ColorPalette::Filament, helix::ColorPalette::Led,
                                   helix::ColorPalette::Theme}) {
            CAPTURE(sz.w, sz.h, static_cast<int>(palette));
            helix::test::ScopedGeometry geometry(sz.w, sz.h);
            helix::ui::ColorPickerModal picker;
            helix::ui::ColorPickerConfig cfg;
            cfg.palette = palette;
            picker.set_config(cfg);
            REQUIRE(picker.show_with_color(test_screen(), 0x808080));
            lv_obj_t* dialog = picker.dialog();
            REQUIRE(dialog != nullptr);
            lv_obj_update_layout(dialog);

            lv_area_t card;
            lv_obj_get_coords(dialog, &card);
            const lv_area_t screen{0, 0, sz.w - 1, sz.h - 1};
            CAPTURE(card.x1, card.y1, card.x2, card.y2);
            CHECK(lv_area_is_in(&card, &screen, 0));
            CHECK(lv_obj_get_scroll_bottom(dialog) <= 0);
            CHECK(lv_obj_get_scroll_right(dialog) <= 0);

            // Every grid cell and the strip lie inside the card, and a cell is
            // tall enough to hit.
            lv_obj_t* grid_tab = lv_obj_find_by_name(dialog, "cp_grid_tab");
            lv_obj_t* strip = lv_obj_find_by_name(dialog, "cp_strip");
            REQUIRE(grid_tab != nullptr);
            REQUIRE(strip != nullptr);
            lv_area_t g;
            lv_area_t st;
            lv_obj_get_coords(grid_tab, &g);
            lv_obj_get_coords(strip, &st);
            CAPTURE(g.x1, g.y1, g.x2, g.y2, st.y2);
            CHECK(lv_area_is_in(&g, &card, 0));
            CHECK(lv_area_is_in(&st, &card, 0));
            const int rows = static_cast<int>(helix::color_palette(palette).size()) /
                             helix::color_palette_columns(palette);
            CHECK(lv_area_get_height(&g) / rows >= 20);
            picker.hide();
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(10);
        }
    }
    display.set_animations_enabled(animations);
}
