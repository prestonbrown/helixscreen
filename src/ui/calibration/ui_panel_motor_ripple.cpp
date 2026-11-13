// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_panel_motor_ripple.h"

#include "ui_callback_helpers.h"
#include "ui_frequency_response_chart.h"
#include "ui_nav.h"

#include "app_globals.h"
#include "core/static_panel_registry.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "motor_ripple_analysis.h"
#include "motor_ripple_store.h"
#include "motor_ripple_view.h"
#include "platform_capabilities.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "stepper_resonance_calibration.h"
#include "theme_manager.h"
#include "toolhead_homing.h"

#ifdef HELIX_ENABLE_MOCKS
#include "ripple_batch_source_mock.h"
#endif

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <cstdlib>

#include "hv/json.hpp"

namespace helix::ui {

namespace ripple = calibration::ripple;
using json = nlohmann::json;

namespace {

constexpr const char* kSeriesTokens[3] = {"info", "warning", "success"};
constexpr size_t kChartPoints = 151; ///< 2 mm/s apart over 0-300 mm/s

/// The stream a sweep reads: the mock's under --test, klippy's dump endpoint otherwise.
std::unique_ptr<calibration::IAccelBatchSource> make_ripple_source() {
#ifdef HELIX_ENABLE_MOCKS
    if (get_runtime_config()->should_mock_moonraker()) {
        return std::make_unique<ripple::MockRippleBatchSource>(ripple::mock_ripple_sample_rate());
    }
#endif
    return calibration::make_accel_batch_source(calibration::SweepPlan{});
}

const json* member(const json* j, const char* key) {
    return j && j->is_object() ? json_util::find_member(*j, key) : nullptr;
}

float array_at(const json* j, size_t i) {
    return j && j->is_array() && j->size() > i ? json_util::as_float((*j)[i]) : 0.0f;
}

void set_text(lv_subject_t& s, const std::string& text) {
    lv_subject_copy_string(&s, text.c_str());
}

int user_int(lv_event_t* e) {
    const char* v = static_cast<const char*>(lv_event_get_user_data(e));
    return v ? std::atoi(v) : 0;
}

} // namespace

MotorRipplePanel& get_motor_ripple_panel() {
    return lazy_global<MotorRipplePanel>("MotorRipplePanel");
}

MotorRipplePanel::MotorRipplePanel() = default;

MotorRipplePanel::~MotorRipplePanel() {
    runner_.reset();
    deinit_subjects_base(subjects_);
}

void MotorRipplePanel::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(view_, 0, "ripple_view", subjects_);
        UI_MANAGED_SUBJECT_INT(bed_clear_, 0, "ripple_bed_clear", subjects_);
        UI_MANAGED_SUBJECT_INT(compare_, 0, "ripple_compare", subjects_);
        UI_MANAGED_SUBJECT_INT(motor_, 0, "ripple_motor", subjects_);
        UI_MANAGED_SUBJECT_INT(has_y_, 1, "ripple_has_y", subjects_);
        UI_MANAGED_SUBJECT_INT(progress_, 0, "ripple_progress", subjects_);
        UI_MANAGED_SUBJECT_STRING(intro_text_, intro_buf_, "", "ripple_intro_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(progress_text_, progress_buf_, "", "ripple_progress_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(summary_text_, summary_buf_, "", "ripple_summary_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(reduction_text_, reduction_buf_, "", "ripple_reduction_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(error_text_, error_buf_, "", "ripple_error_text", subjects_);
    });
}

void MotorRipplePanel::register_callbacks() {
    register_xml_callbacks({
        {"on_ripple_start", [](lv_event_t*) { get_motor_ripple_panel().start_clicked(); }},
        {"on_ripple_again", [](lv_event_t*) { get_motor_ripple_panel().again_clicked(); }},
        {"on_ripple_motor",
         [](lv_event_t* e) { get_motor_ripple_panel().motor_clicked(user_int(e)); }},
        // Closing mid-run cancels in on_dismissed().
        {"on_ripple_close", [](lv_event_t*) { helix::nav::go_back(); }},
    });
}

