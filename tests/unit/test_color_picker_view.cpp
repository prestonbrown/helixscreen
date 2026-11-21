// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_color_picker_modal.h"
#include "ui_color_picker_view.h"
#include "ui_swatch.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "color_recents.h"
#include "config.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ColorPalette;
using helix::ColorRecents;
using helix::HsvChannel;
using helix::ui::ColorPickerConfig;
using helix::ui::ColorPickerView;
using helix::ui::ColorStrip;

namespace {

void forget_recents() {
    auto& data = helix::ConfigTestAccess::data(*helix::Config::get_instance());
    if (data.is_object()) {
        data.erase("color_recents");
    }
}

int subject_int(const char* name) {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
}

std::string subject_string(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}

lv_obj_t* named(lv_obj_t* root, const char* name) {
    return lv_obj_find_by_name(root, name);
}

struct PickerFixture : LVGLUITestFixture {
    PickerFixture() {
        forget_recents();
    }
    ColorPickerView view;
    std::vector<uint32_t> changes;

    void attach(uint32_t rgb, ColorPickerConfig cfg = {}) {
        view.attach(test_screen(), rgb, cfg);
        REQUIRE(view.root() != nullptr);
        view.on_change([this](uint32_t c) { changes.push_back(c); });
        lv_obj_update_layout(test_screen());
    }
};

} // namespace

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: each palette builds its grid", "[color_picker]") {
    for (auto palette : {ColorPalette::Filament, ColorPalette::Led, ColorPalette::Theme}) {
        ColorPickerConfig cfg;
        cfg.palette = palette;
        attach(0x123456, cfg);
        const size_t n = helix::color_palette(palette).size();
        INFO("palette " << static_cast<int>(palette));
        // One clickable cell per palette entry, last index n - 1 and none beyond.
        int cells = 0;
        REQUIRE(named(view.root(), "cp_grid_tab") != nullptr);
        std::vector<lv_obj_t*> stack{named(view.root(), "cp_grid_tab")};
        while (!stack.empty()) {
            lv_obj_t* o = stack.back();
            stack.pop_back();
            if (helix::ui::has_swatch_paint(o) && lv_obj_is_clickable(o)) {
                ++cells;
            }
            for (uint32_t c = 0; c < lv_obj_get_child_count(o); ++c) {
                stack.push_back(lv_obj_get_child(o, static_cast<int32_t>(c)));
            }
        }
        CHECK(cells == static_cast<int>(n));
    }
}

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: a grid tap picks the cell and rings it",
                 "[color_picker]") {
    attach(0x123456);
    CHECK(subject_int("color_picker_cell_selected") == -1);
    view.pick_cell(31);
    const uint32_t expected = helix::color_palette(ColorPalette::Filament)[31];
    CHECK(view.color() == expected);
    CHECK(changes == std::vector<uint32_t>{expected});
    CHECK(subject_int("color_picker_cell_selected") == 31);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%06X", expected);
    CHECK(subject_string("color_picker_hex") == hex);
}

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: set_color shows a color without reporting it",
                 "[color_picker]") {
    attach(0x123456);
    view.set_color(0xFFFFFF);
    CHECK(view.color() == 0xFFFFFF);
    CHECK(changes.empty());
    CHECK(subject_int("color_picker_cell_selected") == 0);
}

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: sliders and steppers edit HSV",
                 "[color_picker]") {
    attach(0xFF0000);
    view.set_tab(ColorPickerView::Tab::Sliders);
    CHECK(subject_int("color_picker_tab") == 1);
    view.step(HsvChannel::Hue, -1);
    CHECK(subject_int("color_picker_hue") == 358);
    CHECK(subject_string("color_picker_hue_text") == "358\xC2\xB0");

    // A drag writes the slider's bound subject; the view follows it.
    lv_subject_set_int(lv_xml_get_subject(nullptr, "color_picker_hue"), 240);
    view.slider_changed(HsvChannel::Hue);
    CHECK(view.color() == 0x0000FF);
    CHECK(changes.back() == 0x0000FF);
}

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: the hex keypad applies six digits",
                 "[color_picker]") {
    attach(0x123456);
    view.open_keypad();
    CHECK(subject_int("color_picker_keypad_open") == 1);
    lv_obj_t* ok = named(view.root(), "cp_keypad_ok");
    REQUIRE(ok != nullptr);
    for (char c : std::string("1E244")) {
        view.keypad_key(c);
    }
    CHECK(lv_obj_has_state(ok, LV_STATE_DISABLED));
    view.keypad_ok(); // refused under six digits
    CHECK(view.keypad_open());
    CHECK(changes.empty());

    view.keypad_key('0');
    CHECK_FALSE(lv_obj_has_state(ok, LV_STATE_DISABLED));
    view.keypad_backspace();
    CHECK(lv_obj_has_state(ok, LV_STATE_DISABLED));
    view.keypad_key('1');
    view.keypad_ok();
    CHECK_FALSE(view.keypad_open());
    CHECK(subject_int("color_picker_keypad_open") == 0);
    CHECK(view.color() == 0x1E2441);
    CHECK(changes == std::vector<uint32_t>{0x1E2441});
}

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: keypad Cancel leaves the color",
                 "[color_picker]") {
    attach(0x123456);
    view.open_keypad();
    for (char c : std::string("ABCDEF")) {
        view.keypad_key(c);
    }
    view.keypad_cancel();
    CHECK(view.color() == 0x123456);
    CHECK(changes.empty());
}

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: the strip shows recents and picks from them",
                 "[color_picker]") {
    attach(0x123456);
    const auto seed = ColorRecents::seed(ColorRecents::Kind::Filament);
    CHECK(view.strip_colors() == seed);
    CHECK(subject_int("color_picker_chip_count") == static_cast<int>(seed.size()));
    CHECK(subject_string("color_picker_strip_caption") == "Recent");
    view.pick_chip(1);
    CHECK(view.color() == seed[1]);
    CHECK(subject_int("color_picker_chip_selected") == 1);
    // Browsing does not remember.
    CHECK(ColorRecents::list(ColorRecents::Kind::Filament) == seed);
}

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: a long press removes chips until Done",
                 "[color_picker]") {
    attach(0x123456);
    const auto seed = ColorRecents::seed(ColorRecents::Kind::Filament);
    view.hold_chip(0);
    CHECK(view.strip_editing());
    CHECK(subject_int("color_picker_strip_editing") == 1);
    view.pick_chip(0); // removes, does not pick
    CHECK(view.color() == 0x123456);
    CHECK(changes.empty());
    const auto now = ColorRecents::list(ColorRecents::Kind::Filament);
    CHECK(now == std::vector<uint32_t>(seed.begin() + 1, seed.end()));
    CHECK(view.strip_colors() == now);
    view.finish_strip_edit();
    CHECK_FALSE(view.strip_editing());
    view.pick_chip(0);
    CHECK(view.color() == seed[1]);
}

