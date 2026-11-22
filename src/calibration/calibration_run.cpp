// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_run.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace helix::calibration {

namespace {

bool in_guided_run(StepKind kind) {
    switch (kind) {
    case StepKind::ZOffset:
    case StepKind::Heaters:
    case StepKind::BedMesh:
    case StepKind::InputShaper:
        return true;
    default:
        return false;
    }
}

/// Hands-on first, then the order the catalog already ranks them in.
int run_rank(StepKind kind) {
    switch (kind) {
    case StepKind::ZOffset:
        return 0;
    case StepKind::Heaters:
        return 1;
    case StepKind::BedMesh:
        return 2;
    default:
        return 3;
    }
}

const char* status_name(StepStatus s) {
    switch (s) {
    case StepStatus::Pending:
        return "pending";
    case StepStatus::Running:
        return "running";
    case StepStatus::Finished:
        return "finished";
    case StepStatus::Failed:
        return "failed";
    case StepStatus::Skipped:
        return "skipped";
    case StepStatus::Aborted:
        return "aborted";
    case StepStatus::NotRun:
        return "not run";
    }
    return "";
}

} // namespace

std::vector<StepInfo> guided_run_steps(const std::vector<StepInfo>& applicable) {
    std::vector<StepInfo> out;
    for (const auto& s : applicable) {
        if (in_guided_run(s.kind) && s.tier != Tier::OnRequest) {
            out.push_back(s);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const StepInfo& a, const StepInfo& b) {
        return run_rank(a.kind) < run_rank(b.kind);
    });
    return out;
}

bool picked_by_default(const StepInfo& step) {
    return step.tier == Tier::Required || step.tier == Tier::Recommended;
}

int heating_minutes(float heating_seconds) {
    if (!(heating_seconds > 0.0f)) {
        return 0;
    }
    return static_cast<int>(std::ceil(heating_seconds / 60.0f));
}

StepInfo with_run_options(StepInfo step, const RunOptions& options) {
    if (step.kind == StepKind::ZOffset && options.zoffset_warm_bed) {
        step.minutes += std::max(0, options.zoffset_warm_bed_minutes);
        step.heats = true;
    }
    if (step.kind == StepKind::Heaters && step.bed == BedTuning::Optional && options.heaters_bed &&
        !step.tunes_bed) {
        step.tunes_bed = true;
        step.minutes += step.bed_minutes;
    }
    return step;
}

std::vector<PrereqLine> prereq_lines(const std::vector<StepInfo>& picked) {
    std::vector<PrereqLine> out;
    for (Prereq p : kPrereqOrder) {
        PrereqLine line{p, {}};
        for (const auto& s : picked) {
            if (s.prereqs & p) {
                line.for_steps.push_back(&s);
            }
        }
        if (!line.for_steps.empty()) {
            out.push_back(std::move(line));
        }
    }
    return out;
}

CalibrationRun::CalibrationRun(IMoonrakerAPI& api,
                               std::vector<std::unique_ptr<CalibrationStep>> steps,
                               std::function<void()> on_change)
    : api_(api), on_change_(std::move(on_change)) {
    entries_.reserve(steps.size());
    contexts_.reserve(steps.size());
    for (size_t i = 0; i < steps.size(); ++i) {
        Entry e;
        e.step = std::move(steps[i]);
        entries_.push_back(std::move(e));
        contexts_.push_back(std::make_unique<StepContext>(StepContext{
            api_,
            [this, i](const StepProgress& p) { on_step_progress(i, p); },
            [this, i](StepOutcome o) { on_step_done(i, std::move(o)); },
            [this, i]() {
                if (phase_ == RunPhase::Running && static_cast<int>(i) == current_) {
                    stop();
                }
            },
            static_cast<int>(i),
            static_cast<int>(steps.size()),
        }));
    }
}

