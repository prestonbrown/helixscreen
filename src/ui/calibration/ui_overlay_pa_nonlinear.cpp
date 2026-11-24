// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_pa_nonlinear.h"

#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "config.h"
#include "core/observer_factory.h"
#include "core/run_after.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "pa_tower_start.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "hv/json.hpp"

namespace helix::ui {

namespace {

/// The firmware tower's default height: the top of what a pass can judge.
constexpr float kTowerHeightMm = 50.0f;

/// What a failed save says. Klipper refusing an option no model reads is
/// almost always a pressure_advance set for this extruder in another file.
std::string save_error_text(const std::string& err) {
    if (err.find("pressure_advance") == std::string::npos) {
        return err;
    }
    return err + "\n" +
           lv_tr("pressure_advance is set for this extruder somewhere the save did not reach, "
                 "such as another included file. Comment it out there and save again.");
}

int user_int(lv_event_t* e) {
    return helix::ui::event_user_int(e).value_or(0);
}

const char* param_label(helix::pa::NlParam p) {
    switch (p) {
    case helix::pa::NlParam::Offset:
        return lv_tr("Nonlinear offset");
    case helix::pa::NlParam::TimeOffset:
        return lv_tr("Time offset");
    case helix::pa::NlParam::Advance:
        break;
    }
    return lv_tr("Linear advance");
}

/// The value @p m reports for @p p, as the tower set it.
std::optional<float> reported(const helix::pa::PaModel& m, helix::pa::NlParam p) {
    switch (p) {
    case helix::pa::NlParam::Offset:
        return m.nonlinear_offset;
    case helix::pa::NlParam::TimeOffset:
        return m.time_offset;
    case helix::pa::NlParam::Advance:
        break;
    }
    return m.linear_advance;
}

std::string format_value(float v) {
    return fmt::format("{:.4f}", v);
}

/// Where the procedure under way is saved, under the printer's settings.
constexpr const char* kSessionKey = "pa_nonlinear_session";

nlohmann::json model_json(const helix::pa::PaModel& m) {
    nlohmann::json j = {{"kind", static_cast<int>(m.kind)}};
    const auto put = [&j](const char* key, const std::optional<float>& v) {
        if (v) {
            j[key] = *v;
        }
    };
    put("k", m.k);
    put("advance", m.linear_advance);
    put("offset", m.nonlinear_offset);
    put("velocity", m.linearization_velocity);
    put("time_offset", m.time_offset);
    put("smooth_time", m.smooth_time);
    return j;
}

helix::pa::PaModel model_from(const nlohmann::json& j) {
    helix::pa::PaModel m;
    const int kind = json_util::safe_int(j, "kind");
    if (kind >= static_cast<int>(helix::pa::ModelKind::Classic) &&
        kind <= static_cast<int>(helix::pa::ModelKind::Recipr)) {
        m.kind = static_cast<helix::pa::ModelKind>(kind);
    }
    const auto get = [&j](const char* key) { return json_util::read_field<float>(j, key); };
    m.k = get("k");
    m.linear_advance = get("advance");
    m.nonlinear_offset = get("offset");
    m.linearization_velocity = get("velocity");
    m.time_offset = get("time_offset");
    m.smooth_time = get("smooth_time");
    return m;
}

/// The procedure a saved session holds, or nullopt.
std::optional<helix::pa::NonlinearProcedure> saved_procedure(const nlohmann::json& saved) {
    const auto* proc = json_util::find_object(saved, "procedure");
    return proc ? helix::pa::NonlinearProcedure::from_json(*proc) : std::nullopt;
}

/// The saved procedure for @p heater and @p kind, or nullopt.
std::optional<nlohmann::json> saved_session(const std::string& heater,
                                            helix::pa::ExtruderKind kind) {
    const auto* cfg = helix::Config::get_instance();
    const nlohmann::json* j = cfg ? cfg->try_get_json(cfg->df() + kSessionKey) : nullptr;
    if (!j || json_util::safe_string(*j, "heater") != heater) {
        return std::nullopt;
    }
    const auto restored = saved_procedure(*j);
    if (!restored || restored->kind() != kind || restored->done()) {
        return std::nullopt;
    }
    return *j;
}

} // namespace

PaNonlinearOverlay::PaNonlinearOverlay() = default;

PaNonlinearOverlay::~PaNonlinearOverlay() {
    print_state_observer_.reset();
    z_observer_.reset();
    pa_observer_.reset();
    if (!StaticPanelRegistry::is_destroyed()) {
        deinit_subjects();
    }
}

// SUBJECTS

void PaNonlinearOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(stage_subject_, READY, "pa_nl_stage", subjects_);
        UI_MANAGED_SUBJECT_STRING(pass_title_, pass_title_buf_, "", "pa_nl_pass_title", subjects_);
        UI_MANAGED_SUBJECT_STRING(pass_hint_, pass_hint_buf_, "", "pa_nl_pass_hint", subjects_);
        UI_MANAGED_SUBJECT_STRING(values_, values_buf_, "", "pa_nl_values", subjects_);
        UI_MANAGED_SUBJECT_INT(has_tower_, 0, "pa_nl_has_tower", subjects_);
        UI_MANAGED_SUBJECT_INT(busy_, 0, "pa_nl_busy", subjects_);
        UI_MANAGED_SUBJECT_STRING(live_value_, live_value_buf_, "--", "pa_nl_live_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(live_text_, live_text_buf_, "", "pa_nl_live_text", subjects_);
        UI_MANAGED_SUBJECT_INT(two_sided_, 0, "pa_nl_two_sided", subjects_);
        UI_MANAGED_SUBJECT_INT(can_back_, 0, "pa_nl_can_back", subjects_);
        UI_MANAGED_SUBJECT_STRING(height_, height_buf_, "", "pa_nl_height", subjects_);
        UI_MANAGED_SUBJECT_STRING(height_value_, height_value_buf_, "", "pa_nl_height_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(error_, error_buf_, "", "pa_nl_error", subjects_);
    });
}

