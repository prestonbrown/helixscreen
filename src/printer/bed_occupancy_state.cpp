// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_occupancy_state.h"

#include "config.h"
#include "connection_state.h"
#include "core/observer_factory.h"
#include "print_lifecycle_state.h"
#include "printer_excluded_objects_state.h"
#include "printer_network_state.h"
#include "printer_print_state.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

constexpr const char* kRegistryName = "BedOccupancyState";

BedOccupancyState* g_bed_occupancy_state = nullptr;

bool lifecycle_ends_print(PrintState state) {
    return state == PrintState::Complete || state == PrintState::Cancelled ||
           state == PrintState::Error;
}

bool same_verdict(const BedOccupancy& a, const BedOccupancy& b) {
    return a.occupied == b.occupied && a.part_height_mm == b.part_height_mm &&
           a.job_id == b.job_id && a.start_time == b.start_time;
}

std::string config_path(Config& config, const char* key) {
    return config.df() + key;
}

std::optional<BedJobSummary> read_job(Config& config, const char* key) {
    const auto text = config.get<std::string>(config_path(config, key), "");
    return text.empty() ? std::nullopt : bed_job_from_json(text);
}

/// Each defined object's slicer polygon, or its bbox as four corners when it has none.
std::vector<BedObjectOutline> defined_outlines(const PrinterExcludedObjectsState& objects) {
    std::vector<BedObjectOutline> out;
    for (const auto& name : objects.get_defined_objects()) {
        const auto info = objects.get_object_geometry(name);
        if (!info) {
            continue;
        }
        BedObjectOutline outline;
        if (!info->polygon.empty()) {
            outline.points = info->polygon;
        } else if (info->has_bbox) {
            outline.points = {info->bbox_min,
                              {info->bbox_max.x, info->bbox_min.y},
                              info->bbox_max,
                              {info->bbox_min.x, info->bbox_max.y}};
        } else {
            continue;
        }
        out.push_back(std::move(outline));
    }
    return out;
}

} // namespace

BedOccupancyState* get_bed_occupancy_state() {
    return g_bed_occupancy_state;
}

void set_bed_occupancy_state(BedOccupancyState* state) {
    g_bed_occupancy_state = state;
}

BedOccupancyState::BedOccupancyState(PrinterState& printer_state, PrintHistoryManager* history)
    : printer_state_(printer_state), history_(history) {
    if (Config* config = Config::get_instance()) {
        const auto text =
            config->get<std::string>(config_path(*config, kBedOutlineSnapshotKey), "");
        if (!text.empty()) {
            snapshot_ = bed_outline_snapshot_from_json(text);
        }
        last_occupied_ = read_job(*config, kBedLastOccupiedKey);
    }
}

BedOccupancyState::~BedOccupancyState() {
    detach_history();
    lifecycle_observer_.reset();
    connection_observer_.reset();
    objects_observer_.reset();
    history_retry_timer_.reset();
    if (subjects_initialized_) {
        deinit_subjects();
        StaticSubjectRegistry::instance().unregister(kRegistryName);
    }
}

void BedOccupancyState::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(occupied_, 0, "bed_occupied", subjects_);
    UI_MANAGED_SUBJECT_INT(version_, 0, "bed_occupancy_version", subjects_);
    subjects_initialized_ = true;
    StaticSubjectRegistry::instance().register_deinit(kRegistryName,
                                                      [this]() { deinit_subjects(); });

    if (history_ && !history_attached_) {
        history_cb_ = [this]() { on_history_changed(); };
        history_->add_observer(&history_cb_);
        history_attached_ = true;
    }

    last_lifecycle_ = printer_state_.print_state().get_print_lifecycle();
    const SubjectLifetime lifetime = printer_state_.get_subjects_lifetime();
    lifecycle_observer_ = ui::observe_print_lifecycle(
        printer_state_.print_state().get_print_lifecycle_subject(), this,
        [](BedOccupancyState* self, PrintState state) { self->on_print_lifecycle(state); },
        lifetime);
    connection_observer_ = ui::observe<int>(
        printer_state_.network_state().get_printer_connection_state_subject(), this,
        [](BedOccupancyState* self, int state) { self->on_connection_state(state); }, lifetime);
    objects_observer_ = ui::observe<int>(
        printer_state_.excluded_objects_state().get_defined_objects_version_subject(), this,
        [](BedOccupancyState* self, int /*version*/) { self->bump_version(); }, lifetime);

    refresh();
    spdlog::debug("[BedOccupancy] Subjects initialized");
}