CalibrationRun::~CalibrationRun() {
    // A step still running would report into a destroyed run.
    // Settled first, so its Aborted report starts nothing.
    if (busy_watch_) {
        busy_watch_->cancel();
    }
    const bool running = phase_ == RunPhase::Running && current_ >= 0 && !waiting_;
    phase_ = RunPhase::Discarded;
    if (running) {
        entries_[static_cast<size_t>(current_)].step->abort();
    }
    entries_.clear();
}

void CalibrationRun::start() {
    if (phase_ != RunPhase::Ready) {
        return;
    }
    spdlog::info("[CalibrationRun] Starting {} step(s)", entries_.size());
    phase_ = RunPhase::Running;
    current_ = -1;
    advance();
}

void CalibrationRun::advance() {
    // A step may report done from inside its own start(); the loop runs the
    // next one instead of nesting.
    if (advancing_) {
        advance_again_ = true;
        return;
    }
    advancing_ = true;
    do {
        advance_again_ = false;
        start_next();
    } while (advance_again_ && phase_ == RunPhase::Running);
    advancing_ = false;
}

void CalibrationRun::start_next() {
    ++current_;
    if (current_ >= static_cast<int>(entries_.size())) {
        finish_steps();
        return;
    }
    const auto i = static_cast<size_t>(current_);
    Entry& e = entries_[i];
    e.status = StepStatus::Running;
    e.progress = {};
    spdlog::info("[CalibrationRun] Step {} ({}) starting", i + 1, e.step->info().id);
    changed();
    e.step->start(*contexts_[i]);
}

void CalibrationRun::on_step_progress(size_t index, const StepProgress& progress) {
    if (phase_ != RunPhase::Running || static_cast<int>(index) != current_) {
        return;
    }
    entries_[index].progress = progress;
    changed();
}

void CalibrationRun::on_step_done(size_t index, StepOutcome outcome) {
    if (phase_ != RunPhase::Running || static_cast<int>(index) != current_) {
        return;
    }
    Entry& e = entries_[index];
    switch (outcome.kind) {
    case StepOutcome::Kind::Finished:
        e.status = StepStatus::Finished;
        e.result = std::move(outcome.result);
        break;
    case StepOutcome::Kind::Failed:
        e.status = StepStatus::Failed;
        e.message = std::move(outcome.message);
        // What it staged before failing, which a save writes too.
        e.result = std::move(outcome.result);
        break;
    case StepOutcome::Kind::Skipped:
        e.status = StepStatus::Skipped;
        break;
    case StepOutcome::Kind::Aborted:
        e.status = StepStatus::Aborted;
        break;
    }
    spdlog::info("[CalibrationRun] Step {} ({}) {}{}", index + 1, e.step->info().id,
                 status_name(e.status), e.message.empty() ? "" : ": " + e.message);
    go_on_after(index);
}

void CalibrationRun::go_on_after(size_t index) {
    const Entry& e = entries_[index];
    if (klipper_left_ready_) {
        // Klipper went down while this step ran, maybe for a heater fault.
        end_early(EndNote::KlipperStopped);
        return;
    }
    if (e.status != StepStatus::Failed || !busy_watch_ || !busy_watch_->busy()) {
        changed();
        advance();
        return;
    }
    // The failed step's command may still be running in Klipper: anything the
    // next step sends would queue behind it and time out there.
    spdlog::info("[CalibrationRun] Step {} ({}) failed with the printer still busy; waiting "
                 "for it to finish before going on",
                 index + 1, e.step->info().id);
    waiting_ = true;
    printer_finishing_ = true;
    changed();
    busy_watch_->when_idle(
        [this, index]() { printer_went_idle(index); }, wait_ceiling_ms(e.step->info()),
        [this, index](bool still_busy) {
            spdlog::warn("[CalibrationRun] Gave up waiting for the printer ({})",
                         still_busy ? "still busy" : "state unreadable");
            printer_finishing_ = still_busy;
            end_early(still_busy ? EndNote::PrinterStayedBusy : EndNote::PrinterLost);
            if (still_busy) {
                // The run has ended, but Save stays shut only while the printer
                // is busy: keep watching, with no ceiling, to open it again.
                busy_watch_->when_idle([this, index]() { printer_went_idle(index); },
                                       std::numeric_limits<uint32_t>::max(), [](bool) {});
            }
        });
}

