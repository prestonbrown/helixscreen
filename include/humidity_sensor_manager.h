// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/subject.h"
#include "humidity_sensor_types.h"
#include "sensor_manager.h"

#include <map>
#include <string>

namespace helix::sensors {

/**
 * @brief Humidity-capable chips (BME280, HTU21D, SHT3X, AHT10/20/20-F, ...).
 *
 * Discovery, roles, enabled state and publishing come from SensorManager. This
 * class owns the humidity readings and the chamber humidity subject. Humidity
 * sensors are not persisted: the CHAMBER and DRYER roles are assigned by sensor
 * name on every discovery and are not offered in Settings.
 *
 * The recognized chips are the table humidity_sensor_chips() in
 * humidity_sensor_types.h; the hardware object table classifies against it.
 *
 * Status JSON format:
 * @code
 * { "bme280 chamber": { "humidity": 45.5, "pressure": 1013.25, "temperature": 25.3 } }
 * @endcode
 */
class HumiditySensorManager : public SensorManager {
  public:
    static HumiditySensorManager& instance();

    /// Chamber humidity subject: percent x 10, -1 when no enabled sensor holds CHAMBER.
    [[nodiscard]] lv_subject_t* get_chamber_humidity_subject();

  protected:
    void register_subjects(SubjectManager& subjects) override;
    void refresh_subjects() override;
    bool parse_status(const nlohmann::json& status) override;
    void on_discovered(const PrinterDiscovery& hw) override;
    void on_entries_changed() override;

  private:
    friend class HumiditySensorManagerTestAccess;

    HumiditySensorManager();
    static Options make_options();

    std::map<std::string, float> humidity_; ///< percent, by klipper_name
    helix::Subject chamber_humidity_{};
};

} // namespace helix::sensors
