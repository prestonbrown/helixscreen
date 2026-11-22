// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_run_steps.h"

#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_zoffset.h"

#include "async_lifetime_guard.h"
#include "calibration_abort.h"
#include "calibration_catalog_inputs.h"
#include "calibration_config_results.h"
#include "calibration_history_store.h"
#include "core/timer_guard.h"
#include "i_moonraker_api.h"
#include "input_shaper_calibrator.h"
#include "klippy_state.h"
#include "moonraker_error.h"
#include "pid_calibrate_tolerance.h"
#include "pid_progress_tracker.h"
#include "printer_calibration_state.h"
#include "printer_discovery.h"
#include "printer_network_state.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "translation_loader.h"
#include "z_offset_utils.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace helix::calibration {

namespace {

constexpr const char* kBedHeater = "heater_bed";

/// The only way to stop a calibration command Klipper is already running: M112,
/// then a firmware restart, announced as an expected restart.
void stop_with_firmware_restart(IMoonrakerAPI& api, const char* tag) {
    helix::ui::begin_expected_klippy_restart("Firmware restarting...", &api);
    emergency_stop_and_restart(&api, tag);
}

// ============================================================================
// Heaters: PID_CALIBRATE per heater, or MPC_CALIBRATE where it already runs MPC
// ============================================================================

class HeatersStep : public CalibrationStep {
  public:
    explicit HeatersStep(StepInfo info) : info_(std::move(info)) {}
    const StepInfo& info() const override {
        return info_;
    }

    void start(StepContext& ctx) override {
        ctx_ = &ctx;
        aborted_ = false;
        heaters_.clear();
        summaries_.clear();
        controls_.clear();
        command_sent_ = false;
        values_ = nlohmann::json::object();
        heaters_.push_back(
            {ctx.api.printer_state().temperature_state().active_extruder_name(), kRunNozzleTempC});
        if (ctx.api.hardware().has_heater_bed()) {
            heaters_.push_back({kBedHeater, kRunBedTempC});
        }
        // Each heater's share of the bar is its share of the step's time: a
        // bed's cycles can take an hour where the nozzle's take minutes.
        shares_.clear();
        priced_s_.clear();
        failed_index_.reset();
        // What is pending before this step stages anything, so a late recheck
        // tells this step's gains from an earlier calibration's left unsaved.
        pending_before_ = nlohmann::json();
        ctx.api.advanced().get_save_config_pending_items(
            lifetime_.bg_cb("CalibrationRun.Heaters.pending_before",
                            [this](const nlohmann::json& items) { pending_before_ = items; }),
            lifetime_.bg_cb("CalibrationRun.Heaters.pending_before_error",
                            [](const MoonrakerError&) {}));
        float total = 0.0f;
        for (const auto& h : heaters_) {
            priced_s_.push_back(heater_calibration_seconds(
                price_heater(ctx.api.printer_state(), h.name, h.temp_c)));
            shares_.push_back(priced_s_.back());
            total += shares_.back();
        }
        for (auto& share : shares_) {
            share = total > 0.0f ? share / total : 1.0f / static_cast<float>(heaters_.size());
        }
        index_ = 0;
        run_current();
    }

    /// Only a running PID_CALIBRATE or MPC_CALIBRATE needs M112; while the
    /// control type is being asked nothing runs in Klipper.
    void abort() override {
        if (!ctx_ || aborted_) {
            return;
        }
        lifetime_.invalidate();
        if (abort_restarts_firmware()) {
            aborted_ = true;
            stop_with_firmware_restart(ctx_->api, "CalibrationRun.Heaters");
        }
        auto* ctx = ctx_;
        ctx_ = nullptr;
        ctx->done(StepOutcome::aborted());
    }

    bool abort_restarts_firmware() const override {
        return ctx_ && command_sent_;
    }

