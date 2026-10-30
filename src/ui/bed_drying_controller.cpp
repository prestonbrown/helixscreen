// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_drying_controller.h"

#include "ui_notification.h"
#include "ui_timer_guard.h"

#include "ams_state.h"
#include "app_globals.h"
#include "dryer_summary.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "motion_presets.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "spdlog/spdlog.h"
#include "temperature_controller.h"

#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <cstring>

namespace helix {

using namespace bed_drying;

namespace {

long long wall_clock_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<bool> sensor_answer(const FilamentSensorManager& fsm, FilamentSensorRole role) {
    if (!fsm.is_master_enabled() || !fsm.is_sensor_available(role)) {
        return std::nullopt;
    }
    return fsm.is_filament_detected(role);
}

} // namespace

BedDryingController::BedDryingController(PrinterState& state, IMoonrakerAPI* api,
                                         TemperatureController* tc, Clock clock)
    : state_(state), api_(api), tc_(tc), clock_(clock ? std::move(clock) : Clock(wall_clock_s)) {}

BedDryingController::~BedDryingController() {
    pre_run_ = PreRun::None;
    drop_unload_wait();
    cancel_timer();
    lifetime_.invalidate();
    if (subjects_initialized_) {
        subjects_initialized_ = false;
        subjects_.deinit_all();
    }
}

void BedDryingController::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(bed_drying_state_, 0, "bed_drying_state", subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_drying_text_, text_buf_, "", "bed_drying_text", subjects_);
    UI_MANAGED_SUBJECT_INT(bed_drying_remaining_min_, -1, "bed_drying_remaining_min", subjects_);
    UI_MANAGED_SUBJECT_INT(bed_drying_progress_pct_, -1, "bed_drying_progress_pct", subjects_);
    UI_MANAGED_SUBJECT_INT(bed_drying_ack_, 0, "bed_drying_ack", subjects_);
    UI_MANAGED_SUBJECT_INT(bed_drying_chamber_assist_, 0, "bed_drying_chamber_assist", subjects_);
    UI_MANAGED_SUBJECT_STRING(bed_drying_chamber_text_, chamber_text_buf_, "",
                              "bed_drying_chamber_text", subjects_);
    subjects_initialized_ = true;
    print_watch_ = ui::observe<int>(
        state_.print_state().get_print_lifecycle_subject(), this,
        [](BedDryingController* self, int /*lifecycle*/) { self->check_print_alarm(); },
        state_.get_subjects_lifetime());
}

void BedDryingController::check_print_alarm() {
    const bool alarm =
        record_.latched && job_holds_machine(state_.print_state().get_print_lifecycle());
    if (alarm && !print_alarm_raised_) {
        spdlog::warn("[BedDrying] A print holds the machine while spools are latched on the bed");
        if (on_print_while_latched_) {
            on_print_while_latched_();
        }
    }
    print_alarm_raised_ = alarm;
}

void BedDryingController::await_unload(helix::SharedCallback<void()> on_done,
                                       helix::SharedCallback<void(bool started)> on_failed) {
    drop_unload_wait();
    unload_done_ = std::move(on_done);
    unload_failed_ = std::move(on_failed);
    unload_seen_busy_ = false;
    set_pre_run(PreRun::Unloading);
    auto& ams = AmsState::instance();
    unload_watch_ = ui::observe<int>(
        ams.get_ams_action_subject(), this,
        [](BedDryingController* self, int value) {
            const auto action = static_cast<AmsAction>(value);
            const bool busy = ams_action_is_busy(action);
            const UnloadProgress progress =
                unload_progress(self->unload_seen_busy_, busy, action == AmsAction::ERROR);
            self->unload_seen_busy_ = self->unload_seen_busy_ || busy;
            if (progress != UnloadProgress::Waiting) {
                self->finish_unload_wait(progress == UnloadProgress::Done);
            }
        },
        ams.get_subjects_lifetime());
    unload_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<BedDryingController*>(lv_timer_get_user_data(t));
            self->unload_timer_ = nullptr; // one-shot: LVGL deletes it after this
            if (!self->unload_seen_busy_) {
                spdlog::warn("[BedDrying] The unload never started; not moving the plate");
                self->finish_unload_wait(false);
            }
        },
        kUnloadStartWindowMs, this);
    lv_timer_set_repeat_count(unload_timer_, 1);
}

void BedDryingController::cancel_unload_wait() {
    drop_unload_wait();
    if (pre_run_ == PreRun::Unloading) {
        set_pre_run(PreRun::None);
    }
}

