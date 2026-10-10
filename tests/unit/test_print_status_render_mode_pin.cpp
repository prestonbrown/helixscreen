// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_render_mode_pin.cpp
 * @brief The print status viewer's render mode follows the preview ladder
 *
 * Command line beats HELIX_GCODE_MODE beats the persisted setting, both when
 * the panel activates and when the setting changes while it is showing.
 */

#include "ui_gcode_viewer.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "runtime_config.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int kSetting3D = 1;
constexpr int kSetting2D = 2;

/// Restores the three ladder sources a case pins.
class RenderModePinFixture : public LVGLUITestFixture {
  public:
    RenderModePinFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        saved_setting_ = DisplaySettingsManager::instance().get_gcode_render_mode();
        saved_cmdline_ = get_runtime_config()->gcode_render_mode;
        unsetenv("HELIX_GCODE_MODE");

        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        NavigationManager::instance().set_panels(panels);
    }

    ~RenderModePinFixture() override {
        NavigationManagerTestAccess::set_panel_stack(NavigationManager::instance(), {home_widget_});
        PrintStatusPanel::destroy_cached_overlay(
            helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
        drain();
        process_lvgl(20);
        unsetenv("HELIX_GCODE_MODE");
        get_runtime_config()->gcode_render_mode = saved_cmdline_;
        DisplaySettingsManager::instance().set_gcode_render_mode(saved_setting_);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    /// Open print status and return its G-code viewer.
    lv_obj_t* open_viewer() {
        REQUIRE(PrintStatusPanel::push_overlay(test_screen()));
        drain();
        lv_obj_t* viewer =
            PrintStatusPanelTestAccess::gcode_viewer(get_global_print_status_panel());
        REQUIRE(viewer != nullptr);
        REQUIRE(PrintStatusPanelTestAccess::is_active(get_global_print_status_panel()));
        return viewer;
    }

  private:
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
    int saved_setting_ = 0;
    int saved_cmdline_ = -1;
};

} // namespace

TEST_CASE_METHOD(RenderModePinFixture, "Activating print status keeps a command-line render mode",
                 "[print_status][render_mode]") {
    get_runtime_config()->gcode_render_mode = static_cast<int>(GcodeViewerRenderMode::Layer2D);
    DisplaySettingsManager::instance().set_gcode_render_mode(kSetting3D);

    lv_obj_t* viewer = open_viewer();

    CHECK(test_access::gcode_viewer_render_mode(viewer) == GcodeViewerRenderMode::Layer2D);
}

TEST_CASE_METHOD(RenderModePinFixture, "Activating print status keeps a HELIX_GCODE_MODE pin",
                 "[print_status][render_mode]") {
    setenv("HELIX_GCODE_MODE", "2D", 1);
    DisplaySettingsManager::instance().set_gcode_render_mode(kSetting3D);

    lv_obj_t* viewer = open_viewer();

    CHECK(test_access::gcode_viewer_render_mode(viewer) == GcodeViewerRenderMode::Layer2D);
}

TEST_CASE_METHOD(RenderModePinFixture, "Activating print status applies the persisted setting",
                 "[print_status][render_mode]") {
    DisplaySettingsManager::instance().set_gcode_render_mode(kSetting2D);

    lv_obj_t* viewer = open_viewer();

    CHECK(test_access::gcode_viewer_render_mode(viewer) == GcodeViewerRenderMode::Layer2D);
}

TEST_CASE_METHOD(RenderModePinFixture, "A settings change does not override a HELIX_GCODE_MODE pin",
                 "[print_status][render_mode]") {
    setenv("HELIX_GCODE_MODE", "2D", 1);
    DisplaySettingsManager::instance().set_gcode_render_mode(kSetting2D);
    lv_obj_t* viewer = open_viewer();

    DisplaySettingsManager::instance().set_gcode_render_mode(kSetting3D);
    drain();

    CHECK(test_access::gcode_viewer_render_mode(viewer) == GcodeViewerRenderMode::Layer2D);
}
