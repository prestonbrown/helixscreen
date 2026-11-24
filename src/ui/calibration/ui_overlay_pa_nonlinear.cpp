// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_pa_nonlinear.h"

#include "ui_calibration_time.h"
#include "ui_callback_helpers.h"
#include "ui_component_keypad.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_pa_tower.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "calibration_catalog.h"
#include "config.h"
#include "core/observer_factory.h"
#include "core/run_after.h"
#include "format_utils.h"
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

using helix::pa::kTowerHeightMm;

/// The values column's rows, and the row of the value a pass sweeps.
enum ValueRow : int { kRowOffset = 0, kRowAdvance = 1, kRowTimeOffset = 2, kRowVelocity = 3 };

int row_of(helix::pa::NlParam p) {
    switch (p) {
    case helix::pa::NlParam::Offset:
        return kRowOffset;
    case helix::pa::NlParam::TimeOffset:
        return kRowTimeOffset;
    case helix::pa::NlParam::Advance:
        break;
    }
    return kRowAdvance;
}

/// A value at the precision its sweep moves in.
std::string format_tick(const helix::pa::TowerSweep& sweep, float v) {
    if (sweep.factor >= 0.01f) {
        return fmt::format("{:.2f}", v);
    }
    if (sweep.factor >= 0.001f) {
        return fmt::format("{:.3f}", v);
    }
    return fmt::format("{:.4f}", v);
}

/// The reference samples a pass shows (assets/images/pa/): right, too little, too much.
const char* const* sample_set(const helix::pa::NonlinearPass& pass) {
    static const char* const kLeft[3] = {"A:assets/images/pa/nl1_left_right.png",
                                         "A:assets/images/pa/nl1_left_too_little.png",
                                         "A:assets/images/pa/nl1_left_too_much.png"};
    static const char* const kBoth[3] = {"A:assets/images/pa/nl2_right.png",
                                         "A:assets/images/pa/nl2_too_little.png",
                                         "A:assets/images/pa/nl2_too_much.png"};
    static const char* const kLit[3] = {"A:assets/images/pa/nl3_front_lit_right.png",
                                        "A:assets/images/pa/nl3_front_lit_too_little.png",
                                        "A:assets/images/pa/nl3_front_lit_too_much.png"};
    if (pass.sweep.param == helix::pa::NlParam::TimeOffset) {
        return kLit;
    }
    return pass.judge == helix::pa::TowerSide::Left ? kLeft : kBoth;
}

/// What a pass's user looks at: before the tower prints, and when judging it.
const char* look_text(const helix::pa::NonlinearPass& pass, bool judging) {
    const bool lit = pass.sweep.param == helix::pa::NlParam::TimeOffset;
    switch (pass.judge) {
    case helix::pa::TowerSide::Left:
        return judging
                   ? lv_tr(
                         "Left side: at what height do the edges of the slow middle look cleanest?")
                   : lv_tr("Left side: the two edges of the slow middle, 5 mm either side of "
                           "center.");
    case helix::pa::TowerSide::LeftVsFront:
        return judging
                   ? lv_tr("The height where the left side is cleanest, then where the front is.")
                   : lv_tr(
                         "Left side and front, each at its slow edges. Each comes out clean at its "
                         "own height; they should match.");
    case helix::pa::TowerSide::Front:
        break;
    }
    if (lit) {
        return judging
                   ? lv_tr("Front, lit from far left or right: where do the marked spots look "
                           "cleanest?")
                   : lv_tr(
                         "Front, lit from far left or right: the slow middle, the pause, the slow "
                         "spot. Very subtle.");
    }
    return judging ? lv_tr("Front: where do the marked spots look cleanest?")
                   : lv_tr("Front: the edges of the slow middle, the pause and the slow spot.");
}