    /// The PID calibration that failed here finished in Klipper after all:
    /// read the gains it staged and report the heater tuned.
    void recheck_after_wait(IMoonrakerAPI& api,
                            std::function<void(std::optional<StepOutcome>)> done) override {
        if (!failed_index_ || failed_mpc_) {
            done(std::nullopt);
            return;
        }
        const size_t at = *failed_index_;
        const Heater h = heaters_[at];
        api.advanced().get_save_config_pending_items(
            lifetime_.bg_cb("CalibrationRun.Heaters.recheck",
                            [this, h, at, done](const nlohmann::json& items) {
                                const auto gains = newly_staged_pid(pending_before_, items, h.name);
                                if (!gains) {
                                    done(std::nullopt);
                                    return;
                                }
                                record_pid(h, gains->kp, gains->ki, gains->kd);
                                failed_index_.reset();
                                if (at + 1 >= heaters_.size()) {
                                    done(StepOutcome::finished(
                                        {joined_summary(), values_, joined_brief()}));
                                    return;
                                }
                                // This heater tuned after all; the next one never ran.
                                const bool next_bed = heaters_[at + 1].name == kBedHeater;
                                done(StepOutcome::failed_after(
                                    fmt::format(fmt::runtime(lv_tr("{} did not run")),
                                                next_bed ? lv_tr("Bed") : lv_tr("Nozzle")),
                                    {joined_summary(), values_, joined_brief()}));
                            }),
            lifetime_.bg_cb("CalibrationRun.Heaters.recheck_error",
                            [done](const MoonrakerError&) { done(std::nullopt); }));
    }

  private:
    struct Heater {
        std::string name;
        int temp_c;
    };

    bool is_bed() const {
        return heaters_[index_].name == kBedHeater;
    }

    const char* label() const {
        return is_bed() ? lv_tr("Bed") : lv_tr("Nozzle");
    }

    void report(const std::string& method, int done_fraction_pct, const std::string& detail = {}) {
        if (!ctx_) {
            return;
        }
        float before = 0.0f;
        for (size_t i = 0; i < index_; ++i) {
            before += shares_[i];
        }
        const int pct =
            static_cast<int>(100.0f * (before + shares_[index_] * done_fraction_pct / 100.0f));
        std::string phase = fmt::format(fmt::runtime(lv_tr("{}: {} at {}°C")), label(), method,
                                        heaters_[index_].temp_c);
        if (!detail.empty()) {
            phase += " · " + detail;
        }
        StepLive live;
        live.heater = heaters_[index_].name;
        live.target_c = heaters_[index_].temp_c;
        live.mpc = mpc_;
        ctx_->progress({std::move(phase), std::clamp(pct, 0, 99), std::move(live)});
    }

    /// A PID cycle's progress. Where the firmware repeats cycles until they
    /// agree within a tolerance, the bar follows how close they are on a log
    /// scale (0.70 of 0.05 reads 12%, 0.27 reads 44%); otherwise it counts the
    /// cycles a fixed-length calibration runs.
    void report_pid_cycle(int sample, float tolerance, std::optional<double> target) {
        if (target && tolerance > 0.0f) {
            const double done = std::log(1.0 / tolerance) / std::log(1.0 / *target);
            report("PID", std::clamp(static_cast<int>(done * 100), 0, 95),
                   fmt::format(fmt::runtime(lv_tr("cycle {}, {:.2f} (needs {:.2f})")), sample,
                               tolerance, *target));
            return;
        }
        report("PID", std::min(95, sample * 100 / PidProgressTracker::kExpectedCycles),
               fmt::format(fmt::runtime(lv_tr("cycle {}")), sample));
    }

    void run_current() {
        if (!ctx_) {
            return;
        }
        if (index_ >= heaters_.size()) {
            auto* ctx = ctx_;
            ctx_ = nullptr;
            ctx->done(StepOutcome::finished({joined_summary(), values_, joined_brief()}));
            return;
        }
        mpc_ = false;
        report(lv_tr("checking"), 0);
        const std::string heater = heaters_[index_].name;
        ctx_->api.advanced().get_heater_control_type(
            heater,
            lifetime_.bg_cb("CalibrationRun.Heaters.control",
                            [this](const std::string& type, bool /*names_cooling_fan*/) {
                                if (type == "mpc") {
                                    run_mpc();
                                } else {
                                    run_pid();
                                }
                            }),
            // No control type reported: a firmware without MPC.
            lifetime_.bg_cb("CalibrationRun.Heaters.control_none",
                            [this](const MoonrakerError&) { run_pid(); }));
    }

