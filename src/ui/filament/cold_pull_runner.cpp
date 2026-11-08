// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cold_pull_runner.h"

#include "ui_manual_pull_prompt.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/observer_factory.h"
#include "fan_gcode.h"
#include "filament_op_execute.h"
#include "filament_op_heater.h"
#include "filament_sensor_manager.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "material_settings_manager.h"
#include "moonraker_error.h"
#include "panel_widget_manager.h"
#include "post_op_cooldown_manager.h"
#include "printer_state.h"
#include "standard_macros.h"
#include "temperature_controller.h"
#include "tool_state.h"
#include "toolhead_homing.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "hv/json.hpp"

namespace helix::ui {

namespace {

constexpr uint32_t kTickMs = 1000;
constexpr const char* kLogTag = "[ColdPull]";
/// Connection state value for an established printer link (PrinterNetworkState).
constexpr int kPrinterConnected = 2;

bool is_terminal(int action) {
    return action == static_cast<int>(AmsAction::IDLE) ||
           action == static_cast<int>(AmsAction::ERROR);
}

/// The part fan this head's tip cools under: its tool's own, else the printer's.
/// Empty when the screen may not command it.
std::string commandable_part_fan(PrinterState& state) {
    std::string fan;
    if (const ToolInfo* tool = ToolState::instance().active_tool();
        tool && tool->fan_name && !tool->fan_name->empty()) {
        fan = *tool->fan_name;
    } else {
        fan = state.fan_state().classify_primary_fans().part;
    }
    if (fan.empty() || fan_gcode(fan, 0).empty()) {
        return {};
    }
    return fan;
}

/// The seated lane, when a filament system has one in the head.
bool lane_seated() {
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend || backend->is_bypass_active()) {
        return false;
    }
    const AmsSystemInfo info = backend->get_system_info();
    return info.current_slot >= 0 && backend->is_filament_loaded();
}

} // namespace

ColdPullRefusal cold_pull_refusal(PrintState lifecycle, bool preheat_pending, bool klippy_ready,
                                  bool head_has_heater, bool running) {
    if (job_holds_machine(lifecycle)) {
        return ColdPullRefusal::JobHoldsMachine;
    }
    if (preheat_pending) {
        return ColdPullRefusal::PreheatPending;
    }
    if (!klippy_ready) {
        return ColdPullRefusal::NotReady;
    }
    if (!head_has_heater) {
        return ColdPullRefusal::NoHeater;
    }
    if (running) {
        return ColdPullRefusal::Running;
    }
    return ColdPullRefusal::Ok;
}

ColdPullRunner* get_cold_pull_runner() {
    return PanelWidgetManager::instance().shared_resource<ColdPullRunner>();
}

ColdPullRunner::ColdPullRunner(PrinterState& state, Clock clock)
    : state_(state), clock_(std::move(clock)) {
    material_ = MaterialSettingsManager::instance().get_cold_pull_material();
    setup_temps_ = MaterialSettingsManager::instance().get_cold_pull_temps(material_);
}

ColdPullRunner::~ColdPullRunner() {
    if (running()) {
        spdlog::info("{} Shutting down mid-pull: heater off, fan restored", kLogTag);
        apply(flow_->exit_actions(job_holds_machine(state_.print_state().get_print_lifecycle())));
    }
    watch(false);
    action_observer_.reset();
    lifetime_.invalidate();
    awake_.reset();
    if (subjects_initialized_) {
        subjects_initialized_ = false;
        subjects_.deinit_all();
    }
}

void ColdPullRunner::before_disconnect() {
    // The printer is reachable but its answer would land after the disconnect.
    if (owed_ && owed_query_in_flight_) {
        finish_owed_from_subject();
    }
    owed_.reset();
    owed_query_in_flight_ = false;
    if (running()) {
        spdlog::info("{} Disconnecting mid-pull: heater off, fan restored", kLogTag);
        apply(flow_->exit_actions(job_holds_machine(state_.print_state().get_print_lifecycle())));
        (void)flow_->disconnected();
        after_event();
    }
    watch(false);
    action_observer_.reset();
    unload_watching_ = false;
}

ColdPullRunner::TimePoint ColdPullRunner::now() const {
    return clock_ ? clock_() : std::chrono::steady_clock::now();
}

