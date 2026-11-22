// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_calibration_run_overlay.h"

#include "ui_calibration_hub_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "calibration_catalog_inputs.h"
#include "calibration_config_results.h"
#include "calibration_history_store.h"
#include "calibration_live_card.h"
#include "config.h"
#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "core/static_subject_registry.h"
#include "display_format.h"
#include "display_settings_manager.h"
#include "display_wake_request.h"
#include "first_run_tour.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "klippy_state.h"
#include "observe_language.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "sound_manager.h"
#include "temperature_rules.h"
#include "thermal_rate_model.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <ctime>

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

/// "4 min · needs you · paper test", "12 min · heats nozzle + bed", "4 min · loud".
std::string meta_line(const StepInfo& s, bool has_bed) {
    std::string out = fmt::format(fmt::runtime(lv_tr("{} min")), s.minutes);
    out += " · ";
    if (s.needs_hands) {
        out += lv_tr("needs you");
    } else if (s.heats) {
        out += s.kind == calibration::StepKind::Heaters
                   ? (has_bed ? lv_tr("heats nozzle + bed") : lv_tr("heats nozzle"))
                   : lv_tr("heats bed");
    } else if (s.loud) {
        out += lv_tr("loud");
    } else {
        out += lv_tr("automatic");
    }
    if (s.prereqs & calibration::kPrereqPaper) {
        out += " · ";
        out += lv_tr("paper test");
    }
    if (s.loud && (s.needs_hands || s.heats)) {
        out += " · ";
        out += lv_tr("loud");
    }
    if (s.prereqs & calibration::kPrereqAccelerometer) {
        out += " · ";
        out += lv_tr("accelerometer");
    }
    return out;
}

/// The 480x272 tile's one-line detail: "12 min · required", "4 min · needs you".
std::string meta_short_line(const StepInfo& s) {
    std::string out = fmt::format(fmt::runtime(lv_tr("{} min")), s.minutes);
    const char* tag = nullptr;
    if (s.tier == calibration::Tier::Required) {
        tag = lv_tr("required");
    } else if (s.needs_hands) {
        tag = lv_tr("needs you");
    } else if (s.tier == calibration::Tier::Optional) {
        tag = lv_tr("optional");
    } else if (s.loud) {
        tag = lv_tr("loud");
    }
    if (tag) {
        out += " · ";
        out += tag;
    }
    return out;
}

/// "Loud and hot at times." for the walk-away stretch, or nothing.
const char* walk_away_note(const std::vector<const StepInfo*>& away) {
    bool loud = false;
    bool hot = false;
    for (const auto* s : away) {
        loud = loud || s->loud;
        hot = hot || s->heats;
    }
    if (loud && hot) {
        return lv_tr("Loud and hot at times.");
    }
    if (loud) {
        return lv_tr("Loud at times.");
    }
    if (hot) {
        return lv_tr("Hot at times.");
    }
    return "";
}

} // namespace

namespace {

/// The heaters a run's heater step tunes, as their tuning stands: each from the
/// last run's values, else from the config. Empty when none is tuned.
std::string heaters_before_text() {
    auto& ps = get_printer_state();
    const auto& hardware = ps.get_discovery();
    std::vector<std::string> names{ps.temperature_state().active_extruder_name()};
    if (hardware.has_heater_bed()) {
        names.emplace_back("heater_bed");
    }
    const auto* last = calibration::CalibrationHistory::instance().entry("heaters");
    const auto& in_config = hardware.heater_controls_in_config();
    std::vector<calibration::HeaterControl> controls;
    for (const auto& name : names) {
        std::string control;
        if (last) {
            if (const auto* values = json_util::find_member(last->values, name.c_str())) {
                control = json_util::safe_string(*values, "control");
            }
        }
        if (control.empty()) {
            const auto found = in_config.find(name);
            if (found != in_config.end()) {
                control = found->second;
            }
        }
        if (!control.empty()) {
            controls.push_back({name == "heater_bed", control});
        }
    }
    return calibration::format_heater_controls(controls);
}

} // namespace