    void run_pid() {
        if (!ctx_) {
            return;
        }
        const Heater h = heaters_[index_];
        spdlog::info("[CalibrationRun] PID_CALIBRATE {} at {}°C", h.name, h.temp_c);
        report("PID", 0);
        command_sent_ = true;
        ctx_->api.advanced().start_pid_calibrate(
            h.name, h.temp_c,
            lifetime_.bg_cb("CalibrationRun.Heaters.pid_done",
                            [this, h](float kp, float ki, float kd) {
                                record_pid(h, kp, ki, kd);
                                next();
                            }),
            lifetime_.bg_cb("CalibrationRun.Heaters.pid_error",
                            [this](const MoonrakerError& err) { fail(err.message); }),
            lifetime_.bg_cb(
                "CalibrationRun.Heaters.pid_progress",
                [this, target = helix::pid::calibrate_tolerance(
                           ctx_->api.hardware().pid_calibrate_takes_tolerance(), h.name)](
                    int sample, float tolerance) { report_pid_cycle(sample, tolerance, target); }),
            static_cast<uint32_t>(priced_s_[index_] * 1000.0f));
    }

    void record_pid(const Heater& h, double kp, double ki, double kd) {
        const bool bed = h.name == kBedHeater;
        values_[h.name] = {
            {"control", "pid"}, {"temp", h.temp_c}, {"kp", kp}, {"ki", ki}, {"kd", kd}};
        summaries_.push_back(
            fmt::format("{} {}", bed ? lv_tr("Bed") : lv_tr("Nozzle"), format_pid(kp, ki, kd)));
        controls_.push_back({bed, "pid"});
    }

    void run_mpc() {
        if (!ctx_) {
            return;
        }
        const Heater h = heaters_[index_];
        spdlog::info("[CalibrationRun] MPC_CALIBRATE {} at {}°C", h.name, h.temp_c);
        mpc_ = true;
        report("MPC", 0);
        command_sent_ = true;
        ctx_->api.advanced().start_mpc_calibrate(
            h.name, h.temp_c, 0,
            lifetime_.bg_cb("CalibrationRun.Heaters.mpc_done",
                            [this, h](const IAdvancedAPI::MPCResult& r) {
                                values_[h.name] = {
                                    {"control", "mpc"},
                                    {"temp", h.temp_c},
                                    {"block_heat_capacity", r.block_heat_capacity},
                                    {"sensor_responsiveness", r.sensor_responsiveness},
                                    {"ambient_transfer", r.ambient_transfer}};
                                summaries_.push_back(fmt::format(
                                    "{} {}", label(), format_mpc(r.block_heat_capacity)));
                                controls_.push_back({is_bed(), "mpc"});
                                next();
                            }),
            lifetime_.bg_cb("CalibrationRun.Heaters.mpc_error",
                            [this](const MoonrakerError& err) { fail(err.message); }),
            lifetime_.bg_cb("CalibrationRun.Heaters.mpc_progress",
                            [this](int phase, int total, const std::string&) {
                                report("MPC", total > 0 ? std::min(95, phase * 100 / total) : 0);
                            }));
    }

    void next() {
        command_sent_ = false;
        ++index_;
        run_current();
    }

    void fail(const std::string& why) {
        if (!ctx_) {
            return;
        }
        auto* ctx = ctx_;
        ctx_ = nullptr;
        const std::string message = MoonrakerError::friendly_error_line(why);
        failed_index_ = index_;
        failed_mpc_ = mpc_;
        if (summaries_.empty()) {
            ctx->done(StepOutcome::failed(message));
            return;
        }
        // The heaters tuned before this one are staged in Klipper.
        ctx->done(StepOutcome::failed_after(message, {joined_summary(), values_, joined_brief()}));
    }

