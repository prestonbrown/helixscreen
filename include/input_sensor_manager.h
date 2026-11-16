// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/subject.h"
#include "input_sensor_types.h"
#include "sensor_manager.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace helix::sensors {

/// The edges a gcode_button's configfile.config section acts on.
[[nodiscard]] InputConfigReaction input_config_reaction(const nlohmann::json& section);

/**
 * @brief Manager for on/off inputs: door switches, buttons, dock sensors.
 *
 * Discovers every Klipper `gcode_button`, tracks its live state, and gives each
 * a role. The role says what an input means to generic consumers; today the one
 * consumer is the door-open print-start gate, which reads the Door inputs. A
 * name containing "door", "lid" or "cover" starts as Door, everything else as
 * Other, and the user's choice is persisted per printer under "input_sensors".
 * set_option() is the Invert switch.
 *
 * Inputs whose meaning belongs to a subsystem (a tool changer's dock and
 * carriage buttons) are listed here as plain inputs; that subsystem keeps
 * owning what they mean.
 *
 * Settings > Sensors lists each input through row(): its live state, its role
 * and the Invert switch.
 *
 * Status JSON format:
 * @code
 * { "gcode_button frontDoor": { "state": "PRESSED" } }
 * @endcode
 */
class InputSensorManager : public SensorManager {
  public:
    static InputSensorManager& instance();

    /// Whether the printer's own config reacts when this Door input opens. The
    /// mid-print warning skips such a door rather than repeat the printer's own.
    [[nodiscard]] bool firmware_reacts_to_opening(const std::string& klipper_name) const;

    /// Short names of the doors in @p newly_open (klipper names) owed a warning
    /// while printing @p print_materials: the door check is on, the print has a
    /// material that needs a warm enclosure (door_warning_material()), and the
    /// printer's own config does not already announce that door opening.
    [[nodiscard]] std::vector<std::string>
    mid_print_door_warnings(const std::vector<std::string>& newly_open,
                            const std::vector<std::string>& print_materials) const;

    /// Whether any input holds the Door role.
    [[nodiscard]] bool has_door_input() const;

    /// Short names of the Door inputs that read open, in discovery order.
    [[nodiscard]] std::vector<std::string> open_doors() const;

    /// Whether the door-open print-start check runs. Until the user sets it, on
    /// exactly when the printer has a Door input.
    [[nodiscard]] bool door_check_enabled() const;
    void set_door_check_enabled(bool enabled);

    /// Invert: a door switch then reads closed while released.
    void set_option(std::string_view klipper_name, bool inverted) override;

    /// Adds the live state (door wording for a Door input; alert while open)
    /// and the Invert value.
    [[nodiscard]] SensorRowView row(size_t index) const override;

  protected:
    void register_subjects(SubjectManager& subjects) override;
    void refresh_subjects() override;
    bool parse_status(const nlohmann::json& status) override;
    void on_discovered(const PrinterDiscovery& hw) override;
    void on_entries_changed() override;
    void read_entry_extra(SensorEntry& entry, const nlohmann::json& saved) override;
    void write_entry_extra(const SensorEntry& entry, nlohmann::json& out) const override;
    void read_extra_settings(const nlohmann::json& saved) override;
    void write_extra_settings(nlohmann::json& out) const override;

  private:
    friend class InputSensorManagerTestAccess;

    InputSensorManager();
    static Options make_options();

    /// Mutex held: the reading for @p klipper_name, or an unreported one.
    [[nodiscard]] const InputReading& reading_locked(const std::string& klipper_name) const;
    /// Mutex held.
    [[nodiscard]] bool reads_open_locked(const SensorEntry& entry) const;
    [[nodiscard]] bool has_door_locked() const;

    std::map<std::string, InputReading> readings_; ///< by klipper_name
    /// The user's door-check choice; nullopt follows has_door_input().
    std::optional<bool> door_check_;
    /// Names present at the last discovery: only a name new to this set gets a
    /// default role, so a rediscovery never undoes the user's choice.
    std::set<std::string> known_;
    /// Per gcode_button klipper name, the edges its config acts on.
    std::map<std::string, InputConfigReaction> config_reactions_;
    /// A status frame changed an input since the last refresh: a door that now
    /// reads open was opened and may be owed a mid-print toast.
    bool status_frame_ = false;
    /// Klipper names of the Door inputs that read open at the last refresh, for
    /// the one-toast-per-opening rule. Main thread only.
    std::vector<std::string> open_doors_seen_;

    helix::Subject door_check_subject_{};
};

} // namespace helix::sensors
