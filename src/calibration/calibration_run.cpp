// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_run.h"

#include <spdlog/spdlog.h>

#include <algorithm>

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
        }));
    }
}

CalibrationRun::~CalibrationRun() {
    // A step still running would report into a destroyed run.
    // Settled first, so its Aborted report starts nothing.
    const bool running = phase_ == RunPhase::Running && current_ >= 0;
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
    changed();
    advance();
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
    Entry& running = entries_[i];
    spdlog::info("[CalibrationRun] Stopping during step {} ({})", i + 1, running.step->info().id);
    // Settle the run first, so the step's own Aborted report is ignored.
    running.status = StepStatus::Aborted;
    for (size_t j = i + 1; j < entries_.size(); ++j) {
        entries_[j].status = StepStatus::NotRun;
    }
    phase_ = RunPhase::Results;
    running.step->abort();
    if (running.step->abort_restarted_firmware()) {
        // A result the firmware persisted itself survives the restart.
        results_lost_ = std::any_of(entries_.begin(), entries_.end(), [](const Entry& e) {
            return e.status == StepStatus::Finished && !e.step->restores_on_discard();
        });
        if (results_lost_) {
            spdlog::warn("[CalibrationRun] The abort restarted the firmware: {} finished "
                         "result(s) are no longer pending",
                         finished_count());
        }
    }
    changed();
}

void CalibrationRun::finish_steps() {
    phase_ = RunPhase::Results;
    spdlog::info("[CalibrationRun] All steps ended, {} finished", finished_count());
    changed();
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
    // Only a finished step's pending config needs the restart; an aborted one
    // already restarted, a failed or skipped one staged nothing, and a restored
    // one never was pending.
    const bool pending =
        !results_lost_ && std::any_of(entries_.begin(), entries_.end(), [](const Entry& e) {
            return e.status == StepStatus::Finished && !e.step->restores_on_discard();
        });
    if (!pending) {
        phase_ = RunPhase::Discarded;
        changed();
        return;
    }
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

} // namespace helix::calibration