void ColdPullRunner::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(stage_, 0, "cold_pull_stage", subjects_);
    UI_MANAGED_SUBJECT_INT(feed_stage_, 0, "cold_pull_feed_stage", subjects_);
    UI_MANAGED_SUBJECT_INT(pass_, 1, "cold_pull_pass", subjects_);
    UI_MANAGED_SUBJECT_INT(end_reason_, 0, "cold_pull_end_reason", subjects_);
    UI_MANAGED_SUBJECT_INT(has_fan_, 1, "cold_pull_has_fan", subjects_);
    UI_MANAGED_SUBJECT_INT(can_home_park_, 0, "cold_pull_can_home_park", subjects_);
    UI_MANAGED_SUBJECT_INT(busy_, 0, "cold_pull_busy", subjects_);
    UI_MANAGED_SUBJECT_INT(unload_failed_, 0, "cold_pull_unload_failed", subjects_);
    UI_MANAGED_SUBJECT_INT(material_subject_, static_cast<int>(material_), "cold_pull_material",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(hold_pressure_, 0, "cold_pull_hold_pressure", subjects_);
    UI_MANAGED_SUBJECT_INT(refusal_, 0, "cold_pull_refusal", subjects_);
    UI_MANAGED_SUBJECT_INT(progress_, 0, "cold_pull_progress", subjects_);
    UI_MANAGED_SUBJECT_STRING(soak_text_, soak_text_buf_, "", "cold_pull_soak_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(pull_text_, pull_text_buf_, "", "cold_pull_pull_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(temp_text_, temp_text_buf_, "", "cold_pull_temp_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(pass_text_, pass_text_buf_, "", "cold_pull_pass_text", subjects_);
    subjects_initialized_ = true;
    publish_setup();
}

bool ColdPullRunner::printer_ready() const {
    auto& net = state_.network_state();
    return lv_subject_get_int(net.get_printer_connection_state_subject()) == kPrinterConnected &&
           lv_subject_get_int(net.get_klippy_state_subject()) ==
               static_cast<int>(KlippyState::READY);
}

ColdPullRefusal ColdPullRunner::can_start() const {
    const FilamentOpHeater* heater = get_filament_op_heater();
    auto& temps = state_.temperature_state();
    const std::string& extruder = temps.active_extruder_name();
    const bool has_heater = !extruder.empty() && temps.get_extruder_temp_subject(extruder);
    return cold_pull_refusal(state_.print_state().get_print_lifecycle(),
                             heater && heater->pending(), printer_ready(), has_heater, running());
}

void ColdPullRunner::prepare_setup() {
    if (running()) {
        return;
    }
    flow_.reset();
    awake_.reset();
    if (subjects_initialized_) {
        lv_subject_set_int(&stage_, static_cast<int>(ColdPullStage::Setup));
        lv_subject_set_int(&end_reason_, 0);
        lv_subject_set_int(&refusal_, static_cast<int>(can_start()));
    }
    publish_setup();
}

bool ColdPullRunner::running() const {
    return flow_ && flow_->stage() != ColdPullStage::Setup && !flow_->ended();
}

ColdPullRefusal ColdPullRunner::start(ColdPullMaterial material, ColdPullTemps temps,
                                      bool hold_pressure) {
    const ColdPullRefusal refusal = can_start();
    if (subjects_initialized_) {
        lv_subject_set_int(&refusal_, static_cast<int>(refusal));
    }
    if (refusal != ColdPullRefusal::Ok) {
        spdlog::info("{} Start refused ({})", kLogTag, static_cast<int>(refusal));
        return refusal;
    }

    auto& temps_state = state_.temperature_state();
    ColdPullParams p;
    p.heater = temps_state.active_extruder_name();
    SafetyLimits limits;
    if (auto* api = get_moonraker_api()) {
        limits = api->get_safety_limits();
    }
    p.floor_c = temperature::extrusion_floor_c(limits, p.heater);
    p.temps = clamp_for(temps, p.heater);
    p.part_fan = commandable_part_fan(state_);
    if (!p.part_fan.empty()) {
        lv_subject_t* speed = state_.fan_state().get_fan_speed_subject(p.part_fan);
        p.prior_fan_percent = speed ? lv_subject_get_int(speed) : 0;
    }
    p.homed = toolhead_is_homed(state_);
    p.has_park_macro = !StandardMacros::instance().get(StandardMacroSlot::ParkToolhead).is_empty();
    p.toolhead_loaded = lane_seated();
    p.hold_pressure = hold_pressure;

    spdlog::info("{} Start: {} soak {}°C pull {}°C, fan '{}' (was {}%), floor {}°C, homed {}, "
                 "park macro {}, lane seated {}, hold pressure {}",
                 kLogTag, p.heater, p.temps.soak_c, p.temps.pull_c, p.part_fan, p.prior_fan_percent,
                 p.floor_c, p.homed, p.has_park_macro, p.toolhead_loaded, p.hold_pressure);
    (void)material;
    // A cleanup still owed to this printer goes out before the new pull heats.
    if (owed_) {
        finish_owed_from_subject();
    }
    action_observer_.reset();
    unload_watching_ = false;
    ++flow_gen_;
    flow_.emplace(std::move(p));
    watch(true);
    apply(flow_->start(now()));
    after_event();
    return ColdPullRefusal::Ok;
}

void ColdPullRunner::watch(bool on) {
    temp_observer_.reset();
    target_observer_.reset();
    lifecycle_observer_.reset();
    connection_observer_.reset();
    klippy_observer_.reset();
    sensor_observer_.reset();
    cancel_tick_timer();
    if (!on) {
        return;
    }
    auto& temps = state_.temperature_state();
    const SubjectLifetime life = state_.get_subjects_lifetime();
    // The heater chosen at start, not the active one: a tool change elsewhere must
    // not leave this head at the soak while the pull follows another.
    const std::string& heater = flow_->params().heater;
    SubjectLifetime temp_life;
    SubjectLifetime target_life;
    if (lv_subject_t* temp = temps.get_extruder_temp_subject(heater, temp_life)) {
        temp_observer_ = observe<int>(
            temp, this, [](ColdPullRunner* self, int) { self->on_temperature(); }, temp_life);
    }
    if (lv_subject_t* target = temps.get_extruder_target_subject(heater, target_life)) {
        target_observer_ = observe<int>(
            target, this, [](ColdPullRunner* self, int) { self->on_temperature(); }, target_life);
    }
    lifecycle_observer_ = observe<int>(
        state_.print_state().get_print_lifecycle_subject(), this,
        [](ColdPullRunner* self, int) { self->on_lifecycle(); }, life);
    connection_observer_ = observe<int>(
        state_.network_state().get_printer_connection_state_subject(), this,
        [](ColdPullRunner* self, int) { self->on_connection(); }, life);
    klippy_observer_ = observe<int>(
        state_.network_state().get_klippy_state_subject(), this,
        [](ColdPullRunner* self, int) { self->on_connection(); }, life);
    auto& sensors = FilamentSensorManager::instance();
    if (lv_subject_t* toolhead = sensors.get_toolhead_detected_subject()) {
        last_sensor_ = lv_subject_get_int(toolhead);
        sensor_observer_ = observe<int>(
            toolhead, this, [](ColdPullRunner* self, int v) { self->on_sensor(v); },
            sensors.subjects_lifetime());
    }
    // TIMER_DTOR_OK: cancelled in the destructor through watch(false).
    tick_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<ColdPullRunner*>(lv_timer_get_user_data(t));
            if (self->flow_) {
                self->apply(self->flow_->tick(self->now()));
                self->after_event();
            }
        },
        kTickMs, this);
}

