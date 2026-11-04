// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe_sensor_manager.h"

#include "core/text_io.h"
#include "printer_discovery.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <string_view>

namespace helix::sensors {

namespace {

constexpr uint8_t kZProbe = static_cast<uint8_t>(ProbeSensorRole::Z_PROBE);

ProbeSensorType type_of(const SensorEntry& entry) {
    return static_cast<ProbeSensorType>(entry.type);
}

// Where each probe type publishes the keys this manager reads, per upstream
// source (table in docs/devel/SENSOR_MANAGEMENT.md). The Cartographer plugin
// nests last_z_result per mode on its own object; the flat keys live on the
// probe object it registers.
std::string status_object(ProbeSensorType type, const std::string& klipper_name) {
    return type == ProbeSensorType::CARTOGRAPHER ? "probe" : klipper_name;
}

// last_query is the result of the last QUERY_PROBE. Beacon publishes none, and
// probe_eddy_current never sets it because it rejects QUERY_PROBE.
bool publishes_last_query(ProbeSensorType type) {
    return type != ProbeSensorType::BEACON && type != ProbeSensorType::EDDY_CURRENT;
}

/// The status fields a probe is subscribed to; last_query first, for the types
/// that publish it.
constexpr const char* kProbeStatusFields[] = {"last_query", "last_z_result", "z_offset"};

// One physical probe can register several objects: Klipper's probe modules,
// Beacon and Cartographer all also register the generic probe object. Keep the
// most specific object only, so a single probe reads as one sensor and is
// subscribed once. An eddy object beside a named scanner is dropped too.
// note() every type of the set first, then ask alias() of each.
struct AliasRule {
    bool has_specific = false;
    bool has_named_scanner = false;

    void note(ProbeSensorType t) {
        has_specific |= t != ProbeSensorType::STANDARD;
        has_named_scanner |= t == ProbeSensorType::CARTOGRAPHER || t == ProbeSensorType::BEACON;
    }
    [[nodiscard]] bool alias(ProbeSensorType t) const {
        return (has_specific && t == ProbeSensorType::STANDARD) ||
               (has_named_scanner && t == ProbeSensorType::EDDY_CURRENT);
    }
};

// Klicky probes register as a plain [probe] and are told apart by their
// deploy/dock macro pair.
bool has_klicky_macros(const std::vector<std::string>& klipper_objects) {
    constexpr std::string_view kMacroPrefix = "gcode_macro ";
    constexpr std::string_view kMacros[] = {"ATTACH_PROBE", "DOCK_PROBE", "_Probe_Deploy",
                                            "_Probe_Stow"};
    bool found[4] = {};
    for (const auto& obj : klipper_objects) {
        const std::string_view name(obj);
        if (name.substr(0, kMacroPrefix.size()) != kMacroPrefix) {
            continue;
        }
        const std::string_view macro = name.substr(kMacroPrefix.size());
        for (size_t i = 0; i < 4; ++i) {
            found[i] = found[i] || macro == kMacros[i];
        }
    }
    return (found[0] && found[1]) || (found[2] && found[3]);
}

// Config values are strings in configfile.config.
std::optional<float> configured_z_offset(const nlohmann::json& section) {
    const auto it = section.find("z_offset");
    if (it == section.end()) {
        return std::nullopt;
    }
    if (it->is_string()) {
        return helix::text_io::parse_leading<float>(it->get<std::string>());
    }
    if (it->is_number()) {
        return it->get<float>();
    }
    return std::nullopt;
}

} // namespace

ProbeSensorManager::Options ProbeSensorManager::make_options() {
    Options o;
    o.name = "ProbeSensorManager";
    o.kinds = {hw::ObjectKind::Probe};
    o.count_subject = "probe_count";
    o.revision_subject = "probe_sensor_revision";
    o.config_key = "probe_sensors";
    // z_offset seeding and the Klicky check read config sections.
    o.rediscover_on_config = true;
    o.settings_section = "probe";
    o.row_toggle = RowToggle::None;
    for (const auto& r : kProbeSensorRoles) {
        if (r.value == ProbeSensorRole::NONE) {
            continue;
        }
        // Probes are assigned by the wizard; Settings shows the role as text.
        o.roles.push_back({static_cast<uint8_t>(r.value), r.id, r.display, /*exclusive=*/true,
                           /*offered=*/false});
    }
    for (const auto& t : kProbeSensorTypes) {
        o.types.push_back({static_cast<uint8_t>(t.value), t.id, t.display});
    }
    return o;
}

ProbeSensorManager& ProbeSensorManager::instance() {
    static ProbeSensorManager instance;
    return instance;
}

ProbeSensorManager::ProbeSensorManager() : SensorManager(make_options()) {}

// ============================================================================
// Discovery
// ============================================================================

void ProbeSensorManager::on_entries_changed() {
    prune_vanished_locked(readings_);
}

namespace {

/// Config-only probe sections folded into the probe object they describe.
struct FoldedSections {
    std::string probe;                 ///< the probe object that took them
    std::vector<std::string> sections; ///< load_cell_probe, dockable_probe, ...
};

// A config-only probe section (load_cell_probe, dockable_probe) describes the
// generic probe object its module registers. Beside a probe object it refines
// that probe's type and supplies its z_offset instead of adding a second
// sensor; with no probe object it is the sensor.
FoldedSections fold_config_sections(std::vector<SensorEntry>& entries,
                                    const std::vector<std::string>& objects) {
    const auto is_object = [&objects](const SensorEntry& e) {
        return std::find(objects.begin(), objects.end(), e.klipper_name) != objects.end();
    };
    auto target = std::find_if(entries.begin(), entries.end(), [&](const SensorEntry& e) {
        return e.klipper_name == "probe" && is_object(e);
    });
    if (target == entries.end()) {
        target = std::find_if(entries.begin(), entries.end(), is_object);
    }
    FoldedSections folded;
    if (target == entries.end()) {
        return folded;
    }
    folded.probe = target->klipper_name;
    for (const auto& entry : entries) {
        if (is_object(entry)) {
            continue;
        }
        folded.sections.push_back(entry.klipper_name);
        if (type_of(*target) == ProbeSensorType::STANDARD &&
            type_of(entry) != ProbeSensorType::STANDARD) {
            target->type = entry.type;
        }
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const SensorEntry& e) { return !is_object(e); }),
                  entries.end());
    return folded;
}

