// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/maintenance_model.h"

#include "ui_maintenance_overlay.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_panel_notification_history.h"
#include "ui_subject_registry.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "chamber_air_modes.h"
#include "config.h"
#include "core/observer_factory.h"
#include "core/text_io.h"
#include "core/timer_guard.h"
#include "core/update_queue.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "observe_language.h"
#include "printer_capabilities_state.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "subject_managed_panel.h"
#include "translation_loader.h"
#include "wall_clock_trust.h"
#include "widget_subjects.h"

#if HELIX_HAS_PLUGINS
#include "lua_bindings.h"
#endif

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <ctime>
#include <deque>

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

/// Delay before the first idle check for due meters after the printer is known, so the
/// initial status has said whether a print is running.
constexpr uint32_t kIdleAnnounceDelayMs = 5000;

/// The notification action id; history entries carry `maintenance:<key>`.
constexpr const char* kNoticeAction = "maintenance";

/// One meter's translatable text. Task page steps past the first empty one are unused.
struct MeterText {
    const char* name;
    const char* short_name;
    const char* description;
    const char* what;
    std::array<const char*, 5> steps;
};

// clang-format off
constexpr std::array<MeterText, kMeterCount> kMeterText = {{
    {TR_NOOP("XY axis"), TR_NOOP("XY"),
     TR_NOOP("Clean and lubricate the X and Y rails or rods"),
     TR_NOOP("Dust and old grease on the X and Y rails or rods cause noise, ringing and wear."),
     {TR_NOOP("Power off the printer."),
      TR_NOOP("Wipe each rail or rod clean with a lint-free cloth."),
      TR_NOOP("Apply a thin film of the lubricant your manual names."),
      TR_NOOP("Move the toolhead end to end by hand to spread it."),
      TR_NOOP("Your printer's manual has the parts and lubricant for your model.")}},
    {TR_NOOP("Z axis"), TR_NOOP("Z"),
     TR_NOOP("Clean and lubricate the Z screws and guides"),
     TR_NOOP("Dirty or dry Z screws bind, which shows up as uneven layer lines."),
     {TR_NOOP("Power off the printer."),
      TR_NOOP("Wipe the Z screws and guides clean."),
      TR_NOOP("Apply the lubricant your manual names, sparingly."),
      TR_NOOP("Raise and lower the bed or gantry through its travel."),
      TR_NOOP("Your printer's manual has the parts and lubricant for your model.")}},
    {TR_NOOP("Nozzle"), TR_NOOP("Nozzle"),
     TR_NOOP("Replace a worn nozzle or hotend"),
     TR_NOOP("Nozzles wear, abrasive filament fastest; a worn one under-extrudes and strings."),
     {TR_NOOP("Heat the hotend."),
      TR_NOOP("Follow your manual to swap the nozzle or hotend."),
      TR_NOOP("Re-check the Z offset after the swap."),
      TR_NOOP("Your printer's manual has the parts and lubricant for your model."), nullptr}},
    {TR_NOOP("Extruder"), TR_NOOP("Extr."),
     TR_NOOP("Brush filament dust out of the extruder gears"),
     TR_NOOP("Filament dust packs into the drive gears and costs grip."),
     {TR_NOOP("Unload filament and power off the printer."),
      TR_NOOP("Open the extruder as your manual shows."),
      TR_NOOP("Brush the gear teeth clean."),
      TR_NOOP("Reassemble and load filament."),
      TR_NOOP("Your printer's manual has the parts and lubricant for your model.")}},
    {TR_NOOP("Air filter"), TR_NOOP("Filter"),
     TR_NOOP("Replace the chamber air filter"),
     TR_NOOP("A saturated filter stops trapping fumes and particles."),
     {TR_NOOP("Power off the printer."),
      TR_NOOP("Remove the filter cover."),
      TR_NOOP("Replace the filter element."),
      TR_NOOP("Your printer's manual has the parts and lubricant for your model."), nullptr}},
    {TR_NOOP("Electronics fans"), TR_NOOP("Fans"),
     TR_NOOP("Clean dust from the mainboard and power supply fans"),
     TR_NOOP("Dusty board and power supply fans run hot and fail early."),
     {TR_NOOP("Power off and unplug the printer."),
      TR_NOOP("Blow the dust out of the fans and vents."),
      TR_NOOP("Check each fan spins freely."),
      TR_NOOP("Your printer's manual has the parts and lubricant for your model."), nullptr}},
    {TR_NOOP("Belt tension"), TR_NOOP("Belts"),
     TR_NOOP("Check the tension of the motion belts"),
     TR_NOOP("Belts stretch and settle; loose ones cause ringing and layer shifts."),
     {TR_NOOP("Power off the printer."),
      TR_NOOP("Check both belts for wear and frayed edges."),
      TR_NOOP("Measure the tension with the Belt Tension tool, or as your manual shows."),
      TR_NOOP("Adjust the tensioners until both belts match."),
      TR_NOOP("Your printer's manual has the parts and lubricant for your model.")}},
}};
// clang-format on