void ColdPullRunner::cancel_tick_timer() {
    lv_timer_cancel_safe(tick_timer_);
    tick_timer_ = nullptr;
}

void ColdPullRunner::after_event() {
    if (!flow_) {
        return;
    }
    if (flow_->hold_awake()) {
        awake_.acquire();
    } else {
        awake_.reset();
    }
    if (flow_->ended()) {
        spdlog::info("{} Ended: stage {}, reason {}", kLogTag, static_cast<int>(flow_->stage()),
                     static_cast<int>(flow_->end_reason()));
        watch(false);
        // A disconnect keeps watching the link for the cleanup it owes.
        if (owed_) {
            auto& net = state_.network_state();
            const SubjectLifetime life = state_.get_subjects_lifetime();
            connection_observer_ = observe<int>(
                net.get_printer_connection_state_subject(), this,
                [](ColdPullRunner* self, int) { self->settle_owed(); }, life);
            klippy_observer_ = observe<int>(
                net.get_klippy_state_subject(), this,
                [](ColdPullRunner* self, int) { self->settle_owed(); }, life);
        }
    }
    publish();
}

void ColdPullRunner::apply(const ColdPullActions& a) {
    if (a.empty() || !flow_) {
        return;
    }
    const ColdPullParams& p = flow_->params();
    IMoonrakerAPI* api = get_moonraker_api();
    send_heater_and_fan(p.heater, p.part_fan, a);
    if (!a.gcode.empty()) {
        send_gcode(a.gcode, /*report=*/true);
    }
    if (a.run_park && api) {
        spdlog::info("{} Parking", kLogTag);
        StandardMacros::instance().execute(
            StandardMacroSlot::ParkToolhead, api, [] {},
            [](const MoonrakerError& e) {
                spdlog::warn("{} Park failed: {}", kLogTag, e.message);
            });
    }
    if (a.home_then_park && api) {
        spdlog::info("{} Homing then parking", kLogTag);
        ensure_homed_then(
            api, lifetime_,
            [] {
                if (auto* api2 = get_moonraker_api()) {
                    StandardMacros::instance().execute(
                        StandardMacroSlot::ParkToolhead, api2, [] {},
                        [](const MoonrakerError& e) {
                            spdlog::warn("{} Park failed: {}", kLogTag, e.message);
                        });
                }
            },
            [](const MoonrakerError& e) {
                spdlog::warn("{} Homing failed: {}", kLogTag, e.message);
            });
    }
    if (a.unload_lane) {
        dispatch_unload();
    }
    if (a.reset_meter) {
        // No cold pull maintenance meter exists yet (#1901); nothing to reset.
        spdlog::debug("{} Done: no cold pull meter to reset", kLogTag);
    }
}

