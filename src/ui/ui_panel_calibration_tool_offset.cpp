// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_calibration_tool_offset.h"

#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "format_utils.h"
#include "i_moonraker_api.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_param_cache.h"
#include "observer_factory.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "tool_offsets.h"
#include "tool_state.h"
#include "ui/ui_lazy_panel_helper.h"
#include "z_offset_utils.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

namespace helix::ui {

namespace cal = helix::tool_offset_calibration;

namespace {

/// tool_cal_state_N carries the ToolStep enum verbatim; the XML's ref_values
/// are these numbers.
int step_value(cal::ToolStep step) {
    return static_cast<int>(step);
}

std::unique_ptr<ToolOffsetCalibrationPanel> g_panel;

} // namespace

ToolOffsetCalibrationPanel& get_global_tool_offset_cal_panel() {
    if (!g_panel) {
        g_panel = std::make_unique<ToolOffsetCalibrationPanel>();
        StaticPanelRegistry::instance().register_destroy("ToolOffsetCalibrationPanel",
                                                         []() { g_panel.reset(); });
    }
    return *g_panel;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

ToolOffsetCalibrationPanel::ToolOffsetCalibrationPanel() {
    spdlog::trace("[ToolOffsetCal] Instance created");
}

ToolOffsetCalibrationPanel::~ToolOffsetCalibrationPanel() {
    // The row timer is cancelled on every normal path; a teardown that
    // destroys the panel mid-run skips them, and StaticPanelRegistry runs
    // before lv_deinit() (#1173). ElapsedLabelTimer cancels itself on
    // destruction.
    idle_wait_observer_.reset();
    active_tool_observer_.reset();
    tools_observer_.reset();
    subjects_.deinit_all();
    subjects_initialized_ = false;
    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[ToolOffsetCal] Destroyed");
    }
}

void ToolOffsetCalibrationPanel::init_subjects() {
    if (subjects_initialized_) {
        return;
    }
    UI_MANAGED_SUBJECT_STRING(status_, status_buffer_, "", "tool_cal_status", subjects_);
    UI_MANAGED_SUBJECT_STRING(hint_, hint_buffer_, "", "tool_cal_hint", subjects_);
    UI_MANAGED_SUBJECT_INT(active_, 0, "tool_cal_active", subjects_);
    // The per-row subjects live in pools sized by refresh_rows(); only the
    // row count is a fixed subject.
    UI_MANAGED_SUBJECT_INT(tool_count_, 0, "tool_cal_tool_count", subjects_);

    static const std::pair<const char*, lv_event_cb_t> callbacks[] = {
        {"on_tool_cal_start", on_start_clicked},
        {"on_tool_cal_stop", on_stop_clicked},
        {"on_tool_cal_save", on_save_clicked},
    };
    for (const auto& [name, cb] : callbacks) {
        lv_xml_register_event_cb(nullptr, name, cb);
    }

    subjects_initialized_ = true;
    spdlog::debug("[ToolOffsetCal] Subjects initialized");
}

lv_obj_t* ToolOffsetCalibrationPanel::create(lv_obj_t* parent) {
    if (overlay_root_) {
        return overlay_root_;
    }
    parent_screen_ = parent;
    // Start the <repeat> at zero rows: on_ui_destroyed() reclaimed the pools,
    // so a stale count would build rows bound to unregistered subjects.
    // refresh_rows() below sizes the pools and sets the real count.
    lv_subject_set_int(&tool_count_, 0);
    if (!create_overlay_from_xml(parent, "calibration_tool_offset_panel")) {
        spdlog::error("[ToolOffsetCal] Failed to create overlay from XML");
        return nullptr;
    }
    // Build the rows now: a hot-reload rebuild while hidden does not re-run
    // on_activate(), and this is what the macros panel does in its create().
    refresh_rows();
    return overlay_root_;
}

void ToolOffsetCalibrationPanel::on_ui_destroyed() {
    // The rows are gone; reclaim their name-registered subjects while LVGL is
    // still live, as the macros panel does for its list.
    row_state_.reclaim();
    row_state_text_.reclaim();
    row_x_.reclaim();
    row_y_.reclaim();
    row_z_.reclaim();
    lv_subject_set_int(&tool_count_, 0);
}

void ToolOffsetCalibrationPanel::on_activate() {
    OverlayBase::on_activate();

    auto& tools = helix::ToolState::instance();
    // The values on the rows ARE ToolState's offsets; follow them so a run's
    // SET_TOOL_PARAMETER writes (and anything else that moves an offset) show
    // up without a refresh of our own. The lifetime token matters: without it
    // the guard never learns the subject died (#705).
    tools_observer_ = helix::ui::observe_int_sync<ToolOffsetCalibrationPanel>(
        tools.get_tools_version_subject(), this,
        [](ToolOffsetCalibrationPanel* self, int /*version*/) { self->on_tools_changed(); },
        tools.get_subjects_lifetime());
    active_tool_observer_ = helix::ui::observe_int_sync<ToolOffsetCalibrationPanel>(
        tools.get_active_tool_subject(), this,
        [](ToolOffsetCalibrationPanel* self, int tool) { self->on_active_tool_changed(tool); },
        tools.get_subjects_lifetime());

    // The subject follows the run, never the other way round: a run that
    // ended while the panel was away has already cleared it, and one that
    // could not start never set it.
    lv_subject_set_int(&active_, run_.active() ? 1 : 0);
    if (!run_.active()) {
        lv_subject_copy_string(&status_, last_error_.empty() ? lv_tr("Ready to calibrate")
                                                             : last_error_.c_str());
    }
    // The macro's own description: is the instruction text, read from the
    // cache discovery filled from configfile.config - as the macros panel
    // reads every description. Without one, say only what is true of every
    // implementation: how it heats, probes and which tool it measures
    // against is the macro's business, not this screen's.
    const std::string desc = helix::MacroParamCache::instance().get(cal::kMacro).description;
    lv_subject_copy_string(&hint_, desc.empty()
                                       ? lv_tr("Runs the printer's tool offset calibration "
                                               "for every tool.")
                                       : desc.c_str());
    refresh_rows();
}

void ToolOffsetCalibrationPanel::on_deactivating(DeactivateReason reason) {
    // A run keeps going on the printer whether or not the panel is on screen.
    // The observers stay with it: ToolState keeps publishing, and the rows
    // must be right when the panel comes back. So does the run's completion:
    // the base expires lifetime_ right after this hook returns, which is why
    // the run and save callbacks ride on run_lifetime_ instead.
    spdlog::debug("[ToolOffsetCal] on_deactivating({})", deactivate_reason_name(reason));
}

void ToolOffsetCalibrationPanel::cleanup() {
    run_lifetime_.invalidate();
    finish_idle_wait();
    active_tool_observer_.reset();
    tools_observer_.reset();
    elapsed_.cancel();
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }
    OverlayBase::cleanup();
    parent_screen_ = nullptr;
}

