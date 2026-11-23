// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wifi_5ghz_detection.cpp
 * @brief Unit tests for WiFi 5GHz capability detection parsing functions
 */

#include "../../include/wifi_5ghz_detection.h"
#include "../../include/wifi_backend.h"

#include "../catch_amalgamated.hpp"

// wifi_parse_freq_list_has_5ghz Tests

TEST_CASE("wifi_parse_freq_list_has_5ghz: 2.4GHz-only frequency list returns false",
          "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz("2412 2437 2462") == false);
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: dual-band frequency list returns true", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz("2412 2437 5180 5240") == true);
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: single 5GHz frequency returns true", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz("5180") == true);
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: empty string returns false", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz("") == false);
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: FAIL response returns false", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz("FAIL") == false);
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: UNKNOWN COMMAND response returns false", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz("UNKNOWN COMMAND") == false);
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: malformed tokens mixed with valid 5GHz returns true",
          "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz("2412 abc 5180 xyz") == true);
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: malformed tokens with only 2.4GHz returns false",
          "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz("2412 abc 2437 xyz") == false);
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: whitespace variations handled gracefully",
          "[wifi][5ghz]") {
    SECTION("tabs between frequencies") {
        REQUIRE(wifi_parse_freq_list_has_5ghz("2412\t5180\t5240") == true);
    }

    SECTION("multiple spaces between frequencies") {
        REQUIRE(wifi_parse_freq_list_has_5ghz("2412   5180   5240") == true);
    }

    SECTION("trailing newline") {
        REQUIRE(wifi_parse_freq_list_has_5ghz("2412 5180 5240\n") == true);
    }

    SECTION("leading and trailing whitespace") {
        REQUIRE(wifi_parse_freq_list_has_5ghz("  2412 5180  ") == true);
    }

    SECTION("mixed tabs and spaces with only 2.4GHz") {
        REQUIRE(wifi_parse_freq_list_has_5ghz("\t 2412 \t 2437 \n") == false);
    }
}

TEST_CASE("wifi_parse_freq_list_has_5ghz: all frequencies below 5000 returns false",
          "[wifi][5ghz]") {
    REQUIRE(wifi_parse_freq_list_has_5ghz(
                "2412 2417 2422 2427 2432 2437 2442 2447 2452 2457 2462") == false);
}

// wifi_parse_iw_phy_has_5ghz Tests

static const char* kIwOutputDualBand = "Band 1:\n"
                                       "    Frequencies:\n"
                                       "        * 2412 MHz [1] (20.0 dBm)\n"
                                       "        * 2437 MHz [6] (20.0 dBm)\n"
                                       "        * 2462 MHz [11] (20.0 dBm)\n"
                                       "Band 2:\n"
                                       "    Frequencies:\n"
                                       "        * 5180 MHz [36] (20.0 dBm)\n"
                                       "        * 5240 MHz [48] (20.0 dBm)\n";

static const char* kIwOutput24Only = "Band 1:\n"
                                     "    Frequencies:\n"
                                     "        * 2412 MHz [1] (20.0 dBm)\n"
                                     "        * 2417 MHz [2] (20.0 dBm)\n"
                                     "        * 2422 MHz [3] (20.0 dBm)\n"
                                     "        * 2427 MHz [4] (20.0 dBm)\n"
                                     "        * 2432 MHz [5] (20.0 dBm)\n"
                                     "        * 2437 MHz [6] (20.0 dBm)\n"
                                     "        * 2442 MHz [7] (20.0 dBm)\n"
                                     "        * 2447 MHz [8] (20.0 dBm)\n"
                                     "        * 2452 MHz [9] (20.0 dBm)\n"
                                     "        * 2457 MHz [10] (20.0 dBm)\n"
                                     "        * 2462 MHz [11] (20.0 dBm)\n";

TEST_CASE("wifi_parse_iw_phy_has_5ghz: dual-band iw output returns true", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_iw_phy_has_5ghz(kIwOutputDualBand) == true);
}

