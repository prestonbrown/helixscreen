// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/dryer_model.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "bed_drying_controller.h"
#include "core/observer_factory.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "helix_psram_attr.h"
#include "printer_state.h"
#include "subject_managed_panel.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace helix::dryer_model {
namespace {

HELIX_PSRAM_BSS lv_subject_t s_available_subject{};
HELIX_PSRAM_BSS lv_subject_t s_version_subject{};
bool s_subjects_initialized = false;
SubjectManager s_subjects;

DryerSummary s_summary;
std::vector<ObserverGuard> s_observers;
// The observers' owner argument; the handlers use no instance state.
int s_anchor = 0;

ChamberCycle s_chamber_cycle;
// A test published a summary; refreshing would replace it with the live sources.
bool s_pinned = false;

// The controller is built after the widget subjects are, so its observers attach on the
// first refresh that finds it.
bool s_bed_watched = false;

bool subject_ready(const lv_subject_t* subject) {
    return subject != nullptr && subject->type != LV_SUBJECT_TYPE_INVALID;
}

// Sources notify far more often than the summary changes (every unit's humidity and
// countdown, the initial value of each observer), so a burst becomes one refresh.
bool s_refresh_queued = false;
void refresh_now();
// Adding an observer notifies it once with the current value; attach_sources() is
// followed by a refresh of its own, so those are not requests.
bool s_attaching = false;
// A source being torn down resets its subjects on the way out; the refresh that would
// follow has nothing left to read.
SubjectLifetime s_printer_life;

void request_refresh() {
    if (s_refresh_queued || s_attaching || (s_printer_life && !*s_printer_life))
        return;
    s_refresh_queued = true;
    ui::queue_update("DryerModel::refresh", [] {
        s_refresh_queued = false;
        refresh_now();
    });
}

void on_int(int*, int) {
    request_refresh();
}
void on_text(int*, const char*) {
    request_refresh();
}

// A temperature moves every status frame; the rows show whole degrees.
int s_chamber_shown_c = -1;
int s_bed_shown_c = -1;

void on_chamber_temp(int*, int deci) {
    const int c = static_cast<int>(std::lround(deci / 10.0));
    if (c != s_chamber_shown_c) {
        s_chamber_shown_c = c;
        request_refresh();
    }
}
void on_bed_temp(int*, int deci) {
    const int c = static_cast<int>(std::lround(deci / 10.0));
    if (c != s_bed_shown_c) {
        s_bed_shown_c = c;
        request_refresh();
    }
}

void watch(lv_subject_t* subject, const SubjectLifetime& life) {
    if (subject_ready(subject))
        s_observers.push_back(
            ui::observe<int>(subject, &s_anchor, on_int, life, ui::Dispatch::Immediate));
}

void watch_text(lv_subject_t* subject, const SubjectLifetime& life) {
    if (subject_ready(subject))
        s_observers.push_back(
            ui::observe<const char*>(subject, &s_anchor, on_text, life, ui::Dispatch::Immediate));
}

void attach_sources() {
    s_observers.clear();
    s_attaching = true;

    // A torn-down source's subjects still read as initialized, and its next init
    // resets their observer lists under any observer attached in between.
    auto& ps = get_printer_state();
    const SubjectLifetime printer_life = ps.get_subjects_lifetime();
    s_printer_life = printer_life;
    if (ps.are_subjects_initialized()) {
        watch(ps.capabilities_state().subject(Capability::HasChamberDryer), printer_life);
        watch(ps.capabilities_state().subject(Capability::CanBedDry), printer_life);
        watch(ps.temperature_state().get_chamber_dryer_active_subject(), printer_life);
        watch(ps.temperature_state().get_chamber_dryer_reported_subject(), printer_life);
        watch_text(ps.temperature_state().get_chamber_dryer_text_subject(), printer_life);
        if (auto* chamber = ps.temperature_state().get_chamber_temp_subject();
            subject_ready(chamber))
            s_observers.push_back(ui::observe<int>(chamber, &s_anchor, on_chamber_temp,
                                                   printer_life, ui::Dispatch::Immediate));
        if (auto* bed_temp = ps.temperature_state().get_bed_temp_subject(); subject_ready(bed_temp))
            s_observers.push_back(ui::observe<int>(bed_temp, &s_anchor, on_bed_temp, printer_life,
                                                   ui::Dispatch::Immediate));
    }

    auto& ams = AmsState::instance();
    if (ams.are_subjects_initialized()) {
        const SubjectLifetime ams_life = ams.get_subjects_lifetime();
        watch(ams.get_units_dryer_version_subject(), ams_life);
        for (int i = 0; i < AmsState::kMaxUnits; ++i) {
            watch(ams.get_unit_humidity_subject(i), ams_life);
            watch(ams.get_env_ind_visible_subject(i), ams_life);
            watch(ams.get_env_ind_drying_active_subject(i), ams_life);
            watch_text(ams.get_env_ind_drying_text_subject(i), ams_life);
        }
    }

    s_bed_watched = false;
    if (auto* bed = get_bed_drying_controller()) {
        s_bed_watched = true;
        const SubjectLifetime bed_life = bed->get_subjects_lifetime();
        watch(bed->get_state_subject(), bed_life);
        watch(bed->get_remaining_min_subject(), bed_life);
        watch(bed->get_progress_pct_subject(), bed_life);
    }
    s_attaching = false;
}

int ready_int_or_zero(lv_subject_t* subject) {
    return subject_ready(subject) ? lv_subject_get_int(subject) : 0;
}

int deci_to_c(lv_subject_t* subject) {
    return subject_ready(subject)
               ? static_cast<int>(std::lround(lv_subject_get_int(subject) / 10.0))
               : -1;
}

bool chamber_reported() {
    return ready_int_or_zero(
               get_printer_state().temperature_state().get_chamber_dryer_reported_subject()) != 0;
}

DryerSummaryInput read_sources() {
    DryerSummaryInput in;
    auto& ps = get_printer_state();

    if (AmsBackend* backend = AmsState::instance().get_backend())
        in.zones = backend->get_environment_zones(-1);

    auto& temps = ps.temperature_state();
    in.chamber.available =
        ready_int_or_zero(ps.capabilities_state().subject(Capability::HasChamberDryer)) != 0;
    in.chamber.active =
        in.chamber.available && ready_int_or_zero(temps.get_chamber_dryer_active_subject()) != 0;
    if (in.chamber.active) {
        in.chamber.remaining_min = temps.chamber_dryer_remaining_min();
        in.chamber.target_c =
            temps.chamber_dryer_target_c() > 0 ? temps.chamber_dryer_target_c() : -1;
    }
    in.chamber.current_c = deci_to_c(temps.get_chamber_temp_subject());

    in.bed.available =
        ready_int_or_zero(ps.capabilities_state().subject(Capability::CanBedDry)) != 0;
    in.bed.current_c = deci_to_c(temps.get_bed_temp_subject());
    if (auto* bed = get_bed_drying_controller(); bed && in.bed.available) {
        in.bed.running = bed->state() == BedDryingController::State::Running;
        if (in.bed.running) {
            in.bed.remaining_min = lv_subject_get_int(bed->get_remaining_min_subject());
            in.bed.progress_pct = lv_subject_get_int(bed->get_progress_pct_subject());
            in.bed.target_c = bed->record().bed_c;
        }
    }
    return in;
}

} // namespace

