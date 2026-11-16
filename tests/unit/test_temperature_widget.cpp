// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/heater_temp_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Minimal fixture: LVGL display + static PrinterState for subjects
class TempWidgetFixture : public LVGLTestFixture {
  public:
    TempWidgetFixture() {
        if (!s_state) {
            s_state = new PrinterState();
            s_state->init_subjects();
        }
    }

    PrinterState& state() {
        return *s_state;
    }

  protected:
    static PrinterState* s_state;
};

PrinterState* TempWidgetFixture::s_state = nullptr;

TEST_CASE_METHOD(TempWidgetFixture,
                 "HeaterTempWidget: per-heater configs are distinct and correctly wired",
                 "[temperature_widget][regression]") {
    const auto& nozzle = nozzle_temp_config();
    const auto& bed = bed_temp_config();
    const auto& chamber = chamber_temp_config();

    SECTION("each config opens the matching temp-graph overlay mode") {
        REQUIRE(nozzle.mode == TempGraphOverlay::Mode::Nozzle);
        REQUIRE(bed.mode == TempGraphOverlay::Mode::Bed);
        REQUIRE(chamber.mode == TempGraphOverlay::Mode::Chamber);
    }

    SECTION("subject getters resolve to the heater's own subjects") {
        REQUIRE(chamber.temp_getter(state()) ==
                state().temperature_state().get_chamber_temp_subject());
        REQUIRE(chamber.target_getter(state()) ==
                state().temperature_state().get_chamber_target_subject());
        REQUIRE(bed.temp_getter(state()) == state().temperature_state().get_bed_temp_subject());
        REQUIRE(nozzle.temp_getter(state()) ==
                state().temperature_state().get_active_extruder_temp_subject());
        // Distinct heaters never alias the same subject.
        REQUIRE(chamber.temp_getter(state()) != bed.temp_getter(state()));
        REQUIRE(chamber.temp_getter(state()) != nozzle.temp_getter(state()));
    }
}
