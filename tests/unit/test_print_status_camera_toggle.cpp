// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_camera_toggle.cpp
 * @brief The Print Status preview card's camera toggle
 *
 * The camera button shows only where the More menu's Camera row does (a
 * webcam, a remote screen). It swaps the preview for the live camera, the
 * choice persists, the stream runs only while the camera shows on an active
 * panel, and the G-code viewer pauses underneath it.
 */

#include "ui_gcode_viewer.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "printer_state.h"

#if HELIX_HAS_CAMERA
namespace helix {
bool open_standalone_camera_fullscreen(lv_obj_t* parent_screen);
}
#endif

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Print status pushed as the app pushes it, with a webcam whose URLs refuse
/// connections, so a stream starts and stops without a camera.
class CameraToggleFixture : public LVGLUITestFixture {
  public:
    CameraToggleFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        DisplaySettingsManager::instance().set_print_status_show_camera(false);

        // This fixture re-creates PrinterState's subjects, which ends the
        // observers a panel built in an earlier case holds on them.
        PrintStatusPanel::destroy_cached_overlay(
            helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
        drain();
        panel().deinit_subjects();
        panel().init_subjects();

        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        NavigationManager::instance().set_panels(panels);
    }

    ~CameraToggleFixture() override {
        NavigationManagerTestAccess::set_panel_stack(NavigationManager::instance(), {home_widget_});
        PrintStatusPanel::destroy_cached_overlay(
            helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
        drain();
        process_lvgl(20);
        get_printer_state().capabilities_state().set_webcam_available(false);
        get_printer_state().set_moonraker_is_remote(false);
        DisplaySettingsManager::instance().set_print_status_show_camera(false);
        drain();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static void set_webcam(bool present) {
        get_printer_state().capabilities_state().set_webcam_available(
            present, "http://127.0.0.1:9/stream", "http://127.0.0.1:9/snapshot.jpg");
        drain();
    }
    static void set_remote(bool remote) {
        get_printer_state().set_moonraker_is_remote(remote);
        drain();
    }

    static PrintStatusPanel& panel() {
        return get_global_print_status_panel();
    }

    void open() {
        REQUIRE(PrintStatusPanel::push_overlay(test_screen()));
        drain();
        REQUIRE(PrintStatusPanelTestAccess::is_active(panel()));
    }

    static lv_obj_t* find(const char* name) {
        lv_obj_t* root = PrintStatusPanel::get_cached_overlay();
        REQUIRE(root != nullptr);
        lv_obj_t* obj = lv_obj_find_by_name(root, name);
        REQUIRE(obj != nullptr);
        return obj;
    }
    static bool hidden(lv_obj_t* obj) {
        return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    static int subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }

    void tap_camera_button() {
        lv_obj_send_event(find("btn_camera_toggle"), LV_EVENT_CLICKED, nullptr);
        drain();
    }

    /// The More menu's Camera row, with the menu raised below the More button.
    lv_obj_t* more_menu_camera_row() {
        PrintStatusPanelTestAccess::open_more_menu(panel(), find("btn_more"));
        drain();
        lv_obj_t* row = lv_obj_find_by_name(lv_obj_get_screen(find("btn_more")), "btn_more_camera");
        REQUIRE(row != nullptr);
        return row;
    }

  private:
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(CameraToggleFixture,
                 "Print status camera button needs a webcam and a remote screen",
                 "[print_status][camera]") {
    open();
    lv_obj_t* button = find("btn_camera_toggle");

    SECTION("webcam on a remote screen shows it, and the More row with it") {
        set_webcam(true);
        set_remote(true);
        CHECK(subject("print_status_camera_available") == 1);
        CHECK_FALSE(hidden(button));
        CHECK_FALSE(hidden(more_menu_camera_row()));
    }
    SECTION("webcam on the printer's own screen hides it") {
        set_webcam(true);
        set_remote(false);
        CHECK(subject("print_status_camera_available") == 0);
        CHECK(hidden(button));
        CHECK(hidden(more_menu_camera_row()));
    }
    SECTION("a remote screen without a webcam hides it") {
        set_webcam(false);
        set_remote(true);
        CHECK(subject("print_status_camera_available") == 0);
        CHECK(hidden(button));
        CHECK(hidden(more_menu_camera_row()));
    }
}

TEST_CASE_METHOD(CameraToggleFixture,
                 "Print status camera button swaps the preview for the camera and back",
                 "[print_status][camera]") {
    set_webcam(true);
    set_remote(true);
    open();
    PrintStatusPanelTestAccess::show_gcode_viewer(panel(), true);
    drain();
    lv_obj_t* camera = find("preview_camera");
    lv_obj_t* view_toggle = find("btn_view_toggle");
    REQUIRE(hidden(camera));
    REQUIRE_FALSE(hidden(view_toggle));

    tap_camera_button();
    CHECK(DisplaySettingsManager::instance().get_print_status_show_camera());
    CHECK(subject("print_status_camera_showing") == 1);
    CHECK_FALSE(hidden(camera));
    CHECK(hidden(view_toggle));

    // The connecting indicator stays up until a frame is on screen.
    lv_obj_t* connecting = find("preview_camera_connecting");
    CHECK_FALSE(hidden(connecting));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "print_status_camera_has_frame"), 1);
    CHECK(hidden(connecting));

    tap_camera_button();
    CHECK_FALSE(DisplaySettingsManager::instance().get_print_status_show_camera());
    CHECK(subject("print_status_camera_showing") == 0);
    CHECK(hidden(camera));
    CHECK_FALSE(hidden(view_toggle));
}

#if HELIX_HAS_CAMERA
TEST_CASE_METHOD(CameraToggleFixture,
                 "Print status camera streams only while it shows on an active panel",
                 "[print_status][camera]") {
    set_webcam(true);
    set_remote(true);
    open();
    REQUIRE_FALSE(PrintStatusPanelTestAccess::camera_streaming(panel()));

    tap_camera_button();
    CHECK(PrintStatusPanelTestAccess::camera_streaming(panel()));

    SECTION("deactivating stops it and activating restarts it") {
        panel().on_deactivate(DeactivateReason::NavigateAway);
        drain();
        CHECK_FALSE(PrintStatusPanelTestAccess::camera_streaming(panel()));
        panel().on_activate();
        drain();
        CHECK(PrintStatusPanelTestAccess::camera_streaming(panel()));
    }
    SECTION("toggling back to the preview stops it") {
        tap_camera_button();
        CHECK_FALSE(PrintStatusPanelTestAccess::camera_streaming(panel()));
    }
    SECTION("losing the webcam stops it and shows the preview, keeping the choice") {
        set_webcam(false);
        CHECK_FALSE(PrintStatusPanelTestAccess::camera_streaming(panel()));
        CHECK(subject("print_status_camera_showing") == 0);
        CHECK(hidden(find("preview_camera")));
        CHECK(DisplaySettingsManager::instance().get_print_status_show_camera());
    }
}
#endif

#if HELIX_HAS_CAMERA
TEST_CASE_METHOD(CameraToggleFixture,
                 "Print status camera restarts when More > Camera opens no viewer",
                 "[print_status][camera]") {
    set_webcam(true);
    set_remote(true);
    open();
    tap_camera_button();
    REQUIRE(PrintStatusPanelTestAccess::camera_streaming(panel()));

    // A fullscreen viewer is already up (its push still queued), so the menu
    // row opens nothing.
    REQUIRE(open_standalone_camera_fullscreen(test_screen()));
    PrintStatusPanelTestAccess::more_action(panel(), helix::ui::PrintStatusMoreMenu::kActionCamera);
    CHECK(PrintStatusPanelTestAccess::camera_streaming(panel()));

    drain();
    helix::nav::go_back();
    drain();
}
#endif

#if HELIX_HAS_CAMERA
TEST_CASE_METHOD(CameraToggleFixture,
                 "Print status More > Camera hands the camera to the viewer it opens",
                 "[print_status][camera]") {
    set_webcam(true);
    set_remote(true);
    open();
    tap_camera_button();
    REQUIRE(PrintStatusPanelTestAccess::camera_streaming(panel()));

    PrintStatusPanelTestAccess::more_action(panel(), helix::ui::PrintStatusMoreMenu::kActionCamera);
    CHECK_FALSE(PrintStatusPanelTestAccess::camera_streaming(panel()));

    drain();
    helix::nav::go_back();
    drain();
}

#endif

TEST_CASE_METHOD(CameraToggleFixture, "Print status G-code viewer pauses under the camera",
                 "[print_status][camera]") {
    set_webcam(true);
    set_remote(true);
    open();
    lv_obj_t* viewer = PrintStatusPanelTestAccess::gcode_viewer(panel());
    REQUIRE(viewer != nullptr);
    PrintStatusPanelTestAccess::show_gcode_viewer(panel(), true);
    drain();
    REQUIRE_FALSE(helix::ui::gcode_viewer_is_paused(viewer));

    tap_camera_button();
    CHECK(helix::ui::gcode_viewer_is_paused(viewer));

    SECTION("the preview controller re-showing the viewer keeps it paused") {
        PrintStatusPanelTestAccess::show_gcode_viewer(panel(), true);
        CHECK(helix::ui::gcode_viewer_is_paused(viewer));
    }
    SECTION("toggling back resumes it") {
        tap_camera_button();
        CHECK_FALSE(helix::ui::gcode_viewer_is_paused(viewer));
    }
}
