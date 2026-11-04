// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "hardware_object_table.h"
#include "json_fwd.h"
#include "lvgl.h"
#include "subject_managed_panel.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace helix {
class Config;
class PrinterDiscovery;
} // namespace helix

namespace helix::sensors {

/// One discovered sensor, as every manager sees it.
struct SensorEntry {
    std::string klipper_name; ///< "temperature_sensor chamber"
    std::string display_name; ///< "chamber"
    uint8_t type = 0;         ///< index into the manager's TypeSpec table
    uint8_t role = 0;         ///< the manager's role enum value; 0 is always "none"
    bool enabled = true;
    bool available = false;
};

/// One role a manager has.
struct RoleSpec {
    uint8_t id;              ///< the manager's role enum value
    const char* config_name; ///< persisted string, unchanged from today
    const char* label_key;   ///< translation key for the Settings dropdown
    bool exclusive;          ///< at most one holder
    bool offered;            ///< listed as a dropdown option
};

/// One sensor type a manager knows.
struct TypeSpec {
    uint8_t id;
    const char* config_name; ///< persisted "type" string, unchanged from today
    const char* label_key;
};

enum class RowToggle : uint8_t { None, Enabled, Invert };

/// What one Settings row shows. Plain data: no LVGL.
struct SensorRowView {
    std::string name;
    std::string detail;    ///< type label or a static reading
    std::string state;     ///< live state text; empty hides it
    bool alert = false;    ///< state drawn in the warning style
    uint8_t role = 0;      ///< the entry's role id
    size_t role_count = 0; ///< offered roles (the section's XML role_options); 0 hides the dropdown
    int selected_role = -1;      ///< index into the offered roles, -1 when the role is not offered
    std::string held_role_label; ///< shown when selected_role is -1 and a role is held
    RowToggle toggle = RowToggle::None;
    bool toggle_value = false;
};

/**
 * @brief The base of every sensor manager.
 *
 * Owns the sensor list, the lifecycle, threading, sync mode, the count and
 * revision subjects, reconcile against discovery, role assignment, settings
 * persistence and the Settings row model. A derived manager holds only what is
 * its own: its typed readings, its subjects and its domain rules.
 *
 * Threading: discover(), load_settings(), the setters and every subject touch
 * run on the main thread. update_from_status() may run on any thread; it
 * parses under the mutex and defers refresh_subjects() to the main thread.
 * refresh_subjects() runs WITHOUT the mutex: it copies what it needs under a
 * short lock, then sets subjects, because observers can call back into
 * managers and AmsState.
 */
class SensorManager {
  public:
    virtual ~SensorManager();
    SensorManager(const SensorManager&) = delete;
    SensorManager& operator=(const SensorManager&) = delete;

    [[nodiscard]] const char* name() const;

    void init_subjects();
    void deinit_subjects();
    [[nodiscard]] SubjectLifetime subjects_lifetime() const;
    void set_sync_mode(bool enabled);

    /// Reconcile against hw.objects_of(kinds) and hw's classified config
    /// sections, then on_discovered(). No-op when the manager is not fed by
    /// PrinterDiscovery.
    void discover(const PrinterDiscovery& hw);

    /// Whether discover() runs again on the configfile pass (Options::rediscover_on_config).
    [[nodiscard]] bool rediscovers_on_config() const;

    /// Default: lock, parse_status(), unlock, publish() if it changed anything.
    virtual void update_from_status(const nlohmann::json& status);

    /// Subscriptions this manager owns beyond hw::status_fields() of its kinds, in
    /// printer.objects.subscribe shape: {object: [fields...] or null}. Probe owns
    /// every probe subscription here (per-type object and fields).
    [[nodiscard]] virtual nlohmann::json extra_subscriptions(const PrinterDiscovery& hw) const;

    void load_settings(); ///< no-op when the manager has no config key
    void save_settings();

    /// klipper_name of every entry in this manager's persisted settings, whether
    /// or not discovery lists it now. Empty when the manager has no config key
    /// or nothing is saved. Reads like load_settings(): malformed entries are skipped.
    [[nodiscard]] std::vector<std::string> saved_sensor_names() const;

    [[nodiscard]] size_t sensor_count() const;
    [[nodiscard]] std::vector<SensorEntry> entries() const;
    [[nodiscard]] bool find(std::string_view klipper_name, SensorEntry& out) const;

    /// Exclusive roles are taken from every other holder.
    virtual void set_role(std::string_view klipper_name, uint8_t role);
    void set_enabled(std::string_view klipper_name, bool enabled);
    virtual void set_option(std::string_view klipper_name, bool value);

    /// Role ids in dropdown order (offered roles only).
    [[nodiscard]] std::vector<uint8_t> offered_roles() const;
    [[nodiscard]] virtual SensorRowView row(size_t index) const;

    [[nodiscard]] lv_subject_t* count_subject();
    [[nodiscard]] lv_subject_t* revision_subject(); ///< bumped on every publish()
    [[nodiscard]] bool subjects_initialized() const;

    /// Slug of this manager's Settings > Sensors section, nullptr when it has none.
    [[nodiscard]] const char* settings_section() const;
    /// Whether Settings lists @p entry. Default: every entry.
    [[nodiscard]] virtual bool listed_in_settings(const SensorEntry& entry) const;
    /// Whether row() shows live readings (Options::rows_show_readings).
    [[nodiscard]] bool rows_show_readings() const;
    /// Indices into entries() of the rows Settings lists, in display order.
    /// Default: every listed_in_settings() entry, in discovery order.
    [[nodiscard]] virtual std::vector<size_t> settings_rows() const;