void drop_alias_objects(std::vector<SensorEntry>& entries) {
    AliasRule rule;
    for (const auto& entry : entries) {
        rule.note(type_of(entry));
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&rule](const SensorEntry& e) { return rule.alias(type_of(e)); }),
                  entries.end());
}

// Klicky probes register as a plain [probe]; their deploy/dock macros tell them apart.
void upgrade_klicky(std::vector<SensorEntry>& entries, const PrinterDiscovery& hw) {
    if (!has_klicky_macros(hw.printer_objects())) {
        return;
    }
    for (auto& entry : entries) {
        if (type_of(entry) == ProbeSensorType::STANDARD) {
            spdlog::debug("[ProbeSensorManager] Upgrading standard probe '{}' to KLICKY",
                          entry.display_name);
            entry.type = static_cast<uint8_t>(ProbeSensorType::KLICKY);
        }
    }
}

/// The configured z_offset of @p name, or of a section folded into it.
std::optional<float> seeded_z_offset(const std::string& name, const FoldedSections& folded,
                                     const PrinterDiscovery& hw) {
    const auto from_section = [&hw](const std::string& section_name) -> std::optional<float> {
        const nlohmann::json* section = hw.section(section_name);
        if (section != nullptr && section->is_object()) {
            return configured_z_offset(*section);
        }
        return std::nullopt;
    };
    if (const auto z_offset = from_section(name)) {
        return z_offset;
    }
    if (name == folded.probe) {
        for (const auto& section_name : folded.sections) {
            if (const auto z_offset = from_section(section_name)) {
                return z_offset;
            }
        }
    }
    return std::nullopt;
}

} // namespace

void ProbeSensorManager::on_discovered(const PrinterDiscovery& hw) {
    auto& entries = entries_locked();
    const FoldedSections folded =
        fold_config_sections(entries, hw.objects_of({hw::ObjectKind::Probe}));
    drop_alias_objects(entries);
    upgrade_klicky(entries, hw);
    apply_type_override_locked();
    on_entries_changed();

    // Seed z_offset from the configfile for probes whose runtime status reports
    // null (flashforge_loadcell); the configfile always has the persisted value.
    for (const auto& entry : entries) {
        if (const auto z_offset = seeded_z_offset(entry.klipper_name, folded, hw)) {
            readings_[entry.klipper_name].z_offset = *z_offset;
            spdlog::debug("[ProbeSensorManager] Seeded z_offset={:.3f}mm from config for {}",
                          *z_offset, entry.display_name);
        }
    }

    // A single probe is the Z probe. A saved role, applied by load_settings()
    // afterwards, overrides this.
    const bool held = std::any_of(entries.begin(), entries.end(),
                                  [](const SensorEntry& e) { return e.role == kZProbe; });
    if (entries.size() == 1 && !held) {
        entries.front().role = kZProbe;
        spdlog::info("[ProbeSensorManager] Auto-assigned Z_PROBE role to '{}'",
                     entries.front().display_name);
    }
    spdlog::info("[ProbeSensorManager] Discovered {} probe sensors", entries.size());
}

nlohmann::json ProbeSensorManager::extra_subscriptions(const PrinterDiscovery& hw) const {
    const auto names = hw.objects_of({hw::ObjectKind::Probe});
    std::vector<ProbeSensorType> types;
    types.reserve(names.size());
    AliasRule rule;
    for (const auto& name : names) {
        types.push_back(static_cast<ProbeSensorType>(hw::classify_object(name).chip));
        rule.note(types.back());
    }

    nlohmann::json objects = nlohmann::json::object();
    for (size_t i = 0; i < names.size(); ++i) {
        if (rule.alias(types[i])) {
            continue;
        }
        nlohmann::json fields = nlohmann::json::array();
        const size_t first = publishes_last_query(types[i]) ? 0 : 1;
        for (size_t f = first; f < std::size(kProbeStatusFields); ++f) {
            fields.push_back(nlohmann::json(kProbeStatusFields[f]));
        }
        objects[status_object(types[i], names[i])] = std::move(fields);
    }
    return objects;
}

