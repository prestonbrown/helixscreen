// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ipv4_revert_retry.h"

#include "ui_error_reporting.h"

#include "app_globals.h"
#include "connection_state.h"
#include "core/http_executor.h"
#include "core/timer_guard.h"
#include "core/update_queue.h"
#include "ipv4_config_system.h"
#include "printer_state.h"

#include <lvgl/lvgl.h>
#include <spdlog/spdlog.h>

#include <vector>

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

/// The startup read of a record that failed to read: which link it is for is
/// unknown until it reads, so the read itself is what retries.
struct StartupRead {
    net::Ipv4RevertRetry schedule;
    ui::LvglTimerGuard timer;
};

StartupRead& startup_read() {
    static StartupRead s;
    return s;
}

/// A give-up whose record could not be marked failed, by link. A new change
/// stays refused while it holds, so the screen says why instead of offering it.
std::string& unrecorded_why(net::Ipv4Link link) {
    static std::string wifi;
    static std::string ethernet;
    return link == net::Ipv4Link::Ethernet ? ethernet : wifi;
}

net::Ipv4StartupHold& startup_hold() {
    static net::Ipv4StartupHold h;
    return h;
}

int print_state_reports() {
    return lv_subject_get_int(
        get_printer_state().print_state().get_print_state_report_seq_subject());
}

/// Nothing is tried while a job holds the machine, nor at startup before the
/// printer has said whether one does.
bool revert_held() {
    if (startup_hold().waiting(print_state_reports(), lv_tick_get())) {
        return true;
    }
    return ipv4_job_holds_machine();
}

std::function<void(net::Ipv4Link, Ipv4RevertEnd)>& listener() {
    static std::function<void(net::Ipv4Link, Ipv4RevertEnd)> l;
    return l;
}

void announce(net::Ipv4Link link, Ipv4RevertEnd end) {
    if (auto& l = listener()) {
        l(link, end);
    }
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
        unrecorded_why(s.link).clear();
        announce(s.link, Ipv4RevertEnd::Restored);
        return;
    case Kind::Nothing:
    case Kind::Skipped:
        s.timer.reset();
        return;
    case Kind::Terminal:
        give_up_ipv4_revert(s.link, outcome.detail, net::Ipv4GiveUp::Terminal);
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
    switch (s.schedule.tick(lv_tick_get(), revert_held())) {
    case net::Ipv4RevertRetry::Step::Wait:
        return;
    case net::Ipv4RevertRetry::Step::Attempt:
        attempt();
        return;
    case net::Ipv4RevertRetry::Step::GiveUp:
        give_up_ipv4_revert(s.link,
                            s.last_failure.empty()
                                ? std::string("no restore succeeded in two minutes")
                                : s.last_failure,
                            net::Ipv4GiveUp::UntilNextStart);
        return;
    }
}

} // namespace

bool ipv4_job_holds_machine() {
    static net::Ipv4JobHold hold;
    PrinterState& printer = get_printer_state();
    const uint32_t now = lv_tick_get();
    hold.sample(
        lv_subject_get_int(printer.network_state().get_printer_connection_state_subject()) ==
            static_cast<int>(ConnectionState::CONNECTED),
        now);
    return hold.holds(
        lv_subject_get_int(printer.print_state().get_job_holds_machine_subject()) != 0, now);
}

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

