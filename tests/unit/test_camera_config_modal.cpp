// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_camera_config_modal.cpp
 * @brief CameraConfigModal's config round trip: the `source` key (which
 *        webcam the widget shows) survives a save, the picker rows mirror the
 *        printer's named webcams, and Automatic clears the key.
 */

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "../lvgl_test_fixture.h"
#include "../test_helpers/camera_config_modal_test_access.h"
#include "camera_config_modal.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using helix::CameraConfigModal;
using Access = helix::CameraConfigModalTestAccess;

namespace {

// A panel no dashboard has, so on_ok()'s write to PanelWidgetManager finds
// no widget entry and touches nothing on disk; the on_save callback carries
// the config the modal built.
constexpr const char* TEST_PANEL = "camera_config_modal_test";

WebcamInfo cam(const std::string& name, const std::string& service = "mjpegstreamer") {
    WebcamInfo c;
    c.name = name;
    c.service = service;
    c.stream_url = "/" + name + "/?action=stream";
    c.snapshot_url = "/" + name + "/?action=snapshot";
    return c;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "CameraConfigModal: save carries source through",
                 "[camera][modal][webcam]") {
    nlohmann::json saved;
    CameraConfigModal modal("camera", TEST_PANEL,
                            [&saved](const nlohmann::json& config) { saved = config; });

    Access::load_config(modal, {{"source", "Bed"}, {"rotation", 90}, {"flip_h", true}});
    // The camera list may not carry "Bed" at all (printer rediscovered without
    // it); the preference is the user's and must not be dropped for that.
    Access::publish_sources(modal, {cam("Nozzle")});
    CameraConfigModal::on_rotate_180(nullptr);
    Access::ok(modal);

    REQUIRE(saved.is_object());
    CHECK(saved.value("source", "") == "Bed");
    CHECK(saved.value("rotation", 0) == 180);
    CHECK(saved.value("flip_h", false) == true);
    CHECK(saved.value("flip_v", true) == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "CameraConfigModal: picking a row writes its name",
                 "[camera][modal][webcam]") {
    nlohmann::json saved;
    CameraConfigModal modal("camera", TEST_PANEL,
                            [&saved](const nlohmann::json& config) { saved = config; });

    Access::load_config(modal, nlohmann::json::object());
    Access::publish_sources(modal, {cam("Nozzle"), cam("Chamber", "webrtc-go2rtc")});
    CHECK(Access::row_active(modal, 0) == 1); // Automatic

    Access::select_source(modal, 2);
    CHECK(Access::source(modal) == "Chamber");
    CHECK(Access::row_active(modal, 0) == 0);
    CHECK(Access::row_active(modal, 1) == 0);
    CHECK(Access::row_active(modal, 2) == 1);

    Access::ok(modal);
    CHECK(saved.value("source", "") == "Chamber");
    CHECK(saved.value("rotation", -1) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "CameraConfigModal: Automatic removes the source key",
                 "[camera][modal][webcam]") {
    nlohmann::json saved;
    CameraConfigModal modal("camera", TEST_PANEL,
                            [&saved](const nlohmann::json& config) { saved = config; });

    Access::load_config(modal, {{"source", "Nozzle"}});
    Access::publish_sources(modal, {cam("Nozzle")});
    CHECK(Access::row_active(modal, 1) == 1);

    Access::select_source(modal, 0);
    CHECK(Access::row_active(modal, 0) == 1);
    CHECK(Access::row_active(modal, 1) == 0);
    Access::ok(modal);

    REQUIRE(saved.is_object());
    CHECK_FALSE(saved.contains("source"));
}

TEST_CASE_METHOD(LVGLTestFixture, "CameraConfigModal: rows mirror the named webcams",
                 "[camera][modal][webcam]") {
    CameraConfigModal modal("camera", TEST_PANEL);
    Access::load_config(modal, nlohmann::json::object());

    WebcamInfo local; // discovery's loopback probe result: unnamed, not pickable
    local.snapshot_url = "http://127.0.0.1:8080/?action=snapshot";
    WebcamInfo down = cam("Toolhead");
    down.unavailable_reason = "service not running: crowsnest (failed/failed)";

    Access::publish_sources(modal, {cam("Nozzle"), local, cam("Chamber", "webrtc-go2rtc"), down});

    CHECK(Access::source_count(modal) == 4); // Automatic + three named
    CHECK(std::string(Access::row_name(modal, 0)) == "Automatic");
    CHECK(std::string(Access::row_name(modal, 1)) == "Nozzle");
    CHECK(std::string(Access::row_note(modal, 1)).empty());
    CHECK(std::string(Access::row_name(modal, 2)) == "Chamber");
    CHECK(std::string(Access::row_note(modal, 2)) == "Snapshot only");
    CHECK(std::string(Access::row_name(modal, 3)) == "Toolhead");
    CHECK(std::string(Access::row_note(modal, 3)) == "Unavailable");
    CHECK(std::string(Access::row_name(modal, 4)).empty());

    // A saved name no row carries reads as Automatic in the picker.
    Access::load_config(modal, {{"source", "Gone"}});
    CHECK(Access::row_active(modal, 0) == 1);
    CHECK(Access::source(modal) == "Gone");
}

TEST_CASE_METHOD(LVGLTestFixture, "CameraConfigModal: destroying the modal withdraws its XML names",
                 "[camera][modal][xml_name]") {
    {
        CameraConfigModal modal("camera", TEST_PANEL, [](const nlohmann::json&) {});
        REQUIRE(lv_xml_get_subject(nullptr, "cam_rot_0_active") != nullptr);
        REQUIRE(lv_xml_get_subject(nullptr, "cam_source_0_name") != nullptr);
    }
    CHECK(lv_xml_get_subject(nullptr, "cam_rot_0_active") == nullptr);
    CHECK(lv_xml_get_subject(nullptr, "cam_source_0_name") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "CameraConfigModal: reopening before the old modal is freed keeps the new names",
                 "[camera][modal][xml_name]") {
    auto first =
        std::make_unique<CameraConfigModal>("camera", TEST_PANEL, [](const nlohmann::json&) {});
    CameraConfigModal second("camera", TEST_PANEL, [](const nlohmann::json&) {});
    lv_subject_t* rot = lv_xml_get_subject(nullptr, "cam_rot_0_active");
    REQUIRE(rot != nullptr);

    first.reset();
    CHECK(lv_xml_get_subject(nullptr, "cam_rot_0_active") == rot);
    CHECK(lv_xml_get_subject(nullptr, "cam_source_0_name") != nullptr);
}

#endif // HELIX_HAS_CAMERA
