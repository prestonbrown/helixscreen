// SPDX-License-Identifier: GPL-3.0-or-later

#include "queued_job_start.h"

#include "ui_error_reporting.h"
#include "ui_filename_utils.h"
#include "ui_modal.h"
#include "ui_panel_print_select.h"
#include "ui_preflight_check_modal.h"
#include "ui_print_preparation_manager.h"
#include "ui_print_start_controller.h"

#include "app_globals.h"
#include "job_queue_start.h"
#include "job_queue_state.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <utility>

namespace helix::queue {

int saved_option_state(const QueuedJobOptions& saved, const std::string& option_id) {
    const auto it = saved.options.find(option_id);
    if (it == saved.options.end()) {
        return -1;
    }
    return it->second ? 1 : 0;
}

bool refuse_queued_start_while_busy(PrinterState& printer_state, const std::string& filename) {
    if (printer_state.print_state().can_start_new_print()) {
        return false;
    }
    NOTIFY_WARNING(lv_tr("Printer is busy - {} stays in the queue"), filename);
    return true;
}

void finish_started_queued_job(AsyncLifetimeGuard& lifetime, IMoonrakerAPI* api,
                               const std::string& job_id) {
    if (!api) {
        return;
    }
    spdlog::info("[QueuedJobStart] Print started for queued job {} - removing from queue", job_id);
    api->queue().remove_jobs(
        {job_id},
        lifetime.bg_cb("QueuedJobStart::queued_job_started",
                       [api, job_id]() {
                           // Options are deleted only once Moonraker confirms the removal:
                           // a refusal (or a stale id after a Moonraker restart) leaves the
                           // job queued, and Start next must still find its saved options.
                           delete_queued_job_options(api, job_id);
                           if (auto* jqs = get_job_queue_state()) {
                               jqs->fetch();
                           }
                       }),
        lifetime.bg_cb("QueuedJobStart::queued_job_removal_failed", [job_id](
                                                                        const MoonrakerError& err) {
            spdlog::warn("[QueuedJobStart] Removing queued job {} failed: {}", job_id, err.message);
            NOTIFY_WARNING(lv_tr("Could not remove the job from the queue"));
            if (auto* jqs = get_job_queue_state()) {
                jqs->fetch();
            }
        }));
}

// ============================================================================
// QueuedJobStarter
// ============================================================================

QueuedJobStarter::QueuedJobStarter(PrinterState& printer_state, IMoonrakerAPI* api)
    : printer_state_(printer_state), api_(api), prep_source_([this]() {
          return get_print_select_panel(printer_state_, api_)->print_preparation_manager();
      }),
      facts_loader_(api) {
    make_controller();
}

void QueuedJobStarter::make_controller() {
    controller_ = std::make_unique<ui::PrintStartController>(printer_state_, api_);
    // No detail view and no navigation: the user stays where the tap was.
    controller_->set_on_print_started([this]() {
        finish_started_queued_job(lifetime_, api_, job_.job_id);
        finish(Outcome::Started);
    });
    controller_->set_on_print_cancelled([this]() { finish(Outcome::NotStarted); });
    // A file the tile could not check is started from the file view; the
    // gates end this start themselves.
    controller_->set_on_file_unchecked(
        [this]() { open_in_file_view("its printer-stopping command check did not run"); });
}

void QueuedJobStarter::publish_in_flight() const {
    if (auto* jqs = get_job_queue_state()) {
        jqs->set_start_in_flight(in_flight_);
    }
}

QueuedJobStarter::~QueuedJobStarter() {
    stall_watchdog_.reset();
    release_prep();
}

void QueuedJobStarter::release_prep() {
    if (!prep_) {
        return;
    }
    prep_->when_ready_to_start({}, {}, {}, nullptr);
    prep_->set_start_option_override(nullptr);
    controller_->set_prep_manager(nullptr);
    prep_ = nullptr;
}

void QueuedJobStarter::abandon() {
    if (in_flight_) {
        spdlog::info("[QueuedJobStart] Dropping the start of queued job {}: the printer is going",
                     job_.job_id);
    }
    end_start();
    // Answers still owed by the old API (the options read, the queue removal)
    // must not land.
    lifetime_.invalidate();
    // Never called into: its owner is being destroyed.
    prep_ = nullptr;
    done_ = nullptr;
    api_ = nullptr;
    facts_loader_.set_api(nullptr);
    make_controller();
    publish_in_flight();
}

void QueuedJobStarter::set_api(IMoonrakerAPI* api) {
    if (in_flight_ || api == api_) {
        return;
    }
    api_ = api;
    controller_->set_api(api);
    facts_loader_.set_api(api);
}

bool QueuedJobStarter::begin(const JobQueueEntry& job, DoneCallback& done) {
    if (in_flight_ || !api_ || refuse_queued_start_while_busy(printer_state_, job.filename)) {
        spdlog::info("[QueuedJobStart] Not starting queued job {} ({})", job.job_id,
                     in_flight_ ? "a start is in flight" : (!api_ ? "no API" : "printer busy"));
        if (done) {
            done(Outcome::Refused);
        }
        return false;
    }
    in_flight_ = true;
    ++start_generation_;
    done_ = std::move(done);
    job_ = job;
    arm_stall_watchdog(kStallMs);
    publish_in_flight();
    return true;
}

void QueuedJobStarter::arm_stall_watchdog(uint32_t ms) {
    if (lv_timer_t* timer = stall_watchdog_.get()) {
        lv_timer_set_period(timer, ms);
        lv_timer_reset(timer);
        return;
    }
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<QueuedJobStarter*>(lv_timer_get_user_data(t));
            // LVGL deletes a finished one-shot itself.
            self->stall_watchdog_.release();
            self->give_up("its file checks did not answer in time");
        },
        ms, this);
    lv_timer_set_repeat_count(timer, 1);
    stall_watchdog_.reset(timer);
}

