// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Layout tests for ActionPromptModal's regular-button rows.
//
// Each prompt_button_group gets its own row, and a run of ungrouped buttons
// shares one. choose_button_row_layout() picks each row's layout: <= 3 buttons
// stay content-sized and wrap; 4 whose labels fit an even share make one
// equal-width row (AFC's "Lane 1".."Lane 4", #1043); 5 or more labelled buttons
// make a grid of equal cells, three across or two when the widest label does
// not fit a third. Text and button rows scroll together in prompt_scroll while
// the footer stays on the card.
//
// Tagged [ui_integration] (NOT hidden) — it shows real widgets and forces a
// real LVGL layout pass, which is the only way the wrap actually manifests.

#include "ui_modal.h"
#include "ui_swatch.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_geometry.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "demo_prompt_lines.h"
#include "display_settings_manager.h"
#include "lvgl/src/misc/lv_area_private.h" // lv_area_is_in
#include "theme_manager.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"

// White-box accessor (declared friend in action_prompt_modal.h) — avoids adding
// _for_testing() methods to the production class ([L065]/[L088]).
struct ActionPromptModalTestAccess {
    static const std::vector<helix::ui::WeakWidget>&
    buttons(const helix::ui::ActionPromptModal& m) {
        return m.created_buttons_;
    }
};

namespace {

helix::PromptData make_regular_prompt(int n_buttons) {
    helix::PromptData data;
    data.title = "Select Lane";
    data.text_lines.push_back("Pick a lane to calibrate");
    for (int i = 0; i < n_buttons; ++i) {
        helix::PromptButton btn;
        btn.label = "Lane " + std::to_string(i + 1);
        btn.gcode = "AFC_CALIBRATION LANE=lane" + std::to_string(i + 1);
        btn.color = "primary";
        btn.is_footer = false;
        data.buttons.push_back(std::move(btn));
    }
    return data;
}

// A prompt parsed from wire lines, as the ctl demos send them.
helix::PromptData parse_prompt(const std::vector<std::string>& lines) {
    helix::ActionPromptManager manager;
    helix::PromptData shown;
    manager.set_on_show([&shown](const helix::PromptData& data) { shown = data; });
    for (const std::string& line : lines) {
        manager.process_line(line);
    }
    return shown;
}

helix::PromptData make_zmod_color_prompt() {
    return parse_prompt(helix::demo::zmod_color_picker_lines());
}

class ActionPromptLayoutFixture : public LVGLUITestFixture {
  public:
    ActionPromptLayoutFixture() {
        prev_animations_ = helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        modal_.set_gcode_callback([](const std::string&) { /* no-op */ });
    }
    ~ActionPromptLayoutFixture() override {
        helix::DisplaySettingsManager::instance().set_animations_enabled(prev_animations_);
    }

    helix::ui::ActionPromptModal modal_;
    bool prev_animations_ = true;
};

} // namespace

// ----------------------------------------------------------------------------
// 4 regular buttons must share a single row (the R2 bug case).
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: 4 regular buttons fit on one row",
                 "[action_prompt][layout][ui_integration]") {
    auto data = make_regular_prompt(4);
    REQUIRE(modal_.show_prompt(test_screen(), data));

    // Force a synchronous layout pass on the whole tree so geometry settles.
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 4);

    // All four buttons must be on the SAME visual row. If the 4th wraps, its y
    // jumps down by ~one button-height (>= ~48px) — far outside this tolerance.
    int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 1; i < btns.size(); ++i) {
        int32_t yi = lv_obj_get_y(btns[i]);
        INFO("button[" << i << "] y=" << yi << " vs button[0] y=" << y0);
        REQUIRE(std::abs(yi - y0) <= 4);
    }

    modal_.hide();
}