struct MeterSubjects {
    lv_subject_t on{};
    lv_subject_t pct{};
    lv_subject_t status{};
    lv_subject_t interval{};
    lv_subject_t name{};
    lv_subject_t short_name{};
    lv_subject_t left{};
    lv_subject_t line{};
    lv_subject_t days{};
    lv_subject_t desc{};
    char name_buf[64]{};
    char short_buf[32]{};
    char left_buf[24]{};
    char line_buf[48]{};
    char desc_buf[96]{};
};

/// One row of the home tile, filled from due_order().
struct RankSubjects {
    lv_subject_t on{};
    lv_subject_t pct{};
    lv_subject_t status{};
    lv_subject_t name{};
    lv_subject_t short_name{};
    lv_subject_t left{};
    char name_buf[64]{};
    char short_buf[32]{};
    char left_buf[24]{};
};

/// The task page: the one meter it is open on.
struct TaskSubjects {
    lv_subject_t pct{};
    lv_subject_t status{};
    lv_subject_t name{};
    lv_subject_t left{};
    lv_subject_t line{};
    lv_subject_t last{};
    lv_subject_t what{};
    lv_subject_t steps{};
    char name_buf[64]{};
    char left_buf[24]{};
    char line_buf[96]{};
    char last_buf[96]{};
    char what_buf[192]{};
    char steps_buf[640]{};
};

