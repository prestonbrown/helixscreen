// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "json_fwd.h"
#include "subject_managed_panel.h"

#include <lvgl.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix {
class Config;
}

namespace helix {

/// Fan type classification for display and control
enum class FanType {
    PART_COOLING,    ///< Main part cooling fan ("fan" or configured part fan)
    HEATER_FAN,      ///< Hotend cooling fan (auto-controlled, not user-adjustable)
    CONTROLLER_FAN,  ///< Electronics cooling (auto-controlled)
    TEMPERATURE_FAN, ///< Thermostatically controlled fan (auto-controlled)
    GENERIC_FAN,     ///< User-controllable generic fan (fan_generic)
    OUTPUT_PIN_FAN   ///< Creality-style output_pin fan (controllable via M106 P<index>)
};

/// A fan's role on the printer: what the wizard assigns, and the slot a
/// fan display shows.
enum class FanRole { Part, Hotend, Chamber, Exhaust, Aux };

/// How much room a fan's name has.
enum class FanLabelLength {
    Chip, ///< a chip pill: "Part", "HE", "Aux"
    Word, ///< a row label: "Part", "Hotend", "Aux"
    Full, ///< the role's name: "Part Fan", "Hotend Fan", "Aux Fan"
};

/// Untranslated i18n key of a role's name at the given length: the one
/// spelling of a fan role everywhere it is printed.
[[nodiscard]] const char* fan_role_label(FanRole role, FanLabelLength length);

/// Untranslated i18n key of a fan type's short name for a type pill. The part
/// and heater types read as the roles they fill ("Part", "HE").
[[nodiscard]] const char* fan_type_label(FanType type);

/**
 * @brief Wizard-configured fan role assignments
 *
 * Maps fan roles to Moonraker object names. Used to:
 * - Correctly classify the configured part fan (even if it's a fan_generic)
 * - Override display names with role-based names for configured fans
 */
struct FanRoleConfig {
    std::string part_fan;    ///< Configured part cooling fan object name
    std::string hotend_fan;  ///< Configured hotend fan object name
    std::string chamber_fan; ///< Configured chamber fan object name
    std::string exhaust_fan; ///< Configured exhaust fan object name
    std::string aux_fan;     ///< Configured aux fan object name

    /// Resolve roles from config against the live discovered fan list, auto-healing
    /// (and persisting) stale roles that name objects no longer present. Pass the
    /// same fan vector handed to init_fans().
    static FanRoleConfig from_config(Config* config,
                                     const std::vector<std::string>& discovered_fans);
};

/**
 * @brief Fan information for multi-fan display
 *
 * Holds display name, current speed, and controllability for each fan
 * discovered from Moonraker.
 */
struct FanInfo {
    std::string object_name;  ///< Full Moonraker object name (e.g., "heater_fan hotend_fan")
    std::string display_name; ///< Human-readable name (e.g., "Hotend Fan")
    FanType type = FanType::GENERIC_FAN;
    int speed_percent = 0;        ///< Current speed 0-100%
    bool is_controllable = false; ///< true for fan_generic, false for heater_fan/controller_fan
    std::optional<int> rpm;       ///< RPM from fan_feedback or Klipper rpm field
    bool ever_ran = false;        ///< true once speed_percent has been > 0 this session —
                                  ///< lets the part slot stay on a real [fan] that is only
                                  ///< momentarily off (first layer, bridges) instead of
                                  ///< flicking to a running auxiliary fan (#1124)
};

/**
 * @brief Picked primary fan object names for the standard 3-fan display.
 *
 * Each member is the object_name of the first fan of that role found in
 * the discovered fan list (in fan-discovery order), or empty if none.
 */
struct PrimaryFans {
    std::string part;   ///< PART_COOLING fan (runtime-resolved), or empty
    std::string hotend; ///< First HEATER_FAN, or empty
    std::string aux;    ///< First of CONTROLLER_FAN, TEMPERATURE_FAN,
                        ///< GENERIC_FAN, OUTPUT_PIN_FAN — or empty

    bool operator==(const PrimaryFans& o) const {
        return part == o.part && hotend == o.hotend && aux == o.aux;
    }
    bool operator!=(const PrimaryFans& o) const {
        return !(*this == o);
    }
};

/// Which primary slots this printer's fans can ever fill, whichever fan holds
/// each now. classify_primary_fans() can move a fan between slots mid-print
/// (a commandable fan that runs takes the part slot from an idle [fan]); this
/// answer does not move with it.
struct PrimaryFanSlots {
    bool part = false;
    bool hotend = false;
    bool aux = false;
};

/**
 * @brief Manages fan-related subjects for printer state
 *
 * Handles both static fan subjects (main fan speed, version) and
 * dynamic per-fan subjects created during printer discovery.
 * Extracted from PrinterState as part of god class decomposition.
 */
class PrinterFanState {
  public:
    PrinterFanState() = default;
    ~PrinterFanState() = default;