// ----------------------------------------------------------------------------
// Regression: 3 regular buttons still share one row AND stay content-sized
// (NOT forced to equal width). Guards against regressing the <= 3 path.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: 3 regular buttons stay content-sized on one row",
                 "[action_prompt][layout][ui_integration]") {
    // Short labels of unequal length: they fit on one content-sized row (as the
    // legacy <= 3 path always has), and content-sizing yields unequal widths —
    // the >= 4 equal-width path would instead force them identical.
    helix::PromptData data;
    data.title = "Confirm";
    data.text_lines.push_back("Choose an action");
    data.buttons.push_back({"OK", "RESUME", "primary", "", false, -1});
    data.buttons.push_back({"Retry", "RETRY", "secondary", "", false, -1});
    data.buttons.push_back({"Cancel", "CANCEL", "error", "", false, -1});

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 3);

    // One row: all share (approximately) the same y.
    int32_t y0 = lv_obj_get_y(btns[0]);
    for (size_t i = 1; i < btns.size(); ++i) {
        REQUIRE(std::abs(lv_obj_get_y(btns[i]) - y0) <= 4);
    }

    // Content-sized: the short and long labels must NOT be equal width.
    int32_t w_ok = lv_obj_get_width(btns[0]);
    int32_t w_long = lv_obj_get_width(btns[1]);
    INFO("w(OK)=" << w_ok << " w(Retry the whole operation)=" << w_long);
    REQUIRE(w_long > w_ok);

    modal_.hide();
}

// ----------------------------------------------------------------------------
// The real AFC calibration prompt: four short lane buttons plus a longer
// "Calibrate All". Whichever layout the fit check picks, no label may be
// clipped — that is the invariant, not the row count.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: AFC calibration buttons are never clipped",
                 "[action_prompt][layout][ui_integration]") {
    // Mirrors src/printer/ams_backend_mock.cpp's simulated AFC_CALIBRATION prompt.
    helix::PromptData data;
    data.title = "AFC Calibration";
    data.text_lines.push_back("Select a lane to calibrate, or calibrate all lanes.");
    for (int i = 1; i <= 4; ++i) {
        data.buttons.push_back({"Lane " + std::to_string(i),
                                "AFC_CALIBRATION LANE=lane" + std::to_string(i), "primary", "",
                                false, -1});
    }
    data.buttons.push_back({"Calibrate All", "AFC_CALIBRATION ALL=1", "secondary", "", false, -1});
    data.buttons.push_back({"Cancel", "AFC_CALIBRATION CANCEL=1", "error", "", true, -1});

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 6); // 5 regular + 1 footer

    // Every regular button must be able to show its label in full.
    for (size_t i = 0; i < 5; ++i) {
        lv_obj_t* label = lv_obj_get_child(btns[i], 0);
        REQUIRE(label != nullptr);
        INFO("button[" << i << "] \"" << data.buttons[i].label << "\" width="
                       << lv_obj_get_width(btns[i]) << " label width=" << lv_obj_get_width(label));
        REQUIRE(lv_obj_get_width(btns[i]) >= lv_obj_get_width(label));
    }

    modal_.hide();
}

