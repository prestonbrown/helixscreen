#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Artillery M1 Pro

platform_stop_competing_uis() {
    # Stop and persistently disable Artillery M1 Pro stock UI services
    # algo_app.service and makerbase-client.services are the competing UIs
    
    if command -v systemctl >/dev/null 2>&1; then
        # Stop algo_app.service (AI service)
        systemctl stop algo_app.service 2>/dev/null || true
        systemctl disable algo_app.service 2>/dev/null || true
        # Stop makerbase-client.services (LCD)
        systemctl stop makerbase-client.services 2>/dev/null || true
        systemctl disable makerbase-client.services 2>/dev/null || true
    else
        # Fallback for non-systemd init systems
        for proc in algo_app makerbase-client; do
            killall "$proc" 2>/dev/null || true
        done
    fi
}

platform_enable_backlight() {
    :
}

platform_wait_for_services() {
    :
}

platform_pre_start() {
    export HELIX_CACHE_DIR="/usr/data/helixscreen/cache"
}

platform_post_stop() {
    :
}
