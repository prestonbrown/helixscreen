// SPDX-License-Identifier: GPL-3.0-or-later

#include "u1_stock_screen.h"

#include "ui_notification.h"

#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace helix::u1 {

bool root_ssh_down(const char* flag) {
    return flag != nullptr && std::strcmp(flag, "1") == 0;
}

void surface_root_ssh_down_from_hook() {
    if (!root_ssh_down(std::getenv(kRootSshDownEnv))) {
        return;
    }
    spdlog::error("[U1] Root Access is on but SSH (dropbear) did not start");
    // Sticky: with the stock screen disabled, SSH is how the user gets back to
    // it, so this must stay until read.
    ui_notification_warning_sticky(
        lv_tr("SSH did not start although Root access is on"),
        lv_tr("Restart the printer. If SSH still refuses, see the Snapmaker U1 install guide"));
}

bool restore_stock_screen_offered(const char* trigger_path) {
    return trigger_path != nullptr && *trigger_path != '\0';
}

bool write_restore_trigger(const std::string& path) {
    std::ofstream out(path, std::ios::trunc);
    out << "Restore the stock screen at the next start (written by HelixScreen Settings)\n";
    out.close();
    if (!out) {
        spdlog::error("[U1] Could not write the restore-stock trigger {}", path);
        return false;
    }
    spdlog::warn("[U1] Restore-stock trigger written: {}", path);
    return true;
}

} // namespace helix::u1