void PaNonlinearOverlay::deinit_subjects() {
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PaNonlinearOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_pa_nl_add_tower",
         [](lv_event_t*) { get_global_pa_nonlinear_overlay().add_tower_section(); }},
        {"on_pa_nl_print", [](lv_event_t*) { get_global_pa_nonlinear_overlay().confirm_print(); }},
        {"on_pa_nl_stop", [](lv_event_t*) { get_global_pa_nonlinear_overlay().stop_tower(); }},
        {"on_pa_nl_height",
         [](lv_event_t* e) { get_global_pa_nonlinear_overlay().adjust_height(user_int(e)); }},
        {"on_pa_nl_height_edit",
         [](lv_event_t*) { get_global_pa_nonlinear_overlay().edit_height(); }},
        {"on_pa_nl_back", [](lv_event_t*) { get_global_pa_nonlinear_overlay().go_back(); }},
        {"on_pa_nl_judge",
         [](lv_event_t* e) { get_global_pa_nonlinear_overlay().judge(user_int(e)); }},
        {"on_pa_nl_save", [](lv_event_t*) { get_global_pa_nonlinear_overlay().save_to_config(); }},
        {"on_pa_nl_keep",
         [](lv_event_t*) {
             auto& self = get_global_pa_nonlinear_overlay();
             spdlog::info("[{}] Nonlinear values kept until the next restart", self.get_name());
             self.close_procedure();
         }},
        {"on_pa_nl_discard", [](lv_event_t*) { get_global_pa_nonlinear_overlay().discard(); }},
    });
}

// LIFECYCLE

void PaNonlinearOverlay::on_activate() {
    OverlayBase::on_activate();
    refresh();
    set_stage(stage_);
}

void PaNonlinearOverlay::on_deactivating(DeactivateReason /*reason*/) {
    decision_awake_.reset();
}

void PaNonlinearOverlay::cleanup() {
    // The tower keeps printing with the screen closed; its observers stay.
    decision_awake_.reset();
    OverlayBase::cleanup();
}

void PaNonlinearOverlay::begin(const helix::pa::TestPrintParams& params,
                               helix::pa::ExtruderKind kind) {
    start(params, kind);
    show(lv_screen_active());
}