lv_obj_t* MotorRipplePanel::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    build_chart();
    return overlay_root_;
}

bool MotorRipplePanel::run_active() const {
    return runner_ &&
           (runner_->phase() == Phase::Preparing || runner_->phase() == Phase::Measuring ||
            runner_->phase() == Phase::Restoring);
}

void MotorRipplePanel::before_show() {
    // Reopened while a run is still putting the printer back: show it, not Start.
    if (run_active()) {
        set_view(View::Running);
        return;
    }
    const auto& hw = get_moonraker_api()->hardware();
    const auto toggle = stepper_resonance_cal::compensation_toggle(hw);
    compare_mode_ = toggle.present();
    lv_subject_set_int(&compare_, compare_mode_ ? 1 : 0);
    const auto kin = ripple::ripple_kinematics(hw.kinematics());
    const size_t motors = 2;
    const int minutes = ripple::ripple_estimate_minutes(
        motors,
        ripple::sweep_speeds(static_cast<float>(lv_subject_get_int(
                                 get_printer_state().motion_state().get_max_velocity_subject())))
            .size(),
        compare_mode_);
    std::string intro = fmt::format(
        fmt::runtime(lv_tr("The toolhead runs back and forth at speeds from 20 to 300 mm/s while "
                           "the accelerometer measures how hard each motor shakes. It takes "
                           "about {0} minutes and it is loud. The speeds that shake most are "
                           "the ones to keep your outer walls away from.")),
        minutes);
    if (compare_mode_) {
        intro += "\n";
        intro += lv_tr("This printer has motor compensation, so the check runs twice: with it "
                       "off, then on.");
    }
    if (kin == ripple::Kinematics::Cartesian) {
        intro += "\n";
        intro += lv_tr("On a bed-slinger the Y motor is measured only with an accelerometer on "
                       "the bed.");
    }
    set_text(intro_text_, intro);
    lv_subject_set_int(&bed_clear_, 0);
    set_view(View::Start);
}

void MotorRipplePanel::on_dismissed() {
    if (runner_ && (runner_->phase() == Phase::Preparing || runner_->phase() == Phase::Measuring)) {
        spdlog::info("[MotorRipple] Closed mid-run: cancelling");
        runner_->cancel();
    }
}

void MotorRipplePanel::on_ui_destroyed() {
    destroy_chart();
}

void MotorRipplePanel::set_view(View v) {
    lv_subject_set_int(&view_, static_cast<int>(v));
}

void MotorRipplePanel::fail(const std::string& reason) {
    set_text(error_text_, reason);
    set_view(View::Error);
}

void MotorRipplePanel::start_clicked() {
    if (lv_subject_get_int(&bed_clear_) == 0 || run_active()) {
        return;
    }
    if (lv_subject_get_int(
            get_printer_state().print_state().get_machine_motion_blocked_subject()) != 0) {
        fail(lv_tr("The printer is busy. Run the check when no print or other motion is in "
                   "progress."));
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        fail(lv_tr("The printer is not connected."));
        return;
    }
    set_text(progress_text_, lv_tr("Getting ready..."));
    lv_subject_set_int(&progress_, 0);
    set_view(View::Running);
    calibration::query_live_sweep_facts(
        *api, lifetime_.bg_cb("MotorRipple::facts", [this](calibration::LiveSweepFacts facts) {
            begin_with_facts(facts);
        }));
}