TEST_CASE_METHOD(PickerFixture, "ColorPickerView: a custom strip is fixed and de-duplicated",
                 "[color_picker]") {
    ColorPickerConfig cfg;
    cfg.palette = ColorPalette::Theme;
    cfg.strip = ColorStrip::Custom;
    cfg.strip_caption = "This theme";
    cfg.custom_strip = [] { return std::vector<uint32_t>{0x111111, 0x222222, 0x111111}; };
    attach(0x123456, cfg);
    CHECK(view.strip_colors() == std::vector<uint32_t>{0x111111, 0x222222});
    CHECK(subject_string("color_picker_strip_caption") == "This theme");
    view.hold_chip(0);
    CHECK_FALSE(view.strip_editing());
}

// ---------------------------------------------------------------------------
// ColorPickerModal (the LED control's picker)
// ---------------------------------------------------------------------------

namespace {

helix::ui::ColorPickerConfig led_config() {
    helix::ui::ColorPickerConfig cfg;
    cfg.palette = ColorPalette::Led;
    cfg.strip = ColorStrip::LedRecents;
    return cfg;
}

} // namespace

TEST_CASE_METHOD(PickerFixture, "LED picker: picks preview live and Cancel puts the original back",
                 "[color_picker][led]") {
    helix::ui::ColorPickerModal modal;
    modal.set_config(led_config());
    modal.set_remember_on_done(true);
    std::vector<uint32_t> previews;
    bool done = false;
    modal.set_preview_callback([&](uint32_t c) { previews.push_back(c); });
    modal.set_color_callback([&](uint32_t, const std::string&) { done = true; });
    REQUIRE(modal.show_with_color(test_screen(), 0xFFA757));

    modal.view().pick_cell(10); // full red
    REQUIRE(previews == std::vector<uint32_t>{0xFF0000});

    lv_obj_send_event(named(modal.dialog(), "cpm_cancel"), LV_EVENT_CLICKED, nullptr);
    CHECK_FALSE(modal.is_visible());
    CHECK_FALSE(done);
    CHECK(previews.back() == 0xFFA757);
    CHECK(ColorRecents::list(ColorRecents::Kind::Led) ==
          ColorRecents::seed(ColorRecents::Kind::Led));
}

TEST_CASE_METHOD(PickerFixture, "LED picker: Done keeps the color and adds it to LED recents",
                 "[color_picker][led]") {
    helix::ui::ColorPickerModal modal;
    modal.set_config(led_config());
    modal.set_remember_on_done(true);
    uint32_t chosen = 0;
    modal.set_color_callback([&](uint32_t c, const std::string&) { chosen = c; });
    REQUIRE(modal.show_with_color(test_screen(), 0xFFA757));
    modal.view().pick_cell(18); // purple

    lv_obj_send_event(named(modal.dialog(), "cpm_done"), LV_EVENT_CLICKED, nullptr);
    const uint32_t purple = helix::color_palette(ColorPalette::Led)[18];
    CHECK_FALSE(modal.is_visible());
    CHECK(chosen == purple);
    CHECK(ColorRecents::list(ColorRecents::Kind::Led).front() == purple);
    // The filament list is another list.
    CHECK(ColorRecents::list(ColorRecents::Kind::Filament) ==
          ColorRecents::seed(ColorRecents::Kind::Filament));
}