void BedOccupancyState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    lifecycle_observer_.reset();
    connection_observer_.reset();
    objects_observer_.reset();
    history_retry_timer_.reset();
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void BedOccupancyState::detach_history() {
    if (history_ && history_attached_) {
        history_->remove_observer(&history_cb_);
    }
    history_attached_ = false;
    history_ = nullptr;
}

bool BedOccupancyState::history_loaded() const {
    return history_ && history_->is_loaded(HistoryScope::RECENT);
}

void BedOccupancyState::on_connection_state(int state) {
    if (state != static_cast<int>(ConnectionState::CONNECTED)) {
        history_retry_timer_.reset();
        return;
    }
    if (history_) {
        history_->ensure_loaded(HistoryScope::RECENT);
        start_history_retry();
    }
}

void BedOccupancyState::start_history_retry() {
    if (history_retry_timer_ || history_loaded()) {
        return;
    }
    history_retry_timer_.reset(lv_timer_create(history_retry_cb, kHistoryRetryMs, this));
}

void BedOccupancyState::history_retry_cb(lv_timer_t* timer) {
    auto* self = static_cast<BedOccupancyState*>(lv_timer_get_user_data(timer));
    if (!self->history_ || self->history_loaded()) {
        self->history_retry_timer_.reset();
        return;
    }
    spdlog::debug("[BedOccupancy] RECENT history not loaded yet, asking again");
    self->history_->ensure_loaded(HistoryScope::RECENT);
}

std::optional<BedJobSummary> BedOccupancyState::newest_job() const {
    // A history that has not loaded is not "no history": a part may still be on the bed.
    if (!history_loaded()) {
        return last_occupied_;
    }
    // The newest job, not the newest whose file still exists: a part is on the bed even
    // when its file was deleted.
    if (history_->get_jobs().empty()) {
        return std::nullopt;
    }
    const PrintHistoryJob& job = history_->get_jobs().front();
    BedJobSummary summary;
    summary.job_id = job.job_id;
    summary.start_time = job.start_time;
    summary.filename = job.filename;
    summary.status = job.status;
    summary.object_height_mm = job.object_height;
    return summary;
}

void BedOccupancyState::refresh() {
    BedOccupancyInput in;
    in.newest = newest_job();
    in.print_active = job_holds_machine(printer_state_.print_state().get_print_lifecycle());
    if (Config* config = Config::get_instance()) {
        if (const auto cleared = read_job(*config, kBedClearedJobKey)) {
            in.cleared = {cleared->job_id, cleared->start_time};
        }
    }

    const BedOccupancy verdict = decide_bed_occupancy(in);
    if (history_loaded()) {
        history_retry_timer_.reset();
        if (!in.print_active) {
            persist_last_occupied(verdict, in.newest);
        }
    }
    if (same_verdict(verdict, current_)) {
        return;
    }
    current_ = verdict;

    const char* reason = "the newest job may have left a part";
    if (!verdict.occupied) {
        if (in.print_active) {
            reason = "a print holds the machine";
        } else if (!in.newest) {
            reason = history_loaded() ? "no print history" : "history not loaded, no part recorded";
        } else if (same_bed_job(in.cleared, *in.newest)) {
            reason = "the user cleared the bed";
        } else {
            reason = "the newest job left no part";
        }
    }
    spdlog::info("[BedOccupancy] {} (job '{}', height {}): {}",
                 verdict.occupied ? "occupied" : "clear",
                 in.newest ? in.newest->job_id : std::string(),
                 verdict.part_height_mm ? fmt::format("{:.2f}mm", *verdict.part_height_mm)
                                        : std::string("unknown"),
                 reason);

    if (subjects_initialized_) {
        lv_subject_set_int(&occupied_, verdict.occupied ? 1 : 0);
    }
    bump_version();
}

