// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "connection_state.h"
#include "observer_factory.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix {

/**
 * @brief Mark a notification-fed cache stale while the Moonraker socket is down
 *
 * EVENT notifications -- notify_history_changed, notify_filelist_changed,
 * notify_job_queue_changed -- only reach a live socket. A drop is therefore a
 * window in which the underlying data can change with nothing left to announce
 * it, while the cache's loaded-latch goes on reporting it as good. STATUS
 * subscriptions do not have this problem: re-subscribing on reconnect replays a
 * full snapshot, so state fed that way self-heals and must not use this.
 *
 * Deliberately NOT hung off IMoonrakerClient::add_connected_observer. That
 * fan-out also fires on every Klippy-ready transition (moonraker_client.cpp),
 * so a refresh there costs a full refetch on every FIRMWARE_RESTART, for data
 * nothing touched. Only a real socket drop can lose a notification.
 *
 * This only MARKS. Refetching is left to the consumers that already lazy-load
 * on the CONNECTED transition, so nothing is pulled for a screen no one is
 * looking at. The @p Cache's invalidate() must leave cached data in place, so a
 * mid-outage reader still renders the last known list instead of blanking.
 *
 * @tparam Cache Must expose `bool has_cached_data() const` and `void invalidate()`.
 * @param state PrinterState whose connection subject to watch.
 * @param cache Cache to mark stale; must outlive the returned guard.
 * @param tag   Log tag, e.g. "HistoryManager".
 * @return Guard that must be stored -- dropping it ends the watch.
 */
template <typename Cache>
[[nodiscard]] ObserverGuard observe_connection_staleness(PrinterState& state, Cache* cache,
                                                         const char* tag) {
    return helix::ui::observe_int_sync<Cache>(
        state.get_printer_connection_state_subject(), cache,
        [tag](Cache* self, int conn_state) {
            // No previous-state tracking: marking stale is idempotent, so
            // re-firing on CONNECTING or RECONNECTING costs nothing and is
            // still true. The has_cached_data() half only keeps the log quiet.
            if (conn_state == static_cast<int>(ConnectionState::CONNECTED) ||
                !self->has_cached_data()) {
                return;
            }
            spdlog::debug("[{}] Connection down, marking cache stale", tag);
            self->invalidate();
        },
        state.get_subjects_lifetime());
}

} // namespace helix
