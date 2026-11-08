// SPDX-License-Identifier: GPL-3.0-or-later
//
// QrScannerOverlay behind the screensaver: the camera stops, the caller's
// result and cancel callbacks survive for the scan after the wake.

#include "ui_overlay_qr_scanner.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/qr_scanner_overlay_test_access.h"
#include "spoolman_types.h"

#include "../catch_amalgamated.hpp"

using helix::ui::QrScannerOverlay;
using helix::ui::QrScannerOverlayTestAccess;

TEST_CASE_METHOD(LVGLTestFixture, "a screensaver suspend keeps the scanner's caller callbacks",
                 "[qr_scanner][suspend]") {
    QrScannerOverlay overlay;
    int results = 0;
    int cancels = 0;
    QrScannerOverlayTestAccess::set_callbacks(
        overlay, [&](const SpoolInfo&) { ++results; }, [&]() { ++cancels; });

    overlay.on_deactivate(DeactivateReason::Suspended);

    auto& on_result = QrScannerOverlayTestAccess::result_callback(overlay);
    auto& on_cancel = QrScannerOverlayTestAccess::cancel_callback(overlay);
    REQUIRE(on_result);
    REQUIRE(on_cancel);
    on_result(SpoolInfo{});
    on_cancel();
    CHECK(results == 1);
    CHECK(cancels == 1);

    // Closing the scanner drops them.
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    CHECK_FALSE(QrScannerOverlayTestAccess::result_callback(overlay));
    CHECK_FALSE(QrScannerOverlayTestAccess::cancel_callback(overlay));
}