    // Non-copyable
    PrinterFanState(const PrinterFanState&) = delete;
    PrinterFanState& operator=(const PrinterFanState&) = delete;

    /**
     * @brief Initialize fan subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Update fan state from Moonraker status JSON
     * @param status JSON object containing fan data
     */
    void update_from_status(const nlohmann::json& status);

    /**
     * @brief Initialize fan tracking from discovered fan objects
     * @param fan_objects List of Moonraker fan object names
     * @param roles Wizard-configured fan role assignments (for naming and classification)
     * @param max_power Per-fan `max_power` from configfile.settings, keyed by
     *        lowercased object name. Klipper reports `speed` scaled by max_power;
     *        this map lets us divide it back out so a full-on fan reads 100%
     *        (matching Mainsail). Absent entries default to 1.0 (no scaling).
     */
    void init_fans(const std::vector<std::string>& fan_objects, const FanRoleConfig& roles = {},
                   const std::unordered_map<std::string, double>& max_power = {});

    /**
     * @brief Re-apply a role mapping to the fans already discovered.
     *
     * For the targeted-roles wizard, where the hardware did not change — only
     * which discovered fan plays which role. Callers used to reach for
     * init_fans() because it was the only entry point that accepted a
     * FanRoleConfig, which meant re-passing a fan list on a config change and
     * hoping it matched what was discovered. This re-uses the retained list, so
     * the two cannot drift apart.
     *
     * Note this is NOT list-preserving: roles decide whether the bare [fan]
     * object is shadowed by a named part fan, so a role change can legitimately
     * add or drop that one entry. Everything else — live speeds, ever_ran, rpm,
     * and the per-fan subjects — rides through, as it does for any re-init.
     */
    void apply_roles(const FanRoleConfig& roles);

    /**
     * @brief Update speed for a specific fan (called during status updates)
     * @param object_name Moonraker object name (e.g., "heater_fan hotend_fan")
     * @param speed Speed as 0.0-1.0 (Moonraker format)
     */
    void update_fan_speed(const std::string& object_name, double speed);

    /// Update RPM reading for a fan (from fan_feedback or Klipper rpm field)
    void update_fan_rpm(const std::string& object_name, int rpm);

    /// Rename a fan: saves custom name to Config, updates display_name, bumps fans_version
    void rename_fan(const std::string& object_name, const std::string& new_name);

    /// Re-resolve every fan's display name in the current language (custom
    /// names stay as typed) and bump fans_version so consumers redraw.
    void refresh_display_names();

    // Subject accessors
    lv_subject_t* get_fan_speed_subject() {
        return &fan_speed_;
    }
    lv_subject_t* get_fans_version_subject() {
        return &fans_version_;
    }

    /// Increments whenever the runtime-resolved primary fan roles change (e.g. a
    /// fan starts/stops and the part slot moves to it). The print-status compact
    /// row re-binds on this, distinct from fans_version which signals structural
    /// (fan set) changes to every fan consumer (#1124).
    lv_subject_t* get_primary_fans_version_subject() {
        return &primary_fans_version_;
    }

    /**
     * @brief Get speed subject for a specific fan (dynamic — requires lifetime token!)
     *
     * Returns the per-fan speed subject for reactive UI updates.
     * Each fan discovered via init_fans() has its own subject.
     *
     * IMPORTANT: These are dynamic subjects that may be destroyed during reconnection.
     * Always pass the returned lifetime token to your observer factory function
     * to prevent use-after-free crashes. See ui_observer_guard.h for details.
     *
     * @param object_name Moonraker object name (e.g., "fan", "heater_fan hotend_fan")
     * @param[out] lifetime Receives the subject's lifetime token (empty if not found)
     * @return Pointer to subject, or nullptr if fan not found
     */
    lv_subject_t* get_fan_speed_subject(const std::string& object_name, SubjectLifetime& lifetime);

    /// @deprecated Use the overload that returns a SubjectLifetime token.
    /// This overload exists only for call sites that don't create observers.
    lv_subject_t* get_fan_speed_subject(const std::string& object_name);

    /**
     * @brief Get all tracked fans
     * @return Const reference to fan info vector
     */
    const std::vector<FanInfo>& get_fans() const {
        return fans_;
    }

    /// Pick first fan of each primary role from the discovered list.
    /// Returns empty strings for roles with no detected fan.
    PrimaryFans classify_primary_fans() const;
    /// The slots classify_primary_fans() can fill on this fan set.
    PrimaryFanSlots possible_primary_fan_slots() const;

    /// Classify a fan object by name. @p configured_part_fan, when set, is the
    /// object the user assigned the part cooling role.
    static FanType classify_fan_type(const std::string& object_name,
                                     const std::string& configured_part_fan);

