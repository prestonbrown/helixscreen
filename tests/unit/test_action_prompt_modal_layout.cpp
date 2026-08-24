// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Layout regression tests for ActionPromptModal's regular-button row.
//
// R2 / prestonbrown/helixscreen#1043: the Klipper action:prompt modal lays its
// regular (non-footer) buttons into a fixed-width (320px) "button_container"
// with flex_flow=row_wrap and content-sized buttons. AFC's lane-picker prompt
// supplies 4 lane buttons ("Lane 1".."Lane 4"); they overflow the ~288px usable
// width and the 4th wraps to a second line.
//
// Fix: when there are >= 4 regular buttons, lay them out as an equal-width
// non-wrapping row (flex grow=1) so they all fit on ONE line. With <= 3 regular
// buttons the legacy content-sized row_wrap behaviour is kept byte-for-byte.
//
// The count alone is not the rule, though. An equal-width row splits the
// container evenly, so it only holds while the labels fit their share: a
// preheat macro offering seven "PLA 220/60" presets gets a few dozen pixels per
// cell and clips every label. Those fall back to row_wrap and take the lines
// they need. The invariant these tests defend is that no label is ever clipped.
//
// Tagged [ui_integration] (NOT hidden) — it shows real widgets and forces a
// real LVGL layout pass, which is the only way the wrap actually manifests.

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "display_settings_manager.h"

#include <vector>

#include "../catch_amalgamated.hpp"

// White-box accessor (declared friend in action_prompt_modal.h) — avoids adding
// _for_testing() methods to the production class ([L065]/[L088]).
struct ActionPromptModalTestAccess {
    static const std::vector<lv_obj_t*>& buttons(const helix::ui::ActionPromptModal& m) {
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
                 "ActionPromptModal: long labels wrap onto multiple rows instead of clipping",
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
