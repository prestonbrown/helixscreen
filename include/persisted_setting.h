// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/subject.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace helix::settings {

/// PerPrinter paths are relative to Config::df(); Global paths are absolute.
enum class Scope : uint8_t { Global, PerPrinter };

/// One int or bool setting that lives in a subject and in settings.json.
/// A bool is stored as a JSON boolean and held as 0/1; an int is clamped to
/// [min, max] on load and on set.
struct PersistedSetting {
    const char* xml_name;
    const char* json_path;
    Scope scope;
    bool is_bool;
    int def, min, max;
    /// Stable snake_case key, unique across all managers, reported to
    /// TelemetryManager::notify_setting_changed() on set. Only int and bool
    /// values are ever sent. nullptr for a setting that is not reported.
    const char* telemetry_key;
};

/// A read-only view of a manager's static table, for tests and diagnostics.
struct TableView {
    const PersistedSetting* rows;
    size_t size;
    const PersistedSetting* begin() const {
        return rows;
    }
    const PersistedSetting* end() const {
        return rows + size;
    }
};

namespace detail {
void init_setting(const PersistedSetting& s, Subject& subject, SubjectManager& subjects);
void reload_setting(const PersistedSetting& s, Subject& subject);
int get_setting(const PersistedSetting& s, const Subject& subject);
void set_setting(const PersistedSetting& s, Subject& subject, int value);
} // namespace detail

/// A fixed set of settings indexed by an enum whose values are the table's
/// row indices. Owns their subjects; the table itself is static storage.
template <typename Key, size_t N> class PersistedSettings {
  public:
    explicit constexpr PersistedSettings(const PersistedSetting (&table)[N]) : table_(table) {}

    /// Read config, clamp, init and register each subject.
    void init(SubjectManager& subjects) {
        for (size_t i = 0; i < N; ++i) {
            detail::init_setting(table_[i], subjects_[i], subjects);
        }
    }

    /// Re-read every initialized subject from Config, leaving its observers bound.
    void reload() {
        for (size_t i = 0; i < N; ++i) {
            detail::reload_setting(table_[i], subjects_[i]);
        }
    }

    /// The subject's value, or the row's default before init().
    int get(Key k) const {
        return detail::get_setting(table_[idx(k)], subjects_[idx(k)]);
    }
    bool get_bool(Key k) const {
        return get(k) != 0;
    }

    /// Clamp, update the subject, persist, and report telemetry if keyed.
    void set(Key k, int value) {
        detail::set_setting(table_[idx(k)], subjects_[idx(k)], value);
    }

    lv_subject_t* subject(Key k) {
        return &subjects_[idx(k)];
    }

  private:
    static constexpr size_t idx(Key k) {
        return static_cast<size_t>(k);
    }

    const PersistedSetting (&table_)[N];
    std::array<helix::Subject, N> subjects_{};
};

} // namespace helix::settings