    /// Check if fan type is user-controllable, i.e. M106-commandable and thus
    /// eligible to be the part cooling fan: PART_COOLING, GENERIC_FAN,
    /// OUTPUT_PIN_FAN. Auto-controlled fans (heater/controller/temperature) are
    /// excluded — they can never be driven as a part fan.
    static bool is_fan_controllable(FanType type);

    /// Fans in @p fan_objects the user can control besides part cooling.
    static int count_extra_controllable_fans(const std::vector<std::string>& fan_objects,
                                             const std::string& configured_part_fan = {});

  private:
    friend class PrinterFanStateTestAccess;

    /// Classify fan type from object name (considers configured part fan)
    FanType classify_fan_type(const std::string& object_name) const;

    /// Check if fan type belongs in the aux slot (anything that isn't the part
    /// cooling or hotend fan): CONTROLLER_FAN, TEMPERATURE_FAN, GENERIC_FAN,
    /// OUTPUT_PIN_FAN.
    static bool is_aux_fan(FanType type);

    /// Resolve the part-cooling slot with runtime awareness. Given the
    /// type-classified candidate (the configured bare "fan"/role, or empty),
    /// prefer it once it has run (sticky); otherwise promote a commandable named
    /// fan that has run (stale-"fan" printers); otherwise keep the canonical
    /// candidate. "Has run" = ever_ran || currently spinning.
    std::string resolve_part_fan(const std::string& configured) const;

    /// Resolve the aux slot, preferring a user-commandable fan (fan_generic /
    /// output_pin) over an auto-controlled one (controller_fan / temperature_fan)
    /// — the commandable fan is the one worth glancing at in the compact row.
    /// Excludes the already-assigned part and hotend fans (#1124).
    std::string resolve_aux_fan(const std::string& part, const std::string& hotend) const;

    /// Recompute classify_primary_fans() and bump primary_fans_version_ if the
    /// resolved roles changed. Called on fan start/stop so the compact row tracks
    /// the live part fan.
    void refresh_primary_fans_selection();

    /// Get role-based display name override, or empty string if none
    std::string get_role_display_name(const std::string& object_name) const;

    /// Normalize a raw Moonraker `speed` (0.0-1.0) by the fan's configured
    /// max_power so a full-on fan reads 1.0. Klipper reports last_fan_value =
    /// value * max_power; dividing it back out recovers the logical fraction,
    /// matching Mainsail. Unknown fans and max_power<=0 default to 1.0 (no
    /// scaling). Result is clamped to [0.0, 1.0].
    double normalize_speed(const std::string& object_name, double raw_speed) const;

    /// Disambiguate fans sharing the "chamber_fan" suffix by role: a HEATER_FAN
    /// becomes "Chamber Heater Fan" and a TEMPERATURE_FAN "Chamber Cooling Fan".
    /// Returns base_name unchanged for any other object or type.
    std::string disambiguate_chamber_fan_name(const std::string& object_name, FanType type,
                                              const std::string& base_name) const;

    /// The name a fan shows: its custom name when the user set one, else its
    /// role or generated name translated into the current language.
    std::string resolve_display_name(const std::string& object_name, FanType type) const;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Static fan subjects
    lv_subject_t fan_speed_{};            ///< Main part-cooling fan, 0-100%
    lv_subject_t fans_version_{};         ///< Increments on fan list (structural) changes
    lv_subject_t primary_fans_version_{}; ///< Increments on primary-role reassignment (#1124)

    /// Object names exactly as discovery handed them over — before the bare-[fan]
    /// shadowing rule filters them into fans_. apply_roles() re-runs that filter,
    /// which needs the unfiltered list: once [fan] is shadowed it is gone from
    /// fans_, so nothing else remembers it existed to un-shadow it later.
    std::vector<std::string> discovered_objects_;

    /// Last resolved primary roles — compared in refresh_primary_fans_selection()
    /// so primary_fans_version_ only ticks on an actual change.
    PrimaryFans primary_fans_cache_;

    // Dynamic per-fan subjects (unique_ptr prevents invalidation on rehash)
    std::unordered_map<std::string, std::unique_ptr<lv_subject_t>> fan_speed_subjects_;
    // Lifetime tokens for dynamic fan subjects — destroyed when subject is deinited,
    // expiring weak_ptrs in ObserverGuards to prevent use-after-free.
    std::unordered_map<std::string, SubjectLifetime> fan_speed_lifetimes_;

    // Fan metadata
    std::vector<FanInfo> fans_;

    // Per-fan `max_power` from configfile.settings, keyed by lowercased object
    // name. Used to normalize reported speeds (see normalize_speed()).
    std::unordered_map<std::string, double> fan_max_power_;

    // Configured fan roles from wizard config
    FanRoleConfig roles_;
    /// Maps configured fan object names to role display names
    std::unordered_map<std::string, std::string> role_display_names_;
};

} // namespace helix
