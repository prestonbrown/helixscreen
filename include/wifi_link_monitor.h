// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file wifi_link_monitor.h
 * @brief Always-on WiFi link telemetry: periodic sample, change log, bundle section.
 *
 * Samples carry frequency/channel, RSSI, link quality and retry counters only.
 * No SSID, BSSID or MAC is ever stored, logged or exported: a set of those
 * resolves to a street address through public WiFi-positioning databases.
 *
 * The pure pieces (proc parser, channel math, change classification, ring
 * buffer) are free of LVGL so the rules are testable without a display.
 */

#pragma once

#include "ui_timer_guard.h"

#include "async_lifetime_guard.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>

#include "hv/json.hpp"

namespace helix {

/// Radio counters from /proc/net/wireless. Absent fields are unreported.
struct WirelessProcStats {
    std::optional<int> link_quality;    ///< driver-scaled quality (commonly 0-70)
    std::optional<int> rssi_dbm;        ///< signal level in dBm
    std::optional<long> tx_retries;     ///< "retry" discard column: excessive Tx retries
    std::optional<long> missed_beacons; ///< "beacon" column
};

struct LinkSample {
    int64_t uptime_s = 0;
    bool connected = false;
    int frequency_mhz = 0; ///< 0 = unknown
    WirelessProcStats radio;
};

enum class LinkEvent { None, Disconnected, Reconnected, FrequencyChanged, RssiDrop };

/// The row for @p iface in /proc/net/wireless; with an empty @p iface, the first row.
/// nullopt when there is no such row: another interface's numbers are never substituted.
std::optional<WirelessProcStats> parse_proc_net_wireless(std::string_view text,
                                                         std::string_view iface = {});

/// 802.11 channel number for a center frequency, 0 when not a WiFi channel.
int wifi_channel_from_frequency(int frequency_mhz);

/// An RSSI fall of at least this many dB between two samples is worth a line.
inline constexpr int RSSI_DROP_DB = 15;
/// Real events log at INFO at most this often; the rest drop to DEBUG.
inline constexpr int64_t LINK_INFO_MIN_INTERVAL_S = 60;

/// What changed from @p prev to @p cur (None for the first sample).
LinkEvent classify_link_change(const LinkSample* prev, const LinkSample& cur);

/// True when @p ev at @p now_s may be logged at INFO given the last INFO time.
/// A restore always may: a logged drop must never lack its recovery.
bool link_info_allowed(LinkEvent ev, int64_t now_s, std::optional<int64_t> last_info_s);

/// Bounded FIFO of samples; the oldest is dropped past capacity.
class LinkHistory {
  public:
    explicit LinkHistory(size_t capacity) : capacity_(capacity) {}
    void push(const LinkSample& s);
    size_t size() const {
        return samples_.size();
    }
    const std::deque<LinkSample>& samples() const {
        return samples_;
    }

  private:
    size_t capacity_;
    std::deque<LinkSample> samples_;
};

/// Debug-bundle `network` section for a history (empty history: no samples).
/// `frequency_available` says whether the backend can report frequency at all, so an
/// absent band reads as "unknown" rather than "unchanged".
nlohmann::json link_history_to_json(const LinkHistory& history, bool frequency_available);

/**
 * @brief Polls the WiFi manager every 30s regardless of which UI is on screen.
 *
 * Main thread only for start()/stop(). The status read itself runs through
 * WiFiManager::get_status_async(), so the UI thread never waits on the radio.
 */
class WifiLinkMonitor {
  public:
    static WifiLinkMonitor& instance();

    void start();
    void stop();

    /// Thread-safe: callable from the debug bundle worker.
    nlohmann::json to_json() const;

    /// Record one sample, logging any change. Exposed for tests.
    void record(const LinkSample& sample);

  private:
    WifiLinkMonitor() : history_(HISTORY_CAPACITY) {}
    static void timer_cb(lv_timer_t* timer);
    void poll();

    static constexpr uint32_t POLL_INTERVAL_MS = 30000;
    static constexpr size_t HISTORY_CAPACITY = 40; ///< 20 minutes at the poll interval

    AsyncLifetimeGuard lifetime_;
    ui::LvglTimerGuard timer_;
    mutable std::mutex mutex_;
    LinkHistory history_;
    std::optional<int64_t> last_info_s_;
    bool frequency_available_ = false;
};

} // namespace helix
