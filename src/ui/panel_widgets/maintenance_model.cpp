// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/maintenance_model.h"

#include "ui_modal.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "helix_psram_attr.h"
#include "lvgl.h"
#include "observe_language.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "subject_managed_panel.h"
#include "text_io.h"
#include "widget_subjects.h"

#if HELIX_HAS_PLUGINS
#include "lua_bindings.h"
#endif

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>

namespace helix::maintenance_model {
namespace {

using maintenance::kMeterCount;
using maintenance::Meter;
using maintenance::State;
using json = nlohmann::json;

/// The example plugin's id: its store file and settings block are named after it.
constexpr const char* kPluginId = "maintenance-meter";

/// Accrued print seconds held only in memory before they are saved: a crash loses at
/// most this.
constexpr int kSaveEverySeconds = 6 * 60;

struct MeterSubjects {
    lv_subject_t on{};
    lv_subject_t pct{};
    lv_subject_t status{};
    lv_subject_t interval{};
    lv_subject_t name{};
    lv_subject_t short_name{};
    lv_subject_t left{};
    lv_subject_t line{};
    lv_subject_t note{};
    char name_buf[64]{};
    char short_buf[32]{};
    char left_buf[24]{};
    char line_buf[48]{};
    char note_buf[96]{};
};

struct Subjects {
    lv_subject_t version{};
    lv_subject_t any_on{};
    lv_subject_t worst_pct{};
    lv_subject_t worst_status{};
    lv_subject_t worst_left{};
    lv_subject_t worst_name{};
    lv_subject_t total_text{};
    char worst_left_buf[24]{};
    char worst_name_buf[32]{};
    char total_text_buf[96]{};
    MeterSubjects meter[kMeterCount];
};
HELIX_PSRAM_BSS Subjects s_subj;
WidgetSubjects s_subjects{"MaintenanceModelSubjects"};

State s_state;
maintenance::DurationTracker s_tracker;
/// The printer section the state was loaded from; saves go back there even after the
/// active printer changes.
std::string s_prefix;
int s_unsaved_s = 0;
/// s_unsaved_s at which the next periodic save is due.
int s_save_at_s = kSaveEverySeconds;

std::vector<ObserverGuard> s_observers;
ObserverGuard s_language_observer;
// The observers' owner argument; the handlers use no instance state.
int s_anchor = 0;

// clang-format off
// Subject names are held by pointer, so they must outlive registration.
constexpr std::array<std::array<const char*, 9>, kMeterCount> kMeterSubjectNames = {{
    {"maintenance_m0_on", "maintenance_m0_pct", "maintenance_m0_status", "maintenance_m0_interval",
     "maintenance_m0_name", "maintenance_m0_short", "maintenance_m0_left", "maintenance_m0_line",
     "maintenance_m0_note"},
    {"maintenance_m1_on", "maintenance_m1_pct", "maintenance_m1_status", "maintenance_m1_interval",
     "maintenance_m1_name", "maintenance_m1_short", "maintenance_m1_left", "maintenance_m1_line",
     "maintenance_m1_note"},
    {"maintenance_m2_on", "maintenance_m2_pct", "maintenance_m2_status", "maintenance_m2_interval",
     "maintenance_m2_name", "maintenance_m2_short", "maintenance_m2_left", "maintenance_m2_line",
     "maintenance_m2_note"},
}};
// clang-format on

const Meter& meter_at(int i) {
    return s_state.meters[static_cast<size_t>(i)];
}

void save() {
    if (s_prefix.empty())
        return;
    if (SettingsManager::instance().set_maintenance_record(s_prefix, s_state)) {
        s_unsaved_s = 0;
        s_save_at_s = kSaveEverySeconds;
    } else {
        // A disk that refused this write gets the next attempt a full interval later,
        // not on every reading.
        s_save_at_s = s_unsaved_s + kSaveEverySeconds;
    }
}

void publish() {
    if (!s_subjects.initialized())
        return;
    // Readings arrive about once a second and most change nothing shown; the version
    // bumps only when something did, so the tile does not re-measure on every one.
    bool changed = false;
    bool any_on = false;
    for (int i = 0; i < kMeterCount; ++i) {
        const Meter& m = meter_at(i);
        MeterSubjects& ms = s_subj.meter[i];
        const bool on = maintenance::enabled(m);
        any_on = any_on || on;
        changed |= ui::set_int_if_changed(&ms.on, on ? 1 : 0);
        changed |= ui::set_int_if_changed(
            &ms.pct, static_cast<int>(std::lround(maintenance::used_fraction(m) * 100.0)));
        changed |=
            ui::set_int_if_changed(&ms.status, static_cast<int>(maintenance::meter_status(m)));
        changed |=
            ui::set_int_if_changed(&ms.interval, maintenance::interval_choice_index(m.interval_h));
        changed |= ui::copy_string_if_changed(&ms.name, meter_name(i).c_str());
        changed |= ui::copy_string_if_changed(&ms.short_name, meter_short_name(i).c_str());
        changed |=
            ui::copy_string_if_changed(&ms.left, hours_text(maintenance::hours_left(m)).c_str());
        changed |= ui::copy_string_if_changed(
            &ms.line,
            fmt::format(lv_tr("{} h of {} h"), maintenance::hours_used(m), m.interval_h).c_str());
        const int left_pct =
            static_cast<int>(std::lround((1.0 - maintenance::used_fraction(m)) * 100.0));
        std::string note = fmt::format(lv_tr("{}% left"), left_pct);
        if (m.reset_at_h) {
            note += " · ";
            note +=
                fmt::format(lv_tr("reset at {} h"), static_cast<int>(std::lround(*m.reset_at_h)));
        }
        changed |= ui::copy_string_if_changed(&ms.note, note.c_str());
    }

    const auto worst = maintenance::most_due(s_state);
    changed |= ui::set_int_if_changed(&s_subj.any_on, any_on ? 1 : 0);
    if (worst) {
        const Meter& m = meter_at(*worst);
        changed |= ui::set_int_if_changed(
            &s_subj.worst_pct,
            static_cast<int>(std::lround(maintenance::used_fraction(m) * 100.0)));
        changed |= ui::set_int_if_changed(&s_subj.worst_status,
                                          static_cast<int>(maintenance::meter_status(m)));
        changed |= ui::copy_string_if_changed(&s_subj.worst_left,
                                              hours_text(maintenance::hours_left(m)).c_str());
        changed |= ui::copy_string_if_changed(&s_subj.worst_name, meter_short_name(*worst).c_str());
    } else {
        changed |= ui::set_int_if_changed(&s_subj.worst_pct, 0);
        changed |=
            ui::set_int_if_changed(&s_subj.worst_status, static_cast<int>(ValueStatus::Normal));
        changed |= ui::copy_string_if_changed(&s_subj.worst_left, "--");
        changed |= ui::copy_string_if_changed(&s_subj.worst_name, lv_tr("Off"));
    }
    changed |= ui::copy_string_if_changed(&s_subj.total_text, total_line().c_str());
    if (changed)
        lv_subject_set_int(&s_subj.version, lv_subject_get_int(&s_subj.version) + 1);
}

void notify_due(int i) {
    const std::string msg =
        fmt::format(lv_tr("{} is due: {} h reached"), meter_name(i), meter_at(i).interval_h);
    spdlog::info("[MaintenanceModel] {} reached its {} h interval",
                 maintenance::kMeterKeys[static_cast<size_t>(i)], meter_at(i).interval_h);
    ToastManager::instance().show(ToastSeverity::WARNING, msg.c_str());
}

void on_print_duration(int*, int seconds) {
    const int delta = s_tracker.observe(seconds);
    if (delta <= 0)
        return;
    const auto due = maintenance::accrue(s_state, delta / 3600.0);
    s_unsaved_s += delta;
    for (int i : due)
        notify_due(i);
    if (!due.empty() || s_unsaved_s >= s_save_at_s)
        save();
    publish();
}

void on_connection_changed(int*, int) {
    s_tracker.reset();
}

void on_print_active(int*, int active) {
    if (active == 0 && s_unsaved_s > 0)
        save();
}

/// The example plugin's saved state, read once when this printer has no record.
std::optional<State> plugin_state() {
#if HELIX_HAS_PLUGINS
    Config* config = Config::get_instance();
    json storage;
    const std::string path = plugin::plugin_storage_path(config->get_path(), kPluginId);
    if (auto text = text_io::read_file(path))
        storage = json::parse(*text, nullptr, false);
    const json settings = config->get<json>(std::string("/plugins/settings/") + kPluginId, json());
    auto migrated = maintenance::from_plugin(storage, settings);
    if (migrated)
        spdlog::info("[MaintenanceModel] Took over the {} plugin's meters ({:.1f} h counted)",
                     kPluginId, migrated->total_h);
    return migrated;
#else
    return std::nullopt;
#endif
}

void load() {
    Config* config = Config::get_instance();
    s_prefix = config ? config->df() : std::string();
    s_unsaved_s = 0;
    s_save_at_s = kSaveEverySeconds;
    s_tracker.reset();
    if (s_prefix.empty()) {
        s_state = maintenance::default_state();
        return;
    }
    if (auto record = SettingsManager::instance().get_maintenance_record(s_prefix)) {
        s_state = *record;
        return;
    }
    s_state = plugin_state().value_or(maintenance::default_state());
    save();
}

void attach_observers() {
    auto& ps = get_printer_state();
    const SubjectLifetime printer_life = ps.get_subjects_lifetime();
    s_observers.push_back(ui::observe<int>(ps.print_state().get_print_duration_subject(), &s_anchor,
                                           on_print_duration, printer_life));
    s_observers.push_back(
        ui::observe<int>(ps.network_state().get_printer_connection_state_subject(), &s_anchor,
                         on_connection_changed, printer_life));
    s_observers.push_back(ui::observe<int>(ps.print_state().get_print_active_subject(), &s_anchor,
                                           on_print_active, printer_life));
}

void teardown() {
    if (s_unsaved_s > 0)
        save();
    s_observers.clear();
    s_language_observer.reset();
    s_prefix.clear();
}

} // namespace

std::string hours_text(int hours) {
    return fmt::format(lv_tr("{} h"), hours);
}

std::string total_line() {
    return fmt::format(lv_tr("Total print time: {} h"),
                       static_cast<int>(std::lround(s_state.total_h)));
}

std::string meter_name(int meter) {
    switch (static_cast<maintenance::MeterId>(meter)) {
    case maintenance::MeterId::Filter:
        return lv_tr("HEPA filter");
    case maintenance::MeterId::Nozzle:
        return lv_tr("Nozzle");
    case maintenance::MeterId::Rails:
        return lv_tr("Linear rails");
    }
    return {};
}

std::string meter_short_name(int meter) {
    switch (static_cast<maintenance::MeterId>(meter)) {
    case maintenance::MeterId::Filter:
        return lv_tr("HEPA");
    case maintenance::MeterId::Nozzle:
        return lv_tr("Nozzle");
    case maintenance::MeterId::Rails:
        return lv_tr("Rails");
    }
    return {};
}

void init_subjects() {
    const bool created = s_subjects.ensure_init(
        [] {
            UI_MANAGED_SUBJECT_INT(s_subj.version, 0, "maintenance_version", s_subjects);
            UI_MANAGED_SUBJECT_INT(s_subj.any_on, 0, "maintenance_any_on", s_subjects);
            UI_MANAGED_SUBJECT_INT(s_subj.worst_pct, 0, "maintenance_worst_pct", s_subjects);
            UI_MANAGED_SUBJECT_INT(s_subj.worst_status, 0, "maintenance_worst_status", s_subjects);
            UI_MANAGED_SUBJECT_STRING(s_subj.worst_left, s_subj.worst_left_buf, "--",
                                      "maintenance_worst_left", s_subjects);
            UI_MANAGED_SUBJECT_STRING(s_subj.worst_name, s_subj.worst_name_buf, "",
                                      "maintenance_worst_name", s_subjects);
            UI_MANAGED_SUBJECT_STRING(s_subj.total_text, s_subj.total_text_buf, "",
                                      "maintenance_total_text", s_subjects);
            for (int i = 0; i < kMeterCount; ++i) {
                MeterSubjects& ms = s_subj.meter[i];
                const auto& n = kMeterSubjectNames[static_cast<size_t>(i)];
                UI_MANAGED_SUBJECT_INT(ms.on, 0, n[0], s_subjects);
                UI_MANAGED_SUBJECT_INT(ms.pct, 0, n[1], s_subjects);
                UI_MANAGED_SUBJECT_INT(ms.status, 0, n[2], s_subjects);
                UI_MANAGED_SUBJECT_INT(ms.interval, 0, n[3], s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.name, ms.name_buf, "", n[4], s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.short_name, ms.short_buf, "", n[5], s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.left, ms.left_buf, "", n[6], s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.line, ms.line_buf, "", n[7], s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.note, ms.note_buf, "", n[8], s_subjects);
            }
        },
        teardown);
    // Replacing the guard drops one left on settings subjects since torn down and rebuilt,
    // so every call re-arms it, and publish() catches up on a switch it missed.
    s_language_observer = ui::observe_language_change(&s_anchor, [](int*) { publish(); });
    if (!created) {
        publish();
        return;
    }
    // After ensure_init(): publish() writes only to subjects it reports as initialized.
    load();
    attach_observers();
    publish();
    spdlog::debug("[MaintenanceModel] Loaded from {} ({:.1f} h counted)", s_prefix,
                  s_state.total_h);
}

const maintenance::State& state() {
    return s_state;
}

lv_subject_t* version_subject() {
    return &s_subj.version;
}

SubjectLifetime lifetime() {
    return s_subjects.get_subjects_lifetime();
}

void reset_meter(int meter) {
    if (meter < 0 || meter >= kMeterCount)
        return;
    maintenance::reset_meter(s_state, meter);
    save();
    publish();
    const std::string msg = fmt::format(lv_tr("{} meter reset"), meter_name(meter));
    ToastManager::instance().show(ToastSeverity::SUCCESS, msg.c_str());
}

void confirm_reset(int meter) {
    if (meter < 0 || meter >= kMeterCount)
        return;
    const std::string title = fmt::format(lv_tr("Reset {}?"), meter_name(meter));
    const std::string message =
        fmt::format(lv_tr("Start a new {} h interval? The hours counted so far are discarded."),
                    meter_at(meter).interval_h);
    ui::modal_confirm(title.c_str(), message.c_str(), ModalSeverity::Warning, lv_tr("Reset"),
                      [meter] { reset_meter(meter); });
}

void set_interval_choice(int meter, int choice_index) {
    if (meter < 0 || meter >= kMeterCount || choice_index < 0 ||
        choice_index >= static_cast<int>(maintenance::kIntervalChoices.size()))
        return;
    const int hours = maintenance::kIntervalChoices[static_cast<size_t>(choice_index)];
    if (meter_at(meter).interval_h == hours)
        return;
    maintenance::set_interval(s_state, meter, hours);
    save();
    publish();
}

std::string interval_options() {
    std::string options;
    for (int hours : maintenance::kIntervalChoices) {
        if (!options.empty())
            options += '\n';
        options += hours == 0 ? std::string(lv_tr("Off")) : hours_text(hours);
    }
    return options;
}

} // namespace helix::maintenance_model