void ColdPullRunner::send_heater_and_fan(const std::string& heater, const std::string& part_fan,
                                         const ColdPullActions& a) {
    if (a.heater_target_c) {
        const int c = *a.heater_target_c;
        spdlog::info("{} Heater {} -> {}°C", kLogTag, heater, c);
        if (c > 0) {
            // A post-op cooldown left by an earlier unload would turn this off mid-pull.
            PostOpCooldownManager::instance().cancel(heater);
            PostOpCooldownManager::instance().cancel(std::string());
        }
        if (auto* tc = get_temperature_controller()) {
            tc->set_target(heater, static_cast<double>(c),
                           {.toast = false, .keep_previous_hot = a.keep_previous_hot});
        }
    }
    if (a.fan_percent && !part_fan.empty()) {
        const std::string g = fan_gcode(part_fan, *a.fan_percent);
        spdlog::info("{} Part fan {} -> {}%", kLogTag, part_fan, *a.fan_percent);
        send_gcode(g, /*report=*/false);
    }
}

void ColdPullRunner::send_gcode(const std::string& gcode, bool report) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (gcode.empty()) {
        return;
    }
    if (!api) {
        if (report && flow_) {
            apply(flow_->command_finished(false, now()));
            after_event();
        }
        return;
    }
    auto finished = [this, report, gen = flow_gen_](bool ok) {
        if (report && flow_ && gen == flow_gen_) {
            apply(flow_->command_finished(ok, now()));
            after_event();
        }
    };
    // Queued behind a blocking op, the command will run but neither reply comes;
    // the step moves on rather than waiting for one.
    api->execute_gcode(
        gcode, lifetime_.bg_cb("ColdPullRunner::gcode_ok", [finished]() { finished(true); }),
        lifetime_.bg_cb("ColdPullRunner::gcode_err",
                        [finished](const MoonrakerError& e) {
                            spdlog::warn("{} Command failed: {}", kLogTag, e.message);
                            finished(false);
                        }),
        /*timeout_ms=*/0, /*silent=*/false,
        /*on_queued=*/
        lifetime_.bg_cb("ColdPullRunner::gcode_queued",
                        [finished]() {
                            spdlog::info("{} Command queued behind a running operation", kLogTag);
                            finished(true);
                        }),
        /*caller_surfaces_errors=*/false); // the callback only logs: Klipper's message shows
}