void PaNonlinearOverlay::start(const helix::pa::TestPrintParams& params,
                               helix::pa::ExtruderKind kind) {
    if (proc_ && (stage_ == PREPARING || stage_ == PRINTING)) {
        // A tower is on its way: the screen comes back, the run is not replaced.
        refresh();
        return;
    }
    const bool resume = proc_ && params_.heater == params.heater && kind_ == kind;
    if (proc_ && !resume) {
        // Another head or extruder kind: the old extruder gets its own model
        // back before the new procedure starts.
        restore_original();
    }
    params_ = params;
    if (!resume) {
        kind_ = kind;
        original_ = get_printer_state().temperature_state().extruder_pa_model(params.heater);
        proc_.emplace(kind, original_.kind == helix::pa::ModelKind::Recipr
                                ? helix::pa::ModelKind::Recipr
                                : helix::pa::ModelKind::Tanh);
        height_mm_ = kTowerHeightMm / 2.0f;
        set_stage(READY);
        spdlog::info("[{}] Nonlinear tuning on {}, {} passes", get_name(), params.heater,
                     proc_->pass_count());
        if (const auto saved = saved_session(params.heater, kind)) {
            offer_resume(*saved);
        }
    }
    if (subjects_initialized_) {
        lv_subject_set_string(&error_, "");
    }
    refresh();
}

void PaNonlinearOverlay::set_stage(Stage s) {
    stage_ = s;
    if (s == JUDGE || s == RESULT) {
        decision_awake_.acquire();
    } else {
        decision_awake_.reset();
    }
    if (s == PREPARING) {
        start_busy_.acquire();
    } else {
        start_busy_.reset();
    }
    if (subjects_initialized_) {
        lv_subject_set_int(&stage_subject_, s);
        lv_subject_set_int(&busy_, s == PREPARING ? 1 : 0);
    }
}

void PaNonlinearOverlay::fail(const std::string& message) {
    if (!message.empty()) {
        spdlog::warn("[{}] {}", get_name(), message);
    }
    if (subjects_initialized_) {
        lv_subject_set_string(&error_, message.c_str());
    }
    set_stage(READY);
    refresh();
}

void PaNonlinearOverlay::show_config_write() {
    if (!subjects_initialized_) {
        return;
    }
    // A config write is no print: nothing to stop, and the readout says so.
    lv_subject_set_int(&busy_, 2);
    lv_subject_set_string(&live_text_, lv_tr("Writing printer.cfg and restarting Klipper"));
    lv_subject_set_string(&live_value_, "");
}

ConfirmOptions PaNonlinearOverlay::owned_confirm() {
    ConfirmOptions options;
    options.owner_token = object_lifetime_.token();
    return options;
}

// READY

bool PaNonlinearOverlay::tower_available() const {
    return tower_added_ ||
           helix::pa::has_tower_module(get_printer_state().get_discovery().printer_objects());
}