    static std::string joined(const std::vector<std::string>& parts) {
        std::string out;
        for (const auto& s : parts) {
            out += out.empty() ? s : " · " + s;
        }
        return out;
    }
    std::string joined_summary() const {
        return joined(summaries_);
    }
    std::string joined_brief() const {
        return format_heater_controls(controls_);
    }

    StepInfo info_;
    StepContext* ctx_ = nullptr;
    std::vector<Heater> heaters_;
    /// Each heater's fraction of the step's time, summing to 1.
    std::vector<float> shares_;
    /// Each heater's priced seconds (heater_calibration_seconds()).
    std::vector<float> priced_s_;
    /// The heater the step failed on, and how, for recheck_after_wait().
    std::optional<size_t> failed_index_;
    bool failed_mpc_ = false;
    /// Klipper's pending config as the step started.
    nlohmann::json pending_before_;
    size_t index_ = 0;
    std::vector<std::string> summaries_;
    std::vector<HeaterControl> controls_;
    nlohmann::json values_ = nlohmann::json::object();
    /// The current heater runs MPC_CALIBRATE.
    bool mpc_ = false;
    /// A calibration command is running in Klipper for the current heater.
    bool command_sent_ = false;
    /// The last abort() restarted the firmware.
    bool aborted_ = false;
    helix::AsyncLifetimeGuard lifetime_;
};

// ============================================================================
// Bed mesh: the bed mesh panel's own calibration, into the default profile
// ============================================================================

class BedMeshStep : public CalibrationStep {
  public:
    explicit BedMeshStep(StepInfo info) : info_(std::move(info)) {}
    ~BedMeshStep() override {
        if (ctx_) {
            get_global_bed_mesh_panel().end_run(BedMeshPanel::RunEnd::Finished);
        }
    }
    const StepInfo& info() const override {
        return info_;
    }

    void start(StepContext& ctx) override {
        ctx_ = &ctx;
        aborted_ = false;
        ctx.progress({lv_tr("Probing the bed"), -1, {}});
        BedMeshPanel::RunHooks hooks;
        hooks.stored = [this](const std::string& profile, std::optional<double> range) {
            if (auto* c = take()) {
                nlohmann::json values = {{"profile", profile}};
                if (range) {
                    values["range"] = *range;
                }
                c->done(StepOutcome::finished(
                    {range ? format_mesh_range(*range)
                           : fmt::format(fmt::runtime(lv_tr("Profile '{}'")), profile),
                     std::move(values)}));
            }
        };
        hooks.failed = [this](const std::string& why, const std::string& staged_profile) {
            auto* c = take();
            if (!c) {
                return;
            }
            if (staged_profile.empty()) {
                c->done(StepOutcome::failed(why));
                return;
            }
            c->done(StepOutcome::failed_after(
                why,
                {fmt::format(fmt::runtime(lv_tr("a temporary mesh profile '{}'")), staged_profile),
                 {{"profile", staged_profile}}}));
        };
        hooks.probed = [this](int current, int total) {
            if (!ctx_) {
                return;
            }
            StepLive live;
            live.probed = current;
            live.probe_total = total;
            ctx_->progress(
                {total > 0
                     ? fmt::format(fmt::runtime(lv_tr("Probing point {} of {}")), current, total)
                     : std::string(lv_tr("Probing the bed")),
                 total > 0 ? std::clamp(current * 100 / total, 0, 99) : -1, std::move(live)});
        };
        get_global_bed_mesh_panel().calibrate_for_run(&ctx.api, std::move(hooks));
    }

    /// Only a running BED_MESH_CALIBRATE needs M112; before it is sent the
    /// panel's preparation is cancelled and the heaters it turned on go off.
    void abort() override {
        if (!ctx_) {
            return;
        }
        aborted_ = abort_restarts_firmware();
        auto* c = take(aborted_ ? BedMeshPanel::RunEnd::Stopped : BedMeshPanel::RunEnd::Cancelled);
        if (aborted_) {
            stop_with_firmware_restart(c->api, "CalibrationRun.BedMesh");
        }
        c->done(StepOutcome::aborted());
    }

    bool abort_restarts_firmware() const override {
        return ctx_ && get_global_bed_mesh_panel().calibration_command_sent();
    }

