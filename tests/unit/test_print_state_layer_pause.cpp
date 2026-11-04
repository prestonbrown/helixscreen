// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_state_layer_pause.cpp
 * @brief PrinterPrintState's per-print layer-pause state (#1703).
 *
 * The rules live in helix::layer_pause (test_layer_pause.cpp). These pin the
 * wiring: which status fields feed it, what a new print clears, and what the
 * two subjects the UI binds to say.
 */

#include "../lvgl_test_fixture.h"
#include "core/update_queue.h"
#include "gcode_pause_scan.h"
#include "layer_pause.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using nlohmann::json;
using A = helix::layer_pause::Availability;

namespace {

constexpr const char* kStateObject = "gcode_macro SET_PRINT_STATS_INFO";

struct LayerPauseFixture : public LVGLTestFixture {
    LayerPauseFixture() {
        state_.init_subjects(false);
    }
    ~LayerPauseFixture() override {
        state_.deinit_subjects();
    }

    void printing(int current, int total) {
        state_.update_from_status(
            {{"print_stats",
              {{"filename", "a.gcode"},
               {"state", "printing"},
               {"info", {{"current_layer", current}, {"total_layer", total}}}}}});
    }

    A availability() {
        return static_cast<A>(
            lv_subject_get_int(state_.print_state().get_layer_pause_availability_subject()));
    }
    int pending() {
        return lv_subject_get_int(state_.print_state().get_layer_pause_layer_subject());
    }

    PrinterState state_;
};

} // namespace

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: availability follows print and slicer layers",
                 "[layer_pause][print_state]") {
    CHECK(availability() == A::Hidden);

    SECTION("a print whose slicer reports no layers gives a reason") {
        state_.update_from_status(
            {{"print_stats", {{"filename", "a.gcode"}, {"state", "printing"}}}});
        CHECK(availability() == A::NoLayerData);
    }
    SECTION("virtual_sdcard layers are not reports the macros fire on") {
        state_.update_from_status(
            {{"print_stats", {{"filename", "a.gcode"}, {"state", "printing"}}},
             {"virtual_sdcard", {{"layer", 4}, {"layer_count", 50}, {"is_active", true}}}});
        CHECK(availability() == A::NoLayerData);
    }
    SECTION("slicer layers make it ready, until the last layer") {
        printing(3, 10);
        CHECK(availability() == A::Ready);
        printing(10, 10);
        CHECK(availability() == A::LastLayer);
    }
    SECTION("paused still counts as a running print") {
        printing(3, 10);
        state_.update_from_status({{"print_stats", {{"state", "paused"}}}});
        CHECK(availability() == A::Ready);
    }
    SECTION("the print ending hides it and drops what was pending") {
        printing(3, 10);
        state_.print_state().note_layer_pause_at_layer(7);
        CHECK(pending() == 7);
        state_.update_from_status({{"print_stats", {{"state", "complete"}}}});
        CHECK(availability() == A::Hidden);
        CHECK(pending() == 0);
    }
}

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: without read-back, what was sent stands in",
                 "[layer_pause][print_state]") {
    printing(3, 10);

    SECTION("next layer is the layer after the current one, until it is reached") {
        state_.print_state().note_layer_pause_next_layer(true);
        CHECK(pending() == 4);
        printing(4, 10);
        CHECK(pending() == 0);
    }
    SECTION("a chosen layer stays pending until reached") {
        state_.print_state().note_layer_pause_at_layer(6);
        printing(5, 10);
        CHECK(pending() == 6);
        printing(6, 10);
        CHECK(pending() == 0);
    }
    SECTION("cancel clears") {
        state_.print_state().note_layer_pause_at_layer(6);
        state_.print_state().note_layer_pause_at_layer(0);
        CHECK(pending() == 0);
    }
    SECTION("a new print forgets the schedule and the slicer flag") {
        state_.print_state().note_layer_pause_at_layer(6);
        state_.print_state().reset_for_new_print();
        CHECK(pending() == 0);
        CHECK_FALSE(state_.print_state().slicer_reports_layers());
    }
}

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: the macro's own state wins when published",
                 "[layer_pause][print_state]") {
    state_.print_state().set_layer_pause_state_object(kStateObject);
    printing(3, 10);

    json armed = {{kStateObject,
                   {{"pause_next_layer", {{"enable", false}, {"call", "PAUSE"}}},
                    {"pause_at_layer", {{"enable", true}, {"layer", 7}, {"call", "PAUSE"}}}}}};
    state_.update_from_status(armed);
    CHECK(pending() == 7);

    SECTION("the macro disarming itself clears it") {
        state_.update_from_status(
            {{kStateObject, {{"pause_at_layer", {{"enable", false}, {"layer", 7}}}}}});
        CHECK(pending() == 0);
    }
    SECTION("passing the layer does not clear it: the macro reports its own firing") {
        printing(5, 10);
        CHECK(state_.print_state().get_layer_pause_schedule().at_layer == 7);
    }
    SECTION("an object under a name nobody subscribed to is ignored") {
        state_.update_from_status(
            {{"gcode_macro OTHER", {{"pause_at_layer", {{"enable", false}, {"layer", 0}}}}}});
        CHECK(pending() == 7);
    }
}

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: no state object means no read-back",
                 "[layer_pause][print_state]") {
    printing(3, 10);
    state_.update_from_status(
        {{kStateObject, {{"pause_at_layer", {{"enable", true}, {"layer", 7}}}}}});
    CHECK(pending() == 0);
}

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: either layer report alone proves slicer layers",
                 "[layer_pause][print_state]") {
    printing(3, 10);
    state_.print_state().reset_for_new_print();
    REQUIRE_FALSE(state_.print_state().slicer_reports_layers());
    // A delta frame carries only what changed: here, the layer.
    state_.update_from_status(
        {{"print_stats",
          {{"filename", "a.gcode"}, {"state", "printing"}, {"info", {{"current_layer", 4}}}}}});
    CHECK(state_.print_state().slicer_reports_layers());
    CHECK(state_.print_state().slicer_current_layer() == 4);

    // So does a total_layer report alone, the first one a print sends.
    state_.print_state().reset_for_new_print();
    state_.update_from_status({{"print_stats", {{"info", {{"total_layer", 12}}}}}});
    CHECK(state_.print_state().slicer_reports_layers());
}

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: discovery publishes the capability and read-back",
                 "[layer_pause][print_state]") {
    auto capability = [this] {
        return lv_subject_get_int(
            state_.capabilities_state().subject(helix::Capability::HasLayerPause));
    };
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"print_stats", "gcode_macro SET_PAUSE_NEXT_LAYER",
                                  "gcode_macro SET_PAUSE_AT_LAYER", kStateObject}));
    state_.set_hardware(hw);
    CHECK(capability() == 1);

    // The state object discovery named is the one read back.
    printing(3, 10);
    state_.update_from_status(
        {{kStateObject, {{"pause_at_layer", {{"enable", true}, {"layer", 8}}}}}});
    CHECK(pending() == 8);

    PrinterDiscovery plain;
    plain.parse_objects(json::array({"print_stats", "gcode_macro PAUSE"}));
    state_.set_hardware(plain);
    CHECK(capability() == 0);
}

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: only print_stats.info layers count",
                 "[layer_pause][print_state]") {
    printing(3, 10);
    // Another source in its own numbering (the gcode echo here; virtual_sdcard
    // is the other) moves the displayed layer; the layer the macros compare
    // against does not move.
    state_.print_state().set_print_layer_current(50);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(state_.print_state().get_print_layer_current_subject()) == 50);
    CHECK(state_.print_state().slicer_current_layer() == 3);
    CHECK(state_.print_state().slicer_total_layers() == 10);
    CHECK(availability() == A::Ready);
    state_.print_state().note_layer_pause_next_layer(true);
    CHECK(pending() == 4);
}

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: a new state object forgets the schedule",
                 "[layer_pause][print_state]") {
    state_.print_state().set_layer_pause_state_object(kStateObject);
    printing(3, 10);
    state_.update_from_status(
        {{kStateObject, {{"pause_at_layer", {{"enable", true}, {"layer", 7}}}}}});
    REQUIRE(pending() == 7);

    SECTION("the same name again (a reconnect or printer switch)") {
        state_.print_state().set_layer_pause_state_object(kStateObject);
        CHECK(pending() == 0);
        CHECK_FALSE(state_.print_state().get_layer_pause_schedule().any());
    }
    SECTION("a different name") {
        state_.print_state().set_layer_pause_state_object("gcode_macro Set_Print_Stats_Info");
        CHECK(pending() == 0);
    }
    SECTION("what was sent is forgotten too") {
        state_.print_state().set_layer_pause_state_object("");
        state_.print_state().note_layer_pause_at_layer(9);
        REQUIRE(pending() == 9);
        state_.print_state().set_layer_pause_state_object("");
        CHECK(pending() == 0);
    }
}

