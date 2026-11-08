// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file bt_discovery_utils.h
 * @brief Bluetooth discovery UUID classification helpers.
 *
 * Kept apart from bt_discovery.cpp so the logic can be unit-tested without
 * pulling in sd-bus or BlueZ dependencies.
 */

#include <cstring>

#ifdef _WIN32
#define strncasecmp _strnicmp
#endif

namespace helix::bluetooth {

/// Target service UUID prefixes (8-char hex)
inline constexpr const char* kSppUuidPrefix = "00001101";
inline constexpr const char* kPhomemoBleUuidPrefix = "0000ff00";
/// Niimbot Transparent UART service UUID (full 128-bit, matched by prefix)
inline constexpr const char* kNiimbotBleUuidPrefix = "e7810a71";
/// MakeID BLE service UUID (full 128-bit, matched by prefix)
inline constexpr const char* kMakeidBleUuidPrefix = "0000abf0";

/// Check if a UUID string matches one of our target label printer service UUIDs.
/// Comparison is case-insensitive on the first 8 hex characters (the UUID16 prefix).
inline bool is_label_printer_uuid(const char* uuid) {
    if (!uuid)
        return false;
    if (strncasecmp(uuid, kSppUuidPrefix, 8) == 0)
        return true;
    if (strncasecmp(uuid, kPhomemoBleUuidPrefix, 8) == 0)
        return true;
    if (strncasecmp(uuid, kNiimbotBleUuidPrefix, 8) == 0)
        return true;
    if (strncasecmp(uuid, kMakeidBleUuidPrefix, 8) == 0)
        return true;
    return false;
}

/// Determine if a UUID indicates a BLE device.
inline bool uuid_is_ble(const char* uuid) {
    if (!uuid)
        return false;
    if (strncasecmp(uuid, kPhomemoBleUuidPrefix, 8) == 0)
        return true;
    if (strncasecmp(uuid, kNiimbotBleUuidPrefix, 8) == 0)
        return true;
    if (strncasecmp(uuid, kMakeidBleUuidPrefix, 8) == 0)
        return true;
    return false;
}

/// Known printer brand entry for unified matching.
struct PrinterBrand {
    const char* prefix;
    bool is_ble;     // true = BLE-only (no SPP/RFCOMM)
    bool is_brother; // true = Brother QL protocol
    bool is_niimbot; // true = Niimbot protocol
    bool is_makeid;  // true = MakeID protocol
};

// Shared table of known label printer brands.
// Phomemo M/Q series support both BLE and SPP but need SPP for printing.
inline constexpr PrinterBrand kKnownBrands[] = {
    // Brother QL/PT/TD/RJ series — SPP only
    {"QL-", false, true, false, false},
    {"PT-", false, true, false, false},
    {"TD-", false, true, false, false},
    {"RJ-", false, true, false, false},

    // Phomemo — dual-mode but prints via SPP
    {"Phomemo", false, false, false, false},

    // Dymo — SPP
    {"LW-", false, false, false, false},
    {"DYMO", false, false, false, false},

    // MUNBYN — SPP
    {"MUNBYN", false, false, false, false},

    // Niimbot — BLE only, custom protocol.
    // B-series share a 384-dot printhead; D-series have a 96-dot printhead.
    {"Niimbot", true, false, true, false},
    {"B21", true, false, true, false},
    {"B18", true, false, true, false},
    {"B3S", true, false, true, false},
    {"B1", true, false, true, false},
    {"D11", true, false, true, false},
    {"D110", true, false, true, false},

    // MakeID — BLE only, custom protocol
    {"MakeID", true, false, false, true},
    {"MAKEID", true, false, false, true},
    {"YichipFPGA", true, false, false, true}, // MakeID E1 uses Yichip BLE SoC

    // Supvan / KataSymbol — BLE only
    {"Supvan", true, false, false, false},
    {"KataSymbol", true, false, false, false},
    {"E10", true, false, false, false},
    {"E16", true, false, false, false},
    {"T50M", true, false, false, false},
};

/// Find a matching brand entry for a device name, or nullptr if unknown.
inline const PrinterBrand* find_brand(const char* name) {
    if (!name || !name[0])
        return nullptr;

    for (const auto& brand : kKnownBrands) {
        if (strncasecmp(name, brand.prefix, strlen(brand.prefix)) == 0)
            return &brand;
    }

    // Phomemo M/Q series: single letter + digits (e.g. M110, Q199)
    if ((name[0] == 'M' || name[0] == 'Q') && name[1] >= '0' && name[1] <= '9') {
        // Return a static entry for this pattern
        static constexpr PrinterBrand phomemo_mq = {"M/Q", false, false, false, false};
        return &phomemo_mq;
    }

    return nullptr;
}

/// Determine if a device name indicates a BLE-only printer (no SPP/RFCOMM).
/// Used when UUID-based detection isn't available (UUIDs resolve late on BLE).
inline bool name_suggests_ble(const char* name) {
    auto* brand = find_brand(name);
    return brand && brand->is_ble;
}

/// Check if a device name looks like a label printer.
inline bool is_likely_label_printer(const char* name) {
    if (find_brand(name))
        return true;

    // Generic keyword match
    if (!name || !name[0])
        return false;
    if (strcasestr(name, "printer") != nullptr)
        return true;
    if (strcasestr(name, "label") != nullptr)
        return true;
    return false;
}

/// Check if a device name indicates a Brother QL-family printer.
inline bool is_brother_printer(const char* name) {
    auto* brand = find_brand(name);
    return brand && brand->is_brother;
}

/// Check if a device name indicates a Brother PT-series (P-Touch) printer.
inline bool is_brother_pt_printer(const char* name) {
    if (!name || !name[0])
        return false;
    return strncasecmp(name, "PT-", 3) == 0;
}

/// Check if a device name indicates a Niimbot printer.
inline bool is_niimbot_printer(const char* name) {
    auto* brand = find_brand(name);
    return brand && brand->is_niimbot;
}

/// Check if a device name indicates a MakeID printer.
inline bool is_makeid_printer(const char* name) {
    auto* brand = find_brand(name);
    return brand && brand->is_makeid;
}

} // namespace helix::bluetooth
