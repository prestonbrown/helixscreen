// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "hardware_object_table.h"
#include "sensor_enum_names.h"

#include <string>
#include <string_view>
#include <vector>

namespace helix::sensors {

/// @brief Role assigned to a humidity sensor
enum class HumiditySensorRole {
    NONE = 0,    ///< Discovered but not assigned to a role
    CHAMBER = 1, ///< Used for monitoring chamber humidity
    DRYER = 2,   ///< Used for monitoring filament dryer humidity
};

/// @brief Type of humidity sensor hardware
enum class HumiditySensorType {
    BME280 = 1,           ///< BME280 sensor (humidity, pressure, temperature)
    HTU21D = 2,           ///< HTU21D sensor (humidity, temperature)
    SHT3X = 3,            ///< SHT3X sensor (humidity, temperature)
    AHT10 = 4,            ///< AHT10 sensor (humidity, temperature)
    AHT20 = 5,            ///< AHT20 sensor (humidity, temperature)
    AHT20_F = 6,          ///< AHT20-F sensor (humidity, temperature)
    AHT3X = 7,            ///< AHT3x object (humidity, temperature)
    TEMPERATURE_OAMS = 8, ///< temperature_oams object (humidity, temperature)
};

/// @brief Descriptor for one humidity-capable Klipper sensor chip.
/// Single source of truth: to support a new humidity chip, add ONE row to
/// humidity_sensor_chips() and ONE enum value above. Everything else
/// (discovery whitelist, config persistence, UI label) derives from this table.
struct HumiditySensorChip {
    HumiditySensorType type;
    std::string_view klipper_prefix; ///< object-name token incl. trailing space, e.g. "bme280 "
    std::string_view config_id;      ///< persisted in settings.json, e.g. "bme280"
    std::string_view display_name;   ///< shown in the UI, e.g. "BME280"
    bool reports_pressure;           ///< true only for chips that publish a pressure field (BME280)
};

/// @brief The set of humidity-capable sensor chips HelixScreen recognizes.
/// Prefixes carry a trailing space so "aht20 " never matches "aht20_f heater_box1".
[[nodiscard]] inline const std::vector<HumiditySensorChip>& humidity_sensor_chips() {
    static const std::vector<HumiditySensorChip> chips = {
        {HumiditySensorType::BME280, "bme280 ", "bme280", "BME280", true},
        {HumiditySensorType::HTU21D, "htu21d ", "htu21d", "HTU21D", false},
        {HumiditySensorType::SHT3X, "sht3x ", "sht3x", "SHT3X", false},
        {HumiditySensorType::AHT10, "aht10 ", "aht10", "AHT10", false},
        {HumiditySensorType::AHT20, "aht20 ", "aht20", "AHT20", false},
        {HumiditySensorType::AHT20_F, "aht20_f ", "aht20_f", "AHT20-F", false},
        {HumiditySensorType::AHT3X, "aht3x ", "aht3x", "AHT3x", false},
        {HumiditySensorType::TEMPERATURE_OAMS, "temperature_oams ", "temperature_oams",
         "temperature_oams", false},
    };
    return chips;
}

/// @brief Match a Klipper object name against the chip table (via the object table).
/// @return descriptor pointer if the object is a known humidity chip, else nullptr.
[[nodiscard]] inline const HumiditySensorChip*
humidity_chip_for_object(const std::string& klipper_name) {
    const auto cls = helix::hw::classify_object(klipper_name);
    if (cls.kind != helix::hw::ObjectKind::HumidityChip) {
        return nullptr;
    }
    for (const auto& c : humidity_sensor_chips()) {
        if (static_cast<uint8_t>(c.type) == cls.chip) {
            return &c;
        }
    }
    return nullptr;
}

/// @brief @p name with the chip's display name appended ("Chamber Temperature (BME280)")
/// when @p klipper_name is a humidity chip, else @p name unchanged.
/// A chip's temperature and a thermistor share the instance name ("bme280 chamber",
/// "temperature_sensor chamber"), so every list naming both must tell them apart.
[[nodiscard]] inline std::string with_humidity_chip_suffix(std::string name,
                                                           const std::string& klipper_name) {
    if (const auto* chip = humidity_chip_for_object(klipper_name)) {
        name += " (";
        name += chip->display_name;
        name += ')';
    }
    return name;
}

inline constexpr EnumName<HumiditySensorRole> kHumiditySensorRoles[] = {
    {HumiditySensorRole::NONE, "none", "Unassigned"},
    {HumiditySensorRole::CHAMBER, "chamber", "Chamber"},
    {HumiditySensorRole::DRYER, "dryer", "Dryer"},
};

[[nodiscard]] inline std::string humidity_role_to_string(HumiditySensorRole role) {
    return enum_id(kHumiditySensorRoles, role);
}

[[nodiscard]] inline HumiditySensorRole humidity_role_from_string(const std::string& str) {
    return enum_from_id(kHumiditySensorRoles, str);
}

[[nodiscard]] inline std::string humidity_role_to_display_string(HumiditySensorRole role) {
    return enum_display(kHumiditySensorRoles, role);
}

/// @brief Convert type enum to config string
/// @param type The type to convert
/// @return Config-safe string (config_id from the chip table), "bme280" if unknown
[[nodiscard]] inline std::string humidity_type_to_string(HumiditySensorType type) {
    for (const auto& c : humidity_sensor_chips()) {
        if (c.type == type)
            return std::string(c.config_id);
    }
    return "bme280";
}

/// @brief Parse type string to enum
/// @param str The config string to parse (matched against chip-table config_id)
/// @return Parsed type, defaults to BME280 if unrecognized
[[nodiscard]] inline HumiditySensorType humidity_type_from_string(const std::string& str) {
    for (const auto& c : humidity_sensor_chips()) {
        if (c.config_id == str)
            return c.type;
    }
    return HumiditySensorType::BME280;
}

/// @brief Convert type enum to human-readable display string
/// @param type The type to convert
/// @return Display name from the chip table, "Unknown" if unrecognized
[[nodiscard]] inline std::string humidity_type_to_display_string(HumiditySensorType type) {
    for (const auto& c : humidity_sensor_chips()) {
        if (c.type == type)
            return std::string(c.display_name);
    }
    return "Unknown";
}

} // namespace helix::sensors
