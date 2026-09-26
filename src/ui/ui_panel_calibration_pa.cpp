// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_calibration_pa.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_temperature_utils.h"
#include "ui_timer_guard.h"
#include "ui_toast_manager.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "filament_database.h"
#include "filament_op_slot_resolver.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "pa_calibration.h"
#include "preset_materials.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "temperature_controller.h"
#include "thermal_rate_model.h"
#include "tool_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace helix::ui {

namespace {

/// Four decimals, always. Every digit is meaningful, and a value shown as
/// "0.04" would be typed into a slicer as a different filament's setting.
std::string format_k(float k) {
    return fmt::format("{:.4f}", k);
}

/// m:ss, the shape of a clock somebody is actually waiting on.
std::string format_clock(int seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    return fmt::format("{}:{:02d}", seconds / 60, seconds % 60);
}

int material_nozzle_temp(const std::string& name) {
    auto mat = filament::find_material(name.c_str());
    return mat ? mat->nozzle_recommended() : PACalibrationPanel::TEMP_DEFAULT;
}

} // namespace

// ============================================================================
// CONSTRUCTION
// ============================================================================

PACalibrationPanel::PACalibrationPanel() {
    spdlog::trace("[PACal] Instance created");
}

PACalibrationPanel::~PACalibrationPanel() {
    // A raw timer cancelled only in cleanup() stays armed on a freed `this`
    // when StaticPanelRegistry::destroy_all() runs before lv_deinit()
    // (CLAUDE.md threading rule 5). Both paths share cancel_eta_timer().
    cancel_eta_timer();
    if (!StaticPanelRegistry::is_destroyed()) {
        deinit_subjects();
    }
    spdlog::trace("[PACal] Instance destroyed");
}

// ============================================================================
// SUBJECTS
// ============================================================================

void PACalibrationPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized", get_name());
        return;
    }

    UI_MANAGED_SUBJECT_INT(state_subject_, IDLE, "pa_cal_state", subjects_);
    UI_MANAGED_SUBJECT_INT(multi_tool_, 0, "pa_cal_multi_tool", subjects_);
    UI_MANAGED_SUBJECT_INT(tool_count_, 1, "pa_cal_tool_count", subjects_);
    UI_MANAGED_SUBJECT_INT(inputs_live_, 1, "pa_cal_inputs_live", subjects_);
    UI_MANAGED_SUBJECT_INT(progress_, 0, "pa_cal_progress", subjects_);
    UI_MANAGED_SUBJECT_INT(result_plausible_, 1, "pa_cal_result_plausible", subjects_);
    UI_MANAGED_SUBJECT_INT(action_is_stop_, 0, "pa_cal_action_is_stop", subjects_);
    UI_MANAGED_SUBJECT_INT(has_last_, 0, "pa_cal_has_last", subjects_);

    for (int i = 0; i < MAX_TOOLS; ++i) {
        UI_MANAGED_SUBJECT_INT(tool_selected_[i], i == 0 ? 1 : 0,
                               fmt::format("pa_cal_tool_selected_{}", i).c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(tool_sub_[i], tool_sub_buf_[i], "",
                                  fmt::format("pa_cal_tool_sub_{}", i).c_str(), subjects_);
    }

    for (int i = 0; i < PRESET_SLOTS; ++i) {
        UI_MANAGED_SUBJECT_INT(preset_selected_[i], 0,
                               fmt::format("pa_cal_preset_selected_{}", i).c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(preset_temp_[i], preset_temp_buf_[i], "",
                                  fmt::format("pa_cal_preset_temp_{}", i).c_str(), subjects_);
    }

    for (int i = 0; i < PHASE_COUNT; ++i) {
        UI_MANAGED_SUBJECT_INT(phase_state_[i], 0, fmt::format("pa_cal_phase_state_{}", i).c_str(),
                               subjects_);
        UI_MANAGED_SUBJECT_STRING(phase_name_[i], phase_name_buf_[i], "",
                                  fmt::format("pa_cal_phase_name_{}", i).c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(phase_meta_[i], phase_meta_buf_[i], "",
                                  fmt::format("pa_cal_phase_meta_{}", i).c_str(), subjects_);
    }

    UI_MANAGED_SUBJECT_STRING(temp_display_, temp_display_buf_, "245°", "pa_cal_temp_display",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(temp_note_, temp_note_buf_, "170-300 °C", "pa_cal_temp_note",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(phase_label_, phase_label_buf_, "", "pa_cal_phase_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(big_, big_buf_, "", "pa_cal_big", subjects_);
    UI_MANAGED_SUBJECT_STRING(big_sub_, big_sub_buf_, "", "pa_cal_big_sub", subjects_);
    UI_MANAGED_SUBJECT_STRING(remaining_, remaining_buf_, "--:--", "pa_cal_remaining", subjects_);
    UI_MANAGED_SUBJECT_STRING(prog_foot_, prog_foot_buf_, "", "pa_cal_prog_foot", subjects_);
    UI_MANAGED_SUBJECT_STRING(result_, result_buf_, "--", "pa_cal_result", subjects_);
    UI_MANAGED_SUBJECT_STRING(keep_note_, keep_note_buf_, "", "pa_cal_keep_note", subjects_);
    UI_MANAGED_SUBJECT_STRING(result_sanity_, result_sanity_buf_, "", "pa_cal_result_sanity",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(error_title_, error_title_buf_, "", "pa_cal_error_title", subjects_);
    UI_MANAGED_SUBJECT_STRING(error_detail_, error_detail_buf_, "", "pa_cal_error_detail",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(error_hint_, error_hint_buf_, "", "pa_cal_error_hint", subjects_);
    UI_MANAGED_SUBJECT_STRING(action_text_, action_text_buf_, "Start", "pa_cal_action_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(last_value_, last_value_buf_, "", "pa_cal_last_value", subjects_);
    UI_MANAGED_SUBJECT_STRING(last_context_, last_context_buf_, "", "pa_cal_last_context",
                              subjects_);

    subjects_initialized_ = true;

    register_xml_callbacks({
        {"on_pa_cal_action", on_action_clicked},
        {"on_pa_cal_start", on_start_clicked},
        {"on_pa_cal_reset", on_reset_clicked},
        {"on_pa_cal_tool", on_tool_clicked},
        {"on_pa_cal_preset", on_preset_clicked},
        {"on_pa_cal_temp_up", on_temp_up},
        {"on_pa_cal_temp_down", on_temp_down},
    });

    spdlog::debug("[{}] Subjects and callbacks registered", get_name());
}

void PACalibrationPanel::deinit_subjects() {
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

lv_obj_t* PACalibrationPanel::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::debug("[{}] Overlay already created", get_name());
        return overlay_root_;
    }
    parent_screen_ = parent;
    if (!create_overlay_from_xml(parent, "calibration_pa_panel")) {
        return nullptr;
    }
    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void PACalibrationPanel::on_activate() {
    OverlayBase::on_activate();

    // A run never survives leaving the screen, so opening it is always a clean
    // slate, except for the last measured value, which is the most useful
    // thing on the screen when coming back.
    attempts_seen_ = 0;

    // The chips must show what is actually mounted. A toolchange started from
    // here takes ~20s and is confirmed by the printer, not by the tap, so the
    // selection follows ToolState rather than leading it.
    // The lifetime is mandatory, not optional: observe_* takes it as a
    // defaulted parameter, so omitting it is silent - the guard never learns
    // the subject died and reset() then touches freed memory (#705).
    auto& tool_state = helix::ToolState::instance();
    active_tool_observer_ = helix::ui::observe_int_sync<PACalibrationPanel>(
        tool_state.get_active_tool_subject(), this,
        [](PACalibrationPanel* self, int) { self->refresh_tools(); },
        tool_state.get_subjects_lifetime());
    // Seed from the mounted tool exactly once, as the screen opens.
    refresh_tools(/*adopt_active=*/true);
    refresh_presets();
    update_temp_display();
    publish_last_result();
    set_state(IDLE);

    spdlog::debug("[{}] on_activate (tool={}, temp={})", get_name(), selected_tool_, target_temp_);
}

void PACalibrationPanel::on_deactivating(DeactivateReason) {
    active_tool_observer_.reset();
    // Leaving mid-run stops it: the machine is extruding, and a run nobody is
    // watching has no way to report anything.
    if (state_ == HEATING || state_ == MEASURING) {
        stop_run(/*user_requested=*/false);
    }
    stop_heat_tracking();
}

void PACalibrationPanel::cleanup() {
    active_tool_observer_.reset();
    stop_heat_tracking();
    OverlayBase::cleanup();
}

// ============================================================================
// CAPABILITY
// ============================================================================

bool PACalibrationPanel::printer_supports_calibration() {
    return helix::pacal::is_supported(get_printer_state().get_discovery());
}

// ============================================================================
// TARGET COLUMN
// ============================================================================

void PACalibrationPanel::refresh_tools(bool adopt_active) {
    auto& tools = helix::ToolState::instance();
    const int count = std::min(tools.tool_count(), MAX_TOOLS);
    const bool per_tool =
        helix::pacal::is_per_tool(get_printer_state().get_discovery()) && tools.is_multi_tool();

    // A picker the firmware cannot honour is worse than no picker: on a
    // whole-machine command the choice would silently do nothing.
    lv_subject_set_int(&multi_tool_, per_tool ? 1 : 0);
    lv_subject_set_int(&tool_count_, std::max(1, count));

    if (selected_tool_ >= count) {
        selected_tool_ = 0;
    }
    // Opening on the tool the machine already has mounted saves the common
    // case a tap and is what the user means by "this filament". Only on open,
    // though - see the note on the declaration.
    if (per_tool && adopt_active) {
        const int active = tools.active_tool_index();
        if (active >= 0 && active < count) {
            selected_tool_ = active;
        }
    }

    // The material each tool is loaded with. Context under the T number is what
    // makes "T1" mean "the PETG one", and a material name is short enough to
    // survive a chip barely 50px wide - a spool name ("Jet Black") is not, and
    // simply clipped. The tool->slot step goes through resolve_op_button_slot(),
    // the same resolver the filament panel's Load/Unload gating uses, so this
    // can never disagree with the rest of the app about which lane feeds a tool.
    AmsBackend* backend = AmsState::instance().get_backend();
    const AmsSystemInfo sys = backend ? backend->get_system_info() : AmsSystemInfo{};
    for (int i = 0; i < MAX_TOOLS; ++i) {
        lv_subject_set_int(&tool_selected_[i], i == selected_tool_ ? 1 : 0);
        // Only when the filament system actually knows. An empty chip says
        // "no information", which is true; a guessed or stale material would
        // make T1 read as the PETG one when it is not.
        std::string sub;
        if (backend && i < count) {
            const int slot = helix::ui::resolve_op_button_slot(sys, i, tools.tool_count());
            if (const SlotInfo* info = sys.get_slot_global(slot)) {
                sub = info->material;
            }
        }
        lv_subject_copy_string(&tool_sub_[i], sub.c_str());
    }
}

void PACalibrationPanel::refresh_presets() {
    for (int i = 0; i < PRESET_SLOTS; ++i) {
        const std::string name = helix::presets::name(i);
        // Nozzle only. The shared preset_material_N_temps subject carries
        // "220°C / 60°C"; the bed is not part of this decision and would read
        // as noise beside a nozzle target.
        const int temp = name.empty() ? 0 : material_nozzle_temp(name);
        lv_subject_copy_string(&preset_temp_[i], temp ? fmt::format("{}°", temp).c_str() : "");
        lv_subject_set_int(&preset_selected_[i], i == selected_preset_ ? 1 : 0);
    }
}

void PACalibrationPanel::update_temp_display() {
    lv_subject_copy_string(&temp_display_, fmt::format("{}°", target_temp_).c_str());
    lv_subject_copy_string(&temp_note_, state_ == IDLE || state_ == COMPLETE || state_ == ERROR
                                            ? lv_tr("170-300 °C")
                                            : lv_tr("locked"));
}

// ============================================================================
// STATE
// ============================================================================

void PACalibrationPanel::set_state(State s) {
    state_ = s;
    lv_subject_set_int(&state_subject_, static_cast<int>(s));

    const bool live = (s == IDLE || s == COMPLETE || s == ERROR);
    lv_subject_set_int(&inputs_live_, live ? 1 : 0);

    update_action_button();
    update_phase_rows();
    update_temp_display();

    spdlog::debug("[{}] state -> {}", get_name(), static_cast<int>(s));
}

void PACalibrationPanel::update_action_button() {
    const bool running = (state_ == HEATING || state_ == MEASURING);
    lv_subject_set_int(&action_is_stop_, running ? 1 : 0);
    lv_subject_copy_string(&action_text_, running ? lv_tr("Stop") : lv_tr("Start"));
}

void PACalibrationPanel::update_phase_rows() {
    lv_subject_copy_string(&phase_name_[PHASE_HEAT], lv_tr("Heat nozzle"));
    lv_subject_copy_string(&phase_name_[PHASE_MEASURE], lv_tr("Measure"));

    // 0 pending, 1 running, 2 done.
    int heat = 0;
    int measure = 0;
    if (state_ == HEATING) {
        heat = 1;
    } else if (state_ == MEASURING) {
        heat = 2;
        measure = 1;
    } else if (state_ == COMPLETE) {
        heat = 2;
        measure = 2;
    }
    lv_subject_set_int(&phase_state_[PHASE_HEAT], heat);
    lv_subject_set_int(&phase_state_[PHASE_MEASURE], measure);
}

void PACalibrationPanel::publish_last_result() {
    lv_subject_set_int(&has_last_, have_result_ ? 1 : 0);
    if (!have_result_) {
        return;
    }
    lv_subject_copy_string(&last_value_, format_k(result_k_).c_str());
    // Material and temperature travel with the number. Without them it is
    // nearly useless a week later - which is exactly the mistake a sticky note
    // makes.
    std::string ctx = result_material_.empty()
                          ? fmt::format("at {}°", result_temp_)
                          : fmt::format("{} at {}°", result_material_, result_temp_);
    if (lv_subject_get_int(&multi_tool_) == 1) {
        ctx = fmt::format("T{}, {}", result_tool_, ctx);
    }
    lv_subject_copy_string(&last_context_, ctx.c_str());
}

// ============================================================================
// RUNNING
// ============================================================================

void PACalibrationPanel::confirm_and_start() {
    if (state_ == HEATING || state_ == MEASURING) {
        return;
    }

    // One dialog, covering only what actually matters and what the machine
    // cannot check for itself. The plate must be ON - the opposite of the tool
    // offset screen, which is a real source of confusion for anyone who used
    // that one, so it is stated rather than implied.
    const std::string msg =
        fmt::format("{}\n\n{}\n\n{}",
                    lv_tr("Filament must be loaded in the extruder, and the "
                          "build plate on with nothing in the way of the head."),
                    lv_tr("The nozzle extrudes a few short test moves in a corner, away from the "
                          "print area, and repeats until it agrees with itself."),
                    fmt::format(fmt::runtime(lv_tr("At {}°C, about {} minutes.")), target_temp_,
                                estimate_minutes()));

    modal_confirm(lv_tr("Before starting"), msg.c_str(), ModalSeverity::Warning, lv_tr("Start"),
                  []() { get_global_pa_cal_panel().begin_run(); });
}

void PACalibrationPanel::begin_run() {
    if (!api_) {
        show_error("No connection to the printer.");
        return;
    }

    attempts_seen_ = 0;
    result_k_ = 0.0f;
    lv_subject_set_int(&progress_, 0);

    // The firmware command assumes a hot nozzle, so the heat-up is ours. Every
    // nozzle target in the app goes through TemperatureController - it is the
    // single authority, and a raw set_temperature() here would bypass the
    // clamping and bookkeeping every other surface relies on.
    auto* controller = get_temperature_controller();
    if (!controller) {
        show_error("Temperature control is unavailable.");
        return;
    }
    controller->set_target(selected_heater(), static_cast<double>(target_temp_));

    set_state(HEATING);
    lv_subject_copy_string(&phase_label_, lv_tr("Heating nozzle"));
    lv_subject_copy_string(
        &prog_foot_,
        lv_tr("Heating is nearly the whole wait. Leaving the screen now stops the run."));
    start_heat_tracking();
}

void PACalibrationPanel::begin_measure() {
    auto proc = helix::pacal::procedure_for(get_printer_state().get_discovery(), selected_tool_,
                                            target_temp_);
    if (!proc) {
        show_error("This printer cannot measure pressure advance automatically.");
        return;
    }
    attempts_expected_ = proc->expected_attempts;

    set_state(MEASURING);
    phase_start_tick_ms_ = lv_tick_get();
    lv_subject_copy_string(&phase_label_, lv_tr("Measuring"));
    lv_subject_copy_string(&big_, "...");
    lv_subject_copy_string(&big_sub_, lv_tr("K so far"));
    lv_subject_copy_string(&prog_foot_,
                           lv_tr("The printer extrudes in a corner and repeats until the readings "
                                 "agree. You do not judge anything by eye."));

    spdlog::info("[{}] Measuring via {}: {}", get_name(), proc->provider, proc->start_gcode);

    // tok.defer, not queue_update: these fire on the response thread and the
    // guard drops the call if the panel is gone before the main-thread drain.
    auto tok = lifetime_.token();
    pa_cancel_ = api_->advanced().start_pa_calibrate(
        *proc,
        [this, tok](float k) {
            tok.defer("PACalibrationPanel::on_result", [this, k]() { on_result(k); });
        },
        [this, tok](const MoonrakerError& err) {
            const std::string msg = err.message;
            tok.defer("PACalibrationPanel::on_error", [this, msg]() { on_error(msg); });
        },
        [this, tok](int attempt, int expected, float k_so_far) {
            tok.defer("PACalibrationPanel::on_attempt", [this, attempt, expected, k_so_far]() {
                on_attempt(attempt, expected, k_so_far);
            });
        });
}

int PACalibrationPanel::estimate_minutes() const {
    const auto proc = helix::pacal::procedure_for(get_printer_state().get_discovery(),
                                                  selected_tool_, target_temp_);
    return proc ? proc->estimate_minutes : helix::pacal::Procedure{}.estimate_minutes;
}

void PACalibrationPanel::stop_run(bool user_requested) {
    if (state_ != HEATING && state_ != MEASURING) {
        return;
    }
    stop_heat_tracking();

    if (state_ == MEASURING) {
        // The command is running and Klipper cannot interrupt it. Cooling the
        // nozzle under a firmware that is still purging turns a stop into a
        // cold-extrude error, so the heater stays with the firmware and only
        // the screen stops waiting.
        if (pa_cancel_) {
            pa_cancel_();
            pa_cancel_ = nullptr;
        }
        ToastManager::instance().show(
            ToastSeverity::INFO, lv_tr("The printer finishes this measurement on its own."), 5000);
    } else if (auto* controller = get_temperature_controller()) {
        // Nothing was sent yet: the heat-up is ours, so it is ours to undo.
        controller->set_target(selected_heater(), 0.0);
    }

    spdlog::info("[{}] Run stopped ({})", get_name(), user_requested ? "user" : "left screen");
    set_state(IDLE);
    lv_subject_set_int(&progress_, 0);
}

void PACalibrationPanel::on_result(float k) {
    // A result only belongs to the run this screen is waiting on.
    if (state_ != MEASURING) {
        return;
    }
    pa_cancel_ = nullptr;
    stop_heat_tracking();

    result_k_ = k;
    have_result_ = true;
    result_tool_ = selected_tool_;
    result_temp_ = target_temp_;
    result_material_ = selected_preset_ >= 0 ? helix::presets::name(selected_preset_) : "";

    // The nozzle has no more work to do.
    if (auto* controller = get_temperature_controller()) {
        controller->set_target(selected_heater(), 0.0);
    }

    const auto& hw = get_printer_state().get_discovery();
    // Whether the value already lives on the printer decides what the user has
    // left to do with it.
    const auto proc = helix::pacal::procedure_for(hw, selected_tool_, target_temp_);
    lv_subject_copy_string(
        &keep_note_,
        proc && proc->applies_result
            ? lv_tr("The printer applied this value and keeps it for this tool.")
            : lv_tr("Copy it into this filament's slicer profile. The printer keeps nothing."));

    // The kind lands inside a translated sentence, so it is translated too.
    const auto extruder_kind_label = [](const char* kind) -> const char* {
        return std::string_view(kind) == "direct drive" ? lv_tr("direct drive") : kind;
    };
    const bool plausible = helix::pacal::is_plausible(hw, k);
    const auto range = helix::pacal::sane_range(hw);

    lv_subject_copy_string(&result_, format_k(k).c_str());
    lv_subject_set_int(&result_plausible_, plausible ? 1 : 0);
    lv_subject_copy_string(
        &result_sanity_,
        plausible
            ? fmt::format(fmt::runtime(lv_tr("Typical for a {} extruder ({:.2f}-{:.2f}).")),
                          extruder_kind_label(range.extruder_kind), range.low, range.high)
                  .c_str()
            // The one judgement the machine cannot make for itself: a value
            // that parsed fine and is still wrong.
            : fmt::format(fmt::runtime(lv_tr("Outside the usual {:.2f}-{:.2f} for a {} extruder. "
                                             "Worth measuring again before trusting it.")),
                          range.low, range.high, extruder_kind_label(range.extruder_kind))
                  .c_str());

    lv_subject_set_int(&progress_, 100);
    publish_last_result();
    set_state(COMPLETE);
    spdlog::info("[{}] Result K={:.4f} (plausible={})", get_name(), k, plausible);
}

void PACalibrationPanel::on_error(const std::string& message) {
    // An error only belongs to the run this screen is waiting on.
    if (state_ != MEASURING) {
        return;
    }
    pa_cancel_ = nullptr;
    stop_heat_tracking();
    if (auto* controller = get_temperature_controller()) {
        controller->set_target(selected_heater(), 0.0);
    }
    show_error(message);
}

void PACalibrationPanel::on_attempt(int attempt, int expected, float k_so_far) {
    if (state_ != MEASURING) {
        return;
    }
    attempts_seen_ = attempt;
    if (expected > 0) {
        attempts_expected_ = expected;
    }
    lv_subject_copy_string(&phase_meta_[PHASE_MEASURE],
                           fmt::format(fmt::runtime(lv_tr("attempt {}")), attempt).c_str());

    // The candidate the printer is trying right now. Without it the big
    // readout sits empty for the whole measuring phase and a machine that is
    // deliberately repeating itself looks like a machine that has hung.
    if (k_so_far > 0.0f) {
        lv_subject_copy_string(&big_, format_k(k_so_far).c_str());
    }
    update_progress_display();
}

void PACalibrationPanel::show_error(const std::string& message) {
    // A refusal is long, specific and the most valuable text on the screen. It
    // gets the whole stage, verbatim - never a truncated title.
    lv_subject_copy_string(&error_title_, lv_tr("Run stopped"));
    lv_subject_copy_string(&error_detail_, message.c_str());
    lv_subject_copy_string(&error_hint_,
                           lv_tr("Nothing was changed. Temperature and material are kept, so a "
                                 "retry is one tap."));
    lv_subject_set_int(&progress_, 0);
    set_state(ERROR);
    spdlog::warn("[{}] Run failed: {}", get_name(), message);
}

// ============================================================================
// HEATING
// ============================================================================

void PACalibrationPanel::start_heat_tracking() {
    stop_heat_tracking();

    auto& state = get_printer_state();
    lv_subject_t* temp_subj = state.get_extruder_temp_subject(selected_heater(), temp_lifetime_);
    if (!temp_subj) {
        spdlog::warn("[{}] No temperature subject for {}", get_name(), selected_heater());
        // Without a thermometer there is no way to know when to measure.
        show_error("The printer is not reporting this tool's temperature.");
        return;
    }

    heat_start_temp_ = helix::ui::temperature::deci_to_degrees_f(lv_subject_get_int(temp_subj));
    phase_start_tick_ms_ = lv_tick_get();

    temp_observer_ = helix::ui::observe_int_sync<PACalibrationPanel>(
        temp_subj, this, [](PACalibrationPanel* self, int value) { self->on_nozzle_temp(value); },
        temp_lifetime_);

    eta_timer_ = lv_timer_create(on_eta_tick, 1000, this);
    update_progress_display();
}

void PACalibrationPanel::stop_heat_tracking() {
    // Lifetime BEFORE observer - CLAUDE.md mandatory ordering.
    temp_lifetime_.reset();
    temp_observer_.reset();
    cancel_eta_timer();
}

void PACalibrationPanel::cancel_eta_timer() {
    // lv_timer_cancel_safe self-guards on lv_is_initialized() and neuters
    // rather than unlinking, so this is safe from the destructor and from
    // inside lv_timer_handler.
    helix::ui::lv_timer_cancel_safe(eta_timer_);
    eta_timer_ = nullptr;
}

void PACalibrationPanel::on_nozzle_temp(int temp_deci) {
    if (state_ != HEATING) {
        return;
    }
    const float temp = helix::ui::temperature::deci_to_degrees_f(temp_deci);

    lv_subject_copy_string(&big_, fmt::format("{:.0f}°", temp).c_str());
    // Spelled out, not "-> 245" and not an arrow glyph: U+2192 is not in the
    // baked Noto subset and drew as a tofu box.
    lv_subject_copy_string(&big_sub_,
                           fmt::format(fmt::runtime(lv_tr("target {}°")), target_temp_).c_str());
    lv_subject_copy_string(&phase_meta_[PHASE_HEAT],
                           fmt::format("{:.0f}° / {}°", temp, target_temp_).c_str());
    update_progress_display();

    if (temp >= static_cast<float>(target_temp_) - AT_TARGET_TOLERANCE_C) {
        spdlog::info("[{}] At target ({:.1f}° >= {}°), starting measurement", get_name(), temp,
                     target_temp_);
        stop_heat_tracking();
        begin_measure();
    }
}

void PACalibrationPanel::on_eta_tick(lv_timer_t* timer) {
    auto* self = static_cast<PACalibrationPanel*>(lv_timer_get_user_data(timer));
    if (self) {
        self->update_progress_display();
    }
}

void PACalibrationPanel::update_progress_display() {
    if (state_ != HEATING && state_ != MEASURING) {
        return;
    }

    int remaining_s = 0;
    int pct = 0;

    if (state_ == HEATING) {
        // The shared thermal model already learns this machine's heat rate for
        // every other preheat in the app; forking an estimator here would make
        // two screens disagree about the same nozzle.
        auto& state = get_printer_state();
        lv_subject_t* temp_subj =
            state.get_extruder_temp_subject(selected_heater(), temp_lifetime_);
        const float temp =
            temp_subj ? helix::ui::temperature::deci_to_degrees_f(lv_subject_get_int(temp_subj))
                      : heat_start_temp_;
        const float heat_s = ThermalRateManager::instance().estimate_heating_seconds(
            selected_heater(), temp, static_cast<float>(target_temp_));
        remaining_s = static_cast<int>(heat_s) + MEASURE_SECONDS_ESTIMATE;

        // Heating owns the bar up to 80%: it is most of the wait, and a bar
        // that sprints to 90% then sits there is a worse lie than a slow one.
        const float span = static_cast<float>(target_temp_) - heat_start_temp_;
        const float done = span > 1.0f ? (temp - heat_start_temp_) / span : 1.0f;
        pct = static_cast<int>(std::clamp(done, 0.0f, 1.0f) * 80.0f);
    } else {
        const uint32_t elapsed_ms = lv_tick_elaps(phase_start_tick_ms_);
        remaining_s = MEASURE_SECONDS_ESTIMATE - static_cast<int>(elapsed_ms / 1000);
        // Attempts are the honest progress signal when the firmware reports
        // them; otherwise fall back to the clock.
        const float frac =
            attempts_expected_ > 0
                ? std::clamp(static_cast<float>(attempts_seen_) / attempts_expected_, 0.0f, 1.0f)
                : std::clamp(static_cast<float>(elapsed_ms) / (MEASURE_SECONDS_ESTIMATE * 1000.0f),
                             0.0f, 1.0f);
        pct = 80 + static_cast<int>(frac * 20.0f);
    }

    lv_subject_set_int(&progress_, std::clamp(pct, 0, 100));
    lv_subject_copy_string(&remaining_, format_clock(std::max(0, remaining_s)).c_str());
}

void PACalibrationPanel::select_tool(int tool) {
    auto& tools = helix::ToolState::instance();
    if (tool < 0 || tool >= tools.tool_count()) {
        return;
    }
    if (tool == tools.active_tool_index()) {
        return; // already on the carriage; SELECT_TOOL would be a firmware no-op
    }

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        // Nothing can mount a tool, so nothing should pretend to. The chip
        // selection stays where the printer actually is.
        spdlog::warn("[{}] No filament backend - cannot change tool", get_name());
        return;
    }

    // change_tool() is the same call the filament panel makes, so this cannot
    // disagree with the rest of the app about how a tool is mounted. It is
    // fire-and-forget: ToolState's active_tool subject is what confirms it, and
    // the chips follow that, not this call.
    spdlog::info("[{}] Changing to tool T{}", get_name(), tool);
    const AmsError err = backend->change_tool(tool);
    if (!err.success()) {
        helix::ui::notify_ams_error(err);
    }
}

std::string PACalibrationPanel::selected_heater() const {
    const auto& tools = helix::ToolState::instance().tools();
    if (selected_tool_ >= 0 && selected_tool_ < static_cast<int>(tools.size())) {
        return tools[selected_tool_].effective_heater();
    }
    return "extruder";
}

// ============================================================================
// EVENT TRAMPOLINES
// ============================================================================

void PACalibrationPanel::on_action_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[PACal] on_action_clicked");
    auto& panel = get_global_pa_cal_panel();
    if (panel.state_ == HEATING || panel.state_ == MEASURING) {
        panel.stop_run(/*user_requested=*/true);
    } else {
        panel.confirm_and_start();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PACalibrationPanel::on_reset_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[PACal] on_reset_clicked");
    // Back to the setup screen, not straight into another run: after seeing a
    // number the usual next move is to change the tool or the temperature, and
    // re-running the identical measurement is rarely what was meant.
    auto& panel = get_global_pa_cal_panel();
    panel.set_state(IDLE);
    lv_subject_set_int(&panel.progress_, 0);
    LVGL_SAFE_EVENT_CB_END();
}

void PACalibrationPanel::on_start_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[PACal] on_start_clicked");
    get_global_pa_cal_panel().confirm_and_start();
    LVGL_SAFE_EVENT_CB_END();
}

void PACalibrationPanel::on_tool_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PACal] on_tool_clicked");
    // user_data carries the tool index as a string ("0".."3"), the same
    // convention as the tool offset panel's per-row buttons.
    const char* arg = static_cast<const char*>(lv_event_get_user_data(e));
    if (arg && *arg) {
        auto& panel = get_global_pa_cal_panel();
        if (panel.state_ == IDLE || panel.state_ == COMPLETE || panel.state_ == ERROR) {
            const int tool = std::atoi(arg);
            panel.selected_tool_ = tool;
            // Pressure advance belongs to an extruder, and the firmware
            // measures whichever one is mounted - so picking a tool here has to
            // actually mount it, not just tint a chip.
            panel.select_tool(tool);
            panel.refresh_tools();
            // A new tool means a new extruder and a new answer; a stale result
            // card beside a different tool would be a lie.
            if (panel.state_ != IDLE) {
                panel.set_state(IDLE);
            }
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PACalibrationPanel::on_preset_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PACal] on_preset_clicked");
    const char* arg = static_cast<const char*>(lv_event_get_user_data(e));
    if (arg && *arg) {
        auto& panel = get_global_pa_cal_panel();
        if (panel.state_ == IDLE || panel.state_ == COMPLETE || panel.state_ == ERROR) {
            const int slot = std::atoi(arg);
            const std::string name = helix::presets::name(slot);
            if (!name.empty()) {
                panel.selected_preset_ = slot;
                panel.target_temp_ = std::clamp(material_nozzle_temp(name), TEMP_MIN, TEMP_MAX);
                panel.refresh_presets();
                panel.update_temp_display();
            }
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PACalibrationPanel::on_temp_up(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[PACal] on_temp_up");
    auto& panel = get_global_pa_cal_panel();
    panel.target_temp_ = std::min(panel.target_temp_ + TEMP_STEP, TEMP_MAX);
    // The temperature no longer matches the preset that set it.
    panel.selected_preset_ = -1;
    panel.refresh_presets();
    panel.update_temp_display();
    LVGL_SAFE_EVENT_CB_END();
}

void PACalibrationPanel::on_temp_down(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[PACal] on_temp_down");
    auto& panel = get_global_pa_cal_panel();
    panel.target_temp_ = std::max(panel.target_temp_ - TEMP_STEP, TEMP_MIN);
    panel.selected_preset_ = -1;
    panel.refresh_presets();
    panel.update_temp_display();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<PACalibrationPanel> g_pa_cal_panel;

PACalibrationPanel& get_global_pa_cal_panel() {
    if (!g_pa_cal_panel) {
        g_pa_cal_panel = std::make_unique<PACalibrationPanel>();
        StaticPanelRegistry::instance().register_destroy("PACalibrationPanel",
                                                         []() { g_pa_cal_panel.reset(); });
    }
    return *g_pa_cal_panel;
}
} // namespace helix::ui