// ----------------------------------------------------------------------------
// Many buttons with labels too long to share one row must WRAP, not clip.
//
// The >= 4 rule above is a button *count* test, but the property that actually
// matters is whether the labels fit. A preheat macro that emits 7 material
// presets ("PLA 220/60" ...) gets ~40px per equal-width cell on a 480px card,
// so every label is clipped to a couple of glyphs. Wrapping is the correct
// answer once the row genuinely cannot hold them.
// ----------------------------------------------------------------------------
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: long labels spread over several lines instead of clipping",
                 "[action_prompt][layout][ui_integration]") {
    helix::PromptData data;
    data.title = "Preheat for Load";
    data.text_lines.push_back("Preheat filament and choose a material");
    for (const char* label : {"PLA 220/60", "PETG 240/80", "ABS 250/100", "ASA 260/100",
                              "TPU 230/50", "PC 280/110", "Nylon 260/80"}) {
        data.buttons.push_back(
            {label, std::string("SET_MATERIAL M=") + label, "primary", "", false, -1});
    }

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    if (lv_obj_t* dialog = modal_.dialog()) {
        lv_obj_update_layout(dialog);
    }

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 7);

    // The property under test: every button is wide enough to actually show its
    // label. Under the equal-width single-row layout each cell is far narrower
    // than its text, which is exactly the reported clipping.
    for (size_t i = 0; i < btns.size(); ++i) {
        lv_obj_t* label = lv_obj_get_child(btns[i], 0);
        REQUIRE(label != nullptr);
        int32_t w_btn = lv_obj_get_width(btns[i]);
        int32_t w_lbl = lv_obj_get_width(label);
        INFO("button[" << i << "] \"" << data.buttons[i].label << "\" width=" << w_btn
                       << " label width=" << w_lbl);
        REQUIRE(w_btn >= w_lbl);
    }

    // Not fitting on one row is the direct consequence: they must occupy at
    // least two distinct rows rather than being squeezed into one.
    int32_t y0 = lv_obj_get_y(btns[0]);
    bool any_wrapped = false;
    for (size_t i = 1; i < btns.size(); ++i) {
        if (std::abs(lv_obj_get_y(btns[i]) - y0) > 4) {
            any_wrapped = true;
        }
    }
    REQUIRE(any_wrapped);

    modal_.hide();
}

// ----------------------------------------------------------------------------
// prompt_button_group_start/end: each group is a row of its own, and the
// ungrouped buttons around it are not pulled into it.
// ----------------------------------------------------------------------------
namespace {
int32_t screen_y(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a.y1;
}
} // namespace

TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: each button group is its own row",
                 "[action_prompt][layout][ui_integration]") {
    // Two short ungrouped buttons, then a Yes/No group, then a lone grouped
    // button. Without rows, all five short labels would share one wrapping row.
    helix::PromptData data;
    data.title = "Groups";
    data.buttons = {
        {"A", "A", "", "", false, -1},  {"B", "B", "", "", false, -1},
        {"Yes", "Y", "", "", false, 0}, {"No", "N", "", "", false, 0},
        {"Z", "Z", "", "", false, 1},   {"Cancel", "C", "error", "", true, -1},
    };

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 6);
    const int32_t row_a = screen_y(btns[0]);
    const int32_t row_yes = screen_y(btns[2]);
    const int32_t row_z = screen_y(btns[4]);
    INFO("ungrouped y=" << row_a << " group0 y=" << row_yes << " group1 y=" << row_z);

    CHECK(screen_y(btns[1]) == row_a);   // ungrouped run shares a row
    CHECK(screen_y(btns[3]) == row_yes); // the group shares a row
    CHECK(row_yes > row_a);              // ...below the ungrouped one
    CHECK(row_z > row_yes);              // and the next group is below that

    modal_.hide();
}

// Klipper's secondary is the neutral button, not a success-green one.
TEST_CASE_METHOD(ActionPromptLayoutFixture, "ActionPromptModal: secondary uses the secondary token",
                 "[action_prompt][layout][ui_integration]") {
    helix::PromptData data;
    data.title = "Colors";
    data.buttons = {{"Later", "LATER", "secondary", "", false, -1}};

    REQUIRE(modal_.show_prompt(test_screen(), data));
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 1);

    const lv_color_t bg = lv_obj_get_style_bg_color(btns[0], LV_PART_MAIN);
    CHECK(lv_color_eq(bg, theme_manager_get_color("secondary")));
    CHECK_FALSE(lv_color_eq(bg, theme_manager_get_color("success")));

    modal_.hide();
}