// ============================================================================
// ROWS
// ============================================================================

void ToolOffsetCalibrationPanel::refresh_rows() {
    if (!subjects_initialized_) {
        return;
    }
    const size_t count = helix::ToolState::instance().tools().size();
    // Pools first, count last: the <repeat> rebuilds on the count and binds
    // each new row to tool_cal_*_<i> by name, so every slot must exist and
    // hold its value before the rows are built.
    row_state_.ensure_size(count);
    row_state_text_.ensure_size(count);
    row_x_.ensure_size(count);
    row_y_.ensure_size(count);
    row_z_.ensure_size(count);
    for (size_t i = 0; i < count; ++i) {
        refresh_row_values(static_cast<int>(i));
        refresh_row_state(static_cast<int>(i));
    }
    lv_subject_set_int(&tool_count_, static_cast<int>(count));
}

void ToolOffsetCalibrationPanel::refresh_row_values(int tool) {
    auto& tools = helix::ToolState::instance();
    helix::xml::IndexedSubjectPool* pools[] = {&row_x_, &row_y_, &row_z_};
    const auto slot = static_cast<size_t>(tool);
    for (helix::Axis axis : helix::kAllAxes) {
        const int idx = helix::axis_index(axis);
        if (!tools.tool_offset_known(tool, axis)) {
            pools[idx]->set_string(slot, "--");
            continue;
        }
        // Plain number, no unit: the column header says mm, and the sign is
        // the whole point of an offset.
        pools[idx]->set_string(slot, fmt::format("{:+.3f}", tools.tool_offset_mm(tool, axis)));
    }
}

