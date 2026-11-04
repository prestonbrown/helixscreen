// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_sensors.cpp
 * @brief Implementation of SensorSettingsOverlay and SensorRowSection
 */

#include "ui_settings_sensors.h"

#include "ui_callback_helpers.h"

#include "app_globals.h"
#include "chamber_assignment_options.h"
#include "filament_sensor_manager.h"
#include "input_sensor_manager.h"
#include "observer_factory.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace helix::settings {

using helix::ui::event_checked;
using helix::ui::event_selected;
using helix::ui::find_required;
using sensors::RowToggle;
using sensors::SensorManager;
using sensors::SensorRowView;

// ============================================================================
// SensorRowSection
// ============================================================================

namespace {

/// The subjects of one row, in sensor_row's sensor_<slug>_row_<field>_<i> naming.
enum RowField : uint8_t {
    kName,
    kDetail,
    kHasDetail,
    kState,
    kHasState,
    kAlert,
    kRole,
    kHasRole,
    kRoleSet,
    kHeldRole,
    kHasHeldRole,
    kToggleKind,
    kToggle,
    kHasNext,
    kRowFieldCount
};

constexpr size_t kNameLen = 96;
constexpr size_t kStateLen = 48;

struct RowFieldSpec {
    const char* name;
    int8_t initial;      ///< int fields only
    uint8_t buf_len;     ///< string fields: buffer size; 0 for an int field
    uint16_t buf_offset; ///< string fields: offset into Row::bufs
};

constexpr RowFieldSpec kRowFields[kRowFieldCount] = {
    {"name", 0, kNameLen, 0},   {"detail", 0, kNameLen, kNameLen},
    {"has_detail", 0, 0, 0},    {"state", 0, kStateLen, 2 * kNameLen},
    {"has_state", 0, 0, 0},     {"alert", 0, 0, 0},
    {"role", -1, 0, 0},         {"has_role", 0, 0, 0},
    {"role_set", 0, 0, 0},      {"held_role", 0, kStateLen, 2 * kNameLen + kStateLen},
    {"has_held_role", 0, 0, 0}, {"toggle_kind", 0, 0, 0},
    {"toggle", 0, 0, 0},        {"has_next", 0, 0, 0},
};

/// The order fill() writes a row's subjects in.
constexpr uint8_t kRowFillOrder[kRowFieldCount] = {
    kName,    kDetail, kHasDetail, kState,       kHasState,   kAlert,  kHasRole,
    kRoleSet, kRole,   kHeldRole,  kHasHeldRole, kToggleKind, kToggle, kHasNext};

} // namespace

struct SensorRowSection::Row {
    std::string ids[kRowFieldCount];
    lv_subject_t subjects[kRowFieldCount]{};
    char bufs[2 * kNameLen + 2 * kStateLen]{};
};

namespace {

std::vector<SensorRowSection*>& live_sections() {
    static std::vector<SensorRowSection*> sections;
    return sections;
}

void set_if_changed(lv_subject_t& subject, int value) {
    if (lv_subject_get_int(&subject) != value) {
        lv_subject_set_int(&subject, value);
    }
}

void set_if_changed(lv_subject_t& subject, const char* value) {
    if (std::strcmp(lv_subject_get_string(&subject), value) != 0) {
        lv_subject_copy_string(&subject, value);
    }
}

/// The section and row a sensor_row event belongs to: user_data is "<slug>:<row>".
SensorRowSection* section_for_event(lv_event_t* e, size_t& row) {
    const char* arg = static_cast<const char*>(lv_event_get_user_data(e));
    if (!arg) {
        return nullptr;
    }
    const std::string key(arg);
    const auto colon = key.rfind(':');
    if (colon == std::string::npos) {
        return nullptr;
    }
    row = static_cast<size_t>(std::atoi(key.c_str() + colon + 1));
    return SensorRowSection::find(key.substr(0, colon));
}

} // namespace

SensorRowSection::SensorRowSection(SensorManager& manager, SubjectManager& subjects)
    : manager_(&manager), subjects_(subjects), slug_(manager.settings_section()),
      count_id_("sensor_" + slug_ + "_row_count") {
    UI_MANAGED_SUBJECT_INT(count_, 0, count_id_.c_str(), subjects_);
    live_sections().push_back(this);
}

