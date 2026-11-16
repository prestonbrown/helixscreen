// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wizard_telemetry_info_text.cpp
 * @brief The telemetry and summary wizard steps share one privacy-text subject.
 *
 * Both steps open telemetry_info_modal, which binds `telemetry_info_text` by
 * name. The name must resolve to the telemetry step's subject whichever step
 * initialised last (prestonbrown/helixscreen#1938).
 */

#include "ui_wizard_summary.h"
#include "ui_wizard_telemetry.h"

#include "../lvgl_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

void require_shared_info_text() {
    WizardTelemetryStep* owner = get_wizard_telemetry_step();
    lv_subject_t* bound = lv_xml_get_subject(nullptr, "telemetry_info_text");
    REQUIRE(bound != nullptr);
    CHECK(bound == owner->info_text_subject());
    const std::string text = lv_subject_get_string(bound);
    CHECK(text.find("HelixScreen is a free, open-source project") != std::string::npos);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Summary after telemetry: the info text has one owner",
                 "[wizard][telemetry][1938]") {
    get_wizard_telemetry_step()->init_subjects();
    get_wizard_summary_step()->init_subjects();
    require_shared_info_text();
}

TEST_CASE_METHOD(LVGLTestFixture, "Telemetry after summary: the info text has one owner",
                 "[wizard][telemetry][1938]") {
    get_wizard_summary_step()->init_subjects();
    get_wizard_telemetry_step()->init_subjects();
    require_shared_info_text();
}