void BedDryingController::cancel_preparation() {
    if (pre_run_ == PreRun::None) {
        return;
    }
    spdlog::info("[BedDrying] Stopped before the spools went on");
    drop_unload_wait();
    ++prep_gen_;
    set_pre_run(PreRun::None);
}

void BedDryingController::set_pre_run(PreRun p) {
    if (pre_run_ != p) {
        pre_run_ = p;
        publish();
    }
}

void BedDryingController::drop_unload_wait() {
    unload_watch_.reset();
    if (unload_timer_) {
        ui::lv_timer_cancel_safe(unload_timer_);
        unload_timer_ = nullptr;
    }
    unload_done_ = nullptr;
    unload_failed_ = nullptr;
}

void BedDryingController::finish_unload_wait(bool done) {
    auto on_done = std::move(unload_done_);
    auto on_failed = std::move(unload_failed_);
    const bool started = unload_seen_busy_;
    cancel_unload_wait();
    spdlog::info("[BedDrying] Unload {}", done ? "finished" : "did not finish");
    if (done && on_done) {
        on_done();
    } else if (!done && on_failed) {
        on_failed(started);
    }
}

long long BedDryingController::now() const {
    return clock_();
}

void BedDryingController::cancel_timer() {
    if (timer_ && lv_is_initialized()) {
        ui::lv_timer_cancel_safe(timer_);
    }
    timer_ = nullptr;
}

std::optional<bool> BedDryingController::toolhead_loaded() const {
    const auto& fsm = FilamentSensorManager::instance();
    lv_subject_t* ams_loaded = AmsState::instance().get_filament_loaded_subject();
    return toolhead_loaded_from(sensor_answer(fsm, FilamentSensorRole::TOOLHEAD),
                                sensor_answer(fsm, FilamentSensorRole::RUNOUT),
                                ams_loaded && lv_subject_get_int(ams_loaded) != 0);
}

int BedDryingController::bed_temp_for(const Material& material) const {
    const int bed_max = tc_ ? static_cast<int>(tc_->keypad_range(HeaterType::Bed).max) : 0;
    return bed_temp_c(material, bed_max);
}

// A temperature_fan chamber only cools: a target of 0 on it runs the fan flat out.
ChamberAssist BedDryingController::chamber_assist_available() const {
    if (!tc_) {
        return ChamberAssist::None;
    }
    const std::string heater = tc_->resolved_name(HeaterType::Chamber);
    return chamber_assist(tc_->chamber_dryer().supported,
                          !heater.empty() && heater.rfind("temperature_fan ", 0) != 0);
}

int BedDryingController::chamber_temp_for(const Material& material) const {
    return chamber_temp_c(
        material, tc_ ? static_cast<int>(tc_->effective_keypad_max(HeaterType::Chamber, 0.0f)) : 0);
}

void BedDryingController::describe_chamber(const Material& material) {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_set_int(&bed_drying_chamber_assist_, static_cast<int>(chamber_assist_available()));
    const std::string text =
        fmt::format("{} ({}°C)", lv_tr("Heat the chamber too"), chamber_temp_for(material));
    lv_subject_copy_string(&bed_drying_chamber_text_, text.c_str());
}

BedDryingController::State BedDryingController::state() const {
    if (!record_.latched) {
        switch (pre_run_) {
        case PreRun::Unloading:
            return State::Unloading;
        case PreRun::Preparing:
            return State::Preparing;
        case PreRun::None:
            return State::Idle;
        }
    }
    if (record_.placing) {
        return State::Placing;
    }
    if (!record_.ended) {
        return State::Running;
    }
    return removal_prompted_ ? State::ReadyToRemove : State::Cooling;
}

void BedDryingController::set_latch(bool on) {
    state_.print_state().set_spool_latch(on, on && tc_ ? tc_->chamber_dryer_tokens()
                                                       : std::vector<std::string>{});
}

void BedDryingController::restore() {
    record_ = SettingsManager::instance().get_bed_drying_record();
    if (!record_.latched) {
        publish();
        return;
    }
    spdlog::info("[BedDrying] Restoring a run: {} (spools on the bed)",
                 record_.placing ? "placing" : (record_.ended ? "ended" : "in progress"));
    if (record_.placing && record_.material >= 0 &&
        record_.material < static_cast<int>(kMaterials.size())) {
        pending_material_ = kMaterials[static_cast<size_t>(record_.material)];
        pending_appliance_ = record_.appliance;
    }
    set_latch(true);
    bed_target_seen_ = false;
    removal_prompted_ = false;
    if (!timer_) {
        timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<BedDryingController*>(lv_timer_get_user_data(t));
                self->tick(self->now());
            },
            1000, this); // TIMER_DTOR_OK: cancelled in ~BedDryingController
    }
    tick(now());
}