SensorRowSection::~SensorRowSection() {
    auto& sections = live_sections();
    sections.erase(std::remove(sections.begin(), sections.end(), this), sections.end());
}

SensorRowSection* SensorRowSection::find(const std::string& slug) {
    for (auto* section : live_sections()) {
        if (section->slug_ == slug) {
            return section;
        }
    }
    return nullptr;
}

void SensorRowSection::grow(size_t rows) {
    while (rows_.size() < rows) {
        auto row = std::make_unique<Row>();
        const std::string prefix = "sensor_" + slug_ + "_row_";
        const std::string suffix = "_" + std::to_string(rows_.size());
        for (size_t f = 0; f < kRowFieldCount; ++f) {
            const RowFieldSpec& spec = kRowFields[f];
            std::string& id = row->ids[f];
            id.reserve(prefix.size() + std::strlen(spec.name) + suffix.size());
            id.append(prefix).append(spec.name).append(suffix);
            lv_subject_t& subject = row->subjects[f];
            // UI_MANAGED_SUBJECT_INT/STRING, written once for every field.
            const bool is_string = spec.buf_len != 0;
            if (is_string) {
                lv_subject_init_string(&subject, row->bufs + spec.buf_offset, nullptr, spec.buf_len,
                                       "");
            } else {
                lv_subject_init_int(&subject, spec.initial);
            }
            helix::xml::register_subject_in_current_scope(id.c_str(), &subject);
            subjects_.register_subject(&subject, id.c_str());
            SubjectDebugRegistry::instance().register_subject(
                &subject, id.c_str(), is_string ? LV_SUBJECT_TYPE_STRING : LV_SUBJECT_TYPE_INT,
                __FILE__, __LINE__);
        }
        rows_.push_back(std::move(row));
    }
}

void SensorRowSection::fill(const SensorManager& manager) {
    const auto entries = manager.entries();
    filled_entries_ = entries;
    const std::vector<size_t> listed = manager.settings_rows();
    slots_.clear();
    for (const size_t i : listed) {
        slots_.push_back({entries[i].klipper_name, RowToggle::None});
    }

    // Grow before the count moves: the <repeat> rebuild it triggers binds by name.
    grow(listed.size());
    for (size_t k = 0; k < listed.size(); ++k) {
        const SensorRowView view = manager.row(listed[k]);
        Row& row = *rows_[k];
        slots_[k].toggle = view.toggle;
        // A role that is not offered selects unset_role_dropdown's trailing "-",
        // one past the offered roles, and a held one also shows as text.
        const bool role_set = view.selected_role >= 0;
        const bool held = !role_set && !view.held_role_label.empty();
        int ints[kRowFieldCount] = {};
        ints[kHasDetail] = view.detail.empty() ? 0 : 1;
        ints[kHasState] = view.state.empty() ? 0 : 1;
        ints[kAlert] = view.alert ? 1 : 0;
        ints[kHasRole] = view.role_count == 0 ? 0 : 1;
        ints[kRoleSet] = role_set ? 1 : 0;
        ints[kRole] = role_set ? view.selected_role : static_cast<int>(view.role_count);
        ints[kHasHeldRole] = held ? 1 : 0;
        ints[kToggleKind] = static_cast<int>(view.toggle);
        ints[kToggle] = view.toggle_value ? 1 : 0;
        ints[kHasNext] = k + 1 < listed.size() ? 1 : 0;
        const char* strings[kRowFieldCount] = {};
        strings[kName] = view.name.c_str();
        strings[kDetail] = view.detail.c_str();
        strings[kState] = view.state.c_str();
        strings[kHeldRole] = held ? view.held_role_label.c_str() : "";
        for (const uint8_t f : kRowFillOrder) {
            if (kRowFields[f].buf_len != 0) {
                set_if_changed(row.subjects[f], strings[f]);
            } else {
                set_if_changed(row.subjects[f], ints[f]);
            }
        }
    }
    set_if_changed(count_, static_cast<int>(listed.size()));
}

