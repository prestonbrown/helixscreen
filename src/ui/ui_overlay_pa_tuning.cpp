// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_pa_tuning.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_panel_print_select.h"
#include "ui_print_start_controller.h"
#include "ui_toast_manager.h"

#include "ams_state.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "macro_param_cache.h"
#include "observer_factory.h"
#include "preset_materials.h"
#include "print_start_analyzer.h"
#include "printer_state.h"
#include "spoolman_types.h"
#include "tool_state.h"
#include "tune_controller.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <set>
#include <string>

namespace helix::ui {

namespace {

/// Ramp endpoint step on the setup screen, per extruder kind.
float ramp_step(helix::pa::ExtruderKind kind) {
    return kind == helix::pa::ExtruderKind::Bowden ? 0.05f : 0.005f;
}

/// Outer clearance the tower needs on each side of its centre: the wall, the
/// brim, and a margin from the bed edge.
constexpr float kBedMarginMm = 10.0f;

std::string utc_now_iso() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

int user_int(lv_event_t* e) {
    return helix::ui::event_user_int(e).value_or(0);
}

} // namespace

PaTuningOverlay::PaTuningOverlay() : session_([this](float k) { send_k(k); }) {}

bool PaTuningOverlay::may_send() const {
    // Outside our own print, a job on the machine is someone else's: K sent
    // now would land in their print.
    return session_.state() == helix::pa::TuningSession::State::Printing ||
           !job_holds_machine(get_printer_state().print_state().get_print_lifecycle());
}

void PaTuningOverlay::send_k(float k) {
    if (!may_send()) {
        spdlog::warn("[{}] Not sending K={:.4f}: another print holds the machine", get_name(), k);
        return;
    }
    helix::tune::set_pressure_advance(api_, extruder_, k);
}

bool PaTuningOverlay::nudge_by(float delta) {
    if (stage_ != RUNNING || !session_.nudge(delta)) {
        return false;
    }
    refresh_running();
    return true;
}

PaTuningOverlay::~PaTuningOverlay() {
    print_state_observer_.reset();
    layer_observer_.reset();
    pa_observer_.reset();
    active_tool_observer_.reset();
    if (!StaticPanelRegistry::is_destroyed()) {
        deinit_subjects();
    }
}

// ============================================================================
// SUBJECTS
// ============================================================================

void PaTuningOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(stage_subject_, SETUP, "pa_tune_stage", subjects_);
        heads_.init_subjects("pa_tune", subjects_);
        for (int i = 0; i < kPresetSlots; ++i) {
            UI_MANAGED_SUBJECT_INT(preset_selected_[i], i == 0 ? 1 : 0,
                                   fmt::format("pa_tune_preset_selected_{}", i).c_str(), subjects_);
            UI_MANAGED_SUBJECT_STRING(preset_temp_[i], preset_temp_buf_[i], "",
                                      fmt::format("pa_tune_preset_temp_{}", i).c_str(), subjects_);
        }
        UI_MANAGED_SUBJECT_INT(kind_subject_, 0, "pa_tune_kind", subjects_);
        UI_MANAGED_SUBJECT_STRING(start_k_, start_k_buf_, "0.0000", "pa_tune_start_k", subjects_);
        UI_MANAGED_SUBJECT_STRING(end_k_, end_k_buf_, "0.1000", "pa_tune_end_k", subjects_);
        UI_MANAGED_SUBJECT_STRING(summary_, summary_buf_, "", "pa_tune_summary", subjects_);
        UI_MANAGED_SUBJECT_INT(can_start_, 1, "pa_tune_can_start", subjects_);