void ToolOffsetCalibrationPanel::refresh_row_state(int tool) {
    const auto slot = static_cast<size_t>(tool);
    if (slot >= row_state_.size()) {
        return; // a row refresh_rows() has not sized yet
    }
    const cal::ToolStep step = run_.step(tool);
    row_state_.set_int(slot, step_value(step));

    // The Measuring row's text is the elapsed counter's; everyone else's is
    // static. The counter is (re)armed only when a row ENTERS Measuring, so a
    // repaint mid-count does not restart it.
    if (step == cal::ToolStep::Measuring) {
        elapsed_.begin(&row_state_text_[tool], [](uint32_t seconds) {
            return fmt::format(fmt::runtime(lv_tr("Measuring... {}s")), seconds);
        });
        return;
    }
    const char* text = "";
    switch (step) {
    case cal::ToolStep::Idle:
        text = "";
        break;
    case cal::ToolStep::Queued:
        text = lv_tr("Queued");
        break;
    case cal::ToolStep::Done:
        text = lv_tr("Done");
        break;
    case cal::ToolStep::Failed:
        text = lv_tr("Failed");
        break;
    case cal::ToolStep::Measuring:
        break;
    }
    row_state_text_.set_string(slot, text);
}

// ============================================================================
// RUN
// ============================================================================

bool ToolOffsetCalibrationPanel::printer_supports_calibration() {
    return cal::supported(get_printer_state().get_discovery());
}

std::string ToolOffsetCalibrationPanel::start_prompt() const {
    // The macro's own description when the config provides one - it knows
    // its temperatures, its sensor and its reference - else the built-in
    // note. Either way the one condition no firmware can check leads: a blob
    // of filament on a nozzle gets measured as part of the nozzle.
    std::string text = lv_tr("Make sure every nozzle is clean.");
    text += "\n\n";
    text += hint_buffer_;
    text += "\n\n";
    text += lv_tr("Every tool's offsets are measured again and replace the current ones. "
                  "Save afterwards to keep the result.");
    return text;
}

void ToolOffsetCalibrationPanel::start_calibration() {
    if (run_.active()) {
        return;
    }
    if (!printer_supports_calibration()) {
        NOTIFY_ERROR("{}", lv_tr("This printer cannot calibrate tool offsets automatically"));
        return;
    }
    const std::string prompt = start_prompt();
    helix::ui::modal_confirm(lv_tr("Calibrate tool offsets?"), prompt.c_str(),
                             ModalSeverity::Warning, lv_tr("Calibrate"),
                             []() { get_global_tool_offset_cal_panel().begin_run(); });
}

void ToolOffsetCalibrationPanel::begin_run() {
    if (run_.active()) {
        return;
    }
    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[ToolOffsetCal] No API - cannot start calibration");
        return;
    }
    const std::string gcode = cal::calibrate_all_gcode(get_printer_state().get_discovery());
    if (gcode.empty()) {
        NOTIFY_ERROR("{}", lv_tr("This printer cannot calibrate tool offsets automatically"));
        return;
    }

    auto& tools = helix::ToolState::instance();
    const int tool_count = static_cast<int>(tools.tools().size());
    if (tool_count == 0) {
        // ToolState is empty between an AMS topology clear and the next
        // init_tools(). Run::begin(0) would stay inactive while the subject
        // below read active: Stop dead, Start hidden, until the process ends.
        NOTIFY_ERROR("{}", lv_tr("No tools to calibrate"));
        return;
    }
    // What every tool holds now: a tool whose offsets differ from this later
    // in the run has been measured.
    run_baseline_known_.assign(static_cast<size_t>(tool_count), {});
    run_baseline_mm_.assign(static_cast<size_t>(tool_count), {});
    for (int i = 0; i < tool_count; ++i) {
        for (helix::Axis axis : helix::kAllAxes) {
            const auto idx = static_cast<size_t>(helix::axis_index(axis));
            run_baseline_known_[static_cast<size_t>(i)][idx] = tools.tool_offset_known(i, axis);
            run_baseline_mm_[static_cast<size_t>(i)][idx] = tools.tool_offset_mm(i, axis);
        }
    }
    run_.begin(tool_count, tools.active_tool_index());
    last_error_.clear();
    lv_subject_set_int(&active_, 1);
    if (run_.measuring_tool() >= 0) {
        lv_subject_copy_string(
            &status_,
            fmt::format(fmt::runtime(lv_tr("Calibrating T{}...")), run_.measuring_tool()).c_str());
    } else {
        lv_subject_copy_string(&status_, lv_tr("Calibrating..."));
    }
    refresh_rows();

    spdlog::info("[ToolOffsetCal] Running {} over {} tools", gcode, tool_count);
    // Moonraker's printer.gcode.script answers when the script finishes, so
    // the success callback IS the completion signal. A full run heats and
    // probes every tool, which can pass the default macro ceiling.
    api->execute_gcode(
        gcode, run_lifetime_.bg_cb("ToolOffsetCal::done", [this]() { on_run_finished(true, ""); }),
        run_lifetime_.bg_cb("ToolOffsetCal::error",
                            [this](const MoonrakerError& err) { on_run_rpc_error(err); }),
        CALIBRATION_TIMEOUT_MS);
}

