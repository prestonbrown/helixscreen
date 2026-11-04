// SPDX-License-Identifier: GPL-3.0-or-later

#include "sensor_manager.h"

#include "accel_sensor_manager.h"
#include "config.h"
#include "core/static_subject_registry.h"
#include "filament_sensor_manager.h"
#include "humidity_sensor_manager.h"
#include "input_sensor_manager.h"
#include "json_utils.h"
#include "load_cell_manager.h"
#include "printer_discovery.h"
#include "probe_sensor_manager.h"
#include "sensor_state.h"
#include "spdlog/spdlog.h"
#include "temperature_sensor_manager.h"
#include "width_sensor_manager.h"

#include <algorithm>
#include <climits>

namespace helix::sensors {

namespace detail {
/// The registry's storage. Declared here and in the test access helper only:
/// production code reads sensor_managers().
std::vector<SensorManager*>& mutable_sensor_managers();
} // namespace detail

SensorManager::SensorManager(Options options) : options_(std::move(options)) {}

SensorManager::~SensorManager() {
    if (deinit_registered_ && !StaticSubjectRegistry::is_destroyed()) {
        StaticSubjectRegistry::instance().unregister(options_.name);
    }
}

const char* SensorManager::name() const {
    return options_.name;
}

// ============================================================================
// Lifecycle
// ============================================================================

void SensorManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    UI_MANAGED_SUBJECT_INT(count_, 0, options_.count_subject, subjects_);
    UI_MANAGED_SUBJECT_INT(revision_, 0, options_.revision_subject, subjects_);
    register_subjects(subjects_);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        subjects_initialized_ = true;
    }

    // Deinit runs before lv_deinit() and before a printer switch rebuilds state.
    StaticSubjectRegistry::instance().register_deinit(options_.name,
                                                      [this]() { deinit_subjects(); });
    deinit_registered_ = true;

    spdlog::trace("[{}] Subjects initialized", options_.name);
    publish();
}

void SensorManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // First: a refresh already queued must not land on the subjects freed below.
    lifetime_.invalidate();
    on_deinit_subjects();
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[{}] Subjects deinitialized", options_.name);
}

SubjectLifetime SensorManager::subjects_lifetime() const {
    return subjects_.get_subjects_lifetime();
}

void SensorManager::set_sync_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sync_mode_ = enabled;
}

bool SensorManager::subjects_initialized() const {
    return subjects_initialized_;
}

// ============================================================================
// Discovery and status
// ============================================================================

void SensorManager::discover(const PrinterDiscovery& hw) {
    if (!options_.from_printer_discovery) {
        return;
    }

    std::vector<SensorEntry> found;
    auto add = [&found](const std::string& klipper_name, const hw::ObjectClass& cls) {
        const bool known = std::any_of(found.begin(), found.end(), [&](const SensorEntry& e) {
            return e.klipper_name == klipper_name;
        });
        if (known) {
            return;
        }
        SensorEntry entry;
        entry.klipper_name = klipper_name;
        entry.display_name = cls.instance.empty() ? klipper_name : cls.instance;
        entry.type = cls.chip;
        entry.available = true;
        found.push_back(std::move(entry));
    };

    // Objects first, in printer.objects.list order; a config section adds a
    // sensor only when no object of that name exists (config-only kinds).
    for (const auto& name : hw.objects_of(options_.kinds)) {
        add(name, hw::classify_object(name));
    }
    static const nlohmann::json kEmptyBody = nlohmann::json::object();
    for (const auto& name : hw.sections_of(options_.kinds)) {
        const nlohmann::json* body = hw.section(name);
        add(name, hw::classify_section(name, body ? *body : kEmptyBody));
    }

    size_t count = 0;
    bool had_entries = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        had_entries = !entries_.empty();
        replace_entries_locked(std::move(found));
        on_discovered(hw);
        count = entries_.size();
    }
    spdlog::debug("[{}] Discovered {} sensors", options_.name, count);
    // A printer with none of this manager's sensors, discovered over a manager that
    // already tracked none, leaves every subject at the value it already holds.
    if (had_entries || count > 0) {
        publish();
    }
}

