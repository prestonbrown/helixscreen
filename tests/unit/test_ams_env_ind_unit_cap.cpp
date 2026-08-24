// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_env_ind_unit_cap.cpp
 * @brief Regression tests — per-unit environment subjects must cover real rigs
 *
 * Debug bundle XGVDYEB5 (a five-unit AFC rig) logged seven LVGL parser warnings
 * per AMS overview rebuild, one per environment-indicator binding on the fifth
 * card:
 *
 *     lv_xml_get_subject: No subject was found with name "ams_env_ind_4_visible"
 *     lv_obj_xml_bind_flag_apply: Subject `ams_env_ind_4_visible` doesn't exist
 *
 * AmsState allocated per-unit subjects for MAX_UNITS = 4 units, while the
 * overview panel formatted ams_env_ind_<i>_* names for every unit the backend
 * reported. Unit 4's badge bound seven names nothing had registered and stayed
 * permanently dark and silent — no crash, because every getter bounds-checks.
 *
 * The fix raises the cap to 8 (what the system-path canvas has always drawn) and
 * routes card name-building through AmsState::env_indicator_subject_names(), so
 * a rig past even that cap gets the always-off placeholders instead of names
 * nothing registered.
 */

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl/lvgl.h>

#include <cstdio>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// The seven environment-indicator subjects a unit card binds, in the order
/// ams_unit_card.xml declares them.
constexpr const char* kEnvIndSuffixes[] = {"temp_text",        "humidity_text", "humidity_status",
                                           "humidity_visible", "visible",       "drying_active",
                                           "drying_text"};

std::string env_ind_name(int unit, const char* suffix) {
    char buf[64];
    snprintf(buf, sizeof(buf), "ams_env_ind_%d_%s", unit, suffix);
    return std::string(buf);
}

std::string unit_name(int unit, const char* suffix) {
    char buf[64];
    snprintf(buf, sizeof(buf), "ams_unit_%d_%s", unit, suffix);
    return std::string(buf);
}

/// Re-register AmsState's subjects into LVGL's global XML scope. A previous test
/// may have left the singleton initialized with register_xml = false, in which
/// case init_subjects() alone early-returns and registers nothing.
void reinit_ams_subjects_for_xml() {
    AmsState::instance().deinit_subjects();
    AmsState::instance().init_subjects(true);
}

} // namespace

// ============================================================================
// The bug: the fifth unit of a real rig had no subjects at all.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Per-unit environment subjects exist for a five-unit rig",
                 "[ams][regression]") {
    reinit_ams_subjects_for_xml();

    // Unit index 4 is the fifth unit of the AFC rig in bundle XGVDYEB5 — the one
    // whose badge went dark. Index 7 is the eighth, the widest rig the AMS
    // system-path canvas draws (ui_system_path_canvas.cpp MAX_UNITS = 8), so the
    // two caps have to agree or the path renders a unit the badge cannot.
    for (int unit : {0, 4, 7}) {
        for (const char* suffix : kEnvIndSuffixes) {
            const std::string name = env_ind_name(unit, suffix);
            INFO("subject " << name);
            CHECK(lv_xml_get_subject(nullptr, name.c_str()) != nullptr);
        }

        // The sibling raw-value subjects registered by the same loop.
        for (const char* suffix : {"temp", "humidity"}) {
            const std::string name = unit_name(unit, suffix);
            INFO("subject " << name);
            CHECK(lv_xml_get_subject(nullptr, name.c_str()) != nullptr);
        }
    }

    // And the C++ accessors agree with what XML can resolve.
    auto& ams = AmsState::instance();
    for (int unit : {0, 4, 7}) {
        INFO("unit " << unit);
        CHECK(ams.get_env_ind_temp_text_subject(unit) != nullptr);
        CHECK(ams.get_env_ind_humidity_text_subject(unit) != nullptr);
        CHECK(ams.get_env_ind_humidity_status_subject(unit) != nullptr);
        CHECK(ams.get_env_ind_humidity_visible_subject(unit) != nullptr);
        CHECK(ams.get_env_ind_visible_subject(unit) != nullptr);
        CHECK(ams.get_env_ind_drying_active_subject(unit) != nullptr);
        CHECK(ams.get_env_ind_drying_text_subject(unit) != nullptr);
        CHECK(ams.get_unit_temp_subject(unit) != nullptr);
        CHECK(ams.get_unit_humidity_subject(unit) != nullptr);
    }

    AmsState::instance().deinit_subjects();
}

