// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file hardware_object_table.h
 * @brief What a Klipper object or config section is, decided in one place.
 *
 * Discovery, the subscription builder, the sensor managers, the hardware role
 * registry, fan state, the LED controller, the MCU performance source and
 * device display names all ask this table. Adding a chip or object name is one
 * row in src/printer/hardware_object_table.cpp.
 */
namespace helix::hw {

enum class ObjectKind : uint8_t {
    Unknown,
    Stepper,
    Mcu,
    Heater,            ///< extruder, extruderN, heater_bed
    GenericHeater,     ///< heater_generic <name>: a heater and a temperature sensor
    TemperatureSensor, ///< temperature_sensor <name>
    TemperatureFan,    ///< temperature_fan <name>: a sensor and a fan
    DriverTemperature, ///< tmc2240 / tmc5160 <stepper>
    HumidityChip,      ///< bme280, htu21d, sht3x, aht10, aht20, aht20_f, aht3x, temperature_oams
    Fan,               ///< fan (the part fan object)
    HeaterFan,
    ControllerFan,
    GenericFan,
    OutputPinFan,
    Led, ///< led, neopixel, dotstar (bare or named)
    OutputPinLed,
    LedEffect,
    Speaker, ///< output_pin named for a beeper/buzzer/speaker
    FanFeedback,
    FilamentSwitch,
    FilamentMotion,
    BackendFilamentSensor, ///< a filament system's own sensor objects (AmsBackend prefixes)
    WidthSensor,
    LoadCell,
    Probe,
    Input,         ///< gcode_button <name>
    Accelerometer, ///< config section only: no get_status
    PowerLossMonitor,
};

/// ObjectClass::chip for ObjectKind::Led.
inline constexpr uint8_t kLedChipGeneric = 0;  ///< [led name]: pins decide color and white
inline constexpr uint8_t kLedChipNeopixel = 1; ///< addressable RGB(W)
inline constexpr uint8_t kLedChipDotstar = 2;  ///< addressable RGB, never white

struct ObjectClass {
    ObjectKind kind = ObjectKind::Unknown;
    std::string instance; ///< the part after the prefix: "chamber" in "temperature_sensor chamber"
    uint8_t chip =
        0; ///< kind-specific type index (humidity chip, probe type, accel chip, width type)
};

/// Classify one name from printer.objects.list.
[[nodiscard]] ObjectClass classify_object(std::string_view klipper_object);

/// Classify one configfile.config section. `body` is the section's contents:
/// a few rules read fields (Beacon's onboard accelerometer).
[[nodiscard]] ObjectClass classify_section(std::string_view section, const nlohmann::json& body);

/// A fixed list of status field names in static storage: nothing is built at boot.
struct FieldList {
    const std::string_view* first = nullptr;
    size_t count = 0;

    [[nodiscard]] const std::string_view* begin() const {
        return first;
    }
    [[nodiscard]] const std::string_view* end() const {
        return first + count;
    }
    [[nodiscard]] size_t size() const {
        return count;
    }
    [[nodiscard]] bool empty() const {
        return count == 0;
    }
    [[nodiscard]] std::string_view front() const {
        return first[0];
    }
    [[nodiscard]] std::string_view back() const {
        return first[count - 1];
    }
};

/// The fields to subscribe for a kind. Empty: subscribe nothing. A single "*"
/// entry: subscribe the whole object (Moonraker null). Probe is empty: its fields vary
/// per probe type and ProbeSensorManager supplies them.
[[nodiscard]] FieldList status_fields(ObjectKind kind);

[[nodiscard]] bool is_heater(ObjectKind kind);      ///< Heater, GenericHeater
[[nodiscard]] bool is_fan(ObjectKind kind);         ///< every fan kind incl. TemperatureFan
[[nodiscard]] bool is_led(ObjectKind kind);         ///< Led, OutputPinLed
[[nodiscard]] bool is_temperature(ObjectKind kind); ///< kinds that report a temperature
[[nodiscard]] bool is_filament_sensor(ObjectKind kind);

/// Persisted / display name of a kind's chip index, e.g. humidity chip 2 -> "SHT3x".
[[nodiscard]] std::string_view chip_name(ObjectKind kind, uint8_t chip);

} // namespace helix::hw
