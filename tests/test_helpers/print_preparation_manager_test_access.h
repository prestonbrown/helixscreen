// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_print_preparation_manager.h"

#include "gcode_ops_detector.h"
#include "lvgl.h"

#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Friend of PrintPreparationManager: reaches the private collectors and the
// pre-start send state.
class PrintPreparationManagerTestAccess {
  public:
    /// Whether a start's option override is set (set_start_option_override()).
    static bool has_start_option_override(const helix::ui::PrintPreparationManager& m) {
        return static_cast<bool>(m.start_option_override_);
    }

    /// What a reconnect or a Klipper restart runs.
    static void refresh_macro_analysis(helix::ui::PrintPreparationManager& m) {
        m.refresh_macro_analysis();
    }

    static std::vector<std::pair<std::string, std::string>>
    get_skip_params(const helix::ui::PrintPreparationManager& m) {
        return m.collect_macro_skip_params();
    }
    static std::vector<std::string>
    get_pre_start_gcode_lines(const helix::ui::PrintPreparationManager& m,
                              const std::string& filename = {},
                              const helix::gcode::ScanResult* scan = nullptr) {
        return m.collect_pre_start_gcode_lines(filename, true, scan);
    }
    /// A scan neither the detail view nor a start asked for: it holds no file.
    static void request_scan(helix::ui::PrintPreparationManager& m, const std::string& filename,
                             helix::FileVersion version = {}) {
        m.request_scan(filename, "", "", version);
    }
    static helix::PreprintPhaseOverrides
    preprint_phase_overrides(const helix::ui::PrintPreparationManager& m) {
        return m.preprint_phase_overrides();
    }
    /// Keep @p scan as the answer for the file whose scan_key() is @p key.
    static void keep_scan(helix::ui::PrintPreparationManager& m, const std::string& key,
                          helix::gcode::ScanResult scan) {
        m.keep_scan(key, key, {}, std::move(scan), {});
    }
    static size_t kept_scan_count(const helix::ui::PrintPreparationManager& m) {
        return m.kept_scans_.size();
    }
    static std::vector<helix::gcode::OperationType>
    get_ops_to_disable(const helix::ui::PrintPreparationManager& m) {
        const auto& shown = m.get_scan_result();
        return m.collect_ops_to_disable(shown ? &*shown : nullptr);
    }
    static std::string build_pre_start_gcode_block(const std::string& setup_gcode,
                                                   const std::vector<std::string>& lines,
                                                   bool emit_setup) {
        return helix::ui::PrintPreparationManager::build_pre_start_gcode_block(setup_gcode, lines,
                                                                               emit_setup);
    }
    /// The modify route's first synchronous bail-out. Private because nothing
    /// outside the manager should dispatch it, but it is the cheapest reachable
    /// failure exit and every other one retires the job the same way.
    static void
    modify_and_print(helix::ui::PrintPreparationManager& m, const std::string& file_path,
                     helix::ui::NavigateToStatusCallback on_navigate_to_status = nullptr,
                     helix::ui::PrintCompletionCallback on_completion = nullptr) {
        // start_print() hands its verified scan to the modify step; a test
        // entering at the modify step hands it the cached one the same way.
        m.start_scan_ = m.get_scan_result();
        m.modify_and_print(file_path, {}, {}, std::move(on_navigate_to_status),
                           std::move(on_completion));
    }
    /// Age the pre-start send timestamp. Production stamps it when the
    /// pre-start gcode RPC leaves; a test models a backed-up ack by winding
    /// it back past the staleness bound.
    static void set_pre_start_sent_ago(helix::ui::PrintPreparationManager& m,
                                       std::chrono::seconds ago) {
        m.pre_start_sent_at_ = std::chrono::steady_clock::now() - ago;
    }
    /// The preparing-job epoch the pre-start block was sent under. Production
    /// snapshots it when the RPC leaves; a test sets it to model a send that
    /// happened while a specific job was armed.
    static void set_pre_start_epoch(helix::ui::PrintPreparationManager& m, int epoch) {
        m.pre_start_epoch_ = epoch;
    }
    /// Drive the shared continuation every pre-start path funnels through.
    static void continue_print_start(helix::ui::PrintPreparationManager& m, const std::string& file,
                                     helix::ui::PrintCompletionCallback on_completion) {
        m.continue_print_start(file, {}, nullptr, on_completion);
    }
    static lv_timer_t* pending_wait_timer(const helix::ui::PrintPreparationManager& m) {
        return m.pre_start_wait_guard_.pending_timer();
    }

    /// Extract plate @p plate of the local archive @p local_zip and run @p rewrite on it,
    /// as an archive start does on its HTTP thread. Returns the copy to upload, or "".
    static std::string
    rewrite_archive_plate(const std::string& archive_path, const std::string& local_zip, int plate,
                          std::function<bool(const std::string& extracted)> changes) {
        helix::ui::PrintPreparationManager::RewriteRoute route;
        route.file_path = archive_path;
        route.local_download_path = local_zip;
        route.archive_plate = plate;
        route.rewrite = [changes](const std::string& extracted) {
            using R = helix::ui::PrintPreparationManager::RewriteResult;
            if (changes(extracted)) {
                return R{R::Kind::Rewritten, extracted};
            }
            std::remove(extracted.c_str()); // an unchanged rewrite consumes its input
            return R{R::Kind::Unchanged, {}};
        };
        const auto result = helix::ui::PrintPreparationManager::rewrite_archive_plate(route);
        return result.kind == helix::ui::PrintPreparationManager::RewriteResult::Kind::Rewritten
                   ? result.modified_path
                   : std::string();
    }
};