void PaNonlinearOverlay::refresh() {
    if (!subjects_initialized_ || !proc_) {
        return;
    }
    lv_subject_set_int(&has_tower_, tower_available() ? 1 : 0);
    lv_subject_set_int(&can_back_, proc_->can_go_back() ? 1 : 0);
    const auto& v = proc_->values();
    lv_subject_set_string(
        &values_,
        fmt::format(fmt::runtime(lv_tr("Linear advance {}\nNonlinear offset {}\nVelocity {:.1f}"
                                       "\nTime offset {}")),
                    format_value(v.linear_advance), format_value(v.nonlinear_offset),
                    v.linearization_velocity, format_value(v.time_offset))
            .c_str());
    if (proc_->done()) {
        lv_subject_set_string(&pass_title_, lv_tr("All passes done"));
        lv_subject_set_string(&pass_hint_,
                              lv_tr("Save writes these values to the extruder's config section "
                                    "and restarts Klipper. Keep runs them until the next "
                                    "restart."));
        return;
    }
    const auto& pass = proc_->current();
    std::string title = fmt::format(fmt::runtime(lv_tr("Pass {} of {}: {}")), proc_->pass_number(),
                                    proc_->pass_count(), param_label(pass.sweep.param));
    if (proc_->repeats() > 0) {
        title += fmt::format(fmt::runtime(lv_tr(" (again, {} of {})")), proc_->repeats() + 1,
                             helix::pa::NonlinearProcedure::kMaxRepeats + 1);
    }
    lv_subject_set_string(&pass_title_, title.c_str());
    const char* hint = "";
    switch (pass.judge) {
    case helix::pa::TowerSide::Left:
        hint = lv_tr("Judge the left side, where the tower switches between slow and fast. "
                     "Note the height where those changes look cleanest.");
        break;
    case helix::pa::TowerSide::Front:
        hint = lv_tr("Judge the front, the fast side. Note the height where it looks cleanest; "
                     "light it from the side, the differences are small.");
        break;
    case helix::pa::TowerSide::LeftVsFront:
        hint = lv_tr("Note where the left side looks best and where the front looks best. "
                     "When they match, that height is the answer.");
        break;
    }
    lv_subject_set_string(&pass_hint_, hint);
    lv_subject_set_int(&two_sided_, pass.judge == helix::pa::TowerSide::LeftVsFront ? 1 : 0);
    lv_subject_set_string(&height_, fmt::format("{:.1f} mm", height_mm_).c_str());
    lv_subject_set_string(
        &height_value_,
        fmt::format("= {}", format_value(helix::pa::value_at(pass.sweep, height_mm_))).c_str());
}

void PaNonlinearOverlay::add_tower_section() {
    if (stage_ != READY || !api_) {
        return;
    }
    modal_confirm(
        lv_tr("Add the test tower to the config?"),
        lv_tr("The firmware's pressure advance tower needs a [pa_test] section in printer.cfg. "
              "HelixScreen adds it with a backup and restarts Klipper."),
        ModalSeverity::Warning, lv_tr("Add and Restart"),
        []() {
            auto& self = get_global_pa_nonlinear_overlay();
            const auto [key, value] = helix::pa::tower_section_key();
            self.set_stage(PREPARING);
            self.show_config_write();
            auto token = self.object_lifetime_.token();
            self.config_editor_.safe_multi_edit(
                *self.api_, helix::pa::kTowerSection,
                {{helix::system::ConfigEdit::Type::ADD_KEY, key, value}},
                [token, s = &self]() {
                    token.defer("PaNonlinearOverlay::tower_added", [s]() {
                        s->tower_added_ = true;
                        s->set_stage(READY);
                        s->refresh();
                    });
                },
                [token, s = &self](const std::string& err) {
                    token.defer("PaNonlinearOverlay::tower_add_failed",
                                [err, s]() { s->fail(err); });
                });
        },
        owned_confirm());
}

void PaNonlinearOverlay::confirm_print() {
    if (stage_ != READY || !proc_ || proc_->done()) {
        return;
    }
    if (refuse_empty_toolhead(params_.tool, object_lifetime_.token())) {
        return;
    }
    confirm_tower_print(
        fmt::format(fmt::runtime(lv_tr("Clear the plate and load {}. The firmware prints its "
                                       "tower in the middle of the bed while it sweeps {}.")),
                    params_.material, param_label(proc_->current().sweep.param)),
        object_lifetime_.token(), []() { get_global_pa_nonlinear_overlay().print_pass(); });
}