void MotorRipplePanel::begin_with_facts(const calibration::LiveSweepFacts& facts) {
    if (!facts.socket_reachable) {
        fail(lv_tr("This check reads the accelerometer directly, so HelixScreen must run on the "
                   "printer's own computer."));
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    IMoonrakerClient* client = get_moonraker_client();
    if (!api || !client) {
        fail(lv_tr("The printer is not connected."));
        return;
    }
    const auto& hw = api->hardware();
    ripple::MotorRippleRunner::Request req;
    req.mode = compare_mode_ ? ripple::MotorRippleRunner::Mode::CompensationCompare
                             : ripple::MotorRippleRunner::Mode::Sweep;
    req.toggle = stepper_resonance_cal::compensation_toggle(hw);
    req.klippy_socket = facts.socket_path;
    req.plan.kinematics = ripple::ripple_kinematics(hw.kinematics());
    req.plan.probe_point = facts.cfg.probe_point;
    skipped_.clear();
    for (auto m : {ripple::MotorId::StepperX, ripple::MotorId::StepperY}) {
        if (auto g = hw.motor_geometry(m)) {
            req.geometry[m] = *g;
        }
    }
    if (req.plan.kinematics == ripple::Kinematics::CoreXY) {
        const auto chip =
            calibration::accel_chip_for_sweep(facts.cfg, calibration::SweepAxis::DIAGONAL);
        req.accel_chip[ripple::MotorId::StepperX] = chip.value_or("");
        req.accel_chip[ripple::MotorId::StepperY] = chip.value_or("");
    } else {
        req.accel_chip[ripple::MotorId::StepperX] =
            calibration::accel_chip_for_sweep(facts.cfg, calibration::SweepAxis::X).value_or("");
        req.plan.y_motor_measurable = !facts.cfg.accel_chip_y.empty();
        if (req.plan.y_motor_measurable) {
            req.accel_chip[ripple::MotorId::StepperY] = facts.cfg.accel_chip_y;
        } else {
            skipped_.push_back(
                {ripple::MotorId::StepperY, "no accelerometer is configured for the Y axis"});
        }
    }
    lv_subject_set_int(&has_y_, req.plan.y_motor_measurable ? 1 : 0);

    // The limits a run must put back, read now rather than from config: a
    // slicer's SET_VELOCITY_LIMIT may already have changed them.
    const json objects = {
        {"toolhead", json::array({"axis_minimum", "axis_maximum", "max_velocity", "max_accel",
                                  "minimum_cruise_ratio", "max_accel_to_decel", "position"})},
        {"gcode_move", json::array({"homing_origin"})}};
    client->send_jsonrpc(
        "printer.objects.query", json{{"objects", objects}},
        lifetime_.bg_cb(
            "MotorRipple::limits",
            [this, req](const json& response) mutable {
                const json* status = member(member(&response, "result"), "status");
                const json* th = member(status, "toolhead");
                const json* origin = member(member(status, "gcode_move"), "homing_origin");
                if (!th) {
                    fail(lv_tr("The printer did not report its toolhead limits."));
                    return;
                }
                const json* lo = member(th, "axis_minimum");
                const json* hi = member(th, "axis_maximum");
                auto& p = req.plan;
                p.x_min = array_at(lo, 0);
                p.y_min = array_at(lo, 1);
                p.x_max = array_at(hi, 0);
                p.y_max = array_at(hi, 1);
                p.max_z = array_at(hi, 2);
                p.has_xy_limits = lo && hi && p.x_max > p.x_min && p.y_max > p.y_min;
                p.current_z = array_at(member(th, "position"), 2);
                p.origin_x = array_at(origin, 0);
                p.origin_y = array_at(origin, 1);
                p.origin_z = array_at(origin, 2);
                p.max_velocity_mm_s = json_util::safe_float(*th, "max_velocity");
                p.max_accel_mm_s2 = json_util::safe_float(*th, "max_accel");
                if (const json* mcr = member(th, "minimum_cruise_ratio"); mcr && mcr->is_number()) {
                    p.minimum_cruise_ratio = mcr->get<float>();
                } else {
                    p.max_accel_to_decel = json_util::safe_float(*th, "max_accel_to_decel");
                }
                begin_run(std::move(req));
            }),
        lifetime_.bg_cb("MotorRipple::limits_failed",
                        [this](const MoonrakerError& err) {
                            fail(fmt::format(fmt::runtime(lv_tr("Could not read the printer's "
                                                                "limits: {}")),
                                             err.message));
                        }),
        0, /*silent=*/true);
}

void MotorRipplePanel::begin_run(ripple::MotorRippleRunner::Request req) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        fail(lv_tr("The printer is not connected."));
        return;
    }
    const auto speeds = ripple::sweep_speeds(req.plan.max_velocity_mm_s);
    speed_max_ = speeds.empty() ? 300.0f : speeds.back();
    for (auto& per_motor : xs_) {
        for (auto& v : per_motor) {
            v.clear();
        }
    }
    for (auto& per_motor : ys_) {
        for (auto& v : per_motor) {
            v.clear();
        }
    }
    y_max_ = 0.0f;
    bands_.clear();
    lv_subject_set_int(&motor_, 0);
    if (chart_) {
        for (size_t i = 0; i < 3; ++i) {
            ui_frequency_response_chart_set_series_muted(chart_, series_[i], compare_mode_);
        }
        ui_frequency_response_chart_set_bands(chart_, nullptr, 0);
        ui_frequency_response_chart_set_freq_range(chart_, 0.0f, speed_max_);
        show_motor(0);
    }

    ensure_homed_then(
        api, lifetime_,
        [this, req = std::move(req)]() mutable {
            IMoonrakerAPI* a = get_moonraker_api();
            if (!a) {
                fail(lv_tr("The printer is not connected."));
                return;
            }
            runner_ = std::make_unique<ripple::MotorRippleRunner>(*a, make_ripple_source);
            ripple::MotorRippleRunner::Callbacks cb;
            cb.on_speed = [this](bool comp, ripple::MotorId m, const ripple::SpeedResult& r,
                                 size_t index,
                                 size_t total) { on_speed(comp, m, r, index, total); };
            cb.on_done = [this](const ripple::SweepResult& r) { on_done(r); };
            cb.on_failed = [this](const std::string& why) { fail(why); };
            if (!runner_->start(std::move(req), std::move(cb))) {
                fail(lv_tr("This printer's travel or settings cannot hold the test moves."));
            }
        },
        [this](const MoonrakerError& err) {
            fail(fmt::format(fmt::runtime(lv_tr("Homing failed: {}")), err.message));
        });
}