  protected:
    struct Options {
        const char* name;
        std::vector<hw::ObjectKind> kinds;
        const char* count_subject;
        const char* revision_subject;
        const char* config_key = nullptr; ///< nullptr: not persisted
        std::vector<RoleSpec> roles;
        std::vector<TypeSpec> types;
        bool from_printer_discovery = true;
        /// Reads config sections in on_discovered(), so discover() runs again
        /// once configfile.config arrives. Off for every other manager: a
        /// second discover() repeats its per-connect resets.
        bool rediscover_on_config = false;
        /// Settings > Sensors section slug: the row subjects are
        /// sensor_<slug>_row_<field>_<i>. nullptr: no section.
        const char* settings_section = nullptr;
        RowToggle row_toggle = RowToggle::Enabled; ///< the toggle row() offers
        /// row() shows live readings. false: rows change only with the entries,
        /// so Settings skips a refill when a status update left them alone.
        bool rows_show_readings = true;
    };
    explicit SensorManager(Options options);

    virtual void register_subjects(SubjectManager& subjects) = 0; ///< main thread
    virtual void refresh_subjects() = 0;                          ///< main thread, mutex NOT held
    virtual bool parse_status(const nlohmann::json& status) = 0;  ///< mutex held
    virtual void on_discovered(const PrinterDiscovery& /*hw*/) {} ///< mutex held; auto roles
    virtual void on_entries_changed() {} ///< mutex held; resize parallel readings
    virtual void read_entry_extra(SensorEntry& /*entry*/, const nlohmann::json& /*saved*/) {}
    virtual void write_entry_extra(const SensorEntry& /*entry*/, nlohmann::json& /*out*/) const {}
    virtual void read_extra_settings(const nlohmann::json& /*saved*/) {}
    virtual void write_extra_settings(nlohmann::json& /*out*/) const {}
    /// Mutex held, lifetime already invalidated: tear down subjects kept outside
    /// the SubjectManager (a DynamicSubjectMap's clear()).
    virtual void on_deinit_subjects() {}

    /// Sync mode: refresh now. Otherwise defer refresh_subjects() through the
    /// lifetime token; dropped once deinit_subjects() has run.
    void publish();

    /// Replace the entries with `discovered`, keeping roles/enabled of names
    /// already known only through load_settings(). For non-PrinterDiscovery
    /// managers (SensorState).
    void reconcile(std::vector<SensorEntry> discovered);

    [[nodiscard]] std::recursive_mutex& mutex() const;
    [[nodiscard]] std::vector<SensorEntry>& entries_locked();
    [[nodiscard]] const std::vector<SensorEntry>& entries_locked() const;
    [[nodiscard]] const Options& options() const;

    /// Mutex held: erase every key of @p by_name (a reading map keyed by
    /// klipper_name) that no entry has, so a vanished sensor's reading cannot
    /// come back with a later sensor of that name.
    template <typename Map> void prune_vanished_locked(Map& by_name) const {
        for (auto it = by_name.begin(); it != by_name.end();) {
            const bool listed =
                std::any_of(entries_.begin(), entries_.end(),
                            [&](const SensorEntry& e) { return e.klipper_name == it->first; });
            it = listed ? std::next(it) : by_name.erase(it);
        }
    }

  private:
    friend class SensorManagerTestAccess;

    /// Main thread: sync the count, bump the revision, then refresh_subjects().
    void refresh_on_main_thread();
    /// Mutex held: swap in `found`, carrying role/enabled over from known names.
    void replace_entries_locked(std::vector<SensorEntry> found);
    /// Mutex held: the entry named @p klipper_name, or nullptr.
    [[nodiscard]] SensorEntry* find_locked(std::string_view klipper_name);
    [[nodiscard]] const SensorEntry* find_locked(std::string_view klipper_name) const;
    /// This manager's key under the active printer in @p config.
    [[nodiscard]] std::string settings_path(Config& config) const;
    [[nodiscard]] const nlohmann::json* saved_settings(Config& config) const;
    [[nodiscard]] const RoleSpec* role_spec(uint8_t id) const;
    [[nodiscard]] const char* role_config_name(uint8_t id) const;
    [[nodiscard]] uint8_t role_from_config_name(std::string_view name) const;
    [[nodiscard]] const TypeSpec* type_spec(uint8_t id) const;
    [[nodiscard]] nlohmann::json settings_json_locked() const;
    void apply_settings_locked(const nlohmann::json& saved);

    Options options_;
    mutable std::recursive_mutex mutex_;
    std::vector<SensorEntry> entries_;
    SubjectManager subjects_;
    lv_subject_t count_{};
    lv_subject_t revision_{};
    bool subjects_initialized_ = false;
    bool sync_mode_ = false;
    bool deinit_registered_ = false;
    helix::AsyncLifetimeGuard lifetime_;
};

/// Every sensor manager, in a stable order. The one list.
[[nodiscard]] const std::vector<SensorManager*>& sensor_managers();

/// The configfile pass: discover() then load_settings() again on every manager
/// that reads config sections, against discovery data that now carries them.
/// Main thread.
void rediscover_from_config(const PrinterDiscovery& hw);

/// Keeps a manager's role enum readable at the boundary.
template <typename Role> [[nodiscard]] Role role_of(const SensorEntry& entry) {
    return static_cast<Role>(entry.role);
}

} // namespace helix::sensors