/// What pass 2's two heights decide, in words: the next tower's change, or
/// the height taken.
std::string verdict_text(const helix::pa::NonlinearProcedure& proc, float left_mm, float front_mm) {
    using helix::pa::NlParam;
    const auto verdict = helix::pa::verdict_from_heights(left_mm, front_mm);
    const float apart = std::abs(left_mm - front_mm);
    const auto c = proc.correction(verdict);
    if (!c) {
        if (verdict == helix::pa::SideVerdict::Same) {
            return fmt::format(
                fmt::runtime(lv_tr("Left and front agree: {:.1f} mm is the answer.")),
                (left_mm + front_mm) / 2.0f);
        }
        return fmt::format(fmt::runtime(lv_tr("Still {:.1f} mm apart after {} towers: the "
                                              "middle, {:.1f} mm, is taken.")),
                           apart, proc.repeats() + 1, (left_mm + front_mm) / 2.0f);
    }
    const bool lower = verdict == helix::pa::SideVerdict::LeftLower;
    if (c->param == NlParam::Offset) {
        return fmt::format(
            fmt::runtime(
                c->raise
                    ? lv_tr("Left side {:.1f} mm higher: the next tower raises the offset 10%.")
                    : lv_tr("Left side {:.1f} mm lower: the next tower lowers the offset 10%.")),
            apart);
    }
    return fmt::format(
        fmt::runtime(
            lower ? lv_tr("Left side {:.1f} mm lower: the next tower raises the advance 10%.")
                  : lv_tr("Left side {:.1f} mm higher: the next tower lowers the advance 10%.")),
        apart);
}

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
    return nonlinear_param_label(p);
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

