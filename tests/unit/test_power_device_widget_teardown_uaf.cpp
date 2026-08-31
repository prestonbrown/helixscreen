// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_power_device_widget_teardown_uaf.cpp
 * @brief The picker backdrop's DELETE hook must not outlive the widget
 *
 * show_device_picker() arms an LV_EVENT_DELETE hook carrying `this` on
 * picker_backdrop_, so a backdrop killed with its parent screen still clears
 * picker_backdrop_ and s_active_picker_. dismiss_device_picker() destroys the
 * backdrop through safe_delete_deferred(), i.e. lv_obj_delete_async(), so the
 * backdrop dies a lv_timer_handler tick later than the call that dismissed it.
 * ~PowerDeviceWidget() reaches dismiss through detach(), which puts the
 * widget's own death inside that window: the hook then runs
 * `self->picker_backdrop_ = nullptr` and compares s_active_picker_ against
 * freed memory. PanelWidget instances are recycled across rebuilds, so this
 * attach/detach cycling is routine rather than exotic.
 *
 * Same family as the ASan-confirmed PowerPanel hook fixed in b0afff654 (#1298),
 * and tested the same way: by reading LVGL's event list rather than triggering
 * the crash, so it fails in a plain build.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/lvgl_event_hook_probe.h"
#include "../test_helpers/power_device_widget_test_access.h"
#include "moonraker_types.h"
#include "power_device_state.h"
#include "power_device_widget.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = helix::PowerDeviceWidgetTestAccess;

namespace {

class PowerPickerTeardownFixture : public LVGLUITestFixture {
  public:
    PowerPickerTeardownFixture() {
        // show_device_picker() returns early when no power devices exist, so
        // without these there is no backdrop and nothing to hook.
        PowerDeviceState::instance().set_devices({
            PowerDevice{"printer_psu", "gpio", "off", false},
            PowerDevice{"chamber_light", "gpio", "on", false},
        });
    }

    ~PowerPickerTeardownFixture() override {
        PowerDeviceState::instance().deinit_subjects();
    }

    /// A widget with just enough state to open its picker. attach() would also
    /// build status observers and the energy carousel; the picker's lifetime
    /// involves neither, and PanelWidgetManager sets parent_screen_ the same
    /// way for a widget that has never been attached.
    std::unique_ptr<PowerDeviceWidget> make_widget(const char* id) {
        auto widget = std::make_unique<PowerDeviceWidget>(id);
        Access::set_parent_screen(*widget, test_screen());
        return widget;
    }
};

} // namespace

TEST_CASE_METHOD(PowerPickerTeardownFixture,
                 "PowerDeviceWidget uninstalls the picker delete hook before it is destroyed",
                 "[power_device_widget][teardown][uaf]") {
    auto widget = make_widget("power_device:1");
    Access::show_picker(*widget);

    lv_obj_t* backdrop = Access::picker_backdrop(*widget);
    REQUIRE(backdrop != nullptr);
    // Guards against the test passing for the wrong reason: with no hook armed,
    // its absence after destruction would prove nothing.
    REQUIRE(event_hook_installed(backdrop, widget.get()));

    const void* dead = widget.get();
    widget.reset(); // ~PowerDeviceWidget -> detach() -> dismiss_device_picker()

    // safe_delete_deferred() only queues the delete, so the backdrop is still
    // standing - which is exactly the window the hook used to fire in.
    REQUIRE(lv_obj_is_valid(backdrop));
    CHECK_FALSE(event_hook_installed(backdrop, dead));
    CHECK(Access::active_picker() == nullptr);

    // LVGL's async pass is where the backdrop actually dies. With the hook
    // still installed this writes through the freed widget.
    process_lvgl(50);
    CHECK_FALSE(lv_obj_is_valid(backdrop));
}

TEST_CASE_METHOD(PowerPickerTeardownFixture,
                 "A picker backdrop killed with its parent still clears the widget's pointers",
                 "[power_device_widget][teardown][uaf]") {
    // The hook is not dead weight once the dismiss path disarms it: a screen
    // teardown destroys the backdrop without anyone calling
    // dismiss_device_picker(), and both pointers still have to be cleared.
    auto widget = make_widget("power_device:1");
    Access::show_picker(*widget);

    lv_obj_t* backdrop = Access::picker_backdrop(*widget);
    REQUIRE(backdrop != nullptr);
    REQUIRE(Access::active_picker() == widget.get());

    lv_obj_delete(backdrop);

    CHECK(Access::picker_backdrop(*widget) == nullptr);
    CHECK(Access::active_picker() == nullptr);
}

TEST_CASE_METHOD(PowerPickerTeardownFixture,
                 "A dismissed backdrop dying later leaves a re-opened picker alone",
                 "[power_device_widget][teardown][uaf]") {
    // Dismiss and re-open inside one async tick: the first backdrop's DELETE
    // lands while the second is already on screen, and the widget's pointers
    // now name the second one.
    auto widget = make_widget("power_device:1");
    Access::show_picker(*widget);
    lv_obj_t* first = Access::picker_backdrop(*widget);
    REQUIRE(first != nullptr);

    Access::dismiss_picker(*widget);
    REQUIRE(lv_obj_is_valid(first)); // queued, not gone

    Access::show_picker(*widget);
    lv_obj_t* second = Access::picker_backdrop(*widget);
    REQUIRE(second != nullptr);
    REQUIRE(second != first);

    process_lvgl(50); // the first backdrop's async delete lands here

    CHECK(Access::picker_backdrop(*widget) == second);
    CHECK(Access::active_picker() == widget.get());

    widget.reset();
    process_lvgl(50);
}