void QueuedJobStarter::give_up(const char* why) {
    if (!in_flight_) {
        return;
    }
    spdlog::warn("[QueuedJobStart] Not starting queued job {}: {}", job_.job_id, why);
    NOTIFY_WARNING(lv_tr("Could not check {} - it stays in the queue"), job_.filename);
    finish(Outcome::NotStarted);
}

void QueuedJobStarter::open_in_file_view(const char* why, bool explain) {
    if (!in_flight_) {
        return;
    }
    spdlog::info("[QueuedJobStart] Opening queued job {} in the file view: {}", job_.job_id, why);
    const JobQueueEntry job = job_;
    finish(Outcome::OpenedFileView);
    open_queued_job_in_file_view(job);
    if (explain) {
        NOTIFY_INFO(lv_tr("{} could not be checked from Home - review it and tap Print"),
                    job.filename);
    }
}

void QueuedJobStarter::start(const JobQueueEntry& job, DoneCallback done) {
    if (!begin(job, done)) {
        return;
    }
    spdlog::info("[QueuedJobStart] Starting queued job {} -> '{}'", job.job_id, job.filename);
    // A missing entry answers defaults: every option at its default.
    load_queued_job_options(lifetime_, api_, job.job_id,
                            [this, job, gen = start_generation_](QueuedJobOptions saved) {
                                if (in_flight_ && gen == start_generation_) {
                                    gather(job, std::move(saved));
                                }
                            });
}

void QueuedJobStarter::start_with_options(const JobQueueEntry& job, QueuedJobOptions saved,
                                          DoneCallback done) {
    if (!begin(job, done)) {
        return;
    }
    gather(job, std::move(saved));
}

void QueuedJobStarter::gather(const JobQueueEntry& job, QueuedJobOptions saved) {
    saved_ = std::move(saved);
    arm_stall_watchdog(kStallMs);

    auto pending = std::make_unique<Pending>();
    pending->job = job;
    pending->name = gcode::get_filename_basename(job.filename);
    pending->dir = gcode_dir_of(job.filename);
    pending_ = std::move(pending);

    prep_ = prep_source_();
    if (!prep_) {
        spdlog::error("[QueuedJobStart] No print preparation manager for queued job {}",
                      job.job_id);
        finish(Outcome::NotStarted);
        return;
    }
    controller_->set_prep_manager(prep_);

    const uint32_t gen = start_generation_;
    facts_loader_.load(
        job.filename,
        [this, gen](FileStartFacts facts) {
            if (gen == start_generation_ && pending_) {
                pending_->facts = std::move(facts);
                wait_for_scan();
            }
        },
        nullptr,
        [this, gen](const std::optional<FileMetadata>& metadata) {
            // A footer that cannot answer falls back to reading the whole file.
            if (gen == start_generation_ && in_flight_ && metadata) {
                arm_stall_watchdog(kStallMs +
                                   static_cast<uint32_t>(metadata->size / kSlowestReadBytesPerMs));
            }
        });
}