const char* nonlinear_param_label(helix::pa::NlParam p) {
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

PaNonlinearOverlay::PaNonlinearOverlay() = default;

PaNonlinearOverlay::~PaNonlinearOverlay() {
    print_state_observer_.reset();
    z_observer_.reset();
    duration_observer_.reset();
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
        UI_MANAGED_SUBJECT_INT(has_tower_, 0, "pa_nl_has_tower", subjects_);
        UI_MANAGED_SUBJECT_INT(busy_, 0, "pa_nl_busy", subjects_);
        UI_MANAGED_SUBJECT_STRING(live_value_, live_value_buf_, "--", "pa_nl_live_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(live_text_, live_text_buf_, "", "pa_nl_live_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(layer_text_, layer_text_buf_, "", "pa_nl_layer_text", subjects_);
        UI_MANAGED_SUBJECT_INT(progress_, 0, "pa_nl_progress", subjects_);
        UI_MANAGED_SUBJECT_STRING(progress_text_, progress_text_buf_, "0%", "pa_nl_progress_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(time_left_, time_left_buf_, "", "pa_nl_time_left", subjects_);
        UI_MANAGED_SUBJECT_INT(two_sided_, 0, "pa_nl_two_sided", subjects_);
        UI_MANAGED_SUBJECT_INT(can_back_, 0, "pa_nl_can_back", subjects_);
        UI_MANAGED_SUBJECT_STRING(height_, height_buf_, "", "pa_nl_height", subjects_);
        UI_MANAGED_SUBJECT_STRING(height_value_, height_value_buf_, "", "pa_nl_height_value",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(error_, error_buf_, "", "pa_nl_error", subjects_);
        UI_MANAGED_SUBJECT_INT(pass_count_, 3, "pa_nl_pass_count", subjects_);
        UI_MANAGED_SUBJECT_INT(pass_index_, 0, "pa_nl_pass_index", subjects_);
        for (int i = 0; i < kPassSlots; ++i) {
            UI_MANAGED_SUBJECT_STRING(pass_label_[i], pass_label_buf_[i], "",
                                      fmt::format("pa_nl_pass_label_{}", i).c_str(), subjects_);
        }
        for (int i = 0; i < kValueRows; ++i) {
            UI_MANAGED_SUBJECT_STRING(value_[i], value_buf_[i], "",
                                      fmt::format("pa_nl_value_{}", i).c_str(), subjects_);
            UI_MANAGED_SUBJECT_STRING(result_[i], result_buf_[i], "",
                                      fmt::format("pa_nl_result_{}", i).c_str(), subjects_);
            UI_MANAGED_SUBJECT_STRING(was_[i], was_buf_[i], "",
                                      fmt::format("pa_nl_was_{}", i).c_str(), subjects_);
        }
        UI_MANAGED_SUBJECT_INT(value_hl_, kRowOffset, "pa_nl_value_hl", subjects_);
        UI_MANAGED_SUBJECT_STRING(look_, look_buf_, "", "pa_nl_look", subjects_);
        for (int i = 0; i < 3; ++i) {
            UI_MANAGED_SUBJECT_POINTER(ref_src_[i], nullptr,
                                       fmt::format("pa_nl_ref_src_{}", i).c_str(), subjects_);
        }
        UI_MANAGED_SUBJECT_STRING(time_line_, time_line_buf_, "", "pa_nl_time_line", subjects_);
        UI_MANAGED_SUBJECT_STRING(time_sub_, time_sub_buf_, "", "pa_nl_time_sub", subjects_);
        UI_MANAGED_SUBJECT_STRING(run_note_, run_note_buf_, "", "pa_nl_run_note", subjects_);
        UI_MANAGED_SUBJECT_STRING(run_chip_, run_chip_buf_, "", "pa_nl_run_chip", subjects_);
        UI_MANAGED_SUBJECT_STRING(ready_primary_, ready_primary_buf_, "", "pa_nl_ready_primary",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(judge_primary_, judge_primary_buf_, "", "pa_nl_judge_primary",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(back_text_, back_text_buf_, "", "pa_nl_back_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(param_, param_buf_, "", "pa_nl_param", subjects_);
        UI_MANAGED_SUBJECT_STRING(height_front_, height_front_buf_, "", "pa_nl_height_front",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(verdict_, verdict_buf_, "", "pa_nl_verdict", subjects_);
        UI_MANAGED_SUBJECT_INT(heights_set_, 1, "pa_nl_heights_set", subjects_);
        UI_MANAGED_SUBJECT_STRING(result_live_, result_live_buf_, "", "pa_nl_result_live",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(save_material_, save_material_buf_, "", "pa_nl_save_material",
                                  subjects_);
    });
}

void PaNonlinearOverlay::deinit_subjects() {
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PaNonlinearOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_pa_nl_stop", [](lv_event_t*) { get_global_pa_nonlinear_overlay().stop_tower(); }},
        {"on_pa_nl_height",
         [](lv_event_t* e) { get_global_pa_nonlinear_overlay().adjust_height(user_int(e)); }},
        {"on_pa_nl_height_edit",
         [](lv_event_t*) { get_global_pa_nonlinear_overlay().edit_height(); }},
        {"on_pa_nl_back", [](lv_event_t*) { get_global_pa_nonlinear_overlay().go_back(); }},
        {"on_pa_nl_judge_primary",
         [](lv_event_t*) { get_global_pa_nonlinear_overlay().judge_primary(); }},
        {"on_pa_nl_ready_primary",
         [](lv_event_t*) { get_global_pa_nonlinear_overlay().ready_primary(); }},
        {"on_pa_nl_height_front",
         [](lv_event_t* e) { get_global_pa_nonlinear_overlay().adjust_front_height(user_int(e)); }},
        {"on_pa_nl_height_front_edit",
         [](lv_event_t*) { get_global_pa_nonlinear_overlay().edit_front_height(); }},
        {"on_pa_nl_tower_tap",
         [](lv_event_t* e) {
             auto* tower = lv_event_get_current_target_obj(e);
             get_global_pa_nonlinear_overlay().tower_tap(pa_tower_tap_face(tower),
                                                         pa_tower_tap_height(tower));
         }},
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
    start_busy_.reset();
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
            // The extruder may still run a saved session's values: its own model
            // is the one recorded before that session, whatever the answer.
            if (const auto* original = json_util::find_object(*saved, "original")) {
                original_ = model_from(*original);
            }
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
    if (s == JUDGE) {
        decision_awake_.acquire();
    } else {
        decision_awake_.reset();
    }
    // Taken by print_pass() while its start script runs; a config write also
    // shows PREPARING and moves nothing.
    if (s != PREPARING) {
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
    const bool has_tower = tower_available();
    lv_subject_set_int(&has_tower_, has_tower ? 1 : 0);
    lv_subject_set_int(&can_back_, proc_->can_go_back() ? 1 : 0);
    lv_subject_set_string(&ready_primary_,
                          has_tower ? lv_tr("Print Tower") : lv_tr("Add Tower to Config"));
    const auto estimate = helix::calibration::firmware_pa_tower_estimate();
    lv_subject_set_string(&time_line_,
                          helix::calibration::time_away_line(helix::calibration::AwayKind::Prints,
                                                             estimate.walk_away_minutes)
                              .c_str());
    lv_subject_set_string(&time_sub_, lv_tr("No need to stay while it prints"));
    lv_subject_set_string(
        &run_note_, lv_tr("No need to stay; this screen waits for you when the tower is done."));
    lv_subject_set_string(&save_material_,
                          fmt::format(fmt::runtime(lv_tr("Save for {}")),
                                      params_.material.empty() ? "PLA" : params_.material)
                              .c_str());

    // The pass bar: each pass's value, the done ones with what they found.
    const auto& v = proc_->values();
    const auto value_of = [&v](helix::pa::NlParam p) {
        switch (p) {
        case helix::pa::NlParam::Offset:
            return v.nonlinear_offset;
        case helix::pa::NlParam::TimeOffset:
            return v.time_offset;
        case helix::pa::NlParam::Advance:
            break;
        }
        return v.linear_advance;
    };
    const int count = std::min(proc_->pass_count(), kPassSlots);
    const int index = proc_->done() ? count : proc_->pass_number() - 1;
    lv_subject_set_int(&pass_count_, count);
    lv_subject_set_int(&pass_index_, index);
    for (int i = 0; i < kPassSlots; ++i) {
        std::string label;
        if (i < count) {
            const auto param = proc_->pass_at(static_cast<size_t>(i)).sweep.param;
            label = fmt::format("{} {}", i + 1, param_label(param));
            if (i < index) {
                label += "  " + format_value(value_of(param));
            }
        }
        lv_subject_set_string(&pass_label_[i], label.c_str());
    }

    // The values column; the swept one as the range its tower covers.
    const float vals[kValueRows] = {v.nonlinear_offset, v.linear_advance, v.time_offset,
                                    v.linearization_velocity};
    for (int i = 0; i < kValueRows; ++i) {
        const std::string text =
            i == kRowVelocity ? fmt::format("{:.1f}", vals[i]) : format_value(vals[i]);
        lv_subject_set_string(&value_[i], text.c_str());
    }
    const float before[kValueRows] = {
        original_.nonlinear_offset.value_or(0.0f),
        original_.linear_advance.value_or(original_.k.value_or(0.0f)),
        original_.time_offset.value_or(0.0f),
        original_.linearization_velocity.value_or(vals[kRowVelocity])};
    for (int i = 0; i < kValueRows; ++i) {
        const bool vel = i == kRowVelocity;
        const auto shown = [vel](float x) {
            return vel ? fmt::format("{:.1f}", x) : format_value(x);
        };
        lv_subject_set_string(&result_[i], shown(vals[i]).c_str());
        lv_subject_set_string(&was_[i],
                              fmt::format(fmt::runtime(lv_tr("was {}")), shown(before[i])).c_str());
    }
    lv_subject_set_string(&result_live_, lv_tr("Live now, until Klipper restarts"));

    if (proc_->done()) {
        lv_subject_set_int(&value_hl_, -1);
        lv_subject_set_string(&pass_title_, lv_tr("All passes done"));
        lv_subject_set_string(&pass_hint_,
                              lv_tr("Save writes these values to the extruder's config section "
                                    "and restarts Klipper. Keep runs them until the next "
                                    "restart."));
        refresh_towers();
        return;
    }
    const auto& pass = proc_->current();
    lv_subject_set_int(&value_hl_, row_of(pass.sweep.param));
    {
        const std::string range = fmt::format(
            "0 to {}", format_tick(pass.sweep, helix::pa::value_at(pass.sweep, kTowerHeightMm)));
        lv_subject_set_string(&value_[row_of(pass.sweep.param)], range.c_str());
    }
    std::string title = fmt::format(fmt::runtime(lv_tr("Pass {} of {}: {}")), proc_->pass_number(),
                                    proc_->pass_count(), param_label(pass.sweep.param));
    if (proc_->repeats() > 0) {
        title += fmt::format(fmt::runtime(lv_tr(" (again, {} of {})")), proc_->repeats() + 1,
                             helix::pa::NonlinearProcedure::kMaxRepeats + 1);
    }
    lv_subject_set_string(&pass_title_, title.c_str());
    lv_subject_set_string(&pass_hint_, look_text(pass, false));
    lv_subject_set_string(&look_, look_text(pass, stage_ == JUDGE));
    lv_subject_set_string(&run_chip_, fmt::format(fmt::runtime(lv_tr("Pass {} of {}")),
                                                  proc_->pass_number(), proc_->pass_count())
                                          .c_str());
    // Back re-opens the latest judgement: this pass's own after a repeat.
    const int back_to = proc_->repeats() > 0 ? proc_->pass_number() : proc_->pass_number() - 1;
    lv_subject_set_string(
        &back_text_, fmt::format(fmt::runtime(lv_tr("Pass {}")), std::max(1, back_to)).c_str());
    const char* const* samples = sample_set(pass);
    for (int i = 0; i < 3; ++i) {
        // Each path is a string literal: the pointer changes with the set, so the image follows.
        lv_subject_set_pointer(&ref_src_[i], const_cast<char*>(samples[i]));
    }

    const bool two = pass.judge == helix::pa::TowerSide::LeftVsFront;
    lv_subject_set_int(&two_sided_, two ? 1 : 0);
    lv_subject_set_string(&param_, param_label(pass.sweep.param));
    const bool both = !two || (left_set_ && front_set_);
    lv_subject_set_int(&heights_set_, both ? 1 : 0);
    lv_subject_set_string(&height_,
                          two && !left_set_ ? "--" : fmt::format("{:.1f} mm", height_mm_).c_str());
    lv_subject_set_string(&height_front_,
                          !front_set_ ? "--" : fmt::format("{:.1f} mm", front_mm_).c_str());
    lv_subject_set_string(&height_value_,
                          format_value(helix::pa::value_at(pass.sweep, height_mm_)).c_str());
    if (two && !both) {
        lv_subject_set_string(&verdict_, "");
        lv_subject_set_string(&judge_primary_, lv_tr("Set both heights"));
    } else if (two) {
        lv_subject_set_string(&verdict_, verdict_text(*proc_, height_mm_, front_mm_).c_str());
        const auto c = proc_->correction(helix::pa::verdict_from_heights(height_mm_, front_mm_));
        lv_subject_set_string(&judge_primary_, c ? lv_tr("Print Again")
                                                 : fmt::format(fmt::runtime(lv_tr("Use {:.1f} mm")),
                                                               (height_mm_ + front_mm_) / 2.0f)
                                                       .c_str());
    } else {
        lv_subject_set_string(&verdict_, "");
        lv_subject_set_string(
            &judge_primary_, fmt::format(fmt::runtime(lv_tr("Use {:.1f} mm")), height_mm_).c_str());
    }
    refresh_towers();
}

void PaNonlinearOverlay::refresh_towers() {
    lv_obj_t* root = overlay_root_;
    if (!root || !proc_) {
        return;
    }
    const bool done = proc_->done();
    const helix::pa::NonlinearPass* pass = done ? nullptr : &proc_->current();
    PaTowerModel base;
    base.banded = false;
    base.height_mm = kTowerHeightMm;
    base.faces = pass ? helix::pa::judged_faces(pass->judge)
                      : std::vector<helix::pa::TowerFace>{helix::pa::TowerFace::Left};
    if (const auto size = get_printer_state().get_discovery().pa_test_size_mm()) {
        base.size_x_mm = size->first;
        base.size_y_mm = size->second;
    }
    if (pass) {
        for (int z = 0; z <= static_cast<int>(kTowerHeightMm); z += 10) {
            base.tick_labels.push_back(
                format_tick(pass->sweep, helix::pa::value_at(pass->sweep, static_cast<float>(z))));
        }
    }
    const bool two = pass && pass->judge == helix::pa::TowerSide::LeftVsFront;
    const auto set = [root](const char* name, const PaTowerModel& m) {
        if (lv_obj_t* t = lv_obj_find_by_name(root, name)) {
            pa_tower_set_model(t, m);
        }
    };

    PaTowerModel ready = base;
    ready.fill = PaTowerFill::Preview;
    set("pa_nl_ready_tower", ready);

    PaTowerModel run = base;
    run.fill = stage_ == PRINTING ? PaTowerFill::Live : PaTowerFill::Outline;
    run.printed_mm = static_cast<float>(lv_subject_get_int(
                         get_printer_state().motion_state().get_gcode_position_z_subject())) /
                     100.0f;
    run.layer_tag = subjects_initialized_ ? lv_subject_get_string(&live_value_) : "";
    set("pa_nl_run_tower", run);

    PaTowerModel judge = base;
    judge.fill = PaTowerFill::Full;
    if (two) {
        if (left_set_) {
            judge.markers.push_back({0, height_mm_, false, "L"});
        }
        if (front_set_) {
            judge.markers.push_back({1, front_mm_, true, "F"});
        }
    } else {
        judge.markers = {{0, height_mm_, false, ""}};
    }
    set("pa_nl_judge_tower", judge);

    // The card's inset: a plain wall, where to look, lit from the side for the time offset.
    PaTowerModel inset = base;
    inset.fill = PaTowerFill::Wall;
    inset.scale = false;
    inset.tick_labels.clear();
    inset.light_right = pass && pass->sweep.param == helix::pa::NlParam::TimeOffset;
    if (two) {
        inset.markers = {{0, kTowerHeightMm * 0.45f, false, "L"},
                         {1, kTowerHeightMm * 0.6f, true, "F"}};
    }
    set("pa_nl_ready_ref_tower", inset);
    set("pa_nl_judge_ref_tower", inset);
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
    time_left_est_.reset();
    script_done_ = false;
    // The tower about to print is judged fresh: a two-sided pass with neither height set.
    left_set_ = front_set_ = proc_->current().judge != helix::pa::TowerSide::LeftVsFront;
    const int pass_seq = ++pass_seq_;
    lv_subject_set_string(&error_, "");
    lv_subject_set_string(&live_text_, lv_tr("Starting the tower"));
    lv_subject_set_string(&live_value_, "--");
    set_stage(PREPARING);
    refresh_towers();
    start_busy_.acquire();
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
    duration_observer_ = helix::ui::observe<int>(
        state.print_state().get_print_duration_subject(), this,
        [](PaNonlinearOverlay* self, int) { self->refresh_live(); }, lifetime);
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

    const auto progress = helix::pa::tower_layer_progress(z);
    lv_subject_set_string(&layer_text_,
                          fmt::format(fmt::runtime(lv_tr("Layer {} of {} · {:.1f} mm")),
                                      progress.layer, progress.layers, z)
                              .c_str());
    const int percent = static_cast<int>(std::lround(progress.fraction * 100.0f));
    lv_subject_set_int(&progress_, percent);
    lv_subject_set_string(&progress_text_, fmt::format("{}%", percent).c_str());
    if (!time_left_est_) {
        const auto estimate = helix::calibration::firmware_pa_tower_estimate();
        time_left_est_.emplace(
            (estimate.walk_away_minutes - helix::calibration::kPaTowerHeatHomeMinutes) * 60);
    }
    time_left_est_->sample(z, lv_subject_get_int(state.print_state().get_print_duration_subject()));
    lv_subject_set_string(&time_left_,
                          helix::format::duration_padded(time_left_est_->seconds_left()).c_str());
    refresh_towers();
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
    if (lv_obj_t* root = overlay_root_) {
        pa_tower_flourish(lv_obj_find_by_name(root, "pa_nl_judge_tower"));
    }
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
    // start_busy_ stays: the heaters are hot and the start macro's moves may
    // still be draining until the tower's job reports printing.
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

/// A step on a height still showing "--" lands on the middle of the tower,
/// where the user can see it; the next step moves from there.
static float stepped_height(bool set, float current_mm, int step_tenths) {
    return set ? current_mm + static_cast<float>(step_tenths) / 10.0f : kTowerHeightMm / 2.0f;
}

void PaNonlinearOverlay::adjust_height(int step_tenths) {
    set_height(stepped_height(left_set_, height_mm_, step_tenths));
}

void PaNonlinearOverlay::set_height(float height_mm) {
    if (stage_ != JUDGE) {
        return;
    }
    // Tenths: the steps and the keypad both land on them, and float sums drift.
    height_mm_ = std::clamp(std::round(height_mm * 10.0f) / 10.0f, 0.0f, kTowerHeightMm);
    left_set_ = true;
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
    const auto reopened = proc_->go_back_judgement();
    if (!reopened) {
        return;
    }
    // The printer runs what the re-opened tower printed with until it is judged
    // again, unless another job holds it.
    if (!job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        send_values();
    }
    save_session();
    set_stage(JUDGE);
    // The judgement comes back as entered: both of a two-sided pass's heights.
    if (reopened->left_mm && reopened->front_mm) {
        height_mm_ = *reopened->left_mm;
        front_mm_ = *reopened->front_mm;
        left_set_ = front_set_ = true;
    } else {
        height_mm_ = reopened->height_mm;
        const bool two = proc_->current().judge == helix::pa::TowerSide::LeftVsFront;
        left_set_ = front_set_ = !two;
    }
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

void PaNonlinearOverlay::judge_primary() {
    if (stage_ != JUDGE || !proc_ || proc_->done()) {
        return;
    }
    if (proc_->current().judge != helix::pa::TowerSide::LeftVsFront) {
        judge(1);
        return;
    }
    // Both faces must have been judged: a height left at its start was never looked at.
    if (!left_set_ || !front_set_) {
        return;
    }
    proc_->record_heights(height_mm_, front_mm_);
    after_judgement();
}

void PaNonlinearOverlay::adjust_front_height(int step_tenths) {
    set_front_height(stepped_height(front_set_, front_mm_, step_tenths));
}

void PaNonlinearOverlay::set_front_height(float height_mm) {
    if (stage_ != JUDGE) {
        return;
    }
    front_mm_ = std::clamp(std::round(height_mm * 10.0f) / 10.0f, 0.0f, kTowerHeightMm);
    front_set_ = true;
    refresh();
}

void PaNonlinearOverlay::edit_front_height() {
    if (stage_ != JUDGE) {
        return;
    }
    ui_keypad_config_t config{};
    config.initial_value = front_mm_;
    config.min_value = 0.0f;
    config.max_value = kTowerHeightMm;
    config.allow_decimal = true;
    config.allow_negative = false;
    config.max_fraction_digits = 1;
    config.title_label = lv_tr("Front best height");
    config.unit_label = "mm";
    config.on_confirm = [this, token = object_lifetime_.token()](float v) {
        if (!token.expired()) {
            set_front_height(v);
        }
    };
    ui_keypad_show(&config);
}

void PaNonlinearOverlay::tower_tap(int face, float height_mm) {
    if (stage_ != JUDGE || !proc_ || proc_->done()) {
        return;
    }
    // On pass 2 the face tapped is the height set: left or front.
    if (proc_->current().judge == helix::pa::TowerSide::LeftVsFront && face == 1) {
        set_front_height(height_mm);
    } else {
        set_height(height_mm);
    }
}

void PaNonlinearOverlay::ready_primary() {
    if (tower_available()) {
        confirm_print();
    } else {
        add_tower_section();
    }
}

void PaNonlinearOverlay::after_judgement() {
    send_values();
    save_session();
    height_mm_ = kTowerHeightMm / 2.0f;
    front_mm_ = kTowerHeightMm / 2.0f;
    // A two-sided pass opens with neither height judged; a one-sided one with
    // the middle of the tower to step from.
    left_set_ = front_set_ =
        proc_->done() || proc_->current().judge != helix::pa::TowerSide::LeftVsFront;
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
    // No answer starts fresh and leaves the saved session to be offered again.
    options.on_dismiss = [this]() {
        spdlog::info("[{}] Resume offer dismissed; starting from pass 1", get_name());
    };
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