void ToolOffsetCalibrationPanel::on_run_rpc_error(const MoonrakerError& err) {
    if (!run_.active()) {
        return; // a Stop already settled it
    }
    // The rpc ceiling is not the printer's: Moonraker never times out
    // printer.gcode.script, so an expiry while Klipper still reports
    // idle_timeout "Printing" means the macro is still running - and failing
    // the run here would re-enable Save under a queue the macro still blocks.
    // Complete on the busy->idle edge instead, as PrintPreparationManager does
    // for a pre-start macro that outlives its ceiling.
    if (err.type == MoonrakerErrorType::TIMEOUT &&
        lv_subject_get_int(get_printer_state().get_idle_timeout_printing_subject()) == 1) {
        begin_idle_wait();
        return;
    }
    on_run_finished(false, err.user_message());
}

void ToolOffsetCalibrationPanel::begin_idle_wait() {
    if (idle_wait_active_) {
        return;
    }
    spdlog::warn("[ToolOffsetCal] Calibration rpc timed out with the printer still busy - "
                 "waiting for the busy->idle edge");
    idle_wait_active_ = true;
    lv_subject_copy_string(&status_,
                           lv_tr("Calibration may still be running — response timed out"));

    // Backstop: the macro already had a full ceiling on the rpc side; a
    // printer still busy after another one is wedged. Re-read the subject when
    // it fires - an edge that landed between the last notification and the
    // timer is a finished run.
    idle_wait_backstop_.begin(CALIBRATION_TIMEOUT_MS, [this]() {
        const bool still_busy =
            lv_subject_get_int(get_printer_state().get_idle_timeout_printing_subject()) == 1;
        finish_idle_wait();
        if (still_busy) {
            spdlog::error(
                "[ToolOffsetCal] Printer still busy after the backstop - failing the run");
            on_run_finished(false, "");
            return;
        }
        on_run_finished(true, "");
    });
    // The edge is the normal completion. observe_int_sync defers the handler
    // through the UpdateQueue, so the observer can be torn down from inside it.
    helix::PrinterState& ps = get_printer_state();
    idle_wait_observer_ = helix::ui::observe_int_sync<ToolOffsetCalibrationPanel>(
        ps.get_idle_timeout_printing_subject(), this,
        [](ToolOffsetCalibrationPanel* self, int busy) {
            if (!self->idle_wait_active_ || busy == 1) {
                return;
            }
            self->finish_idle_wait();
            spdlog::info("[ToolOffsetCal] Printer went idle after the rpc timeout - "
                         "treating the run as finished");
            self->on_run_finished(true, "");
        },
        ps.get_subjects_lifetime());
}

void ToolOffsetCalibrationPanel::finish_idle_wait() {
    idle_wait_active_ = false;
    // Observer first, so a queued stale notification finds the guard dead.
    idle_wait_observer_.reset();
    idle_wait_backstop_.end();
}

void ToolOffsetCalibrationPanel::on_run_finished(bool ok, const std::string& error) {
    if (!run_.active()) {
        return; // a Stop already settled it
    }
<<<<<<< HEAD
    elapsed_.cancel();
=======
    finish_idle_wait();
>>>>>>> 4960c6cf5 (fix(tool-offsets): own rpc ceiling, and a timeout under a busy printer waits)
    run_.finish(ok);
    lv_subject_set_int(&active_, 0);

    if (ok) {
        spdlog::info("[ToolOffsetCal] Calibration finished");
        lv_subject_copy_string(&status_, lv_tr("Calibration complete - save to keep the offsets"));
        refresh_rows();
        return;
    }
    last_error_ = error.empty() ? lv_tr("Calibration failed") : error;
    spdlog::error("[ToolOffsetCal] Calibration failed: {}", last_error_);
    lv_subject_copy_string(&status_, last_error_.c_str());
    refresh_rows();
    // The refusal is a one-time event with a verbatim firmware message; a
    // dismissible alert, not a permanent card. Off screen it is a toast: the
    // status line carries last_error_ when the panel comes back.
    if (is_visible()) {
        helix::ui::modal_alert(lv_tr("Calibration failed"), last_error_.c_str(),
                               ModalSeverity::Error);
    } else {
        NOTIFY_ERROR("{}", last_error_);
    }
}