void init_subjects() {
    if (!s_subjects_initialized) {
        UI_MANAGED_SUBJECT_INT(s_available_subject, 0, "home_dryer_available", s_subjects);
        UI_MANAGED_SUBJECT_INT(s_version_subject, 0, "home_dryer_version", s_subjects);
        s_subjects_initialized = true;
        StaticSubjectRegistry::instance().register_deinit("DryerModelSubjects", []() {
            s_observers.clear();
            if (s_subjects_initialized && lv_is_initialized()) {
                s_subjects.deinit_all();
                s_subjects_initialized = false;
            }
            s_summary = {};
            s_chamber_cycle = {};
            s_bed_watched = false;
            s_refresh_queued = false;
        });
    }
    // A queue that was discarded under a pending request would otherwise block every later one.
    s_refresh_queued = false;
    s_pinned = false;
    attach_sources();
    refresh_now();
}

namespace {

void publish(DryerSummary fresh) {
    const bool changed = !(fresh == s_summary);
    s_summary = std::move(fresh);
    lv_subject_set_int(&s_available_subject, s_summary.available ? 1 : 0);
    if (changed) {
        lv_subject_set_int(&s_version_subject, lv_subject_get_int(&s_version_subject) + 1);
        spdlog::debug("[DryerModel] {} source(s), {} drying, highest RH {}",
                      s_summary.sources.size(), s_summary.drying_count, s_summary.highest_rh_pct);
    }
}

void refresh_now() {
    if (!s_subjects_initialized || s_pinned)
        return;
    if (!s_bed_watched && get_bed_drying_controller())
        attach_sources();
    DryerSummaryInput in = read_sources();
    s_chamber_cycle = next_chamber_cycle(s_chamber_cycle, chamber_reported(), in.chamber.active,
                                         in.chamber.remaining_min);
    in.chamber.total_min = s_chamber_cycle.counted ? s_chamber_cycle.total_min : -1;
    publish(summarize_dryers(in));
}

} // namespace

void refresh() {
    if (!s_subjects_initialized)
        return;
    // Sources that were deinited and brought back since the observers attached are not
    // the ones those observers hold.
    if (!s_pinned)
        attach_sources();
    refresh_now();
}

void publish_for_test(DryerSummary summary) {
    if (!s_subjects_initialized)
        return;
    s_pinned = true;
    publish(std::move(summary));
}

void release_for_test() {
    s_pinned = false;
}

const DryerSummary& summary() {
    return s_summary;
}

lv_subject_t* version_subject() {
    return &s_version_subject;
}

SubjectLifetime lifetime() {
    return s_subjects.get_subjects_lifetime();
}

} // namespace helix::dryer_model
