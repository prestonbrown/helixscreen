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
#include "i_moonraker_client.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "tool_offsets.h"
#include "tool_state.h"
#include "ui/ui_lazy_panel_helper.h"
#include "z_offset_utils.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

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

    for (int i = 0; i < MAX_TOOLS; ++i) {
        UI_MANAGED_SUBJECT_INT(row_visible_[i], 0,
                               fmt::format("tool_cal_row_visible_{}", i).c_str(), subjects_);
        UI_MANAGED_SUBJECT_INT(row_state_[i], step_value(cal::ToolStep::Idle),
                               fmt::format("tool_cal_state_{}", i).c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(row_state_text_[i], row_state_text_buffer_[i], "",
                                  fmt::format("tool_cal_state_text_{}", i).c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(row_x_[i], row_x_buffer_[i], "--",
                                  fmt::format("tool_cal_x_{}", i).c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(row_y_[i], row_y_buffer_[i], "--",
                                  fmt::format("tool_cal_y_{}", i).c_str(), subjects_);
        UI_MANAGED_SUBJECT_STRING(row_z_[i], row_z_buffer_[i], "--",
                                  fmt::format("tool_cal_z_{}", i).c_str(), subjects_);
    }

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
    if (!create_overlay_from_xml(parent, "calibration_tool_offset_panel")) {
        spdlog::error("[ToolOffsetCal] Failed to create overlay from XML");
        return nullptr;
    }
    return overlay_root_;
}

void ToolOffsetCalibrationPanel::show() {
    if (!overlay_root_) {
        spdlog::error("[ToolOffsetCal] Cannot show: overlay not created");
        return;
    }
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
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

    if (!run_.active()) {
        lv_subject_copy_string(&status_, last_error_.empty() ? lv_tr("Ready to calibrate")
                                                             : last_error_.c_str());
    }
    // Until the macro's own description arrives, say only what is true of
    // every implementation: how it heats, probes and which tool it measures
    // against is the macro's business, not this screen's.
    lv_subject_copy_string(&hint_,
                           lv_tr("Runs the printer's tool offset calibration for every tool."));
    refresh_rows();
    fetch_macro_description();
}

void ToolOffsetCalibrationPanel::on_deactivate() {
    // A run keeps going on the printer whether or not the panel is on screen.
    // The observers stay with it: ToolState keeps publishing, and the rows
    // must be right when the panel comes back.
    OverlayBase::on_deactivate();
}

void ToolOffsetCalibrationPanel::cleanup() {
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
    const auto& tools = helix::ToolState::instance().tools();
    for (int i = 0; i < MAX_TOOLS; ++i) {
        const bool visible = i < static_cast<int>(tools.size());
        lv_subject_set_int(&row_visible_[i], visible ? 1 : 0);
        if (!visible) {
            continue;
        }
        refresh_row_values(i);
        refresh_row_state(i);
    }
}

void ToolOffsetCalibrationPanel::refresh_row_values(int tool) {
    auto& tools = helix::ToolState::instance();
    lv_subject_t* subjects[] = {&row_x_[tool], &row_y_[tool], &row_z_[tool]};
    char* buffers[] = {row_x_buffer_[tool], row_y_buffer_[tool], row_z_buffer_[tool]};
    for (helix::Axis axis : helix::kAllAxes) {
        const int idx = helix::axis_index(axis);
        if (!tools.tool_offset_known(tool, axis)) {
            lv_subject_copy_string(subjects[idx], "--");
            continue;
        }
        char text[16];
        // Plain number, no unit: the column header says mm, and the sign is
        // the whole point of an offset.
        std::snprintf(text, sizeof(text), "%+.3f", tools.tool_offset_mm(tool, axis));
        std::snprintf(buffers[idx], sizeof(row_x_buffer_[tool]), "%s", text);
        lv_subject_copy_string(subjects[idx], buffers[idx]);
    }
}

void ToolOffsetCalibrationPanel::refresh_row_state(int tool) {
    const cal::ToolStep step = run_.step(tool);
    lv_subject_set_int(&row_state_[tool], step_value(step));

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
    lv_subject_copy_string(&row_state_text_[tool], text);
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
    const int tool_count = std::min(static_cast<int>(tools.tools().size()), MAX_TOOLS);
    // What every tool holds now: a tool whose offsets differ from this later
    // in the run has been measured.
    for (int i = 0; i < tool_count; ++i) {
        for (helix::Axis axis : helix::kAllAxes) {
            const int idx = helix::axis_index(axis);
            run_baseline_known_[i][idx] = tools.tool_offset_known(i, axis);
            run_baseline_mm_[i][idx] = tools.tool_offset_mm(i, axis);
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
        gcode, lifetime_.bg_cb("ToolOffsetCal::done", [this]() { on_run_finished(true, ""); }),
        lifetime_.bg_cb(
            "ToolOffsetCal::error",
            [this](const MoonrakerError& err) { on_run_finished(false, err.user_message()); }),
        IMoonrakerAPI::PRE_START_MACRO_TIMEOUT_MS);
}

void ToolOffsetCalibrationPanel::on_run_finished(bool ok, const std::string& error) {
    if (!run_.active()) {
        return; // a Stop already settled it
    }
    elapsed_.cancel();
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
    // dismissible alert, not a permanent card.
    helix::ui::modal_alert(lv_tr("Calibration failed"), last_error_.c_str(), ModalSeverity::Error);
}

bool ToolOffsetCalibrationPanel::abort_in_progress_calibration() {
    if (!run_.active()) {
        return false;
    }
    spdlog::info("[ToolOffsetCal] Aborting calibration (M112 + firmware restart)");

    // Expected reconnect: keep the shutdown/disconnect modals quiet.
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::LONG);
    auto* api = get_moonraker_api();
    if (api) {
        api->suppress_disconnect_modal(15000);
    }

    // Drop the in-flight execute_gcode callbacks: they would report the M112
    // shutdown as the run's failure.
    lifetime_.invalidate();
    elapsed_.cancel();
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
    if (helix::ToolState::instance().dirty_tool_indices().empty()) {
        return;
    }
    const auto& hw = get_printer_state().get_discovery();
    if (!helix::tool_offsets::persist_requires_save_config(hw)) {
        send_save();
        return;
    }
    helix::ui::modal_confirm(
        lv_tr("Save offsets?"),
        lv_tr("This writes the tool offsets to the printer's config and restarts Klipper, "
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
    // The same path the header's save button takes, minus the machine-wide
    // baby step: only the tools are this panel's business.
    helix::zoffset::save_dirty_offsets(
        api, save_watch_, ps.get_z_offset_calibration_strategy(), ps.get_discovery(),
        /*global_dirty=*/false,
        lifetime_.bg_cb("ToolOffsetCal::saved",
                        [this]() {
                            lv_subject_copy_string(&status_, lv_tr("Offsets saved"));
                            NOTIFY_SUCCESS("{}", lv_tr("Tool offsets saved"));
                        }),
        lifetime_.bg_cb("ToolOffsetCal::save_failed",
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
    for (int i = 0; i < std::min(run_.tool_count(), MAX_TOOLS); ++i) {
        refresh_row_state(i);
    }
}

void ToolOffsetCalibrationPanel::on_tools_changed() {
    if (run_.active()) {
        auto& tools = helix::ToolState::instance();
        for (int i = 0; i < std::min(run_.tool_count(), MAX_TOOLS); ++i) {
            bool moved = false;
            for (helix::Axis axis : helix::kAllAxes) {
                const int idx = helix::axis_index(axis);
                const bool known = tools.tool_offset_known(i, axis);
                if (known != run_baseline_known_[i][idx] ||
                    (known && tools.tool_offset_mm(i, axis) != run_baseline_mm_[i][idx])) {
                    moved = true;
                    break;
                }
            }
            if (moved) {
                run_.on_tool_measured(i);
            }
        }
    }
    refresh_rows();
}

void ToolOffsetCalibrationPanel::fetch_macro_description() {
    auto* client = get_moonraker_client();
    if (!client) {
        return;
    }
    // printer.gcode.help -> {"CMD": "description", ...}; the macro's own
    // `description:` is the instruction text when the config provides one.
    client->send_jsonrpc(
        "printer.gcode.help", nlohmann::json::object(),
        lifetime_.bg_cb("ToolOffsetCal::gcode_help", [this](const nlohmann::json& resp) {
            const nlohmann::json& result = resp.contains("result") ? resp["result"] : resp;
            if (!subjects_initialized_ || !result.is_object() || !result.contains(cal::kMacro) ||
                !result[cal::kMacro].is_string()) {
                return;
            }
            const std::string desc = result[cal::kMacro].get<std::string>();
            if (desc.empty() || desc == "G-Code macro") {
                return; // Klipper's placeholder for a macro without description:
            }
            lv_subject_copy_string(&hint_, desc.c_str());
        }));
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
