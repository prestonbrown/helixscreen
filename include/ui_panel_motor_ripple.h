// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"
#include "motor_ripple_runner.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <array>
#include <memory>
#include <vector>

struct ui_frequency_response_chart_t;

namespace helix::ui {

/**
 * @brief Vertical Artifact Check: a motor ripple speed sweep and its results
 *
 * Start (clear the bed, start), Running (the chart fills per speed), Results
 * (bands of speed to avoid, and on printers with motor compensation, the
 * ripple with it off against on), Error. Closing mid-run cancels, and the
 * runner puts the printer back.
 *
 * @threading Main thread only
 */
class MotorRipplePanel : public OverlayBase {
  public:
    enum class View { Start = 0, Running = 1, Results = 2, Error = 3 };

    MotorRipplePanel();
    ~MotorRipplePanel() override;

    const char* get_name() const override {
        return "Vertical Artifact Check";
    }
    const char* xml_component() const override {
        return "motor_ripple_panel";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    void before_show() override;
    void on_dismissed() override;
    void on_ui_destroyed() override;

    void start_clicked();
    void again_clicked();
    void motor_clicked(int motor);

  private:
    using Phase = calibration::ripple::MotorRippleRunner::Phase;

    [[nodiscard]] bool run_active() const;
    void set_view(View v);
    void fail(const std::string& reason);
    void begin_with_facts(const calibration::LiveSweepFacts& facts);
    void begin_run(calibration::ripple::MotorRippleRunner::Request req);
    void on_speed(bool compensated, calibration::ripple::MotorId motor,
                  const calibration::ripple::SpeedResult& r, size_t index, size_t total);
    void on_done(const calibration::ripple::SweepResult& result);
    void build_chart();
    void destroy_chart();
    void show_motor(int motor);

    SubjectManager subjects_;
    lv_subject_t view_{};
    lv_subject_t bed_clear_{};
    lv_subject_t compare_{};
    lv_subject_t motor_{};
    lv_subject_t has_y_{};
    lv_subject_t progress_{};
    lv_subject_t intro_text_{};
    char intro_buf_[512] = {};
    lv_subject_t progress_text_{};
    char progress_buf_[160] = {};
    lv_subject_t summary_text_{};
    char summary_buf_[768] = {};
    lv_subject_t reduction_text_{};
    char reduction_buf_[512] = {};
    lv_subject_t error_text_{};
    char error_buf_[256] = {};

    std::unique_ptr<calibration::ripple::MotorRippleRunner> runner_;
    ui_frequency_response_chart_t* chart_ = nullptr;
    std::array<int, 6> series_{}; ///< 1x/2x/4x plain, then compensated
    /// Per motor, per series: speeds and amplitudes collected so far.
    std::array<std::array<std::vector<float>, 6>, 2> xs_;
    std::array<std::array<std::vector<float>, 6>, 2> ys_;
    float y_max_ = 0.0f;
    float speed_max_ = 300.0f;
    std::vector<calibration::ripple::RippleBand> bands_;
    std::vector<calibration::ripple::SkippedMotor> skipped_;
    bool compare_mode_ = false;
};

MotorRipplePanel& get_motor_ripple_panel();
void open_motor_ripple_panel();
/// The Advanced row's click callback; once at startup.
void init_motor_ripple_row_handler();

} // namespace helix::ui