        UI_MANAGED_SUBJECT_STRING(k_big_, k_big_buf_, "--", "pa_tune_k_big", subjects_);
        UI_MANAGED_SUBJECT_STRING(layer_text_, layer_text_buf_, "", "pa_tune_layer_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(mode_text_, mode_text_buf_, "", "pa_tune_mode_text", subjects_);
        UI_MANAGED_SUBJECT_INT(manual_, 0, "pa_tune_manual", subjects_);
        UI_MANAGED_SUBJECT_INT(step_idx_subject_, step_idx_, "pa_tune_step_idx", subjects_);

        UI_MANAGED_SUBJECT_INT(band_count_, 0, "pa_tune_band_count", subjects_);
        for (int i = 0; i < kMaxBands; ++i) {
            UI_MANAGED_SUBJECT_STRING(band_range_[i], band_range_buf_[i], "",
                                      fmt::format("pa_tune_band_range_{}", i).c_str(), subjects_);
            UI_MANAGED_SUBJECT_STRING(band_k_[i], band_k_buf_[i], "",
                                      fmt::format("pa_tune_band_k_{}", i).c_str(), subjects_);
            UI_MANAGED_SUBJECT_INT(band_selected_[i], 0,
                                   fmt::format("pa_tune_band_selected_{}", i).c_str(), subjects_);
        }
        UI_MANAGED_SUBJECT_INT(keep_selected_, 0, "pa_tune_keep_selected", subjects_);
        UI_MANAGED_SUBJECT_STRING(keep_text_, keep_text_buf_, "", "pa_tune_keep_text", subjects_);
        UI_MANAGED_SUBJECT_INT(has_pick_, 0, "pa_tune_has_pick", subjects_);

        UI_MANAGED_SUBJECT_STRING(result_k_, result_k_buf_, "--", "pa_tune_result_k", subjects_);
        UI_MANAGED_SUBJECT_STRING(result_note_, result_note_buf_, "", "pa_tune_result_note",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(spoolman_text_, spoolman_text_buf_, "", "pa_tune_spoolman_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(can_save_spool_, 0, "pa_tune_can_save_spool", subjects_);
        UI_MANAGED_SUBJECT_STRING(error_, error_buf_, "", "pa_tune_error", subjects_);
    });
}

void PaTuningOverlay::deinit_subjects() {
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PaTuningOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_pa_tune_tool",
         [](lv_event_t* e) {
             auto& self = get_global_pa_tuning_overlay();
             if (self.stage_ == SETUP) {
                 self.heads_.select(user_int(e));
             }
         }},
        {"on_pa_tune_preset",
         [](lv_event_t* e) {
             auto& self = get_global_pa_tuning_overlay();
             if (self.stage_ != SETUP) {
                 return;
             }
             const int slot = user_int(e);
             if (const auto temps = helix::presets::slot_temps(slot)) {
                 self.selected_preset_ = slot;
                 self.nozzle_temp_ = temps->nozzle;
                 self.bed_temp_ = temps->bed;
             }
             self.refresh_presets();
             self.refresh_setup();
         }},
        {"on_pa_tune_kind",
         [](lv_event_t* e) {
             auto& self = get_global_pa_tuning_overlay();
             if (self.stage_ == SETUP) {
                 self.set_kind(static_cast<helix::pa::ExtruderKind>(user_int(e) == 1 ? 1 : 0));
             }
         }},
        {"on_pa_tune_start_k",
         [](lv_event_t* e) { get_global_pa_tuning_overlay().adjust_ramp_k(true, user_int(e)); }},
        {"on_pa_tune_end_k",
         [](lv_event_t* e) { get_global_pa_tuning_overlay().adjust_ramp_k(false, user_int(e)); }},
        {"on_pa_tune_start",
         [](lv_event_t*) { get_global_pa_tuning_overlay().confirm_and_start(); }},
        {"on_pa_tune_nudge",
         [](lv_event_t* e) { get_global_pa_tuning_overlay().nudge(user_int(e)); }},
        {"on_pa_tune_step",
         [](lv_event_t* e) {
             auto& self = get_global_pa_tuning_overlay();
             self.step_idx_ = std::clamp(user_int(e), 0, helix::pa::kStepCount - 1);
             lv_subject_set_int(&self.step_idx_subject_, self.step_idx_);
         }},
        {"on_pa_tune_end", [](lv_event_t*) { get_global_pa_tuning_overlay().end_test(); }},
        {"on_pa_tune_band",
         [](lv_event_t* e) { get_global_pa_tuning_overlay().select_band(user_int(e)); }},
        {"on_pa_tune_keep",
         [](lv_event_t*) { get_global_pa_tuning_overlay().select_band(kMaxBands); }},
        {"on_pa_tune_apply", [](lv_event_t*) { get_global_pa_tuning_overlay().apply_pick(); }},
        {"on_pa_tune_save_spool",
         [](lv_event_t*) { get_global_pa_tuning_overlay().save_to_spool(); }},
        {"on_pa_tune_discard",
         [](lv_event_t*) {
             auto& self = get_global_pa_tuning_overlay();
             // abandon() restores the original K through send_k(), which
             // stays silent while a foreign print holds the machine.
             self.session_.abandon();
             self.close_session();
         }},
        {"on_pa_tune_done", [](lv_event_t*) { get_global_pa_tuning_overlay().close_session(); }},
    });
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void PaTuningOverlay::on_activate() {
    OverlayBase::on_activate();
    if (stage_ == SETUP) {
        auto& tools = helix::ToolState::instance();
        active_tool_observer_ = helix::ui::observe<int>(
            tools.get_active_tool_subject(), this,
            [](PaTuningOverlay* self, int) { self->heads_.refresh(); },
            tools.get_subjects_lifetime());
        if (const auto temps = helix::presets::slot_temps(selected_preset_)) {
            nozzle_temp_ = temps->nozzle;
            bed_temp_ = temps->bed;
        }
        heads_.refresh();
        refresh_presets();
        refresh_setup();
    } else if (stage_ == RUNNING || stage_ == STARTING) {
        refresh_running();
    }
    set_stage(stage_);
}