TEST_CASE_METHOD(LayerPauseFixture, "Layer pause: pausing at the sent layer clears it",
                 "[layer_pause][print_state]") {
    printing(4, 10);
    state_.print_state().note_layer_pause_at_layer(5);
    REQUIRE(pending() == 5);
    // The macro pauses before the layer-5 report goes through.
    state_.update_from_status({{"print_stats", {{"state", "paused"}}}});
    CHECK(pending() == 0);

    // A schedule made while already paused survives the rest of the pause.
    state_.print_state().note_layer_pause_at_layer(5);
    state_.update_from_status({{"print_stats", {{"state", "paused"}}}});
    CHECK(pending() == 5);
}

TEST_CASE_METHOD(LayerPauseFixture,
                 "Layer pause: file_may_report_layer judges by this print's scan",
                 "[layer_pause][print_state]") {
    printing(3, 10);
    CHECK(state_.print_state().file_may_report_layer(42)); // no scan: nothing to judge by
    auto scan = std::make_shared<const helix::gcode::PauseScanResult>(
        helix::gcode::PauseScanResult{{}, {{4, 0.4f, 0.4f}, {6, 0.6f, 0.6f}}, {}});
    state_.print_state().set_pause_scan(scan, "a.gcode");
    CHECK(state_.print_state().file_may_report_layer(6));
    CHECK_FALSE(state_.print_state().file_may_report_layer(5));
    state_.print_state().set_pause_scan(scan, "other.gcode");
    CHECK(state_.print_state().file_may_report_layer(5)); // not this print's scan
}