void MotorRipplePanel::on_speed(bool compensated, ripple::MotorId motor,
                                const ripple::SpeedResult& r, size_t index, size_t total) {
    const size_t m = motor == ripple::MotorId::StepperX ? 0 : 1;
    for (size_t k = 0; k < 3; ++k) {
        const size_t s = k + (compensated ? 3 : 0);
        xs_[m][s].push_back(r.speed_mm_s);
        ys_[m][s].push_back(r.ripple[k] ? *r.ripple[k] : NAN);
        if (r.ripple[k]) {
            y_max_ = std::max(y_max_, *r.ripple[k]);
        }
    }
    set_text(progress_text_,
             ripple::ripple_progress_text(motor, r.speed_mm_s, index, total, compensated));
    lv_subject_set_int(&progress_, total ? static_cast<int>(100 * (index + 1) / total) : 0);
    if (lv_subject_get_int(&motor_) != static_cast<int>(m)) {
        lv_subject_set_int(&motor_, static_cast<int>(m));
    }
    show_motor(static_cast<int>(m));
}

void MotorRipplePanel::on_done(const ripple::SweepResult& result) {
    ripple::SweepResult saved = result;
    saved.skipped = skipped_;
    if (IMoonrakerAPI* save_api = get_moonraker_api()) {
        ripple::RippleResultCache::instance().save(*save_api, saved);
    }
    bands_ = ripple::merge_bands(result.bands);
    std::string summary = ripple::ripple_band_summary(result.bands);
    summary += "\n\n";
    summary += lv_tr("Keep outer walls out of these speeds (Orca: Outer wall speed; "
                     "PrusaSlicer: External perimeters).");
    if (result.ceiling_hz > 0.0f && result.ceiling_hz < 700.0f) {
        summary += "\n";
        summary += fmt::format(
            fmt::runtime(lv_tr("This printer's accelerometer measures up to {0:.0f} Hz, so "
                               "faster ripples are not shown.")),
            result.ceiling_hz);
    }
    for (const auto& s : skipped_) {
        summary += "\n";
        summary += fmt::format(fmt::runtime(lv_tr("The {0} motor was not measured: {1}.")),
                               s.motor == ripple::MotorId::StepperX ? "X" : "Y", s.reason);
    }
    if (!result.printer_restored) {
        summary += "\n";
        summary += lv_tr("The printer refused the command that puts its speed limits back. "
                         "Restart Klipper before printing.");
    }
    set_text(summary_text_, summary);
    IMoonrakerAPI* api = get_moonraker_api();
    const auto toggle = api ? stepper_resonance_cal::compensation_toggle(api->hardware())
                            : stepper_resonance_cal::CompensationToggle{};
    set_text(reduction_text_,
             result.compensated_sweeps.empty()
                 ? std::string()
                 : ripple::ripple_reduction_summary(result, toggle.applies_factory_values));
    if (chart_) {
        std::vector<FrChartBand> fb;
        for (const auto& b : bands_) {
            fb.push_back({b.lo_mm_s, b.hi_mm_s});
        }
        ui_frequency_response_chart_set_bands(chart_, fb.data(), fb.size());
    }
    show_motor(lv_subject_get_int(&motor_));
    set_view(View::Results);
}

