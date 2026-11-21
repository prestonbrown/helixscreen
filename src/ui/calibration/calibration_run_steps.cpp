// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_run_steps.h"

#include "ui_emergency_stop.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_zoffset.h"

#include "async_lifetime_guard.h"
#include "calibration_abort.h"
#include "calibration_config_results.h"
#include "calibration_history_store.h"
#include "i_moonraker_api.h"
#include "input_shaper_calibrator.h"
#include "moonraker_error.h"
#include "pid_progress_tracker.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "translation_loader.h"
#include "z_offset_utils.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
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
        command_sent_ = false;
        values_ = nlohmann::json::object();
        heaters_.push_back(
            {ctx.api.printer_state().temperature_state().active_extruder_name(), kRunNozzleTempC});
        if (ctx.api.hardware().has_heater_bed()) {
            heaters_.push_back({kBedHeater, kRunBedTempC});
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
        if (command_sent_) {
            aborted_ = true;
            stop_with_firmware_restart(ctx_->api, "CalibrationRun.Heaters");
        }
        auto* ctx = ctx_;
        ctx_ = nullptr;
        ctx->done(StepOutcome::aborted());
    }

    bool abort_restarted_firmware() const override {
        return aborted_;
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

    void report(const std::string& method, int done_fraction_pct) {
        if (!ctx_) {
            return;
        }
        const int share = 100 / static_cast<int>(heaters_.size());
        const int pct = static_cast<int>(index_) * share + done_fraction_pct * share / 100;
        ctx_->progress({fmt::format(fmt::runtime(lv_tr("{}: {} at {}°C")), label(), method,
                                    heaters_[index_].temp_c),
                        std::clamp(pct, 0, 99)});
    }

    void run_current() {
        if (!ctx_) {
            return;
        }
        if (index_ >= heaters_.size()) {
            auto* ctx = ctx_;
            ctx_ = nullptr;
            ctx->done(StepOutcome::finished({joined_summary(), values_}));
            return;
        }
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
            lifetime_.bg_cb(
                "CalibrationRun.Heaters.pid_done",
                [this, h](float kp, float ki, float kd) {
                    values_[h.name] = {
                        {"control", "pid"}, {"temp", h.temp_c}, {"kp", kp}, {"ki", ki}, {"kd", kd}};
                    summaries_.push_back(fmt::format("{} {}", label(), format_pid(kp, ki, kd)));
                    next();
                }),
            lifetime_.bg_cb("CalibrationRun.Heaters.pid_error",
                            [this](const MoonrakerError& err) { fail(err.message); }),
            lifetime_.bg_cb(
                "CalibrationRun.Heaters.pid_progress", [this](int sample, float /*tolerance*/) {
                    report("PID", std::min(95, sample * 100 / PidProgressTracker::kExpectedCycles));
                }));
    }

    void run_mpc() {
        if (!ctx_) {
            return;
        }
        const Heater h = heaters_[index_];
        spdlog::info("[CalibrationRun] MPC_CALIBRATE {} at {}°C", h.name, h.temp_c);
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
        if (summaries_.empty()) {
            ctx->done(StepOutcome::failed(message));
            return;
        }
        // The heaters tuned before this one are staged in Klipper.
        ctx->done(StepOutcome::failed_after(message, {joined_summary(), values_}));
    }

    std::string joined_summary() const {
        std::string summary;
        for (const auto& s : summaries_) {
            summary += summary.empty() ? s : " · " + s;
        }
        return summary;
    }

    StepInfo info_;
    StepContext* ctx_ = nullptr;
    std::vector<Heater> heaters_;
    size_t index_ = 0;
    std::vector<std::string> summaries_;
    nlohmann::json values_ = nlohmann::json::object();
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
        ctx.progress({lv_tr("Probing the bed"), -1});
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
        hooks.failed = [this](const std::string& why) {
            if (auto* c = take()) {
                c->done(StepOutcome::failed(why));
            }
        };
        get_global_bed_mesh_panel().calibrate_for_run(&ctx.api, std::move(hooks));
    }

    /// Only a running BED_MESH_CALIBRATE needs M112; before it is sent the
    /// panel's preparation is cancelled and the heaters it turned on go off.
    void abort() override {
        if (!ctx_) {
            return;
        }
        auto& panel = get_global_bed_mesh_panel();
        aborted_ = panel.calibration_command_sent();
        auto* c = take(aborted_ ? BedMeshPanel::RunEnd::Stopped : BedMeshPanel::RunEnd::Cancelled);
        if (aborted_) {
            stop_with_firmware_restart(c->api, "CalibrationRun.BedMesh");
        }
        c->done(StepOutcome::aborted());
    }

    bool abort_restarted_firmware() const override {
        return aborted_;
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
        ctx.progress({lv_tr("Checking the accelerometer"), -1});
        calibrator_->run_both_axes(
            lifetime_.bg_cb(
                "CalibrationRun.Shaper.progress",
                [this](char axis, int percent, ShaperCalibrationPhase phase) {
                    if (!ctx_) {
                        return;
                    }
                    const bool analyzing = phase != ShaperCalibrationPhase::Sweeping;
                    const std::string text =
                        analyzing ? fmt::format(fmt::runtime(lv_tr("Analyzing the {} axis")), axis)
                                  : fmt::format(fmt::runtime(lv_tr("Shaking the {} axis")), axis);
                    const int base = axis == 'X' ? 0 : 50;
                    ctx_->progress({text, analyzing ? -1 : base + percent / 2});
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
        auto* c = take();
        if (!c) {
            return;
        }
        lifetime_.invalidate();
        const auto state =
            calibrator_ ? calibrator_->get_state() : InputShaperCalibrator::State::IDLE;
        aborted_ = state == InputShaperCalibrator::State::TESTING_X ||
                   state == InputShaperCalibrator::State::TESTING_Y;
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

    bool abort_restarted_firmware() const override {
        return aborted_;
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
    explicit ZOffsetStep(StepInfo info) : info_(std::move(info)) {}
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
        ctx.progress({lv_tr("Waiting for you at the Z offset screen"), -1});
        get_global_zoffset_cal_panel().open_for_run(
            &ctx.api, [this, alive = std::weak_ptr<int>(alive_)](
                          bool accepted, float z, ZOffsetCalibrationStrategy strategy) {
                if (alive.expired() ||
                    !ctx_) { // L081_OK: main thread, from the panel's own handlers
                    return;
                }
                auto* c = ctx_;
                ctx_ = nullptr;
                if (!accepted) {
                    c->done(StepOutcome::skipped());
                    return;
                }
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
    StepContext* ctx_ = nullptr;
    bool accepted_ = false;
    ZOffsetCalibrationStrategy strategy_ = ZOffsetCalibrationStrategy::PROBE_CALIBRATE;
    double old_offset_mm_ = 0.0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace

std::unique_ptr<CalibrationStep> make_run_step(const StepInfo& info) {
    switch (info.kind) {
    case StepKind::ZOffset:
        return std::make_unique<ZOffsetStep>(info);
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
