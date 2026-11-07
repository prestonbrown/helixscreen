// SPDX-License-Identifier: GPL-3.0-or-later
//
// QrScannerOverlay behind the screensaver: the camera stops, the caller's
// result and cancel callbacks survive for the scan after the wake.

#include "ui_nav_manager.h"
#include "ui_overlay_qr_scanner.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "spoolman_types.h"

#include <array>

#include "../catch_amalgamated.hpp"

using helix::ui::get_qr_scanner_overlay;

TEST_CASE_METHOD(LVGLUITestFixture, "a screensaver suspend keeps the scanner's caller callbacks",
                 "[qr_scanner][suspend]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(lv_screen_active());
    }
    auto& nav = NavigationManager::instance();
    nav.set_panels(panels.data());
    auto& queue = helix::ui::UpdateQueue::instance();

    int cancels = 0;
    get_qr_scanner_overlay().show(
        lv_screen_active(), 0, [](const SpoolInfo&) {}, [&cancels]() { ++cancels; });
    queue.drain();
    nav.suspend_active();
    queue.drain();
    nav.resume_active();
    queue.drain();

    lv_event_cb_t close = lv_xml_get_event_cb(nullptr, "on_qr_scanner_close");
    REQUIRE(close != nullptr);
    close(nullptr);
    queue.drain();
    CHECK(cancels == 1);

    // Closing the scanner dropped them, so a stray close reaches no caller.
    close(nullptr);
    queue.drain();
    CHECK(cancels == 1);
    StaticPanelRegistry::instance().destroy_all();
    queue.drain();
}