void PaTuningOverlay::cleanup() {
    active_tool_observer_.reset();
    stop_observing();
    OverlayBase::cleanup();
}

bool PaTuningOverlay::session_active() const {
    return stage_ == STARTING || stage_ == RUNNING || stage_ == PICK;
}

void PaTuningOverlay::set_stage(Stage s) {
    stage_ = s;
    if (subjects_initialized_) {
        lv_subject_set_int(&stage_subject_, s);
    }
}

// ============================================================================
// SETUP
// ============================================================================

PaTuningOverlay::ChosenHead PaTuningOverlay::choose_head(const std::vector<PhysicalHead>& heads,
                                                         bool multi_extruder, int selected,
                                                         int active_tool,
                                                         const std::string& active_extruder) {
    ChosenHead out{-1, active_extruder};
    if (heads.empty()) {
        return out;
    }
    const auto& head =
        heads[static_cast<size_t>(std::clamp(selected, 0, static_cast<int>(heads.size()) - 1))];
    if (head.extruder_name) {
        out.extruder = *head.extruder_name;
    }
    // Only several physical extruders make a tool worth selecting: lanes into
    // one hotend are a filament choice, and selecting one is a lane change.
    if (multi_extruder) {
        out.tool = head.representative_tool(active_tool);
    }
    return out;
}

PaTuningOverlay::ChosenHead PaTuningOverlay::chosen_head() const {
    auto& tools = helix::ToolState::instance();
    return choose_head(tools.heated_heads(), tools.has_multiple_extruders(), heads_.selected(),
                       tools.active_tool_index(),
                       get_printer_state().temperature_state().active_extruder_name());
}

void PaTuningOverlay::refresh_presets() {
    for (int i = 0; i < kPresetSlots; ++i) {
        const auto temps = helix::presets::slot_temps(i);
        lv_subject_copy_string(&preset_temp_[i],
                               temps ? fmt::format("{}° / {}°", temps->nozzle, temps->bed).c_str()
                                     : "");
        lv_subject_set_int(&preset_selected_[i], i == selected_preset_ ? 1 : 0);
    }
}

void PaTuningOverlay::set_kind(helix::pa::ExtruderKind kind) {
    kind_ = kind;
    ramp_ = helix::pa::default_ramp(kind);
    lv_subject_set_int(&kind_subject_, static_cast<int>(kind));
    refresh_setup();
}

void PaTuningOverlay::adjust_ramp_k(bool start, int direction) {
    if (stage_ != SETUP || direction == 0) {
        return;
    }
    const float delta = ramp_step(kind_) * (direction > 0 ? 1.0f : -1.0f);
    float& k = start ? ramp_.start_k : ramp_.end_k;
    k = helix::pa::clamp_k(std::round((k + delta) * 10000.0f) / 10000.0f);
    refresh_setup();
}

helix::pa::TestPrintParams PaTuningOverlay::build_params() const {
    helix::pa::TestPrintParams p;
    p.bands = ramp_.bands;
    p.nozzle_temp = nozzle_temp_;
    p.bed_temp = bed_temp_;
    p.material = helix::presets::name(selected_preset_);
    if (const auto mat = filament::find_material(p.material)) {
        // An enclosure material warps under a full part-cooling fan.
        p.fan_percent = mat->needs_enclosure() ? 20 : 100;
    }
    auto& state = get_printer_state();
    p.filament_diameter_mm = state.get_discovery().filament_diameter_mm();
    p.nozzle_diameter_mm = state.get_discovery().nozzle_diameter_mm();
    p.macros = machine_macros();
    if (const auto area = printable_area()) {
        p.bed_x_min = area->x_min;
        p.bed_x_max = area->x_max;
        p.bed_y_min = area->y_min;
        p.bed_y_max = area->y_max;
    } else {
        p.bed_x_min = p.bed_x_max = p.bed_y_min = p.bed_y_max = 0.0f;
    }
    const ChosenHead head = chosen_head();
    p.tool = head.tool;
    p.heater = head.extruder;
    p.restore_k = state.temperature_state().extruder_pressure_advance(head.extruder);
    return p;
}

