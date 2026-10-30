// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/heater_temp_widget.h"
#include "temperature_service.h"

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

// Helper: build a mock widget tree that mirrors panel_widget_temperature.xml
// Returns the outer container; creates a child named "temp_btn".
static lv_obj_t* create_mock_temperature_widget(lv_obj_t* parent) {
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_name(container, "panel_widget_temperature");

    lv_obj_t* btn = lv_obj_create(container);
    lv_obj_set_name(btn, "temp_btn");

    return container;
}

TEST_CASE_METHOD(TempWidgetFixture,
                 "TemperatureWidget: user_data on container, per-callback user_data on button",
                 "[temperature_widget][regression]") {
    // Simulate TemperatureService shared resource
    auto tcp = std::make_shared<TemperatureService>(state(), nullptr);
    helix_test::ScopedSharedResource<TemperatureService> scope(tcp);

    // Build mock widget tree
    lv_obj_t* container = create_mock_temperature_widget(test_screen());
    lv_obj_t* btn = lv_obj_find_by_name(container, "temp_btn");
    REQUIRE(btn != nullptr);

    // Create and attach widget
    HeaterTempWidget widget(state(), tcp.get(), nozzle_temp_config());
    widget.attach_tile(container, test_screen());

    SECTION("user_data is on the container") {
        auto* recovered = static_cast<HeaterTempWidget*>(lv_obj_get_user_data(container));
        REQUIRE(recovered == &widget);
    }

    SECTION("user_data is NOT on the button (per-callback user_data used instead)") {
        auto* btn_data = lv_obj_get_user_data(btn);
        // Button uses per-callback user_data via lv_obj_add_event_cb, not obj user_data
        REQUIRE(btn_data == nullptr);
    }

    SECTION("detach clears container user_data") {
        widget.detach_tile();
        auto* recovered = lv_obj_get_user_data(container);
        REQUIRE(recovered == nullptr);
    }

    // Clean up (detach is idempotent)
    widget.detach_tile();
}

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
