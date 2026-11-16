// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/bed_occupancy_state_test_access.h"
#include "bed_occupancy_state.h"
#include "bed_plate_context.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <optional>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A printer state that has reported no axis bounds, and a bed-occupancy verdict of the
/// test's choosing published as the global state.
class PlateContextFixture : public LVGLUITestFixture {
  public:
    PlateContextFixture() : occupancy_(printer_state_, nullptr) {
        printer_state_.init_subjects(false);
        SettingsManager::instance().init_subjects();
        occupancy_.init_subjects();
        ui::UpdateQueue::instance().drain();
        previous_ = get_bed_occupancy_state();
        set_bed_occupancy_state(&occupancy_);
    }
    ~PlateContextFixture() override {
        set_bed_occupancy_state(previous_);
    }

    void put_part(bool occupied, std::optional<double> height) {
        BedOccupancy verdict;
        verdict.occupied = occupied;
        verdict.part_height_mm = height;
        verdict.job_id = occupied ? "job-on-the-bed" : "";
        BedOccupancyStateTestAccess::set_verdict(occupancy_, verdict);
    }

    /// Click @p button on the open dialog, which closes it.
    void answer(const char* button) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_send_event(lv_obj_find_by_name(dialog, button), LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        ui::UpdateQueue::instance().drain();
    }

    PrinterState printer_state_;
    BedOccupancyState occupancy_;
    BedOccupancyState* previous_ = nullptr;
};

double clearance() {
    return static_cast<double>(SettingsManager::instance().get_bed_map_clearance_mm());
}

} // namespace

TEST_CASE_METHOD(PlateContextFixture, "unknown Z bounds refuse a plate move over a part",
                 "[bed_plate_context][motion]") {
    REQUIRE_FALSE(move_clamp_bounds(printer_state_).has_z);

    put_part(false, std::nullopt);
    CHECK(current_plate_lift(printer_state_, 0.2).kind == PlateLift::Kind::None);
    CHECK(park_plate_lift(printer_state_, 0.2).kind == PlateLift::Kind::None);

    put_part(true, 38.0);
    CHECK(current_plate_lift(printer_state_, 0.2).kind == PlateLift::Kind::Refuse);
    CHECK(park_plate_lift(printer_state_, 0.2).kind == PlateLift::Kind::Refuse);

    put_part(true, std::nullopt);
    CHECK(current_plate_lift(printer_state_, 0.2).kind == PlateLift::Kind::Refuse);
    CHECK(park_plate_lift(printer_state_, 0.2).kind == PlateLift::Kind::Refuse);
}

TEST_CASE_METHOD(PlateContextFixture, "Z down stops at the floor over a part of unknown height",
                 "[bed_plate_context][motion]") {
    REQUIRE(clearance() > 0.0);

    put_part(false, std::nullopt);
    CHECK(clamp_z_down_to_current_floor(0.0) == Catch::Approx(0.0));

    put_part(true, std::nullopt);
    CHECK(clamp_z_down_to_current_floor(0.0) == Catch::Approx(clearance()));
    CHECK(clamp_z_down_to_current_floor(clearance() + 3.0) == Catch::Approx(clearance() + 3.0));

    put_part(true, 38.0);
    CHECK(clamp_z_down_to_current_floor(0.0) == Catch::Approx(38.0 + clearance()));
}

TEST_CASE_METHOD(PlateContextFixture,
                 "a calibration asks about the bed only while a print may be on it",
                 "[bed_plate_context][2105]") {
    AsyncLifetimeGuard owner;
    int starts = 0;
    int declines = 0;
    const auto start = [&starts] { ++starts; };
    const auto declined = [&declines] { ++declines; };

    put_part(false, std::nullopt);
    ensure_bed_clear_then(owner.token(), start);
    CHECK(starts == 1);
    CHECK(ModalStack::instance().top_dialog() == nullptr);

    put_part(true, std::nullopt);
    ensure_bed_clear_then(owner.token(), start, declined);
    CHECK(starts == 1);
    answer("btn_secondary");
    CHECK(starts == 1);
    CHECK(declines == 1);
    CHECK(occupancy_.current().occupied);

    ensure_bed_clear_then(owner.token(), start, declined);
    answer("btn_primary");
    CHECK(starts == 2);
    CHECK(declines == 1);
    CHECK_FALSE(occupancy_.current().occupied);

    // A screen closed while the question is up starts nothing, but the answer still
    // clears the bed.
    put_part(true, std::nullopt);
    ensure_bed_clear_then(owner.token(), start);
    owner.invalidate();
    answer("btn_primary");
    CHECK(starts == 2);
    CHECK_FALSE(occupancy_.current().occupied);
}