// ============================================================================
// The cap still has to be a hard edge — raising it must not mean the getters
// stopped bounds-checking, or a wider rig turns a dark badge into a UAF.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Environment-subject lookups past the unit cap return nullptr",
                 "[ams][regression]") {
    reinit_ams_subjects_for_xml();

    auto& ams = AmsState::instance();
    for (int unit : {AmsState::MAX_UNITS, AmsState::MAX_UNITS + 1, -1}) {
        INFO("unit " << unit);
        CHECK(ams.get_env_ind_temp_text_subject(unit) == nullptr);
        CHECK(ams.get_env_ind_humidity_text_subject(unit) == nullptr);
        CHECK(ams.get_env_ind_humidity_status_subject(unit) == nullptr);
        CHECK(ams.get_env_ind_humidity_visible_subject(unit) == nullptr);
        CHECK(ams.get_env_ind_visible_subject(unit) == nullptr);
        CHECK(ams.get_env_ind_drying_active_subject(unit) == nullptr);
        CHECK(ams.get_env_ind_drying_text_subject(unit) == nullptr);
        CHECK(ams.get_unit_temp_subject(unit) == nullptr);
        CHECK(ams.get_unit_humidity_subject(unit) == nullptr);
    }

    // The registration loop and the array bound must be the same number: an
    // ams_env_ind_<MAX_UNITS>_* name resolving here would mean the loop ran past
    // the arrays it indexes.
    for (const char* suffix : kEnvIndSuffixes) {
        const std::string name = env_ind_name(AmsState::MAX_UNITS, suffix);
        INFO("subject " << name);
        CHECK(lv_xml_get_subject(nullptr, name.c_str()) == nullptr);
    }

    AmsState::instance().deinit_subjects();
}

// ============================================================================
// Degradation: a rig past the cap still gets cards, with the badge bound to
// placeholders that read as "no environment data" instead of to nothing.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Unit cards past the cap bind to the always-off placeholders",
                 "[ams][regression]") {
    reinit_ams_subjects_for_xml();

    // Whatever a backend reports, every name a card binds must resolve. This is
    // the assertion the bundle's warnings were the runtime symptom of.
    for (int unit = 0; unit < AmsState::MAX_UNITS + 4; ++unit) {
        const auto names = AmsState::env_indicator_subject_names(unit);
        for (const std::string& name :
             {names.temp_text, names.humidity_text, names.humidity_status, names.humidity_visible,
              names.visible, names.drying_active, names.drying_text}) {
            INFO("unit " << unit << " binds " << name);
            CHECK(lv_xml_get_subject(nullptr, name.c_str()) != nullptr);
        }
    }

    // In range, a card binds its own unit's set — not unit 0's, which is what
    // makes each badge show its own box's readings.
    const auto in_range = AmsState::env_indicator_subject_names(4);
    CHECK(in_range.temp_text == "ams_env_ind_4_temp_text");
    CHECK(in_range.humidity_text == "ams_env_ind_4_humidity_text");
    CHECK(in_range.humidity_status == "ams_env_ind_4_humidity_status");
    CHECK(in_range.humidity_visible == "ams_env_ind_4_humidity_visible");
    CHECK(in_range.visible == "ams_env_ind_4_visible");
    CHECK(in_range.drying_active == "ams_env_ind_4_drying_active");
    CHECK(in_range.drying_text == "ams_env_ind_4_drying_text");

    // Past the cap there is no per-unit set, so the card falls back to the
    // placeholders rather than to unit 0's data (which would be a lie) or to an
    // unregistered name (which was the bug).
    const std::string off_flag = AmsState::ENV_IND_OFF_FLAG_SUBJECT;
    const std::string off_text = AmsState::ENV_IND_OFF_TEXT_SUBJECT;
    for (int unit : {AmsState::MAX_UNITS, AmsState::MAX_UNITS + 3, -1}) {
        INFO("unit " << unit);
        const auto over = AmsState::env_indicator_subject_names(unit);
        CHECK(over.temp_text == off_text);
        CHECK(over.humidity_text == off_text);
        CHECK(over.drying_text == off_text);
        CHECK(over.humidity_status == off_flag);
        CHECK(over.humidity_visible == off_flag);
        CHECK(over.visible == off_flag);
        CHECK(over.drying_active == off_flag);
    }

    // The placeholders only do their job if they read as "nothing to show":
    // ams_environment_indicator.xml hides its root on $visible == 0.
    lv_subject_t* flag = lv_xml_get_subject(nullptr, AmsState::ENV_IND_OFF_FLAG_SUBJECT);
    REQUIRE(flag != nullptr);
    CHECK(lv_subject_get_int(flag) == 0);

    lv_subject_t* text = lv_xml_get_subject(nullptr, AmsState::ENV_IND_OFF_TEXT_SUBJECT);
    REQUIRE(text != nullptr);
    const char* text_value = lv_subject_get_string(text);
    REQUIRE(text_value != nullptr);
    CHECK(std::string(text_value).empty());

    AmsState::instance().deinit_subjects();
}
