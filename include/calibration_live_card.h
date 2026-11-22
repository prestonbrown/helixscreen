// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The guided run's live card (ui_xml/components/calibration_live_card.xml): what
// the running step is doing and a live view for its kind, fed by the step's
// progress. A temperature graph while a heater is calibrated, the bed mesh
// filling in as it is probed, the resonance curve drawing in during a sweep,
// each belt's pull while it is tensioned. Main thread only.

#include "ui_shaper_sweep_chart.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "calibration_catalog.h"
#include "calibration_step.h"
#include "core/observer_guard.h"
#include "core/subject.h"
#include "live_sweep_preview.h"
#include "subject_managed_panel.h"

#include <memory>
#include <optional>
#include <string>

namespace helix {
class TempGraphController;
}

namespace helix::calibration {
class CalibrationRun;
}

namespace helix::ui {

/// cal_live_kind: which live view the card shows.
enum class LiveKind : int { None = 0, Heat = 1, Mesh = 2, Shaper = 3, Belt = 4, Done = 5 };

/// What the card says about a step beside its live view.
struct LiveCardText {
    LiveKind kind = LiveKind::None;
    std::string what;   ///< what the step is doing, translated
    std::string badge;  ///< translated, upper case; empty for none
    int badge_tone = 0; ///< 0 neutral, 1 motion, 2 hot, 3 loud, 4 done
    std::string warn;   ///< translated
    bool hot = false;   ///< the warning is about heat
};

/// The card's words for a step of @p kind, given its live progress. A kind
/// without a live view comes back as LiveKind::None with a neutral badge.
[[nodiscard]] LiveCardText live_card_text(calibration::StepKind kind,
                                          const calibration::StepLive& live);
/// The card's words once every step has ended.
[[nodiscard]] LiveCardText live_card_done_text();

/// The fill of a belt's track, in percent of the track: @p pull_fraction of
/// the way to the target mark at kBeltTargetPercent.
inline constexpr int kBeltTargetPercent = 72;
[[nodiscard]] int belt_fill_percent(float pull_fraction);

/// Percent of the bed the mesh view still dims: 100 before the first point,
/// 0 once every point is probed or when the total is unknown.
[[nodiscard]] int mesh_unprobed_percent(int probed, int total);

class CalibrationLiveCard {
  public:
    CalibrationLiveCard() = default;
    ~CalibrationLiveCard();
    CalibrationLiveCard(const CalibrationLiveCard&) = delete;
    CalibrationLiveCard& operator=(const CalibrationLiveCard&) = delete;

    /// Show @p run's running step in the card under @p root. Call after every
    /// change to the run and before the card's XML is created (it registers
    /// the cal_live_* subjects on first call); a null @p run shows nothing.
    void update(const calibration::CalibrationRun* run, lv_obj_t* root);

    /// Drop the live views: the charts, the stream, the graph. The card's
    /// widget tree deleting itself does this too.
    void detach();

  private:
    void init_subjects();
    void deinit_subjects();
    void publish_text(const LiveCardText& text);
    /// The card's translated strings for what it shows now; also on a language change.
    void publish_strings();
    static float belt_pull(const calibration::StepProgress& progress, int belt);
    void show_heat(lv_obj_t* root, const calibration::StepLive& live);
    void show_mesh(lv_obj_t* root, const calibration::StepLive& live);
    void show_shaper(lv_obj_t* root, const calibration::StepLive& live);
    void show_belt(const calibration::StepProgress& progress);
    void feed_mesh_map(lv_obj_t* map);
    void watch_deletion(lv_obj_t* body);
    static void on_body_deleted(lv_event_t* e);
    void stop_heat();
    void stop_shaper();

    bool subjects_initialized_ = false;
    SubjectManager subjects_;
    helix::Subject kind_;
    helix::Subject what_;
    char what_buf_[160]{};
    helix::Subject badge_;
    char badge_buf_[32]{};
    helix::Subject badge_tone_;
    helix::Subject warn_;
    char warn_buf_[96]{};
    helix::Subject hot_;
    helix::Subject heat_bed_;
    helix::Subject mesh_unprobed_;
    helix::Subject mesh_count_;
    char mesh_count_buf_[64]{};
    helix::Subject shaper_has_curve_;
    helix::Subject shaper_note_;
    char shaper_note_buf_[128]{};
    static constexpr int kBelts = 2;
    helix::Subject belt_fill_[kBelts];
    helix::Subject belt_done_[kBelts];
    helix::Subject belt_name_[kBelts];
    char belt_name_bufs_[kBelts][32]{};
    helix::Subject belt_state_[kBelts];
    char belt_state_bufs_[kBelts][32]{};

    /// What the card shows, kept so a language change can re-word it.
    bool shown_done_ = false;
    std::optional<calibration::StepKind> shown_kind_;
    calibration::StepProgress shown_progress_;
    bool shaper_cannot_stream_ = false;
    ObserverGuard language_observer_;

    /// The card's body the live views were built in; a new one means a new tree.
    WidgetRef body_;

    std::unique_ptr<helix::TempGraphController> heat_graph_;
    std::string heat_heater_;

    std::optional<int> mesh_fed_version_;

    ShaperSweepChart shaper_chart_;
    std::optional<calibration::LiveSweepFacts> shaper_facts_;
    bool shaper_facts_asked_ = false;
    char shaper_axis_ = 0;
    bool shaper_armed_ = false;

    helix::AsyncLifetimeGuard lifetime_;
};

/// The one card the guided run shows.
CalibrationLiveCard& get_calibration_live_card();

} // namespace helix::ui