std::string run_before_text(const StepInfo& s, int configured_z_offset_microns) {
    // The offset the Z offset screen shows as saved is the one its step starts
    // from; zero is an untuned probe, read from the history and config below.
    if (s.kind == calibration::StepKind::ZOffset && configured_z_offset_microns != 0) {
        return calibration::format_z_offset(configured_z_offset_microns / 1000.0);
    }
    if (s.kind == calibration::StepKind::Heaters) {
        const std::string tuned = heaters_before_text();
        return tuned.empty() ? lv_tr("not set") : tuned;
    }
    if (const auto* last = calibration::CalibrationHistory::instance().entry(s.id)) {
        if (!last->summary.empty()) {
            return last->summary;
        }
    }
    const auto& in_config = get_printer_state().get_discovery().calibration_in_config();
    const auto found = in_config.find(s.id);
    if (found != in_config.end()) {
        return found->second;
    }
    return s.kind == calibration::StepKind::BedMesh ? lv_tr("none") : lv_tr("not set");
}

namespace {

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

/// A step's line in the step list and the results table: its brief result when it
/// has one ("nozzle MPC · bed PID"); the history keeps the full summary.
std::string entry_status_text(const calibration::CalibrationRun::Entry& e) {
    switch (e.status) {
    case StepStatus::Pending:
        return fmt::format(fmt::runtime(lv_tr("about {} min")), calibration::minutes_left(e));
    case StepStatus::Running:
        return fmt::format(fmt::runtime(lv_tr("about {} min left")), calibration::minutes_left(e));
    case StepStatus::Finished:
        return e.result.brief.empty() ? e.result.summary : e.result.brief;
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

std::string results_primary_label(bool offers_save, bool restart_known, bool save_restarts) {
    if (!offers_save) {
        return lv_tr("Done");
    }
    return restart_known && !save_restarts ? lv_tr("Save") : lv_tr("Save and restart Klipper");
}

std::string run_end_title(const calibration::RunTally& tally) {
    using Outcome = calibration::RunTally::Outcome;
    switch (tally.outcome()) {
    case Outcome::AllFinished:
        break;
    case Outcome::NoneFinished:
        return tally.total == 1 ? lv_tr("The step didn't finish") : lv_tr("No step finished");
    case Outcome::SomeUnfinished:
        return fmt::format(fmt::runtime(lv_tr("{} of {} steps didn't finish")), tally.unfinished(),
                           tally.total);
    }
    return lv_tr("Calibration finished");
}

std::string end_note_text(calibration::CalibrationRun::EndNote note) {
    using Note = calibration::CalibrationRun::EndNote;
    switch (note) {
    case Note::None:
        break;
    case Note::KlipperStopped:
        return lv_tr("Klipper stopped during the calibration, so the remaining steps did not "
                     "run.");
    case Note::PrinterStayedBusy:
        return lv_tr("The printer stayed busy long after the step failed, so the remaining steps "
                     "did not run.");
    case Note::PrinterLost:
        return lv_tr("The printer's state could no longer be read, so the remaining steps did "
                     "not run.");
    }
    return {};
}

std::string stop_confirm_message(calibration::CalibrationRun::StopEffect effect) {
    using Effect = calibration::CalibrationRun::StopEffect;
    switch (effect) {
    case Effect::PrinterFinishes:
        return lv_tr("The remaining steps will not run. The printer finishes the command it is "
                     "running first, and its heaters stay on until then.");
    case Effect::RestartDropsResults:
        return lv_tr("The running step stops at once and the printer restarts, which also drops "
                     "the results of the steps that finished.");
    case Effect::Restarts:
        return lv_tr("The running step stops at once and the printer restarts.");
    case Effect::EndsStep:
        break;
    }
    return lv_tr("The running step stops. The steps that finished keep their results.");
}

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
    add_int(page_count_, static_cast<int>(RunPage::Results) + 1, "cal_run_page_count");
    add_int(pick_count_, 0, "cal_run_pick_count");
    add_string(pick_summary_, pick_summary_buf_, kShortLen, "cal_run_pick_summary");
    add_string(hands_line_, hands_line_buf_, kLongLen, "cal_run_hands_line");
    add_string(pick_subtitle_, pick_subtitle_buf_, kLongLen, "cal_run_pick_subtitle");
    add_string(pick_subtitle_short_, pick_subtitle_short_buf_, kShortLen,
               "cal_run_pick_subtitle_short");
    add_string(pick_summary_short_, pick_summary_short_buf_, kShortLen,
               "cal_run_pick_summary_short");
    add_string(hands_short_, hands_short_buf_, kShortLen, "cal_run_hands_short");
    add_int(zoffset_warm_shown_, 0, "cal_run_zoffset_warm_shown");
    add_int(zoffset_warm_, 0, "cal_run_zoffset_warm");
    for (size_t i = 0; i < kSteps; ++i) {
        const std::string id = kRunStepIds[i];
        add_int(shown_[i], 0, "cal_run_shown_" + id);
        add_int(picked_subj_[i], 0, "cal_run_picked_" + id);
        add_int(locked_[i], 0, "cal_run_locked_" + id);
        add_int(optional_[i], 0, "cal_run_optional_" + id);
        add_int(keep_[i], 0, "cal_run_keep_" + id);
        add_string(meta_[i], meta_bufs_[i], kShortLen, "cal_run_meta_" + id);
        add_string(meta_short_[i], meta_short_bufs_[i], kShortLen, "cal_run_meta_short_" + id);
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
    add_string(you_steps_, you_steps_buf_, kLongLen, "cal_run_you_steps");
    add_string(chime_line_, chime_line_buf_, kLongLen, "cal_run_chime_line");
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
    add_string(header_title_, header_title_buf_, kShortLen, "cal_run_header_title");
    add_string(eta_, eta_buf_, kShortLen, "cal_run_eta");
    add_int(overall_, 0, "cal_run_overall");
    add_int(running_now_, 0, "cal_run_running_now");
    add_int(step_count_, 0, "cal_run_step_count");
    add_int(step_current_, 0, "cal_run_step_current");
    add_string(results_title_, results_title_buf_, kShortLen, "cal_run_results_title");
    add_int(results_warn_, 0, "cal_run_results_warn");
    add_string(results_primary_, results_primary_buf_, kShortLen, "cal_run_results_primary");
    add_int(results_busy_, 0, "cal_run_results_busy");
    add_int(offers_save_, 0, "cal_run_offers_save");
    add_int(hide_discard_, 1, "cal_run_hide_discard");
    add_string(results_sub_, results_sub_buf_, kLongLen, "cal_run_results_sub");
    add_string(save_line_, save_line_buf_, kLongLen, "cal_run_save_line");
    add_int(has_error_, 0, "cal_run_has_error");
    add_string(error_, error_buf_, kLongLen, "cal_run_error");

    language_obs_ = helix::ui::observe_language_change(
        this, [](CalibrationRunOverlay* self) { self->publish(); });

    // Runs from StaticSubjectRegistry::deinit_all(), which may come after
    // StaticPanelRegistry has destroyed the overlay.
    StaticSubjectRegistry::instance().register_deinit("CalibrationRunOverlay", []() {
        if (auto* self = helix::lazy_global_if_exists<CalibrationRunOverlay>()) {
            self->klippy_obs_.reset();
            self->language_obs_.reset();
            self->run_.reset();
            self->awake_.release();
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

lv_obj_t* CalibrationRunOverlay::create(lv_obj_t* parent) {
    lv_obj_t* root = OverlayBase::create(parent);
    if (root) {
        helix::nav::set_overlay_width_unmanaged(root);
    }
    return root;
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
    zoffset_warm_bed_ = false;
    for (const auto& s : candidates_) {
        const int i = step_index(s.id);
        if (i >= 0) {
            picked_[static_cast<size_t>(i)] = calibration::picked_by_default(s);
        }
    }
    spdlog::debug("[{}] {} step(s) offered", get_name(), candidates_.size());
}

std::vector<StepInfo> CalibrationRunOverlay::picked_steps() const {
    const auto options = run_options();
    std::vector<StepInfo> out;
    for (const auto& s : candidates_) {
        const int i = step_index(s.id);
        if (i >= 0 && picked_[static_cast<size_t>(i)]) {
            out.push_back(calibration::with_run_options(s, options));
        }
    }
    return out;
}

calibration::RunOptions CalibrationRunOverlay::run_options() const {
    calibration::RunOptions options;
    auto& ps = get_printer_state();
    options.zoffset_warm_bed = zoffset_warm_bed_ && ps.get_discovery().has_heater_bed();
    if (options.zoffset_warm_bed) {
        // The shared thermal model prices every other warm-up in the app.
        const float bed_c = temperature_rules::deci_to_degrees_f(
            lv_subject_get_int(ps.temperature_state().get_bed_temp_subject()));
        options.zoffset_warm_bed_minutes =
            calibration::heating_minutes(ThermalRateManager::instance().estimate_heating_seconds(
                "heater_bed", bed_c, static_cast<float>(zoffset_cal::warm_bed_target_c())));
    }
    return options;
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
    auto& discovery = get_printer_state().get_discovery();
    const bool has_bed = discovery.has_heater_bed();
    const Config* config = Config::get_instance();
    const std::string printer = config ? config->get_active_printer_name() : std::string();
    const std::string short_line =
        fmt::format(fmt::runtime(lv_tr("{} · {} firmware")), printer, discovery.firmware_name());
    set_text(pick_subtitle_short_, short_line);
    set_text(pick_subtitle_,
             fmt::format(fmt::runtime(lv_tr("{} · pick what to run, then it runs on its own")),
                         short_line));
    const auto options = run_options();
    bool zoffset_offered = false;
    for (size_t i = 0; i < kSteps; ++i) {
        const StepInfo* candidate = nullptr;
        for (const auto& s : candidates_) {
            if (std::strcmp(s.id, kRunStepIds[i]) == 0) {
                candidate = &s;
            }
        }
        lv_subject_set_int(&shown_[i], candidate ? 1 : 0);
        if (!candidate) {
            continue;
        }
        zoffset_offered = zoffset_offered || candidate->kind == calibration::StepKind::ZOffset;
        const StepInfo shown_step = calibration::with_run_options(*candidate, options);
        const StepInfo* step = &shown_step;
        const bool locked = step->tier == calibration::Tier::Required;
        if (locked) {
            picked_[i] = true;
        }
        lv_subject_set_int(&picked_subj_[i], picked_[i] ? 1 : 0);
        lv_subject_set_int(&locked_[i], locked ? 1 : 0);
        lv_subject_set_int(&optional_[i], step->tier == calibration::Tier::Optional ? 1 : 0);
        set_text(meta_[i], meta_line(*step, has_bed));
        set_text(meta_short_[i], meta_short_line(*step));
        const auto& in_config = get_printer_state().get_discovery().calibration_in_config();
        const auto found = in_config.find(step->id);
        set_text(last_[i],
                 tile_status(*step, calibration::CalibrationHistory::instance().entry(step->id),
                             found != in_config.end() ? &found->second : nullptr)
                     .text);
    }
    lv_subject_set_int(&zoffset_warm_shown_, zoffset_offered && has_bed ? 1 : 0);
    lv_subject_set_int(&zoffset_warm_, options.zoffset_warm_bed ? 1 : 0);
    const auto picked = picked_steps();
    const int n = static_cast<int>(picked.size());
    const int minutes = calibration::total_minutes(picked);
    const int hands = calibration::hands_on_minutes(picked);
    lv_subject_set_int(&pick_count_, n);
    set_text(pick_summary_,
             n == 1 ? fmt::format(fmt::runtime(lv_tr("1 step · about {} min")), minutes)
                    : fmt::format(fmt::runtime(lv_tr("{} steps · about {} min")), n, minutes));
    set_text(pick_summary_short_,
             n == 1 ? fmt::format(fmt::runtime(lv_tr("1 step · {} min")), minutes)
                    : fmt::format(fmt::runtime(lv_tr("{} steps · {} min")), n, minutes));
    set_text(hands_line_,
             hands > 0 ? fmt::format(fmt::runtime(lv_tr("You are needed for the first {} min, "
                                                        "then it runs on its own")),
                                     hands)
                       : std::string(lv_tr("Runs on its own once started")));
    set_text(hands_short_, hands > 0 ? fmt::format(fmt::runtime(lv_tr("you: first {} min")), hands)
                                     : std::string(lv_tr("runs on its own")));
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
    set_text(prereq_left_,
             missing == 0 ? std::string(lv_tr("All set"))
             : missing == 1
                 ? std::string(lv_tr("1 thing left to confirm"))
                 : fmt::format(fmt::runtime(lv_tr("{} things left to confirm")), missing));

    const int hands = calibration::hands_on_minutes(picked);
    const int total = calibration::total_minutes(picked);
    std::vector<const StepInfo*> mine;
    std::vector<const StepInfo*> away;
    bool paper = false;
    for (const auto& s : picked) {
        (s.needs_hands ? mine : away).push_back(&s);
        paper = paper || (s.needs_hands && (s.prereqs & calibration::kPrereqPaper));
    }
    lv_subject_set_int(&has_hands_, hands > 0 ? 1 : 0);
    set_text(you_line_, fmt::format(fmt::runtime(lv_tr("You, about {} min")), hands));
    std::string you_steps = join_names(mine);
    if (paper) {
        you_steps += " ";
        you_steps += lv_tr("paper test");
    }
    set_text(you_steps_, you_steps + ". " + lv_tr("Stay at the printer."));
    set_text(away_line_,
             fmt::format(fmt::runtime(lv_tr("Walk away, about {} min")), total - hands));
    std::string away_steps = join_names(away);
    if (!away_steps.empty()) {
        away_steps += ".";
        if (const char* note = walk_away_note(away); *note) {
            away_steps += " ";
            away_steps += note;
        }
    }
    set_text(away_steps_, away_steps);
    if (SoundManager::instance().is_available()) {
        set_text(chime_line_, lv_tr("It chimes if it needs you, and when it is done."));
    } else {
        const std::time_t done_at = std::time(nullptr) + static_cast<std::time_t>(total) * 60;
        set_text(chime_line_,
                 fmt::format(fmt::runtime(lv_tr("Check back around {}. The screen shows if it "
                                                "needs you.")),
                             display_format::format_time(done_at)));
    }
}

void CalibrationRunOverlay::publish_run() {
    lv_subject_set_int(&has_bed_, get_printer_state().get_discovery().has_heater_bed() ? 1 : 0);
    get_calibration_live_card().update(run_.get(), overlay_root_);
    if (!run_) {
        for (size_t i = 0; i < kSteps; ++i) {
            lv_subject_set_int(&in_run_[i], 0);
            lv_subject_set_int(&keep_[i], 0);
        }
        lv_subject_set_int(&over_, 0);
        lv_subject_set_int(&running_now_, 1);
        lv_subject_set_int(&overall_, 0);
        lv_subject_set_int(&hide_discard_, 1);
        lv_subject_set_int(&results_busy_, 0);
        lv_subject_set_int(&offers_save_, 0);
        return;
    }
    const auto& entries = run_->entries();
    const RunPhase phase = run_->phase();
    keep_owner_informed();
    const bool dropped_all =
        run_->results_lost() || phase == RunPhase::Discarding || phase == RunPhase::Discarded;
    for (size_t i = 0; i < kSteps; ++i) {
        lv_subject_set_int(&in_run_[i], 0);
    }
    int total_min = 0;
    int done_min_x100 = 0;
    int saving = 0;
    for (const auto& e : entries) {
        const int minutes = e.step->info().minutes;
        total_min += minutes;
        if (e.status == StepStatus::Running) {
            done_min_x100 += minutes * std::max(0, e.progress.percent);
        } else if (e.status != StepStatus::Pending) {
            done_min_x100 += minutes * 100;
        }
        const bool keeps = !dropped_all && calibration::CalibrationRun::saves(e);
        saving += keeps ? 1 : 0;
        const int i = step_index(e.step->info().id);
        if (i < 0) {
            continue;
        }
        const auto k = static_cast<size_t>(i);
        lv_subject_set_int(&in_run_[k], 1);
        lv_subject_set_int(&keep_[k], keeps ? 1 : 0);
        const bool dropped = run_->results_lost() && e.status == StepStatus::Finished &&
                             !e.step->restores_on_discard();
        lv_subject_set_int(&state_[k], dropped ? 3 : dot_state(e.status));
        const std::string dropped_text = lv_tr("Dropped when the printer restarted");
        const std::string status = dropped ? dropped_text : entry_status_text(e);
        set_text(detail_[k], status);
        set_text(after_[k], e.restore_note.empty() ? status : e.restore_note);
        set_text(before_subj_[k], before_text_[k]);
    }

    const int current = run_->current();
    const int waiting_on = run_->waiting_on();
    const bool over = phase != RunPhase::Running && phase != RunPhase::Ready;
    const calibration::RunTally tally = calibration::run_tally(entries);
    const bool all_finished = tally.outcome() == calibration::RunTally::Outcome::AllFinished;
    const std::string end_title = run_end_title(tally);
    if (over && run_minutes_ < 0) {
        run_minutes_ = static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(
                                            std::chrono::steady_clock::now() - run_started_)
                                            .count());
    }
    lv_subject_set_int(&over_, over ? 1 : 0);
    lv_subject_set_int(&running_now_, over ? 0 : 1);
    lv_subject_set_int(&step_count_, static_cast<int>(entries.size()));
    lv_subject_set_int(&step_current_, current >= 0 ? current : static_cast<int>(entries.size()));
    lv_subject_set_int(&overall_, over ? 100 : (total_min > 0 ? done_min_x100 / total_min : 0));
    set_text(header_title_, over ? end_title : std::string(lv_tr("Calibrating")));
    if (over) {
        set_text(eta_, all_finished ? lv_tr("all done") : "");
    } else if (waiting_on >= 0) {
        set_text(eta_, lv_tr("waiting for the printer"));
    } else {
        set_text(eta_, fmt::format(fmt::runtime(lv_tr("about {} min left · you can leave")),
                                   std::max(1, calibration::minutes_left(entries))));
    }
    if (!over) {
        set_text(footer_, lv_tr("Results are saved together at the end"));
    } else if (all_finished) {
        set_text(footer_, lv_tr("Next, review the results and save them once"));
    } else {
        set_text(footer_, lv_tr("Next, see what finished and why the rest did not"));
    }
    if (current >= 0) {
        const auto& e = entries[static_cast<size_t>(current)];
        const auto& info = e.step->info();
        set_text(title_, lv_tr(info.name));
        set_text(phase_text_, e.progress.phase);
        lv_subject_set_int(&indeterminate_, e.progress.percent < 0 ? 1 : 0);
        lv_subject_set_int(&progress_, std::max(0, e.progress.percent));
        lv_subject_set_int(&live_heat_, info.kind == calibration::StepKind::Heaters ? 1 : 0);
        set_text(warn_, warning_for(info.kind));
    } else if (waiting_on >= 0) {
        set_text(title_, lv_tr("Waiting for the printer to finish"));
        set_text(phase_text_, "");
        lv_subject_set_int(&indeterminate_, 1);
        lv_subject_set_int(&live_heat_, 0);
        set_text(warn_, "");
    } else if (over) {
        set_text(title_, all_finished ? lv_tr("All steps done") : lv_tr("Calibration ended"));
        set_text(phase_text_, "");
        lv_subject_set_int(&live_heat_, 0);
        set_text(warn_, "");
    }

    // Results page.
    const bool can_save = run_->can_save();
    const bool busy = phase == RunPhase::Saving || phase == RunPhase::Discarding;
    lv_subject_set_int(&results_busy_, busy ? 1 : 0);
    lv_subject_set_int(&hide_discard_, run_->can_discard() && !busy ? 0 : 1);
    lv_subject_set_int(&results_warn_, run_->results_lost() || !all_finished ? 1 : 0);
    std::string title = end_title;
    if (phase == RunPhase::Saved) {
        title = lv_tr("Calibration saved");
    } else if (phase == RunPhase::Discarded) {
        title = lv_tr("Nothing was saved");
    } else if (run_->results_lost()) {
        title = lv_tr("Calibration stopped");
    }
    set_text(results_title_, title);
    const int finished = run_->finished_count();
    if (run_->results_lost()) {
        set_text(results_sub_, lv_tr("Stopping restarted the printer, so nothing can be saved"));
    } else if (run_->printer_finishing()) {
        set_text(results_sub_, lv_tr("The printer is finishing a step's command. Save once it is "
                                     "done."));
    } else if (!can_save) {
        set_text(results_sub_, finished == 0 ? lv_tr("Nothing to save. Each step says why.")
                                             : lv_tr("Nothing to save"));
    } else {
        const int minutes = std::max(1, run_minutes_);
        set_text(results_sub_,
                 finished == 1
                     ? fmt::format(fmt::runtime(lv_tr("1 step in {} min · review, then save once")),
                                   minutes)
                     : fmt::format(fmt::runtime(lv_tr("{} steps in {} min · review, then save "
                                                      "once")),
                                   finished, minutes));
    }
    const bool offers_save = can_save && !busy;
    lv_subject_set_int(&offers_save_, offers_save ? 1 : 0);
    set_text(results_primary_, results_primary_label(offers_save, restart_known_, save_restarts_));
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
        if (run_->printer_finishing()) {
            set_text(save_line_, lv_tr("Waiting for the printer to finish its command"));
        } else if (!can_save) {
            set_text(save_line_, lv_tr("Nothing to save."));
        } else {
            const std::string count =
                saving == 1 ? std::string(lv_tr("Saving 1 result"))
                            : fmt::format(fmt::runtime(lv_tr("Saving {} results")), saving);
            set_text(save_line_, count + " · " +
                                     (!restart_known_  ? lv_tr("Klipper may restart")
                                      : save_restarts_ ? lv_tr("one restart")
                                                       : lv_tr("no restart")));
        }
        break;
    }
    const std::string err = run_->error().empty() ? end_note_text(run_->end_note()) : run_->error();
    lv_subject_set_int(&has_error_, err.empty() ? 0 : 1);
    set_text(error_, err);
}

void CalibrationRunOverlay::keep_owner_informed() {
    // The owner was told they can leave, and that the screen shows when it needs them.
    const calibration::RunSighting now = calibration::sight(*run_);
    if (calibration::calls_for_owner(last_sighting_, now)) {
        spdlog::info("[{}] The run needs the owner's eye; waking the display", get_name());
        helix::request_display_wake();
    }
    last_sighting_ = now;
    awake_.update(now.phase, DisplaySettingsManager::instance().get_sleep_while_printing());
}

void CalibrationRunOverlay::query_save_restarts() {
    if (restart_known_ || !run_ || !run_->can_save()) {
        return;
    }
    restart_query_.query_restarts(get_moonraker_api(), [this](bool restarts) {
        if (!run_) {
            return;
        }
        restart_known_ = true;
        save_restarts_ = restarts;
        publish_run();
    });
}

void CalibrationRunOverlay::toggle_pick(const std::string& step_id) {
    const int i = step_index(step_id.c_str());
    if (i < 0 || page_ != RunPage::Pick || lv_subject_get_int(&locked_[static_cast<size_t>(i)])) {
        return;
    }
    picked_[static_cast<size_t>(i)] = !picked_[static_cast<size_t>(i)];
    publish_pick();
}

void CalibrationRunOverlay::toggle_zoffset_warm_bed() {
    if (page_ != RunPage::Pick) {
        return;
    }
    zoffset_warm_bed_ = !zoffset_warm_bed_;
    spdlog::info("[{}] Warm the bed first: {}", get_name(), zoffset_warm_bed_ ? "on" : "off");
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
    const auto options = run_options();
    for (const auto& info : picked_steps()) {
        if (const int i = step_index(info.id); i >= 0) {
            before_text_[static_cast<size_t>(i)] =
                run_before_text(info, get_printer_state().get_configured_z_offset_microns());
        }
        if (auto step = calibration::make_run_step(info, options)) {
            steps.push_back(std::move(step));
        }
    }
    finisher_ = std::make_unique<calibration::MoonrakerRunFinisher>(*api);
    run_ = std::make_unique<calibration::CalibrationRun>(*api, std::move(steps),
                                                         [this]() { publish_run(); });
    run_->set_busy_watch(calibration::make_printer_busy_watch(get_printer_state()));
    // A shutdown or a restart the run did not ask for drops what it staged.
    klippy_obs_ = observe<int>(
        get_printer_state().network_state().get_klippy_state_subject(), this,
        [](CalibrationRunOverlay* self, int state) {
            if (self->run_ && static_cast<KlippyState>(state) != KlippyState::READY) {
                self->run_->klippy_restarted();
            }
        },
        get_printer_state().get_subjects_lifetime());
    run_started_ = std::chrono::steady_clock::now();
    run_minutes_ = -1;
    last_sighting_ = {};
    restart_known_ = false;
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
    const auto shown = run_->stop_effect();
    modal_confirm(
        lv_tr("Stop calibration?"), stop_confirm_message(shown).c_str(), ModalSeverity::Warning,
        lv_tr("Stop"), [this, shown]() { confirm_stop(shown); }, opts);
}

void CalibrationRunOverlay::confirm_stop(calibration::CalibrationRun::StopEffect shown) {
    if (!run_) {
        return;
    }
    // The run moved on while the dialog was open: the owner agreed to a
    // different stop, so ask again with what stopping does now.
    if (run_->phase() == RunPhase::Running && run_->stop_effect() != shown) {
        spdlog::info("[CalibrationRun] The stop's effect changed while confirming; asking again");
        stop();
        return;
    }
    run_->stop();
    set_page(RunPage::Results);
    publish_run();
}

void CalibrationRunOverlay::see_results() {
    set_page(RunPage::Results);
    publish_run();
    query_save_restarts();
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

void CalibrationRunOverlay::results_primary() {
    if (run_ && run_->can_save()) {
        save();
    } else {
        done();
    }
}

void CalibrationRunOverlay::close() {
    if (!run_pending()) {
        helix::nav::go_back();
    }
}

void CalibrationRunOverlay::done() {
    if (run_ && run_->phase() == RunPhase::Saved) {
        ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Calibration saved"), 2500);
    }
    klippy_obs_.reset();
    run_.reset();
    finisher_.reset();
    awake_.release();
    helix::nav::go_back();
}

void init_calibration_run_handlers() {
    register_xml_callbacks({
        {"on_cal_run_pick_toggle",
         [](lv_event_t* e) {
             const char* id = static_cast<const char*>(lv_event_get_user_data(e));
             get_calibration_run_overlay().toggle_pick(id ? id : "");
         }},
        {"on_cal_run_zoffset_warm_toggle",
         [](lv_event_t*) { get_calibration_run_overlay().toggle_zoffset_warm_bed(); }},
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
        {"on_cal_run_discard", [](lv_event_t*) { get_calibration_run_overlay().discard(); }},
        {"on_cal_run_results_primary",
         [](lv_event_t*) { get_calibration_run_overlay().results_primary(); }},
        {"on_cal_run_close", [](lv_event_t*) { get_calibration_run_overlay().close(); }},
    });
}

} // namespace helix::ui