bool SensorManager::rediscovers_on_config() const {
    return options_.rediscover_on_config;
}

void SensorManager::reconcile(std::vector<SensorEntry> discovered) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        replace_entries_locked(std::move(discovered));
    }
    publish();
}

void SensorManager::replace_entries_locked(std::vector<SensorEntry> found) {
    for (auto& entry : found) {
        if (const SensorEntry* known = find_locked(entry.klipper_name)) {
            entry.role = known->role;
            entry.enabled = known->enabled;
        }
    }
    entries_ = std::move(found);
    on_entries_changed();
}

void SensorManager::update_from_status(const nlohmann::json& status) {
    bool changed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        changed = parse_status(status);
    }
    if (changed) {
        publish();
    }
}

nlohmann::json SensorManager::extra_subscriptions(const PrinterDiscovery& /*hw*/) const {
    return nlohmann::json::object();
}

// ============================================================================
// Publishing
// ============================================================================

void SensorManager::publish() {
    bool sync = false;
    bool initialized = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        sync = sync_mode_;
        initialized = subjects_initialized_;
    }
    // Without subjects there is nothing to refresh, and init_subjects() publishes.
    if (!sync && !initialized) {
        return;
    }
    if (sync) {
        refresh_on_main_thread();
        return;
    }
    // The token check runs on the main thread before the body, and
    // deinit_subjects() invalidates it, so `this` is live whenever the body runs.
    lifetime_.token().defer(options_.name, [this]() { refresh_on_main_thread(); });
}

void SensorManager::refresh_on_main_thread() {
    if (!subjects_initialized_) {
        return;
    }
    int count = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        count = static_cast<int>(entries_.size());
    }
    if (lv_subject_get_int(&count_) != count) {
        lv_subject_set_int(&count_, count);
    }
    const int revision = lv_subject_get_int(&revision_);
    lv_subject_set_int(&revision_, revision == INT_MAX ? 0 : revision + 1);
    refresh_subjects();
}

// ============================================================================
// Settings
// ============================================================================

