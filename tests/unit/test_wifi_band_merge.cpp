// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wifi_band_merge.cpp
 * @brief Band preservation across scan-result deduplication (helixscreen#1189)
 *
 * Keying deduplicate_by_ssid() on SSID alone and keeping the strongest signal
 * would lose a dual-band router's 5GHz BSS entirely: at any normal distance
 * 2.4GHz wins on RSSI, the 5GHz struct is discarded, and nothing records that
 * the band was ever seen. These tests pin the behaviour: one row per SSID,
 * carrying the union of every band it was observed on.
 */

#include "../../include/wifi_backend.h"
#include "../../include/wifi_backend_mock.h"
#include "../../include/wifi_ui_utils.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::ui::wifi::wifi_format_band_label;
using helix::ui::wifi::wifi_scan_spans_multiple_bands;

namespace {

const WiFiNetwork* find_ssid(const std::vector<WiFiNetwork>& nets, const std::string& ssid) {
    auto it = std::find_if(nets.begin(), nets.end(),
                           [&ssid](const WiFiNetwork& n) { return n.ssid == ssid; });
    return it == nets.end() ? nullptr : &*it;
}

} // namespace

// =============================================================================
// Frequency -> band classification
// =============================================================================

TEST_CASE("wifi_band_flag_from_frequency classifies the WiFi bands", "[wifi][band][1189]") {
    SECTION("2.4GHz channels") {
        CHECK(wifi_band_flag_from_frequency(2412) == WIFI_BAND_2_4GHZ); // ch 1
        CHECK(wifi_band_flag_from_frequency(2437) == WIFI_BAND_2_4GHZ); // ch 6
        CHECK(wifi_band_flag_from_frequency(2484) == WIFI_BAND_2_4GHZ); // ch 14 (JP)
    }

    SECTION("5GHz channels") {
        CHECK(wifi_band_flag_from_frequency(5180) == WIFI_BAND_5GHZ); // ch 36, UNII-1
        CHECK(wifi_band_flag_from_frequency(5500) == WIFI_BAND_5GHZ); // ch 100, DFS
        CHECK(wifi_band_flag_from_frequency(5825) == WIFI_BAND_5GHZ); // ch 165, UNII-3
        CHECK(wifi_band_flag_from_frequency(4980) == WIFI_BAND_5GHZ); // 4.9GHz allocation
    }

    SECTION("6GHz channels") {
        CHECK(wifi_band_flag_from_frequency(5955) == WIFI_BAND_6GHZ); // ch 1, 6E
        CHECK(wifi_band_flag_from_frequency(7115) == WIFI_BAND_6GHZ); // ch 233, 6E
    }

    SECTION("Unknown or non-WiFi frequencies") {
        CHECK(wifi_band_flag_from_frequency(0) == WIFI_BAND_NONE);
        CHECK(wifi_band_flag_from_frequency(-1) == WIFI_BAND_NONE);
        CHECK(wifi_band_flag_from_frequency(900) == WIFI_BAND_NONE);
        CHECK(wifi_band_flag_from_frequency(3600) == WIFI_BAND_NONE);
        CHECK(wifi_band_flag_from_frequency(9000) == WIFI_BAND_NONE);
    }
}

TEST_CASE("WiFiNetwork derives band_mask from its frequency", "[wifi][band][1189]") {
    CHECK(WiFiNetwork("Net", 60, true, "WPA2", 5180).band_mask == WIFI_BAND_5GHZ);
    CHECK(WiFiNetwork("Net", 60, true, "WPA2", 2412).band_mask == WIFI_BAND_2_4GHZ);
    CHECK(WiFiNetwork("Net", 60, true, "WPA2").band_mask == WIFI_BAND_NONE);
}

// =============================================================================
// The regression itself: merging must not erase a band
// =============================================================================

TEST_CASE("Dual-band SSID keeps both bands after merge", "[wifi][band][1189]") {
    // The reporter's topology: one SSID, 2.4GHz stronger than its 5GHz twin.
    std::vector<WiFiNetwork> scan = {
        WiFiNetwork("HomeNet", 82, true, "WPA2", 2437), // 2.4GHz wins on RSSI
        WiFiNetwork("HomeNet", 54, true, "WPA2", 5745), // 5GHz twin, weaker
    };

    auto merged = wifi_merge_networks_by_ssid(scan);

    REQUIRE(merged.size() == 1);
    CHECK(merged[0].ssid == "HomeNet");
    CHECK(merged[0].signal_strength == 82); // strongest survives
    // The whole point: the discarded BSS still contributes its band.
    CHECK((merged[0].band_mask & WIFI_BAND_5GHZ) != 0);
    CHECK((merged[0].band_mask & WIFI_BAND_2_4GHZ) != 0);
    CHECK(wifi_format_band_label(merged[0].band_mask) == "2.4/5G");
}

TEST_CASE("Merge keeps the strongest BSS regardless of arrival order", "[wifi][band][1189]") {
    SECTION("Stronger BSS arrives second") {
        std::vector<WiFiNetwork> scan = {
            WiFiNetwork("Mesh", 30, true, "WPA2", 5180),
            WiFiNetwork("Mesh", 90, true, "WPA2", 2412),
        };
        auto merged = wifi_merge_networks_by_ssid(scan);
        REQUIRE(merged.size() == 1);
        CHECK(merged[0].signal_strength == 90);
        CHECK(merged[0].frequency_mhz == 2412);
        CHECK(merged[0].band_mask == (WIFI_BAND_2_4GHZ | WIFI_BAND_5GHZ));
    }

    SECTION("Stronger BSS arrives first") {
        std::vector<WiFiNetwork> scan = {
            WiFiNetwork("Mesh", 90, true, "WPA2", 2412),
            WiFiNetwork("Mesh", 30, true, "WPA2", 5180),
        };
        auto merged = wifi_merge_networks_by_ssid(scan);
        REQUIRE(merged.size() == 1);
        CHECK(merged[0].signal_strength == 90);
        CHECK(merged[0].frequency_mhz == 2412);
        CHECK(merged[0].band_mask == (WIFI_BAND_2_4GHZ | WIFI_BAND_5GHZ));
    }
}

TEST_CASE("Merge collapses three same-band BSSes without inventing bands", "[wifi][band][1189]") {
    std::vector<WiFiNetwork> scan = {
        WiFiNetwork("MeshNet", 40, true, "WPA2", 2412),
        WiFiNetwork("MeshNet", 85, true, "WPA2", 2437),
        WiFiNetwork("MeshNet", 60, true, "WPA2", 2462),
    };

    auto merged = wifi_merge_networks_by_ssid(scan);

    REQUIRE(merged.size() == 1);
    CHECK(merged[0].signal_strength == 85);
    CHECK(merged[0].band_mask == WIFI_BAND_2_4GHZ);
    CHECK(wifi_format_band_label(merged[0].band_mask) == "2.4G");
}

TEST_CASE("Merge preserves distinct SSIDs in first-seen order", "[wifi][band][1189]") {
    std::vector<WiFiNetwork> scan = {
        WiFiNetwork("Alpha", 70, true, "WPA2", 2412),
        WiFiNetwork("Beta", 50, false, "Open", 5180),
        WiFiNetwork("Alpha", 90, true, "WPA2", 5200),
        WiFiNetwork("Gamma", 20, true, "WPA3", 2462),
    };

    auto merged = wifi_merge_networks_by_ssid(scan);

    REQUIRE(merged.size() == 3);
    CHECK(merged[0].ssid == "Alpha");
    CHECK(merged[1].ssid == "Beta");
    CHECK(merged[2].ssid == "Gamma");
    CHECK(merged[0].band_mask == (WIFI_BAND_2_4GHZ | WIFI_BAND_5GHZ));
    CHECK(merged[1].band_mask == WIFI_BAND_5GHZ);
    CHECK(merged[2].band_mask == WIFI_BAND_2_4GHZ);
}

TEST_CASE("Merge tolerates unknown frequencies", "[wifi][band][1189]") {
    std::vector<WiFiNetwork> scan = {
        WiFiNetwork("NoFreq", 70, true, "WPA2"),       // backend reported no freq
        WiFiNetwork("NoFreq", 40, true, "WPA2", 5180), // twin with a known band
    };

    auto merged = wifi_merge_networks_by_ssid(scan);

    REQUIRE(merged.size() == 1);
    CHECK(merged[0].signal_strength == 70);
    // A band that IS known must not be lost just because the winner had none.
    CHECK(merged[0].band_mask == WIFI_BAND_5GHZ);
}

TEST_CASE("Merge of an empty scan yields an empty list", "[wifi][band][1189]") {
    CHECK(wifi_merge_networks_by_ssid({}).empty());
}

// =============================================================================
// UI formatting rules
// =============================================================================

TEST_CASE("wifi_format_band_label renders a compact badge", "[wifi][band][1189]") {
    CHECK(wifi_format_band_label(WIFI_BAND_NONE).empty());
    CHECK(wifi_format_band_label(WIFI_BAND_2_4GHZ) == "2.4G");
    CHECK(wifi_format_band_label(WIFI_BAND_5GHZ) == "5G");
    CHECK(wifi_format_band_label(WIFI_BAND_6GHZ) == "6G");
    CHECK(wifi_format_band_label(WIFI_BAND_2_4GHZ | WIFI_BAND_5GHZ) == "2.4/5G");
    CHECK(wifi_format_band_label(WIFI_BAND_5GHZ | WIFI_BAND_6GHZ) == "5/6G");
    CHECK(wifi_format_band_label(WIFI_BAND_2_4GHZ | WIFI_BAND_5GHZ | WIFI_BAND_6GHZ) == "2.4/5/6G");
}

TEST_CASE("Band badges are only offered when the scan spans bands", "[wifi][band][1189]") {
    SECTION("2.4GHz-only radio: every row would say the same thing") {
        std::vector<WiFiNetwork> scan = {
            WiFiNetwork("A", 80, true, "WPA2", 2412),
            WiFiNetwork("B", 60, true, "WPA2", 2437),
        };
        CHECK_FALSE(wifi_scan_spans_multiple_bands(scan));
    }

    SECTION("Mixed neighbourhood") {
        std::vector<WiFiNetwork> scan = {
            WiFiNetwork("A", 80, true, "WPA2", 2412),
            WiFiNetwork("B", 60, true, "WPA2", 5180),
        };
        CHECK(wifi_scan_spans_multiple_bands(scan));
    }

    SECTION("A single dual-band SSID is enough") {
        std::vector<WiFiNetwork> scan = {
            WiFiNetwork("Only", 80, true, "WPA2", 2412),
        };
        scan[0].band_mask = WIFI_BAND_2_4GHZ | WIFI_BAND_5GHZ;
        CHECK(wifi_scan_spans_multiple_bands(scan));
    }

    SECTION("Backend that reports no frequency at all") {
        std::vector<WiFiNetwork> scan = {
            WiFiNetwork("A", 80, true, "WPA2"),
            WiFiNetwork("B", 60, true, "WPA2"),
        };
        CHECK_FALSE(wifi_scan_spans_multiple_bands(scan));
    }

    SECTION("Empty scan") {
        CHECK_FALSE(wifi_scan_spans_multiple_bands({}));
    }
}

// =============================================================================
// Mock backend wiring — the dual-band SSID must survive to the picker
// =============================================================================

TEST_CASE("Mock backend surfaces its dual-band SSID as one row with both bands",
          "[wifi][band][1189][mock]") {
    WifiBackendMock backend;
    REQUIRE(backend.start().success());

    std::vector<WiFiNetwork> networks;
    REQUIRE(backend.get_scan_results(networks).success());

    const WiFiNetwork* office = find_ssid(networks, "Office-Main");
    REQUIRE(office != nullptr);
    CHECK(office->band_mask == (WIFI_BAND_2_4GHZ | WIFI_BAND_5GHZ));

    // Exactly one row for the dual-band SSID, not two and not zero.
    size_t office_rows = static_cast<size_t>(
        std::count_if(networks.begin(), networks.end(),
                      [](const WiFiNetwork& n) { return n.ssid == "Office-Main"; }));
    CHECK(office_rows == 1);

    // A 5GHz-only neighbour is still listed on its own band.
    const WiFiNetwork* home = find_ssid(networks, "HomeNetwork-5G");
    REQUIRE(home != nullptr);
    CHECK(home->band_mask == WIFI_BAND_5GHZ);

    CHECK(wifi_scan_spans_multiple_bands(networks));

    backend.stop();
}