helix::pa::MachineMacros PaTuningOverlay::machine_macros() {
    const auto& cache = MacroParamCache::instance();
    const std::vector<std::string> starts(std::begin(PrintStartAnalyzer::kMacroNames),
                                          std::end(PrintStartAnalyzer::kMacroNames));
    const std::vector<std::string> ends(std::begin(PrintStartAnalyzer::kEndMacroNames),
                                        std::end(PrintStartAnalyzer::kEndMacroNames));
    return helix::pa::pick_machine_macros(
        starts, ends, [&cache](const std::string& name) -> std::optional<std::vector<std::string>> {
            if (!cache.has_macro(name)) {
                return std::nullopt;
            }
            std::vector<std::string> params;
            for (const auto& param : cache.get(name).params) {
                params.push_back(param.name);
            }
            return params;
        });
}

std::optional<helix::pa::PrintArea> PaTuningOverlay::printable_area() {
    auto& state = get_printer_state();
    const AxisBounds machine = state.motion_state().get_axis_bounds();
    const AxisBounds gcode = state.motion_state().get_gcode_axis_bounds();
    helix::pa::AreaSources src;
    src.has_axes = gcode.has_x && gcode.has_y;
    src.axis_x_min = gcode.x_min;
    src.axis_x_max = gcode.x_max;
    src.axis_y_min = gcode.y_min;
    src.axis_y_max = gcode.y_max;
    const auto& discovery = state.get_discovery();
    src.round_bed = circular_bed_kinematics(discovery.kinematics());
    const BuildVolume& volume = discovery.build_volume();
    if (volume.plate_x_max > volume.plate_x_min && volume.plate_y_max > volume.plate_y_min) {
        // The plate is declared in machine space; shift it by the same offset
        // that turns the machine axes into gcode ones.
        const float dx = gcode.x_min - machine.x_min;
        const float dy = gcode.y_min - machine.y_min;
        src.has_plate = true;
        src.plate_x_min = volume.plate_x_min + dx;
        src.plate_x_max = volume.plate_x_max + dx;
        src.plate_y_min = volume.plate_y_min + dy;
        src.plate_y_max = volume.plate_y_max + dy;
    }
    return helix::pa::printable_area(src);
}

void PaTuningOverlay::refresh_setup() {
    lv_subject_copy_string(&start_k_, helix::pa::format_k(ramp_.start_k).c_str());
    lv_subject_copy_string(&end_k_, helix::pa::format_k(ramp_.end_k).c_str());

    const auto params = build_params();
    const int layers = helix::pa::layer_count(params.geometry, params.bands);
    const int minutes = std::max(1, helix::pa::estimate_print_seconds(params) / 60);
    const std::string summary = fmt::format(
        fmt::runtime(lv_tr("Test tower {:.0f} mm wide and {:.0f} mm tall, about {} min plus "
                           "heating: {} bands of {:.0f} mm at {}°C.")),
        params.geometry.size_mm, helix::pa::layer_z(params.geometry, layers), minutes, ramp_.bands,
        params.geometry.band_mm, nozzle_temp_);
    const auto macros = params.macros;
    const std::string starts =
        macros.start.empty()
            ? std::string(lv_tr("It homes and heats on its own (no start macro found)."))
            : fmt::format(fmt::runtime(lv_tr("It starts with your {} macro.")), macros.start);
    lv_subject_copy_string(&summary_, fmt::format("{} {}", summary, starts).c_str());

    auto& state = get_printer_state();
    const bool busy = job_holds_machine(state.print_state().get_print_lifecycle());
    lv_subject_set_int(&can_start_, (!busy && ramp_.end_k > ramp_.start_k) ? 1 : 0);
}

ConfirmOptions PaTuningOverlay::owned_confirm() {
    ConfirmOptions options;
    options.owner_token = object_lifetime_.token();
    return options;
}

void PaTuningOverlay::confirm_and_start() {
    if (stage_ != SETUP) {
        return;
    }
    if (job_holds_machine(get_printer_state().print_state().get_print_lifecycle())) {
        fail_start(lv_tr("A print is already running."));
        return;
    }
    if (refuse_blocked_nozzle()) {
        return;
    }
    const auto params = build_params();
    const std::string msg = fmt::format(
        "{}\n\n{}",
        fmt::format(fmt::runtime(lv_tr("Clear the plate and load {}. The tower prints in the "
                                       "middle of the bed.")),
                    params.material),
        lv_tr("Pressure advance climbs as the tower grows. Watch the corners: tap - or + "
              "whenever you like and the ramp stops so you can judge your own value."));
    modal_confirm(
        lv_tr("Print the test tower?"), msg.c_str(), ModalSeverity::Warning, lv_tr("Start"),
        []() { get_global_pa_tuning_overlay().begin_upload(); }, owned_confirm());
}

