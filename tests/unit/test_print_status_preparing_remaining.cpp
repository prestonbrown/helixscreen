// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_preparing_remaining.cpp
 * @brief While preparing, the remaining time counts the pre-print once.
 *
 * Two writers feed the preparing countdown: the pre-print predictor every tick,
 * and print_time_left on each status frame, which during preparation is already
 * pre-print remaining plus the slicer's job estimate. The panel adds the pre-print
 * remaining to the slicer estimate only.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "format_utils.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = PrintStatusPanelTestAccess;

namespace {

struct PreparingRemainingFixture : public LVGLTestFixture {
    PreparingRemainingFixture() {
        state_.init_subjects(false);
        panel_ = std::make_unique<PrintStatusPanel>(state_, nullptr);
        panel_->init_subjects();
        Access::set_thumbnail_widget(*panel_, lv_image_create(test_screen()));
    }

    ~PreparingRemainingFixture() override {
        panel_.reset();
        state_.deinit_subjects();
    }

    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    PrinterState state_;
    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace

TEST_CASE_METHOD(PreparingRemainingFixture,
                 "Preparing remaining is pre-print plus the slicer estimate, counted once",
                 "[print_status][preprint_eta]") {
    constexpr int kSlicerSeconds = 3600;
    constexpr int kPreprintSeconds = 900;

    auto& print = state_.print_state();
    print.begin_preparing(PrintJobRef{"part.gcode", "", ""});
    print.set_estimated_print_time(kSlicerSeconds);
    print.set_print_start_state(PrintStartPhase::HEATING_BED, "Heating bed", 10);
    drain();

    // What the ETA estimator publishes while preparing: prep plus the whole job.
    lv_subject_set_int(print.get_print_time_left_subject(), kPreprintSeconds + kSlicerSeconds);
    drain();

    print.set_preprint_remaining_seconds(kPreprintSeconds);
    drain();

    lv_subject_t* shown = lv_xml_get_subject(nullptr, "print_remaining");
    REQUIRE(shown != nullptr);
    REQUIRE(std::string(lv_subject_get_string(shown)) ==
            helix::format::duration_padded(kPreprintSeconds + kSlicerSeconds));
}