void PaNonlinearOverlay::print_pass() {
    if (!api_ || !proc_ || proc_->done()) {
        return;
    }
    if (job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        fail(lv_tr("A print is already running."));
        return;
    }
    if (!tower_available()) {
        fail(lv_tr("The test tower is not in the printer's config yet."));
        return;
    }
    if (refuse_empty_toolhead(params_.tool, object_lifetime_.token())) {
        return;
    }
    std::string script;
    for (const auto& line : helix::pa::machine_ready_lines(params_)) {
        script += line + "\n";
    }
    // After the start macro: one that sets pressure advance itself would
    // otherwise undo the model switch.
    script += helix::pa::set_nonlinear_gcode(params_.heater, proc_->values()) + "\n";
    script += helix::pa::tuning_tower_gcode(proc_->current().sweep) + "\n";
    script += helix::pa::print_tower_gcode(params_.nozzle_diameter_mm, params_.nozzle_temp, 0);

    saw_printing_ = false;
    script_done_ = false;
    const int pass_seq = ++pass_seq_;
    lv_subject_set_string(&error_, "");
    lv_subject_set_string(&live_text_, lv_tr("Heating and homing"));
    lv_subject_set_string(&live_value_, "--");
    set_stage(PREPARING);
    begin_observing();
    spdlog::info("[{}] Pass {} on {}: {}", get_name(), proc_->pass_number(), params_.heater,
                 helix::pa::tuning_tower_gcode(proc_->current().sweep));
    api_->execute_gcode(script,
                        object_lifetime_.bg_cb("PaNonlinearOverlay::script_done",
                                               [this, pass_seq]() { on_script_done(pass_seq); }),
                        object_lifetime_.bg_cb("PaNonlinearOverlay::start_failed",
                                               [this, pass_seq](const MoonrakerError& err) {
                                                   on_script_failed(pass_seq, err);
                                               }),
                        IMoonrakerAPI::kPreStartMacroTimeoutMs, false, nullptr, true);
}

// PRINTING

void PaNonlinearOverlay::begin_observing() {
    auto& state = get_printer_state();
    const auto lifetime = state.get_subjects_lifetime();
    z_observer_ = helix::ui::observe<int>(
        state.motion_state().get_gcode_position_z_subject(), this,
        [](PaNonlinearOverlay* self, int) { self->refresh_live(); }, lifetime);
    // The sweep sets each height's value after the move that reaches it, so
    // the readout follows the model as well as the height.
    pa_observer_ = helix::ui::observe<int>(
        state.temperature_state().get_pa_model_revision_subject(), this,
        [](PaNonlinearOverlay* self, int) { self->refresh_live(); }, lifetime);
    // RAW_PRINT_STATE_OK: the pass follows the firmware tower's own job, from
    // the edge it starts to the edge it ends, cancel and error included.
    print_state_observer_ = helix::ui::observe_print_state(
        state.print_state().get_print_state_enum_subject(), this,
        [](PaNonlinearOverlay* self, PrintJobState s) { self->on_print_state(s); }, lifetime);
}

void PaNonlinearOverlay::stop_observing() {
    print_state_observer_.reset();
    z_observer_.reset();
    pa_observer_.reset();
}

void PaNonlinearOverlay::refresh_live() {
    if (!subjects_initialized_ || !proc_ || proc_->done() || stage_ != PRINTING) {
        return;
    }
    auto& state = get_printer_state();
    const float z = static_cast<float>(
                        lv_subject_get_int(state.motion_state().get_gcode_position_z_subject())) /
                    100.0f;
    const auto param = proc_->current().sweep.param;
    const auto v = reported(state.temperature_state().extruder_pa_model(params_.heater), param);
    lv_subject_set_string(&live_value_, v ? format_value(*v).c_str() : "--");
    lv_subject_set_string(
        &live_text_,
        fmt::format(fmt::runtime(lv_tr("{} at {:.1f} mm")), param_label(param), z).c_str());
}

void PaNonlinearOverlay::on_print_state(PrintJobState state) {
    const std::string& file = get_printer_state().print_state().get_effective_print_filename();
    const bool ours = file == kTowerPrintName;
    if (printer_has_job(state)) {
        if (!ours || saw_printing_) {
            return;
        }
        saw_printing_ = true;
        set_stage(PRINTING);
        refresh_live();
        if (raise_on_start_) {
            object_lifetime_.defer("PaNonlinearOverlay::raise",
                                   [this]() { raise(kRaiseAttempts); });
        }
        return;
    }
    // Loading the tower resets print_stats to standby before it prints, and
    // that frame is no end. Once the tower has printed, standby ends it too: a
    // restart mid-tower, or a cancel folded into one frame, reports nothing else.
    if (!job_has_ended(state) && !saw_printing_) {
        return;
    }
    // Before the script returns, the state showing is the previous pass's.
    // After it, the firmware's own refusal (its temperature check runs once
    // the tower starts) can arrive as an end with no printing frame before it.
    if (!saw_printing_ && !(script_done_ && ours)) {
        return;
    }
    stop_observing();
    finish_tower(state);
    if (state == PrintJobState::ERROR) {
        fail(lv_tr("The tower stopped with an error. Nothing was changed."));
        return;
    }
    // Complete, or stopped by the user: judge what printed.
    set_stage(JUDGE);
    refresh();
}

