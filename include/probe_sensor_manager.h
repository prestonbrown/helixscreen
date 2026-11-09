// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "probe_sensor_types.h"
#include "sensor_manager.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace helix::sensors {

/**
 * @brief Native Klipper probes (probe, bltouch, smart_effector, probe_eddy_current,
 *        beacon, cartographer, plus the config-only load_cell_probe and dockable_probe).
 *
 * Discovery, roles, enabled state, persistence and publishing come from
 * SensorManager. This class owns what is probe-specific: the per-type status
 * object and fields, the Z offset seeded from the configfile, the Klicky macro
 * upgrade, the single-probe Z_PROBE default and the z_offset subject.
 *
 * Status keys read: last_z_result, last_query (the last QUERY_PROBE result) and
 * z_offset. Which object carries them differs per type. Mainline modules do not
 * publish z_offset, so it is seeded from the configfile; the Creality K1/K2 and
 * QIDI forks publish it on their probe. Per-type table:
 * docs/devel/SENSOR_MANAGEMENT.md.
 *
 * @note Switch sensors configured as probes are handled by the filament
 *       manager, not this manager.
 */
class ProbeSensorManager : public SensorManager {
  public:
    static ProbeSensorManager& instance();

    /// Typed view of the entries, one config per physical probe.
    [[nodiscard]] std::vector<ProbeSensorConfig> get_sensors() const;

    /// Z offset in mm of the enabled, available Z_PROBE holder, or 0.0 when none.
    [[nodiscard]] float get_z_offset() const;

    /// Whether that holder's Z offset is known: its status published one or a config
    /// section configures it. An unknown offset is not a zero one.
    [[nodiscard]] bool has_z_offset() const;

    /// Z offset subject: mm x 1000, -1 when no enabled sensor holds Z_PROBE.
    [[nodiscard]] lv_subject_t* get_probe_z_offset_subject();

    /// Override the probe type for the primary probe sensor.
    /// Called by PrinterState after printer detection identifies a specific probe type
    /// that can't be inferred from Klipper object names alone (e.g., prtouch_v2 registers
    /// as generic "probe"). The override is kept and applied again on every later
    /// discover(), which re-derives types from object names; STANDARD clears it.
    void set_probe_type_override(ProbeSensorType type);

    /// Every probe subscription: the per-type object and its fields. Each probe the
    /// object list keeps is mapped to the object its module publishes on; types
    /// without a usable last_query skip it. z_offset and name are requested
    /// everywhere; a module without one answers null.
    [[nodiscard]] nlohmann::json extra_subscriptions(const PrinterDiscovery& hw) const override;

  protected:
    void register_subjects(SubjectManager& subjects) override;
    void refresh_subjects() override;
    bool parse_status(const nlohmann::json& status) override;
    void on_discovered(const PrinterDiscovery& hw) override;
    void on_entries_changed() override;

  private:
    friend class ProbeSensorManagerTestAccess;

    ProbeSensorManager();
    static Options make_options();

    /// Mutex held: the enabled, available Z_PROBE holder's reading, or nullptr.
    [[nodiscard]] const ProbeSensorState* z_probe_reading_locked() const;

    /// Mutex held: give the first STANDARD probe the printer database's type.
    /// Returns whether an entry changed.
    bool apply_type_override_locked();

    std::map<std::string, ProbeSensorState> readings_; ///< by klipper_name
    /// The probe object that config-only sections folded into, and the
    /// configured z_offset of each of those sections, by section name.
    std::string folded_probe_;
    std::map<std::string, float> folded_z_offsets_;
    lv_subject_t z_offset_{};
    /// From the printer database; STANDARD = none.
    ProbeSensorType type_override_ = ProbeSensorType::STANDARD;
};

} // namespace helix::sensors