void MotorRipplePanel::again_clicked() {
    lv_subject_set_int(&bed_clear_, 0);
    set_view(View::Start);
}

void MotorRipplePanel::motor_clicked(int motor) {
    lv_subject_set_int(&motor_, motor == 1 ? 1 : 0);
    show_motor(motor == 1 ? 1 : 0);
}

void MotorRipplePanel::build_chart() {
    lv_obj_t* host = overlay_root_ ? lv_obj_find_by_name(overlay_root_, "ripple_chart") : nullptr;
    if (!host) {
        return;
    }
    chart_ = ui_frequency_response_chart_create(host);
    if (!chart_) {
        return;
    }
    ui_frequency_response_chart_configure_for_platform(chart_, PlatformCapabilities::detect().tier);
    ui_frequency_response_chart_set_x_unit(chart_, "mm/s");
    ui_frequency_response_chart_set_freq_range(chart_, 0.0f, speed_max_);
    const char* names[6] = {"1x", "2x", "4x", "1x on", "2x on", "4x on"};
    for (size_t i = 0; i < 6; ++i) {
        series_[i] = ui_frequency_response_chart_add_series(
            chart_, names[i], theme_manager_get_color(kSeriesTokens[i % 3]));
        if (i < 3) {
            ui_frequency_response_chart_set_series_muted(chart_, series_[i], compare_mode_);
        }
    }
}

void MotorRipplePanel::destroy_chart() {
    if (chart_) {
        ui_frequency_response_chart_destroy(chart_);
        chart_ = nullptr;
    }
}

void MotorRipplePanel::show_motor(int motor) {
    if (!chart_) {
        return;
    }
    const size_t m = motor == 1 ? 1 : 0;
    ui_frequency_response_chart_set_amplitude_range(chart_, 0.0f,
                                                    y_max_ > 0.0f ? y_max_ * 1.1f : 1000.0f);
    // The chart spaces its points evenly over the range; the sweep's speeds
    // are geometric, so they are resampled onto an even grid first.
    std::vector<float> grid(kChartPoints);
    for (size_t i = 0; i < kChartPoints; ++i) {
        grid[i] = speed_max_ * static_cast<float>(i) / static_cast<float>(kChartPoints - 1);
    }
    for (size_t s = 0; s < 6; ++s) {
        const auto ys =
            ripple::ripple_resample_uniform(xs_[m][s], ys_[m][s], 0.0f, speed_max_, kChartPoints);
        ui_frequency_response_chart_set_data(chart_, series_[s], grid.data(), ys.data(), ys.size());
    }
}

void open_motor_ripple_panel() {
    get_motor_ripple_panel().show(lv_screen_active());
}

void init_motor_ripple_row_handler() {
    register_xml_callbacks({
        {"on_motor_ripple_row_clicked", [](lv_event_t*) { open_motor_ripple_panel(); }},
    });
}

} // namespace helix::ui
