// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_clog_detection_config_gate.cpp
 * @brief Backend gating for the clog detection config modal (#1155).
 *
 * Run with: ./build/bin/helix-tests "[clog][gate]"
 *
 * Bug: the modal's Detection Mode buttons wrote the mode with
 *   MMU_TEST_CONFIG clog_detection=<n> [detection_length=<mm>]
 * unconditionally. MMU_TEST_CONFIG is a Happy Hare command. The clog widget is
 * offered whenever clog_meter_mode > 0, which includes AFC buffer fault
 * detection (mode 3, see ui_clog_meter.h), so an AFC user could open the modal,
 * pick a detection mode, hit Save, and get "Unknown command" in the console.
 * ACE, CFS, QIDI Box and the tool changers had the same exposure.
 *
 * Two halves are covered here:
 *   1. build_detection_mode_gcode() produces a command only where the backend
 *      names one — the belt-and-braces guard on the send path.
 *   2. The XML mode/length sections bind their hidden flag to
 *      clog_cfg_mode_supported, the subject the modal drives from the same
 *      decision — the UI half.
 */

#include "../test_fixtures.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "clog_detection_config_modal.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// 1. The gcode decision itself
// ============================================================================

TEST_CASE("Detection-mode gcode is emitted for Happy Hare of unknown version", "[clog][gate]") {
    // Before the connect-time query names a version, the pre-3.42 names.
    AmsBackendHappyHare hh(nullptr, nullptr);
    using M = ClogDetectionConfigModal;

    SECTION("auto mode carries no detection length") {
        REQUIRE(M::build_detection_mode_gcode(&hh, 2, 12.0f) ==
                std::optional<std::string>("MMU_TEST_CONFIG enable_clog_detection=2"));
    }
    SECTION("manual mode carries the detection length") {
        REQUIRE(M::build_detection_mode_gcode(&hh, 1, 12.0f) ==
                std::optional<std::string>(
                    "MMU_TEST_CONFIG enable_clog_detection=1 mmu_calibration_clog_length=12.0"));
    }
    SECTION("manual mode with no length falls back to the bare form") {
        REQUIRE(M::build_detection_mode_gcode(&hh, 1, 0.0f) ==
                std::optional<std::string>("MMU_TEST_CONFIG enable_clog_detection=1"));
    }
    SECTION("off is still a Happy Hare write") {
        REQUIRE(M::build_detection_mode_gcode(&hh, 0, 0.0f) ==
                std::optional<std::string>("MMU_TEST_CONFIG enable_clog_detection=0"));
    }
}

TEST_CASE("Detection-mode gcode is never emitted for backends without the setting",
          "[clog][gate]") {
    // AFC is the reported case (#1155): its nearest analogue is the buffer's
    // error_sensitivity, which is not a detection length, so there is no
    // drop-in mapping and sending nothing is correct.
    AmsBackendAfc afc(nullptr, nullptr);
    for (int mode : {0, 1, 2}) {
        CAPTURE(mode);
        REQUIRE_FALSE(ClogDetectionConfigModal::build_detection_mode_gcode(&afc, mode, 12.0f));
        REQUIRE_FALSE(ClogDetectionConfigModal::build_detection_mode_gcode(nullptr, mode, 0.0f));
    }
}

TEST_CASE("Save sends the detection length only when the user moved it", "[clog][gate]") {
    AmsBackendHappyHare hh(nullptr, nullptr);
    using M = ClogDetectionConfigModal;
    CHECK_FALSE(M::detection_save_gcode(&hh, 1, 1, 20.0f, 20.0f));
    CHECK(M::detection_save_gcode(&hh, 1, 1, 15.0f, 20.0f) ==
          std::optional<std::string>(
              "MMU_TEST_CONFIG enable_clog_detection=1 mmu_calibration_clog_length=15.0"));
    CHECK(M::detection_save_gcode(&hh, 1, 2, 20.0f, 20.0f) ==
          std::optional<std::string>("MMU_TEST_CONFIG enable_clog_detection=1"));
    CHECK(M::detection_save_gcode(&hh, 0, 1, 15.0f, 20.0f) ==
          std::optional<std::string>("MMU_TEST_CONFIG enable_clog_detection=0"));
}

// ============================================================================
// 2. The UI half — the XML sections hide on the same signal
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "Clog config hides mode/length when the backend has no gcode",
                 "[clog][gate][xml]") {
    // Constructing the modal registers clog_cfg_* subjects and the button
    // callbacks into the global XML scope; the component must be created after.
    ClogDetectionConfigModal modal("clog_detection", "home");

    REQUIRE(register_component("components/clog_detection_config_modal"));

    lv_obj_t* dlg = create_component("clog_detection_config_modal");
    REQUIRE(dlg != nullptr);

    lv_obj_t* mode_section = lv_obj_find_by_name(dlg, "mode_section");
    lv_obj_t* det_length_section = lv_obj_find_by_name(dlg, "det_length_section");
    REQUIRE(mode_section != nullptr);
    REQUIRE(det_length_section != nullptr);

    // The subject name in the XML must be the one the modal actually registers.
    lv_subject_t* supported = lv_xml_get_subject(nullptr, "clog_cfg_mode_supported");
    REQUIRE(supported != nullptr);

    SECTION("unsupported backend (AFC and friends) hides both sections") {
        lv_subject_set_int(supported, 0);
        lv_obj_update_layout(dlg);
        REQUIRE(lv_obj_has_flag(mode_section, LV_OBJ_FLAG_HIDDEN));
        REQUIRE(lv_obj_has_flag(det_length_section, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("Happy Hare shows both sections") {
        lv_subject_set_int(supported, 1);
        lv_obj_update_layout(dlg);
        REQUIRE_FALSE(lv_obj_has_flag(mode_section, LV_OBJ_FLAG_HIDDEN));
        REQUIRE_FALSE(lv_obj_has_flag(det_length_section, LV_OBJ_FLAG_HIDDEN));
    }

    // Tear the widget tree down before `modal` (and its subjects) go out of
    // scope — the bindings above observe subjects the modal owns.
    lv_obj_delete(dlg);
}
