// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ipv4_revert_retry.h"

#include "ui_error_reporting.h"
#include "ui_timer_guard.h"

#include "app_globals.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "ipv4_config_system.h"
#include "printer_state.h"

#include <lvgl/lvgl.h>
#include <spdlog/spdlog.h>

namespace helix::settings {

namespace {

constexpr uint32_t kTickMs = 1000;

struct RevertRetry {
    net::Ipv4Link link = net::Ipv4Link::Wifi;
    bool include_own = false;
    net::Ipv4RevertRetry schedule;
    ui::LvglTimerGuard timer;
    bool in_flight = false;
    std::string last_failure;
};

RevertRetry& state() {
    static RevertRetry s;
    return s;
}

bool job_holds_machine() {
    return lv_subject_get_int(get_printer_state().print_state().get_job_holds_machine_subject()) !=
           0;
}

void finish(const net::Ipv4RestoreOutcome& outcome) {
    RevertRetry& s = state();
    s.in_flight = false;
    using Kind = net::Ipv4RestoreOutcome::Kind;
    switch (outcome.kind) {
    case Kind::Restored:
        s.timer.reset();
        spdlog::info("[Ipv4] The unconfirmed IPv4 change is undone");
        NOTIFY_INFO("{}", lv_tr("Previous IPv4 settings restored"));
        return;
    case Kind::Nothing:
    case Kind::Skipped:
        s.timer.reset();
        return;
    case Kind::Terminal:
        give_up_ipv4_revert(s.link, outcome.detail);
        return;
    case Kind::Retry:
        s.last_failure = outcome.detail;
        return;
    }
}

void attempt() {
    RevertRetry& s = state();
    s.in_flight = true;
    const net::Ipv4Link link = s.link;
    const bool include_own = s.include_own;
    http::HttpExecutor::fast().start();
    http::HttpExecutor::fast().submit([link, include_own]() {
        const net::Ipv4RestoreOutcome outcome = net::restore_pending_ipv4(link, include_own);
        ui::queue_update("Ipv4RevertRetry::finish", [outcome]() { finish(outcome); });
    });
}

void tick() {
    RevertRetry& s = state();
    if (s.in_flight) {
        return;
    }
    switch (s.schedule.tick(lv_tick_get(), job_holds_machine())) {
    case net::Ipv4RevertRetry::Step::Wait:
        return;
    case net::Ipv4RevertRetry::Step::Attempt:
        attempt();
        return;
    case net::Ipv4RevertRetry::Step::GiveUp:
        give_up_ipv4_revert(s.link, s.last_failure.empty()
                                        ? std::string("no restore succeeded in two minutes")
                                        : s.last_failure);
        return;
    }
}

} // namespace

void retry_ipv4_revert(net::Ipv4Link link, bool include_own) {
    RevertRetry& s = state();
    if (s.timer) {
        s.include_own = s.include_own || include_own;
        return;
    }
    s.link = link;
    s.include_own = include_own;
    s.last_failure.clear();
    s.schedule.start(lv_tick_get());
    s.timer.reset(lv_timer_create([](lv_timer_t*) { tick(); }, kTickMs, nullptr));
    tick();
}

void give_up_ipv4_revert(net::Ipv4Link link, const std::string& why) {
    state().timer.reset();
    spdlog::error("[Ipv4] Gave up restoring the previous IPv4 config on {}: {}",
                  net::ipv4_link_name(link), why);
    http::HttpExecutor::fast().start();
    http::HttpExecutor::fast().submit([link, why]() { net::mark_pending_ipv4_failed(link, why); });
    NOTIFY_ERROR(lv_tr("The previous IPv4 settings could not be restored: {}"), why);
}

void start_ipv4_startup_revert() {
    const net::Ipv4RecordLoad loaded = net::read_pending_ipv4_revert();
    if (loaded.kind == net::Ipv4RecordLoad::Kind::Unreadable) {
        give_up_ipv4_revert(net::Ipv4Link::Wifi,
                            net::pending_ipv4_revert_path() + " cannot be read");
        return;
    }
    const auto& pending = loaded.record;
    if (loaded.kind != net::Ipv4RecordLoad::Kind::Ok || pending.failed ||
        pending.owner == net::ipv4_process_owner()) {
        return;
    }
    spdlog::warn("[Ipv4] An IPv4 change on {} was never kept: restoring {}",
                 net::ipv4_link_name(pending.link), net::ipv4_config_text(pending.previous));
    retry_ipv4_revert(pending.link, /*include_own=*/false);
}

} // namespace helix::settings
