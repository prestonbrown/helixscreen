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
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/scoped_mock_ams.h"
#include "ams_backend_mock.h"
#include "app_constants.h"
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

TEST_CASE_METHOD(ReprintGatesFixture, "a reprint superseded before its file checks answer is told",
                 "[reprint_gates][reprint][print_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    weigh_lane(ams, 500.0F);
    PlantedGcode file("reprint_superseded.gcode", "", kRedPla140g);

    int first_errors = 0;
    mock_client_.defer_next("server.files.metadata");
    controller().initiate_reprint(
        file.name(), "", {0}, []() {}, [&first_errors]() { ++first_errors; });
    settle();
    CHECK(first_errors == 0);

    reprint(file.name());
    mock_client_.fire_deferred("server.files.metadata");
    settle();

    CHECK(first_errors == 1);
    REQUIRE(start_params.size() == 1);
    CHECK(started == 1);
    CHECK(errors == 0);
}

TEST_CASE_METHOD(ReprintGatesFixture,
                 "a Print Files start while a reprint reads its file ends the reprint",
                 "[reprint_gates][reprint][print_start]") {
    PlantedGcode reprinted("reprint_raced.gcode", "", kRedPla140g);
    auto& c = controller();
    // One gate that always asks, so Print Files' start waits on its dialog.
    struct GateList {
        helix::ui::PrintStartController& c;
        ~GateList() {
            PrintStartControllerTestAccess::set_gates(c, helix::default_print_start_gates());
        }
    } restore{c};
    PrintStartControllerTestAccess::set_gates(
        c, {{"always_asks", +[](const helix::PrintStartContext&) {
                 helix::CheckResult r;
                 r.verdict = helix::CheckResult::Verdict::Warn;
                 r.title = "Always";
                 r.body = "Always asks";
                 r.proceed_label = "Start Anyway";
                 return r;
             }}});

    mock_client_.defer_next("server.files.metadata");
    c.initiate_reprint(reprinted.name(), "", {0}, [this]() { ++started; }, [this]() { ++errors; });
    settle();
    REQUIRE(errors == 0);

    c.set_file("print_files_pick.gcode", "", {"#FF0000"}, "");
    c.initiate();
    settle();
    // The reprint's caller hears at once, so its button comes back.
    CHECK(errors == 1);
    lv_obj_t* print_files_dialog = PrintStartControllerTestAccess::print_gate_modal(c);
    REQUIRE(print_files_dialog != nullptr);

    // The reprint's file read answers late and changes nothing.
    mock_client_.fire_deferred("server.files.metadata");
    settle();
    CHECK(PrintStartControllerTestAccess::print_gate_modal(c) == print_files_dialog);
    CHECK(start_params.empty());
    CHECK(started == 0);
    CHECK(errors == 1);
    press("btn_secondary");
}

TEST_CASE_METHOD(ReprintGatesFixture,
                 "a reprint whose printer got busy while it read starts nothing",
                 "[reprint_gates][reprint][print_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    weigh_lane(ams, 500.0F);
    PlantedGcode file("reprint_busy.gcode", "", kRedPla140g);

    mock_client_.defer_next("server.files.metadata");
    controller().initiate_reprint(
        file.name(), "", {0}, [this]() { ++started; }, [this]() { ++errors; });
    settle();
    helix::test::set_wire_state(get_printer_state(), helix::PrintJobState::PRINTING);
    settle();
    mock_client_.fire_deferred("server.files.metadata");
    settle();

    CHECK(start_params.empty());
    CHECK(started == 0);
    CHECK(errors == 1);
    CHECK(ModalStack::instance().top_dialog() == nullptr);
}

TEST_CASE_METHOD(ReprintGatesFixture, "a reprint whose file checks stall gives up",
                 "[reprint_gates][reprint][print_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    weigh_lane(ams, 500.0F);
    PlantedGcode file("reprint_stalled.gcode", "", kRedPla140g);

    mock_client_.defer_next("server.files.metadata");
    reprint(file.name());
    REQUIRE(errors == 0);

    lv_tick_inc(app_constants::transfer::STALL_MS + 1);
    lv_timer_handler();
    settle();
    CHECK(errors == 1);

    // A read that answers after all starts nothing.
    mock_client_.fire_deferred("server.files.metadata");
    settle();
    CHECK(start_params.empty());
    CHECK(started == 0);
    CHECK(errors == 1);
}