// A hex in the third field (or the legacy fourth) fills the button with that
// color, and the label picks whatever reads on it; a field that is not a color
// leaves the themed default (prestonbrown/helixscreen#1806).
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: hex button fills with the color and keeps text readable",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    using helix::ActionPromptManager;
    helix::PromptData data;
    data.title = "Colors";
    data.buttons = {
        ActionPromptManager::parse_button_spec("Standard|GCODE_CMD_1|error"),
        ActionPromptManager::parse_button_spec("HEX Dark (White Text)|GCODE_CMD_2|ff5722"),
        ActionPromptManager::parse_button_spec("HEX Light (Black Text)|GCODE_CMD_3|#e0e0e0"),
        ActionPromptManager::parse_button_spec("Legacy|GCODE_CMD_4|primary|21cc3d"),
        ActionPromptManager::parse_button_spec("Garbage|GCODE_CMD_5|zzzzzz"),
    };

    REQUIRE(modal_.show_prompt(test_screen(), data));
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 5);

    auto bg = [&](size_t i) { return lv_obj_get_style_bg_color(btns[i], LV_PART_MAIN); };
    CHECK(lv_color_eq(bg(0), theme_manager_get_color("danger")));
    CHECK(lv_color_eq(bg(1), lv_color_hex(0xff5722)));
    CHECK(lv_color_eq(bg(2), lv_color_hex(0xe0e0e0)));
    CHECK(lv_color_eq(bg(3), lv_color_hex(0x21cc3d)));
    CHECK(lv_color_eq(bg(4), theme_manager_get_color("primary")));

    for (size_t i = 0; i < btns.size(); ++i) {
        lv_obj_t* label = lv_obj_get_child(btns[i], 0);
        REQUIRE(label != nullptr);
        const lv_color_t text = lv_obj_get_style_text_color(label, LV_PART_MAIN);
        INFO("button[" << i << "] \"" << data.buttons[i].label << "\"");
        CHECK(helix::contrast_ratio(text, bg(i)) >= kThemeTextContrastThreshold);
    }

    modal_.hide();
}

// A Z-Mod swatch tile carries no text: it shows a circle of its color painted
// by the swatch painter, which owns the contrast edge, and the tile itself
// draws nothing.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a label-less hex button is a swatch",
                 "[action_prompt][layout][ui_integration][hex_color][1806][swatch]") {
    using helix::ActionPromptManager;
    helix::PromptData data;
    data.title = "Select color";
    data.buttons = {
        ActionPromptManager::parse_button_spec("_ |CHANGE_ZCOLOR SLOT=1 HEX=f72224|f72224"),
        ActionPromptManager::parse_button_spec("Named|GCODE|e0e0e0"),
    };

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 2);

    lv_obj_t* disc = lv_obj_find_by_name(btns[0], "color_swatch_disc");
    REQUIRE(disc != nullptr);
    CHECK(helix::ui::has_swatch_paint(disc));
    const auto colors = helix::ui::swatch_paint_colors(disc);
    REQUIRE(colors.size() == 1);
    CHECK(lv_color_eq(colors[0], lv_color_hex(0xf72224)));
    CHECK(lv_color_eq(helix::ui::swatch_color_of(btns[0]), lv_color_hex(0xf72224)));
    CHECK(lv_obj_get_style_radius(disc, LV_PART_MAIN) == LV_RADIUS_CIRCLE);
    CHECK(lv_obj_get_width(disc) == lv_obj_get_height(disc));
    CHECK(lv_obj_get_width(disc) > 0);
    CHECK(lv_obj_get_style_bg_opa(btns[0], LV_PART_MAIN) == LV_OPA_TRANSP);
    CHECK(lv_obj_get_style_border_width(btns[0], LV_PART_MAIN) == 0);

    // A labelled hex button is an ordinary filled button.
    CHECK(lv_obj_find_by_name(btns[1], "color_swatch_disc") == nullptr);
    CHECK(lv_obj_get_style_bg_opa(btns[1], LV_PART_MAIN) == LV_OPA_COVER);

    modal_.hide();
}

