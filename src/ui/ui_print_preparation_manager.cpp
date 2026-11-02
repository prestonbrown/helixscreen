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
#include "gcode_edit_list.h"
#include "gcode_tool_remapper.h"
#include "helix_fs.h"
#include "http_executor.h"
#include "lvgl.h"
#include "macro_param_cache.h"
#include "memory_monitor.h"
#include "memory_utils.h"
#include "moonraker_auth.h"
#include "moonraker_manager.h"
#include "observer_factory.h"
#include "operation_registry.h"
#include "preprint_skip_wrappers.h"
#include "print_start_collector.h"
#include "print_start_phase_label.h"
#include "scope_exit.h"
#include "storage_report_service.h"
#include "system/telemetry_manager.h"
#include "text_io.h"
#include "three_mf_reader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unistd.h>

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
constexpr MacroOptionId kMacroOptionIds[] = {
    {helix::PrintStartOpCategory::BED_MESH, "bed_mesh", PrePrintCategory::Mechanical},
    {helix::PrintStartOpCategory::QGL, "qgl", PrePrintCategory::Mechanical},
    {helix::PrintStartOpCategory::Z_TILT, "z_tilt", PrePrintCategory::Mechanical},
    {helix::PrintStartOpCategory::NOZZLE_CLEAN, "nozzle_clean", PrePrintCategory::Quality},
};

/// The MacroParam option for one of kMacroOptionIds, or nullopt when the analysis
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
    opt.order = static_cast<int>(&entry - kMacroOptionIds);
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

