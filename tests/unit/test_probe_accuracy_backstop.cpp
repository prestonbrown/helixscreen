// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_probe_accuracy_backstop.cpp
 * @brief A PROBE_ACCURACY run whose RPC is lost to the transport still reaches
 *        a terminal state the user can see.
 *
 * Run with: ./build/bin/helix-tests "[probe][1543]"
 *
 * The absorb path keeps the gcode_response handler registered because the
 * printer may still be measuring. Nothing else in the overlay can conclude the
 * run: progress advances only on arriving sample lines, and the handler is
 * removed only by a results line, an error line, or the next run. The backstop
 * is the one thing that can move the modal out of "measuring"
 * (prestonbrown/helixscreen#1543).
 */

#include "ui_probe_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/probe_overlay_test_access.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

class ProbeAccuracyFixture : public LVGLTestFixture {
  public:
    ProbeAccuracyFixture() : api_(client_, state_) {
        overlay_.init_subjects();
        overlay_.set_api(&api_);
    }

    ~ProbeAccuracyFixture() override {
        overlay_.set_api(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Run the armed backstop now instead of waiting out its real budget.
    void fire_backstop() {
        lv_timer_t* timer = ProbeOverlayTestAccess::accuracy_backstop_timer(overlay_);
        REQUIRE(timer != nullptr);
        lv_timer_set_period(timer, 1);
        lv_timer_ready(timer);
        process_lvgl(20);
    }

    static constexpr uint32_t kBackstopMs = 300000;

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state_;
    MoonrakerAPIMock api_;
    ProbeOverlay overlay_;
};

} // namespace

TEST_CASE_METHOD(ProbeAccuracyFixture,
                 "a probe accuracy run lost to the transport ends in a state the user can see",
                 "[probe][1543]") {
    ProbeOverlayTestAccess::stage_accuracy_run(overlay_, "probe_accuracy_1");
    REQUIRE(ProbeOverlayTestAccess::accuracy_state(overlay_) == 1); // PROBING

    overlay_.arm_accuracy_backstop("probe_accuracy_1", kBackstopMs);
    fire_backstop();

    CHECK(ProbeOverlayTestAccess::accuracy_state(overlay_) == 3); // ERROR
    CHECK(ProbeOverlayTestAccess::accuracy_error(overlay_).find("probe accuracy") !=
          std::string::npos);
}

TEST_CASE_METHOD(ProbeAccuracyFixture, "a backstop for a superseded run leaves the current one be",
                 "[probe][1543]") {
    ProbeOverlayTestAccess::stage_accuracy_run(overlay_, "probe_accuracy_2");

    overlay_.arm_accuracy_backstop("probe_accuracy_1", kBackstopMs);

    CHECK(ProbeOverlayTestAccess::accuracy_backstop_timer(overlay_) == nullptr);
    CHECK(ProbeOverlayTestAccess::accuracy_state(overlay_) == 1); // still PROBING
}