void ColdPullRunner::dispatch_unload() {
    AmsBackend* backend = AmsState::instance().get_backend();
    AmsSystemInfo info;
    if (backend) {
        info = backend->get_system_info();
    }
    const int slot = info.current_slot;
    const bool loaded = read_unload_target_loaded(backend, info, slot);
    spdlog::info("{} Unloading lane {} before the pull", kLogTag, slot);

    auto finish = [this, gen = flow_gen_](bool ok) {
        if (gen != flow_gen_) {
            return;
        }
        action_observer_.reset();
        unload_watching_ = false;
        if (flow_) {
            apply(flow_->lane_unloaded(ok, now()));
            after_event();
        }
    };
    // Where this head's unload is manual the executor only says so and never calls
    // back; the step shows it as a failed unload, with the reason in the toast.
    if (notify_if_filament_op_unavailable(backend, slot, /*load=*/false)) {
        finish(false);
        return;
    }
    FilamentOpSurface surface;
    surface.log_tag = kLogTag;
    // A filament system's unload completes on its action feed, not a reply.
    surface.on_begin = [this](const FilamentOpPlan& plan) {
        if (plan.tier != FilamentTier::AmsBackend) {
            return;
        }
        lv_subject_t* action = AmsState::instance().get_ams_action_subject();
        unload_last_action_ = action ? lv_subject_get_int(action) : -1;
        unload_seen_busy_ = false;
        unload_watching_ = true;
        action_observer_ = observe<int>(
            action, this, [](ColdPullRunner* self, int v) { self->on_ams_action(v); },
            AmsState::instance().get_subjects_lifetime());
    };
    surface.on_failed = [finish](const FilamentOpPlan&, const AmsError&, bool&) { finish(false); };
    surface.on_async_failed = [finish](const FilamentOpPlan&) { finish(false); };
    // Refused because nothing is loaded leaves the head clear, which is the goal.
    surface.on_refused = [finish](const FilamentOpPlan&) { finish(!lane_seated()); };
    surface.on_async_success = [finish]() { finish(true); };
    surface.on_preheat_ended = [finish]() { finish(false); };
    auto token = lifetime_.token();
    surface.guard = [token](helix::SharedCallback<void()> fn) mutable {
        token.defer("ColdPullRunner::unload", std::move(fn));
    };
    execute_filament_unload(backend, slot, loaded, surface);
}

void ColdPullRunner::on_ams_action(int action) {
    if (!unload_watching_ || action == unload_last_action_) {
        return;
    }
    unload_last_action_ = action;
    if (!is_terminal(action)) {
        unload_seen_busy_ = true;
        return;
    }
    if (!unload_seen_busy_) {
        return;
    }
    action_observer_.reset();
    unload_watching_ = false;
    if (flow_) {
        apply(flow_->lane_unloaded(action == static_cast<int>(AmsAction::IDLE), now()));
        after_event();
    }
}

void ColdPullRunner::on_temperature() {
    if (!flow_) {
        return;
    }
    apply(flow_->temperature(heater_temp_c(), heater_target_c(), now()));
    after_event();
}

void ColdPullRunner::on_lifecycle() {
    if (flow_ && job_holds_machine(state_.print_state().get_print_lifecycle())) {
        apply(flow_->job_started());
        after_event();
    }
}

void ColdPullRunner::on_connection() {
    if (!flow_ || printer_ready()) {
        return;
    }
    if (running()) {
        const std::optional<int> target = flow_->owned_target_c();
        ColdPullActions cleanup = flow_->exit_actions(false);
        if (!cleanup.empty()) {
            owed_ =
                Owed{flow_->params().heater, flow_->params().part_fan, target.value_or(0), cleanup};
        }
    }
    apply(flow_->disconnected());
    after_event();
}

