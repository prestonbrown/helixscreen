// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_preparation_manager.h"

#include "ui_busy_overlay.h"
#include "ui_error_reporting.h"
#include "ui_filename_utils.h"
#include "ui_panel_print_status.h"
#include "ui_pre_print_options_renderer.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "active_print_media_manager.h"
#include "app_globals.h"
#include "gcode_tool_remapper.h"
#include "helix_fs.h"
#include "http_executor.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "macro_param_cache.h"
#include "memory_monitor.h"
#include "memory_utils.h"
#include "moonraker_manager.h"
#include "observer_factory.h"
#include "operation_registry.h"
#include "preprint_skip_wrappers.h"
#include "print_start_collector.h"
#include "system/telemetry_manager.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>

// Forward declaration for global print status panel (declared in ui_panel_print_status.h)
PrintStatusPanel& get_global_print_status_panel();

namespace helix::ui {

namespace hfs = helix::fs;

// Bring helix:: types into scope for cleaner code
using helix::CapabilityOrigin;
using helix::OperationCategory;

namespace {

/// The PRINT_START operations macro analysis can turn into an option, in row order.
struct MacroOptionId {
    helix::PrintStartOpCategory category;
    const char* id;
    PrePrintCategory group;
};
constexpr MacroOptionId MACRO_OPTION_IDS[] = {
    {helix::PrintStartOpCategory::BED_MESH, "bed_mesh", PrePrintCategory::Mechanical},
    {helix::PrintStartOpCategory::QGL, "qgl", PrePrintCategory::Mechanical},
    {helix::PrintStartOpCategory::Z_TILT, "z_tilt", PrePrintCategory::Mechanical},
    {helix::PrintStartOpCategory::NOZZLE_CLEAN, "nozzle_clean", PrePrintCategory::Quality},
};

/// The MacroParam option for one of MACRO_OPTION_IDS, or nullopt when the analysis
/// did not find that operation controllable. Rows and the skip params sent at print
/// start both come from here, so they cannot disagree.
std::optional<PrePrintOption>
macro_option_for(const std::optional<helix::PrintStartAnalysis>& analysis,
                 const MacroOptionId& entry) {
    if (!analysis || !analysis->found) {
        return std::nullopt;
    }
    CapabilityMatrix matrix;
    matrix.add_from_macro_analysis(*analysis);
    const auto source = matrix.get_best_source(entry.category);
    if (!source) {
        return std::nullopt;
    }

    PrePrintOption opt;
    opt.id = entry.id;
    opt.category = entry.group;
    opt.order = static_cast<int>(&entry - MACRO_OPTION_IDS);
    opt.default_enabled = true; // the macro runs the operation unless told to skip it
    opt.strategy_kind = PrePrintStrategyKind::MacroParam;
    PrePrintStrategyMacroParam param;
    param.param_name = source->param_name;
    param.enable_value = source->enable_value;
    param.skip_value = source->skip_value;
    opt.strategy = std::move(param);
    return opt;
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

PrintPreparationManager::~PrintPreparationManager() {
    if (printer_state_) {
        printer_state_->set_skip_pending_handler(nullptr);
    }
    // lifetime_ destructor calls invalidate() automatically
}

// ============================================================================
// Pre-Print Option Set Cache Helper
// ============================================================================

const PrePrintOptionSet& PrintPreparationManager::get_cached_options() const {
    // Delegate to PrinterState which owns the cache
    if (printer_state_) {
        return printer_state_->profile_state().pre_print_option_set();
    }

    // Return empty set if PrinterState not set
    static const PrePrintOptionSet empty_set;
    return empty_set;
}

// ============================================================================
// Option State Resolution (LT3)
// ============================================================================

PrePrintOptionState PrintPreparationManager::get_option_state(const std::string& id) const {
    const auto& opts = get_cached_options();

    // Option gated on a required macro that isn't registered with Klipper:
    // surface as NOT_APPLICABLE so the UI hides the toggle and
    // collect_pre_start_gcode_lines skips it. (e.g. K2 AI detect requires
    // LOAD_AI_RUN — only present on Creality OS variants.)
    if (const PrePrintOption* opt = opts.find(id)) {
        if (is_macro_gate_closed(*opt)) {
            return PrePrintOptionState::NOT_APPLICABLE;
        }
    }

    // 1. Provider takes priority. The detail panel registers a provider that
    //    reads from per-option dynamic subjects. The provider returns 0/1
    //    when bound; any other value means "not bound" — fall through.
    if (option_state_provider_) {
        int v = option_state_provider_(id);
        if (v == 0) {
            return PrePrintOptionState::DISABLED;
        }
        if (v == 1) {
            return PrePrintOptionState::ENABLED;
        }
    }

    // 2. Fall back to the option's default_enabled from the cached set.
    //    This is the right answer when no panel is attached (e.g. macro
    //    analysis runs in headless contexts and asks for the option's
    //    intended state).
    if (const PrePrintOption* opt = opts.find(id)) {
        return opt->default_enabled ? PrePrintOptionState::ENABLED : PrePrintOptionState::DISABLED;
    }

    return PrePrintOptionState::NOT_APPLICABLE;
}

// ============================================================================
// Setup
// ============================================================================

void PrintPreparationManager::set_dependencies(IMoonrakerAPI* api, PrinterState* printer_state) {
    api_ = api;
    printer_state_ = printer_state;

    if (printer_state_) {
        connection_observer_ = helix::ui::observe<int>(
            printer_state_->network_state().get_printer_connection_state_subject(), this,
            [](PrintPreparationManager* self, int state) { self->on_connection_state(state); },
            printer_state_->get_subjects_lifetime());
        klippy_observer_ = helix::ui::observe<int>(
            printer_state_->network_state().get_klippy_state_subject(), this,
            [](PrintPreparationManager* self, int state) { self->on_klippy_state(state); },
            printer_state_->get_subjects_lifetime());
        job_holds_observer_ = helix::ui::observe<int>(
            printer_state_->print_state().get_job_holds_machine_subject(), this,
            [](PrintPreparationManager* self, int) { self->reset_pending_skips(); },
            printer_state_->get_subjects_lifetime());
        printer_state_->set_skip_pending_handler([this]() { reset_pending_skips(); });
    }
}

void PrintPreparationManager::on_connection_state(int state) {
    const int connected = static_cast<int>(ConnectionState::CONNECTED);
    const bool reconnected = state == connected && last_connection_state_ != connected;
    last_connection_state_ = state;
    if (reconnected) {
        // A new connection can be a different printer, or one whose config changed.
        klippy_restarting_ = false;
        refresh_macro_analysis();
    } else if (state == connected) {
        analyze_print_start_macro();
    }
}

void PrintPreparationManager::on_klippy_state(int state) {
    // A restart is how an edited macro takes effect. Klippy reads not-ready
    // before the first connect too; only a dip seen while connected counts.
    if (state != static_cast<int>(KlippyState::READY)) {
        klippy_restarting_ = last_connection_state_ == static_cast<int>(ConnectionState::CONNECTED);
        return;
    }
    if (klippy_restarting_) {
        klippy_restarting_ = false;
        refresh_macro_analysis();
    }
}

void PrintPreparationManager::reset_pending_skips() {
    if (!api_ || !printer_state_) {
        return;
    }
    if (!printer_state_->skip_pending()) {
        skip_reset_sent_ = false;
        return;
    }
    if (skip_reset_sent_ ||
        lv_subject_get_int(printer_state_->print_state().get_job_holds_machine_subject()) != 0) {
        return;
    }
    skip_reset_sent_ = true;
    spdlog::info("[PrintPreparationManager] Leveling skip set with no job running; resetting");
    api_->execute_gcode(
        helix::skip_wrappers::PREP_MACRO, []() {},
        [](const MoonrakerError& err) {
            spdlog::warn("[PrintPreparationManager] Resetting the leveling skips failed: {}",
                         err.message);
        },
        0, true, nullptr, /*caller_surfaces_errors=*/false);
}

void PrintPreparationManager::refresh_macro_analysis() {
    macro_analysis_.reset();
    publish_macro_option_count();
    if (macro_analysis_in_progress_) {
        macro_analysis_stale_ = true;
        return;
    }
    analyze_print_start_macro();
}

bool PrintPreparationManager::restart_stale_macro_analysis() {
    if (!macro_analysis_stale_) {
        return false;
    }
    macro_analysis_stale_ = false;
    macro_analysis_retry_count_ = 0;
    spdlog::debug("[PrintPreparationManager] Printer changed during PRINT_START analysis, "
                  "analyzing again");
    analyze_print_start_macro_internal();
    return true;
}

void PrintPreparationManager::ensure_estimate_subject_initialized() {
    if (!estimate_subject_initialized_) {
        lv_subject_init_int(&preprint_estimate_subject_, 0);
        estimate_subject_initialized_ = true;
    }
}

// ============================================================================
// Pre-print Estimate
// ============================================================================

lv_subject_t* PrintPreparationManager::get_preprint_estimate_subject() {
    ensure_estimate_subject_initialized();
    return &preprint_estimate_subject_;
}

void PrintPreparationManager::recalculate_estimate() {
    // Callers reach this before any getter fetch (the detail view computes
    // the estimate on open), so the subject is initialised here.
    ensure_estimate_subject_initialized();

    if (!printer_state_)
        return;

    auto& mgr = ThermalRateManager::instance();

    // Current temps (decidegrees -> degrees)
    float ext_temp = helix::ui::temperature::deci_to_degrees_f(
        lv_subject_get_int(printer_state_->temperature_state().get_active_extruder_temp_subject()));
    float ext_target = helix::ui::temperature::deci_to_degrees_f(lv_subject_get_int(
        printer_state_->temperature_state().get_active_extruder_target_subject()));
    float bed_temp = helix::ui::temperature::deci_to_degrees_f(
        lv_subject_get_int(printer_state_->temperature_state().get_bed_temp_subject()));
    float bed_target = helix::ui::temperature::deci_to_degrees_f(
        lv_subject_get_int(printer_state_->temperature_state().get_bed_target_subject()));

    float total = 0.0f;

    // Heating estimates
    total += mgr.estimate_heating_seconds("extruder", ext_temp, ext_target);
    total += mgr.estimate_heating_seconds("heater_bed", bed_temp, bed_target);

    // Non-heating ops from predictor (cached to avoid reparsing config JSON on every toggle)
    if (!predictor_cached_) {
        auto entries = helix::PreprintPredictor::load_entries_from_config();
        bool is_warm = bed_temp >= 40.0f;
        cached_predictor_ = helix::PreprintPredictor{};
        cached_predictor_.load_entries(entries,
                                       is_warm ? StartCondition::WARM : StartCondition::COLD);
        predictor_cached_ = true;
    }

    // Homing always happens, and takes the collector's estimate for it
    total += static_cast<float>(cached_predictor_.predicted_homing_seconds());
    auto phases = cached_predictor_.predicted_phases();

    // Add phase estimate if the option is currently enabled. State is read
    // through the new framework (get_option_state(id) — provider-driven from
    // the active panel's renderer; falls back to default_enabled if no panel
    // is attached).
    auto add_if_enabled = [&](const std::string& id, int phase_int, float default_s) {
        if (get_option_state(id) != PrePrintOptionState::ENABLED) {
            return;
        }
        auto it = phases.find(phase_int);
        total += (it != phases.end()) ? static_cast<float>(it->second) : default_s;
    };

    add_if_enabled("bed_mesh", static_cast<int>(helix::PrintStartPhase::BED_MESH), 90.0f);
    add_if_enabled("qgl", static_cast<int>(helix::PrintStartPhase::QGL), 60.0f);
    add_if_enabled("z_tilt", static_cast<int>(helix::PrintStartPhase::Z_TILT), 45.0f);
    add_if_enabled("nozzle_clean", static_cast<int>(helix::PrintStartPhase::CLEANING), 15.0f);
    add_if_enabled("purge_line", static_cast<int>(helix::PrintStartPhase::PURGING), 10.0f);

    int estimate_s = static_cast<int>(total);
    lv_subject_set_int(&preprint_estimate_subject_, estimate_s);

    spdlog::debug("[PrintPreparationManager] Pre-print estimate: {}s", estimate_s);
}

void PrintPreparationManager::invalidate_predictor_cache() {
    predictor_cached_ = false;
}

// ============================================================================
// PRINT_START Macro Analysis
// ============================================================================

void PrintPreparationManager::analyze_print_start_macro() {
    // Skip if analysis already in progress
    if (macro_analysis_in_progress_) {
        spdlog::debug("[PrintPreparationManager] PRINT_START analysis already in progress");
        return;
    }

    // Skip if we already have a cached result
    if (macro_analysis_.has_value()) {
        spdlog::debug("[PrintPreparationManager] Using cached PRINT_START analysis");
        publish_macro_option_count();
        if (on_macro_analysis_complete_) {
            on_macro_analysis_complete_(*macro_analysis_);
        }
        return;
    }

    // Reset retry counter when starting fresh
    macro_analysis_retry_count_ = 0;

    // Delegate to internal implementation
    analyze_print_start_macro_internal();
}

void PrintPreparationManager::analyze_print_start_macro_internal() {
    if (!api_) {
        spdlog::warn("[PrintPreparationManager] Cannot analyze PRINT_START - no API connection");
        return;
    }

    // Check if WebSocket connection is actually established
    if (api_->get_connection_state() != ConnectionState::CONNECTED) {
        spdlog::debug("[PrintPreparationManager] Deferring PRINT_START analysis - not connected");
        // A retry can land here mid-analysis; the next connect starts over.
        macro_analysis_in_progress_ = false;
        macro_analysis_stale_ = false;
        return;
    }

    macro_analysis_in_progress_ = true;
    spdlog::debug(
        "[PrintPreparationManager] Starting PRINT_START macro analysis (attempt {} of {})",
        macro_analysis_retry_count_ + 1, MAX_MACRO_ANALYSIS_RETRIES + 1);

    auto token = lifetime_.token();
    helix::PrintStartAnalyzer analyzer;

    analyzer.analyze(
        api_,
        // Success callback - runs on HTTP thread; the body just logs and
        // marshals to main via tok.defer (no inline LVGL or member writes).
        [this, token](const helix::PrintStartAnalysis& analysis) {
            token.defer("PrintPreparationManager::macro_analysis_success", [this, analysis]() {
                if (restart_stale_macro_analysis()) {
                    return;
                }
                spdlog::debug("[PrintPreparationManager] PRINT_START analysis complete: {}",
                              analysis.summary());
                macro_analysis_ = analysis;
                macro_analysis_in_progress_ = false;
                publish_macro_option_count();
                if (on_macro_analysis_complete_) {
                    on_macro_analysis_complete_(analysis);
                }
            });
        },
        // Error callback - runs on HTTP thread; same pattern as success.
        [this, token](const MoonrakerError& error) {
            token.defer("PrintPreparationManager::macro_analysis_error", [this, error]() {
                if (restart_stale_macro_analysis()) {
                    return;
                }
                spdlog::warn(
                    "[PrintPreparationManager] PRINT_START analysis failed (attempt {}): {}",
                    macro_analysis_retry_count_ + 1, error.message);
                // Check if we should retry
                if (macro_analysis_retry_count_ < MAX_MACRO_ANALYSIS_RETRIES) {
                    macro_analysis_retry_count_++;
                    // Exponential backoff: 1s, 2s
                    int delay_ms = 1000 * (1 << (macro_analysis_retry_count_ - 1));

                    spdlog::info("[PrintPreparationManager] Retrying PRINT_START analysis in "
                                 "{}ms (attempt {} of {})",
                                 delay_ms, macro_analysis_retry_count_ + 1,
                                 MAX_MACRO_ANALYSIS_RETRIES + 1);

                    // Schedule retry via LVGL timer
                    struct RetryTimerData {
                        PrintPreparationManager* mgr;
                        helix::LifetimeToken token;
                    };
                    auto timer_data_ptr =
                        std::make_unique<RetryTimerData>(RetryTimerData{this, lifetime_.token()});

                    lv_timer_t* retry_timer = lv_timer_create(
                        [](lv_timer_t* timer) {
                            std::unique_ptr<RetryTimerData> data(
                                static_cast<RetryTimerData*>(lv_timer_get_user_data(timer)));
                            if (data && !data->token.expired()) {
                                data->mgr->analyze_print_start_macro_internal();
                            }
                            lv_timer_delete(timer);
                        },
                        delay_ms, timer_data_ptr.release());
                    lv_timer_set_repeat_count(retry_timer, 1);
                    return;
                }

                // Final failure - notify user
                spdlog::error(
                    "[PrintPreparationManager] PRINT_START analysis failed after {} attempts",
                    MAX_MACRO_ANALYSIS_RETRIES + 1);
                NOTIFY_ERROR(lv_tr("Could not analyze PRINT_START macro. Some print options may be "
                                   "unavailable."));

                // Set empty result
                macro_analysis_in_progress_ = false;
                helix::PrintStartAnalysis not_found;
                not_found.found = false;
                macro_analysis_ = not_found;
                publish_macro_option_count();
                if (on_macro_analysis_complete_) {
                    on_macro_analysis_complete_(not_found);
                }
            });
        });
}

PrePrintOptionSet PrintPreparationManager::displayed_options() const {
    PrePrintOptionSet displayed = get_cached_options();
    const bool database_declares_options =
        printer_state_ &&
        !PrinterDetector::get_pre_print_option_set(printer_state_->profile_state().printer_type())
             .options.empty();
    if (database_declares_options) {
        return displayed;
    }
    for (const auto& entry : MACRO_OPTION_IDS) {
        if (displayed.find(entry.id)) {
            continue;
        }
        if (auto opt = macro_option_for(macro_analysis_, entry)) {
            displayed.options.push_back(std::move(*opt));
        }
    }
    sort_pre_print_options(displayed.options);
    return displayed;
}

// ============================================================================
// CapabilityMatrix Integration
// ============================================================================

CapabilityMatrix PrintPreparationManager::build_capability_matrix() const {
    CapabilityMatrix matrix;

    // Layer 1: Database capabilities (highest priority)
    const auto& db_options = get_cached_options();
    if (!db_options.empty()) {
        matrix.add_from_database(db_options);
    }

    // Layer 2: Macro analysis (medium priority)
    if (macro_analysis_ && macro_analysis_->found) {
        matrix.add_from_macro_analysis(*macro_analysis_);
    }

    // Layer 3: File scan (lowest priority)
    if (cached_scan_result_) {
        matrix.add_from_file_scan(*cached_scan_result_);
    }

    return matrix;
}

void PrintPreparationManager::set_macro_analysis(const helix::PrintStartAnalysis& analysis) {
    macro_analysis_ = analysis;
    publish_macro_option_count();
}

// The options card shows only when some row will render, and the rows this
// analysis adds are invisible to PrinterState otherwise.
void PrintPreparationManager::publish_macro_option_count() {
    if (!printer_state_) {
        return;
    }
    const size_t displayed = displayed_options().options.size();
    const size_t declared = get_cached_options().options.size();
    printer_state_->set_macro_option_count(displayed - declared);
}

void PrintPreparationManager::set_cached_scan_result(const gcode::ScanResult& scan,
                                                     const std::string& filename) {
    cached_scan_result_ = scan;
    cached_scan_filename_ = filename;
    cached_scan_key_ = filename;
}

// ============================================================================
// G-code Scanning
// ============================================================================

void PrintPreparationManager::scan_file_for_operations(const std::string& filename,
                                                       const std::string& current_path,
                                                       const std::string& local_path) {
    // The cached result is reusable only while its printer-stopping command
    // answer still holds. An answer computed before the printer's macros were
    // read, or against a macro set it has since replaced, says nothing about
    // this printer, so the file is scanned again.
    const std::string file_path = current_path.empty() ? filename : current_path + "/" + filename;
    const std::string key = local_path.empty() ? file_path : local_path;
    if (cached_scan_key_ == key && cached_scan_filename_ == filename &&
        cached_scan_result_.has_value() && has_printer_stop_answer_for(filename)) {
        spdlog::debug("[PrintPreparationManager] Using cached scan result for {}", key);
        return;
    }
    // Every answer below belongs to this key; a same-named file's answer must
    // not stand in for it while this one is pending.
    requested_scan_key_ = key;
    if (cached_scan_key_ != key) {
        cached_scan_result_.reset();
        cached_scan_filename_.clear();
        printer_stop_check_filename_.clear();
    }

    if (!api_) {
        spdlog::warn("[PrintPreparationManager] Cannot scan G-code - no API connection");
        answer_printer_stop_check(filename, helix::printer_stop_not_run("no printer connection"));
        return;
    }

    if (helix::gcode::is_3mf(filename)) {
        // An empty result, not none: every cache reader must stop seeing the
        // previously opened file's operations.
        cached_scan_result_ = gcode::ScanResult{};
        cached_scan_filename_ = filename;
        cached_scan_key_ = key;
        answer_printer_stop_check(filename,
                                  printer_stop_not_run("a .3mf project holds no G-code to scan"));
        return;
    }

    spdlog::info("[PrintPreparationManager] Scanning G-code for embedded operations: {}",
                 local_path.empty() ? file_path : local_path);

    auto token = lifetime_.token();

    // Only the file's head is needed for preamble scanning (thumbnails + slicer
    // metadata + START_PRINT call + any early G-code ops), which avoids
    // downloading multi-MB files just to scan the first few hundred lines.
    // Both callbacks run on a background thread: parse there, then defer the
    // shared state updates to the main thread.
    auto on_content = [this, token, filename, key](const std::string& content) {
        gcode::GCodeOpsDetector detector;
        auto scan_result = detector.scan_content(content);
        // The whole downloaded head, not the detector's shorter op window:
        // a large thumbnail block can push the definitions past that.
        scan_result.objects = gcode::collect_exclude_object_defines(content);

        if (scan_result.operations.empty()) {
            spdlog::debug("[PrintPreparationManager] No embedded operations found in {}", filename);
        } else {
            spdlog::info("[PrintPreparationManager] Found {} embedded operations in {}:",
                         scan_result.operations.size(), filename);
            for (const auto& op : scan_result.operations) {
                spdlog::info("[PrintPreparationManager]   - {} at line {} ({})", op.display_name(),
                             op.line_number, op.raw_line.substr(0, 50));
            }
        }

        helix::PrinterStopCheck stop_check =
            helix::printer_stop_check_in(content, helix::PRINTER_STOP_SCAN_BYTES);
        if (stop_check.state == helix::PrinterStopCheck::State::Stops) {
            spdlog::warn("[PrintPreparationManager] {} line {} calls {}, which stops this printer",
                         filename, stop_check.line_number, stop_check.command);
        }

        token.defer("PrintPreparationManager::scan_success",
                    [this, filename, key, scan_result, stop_check]() {
                        if (key != requested_scan_key_) {
                            return;
                        }
                        cached_scan_result_ = scan_result;
                        cached_scan_filename_ = filename;
                        cached_scan_key_ = key;
                        answer_printer_stop_check(filename, stop_check);
                    });
    };
    // A failed read just logs; it never blocks the UI.
    auto on_failure = [this, token, filename, key](const std::string& message) {
        spdlog::warn("[PrintPreparationManager] Failed to scan G-code {}: {}", filename, message);
        std::string reason = "the file could not be read: " + message;
        token.defer("PrintPreparationManager::scan_error", [this, filename, key, reason]() {
            if (key != requested_scan_key_) {
                return;
            }
            cached_scan_result_.reset();
            cached_scan_filename_.clear();
            cached_scan_key_.clear();
            answer_printer_stop_check(filename, helix::printer_stop_not_run(reason));
        });
    };

    // A bounded read, on the fast lane: the slow lane's one worker can sit
    // behind a multi-minute upload.
    if (!local_path.empty()) {
        helix::http::HttpExecutor::fast().submit([local_path, on_content, on_failure]() {
            auto head = helix::text_io::read_file(local_path, helix::PRINTER_STOP_SCAN_BYTES);
            if (!head) {
                on_failure("cannot read " + local_path);
                return;
            }
            on_content(*head);
        });
        return;
    }

    api_->transfers().download_file_partial(
        "gcodes", file_path, helix::PRINTER_STOP_SCAN_BYTES, on_content,
        [on_failure](const MoonrakerError& error) { on_failure(error.message); });
}

void PrintPreparationManager::clear_scan_cache() {
    cached_scan_result_.reset();
    cached_scan_filename_.clear();
    cached_scan_key_.clear();
    requested_scan_key_.clear();
    cached_file_size_.reset();
    printer_stop_check_ = {};
    printer_stop_check_filename_.clear();
}

helix::PrinterStopCheck
PrintPreparationManager::printer_stop_check_for(const std::string& filename) const {
    if (has_printer_stop_answer_for(filename)) {
        return printer_stop_check_;
    }
    return helix::printer_stop_not_run(
        "the file scan has not answered for the printer's current macros");
}

bool PrintPreparationManager::has_printer_stop_answer_for(const std::string& filename) const {
    return !filename.empty() && printer_stop_check_filename_ == filename &&
           printer_stop_check_.macro_generation == helix::MacroParamCache::instance().generation();
}

void PrintPreparationManager::set_on_scan_answered(std::function<void()> cb) {
    on_scan_answered_ = std::move(cb);
}

void PrintPreparationManager::answer_printer_stop_check(const std::string& filename,
                                                        helix::PrinterStopCheck check) {
    printer_stop_check_ = std::move(check);
    printer_stop_check_filename_ = filename;
    if (on_scan_answered_) {
        on_scan_answered_();
    }
}

bool PrintPreparationManager::has_scan_result_for(const std::string& filename) const {
    return cached_scan_filename_ == filename && cached_scan_result_.has_value();
}

const gcode::PrintStartCallInfo*
PrintPreparationManager::print_start_for(const std::string& filename) const {
    if (!has_scan_result_for(filename) || !cached_scan_result_->print_start.found) {
        return nullptr;
    }
    return &cached_scan_result_->print_start;
}

// ============================================================================
// Resource Safety
// ============================================================================

void PrintPreparationManager::set_cached_file_size(size_t size) {
    cached_file_size_ = size;
    spdlog::debug("[PrintPreparationManager] Cached file size: {} bytes ({:.1f} MB)", size,
                  static_cast<double>(size) / (1024.0 * 1024.0));
}

std::string PrintPreparationManager::get_temp_directory() const {
    // Delegate to global helper for consistent cache directory selection
    return get_helix_cache_dir("gcode_temp");
}

bool PrintPreparationManager::can_modify_gcode() const {
    // Pre-print modifications rewrite the job file, and the plugin is what puts
    // the original filename back in Moonraker's history afterwards. Without it
    // finished jobs are listed as ".helix_temp/modified_1766807545p_name.gcode",
    // so we decline rather than clutter the history. The rewrite also streams
    // through a local copy, which some transports cannot keep.
    return printer_state_ != nullptr &&
           printer_state_->plugin_status_state().service_has_helix_plugin() &&
           transport_keeps_local_copies();
}

bool PrintPreparationManager::transport_keeps_local_copies() const {
    // No API is refused where a download would start, not here.
    return api_ == nullptr || api_->transfers().supports_local_copies();
}

// ============================================================================
// Print Execution
// ============================================================================

PrePrintOptions PrintPreparationManager::read_options_from_subjects() const {
    PrePrintOptions options;

    auto enabled = [this](const std::string& id) {
        return get_option_state(id) == PrePrintOptionState::ENABLED;
    };

    options.bed_mesh = enabled("bed_mesh");
    options.qgl = enabled("qgl");
    options.z_tilt = enabled("z_tilt");
    options.nozzle_clean = enabled("nozzle_clean");
    options.purge_line = enabled("purge_line");
    options.timelapse = enabled("timelapse");

    return options;
}

void PrintPreparationManager::start_print(const std::string& filename,
                                          const std::string& current_path,
                                          NavigateToStatusCallback on_navigate_to_status,
                                          PrintCompletionCallback on_completion) {
    // Snapshot whether this start is under a preparing job. Only then does the
    // job disappearing later mean the user cancelled; a caller that never armed
    // one must still be able to start a print.
    armed_at_start_ = printer_state_ && printer_state_->print_state().has_preparing_job();

    if (!api_) {
        spdlog::error("[PrintPreparationManager] Cannot start print - not connected to printer");
        NOTIFY_ERROR(lv_tr("Cannot start print: not connected to printer"));
        if (on_completion) {
            on_completion(false, "Not connected to printer");
        }
        return;
    }

    // Mark this as an in-app print for telemetry source tracking
    TelemetryManager::instance().notify_print_started_in_app();

    // No double-tap guard here any more. The flag it used to read is now
    // published by PrinterPrintState from the preparing job, and the caller arms
    // that job immediately before calling us - so this check would only ever see
    // the job it was just handed and would reject every print. The guard belongs
    // at the arming site, and PrintStartController::start_now() already runs it
    // (can_start_new_print(), before begin_preparing()).

    // The in-progress flag is published by PrinterPrintState from the preparing
    // job itself, so there is no longer a wrapper clearing it on every exit.
    PrintCompletionCallback wrapped_completion = on_completion;

    // Build full path for print
    std::string filename_to_print = current_path.empty() ? filename : current_path + "/" + filename;

    // Read checkbox states for logging and timelapse
    PrePrintOptions options = read_options_from_subjects();

    spdlog::debug(
        "[PrintPreparationManager] Starting print: {} (pre-print options: mesh={}, qgl={}, "
        "z_tilt={}, clean={}, timelapse={})",
        filename_to_print, options.bed_mesh, options.qgl, options.z_tilt, options.nozzle_clean,
        options.timelapse);

    // Dispatch RuntimeCommand-strategy options. Currently used for the
    // dynamically-synthesized `timelapse` option; the sentinel commands
    // "timelapse:on" / "timelapse:off" are recognized here and routed to the
    // moonraker-timelapse API. Other RuntimeCommand options will need their
    // own dispatch arms.
    const auto& db_options_for_runtime = get_cached_options();
    for (const auto& opt : db_options_for_runtime.options) {
        if (opt.strategy_kind != PrePrintStrategyKind::RuntimeCommand) {
            continue;
        }
        const auto* cmd = std::get_if<PrePrintStrategyRuntimeCommand>(&opt.strategy);
        if (!cmd) {
            continue;
        }
        const PrePrintOptionState state = get_option_state(opt.id);
        if (state == PrePrintOptionState::NOT_APPLICABLE) {
            continue;
        }
        const bool enabled = (state == PrePrintOptionState::ENABLED);
        const std::string& sentinel = enabled ? cmd->command_enabled : cmd->command_disabled;
        if (sentinel == "timelapse:on" || sentinel == "timelapse:off") {
            const bool tl_enable = (sentinel == "timelapse:on");
            api_->timelapse().set_timelapse_enabled(
                tl_enable,
                [tl_enable]() {
                    spdlog::info("[PrintPreparationManager] Timelapse {} for this print",
                                 tl_enable ? "enabled" : "disabled");
                },
                [](const MoonrakerError& err) {
                    spdlog::error("[PrintPreparationManager] Failed to update timelapse state: {}",
                                  err.message);
                });
        } else if (!sentinel.empty()) {
            spdlog::warn("[PrintPreparationManager] Unhandled RuntimeCommand sentinel '{}' for "
                         "option '{}' — ignoring",
                         sentinel, opt.id);
        }
    }

    // Check if user disabled operations that are embedded in the G-code file
    std::vector<gcode::OperationType> ops_to_disable = collect_ops_to_disable();

    // Check if user disabled operations that are in the PRINT_START macro
    // These need skip params appended to the PRINT_START call
    std::vector<std::pair<std::string, std::string>> macro_skip_params =
        collect_macro_skip_params();

    // Pre-start gcode mechanism. Two distinct sources combine into a single
    // pre-START_PRINT gcode call:
    //
    //   1. Printer-level `setup_gcode` (e.g. K1/K1C "PRINT_PREPARED"). Fires
    //      unconditionally when its trigger condition is met (skip params
    //      present) — its purpose is to set up macro variables that can't be
    //      passed as START_PRINT params. The PREPARE param in
    //      printer_database.json serves as a sentinel: it makes the checkbox
    //      visible and causes collect_macro_skip_params() to return
    //      non-empty, gating entry into this path. The param value is never
    //      sent.
    //
    //   2. Per-option PreStartGcode strategy lines (e.g. K2 Plus
    //      "LOAD_AI_RUN SWITCH=1"). Fired per-option whether enabled or
    //      disabled — see collect_pre_start_gcode_lines().
    //
    // Lines are concatenated with newlines; Moonraker forwards the whole
    // block to Klipper as a single gcode_script.
    const auto& db_options = get_cached_options();
    std::vector<std::string> pre_start_lines = collect_pre_start_gcode_lines(filename_to_print);
    // A skip an earlier print left unconsumed must not ride into this one,
    // including for a step whose toggle is hidden now.
    if (printer_state_ && !printer_state_->get_discovery().skip_active().empty()) {
        pre_start_lines.insert(pre_start_lines.begin(), helix::skip_wrappers::PREP_MACRO);
    }
    const bool emit_printer_setup = !macro_skip_params.empty() && !db_options.setup_gcode.empty();
    std::string combined =
        build_pre_start_gcode_block(db_options.setup_gcode, pre_start_lines, emit_printer_setup);

    if (!combined.empty()) {
        spdlog::info("[PrintPreparationManager] Executing pre-start gcode ({} line(s)): {}",
                     (emit_printer_setup ? 1 : 0) + pre_start_lines.size(), combined);

        // This block runs in front of the job, so it sits inside the pre-print
        // measurement window. Tell the collector, or its timings get averaged
        // with printer-edge measurements that never included one - which both
        // skews the displayed estimate and feeds a too-small predicted total
        // into the collector's own adaptive timeout.
        if (auto* mgr = get_moonraker_manager()) {
            if (auto collector = mgr->print_start_collector()) {
                collector->note_host_side_pre_start(combined);
            }
        }

        auto token = lifetime_.token();
        pre_start_sent_at_ = std::chrono::steady_clock::now();
        // Snapshot the intent this block is being sent on behalf of. A cancel,
        // a failure, or another print taking over all move the epoch, so the
        // ack can be judged on whether the job it belongs to still exists
        // rather than on how long it took to arrive.
        pre_start_epoch_ =
            printer_state_
                ? lv_subject_get_int(printer_state_->print_state().get_preparing_epoch_subject())
                : 0;
        // The busy gate must not queue this send fire-and-forget: its on_success
        // is the only trigger that launches the job, so a discretionary block
        // (a pre_start_gcode heater template) would never fire it and the print
        // would never start.
        api_->execute_gcode(
            combined,
            [this, token, filename_to_print, ops_to_disable, on_navigate_to_status,
             wrapped_completion]() {
                token.defer("PrintPreparationManager::pre_start_gcode_success",
                            [this, filename_to_print, ops_to_disable, on_navigate_to_status,
                             wrapped_completion]() {
                                spdlog::info("[PrintPreparationManager] Pre-start gcode executed");
                                continue_print_start(filename_to_print, ops_to_disable,
                                                     on_navigate_to_status, wrapped_completion);
                            });
            },
            [this, token, filename_to_print, ops_to_disable, on_navigate_to_status,
             wrapped_completion](const MoonrakerError& err) {
                token.defer("PrintPreparationManager::pre_start_gcode_error",
                            [this, filename_to_print, ops_to_disable, on_navigate_to_status,
                             wrapped_completion, err]() {
                                handle_pre_start_gcode_error(err, filename_to_print, ops_to_disable,
                                                             on_navigate_to_status,
                                                             wrapped_completion);
                            });
            },
            IMoonrakerAPI::PRE_START_MACRO_TIMEOUT_MS,
            /*silent=*/false, /*on_queued=*/nullptr, /*caller_surfaces_errors=*/true,
            /*bypass_busy_gate=*/true);
        return;
    }

    // Determine if we need to modify the G-code file
    bool needs_file_modification = !ops_to_disable.empty();
    bool needs_macro_params = !macro_skip_params.empty();

    if (needs_file_modification || needs_macro_params) {
        helix::MemoryMonitor::log_now("print_modification_start", spdlog::level::debug);
        if (!can_modify_gcode()) {
            warn_modifications_dropped(ops_to_disable);
            // Clear modifications so we fall through to normal print path
            ops_to_disable.clear();
            macro_skip_params.clear();
        } else {
            spdlog::info("[PrintPreparationManager] Modifying G-code server-side: {} file ops, "
                         "{} macro params",
                         ops_to_disable.size(), macro_skip_params.size());
            modify_and_print(filename_to_print, ops_to_disable, macro_skip_params,
                             on_navigate_to_status);
            return; // modify_and_print handles everything including navigation
        }
    }

    // CHECKED checkboxes = trust the macro to handle the operation (do nothing extra)
    // UNCHECKED checkboxes = already handled above via file modification or skip params
    // No need for manual G-code execution - just start the print
    start_print_directly(filename_to_print, on_navigate_to_status, wrapped_completion);
}

bool PrintPreparationManager::is_print_in_progress() const {
    return printer_state_ && printer_state_->print_state().is_print_in_progress();
}

// ============================================================================
// Internal Methods
// ============================================================================

namespace {

// The four pre-print ops that can be embedded directly in a sliced G-code file
// (as opposed to being handled purely inside the START_PRINT macro). Disabling
// one of these when the file embeds it forces a file modification. Single
// source of truth shared by collect_ops_to_disable() and
// disabling_option_requires_plugin() so the id->OperationType mapping can't
// drift between them.
std::optional<gcode::OperationType> file_embeddable_op_for_id(const std::string& id) {
    if (id == "bed_mesh")
        return gcode::OperationType::BED_MESH;
    if (id == "qgl")
        return gcode::OperationType::QGL;
    if (id == "z_tilt")
        return gcode::OperationType::Z_TILT;
    if (id == "nozzle_clean")
        return gcode::OperationType::NOZZLE_CLEAN;
    return std::nullopt;
}

// Whether turning this option off may strip its op out of the sliced file. Not
// when a self-storing firmware holds the option's value (supplied through
// preprint_prefs::read_persisted_defaults()): that firmware skips the file's
// own command when its setting is off, so the option only drives its
// pre-start line. Nor for a leveling-skip toggle: its wrapper intercepts the
// file's own command too. Every other option may.
bool option_may_strip_file(const PrePrintOption& opt) {
    return !opt.default_from_firmware && !helix::skip_wrappers::is_wrapper_option(opt);
}

// Transfer callbacks run on the HTTP thread. BusyOverlay is process-wide, so
// these updates belong to no object and still run if the manager is gone.
void queue_busy_hide() {
    helix::ui::queue_update("PrintPreparationManager::busy_hide", []() { BusyOverlay::hide(); });
}

} // namespace

std::vector<gcode::OperationType> PrintPreparationManager::collect_ops_to_disable() const {
    std::vector<gcode::OperationType> ops_to_disable;

    if (!cached_scan_result_.has_value()) {
        return ops_to_disable; // No scan result, nothing to disable
    }

    // Check each operation type: if file has it embedded AND user explicitly disabled it
    // Note: hidden (NOT_APPLICABLE) options are NOT candidates for disabling.
    // State resolution flows through the new framework via get_option_state(id).
    for (const char* id : {"bed_mesh", "qgl", "z_tilt", "nozzle_clean"}) {
        const std::optional<gcode::OperationType> op = file_embeddable_op_for_id(id);
        if (get_option_state(id) != PrePrintOptionState::DISABLED ||
            !cached_scan_result_->has_operation(*op)) {
            continue;
        }
        const PrePrintOption* opt = get_cached_options().find(id);
        if (opt && !option_may_strip_file(*opt)) {
            spdlog::debug("[PrintPreparationManager] '{}' is gated by the firmware's stored "
                          "setting, leaving the file's embedded op alone",
                          id);
            continue;
        }
        ops_to_disable.push_back(*op);
        spdlog::debug("[PrintPreparationManager] User disabled '{}', file has it embedded", id);
    }

    return ops_to_disable;
}

bool PrintPreparationManager::disabling_option_requires_plugin(const PrePrintOption& opt) const {
    const bool is_macro_param = (opt.strategy_kind == PrePrintStrategyKind::MacroParam);

    // (a) Does disabling THIS option force a file modification because the op is
    //     embedded in the currently-scanned file?
    const std::optional<gcode::OperationType> embedded_op = file_embeddable_op_for_id(opt.id);
    const bool file_embedded = embedded_op.has_value() && cached_scan_result_.has_value() &&
                               cached_scan_result_->has_operation(*embedded_op) &&
                               option_may_strip_file(opt);

    // (b) is the MacroParam skip-rewrite path. If neither (a) nor a MacroParam
    //     skip is in play, nothing about this option needs the plugin.
    if (!is_macro_param && !file_embedded) {
        return false;
    }

    // Does disabling it still do something without the plugin? A PreStartGcode
    // option always emits its own line. A MacroParam skip rides a pre-start
    // block: printer-level setup_gcode (gated on a MacroParam skip, see
    // emit_printer_setup in start_print()) or any PreStartGcode line. This is
    // the K2 Plus PREPARE case (MacroParam bed_mesh + setup_gcode), which must
    // stay visible without the plugin.
    const auto& option_set = get_cached_options();
    const bool pre_start_block =
        !option_set.setup_gcode.empty() || !collect_pre_start_gcode_lines({}, false).empty();
    if (opt.strategy_kind == PrePrintStrategyKind::PreStartGcode ||
        (is_macro_param && pre_start_block)) {
        return false;
    }

    // Left: an embedded-op strip or a MacroParam rewrite of the START_PRINT
    // call, both of which every start path drops when the plugin is absent.
    return true;
}

void PrintPreparationManager::warn_modifications_dropped(
    const std::vector<gcode::OperationType>& ops_to_disable) const {
    // Name the features being dropped: "Cannot modify G-code" alone leaves the
    // user guessing which of the print dialog's controls it refers to (#1269).
    const std::string dropped = describe_dropped_modifications(ops_to_disable);
    if (!transport_keeps_local_copies()) {
        // Installing the plugin would change nothing here, so it goes unnamed.
        spdlog::warn("[PrintPreparationManager] Transport keeps no local copy - skipping "
                     "modification, printing original file");
        if (dropped.empty()) {
            NOTIFY_WARNING(
                lv_tr("Modifying G-code is not available on this device. Printing original file."));
        } else {
            NOTIFY_WARNING(lv_tr("{} is not available on this device. Printing original file."),
                           dropped);
        }
        return;
    }
    spdlog::warn("[PrintPreparationManager] No HelixPrint plugin - skipping modification, "
                 "printing original file");
    if (dropped.empty()) {
        // A fixed sentence rather than an interpolated reason, so no English
        // fragment shows up in other locales.
        NOTIFY_WARNING(
            lv_tr("Modifying G-code needs the HelixPrint plugin. Printing original file."));
    } else {
        NOTIFY_WARNING(lv_tr("{} needs the HelixPrint plugin. Printing original file."), dropped);
    }
}

// Translated, comma-joined names of the features a dropped modification would
// have carried, for the "needs the HelixPrint plugin" warning. Uses the same
// label the toggle row shows, so the message points at something the user can
// recognize in the dialog they just came from.
std::string PrintPreparationManager::describe_dropped_modifications(
    const std::vector<gcode::OperationType>& ops_to_disable) const {
    std::vector<std::string> names;
    std::set<std::string> covered;

    for (const auto& opt : get_cached_options().options) {
        const PrePrintOptionState state = get_option_state(opt.id);

        // (a) a file-embeddable op this print was going to strip out
        const std::optional<gcode::OperationType> embedded_op = file_embeddable_op_for_id(opt.id);
        const bool strips_embedded_op =
            embedded_op.has_value() && std::find(ops_to_disable.begin(), ops_to_disable.end(),
                                                 *embedded_op) != ops_to_disable.end();

        // (b) a MacroParam skip rewritten into the START_PRINT call
        const bool rewrites_macro_param =
            opt.strategy_kind == PrePrintStrategyKind::MacroParam &&
            (state == PrePrintOptionState::DISABLED ||
             (opt.adaptive_active && state == PrePrintOptionState::ENABLED));

        if (strips_embedded_op || rewrites_macro_param) {
            names.push_back(PrePrintOptionsRenderer::label_for(opt));
            covered.insert(opt.id);
        }
    }

    // LAYER 2 mirror: collect_macro_skip_params() also emits for ops the DB
    // never declared, picked up from PRINT_START analysis.
    for (const auto& entry : MACRO_OPTION_IDS) {
        if (covered.count(entry.id) ||
            get_option_state(entry.id) != PrePrintOptionState::DISABLED) {
            continue;
        }
        if (auto synthetic = macro_option_for(macro_analysis_, entry)) {
            names.push_back(PrePrintOptionsRenderer::label_for(*synthetic));
        }
    }

    std::string joined;
    for (const auto& name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

// Adaptive meshing is the one case where an ENABLED option emits skip params.
// Every other emitter fires on DISABLED, and those options are hidden up front
// when the plugin is missing (disabling_option_requires_plugin() ->
// PrintSelectDetailView's visibility_lookup). The adaptive pair has no such
// gate: bed_mesh ENABLED is the default, so on a plugin-less printer with no
// pre-start mechanism start_print() collected the params, found it could not
// rewrite the file, dropped them, and warned - on every print, for a state the
// user never chose and no visible toggle could change (#1269).
//
// So emit only when the params can actually be delivered. The three arms mirror
// disabling_option_requires_plugin() exactly:
//   - plugin present    -> modify_and_print() rewrites the PRINT_START call
//   - setup_gcode set   -> non-empty skip params trigger the printer-level
//                          pre-start block (emit_printer_setup in start_print),
//                          which is how K1/K1C PRINT_PREPARED fires. Suppressing
//                          here would silently disarm that trigger.
//   - pre-start lines   -> the same pre-start path fires for per-option
//                          PreStartGcode strategies.
// Otherwise the emit is pure noise: the unmodified file still prints and the
// macro runs its own default mesh, which is exactly what happens today after
// the drop - minus the warning.
bool PrintPreparationManager::adaptive_emit_is_deliverable() const {
    if (can_modify_gcode()) {
        return true;
    }
    if (!get_cached_options().setup_gcode.empty()) {
        return true;
    }
    return !collect_pre_start_gcode_lines({}, false).empty();
}

std::vector<std::pair<std::string, std::string>>
PrintPreparationManager::collect_macro_skip_params() const {
    // THREADING: This method reads macro_analysis_ and checkbox states.
    // Must be called from the main LVGL thread (same thread that updates these via
    // ui_queue_update() callbacks). LVGL's single-threaded model ensures no races.

    std::vector<std::pair<std::string, std::string>> skip_params;
    std::set<std::string> handled_ids; // option ids already covered by DB

    // LAYER 1: Database options. Authoritative per-printer mapping (e.g. K2 Plus
    // bed_mesh → PREPARE=1). When the DB declares an option, its handling
    // takes precedence over macro analysis for that same option id.
    const auto& db_options = get_cached_options();
    if (!db_options.empty()) {
        for (const auto& opt : db_options.options) {
            // Only add skip param when user explicitly disabled the option
            // (visible + unchecked). Hidden / not-applicable means the
            // printer doesn't support the op and skip params shouldn't be
            // appended.
            if (get_option_state(opt.id) != PrePrintOptionState::DISABLED) {
                // Adaptive bed mesh: the bed_mesh toggle is relabeled "Adaptive
                // Bed Mesh" when adaptive_active (set by apply_dynamic_options:
                // adaptive_param present + exclude_object + no custom template).
                // When the toggle is ENABLED, emit the adaptive token (e.g.
                // ADAPTIVE=1) ALONGSIDE the enable param so START_PRINT forwards
                // it into BED_MESH_CALIBRATE. Gated on ENABLED — never on skip.
                if (opt.adaptive_active && opt.strategy_kind == PrePrintStrategyKind::MacroParam &&
                    get_option_state(opt.id) == PrePrintOptionState::ENABLED) {
                    const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&opt.strategy);
                    if (macro && !macro->adaptive_param.empty()) {
                        if (adaptive_emit_is_deliverable()) {
                            // Emit BOTH the enable param and the adaptive token, e.g.
                            // SKIP_LEVELING=0 ADAPTIVE=1. The enable param is normally
                            // omitted on ENABLED (macro default), but adaptive meshing
                            // must explicitly run the mesh, so make it unambiguous.
                            skip_params.emplace_back(macro->param_name, macro->enable_value);
                            skip_params.emplace_back(macro->adaptive_param, macro->adaptive_value);
                            spdlog::debug(
                                "[PrintPreparationManager] Adaptive bed mesh: {}={} {}={} "
                                "(id={})",
                                macro->param_name, macro->enable_value, macro->adaptive_param,
                                macro->adaptive_value, opt.id);
                        } else {
                            spdlog::debug("[PrintPreparationManager] Adaptive bed mesh params "
                                          "suppressed (id={}): the PRINT_START rewrite that would "
                                          "carry them is unreachable, so start_print() would drop "
                                          "them and warn about a state the user never chose",
                                          opt.id);
                        }
                    }
                }
                // Even when ENABLED/NOT_APPLICABLE, the DB has spoken for this
                // id — don't let macro analysis emit a duplicate param under
                // the assumption the DB didn't cover it.
                handled_ids.insert(opt.id);
                continue;
            }

            switch (opt.strategy_kind) {
            case PrePrintStrategyKind::MacroParam: {
                const auto* macro = std::get_if<PrePrintStrategyMacroParam>(&opt.strategy);
                if (macro) {
                    skip_params.emplace_back(macro->param_name, macro->skip_value);
                    spdlog::debug("[PrintPreparationManager] DB param: {}={} (id={})",
                                  macro->param_name, macro->skip_value, opt.id);
                }
                handled_ids.insert(opt.id);
                break;
            }
            case PrePrintStrategyKind::PreStartGcode:
                // PreStartGcode is handled by collect_pre_start_gcode_lines();
                // it fires before START_PRINT as a separate gcode call rather
                // than appending KEY=value tokens to the macro invocation.
                handled_ids.insert(opt.id);
                break;
            case PrePrintStrategyKind::RuntimeCommand:
                // RuntimeCommand options dispatch in start_print() (e.g.
                // timelapse:on/off → MoonrakerTimelapseAPI), not via macro
                // skip params. Mark handled so macro analysis layer below
                // doesn't double-emit a SKIP param for the same id.
                handled_ids.insert(opt.id);
                break;
            case PrePrintStrategyKind::QueueAheadJob:
                spdlog::warn("[PrintPreparationManager] Option '{}' uses strategy not yet wired up "
                             "(QueueAheadJob). Ignoring.",
                             opt.id);
                handled_ids.insert(opt.id);
                break;
            }
        }
    }

    // LAYER 2: Macro analysis. Picks up ops the DB didn't cover (e.g. QGL on a
    // Voron whose entry only declares bed_mesh). DB-handled ids are skipped to
    // avoid double-emission.
    for (const auto& entry : MACRO_OPTION_IDS) {
        if (handled_ids.count(entry.id) ||
            get_option_state(entry.id) != PrePrintOptionState::DISABLED) {
            continue;
        }
        const auto opt = macro_option_for(macro_analysis_, entry);
        const auto* param = opt ? std::get_if<PrePrintStrategyMacroParam>(&opt->strategy) : nullptr;
        if (param) {
            skip_params.emplace_back(param->param_name, param->skip_value);
            spdlog::debug("[PrintPreparationManager] Macro-analysis param: {}={} (id={})",
                          param->param_name, param->skip_value, entry.id);
        }
    }

    if (!skip_params.empty()) {
        spdlog::info("[PrintPreparationManager] Collected {} skip params (DB+macro combined)",
                     skip_params.size());
    }

    return skip_params;
}

std::vector<std::string>
PrintPreparationManager::collect_pre_start_gcode_lines(const std::string& filename,
                                                       bool with_skip_toggles) const {
    std::vector<std::string> lines;

    const auto& db_options = get_cached_options();
    if (db_options.options.empty()) {
        return lines;
    }

    for (const auto& opt : db_options.options) {
        if (opt.strategy_kind != PrePrintStrategyKind::PreStartGcode ||
            (!with_skip_toggles && helix::skip_wrappers::is_wrapper_option(opt))) {
            continue;
        }

        // Skip options whose required macro isn't registered with Klipper
        // (e.g. K2 AI detect uses LOAD_AI_RUN, which only exists on Creality
        // OS variants — sending it on stock K2 fires "Unknown command:key61").
        if (is_macro_gate_closed(opt)) {
            spdlog::debug("[PrintPreparationManager] Skipping pre-start option '{}': "
                          "required macro '{}' not registered",
                          opt.id, opt.requires_macro);
            continue;
        }

        // Skip options that don't apply to this printer (hidden by visibility,
        // or not bound to any UI row and absent from the cached set's
        // capability list — unlikely here since we just pulled from the set).
        const PrePrintOptionState state = get_option_state(opt.id);
        if (state == PrePrintOptionState::NOT_APPLICABLE) {
            continue;
        }

        const bool enabled = (state == PrePrintOptionState::ENABLED);

        // Macros with no "off" form opt out of the disabled emission entirely:
        // sending Creality's BED_MESH_CALIBRATE_START_PRINT with a 0 would
        // still mesh, so "disabled" has to mean sending nothing at all.
        const auto* pre = std::get_if<PrePrintStrategyPreStartGcode>(&opt.strategy);
        if (!enabled && pre && !pre->emit_when_disabled) {
            spdlog::debug("[PrintPreparationManager] Option '{}' disabled and has no off form "
                          "— emitting nothing",
                          opt.id);
            continue;
        }

        // Job temps come from the file's own START_PRINT line via the scan
        // cache — Moonraker metadata is wrong on multi-material files. Only
        // trust the cache when it was built for the file being printed;
        // otherwise 0 lets the firmware macro keep its own default.
        PreStartGcodeContext ctx;
        ctx.filename = filename;
        if (const auto* start = print_start_for(filename)) {
            ctx.bed_temp = start->bed_temp;
            ctx.extruder_temp = start->extruder_temp;
        }
        std::string line = render_pre_start_gcode(opt, enabled, ctx);
        if (line.empty()) {
            // render_pre_start_gcode logs its own warning on type mismatch.
            continue;
        }
        spdlog::debug("[PrintPreparationManager] Pre-start gcode for option '{}' (enabled={}): {}",
                      opt.id, enabled, line);
        lines.push_back(std::move(line));
    }

    if (!lines.empty()) {
        spdlog::info("[PrintPreparationManager] Collected {} pre-start gcode line(s)",
                     lines.size());
    }
    return lines;
}

void PrintPreparationManager::continue_print_start(
    const std::string& filename, const std::vector<gcode::OperationType>& ops_to_disable,
    NavigateToStatusCallback on_navigate_to_status, PrintCompletionCallback on_completion) {
    // Every pre-start path funnels through here before a job is actually
    // started, which makes it the one place a cancellation can be honoured.
    //
    // PrintStartController arms the preparing job before delegating here, so by
    // the time this runs there is always one - unless the user cancelled while a
    // blocking pre-start macro was running, or another print superseded ours.
    // A pre-start macro can run for ten minutes; starting the job after the user
    // has already cancelled it is the worst outcome available.
    if (armed_at_start_ && printer_state_ && !printer_state_->print_state().has_preparing_job()) {
        spdlog::info(
            "[PrintPreparationManager] Start abandoned - '{}' is no longer being "
            "prepared ({})",
            filename,
            helix::preparing_exit_name(printer_state_->print_state().last_preparing_exit()));
        if (on_completion) {
            on_completion(false, "");
        }
        return;
    }

    // Intent guard. The preparing-job check above catches a job that is simply
    // gone; this catches the subtler case it cannot see — the job being a
    // DIFFERENT one than the block was sent for, because ours was retired and
    // another print armed in its place. PrinterPrintState bumps the epoch on
    // begin_preparing and zeroes it on retire_preparing, so cancel, failure and
    // supersede all land here.
    //
    // This is what the old 3-minute staleness bound was really reaching for.
    // The bug it was written against (K1C 2026-08-20: klippy flushed a 370s
    // backed-up ack at cancel time and relaunched the cancelled print) is a
    // question about INTENT, and elapsed time is a poor proxy for it — as a K2
    // Plus proved on 2026-08-25, where a pre-start block containing a bed mesh
    // legitimately ran 494s, succeeded, and had its print silently dropped.
    // A late ack and a slow-but-wanted macro are the same code path with the
    // same signature; only the epoch tells them apart.
    if (pre_start_epoch_ != 0 && printer_state_) {
        const int now_epoch =
            lv_subject_get_int(printer_state_->print_state().get_preparing_epoch_subject());
        if (now_epoch != pre_start_epoch_) {
            spdlog::warn("[PrintPreparationManager] Dropping pre-start completion for a retired "
                         "job (epoch {} -> {}) - not starting '{}'",
                         pre_start_epoch_, now_epoch, filename);
            if (on_completion) {
                on_completion(false, "");
            }
            return;
        }
    }

    // Time backstop, for the case with NO intent signal at all: a caller that
    // armed no preparing job leaves nothing to compare, so the only remaining
    // question is whether this ack is plausibly fresh. Deliberately NOT applied
    // when an epoch was captured — there the check above is exact, and a clock
    // can only overrule it wrongly.
    if (pre_start_epoch_ == 0 && pre_start_sent_at_ != std::chrono::steady_clock::time_point{}) {
        constexpr auto STALE_PRE_START_BOUND = std::chrono::minutes(3);
        const auto age = std::chrono::steady_clock::now() - pre_start_sent_at_;
        if (age > STALE_PRE_START_BOUND) {
            spdlog::warn("[PrintPreparationManager] Dropping stale pre-start completion "
                         "({}s old, bound 180s, no preparing job to check) - not starting '{}'",
                         std::chrono::duration_cast<std::chrono::seconds>(age).count(), filename);
            if (on_completion) {
                on_completion(false, "");
            }
            return;
        }
    }

    if (ops_to_disable.empty()) {
        start_print_directly(filename, on_navigate_to_status, on_completion);
    } else if (can_modify_gcode()) {
        modify_and_print(filename, ops_to_disable, {}, on_navigate_to_status);
    } else {
        warn_modifications_dropped(ops_to_disable);
        start_print_directly(filename, on_navigate_to_status, on_completion);
    }
}

void PrintPreparationManager::handle_pre_start_gcode_error(
    const MoonrakerError& error, const std::string& filename,
    const std::vector<gcode::OperationType>& ops_to_disable,
    NavigateToStatusCallback on_navigate_to_status, PrintCompletionCallback on_completion) {
    // The RPC ceiling is not the printer's ceiling: execute_gcode blocks until
    // the macro finishes, and a long pre-start macro can outlive the request.
    // A timeout or dropped socket while Klipper still reports idle_timeout
    // "Printing" means the macro is still running — fail only once the printer
    // itself stops (prestonbrown/helixscreen#1543: the transport vanishing is
    // not the printer's opinion of the macro).
    if ((error.type == MoonrakerErrorType::TIMEOUT ||
         error.type == MoonrakerErrorType::CONNECTION_LOST) &&
        printer_state_ &&
        lv_subject_get_int(
            printer_state_->calibration_state().get_idle_timeout_printing_subject()) == 1) {
        begin_pre_start_completion_wait(error, filename, ops_to_disable, on_navigate_to_status,
                                        on_completion);
        return;
    }

    spdlog::error("[PrintPreparationManager] Pre-start gcode failed: {} ({})", error.message,
                  error.get_type_string());
    NOTIFY_ERROR(lv_tr("Pre-print command failed: {}"), error.message);
    if (on_completion) {
        on_completion(false, error.message);
    }
}

void PrintPreparationManager::begin_pre_start_completion_wait(
    const MoonrakerError& timeout_error, const std::string& filename,
    const std::vector<gcode::OperationType>& ops_to_disable,
    NavigateToStatusCallback on_navigate_to_status, PrintCompletionCallback on_completion) {
    spdlog::warn("[PrintPreparationManager] Pre-start RPC timed out ({}) but Klipper is still "
                 "executing it - waiting for the busy->idle edge before starting the print",
                 timeout_error.message);
    // This wait is measured in minutes by design (long pre-start macros), so
    // the send-time staleness bound must no longer apply when the wait ends
    // in continue_print_start - only the preparing-job guard judges this path.
    pre_start_sent_at_ = {};
    pre_start_wait_active_ = true;

    // Backstop: the macro already had a full ceiling on the RPC side. If the
    // printer is STILL busy after another one, something is wedged - fail
    // rather than spin forever. An edge that landed between the last subject
    // update and this timer firing is handled by re-reading the subject: idle
    // means the macro finished and we can still start the print.
    pre_start_wait_guard_.begin(
        IMoonrakerAPI::PRE_START_MACRO_TIMEOUT_MS,
        [this, filename, ops_to_disable, on_navigate_to_status, on_completion, timeout_error]() {
            const bool still_busy =
                printer_state_ &&
                lv_subject_get_int(
                    printer_state_->calibration_state().get_idle_timeout_printing_subject()) == 1;
            finish_pre_start_wait();
            if (!still_busy) {
                spdlog::info("[PrintPreparationManager] Pre-start macro finished "
                             "(backstop re-check) - starting print");
                continue_print_start(filename, ops_to_disable, on_navigate_to_status,
                                     on_completion);
                return;
            }
            spdlog::error("[PrintPreparationManager] Pre-start macro still "
                          "executing after backstop - aborting print start");
            NOTIFY_ERROR(lv_tr("Pre-print command failed: {}"),
                         "printer still busy after extended wait");
            if (on_completion) {
                on_completion(false, timeout_error.message);
            }
        });

    // The busy->idle edge is the normal completion signal. observe<int>
    // defers the handler through UpdateQueue, so the observer can be torn down
    // from inside the handler without re-entrancy.
    pre_start_wait_observer_ = helix::ui::observe<int>(
        printer_state_->calibration_state().get_idle_timeout_printing_subject(), this,
        [this, filename, ops_to_disable, on_navigate_to_status,
         on_completion](PrintPreparationManager* self, int busy) {
            if (!self->pre_start_wait_active_ || busy == 1) {
                return;
            }
            self->finish_pre_start_wait();
            spdlog::info("[PrintPreparationManager] Pre-start macro finished (idle edge) - "
                         "starting print");
            self->continue_print_start(filename, ops_to_disable, on_navigate_to_status,
                                       on_completion);
        },
        printer_state_->get_subjects_lifetime());
}

void PrintPreparationManager::finish_pre_start_wait() {
    pre_start_wait_active_ = false;
    // Observer first: after reset() a queued stale apply finds the guard's
    // epoch dead and no-ops, so a leftover notification cannot re-enter.
    pre_start_wait_observer_.reset();
    pre_start_wait_guard_.end();
}

std::string PrintPreparationManager::build_pre_start_gcode_block(
    const std::string& setup_gcode, const std::vector<std::string>& pre_start_lines,
    bool emit_setup) {
    std::string combined;
    if (emit_setup && !setup_gcode.empty()) {
        combined = setup_gcode;
    }
    for (const auto& line : pre_start_lines) {
        if (!combined.empty()) {
            combined += '\n';
        }
        combined += line;
    }
    return combined;
}

void PrintPreparationManager::abandon_start(const char* where) {
    if (!printer_state_) {
        return;
    }
    spdlog::warn("[PrintPreparationManager] Start abandoned at {} - retiring the preparing job",
                 where);
    printer_state_->print_state().retire_preparing(helix::PreparingExit::Failed);
}

void PrintPreparationManager::modify_and_print(
    const std::string& file_path, const std::vector<gcode::OperationType>& ops_to_disable,
    const std::vector<std::pair<std::string, std::string>>& macro_skip_params,
    NavigateToStatusCallback on_navigate_to_status) {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("Cannot start print: not connected to printer"));
        abandon_start("modify_no_api");
        return;
    }

    if (!cached_scan_result_.has_value()) {
        spdlog::error("[PrintPreparationManager] modify_and_print called without scan result");
        NOTIFY_ERROR(lv_tr("Internal error: no scan result"));
        abandon_start("modify_no_scan_result");
        return;
    }

    spdlog::info("[PrintPreparationManager] Modifying G-code: {} file ops to disable, {} macro "
                 "skip params",
                 ops_to_disable.size(), macro_skip_params.size());

    // Extract just the filename for display purposes
    size_t last_slash = file_path.rfind('/');
    std::string display_filename =
        (last_slash != std::string::npos) ? file_path.substr(last_slash + 1) : file_path;

    // Build modification identifiers for plugin
    std::vector<std::string> mod_names;
    for (const auto& op : ops_to_disable) {
        mod_names.push_back(gcode::GCodeOpsDetector::operation_type_name(op) + "_disabled");
    }
    // Add skip params to mod_names for tracking
    for (const auto& [param_name, param_value] : macro_skip_params) {
        mod_names.push_back("skip_" + param_name);
    }

    // UNIFIED STREAMING PATH: Always use streaming to avoid memory spikes
    // 1. Download to disk (streaming)
    // 2. Modify on disk (file-to-file, minimal memory)
    // 3. Upload modified file to server
    // 4. If plugin available: use path-based API for symlink/history patching
    //    Otherwise: use standard start_print
    //
    // This prevents TTC errors on memory-constrained devices like AD5M (~108MB RAM)
    // by never loading the entire G-code file into memory.
    bool has_plugin =
        printer_state_ && printer_state_->plugin_status_state().service_has_helix_plugin();
    spdlog::info("[PrintPreparationManager] Using unified streaming modification flow (plugin: {})",
                 has_plugin);
    modify_and_print_streaming(file_path, display_filename, ops_to_disable, macro_skip_params,
                               mod_names, on_navigate_to_status, has_plugin);
}

void PrintPreparationManager::modify_and_print_streaming(
    const std::string& file_path, const std::string& display_filename,
    const std::vector<gcode::OperationType>& ops_to_disable,
    const std::vector<std::pair<std::string, std::string>>& macro_skip_params,
    const std::vector<std::string>& mod_names, NavigateToStatusCallback on_navigate_to_status,
    bool use_plugin) {
    auto token = lifetime_.token();         // Capture for lifetime checking in async callbacks
    auto scan_result = cached_scan_result_; // Copy for lambda capture

    // Validate scan_result before proceeding (SERIOUS-3 fix)
    if (!scan_result.has_value()) {
        NOTIFY_ERROR(lv_tr("Cannot modify G-code: scan result not available"));
        abandon_start("streaming_no_scan_result");
        return;
    }

    // Get temp directory for intermediate files
    std::string temp_dir = get_temp_directory();
    if (temp_dir.empty()) {
        NOTIFY_ERROR(lv_tr("Cannot modify G-code: no temp directory available"));
        abandon_start("streaming_no_temp_dir");
        return;
    }

    // Generate unique temp file paths
    auto timestamp = std::to_string(std::time(nullptr));
    std::string local_download_path = temp_dir + "/helix_download_" + timestamp + ".gcode";
    std::string remote_temp_path = gcode::make_rewritten_gcode_path(file_path);

    spdlog::info("[PrintPreparationManager] Streaming modification: downloading to {}",
                 local_download_path);

    // Show busy overlay (will appear after 300ms grace period if operation takes that long)
    BusyOverlay::show("Preparing print...");

    // Progress callback for download - NOTE: called from HTTP thread
    auto download_progress = [](size_t received, size_t total) {
        BusyOverlay::queue_progress("Downloading", received, total);
    };

    // Step 1: Download file to disk (streaming, not memory)
    api_->transfers().download_file_to_path(
        "gcodes", file_path, local_download_path,
        // Download success - NOTE: runs on HTTP thread.
        // L081 Mechanism C: bg-safe work (gcode modification + filesystem
        // cleanup) runs locally; the `this->` accesses (printer_state_,
        // api_->) are wrapped in token.defer().
        [this, token, file_path, display_filename, ops_to_disable, macro_skip_params, mod_names,
         scan_result, local_download_path, remote_temp_path, on_navigate_to_status,
         use_plugin](const std::string& /*dest_path*/) {
            spdlog::info("[PrintPreparationManager] Downloaded to disk, applying streaming "
                         "modification");

            // Step 2: Apply streaming modification (file-to-file, minimal memory)
            gcode::GCodeFileModifier modifier;

            // Disable file-embedded operations (comment them out)
            modifier.disable_operations(*scan_result, ops_to_disable);

            // Add skip parameters to PRINT_START call (if any)
            if (!macro_skip_params.empty()) {
                if (modifier.add_print_start_skip_params(*scan_result, macro_skip_params)) {
                    spdlog::info("[PrintPreparationManager] Added {} skip params to PRINT_START",
                                 macro_skip_params.size());
                } else {
                    spdlog::warn("[PrintPreparationManager] Could not add skip params - "
                                 "PRINT_START not found in G-code");
                }
            }

            auto result = modifier.apply_streaming(local_download_path);

            // Clean up download file (no longer needed) — bg-safe filesystem op,
            // runs even if owner is destroyed.
            if (!hfs::remove(local_download_path) && errno != ENOENT) {
                spdlog::warn("[PrintPreparationManager] Failed to clean up download file: {}",
                             std::strerror(errno));
            }

            if (!result.success) {
                helix::ui::notify_error_tr(TR_NOOP("Failed to modify G-code: {}"),
                                           result.error_message);
                // Defer this-> access to main thread.
                token.defer("PrintPreparationManager::modify_fail_clear_progress",
                            [this]() { abandon_start("modify_failed"); });
                return;
            }

            spdlog::info("[PrintPreparationManager] Modification complete ({} lines modified), "
                         "uploading {}",
                         result.lines_modified, result.modified_path);
            helix::MemoryMonitor::log_now("print_modification_done", spdlog::level::debug);

            // Step 3: Upload modified file from disk — defer the api_-> kick-off
            // to main thread (touches this->api_).
            std::string modified_path = result.modified_path; // Copy for lambda
            token.defer("PrintPreparationManager::modify_upload_kickoff", [this, token,
                                                                           modified_path,
                                                                           display_filename,
                                                                           remote_temp_path,
                                                                           file_path, mod_names,
                                                                           on_navigate_to_status,
                                                                           use_plugin]() {
                api_->transfers().upload_file_from_path(
                    "gcodes", remote_temp_path, modified_path,
                    // Upload success - NOTE: runs on HTTP thread, defer LVGL ops.
                    // L081 Mechanism C: filesystem cleanup runs locally; `this->`
                    // (api_->) work is deferred to main.
                    [this, token, modified_path, display_filename, remote_temp_path, file_path,
                     mod_names, on_navigate_to_status, use_plugin]() {
                        // Clean up local modified file (safe - filesystem op, always do it)
                        if (!hfs::remove(modified_path) && errno != ENOENT) {
                            spdlog::warn(
                                "[PrintPreparationManager] Failed to clean up modified file: {}",
                                std::strerror(errno));
                        }

                        spdlog::info("[PrintPreparationManager] Modified file uploaded, starting "
                                     "print (use_plugin={})",
                                     use_plugin);
                        helix::MemoryMonitor::log_now("print_start_dispatched",
                                                      spdlog::level::debug);

                        // Step 4: Start print with modified file — defer api_-> kick-off
                        // to main thread. The on_print_success / on_print_error lambdas
                        // are constructed/captured here (no `this` deref), then dispatched.
                        token.defer(
                            "PrintPreparationManager::start_print_kickoff",
                            [this, token, display_filename, remote_temp_path, file_path, mod_names,
                             on_navigate_to_status, use_plugin]() {
                                // If plugin available, use path-based API for
                                // symlink/history patching. Otherwise, use standard
                                // start_print.

                                // Define common callbacks to avoid code duplication
                                auto on_print_success = [this, token, on_navigate_to_status,
                                                         display_filename, file_path]() {
                                    spdlog::info("[PrintPreparationManager] Print started with "
                                                 "modified G-code (streaming, original: {})",
                                                 display_filename);

                                    // L081 Mechanism C: printer_state_ is a this->member;
                                    // start_print success cb fires on HTTP bg.
                                    token.defer(
                                        "PrintPreparationManager::print_success_clear_progress",
                                        [this]() {});

                                    // Defer LVGL operations to main thread
                                    struct PrintStartedData {
                                        std::string display_filename; // For display purposes
                                        std::string original_path; // Full path for metadata lookup
                                        NavigateToStatusCallback navigate_cb;
                                    };
                                    helix::ui::queue_update<PrintStartedData>(
                                        std::make_unique<PrintStartedData>(PrintStartedData{
                                            display_filename, file_path, on_navigate_to_status}),
                                        [](PrintStartedData* d) {
                                            // Hide overlay now that print is starting
                                            BusyOverlay::hide();

                                            // Name this print by the file the user chose, not
                                            // the rewritten copy print_stats will report. One
                                            // call: PrinterState is the single authority, so
                                            // the panel and the media manager can no longer
                                            // disagree about which print this is.
                                            get_printer_state()
                                                .print_state()
                                                .set_print_identity_override(d->original_path);

                                            if (d->navigate_cb) {
                                                d->navigate_cb();
                                            }
                                        });
                                };

                                auto on_print_error = [this, token, remote_temp_path](
                                                          const MoonrakerError& error) {
                                    queue_busy_hide();

                                    helix::ui::notify_error_tr(TR_NOOP("Failed to start print: {}"),
                                                               error);
                                    LOG_ERROR_INTERNAL(
                                        "[PrintPreparationManager] Print start failed for {}: {}",
                                        remote_temp_path, error.message);

                                    // L081 Mechanism C: printer_state_ is a this->member +
                                    // api_->files() touches this->api_; both deferred together.
                                    // start_print_* error cb fires on HTTP bg.
                                    token.defer(
                                        "PrintPreparationManager::start_print_error_cleanup",
                                        [this, remote_temp_path]() {
                                            abandon_start("print_start_failed");
                                            // Clean up remote temp file on failure
                                            // Moonraker's delete_file requires full path
                                            // including root
                                            std::string full_path = "gcodes/" + remote_temp_path;
                                            api_->files().delete_file(
                                                full_path,
                                                []() {
                                                    spdlog::debug(
                                                        "[PrintPreparationManager] Cleaned up "
                                                        "remote temp file after print failure");
                                                },
                                                [](const MoonrakerError& /*del_err*/) {
                                                    // Ignore delete errors - file may not exist
                                                    // or cleanup isn't critical
                                                });
                                        });
                                };

                                if (use_plugin) {
                                    // Plugin path: Use path-based API (v2.0)
                                    // The plugin will create symlink, patch history, and start
                                    // print
                                    api_->job().start_modified_print(
                                        file_path,        // Original filename for history
                                        remote_temp_path, // Path to uploaded modified file
                                        mod_names,
                                        [on_print_success](const ModifiedPrintResult& result) {
                                            spdlog::info(
                                                "[PrintPreparationManager] Plugin accepted print: "
                                                "{} -> {}",
                                                result.original_filename, result.print_filename);
                                            on_print_success();
                                        },
                                        on_print_error);
                                } else {
                                    // Standard path: Just start print with modified file
                                    api_->job().start_print(remote_temp_path, on_print_success,
                                                            on_print_error);
                                }
                            });
                    },
                    // Upload error - clean up local file. Runs on HTTP bg thread.
                    [this, token, modified_path](const MoonrakerError& error) {
                        queue_busy_hide();

                        // Clean up local file even on error (bg-safe filesystem op)
                        hfs::remove(modified_path);

                        helix::ui::notify_error_tr(TR_NOOP("Failed to upload modified G-code: {}"),
                                                   error);
                        LOG_ERROR_INTERNAL("[PrintPreparationManager] Upload failed: {}",
                                           error.message);
                        // L081 Mechanism C: printer_state_ is a this->member.
                        token.defer("PrintPreparationManager::upload_fail_clear_progress",
                                    [this]() { abandon_start("upload_failed"); });
                    },
                    // Upload progress callback
                    [](size_t sent, size_t total) {
                        BusyOverlay::queue_progress("Uploading", sent, total);
                    });
            }); // close PrintPreparationManager::modify_upload_kickoff defer
        },
        // Download error - clean up partial download. Runs on HTTP bg thread.
        [this, token, file_path, local_download_path](const MoonrakerError& error) {
            queue_busy_hide();

            // Clean up partial download if any (bg-safe filesystem op)
            hfs::remove(local_download_path);

            helix::ui::notify_error_tr(TR_NOOP("Failed to download G-code for modification: {}"),
                                       error);
            LOG_ERROR_INTERNAL("[PrintPreparationManager] Download failed for {}: {}", file_path,
                               error.message);
            // L081 Mechanism C: printer_state_ is a this->member.
            token.defer("PrintPreparationManager::download_fail_clear_progress",
                        [this]() { abandon_start("download_failed"); });
        },
        // Download progress callback
        download_progress);
}

void PrintPreparationManager::modify_and_print_with_remap(
    const std::string& file_path, const std::map<int, int>& remap,
    NavigateToStatusCallback on_navigate_to_status) {
    if (!api_) {
        spdlog::error("[PrintPreparationManager] modify_and_print_with_remap: no API");
        NOTIFY_ERROR(lv_tr("Cannot remap: internal error"));
        abandon_start("remap_internal_error");
        return;
    }

    // Printing the original instead would run the job on the tools the user just
    // remapped away from, so this refuses rather than falls back.
    if (!transport_keeps_local_copies()) {
        NOTIFY_ERROR(lv_tr("Remapping G-code is not available on this device."));
        abandon_start("remap_no_local_copies");
        return;
    }

    // Extract just the filename for display / temp naming.
    size_t last_slash = file_path.rfind('/');
    std::string display_filename =
        (last_slash != std::string::npos) ? file_path.substr(last_slash + 1) : file_path;

    auto token = lifetime_.token();

    // The download lands in the gcode_mod cache under the mod_ prefix, which is
    // the only shape GCodeFileModifier::cleanup_temp_files() reaps. A crash
    // between the download and the delete below otherwise leaves a full copy of
    // the job on a board that has no room for one and no sweeper that sees it.
    const std::string local_download_path =
        gcode::GCodeFileModifier::generate_temp_path("remap_dl_" + display_filename);
    if (local_download_path.empty()) {
        NOTIFY_ERROR(lv_tr("Cannot remap G-code: no temp directory available"));
        abandon_start("remap_no_temp_dir");
        return;
    }

    const std::string remote_temp_path = gcode::make_rewritten_gcode_path(file_path);

    spdlog::info("[PrintPreparationManager] Remap modification: {} tool mapping(s), downloading {}",
                 remap.size(), file_path);

    BusyOverlay::show("Preparing print...");

    auto download_progress = [](size_t received, size_t total) {
        BusyOverlay::queue_progress("Downloading", received, total);
    };

    // Step 1: Download original file to disk (streaming).
    api_->transfers().download_file_to_path(
        "gcodes", file_path, local_download_path,
        // Download success - runs on HTTP bg thread. All bg-safe local work
        // (read file, compute replacements, apply_streaming) runs here; every
        // this->/api_-> access is deferred to the main thread via token.defer.
        [this, token, file_path, display_filename, remap, local_download_path, remote_temp_path,
         on_navigate_to_status](const std::string& /*dest_path*/) {
            // A download that produced nothing is a failed download, not a file
            // whose every line happens to be unchanged, and the two must not
            // take the same exit.
            if (helix::text_io::file_size(local_download_path).value_or(0) == 0) {
                hfs::remove(local_download_path);
                helix::ui::notify_error_tr(TR_NOOP("Failed to read G-code for remap"));
                token.defer("PrintPreparationManager::remap_read_fail", [this]() {
                    BusyOverlay::hide();
                    abandon_start("remap_read_failed");
                });
                return;
            }

            // Rewrite file-to-file. Peak memory is one line, so a 400MB job
            // costs what a 4MB one does; holding the content to find the
            // changed lines would put the whole file in RAM on a board that
            // has none to spare.
            const std::string modified_path =
                gcode::GCodeFileModifier::generate_temp_path(local_download_path);
            // nullopt covers a failed flush too: a volume that fills mid-write
            // opens fine and yields a truncated file that would otherwise upload
            // and print as if whole.
            const std::optional<size_t> rewrite =
                helix::GcodeToolRemapper::apply_to_file(local_download_path, modified_path, remap);
            const bool rewrite_ok = rewrite.has_value();
            const size_t lines_changed = rewrite.value_or(0);

            // Identity remap (nothing changes): print the original directly,
            // no temp copy. Clean up both local files and dispatch a plain start.
            if (rewrite_ok && lines_changed == 0) {
                hfs::remove(modified_path);
                hfs::remove(local_download_path);
                spdlog::info("[PrintPreparationManager] Remap produced no changes; "
                             "printing original {}",
                             file_path);
                token.defer("PrintPreparationManager::remap_identity_start",
                            [this, file_path, on_navigate_to_status]() {
                                BusyOverlay::hide();
                                // No completion callback needed: this is the
                                // SUCCESS path - the remap was a no-op, so the
                                // original file starts and the printer's own
                                // PRINTING report retires the preparing job as
                                // Confirmed. Every FAILURE exit in this file
                                // calls abandon_start() instead, because nothing
                                // else can: these routes take no completion
                                // callback, so PrintStartController's
                                // retire_preparing(Failed) is unreachable.
                                start_print_directly(file_path, on_navigate_to_status, nullptr);
                            });
                return;
            }

            // Download file no longer needed (bg-safe filesystem op).
            hfs::remove(local_download_path);

            if (!rewrite_ok) {
                hfs::remove(modified_path);
                helix::ui::notify_error_tr(TR_NOOP("Failed to remap G-code: {}"),
                                           std::string("could not write ") + modified_path);
                token.defer("PrintPreparationManager::remap_apply_fail", [this]() {
                    BusyOverlay::hide();
                    abandon_start("remap_apply_failed");
                });
                return;
            }

            spdlog::info("[PrintPreparationManager] Remap applied ({} lines), uploading {}",
                         lines_changed, modified_path);

            // Step 3: Upload modified copy from disk — defer api_-> kickoff to main.
            std::vector<std::string> mod_names;
            mod_names.reserve(remap.size());
            for (const auto& [logical, physical] : remap) {
                mod_names.push_back("remap_T" + std::to_string(logical) + "_to_T" +
                                    std::to_string(physical));
            }

            token.defer("PrintPreparationManager::remap_upload_kickoff", [this, token,
                                                                          modified_path,
                                                                          remote_temp_path,
                                                                          file_path,
                                                                          display_filename,
                                                                          mod_names,
                                                                          on_navigate_to_status]() {
                api_->transfers().upload_file_from_path(
                    "gcodes", remote_temp_path, modified_path,
                    // Upload success - runs on HTTP bg thread.
                    [this, token, modified_path, remote_temp_path, file_path, display_filename,
                     mod_names, on_navigate_to_status]() {
                        hfs::remove(modified_path);

                        spdlog::info("[PrintPreparationManager] Remapped file uploaded, "
                                     "starting print via plugin");

                        token.defer(
                            "PrintPreparationManager::remap_start_kickoff",
                            [this, token, remote_temp_path, file_path, display_filename, mod_names,
                             on_navigate_to_status]() {
                                auto on_print_success = [this, token, on_navigate_to_status,
                                                         display_filename, file_path]() {
                                    spdlog::info("[PrintPreparationManager] Remapped print "
                                                 "started (original: {})",
                                                 display_filename);
                                    token.defer("PrintPreparationManager::remap_success_clear",
                                                [this]() {});

                                    struct PrintStartedData {
                                        std::string display_filename;
                                        std::string original_path;
                                        NavigateToStatusCallback navigate_cb;
                                    };
                                    helix::ui::queue_update<PrintStartedData>(
                                        std::make_unique<PrintStartedData>(PrintStartedData{
                                            display_filename, file_path, on_navigate_to_status}),
                                        [](PrintStartedData* d) {
                                            BusyOverlay::hide();
                                            get_printer_state()
                                                .print_state()
                                                .set_print_identity_override(d->original_path);
                                            if (d->navigate_cb)
                                                d->navigate_cb();
                                        });
                                };

                                auto on_print_error = [this, token, remote_temp_path](
                                                          const MoonrakerError& error) {
                                    queue_busy_hide();
                                    helix::ui::notify_error_tr(TR_NOOP("Failed to start print: {}"),
                                                               error);
                                    LOG_ERROR_INTERNAL(
                                        "[PrintPreparationManager] Remapped print start "
                                        "failed for {}: {}",
                                        remote_temp_path, error.message);
                                    token.defer(
                                        "PrintPreparationManager::remap_start_error_cleanup",
                                        [this, remote_temp_path]() {
                                            abandon_start("remap_print_start_failed");
                                            std::string full_path = "gcodes/" + remote_temp_path;
                                            api_->files().delete_file(
                                                full_path, []() {},
                                                [](const MoonrakerError& /*e*/) {});
                                        });
                                };

                                // Plugin path: symlink + history patch keeps the original
                                // filename in print history. Caller guarantees the plugin
                                // is installed (open_remap_modal guards on it for the
                                // GcodeRewrite strategy).
                                api_->job().start_modified_print(
                                    file_path, remote_temp_path, mod_names,
                                    [on_print_success](const ModifiedPrintResult& result) {
                                        spdlog::info("[PrintPreparationManager] Plugin accepted "
                                                     "remapped print: {} -> {}",
                                                     result.original_filename,
                                                     result.print_filename);
                                        on_print_success();
                                    },
                                    on_print_error);
                            });
                    },
                    // Upload error - runs on HTTP bg thread.
                    [this, token, modified_path](const MoonrakerError& error) {
                        queue_busy_hide();
                        hfs::remove(modified_path);
                        helix::ui::notify_error_tr(TR_NOOP("Failed to upload remapped G-code: {}"),
                                                   error);
                        LOG_ERROR_INTERNAL("[PrintPreparationManager] Remap upload failed: {}",
                                           error.message);
                        token.defer("PrintPreparationManager::remap_upload_fail_clear",
                                    [this]() { abandon_start("remap_upload_failed"); });
                    },
                    // Upload progress callback
                    [](size_t sent, size_t total) {
                        BusyOverlay::queue_progress("Uploading", sent, total);
                    });
            });
        },
        // Download error - runs on HTTP bg thread.
        [this, token, file_path, local_download_path](const MoonrakerError& error) {
            queue_busy_hide();
            hfs::remove(local_download_path);
            helix::ui::notify_error_tr(TR_NOOP("Failed to download G-code for remap: {}"), error);
            LOG_ERROR_INTERNAL("[PrintPreparationManager] Remap download failed for {}: {}",
                               file_path, error.message);
            token.defer("PrintPreparationManager::remap_download_fail_clear",
                        [this]() { abandon_start("remap_download_failed"); });
        },
        download_progress);
}

void PrintPreparationManager::start_print_directly(const std::string& filename,
                                                   NavigateToStatusCallback on_navigate_to_status,
                                                   PrintCompletionCallback on_completion) {
    api_->job().start_print(
        filename,
        // Success callback
        [on_navigate_to_status, on_completion]() {
            spdlog::debug("[PrintPreparationManager] Print started successfully");

            if (on_navigate_to_status) {
                on_navigate_to_status();
            }

            if (on_completion) {
                on_completion(true, "");
            }
        },
        // Error callback
        [filename, on_completion](const MoonrakerError& error) {
            helix::ui::notify_error_tr(TR_NOOP("Failed to start print: {}"), error);
            LOG_ERROR_INTERNAL("[PrintPreparationManager] Print start failed for {}: {} ({})",
                               filename, error.message, error.get_type_string());

            if (on_completion) {
                on_completion(false, error.message);
            }
        });
}

} // namespace helix::ui
