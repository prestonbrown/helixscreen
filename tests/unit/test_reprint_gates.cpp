// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_reprint_gates.cpp
 * @brief Print Status's Reprint asks the pre-print gates before it starts.
 *
 * Run with: ./build/bin/helix-tests "[reprint_gates]"
 *
 * The reprint goes through Print Files' own controller, as Print Status sends
 * it, so the file Print Files has open never answers for the reprinted one.
 */

#include "ui_modal.h"
#include "ui_panel_print_select.h"
#include "ui_print_start_controller.h"
#include "ui_update_queue.h"

#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/planted_gcode.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/queued_start_fixture.h"
#include "../test_helpers/scoped_mock_ams.h"
#include "app_globals.h"
#include "moonraker_client_mock.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PlantedGcode;

namespace {

/// One PLA tool in red that prints 140 g.
constexpr const char* kRedPla140g = "; filament_type = PLA\n"
                                    "; filament_colour = #FF0000\n"
                                    "G28\n"
                                    "; filament used [g] = 140.00\n";

class ReprintGatesFixture : public helix::test::QueuedStartMockedFixture {
  public:
    ReprintGatesFixture() {
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, "printer.print.start",
            [this](MoonrakerClientMock*, const json& params,
                   helix::SharedCallback<void(const json&)> ok,
                   helix::SharedCallback<void(const MoonrakerError&)>) -> bool {
                start_params.push_back(params);
                ok(json{{"result", "ok"}});
                return true;
            });
    }

    helix::ui::PrintStartController& controller() {
        return print_start_controller_for(get_printer_state(), api_.get());
    }

    void reprint(const std::string& file) {
        controller().initiate_reprint(
            file, "", {0}, [this]() { ++started; }, [this]() { ++errors; });
        settle();
    }

    void press(const char* button) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_t* btn = lv_obj_find_by_name(dialog, button);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        settle();
        lv_timer_handler();
        settle();
    }

    std::vector<json> start_params;
    int started = 0;
    int errors = 0;
};

/// A red PLA lane with @p grams on it, which tool 0 of kRedPla140g maps to.
void weigh_lane(helix::test::ScopedMockAms& ams, float grams) {
    auto slot = ams.backend->get_slot_info(0);
    slot.remaining_weight_g = grams;
    helix::test::apply_edit(*ams.backend, 0, slot);
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(ReprintGatesFixture, "a reprint short on filament asks before it starts",
                 "[reprint_gates][reprint][print_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    weigh_lane(ams, 60.0F);
    PlantedGcode file("reprint_short.gcode", "", kRedPla140g);

    reprint(file.name());

    auto& c = controller();
    REQUIRE(PrintStartControllerTestAccess::print_gate_modal(c) != nullptr);
    CHECK(PrintStartControllerTestAccess::open_gate_name(c) == "insufficient_lane_weight");
    CHECK(start_params.empty());
    CHECK_FALSE(get_printer_state().print_state().has_preparing_job());

    SECTION("Cancel starts nothing and hands the button back") {
        press("btn_secondary");
        CHECK(start_params.empty());
        CHECK(errors == 1);
        CHECK(started == 0);
        CHECK_FALSE(get_printer_state().print_state().has_preparing_job());
    }
    SECTION("Start Anyway starts the reprint once, as the reprint sends it") {
        press("btn_primary");
        REQUIRE(start_params.size() == 1);
        CHECK(start_params[0].value("filename", std::string()) == file.name());
        CHECK(started == 1);
        CHECK(errors == 0);
    }
}

TEST_CASE_METHOD(ReprintGatesFixture, "a reprint no gate stops starts once with no dialog",
                 "[reprint_gates][reprint][print_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    weigh_lane(ams, 500.0F);
    PlantedGcode file("reprint_enough.gcode", "", kRedPla140g);

    reprint(file.name());

    CHECK(PrintStartControllerTestAccess::print_gate_modal(controller()) == nullptr);
    CHECK(ModalStack::instance().top_dialog() == nullptr);
    REQUIRE(start_params.size() == 1);
    CHECK(start_params[0].value("filename", std::string()) == file.name());
    CHECK(started == 1);
    CHECK(errors == 0);
}

TEST_CASE_METHOD(ReprintGatesFixture,
                 "a multi-color reprint judges its own palette, not the file Print Files holds",
                 "[reprint_gates][reprint][print_start]") {
    // Two lanes for a four-color file: tools 2 and 3 have none.
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    PlantedGcode file("reprint_four_color.gcode", "",
                      "; filament_type = PLA;PLA;PLA;PLA\n"
                      "; filament_colour = #FF0000;#00FF00;#0000FF;#FFFF00\n"
                      "G28\nT0\nT1\nT2\nT3\n"
                      "; filament used [g] = 1.00, 1.00, 1.00, 1.00\n");
    // Print Files holds a one-color file of its own, or none at all.
    if (GENERATE(false, true)) {
        controller().set_file("single.gcode", "", {"#FF0000"}, "");
    }

    controller().initiate_reprint(
        file.name(), "", {0, 1, 2, 3}, [this]() { ++started; }, [this]() { ++errors; });
    settle();

    auto& c = controller();
    // Earlier gates may ask first; each is confirmed until the tool question.
    for (int i = 0; i < 8 && PrintStartControllerTestAccess::print_gate_modal(c) != nullptr &&
                    PrintStartControllerTestAccess::open_gate_name(c) != "unresolved_tools";
         ++i) {
        press("btn_primary");
    }
    REQUIRE(PrintStartControllerTestAccess::print_gate_modal(c) != nullptr);
    CHECK(PrintStartControllerTestAccess::open_gate_name(c) == "unresolved_tools");
    CHECK(start_params.empty());
    press("btn_secondary");
    CHECK(errors == 1);
}