void PaTuningOverlay::fail_start(const std::string& message) {
    if (!message.empty()) {
        spdlog::warn("[{}] {}", get_name(), message);
    }
    lv_subject_copy_string(&error_, message.c_str());
    set_stage(SETUP);
    refresh_setup();
}

bool PaTuningOverlay::refuse_blocked_nozzle() {
    // The tower heats its extruder by name, which no printer-side M104 guard
    // sees, so the filament system's refusal is checked here.
    const std::string blocked = AmsState::instance().nozzle_heat_blocked_reason();
    if (blocked.empty()) {
        return false;
    }
    fail_start(blocked);
    return true;
}

void PaTuningOverlay::begin_upload() {
    if (!api_) {
        fail_start(lv_tr("No connection to the printer."));
        return;
    }
    if (refuse_blocked_nozzle()) {
        return;
    }
    const auto params = build_params();
    const float size_needed = params.geometry.size_mm + 2.0f * kBedMarginMm;
    if (params.bed_x_max - params.bed_x_min < size_needed ||
        params.bed_y_max - params.bed_y_min < size_needed) {
        fail_start(lv_tr("The bed is too small for the test tower, or its size is unknown."));
        return;
    }

    extruder_ = params.heater;
    tuned_tool_ = params.tool;
    session_.start(params.geometry, ramp_, params.restore_k);
    saw_printing_ = false;
    picked_ = -1;
    lv_subject_copy_string(&error_, "");
    set_stage(STARTING);
    refresh_running();

    const std::string gcode = helix::pa::generate_test_gcode(params);
    spdlog::info("[{}] Uploading {} ({} bytes) for {}, K {} -> {}", get_name(), TEST_FILENAME,
                 gcode.size(), extruder_, helix::pa::format_k(ramp_.start_k),
                 helix::pa::format_k(ramp_.end_k));
    api_->transfers().upload_file(
        "gcodes", TEST_FILENAME, gcode,
        object_lifetime_.bg_cb("PaTuningOverlay::uploaded",
                               [this]() {
                                   // Observe before starting, so the first
                                   // "printing" report cannot slip past.
                                   begin_observing();
                                   start_uploaded_print();
                               }),
        object_lifetime_.bg_cb("PaTuningOverlay::upload_failed", [this](const MoonrakerError& err) {
            session_.abandon();
            fail_start(err.message);
        }));
}

std::map<int, int> PaTuningOverlay::tower_routing(int tuned_tool) {
    // The tower's tool prints on its own head: the one heated and tuned. A
    // previous job's crossover must not carry over to it.
    const int tool = std::max(0, tuned_tool);
    return {{tool, tool}};
}

void PaTuningOverlay::start_uploaded_print() {
    // The controller's failure path already shows its own error, so the setup
    // screen only returns; the raw fallback has no other message and says one.
    auto on_failed = [this](const std::string& message) {
        stop_observing();
        session_.abandon();
        delete_tower_file();
        fail_start(message);
    };
    // The reprint path: a file already on the printer gets the same pre-start
    // send (a toolchanger's extruder map) as any other start, with the
    // tower's own routing rather than the last job's.
    auto* select_panel = get_print_select_panel(get_printer_state(), api_);
    if (auto* controller = select_panel ? select_panel->get_print_start_controller() : nullptr) {
        const int tool = std::max(0, tuned_tool_);
        auto tok = object_lifetime_.token();
        controller->initiate_reprint_with_routing(
            TEST_FILENAME, std::set<int>{tool}, tower_routing(tuned_tool_), []() {},
            [tok, on_failed]() {
                tok.defer("PaTuningOverlay::start_failed", [on_failed]() { on_failed({}); });
            });
        return;
    }
    api_->job().start_print(
        TEST_FILENAME, []() {},
        object_lifetime_.bg_cb("PaTuningOverlay::start_failed",
                               [on_failed](const MoonrakerError& err) { on_failed(err.message); }));
}

void PaTuningOverlay::delete_tower_file() {
    if (!api_) {
        return;
    }
    // A tower reprinted from history would restore the K this run started
    // from, long after it stopped being the original.
    api_->files().delete_file(
        std::string("gcodes/") + TEST_FILENAME, []() {},
        [](const MoonrakerError& err) {
            spdlog::warn("[PA Tuning] Could not delete {}: {}", TEST_FILENAME, err.message);
        });
}

// ============================================================================
// RUN
// ============================================================================