// On a 480x272 panel the swatch grid takes the room the screen has: the card
// widens until every tile is a full touch target across, and the tiles grow
// into the free height under the card cap, with nothing pushed off the card.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: swatch grid uses the room a 480x272 screen has",
                 "[action_prompt][layout][ui_integration][hex_color][1806][swatch]") {
    helix::test::ScopedGeometry micro(480, 272);
    REQUIRE(modal_.show_prompt(test_screen(), make_zmod_color_prompt()));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 25);
    lv_area_t card;
    lv_obj_get_coords(dialog, &card);
    lv_area_t screen;
    lv_obj_get_coords(test_screen(), &screen);
    CHECK(lv_area_is_in(&card, &screen, 0));

    // Width holds the touch target. Height may drop below it so every color
    // stays visible, but tiles must grow into the free room: taller than
    // button_height, which is all an unfitted tile gets here.
    const int32_t touch_min = theme_manager_get_spacing("touch_target_min");
    const int32_t button_h = theme_manager_get_spacing("button_height");
    REQUIRE(touch_min > 0);
    REQUIRE(button_h > 0);
    for (size_t i = 0; i < 24; ++i) {
        lv_area_t a;
        lv_obj_get_coords(btns[i], &a);
        CAPTURE(i, lv_area_get_width(&a), lv_area_get_height(&a), touch_min, button_h);
        CHECK(lv_area_get_width(&a) >= touch_min);
        CHECK(lv_area_get_height(&a) > button_h);
        CHECK(lv_area_is_in(&a, &card, 0));
        // The circle fills the tile's shorter side.
        lv_obj_t* disc = lv_obj_find_by_name(btns[i], "color_swatch_disc");
        REQUIRE(disc != nullptr);
        CHECK(lv_obj_get_width(disc) == std::min(lv_area_get_width(&a), lv_area_get_height(&a)));
        CHECK(lv_obj_get_height(disc) == lv_obj_get_width(disc));
    }
    lv_area_t footer;
    lv_obj_get_coords(btns[24], &footer);
    CHECK(lv_area_is_in(&footer, &card, 0));

    modal_.hide();
}

// A screen with room for full-size tiles keeps the card and tiles it had.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: swatch grid at 800x480 keeps the standard card",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    helix::test::ScopedGeometry medium(800, 480);
    REQUIRE(modal_.show_prompt(test_screen(), make_zmod_color_prompt()));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    CHECK(lv_obj_get_width(dialog) == 480);
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 25);
    const int32_t touch_min = theme_manager_get_spacing("touch_target_min");
    const int32_t button_h = theme_manager_get_spacing("button_height");
    for (size_t i = 0; i < 24; ++i) {
        CAPTURE(i, touch_min, button_h);
        CHECK(lv_obj_get_height(btns[i]) == button_h);
        CHECK(lv_obj_get_width(btns[i]) >= touch_min);
    }

    modal_.hide();
}

// Labelled buttons are not swatches: a 480x272 prompt of them keeps the
// standard card width and button height.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: labelled buttons at 480x272 keep the standard card",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    helix::test::ScopedGeometry micro(480, 272);
    REQUIRE(modal_.show_prompt(test_screen(), make_regular_prompt(4)));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    CHECK(lv_obj_get_width(dialog) == 480 * 70 / 100);
    for (lv_obj_t* btn : ActionPromptModalTestAccess::buttons(modal_)) {
        CHECK(lv_obj_get_height(btn) == theme_manager_get_spacing("button_height"));
    }

    modal_.hide();
}

TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a button deleted while shown is skipped at hide",
                 "[action_prompt][layout][ui_integration]") {
    REQUIRE(modal_.show_prompt(test_screen(), make_regular_prompt(2)));
    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 2);

    lv_obj_delete(btns[0]);
    CHECK(btns[0].get() == nullptr);
    CHECK(btns[1].get() != nullptr);

    modal_.hide();
}

