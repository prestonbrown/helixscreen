// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_print_preparation_manager.h"

#include "gcode_ops_detector.h"
#include "lvgl.h"

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Friend of PrintPreparationManager: reaches the private collectors and the
// pre-start send state.
class PrintPreparationManagerTestAccess {
  public:
    static std::vector<std::pair<std::string, std::string>>
    get_skip_params(const helix::ui::PrintPreparationManager& m) {
        return m.collect_macro_skip_params();
    }
    static std::vector<std::string>
    get_pre_start_gcode_lines(const helix::ui::PrintPreparationManager& m,
                              const std::string& filename = {}) {
        return m.collect_pre_start_gcode_lines(filename);
    }
    static std::vector<helix::gcode::OperationType>
    get_ops_to_disable(const helix::ui::PrintPreparationManager& m) {
        return m.collect_ops_to_disable();
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
    static void modify_and_print(helix::ui::PrintPreparationManager& m,
                                 const std::string& file_path,
                                 std::function<void()> on_navigate_to_status = nullptr) {
        m.modify_and_print(file_path, {}, {}, std::move(on_navigate_to_status));
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
};