void PaTuningOverlay::begin_observing() {
    auto& state = get_printer_state();
    const auto lifetime = state.get_subjects_lifetime();
    pa_observer_ = helix::ui::observe<int>(
        state.temperature_state().get_active_extruder_pressure_advance_subject(), this,
        [](PaTuningOverlay* self, int value) { self->on_pa_reported(value); }, lifetime);
    layer_observer_ = helix::ui::observe<int>(
        state.print_state().get_print_layer_current_subject(), this,
        [](PaTuningOverlay* self, int layer) { self->on_layer(layer); }, lifetime);
    // RAW_PRINT_STATE_OK: the run follows the printer's own job for the uploaded
    // file, from the edge Klipper accepts it to the edge it ends, error included.
    print_state_observer_ = helix::ui::observe_print_state(
        state.print_state().get_print_state_enum_subject(), this,
        [](PaTuningOverlay* self, PrintJobState s) { self->on_print_state(s); }, lifetime);
}

void PaTuningOverlay::stop_observing() {
    print_state_observer_.reset();
    layer_observer_.reset();
    pa_observer_.reset();
}

void PaTuningOverlay::on_print_state(PrintJobState state) {
    const std::string& file = get_printer_state().print_state().get_effective_print_filename();
    const bool ours = file.find(TEST_FILENAME) != std::string::npos;

    if (printer_has_job(state)) {
        if (!ours || saw_printing_) {
            return;
        }
        saw_printing_ = true;
        session_.on_print_started();
        set_stage(RUNNING);
        refresh_running();
        // Print Status opens itself on every print start; the tuner is what
        // this print is for, so it goes back on top once that push has run.
        if (raise_on_start_) {
            object_lifetime_.defer("PaTuningOverlay::raise", [this]() { raise(kRaiseAttempts); });
        }
        return;
    }
    // A finished state from an earlier print is still showing until ours
    // starts; only an end after our own start means anything.
    if (!saw_printing_) {
        return;
    }
    session_.on_print_ended();
    stop_observing();
    delete_tower_file();
    if (session_.state() == helix::pa::TuningSession::State::Picking) {
        picked_ = -1;
        refresh_bands();
        set_stage(PICK);
        spdlog::info("[{}] Test print ended; {} bands to pick from", get_name(),
                     session_.bands().size());
    } else {
        fail_start(lv_tr("The print ended before any layer was printed. Nothing was changed."));
    }
}

void PaTuningOverlay::raise(int attempts_left) {
    lv_obj_t* root = get_root();
    if (root && helix::nav::is_on_top(root)) {
        return;
    }
    if (root && helix::nav::is_in_stack(root)) {
        // Buried under Print Status: drop it from the stack, then push it
        // again once the navigation manager has let go of it.
        close();
        if (attempts_left > 0) {
            object_lifetime_.defer("PaTuningOverlay::raise",
                                   [this, attempts_left]() { raise(attempts_left - 1); });
        }
        return;
    }
    show(lv_display_get_screen_active(nullptr));
}

void PaTuningOverlay::on_layer(int layer) {
    if (!saw_printing_) {
        return;
    }
    session_.on_layer(layer);
    refresh_running();
}

void PaTuningOverlay::on_pa_reported(int subject_value) {
    // The active extruder's report; during the run that is the tuned one, but a
    // per-extruder reading is the more direct witness when it exists.
    auto k = get_printer_state().temperature_state().extruder_pressure_advance(extruder_);
    if (!k) {
        k = helix::tune::pressure_advance_from_subject(subject_value);
    }
    if (!k) {
        return;
    }
    session_.on_k_reported(*k);
    refresh_running();
}

void PaTuningOverlay::nudge(int direction) {
    if (direction == 0) {
        return;
    }
    const float step = helix::pa::kStepSizes[std::clamp(step_idx_, 0, helix::pa::kStepCount - 1)];
    nudge_by(direction > 0 ? step : -step);
}

void PaTuningOverlay::end_test() {
    if (stage_ != RUNNING && stage_ != STARTING) {
        return;
    }
    modal_confirm(
        lv_tr("End the test print?"),
        lv_tr("The print is canceled and you pick the best band from what has printed so far."),
        ModalSeverity::Warning, lv_tr("End Test"),
        []() {
            auto& self = get_global_pa_tuning_overlay();
            if (!self.api_) {
                return;
            }
            self.api_->job().cancel_print([]() {},
                                          [](const MoonrakerError& err) {
                                              spdlog::warn("[PA Tuning] Cancel failed: {}",
                                                           err.message);
                                          });
        },
        owned_confirm());
}