// More swatch rows than the card can hold at button_height: the tiles stop
// shrinking there and the grid scrolls, so the footer stays on the card.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a swatch grid too tall for 480x272 scrolls",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    helix::test::ScopedGeometry micro(480, 272);
    helix::PromptData data = make_zmod_color_prompt();
    helix::PromptButton footer = data.buttons.back();
    data.buttons.pop_back();
    const size_t swatches = data.buttons.size();
    for (size_t i = 0; i < swatches; ++i) {
        helix::PromptButton extra = data.buttons[i];
        extra.group_id += 3;
        data.buttons.push_back(extra);
    }
    data.buttons.push_back(footer);

    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 2 * swatches + 1);
    for (size_t i = 0; i < 2 * swatches; ++i) {
        CAPTURE(i);
        CHECK(lv_obj_get_height(btns[i]) == theme_manager_get_spacing("button_height"));
    }
    lv_obj_t* scroll = lv_obj_find_by_name(dialog, "prompt_scroll");
    REQUIRE(scroll != nullptr);
    CHECK(lv_obj_has_flag(scroll, LV_OBJ_FLAG_SCROLLABLE));
    CHECK(lv_obj_get_scroll_bottom(scroll) > 0);

    lv_area_t card, footer_area;
    lv_obj_get_coords(dialog, &card);
    lv_obj_get_coords(btns.back(), &footer_area);
    CHECK(lv_area_is_in(&footer_area, &card, 0));

    modal_.hide();
}

// ----------------------------------------------------------------------------
// choose_button_row_layout(): the row layout decision, without widgets
// ----------------------------------------------------------------------------
TEST_CASE("choose_button_row_layout picks wrap, one row or a grid", "[action_prompt][layout]") {
    using helix::ui::ButtonRowKind;
    using helix::ui::choose_button_row_layout;
    // A 480px card's button row: 448px across, 8px gaps, 8px cell padding.
    helix::ui::ButtonRowMeasure m;
    m.available = 448;
    m.gap = 8;
    m.cell_pad = 8;
    m.widest_label = 60;

    SECTION("three or fewer wrap") {
        m.count = 3;
        CHECK(choose_button_row_layout(m).kind == ButtonRowKind::Wrap);
    }
    SECTION("four that fit an even share make one row") {
        m.count = 4;
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::EqualRow);
        CHECK(layout.cols == 4);
    }
    SECTION("four that do not fit an even share wrap") {
        m.count = 4;
        m.widest_label = 110; // + 16 padding > (448 - 24) / 4 = 106
        CHECK(choose_button_row_layout(m).kind == ButtonRowKind::Wrap);
    }
    SECTION("five labelled buttons make a three-column grid") {
        m.count = helix::ui::kPromptGridMinButtons;
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::Grid);
        CHECK(layout.cols == 3);
    }
    SECTION("a label too wide for a third drops the grid to two columns") {
        m.count = 6;
        m.widest_label = 130; // + 16 > (448 - 16) / 3 = 144
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::Grid);
        CHECK(layout.cols == 2);
    }
    SECTION("the widest label exactly filling a third keeps three columns") {
        m.count = 6;
        m.widest_label = 144 - 16;
        CHECK(choose_button_row_layout(m).cols == 3);
    }
    SECTION("a label too wide even for half still gets two columns, ellipsized") {
        m.count = 6;
        m.widest_label = 400;
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::Grid);
        CHECK(layout.cols == 2);
    }
    SECTION("a row of swatches stays one line of tiles") {
        m.count = 6;
        m.all_swatches = true;
        m.widest_label = 0;
        const auto layout = choose_button_row_layout(m);
        CHECK(layout.kind == ButtonRowKind::EqualRow);
        CHECK(layout.cols == 6);
    }
    SECTION("an unmeasured row wraps") {
        m.count = 6;
        m.available = 0;
        CHECK(choose_button_row_layout(m).kind == ButtonRowKind::Wrap);
    }
}