const RoleSpec* SensorManager::role_spec(uint8_t id) const {
    for (const auto& spec : options_.roles) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

const char* SensorManager::role_config_name(uint8_t id) const {
    const RoleSpec* spec = role_spec(id);
    return spec ? spec->config_name : "none";
}

uint8_t SensorManager::role_from_config_name(std::string_view name) const {
    for (const auto& spec : options_.roles) {
        if (name == spec.config_name) {
            return spec.id;
        }
    }
    return 0;
}

const TypeSpec* SensorManager::type_spec(uint8_t id) const {
    for (const auto& spec : options_.types) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

nlohmann::json SensorManager::settings_json_locked() const {
    nlohmann::json sensors = nlohmann::json::array();
    for (const auto& entry : entries_) {
        nlohmann::json out;
        out["klipper_name"] = entry.klipper_name;
        out["role"] = role_config_name(entry.role);
        out["enabled"] = entry.enabled;
        if (const TypeSpec* spec = type_spec(entry.type)) {
            out["type"] = spec->config_name;
        } else if (!options_.types.empty()) {
            out["type"] = options_.types.front().config_name;
        }
        write_entry_extra(entry, out);
        sensors.push_back(std::move(out));
    }
    nlohmann::json out;
    out["sensors"] = std::move(sensors);
    write_extra_settings(out);
    return out;
}

namespace {
/// The klipper_name of one saved entry; empty when the entry is not an object
/// or the name is missing or not a string.
std::string saved_klipper_name(const nlohmann::json& item) {
    if (!item.is_object()) {
        return {};
    }
    const auto name = item.find("klipper_name");
    if (name == item.end() || !name->is_string()) {
        return {};
    }
    return json_util::as_string(*name);
}
} // namespace

void SensorManager::apply_settings_locked(const nlohmann::json& saved) {
    // Lenient: a wrong-typed field is skipped and the rest still applies. A role
    // that is present reads as none unless it names a role.
    if (const auto sensors = saved.find("sensors"); sensors != saved.end() && sensors->is_array()) {
        for (const auto& item : *sensors) {
            const std::string klipper_name = saved_klipper_name(item);
            if (klipper_name.empty()) {
                continue;
            }
            SensorEntry* entry = find_locked(klipper_name);
            if (entry == nullptr) {
                continue;
            }
            if (const auto role = item.find("role"); role != item.end()) {
                entry->role =
                    role->is_string() ? role_from_config_name(json_util::as_string(*role)) : 0;
            }
            if (const auto enabled = item.find("enabled");
                enabled != item.end() && enabled->is_boolean()) {
                entry->enabled = json_util::as_bool(*enabled, entry->enabled);
            }
            read_entry_extra(*entry, item);
        }
    }
    read_extra_settings(saved);
}

std::string SensorManager::settings_path(Config& config) const {
    return config.df() + options_.config_key;
}

const nlohmann::json* SensorManager::saved_settings(Config& config) const {
    return config.try_get_json(settings_path(config));
}

void SensorManager::load_settings() {
    if (options_.config_key == nullptr) {
        return;
    }
    Config* config = Config::get_instance();
    if (config == nullptr) {
        return;
    }
    static const nlohmann::json kNothingSaved = nlohmann::json::object();
    const nlohmann::json* saved = saved_settings(*config);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        // Nothing saved still reaches read_extra_settings, which restores its defaults.
        apply_settings_locked(saved && saved->is_object() ? *saved : kNothingSaved);
    }
    spdlog::debug("[{}] Settings loaded ({})", options_.name, saved ? "saved" : "defaults");
    publish();
}

std::vector<std::string> SensorManager::saved_sensor_names() const {
    std::vector<std::string> names;
    if (options_.config_key == nullptr) {
        return names;
    }
    Config* config = Config::get_instance();
    if (config == nullptr) {
        return names;
    }
    const nlohmann::json* saved = saved_settings(*config);
    if (saved == nullptr || !saved->is_object()) {
        return names;
    }
    const auto sensors = saved->find("sensors");
    if (sensors == saved->end() || !sensors->is_array()) {
        return names;
    }
    for (const auto& item : *sensors) {
        std::string klipper_name = saved_klipper_name(item);
        if (!klipper_name.empty()) {
            names.push_back(std::move(klipper_name));
        }
    }
    return names;
}

void SensorManager::save_settings() {
    if (options_.config_key == nullptr) {
        return;
    }
    Config* config = Config::get_instance();
    if (config == nullptr) {
        return;
    }
    nlohmann::json out;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        out = settings_json_locked();
    }
    config->get_json(settings_path(*config)) = std::move(out);
    config->save();
    spdlog::debug("[{}] Settings saved", options_.name);
}

// ============================================================================
// Reads
// ============================================================================

size_t SensorManager::sensor_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return entries_.size();
}

std::vector<SensorEntry> SensorManager::entries() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return entries_;
}

bool SensorManager::find(std::string_view klipper_name, SensorEntry& out) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const SensorEntry* entry = find_locked(klipper_name);
    if (entry == nullptr) {
        return false;
    }
    out = *entry;
    return true;
}

SensorEntry* SensorManager::find_locked(std::string_view klipper_name) {
    for (auto& entry : entries_) {
        if (entry.klipper_name == klipper_name) {
            return &entry;
        }
    }
    return nullptr;
}

const SensorEntry* SensorManager::find_locked(std::string_view klipper_name) const {
    return const_cast<SensorManager*>(this)->find_locked(klipper_name);
}

// ============================================================================
// Edits
// ============================================================================

void SensorManager::set_role(std::string_view klipper_name, uint8_t role) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        SensorEntry* entry = find_locked(klipper_name);
        if (entry == nullptr) {
            return;
        }
        const RoleSpec* spec = role_spec(role);
        if (role != 0 && spec != nullptr && spec->exclusive) {
            for (auto& other : entries_) {
                if (other.role == role && other.klipper_name != klipper_name) {
                    other.role = 0;
                }
            }
        }
        entry->role = role;
        spdlog::info("[{}] Set role for {} to {}", options_.name, entry->display_name,
                     role_config_name(role));
    }
    publish();
}

