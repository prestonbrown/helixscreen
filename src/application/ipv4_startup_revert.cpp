// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipv4_startup_revert.h"

#include "ui_error_reporting.h"
#include "ui_timer_guard.h"

#include "core/http_executor.h"
#include "core/update_queue.h"
#include "ipv4_config_system.h"

#include <lvgl/lvgl.h>
#include <spdlog/spdlog.h>

namespace helix {

namespace {

struct StartupRevert {
    net::Ipv4Link link = net::Ipv4Link::Wifi;
    net::Ipv4RetrySchedule schedule;
    ui::LvglTimerGuard timer;
    bool in_flight = false;
};

StartupRevert& state() {
    static StartupRevert s;
    return s;
}

void finish(bool ok);

void attempt() {
    StartupRevert& s = state();
    s.in_flight = true;
    s.schedule.tried(lv_tick_get());
    const net::Ipv4Link link = s.link;
    http::HttpExecutor::fast().start();
    http::HttpExecutor::fast().submit([link]() {
        const bool ok = net::restore_pending_ipv4(link, /*stale_only=*/true).ok();
        ui::queue_update("Ipv4StartupRevert::finish", [ok]() { finish(ok); });
    });
}

void finish(bool ok) {
    StartupRevert& s = state();
    s.in_flight = false;
    if (ok) {
        s.timer.reset();
        spdlog::info("[Ipv4] The IPv4 change never kept is undone");
        NOTIFY_INFO("{}", lv_tr("Previous IPv4 settings restored"));
        return;
    }
    if (s.schedule.exhausted(lv_tick_get())) {
        s.timer.reset();
        spdlog::error("[Ipv4] Gave up restoring the previous IPv4 config; the record stays for "
                      "the next start");
        NOTIFY_ERROR("{}", lv_tr("Could not restore the previous IPv4 settings. They are "
                                 "restored at the next start."));
    }
}

} // namespace

void start_ipv4_startup_revert() {
    const auto pending = net::load_pending_ipv4_revert();
    if (!pending || !net::is_stale_pending_ipv4_revert(pending, pending->link)) {
        return;
    }
    spdlog::warn("[Ipv4] A {} IPv4 change was never kept: restoring {}",
                 net::ipv4_link_name(pending->link), net::ipv4_config_text(pending->previous));
    StartupRevert& s = state();
    s.link = pending->link;
    s.schedule.start(lv_tick_get());
    s.timer.reset(lv_timer_create(
        [](lv_timer_t*) {
            StartupRevert& st = state();
            if (!st.in_flight && st.schedule.due(lv_tick_get())) {
                attempt();
            }
        },
        1000, nullptr));
    attempt();
}

} // namespace helix