// ----------------------------------------------------------------------------
// AFC_CALIBRATION on a six-unit AFC: a grid of unit buttons, every one
// reachable by scrolling, and both footer buttons on the card.
// ----------------------------------------------------------------------------
namespace {

lv_area_t coords_of(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a;
}

void check_afc_calibration_fits(helix::ui::ActionPromptModal& modal, lv_obj_t* screen) {
    REQUIRE(modal.show_prompt(screen, parse_prompt(helix::demo::afc_calibration_lines())));
    lv_obj_t* dialog = modal.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(screen);

    const auto& btns = ActionPromptModalTestAccess::buttons(modal);
    REQUIRE(btns.size() == 8); // six units + two footer buttons
    const lv_area_t card = coords_of(dialog);
    const lv_area_t screen_area = coords_of(screen);
    CHECK(lv_area_is_in(&card, &screen_area, 0));

    // The footer never leaves the card.
    for (size_t i = 6; i < 8; ++i) {
        const lv_area_t footer = coords_of(btns[i]);
        CAPTURE(i, card.y2, footer.y2);
        CHECK(lv_area_is_in(&footer, &card, 0));
    }

    // A grid: equal cells, at most three to a line, the first line full.
    const int32_t w0 = lv_obj_get_width(btns[0]);
    std::vector<int32_t> line_ys;
    for (size_t i = 0; i < 6; ++i) {
        CAPTURE(i, lv_obj_get_width(btns[i]), w0);
        CHECK(lv_obj_get_width(btns[i]) == w0);
        const int32_t y = coords_of(btns[i]).y1;
        if (std::find(line_ys.begin(), line_ys.end(), y) == line_ys.end()) {
            line_ys.push_back(y);
        }
    }
    CHECK(line_ys.size() >= 2);
    CHECK(line_ys.size() <= 3);

    // Every unit button can be scrolled fully into the visible scroll area.
    lv_obj_t* scroll = lv_obj_find_by_name(dialog, "prompt_scroll");
    REQUIRE(scroll != nullptr);
    for (size_t i = 0; i < 6; ++i) {
        lv_obj_scroll_to_view_recursive(btns[i], LV_ANIM_OFF);
        lv_obj_update_layout(screen);
        const lv_area_t area = coords_of(scroll);
        const lv_area_t btn = coords_of(btns[i]);
        CAPTURE(i, area.y1, area.y2, btn.y1, btn.y2);
        CHECK(lv_area_is_in(&btn, &area, 0));
        CHECK(lv_area_is_in(&btn, &card, 0));
    }
    modal.hide();
}

} // namespace

TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: six-unit AFC calibration fits a 480x272 card",
                 "[action_prompt][layout][ui_integration]") {
    helix::test::ScopedGeometry micro(480, 272);
    check_afc_calibration_fits(modal_, test_screen());
}

TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: six-unit AFC calibration fits an 800x480 card",
                 "[action_prompt][layout][ui_integration]") {
    helix::test::ScopedGeometry medium(800, 480);
    check_afc_calibration_fits(modal_, test_screen());
}

TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: six-unit AFC calibration fits a 480x320 card",
                 "[action_prompt][layout][ui_integration]") {
    helix::test::ScopedGeometry tiny(480, 320);
    check_afc_calibration_fits(modal_, test_screen());
}

