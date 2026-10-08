// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_link_monitor.h"

#include "text_io.h"
#include "wifi_backend.h"
#include "wifi_manager.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <fstream>
#include <sstream>

namespace helix {

namespace {

int64_t uptime_seconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(steady_clock::now().time_since_epoch()).count();
}

/// "70." / "-40." / "0" -> integer; the trailing '.' marks a driver-updated value.
std::optional<int> parse_proc_int(std::string_view tok, bool* had_dot = nullptr) {
    const bool dot = !tok.empty() && tok.back() == '.';
    if (dot) {
        tok.remove_suffix(1);
    }
    if (had_dot) {
        *had_dot = dot;
    }
    return text_io::parse_int<int>(tok);
}

const char* band_name(int frequency_mhz) {
    switch (wifi_band_flag_from_frequency(frequency_mhz)) {
    case WIFI_BAND_2_4GHZ:
        return "2.4GHz";
    case WIFI_BAND_5GHZ:
        return "5GHz";
    case WIFI_BAND_6GHZ:
        return "6GHz";
    default:
        return nullptr;
    }
}

nlohmann::json sample_to_json(const LinkSample& s) {
    nlohmann::json j = {{"uptime_s", s.uptime_s}, {"connected", s.connected}};
    // Unreported fields are omitted rather than defaulted: 0 is a real value for each.
    if (s.frequency_mhz > 0) {
        j["frequency_mhz"] = s.frequency_mhz;
        if (const char* band = band_name(s.frequency_mhz)) {
            j["band"] = band;
        }
        if (int ch = wifi_channel_from_frequency(s.frequency_mhz)) {
            j["channel"] = ch;
        }
    }
    if (s.radio.rssi_dbm) {
        j["rssi_dbm"] = *s.radio.rssi_dbm;
    }
    if (s.radio.link_quality) {
        j["link_quality"] = *s.radio.link_quality;
    }
    if (s.radio.tx_retries) {
        j["tx_retries"] = *s.radio.tx_retries;
    }
    if (s.radio.missed_beacons) {
        j["missed_beacons"] = *s.radio.missed_beacons;
    }
    return j;
}

} // namespace

std::optional<WirelessProcStats> parse_proc_net_wireless(std::string_view text,
                                                         std::string_view iface) {
    for (std::string_view line : text_io::lines(text)) {
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue; // the two header rows carry no interface name
        }
        if (!iface.empty() && text_io::trim(line.substr(0, colon)) != iface) {
            continue;
        }
        // Columns after "iface:": status link level noise nwid crypt frag retry misc beacon
        const auto cols = text_io::split_ws(line.substr(colon + 1));
        if (cols.size() < 10) {
            continue;
        }
        WirelessProcStats st;
        st.link_quality = parse_proc_int(cols[1]);
        bool dot = false;
        if (auto level = parse_proc_int(cols[2], &dot)) {
            // Without the '.' marker the level is an unsigned byte offset by 256.
            int dbm = (!dot && *level > 0) ? *level - 256 : *level;
            if (dbm < 0 && dbm >= -120) {
                st.rssi_dbm = dbm;
            }
        }
        st.tx_retries = text_io::parse_int<long>(cols[7]);
        st.missed_beacons = text_io::parse_int<long>(cols[9]);
        return st;
    }
    return std::nullopt;
}

int wifi_channel_from_frequency(int f) {
    if (f == 2484) {
        return 14;
    }
    if (f >= 2412 && f <= 2472) {
        return (f - 2407) / 5;
    }
    if (f >= 5160 && f <= 5885) {
        return (f - 5000) / 5;
    }
    if (f >= 5955 && f <= 7115) {
        return (f - 5950) / 5;
    }
    return 0;
}

LinkEvent classify_link_change(const LinkSample* prev, const LinkSample& cur) {
    if (!prev) {
        return LinkEvent::None;
    }
    if (prev->connected && !cur.connected) {
        return LinkEvent::Disconnected;
    }
    if (!prev->connected && cur.connected) {
        return LinkEvent::Reconnected;
    }
    if (!cur.connected) {
        return LinkEvent::None;
    }
    if (prev->frequency_mhz > 0 && cur.frequency_mhz > 0 &&
        prev->frequency_mhz != cur.frequency_mhz) {
        return LinkEvent::FrequencyChanged;
    }
    if (prev->radio.rssi_dbm && cur.radio.rssi_dbm &&
        *prev->radio.rssi_dbm - *cur.radio.rssi_dbm >= RSSI_DROP_DB) {
        return LinkEvent::RssiDrop;
    }
    return LinkEvent::None;
}