void ColdPullRunner::settle_owed() {
    if (!owed_ || owed_query_in_flight_ || !printer_ready()) {
        return;
    }
    IMoonrakerClient* client = get_moonraker_client();
    if (!client) {
        return;
    }
    // The heater's subject still holds what it read before the link went down, so the
    // printer is asked for the target it has now.
    owed_query_in_flight_ = true;
    const uint32_t gen = ++owed_gen_;
    const std::string heater = owed_->heater;
    const json params = {{"objects", {{heater, json::array({"target"})}}}};
    client->send_jsonrpc(
        "printer.objects.query", params,
        lifetime_.bg_cb("ColdPullRunner::owed_target",
                        [this, heater, gen](const json& response) {
                            if (gen != owed_gen_ || !owed_query_in_flight_) {
                                return;
                            }
                            owed_query_in_flight_ = false;
                            std::optional<int> target;
                            const json* v = json_util::find_member(response, "result");
                            for (const char* key : {"status", heater.c_str(), "target"}) {
                                v = v ? json_util::find_member(*v, key) : nullptr;
                            }
                            if (v && v->is_number()) {
                                target = static_cast<int>(std::lround(json_util::as_double(*v)));
                            }
                            finish_owed(target);
                        }),
        lifetime_.bg_cb("ColdPullRunner::owed_target_err",
                        [this, gen](const MoonrakerError& e) {
                            if (gen != owed_gen_ || !owed_query_in_flight_) {
                                return;
                            }
                            owed_query_in_flight_ = false;
                            spdlog::warn("{} Could not read the owed heater's target: {}", kLogTag,
                                         e.message);
                            // Still connected, no other ready edge is coming: go by
                            // what the subject shows. Dropped again, the next one asks.
                            if (printer_ready()) {
                                finish_owed_from_subject();
                            }
                        }),
        /*timeout_ms=*/0, /*silent=*/true);
}

void ColdPullRunner::finish_owed(std::optional<int> target) {
    if (!owed_) {
        return;
    }
    const Owed owed = std::move(*owed_);
    owed_.reset();
    connection_observer_.reset();
    klippy_observer_.reset();
    if (job_holds_machine(state_.print_state().get_print_lifecycle())) {
        return;
    }
    // A Klipper restart already zeroed it; a changed target is someone else's.
    if (owed.target_c == 0 || target != owed.target_c) {
        if (owed.actions.fan_percent) {
            ColdPullActions fan_only;
            fan_only.fan_percent = owed.actions.fan_percent;
            send_heater_and_fan(owed.heater, owed.part_fan, fan_only);
        }
        return;
    }
    spdlog::info("{} Reconnected with our {}°C target still on {}: cleaning up", kLogTag,
                 owed.target_c, owed.heater);
    send_heater_and_fan(owed.heater, owed.part_fan, owed.actions);
}

void ColdPullRunner::finish_owed_from_subject() {
    if (!owed_) {
        return;
    }
    owed_query_in_flight_ = false;
    lv_subject_t* s = state_.temperature_state().get_extruder_target_subject(owed_->heater);
    finish_owed(s ? std::optional<int>(temperature::deci_to_degrees(lv_subject_get_int(s)))
                  : std::nullopt);
}

void ColdPullRunner::on_sensor(int value) {
    const int prev = std::exchange(last_sensor_, value);
    if (flow_ && prev == 0 && value == 1) {
        apply(flow_->filament_detected(now()));
        after_event();
    }
}

#define COLD_PULL_FORWARD(method)                                                                  \
    void ColdPullRunner::method() {                                                                \
        if (!flow_) {                                                                              \
            return;                                                                                \
        }                                                                                          \
        apply(flow_->method(now()));                                                               \
        after_event();                                                                             \
    }

COLD_PULL_FORWARD(home_and_park)
COLD_PULL_FORWARD(unload_lane)
COLD_PULL_FORWARD(toolhead_clear)
COLD_PULL_FORWARD(extrude)
COLD_PULL_FORWARD(fed)
COLD_PULL_FORWARD(pulled)
COLD_PULL_FORWARD(repeat)
COLD_PULL_FORWARD(done)

#undef COLD_PULL_FORWARD

void ColdPullRunner::cancel() {
    if (!flow_) {
        return;
    }
    if (job_holds_machine(state_.print_state().get_print_lifecycle())) {
        apply(flow_->job_started());
    } else {
        apply(flow_->cancel());
    }
    after_event();
}

// ============================================================================
// Setup
// ============================================================================

void ColdPullRunner::select_material(ColdPullMaterial material) {
    material_ = material;
    setup_temps_ = MaterialSettingsManager::instance().get_cold_pull_temps(material);
    MaterialSettingsManager::instance().set_cold_pull_material(material);
    publish_setup();
}

ColdPullTemps ColdPullRunner::clamp_for(ColdPullTemps temps, const std::string& heater) const {
    SafetyLimits limits;
    if (auto* api = get_moonraker_api()) {
        limits = api->get_safety_limits();
    }
    return clamp_cold_pull_temps(temps, temperature::extrusion_floor_c(limits, heater),
                                 temperature::nozzle_max_temp_c(limits, heater));
}