void SensorManager::set_enabled(std::string_view klipper_name, bool enabled) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        SensorEntry* entry = find_locked(klipper_name);
        if (entry == nullptr) {
            return;
        }
        entry->enabled = enabled;
        spdlog::info("[{}] Set enabled for {} to {}", options_.name, entry->display_name, enabled);
    }
    publish();
}

void SensorManager::set_option(std::string_view /*klipper_name*/, bool /*value*/) {}

// ============================================================================
// Settings rows
// ============================================================================

std::vector<uint8_t> SensorManager::offered_roles() const {
    std::vector<uint8_t> out;
    for (const auto& spec : options_.roles) {
        if (spec.offered) {
            out.push_back(spec.id);
        }
    }
    return out;
}

SensorRowView SensorManager::row(size_t index) const {
    SensorEntry entry;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (index >= entries_.size()) {
            return {};
        }
        entry = entries_[index];
    }

    SensorRowView view;
    view.name = entry.display_name;
    if (const TypeSpec* type = type_spec(entry.type)) {
        view.detail = lv_tr(type->label_key);
    }
    // An object whose name carries no instance ("bltouch", "adxl345") is named
    // by its type, with the Klipper name underneath.
    if (entry.display_name == entry.klipper_name && !view.detail.empty()) {
        view.name = view.detail;
        view.detail = entry.klipper_name;
    }
    view.role = entry.role;
    for (const auto& spec : options_.roles) {
        if (!spec.offered) {
            continue;
        }
        if (spec.id == entry.role) {
            view.selected_role = static_cast<int>(view.role_count);
        }
        ++view.role_count;
    }
    if (view.selected_role < 0 && entry.role != 0) {
        if (const RoleSpec* held = role_spec(entry.role)) {
            view.held_role_label = lv_tr(held->label_key);
        }
    }
    view.toggle = options_.row_toggle;
    view.toggle_value = view.toggle == RowToggle::Enabled && entry.enabled;
    return view;
}

// ============================================================================
// Accessors
// ============================================================================

lv_subject_t* SensorManager::count_subject() {
    return &count_;
}

lv_subject_t* SensorManager::revision_subject() {
    return &revision_;
}

const char* SensorManager::settings_section() const {
    return options_.settings_section;
}

bool SensorManager::listed_in_settings(const SensorEntry& /*entry*/) const {
    return true;
}

bool SensorManager::rows_show_readings() const {
    return options_.rows_show_readings;
}

std::vector<size_t> SensorManager::settings_rows() const {
    const auto all = entries();
    std::vector<size_t> rows;
    for (size_t i = 0; i < all.size(); ++i) {
        if (listed_in_settings(all[i])) {
            rows.push_back(i);
        }
    }
    return rows;
}

std::recursive_mutex& SensorManager::mutex() const {
    return mutex_;
}

std::vector<SensorEntry>& SensorManager::entries_locked() {
    return entries_;
}

const std::vector<SensorEntry>& SensorManager::entries_locked() const {
    return entries_;
}

const SensorManager::Options& SensorManager::options() const {
    return options_;
}

// ============================================================================
// Registry
// ============================================================================

std::vector<SensorManager*>& detail::mutable_sensor_managers() {
    static std::vector<SensorManager*> managers = {&FilamentSensorManager::instance(),
                                                   &WidthSensorManager::instance(),
                                                   &HumiditySensorManager::instance(),
                                                   &ProbeSensorManager::instance(),
                                                   &LoadCellManager::instance(),
                                                   &AccelSensorManager::instance(),
                                                   &InputSensorManager::instance(),
                                                   &TemperatureSensorManager::instance(),
                                                   &SensorState::instance()};
    return managers;
}

const std::vector<SensorManager*>& sensor_managers() {
    return detail::mutable_sensor_managers();
}

void rediscover_from_config(const PrinterDiscovery& hw) {
    for (auto* manager : sensor_managers()) {
        if (manager->rediscovers_on_config()) {
            // on_discovered() may auto-assign roles again; saved settings win, as
            // they do on the first pass.
            manager->discover(hw);
            manager->load_settings();
        }
    }
}

} // namespace helix::sensors
