// SPDX-License-Identifier: GPL-3.0-or-later

#include "u1_stock_screen.h"

#include "ui_notification.h"

#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>

namespace helix::u1 {

bool root_ssh_down(const char* flag) {
    return flag != nullptr && std::strcmp(flag, "1") == 0;
}

void surface_root_ssh_down_from_hook() {
    if (!root_ssh_down(std::getenv(kRootSshDownEnv))) {
        return;
    }
    spdlog::error("[U1] Root Access is on but SSH (dropbear) did not start");
    // With the stock screen disabled, SSH is how the user gets back to it.
    ui_notification_warning_with_detail(
        lv_tr("SSH did not start although Root access is on"),
        lv_tr("Restart the printer. If SSH still refuses, see the Snapmaker U1 install guide"));
}

} // namespace helix::u1