void PaNonlinearOverlay::on_script_failed(int pass_seq, const MoonrakerError& err) {
    if (pass_seq != pass_seq_ || stage_ != PREPARING) {
        return;
    }
    if (err.type == MoonrakerErrorType::TIMEOUT ||
        err.type == MoonrakerErrorType::CONNECTION_LOST) {
        // No answer is no refusal: Klipper is still running the script (a long
        // heat soak outlasts the request), and anything sent now would queue
        // behind it and land mid-tower. The print state, or a long watchdog,
        // decides.
        spdlog::warn("[{}] No answer to pass {}'s start ({}); waiting on the printer", get_name(),
                     pass_seq, err.message);
        if (subjects_initialized_) {
            lv_subject_set_string(&live_text_, lv_tr("Waiting for the printer to start the tower"));
        }
        arm_start_watchdog(pass_seq, kLateTowerStartTimeoutMs, false);
        return;
    }
    stop_observing();
    send_ending(false);
    fail(err.message);
}

void PaNonlinearOverlay::arm_start_watchdog(int pass_seq, uint32_t delay_ms,
                                            bool disarm_on_expiry) {
    auto token = object_lifetime_.token();
    helix::run_after_ms(delay_ms, [token, pass_seq, disarm_on_expiry, this]() {
        token.defer("PaNonlinearOverlay::tower_start_timeout",
                    [pass_seq, disarm_on_expiry, this]() {
                        if (pass_seq != pass_seq_ || stage_ != PREPARING || saw_printing_) {
                            return;
                        }
                        stop_observing();
                        if (disarm_on_expiry) {
                            send_ending(false);
                        }
                        fail(lv_tr("The printer did not start the tower."));
                    });
    });
}

void PaNonlinearOverlay::on_script_done(int pass_seq) {
    if (pass_seq != pass_seq_ || stage_ != PREPARING) {
        return;
    }
    script_done_ = true;
    // PRINT_PA_TOWER returned; the tower should be printing within moments.
    arm_start_watchdog(pass_seq, kTowerStartTimeoutMs, true);
}

void PaNonlinearOverlay::finish_tower(PrintJobState state) {
    // A job that did not complete (canceled, refused, cut off by a restart) has
    // had its heaters turned off already: the end macro's retract would fail.
    send_ending(true, state == PrintJobState::COMPLETE ? helix::pa::JobEnd::Completed
                                                       : helix::pa::JobEnd::Stopped);
}

void PaNonlinearOverlay::send_ending(bool release_machine, helix::pa::JobEnd end) {
    if (!api_ || !proc_) {
        return;
    }
    // The sweep would otherwise run through the next print, and leaves its
    // last height's value behind: both go before anything else.
    std::string script = helix::pa::disarm_tuning_tower_gcode() + "\n" +
                         helix::pa::set_nonlinear_gcode(params_.heater, proc_->values());
    if (release_machine) {
        for (const auto& line : helix::pa::machine_finish_lines(params_, end)) {
            script += "\n" + line;
        }
    }
    api_->execute_gcode(
        script, []() {},
        [](const MoonrakerError& err) {
            spdlog::warn("[PA Nonlinear] Ending the tower failed: {}", err.message);
        },
        IMoonrakerAPI::kMacroTimeoutMs, false, nullptr, /*caller_surfaces_errors=*/false);
}

void PaNonlinearOverlay::stop_tower() {
    if (stage_ != PRINTING && stage_ != PREPARING) {
        return;
    }
    modal_confirm(
        lv_tr("Stop the tower?"),
        lv_tr("The print is canceled and you judge what has printed so far."),
        ModalSeverity::Warning, lv_tr("Stop"),
        [this]() {
            if (!api_) {
                return;
            }
            api_->job().cancel_print([]() {},
                                     [](const MoonrakerError& err) {
                                         spdlog::warn("[PA Nonlinear] Cancel failed: {}",
                                                      err.message);
                                     });
        },
        owned_confirm());
}

