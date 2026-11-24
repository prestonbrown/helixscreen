// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// What both pressure advance tuners ask before a tower prints: filament in the toolhead,
// and one confirmation that also stands for "the bed is clear".

#include "ui_modal.h"
#include "ui_overlay_pa_tuning.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/bed_occupancy_state_test_access.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_mock.h"
#include "app_globals.h"
#include "bed_occupancy_state.h"
#include "bed_plate_context.h"
#include "pa_tower_start.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::PaTuningOverlay;

namespace {

/// A mock filament system whose answer for a head is the test's to choose.
class HeadAnswerBackend : public AmsBackendMock {
  public:
    explicit HeadAnswerBackend(int slots) : AmsBackendMock(slots) {}
    std::optional<bool> answer;
    [[nodiscard]] std::optional<bool> head_has_filament(int) const override {
        return answer;
    }
};

class TowerStartFixture : public LVGLUITestFixture {
  public:
    TowerStartFixture() : occupancy_(get_printer_state(), nullptr) {
        get_printer_state().init_subjects(false);
        helix::ui::modal_init_subjects();
        occupancy_.init_subjects();
        helix::ui::UpdateQueue::instance().drain();
        previous_ = get_bed_occupancy_state();
        set_bed_occupancy_state(&occupancy_);
    }
    ~TowerStartFixture() override {
        set_bed_occupancy_state(previous_);
    }

    void put_part_on_bed() {
        BedOccupancy verdict;
        verdict.occupied = true;
        verdict.job_id = "job-on-the-bed";
        BedOccupancyStateTestAccess::set_verdict(occupancy_, verdict);
    }

    /// Click @p button on the open dialog, which closes it.
    void answer(const char* button) {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_send_event(lv_obj_find_by_name(dialog, button), LV_EVENT_CLICKED, nullptr);
        process_lvgl(50);
        helix::ui::UpdateQueue::instance().drain();
    }

    BedOccupancyState occupancy_;
    BedOccupancyState* previous_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(TowerStartFixture, "starting a tower asks once, and Start marks the bed cleared",
                 "[pa_tune][pa_tower_start]") {
    AsyncLifetimeGuard owner;
    int starts = 0;
    put_part_on_bed();

    helix::ui::confirm_tower_print("Clear the plate.", owner.token(), [&starts] { ++starts; });
    answer("btn_secondary");
    CHECK(starts == 0);
    CHECK(occupancy_.current().occupied);

    helix::ui::confirm_tower_print("Clear the plate.", owner.token(), [&starts] { ++starts; });
    answer("btn_primary");
    CHECK(starts == 1);
    CHECK_FALSE(occupancy_.current().occupied);
    CHECK(ModalStack::instance().top_dialog() == nullptr);
}

TEST_CASE_METHOD(TowerStartFixture, "the linear tower's setup asks one question, not two",
                 "[pa_tune][pa_tower_start][pa_tuner]") {
    PaTuningOverlay overlay;
    overlay.init_subjects();
    overlay.set_api(api());
    put_part_on_bed();

    overlay.confirm_and_start_for_test();
    REQUIRE(ModalStack::instance().top_dialog() != nullptr);
    answer("btn_primary");
    // The first answer started the tower, so no "Is the bed clear?" follows it.
    CHECK(ModalStack::instance().top_dialog() == nullptr);
    CHECK_FALSE(occupancy_.current().occupied);
    overlay.cleanup();
}

TEST_CASE("a tower head is empty only where something reports it so", "[pa_tune][pa_tower_start]") {
    helix::test::RegisteredBackend<HeadAnswerBackend> harness(4);

    harness->answer = false;
    CHECK(helix::ui::tower_head_empty(2));
    harness->answer = true;
    CHECK_FALSE(helix::ui::tower_head_empty(2));
    // Nothing on this machine can tell: the tower is not blocked.
    harness->answer = std::nullopt;
    CHECK_FALSE(helix::ui::tower_head_empty(2));
}
