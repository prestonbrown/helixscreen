// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_dryer_presentation.h"

#include "../helix_test_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::drying_status_text;

namespace {

DryerInfo running(DryingPhase phase, int remaining_min) {
    DryerInfo d;
    d.supported = true;
    d.active = true;
    d.target_temp_c = 55.0f;
    d.remaining_min = remaining_min;
    d.phase = phase;
    return d;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "The drying status line says what the dryer is doing",
                 "[ams][dryer][dryer_presentation]") {
    CHECK(drying_status_text(running(DryingPhase::Heating, 205)) == "Heating to 55\xC2\xB0"
                                                                    "C: 3:25 left");
    CHECK(drying_status_text(running(DryingPhase::Holding, 205)) == "Drying: 3:25 left");
    CHECK(drying_status_text(running(DryingPhase::Unknown, 61)) == "Drying: 1:01 left");

    SECTION("heating with no known target reads as drying") {
        DryerInfo d = running(DryingPhase::Heating, 30);
        d.target_temp_c = 0.0f;
        CHECK(drying_status_text(d) == "Drying: 0:30 left");
    }
    SECTION("an idle dryer has no status line") {
        DryerInfo d = running(DryingPhase::Heating, 30);
        d.active = false;
        CHECK(drying_status_text(d).empty());
    }
}
