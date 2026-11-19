// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/subject.h"
#include "sensor_manager.h"
#include "width_sensor_types.h"

#include <map>
#include <string>

namespace helix::sensors {

/**
 * @brief Filament width sensors (TSL1401CL and Hall-effect).
 *
 * Discovery, roles, enabled state, persistence and publishing come from
 * SensorManager. This class owns the diameter readings and the two subjects
 * that expose the FLOW_COMPENSATION sensor's diameter.
 *
 * Status JSON format:
 * @code
 * { "tsl1401cl_filament_width_sensor": { "Diameter": 1.75, "Raw": 12345 } }
 * @endcode
 */
class WidthSensorManager : public SensorManager {
  public:
    static WidthSensorManager& instance();

    /// Diameter subject: mm x 1000, -1 when no enabled sensor holds the role.
    [[nodiscard]] lv_subject_t* get_diameter_subject();

    /// Formatted diameter ("1.75mm" or "--"), written by the home widget.
    [[nodiscard]] lv_subject_t* get_diameter_text_subject();

    /// "Hall width sensor", with "Hall · hall_filament_width_sensor" underneath.
    [[nodiscard]] SensorRowView row(size_t index) const override;

  protected:
    void register_subjects(SubjectManager& subjects) override;
    void refresh_subjects() override;
    bool parse_status(const nlohmann::json& status) override;
    void on_discovered(const PrinterDiscovery& hw) override;
    void on_entries_changed() override;

  private:
    friend class WidthSensorManagerTestAccess;

    WidthSensorManager();
    static Options make_options();

    /// Mutex held: diameter of the enabled, available sensor holding FLOW_COMPENSATION, or -1.
    [[nodiscard]] int flow_diameter_micrometers_locked() const;

    std::map<std::string, WidthSensorState> readings_; ///< by klipper_name
    helix::Subject diameter_{};
    helix::Subject diameter_text_{};
    char diameter_text_buf_[16]{};
};

} // namespace helix::sensors
