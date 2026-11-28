// SPDX-License-Identifier: GPL-3.0-or-later

// A calibration asks one question before the machine moves, and that question
// also stands for "the bed is clear".

#include "ui_modal.h"
#include "ui_panel_calibration_tool_offset.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/part_on_bed.h"
#include "app_globals.h"
#include "bed_occupancy_state.h"
#include "bed_plate_context.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <memory>
#include <optional>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

class StartConfirmFixture : public LVGLUITestFixture {
  public:
    StartConfirmFixture() {
        get_printer_state().init_subjects(false);
        helix::ui::modal_init_subjects();
        bed_ = std::make_unique<helix::test::PartOnBed>(get_printer_state());
        helix::ui::UpdateQueue::instance().drain();
        previous_ = get_bed_occupancy_state();
        set_bed_occupancy_state(&bed_->state());
        // A part is on the bed, of a height the file did not record.
        bed_->put(true, std::nullopt);
    }
    ~StartConfirmFixture() override {
        set_bed_occupancy_state(previous_);
        bed_.reset();
        bed_.reset();
    }

    void answer(const char* button) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_send_event(lv_obj_find_by_name(dialog, button), LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        helix::ui::UpdateQueue::instance().drain();
    }

    std::unique_ptr<helix::test::PartOnBed> bed_;
    BedOccupancyState* previous_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(StartConfirmFixture, "A calibration's one question marks the bed cleared",
                 "[calibration_start][bed_plate]") {
    AsyncLifetimeGuard owner;
    int starts = 0;
    int declines = 0;
    const auto ask = [&] {
        confirm_calibration_start(
            "Find the cutter?", "Clear the bed first.", "Start", owner.token(),
            [&starts] { ++starts; }, [&declines] { ++declines; });
    };

    ask();
    answer("btn_secondary");
    CHECK(starts == 0);
    CHECK(declines == 1);
    CHECK(bed_->state().current().occupied);

    ask();
    answer("btn_primary");
    CHECK(starts == 1);
    CHECK_FALSE(bed_->state().current().occupied);
    // Nothing asks "Is the bed clear?" after it.
    CHECK(ModalStack::instance().top_dialog() == nullptr);
}

TEST_CASE_METHOD(StartConfirmFixture, "Tool offsets ask one question with a print on the bed",
                 "[calibration_start][tool_offset_cal]") {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"toolchanger", "tool T0", "tool T1", "gcode_macro CALIBRATE_TOOL_OFFSETS"}));
    get_printer_state().set_hardware(hw);

    helix::ui::ToolOffsetCalibrationPanel panel;
    panel.init_subjects();
    panel.start_calibration();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    answer("btn_primary");
    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK_FALSE(bed_->state().current().occupied);
    panel.cleanup();
    get_printer_state().set_hardware(PrinterDiscovery{});
}