void BedDryingController::prepare(const Material& material, bool with_appliance,
                                  helix::SharedCallback<void()> on_ready,
                                  helix::SharedCallback<void(const std::string&)> on_error) {
    if (!api_ || record_.latched || pre_run_ == PreRun::Preparing) {
        if (on_error) {
            on_error(lv_tr("Drying is already running"));
        }
        return;
    }
    pending_material_ = material;
    pending_appliance_ = with_appliance;
    if (tc_) {
        // The chamber cap comes from the configfile; ask now, so it is known by
        // the time the spools are on and the target goes out.
        tc_->ensure_limits(HeaterType::Chamber);
    }
    set_pre_run(PreRun::Preparing);
    const unsigned gen = prep_gen_;

    // G1 takes G-code coordinates, and the park stays over the plate: travel
    // past it can hold tool docks or a purge bucket.
    const AxisBounds b = state_.motion_state().get_gcode_axis_bounds();
    const auto park = plate_rear_park(
        preset_area(state_.motion_state().get_axis_bounds(), b, api_->hardware().build_volume()));
    std::string move = fmt::format("G90\nG1 Z{:.1f} F600", clearance_z(b.z_max));
    if (park) {
        move += fmt::format("\nG1 X{:.1f} Y{:.1f} F6000", *park->x, *park->y);
    }
    move += "\nM400";

    auto tok = lifetime_.token();
    auto fail = [this, tok, gen, on_error](const MoonrakerError& err) {
        if (tok.expired()) {
            return;
        }
        tok.defer("BedDrying::prepare_failed", [this, gen, on_error, msg = err.message]() {
            if (gen != prep_gen_) {
                return;
            }
            set_pre_run(PreRun::None);
            if (on_error) {
                on_error(msg);
            }
        });
    };
    auto do_move = [this, tok, gen, move, on_ready, on_error, fail]() {
        api_->execute_gcode(
            move,
            [this, tok, gen, on_ready, on_error]() {
                if (tok.expired()) {
                    return;
                }
                tok.defer("BedDrying::placed_ready", [this, gen, on_ready, on_error]() {
                    if (gen != prep_gen_) {
                        return;
                    }
                    set_pre_run(PreRun::None);
                    if (!begin_placement()) {
                        if (on_error) {
                            on_error(lv_tr("Could not save the drying state"));
                        }
                        return;
                    }
                    if (on_ready) {
                        on_ready();
                    }
                });
            },
            fail, 180000);
    };

    const char* homed = lv_subject_get_string(state_.motion_state().get_homed_axes_subject());
    const bool all_homed =
        homed && std::strchr(homed, 'x') && std::strchr(homed, 'y') && std::strchr(homed, 'z');
    spdlog::info("[BedDrying] Preparing: {}clearance move to Z {:.1f}", all_homed ? "" : "home, ",
                 clearance_z(b.z_max));
    if (all_homed) {
        do_move();
        return;
    }
    api_->motion().home_axes(
        "",
        [this, tok, gen, do_move]() {
            if (tok.expired()) {
                return;
            }
            tok.defer("BedDrying::homed", [this, gen, do_move]() {
                if (gen == prep_gen_) {
                    do_move();
                }
            });
        },
        fail);
}

// Spools can land on the plate from the moment the place prompt opens, so the
// latch is set and saved here, before the prompt is shown.
bool BedDryingController::begin_placement() {
    RunRecord placing;
    placing.latched = true;
    placing.placing = true;
    placing.appliance = pending_appliance_;
    for (size_t i = 0; i < kMaterials.size(); ++i) {
        if (kMaterials[i].name == pending_material_.name) {
            placing.material = static_cast<int>(i);
        }
    }
    if (!SettingsManager::instance().set_bed_drying_record(placing)) {
        return false;
    }
    record_ = placing;
    set_latch(true);
    removal_prompted_ = false;
    // The motors are live after the move; the idle timeout's M84 must not fire
    // while spools go onto the plate.
    if (tc_) {
        auto tok = lifetime_.token();
        tc_->read_configured_idle_timeout([this, tok](int configured_s) {
            if (tok.expired() || !record_.latched) {
                return;
            }
            record_.idle_restore_s = configured_s;
            (void)SettingsManager::instance().set_bed_drying_record(record_);
            hold_idle(kSpoolsOnBedHoldS);
        });
    }
    restore();
    return true;
}

