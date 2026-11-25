// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The DRM backend's evdev devices are wake sources: while the display sleeps, the
// main loop waits on their fds instead of polling them (prestonbrown/helixscreen#2037).

#ifdef HELIX_DISPLAY_DRM

#include "display_backend_drm.h"
#include "input_wake_source_list.h"
#include "lvgl_test_fixture.h"
#include "test_helpers/scoped_env.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

namespace {

bool wakes_on(const std::vector<helix::InputWakeSource>& sources, const lv_indev_t* indev) {
    return std::any_of(sources.begin(), sources.end(),
                       [indev](const helix::InputWakeSource& source) {
                           return source.indev == indev && source.fd >= 0;
                       });
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "DRM backend: the evdev devices it opens are wake sources",
                 "[display][drm][sleep_input_wait]") {
    // An explicit path is the whole decision, so no device is scanned for and nothing
    // of the machine this runs on is opened. libinput refuses /dev/null, so the evdev
    // driver opens it on a libinput build too.
    helix::ScopedEnv touch("HELIX_TOUCH_DEVICE", "/dev/null");
    helix::ScopedEnv keyboard("HELIX_KEYBOARD_DEVICE", "/dev/null");
    helix::ScopedEnv mouse("HELIX_MOUSE_DEVICE", nullptr);
    {
        DisplayBackendDRM backend("/dev/dri/card99");
        lv_indev_t* pointer = backend.create_input_pointer();
        lv_indev_t* keypad = backend.create_input_keyboard();
        REQUIRE(pointer != nullptr);
        REQUIRE(keypad != nullptr);
        CHECK(wakes_on(backend.input_wake_sources(), pointer));
        CHECK(wakes_on(backend.input_wake_sources(), keypad));
        REQUIRE(backend.input_wake_sources().size() == 2);

        // A device LVGL deletes, as evdev does for an unplugged keyboard, stops being one.
        lv_indev_delete(keypad);
        CHECK_FALSE(wakes_on(backend.input_wake_sources(), keypad));
        CHECK(backend.input_wake_sources().size() == 1);

        // The devices go before the backend whose hooks they carry.
        lv_indev_delete(pointer);
        CHECK(backend.input_wake_sources().empty());
    }
}

#endif // HELIX_DISPLAY_DRM