bool SensorRowSection::rows_may_have_changed() const {
    if (manager_->rows_show_readings()) {
        return true;
    }
    const auto now = manager_->entries();
    return !std::equal(now.begin(), now.end(), filled_entries_.begin(), filled_entries_.end(),
                       [](const sensors::SensorEntry& a, const sensors::SensorEntry& b) {
                           return a.klipper_name == b.klipper_name &&
                                  a.display_name == b.display_name && a.type == b.type &&
                                  a.role == b.role && a.enabled == b.enabled &&
                                  a.available == b.available;
                       });
}

void SensorRowSection::pick_role(size_t row, int option) {
    if (row >= slots_.size() || option < 0) {
        return;
    }
    const auto offered = manager_->offered_roles();
    if (static_cast<size_t>(option) >= offered.size()) {
        return;
    }
    const std::string name = slots_[row].klipper_name;
    manager_->set_role(name, offered[static_cast<size_t>(option)]);
    manager_->save_settings();
}

void SensorRowSection::set_toggle(size_t row, bool on) {
    if (row >= slots_.size()) {
        return;
    }
    const std::string name = slots_[row].klipper_name;
    switch (slots_[row].toggle) {
    case RowToggle::Enabled:
        manager_->set_enabled(name, on);
        break;
    case RowToggle::Invert:
        manager_->set_option(name, on);
        break;
    case RowToggle::None:
        return;
    }
    manager_->save_settings();
}

void SensorRowSection::watch(std::function<void()> after_fill) {
    after_fill_ = std::move(after_fill);
    if (!manager_->subjects_initialized()) {
        return;
    }
    revision_observer_ = helix::ui::observe<int>(
        manager_->revision_subject(), this,
        [](SensorRowSection* self, int /*revision*/) {
            if (!self->rows_may_have_changed()) {
                return;
            }
            self->fill(*self->manager_);
            if (self->after_fill_) {
                self->after_fill_();
            }
        },
        manager_->subjects_lifetime());
}

void SensorRowSection::stop() {
    revision_observer_.reset();
}

// ============================================================================
// SensorSettingsOverlay
// ============================================================================

SensorSettingsOverlay::~SensorSettingsOverlay() {
    for (auto& section : sections_) {
        section->stop();
    }
}

void SensorSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_sensor_row_role_changed",
         [](lv_event_t* e) {
             size_t row = 0;
             if (auto* section = section_for_event(e, row)) {
                 section->pick_role(row, event_selected(e));
             }
         }},
        {"on_sensor_row_toggle_changed",
         [](lv_event_t* e) {
             size_t row = 0;
             if (auto* section = section_for_event(e, row)) {
                 section->set_toggle(row, event_checked(e));
             }
         }},
        {"on_input_door_check_changed",
         [](lv_event_t* e) {
             auto& mgr = helix::sensors::InputSensorManager::instance();
             mgr.set_door_check_enabled(event_checked(e));
             mgr.save_settings();
         }},
        {"on_switch_master_toggle_changed",
         [](lv_event_t* e) {
             get_sensor_settings_overlay().handle_switch_master_toggle_changed(event_checked(e));
         }},
        {"on_chamber_heater_changed",
         [](lv_event_t* e) {
             const std::string value = chamber_assignment_for_index(
                 get_sensor_settings_overlay().chamber_heater_names_, event_selected(e));
             if (!value.empty()) {
                 helix::SettingsManager::instance().set_chamber_heater_assignment(value);
                 spdlog::info("[SensorSettings] Chamber heater assignment: {}", value);
             }
         }},
        {"on_chamber_sensor_changed",
         [](lv_event_t* e) {
             const std::string value = chamber_assignment_for_index(
                 get_sensor_settings_overlay().chamber_sensor_names_, event_selected(e));
             if (!value.empty()) {
                 helix::SettingsManager::instance().set_chamber_sensor_assignment(value);
                 spdlog::info("[SensorSettings] Chamber sensor assignment: {}", value);
             }
         }},
    });
}

void SensorSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(total_count_, 0, "sensor_total_count", subjects_);
        for (auto* manager : sensors::sensor_managers()) {
            if (manager->settings_section()) {
                sections_.push_back(std::make_unique<SensorRowSection>(*manager, subjects_));
            }
        }
    });
}