void give_up_ipv4_revert(std::optional<net::Ipv4Link> link, const std::string& why,
                         net::Ipv4GiveUp how) {
    state().timer.reset();
    spdlog::error("[Ipv4] Gave up restoring the previous IPv4 config on {}{}: {}",
                  link ? net::ipv4_link_name(*link) : "every link",
                  how == net::Ipv4GiveUp::UntilNextStart ? " until the next start" : "", why);
    const std::vector<net::Ipv4Link> told =
        link ? std::vector<net::Ipv4Link>{*link}
             : std::vector<net::Ipv4Link>{net::Ipv4Link::Wifi, net::Ipv4Link::Ethernet};
    http::HttpExecutor::fast().start();
    http::HttpExecutor::fast().submit([link, told, why, how]() {
        const net::Ipv4Mark mark = net::mark_pending_ipv4_given_up(link, why, how);
        ui::queue_update("Ipv4RevertRetry::gave_up", [told, why, mark]() {
            const bool marked = mark != net::Ipv4Mark::Unmarked;
            if (mark == net::Ipv4Mark::Marked) {
                NOTIFY_ERROR(lv_tr("The previous IPv4 settings could not be restored: {}"), why);
            } else if (!marked) {
                spdlog::error("[Ipv4] {} could not be marked failed",
                              net::pending_ipv4_revert_path());
                NOTIFY_ERROR(lv_tr("The previous IPv4 settings could not be restored, and the "
                                   "record of the change could not be updated: {}"),
                             why);
            }
            for (const net::Ipv4Link l : told) {
                unrecorded_why(l) = marked ? std::string() : why;
                announce(l, marked ? Ipv4RevertEnd::GaveUp : Ipv4RevertEnd::GaveUpUnrecorded);
            }
        });
    });
}

std::optional<std::string> ipv4_revert_unrecorded(net::Ipv4Link link) {
    const std::string& why = unrecorded_why(link);
    if (why.empty()) {
        return std::nullopt;
    }
    return why;
}

void set_ipv4_revert_listener(std::function<void(net::Ipv4Link, Ipv4RevertEnd)> l) {
    listener() = std::move(l);
}

namespace {

/// Acts on the record another run left. False while it cannot be read.
bool act_on_startup_record(const net::Ipv4RecordLoad& loaded) {
    using RecordKind = net::Ipv4RecordLoad::Kind;
    if (loaded.kind == RecordKind::ReadError) {
        return false;
    }
    if (loaded.kind == RecordKind::Unreadable) {
        give_up_ipv4_revert(loaded.unreadable_link, net::pending_ipv4_undecodable_detail(),
                            net::Ipv4GiveUp::Terminal);
        return true;
    }
    const auto& pending = loaded.record;
    if (loaded.kind != RecordKind::Ok || pending.failed ||
        pending.owner == net::ipv4_process_owner()) {
        return true;
    }
    spdlog::warn("[Ipv4] An IPv4 change on {} was never kept: restoring {} once the printer "
                 "reports whether a job runs",
                 net::ipv4_link_name(pending.link), net::ipv4_config_text(pending.previous));
    retry_ipv4_revert(pending.link, /*include_own=*/false);
    return true;
}

void startup_read_tick() {
    StartupRead& s = startup_read();
    switch (s.schedule.tick(lv_tick_get(), ipv4_job_holds_machine())) {
    case net::Ipv4RevertRetry::Step::Wait:
        return;
    case net::Ipv4RevertRetry::Step::Attempt:
        if (act_on_startup_record(net::read_pending_ipv4_revert())) {
            s.timer.reset();
        }
        return;
    case net::Ipv4RevertRetry::Step::GiveUp:
        // The bytes may be good, so they stay for the next start.
        s.timer.reset();
        spdlog::error("[Ipv4] {} could not be read in two minutes",
                      net::pending_ipv4_revert_path());
        NOTIFY_ERROR(lv_tr("The previous IPv4 settings could not be checked: {} could not be "
                           "read"),
                     net::pending_ipv4_revert_path());
        return;
    }
}

} // namespace

void start_ipv4_startup_revert() {
    startup_hold().arm(print_state_reports(), lv_tick_get());
    if (act_on_startup_record(net::read_pending_ipv4_revert())) {
        return;
    }
    spdlog::warn("[Ipv4] {} could not be read: trying again", net::pending_ipv4_revert_path());
    StartupRead& s = startup_read();
    s.schedule.start(lv_tick_get());
    s.timer.reset(lv_timer_create([](lv_timer_t*) { startup_read_tick(); }, kTickMs, nullptr));
}

lv_timer_t* ipv4_revert_retry_timer_for_test() {
    return state().timer.get();
}

lv_timer_t* ipv4_startup_read_timer_for_test() {
    return startup_read().timer.get();
}

} // namespace helix::settings