struct Subjects {
    lv_subject_t version{};
    lv_subject_t any_on{};
    lv_subject_t worst_pct{};
    lv_subject_t worst_status{};
    lv_subject_t worst_left{};
    lv_subject_t worst_name{};
    lv_subject_t total_text{};
    lv_subject_t belt_available{};
    char worst_left_buf[24]{};
    char worst_name_buf[32]{};
    char total_text_buf[96]{};
    MeterSubjects meter[kMeterCount];
    RankSubjects rank[maintenance::kTileRows];
    TaskSubjects task;
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
/// The task page's meter.
int s_task_meter = 0;
bool s_print_was_active = false;
/// The first idle check after the printer became known has run.
bool s_idle_checked = false;
ui::LvglTimerGuard s_idle_timer;

/// Subject names are held by pointer, so they must outlive registration.
const char* subject_name(std::string name) {
    static std::deque<std::string> s_names;
    s_names.push_back(std::move(name));
    return s_names.back().c_str();
}

/// The wall clock, or nullopt until NTP or an RTC has set it.
std::optional<int64_t> trusted_now() {
    const auto now = static_cast<int64_t>(std::time(nullptr));
    return wall_clock_trusted(now) ? std::optional<int64_t>(now) : std::nullopt;
}

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

std::string left_text(const Meter& m, std::optional<int64_t> now) {
    if (maintenance::days_are_closer(m, now))
        return fmt::format(lv_tr("{} d"), maintenance::days_left(m, now).value_or(0));
    return hours_text(maintenance::hours_left(m));
}

int due_percent(const Meter& m, std::optional<int64_t> now) {
    return static_cast<int>(std::lround(maintenance::used_fraction(m, now) * 100.0));
}

std::string task_last_text(const Meter& m) {
    if (!m.reset_at_h)
        return {};
    std::string text =
        fmt::format(lv_tr("Last done at {} h"), static_cast<int>(std::lround(*m.reset_at_h)));
    if (m.reset_at_s) {
        const std::time_t stamp = static_cast<std::time_t>(*m.reset_at_s);
        std::tm local{};
        char date[16]{};
        if (localtime_r(&stamp, &local) && std::strftime(date, sizeof(date), "%Y-%m-%d", &local))
            text += fmt::format(" ({})", date);
    }
    return text;
}

std::string task_steps_text(int i) {
    std::string text;
    int number = 0;
    for (const char* step : kMeterText[static_cast<size_t>(i)].steps) {
        if (!step)
            break;
        text += fmt::format("{}{}. {}", number ? "\n" : "", number + 1, lv_tr(step));
        ++number;
    }
    return text;
}

bool publish_task(std::optional<int64_t> now) {
    TaskSubjects& ts = s_subj.task;
    const Meter& m = meter_at(s_task_meter);
    bool changed = ui::set_int_if_changed(&ts.pct, due_percent(m, now));
    changed |=
        ui::set_int_if_changed(&ts.status, static_cast<int>(maintenance::meter_status(m, now)));
    changed |= ui::copy_string_if_changed(&ts.name, meter_name(s_task_meter).c_str());
    changed |= ui::copy_string_if_changed(&ts.left, left_text(m, now).c_str());
    std::string line = fmt::format(lv_tr("{} h of {} h"), maintenance::hours_used(m), m.interval_h);
    if (m.interval_d > 0)
        line += " · " + fmt::format(lv_tr("every {} days"), m.interval_d);
    changed |= ui::copy_string_if_changed(&ts.line, line.c_str());
    changed |= ui::copy_string_if_changed(&ts.last, task_last_text(m).c_str());
    changed |= ui::copy_string_if_changed(
        &ts.what, lv_tr(kMeterText[static_cast<size_t>(s_task_meter)].what));
    changed |= ui::copy_string_if_changed(&ts.steps, task_steps_text(s_task_meter).c_str());
    return changed;
}

void publish() {
    if (!s_subjects.initialized())
        return;
    const auto now = trusted_now();
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
        changed |= ui::set_int_if_changed(&ms.pct, due_percent(m, now));
        changed |=
            ui::set_int_if_changed(&ms.status, static_cast<int>(maintenance::meter_status(m, now)));
        changed |=
            ui::set_int_if_changed(&ms.interval, maintenance::interval_choice_index(m.interval_h));
        changed |= ui::set_int_if_changed(&ms.days, maintenance::day_choice_index(m.interval_d));
        changed |= ui::copy_string_if_changed(&ms.name, meter_name(i).c_str());
        changed |= ui::copy_string_if_changed(&ms.short_name, meter_short_name(i).c_str());
        changed |= ui::copy_string_if_changed(
            &ms.desc, lv_tr(kMeterText[static_cast<size_t>(i)].description));
        changed |= ui::copy_string_if_changed(&ms.left, left_text(m, now).c_str());
        changed |= ui::copy_string_if_changed(
            &ms.line,
            fmt::format(lv_tr("{} h of {} h"), maintenance::hours_used(m), m.interval_h).c_str());
    }