void QueuedJobStarter::wait_for_scan() {
    arm_stall_watchdog(kStallMs);
    // The operations scan answers the printer-stopping command check, and
    // macro-analysis option rows exist only once the analysis lands. The
    // answer is for this file's path, never another file the view opened.
    const uint32_t gen = start_generation_;
    prep_->when_ready_to_start(
        pending_->name, pending_->dir, file_version_of(pending_->facts->metadata),
        [this, gen](std::optional<PrinterStopCheck> check) {
            if (gen != start_generation_ || !pending_ || !pending_->facts) {
                return;
            }
            // The tile has no screen to show what an unread file holds, so a
            // file whose printer-stopping command check cannot run here (a
            // .3mf, unread macros, a failed read) is started from the file view.
            if (!check) {
                open_in_file_view("its file check kept going stale");
                return;
            }
            if (check->state == PrinterStopCheck::State::NotRun) {
                spdlog::info("[QueuedJobStart] Printer-stop check did not run: {}",
                             check->not_run_reason);
                open_in_file_view("its printer-stopping command check did not run");
                return;
            }
            stall_watchdog_.reset();
            auto pending = std::move(pending_);
            FileStartFacts facts = std::move(*pending->facts);
            run_checks(*pending, std::move(facts));
        });
}

void QueuedJobStarter::run_checks(const Pending& pending, FileStartFacts facts) {
    const JobQueueEntry& job = pending.job;
    auto start_gates = [this, name = pending.name, dir = pending.dir, facts]() {
        // Every option the job saved answers from the saved entry; the rest
        // take their defaults, as for a file opened fresh.
        prep_->set_start_option_override(
            [this](const std::string& id) { return saved_option_state(saved_, id); });
        controller_->set_file_facts(facts);
        controller_->set_file(name, dir, facts.palette.colors, "");
        controller_->initiate();
    };

    // A used tool mapped to an empty slot: the same modal a Print tap shows.
    const PreflightResult preflight =
        preflight_for(facts, prep_->enabled_options_need_material([this](const std::string& id) {
            return saved_option_state(saved_, id);
        }));
    if (!preflight.has_block()) {
        start_gates();
        return;
    }

    spdlog::info("[QueuedJobStart] Queued job {} maps a used tool to an empty slot", job.job_id);
    // The starter outlives every start, so the dialog answers only the start
    // that opened it: a finished or abandoned one has nothing left to drive.
    const auto current = [this, gen = start_generation_]() {
        return in_flight_ && gen == start_generation_;
    };
    auto chosen = std::make_shared<bool>(false);
    auto modal = std::make_unique<ui::PreflightCheckModal>();
    modal->set_checks(preflight);
    modal->set_on_force([current, chosen, start_gates]() {
        if (!current()) {
            return;
        }
        *chosen = true;
        start_gates();
    });
    // Remap edits the file view's mapping card, so the job goes there.
    modal->set_on_remap([this, current, chosen]() {
        if (!current()) {
            return;
        }
        *chosen = true;
        open_in_file_view("Remap chosen", false);
    });
    modal->set_on_closed([this, current, chosen]() {
        if (!current() || *chosen) {
            return;
        }
        finish(Outcome::NotStarted);
    });
    if (!Modal::show_owned(std::move(modal), lv_screen_active())) {
        finish(Outcome::NotStarted);
    }
}

void QueuedJobStarter::end_start() {
    in_flight_ = false;
    ++start_generation_;
    stall_watchdog_.reset();
    pending_.reset();
    facts_loader_.cancel();
}

void QueuedJobStarter::finish(Outcome outcome) {
    if (!in_flight_) {
        return;
    }
    end_start();
    release_prep();
    publish_in_flight();
    auto done = std::move(done_);
    done_ = nullptr;
    if (done) {
        done(outcome);
    }
}

namespace {
// Never destructed, for the reason src/ui/job_queue_start.cpp#queue_modal
// gives: its controller would tear down after lv_deinit().
QueuedJobStarter* s_starter = nullptr;
} // namespace

QueuedJobStarter& queued_job_starter() {
    if (!s_starter) {
        s_starter = new QueuedJobStarter(get_printer_state(), get_moonraker_api());
    }
    s_starter->set_api(get_moonraker_api());
    return *s_starter;
}

void abandon_queued_job_start() {
    if (s_starter) {
        s_starter->abandon();
    }
}

} // namespace helix::queue