void CalibrationRun::printer_went_idle(size_t index) {
    spdlog::info("[CalibrationRun] The printer is idle");
    std::weak_ptr<int> alive = alive_;
    entries_[index].step->recheck_after_wait(
        api_, [this, alive, index](std::optional<StepOutcome> late) {
            if (alive.expired()) { // L081_OK: steps call back on the main thread
                return;
            }
            printer_finishing_ = false;
            Entry& e = entries_[index];
            if (late && e.status == StepStatus::Failed) {
                spdlog::info("[CalibrationRun] Step {} ({}) staged its result after all", index + 1,
                             e.step->info().id);
                e.status = late->kind == StepOutcome::Kind::Finished ? StepStatus::Finished
                                                                     : StepStatus::Failed;
                e.result = std::move(late->result);
                e.message = std::move(late->message);
            }
            if (waiting_ && phase_ == RunPhase::Running) {
                if (klipper_left_ready_) {
                    end_early(EndNote::KlipperStopped);
                    return;
                }
                waiting_ = false;
                changed();
                advance();
                return;
            }
            changed();
        });
}

void CalibrationRun::end_early(EndNote note) {
    if (phase_ != RunPhase::Running) {
        changed();
        return;
    }
    waiting_ = false;
    end_note_ = note;
    for (size_t j = static_cast<size_t>(current_) + 1; j < entries_.size(); ++j) {
        entries_[j].status = StepStatus::NotRun;
    }
    phase_ = RunPhase::Results;
    changed();
}

uint32_t wait_ceiling_ms(const StepInfo& step) {
    constexpr uint32_t kMinuteMs = 60u * 1000u;
    return (2u * static_cast<uint32_t>(std::max(0, step.minutes)) + 30u) * kMinuteMs;
}

void CalibrationRun::stop() {
    if (phase_ == RunPhase::Ready) {
        for (auto& e : entries_) {
            e.status = StepStatus::NotRun;
        }
        phase_ = RunPhase::Results;
        changed();
        return;
    }
    if (phase_ != RunPhase::Running) {
        return;
    }
    const auto i = static_cast<size_t>(current_);
    if (waiting_) {
        // Nothing of the run's own is running: the failed step keeps its result,
        // and the watch keeps watching, so Save waits for the printer.
        spdlog::info("[CalibrationRun] Stopping while waiting for the printer after step {}",
                     i + 1);
        waiting_ = false;
        for (size_t j = i + 1; j < entries_.size(); ++j) {
            entries_[j].status = StepStatus::NotRun;
        }
        phase_ = RunPhase::Results;
        changed();
        return;
    }
    Entry& running = entries_[i];
    spdlog::info("[CalibrationRun] Stopping during step {} ({})", i + 1, running.step->info().id);
    // Settle the run first, so the step's own Aborted report is ignored.
    running.status = StepStatus::Aborted;
    for (size_t j = i + 1; j < entries_.size(); ++j) {
        entries_[j].status = StepStatus::NotRun;
    }
    phase_ = RunPhase::Results;
    const bool restarts = running.step->abort_restarts_firmware();
    running.step->abort();
    if (restarts) {
        results_lost_ = has_pending_result();
        if (results_lost_) {
            spdlog::warn("[CalibrationRun] The abort restarted the firmware: {} finished "
                         "result(s) are no longer pending",
                         finished_count());
        }
    }
    changed();
}

CalibrationRun::StopEffect CalibrationRun::stop_effect() const {
    if (phase_ != RunPhase::Running || current_ < 0) {
        return StopEffect::EndsStep;
    }
    if (waiting_) {
        return StopEffect::PrinterFinishes;
    }
    if (!entries_[static_cast<size_t>(current_)].step->abort_restarts_firmware()) {
        return StopEffect::EndsStep;
    }
    return has_pending_result() ? StopEffect::RestartDropsResults : StopEffect::Restarts;
}