lv_obj_t* SensorSettingsOverlay::create(lv_obj_t* parent) {
    if (!are_subjects_initialized()) {
        init_subjects();
    }
    refill_all();
    lv_obj_t* root = OverlayBase::create(parent);
    if (root) {
        populate_chamber_assignment();
    }
    return root;
}

void SensorSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    if (!are_subjects_initialized()) {
        init_subjects();
    }
    for (auto& section : sections_) {
        section->watch([this]() { update_total(); });
    }
    refill_all();
    populate_chamber_assignment();
}

void SensorSettingsOverlay::on_deactivating(DeactivateReason /*reason*/) {
    for (auto& section : sections_) {
        section->stop();
    }
}

SensorRowSection* SensorSettingsOverlay::section(const std::string& slug) const {
    for (const auto& s : sections_) {
        if (s->slug() == slug) {
            return s.get();
        }
    }
    return nullptr;
}

void SensorSettingsOverlay::refill_all() {
    for (auto& section : sections_) {
        section->fill(section->manager());
    }
    update_total();
}

void SensorSettingsOverlay::update_total() {
    size_t total = 0;
    // Rows listed, not sensors discovered: a manager whose every sensor belongs
    // to a filament system lists none, and its section stays hidden.
    for (const auto& section : sections_) {
        total += section->row_count();
    }
    set_if_changed(total_count_, static_cast<int>(total));
}

// ============================================================================
// CHAMBER ASSIGNMENT
// ============================================================================

ChamberAssignmentLabels chamber_assignment_labels() {
    return ChamberAssignmentLabels{lv_tr("Auto"), lv_tr("(none detected)"), lv_tr("not detected"),
                                   lv_tr("None (disable)")};
}

void SensorSettingsOverlay::populate_chamber_assignment() {
    if (!overlay_root_)
        return;

    auto& settings = helix::SettingsManager::instance();
    auto& discovery = get_printer_state().get_discovery();

    // --- Chamber Heater Dropdown ---
    lv_obj_t* heater_dd = find_required(overlay_root_, "chamber_heater_dropdown", get_name());
    if (heater_dd) {
        std::vector<std::string> assignable;
        for (const auto& heater : discovery.heaters()) {
            // Skip bed and extruder heaters — only show generic heaters
            if (heater == "heater_bed" || heater.rfind("extruder", 0) == 0) {
                continue;
            }
            assignable.push_back(heater);
        }

        auto built = build_chamber_assignment_options(
            assignable, discovery.chamber_heater_name(), settings.get_chamber_heater_assignment(),
            "heater_generic ", chamber_assignment_labels());

        lv_dropdown_set_options(heater_dd, built.options.c_str());
        lv_dropdown_set_selected(heater_dd, built.selected);

        chamber_heater_names_ = std::move(built.names);
    }

    // --- Chamber Sensor Dropdown ---
    lv_obj_t* sensor_dd = find_required(overlay_root_, "chamber_sensor_dropdown", get_name());
    if (sensor_dd) {
        // A chamber heater measures its own chamber, so discovery leaves the
        // sensor pick empty and the heater is what Auto reads. Name it: a
        // chamber that has a reading must not report none detected. The bare
        // object name carries no "temperature_sensor " prefix to strip, so it
        // reaches the label as the user's own object name.
        const std::string& detected_sensor = discovery.chamber_sensor_name().empty()
                                                 ? discovery.chamber_heater_object_name()
                                                 : discovery.chamber_sensor_name();
        auto built = build_chamber_assignment_options(
            discovery.sensors(), detected_sensor, settings.get_chamber_sensor_assignment(),
            "temperature_sensor ", chamber_assignment_labels());

        lv_dropdown_set_options(sensor_dd, built.options.c_str());
        lv_dropdown_set_selected(sensor_dd, built.selected);

        chamber_sensor_names_ = std::move(built.names);
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SensorSettingsOverlay::handle_switch_master_toggle_changed(bool enabled) {
    auto& mgr = helix::FilamentSensorManager::instance();
    mgr.set_master_enabled(enabled);
    mgr.save_settings();
    spdlog::info("[{}] Switch sensor master enabled: {}", get_name(), enabled ? "ON" : "OFF");
}

} // namespace helix::settings