bool ToolOffsetCalibrationPanel::abort_in_progress_calibration() {
    if (!run_.active()) {
        return false;
    }
    spdlog::info("[ToolOffsetCal] Aborting calibration (M112 + firmware restart)");

    // E-stop + firmware restart: klippy comes back, so this is an expected
    // reconnect, not a fault. Same call as the PID and input-shaper aborts.
    helix::ui::begin_expected_klippy_restart("Firmware restarting...");
    auto* api = get_moonraker_api();

    // Drop the in-flight execute_gcode callbacks: they would report the M112
    // shutdown as the run's failure.
    run_lifetime_.invalidate();
<<<<<<< HEAD
    elapsed_.cancel();
=======
    finish_idle_wait();
>>>>>>> 4960c6cf5 (fix(tool-offsets): own rpc ceiling, and a timeout under a busy printer waits)
    run_.abort();
    lv_subject_set_int(&active_, 0);
    lv_subject_copy_string(&status_, lv_tr("Stopped"));
    refresh_rows();

    if (api) {
        api->emergency_stop(
            [api]() {
                spdlog::debug("[ToolOffsetCal] M112 sent, restarting firmware");
                api->restart_firmware(
                    []() {},
                    [](const MoonrakerError& err) {
                        spdlog::error("[ToolOffsetCal] Firmware restart failed: {}", err.message);
                    });
            },
            [](const MoonrakerError& err) {
                spdlog::error("[ToolOffsetCal] Emergency stop failed: {}", err.message);
            });
    }
    return true;
}

// ============================================================================
// SAVE
// ============================================================================

void ToolOffsetCalibrationPanel::save_offsets() {
    if (run_.active()) {
        spdlog::warn("[ToolOffsetCal] Ignoring Save while a calibration is running");
        return;
    }
    // The button is disabled for both of these; this is the same refusal for
    // any other way in (as the bypass toggle refuses mid-print in code too).
    // A print can start from the web UI while this overlay is open, and Save
    // ends in SAVE_CONFIG, which restarts Klipper under it.
    if (job_holds_machine(get_printer_state().get_print_lifecycle())) {
        NOTIFY_WARNING(lv_tr("Cannot save offsets while printing"));
        spdlog::info("[ToolOffsetCal] Refused Save - a job holds the machine");
        return;
    }
    if (helix::ToolState::instance().dirty_tool_indices().empty()) {
        return;
    }
    // Warn only when a restart is actually coming, as the header's save
    // decides it: a pending machine-wide babystep always ends in SAVE_CONFIG,
    // and so do staged tool parameters; a firmware that persists immediately
    // restarts nothing.
    helix::PrinterState& ps = get_printer_state();
    const bool global_dirty = lv_subject_get_int(ps.get_gcode_z_offset_subject()) != 0;
    const bool restart_expected =
        global_dirty || helix::tool_offsets::persist_requires_save_config(ps.get_discovery());
    if (!restart_expected) {
        send_save();
        return;
    }
    helix::ui::modal_confirm(
        lv_tr("Save offsets?"),
        // The one restart also commits the babystep; say so in the words the
        // Controls and header saves use.
        global_dirty
            ? lv_tr("This will save the Z-offset and restart Klipper to write the configuration. "
                    "The printer will briefly disconnect.")
            : lv_tr("This writes the tool offsets to the printer's config and restarts Klipper, "
                    "which takes a few seconds. Until then they apply only to this session."),
        ModalSeverity::Warning, lv_tr("Save"),
        []() { get_global_tool_offset_cal_panel().send_save(); });
}