    const std::vector<int> order = maintenance::due_order(s_state, now);
    for (int k = 0; k < maintenance::kTileRows; ++k) {
        RankSubjects& rs = s_subj.rank[k];
        const bool on = k < static_cast<int>(order.size());
        changed |= ui::set_int_if_changed(&rs.on, on ? 1 : 0);
        if (!on)
            continue;
        const int i = order[static_cast<size_t>(k)];
        const Meter& m = meter_at(i);
        changed |= ui::set_int_if_changed(&rs.pct, due_percent(m, now));
        changed |=
            ui::set_int_if_changed(&rs.status, static_cast<int>(maintenance::meter_status(m, now)));
        changed |= ui::copy_string_if_changed(&rs.name, meter_name(i).c_str());
        changed |= ui::copy_string_if_changed(&rs.short_name, meter_short_name(i).c_str());
        changed |= ui::copy_string_if_changed(&rs.left, left_text(m, now).c_str());
    }

    changed |= publish_task(now);

    const auto worst = maintenance::most_due(s_state, now);
    changed |= ui::set_int_if_changed(&s_subj.any_on, any_on ? 1 : 0);
    if (worst) {
        const Meter& m = meter_at(*worst);
        changed |= ui::set_int_if_changed(&s_subj.worst_pct, due_percent(m, now));
        changed |= ui::set_int_if_changed(&s_subj.worst_status,
                                          static_cast<int>(maintenance::meter_status(m, now)));
        changed |= ui::copy_string_if_changed(&s_subj.worst_left, left_text(m, now).c_str());
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

void task_toast_clicked(void* user_data) {
    open_task_page(static_cast<int>(reinterpret_cast<intptr_t>(user_data)));
}

void overlay_toast_clicked(void*) {
    get_maintenance_overlay().show(lv_screen_active());
}

/// Announces every meter that fell due and has not been announced: one toast, and one
/// history entry per meter. A print still running holds the notices for its end.
void announce_pending() {
    if (lv_subject_get_int(get_printer_state().print_state().get_print_active_subject()) != 0)
        return;
    const std::vector<int> due = maintenance::take_pending_notices(s_state, trusted_now());
    if (due.empty())
        return;
    save();
    publish();
    for (int i : due) {
        const std::string message = fmt::format(lv_tr("{} is due"), meter_name(i));
        const std::string action =
            fmt::format("{}:{}", kNoticeAction, maintenance::kMeterKeys[static_cast<size_t>(i)]);
        spdlog::info("[MaintenanceModel] {} is due",
                     maintenance::kMeterKeys[static_cast<size_t>(i)]);
        ui_notification_warning_with_action(lv_tr("Maintenance"), message.c_str(), action.c_str());
    }
    if (due.size() == 1) {
        const std::string message = fmt::format(lv_tr("{} is due"), meter_name(due[0]));
        ToastManager::instance().show_with_action(
            ToastSeverity::WARNING, message.c_str(), lv_tr("Details"), task_toast_clicked,
            reinterpret_cast<void*>(static_cast<intptr_t>(due[0])));
    } else {
        const std::string message = fmt::format(lv_tr("{} maintenance tasks are due"), due.size());
        ToastManager::instance().show_with_action(ToastSeverity::WARNING, message.c_str(),
                                                  lv_tr("View"), overlay_toast_clicked, nullptr);
    }
}

/// Whether the Filter meter follows a firmware filter counter: the printer's chamber
/// air provider keeps one and it has reported a value.
bool filter_from_firmware() {
    auto& ps = get_printer_state();
    return !chamber_air::filter_reset_gcode(ps.get_discovery()).empty() &&
           ps.chamber_air().filter_seconds().has_value();
}

void on_print_duration(int*, int seconds) {
    const int delta = s_tracker.observe(seconds);
    if (delta <= 0)
        return;
    maintenance::accrue(s_state, delta / 3600.0, filter_from_firmware());
    s_unsaved_s += delta;
    if (s_unsaved_s >= s_save_at_s)
        save();
    publish();
}

void on_firmware_filter_minutes(int*, int minutes) {
    if (minutes < 0 || !filter_from_firmware())
        return;
    const auto secs = get_printer_state().chamber_air().filter_seconds();
    maintenance::apply_firmware_filter_hours(s_state, secs.value_or(0.0) / 3600.0);
    publish();
    if (s_idle_checked)
        announce_pending();
}

void on_connection_changed(int*, int) {
    s_tracker.reset();
}

void on_print_active(int*, int active) {
    const bool ended = active == 0 && s_print_was_active;
    s_print_was_active = active != 0;
    if (active != 0)
        return;
    if (s_unsaved_s > 0)
        save();
    if (ended)
        announce_pending();
}

/// What the printer reports for the applicability rules.
maintenance::Applicability printer_applicability() {
    auto& ps = get_printer_state();
    auto& caps = ps.capabilities_state();
    const auto capable = [&caps](Capability c) { return lv_subject_get_int(caps.subject(c)) != 0; };
    maintenance::Applicability a;
    a.enclosed = capable(Capability::IsEnclosed);
    a.chamber_filter_fan = capable(Capability::HasChamberFilterFan);
    a.firmware_filter = !chamber_air::filter_reset_gcode(ps.get_discovery()).empty();
    a.controller_fan = !ps.get_discovery().objects_of({hw::ObjectKind::ControllerFan}).empty();
    return a;
}

void idle_check_timer_cb(lv_timer_t*) {
    s_idle_timer.reset();
    check_idle_notices();
}

/// Seeds the meters once discovery has run and the printer has a type; a printer the
/// wizard is still identifying waits for it, because its model row decides the defaults.
void try_seed() {
    if (!s_subjects.initialized())
        return;
    auto& ps = get_printer_state();
    if (lv_subject_get_int(ps.capabilities_state().subject(Capability::HardwareGeneration)) <= 0)
        return;
    const std::string& printer_type = ps.profile_state().printer_type();
    if (printer_type.empty() && is_wizard_active())
        return;
    const maintenance::ModelDefaults model =
        PrinterDetector::get_maintenance_defaults(printer_type);
    const auto belt = static_cast<size_t>(maintenance::MeterId::BeltTension);
    ui::set_int_if_changed(&s_subj.belt_available, model[belt].hours.value_or(0) > 0 ? 1 : 0);
    const bool seeded = maintenance::seed_defaults(s_state, printer_applicability(), model);
    const bool stamped = maintenance::stamp_calendar_starts(s_state, trusted_now());
    if (seeded || stamped)
        save();
    publish();
    if (!s_idle_checked && !s_idle_timer) {
        s_idle_timer.reset(lv_timer_create(idle_check_timer_cb, kIdleAnnounceDelayMs, nullptr));
    }
}

void on_printer_known(int*, int) {
    try_seed();
}

void on_notice_action(const std::string& key) {
    const auto it = std::find_if(maintenance::kMeterKeys.begin(), maintenance::kMeterKeys.end(),
                                 [&key](const char* k) { return key == k; });
    if (it == maintenance::kMeterKeys.end()) {
        spdlog::info("[MaintenanceModel] Notice for an unknown meter '{}' ignored", key);
        return;
    }
    open_task_page(static_cast<int>(std::distance(maintenance::kMeterKeys.begin(), it)));
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
    s_observers.push_back(ui::observe<int>(ps.chamber_air().filter_minutes_subject(), &s_anchor,
                                           on_firmware_filter_minutes, printer_life));
    s_observers.push_back(
        ui::observe<int>(ps.capabilities_state().subject(Capability::HardwareGeneration), &s_anchor,
                         on_printer_known, printer_life));
    s_observers.push_back(ui::observe<const char*>(
        ps.profile_state().get_printer_type_subject(), &s_anchor,
        [](int*, const char*) { try_seed(); }, printer_life));
}

void teardown() {
    if (s_unsaved_s > 0)
        save();
    s_observers.clear();
    s_language_observer.reset();
    s_idle_timer.reset();
    s_idle_checked = false;
    s_print_was_active = false;
    ui::set_notification_action_handler(kNoticeAction, nullptr);
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
    if (meter < 0 || meter >= kMeterCount)
        return {};
    return lv_tr(kMeterText[static_cast<size_t>(meter)].name);
}

std::string meter_short_name(int meter) {
    if (meter < 0 || meter >= kMeterCount)
        return {};
    return lv_tr(kMeterText[static_cast<size_t>(meter)].short_name);
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
            UI_MANAGED_SUBJECT_INT(s_subj.belt_available, 0, "maintenance_belt_available",
                                   s_subjects);
            for (int i = 0; i < kMeterCount; ++i) {
                MeterSubjects& ms = s_subj.meter[i];
                const auto n = [i](const char* field) {
                    return subject_name(fmt::format("maintenance_m{}_{}", i, field));
                };
                UI_MANAGED_SUBJECT_INT(ms.on, 0, n("on"), s_subjects);
                UI_MANAGED_SUBJECT_INT(ms.pct, 0, n("pct"), s_subjects);
                UI_MANAGED_SUBJECT_INT(ms.status, 0, n("status"), s_subjects);
                UI_MANAGED_SUBJECT_INT(ms.interval, 0, n("interval"), s_subjects);
                UI_MANAGED_SUBJECT_INT(ms.days, 0, n("days"), s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.name, ms.name_buf, "", n("name"), s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.short_name, ms.short_buf, "", n("short"), s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.left, ms.left_buf, "", n("left"), s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.line, ms.line_buf, "", n("line"), s_subjects);
                UI_MANAGED_SUBJECT_STRING(ms.desc, ms.desc_buf, "", n("desc"), s_subjects);
            }
            for (int k = 0; k < maintenance::kTileRows; ++k) {
                RankSubjects& rs = s_subj.rank[k];
                const auto n = [k](const char* field) {
                    return subject_name(fmt::format("maintenance_r{}_{}", k, field));
                };
                UI_MANAGED_SUBJECT_INT(rs.on, 0, n("on"), s_subjects);
                UI_MANAGED_SUBJECT_INT(rs.pct, 0, n("pct"), s_subjects);
                UI_MANAGED_SUBJECT_INT(rs.status, 0, n("status"), s_subjects);
                UI_MANAGED_SUBJECT_STRING(rs.name, rs.name_buf, "", n("name"), s_subjects);
                UI_MANAGED_SUBJECT_STRING(rs.short_name, rs.short_buf, "", n("short"), s_subjects);
                UI_MANAGED_SUBJECT_STRING(rs.left, rs.left_buf, "", n("left"), s_subjects);
            }
            TaskSubjects& ts = s_subj.task;
            UI_MANAGED_SUBJECT_INT(ts.pct, 0, "maintenance_task_pct", s_subjects);
            UI_MANAGED_SUBJECT_INT(ts.status, 0, "maintenance_task_status", s_subjects);
            UI_MANAGED_SUBJECT_STRING(ts.name, ts.name_buf, "", "maintenance_task_name",
                                      s_subjects);
            UI_MANAGED_SUBJECT_STRING(ts.left, ts.left_buf, "", "maintenance_task_left",
                                      s_subjects);
            UI_MANAGED_SUBJECT_STRING(ts.line, ts.line_buf, "", "maintenance_task_line",
                                      s_subjects);
            UI_MANAGED_SUBJECT_STRING(ts.last, ts.last_buf, "", "maintenance_task_last",
                                      s_subjects);
            UI_MANAGED_SUBJECT_STRING(ts.what, ts.what_buf, "", "maintenance_task_what",
                                      s_subjects);
            UI_MANAGED_SUBJECT_STRING(ts.steps, ts.steps_buf, "", "maintenance_task_steps",
                                      s_subjects);
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
    ui::set_notification_action_handler(kNoticeAction, on_notice_action);
    load();
    attach_observers();
    publish();
    try_seed();
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
    // A firmware-kept filter counter is cleared on the printer first; the meter
    // follows once the printer has taken it, so a refused reset leaves both alone.
    if (meter == static_cast<int>(maintenance::MeterId::Filter) && filter_from_firmware()) {
        IMoonrakerAPI* api = get_moonraker_api();
        if (!api) {
            ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("No printer connection"));
            return;
        }
        const std::string gcode =
            chamber_air::filter_reset_gcode(get_printer_state().get_discovery());
        spdlog::info("[MaintenanceModel] Resetting the firmware filter counter: {}", gcode);
        api->execute_gcode(
            gcode,
            [meter]() {
                ui::queue_update("MaintenanceModel::filter_reset",
                                 [meter]() { reset_meter_locally(meter); });
            },
            [](const MoonrakerError& err) {
                spdlog::warn("[MaintenanceModel] Filter counter reset failed: {}", err.message);
                ui::queue_update("MaintenanceModel::filter_reset_failed", []() {
                    ToastManager::instance().show(ToastSeverity::ERROR,
                                                  lv_tr("The printer did not reset the filter"));
                });
            });
        return;
    }
    reset_meter_locally(meter);
}

void reset_meter_locally(int meter) {
    if (meter < 0 || meter >= kMeterCount)
        return;
    maintenance::reset_meter(s_state, meter, trusted_now());
    save();
    publish();
    const std::string msg = fmt::format(lv_tr("{} meter reset"), meter_name(meter));
    ToastManager::instance().show(ToastSeverity::SUCCESS, msg.c_str());
}

void confirm_reset(int meter) {
    if (meter < 0 || meter >= kMeterCount)
        return;
    const std::string title = fmt::format(lv_tr("Mark {} done?"), meter_name(meter));
    const std::string message =
        fmt::format(lv_tr("Start a new {} h interval? The hours counted so far are discarded."),
                    meter_at(meter).interval_h);
    ui::modal_confirm(title.c_str(), message.c_str(), ModalSeverity::Warning, lv_tr("Mark done"),
                      [meter] { reset_meter(meter); });
}

void set_interval_choice(int meter, int choice_index) {
    if (meter < 0 || meter >= kMeterCount || choice_index < 0 ||
        choice_index >= static_cast<int>(maintenance::kIntervalChoices.size()))
        return;
    const int hours = maintenance::kIntervalChoices[static_cast<size_t>(choice_index)];
    if (meter_at(meter).interval_h == hours)
        return;
    maintenance::set_interval(s_state, meter, hours, trusted_now());
    save();
    publish();
}

void set_days_choice(int meter, int choice_index) {
    if (meter < 0 || meter >= kMeterCount || choice_index < 0 ||
        choice_index >= static_cast<int>(maintenance::kDayChoices.size()))
        return;
    const int days = maintenance::kDayChoices[static_cast<size_t>(choice_index)];
    if (meter_at(meter).interval_d == days)
        return;
    maintenance::set_interval_days(s_state, meter, days, trusted_now());
    save();
    publish();
}

void check_idle_notices() {
    s_idle_checked = true;
    announce_pending();
}

void set_task_meter(int meter) {
    if (meter < 0 || meter >= kMeterCount)
        return;
    s_task_meter = meter;
    publish();
}

int task_meter() {
    return s_task_meter;
}

void open_task_page(int meter) {
    if (meter < 0 || meter >= kMeterCount)
        return;
    set_task_meter(meter);
    get_maintenance_task_overlay().show(lv_screen_active());
}

std::string day_options() {
    std::string options = lv_tr("No day limit");
    for (size_t i = 1; i < maintenance::kDayChoices.size(); ++i) {
        options += '\n';
        options += fmt::format(lv_tr("{} days"), maintenance::kDayChoices[i]);
    }
    return options;
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
