// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/text_io.h"

#include <string>
#include <string_view>

/**
 * @brief Parse wpa_supplicant GET_CAPABILITY freq response for 5GHz support
 *
 * Splits space/tab-separated frequency integers and returns true if any >= 5000.
 * Handles FAIL, UNKNOWN, malformed tokens gracefully.
 *
 * @param freq_response Raw response from GET_CAPABILITY freq
 * @return true if any frequency >= 5000 MHz
 */
inline bool wifi_parse_freq_list_has_5ghz(const std::string& freq_response) {
    if (freq_response.empty()) {
        return false;
    }

    for (std::string_view token : helix::text_io::split_ws(freq_response)) {
        // Non-numeric tokens (FAIL, UNKNOWN, etc.) parse as nullopt and are skipped.
        auto freq = helix::text_io::parse_int<int>(token);
        if (freq && *freq >= 5000) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Parse `iw phy` info output for 5GHz frequency support
 *
 * Searches for frequency lines like "* 5180 MHz" where freq >= 5000.
 *
 * @param iw_output Raw output from `iw phy <phy> info`
 * @return true if any frequency >= 5000 MHz found
 */
inline bool wifi_parse_iw_phy_has_5ghz(const std::string& iw_output) {
    if (iw_output.empty()) {
        return false;
    }

    for (std::string_view line : helix::text_io::lines(iw_output)) {
        // Look for lines like "        * 5180 MHz [36] (20.0 dBm)"
        auto mhz_pos = line.find("MHz");
        if (mhz_pos == std::string::npos) {
            continue;
        }

        // Extract frequency number before "MHz"
        // Walk backwards from mhz_pos to find the number
        auto num_end = mhz_pos;
        while (num_end > 0 && line[num_end - 1] == ' ') {
            num_end--;
        }
        auto num_start = num_end;
        while (num_start > 0 && line[num_start - 1] >= '0' && line[num_start - 1] <= '9') {
            num_start--;
        }

        if (num_start >= num_end) {
            continue;
        }

        auto freq = helix::text_io::parse_int<int>(line.substr(num_start, num_end - num_start));
        if (freq && *freq >= 5000) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Parse nmcli WIFI-PROPERTIES output for 5GHz support
 *
 * Primary: checks for "5GHZ:yes" (terse format).
 * Secondary: checks for "5 GHz" (non-terse format).
 * Explicit "5GHZ:no" → false.
 *
 * @param props Raw nmcli output of WIFI-PROPERTIES
 * @return true if 5GHz support detected
 */
inline bool wifi_parse_nm_wifi_properties_has_5ghz(const std::string& props) {
    if (props.empty()) {
        return false;
    }

    // Primary: terse format "5GHZ:yes" or "5GHZ:no"
    auto pos_5ghz = props.find("5GHZ:");
    if (pos_5ghz != std::string::npos) {
        auto value_start = pos_5ghz + 5; // length of "5GHZ:"
        if (value_start < props.size()) {
            // Check for "yes" vs "no"
            if (props.compare(value_start, 3, "yes") == 0) {
                return true;
            }
            if (props.compare(value_start, 2, "no") == 0) {
                return false;
            }
        }
    }

    // Secondary: non-terse format with "5 GHz"
    if (props.find("5 GHz") != std::string::npos) {
        return true;
    }

    return false;
}
