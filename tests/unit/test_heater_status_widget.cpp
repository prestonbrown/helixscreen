// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_heater_status_widget.cpp
 * @brief The heater_status component's glyph channel: exactly one of the four
 *        state icons is visible for every HeaterStatusState, including Auto
 *        (the chamber's standing/resting setpoint), which renders the circled-A
 *        auto indicator rather than the Maintaining check.
 */

#include "ui_temperature_utils.h"

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

namespace {

class HeaterStatusFixture : public XMLTestFixture {
  public:
    HeaterStatusFixture() : XMLTestFixture() {
        if (!lv_xml_get_subject(nullptr, "hs_state")) {
            lv_subject_init_int(&state_subject_, 0);
            lv_xml_register_subject(nullptr, "hs_state", &state_subject_);
        }
        state_ = lv_xml_get_subject(nullptr, "hs_state");
        REQUIRE(state_ != nullptr);
        REQUIRE(register_component("components/heater_status"));

        const char* attrs[] = {"state_subject", "hs_state", nullptr};
        widget_ = static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "heater_status", attrs));
        REQUIRE(widget_ != nullptr);
    }

    static bool hidden(const char* name) {
        lv_obj_t* icon = lv_obj_find_by_name(lv_screen_active(), name);
        REQUIRE(icon != nullptr);
        return lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }

    lv_subject_t* state_ = nullptr;

  private:
    lv_obj_t* widget_ = nullptr;
    lv_subject_t state_subject_{};
};

} // namespace

TEST_CASE_METHOD(HeaterStatusFixture, "heater_status shows exactly one glyph per state",
                 "[temperature][heater_status]") {
    using namespace helix::ui::temperature;
    const auto color = [](const char* name) {
        return lv_obj_get_style_text_color(lv_obj_find_by_name(lv_screen_active(), name),
                                           LV_PART_MAIN);
    };
    CHECK(lv_color_eq(color("status_heating"), get_heating_state_color(HeatState::Heating)));
    CHECK(lv_color_eq(color("status_ready"), get_heating_state_color(HeatState::AtTemp)));
    CHECK(lv_color_eq(color("status_cooling"), get_heating_state_color(HeatState::Cooling)));

    const int all_states[] = {
        static_cast<int>(HeaterStatusState::None),  static_cast<int>(HeaterStatusState::Heating),
        static_cast<int>(HeaterStatusState::Ready), static_cast<int>(HeaterStatusState::Cooling),
        static_cast<int>(HeaterStatusState::Auto),
    };
    const char* glyphs[] = {"status_heating", "status_ready", "status_cooling", "status_auto"};
    // State value i+1 lights glyph i; state 0 (None) lights none.
    const int lit_for_state[] = {-1, 0, 1, 2, 3};

    for (int s : all_states) {
        lv_subject_set_int(state_, s);
        for (int g = 0; g < 4; g++) {
            const bool should_hide = lit_for_state[s] != g;
            INFO("state " << s << " glyph " << glyphs[g]);
            REQUIRE(hidden(glyphs[g]) == should_hide);
        }
    }
}