void BedOccupancyState::persist_last_occupied(const BedOccupancy& verdict,
                                              const std::optional<BedJobSummary>& job) {
    std::optional<BedJobSummary> record;
    if (verdict.occupied && job) {
        record = job;
    }
    const auto same = [](const std::optional<BedJobSummary>& a,
                         const std::optional<BedJobSummary>& b) {
        if (!a || !b) {
            return !a && !b;
        }
        return same_bed_job({a->job_id, a->start_time}, *b) && a->filename == b->filename &&
               a->object_height_mm == b->object_height_mm;
    };
    if (same(record, last_occupied_)) {
        return;
    }
    last_occupied_ = record;
    if (Config* config = Config::get_instance()) {
        config->set<std::string>(config_path(*config, kBedLastOccupiedKey),
                                 record ? bed_job_to_json(*record) : std::string());
        config->save();
    }
}

void BedOccupancyState::bump_version() {
    if (subjects_initialized_) {
        lv_subject_set_int(&version_, lv_subject_get_int(&version_) + 1);
    }
}

std::vector<BedObjectOutline> BedOccupancyState::outlines() const {
    const bool print_active = job_holds_machine(printer_state_.print_state().get_print_lifecycle());
    if (!current_.occupied && !print_active) {
        return {};
    }

    std::vector<BedObjectOutline> live = defined_outlines(printer_state_.excluded_objects_state());
    if (!live.empty() || print_active) {
        return live;
    }

    const auto newest = newest_job();
    if (snapshot_ && newest && snapshot_->filename == newest->filename) {
        return snapshot_->objects;
    }
    return {};
}

void BedOccupancyState::mark_cleared() {
    if (!current_.occupied) {
        return;
    }
    if (Config* config = Config::get_instance()) {
        BedJobSummary cleared;
        cleared.job_id = current_.job_id;
        cleared.start_time = current_.start_time;
        config->set<std::string>(config_path(*config, kBedClearedJobKey), bed_job_to_json(cleared));
        config->save();
    }
    spdlog::info("[BedOccupancy] User cleared the bed after job '{}'", current_.job_id);
    refresh();
}

void BedOccupancyState::on_history_changed() {
    refresh();
}

void BedOccupancyState::on_print_lifecycle(PrintState state) {
    const PrintState previous = last_lifecycle_;
    last_lifecycle_ = state;
    if (job_holds_machine(previous) && lifecycle_ends_print(state)) {
        capture_outline_snapshot();
    }
    refresh();
    // outlines() switches between live geometry and the snapshot on this, even when the
    // verdict stays the same.
    if (job_holds_machine(previous) != job_holds_machine(state)) {
        bump_version();
    }
}

void BedOccupancyState::capture_outline_snapshot() {
    BedOutlineSnapshot snapshot;
    snapshot.filename = printer_state_.print_state().get_effective_print_filename();
    snapshot.objects = defined_outlines(printer_state_.excluded_objects_state());
    if (Config* config = Config::get_instance()) {
        config->set<std::string>(config_path(*config, kBedOutlineSnapshotKey),
                                 bed_outline_snapshot_to_json(snapshot));
        config->save();
    }
    spdlog::info("[BedOccupancy] Captured {} object outline(s) for '{}'", snapshot.objects.size(),
                 snapshot.filename);
    snapshot_ = std::move(snapshot);
    bump_version();
}

} // namespace helix