void CalibrationRun::finish_steps() {
    phase_ = RunPhase::Results;
    spdlog::info("[CalibrationRun] All steps ended, {} finished", finished_count());
    changed();
}

bool CalibrationRun::saves(const Entry& e) {
    return (e.status == StepStatus::Finished ||
            (e.status == StepStatus::Failed && !e.result.summary.empty())) &&
           !e.step->restores_on_discard();
}

bool CalibrationRun::has_pending_result() const {
    return std::any_of(entries_.begin(), entries_.end(), [](const Entry& e) { return saves(e); });
}

bool CalibrationRun::can_discard() const {
    if ((phase_ != RunPhase::Results && phase_ != RunPhase::SaveFailed) || printer_finishing_) {
        return false;
    }
    return can_save() || std::any_of(entries_.begin(), entries_.end(), [](const Entry& e) {
               return e.status == StepStatus::Finished && e.step->restores_on_discard();
           });
}

void CalibrationRun::klippy_restarted() {
    // The command the run waited on died with Klipper. Never go on to the next
    // step from here: the restart may follow a heater fault.
    if (phase_ == RunPhase::Running) {
        // A step still running ends the run when it reports, instead of going on.
        klipper_left_ready_ = true;
    }
    if (printer_finishing_ || waiting_) {
        spdlog::warn("[CalibrationRun] Klipper stopped while the printer finished a failed step");
        busy_watch_->cancel();
        printer_finishing_ = false;
        if (waiting_) {
            end_early(EndNote::KlipperStopped);
        }
    }
    // The restarts Save all and Discard all cause are their own; a stop's has
    // already marked what it lost.
    if (phase_ == RunPhase::Ready || phase_ == RunPhase::Saving || phase_ == RunPhase::Saved ||
        phase_ == RunPhase::Discarding || phase_ == RunPhase::Discarded || results_lost_) {
        return;
    }
    // A shutdown or a restart from elsewhere drops everything pending.
    if (has_pending_result()) {
        results_lost_ = true;
        spdlog::warn("[CalibrationRun] Klipper restarted outside the run: the staged results "
                     "are no longer pending");
        changed();
    }
}

int CalibrationRun::finished_count() const {
    return static_cast<int>(std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) {
        return e.status == StepStatus::Finished;
    }));
}

void CalibrationRun::save_all(RunFinisher& finisher) {
    if (!can_save()) {
        return;
    }
    phase_ = RunPhase::Saving;
    error_.clear();
    changed();
    std::weak_ptr<int> alive = alive_;
    finisher.save_all(
        [this, alive, &finisher]() {
            if (alive.expired() ||
                phase_ != RunPhase::Saving) { // L081_OK: RunFinisher calls back on the main thread
                return;
            }
            for (const auto& e : entries_) {
                if (e.status == StepStatus::Finished) {
                    finisher.record(e.step->info(), e.result);
                }
            }
            phase_ = RunPhase::Saved;
            spdlog::info("[CalibrationRun] Saved {} result(s)", finished_count());
            changed();
        },
        [this, alive](const std::string& why) {
            if (alive.expired() ||
                phase_ != RunPhase::Saving) { // L081_OK: RunFinisher calls back on the main thread
                return;
            }
            error_ = why;
            phase_ = RunPhase::SaveFailed;
            spdlog::warn("[CalibrationRun] Save failed: {}", why);
            changed();
        });
}

void CalibrationRun::discard_all(RunFinisher& finisher) {
    if (phase_ != RunPhase::Results && phase_ != RunPhase::SaveFailed) {
        return;
    }
    phase_ = RunPhase::Discarding;
    error_.clear();
    changed();
    restore_from(0, finisher);
}

