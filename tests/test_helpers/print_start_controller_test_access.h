// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file print_start_controller_test_access.h
 * @brief Friend shim for PrintStartController's private members.
 *
 * Declared `friend class ::PrintStartControllerTestAccess` in
 * ui_print_start_controller.h, so this class must live at GLOBAL scope and be
 * defined exactly once across the whole test binary — two translation units
 * each defining their own version with different members is an ODR violation
 * that links fine and misbehaves at runtime. Hence the shared header.
 */

#include "ui_print_start_controller.h"

#include "ams_state.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class PrintStartControllerTestAccess {
  public:
    // --- gate-pipeline runner (test_print_start_gates.cpp) ---

    static void run_gates(helix::ui::PrintStartController& c, size_t index = 0) {
        c.run_gates_from(index);
    }

    static void gate_proceed(helix::ui::PrintStartController& c) {
        c.on_gate_proceed();
    }

    static void gate_cancel(helix::ui::PrintStartController& c) {
        c.on_gate_cancel();
    }

    static void set_gates(helix::ui::PrintStartController& c,
                          std::vector<helix::PrintStartGate> gates) {
        c.gate_list_ = std::move(gates);
    }

    static lv_obj_t* print_gate_modal(const helix::ui::PrintStartController& c) {
        return c.print_gate_modal_;
    }

    static size_t gate_resume_index(const helix::ui::PrintStartController& c) {
        return c.gate_resume_index_;
    }

    /// The file set_file() last handed the controller: {filename, directory}.
    static std::pair<std::string, std::string> file(const helix::ui::PrintStartController& c) {
        return {c.filename_, c.path_};
    }

    static const std::vector<std::string>&
    filament_colors(const helix::ui::PrintStartController& c) {
        return c.filament_colors_;
    }

    /// The context the gates read, gathered as run_gates_from() gathers it.
    static helix::PrintStartContext gather(const helix::ui::PrintStartController& c) {
        return c.gather_print_start_context();
    }

    /// The name of the gate the open dialog belongs to.
    static std::string_view open_gate_name(const helix::ui::PrintStartController& c) {
        return c.gate_list_[c.gate_resume_index_].name;
    }

    // --- print-start success hook (test_job_queue_start_guard.cpp) ---

    /// Fire on_print_started_, the callback the start pipeline invokes once
    /// Moonraker confirms the print actually started. Going through the
    /// stored callback (rather than the owner's consume method) proves the
    /// panel wired set_on_print_started, not just that the consume exists.
    static void fire_print_started(helix::ui::PrintStartController& c) {
        if (c.on_print_started_) {
            c.on_print_started_();
        }
    }

    // --- remap-unsupported discriminator (test_print_start_filament_gate.cpp) ---

    static bool should_warn_remap_unsupported(const helix::AmsBackend& backend) {
        return helix::ui::PrintStartController::should_warn_remap_unsupported(backend);
    }

    // --- remap restore (test_remap_restore_confirmation.cpp) ---

    /// Seed the snapshot apply_filament_remaps() would have taken, without
    /// needing a real print-start round trip.
    /// The record is AmsState's, shared by every controller; @p c is kept so
    /// call sites read as the controller's snapshot. The type is left
    /// unrecorded, which any backend matches.
    static void seed_saved_mapping(helix::ui::PrintStartController& /*c*/, std::vector<int> mapping,
                                   int backend_index) {
        helix::AmsState::instance().set_pending_tool_map_restore(
            backend_index, helix::AmsType::NONE, std::move(mapping));
    }

    static void persist_record() {
        helix::ui::PrintStartController::persist_remap_state();
    }

    static void clear_record() {
        helix::ui::PrintStartController::clear_persisted_remap_state();
    }

    static void restore(helix::ui::PrintStartController& c) {
        c.restore_filament_mapping();
    }

    static bool apply_remaps(helix::ui::PrintStartController& c) {
        return c.apply_filament_remaps();
    }

    /// The pre-print send execute_print_start() makes for a backend that
    /// requires one.
    static void send_preprint(helix::ui::PrintStartController& c, const std::set<int>& tools_used,
                              const std::map<int, int>& remap, std::function<void()> on_done,
                              std::function<void()> on_abort) {
        c.send_backend_preprint_then(tools_used, remap, std::move(on_done), std::move(on_abort));
    }

    /// Register the restore observer the way execute_print_start() does right
    /// after apply_filament_remaps() sends remaps, which is BEFORE start_now
    /// calls begin_preparing(), while the wire may still report the previous
    /// job's terminal state.
    static void observe_for_restore(helix::ui::PrintStartController& c) {
        c.observe_lifecycle_for_restore();
    }

    // --- pre-start object picks (test_print_select_pre_start_exclude.cpp) ---

    /// Run the start pipeline past initiate()'s grace period and gates.
    static void execute(helix::ui::PrintStartController& c) {
        c.execute_print_start();
    }

    static const std::vector<std::string>& exclude_picks(const helix::ui::PrintStartController& c) {
        return c.exclude_picks_;
    }
};