  private:
    StepContext* take(BedMeshPanel::RunEnd how = BedMeshPanel::RunEnd::Finished) {
        auto* c = ctx_;
        ctx_ = nullptr;
        if (c) {
            get_global_bed_mesh_panel().end_run(how);
        }
        return c;
    }

    StepInfo info_;
    StepContext* ctx_ = nullptr;
    bool aborted_ = false;
};

// ============================================================================
// Input shaper: the calibrator's noise check, then both axes
// ============================================================================

class InputShaperStep : public CalibrationStep {
  public:
    explicit InputShaperStep(StepInfo info) : info_(std::move(info)) {}
    const StepInfo& info() const override {
        return info_;
    }

    void start(StepContext& ctx) override {
        ctx_ = &ctx;
        aborted_ = false;
        calibrator_ = std::make_unique<InputShaperCalibrator>(&ctx.api);
        ctx.progress({lv_tr("Checking the accelerometer"), -1, {}});
        calibrator_->run_both_axes(
            lifetime_.bg_cb(
                "CalibrationRun.Shaper.progress",
                [this](char axis, int percent, ShaperCalibrationPhase phase, float freq_hz) {
                    if (!ctx_) {
                        return;
                    }
                    const bool analyzing = phase != ShaperCalibrationPhase::Sweeping;
                    const std::string text =
                        analyzing ? fmt::format(fmt::runtime(lv_tr("Analyzing the {} axis")), axis)
                                  : fmt::format(fmt::runtime(lv_tr("Shaking the {} axis")), axis);
                    const int base = axis == 'X' ? 0 : 50;
                    StepLive live;
                    live.axis = axis;
                    live.analyzing = analyzing;
                    live.sweep_hz = analyzing ? 0.0f : freq_hz;
                    ctx_->progress({text, analyzing ? -1 : base + percent / 2, std::move(live)});
                }),
            lifetime_.bg_cb(
                "CalibrationRun.Shaper.done",
                [this](const InputShaperCalibrator::CalibrationResults& r) { finish(r); }),
            lifetime_.bg_cb("CalibrationRun.Shaper.error", [this](const std::string& why) {
                auto* c = take();
                if (!c) {
                    return;
                }
                // X's SHAPER_CALIBRATE staged its shaper before Y failed.
                const auto& done_axes = calibrator_->get_results();
                if (done_axes.has_x()) {
                    const auto& x = done_axes.x_result;
                    c->done(StepOutcome::failed_after(
                        why,
                        {join_shaper_axes(format_shaper_axis(x.shaper_type, x.shaper_freq), ""),
                         {{"x", {{"type", x.shaper_type}, {"freq", x.shaper_freq}}}}}));
                    return;
                }
                c->done(StepOutcome::failed(why));
            }));
    }

    void abort() override {
        aborted_ = abort_restarts_firmware();
        auto* c = take();
        if (!c) {
            return;
        }
        lifetime_.invalidate();
        if (aborted_) {
            // Only a running SHAPER_CALIBRATE needs M112.
            helix::ui::begin_expected_klippy_restart("Firmware restarting...", &c->api);
            calibrator_->emergency_abort();
        } else {
            // Homing or the noise check: dropping the calibrator ends its chain.
            calibrator_.reset();
        }
        c->done(StepOutcome::aborted());
    }

    bool abort_restarts_firmware() const override {
        if (!ctx_ || !calibrator_) {
            return false;
        }
        const auto state = calibrator_->get_state();
        return state == InputShaperCalibrator::State::TESTING_X ||
               state == InputShaperCalibrator::State::TESTING_Y;
    }

  private:
    StepContext* take() {
        auto* c = ctx_;
        ctx_ = nullptr;
        return c;
    }

    void finish(const InputShaperCalibrator::CalibrationResults& r) {
        auto* c = take();
        if (!c) {
            return;
        }
        // Shaper names are Klipper's lowercase ids; the screens show them in capitals.
        const auto axis = [](const InputShaperResult& a) {
            return a.shaper_type.empty() ? std::string()
                                         : format_shaper_axis(a.shaper_type, a.shaper_freq);
        };
        nlohmann::json values = {
            {"x", {{"type", r.x_result.shaper_type}, {"freq", r.x_result.shaper_freq}}},
            {"y", {{"type", r.y_result.shaper_type}, {"freq", r.y_result.shaper_freq}}}};
        c->done(StepOutcome::finished(
            {join_shaper_axes(axis(r.x_result), axis(r.y_result)), std::move(values)}));
    }

