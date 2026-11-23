// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Guided calibration: pick the steps, confirm what they need, let them run, then
// save or discard everything at once. The run lives here, not in the view, so
// closing the screen mid-run leaves it running and reopening shows where it is.

#include "calibration_run.h"
#include "calibration_run_steps.h"
#include "core/observer_guard.h"
#include "core/subject.h"
#include "display_awake_hold.h"
#include "overlay_base.h"
#include "save_config_restart.h"
#include "subject_managed_panel.h"

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace helix::ui {

/// The step ids a guided run can hold, in run order: the rows of every page.
inline constexpr std::array<const char*, 4> kRunStepIds = {"z_offset", "heaters", "bed_mesh",
                                                           "input_shaper"};
/// The prerequisite ids, in calibration::kPrereqOrder's order.
inline constexpr std::array<const char*, 6> kRunPrereqIds = {
    "bed_clear", "build_plate", "toolhead_unloaded", "filament_loaded", "paper", "accelerometer"};

enum class RunPage : int { Pick = 0, Prereqs = 1, Running = 2, Results = 3 };

/// The Results page's primary button: "Done" when nothing is left to save or a
/// save or discard is under way; "Save" only once Klipper said the save skips its
/// restart; "Save and restart Klipper" otherwise, before that answer too.
[[nodiscard]] std::string results_primary_label(bool offers_save, bool restart_known,
                                                bool save_restarts);

/// A step's BEFORE on the Results page: for Z offset the saved offset the step
/// starts from (@p configured_z_offset_microns, 0 when untuned), else the last
/// result in the history, else what the config holds, else "not set".
[[nodiscard]] std::string run_before_text(const calibration::StepInfo& s,
                                          int configured_z_offset_microns);

/// The title a run that has ended goes by: "Calibration finished" when every
/// step finished, else how many did not ("2 of 3 steps didn't finish", "No step
/// finished").
[[nodiscard]] std::string run_end_title(const calibration::RunTally& tally);

/// Why the run ended early, for the Results page; empty for none.
[[nodiscard]] std::string end_note_text(calibration::CalibrationRun::EndNote note);

/// Holds the screen up while a run's steps run when sleep-while-printing is
/// off, the rule a print follows. Main thread.
class RunScreenHold {
  public:
    void update(calibration::RunPhase phase, bool sleep_while_printing) {
        if (phase == calibration::RunPhase::Running && !sleep_while_printing) {
            token_.acquire();
        } else {
            token_.reset();
        }
    }
    void release() {
        token_.reset();
    }

  private:
    helix::DisplayAwakeToken token_;
};

/// The Stop confirmation's message for what stopping would do now.
[[nodiscard]] std::string stop_confirm_message(calibration::CalibrationRun::StopEffect effect);

class CalibrationRunOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Guided calibration";
    }
    const char* xml_component() const override {
        return "calibration_run_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }
    /// Full screen, over the navigation bar: the wizard has its own header and footer.
    lv_obj_t* create(lv_obj_t* parent) override;

    void init_subjects() override;
    void on_activate() override;
    /// Lets the first-run tour go, which setup holds while it hands over to the run.
    void on_dismissed() override;

    /// Open at the page the run is on: Pick when no run is under way.
    void open();
    /// Whether a run is under way or waiting for Save or Discard.
    [[nodiscard]] bool run_pending() const;

    void toggle_pick(const std::string& step_id);
    /// The Z offset card's "Warm the bed first".
    void toggle_zoffset_warm_bed();
    /// The Heaters card's Bed chip. Ignored where the bed is not the owner's
    /// choice: a bed with no tuning is always tuned (calibration::BedTuning::Required).
    void toggle_heaters_bed();
    void toggle_prereq(const std::string& prereq_id);
    void next();
    void back();
    void start();
    void stop();
    /// The Stop dialog's confirm: stops, unless stopping now does something
    /// other than what @p shown told the owner, which asks again.
    void confirm_stop(calibration::CalibrationRun::StopEffect shown);
    void see_results();
    void save();
    void discard();
    void done();
    /// The Results page's primary button: Save while there is something to save, else Done.
    void results_primary();
    /// Leave without running anything.
    void close();

  private:
    /// The steps this printer offers a run, with their default ticks.
    void prepare_pick();
    void publish();
    void publish_pick();
    void publish_prereqs();
    void publish_run();
    void set_page(RunPage page);
    /// Ask once whether the save restarts Klipper, so the Save button can say so.
    void query_save_restarts();
    /// Wake the screen for what the owner must see, and hold it up while the
    /// steps run when sleep-while-printing is off.
    void keep_owner_informed();
    /// The picked steps as the run's options run them.
    [[nodiscard]] std::vector<calibration::StepInfo> picked_steps() const;
    /// The Pick page's options; warming only where there is a heated bed.
    [[nodiscard]] calibration::RunOptions run_options() const;

    void add_int(helix::Subject& subject, int initial, std::string name);
    void add_string(helix::Subject& subject, char* buf, size_t size, std::string name);
    void set_text(helix::Subject& subject, const std::string& text);

    static constexpr size_t kSteps = kRunStepIds.size();
    static constexpr size_t kPrereqs = kRunPrereqIds.size();
    static constexpr size_t kShortLen = 96;
    static constexpr size_t kLongLen = 256;

    std::vector<calibration::StepInfo> candidates_;
    std::array<bool, kSteps> picked_{};
    std::array<bool, kPrereqs> ticked_{};
    /// "Warm the bed first" on the Z offset card.
    bool zoffset_warm_bed_ = false;
    /// The Bed chip on the Heaters card, where the bed is the owner's choice.
    bool heaters_bed_ = false;
    /// Each step's value when the run started: the history changes as soon as Save all lands.
    std::array<std::string, kSteps> before_text_{};
    std::unique_ptr<calibration::CalibrationRun> run_;
    std::unique_ptr<calibration::MoonrakerRunFinisher> finisher_;
    /// Tells the run when Klipper goes down under it.
    ObserverGuard klippy_obs_;
    /// Re-publishes every composed line in the new language.
    ObserverGuard language_obs_;
    RunPage page_ = RunPage::Pick;
    /// Answers whether the one save restarts Klipper; MoonrakerRunFinisher saves.
    SaveConfigWatch restart_query_;
    bool restart_known_ = false;
    bool save_restarts_ = true;
    std::chrono::steady_clock::time_point run_started_{};
    /// Whole minutes the steps took, fixed when the last one ends.
    int run_minutes_ = -1;
    /// The run as last published, to tell what changed since.
    calibration::RunSighting last_sighting_;
    /// Holds the screen up while the steps run, as sleep-while-printing off
    /// holds it for a print.
    RunScreenHold awake_;

    SubjectManager subjects_;
    /// Stable storage for the subject names the XML registry keeps pointers to.
    std::deque<std::string> names_;

    helix::Subject page_subject_;
    /// How many pages the run has, for the Pick header's dots.
    helix::Subject page_count_;
    helix::Subject pick_count_;
    helix::Subject pick_summary_;
    char pick_summary_buf_[kShortLen]{};
    helix::Subject hands_line_;
    char hands_line_buf_[kLongLen]{};
    helix::Subject pick_subtitle_;
    char pick_subtitle_buf_[kLongLen]{};
    helix::Subject pick_subtitle_short_;
    char pick_subtitle_short_buf_[kShortLen]{};
    helix::Subject pick_summary_short_;
    char pick_summary_short_buf_[kShortLen]{};
    helix::Subject hands_short_;
    char hands_short_buf_[kShortLen]{};
    /// The Z offset card's warm-up option: whether it shows, and whether it is on.
    helix::Subject zoffset_warm_shown_;
    helix::Subject zoffset_warm_;
    /// The Heaters card's bed option: whether it shows, and 0 off, 1 on, 2 required.
    helix::Subject heaters_bed_shown_;
    helix::Subject heaters_bed_subj_;

    std::array<helix::Subject, kSteps> shown_{};
    std::array<helix::Subject, kSteps> picked_subj_{};
    std::array<helix::Subject, kSteps> locked_{};
    std::array<helix::Subject, kSteps> optional_{};
    std::array<helix::Subject, kSteps> keep_{};
    std::array<helix::Subject, kSteps> meta_{};
    std::array<helix::Subject, kSteps> meta_short_{};
    std::array<helix::Subject, kSteps> last_{};
    std::array<helix::Subject, kSteps> in_run_{};
    std::array<helix::Subject, kSteps> state_{};
    std::array<helix::Subject, kSteps> detail_{};
    std::array<helix::Subject, kSteps> before_subj_{};
    std::array<helix::Subject, kSteps> after_{};
    char meta_bufs_[kSteps][kShortLen]{};
    char meta_short_bufs_[kSteps][kShortLen]{};
    char last_bufs_[kSteps][kShortLen]{};
    char detail_bufs_[kSteps][kLongLen]{};
    char before_bufs_[kSteps][kShortLen]{};
    char after_bufs_[kSteps][kLongLen]{};

    std::array<helix::Subject, kPrereqs> prereq_shown_{};
    std::array<helix::Subject, kPrereqs> prereq_checked_{};
    std::array<helix::Subject, kPrereqs> prereq_detected_{};
    std::array<helix::Subject, kPrereqs> prereq_for_{};
    char prereq_for_bufs_[kPrereqs][kLongLen]{};

    helix::Subject prereq_ready_;
    helix::Subject prereq_left_;
    char prereq_left_buf_[kShortLen]{};
    helix::Subject has_hands_;
    helix::Subject you_line_;
    char you_line_buf_[kShortLen]{};
    helix::Subject you_steps_;
    char you_steps_buf_[kLongLen]{};
    helix::Subject chime_line_;
    char chime_line_buf_[kLongLen]{};
    helix::Subject away_line_;
    char away_line_buf_[kShortLen]{};
    helix::Subject away_steps_;
    char away_steps_buf_[kLongLen]{};

    helix::Subject title_;
    char title_buf_[kShortLen]{};
    helix::Subject phase_text_;
    char phase_text_buf_[kLongLen]{};
    helix::Subject progress_;
    helix::Subject indeterminate_;
    helix::Subject over_;
    helix::Subject live_heat_;
    helix::Subject has_bed_;
    helix::Subject warn_;
    char warn_buf_[kShortLen]{};
    helix::Subject footer_;
    char footer_buf_[kShortLen]{};
    helix::Subject header_title_;
    char header_title_buf_[kShortLen]{};
    helix::Subject eta_;
    char eta_buf_[kShortLen]{};
    helix::Subject overall_;
    helix::Subject running_now_;
    /// For the wizard header's step marks on screens the run opens: how many
    /// steps, and the running one (0-based; the count once all have ended).
    helix::Subject step_count_;
    helix::Subject step_current_;

    helix::Subject results_title_;
    char results_title_buf_[kShortLen]{};
    helix::Subject results_warn_;
    helix::Subject results_primary_;
    char results_primary_buf_[kShortLen]{};
    helix::Subject results_busy_;
    /// 1 while the results primary is a save, which needs the printer.
    helix::Subject offers_save_;
    helix::Subject hide_discard_;
    helix::Subject results_sub_;
    char results_sub_buf_[kLongLen]{};
    helix::Subject save_line_;
    char save_line_buf_[kLongLen]{};
    /// Under the save line: whether saving restarts Klipper, when it offers a save.
    helix::Subject save_sub_;
    char save_sub_buf_[kShortLen]{};
    helix::Subject has_error_;
    helix::Subject error_;
    char error_buf_[kLongLen]{};
};

CalibrationRunOverlay& get_calibration_run_overlay();

/// Registers the run's XML callbacks. Startup, before XML.
void init_calibration_run_handlers();

} // namespace helix::ui
