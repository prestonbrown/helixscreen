// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_camera_lifecycle.cpp
 * @brief The preview card camera's subjects and stream across the panel's own lifecycle
 */

#include "ui_panel_print_status.h"

#include "../test_helpers/print_status_panel_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using print_status_panel_test::PrintStatusPanelFixture;

namespace {

void set_remote_webcam(bool on) {
    get_printer_state().capabilities_state().set_webcam_available(
        on, "http://127.0.0.1:9/stream", "http://127.0.0.1:9/snapshot.jpg");
    get_printer_state().set_moonraker_is_remote(on);
    helix::ui::UpdateQueue::instance().drain();
}

/// A panel built on a printer with a webcam and a remote screen.
class RemoteWebcamPanelFixture : public PrintStatusPanelFixture {
  public:
    RemoteWebcamPanelFixture()
        : PrintStatusPanelFixture([]() -> std::shared_ptr<void> {
              set_remote_webcam(true);
              return std::shared_ptr<void>(nullptr, [](void*) { set_remote_webcam(false); });
          }) {}
};

int subject_int(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

} // namespace

TEST_CASE_METHOD(RemoteWebcamPanelFixture,
                 "Print status camera availability is right before any observer fires",
                 "[print_status][camera]") {
    // No drain since init_subjects(): the deferred observers have not run.
    CHECK(subject_int("print_status_camera_available") == 1);
}

TEST_CASE_METHOD(RemoteWebcamPanelFixture,
                 "Print status deinit_subjects lets go of the camera's inputs",
                 "[print_status][camera]") {
    helix::ui::UpdateQueue::instance().drain();
    delete_widget_tree();
    lv_subject_t* choice = DisplaySettingsManager::instance().subject_print_status_show_camera();
    const uint32_t before = lv_ll_get_len(&choice->subs_ll);
    panel().deinit_subjects();
    CHECK(lv_ll_get_len(&choice->subs_ll) == before - 1);
}

#if HELIX_HAS_CAMERA
TEST_CASE_METHOD(RemoteWebcamPanelFixture,
                 "Print status destroyed with its tree alive clears the camera image",
                 "[print_status][camera]") {
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* image = lv_obj_find_by_name(root_, "preview_camera_image");
    REQUIRE(image != nullptr);
    PrintStatusPanelTestAccess::start_preview_camera(panel());
    REQUIRE(PrintStatusPanelTestAccess::camera_streaming(panel()));

    // A frame on screen when the panel goes.
    lv_draw_buf_t* frame = lv_draw_buf_create(4, 4, LV_COLOR_FORMAT_RGB888, 0);
    lv_image_set_src(image, frame);

    panel_release();
    CHECK(lv_image_get_src(image) == nullptr);
    lv_image_set_src(image, nullptr);
    lv_draw_buf_destroy(frame);
}

TEST_CASE_METHOD(RemoteWebcamPanelFixture, "Print status camera stops with its widget tree",
                 "[print_status][camera]") {
    helix::ui::UpdateQueue::instance().drain();
    PrintStatusPanelTestAccess::start_preview_camera(panel());
    REQUIRE(PrintStatusPanelTestAccess::camera_streaming(panel()));

    delete_widget_tree();
    CHECK_FALSE(PrintStatusPanelTestAccess::camera_streaming(panel()));
}
#endif