bool BedDryingController::confirm_placed() {
    if (!record_.latched || !record_.placing || !api_ || pending_material_.hours <= 0) {
        return false;
    }
    const Material& m = pending_material_;
    const long long start = now();
    RunRecord run = record_;
    run.placing = false;
    run.start_s = start;
    run.end_s = start + static_cast<long long>(m.hours) * 3600;
    run.bed_c = bed_temp_for(m);
    const ChamberAssist assist =
        pending_appliance_ ? chamber_assist_available() : ChamberAssist::None;
    run.appliance = assist == ChamberAssist::Dryer;
    run.chamber_c = assist == ChamberAssist::Heater ? chamber_temp_for(m) : 0;

    // The run reaches disk before any heat is sent.
    if (!SettingsManager::instance().set_bed_drying_record(run)) {
        return false;
    }
    record_ = run;
    set_latch(true);
    bed_target_seen_ = false;
    removal_prompted_ = false;
    spdlog::info("[BedDrying] Spools on the bed: {} at {}C for {}h{}{}", m.name, record_.bed_c,
                 m.hours, record_.appliance ? ", chamber dryer alongside" : "",
                 record_.chamber_c > 0 ? fmt::format(", chamber at {}C", record_.chamber_c) : "");

    if (tc_) {
        tc_->set_target(HeaterType::Bed, record_.bed_c);
        if (record_.appliance) {
            // The appliance heats the air around the spools, far below the bed.
            tc_->start_chamber_drying(static_cast<float>(m.air_c), m.hours * 60, false, false);
        }
        if (record_.chamber_c > 0) {
            tc_->set_target(HeaterType::Chamber, record_.chamber_c, {.toast = false});
        }
    }
    // Held to the planned end plus the dead-man margin: if HelixScreen can no
    // longer end the run, Klipper's own idle timeout does.
    hold_idle(static_cast<int>(record_.end_s - now()) + kDeadManMarginS);
    restore();
    return true;
}

void BedDryingController::cancel_placement() {
    if (!record_.latched || !record_.placing) {
        return;
    }
    spdlog::info("[BedDrying] No spools placed");
    confirm_removed();
}

void BedDryingController::hold_idle(int seconds) {
    if (api_) {
        api_->execute_gcode(fmt::format("SET_IDLE_TIMEOUT TIMEOUT={}", seconds), nullptr, nullptr);
    }
}

bool BedDryingController::klipper_ready() const {
    lv_subject_t* klippy = state_.network_state().get_klippy_state_subject();
    return klippy && lv_subject_get_int(klippy) == static_cast<int>(KlippyState::READY);
}

void BedDryingController::stop() {
    if (record_.latched && !record_.ended) {
        end_run("stopped");
    }
}

// The bed and a chamber heater go off only while still ours: a target someone
// set by hand since owns it now. A target reading 0 is ours too; an off sent to
// a cold heater costs nothing.
void BedDryingController::end_run(const char* why) {
    spdlog::info("[BedDrying] Run ended ({})", why);
    record_.ended = true;
    (void)SettingsManager::instance().set_bed_drying_record(record_);
    if (tc_) {
        lv_subject_t* target = state_.temperature_state().get_bed_target_subject();
        const int target_deci = target ? lv_subject_get_int(target) : 0;
        if (target_deci == 0 || target_deci == record_.bed_c * 10) {
            tc_->set_target(HeaterType::Bed, 0);
        }
        if (record_.chamber_c > 0) {
            lv_subject_t* chamber = state_.temperature_state().get_chamber_target_subject();
            const int chamber_deci = chamber ? lv_subject_get_int(chamber) : 0;
            if (chamber_deci == 0 || chamber_deci == record_.chamber_c * 10) {
                tc_->set_target(HeaterType::Chamber, 0, {.toast = false});
            }
        }
        if (record_.appliance) {
            tc_->stop_chamber_drying();
        }
    }
    // The heat is off but the spools are still on the plate: keep the motors
    // held until they are confirmed off, rather than restoring the timeout.
    hold_idle(kSpoolsOnBedHoldS);
    publish();
}