void PaTuningOverlay::refresh_running() {
    if (!subjects_initialized_) {
        return;
    }
    const auto& g = session_.geometry();
    lv_subject_copy_string(&k_big_, helix::pa::format_k(session_.current_k()).c_str());

    const int layer = session_.layer();
    const int total = helix::pa::layer_count(g, session_.ramp().bands);
    if (layer > 0) {
        lv_subject_copy_string(&layer_text_,
                               fmt::format(fmt::runtime(lv_tr("Layer {} of {} · {:.2f} mm")), layer,
                                           total, helix::pa::layer_z(g, layer))
                                   .c_str());
    } else {
        lv_subject_copy_string(&layer_text_, lv_tr("Heating and homing"));
    }

    const bool manual = session_.mode() == helix::pa::TuningSession::Mode::Manual;
    lv_subject_set_int(&manual_, manual ? 1 : 0);
    std::string mode;
    if (manual) {
        mode = lv_tr("Manual: the ramp has stopped at your value");
    } else {
        const int band = helix::pa::band_for_layer(g, session_.ramp().bands, std::max(1, layer));
        mode = band < 0 ? std::string(lv_tr("Ramp: base layers"))
                        : fmt::format(fmt::runtime(lv_tr("Ramp: band {} of {}")), band + 1,
                                      session_.ramp().bands);
    }
    lv_subject_copy_string(&mode_text_, mode.c_str());
    refresh_bands(/*newest_first=*/true);
}

// ============================================================================
// PICK / RESULT
// ============================================================================

void PaTuningOverlay::refresh_bands(bool newest_first) {
    auto bands = session_.bands(kMaxBands);
    if (newest_first) {
        std::reverse(bands.begin(), bands.end());
    }
    const int n = std::min(static_cast<int>(bands.size()), kMaxBands);
    for (int i = 0; i < kMaxBands; ++i) {
        if (i < n) {
            const auto& b = bands[static_cast<size_t>(i)];
            lv_subject_copy_string(&band_range_[i],
                                   fmt::format("{:.1f} - {:.1f} mm", b.z_low, b.z_high).c_str());
            lv_subject_copy_string(&band_k_[i], helix::pa::format_k(b.k).c_str());
        }
        lv_subject_set_int(&band_selected_[i], i == picked_ ? 1 : 0);
    }
    lv_subject_set_int(&band_count_, n);
    lv_subject_set_int(&keep_selected_, picked_ == kMaxBands ? 1 : 0);
    lv_subject_copy_string(&keep_text_, fmt::format(fmt::runtime(lv_tr("Keep current ({})")),
                                                    helix::pa::format_k(session_.current_k()))
                                            .c_str());
    lv_subject_set_int(&has_pick_, picked_ >= 0 ? 1 : 0);
}

void PaTuningOverlay::select_band(int index) {
    if (stage_ != PICK) {
        return;
    }
    const int n = lv_subject_get_int(&band_count_);
    if (index != kMaxBands && (index < 0 || index >= n)) {
        return;
    }
    picked_ = index;
    for (int i = 0; i < kMaxBands; ++i) {
        lv_subject_set_int(&band_selected_[i], i == picked_ ? 1 : 0);
    }
    lv_subject_set_int(&keep_selected_, picked_ == kMaxBands ? 1 : 0);
    lv_subject_set_int(&has_pick_, 1);
}

void PaTuningOverlay::apply_pick() {
    if (stage_ != PICK || picked_ < 0) {
        return;
    }
    if (!may_send()) {
        NOTIFY_WARNING(lv_tr("A print is running. Apply the result after it ends."));
        return;
    }
    if (picked_ == kMaxBands) {
        session_.keep_current();
    } else if (!session_.pick_band(picked_, kMaxBands)) {
        return;
    }
    show_result();
}

void PaTuningOverlay::show_result() {
    const auto k = session_.result();
    if (!k) {
        return;
    }
    const std::string text = helix::pa::format_k(*k);
    lv_subject_copy_string(&result_k_, text.c_str());
    lv_subject_copy_string(
        &result_note_,
        fmt::format(fmt::runtime(lv_tr("Applied to {}. Klipper forgets it on restart: enter "
                                       "{} as Pressure advance in your slicer's filament "
                                       "profile, or save it to the spool.")),
                    extruder_label(), text)
            .c_str());
    lv_subject_copy_string(&spoolman_text_, lv_tr("Looking for the active spool..."));
    lv_subject_set_int(&can_save_spool_, 0);
    set_stage(RESULT);
    active_spool_.reset();

    if (!api_) {
        publish_to_moonraker(*k);
        return;
    }
    // The tuned tool's own spool when the printer assigns spools per tool;
    // Spoolman's single active spool otherwise.
    const auto& tools = helix::ToolState::instance().tools();
    if (tuned_tool_ >= 0 && tuned_tool_ < static_cast<int>(tools.size()) &&
        tools[static_cast<size_t>(tuned_tool_)].spoolman_id > 0) {
        fetch_result_spool(tools[static_cast<size_t>(tuned_tool_)].spoolman_id);
        return;
    }
    api_->spoolman().get_spoolman_status(
        object_lifetime_.bg_cb("PaTuningOverlay::spoolman_status",
                               [this](bool connected, int spool_id) {
                                   if (!connected || spool_id <= 0) {
                                       lv_subject_copy_string(&spoolman_text_,
                                                              lv_tr("No active Spoolman spool."));
                                       publish_to_moonraker(session_.result().value_or(0.0f));
                                       return;
                                   }
                                   fetch_result_spool(spool_id);
                               }),
        object_lifetime_.bg_cb(
            "PaTuningOverlay::spoolman_status_failed", [this](const MoonrakerError&) {
                lv_subject_copy_string(&spoolman_text_, lv_tr("Spoolman is not available."));
                publish_to_moonraker(session_.result().value_or(0.0f));
            }));
}