int ColdPullRunner::heater_temp_c() const {
    lv_subject_t* s =
        flow_ ? state_.temperature_state().get_extruder_temp_subject(flow_->params().heater)
              : nullptr;
    return s ? temperature::deci_to_degrees(lv_subject_get_int(s)) : 0;
}

int ColdPullRunner::heater_target_c() const {
    lv_subject_t* s =
        flow_ ? state_.temperature_state().get_extruder_target_subject(flow_->params().heater)
              : nullptr;
    return s ? temperature::deci_to_degrees(lv_subject_get_int(s)) : 0;
}

void ColdPullRunner::adjust_soak(int delta_c) {
    ColdPullTemps t = setup_temps_;
    t.soak_c += delta_c;
    setup_temps_ = t;
    publish_setup();
}

void ColdPullRunner::adjust_pull(int delta_c) {
    ColdPullTemps t = setup_temps_;
    t.pull_c += delta_c;
    setup_temps_ = t;
    publish_setup();
}

ColdPullRefusal ColdPullRunner::start_from_setup() {
    setup_temps_ = clamp_for(setup_temps_, state_.temperature_state().active_extruder_name());
    const bool hold = subjects_initialized_ && lv_subject_get_int(&hold_pressure_) != 0;
    if (setup_temps_ == cold_pull_default_temps(material_)) {
        MaterialSettingsManager::instance().reset_cold_pull_temps(material_);
    } else {
        MaterialSettingsManager::instance().set_cold_pull_temps(material_, setup_temps_);
    }
    return start(material_, setup_temps_, hold);
}

// ============================================================================
// Publishing
// ============================================================================

void ColdPullRunner::publish_setup() {
    // Setup shows what start() will send: a stored edit outside this head's range
    // shows clamped.
    setup_temps_ = clamp_for(setup_temps_, state_.temperature_state().active_extruder_name());
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_set_int(&material_subject_, static_cast<int>(material_));
    lv_subject_copy_string(&soak_text_, fmt::format("{}°C", setup_temps_.soak_c).c_str());
    lv_subject_copy_string(&pull_text_, fmt::format("{}°C", setup_temps_.pull_c).c_str());
    const bool fan = !commandable_part_fan(state_).empty();
    lv_subject_set_int(&has_fan_, fan ? 1 : 0);
}

void ColdPullRunner::publish() {
    if (!subjects_initialized_ || !flow_) {
        return;
    }
    const ColdPullFlow& f = *flow_;
    const ColdPullStage stage = f.stage();
    const int current = heater_temp_c();
    if (lv_subject_get_int(&stage_) != static_cast<int>(stage)) {
        ring_from_c_ = current;
    }
    lv_subject_set_int(&stage_, static_cast<int>(stage));
    lv_subject_set_int(&feed_stage_, static_cast<int>(f.feed_stage()));
    lv_subject_set_int(&pass_, f.pass());
    lv_subject_set_int(&end_reason_, static_cast<int>(f.end_reason()));
    lv_subject_set_int(&busy_, f.busy() ? 1 : 0);
    lv_subject_set_int(&unload_failed_, f.unload_failed() ? 1 : 0);
    lv_subject_set_int(&has_fan_, f.params().part_fan.empty() ? 0 : 1);
    lv_subject_set_int(&can_home_park_, f.params().has_park_macro && !f.params().homed ? 1 : 0);
    lv_subject_copy_string(&soak_text_, fmt::format("{}°C", f.params().temps.soak_c).c_str());
    lv_subject_copy_string(&pull_text_, fmt::format("{}°C", f.params().temps.pull_c).c_str());

    const int goal = f.goal_c();
    int pct = 0;
    if (goal > 0) {
        // Rising toward the soak, or falling toward the pull, from where the stage began.
        const int span = std::abs(goal - ring_from_c_);
        const int done_c = span == 0 ? 1 : std::abs(current - ring_from_c_);
        pct = span == 0 ? 100 : std::clamp(done_c * 100 / span, 0, 100);
        lv_subject_copy_string(&temp_text_, fmt::format("{} / {}°C", current, goal).c_str());
    } else {
        lv_subject_copy_string(&temp_text_, fmt::format("{}°C", current).c_str());
    }
    lv_subject_set_int(&progress_, pct);
    lv_subject_copy_string(&pass_text_,
                           (std::string(lv_tr("Pass")) + " " + std::to_string(f.pass())).c_str());
}

} // namespace helix::ui
