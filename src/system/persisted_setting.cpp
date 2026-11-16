// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "persisted_setting.h"

#include "config.h"
#include "spdlog/spdlog.h"
#include "system/telemetry_manager.h"

#include <algorithm>
#include <string>

namespace helix::settings::detail {

namespace {
std::string path_of(const PersistedSetting& s, const Config& config) {
    return s.scope == Scope::PerPrinter ? config.df() + s.json_path : std::string(s.json_path);
}

int normalize(const PersistedSetting& s, int value) {
    return s.is_bool ? (value != 0 ? 1 : 0) : std::clamp(value, s.min, s.max);
}
} // namespace

namespace {
int load_value(const PersistedSetting& s) {
    const Config* config = Config::get_instance();
    const std::string path = path_of(s, *config);
    return s.is_bool ? (config->get<bool>(path, s.def != 0) ? 1 : 0)
                     : normalize(s, config->get<int>(path, s.def));
}
} // namespace

namespace {
/// True once the subject has held an int. A subject built before lv_init() has no node
/// until init(); a reset one keeps its node and its type.
bool holds_int(const Subject& subject) {
    const lv_subject_t* node = subject.get();
    return node != nullptr && node->type == LV_SUBJECT_TYPE_INT;
}
} // namespace

void init_setting(const PersistedSetting& s, Subject& subject, SubjectManager& subjects) {
    const int value = load_value(s);
    UI_MANAGED_SUBJECT_INT(subject, value, s.xml_name, subjects);
}

void reload_setting(const PersistedSetting& s, Subject& subject) {
    if (holds_int(subject)) {
        lv_subject_set_int(&subject, load_value(s));
    }
}

int get_setting(const PersistedSetting& s, const Subject& subject) {
    if (!holds_int(subject)) {
        return s.def;
    }
    return lv_subject_get_int(subject.get());
}

void set_setting(const PersistedSetting& s, Subject& subject, int value) {
    value = normalize(s, value);
    Config* config = Config::get_instance();
    const std::string path = path_of(s, *config);
    spdlog::debug("[Settings] {} = {}", path, value);

    const int old_value = lv_subject_get_int(&subject);
    lv_subject_set_int(&subject, value);

    if (s.is_bool) {
        config->set<bool>(path, value != 0);
    } else {
        config->set<int>(path, value);
    }
    config->save();

    if (s.telemetry_key != nullptr) {
        TelemetryManager::instance().notify_setting_changed(
            s.telemetry_key, std::to_string(old_value), std::to_string(value));
    }
}

} // namespace helix::settings::detail
