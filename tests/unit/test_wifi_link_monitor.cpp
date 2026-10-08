// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// WiFi link telemetry: /proc parsing, change classification, rate limit, the
// bounded history and the identifier-free `network` bundle section.

#include "wifi_link_monitor.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr const char* PROC_SAMPLE =
    "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
    " face | tstat| link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n"
    " wlan0: 0000   58.  -52.  -256        0      0      0     17      3        4\n";

LinkSample sample(int64_t t, bool connected, int freq, std::optional<int> rssi) {
    LinkSample s;
    s.uptime_s = t;
    s.connected = connected;
    s.frequency_mhz = freq;
    s.radio.rssi_dbm = rssi;
    return s;
}

} // namespace

TEST_CASE("/proc/net/wireless parses quality, level and counters", "[wifi][link]") {
    auto st = parse_proc_net_wireless(PROC_SAMPLE);
    REQUIRE(st);
    CHECK(*st->link_quality == 58);
    CHECK(*st->rssi_dbm == -52);
    CHECK(*st->tx_retries == 17);
    CHECK(*st->missed_beacons == 4);
}

TEST_CASE("/proc/net/wireless edge cases", "[wifi][link]") {
    CHECK_FALSE(parse_proc_net_wireless(""));
    // Header rows only: no interface present.
    CHECK_FALSE(parse_proc_net_wireless(
        "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
        " face | tstat| link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n"));
    // Unsigned-byte level (no '.') is offset by 256.
    auto st = parse_proc_net_wireless(" wlan0: 0000 50 200 0 0 0 0 0 0 0\n");
    REQUIRE(st);
    CHECK(*st->rssi_dbm == -56);
    // A raw small level is not a dBm reading: omitted, not invented.
    st = parse_proc_net_wireless(" wlan0: 0000 50 30 0 0 0 0 0 0 0\n");
    REQUIRE(st);
    CHECK_FALSE(st->rssi_dbm);
}

TEST_CASE("channel from frequency", "[wifi][link]") {
    CHECK(wifi_channel_from_frequency(2412) == 1);
    CHECK(wifi_channel_from_frequency(2484) == 14);
    CHECK(wifi_channel_from_frequency(5180) == 36);
    CHECK(wifi_channel_from_frequency(5955) == 1);
    CHECK(wifi_channel_from_frequency(0) == 0);
}

TEST_CASE("link change classification", "[wifi][link]") {
    auto up24 = sample(0, true, 2437, -50);
    CHECK(classify_link_change(nullptr, up24) == LinkEvent::None);
    CHECK(classify_link_change(&up24, sample(30, true, 2437, -55)) == LinkEvent::None);
    CHECK(classify_link_change(&up24, sample(30, true, 5180, -50)) == LinkEvent::FrequencyChanged);
    CHECK(classify_link_change(&up24, sample(30, true, 2437, -65)) == LinkEvent::RssiDrop);
    CHECK(classify_link_change(&up24, sample(30, true, 2437, -64)) == LinkEvent::None);
    CHECK(classify_link_change(&up24, sample(30, false, 0, std::nullopt)) ==
          LinkEvent::Disconnected);
    auto down = sample(0, false, 0, std::nullopt);
    CHECK(classify_link_change(&down, up24) == LinkEvent::Reconnected);
    CHECK(classify_link_change(&down, sample(30, false, 0, std::nullopt)) == LinkEvent::None);
    // Unknown frequency on either side is not a change.
    CHECK(classify_link_change(&up24, sample(30, true, 0, -50)) == LinkEvent::None);
}

TEST_CASE("INFO rate limit", "[wifi][link]") {
    CHECK(link_info_allowed(LinkEvent::Disconnected, 100, std::nullopt));
    CHECK_FALSE(
        link_info_allowed(LinkEvent::Disconnected, 100 + LINK_INFO_MIN_INTERVAL_S - 1, 100));
    CHECK(link_info_allowed(LinkEvent::Disconnected, 100 + LINK_INFO_MIN_INTERVAL_S, 100));
    // A restore is exempt so a logged drop always has its recovery.
    CHECK(link_info_allowed(LinkEvent::Reconnected, 101, 100));
}

TEST_CASE("link history is bounded and drops the oldest", "[wifi][link]") {
    LinkHistory h(3);
    for (int i = 0; i < 10; ++i) {
        h.push(sample(i, true, 2437, -50));
    }
    REQUIRE(h.size() == 3);
    CHECK(h.samples().front().uptime_s == 7);
    CHECK(h.samples().back().uptime_s == 9);
}

TEST_CASE("network bundle section carries link fields and no identifiers",
          "[wifi][link][security]") {
    LinkHistory h(5);
    LinkSample s = sample(5, true, 5180, -48);
    s.radio.link_quality = 60;
    s.radio.tx_retries = 9;
    h.push(s);
    h.push(sample(35, false, 0, std::nullopt));

    auto j = link_history_to_json(h, true);
    auto cur = j["current"];
    CHECK(cur["connected"] == false);
    CHECK_FALSE(cur.contains("frequency_mhz")); // omitted, not zero-filled

    auto first = j["history"][0];
    CHECK(first["frequency_mhz"] == 5180);
    CHECK(first["band"] == "5GHz");
    CHECK(first["channel"] == 36);
    CHECK(first["rssi_dbm"] == -48);
    CHECK(first["link_quality"] == 60);
    CHECK(first["tx_retries"] == 9);

    const std::string dump = j.dump();
    for (const char* banned : {"ssid", "bssid", "mac", "address"}) {
        CHECK(dump.find(banned) == std::string::npos);
    }
}

TEST_CASE("/proc/net/wireless picks the managed interface row", "[wifi][link]") {
    const char* two = " p2p0: 0000 10. -80. -256 0 0 0 99 0 9\n"
                      " wlan0: 0000 58. -52. -256 0 0 0 17 3 4\n";
    auto st = parse_proc_net_wireless(two, "wlan0");
    REQUIRE(st);
    CHECK(*st->rssi_dbm == -52);
    CHECK(*st->tx_retries == 17);
    // Named interface absent: no substitute from another row.
    CHECK_FALSE(parse_proc_net_wireless(two, "wlan1"));
    // No name known: first row.
    CHECK(*parse_proc_net_wireless(two)->tx_retries == 99);
}

TEST_CASE("network section states when frequency is unavailable", "[wifi][link]") {
    LinkHistory h(3);
    h.push(sample(1, true, 0, -50));
    CHECK(link_history_to_json(h, false)["frequency_available"] == false);
    CHECK(link_history_to_json(h, true)["frequency_available"] == true);
}
