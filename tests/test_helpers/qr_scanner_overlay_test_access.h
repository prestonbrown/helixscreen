// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_overlay_qr_scanner.h"

namespace helix::ui {

/// Private callback access for QrScannerOverlay tests: the public opener,
/// show(), also pushes the overlay onto the navigation stack.
struct QrScannerOverlayTestAccess {
    static void set_callbacks(QrScannerOverlay& o, QrScannerOverlay::ResultCallback on_result,
                              QrScannerOverlay::CancelCallback on_cancel) {
        o.result_callback_ = std::move(on_result);
        o.cancel_callback_ = std::move(on_cancel);
    }
    static QrScannerOverlay::ResultCallback& result_callback(QrScannerOverlay& o) {
        return o.result_callback_;
    }
    static QrScannerOverlay::CancelCallback& cancel_callback(QrScannerOverlay& o) {
        return o.cancel_callback_;
    }
};

} // namespace helix::ui