    StepInfo info_;
    StepContext* ctx_ = nullptr;
    std::unique_ptr<InputShaperCalibrator> calibrator_;
    bool aborted_ = false;
    helix::AsyncLifetimeGuard lifetime_;
};

// ============================================================================
// Z offset: the Z offset panel in run mode; leaving it unaccepted skips
// ============================================================================

class ZOffsetStep : public CalibrationStep {
  public:
    ZOffsetStep(StepInfo info, bool warm_bed) : info_(std::move(info)), warm_bed_(warm_bed) {}
    const StepInfo& info() const override {
        return info_;
    }

    void start(StepContext& ctx) override {
        ctx_ = &ctx;
        accepted_ = false;
        // What a firmware-managed offset is before the step, for Discard all.
        old_offset_mm_ = lv_subject_get_int(
                             ctx.api.printer_state().motion_state().get_gcode_z_offset_subject()) /
                         1000.0;
        ctx.progress({lv_tr("Waiting for you at the Z offset screen"), -1, {}});
        get_global_zoffset_cal_panel().open_for_run(
            &ctx.api, ctx.index, ctx.count, warm_bed_,
            [this, alive = std::weak_ptr<int>(alive_)](ZOffsetCalibrationPanel::RunEnd end, float z,
                                                       ZOffsetCalibrationStrategy strategy) {
                if (alive.expired() ||
                    !ctx_) { // L081_OK: main thread, from the panel's own handlers
                    return;
                }
                if (end == ZOffsetCalibrationPanel::RunEnd::Left) {
                    ask_continue();
                    return;
                }
                if (end == ZOffsetCalibrationPanel::RunEnd::Skipped) {
                    auto* c = ctx_;
                    ctx_ = nullptr;
                    c->done(StepOutcome::skipped());
                    return;
                }
                auto* c = ctx_;
                ctx_ = nullptr;
                accepted_ = true;
                strategy_ = strategy;
                if (strategy == ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) {
                    // The panel set this offset itself; nothing is staged.
                    c->done(StepOutcome::finished({format_z_offset(z), {{"z_offset", z}}}));
                    return;
                }
                read_staged(*c, z);
            });
    }

    /// A manual probe ends with ABORT; nothing is sent when none is running.
    void abort() override {
        if (auto* c = ctx_) {
            ctx_ = nullptr;
            get_global_zoffset_cal_panel().abort_run();
            c->done(StepOutcome::aborted());
        }
    }

    /// The firmware persists a firmware-managed offset as soon as it is set.
    bool restores_on_discard() const override {
        return accepted_ && strategy_ == ZOffsetCalibrationStrategy::FIRMWARE_MANAGED;
    }

    void restore(IMoonrakerAPI& api, std::function<void(std::string note)> done) override {
        auto& ps = api.printer_state();
        const std::string cmd = zoffset::firmware_managed_offset_gcode(
            old_offset_mm_, api.hardware(),
            lv_subject_get_int(ps.print_state().get_print_active_subject()) != 0);
        spdlog::info("[CalibrationRun] Restoring the Z offset: {}", cmd);
        const double old_mm = old_offset_mm_;
        api.execute_gcode(
            cmd,
            lifetime_.bg_cb("CalibrationRun.ZOffset.restored",
                            [done, old_mm]() {
                                done(fmt::format(fmt::runtime(lv_tr("Restored to {:.3f} mm")),
                                                 old_mm));
                            }),
            lifetime_.bg_cb("CalibrationRun.ZOffset.restore_failed",
                            [done](const MoonrakerError& err) {
                                done(fmt::format(fmt::runtime(lv_tr("Could not restore: {}")),
                                                 err.localized_message()));
                            }));
    }

