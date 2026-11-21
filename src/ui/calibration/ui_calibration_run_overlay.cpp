// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_calibration_run_overlay.h"

#include "ui_calibration_hub_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "calibration_catalog_inputs.h"
#include "calibration_history_store.h"
#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "core/static_subject_registry.h"
#include "first_run_tour.h"
#include "i_moonraker_api.h"
#include "klippy_state.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::ui {

namespace {

using calibration::RunPhase;
using calibration::StepInfo;
using calibration::StepStatus;

int step_index(const char* id) {
    for (size_t i = 0; i < kRunStepIds.size(); ++i) {
        if (std::strcmp(kRunStepIds[i], id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string join_names(const std::vector<const StepInfo*>& steps) {
    std::string out;
    for (const auto* s : steps) {
        if (!out.empty()) {
            out += ", ";
        }
        out += lv_tr(s->name);
    }
    return out;
}

/// "5 min · needs you · heats · loud".
std::string meta_line(const StepInfo& s) {
    std::string out = fmt::format(fmt::runtime(lv_tr("{} min")), s.minutes);
    out += " · ";
    out += s.needs_hands ? lv_tr("needs you") : lv_tr("automatic");
    if (s.prereqs & calibration::kPrereqPaper) {
        out += " · ";
        out += lv_tr("paper test");
    }
    if (s.heats) {
        out += " · ";
        out += lv_tr("heats");
    }
    if (s.loud) {
        out += " · ";
        out += lv_tr("loud");
    }
    return out;
}

/// What the printer had before the run: the step's last result, else what its
/// config holds.
std::string before_text(const StepInfo& s) {
    if (const auto* last = calibration::CalibrationHistory::instance().entry(s.id)) {
        if (!last->summary.empty()) {
            return last->summary;
        }
    }
    const auto& in_config = get_printer_state().get_discovery().calibration_in_config();
    const auto found = in_config.find(s.id);
    return found != in_config.end() ? found->second : lv_tr("Not set");
}

/// The row dot: 0 waiting, 1 running, 2 finished, 3 did not finish.
int dot_state(StepStatus status) {
    switch (status) {
    case StepStatus::Pending:
        return 0;
    case StepStatus::Running:
        return 1;
    case StepStatus::Finished:
        return 2;
    default:
        return 3;
    }
}

std::string entry_status_text(const calibration::CalibrationRun::Entry& e) {
    switch (e.status) {
    case StepStatus::Pending:
        return fmt::format(fmt::runtime(lv_tr("about {} min")), e.step->info().minutes);
    case StepStatus::Running:
        return e.progress.phase;
    case StepStatus::Finished:
        return e.result.summary;
    case StepStatus::Failed:
        if (!e.result.summary.empty()) {
            // What it staged before failing is saved with the rest.
            return fmt::format(fmt::runtime(lv_tr("Failed: {}. Saves {}")), e.message,
                               e.result.summary);
        }
        return fmt::format(fmt::runtime(lv_tr("Failed: {}")), e.message);
    case StepStatus::Skipped:
        return lv_tr("Skipped");
    case StepStatus::Aborted:
        return lv_tr("Stopped");
    case StepStatus::NotRun:
        return lv_tr("Not run");
    }
    return {};
}

const char* warning_for(calibration::StepKind kind) {
    switch (kind) {
    case calibration::StepKind::Heaters:
        return lv_tr("Do not touch the nozzle or the bed.");
    case calibration::StepKind::BedMesh:
        return lv_tr("The toolhead moves. Keep hands clear.");
    case calibration::StepKind::InputShaper:
        return lv_tr("It is loud. This is normal.");
    default:
        return "";
    }
}

} // namespace

CalibrationRunOverlay& get_calibration_run_overlay() {
    return helix::lazy_global<CalibrationRunOverlay>("CalibrationRunOverlay");
}

void CalibrationRunOverlay::add_int(helix::Subject& subject, int initial, std::string name) {
    names_.push_back(std::move(name));
    UI_MANAGED_SUBJECT_INT(subject, initial, names_.back().c_str(), subjects_);
}

void CalibrationRunOverlay::add_string(helix::Subject& subject, char* buf, size_t size,
                                       std::string name) {
    names_.push_back(std::move(name));
    UI_MANAGED_SUBJECT_STRING_N(subject, buf, size, "", names_.back().c_str(), subjects_);
}

void CalibrationRunOverlay::set_text(helix::Subject& subject, const std::string& text) {
    lv_subject_set_string(&subject, text.c_str());
}

void CalibrationRunOverlay::init_subjects() {
    add_int(page_subject_, 0, "cal_run_page");
    add_int(pick_count_, 0, "cal_run_pick_count");
    add_string(pick_summary_, pick_summary_buf_, kShortLen, "cal_run_pick_summary");
    add_string(hands_line_, hands_line_buf_, kLongLen, "cal_run_hands_line");
    for (size_t i = 0; i < kSteps; ++i) {
        const std::string id = kRunStepIds[i];
        add_int(shown_[i], 0, "cal_run_shown_" + id);
        add_int(picked_subj_[i], 0, "cal_run_picked_" + id);
        add_int(locked_[i], 0, "cal_run_locked_" + id);
        add_string(meta_[i], meta_bufs_[i], kShortLen, "cal_run_meta_" + id);
        add_string(last_[i], last_bufs_[i], kShortLen, "cal_run_last_" + id);
        add_int(in_run_[i], 0, "cal_run_in_run_" + id);
        add_int(state_[i], 0, "cal_run_state_" + id);
        add_string(detail_[i], detail_bufs_[i], kLongLen, "cal_run_detail_" + id);
        add_string(before_subj_[i], before_bufs_[i], kShortLen, "cal_run_before_" + id);
        add_string(after_[i], after_bufs_[i], kLongLen, "cal_run_after_" + id);
    }
    for (size_t i = 0; i < kPrereqs; ++i) {
        const std::string id = kRunPrereqIds[i];
        add_int(prereq_shown_[i], 0, "cal_prereq_shown_" + id);
        add_int(prereq_checked_[i], 0, "cal_prereq_checked_" + id);
        add_int(prereq_detected_[i], 0, "cal_prereq_detected_" + id);
        add_string(prereq_for_[i], prereq_for_bufs_[i], kLongLen, "cal_prereq_for_" + id);
    }
    add_int(prereq_ready_, 0, "cal_run_prereq_ready");
    add_string(prereq_left_, prereq_left_buf_, kShortLen, "cal_run_prereq_left");
    add_int(has_hands_, 0, "cal_run_has_hands");
    add_string(you_line_, you_line_buf_, kShortLen, "cal_run_you_line");
    add_string(away_line_, away_line_buf_, kShortLen, "cal_run_away_line");
    add_string(away_steps_, away_steps_buf_, kLongLen, "cal_run_away_steps");
    add_string(title_, title_buf_, kShortLen, "cal_run_title");
    add_string(phase_text_, phase_text_buf_, kLongLen, "cal_run_phase");
    add_int(progress_, 0, "cal_run_progress");
    add_int(indeterminate_, 1, "cal_run_indeterminate");
    add_int(over_, 0, "cal_run_over");
    add_int(live_heat_, 0, "cal_run_live_heat");
    add_int(has_bed_, 0, "cal_run_has_bed");
    add_string(warn_, warn_buf_, kShortLen, "cal_run_warn");
    add_string(footer_, footer_buf_, kShortLen, "cal_run_footer");
    add_int(run_phase_, 0, "cal_run_result_phase");
    add_int(can_save_, 0, "cal_run_can_save");
    add_string(results_sub_, results_sub_buf_, kLongLen, "cal_run_results_sub");
    add_string(save_line_, save_line_buf_, kLongLen, "cal_run_save_line");
    add_int(has_error_, 0, "cal_run_has_error");
    add_string(error_, error_buf_, kLongLen, "cal_run_error");

    // Runs from StaticSubjectRegistry::deinit_all(), which may come after
    // StaticPanelRegistry has destroyed the overlay.
    StaticSubjectRegistry::instance().register_deinit("CalibrationRunOverlay", []() {
        if (auto* self = helix::lazy_global_if_exists<CalibrationRunOverlay>()) {
            self->klippy_obs_.reset();
            self->run_.reset();
            self->subjects_.deinit_all();
            self->subjects_initialized_ = false;
        }
    });
}

bool CalibrationRunOverlay::run_pending() const {
    if (!run_) {
        return false;
    }
    const RunPhase p = run_->phase();
    return p != RunPhase::Saved && p != RunPhase::Discarded;
}

void CalibrationRunOverlay::open() {
    if (!subjects_initialized_) {
        init_subjects();
        subjects_initialized_ = true;
    }
    if (!run_pending()) {
        run_.reset();
        prepare_pick();
        set_page(RunPage::Pick);
    }
    publish();
    show(lv_screen_active());
}

void CalibrationRunOverlay::on_activate() {
    OverlayBase::on_activate();
    // Back from the Z offset screen, or reopened while a run waits.
    publish();
}

void CalibrationRunOverlay::on_dismissed() {
    tour::FirstRunTour::instance().release_auto_start();
}

void CalibrationRunOverlay::prepare_pick() {
    candidates_ =
        calibration::guided_run_steps(calibration::current_printer_steps(get_printer_state()));
    picked_.fill(false);
    ticked_.fill(false);
    for (const auto& s : candidates_) {
        const int i = step_index(s.id);
        if (i >= 0) {
            picked_[static_cast<size_t>(i)] = calibration::picked_by_default(s);
        }
    }
    spdlog::debug("[{}] {} step(s) offered", get_name(), candidates_.size());
}

std::vector<StepInfo> CalibrationRunOverlay::picked_steps() const {
    std::vector<StepInfo> out;
    for (const auto& s : candidates_) {
        const int i = step_index(s.id);
        if (i >= 0 && picked_[static_cast<size_t>(i)]) {
            out.push_back(s);
        }
    }
    return out;
}

void CalibrationRunOverlay::set_page(RunPage page) {
    page_ = page;
    lv_subject_set_int(&page_subject_, static_cast<int>(page));
}

void CalibrationRunOverlay::publish() {
    publish_pick();
    publish_prereqs();
    publish_run();
}

void CalibrationRunOverlay::publish_pick() {
    for (size_t i = 0; i < kSteps; ++i) {
        const StepInfo* step = nullptr;
        for (const auto& s : candidates_) {
            if (std::strcmp(s.id, kRunStepIds[i]) == 0) {
                step = &s;
            }
        }
        lv_subject_set_int(&shown_[i], step ? 1 : 0);
        if (!step) {
            continue;
        }
        const bool locked = step->tier == calibration::Tier::Required;
        if (locked) {
            picked_[i] = true;
        }
        lv_subject_set_int(&picked_subj_[i], picked_[i] ? 1 : 0);
        lv_subject_set_int(&locked_[i], locked ? 1 : 0);
        set_text(meta_[i], meta_line(*step));
        const auto& in_config = get_printer_state().get_discovery().calibration_in_config();
        const auto found = in_config.find(step->id);
        set_text(last_[i],
                 tile_status(*step, calibration::CalibrationHistory::instance().entry(step->id),
                             found != in_config.end() ? &found->second : nullptr)
                     .text);
    }
    const auto picked = picked_steps();
    const int n = static_cast<int>(picked.size());
    const int minutes = calibration::total_minutes(picked);
    const int hands = calibration::hands_on_minutes(picked);
    lv_subject_set_int(&pick_count_, n);
    set_text(pick_summary_,
             n == 1 ? fmt::format(fmt::runtime(lv_tr("1 step · about {} min")), minutes)
                    : fmt::format(fmt::runtime(lv_tr("{} steps · about {} min")), n, minutes));
    set_text(hands_line_,
             hands > 0
                 ? fmt::format(fmt::runtime(lv_tr("You: about {} min at the start, then it runs "
                                                  "on its own")),
                               hands)
                 : std::string(lv_tr("It runs on its own")));
}

void CalibrationRunOverlay::publish_prereqs() {
    const auto picked = picked_steps();
    const auto lines = calibration::prereq_lines(picked);
    int missing = 0;
    for (size_t i = 0; i < kPrereqs; ++i) {
        const calibration::Prereq p = calibration::kPrereqOrder[i];
        const calibration::PrereqLine* line = nullptr;
        for (const auto& l : lines) {
            if (l.prereq == p) {
                line = &l;
            }
        }
        lv_subject_set_int(&prereq_shown_[i], line ? 1 : 0);
        // The run offers input shaping only where discovery found an accelerometer.
        const bool detected = p == calibration::kPrereqAccelerometer && line;
        if (detected) {
            ticked_[i] = true;
        }
        lv_subject_set_int(&prereq_detected_[i], detected ? 1 : 0);
        lv_subject_set_int(&prereq_checked_[i], ticked_[i] ? 1 : 0);
        if (line) {
            set_text(prereq_for_[i],
                     fmt::format(fmt::runtime(lv_tr("For: {}")), join_names(line->for_steps)));
            if (!ticked_[i]) {
                ++missing;
            }
        }
    }
    lv_subject_set_int(&prereq_ready_, missing == 0 && !picked.empty() ? 1 : 0);
    set_text(prereq_left_, missing == 0
                               ? std::string(lv_tr("Ready"))
                               : fmt::format(fmt::runtime(lv_tr("{} left to check")), missing));

    const int hands = calibration::hands_on_minutes(picked);
    std::vector<const StepInfo*> away;
    for (const auto& s : picked) {
        if (!s.needs_hands) {
            away.push_back(&s);
        }
    }
    lv_subject_set_int(&has_hands_, hands > 0 ? 1 : 0);
    set_text(you_line_, fmt::format(fmt::runtime(lv_tr("You, about {} min")), hands));
    set_text(away_line_, fmt::format(fmt::runtime(lv_tr("Then walk away, about {} min")),
                                     calibration::total_minutes(picked) - hands));
    set_text(away_steps_, join_names(away));
}

void CalibrationRunOverlay::publish_run() {
    lv_subject_set_int(&has_bed_, get_printer_state().get_discovery().has_heater_bed() ? 1 : 0);
    if (!run_) {
        for (size_t i = 0; i < kSteps; ++i) {
            lv_subject_set_int(&in_run_[i], 0);
        }
        lv_subject_set_int(&run_phase_, static_cast<int>(RunPhase::Ready));
        lv_subject_set_int(&over_, 0);
        return;
    }
    const auto& entries = run_->entries();
    for (size_t i = 0; i < kSteps; ++i) {
        lv_subject_set_int(&in_run_[i], 0);
    }
    int remaining_min = 0;
    for (const auto& e : entries) {
        const int i = step_index(e.step->info().id);
        if (i < 0) {
            continue;
        }
        const auto k = static_cast<size_t>(i);
        lv_subject_set_int(&in_run_[k], 1);
        const bool dropped = run_->results_lost() && e.status == StepStatus::Finished &&
                             !e.step->restores_on_discard();
        lv_subject_set_int(&state_[k], dropped ? 3 : dot_state(e.status));
        const std::string status = dropped
                                       ? std::string(lv_tr("Dropped when the printer restarted"))
                                       : entry_status_text(e);
        set_text(detail_[k], status);
        set_text(after_[k], e.restore_note.empty() ? status : e.restore_note);
        set_text(before_subj_[k], before_text_[k]);
        if (e.status == StepStatus::Pending || e.status == StepStatus::Running) {
            remaining_min += e.step->info().minutes;
        }
    }

    const RunPhase phase = run_->phase();
    const int current = run_->current();
    const bool over = phase != RunPhase::Running && phase != RunPhase::Ready;
    lv_subject_set_int(&over_, over ? 1 : 0);
    lv_subject_set_int(&run_phase_, static_cast<int>(phase));
    if (current >= 0) {
        const auto& e = entries[static_cast<size_t>(current)];
        const auto& info = e.step->info();
        set_text(title_, lv_tr(info.name));
        set_text(phase_text_, e.progress.phase);
        lv_subject_set_int(&indeterminate_, e.progress.percent < 0 ? 1 : 0);
        lv_subject_set_int(&progress_, std::max(0, e.progress.percent));
        lv_subject_set_int(&live_heat_, info.kind == calibration::StepKind::Heaters ? 1 : 0);
        set_text(warn_, warning_for(info.kind));
        set_text(footer_, fmt::format(fmt::runtime(lv_tr("Step {} of {} · about {} min left")),
                                      current + 1, entries.size(), remaining_min));
    } else if (over) {
        set_text(title_, lv_tr("All steps done"));
        set_text(phase_text_, lv_tr("Review the results, then save them all at once."));
        lv_subject_set_int(&live_heat_, 0);
        set_text(warn_, "");
        set_text(footer_, "");
    }

    lv_subject_set_int(&can_save_, run_->can_save() ? 1 : 0);
    const int finished = run_->finished_count();
    if (run_->results_lost()) {
        set_text(results_sub_, lv_tr("Stopping restarted the printer, which dropped the results "
                                     "of the steps that finished. Nothing can be saved."));
    } else {
        set_text(results_sub_,
                 fmt::format(fmt::runtime(lv_tr("{} of {} steps finished. Review them, then save "
                                                "them all at once.")),
                             finished, entries.size()));
    }
    switch (phase) {
    case RunPhase::Saving:
        set_text(save_line_, lv_tr("Saving..."));
        break;
    case RunPhase::Saved:
        set_text(save_line_, lv_tr("Saved. The Calibration screen shows the new results."));
        break;
    case RunPhase::Discarding:
        set_text(save_line_, lv_tr("Restarting Klipper to drop the results..."));
        break;
    case RunPhase::Discarded:
        set_text(save_line_, lv_tr("Nothing was saved."));
        break;
    default:
        set_text(save_line_,
                 run_->can_save()
                     ? std::string(lv_tr("One save for everything. Klipper may restart."))
                     : std::string(lv_tr("Nothing to save.")));
        break;
    }
    const std::string& err = run_->error();
    lv_subject_set_int(&has_error_, err.empty() ? 0 : 1);
    set_text(error_, err);
}

void CalibrationRunOverlay::toggle_pick(const std::string& step_id) {
    const int i = step_index(step_id.c_str());
    if (i < 0 || page_ != RunPage::Pick || lv_subject_get_int(&locked_[static_cast<size_t>(i)])) {
        return;
    }
    picked_[static_cast<size_t>(i)] = !picked_[static_cast<size_t>(i)];
    publish_pick();
}

void CalibrationRunOverlay::toggle_prereq(const std::string& prereq_id) {
    for (size_t i = 0; i < kPrereqs; ++i) {
        if (prereq_id == kRunPrereqIds[i] && !lv_subject_get_int(&prereq_detected_[i])) {
            ticked_[i] = !ticked_[i];
        }
    }
    publish_prereqs();
}

void CalibrationRunOverlay::next() {
    if (picked_steps().empty()) {
        return;
    }
    ticked_.fill(false);
    publish_prereqs();
    set_page(RunPage::Prereqs);
}

void CalibrationRunOverlay::back() {
    set_page(RunPage::Pick);
}

void CalibrationRunOverlay::start() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api || run_ || !lv_subject_get_int(&prereq_ready_)) {
        return;
    }
    std::vector<std::unique_ptr<calibration::CalibrationStep>> steps;
    for (const auto& info : picked_steps()) {
        if (const int i = step_index(info.id); i >= 0) {
            before_text_[static_cast<size_t>(i)] = before_text(info);
        }
        if (auto step = calibration::make_run_step(info)) {
            steps.push_back(std::move(step));
        }
    }
    finisher_ = std::make_unique<calibration::MoonrakerRunFinisher>(*api);
    run_ = std::make_unique<calibration::CalibrationRun>(*api, std::move(steps),
                                                         [this]() { publish_run(); });
    // A shutdown or a restart the run did not ask for drops what it staged.
    klippy_obs_ = observe<int>(
        get_printer_state().network_state().get_klippy_state_subject(), this,
        [](CalibrationRunOverlay* self, int state) {
            if (self->run_ && static_cast<KlippyState>(state) != KlippyState::READY) {
                self->run_->klippy_restarted();
            }
        },
        get_printer_state().get_subjects_lifetime());
    set_page(RunPage::Running);
    run_->start();
    publish_run();
}

void CalibrationRunOverlay::stop() {
    if (!run_ || run_->phase() != RunPhase::Running) {
        return;
    }
    ConfirmOptions opts;
    opts.owner_token = lifetime_.token();
    opts.destructive = true;
    modal_confirm(
        lv_tr("Stop calibration?"),
        lv_tr("The running step stops at once and the printer restarts, which also drops "
              "the results of the steps that finished."),
        ModalSeverity::Warning, lv_tr("Stop"),
        [this]() {
            if (run_) {
                run_->stop();
                set_page(RunPage::Results);
                publish_run();
            }
        },
        opts);
}

void CalibrationRunOverlay::see_results() {
    set_page(RunPage::Results);
    publish_run();
}

void CalibrationRunOverlay::save() {
    if (run_ && finisher_) {
        run_->save_all(*finisher_);
    }
}

void CalibrationRunOverlay::discard() {
    if (run_ && finisher_) {
        run_->discard_all(*finisher_);
    }
}

void CalibrationRunOverlay::done() {
    if (run_ && run_->phase() == RunPhase::Saved) {
        ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Calibration saved"), 2500);
    }
    klippy_obs_.reset();
    run_.reset();
    finisher_.reset();
    helix::nav::go_back();
}

void init_calibration_run_handlers() {
    register_xml_callbacks({
        {"on_cal_run_pick_toggle",
         [](lv_event_t* e) {
             const char* id = static_cast<const char*>(lv_event_get_user_data(e));
             get_calibration_run_overlay().toggle_pick(id ? id : "");
         }},
        {"on_cal_run_prereq_toggle",
         [](lv_event_t* e) {
             const char* id = static_cast<const char*>(lv_event_get_user_data(e));
             get_calibration_run_overlay().toggle_prereq(id ? id : "");
         }},
        {"on_cal_run_next", [](lv_event_t*) { get_calibration_run_overlay().next(); }},
        {"on_cal_run_back", [](lv_event_t*) { get_calibration_run_overlay().back(); }},
        {"on_cal_run_start", [](lv_event_t*) { get_calibration_run_overlay().start(); }},
        {"on_cal_run_stop", [](lv_event_t*) { get_calibration_run_overlay().stop(); }},
        {"on_cal_run_see_results",
         [](lv_event_t*) { get_calibration_run_overlay().see_results(); }},
        {"on_cal_run_save", [](lv_event_t*) { get_calibration_run_overlay().save(); }},
        {"on_cal_run_discard", [](lv_event_t*) { get_calibration_run_overlay().discard(); }},
        {"on_cal_run_done", [](lv_event_t*) { get_calibration_run_overlay().done(); }},
    });
}

} // namespace helix::ui