void PaNonlinearOverlay::raise(int attempts_left) {
    lv_obj_t* root = get_root();
    if (root && helix::nav::is_on_top(root)) {
        return;
    }
    if (root && helix::nav::is_in_stack(root)) {
        close();
        if (attempts_left > 0) {
            object_lifetime_.defer("PaNonlinearOverlay::raise",
                                   [this, attempts_left]() { raise(attempts_left - 1); });
        }
        return;
    }
    show(lv_screen_active());
}

// JUDGE

void PaNonlinearOverlay::adjust_height(int step_tenths) {
    set_height(height_mm_ + static_cast<float>(step_tenths) / 10.0f);
}

void PaNonlinearOverlay::set_height(float height_mm) {
    if (stage_ != JUDGE) {
        return;
    }
    // Tenths: the steps and the keypad both land on them, and float sums drift.
    height_mm_ = std::clamp(std::round(height_mm * 10.0f) / 10.0f, 0.0f, kTowerHeightMm);
    refresh();
}

void PaNonlinearOverlay::edit_height() {
    if (stage_ != JUDGE) {
        return;
    }
    ui_keypad_config_t config{};
    config.initial_value = height_mm_;
    config.min_value = 0.0f;
    config.max_value = kTowerHeightMm;
    config.allow_decimal = true;
    config.allow_negative = false;
    config.max_fraction_digits = 1;
    config.title_label = lv_tr("Best height");
    config.unit_label = "mm";
    config.on_confirm = [this, token = object_lifetime_.token()](float v) {
        if (!token.expired()) {
            set_height(v);
        }
    };
    ui_keypad_show(&config);
}

void PaNonlinearOverlay::go_back() {
    if (!proc_ || !(stage_ == READY || stage_ == JUDGE || stage_ == RESULT)) {
        return;
    }
    const auto height = proc_->go_back();
    if (!height) {
        return;
    }
    // The printer runs what the re-opened tower printed with until it is judged again.
    send_values();
    save_session();
    set_stage(JUDGE);
    height_mm_ = *height;
    if (subjects_initialized_) {
        lv_subject_set_string(&error_, "");
    }
    refresh();
}

void PaNonlinearOverlay::judge(int verdict) {
    if (stage_ != JUDGE || !proc_ || proc_->done()) {
        return;
    }
    if (proc_->current().judge == helix::pa::TowerSide::LeftVsFront) {
        const int v = std::clamp(verdict, 0, 2);
        proc_->record_verdict(static_cast<helix::pa::SideVerdict>(v), height_mm_);
    } else {
        proc_->record_height(height_mm_);
    }
    after_judgement();
}

void PaNonlinearOverlay::after_judgement() {
    send_values();
    save_session();
    height_mm_ = kTowerHeightMm / 2.0f;
    set_stage(proc_->done() ? RESULT : READY);
    refresh();
}

void PaNonlinearOverlay::send_values() {
    if (!api_ || !proc_) {
        return;
    }
    api_->execute_gcode(
        helix::pa::set_nonlinear_gcode(params_.heater, proc_->values()), []() {},
        [](const MoonrakerError& err) {
            spdlog::warn("[PA Nonlinear] Could not apply the values: {}", err.message);
        },
        0, false, nullptr, /*caller_surfaces_errors=*/false);
}

// RESULT