bool link_info_allowed(LinkEvent ev, int64_t now_s, std::optional<int64_t> last_info_s) {
    return ev == LinkEvent::Reconnected || !last_info_s ||
           now_s - *last_info_s >= LINK_INFO_MIN_INTERVAL_S;
}

void LinkHistory::push(const LinkSample& s) {
    samples_.push_back(s);
    while (samples_.size() > capacity_) {
        samples_.pop_front();
    }
}

nlohmann::json link_history_to_json(const LinkHistory& history, bool frequency_available) {
    nlohmann::json out = {{"frequency_available", frequency_available},
                          {"history", nlohmann::json::array()}};
    for (const auto& s : history.samples()) {
        out["history"].push_back(sample_to_json(s));
    }
    if (!history.samples().empty()) {
        out["current"] = sample_to_json(history.samples().back());
    }
    return out;
}

WifiLinkMonitor& WifiLinkMonitor::instance() {
    static WifiLinkMonitor inst;
    return inst;
}

void WifiLinkMonitor::start() {
    if (timer_) {
        return;
    }
    timer_.reset(lv_timer_create(&WifiLinkMonitor::timer_cb, POLL_INTERVAL_MS, this));
    poll();
}

void WifiLinkMonitor::stop() {
    lifetime_.invalidate();
    timer_.reset();
}

void WifiLinkMonitor::timer_cb(lv_timer_t* timer) {
    static_cast<WifiLinkMonitor*>(lv_timer_get_user_data(timer))->poll();
}

void WifiLinkMonitor::poll() {
    auto wifi = get_wifi_manager();
    if (!wifi) {
        return;
    }
    const std::string netdev = wifi->netdev_name();
    const bool freq_ok = wifi->reports_frequency();
    wifi->get_status_async(lifetime_.token(),
                           [this, netdev, freq_ok](const WifiBackend::ConnectionStatus& st) {
                               LinkSample s;
                               s.uptime_s = uptime_seconds();
                               s.connected = st.connected;
                               if (st.connected) {
                                   s.frequency_mhz = st.frequency_mhz;
                                   // /proc is a kernel table read, not a device round trip; absent
                                   // off Linux.
                                   std::ifstream f("/proc/net/wireless");
                                   std::stringstream buf;
                                   buf << f.rdbuf();
                                   if (auto radio = parse_proc_net_wireless(buf.str(), netdev)) {
                                       s.radio = *radio;
                                   }
                               }
                               {
                                   std::lock_guard<std::mutex> lock(mutex_);
                                   frequency_available_ = freq_ok;
                               }
                               record(s);
                           });
}

void WifiLinkMonitor::record(const LinkSample& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    const LinkSample* prev = history_.samples().empty() ? nullptr : &history_.samples().back();
    const LinkEvent ev = classify_link_change(prev, s);

    if (ev != LinkEvent::None) {
        const bool info = link_info_allowed(ev, s.uptime_s, last_info_s_);
        const auto lvl = info ? spdlog::level::info : spdlog::level::debug;
        if (info) {
            last_info_s_ = s.uptime_s;
        }
        switch (ev) {
        case LinkEvent::Disconnected:
            spdlog::log(lvl, "[WifiLink] Link lost");
            break;
        case LinkEvent::Reconnected:
            spdlog::log(lvl, "[WifiLink] Link restored: {} MHz ch{} rssi {} dBm", s.frequency_mhz,
                        wifi_channel_from_frequency(s.frequency_mhz), s.radio.rssi_dbm.value_or(0));
            break;
        case LinkEvent::FrequencyChanged:
            spdlog::log(lvl, "[WifiLink] Frequency {} -> {} MHz (ch{})", prev->frequency_mhz,
                        s.frequency_mhz, wifi_channel_from_frequency(s.frequency_mhz));
            break;
        case LinkEvent::RssiDrop:
            spdlog::log(lvl, "[WifiLink] RSSI dropped {} -> {} dBm", *prev->radio.rssi_dbm,
                        *s.radio.rssi_dbm);
            break;
        case LinkEvent::None:
            break;
        }
    } else {
        spdlog::debug("[WifiLink] connected={} {} MHz rssi {} dBm quality {} retries {}",
                      s.connected, s.frequency_mhz, s.radio.rssi_dbm.value_or(0),
                      s.radio.link_quality.value_or(-1), s.radio.tx_retries.value_or(-1));
    }
    history_.push(s);
}

nlohmann::json WifiLinkMonitor::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return link_history_to_json(history_, frequency_available_);
}

} // namespace helix