bool PrintPreparationManager::enabled_options_need_material(
    const OptionStateProvider& saved) const {
    for (const PrePrintOption& opt : get_cached_options().options) {
        if (!opt.needs_material) {
            continue;
        }
        const int v = saved ? saved(opt.id) : -1;
        if (v == 1 || (v != 0 && get_option_state(opt.id) == PrePrintOptionState::ENABLED)) {
            return true;
        }
    }
    return false;
}

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

    // 1. Providers take priority: a start's override, then the attached one.
    //    The detail panel registers a provider that reads from per-option
    //    dynamic subjects. A provider returns 0/1 when bound; any other value
    //    means "not bound" — fall through.
    for (const OptionStateProvider* provider : {&start_option_override_, &option_state_provider_}) {
        if (!*provider) {
            continue;
        }
        const int v = (*provider)(id);
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
    // Answered now, while the API is known alive: a queued observer handler can
    // run after its owner has freed it.
    keeps_local_copies_ = keeps_local_copies(api);
    if (!printer_state_) {
        plugin_observer_.reset(); // nothing left to publish to
    }

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
        // Whether rewrite-gated macro rows render turns on the plugin and the
        // transport. Observing fires once now, which covers a new transport.
        plugin_observer_ = helix::ui::observe<int>(
            printer_state_->plugin_status_state().get_helix_plugin_installed_subject(), this,
            [](PrintPreparationManager* self, int) { self->publish_macro_option_count(); },
            printer_state_->get_subjects_lifetime());
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
        helix::skip_wrappers::kPrepMacro, []() {},
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

void PrintPreparationManager::recalculate_estimate(const FileMetadata* file_metadata,
                                                   const std::string& file_key) {
    // Callers reach this before any getter fetch (the detail view computes
    // the estimate on open), so the subject is initialised here.
    ensure_estimate_subject_initialized();

    // The heat-up runs to the file's own temperatures. Until its metadata is
    // in, any number would leave them out, so there is no estimate.
    if (!printer_state_ || !file_metadata) {
        lv_subject_set_int(&preprint_estimate_subject_, 0);
        return;
    }

    auto& temps = printer_state_->temperature_state();
    const float ext_temp = helix::ui::temperature::deci_to_degrees_f(
        lv_subject_get_int(temps.get_active_extruder_temp_subject()));
    const float bed_temp =
        helix::ui::temperature::deci_to_degrees_f(lv_subject_get_int(temps.get_bed_temp_subject()));
    const auto nozzle_target = static_cast<float>(file_metadata->first_layer_extr_temp);
    const auto bed_target = static_cast<float>(file_metadata->first_layer_bed_temp);

    auto& rates = ThermalRateManager::instance();
    helix::PreprintHeating heating;
    heating.nozzle_seconds = rates.estimate_heating_seconds("extruder", ext_temp, nozzle_target);
    heating.bed_seconds = rates.estimate_heating_seconds("heater_bed", bed_temp, bed_target);
    heating.nozzle_target_known = nozzle_target > 0.0f;

    // The history this start will be measured against: its temp bucket and
    // whether a host-side block runs ahead of the job (the collector's window),
    // built from what start_print() builds it from.
    const int temp_bucket = helix::PreprintPredictor::temp_bucket_for(bed_temp);
    const gcode::ScanResult* scan = scan_result_for(file_key);
    const helix::PreprintWindow window =
        pre_start_block(collect_macro_skip_params(), file_key, scan).gcode.empty()
            ? helix::PreprintWindow::PrinterEdge
            : helix::PreprintWindow::HostPreStart;
    if (!predictor_cached_ || cached_temp_bucket_ != temp_bucket || cached_window_ != window) {
        cached_predictor_ = helix::PreprintPredictor{};
        cached_predictor_.load_entries(helix::PreprintPredictor::load_entries_from_config(),
                                       temp_bucket, window);
        cached_temp_bucket_ = temp_bucket;
        cached_window_ = window;
        predictor_cached_ = true;
    }

    const helix::PreprintPhaseOverrides overrides = preprint_phase_overrides();
    const int estimate_s =
        static_cast<int>(cached_predictor_.estimate(heating, overrides).total_seconds);
    lv_subject_set_int(&preprint_estimate_subject_, estimate_s);

    spdlog::debug("[PrintPreparationManager] Pre-print estimate: {}s (nozzle {:.0f}->{:.0f}C, "
                  "bed {:.0f}->{:.0f}C, {} history entries)",
                  estimate_s, ext_temp, nozzle_target, bed_temp, bed_target,
                  cached_predictor_.get_entries().size());
}

helix::PreprintPhaseOverrides PrintPreparationManager::preprint_phase_overrides() const {
    // A shown toggle the user turned off skips its phase; one left on runs it.
    // A phase with no toggle runs as the start macro does.
    helix::PreprintPhaseOverrides overrides;
    for (const helix::PhaseOption& option : helix::print_start_phase_options()) {
        const PrePrintOptionState state = get_option_state(std::string(option.id));
        if (state != PrePrintOptionState::NOT_APPLICABLE) {
            overrides[static_cast<int>(option.phase)] = state == PrePrintOptionState::ENABLED;
        }
    }
    return overrides;
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
        // A collector rebuilt since the analysis ran has not seen it yet.
        adopt_macro_analysis(*macro_analysis_);
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
        notify_start_waiter();
        return;
    }

    macro_analysis_in_progress_ = true;
    spdlog::debug(
        "[PrintPreparationManager] Starting PRINT_START macro analysis (attempt {} of {})",
        macro_analysis_retry_count_ + 1, kMaxMacroAnalysisRetries + 1);

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
                macro_analysis_in_progress_ = false;
                adopt_macro_analysis(analysis);
                if (on_macro_analysis_complete_) {
                    on_macro_analysis_complete_(analysis);
                }
                notify_start_waiter();
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
                if (macro_analysis_retry_count_ < kMaxMacroAnalysisRetries) {
                    macro_analysis_retry_count_++;
                    // Exponential backoff: 1s, 2s
                    int delay_ms = 1000 * (1 << (macro_analysis_retry_count_ - 1));

                    spdlog::info("[PrintPreparationManager] Retrying PRINT_START analysis in "
                                 "{}ms (attempt {} of {})",
                                 delay_ms, macro_analysis_retry_count_ + 1,
                                 kMaxMacroAnalysisRetries + 1);

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
                    kMaxMacroAnalysisRetries + 1);
                // A printer refusing our login says why in its own message.
                if (!helix::moonraker_auth::unauthorized()) {
                    NOTIFY_ERROR(lv_tr("Could not analyze PRINT_START macro. Some print options "
                                       "may be unavailable."));
                }

                // Set empty result
                macro_analysis_in_progress_ = false;
                helix::PrintStartAnalysis not_found;
                not_found.found = false;
                adopt_macro_analysis(not_found);
                if (on_macro_analysis_complete_) {
                    on_macro_analysis_complete_(not_found);
                }
                notify_start_waiter();
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
    for (const auto& entry : kMacroOptionIds) {
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

    // Layer 3: File scan (lowest priority): the file the detail view shows
    if (const auto& scan = get_scan_result()) {
        matrix.add_from_file_scan(*scan);
    }

    return matrix;
}

void PrintPreparationManager::set_macro_analysis(const helix::PrintStartAnalysis& analysis) {
    adopt_macro_analysis(analysis);
}

void PrintPreparationManager::adopt_macro_analysis(const helix::PrintStartAnalysis& analysis) {
    macro_analysis_ = analysis;
    publish_macro_option_count();
    // The macro's steps are the checklist's expected order when the file has
    // no start block of its own.
    if (auto* mgr = get_moonraker_manager()) {
        if (auto collector = mgr->print_start_collector()) {
            collector->set_macro_analysis(analysis);
        }
    }
}

// The options card shows only when some row will render, and the rows this
// analysis adds are invisible to PrinterState otherwise.
void PrintPreparationManager::publish_macro_option_count() {
    if (!printer_state_) {
        return;
    }
    const PrePrintOptionSet displayed = displayed_options();
    const PrePrintOptionSet& declared = get_cached_options();
    size_t macro_rows = displayed.options.size() - declared.options.size();
    // A row the detail view hides for want of a rewrite never renders, so it
    // cannot be what makes the options card worth showing.
    if (helix::gcode_rewrite_available(gcode_rewrite_block()) == 0) {
        for (const auto& opt : displayed.options) {
            if (!declared.find(opt.id) && disabling_option_requires_plugin(opt)) {
                --macro_rows;
            }
        }
    }
    printer_state_->set_macro_option_count(macro_rows);
}

void PrintPreparationManager::set_cached_scan_result(const gcode::ScanResult& scan,
                                                     const std::string& filename) {
    current_scan_key_ = filename;
    const auto* kept = kept_scans_.find(filename);
    store_kept_scan(filename, kept ? kept->version : FileVersion{},
                    KeptScan{filename, scan, std::nullopt});
}

// ============================================================================
// G-code Scanning
// ============================================================================

std::string PrintPreparationManager::scan_key(const std::string& filename,
                                              const std::string& current_path,
                                              const std::string& local_path) {
    // A stick file has no Moonraker path; its own path keeps same-named files
    // in different folders apart.
    if (!local_path.empty()) {
        return local_path;
    }
    return gcode::join_gcode_path(current_path, filename);
}

void PrintPreparationManager::scan_file_for_operations(const std::string& filename,
                                                       const std::string& current_path,
                                                       const std::string& local_path,
                                                       FileVersion version) {
    current_scan_key_ = scan_key(filename, current_path, local_path);
    request_scan(filename, current_path, local_path, version);
}

const PrintPreparationManager::KeptScan*
PrintPreparationManager::kept_scan(const std::string& key) const {
    const auto* kept = kept_scans_.find(key);
    return kept ? &kept->value : nullptr;
}

void PrintPreparationManager::request_scan(const std::string& filename,
                                           const std::string& current_path,
                                           const std::string& local_path, FileVersion version) {
    const std::string file_path = scan_key(filename, current_path);
    const std::string key = scan_key(filename, current_path, local_path);
    if (const auto* kept = kept_scans_.find(key)) {
        // A scan of another version of the file (re-sliced under the same
        // name) has the wrong line numbers for this one, so it is dropped.
        if (kept->version != version) {
            spdlog::info("[PrintPreparationManager] {} changed since its scan - scanning again",
                         key);
            kept_scans_.erase(key);
        } else if (kept->value.result && has_printer_stop_answer_for(key)) {
            // Reusable only while its printer-stopping command answer still
            // holds: one computed before the printer's macros were read, or
            // against a macro set since replaced, says nothing about this
            // printer.
            spdlog::debug("[PrintPreparationManager] Using kept scan result for {}", key);
            kept_scans_.mark_used(key);
            return;
        }
    }
    if (auto it = scans_in_flight_.find(key); it != scans_in_flight_.end()) {
        if (it->second.version == version) {
            return; // its answer is already on the way
        }
    }
    // A read under way of another version, or of other G-code, is answered
    // into nothing: only this request's answer is kept.
    drop_scan_in_flight(key);
    const uint64_t request = ++scan_requests_;
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    scans_in_flight_[key] = InFlightScan{version, request, cancel};
    const uint64_t epoch = scan_epoch_;

    if (!api_) {
        spdlog::warn("[PrintPreparationManager] Cannot scan G-code - no API connection");
        // Nothing was read, so whatever this file's scan already held stands.
        const KeptScan* kept = kept_scan(key);
        keep_scan(key, filename, version, kept ? kept->result : std::nullopt,
                  helix::printer_stop_not_run("no printer connection"));
        return;
    }

    const auto archive = archive_scan_sources_.find(key);
    if (helix::gcode::is_3mf(filename) && archive == archive_scan_sources_.end()) {
        // An empty result, not none: a .3mf with nothing named to print holds
        // no operations to strip.
        keep_scan(key, filename, version, gcode::ScanResult{},
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
    // An answer lands only while it is still for the version last asked for.
    auto current = [this, key, request, epoch]() {
        const auto it = scans_in_flight_.find(key);
        return epoch == scan_epoch_ && it != scans_in_flight_.end() &&
               it->second.request == request;
    };
    auto on_content = [this, token, filename, key, version, current](const std::string& content) {
        gcode::GCodeOpsDetector detector;
        auto scan_result = detector.scan_content(content);
        // The whole downloaded head, not the detector's shorter op window:
        // a large thumbnail block can push the definitions past that.
        scan_result.objects = gcode::collect_exclude_object_defines(content);
        scan_result.fit_head = helix::fit_head_facts(content);

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
            helix::printer_stop_check_in(content, helix::kPrinterStopScanBytes);
        if (stop_check.state == helix::PrinterStopCheck::State::Stops) {
            spdlog::warn("[PrintPreparationManager] {} line {} calls {}, which stops this printer",
                         filename, stop_check.line_number, stop_check.command);
        }

        token.defer("PrintPreparationManager::scan_success",
                    [this, filename, key, version, current, scan_result, stop_check]() {
                        if (current()) {
                            keep_scan(key, filename, version, scan_result, stop_check);
                        }
                    });
    };
    // A failed read just logs; it never blocks the UI.
    auto on_failure = [this, token, filename, key, version, current](const std::string& message) {
        spdlog::warn("[PrintPreparationManager] Failed to scan G-code {}: {}", filename, message);
        std::string reason = "the file could not be read: " + message;
        token.defer("PrintPreparationManager::scan_error",
                    [this, filename, key, version, current, reason]() {
                        if (current()) {
                            keep_scan(key, filename, version, std::nullopt,
                                      helix::printer_stop_not_run(reason));
                        }
                    });
    };

    // A bounded read. A stick file is read on the fast lane. A printer file's
    // partial download runs on the slow lane, so it can queue behind a
    // whole-file transfer already under way; a start that needs this answer
    // waits for it (print_waits_on_scan()).
    if (!local_path.empty()) {
        helix::http::HttpExecutor::fast().submit([local_path, on_content, on_failure]() {
            auto head = helix::text_io::read_file(local_path, helix::kPrinterStopScanBytes);
            if (!head) {
                on_failure("cannot read " + local_path);
                return;
            }
            on_content(*head);
        });
        return;
    }

    if (archive != archive_scan_sources_.end()) {
        // What prints for the archive: the printer's conversion, read like any
        // G-code, or the plate's own entry, inflated out of the archive.
        const ArchiveScanSource& source = archive->second;
        if (!source.converted_output.empty()) {
            api_->transfers().download_file_partial(
                "gcodes", source.converted_output, helix::kPrinterStopScanBytes, on_content,
                [on_failure](const MoonrakerError& error) { on_failure(error.message); }, cancel);
        } else {
            three_mf::ThreeMfReader::read_entry_head(
                api_->transfers(), file_path, source.plate_gcode, helix::kPrinterStopScanBytes,
                on_content, on_failure, cancel);
        }
        return;
    }

    api_->transfers().download_file_partial(
        "gcodes", file_path, helix::kPrinterStopScanBytes, on_content,
        [on_failure](const MoonrakerError& error) { on_failure(error.message); }, cancel);
}

void PrintPreparationManager::set_archive_scan_source(const std::string& filename,
                                                      const std::string& current_path,
                                                      FileVersion version,
                                                      std::optional<ArchiveScanSource> source) {
    const std::string key = scan_key(filename, current_path);
    const auto it = archive_scan_sources_.find(key);
    const bool had = it != archive_scan_sources_.end();
    if ((!had && !source) || (had && source && it->second == *source)) {
        return;
    }
    if (source) {
        archive_scan_sources_[key] = std::move(*source);
        // Only the archive shown, the one a start waits on, and the one being
        // named are scanned again.
        for (auto s = archive_scan_sources_.begin(); s != archive_scan_sources_.end();) {
            const bool wanted =
                s->first == key || s->first == current_scan_key_ || s->first == start_pin_key_;
            s = wanted ? std::next(s) : archive_scan_sources_.erase(s);
        }
    } else {
        archive_scan_sources_.erase(key);
    }
    // The kept answer was for other G-code: its line numbers describe nothing
    // that prints now.
    kept_scans_.erase(key);
    drop_scan_in_flight(key);
    if (key == current_scan_key_) {
        request_scan(filename, current_path, "", version);
    }
}

void PrintPreparationManager::keep_scan(const std::string& key, const std::string& filename,
                                        FileVersion version,
                                        std::optional<gcode::ScanResult> result,
                                        helix::PrinterStopCheck printer_stop) {
    scans_in_flight_.erase(key);
    store_kept_scan(key, version, KeptScan{filename, std::move(result), std::move(printer_stop)});
    if (on_scan_answered_) {
        on_scan_answered_();
    }
    on_scan_answered_for_waiter(key);
}

void PrintPreparationManager::store_kept_scan(const std::string& key, FileVersion version,
                                              KeptScan scan) {
    kept_scans_.put(key, version, std::move(scan));
    kept_scans_.evict(kMaxKeptScans, [this](const std::string& k) {
        return k == current_scan_key_ || k == start_pin_key_;
    });
}

void PrintPreparationManager::drop_scan_in_flight(const std::string& key) {
    if (const auto it = scans_in_flight_.find(key); it != scans_in_flight_.end()) {
        if (it->second.cancel) {
            it->second.cancel->store(true);
        }
        scans_in_flight_.erase(it);
    }
}

void PrintPreparationManager::clear_scan_cache() {
    kept_scans_.clear();
    for (auto& [key, scan] : scans_in_flight_) {
        if (scan.cancel) {
            scan.cancel->store(true);
        }
    }
    scans_in_flight_.clear();
    current_scan_key_.clear();
    start_pin_key_.clear();
    ++scan_epoch_;
    cached_file_size_.reset();
}

helix::PrinterStopCheck
PrintPreparationManager::printer_stop_check_for(const std::string& key) const {
    if (has_printer_stop_answer_for(key)) {
        return *kept_scan(key)->printer_stop;
    }
    return helix::printer_stop_not_run(
        "the file scan has not answered for the printer's current macros");
}

void PrintPreparationManager::set_on_scan_answered(helix::Callback<void()> cb) {
    on_scan_answered_ = std::move(cb);
}

bool PrintPreparationManager::has_printer_stop_answer_for(const std::string& key) const {
    const KeptScan* kept = kept_scan(key);
    return kept && kept->printer_stop &&
           kept->printer_stop->macro_generation == helix::MacroParamCache::instance().generation();
}

void PrintPreparationManager::when_ready_to_start(const std::string& filename,
                                                  const std::string& current_path,
                                                  FileVersion version, StartReadyCallback ready) {
    if (!ready) {
        start_waiter_.reset();
        start_pin_key_.clear();
        return;
    }
    const std::string key = scan_key(filename, current_path);
    start_waiter_ = StartWaiter{filename, current_path, key, version, false, 1, std::move(ready)};
    start_pin_key_ = key;
    request_start_waiter_scan();
    notify_start_waiter();
}

void PrintPreparationManager::request_start_waiter_scan() {
    const std::string key = start_waiter_->key;
    // Never through scan_file_for_operations(): this file is not the one the
    // detail view shows, and must not become it.
    request_scan(start_waiter_->filename, start_waiter_->current_path, {}, start_waiter_->version);
    if (start_waiter_ && start_waiter_->key == key && has_printer_stop_answer_for(key)) {
        start_waiter_->answered = true;
    }
}

void PrintPreparationManager::on_scan_answered_for_waiter(const std::string& key) {
    if (start_waiter_ && !start_waiter_->answered && key == start_waiter_->key) {
        start_waiter_->answered = true;
    }
    notify_start_waiter();
}

void PrintPreparationManager::notify_start_waiter() {
    if (!start_waiter_ || !start_waiter_->answered || macro_analysis_in_progress_) {
        return;
    }
    if (!has_printer_stop_answer_for(start_waiter_->key)) {
        // The macros were read again since the answer: it no longer describes
        // this printer. Scan once more, then give up.
        if (start_waiter_->rescans_left-- > 0) {
            spdlog::info("[PrintPreparationManager] Scan of {} predates the printer's macros - "
                         "scanning again",
                         start_waiter_->key);
            start_waiter_->answered = false;
            request_start_waiter_scan();
            notify_start_waiter();
            return;
        }
        spdlog::warn("[PrintPreparationManager] Scan of {} kept going stale - giving up",
                     start_waiter_->key);
        auto ready = std::move(start_waiter_->ready);
        start_waiter_.reset();
        ready(std::nullopt);
        return;
    }
    auto ready = std::move(start_waiter_->ready);
    const helix::PrinterStopCheck check = *kept_scan(start_waiter_->key)->printer_stop;
    start_waiter_.reset();
    ready(check);
}

const gcode::ScanResult* PrintPreparationManager::scan_result_for(const std::string& key) const {
    const KeptScan* kept = kept_scan(key);
    return kept && kept->result ? &*kept->result : nullptr;
}

const std::optional<gcode::ScanResult>& PrintPreparationManager::get_scan_result() const {
    static const std::optional<gcode::ScanResult> none;
    const KeptScan* kept = kept_scan(current_scan_key_);
    return kept ? kept->result : none;
}

bool PrintPreparationManager::has_scan_result_for(const std::string& key) const {
    return scan_result_for(key) != nullptr;
}

const gcode::PrintStartCallInfo*
PrintPreparationManager::print_start_for(const std::string& key) const {
    const gcode::ScanResult* scan = scan_result_for(key);
    return scan && scan->print_start.found ? &scan->print_start : nullptr;
}

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
    return gcode_rewrite_block() == GcodeRewriteBlock::None;
}

GcodeRewriteBlock PrintPreparationManager::gcode_rewrite_block() const {
    return rewrite_block_with(printer_state_, keeps_local_copies_);
}

GcodeRewriteBlock PrintPreparationManager::gcode_rewrite_block_for(PrinterState* printer_state,
                                                                   IMoonrakerAPI* api) {
    return rewrite_block_with(printer_state, keeps_local_copies(api));
}

GcodeRewriteBlock PrintPreparationManager::rewrite_block_with(PrinterState* printer_state,
                                                              bool local_copies) {
    // Pre-print modifications rewrite the job file, and the plugin is what puts
    // the original filename back in Moonraker's history afterwards. Without it
    // finished jobs are listed as ".helix_temp/modified_1766807545p_name.gcode",
    // so we decline rather than clutter the history. No printer state reads as
    // the plugin absent.
    const int plugin =
        printer_state ? printer_state->plugin_status_state().helix_plugin_state() : 0;
    return helix::gcode_rewrite_block(plugin, local_copies);
}

bool PrintPreparationManager::keeps_local_copies(IMoonrakerAPI* api) {
    // No API is refused where a download would start, not here.
    return api == nullptr || api->transfers().supports_local_copies();
}

bool PrintPreparationManager::transport_keeps_local_copies() const {
    return keeps_local_copies_;
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
                                          PrintCompletionCallback on_completion,
                                          const std::string& expected_scan_key) {
    // A start's override answers this one start: every option read happens
    // before this returns, and a later Print tap must read its own rows.
    const helix::ScopeExit drop_start_override([this]() { start_option_override_ = nullptr; });

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

    // The scan rewrites the file by line number, so only this file's own kept
    // scan may reach the start: the one kept under the key its caller names
    // (a stick file's scan stands for its copy on the printer), else its path.
    const std::string start_key =
        expected_scan_key.empty() ? scan_key(filename, current_path) : expected_scan_key;
    const gcode::ScanResult* own_scan = scan_result_for(start_key);
    start_scan_ = own_scan ? std::optional<gcode::ScanResult>(*own_scan) : std::nullopt;
    // Copied, so the scan no longer needs keeping for this start.
    if (start_pin_key_ == start_key) {
        start_pin_key_.clear();
    }

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
    std::string filename_to_print = gcode::join_gcode_path(current_path, filename);

    // Read checkbox states for logging and timelapse
    PrePrintOptions options = read_options_from_subjects();

    spdlog::debug(
        "[PrintPreparationManager] Starting print: {} (pre-print options: mesh={}, qgl={}, "
        "z_tilt={}, clean={}, timelapse={})",
        filename_to_print, options.bed_mesh, options.qgl, options.z_tilt, options.nozzle_clean,
        options.timelapse);

    // Check if user disabled operations that are embedded in the G-code file
    std::vector<gcode::OperationType> ops_to_disable =
        collect_ops_to_disable(start_scan_ ? &*start_scan_ : nullptr);

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
    const PreStartBlock pre_start = pre_start_block(macro_skip_params, filename_to_print,
                                                    start_scan_ ? &*start_scan_ : nullptr);
    const std::string& combined = pre_start.gcode;

    // Without this file's scan nothing can say which disabled steps the file
    // embeds, nor where its START_PRINT call is. A start that would act on
    // either is refused rather than run with the user's choices dropped. The
    // file view holds Print while that scan is still reading, so this answers
    // a scan that failed or timed out.
    if (!start_scan_ && start_acts_on_scan(macro_skip_params, pre_start)) {
        spdlog::error("[PrintPreparationManager] Refusing to start {}: no scan of {} answers for "
                      "the steps turned off",
                      filename_to_print, start_key);
        // A read that failed is read again, so the retry the message offers
        // can succeed.
        if (const auto* kept = kept_scans_.find(start_key); kept && !kept->value.result) {
            const std::string scanned_name = kept->value.filename;
            const FileVersion version = kept->version;
            const std::string local_path =
                start_key == scan_key(filename, current_path) ? std::string() : start_key;
            request_scan(scanned_name, current_path, local_path, version);
        }
        if (on_completion) {
            on_completion(false,
                          fmt::format(fmt::runtime(lv_tr(
                                          "Cannot start {}: the file could not be checked yet for "
                                          "the steps you turned off. Try again in a moment.")),
                                      filename));
        }
        return;
    }

    // Mark this as an in-app print for telemetry source tracking
    TelemetryManager::instance().notify_print_started_in_app();

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

    // The countdown estimates with the same toggles print details did, and
    // the run's history entry records what this start skips, so a later
    // estimate can tell a phase no start ran from one the matcher missed.
    if (auto* mgr = get_moonraker_manager()) {
        if (auto collector = mgr->print_start_collector()) {
            collector->note_phase_overrides(preprint_phase_overrides());
        }
    }

    if (!combined.empty()) {
        spdlog::info("[PrintPreparationManager] Executing pre-start gcode ({} line(s)): {}",
                     (pre_start.emit_setup ? 1 : 0) + pre_start.lines.size(), combined);

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
            IMoonrakerAPI::kPreStartMacroTimeoutMs,
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
                             on_navigate_to_status, wrapped_completion);
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

// Whether @p scan holds a call of @p line's own command word under @p option_id's
// operation, i.e. the file performs that step itself. Only the same command
// counts: other BED_MESH_* lines load, clear or offset a stored mesh, and a
// START_PRINT parameter asking for a mesh proves nothing about a macro that may
// ignore it. A miss sends the line: in doubt the step runs twice rather than
// not at all.
bool file_runs_command(const gcode::ScanResult& scan, const std::string& option_id,
                       const std::string& line) {
    const std::optional<gcode::OperationType> op = file_embeddable_op_for_id(option_id);
    if (!op) {
        return false;
    }
    const std::string word = text_io::to_upper(gcode::GCodeOpsDetector::command_word(line));
    return scan.operation_commands.count({*op, word}) > 0;
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

std::vector<gcode::OperationType>
PrintPreparationManager::collect_ops_to_disable(const gcode::ScanResult* scan) const {
    std::vector<gcode::OperationType> ops_to_disable;
    if (!scan) {
        return ops_to_disable; // No scan result, nothing to disable
    }
    for (const auto op : ops_user_disabled()) {
        if (scan->has_operation(op)) {
            ops_to_disable.push_back(op);
            spdlog::debug("[PrintPreparationManager] User disabled '{}', file has it embedded",
                          gcode::GCodeOpsDetector::operation_type_name(op));
        }
    }
    return ops_to_disable;
}

std::vector<gcode::OperationType> PrintPreparationManager::ops_user_disabled() const {
    std::vector<gcode::OperationType> ops_to_disable;

    // Each file-embeddable op the user explicitly disabled, whatever the file holds.
    // Note: hidden (NOT_APPLICABLE) options are NOT candidates for disabling.
    // State resolution flows through the new framework via get_option_state(id).
    for (const char* id : {"bed_mesh", "qgl", "z_tilt", "nozzle_clean"}) {
        const std::optional<gcode::OperationType> op = file_embeddable_op_for_id(id);
        if (get_option_state(id) != PrePrintOptionState::DISABLED) {
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
    }

    return ops_to_disable;
}

bool PrintPreparationManager::disabling_option_requires_plugin(const PrePrintOption& opt) const {
    const bool is_macro_param = (opt.strategy_kind == PrePrintStrategyKind::MacroParam);

    // (a) Does disabling THIS option force a file modification because the op is
    //     embedded in the file the detail view shows?
    const std::optional<gcode::OperationType> embedded_op = file_embeddable_op_for_id(opt.id);
    const auto& shown_scan = get_scan_result();
    const bool file_embedded = embedded_op.has_value() && shown_scan.has_value() &&
                               shown_scan->has_operation(*embedded_op) &&
                               option_may_strip_file(opt);

    // (b) is the MacroParam skip-rewrite path. If neither (a) nor a MacroParam
    //     skip is in play, nothing about this option needs the plugin.
    if (!is_macro_param && !file_embedded) {
        return false;
    }

    // Does disabling it still do something without the plugin? A PreStartGcode
    // option always emits its own line. A MacroParam skip rides a pre-start
    // block: printer-level setup_gcode (gated on a MacroParam skip, see
    // PreStartBlock::emit_setup in start_print()) or any PreStartGcode line. This is
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
    if (gcode_rewrite_block() == GcodeRewriteBlock::NoLocalCopies) {
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
    for (const auto& entry : kMacroOptionIds) {
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
//                          pre-start block (PreStartBlock::emit_setup in start_print),
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
    for (const auto& entry : kMacroOptionIds) {
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
        spdlog::debug("[PrintPreparationManager] Collected {} skip params (DB+macro combined)",
                      skip_params.size());
    }

    return skip_params;
}

PrintPreparationManager::PreStartBlock PrintPreparationManager::pre_start_block(
    const std::vector<std::pair<std::string, std::string>>& macro_skip_params,
    const std::string& filename, const gcode::ScanResult* scan) const {
    PreStartBlock block;
    const auto& db_options = get_cached_options();
    block.lines = collect_pre_start_gcode_lines(filename, true, scan);
    // A skip an earlier print left unconsumed must not ride into this one,
    // including for a step whose toggle is hidden now.
    if (printer_state_ && !printer_state_->get_discovery().skip_active().empty()) {
        block.lines.insert(block.lines.begin(), helix::skip_wrappers::kPrepMacro);
    }
    block.emit_setup = !macro_skip_params.empty() && !db_options.setup_gcode.empty();
    block.gcode =
        build_pre_start_gcode_block(db_options.setup_gcode, block.lines, block.emit_setup);
    return block;
}

bool PrintPreparationManager::start_acts_on_scan(
    const std::vector<std::pair<std::string, std::string>>& macro_skip_params,
    const PreStartBlock& block) const {
    const bool rewrites_start_call =
        !macro_skip_params.empty() && block.gcode.empty() && can_modify_gcode();
    return !ops_user_disabled().empty() || rewrites_start_call;
}

bool PrintPreparationManager::print_waits_on_scan(const std::string& key) const {
    if (scans_in_flight_.count(key) == 0) {
        return false;
    }
    const auto skip_params = collect_macro_skip_params();
    return start_acts_on_scan(skip_params, pre_start_block(skip_params, {}, nullptr));
}

std::vector<std::string> PrintPreparationManager::collect_pre_start_gcode_lines(
    const std::string& filename, bool with_skip_toggles, const gcode::ScanResult* scan) const {
    std::vector<std::string> lines;
    const gcode::PrintStartCallInfo* job_start =
        scan && scan->print_start.found ? &scan->print_start : nullptr;

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

        PreStartGcodeContext ctx;
        ctx.filename = filename;
        if (job_start) {
            ctx.bed_temp = job_start->bed_temp;
            ctx.extruder_temp = job_start->extruder_temp;
        }
        std::string line = render_pre_start_gcode(opt, enabled, ctx);
        if (line.empty()) {
            // render_pre_start_gcode logs its own warning on type mismatch.
            continue;
        }

        // A line with no off form IS the step; a file that runs the step
        // itself would have the printer run it twice.
        if (enabled && pre && !pre->emit_when_disabled && scan &&
            file_runs_command(*scan, opt.id, line)) {
            spdlog::info("[PrintPreparationManager] Not sending pre-start '{}': {} runs it itself",
                         opt.id, filename);
            continue;
        }
        spdlog::debug("[PrintPreparationManager] Pre-start gcode for option '{}' (enabled={}): {}",
                      opt.id, enabled, line);
        lines.push_back(std::move(line));
    }

    if (!lines.empty()) {
        spdlog::debug("[PrintPreparationManager] Collected {} pre-start gcode line(s)",
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
        constexpr auto kStalePreStartBound = std::chrono::minutes(3);
        const auto age = std::chrono::steady_clock::now() - pre_start_sent_at_;
        if (age > kStalePreStartBound) {
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
        modify_and_print(filename, ops_to_disable, {}, on_navigate_to_status, on_completion);
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
        IMoonrakerAPI::kPreStartMacroTimeoutMs,
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

void PrintPreparationManager::abandon_start(const char* where,
                                            const PrintCompletionCallback& on_completion) {
    if (printer_state_) {
        spdlog::warn("[PrintPreparationManager] Start abandoned at {} - retiring the preparing job",
                     where);
        printer_state_->print_state().retire_preparing(helix::PreparingExit::Failed);
    }
    if (on_completion) {
        on_completion(false, "");
    }
}

namespace {

/// Record a start's answer for the job armed at @p epoch. Safe from any
/// thread: the write hops to the main thread. PrinterState outlives every
/// start, so the bare pointer cannot dangle.
void note_start_answered(helix::PrinterState* ps, int epoch, bool accepted) {
    if (!ps) {
        return;
    }
    helix::ui::queue_update("PrintPreparationManager::start_answered", [ps, epoch, accepted]() {
        ps->print_state().note_preparing_start_answered(epoch, accepted);
    });
}

/// Main thread. A start whose answer was lost stays preparing for the printer
/// to settle (PrinterPrintState::hold_unanswered_start). True when held.
bool held_unanswered(helix::PrinterState* ps, int epoch, const MoonrakerError& error) {
    return ps && ps->print_state().hold_unanswered_start(epoch, error);
}

} // namespace

int PrintPreparationManager::current_preparing_epoch() const {
    return printer_state_
               ? lv_subject_get_int(printer_state_->print_state().get_preparing_epoch_subject())
               : 0;
}

bool PrintPreparationManager::start_withdrawn(int preparing_epoch) const {
    return preparing_epoch != 0 && current_preparing_epoch() != preparing_epoch;
}

void PrintPreparationManager::note_start_sent() {
    if (printer_state_) {
        printer_state_->print_state().note_preparing_start_sent();
    }
}

void PrintPreparationManager::modify_and_print(
    const std::string& file_path, const std::vector<gcode::OperationType>& ops_to_disable,
    const std::vector<std::pair<std::string, std::string>>& macro_skip_params,
    NavigateToStatusCallback on_navigate_to_status, PrintCompletionCallback on_completion) {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("Cannot start print: not connected to printer"));
        abandon_start("modify_no_api", on_completion);
        return;
    }

    if (!start_scan_.has_value()) {
        spdlog::error("[PrintPreparationManager] modify_and_print called without scan result");
        NOTIFY_ERROR(lv_tr("Internal error: no scan result"));
        abandon_start("modify_no_scan_result", on_completion);
        return;
    }

    spdlog::info("[PrintPreparationManager] Modifying G-code: {} file ops to disable, {} macro "
                 "skip params",
                 ops_to_disable.size(), macro_skip_params.size());

    // Modification identifiers for the plugin's history record.
    std::vector<std::string> mod_names;
    for (const auto& op : ops_to_disable) {
        mod_names.push_back(gcode::GCodeOpsDetector::operation_type_name(op) + "_disabled");
    }
    for (const auto& [param_name, param_value] : macro_skip_params) {
        mod_names.push_back("skip_" + param_name);
    }

    std::string temp_dir = get_temp_directory();
    if (temp_dir.empty()) {
        NOTIFY_ERROR(lv_tr("Cannot modify G-code: no temp directory available"));
        abandon_start("streaming_no_temp_dir", on_completion);
        return;
    }

    RewriteRoute route;
    route.file_path = printable_path(file_path);
    route.local_download_path =
        temp_dir + "/helix_download_" + std::to_string(std::time(nullptr)) + ".gcode";
    route.mod_names = std::move(mod_names);
    // Without the plugin the modified copy is started as itself.
    route.use_plugin =
        printer_state_ && printer_state_->plugin_status_state().service_has_helix_plugin();
    route.archive_plate = archive_plate_for(file_path);
    route.download_error = TR_NOOP("Failed to download G-code for modification: {}");
    route.upload_error = TR_NOOP("Failed to upload modified G-code: {}");

    gcode::EditList edits;
    {
        gcode::GCodeFileModifier modifier;
        // Disable file-embedded operations (comment them out)
        modifier.disable_operations(*start_scan_, ops_to_disable);
        // Add skip parameters to PRINT_START call (if any)
        if (!macro_skip_params.empty()) {
            if (modifier.add_print_start_skip_params(*start_scan_, macro_skip_params)) {
                spdlog::info("[PrintPreparationManager] Added {} skip params to PRINT_START",
                             macro_skip_params.size());
            } else {
                spdlog::warn("[PrintPreparationManager] Could not add skip params - "
                             "PRINT_START not found in G-code");
            }
        }
        edits.modifications = modifier.modifications();
    }
    route.server_edits = gcode::to_server_edits(edits);
    route.file_size = cached_file_size_.value_or(0);
    // Copied for the HTTP thread: the edits were made against this file's own scan.
    route.rewrite = [mods =
                         std::move(edits.modifications)](const std::string& local_download_path) {
        gcode::GCodeFileModifier modifier;
        for (const auto& mod : mods) {
            modifier.add_modification(mod);
        }
        auto result = modifier.apply_streaming(local_download_path);
        if (!hfs::remove(local_download_path) && errno != ENOENT) {
            spdlog::warn("[PrintPreparationManager] Failed to clean up download file: {}",
                         std::strerror(errno));
        }
        if (!result.success) {
            helix::ui::notify_error_tr(TR_NOOP("Failed to modify G-code: {}"),
                                       result.error_message);
            return RewriteResult{RewriteResult::Kind::Failed, {}};
        }
        spdlog::info("[PrintPreparationManager] Modification complete ({} lines modified)",
                     result.lines_modified);
        helix::MemoryMonitor::log_now("print_modification_done", spdlog::level::debug);
        return RewriteResult{RewriteResult::Kind::Rewritten, result.modified_path};
    };
    rewrite_and_print(std::move(route), std::move(on_navigate_to_status), std::move(on_completion));
}

void PrintPreparationManager::modify_and_print_with_remap(
    const std::string& file_path, const std::map<int, int>& remap,
    NavigateToStatusCallback on_navigate_to_status, PrintCompletionCallback on_completion) {
    if (!api_) {
        spdlog::error("[PrintPreparationManager] modify_and_print_with_remap: no API");
        NOTIFY_ERROR(lv_tr("Couldn't map filament: internal error"));
        abandon_start("remap_internal_error", on_completion);
        return;
    }

    // Printing the original instead would run the job on the tools the user just
    // remapped away from, so this refuses rather than falls back.
    if (!transport_keeps_local_copies()) {
        NOTIFY_ERROR(lv_tr("Filament Mapping is not available on this device."));
        abandon_start("remap_no_local_copies", on_completion);
        return;
    }

    // The download lands in the gcode_mod cache under the mod_ prefix, which is
    // the only shape GCodeFileModifier::cleanup_temp_files() reaps. A crash
    // between the download and the delete below otherwise leaves a full copy of
    // the job on a board that has no room for one and no sweeper that sees it.
    const std::string local_download_path = gcode::GCodeFileModifier::generate_temp_path(
        "remap_dl_" + gcode::get_filename_basename(file_path));
    if (local_download_path.empty()) {
        NOTIFY_ERROR(lv_tr("Couldn't map filament: no temp directory available"));
        abandon_start("remap_no_temp_dir", on_completion);
        return;
    }

    spdlog::info("[PrintPreparationManager] Remap modification: {} tool mapping(s), downloading {}",
                 remap.size(), file_path);

    RewriteRoute route;
    route.file_path = printable_path(file_path);
    route.local_download_path = local_download_path;
    for (const auto& [logical, physical] : remap) {
        route.mod_names.push_back("remap_T" + std::to_string(logical) + "_to_T" +
                                  std::to_string(physical));
    }
    // The caller guarantees the plugin is installed (open_remap_modal guards on
    // it for the GcodeRewrite strategy).
    route.use_plugin = true;
    route.archive_plate = archive_plate_for(file_path);
    route.download_error = TR_NOOP("Failed to download G-code for Filament Mapping: {}");
    route.upload_error = TR_NOOP("Failed to upload the rewritten G-code: {}");
    route.server_edits = gcode::to_server_edits(gcode::EditList{{}, remap});
    route.file_size = cached_file_size_.value_or(0);
    route.rewrite = [file_path, remap](const std::string& download_path) {
        // A download that produced nothing is a failed download, not a file
        // whose every line happens to be unchanged, and the two must not
        // take the same exit.
        if (helix::text_io::file_size(download_path).value_or(0) == 0) {
            hfs::remove(download_path);
            helix::ui::notify_error_tr(TR_NOOP("Failed to read G-code for Filament Mapping"));
            return RewriteResult{RewriteResult::Kind::Failed, {}};
        }

        // Rewrite file-to-file. Peak memory is one line, so a 400MB job
        // costs what a 4MB one does; holding the content to find the
        // changed lines would put the whole file in RAM on a board that
        // has none to spare.
        const std::string modified_path =
            gcode::GCodeFileModifier::generate_temp_path(download_path);
        // nullopt covers a failed flush too: a volume that fills mid-write
        // opens fine and yields a truncated file that would otherwise upload
        // and print as if whole.
        const std::optional<size_t> rewrite =
            helix::GcodeToolRemapper::apply_to_file(download_path, modified_path, remap);
        hfs::remove(download_path);

        if (!rewrite) {
            hfs::remove(modified_path);
            helix::ui::notify_error_tr(TR_NOOP("Failed to rewrite G-code for Filament Mapping: {}"),
                                       std::string("could not write ") + modified_path);
            return RewriteResult{RewriteResult::Kind::Failed, {}};
        }
        // Identity remap (nothing changes): print the original directly,
        // no temp copy.
        if (*rewrite == 0) {
            hfs::remove(modified_path);
            spdlog::info("[PrintPreparationManager] Remap produced no changes; "
                         "printing original {}",
                         file_path);
            return RewriteResult{RewriteResult::Kind::Unchanged, {}};
        }
        spdlog::info("[PrintPreparationManager] Remap applied ({} lines)", *rewrite);
        return RewriteResult{RewriteResult::Kind::Rewritten, modified_path};
    };
    rewrite_and_print(std::move(route), std::move(on_navigate_to_status), std::move(on_completion));
}

void PrintPreparationManager::rewrite_and_print(RewriteRoute route,
                                                NavigateToStatusCallback on_navigate_to_status,
                                                PrintCompletionCallback on_completion) {
    // Download to disk, rewrite file-to-file, upload from disk: the whole file
    // is never in memory, which on a board like the AD5M (~108MB RAM) is the
    // difference between a start and a TTC error.
    auto token = lifetime_.token();
    // An extracted plate always starts as itself: the plugin would link it
    // under the archive's name, which Klipper does not print.
    if (route.archive_plate > 0) {
        route.use_plugin = false;
    }
    // A route that already names its job keeps it, so a retry of a withdrawn
    // start cannot adopt "no job" and print anyway.
    if (route.preparing_epoch == 0) {
        route.preparing_epoch = current_preparing_epoch();
    }
    if (start_withdrawn(route.preparing_epoch)) {
        spdlog::info("[PrintPreparationManager] Start abandoned - '{}' is no longer being "
                     "prepared",
                     route.file_path);
        if (on_completion) {
            on_completion(false, "");
        }
        return;
    }
    const auto job = std::make_shared<const RewriteRoute>(std::move(route));
    const std::string remote_temp_path =
        job->archive_plate > 0
            ? gcode::make_extracted_plate_path(job->file_path, job->archive_plate, staging_root())
            : gcode::make_rewritten_gcode_path(job->file_path, staging_root());

    const PluginFeatureState server_edits =
        printer_state_ ? printer_state_->plugin_status_state().plugin_feature(
                             gcode::kServerEditsFeature, gcode::kServerEditsMinVersion)
                       : PluginFeatureState::Absent;
    if (gcode::choose_edit_transport(server_edits, job->archive_plate > 0,
                                     job->server_edits.has_value()) ==
        gcode::EditTransport::Plugin) {
        // The plugin rewrites beside the file: nothing is downloaded or uploaded.
        spdlog::info("[PrintPreparationManager] Server-side edits: {} -> {}", job->file_path,
                     remote_temp_path);
        BusyOverlay::show("Preparing print...");
        start_staged_print(job, remote_temp_path, true, std::move(on_navigate_to_status),
                           std::move(on_completion));
        return;
    }

    spdlog::info("[PrintPreparationManager] Streaming modification: downloading {} to {} "
                 "(plugin: {})",
                 job->file_path, job->local_download_path, job->use_plugin);

    // Show busy overlay (will appear after 300ms grace period if operation takes that long)
    BusyOverlay::show("Preparing print...");

    // Progress callback for download - NOTE: called from HTTP thread
    auto download_progress = [](size_t received, size_t total) {
        BusyOverlay::queue_progress("Downloading", received, total);
    };

    api_->transfers().download_file_to_path(
        "gcodes", job->file_path, job->local_download_path,
        // Download success - runs on the HTTP thread. The rewrite runs here;
        // every this->/api_-> access is deferred to the main thread.
        [this, token, job, remote_temp_path, on_navigate_to_status,
         on_completion](const std::string& /*dest_path*/) {
            const RewriteResult rewritten = job->archive_plate > 0
                                                ? rewrite_archive_plate(*job)
                                                : job->rewrite(job->local_download_path);

            if (rewritten.kind == RewriteResult::Kind::Failed) {
                token.defer("PrintPreparationManager::rewrite_failed", [this, on_completion]() {
                    BusyOverlay::hide();
                    abandon_start("rewrite_failed", on_completion);
                });
                return;
            }

            if (rewritten.kind == RewriteResult::Kind::Unchanged) {
                // The original file starts, and the printer's own PRINTING
                // report retires the preparing job as Confirmed.
                token.defer("PrintPreparationManager::rewrite_unchanged_start",
                            [this, job, on_navigate_to_status, on_completion]() {
                                BusyOverlay::hide();
                                if (start_withdrawn(job->preparing_epoch)) {
                                    spdlog::info("[PrintPreparationManager] Start abandoned - "
                                                 "'{}' is no longer being prepared",
                                                 job->file_path);
                                    if (on_completion) {
                                        on_completion(false, "");
                                    }
                                    return;
                                }
                                start_print_directly(job->file_path, on_navigate_to_status,
                                                     on_completion);
                            });
                return;
            }

            const std::string modified_path = rewritten.modified_path;
            spdlog::info("[PrintPreparationManager] Rewrite done, uploading {}", modified_path);

            // Upload the rewritten copy from disk once the printer has room for
            // it; the api_-> kick-off runs on main.
            token.defer("PrintPreparationManager::modify_upload_kickoff", [this, token, job,
                                                                           modified_path,
                                                                           remote_temp_path,
                                                                           on_navigate_to_status,
                                                                           on_completion]() {
                const uint64_t size = text_io::file_size(modified_path).value_or(0);
                const size_t slash = remote_temp_path.rfind('/');
                const std::string upload_dir =
                    slash == std::string::npos ? std::string() : remote_temp_path.substr(0, slash);
                storage::check_upload_space(
                    &api_->get_client(), upload_dir, size, token,
                    [this, token, job, modified_path, remote_temp_path, on_navigate_to_status,
                     on_completion](storage::UploadSpace space) {
                        if (space == storage::UploadSpace::TooFull) {
                            BusyOverlay::hide();
                            hfs::remove(modified_path);
                            helix::ui::notify_error_tr(storage::kNoUploadSpaceMessage);
                            abandon_start("no_upload_space", on_completion);
                            return;
                        }
                        api_->transfers().upload_file_from_path(
                            "gcodes", remote_temp_path, modified_path,
                            // Upload success - runs on the HTTP thread.
                            [this, token, job, modified_path, remote_temp_path,
                             on_navigate_to_status, on_completion]() {
                                if (!hfs::remove(modified_path) && errno != ENOENT) {
                                    spdlog::warn("[PrintPreparationManager] Failed to clean up "
                                                 "modified file: {}",
                                                 std::strerror(errno));
                                }

                                spdlog::info(
                                    "[PrintPreparationManager] Modified file uploaded, starting "
                                    "print (use_plugin={})",
                                    job->use_plugin);
                                helix::MemoryMonitor::log_now("print_start_dispatched",
                                                              spdlog::level::debug);

                                token.defer("PrintPreparationManager::start_print_kickoff",
                                            [this, job, remote_temp_path, on_navigate_to_status,
                                             on_completion]() {
                                                start_staged_print(job, remote_temp_path, false,
                                                                   on_navigate_to_status,
                                                                   on_completion);
                                            });
                            },
                            // Upload error - runs on the HTTP thread.
                            [this, token, job, modified_path,
                             on_completion](const MoonrakerError& error) {
                                queue_busy_hide();
                                hfs::remove(modified_path);
                                helix::ui::notify_error_tr(job->upload_error, error);
                                LOG_ERROR_INTERNAL("[PrintPreparationManager] Upload failed: {}",
                                                   error.message);
                                token.defer("PrintPreparationManager::upload_fail_clear_progress",
                                            [this, on_completion]() {
                                                abandon_start("upload_failed", on_completion);
                                            });
                            },
                            // Upload progress callback
                            [](size_t sent, size_t total) {
                                BusyOverlay::queue_progress("Uploading", sent, total);
                            });
                    });
            });
        },
        // Download error - clean up any partial download. Runs on the HTTP thread.
        [this, token, job, on_completion](const MoonrakerError& error) {
            queue_busy_hide();
            hfs::remove(job->local_download_path);
            helix::ui::notify_error_tr(job->download_error, error);
            LOG_ERROR_INTERNAL("[PrintPreparationManager] Download failed for {}: {}",
                               job->file_path, error.message);
            token.defer(
                "PrintPreparationManager::download_fail_clear_progress",
                [this, on_completion]() { abandon_start("download_failed", on_completion); });
        },
        download_progress);
}

void PrintPreparationManager::start_staged_print(std::shared_ptr<const RewriteRoute> job,
                                                 const std::string& remote_temp_path,
                                                 bool plugin_edits,
                                                 NavigateToStatusCallback on_navigate_to_status,
                                                 PrintCompletionCallback on_completion) {
    // Download, rewrite and upload can take minutes; a job cancelled during
    // them must not start once they finish.
    if (start_withdrawn(job->preparing_epoch)) {
        spdlog::info("[PrintPreparationManager] Start abandoned - '{}' is no longer being "
                     "prepared",
                     job->file_path);
        BusyOverlay::hide();
        // The plugin writes its copy only once asked to start; ours is uploaded.
        if (!plugin_edits && api_) {
            api_->files().delete_file(
                "gcodes/" + remote_temp_path, []() {}, [](const MoonrakerError& /*del_err*/) {});
        }
        if (on_completion) {
            on_completion(false, "");
        }
        return;
    }
    note_start_sent();
    // The transfer is done. A firmware that runs its start sequence inside
    // SDCARD_PRINT_FILE answers the start only once that sequence ends, and
    // the overlay would keep Stop out of reach for all of it.
    BusyOverlay::hide();
    helix::PrinterState* ps = printer_state_;
    const int epoch = current_preparing_epoch();

    auto token = lifetime_.token();
    auto on_print_success = [job, on_navigate_to_status, ps, epoch]() {
        spdlog::info("[PrintPreparationManager] Print started with "
                     "modified G-code (original: {})",
                     job->file_path);
        note_start_answered(ps, epoch, true);

        // Defer LVGL operations to main thread
        struct PrintStartedData {
            std::string original_path; // Full path for metadata lookup
            NavigateToStatusCallback navigate_cb;
        };
        helix::ui::queue_update<PrintStartedData>(
            std::make_unique<PrintStartedData>(
                PrintStartedData{job->file_path, on_navigate_to_status}),
            [](PrintStartedData* d) {
                BusyOverlay::hide();

                // Name this print by the file the user chose, not the
                // rewritten copy print_stats will report. PrinterState is
                // the single authority, so the panel and the media manager
                // agree about which print this is.
                get_printer_state().print_state().set_print_identity_override(d->original_path);

                if (d->navigate_cb) {
                    d->navigate_cb();
                }
            });
    };

    auto on_print_error = [this, token, remote_temp_path, on_completion, ps,
                           epoch](const MoonrakerError& error) {
        token.defer("PrintPreparationManager::start_print_error_cleanup",
                    [this, remote_temp_path, on_completion, ps, epoch, error]() {
                        if (held_unanswered(ps, epoch, error)) {
                            return;
                        }
                        queue_busy_hide();
                        helix::ui::notify_error_tr(storage::print_start_failure_tag(error.code),
                                                   error);
                        LOG_ERROR_INTERNAL("[PrintPreparationManager] Print start failed for "
                                           "{}: {}",
                                           remote_temp_path, error.message);
                        abandon_start("print_start_failed", on_completion);
                        // Moonraker's delete_file takes the root too.
                        api_->files().delete_file(
                            "gcodes/" + remote_temp_path,
                            []() {
                                spdlog::debug("[PrintPreparationManager] Cleaned up "
                                              "remote temp file after print failure");
                            },
                            // The file may not exist; cleanup is best effort.
                            [](const MoonrakerError& /*del_err*/) {});
                    });
    };

    auto on_plugin_accepted = [on_print_success](const ModifiedPrintResult& result) {
        spdlog::info("[PrintPreparationManager] Plugin accepted print: {} -> {}",
                     result.original_filename, result.print_filename);
        on_print_success();
    };

    if (plugin_edits) {
        // The plugin writes the copy itself, and removes it if the start fails, so no
        // branch here deletes it.
        auto on_edit_error = [this, token, job, on_navigate_to_status, on_completion, ps,
                              epoch](const MoonrakerError& error) {
            switch (gcode::classify_print_edited_failure(error)) {
            case gcode::EditedPrintFailure::Unsupported:
                spdlog::warn("[PrintPreparationManager] Plugin cannot edit {} ({}); rewriting "
                             "on the screen",
                             job->file_path, error.message);
                token.defer("PrintPreparationManager::edit_fallback",
                            [this, job, on_navigate_to_status, on_completion]() {
                                // Refused before anything ran: the fallback
                                // sends its own start.
                                if (printer_state_) {
                                    printer_state_->print_state().note_preparing_start_answered(
                                        job->preparing_epoch, false);
                                }
                                RewriteRoute route = *job;
                                route.server_edits.reset();
                                rewrite_and_print(std::move(route), on_navigate_to_status,
                                                  on_completion);
                            });
                return;
            case gcode::EditedPrintFailure::Unanswered:
                // The plugin may still start this print: starting it again here would
                // print the job twice.
                token.defer("PrintPreparationManager::edit_unanswered",
                            [this, job, on_completion, ps, epoch, error]() {
                                if (held_unanswered(ps, epoch, error)) {
                                    return;
                                }
                                queue_busy_hide();
                                helix::ui::notify_error_tr(
                                    TR_NOOP("The printer did not confirm the print started ({}). "
                                            "It may still start: check the printer before "
                                            "starting it again."),
                                    error);
                                LOG_ERROR_INTERNAL(
                                    "[PrintPreparationManager] print_edited unanswered for {}: {}",
                                    job->file_path, error.message);
                                abandon_start("print_edited_failed", on_completion);
                            });
                return;
            case gcode::EditedPrintFailure::Refused:
                queue_busy_hide();
                helix::ui::notify_error_tr(storage::print_start_failure_tag(error.code), error);
                LOG_ERROR_INTERNAL("[PrintPreparationManager] print_edited refused for {}: {}",
                                   job->file_path, error.message);
                break;
            }
            token.defer("PrintPreparationManager::edit_error_cleanup", [this, on_completion]() {
                abandon_start("print_edited_failed", on_completion);
            });
        };
        api_->job().start_edited_print(job->file_path, remote_temp_path, *job->server_edits,
                                       job->mod_names, job->file_size, on_plugin_accepted,
                                       on_edit_error);
    } else if (job->use_plugin) {
        // The plugin symlinks the copy under the original name,
        // patches history, and starts the print.
        api_->job().start_modified_print(job->file_path, remote_temp_path, job->mod_names,
                                         on_plugin_accepted, on_print_error);
    } else {
        api_->job().start_print(remote_temp_path, on_print_success, on_print_error);
    }
}

void PrintPreparationManager::set_archive_plate(const std::string& file_path, int plate,
                                                const std::string& converted_output) {
    const bool marked = plate > 0 || !converted_output.empty();
    archive_path_ = marked ? file_path : std::string();
    archive_plate_ = marked && converted_output.empty() ? std::max(plate, 0) : 0;
    archive_output_ = marked ? converted_output : std::string();
}

void PrintPreparationManager::start_archive_reprint(const std::string& file_path, int plate,
                                                    PrintCompletionCallback on_completion) {
    set_archive_plate(file_path, plate);
    print_archive_plate(file_path, plate, nullptr, std::move(on_completion));
}

int PrintPreparationManager::archive_plate_for(const std::string& file_path) const {
    return !archive_path_.empty() && file_path == archive_path_ ? archive_plate_ : 0;
}

std::string PrintPreparationManager::printable_path(const std::string& file_path) const {
    return !archive_output_.empty() && file_path == archive_path_ ? archive_output_ : file_path;
}

gcode::StagingRoot PrintPreparationManager::staging_root() const {
    if (!printer_state_) {
        return gcode::StagingRoot::Hidden;
    }
    return gcode::staging_root_for(printer_state_->plugin_status_state().service_has_helix_plugin(),
                                   printer_state_->plugin_status_state().link_dir(),
                                   printer_state_->profile_state().klipper_hides_dot_dirs());
}

PrintPreparationManager::RewriteResult
PrintPreparationManager::rewrite_archive_plate(const RewriteRoute& job) {
    // HTTP thread. The archive is extracted beside the other scratch copies,
    // under the prefix GCodeFileModifier::cleanup_temp_files() reaps after a
    // crash.
    const auto extract = [&job]() -> std::string {
        const std::string out =
            gcode::GCodeFileModifier::generate_temp_path(gcode::get_filename_basename(
                three_mf::extracted_gcode_name(job.file_path, job.archive_plate, true)));
        if (out.empty()) {
            helix::ui::notify_error_tr(TR_NOOP("Could not extract plate {} of {}: {}"),
                                       job.archive_plate, job.file_path,
                                       std::string("no temp directory"));
            return {};
        }
        const zip::ZipError err =
            three_mf::ThreeMfReader::extract_plate(job.local_download_path, job.archive_plate, out);
        if (err != zip::ZipError::None) {
            helix::ui::notify_error_tr(TR_NOOP("Could not extract plate {} of {}: {}"),
                                       job.archive_plate, job.file_path,
                                       std::string(zip::zip_error_name(err)));
            return {};
        }
        return out;
    };
    const helix::ScopeExit drop_archive([&job]() { hfs::remove(job.local_download_path); });

    const std::string extracted = extract();
    if (extracted.empty()) {
        return RewriteResult{RewriteResult::Kind::Failed, {}};
    }
    spdlog::info("[PrintPreparationManager] Extracted plate {} of {} to {}", job.archive_plate,
                 job.file_path, extracted);

    // A rewrite that changes nothing consumes its input, and the archive
    // itself cannot print, so the plate must outlive the rewrite. A hard link
    // keeps it at no cost in space, so the archive goes now and the disk never
    // holds both; where links are unavailable the archive stays until the
    // rewrite has answered, to extract the plate again from.
    const std::string kept = extracted + ".keep";
    const bool linked = ::link(extracted.c_str(), kept.c_str()) == 0;
    if (linked) {
        hfs::remove(job.local_download_path);
    }
    RewriteResult rewritten = job.rewrite(extracted);
    if (rewritten.kind == RewriteResult::Kind::Unchanged) {
        if (linked) {
            return RewriteResult{RewriteResult::Kind::Rewritten, kept};
        }
        const std::string again = extract();
        rewritten = again.empty() ? RewriteResult{RewriteResult::Kind::Failed, {}}
                                  : RewriteResult{RewriteResult::Kind::Rewritten, again};
    } else if (linked) {
        hfs::remove(kept);
    }
    return rewritten;
}

void PrintPreparationManager::print_archive_plate(const std::string& file_path, int plate,
                                                  NavigateToStatusCallback on_navigate_to_status,
                                                  PrintCompletionCallback on_completion) {
    if (!api_) {
        NOTIFY_ERROR(lv_tr("Cannot start print: not connected to printer"));
        abandon_start("archive_no_api", on_completion);
        return;
    }
    if (!transport_keeps_local_copies()) {
        NOTIFY_WARNING(lv_tr("This .3mf has to be converted to G-code on the printer before it "
                             "can print."));
        abandon_start("archive_no_local_copies", on_completion);
        return;
    }
    const std::string local_download_path = gcode::GCodeFileModifier::generate_temp_path(
        "archive_dl_" + gcode::get_filename_basename(file_path));
    if (local_download_path.empty()) {
        NOTIFY_ERROR(lv_tr("Cannot start print: no temp directory available"));
        abandon_start("archive_no_temp_dir", on_completion);
        return;
    }

    spdlog::info("[PrintPreparationManager] Printing plate {} of {} as extracted G-code", plate,
                 file_path);
    RewriteRoute route;
    route.file_path = file_path;
    route.local_download_path = local_download_path;
    route.archive_plate = plate;
    route.download_error = TR_NOOP("Failed to download the .3mf for printing: {}");
    route.upload_error = TR_NOOP("Failed to upload the extracted G-code: {}");
    // Nothing to change in the plate: it uploads as extracted.
    route.rewrite = [](const std::string& extracted) {
        return RewriteResult{RewriteResult::Kind::Rewritten, extracted};
    };
    rewrite_and_print(std::move(route), std::move(on_navigate_to_status), std::move(on_completion));
}

void PrintPreparationManager::start_print_directly(const std::string& filename,
                                                   NavigateToStatusCallback on_navigate_to_status,
                                                   PrintCompletionCallback on_completion) {
    if (const int plate = archive_plate_for(filename); plate > 0) {
        print_archive_plate(filename, plate, std::move(on_navigate_to_status),
                            std::move(on_completion));
        return;
    }
    if (const std::string printable = printable_path(filename); printable != filename) {
        spdlog::info("[PrintPreparationManager] Printing the printer's conversion {} of {}",
                     printable, filename);
        start_print_directly(printable, std::move(on_navigate_to_status), std::move(on_completion));
        return;
    }
    note_start_sent();
    helix::PrinterState* ps = printer_state_;
    const int epoch = current_preparing_epoch();
    api_->job().start_print(
        filename,
        // Success callback
        [on_navigate_to_status, on_completion, ps, epoch]() {
            spdlog::debug("[PrintPreparationManager] Print started successfully");
            note_start_answered(ps, epoch, true);

            if (on_navigate_to_status) {
                on_navigate_to_status();
            }

            if (on_completion) {
                on_completion(true, "");
            }
        },
        // Error callback
        [filename, on_completion, ps, epoch](const MoonrakerError& error) {
            // A refusal (spool latch, bad path) means nothing reached the
            // printer, so Stop must not cancel there. A transport loss says
            // nothing about whether it did.
            if (!error.is_transport_loss()) {
                note_start_answered(ps, epoch, false);
            }
            helix::ui::queue_update(
                "PrintPreparationManager::start_failed",
                [filename, on_completion, ps, epoch, error]() {
                    if (held_unanswered(ps, epoch, error)) {
                        return;
                    }
                    helix::ui::notify_error_tr(TR_NOOP("Failed to start print: {}"), error);
                    LOG_ERROR_INTERNAL("[PrintPreparationManager] Print start failed for {}: {} "
                                       "({})",
                                       filename, error.message, error.get_type_string());
                    if (on_completion) {
                        on_completion(false, error.message);
                    }
                });
        });
}

} // namespace helix::ui