void BedDryingController::confirm_removed() {
    if (!record_.latched) {
        return;
    }
    if (!record_.ended && !record_.placing) {
        end_run("spools removed");
    }
    // A hold went out either way; an unread configured value falls back to
    // Klipper's own default rather than leaving the long hold in place.
    hold_idle(record_.idle_restore_s > 0 ? record_.idle_restore_s : kKlipperDefaultIdleS);
    spdlog::info("[BedDrying] Spools off the bed; latch cleared");
    record_ = RunRecord{};
    (void)SettingsManager::instance().clear_bed_drying_record();
    set_latch(false);
    removal_prompted_ = false;
    cancel_timer();
    publish();
}

void BedDryingController::tick(long long now_s) {
    if (!record_.latched) {
        publish();
        return;
    }
    // A run restored before discovery registered no dryer tokens; picking them
    // up here lets the dryer's stop through once the appliance is known.
    set_latch(true);
    // Sends made before Klipper is ready go nowhere and are never retried, so
    // the run is only advanced once it is (a restore runs before connecting).
    if (record_.placing || !klipper_ready()) {
        publish();
        return;
    }
    if (!record_.ended) {
        lv_subject_t* target = state_.temperature_state().get_bed_target_subject();
        const int target_deci = target ? lv_subject_get_int(target) : 0;
        if (target_deci == record_.bed_c * 10) {
            bed_target_seen_ = true;
        }
        if (phase_at(now_s, record_.end_s) == Phase::Ended) {
            end_run("cycle complete");
        } else if (bed_target_seen_ && target_deci == 0) {
            // Klipper dropped the bed target: a restart, an emergency stop, or
            // its idle timeout. The run is over either way.
            end_run("bed turned off");
        } else if (!record_.flip_notified && flip_due(now_s, record_.start_s, record_.end_s)) {
            record_.flip_notified = true;
            (void)SettingsManager::instance().set_bed_drying_record(record_);
            ui_notification_info(lv_tr("Flip the spools"),
                                 lv_tr("Halfway through drying: flip the spools over. Use "
                                       "gloves, the plate is hot."));
        }
    }
    if (record_.ended && !removal_prompted_) {
        lv_subject_t* temp = state_.temperature_state().get_bed_temp_subject();
        const double bed_c = temp ? lv_subject_get_int(temp) / 10.0 : 0.0;
        if (may_prompt_removal(bed_c)) {
            removal_prompted_ = true;
            publish();
            if (on_ready_to_remove_) {
                on_ready_to_remove_();
            }
            return;
        }
    }
    publish();
}

void BedDryingController::publish() {
    if (!subjects_initialized_) {
        return;
    }
    check_print_alarm();
    const State s = state();
    std::string text;
    int remaining_min = -1;
    int progress_pct = -1;
    switch (s) {
    case State::Running: {
        const long long left = std::max(0LL, record_.end_s - now());
        remaining_min = static_cast<int>((left + 59) / 60);
        progress_pct = progress_pct_from(
            remaining_min, static_cast<int>((record_.end_s - record_.start_s + 59) / 60));
        text = fmt::format("{} {}°C  {}:{:02d} {}", lv_tr("Drying on the bed"), record_.bed_c,
                           left / 3600, (left % 3600) / 60, lv_tr("left"));
        break;
    }
    case State::Cooling: {
        lv_subject_t* temp = state_.temperature_state().get_bed_temp_subject();
        const int bed_c = temp ? lv_subject_get_int(temp) / 10 : 0;
        text = fmt::format("{} {}°C", lv_tr("Bed cooling, spools still on the bed:"), bed_c);
        break;
    }
    case State::ReadyToRemove:
        text = lv_tr("Remove the spools from the bed");
        break;
    case State::Placing:
        text = lv_tr("Spools on the bed: start drying, or confirm none were placed");
        break;
    case State::Unloading:
        text = lv_tr("Unloading filament before drying...");
        break;
    case State::Preparing:
        text = lv_tr("Homing and moving the plate...");
        break;
    case State::Idle:
        break;
    }
    lv_subject_set_int(&bed_drying_remaining_min_, remaining_min);
    lv_subject_set_int(&bed_drying_progress_pct_, progress_pct);
    lv_subject_set_int(&bed_drying_state_, static_cast<int>(s));
    if (text != lv_subject_get_string(&bed_drying_text_)) {
        lv_subject_copy_string(&bed_drying_text_, text.c_str());
    }
}

BedDryingController* get_bed_drying_controller() {
    return PanelWidgetManager::instance().shared_resource<BedDryingController>();
}

} // namespace helix