// On a 480x320 screen the prompt's buttons take the compact height, so the hex
// prompt shows its labelled buttons, both swatch rows and its footer without
// scrolling.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: the hex color prompt fits a 480x320 card unscrolled",
                 "[action_prompt][layout][ui_integration][hex_color][1806]") {
    helix::test::ScopedGeometry tiny(480, 320);
    REQUIRE(modal_.show_prompt(test_screen(), parse_prompt(helix::demo::hex_color_prompt_lines())));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 16); // three labelled, twelve swatches, one footer
    const lv_area_t card = coords_of(dialog);
    const lv_area_t screen = coords_of(test_screen());
    CHECK(lv_area_is_in(&card, &screen, 0));

    const int32_t compact_h = theme_manager_get_spacing("button_height_sm");
    for (size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(lv_obj_get_height(btns[i]) == compact_h);
    }
    CHECK(lv_obj_get_height(btns.back()) == compact_h);

    lv_obj_t* scroll = lv_obj_find_by_name(dialog, "prompt_scroll");
    REQUIRE(scroll != nullptr);
    CHECK(lv_obj_get_scroll_bottom(scroll) <= 0);
    for (size_t i = 0; i < btns.size(); ++i) {
        const lv_area_t a = coords_of(btns[i]);
        CAPTURE(i, a.y1, a.y2, card.y2);
        CHECK(lv_area_is_in(&a, &card, 0));
    }

    modal_.hide();
}

// The Z-Mod picker's three swatch rows at 480x320: the card stays on screen and
// the footer on the card.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: Z-Mod swatch grid keeps its footer on a 480x320 card",
                 "[action_prompt][layout][ui_integration][hex_color][1806][swatch]") {
    helix::test::ScopedGeometry tiny(480, 320);
    REQUIRE(modal_.show_prompt(test_screen(), make_zmod_color_prompt()));
    lv_obj_t* dialog = modal_.dialog();
    REQUIRE(dialog != nullptr);
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 25);
    const lv_area_t card = coords_of(dialog);
    const lv_area_t screen = coords_of(test_screen());
    CHECK(lv_area_is_in(&card, &screen, 0));
    const lv_area_t footer = coords_of(btns.back());
    CHECK(lv_area_is_in(&footer, &card, 0));

    modal_.hide();
}

// One label longer than the card is wide must not pan the prompt sideways.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a wrap-row label wider than the card ends in an ellipsis",
                 "[action_prompt][layout][ui_integration]") {
    helix::PromptData data;
    data.title = "Long";
    data.text_lines.push_back("Pick one");
    helix::PromptButton btn;
    btn.label = std::string(120, 'W');
    btn.gcode = "RESPOND MSG=x";
    btn.color = "primary";
    data.buttons.push_back(btn);
    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == 1);
    lv_obj_t* row = lv_obj_get_parent(btns[0]);
    CHECK(lv_obj_get_width(btns[0]) <= lv_obj_get_content_width(row));
    lv_obj_t* label = lv_obj_get_child_by_type(btns[0], 0, &lv_label_class);
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_width(label) <= lv_obj_get_content_width(btns[0]));
    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t* scroll = lv_obj_find_by_name(modal_.dialog(), "prompt_scroll");
    REQUIRE(scroll != nullptr);
    CHECK(lv_obj_get_scroll_dir(scroll) == LV_DIR_VER);
    CHECK(lv_obj_get_scroll_right(scroll) <= 0);

    modal_.hide();
}

// A grid cell is a fixed share of the row, so its label ends in an ellipsis too.
TEST_CASE_METHOD(ActionPromptLayoutFixture,
                 "ActionPromptModal: a grid label wider than its cell ends in an ellipsis",
                 "[action_prompt][layout][ui_integration]") {
    helix::PromptData data = make_regular_prompt(helix::ui::kPromptGridMinButtons + 1);
    data.buttons[0].label = std::string(120, 'W');
    REQUIRE(modal_.show_prompt(test_screen(), data));
    lv_obj_update_layout(test_screen());

    const auto& btns = ActionPromptModalTestAccess::buttons(modal_);
    REQUIRE(btns.size() == data.buttons.size());
    // Equal cells: the long label did not stretch its button.
    REQUIRE(lv_obj_get_width(btns[0]) == lv_obj_get_width(btns[1]));
    lv_obj_t* label = lv_obj_get_child_by_type(btns[0], 0, &lv_label_class);
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_width(label) <= lv_obj_get_content_width(btns[0]));
    CHECK(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_DOTS);

    modal_.hide();
}