  private:
    /// The screen closed without Accept. The steps after this one heat and
    /// move the printer, so going on to them is the owner's choice; anything
    /// but Continue stops the run.
    void ask_continue() {
        helix::ui::ConfirmOptions options;
        options.cancel_text = lv_tr("Stop calibration");
        options.on_cancel = [this, alive = std::weak_ptr<int>(alive_)]() {
            if (!alive.expired() && ctx_) { // L081_OK: main thread, a modal's own handler
                ctx_->stop_run();
            }
        };
        options.on_dismiss = options.on_cancel;
        helix::ui::modal_confirm(
            lv_tr("Z offset not set"),
            lv_tr("Continue with the remaining steps, or stop the calibration?"),
            ModalSeverity::Warning, lv_tr("Continue"),
            [this, alive = std::weak_ptr<int>(alive_)]() {
                if (alive.expired() || !ctx_) { // L081_OK: main thread, a modal's own handler
                    return;
                }
                auto* c = ctx_;
                ctx_ = nullptr;
                c->done(StepOutcome::skipped());
            },
            options);
    }

    /// The value ACCEPT staged, as SAVE_CONFIG will write it. @p z, the Z
    /// position accepted at, stands in when the firmware reports none.
    void read_staged(StepContext& c, float z) {
        StepContext* ctx = &c;
        const auto fallback = [ctx, z]() {
            ctx->done(StepOutcome::finished(
                {fmt::format(fmt::runtime(lv_tr("Accepted at Z {:.3f} mm")), z),
                 {{"accepted_z", z}}}));
        };
        ctx->api.advanced().get_save_config_pending_items(
            lifetime_.bg_cb("CalibrationRun.ZOffset.staged",
                            [ctx, fallback](const nlohmann::json& items) {
                                if (const auto staged = staged_z_offset(items)) {
                                    ctx->done(StepOutcome::finished(
                                        {format_z_offset(*staged), {{"z_offset", *staged}}}));
                                    return;
                                }
                                fallback();
                            }),
            lifetime_.bg_cb("CalibrationRun.ZOffset.staged_error",
                            [fallback](const MoonrakerError&) { fallback(); }));
    }

    StepInfo info_;
    /// The bed warms before probing, as the Z offset screen's Warm Bed does.
    bool warm_bed_ = false;
    StepContext* ctx_ = nullptr;
    bool accepted_ = false;
    ZOffsetCalibrationStrategy strategy_ = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    double old_offset_mm_ = 0.0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    helix::AsyncLifetimeGuard lifetime_;
};

/// Klipper's idle_timeout reads "Printing" while any command runs, a
/// calibration that outlived the app's wait included. The watch reads it once a
/// second rather than observing it: one timer covers the idle report, the
/// ceiling, and a printer switch that frees the subject under the wait.
/// TIMER_DTOR_OK: the destructor cancels through cancel().
class IdleTimeoutBusyWatch : public PrinterBusyWatch {
  public:
    explicit IdleTimeoutBusyWatch(PrinterState& state) : state_(state) {}
    ~IdleTimeoutBusyWatch() override {
        cancel();
    }

    bool busy() const override {
        return lv_subject_get_int(state_.calibration_state().get_idle_timeout_printing_subject()) ==
               1;
    }

    void when_idle(std::function<void()> idle, uint32_t ceiling_ms,
                   std::function<void(bool)> gave_up) override {
        cancel();
        idle_ = std::move(idle);
        gave_up_ = std::move(gave_up);
        ceiling_ms_ = ceiling_ms;
        started_ = lv_tick_get();
        (void)state_.calibration_state().get_idle_timeout_printing_subject(lifetime_);
        arm();
    }

    void cancel() override {
        if (timer_) {
            helix::ui::lv_timer_cancel_safe(timer_);
            timer_ = nullptr;
        }
        idle_ = nullptr;
        gave_up_ = nullptr;
        lifetime_.reset();
    }

  private:
    static constexpr uint32_t kPollMs = 1000;