namespace {

/// A pre-print-send backend that last routed tool 0 to head 3.
class RoutedToHead3 : public AmsBackendMock {
  public:
    RoutedToHead3() : AmsBackendMock(4) {
        set_snapmaker_mode(true);
    }
    std::vector<int> last_print_tool_mapping() const override {
        return {3, 1, 2, 0};
    }
};

void fill_head(AmsBackendMock& backend, int head, uint32_t rgb, float grams, SlotStatus status) {
    auto slot = backend.get_slot_info(head);
    slot.color_rgb = rgb;
    slot.material = "PLA";
    slot.remaining_weight_g = grams;
    helix::test::apply_edit(backend, head, slot);
    backend.force_slot_status(head, status);
}

} // namespace

TEST_CASE_METHOD(ReprintGatesFixture, "a reprint's gates judge the head its routing sends it to",
                 "[reprint_gates][reprint][print_start][preprint]") {
    AmsState::instance().init_subjects(false);
    helix::test::RegisteredBackend<RoutedToHead3> backend;
    backend->set_operation_delay(0);
    backend->start();
    helix::ui::UpdateQueue::instance().drain();
    struct Stop {
        AmsBackendMock& b;
        ~Stop() {
            helix::ui::UpdateQueue::instance().drain();
            b.stop();
        }
    } stop{*backend};
    // Only head 3, where the last print ran tool 0, has enough filament; the
    // seed alone (tool 0 on head 0, or color matching on head 1) is short.
    fill_head(*backend, 0, 0x0000FF, 60.0F, SlotStatus::LOADED);
    fill_head(*backend, 1, 0xFF0000, 60.0F, SlotStatus::LOADED);
    fill_head(*backend, 2, 0x00FF00, 60.0F, SlotStatus::LOADED);
    fill_head(*backend, 3, 0xEE1010, 500.0F, SlotStatus::LOADED);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(backend->requires_preprint_send());
    PlantedGcode file("reprint_routed.gcode", "", kRedPla140g);

    reprint(file.name());

    CHECK(PrintStartControllerTestAccess::print_gate_modal(controller()) == nullptr);
    REQUIRE(start_params.size() == 1);
    CHECK(started == 1);
    CHECK(errors == 0);
}

TEST_CASE_METHOD(ReprintGatesFixture, "a reprint with no callbacks still gives up a stalled read",
                 "[reprint_gates][reprint][print_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    weigh_lane(ams, 500.0F);
    PlantedGcode file("reprint_stalled_bare.gcode", "", kRedPla140g);

    mock_client_.defer_next("server.files.metadata");
    controller().initiate_reprint(file.name(), "", {0}, nullptr, nullptr);
    settle();
    lv_tick_inc(app_constants::transfer::STALL_MS + 1);
    lv_timer_handler();
    settle();

    // The read answering after the give-up starts nothing.
    mock_client_.fire_deferred("server.files.metadata");
    settle();
    CHECK(start_params.empty());
}

TEST_CASE_METHOD(ReprintGatesFixture, "a Print Files start ends a reprint waiting on a gate dialog",
                 "[reprint_gates][reprint][print_start]") {
    helix::test::ScopedMockAms ams({{0xFF0000, "PLA"}, {0x00FF00, "PLA"}});
    weigh_lane(ams, 60.0F);
    PlantedGcode file("reprint_dialog_raced.gcode", "", kRedPla140g);

    reprint(file.name());
    auto& c = controller();
    REQUIRE(PrintStartControllerTestAccess::print_gate_modal(c) != nullptr);
    REQUIRE(PrintStartControllerTestAccess::open_gate_name(c) == "insufficient_lane_weight");

    c.set_file("print_files_pick.gcode", "", {"#FF0000"}, "");
    c.initiate();
    settle();
    lv_timer_handler();
    settle();

    // The reprint's caller hears, and no dialog left open belongs to it.
    CHECK(errors == 1);
    CHECK(started == 0);
    if (PrintStartControllerTestAccess::print_gate_modal(c) != nullptr) {
        CHECK(PrintStartControllerTestAccess::open_gate_name(c) != "insufficient_lane_weight");
        press("btn_secondary");
    }
    for (const auto& params : start_params) {
        CHECK(params.value("filename", std::string()) != file.name());
    }
}
