// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_network_settings_teardown_uaf.cpp
 * @brief The overlay must uninstall its modal DELETE hooks before it dies
 *
 * NetworkSettingsOverlay arms an LV_EVENT_DELETE hook carrying `this` on every
 * modal it opens, so a modal the modal system closes on its own cannot leave a
 * dangling cached pointer behind (#1341). The hook is the mirror image of that
 * problem when the overlay dies first: ~NetworkSettingsOverlay() closes any
 * still-open modal with modal_hide(), which only starts an exit ANIMATION -
 * ModalStack::animate_exit() deletes the tree when the animation completes,
 * after the destructor has returned. The overlay is owned by
 * StaticPanelRegistry, whose destroy_all() runs before lv_deinit(), so the
 * modal tree routinely outlives it and on_modal_deleted() then writes
 * `*cached = nullptr` through freed memory.
 *
 * Same family as the ASan-confirmed PowerPanel hook fixed in b0afff654 (#1298),
 * and tested the same way: by reading LVGL's event list rather than triggering
 * the crash, so it fails in a plain build.
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

    /// A stand-in for a modal: a real screen child, watched by the real
    /// handler, exactly as show_password_modal() and friends arm it.
    lv_obj_t* watched_modal(NetworkSettingsOverlay& overlay) {
        lv_obj_t* modal = lv_obj_create(test_screen());
        REQUIRE(modal != nullptr);
        Access::watch(overlay, modal);
        return modal;
    }
};

} // namespace

TEST_CASE_METHOD(NetworkOverlayTeardownFixture,
                 "NetworkSettingsOverlay uninstalls every modal delete hook when destroyed",
                 "[network_settings][teardown][uaf]") {
    auto overlay = make_overlay();

    lv_obj_t* password = watched_modal(*overlay);
    lv_obj_t* hidden = watched_modal(*overlay);
    lv_obj_t* test_modal = watched_modal(*overlay);
    lv_obj_t* step = watched_modal(*overlay);
    Access::password_modal(*overlay) = password;
    Access::hidden_network_modal(*overlay) = hidden;
    Access::test_modal(*overlay) = test_modal;
    Access::step_widget(*overlay) = step;

    // Guards against the test passing for the wrong reason: with no hook armed,
    // its absence after destruction would prove nothing.
    const void* dead = overlay.get();
    REQUIRE(event_hook_installed(password, dead));
    REQUIRE(event_hook_installed(hidden, dead));
    REQUIRE(event_hook_installed(test_modal, dead));
    REQUIRE(event_hook_installed(step, dead));

    overlay.reset();

    // All four slots share one handler, and step_widget_ is the one a partial
    // fix drops - it is a grandchild of test_modal_ rather than a modal.
    CHECK_FALSE(event_hook_installed(password, dead));
    CHECK_FALSE(event_hook_installed(hidden, dead));
    CHECK_FALSE(event_hook_installed(test_modal, dead));
    CHECK_FALSE(event_hook_installed(step, dead));

    // The modals outlive the overlay exactly as they do when animate_exit()
    // completes after destroy_all(). With a hook still installed, this is
    // on_modal_deleted() running on freed memory.
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

    lv_obj_t* early = watched_modal(*overlay);
    lv_obj_t* survivor = watched_modal(*overlay);
    Access::password_modal(*overlay) = early;
    Access::hidden_network_modal(*overlay) = survivor;

    // The #1341 ordering: the modal system kills one modal while the overlay
    // is still alive, and the hook clears that slot.
    lv_obj_delete(early);
    REQUIRE(Access::password_modal(*overlay) == nullptr);
    REQUIRE(Access::hidden_network_modal(*overlay) == survivor);

    // The destructor has to walk past the nulled slot rather than into it, and
    // still disarm the one that is left.
    const void* dead = overlay.get();
    overlay.reset();

    CHECK_FALSE(event_hook_installed(survivor, dead));

    lv_obj_delete(survivor);
    process_lvgl(50);
    SUCCEED("mixed live/dead modal set torn down without touching freed memory");
}
