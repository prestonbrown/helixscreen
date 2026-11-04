// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_ap_steering.h"

#include <algorithm>
#include <cstdio>

namespace helix {

void WifiApSteering::on_associated(const Bssid& bssid) {
    if (!associated_ || bssid != current_) {
        stalled_drops_ = 0;
    }
    current_ = bssid;
    associated_ = true;
}

bool WifiApSteering::on_link_drop(int64_t now_ms, int64_t silence_ms) {
    if (!associated_ || silence_ms < kLinkStallSilenceMs) {
        return false;
    }
    if (stalled_drops_ == 0 || now_ms - first_drop_ms_ > kLinkStallWindowMs) {
        stalled_drops_ = 0;
        first_drop_ms_ = now_ms;
    }
    return ++stalled_drops_ >= kLinkStallDrops;
}

void WifiApSteering::scan_started(int64_t now_ms) {
    scan_requested_ms_ = now_ms;
}

bool WifiApSteering::take_scan_request(int64_t now_ms) {
    const bool answered = scan_requested_ms_ && now_ms - *scan_requested_ms_ <= kScanRequestTtlMs;
    scan_requested_ms_.reset();
    return answered;
}

std::optional<WifiApSteering::Bssid>
WifiApSteering::pick_alternative(const std::vector<Candidate>& seen, int64_t now_ms) {
    stalled_drops_ = 0;
    avoided_.erase(std::remove_if(avoided_.begin(), avoided_.end(),
                                  [&](const Avoided& a) { return a.until_ms <= now_ms; }),
                   avoided_.end());
    avoided_.push_back({current_, now_ms + kAvoidMs});

    int floor = kMinRssi;
    for (const Candidate& c : seen) {
        if (c.bssid == current_) {
            floor = std::max(floor, c.rssi - kMaxRssiLossDb);
        }
    }

    std::optional<Candidate> best;
    for (const Candidate& c : seen) {
        const bool avoided = std::any_of(avoided_.begin(), avoided_.end(),
                                         [&](const Avoided& a) { return a.bssid == c.bssid; });
        if (avoided || c.rssi < floor) {
            continue;
        }
        if (!best || c.rssi > best->rssi) {
            best = c;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return best->bssid;
}

std::string format_bssid(const WifiApSteering::Bssid& b) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4],
                  b[5]);
    return buf;
}

} // namespace helix