void CalibrationRun::restore_from(size_t from, RunFinisher& finisher) {
    std::weak_ptr<int> alive = alive_;
    for (size_t i = from; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        if (e.status != StepStatus::Finished || !e.step->restores_on_discard()) {
            continue;
        }
        spdlog::info("[CalibrationRun] Restoring {} before discarding", e.step->info().id);
        e.step->restore(api_, [this, alive, i, &finisher](std::string note) {
            if (alive.expired() ||
                phase_ != RunPhase::Discarding) { // L081_OK: steps call back on the main thread
                return;
            }
            entries_[i].restore_note = std::move(note);
            changed();
            restore_from(i + 1, finisher);
        });
        return;
    }
    // What is pending, not what the steps say: a failed step can have staged
    // part of its result before it failed.
    finisher.read_pending([this, alive, &finisher](bool any_pending) {
        if (alive.expired() ||
            phase_ != RunPhase::Discarding) { // L081_OK: RunFinisher calls back on the main thread
            return;
        }
        if (!any_pending) {
            phase_ = RunPhase::Discarded;
            spdlog::info("[CalibrationRun] Nothing pending: discarded without a restart");
            changed();
            return;
        }
        restart_to_discard(finisher);
    });
}

void CalibrationRun::restart_to_discard(RunFinisher& finisher) {
    std::weak_ptr<int> alive = alive_;
    finisher.discard_all(
        [this, alive]() {
            if (alive.expired() ||
                phase_ !=
                    RunPhase::Discarding) { // L081_OK: RunFinisher calls back on the main thread
                return;
            }
            phase_ = RunPhase::Discarded;
            spdlog::info("[CalibrationRun] Discarded the run's results");
            changed();
        },
        [this, alive](const std::string& why) {
            if (alive.expired() ||
                phase_ !=
                    RunPhase::Discarding) { // L081_OK: RunFinisher calls back on the main thread
                return;
            }
            // Still pending in Klipper: back to the choice, with the reason.
            error_ = why;
            phase_ = RunPhase::Results;
            spdlog::warn("[CalibrationRun] Discard failed: {}", why);
            changed();
        });
}

void CalibrationRun::changed() {
    if (on_change_) {
        on_change_();
    }
}

RunTally run_tally(const std::vector<CalibrationRun::Entry>& entries) {
    RunTally t;
    t.total = static_cast<int>(entries.size());
    t.finished = static_cast<int>(
        std::count_if(entries.begin(), entries.end(), [](const CalibrationRun::Entry& e) {
            return e.status == StepStatus::Finished;
        }));
    return t;
}

RunSighting sight(const CalibrationRun& run) {
    RunSighting s;
    s.phase = run.phase();
    s.current = run.current();
    const auto& entries = run.entries();
    if (s.current >= 0) {
        s.needs_hands = entries[static_cast<size_t>(s.current)].step->info().needs_hands;
    }
    for (const auto& e : entries) {
        if (e.status != StepStatus::Pending && e.status != StepStatus::Running &&
            e.status != StepStatus::Finished) {
            ++s.unfinished;
        }
    }
    return s;
}

bool calls_for_owner(const RunSighting& before, const RunSighting& now) {
    if (before.phase == RunPhase::Running && now.phase == RunPhase::Results) {
        return true;
    }
    if (now.phase != RunPhase::Running) {
        return false;
    }
    if (now.unfinished > before.unfinished) {
        return true;
    }
    return now.needs_hands && now.current != before.current;
}

int minutes_left(const CalibrationRun::Entry& entry) {
    const int minutes = entry.step->info().minutes;
    switch (entry.status) {
    case StepStatus::Pending:
        return minutes;
    case StepStatus::Running:
        return entry.progress.percent < 0
                   ? minutes
                   : std::max(1, (minutes * (100 - entry.progress.percent) + 99) / 100);
    default:
        return 0;
    }
}

int minutes_left(const std::vector<CalibrationRun::Entry>& entries) {
    int left = 0;
    for (const auto& e : entries) {
        left += minutes_left(e);
    }
    return left;
}

} // namespace helix::calibration