void ToolOffsetCalibrationPanel::send_save() {
    auto* api = get_moonraker_api();
    if (!api) {
        NOTIFY_ERROR("{}", lv_tr("No printer connection"));
        return;
    }
    helix::PrinterState& ps = get_printer_state();
    lv_subject_copy_string(&status_, lv_tr("Saving offsets..."));
    // The same path the header's save button takes, babystep included: the
    // SAVE_CONFIG this ends in restarts Klipper, which resets homing_origin,
    // so a pending machine-wide babystep is either applied and committed in
    // this same restart or silently lost by it - and its pending delta would
    // then go on being shown in Controls for an adjustment that no longer
    // existed. Computed as the header and Controls saves compute it.
    const bool global_dirty = lv_subject_get_int(ps.get_gcode_z_offset_subject()) != 0;
    helix::zoffset::save_dirty_offsets(
        api, save_watch_, ps.get_z_offset_calibration_strategy(), ps.get_discovery(), global_dirty,
        run_lifetime_.bg_cb("ToolOffsetCal::saved",
                            [this]() {
                                lv_subject_copy_string(&status_, lv_tr("Offsets saved"));
                                NOTIFY_SUCCESS("{}", lv_tr("Tool offsets saved"));
                            }),
        run_lifetime_.bg_cb("ToolOffsetCal::save_failed",
                            [this](const std::string& error) {
                                lv_subject_copy_string(&status_, error.c_str());
                                NOTIFY_ERROR("{}", error);
                            }),
        &ps);
}

// ============================================================================
// PROGRESS (from status, never the console)
// ============================================================================

void ToolOffsetCalibrationPanel::on_active_tool_changed(int tool) {
    if (!run_.active()) {
        return;
    }
    run_.on_tool_selected(tool);
    if (run_.measuring_tool() == tool) {
        elapsed_.cancel(); // the previous row's counter; the new row arms its own
        lv_subject_copy_string(
            &status_, fmt::format(fmt::runtime(lv_tr("Calibrating T{}...")), tool).c_str());
    }
    // The bookkeeping above runs whether or not the panel is on screen; the
    // repaint is only for a visible panel - on_activate() redraws every row
    // from the same state when it comes back (as the bed mesh panel stops its
    // renderer while hidden and reloads on return).
    if (!is_visible()) {
        return;
    }
    for (int i = 0; i < run_.tool_count(); ++i) {
        refresh_row_state(i);
    }
}

void ToolOffsetCalibrationPanel::on_tools_changed() {
    if (run_.active()) {
        auto& tools = helix::ToolState::instance();
        const int tracked = std::min(run_.tool_count(), static_cast<int>(run_baseline_mm_.size()));
        for (int i = 0; i < tracked; ++i) {
            bool moved = false;
            for (helix::Axis axis : helix::kAllAxes) {
                const auto idx = static_cast<size_t>(helix::axis_index(axis));
                const auto row = static_cast<size_t>(i);
                const bool known = tools.tool_offset_known(i, axis);
                if (known != run_baseline_known_[row][idx] ||
                    (known && tools.tool_offset_mm(i, axis) != run_baseline_mm_[row][idx])) {
                    moved = true;
                    break;
                }
            }
            if (moved) {
                run_.on_tool_measured(i);
            }
        }
    }
    if (is_visible()) {
        refresh_rows(); // hidden: on_activate() repaints from the same state
    }
}

// ============================================================================
// XML EVENT TRAMPOLINES
// ============================================================================

void ToolOffsetCalibrationPanel::on_start_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolOffsetCal] start");
    get_global_tool_offset_cal_panel().start_calibration();
    LVGL_SAFE_EVENT_CB_END();
}

void ToolOffsetCalibrationPanel::on_stop_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolOffsetCal] stop");
    get_global_tool_offset_cal_panel().abort_in_progress_calibration();
    LVGL_SAFE_EVENT_CB_END();
}

void ToolOffsetCalibrationPanel::on_save_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolOffsetCal] save");
    get_global_tool_offset_cal_panel().save_offsets();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// ADVANCED-PANEL ROW ENTRY
// ============================================================================

namespace {

lv_obj_t* g_advanced_row_panel = nullptr;

void on_tool_offset_row_clicked(lv_event_t* e) {
    (void)e;
    LVGL_SAFE_EVENT_CB_BEGIN("[ToolOffsetCal] advanced row");
    lv_obj_t* screen = lv_screen_active();
    helix::ui::lazy_create_and_push_overlay<ToolOffsetCalibrationPanel>(
        get_global_tool_offset_cal_panel, g_advanced_row_panel, screen, "Tool Offset Calibration",
        "AdvancedPanel");
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace

void init_tool_offset_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_tool_offset_row_clicked", on_tool_offset_row_clicked);
}

} // namespace helix::ui
