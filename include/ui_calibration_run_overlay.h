// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Guided calibration: pick the steps, confirm what they need, let them run, then
// save or discard everything at once. The run lives here, not in the view, so
// closing the screen mid-run leaves it running and reopening shows where it is.

#include "calibration_run.h"
#include "calibration_run_steps.h"
#include "core/subject.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <array>
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

    void init_subjects() override;
    void on_activate() override;

    /// Open at the page the run is on: Pick when no run is under way.
    void open();
    /// Whether a run is under way or waiting for Save or Discard.
    [[nodiscard]] bool run_pending() const;

    void toggle_pick(const std::string& step_id);
    void toggle_prereq(const std::string& prereq_id);
    void next();
    void back();
    void start();
    void stop();
    void see_results();
    void save();
    void discard();
    void done();

  private:
    /// The steps this printer offers a run, with their default ticks.
    void prepare_pick();
    void publish();
    void publish_pick();
    void publish_prereqs();
    void publish_run();
    void set_page(RunPage page);
    [[nodiscard]] std::vector<calibration::StepInfo> picked_steps() const;

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
    std::unique_ptr<calibration::CalibrationRun> run_;
    std::unique_ptr<calibration::MoonrakerRunFinisher> finisher_;
    RunPage page_ = RunPage::Pick;

    SubjectManager subjects_;
    /// Stable storage for the subject names the XML registry keeps pointers to.
    std::deque<std::string> names_;

    helix::Subject page_subject_;
    helix::Subject pick_count_;
    helix::Subject pick_summary_;
    char pick_summary_buf_[kShortLen]{};
    helix::Subject hands_line_;
    char hands_line_buf_[kLongLen]{};

    std::array<helix::Subject, kSteps> shown_{};
    std::array<helix::Subject, kSteps> picked_subj_{};
    std::array<helix::Subject, kSteps> locked_{};
    std::array<helix::Subject, kSteps> meta_{};
    std::array<helix::Subject, kSteps> last_{};
    std::array<helix::Subject, kSteps> in_run_{};
    std::array<helix::Subject, kSteps> state_{};
    std::array<helix::Subject, kSteps> detail_{};
    std::array<helix::Subject, kSteps> before_{};
    std::array<helix::Subject, kSteps> after_{};
    char meta_bufs_[kSteps][kShortLen]{};
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

    helix::Subject run_phase_;
    helix::Subject can_save_;
    helix::Subject results_sub_;
    char results_sub_buf_[kLongLen]{};
    helix::Subject save_line_;
    char save_line_buf_[kLongLen]{};
    helix::Subject has_error_;
    helix::Subject error_;
    char error_buf_[kLongLen]{};
};

CalibrationRunOverlay& get_calibration_run_overlay();

/// Registers the run's XML callbacks, including the Calibration screen's
/// "Guided calibration" banner. Startup, before XML.
void init_calibration_run_handlers();

} // namespace helix::ui
