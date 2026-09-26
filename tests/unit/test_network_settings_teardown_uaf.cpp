// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_teardown_uaf.cpp
 * @brief The overlay must leave no modal DELETE hook behind when it dies
 *
 * NetworkSettingsOverlay watches every modal it opens so a modal the modal
 * system closes on its own leaves a null behind (#1341). The mirror image is
 * the overlay dying first: ~NetworkSettingsOverlay() closes any still-open
 * modal with modal_hide(), which only starts an exit ANIMATION, and the modal
 * tree is deleted after the destructor has returned. The overlay is owned by
 * StaticPanelRegistry, whose destroy_all() runs before lv_deinit(), so the
 * modal tree routinely outlives it, and a hook still installed would then
 * write through freed memory (#1298).
 *
 * Tested by reading LVGL's event list rather than triggering the crash, so it
 * fails in a plain build.
 */

#include "ui_overlay_network_settings.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/lvgl_event_hook_probe.h"
#include "../test_helpers/network_settings_overlay_test_access.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using Access = NetworkSettingsOverlayTestAccess;

namespace {

class NetworkOverlayTeardownFixture : public LVGLTestFixture {
  protected:
    /// A locally owned overlay, not get_network_settings_overlay(): the
    /// production instance is destroyed by StaticPanelRegistry::destroy_all(),
    /// and running that destructor while the widget tree still stands is the
    /// whole point of these tests.
    std::unique_ptr<NetworkSettingsOverlay> make_overlay() {
        return std::make_unique<NetworkSettingsOverlay>();
    }

    /// A stand-in for a modal: a real screen child. Assigning it to one of the
    /// overlay's handles arms the watch, exactly as show_password_modal() and
    /// friends do.
    lv_obj_t* watched_modal() {
        lv_obj_t* modal = lv_obj_create(test_screen());
        REQUIRE(modal != nullptr);
        return modal;
    }
};

} // namespace

TEST_CASE_METHOD(NetworkOverlayTeardownFixture,
                 "NetworkSettingsOverlay uninstalls every modal delete hook when destroyed",
                 "[network_settings][teardown][uaf]") {
    auto overlay = make_overlay();

    lv_obj_t* password = watched_modal();
    lv_obj_t* hidden = watched_modal();
    lv_obj_t* test_modal = watched_modal();
    lv_obj_t* step = watched_modal();
    Access::password_modal(*overlay) = password;
    Access::hidden_network_modal(*overlay) = hidden;
    Access::test_modal(*overlay) = test_modal;
    Access::step_widget(*overlay) = step;

    // Guards against the test passing for the wrong reason: with no hook armed,
    // its absence after destruction would prove nothing. Each hook carries the
    // address of its handle, which lives inside the overlay.
    const void* dead_password = &Access::password_modal(*overlay);
    const void* dead_hidden = &Access::hidden_network_modal(*overlay);
    const void* dead_test = &Access::test_modal(*overlay);
    const void* dead_step = &Access::step_widget(*overlay);
    REQUIRE(event_hook_installed(password, dead_password));
    REQUIRE(event_hook_installed(hidden, dead_hidden));
    REQUIRE(event_hook_installed(test_modal, dead_test));
    REQUIRE(event_hook_installed(step, dead_step));

    overlay.reset();

    CHECK_FALSE(event_hook_installed(password, dead_password));
    CHECK_FALSE(event_hook_installed(hidden, dead_hidden));
    CHECK_FALSE(event_hook_installed(test_modal, dead_test));
    CHECK_FALSE(event_hook_installed(step, dead_step));

    // The modals outlive the overlay exactly as they do when animate_exit()
    // completes after destroy_all(). A hook still installed would write into
    // the freed overlay here.
    lv_obj_delete(password);
    lv_obj_delete(hidden);
    lv_obj_delete(test_modal);
    lv_obj_delete(step);
    process_lvgl(50);
    SUCCEED("modals torn down after the overlay without touching freed memory");
}

TEST_CASE_METHOD(NetworkOverlayTeardownFixture,
                 "A modal that died first leaves the overlay destructor nothing to uninstall",
                 "[network_settings][teardown][uaf]") {
    auto overlay = make_overlay();

    lv_obj_t* early = watched_modal();
    lv_obj_t* survivor = watched_modal();
    Access::password_modal(*overlay) = early;
    Access::hidden_network_modal(*overlay) = survivor;

    // The #1341 ordering: the modal system kills one modal while the overlay
    // is still alive, and the hook clears that slot.
    lv_obj_delete(early);
    REQUIRE(Access::password_modal(*overlay) == nullptr);
    REQUIRE(Access::hidden_network_modal(*overlay) == survivor);

    // The destructor has to walk past the nulled handle rather than into it,
    // and still disarm the one that is left.
    const void* dead = &Access::hidden_network_modal(*overlay);
    REQUIRE(event_hook_installed(survivor, dead));
    overlay.reset();

    CHECK_FALSE(event_hook_installed(survivor, dead));

    lv_obj_delete(survivor);
    process_lvgl(50);
    SUCCEED("mixed live/dead modal set torn down without touching freed memory");
}