TEST_CASE("wifi_parse_iw_phy_has_5ghz: 2.4GHz-only iw output returns false", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_iw_phy_has_5ghz(kIwOutput24Only) == false);
}

TEST_CASE("wifi_parse_iw_phy_has_5ghz: empty string returns false", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_iw_phy_has_5ghz("") == false);
}

TEST_CASE("wifi_parse_iw_phy_has_5ghz: truncated output returns false", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_iw_phy_has_5ghz("Band 1:\n    Frequenc") == false);
}

TEST_CASE("wifi_parse_iw_phy_has_5ghz: malformed output without MHz markers returns false",
          "[wifi][5ghz]") {
    REQUIRE(wifi_parse_iw_phy_has_5ghz("some random text\nwithout frequency data\n") == false);
}

// wifi_parse_nm_wifi_properties_has_5ghz Tests

TEST_CASE("wifi_parse_nm_wifi_properties_has_5ghz: terse 5GHZ yes returns true", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_nm_wifi_properties_has_5ghz("WIFI-PROPERTIES.FREQ:5GHZ:yes") == true);
}

TEST_CASE("wifi_parse_nm_wifi_properties_has_5ghz: terse 5GHZ yes with other properties",
          "[wifi][5ghz]") {
    std::string props = "WIFI-PROPERTIES.FREQ:2GHZ:yes\n"
                        "WIFI-PROPERTIES.FREQ:5GHZ:yes\n"
                        "WIFI-PROPERTIES.WFD:no\n";
    REQUIRE(wifi_parse_nm_wifi_properties_has_5ghz(props) == true);
}

TEST_CASE("wifi_parse_nm_wifi_properties_has_5ghz: non-terse '5 GHz' returns true",
          "[wifi][5ghz]") {
    REQUIRE(wifi_parse_nm_wifi_properties_has_5ghz("Supports 5 GHz band") == true);
}

TEST_CASE("wifi_parse_nm_wifi_properties_has_5ghz: terse 5GHZ no returns false", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_nm_wifi_properties_has_5ghz("WIFI-PROPERTIES.FREQ:5GHZ:no") == false);
}

TEST_CASE("wifi_parse_nm_wifi_properties_has_5ghz: empty string returns false", "[wifi][5ghz]") {
    REQUIRE(wifi_parse_nm_wifi_properties_has_5ghz("") == false);
}

TEST_CASE("wifi_parse_nm_wifi_properties_has_5ghz: only 2GHZ yes returns false", "[wifi][5ghz]") {
    std::string props = "WIFI-PROPERTIES.FREQ:2GHZ:yes\n"
                        "WIFI-PROPERTIES.WFD:no\n";
    REQUIRE(wifi_parse_nm_wifi_properties_has_5ghz(props) == false);
}

// WiFiNetwork and ConnectionStatus frequency field defaults

TEST_CASE("WiFiNetwork default frequency_mhz is 0", "[wifi][5ghz]") {
    WiFiNetwork net;
    REQUIRE(net.frequency_mhz == 0);
}

TEST_CASE("WiFiNetwork constructor with freq param stores frequency", "[wifi][5ghz]") {
    WiFiNetwork net("TestNetwork", 75, true, "WPA2", 5180);
    REQUIRE(net.frequency_mhz == 5180);
    REQUIRE(net.ssid == "TestNetwork");
    REQUIRE(net.signal_strength == 75);
    REQUIRE(net.is_secured == true);
    REQUIRE(net.security_type == "WPA2");
}

TEST_CASE("WiFiNetwork constructor without freq param defaults to 0", "[wifi][5ghz]") {
    WiFiNetwork net("TestNetwork", 50, false, "Open");
    REQUIRE(net.frequency_mhz == 0);
}

TEST_CASE("WifiBackend::ConnectionStatus default frequency_mhz is 0", "[wifi][5ghz]") {
    WifiBackend::ConnectionStatus status{};
    REQUIRE(status.frequency_mhz == 0);
}