    /// One read a second, each a one-shot timer that arms the next.
    void arm() {
        timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<IdleTimeoutBusyWatch*>(lv_timer_get_user_data(t));
                // One-shot: LVGL deletes it once this returns.
                self->timer_ = nullptr;
                self->check();
            },
            kPollMs, this);
        lv_timer_set_repeat_count(timer_, 1);
    }

    void check() {
        // Each answer ends the wait before it is handed over, since the
        // handler may start another wait on this watch.
        if (!lifetime_ || !*lifetime_) {
            auto gave_up = std::move(gave_up_);
            cancel();
            if (gave_up) {
                gave_up(false);
            }
            return;
        }
        // A klippy transition zeroes the busy flag at once, before the run
        // hears Klipper went down; idle only counts from a READY Klipper.
        const bool ready =
            static_cast<KlippyState>(lv_subject_get_int(
                state_.network_state().get_klippy_state_subject())) == KlippyState::READY;
        if (ready && !busy()) {
            auto idle = std::move(idle_);
            cancel();
            if (idle) {
                idle();
            }
            return;
        }
        if (lv_tick_elaps(started_) >= ceiling_ms_) {
            auto gave_up = std::move(gave_up_);
            cancel();
            if (gave_up) {
                gave_up(true);
            }
            return;
        }
        arm();
    }

    PrinterState& state_;
    std::function<void()> idle_;
    std::function<void(bool)> gave_up_;
    uint32_t ceiling_ms_ = 0;
    uint32_t started_ = 0;
    std::shared_ptr<bool> lifetime_;
    lv_timer_t* timer_ = nullptr;
};

} // namespace

std::unique_ptr<CalibrationStep> make_run_step(const StepInfo& info, const RunOptions& options) {
    switch (info.kind) {
    case StepKind::ZOffset:
        return std::make_unique<ZOffsetStep>(info, options.zoffset_warm_bed);
    case StepKind::Heaters:
        return std::make_unique<HeatersStep>(info);
    case StepKind::BedMesh:
        return std::make_unique<BedMeshStep>(info);
    case StepKind::InputShaper:
        return std::make_unique<InputShaperStep>(info);
    default:
        return nullptr;
    }
}

// ============================================================================
// Finisher
// ============================================================================

std::unique_ptr<PrinterBusyWatch> make_printer_busy_watch(PrinterState& state) {
    return std::make_unique<IdleTimeoutBusyWatch>(state);
}

void MoonrakerRunFinisher::save_all(helix::SharedCallback<void()> on_saved,
                                    helix::SharedCallback<void(const std::string&)> on_failed) {
    spdlog::info("[CalibrationRun] Saving every result with one SAVE_CONFIG");
    watch_.begin(&api_, TR_NOOP("Saving calibration results..."), std::move(on_saved),
                 std::move(on_failed));
}

void MoonrakerRunFinisher::discard_all(std::function<void()> on_done,
                                       std::function<void(const std::string&)> on_failed) {
    spdlog::info("[CalibrationRun] Discarding: restarting Klipper drops every pending result");
    helix::ui::begin_expected_klippy_restart("Restarting Klipper...", &api_);
    api_.restart_klipper(
        lifetime_.bg_cb("CalibrationRun.discard_done", [on_done]() { on_done(); }),
        lifetime_.bg_cb("CalibrationRun.discard_error", [on_failed](const MoonrakerError& err) {
            spdlog::warn("[CalibrationRun] RESTART failed: {}", err.message);
            on_failed(err.localized_message());
        }));
}

void MoonrakerRunFinisher::read_pending(std::function<void(bool any_pending)> done) {
    api_.advanced().get_save_config_pending_items(
        lifetime_.bg_cb("CalibrationRun.read_pending",
                        [done](const nlohmann::json& items) {
                            // Null is a reply without the field: assume something is pending.
                            done(!items.is_object() || !items.empty());
                        }),
        lifetime_.bg_cb("CalibrationRun.read_pending_error",
                        [done](const MoonrakerError&) { done(true); }));
}

void MoonrakerRunFinisher::record(const StepInfo& step, const StepResult& result) {
    CalibrationHistory::instance().record(api_, step.id, result.summary, result.values);
}

} // namespace helix::calibration