void PaNonlinearOverlay::save_to_config() {
    if (stage_ != RESULT || !api_ || !proc_) {
        return;
    }
    std::vector<helix::system::ConfigEdit> edits;
    std::vector<helix::system::ExpectedSetting> expected;
    for (const auto& k : helix::pa::nonlinear_config_keys(proc_->values())) {
        if (k.comment_out) {
            // REMOVE_KEY comments the line out on both edit routes; nothing is deleted.
            edits.push_back({helix::system::ConfigEdit::Type::REMOVE_KEY, k.key, ""});
        } else {
            edits.push_back({helix::system::ConfigEdit::Type::ADD_KEY, k.key, k.value});
        }
    }
    const char* model = proc_->values().model == helix::pa::ModelKind::Recipr ? "recipr" : "tanh";
    expected.push_back({params_.heater, "pressure_advance_model", model});
    set_stage(PREPARING);
    show_config_write();
    auto token = object_lifetime_.token();
    config_editor_.safe_multi_edit(
        *api_, params_.heater, edits,
        [token, this]() {
            helix::ui::queue_update("PaNonlinearOverlay::saved_toast", []() {
                ToastManager::instance().show(ToastSeverity::SUCCESS,
                                              lv_tr("Nonlinear pressure advance saved"), 2500);
            });
            token.defer("PaNonlinearOverlay::saved", [this]() { close_procedure(); });
        },
        [token, this](const std::string& err) {
            token.defer("PaNonlinearOverlay::save_failed", [err, this]() {
                set_stage(RESULT);
                refresh();
                if (subjects_initialized_) {
                    lv_subject_set_string(&error_, save_error_text(err).c_str());
                }
            });
        },
        15000, std::move(expected));
}

void PaNonlinearOverlay::discard() {
    if (!proc_) {
        close();
        return;
    }
    if (stage_ == PRINTING || (stage_ == PREPARING && !script_done_)) {
        return;
    }
    restore_original();
    close_procedure();
}

void PaNonlinearOverlay::restore_original() {
    if (!api_ || !proc_ ||
        job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        return;
    }
    api_->execute_gcode(
        helix::pa::disarm_tuning_tower_gcode() + "\n" +
            helix::pa::restore_model_gcode(params_.heater, original_),
        []() {},
        [](const MoonrakerError& err) {
            spdlog::warn("[PA Nonlinear] Could not restore the original model: {}", err.message);
        },
        0, false, nullptr, /*caller_surfaces_errors=*/false);
}

void PaNonlinearOverlay::close_procedure() {
    stop_observing();
    clear_saved_session();
    proc_.reset();
    set_stage(READY);
    close();
}

// SESSION

void PaNonlinearOverlay::save_session() {
    auto* cfg = Config::get_instance();
    if (!cfg || !proc_) {
        return;
    }
    cfg->set<nlohmann::json>(cfg->df() + kSessionKey, {{"heater", params_.heater},
                                                       {"procedure", proc_->to_json()},
                                                       {"original", model_json(original_)}});
    cfg->save();
}

void PaNonlinearOverlay::clear_saved_session() {
    auto* cfg = Config::get_instance();
    if (!cfg || !cfg->exists(cfg->df() + kSessionKey)) {
        return;
    }
    std::string printer = cfg->df();
    printer.pop_back(); // the section itself, without its trailing '/'
    cfg->get_json(printer).erase(kSessionKey);
    cfg->save();
}

void PaNonlinearOverlay::offer_resume(const nlohmann::json& saved) {
    const auto proc = saved_procedure(saved);
    if (!proc) {
        return;
    }
    ConfirmOptions options = owned_confirm();
    options.cancel_text = lv_tr("Start Over");
    // owned_confirm()'s token keeps both callbacks from outliving this object.
    options.on_cancel = [this]() { clear_saved_session(); };
    modal_confirm(
        lv_tr("Continue the earlier tuning?"),
        fmt::format(fmt::runtime(lv_tr("Passes judged before are kept and pass {} "
                                       "of {} is next. Start Over begins again from "
                                       "pass 1.")),
                    proc->pass_number(), proc->pass_count())
            .c_str(),
        ModalSeverity::Info, lv_tr("Continue"), [this, saved]() { resume(saved); }, options);
}

void PaNonlinearOverlay::resume(const nlohmann::json& saved) {
    if (!proc_ || stage_ != READY) {
        return;
    }
    auto proc = saved_procedure(saved);
    if (!proc || proc->kind() != kind_) {
        return;
    }
    proc_ = std::move(proc);
    if (const auto* original = json_util::find_object(saved, "original")) {
        original_ = model_from(*original);
    }
    spdlog::info("[{}] Continuing nonlinear tuning on {} at pass {}", get_name(), params_.heater,
                 proc_->pass_number());
    // The printer may have restarted since: it runs the procedure's values again,
    // unless a job holds it.
    if (!job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        send_values();
    }
    refresh();
}

} // namespace helix::ui