// ============================================================================
// Status and subjects
// ============================================================================

bool ProbeSensorManager::parse_status(const nlohmann::json& status) {
    bool changed = false;
    for (const auto& entry : entries_locked()) {
        const std::string key = status_object(type_of(entry), entry.klipper_name);
        const auto sensor = status.find(key);
        if (sensor == status.end() || !sensor->is_object()) {
            continue;
        }
        ProbeSensorState& state = readings_[entry.klipper_name];
        const ProbeSensorState before = state;

        // A null or absent field keeps the previous value. Klipper answers a
        // requested key its module lacks with null, so the configfile-seeded
        // z_offset survives on mainline, where no probe module publishes it.
        // The Creality K1/K2 and QIDI forks do, and K1's Z_OFFSET_APPLY_PROBE
        // changes it live.
        const auto z = sensor->find("last_z_result");
        if (z != sensor->end() && z->is_number()) {
            state.last_z_result = z->get<float>();
        }
        const auto offset = sensor->find("z_offset");
        if (offset != sensor->end() && offset->is_number()) {
            state.z_offset = offset->get<float>();
        }
        // Klipper publishes a bool, Cartographer an int.
        const auto query = sensor->find("last_query");
        if (query != sensor->end() && (query->is_boolean() || query->is_number())) {
            state.triggered = query->is_boolean() ? query->get<bool>() : query->get<int>() != 0;
        }

        if (state.last_z_result != before.last_z_result || state.triggered != before.triggered ||
            state.z_offset != before.z_offset) {
            changed = true;
            spdlog::debug("[ProbeSensorManager] Sensor {} updated: last_z_result={:.3f}mm, "
                          "last_query={}, z_offset={:.3f}mm",
                          entry.display_name, state.last_z_result, state.triggered, state.z_offset);
        }
    }
    return changed;
}

const ProbeSensorState* ProbeSensorManager::z_probe_reading_locked() const {
    static const ProbeSensorState kNoReading;
    for (const auto& entry : entries_locked()) {
        if (entry.role != kZProbe || !entry.enabled || !entry.available) {
            continue;
        }
        const auto reading = readings_.find(entry.klipper_name);
        return reading != readings_.end() ? &reading->second : &kNoReading;
    }
    return nullptr;
}

void ProbeSensorManager::register_subjects(SubjectManager& subjects) {
    // -1 = no sensor assigned
    UI_MANAGED_SUBJECT_INT(z_offset_, -1, "probe_z_offset", subjects);
}

void ProbeSensorManager::refresh_subjects() {
    int z_offset = -1;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        if (const auto* reading = z_probe_reading_locked()) {
            z_offset = static_cast<int>(reading->z_offset * 1000.0f);
        }
    }
    lv_subject_set_int(&z_offset_, z_offset);
    spdlog::trace("[ProbeSensorManager] z_offset={}", z_offset);
}

// ============================================================================
// Reads and edits
// ============================================================================

std::vector<ProbeSensorConfig> ProbeSensorManager::get_sensors() const {
    std::vector<ProbeSensorConfig> out;
    for (const auto& entry : entries()) {
        ProbeSensorConfig config(entry.klipper_name, entry.display_name, type_of(entry));
        config.role = role_of<ProbeSensorRole>(entry);
        config.enabled = entry.enabled;
        out.push_back(std::move(config));
    }
    return out;
}

float ProbeSensorManager::get_z_offset() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    const auto* reading = z_probe_reading_locked();
    return reading ? reading->z_offset : 0.0f;
}

lv_subject_t* ProbeSensorManager::get_probe_z_offset_subject() {
    return &z_offset_;
}

bool ProbeSensorManager::apply_type_override_locked() {
    if (type_override_ == ProbeSensorType::STANDARD) {
        return false;
    }
    for (auto& entry : entries_locked()) {
        if (type_of(entry) == ProbeSensorType::STANDARD) {
            spdlog::info("[ProbeSensorManager] Overriding probe type from STANDARD to {} "
                         "(printer database)",
                         probe_type_to_string(type_override_));
            entry.type = static_cast<uint8_t>(type_override_);
            return true;
        }
    }
    return false;
}

void ProbeSensorManager::set_probe_type_override(ProbeSensorType type) {
    bool changed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        type_override_ = type;
        changed = apply_type_override_locked();
    }
    if (changed) {
        publish();
    } else {
        spdlog::debug("[ProbeSensorManager] No STANDARD probe to override (may already be typed)");
    }
}

} // namespace helix::sensors