void PaTuningOverlay::fetch_result_spool(int spool_id) {
    api_->spoolman().get_spoolman_spool(
        spool_id,
        object_lifetime_.bg_cb(
            "PaTuningOverlay::result_spool",
            [this](const std::optional<SpoolInfo>& spool) {
                active_spool_ = spool;
                if (spool && spool->filament_id > 0) {
                    lv_subject_copy_string(
                        &spoolman_text_,
                        fmt::format(fmt::runtime(lv_tr("Active spool: {}")), spool->display_name())
                            .c_str());
                    lv_subject_set_int(&can_save_spool_, 1);
                } else {
                    lv_subject_copy_string(&spoolman_text_, lv_tr("No active Spoolman spool."));
                }
                publish_to_moonraker(session_.result().value_or(0.0f));
            }),
        object_lifetime_.bg_cb(
            "PaTuningOverlay::result_spool_failed", [this](const MoonrakerError&) {
                lv_subject_copy_string(&spoolman_text_, lv_tr("No active Spoolman spool."));
                publish_to_moonraker(session_.result().value_or(0.0f));
            }));
}

std::string PaTuningOverlay::extruder_label() const {
    const std::string label = helix::ToolState::instance().display_label_for_extruder(extruder_);
    return label.empty() ? std::string(lv_tr("the extruder")) : label;
}

void PaTuningOverlay::save_to_spool() {
    const auto k = session_.result();
    if (stage_ != RESULT || !k || !api_ || !active_spool_ || active_spool_->filament_id <= 0) {
        return;
    }
    lv_subject_set_int(&can_save_spool_, 0);
    const int filament_id = active_spool_->filament_id;
    const std::string name = active_spool_->display_name();
    api_->spoolman().set_spoolman_filament_extra_number(
        filament_id, kSpoolmanPaExtraKey, lv_tr("Pressure advance"), *k,
        object_lifetime_.bg_cb(
            "PaTuningOverlay::spool_saved",
            [this, name]() {
                lv_subject_copy_string(
                    &spoolman_text_,
                    fmt::format(fmt::runtime(lv_tr("Saved to {}. It applies whenever this "
                                                   "filament becomes active.")),
                                name)
                        .c_str());
            }),
        object_lifetime_.bg_cb(
            "PaTuningOverlay::spool_save_failed", [this](const MoonrakerError& err) {
                lv_subject_set_int(&can_save_spool_, 1);
                helix::ui::notify_error_tr(TR_NOOP("Failed to save to Spoolman: {}"), err);
            }));
}

void PaTuningOverlay::publish_to_moonraker(float k) {
    if (!api_) {
        return;
    }
    helix::pa::SavedK saved;
    saved.k = k;
    saved.extruder = extruder_;
    saved.material = helix::presets::name(selected_preset_);
    saved.nozzle_temp = nozzle_temp_;
    saved.updated = utc_now_iso();
    if (active_spool_) {
        saved.spool_id = active_spool_->id;
        saved.filament_id = active_spool_->filament_id;
        saved.filament_name = active_spool_->display_name();
    }
    // Read-modify-write so results for other filaments and extruders survive.
    auto post = [api = api_, saved](const nlohmann::json& existing) {
        api->database_post_item(
            helix::pa::kMoonrakerNamespace, helix::pa::kMoonrakerKey,
            helix::pa::merge_moonraker_record(existing, saved),
            []() { spdlog::debug("[PA Tuning] Result published to Moonraker"); },
            [](const MoonrakerError& err) {
                spdlog::warn("[PA Tuning] Could not publish to Moonraker: {}", err.message);
            });
    };
    api_->database_get_item(helix::pa::kMoonrakerNamespace, helix::pa::kMoonrakerKey, post,
                            [post](const MoonrakerError&) { post(nullptr); });
}

void PaTuningOverlay::close_session() {
    stop_observing();
    picked_ = -1;
    set_stage(SETUP);
    close();
}

} // namespace helix::ui
