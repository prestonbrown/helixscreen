// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "abort_manager.h"
#include "app_globals.h"
#include "chamber_stall_prompt.h"
#include "helix_psram_attr.h"
#include "i_moonraker_api.h"
#include "lvgl.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "temperature_controller.h"
#include "text_io.h"
#include "wait_line_reader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "hv/json.hpp"

namespace helix {

ChamberStallAction ChamberStallArbiter::decide(const ChamberStallInputs& in) {
    if (!in.job_active) {
        prompted_unheated_ = false;
        if (in.rejected && in.rejection_seq != toasted_seq_) {
            toasted_seq_ = in.rejection_seq;
            return ChamberStallAction::Toast;
        }
        return ChamberStallAction::None;
    }
    if (!in.chamber_wait) {
        return ChamberStallAction::None;
    }
    if (in.rejected) {
        if (in.rejection_seq == prompted_seq_) {
            return ChamberStallAction::None;
        }
        prompted_seq_ = in.rejection_seq;
        toasted_seq_ = in.rejection_seq;
        return ChamberStallAction::Prompt;
    }
    if (in.chamber_target_deci > 0 || prompted_unheated_) {
        return ChamberStallAction::None;
    }
    prompted_unheated_ = true;
    return ChamberStallAction::Prompt;
}

bool ChamberStallArbiter::should_close(const ChamberStallInputs& in) {
    return !in.job_active || !in.chamber_wait || (!in.rejected && in.chamber_target_deci > 0);
}

ChamberCancelPath chamber_cancel_path(WaitKind kind) {
    switch (kind) {
    case WaitKind::M191:
        return ChamberCancelPath::ClearTargetThenCancel;
    case WaitKind::TemperatureWait:
        return ChamberCancelPath::EmergencyStop;
    case WaitKind::M109:
    case WaitKind::M190:
    case WaitKind::Other:
        break;
    }
    return ChamberCancelPath::ClearTargetThenCancel;
}

namespace ui {
namespace {

ChamberStallPrompt* g_prompt = nullptr;
/// The open emergency-stop confirmation, so the prompt can withdraw it when
/// the wait it was asked for ends. Cleared by every path that closes it.
lv_obj_t* g_estop_confirm = nullptr;

/// The chamber prompt. Its subjects are global so the nested
/// modal_button_row resolves them, and static so they outlive any one show.
class ChamberStallModal : public Modal {
  public:
    const char* get_name() const override {
        return "Chamber Stall";
    }
    const char* component_name() const override {
        return "chamber_stall_modal";
    }

    static void init_subjects() {
        if (subjects_initialized_) {
            return;
        }
        subjects_initialized_ = true;
        lv_subject_init_string(&message_subject_, message_buf_, nullptr, sizeof(message_buf_), "");
        lv_subject_init_string(&hint_subject_, hint_buf_, nullptr, sizeof(hint_buf_), "");
        lv_subject_init_int(&no_retry_subject_, 0);
        lv_subject_init_int(&busy_subject_, 0);
        lv_xml_register_subject(nullptr, "chamber_stall_message", &message_subject_);
        lv_xml_register_subject(nullptr, "chamber_stall_cancel_hint", &hint_subject_);
        lv_xml_register_subject(nullptr, "chamber_stall_no_retry", &no_retry_subject_);
        lv_xml_register_subject(nullptr, "chamber_stall_busy", &busy_subject_);
        StaticSubjectRegistry::instance().register_deinit("ChamberStallModal", []() {
            if (!subjects_initialized_) {
                return;
            }
            lv_xml_unregister_subject(nullptr, "chamber_stall_message");
            lv_xml_unregister_subject(nullptr, "chamber_stall_cancel_hint");
            lv_xml_unregister_subject(nullptr, "chamber_stall_no_retry");
            lv_xml_unregister_subject(nullptr, "chamber_stall_busy");
            lv_subject_deinit(&message_subject_);
            lv_subject_deinit(&hint_subject_);
            lv_subject_deinit(&no_retry_subject_);
            lv_subject_deinit(&busy_subject_);
            subjects_initialized_ = false;
        });
    }

    static void set_content(const std::string& message, const std::string& hint, bool can_retry,
                            bool busy) {
        init_subjects();
        lv_subject_copy_string(&message_subject_, message.c_str());
        lv_subject_copy_string(&hint_subject_, hint.c_str());
        lv_subject_set_int(&no_retry_subject_, can_retry ? 0 : 1);
        lv_subject_set_int(&busy_subject_, busy ? 1 : 0);
    }

    /// The open prompt's dialog, wherever it sits in the stack.
    static lv_obj_t* open_dialog() {
        return ModalStack::instance().dialog_for_component("chamber_stall_modal");
    }
    static bool showing() {
        return open_dialog() != nullptr;
    }

  protected:
    void on_show() override {
        wire_ok_button("btn_primary");
        wire_cancel_button("btn_secondary");
        wire_quaternary_button("btn_quaternary");
    }
    // Try Again stays open: the outcome shows here, and the prompt closes on
    // its own once the heater takes the target or the wait ends.
    void on_ok() override;
    void on_quaternary() override;

  private:
    static inline lv_subject_t message_subject_{};
    static inline HELIX_PSRAM_BSS char message_buf_[384] = {};
    static inline lv_subject_t hint_subject_{};
    static inline char hint_buf_[192] = {};
    static inline lv_subject_t no_retry_subject_{};
    static inline lv_subject_t busy_subject_{};
    static inline bool subjects_initialized_ = false;
};

bool job_holds_machine() {
    lv_subject_t* job = get_printer_state().print_state().get_job_holds_machine_subject();
    return job && lv_subject_get_int(job) != 0;
}

} // namespace

class ChamberStallModalAccess {
  public:
    static void try_again() {
        if (g_prompt) {
            g_prompt->on_try_again();
        }
    }
    static void cancel() {
        if (g_prompt) {
            g_prompt->on_cancel();
        } else {
            ChamberStallPrompt::cancel_print(WaitKind::Other);
        }
    }
};

namespace {
void ChamberStallModal::on_ok() {
    ChamberStallModalAccess::try_again();
}
void ChamberStallModal::on_quaternary() {
    hide();
    ChamberStallModalAccess::cancel();
}
} // namespace

ChamberStallPrompt::ChamberStallPrompt() {
    ChamberStallModal::init_subjects();
    g_prompt = this;
    auto& state = get_printer_state();
    auto& temps = state.temperature_state();
    const auto& lifetime = state.get_subjects_lifetime();
    auto on_change = [](ChamberStallPrompt* self, int) { self->evaluate(); };

    SubjectLifetime rejected_lifetime;
    lv_subject_t* rejected = temps.get_chamber_heater_command_rejected_subject(rejected_lifetime);
    rejected_observer_ = observe<int>(rejected, this, on_change, rejected_lifetime);
    SubjectLifetime seq_lifetime;
    lv_subject_t* seq = temps.get_chamber_heater_rejection_seq_subject(seq_lifetime);
    seq_observer_ = observe<int>(seq, this, on_change, seq_lifetime);
    // The reason can change under a latch that stays set (a later code on the
    // same rejection), and an open prompt shows it.
    text_observer_ = observe<const char*>(
        temps.get_chamber_heater_rejection_text_subject(), this,
        [](ChamberStallPrompt* self, const char*) { self->evaluate(); }, lifetime);
    SubjectLifetime target_lifetime;
    lv_subject_t* target = temps.get_chamber_target_subject(target_lifetime);
    target_observer_ = observe<int>(target, this, on_change, target_lifetime);
    wait_observer_ = observe<int>(state.print_state().get_print_start_chamber_wait_subject(), this,
                                  on_change, lifetime);
    job_observer_ = observe<int>(state.print_state().get_job_holds_machine_subject(), this,
                                 on_change, lifetime);
}

ChamberStallPrompt::~ChamberStallPrompt() {
    if (g_prompt == this) {
        g_prompt = nullptr;
    }
}

double ChamberStallPrompt::retry_target_c(double wait_target_c) {
    const auto rejection = get_printer_state().temperature_state().chamber_command_rejection();
    if (rejection.requested_target_c > 0) {
        return rejection.requested_target_c;
    }
    const auto* controller = get_temperature_controller();
    if (controller && controller->last_chamber_target_c() > 0) {
        return controller->last_chamber_target_c();
    }
    return wait_target_c > 0 ? wait_target_c : 0.0;
}

bool ChamberStallPrompt::retry() {
    auto* controller = get_temperature_controller();
    const double target =
        retry_target_c(g_prompt ? chamber_wait_target_c(g_prompt->current_wait()) : 0);
    if (!controller || target <= 0 || !controller->chamber_has_direct_route()) {
        spdlog::info("[ChamberStall] Retry unavailable (target={}°C, route={})", target,
                     controller && controller->chamber_has_direct_route());
        return false;
    }
    controller->retry_chamber_target(target);
    return true;
}

void ChamberStallPrompt::cancel_print(WaitKind kind) {
    auto* controller = get_temperature_controller();
    const ChamberCancelPath path = chamber_cancel_path(kind);
    spdlog::info("[ChamberStall] Cancel print: wait kind {}, path {}", static_cast<int>(kind),
                 static_cast<int>(path));
    if (path == ChamberCancelPath::EmergencyStop) {
        ConfirmOptions options;
        options.on_cancel = []() { g_estop_confirm = nullptr; };
        options.on_dismiss = []() { g_estop_confirm = nullptr; };
        g_estop_confirm = helix::ui::modal_confirm(
            lv_tr("Stop with an emergency stop?"),
            lv_tr("The printer is waiting for the chamber to reach a temperature, and only an "
                  "emergency stop and a firmware restart end that wait. The print stops at once "
                  "and cannot be resumed."),
            ModalSeverity::Error, lv_tr("Emergency stop"),
            []() {
                g_estop_confirm = nullptr;
                confirm_emergency_stop();
            },
            options);
        return;
    }
    // A target of 0 ends an M191 wait, and so does the cancel itself on
    // firmware that fires pause_resume:cancel. The cancel goes straight out:
    // a queue probe would stall behind the wait and escalate for nothing.
    // AbortManager escalates only if the cancel then times out, per the
    // user's cancel-escalation setting.
    if (controller && controller->chamber_has_direct_route()) {
        controller->retry_chamber_target(0.0);
    }
    AbortManager::instance().start_abort(AbortManager::Entry::Cancel);
}

WaitLine ChamberStallPrompt::current_wait() const {
    const auto& temps = get_printer_state().temperature_state();
    if (temps.chamber_wait_active()) {
        WaitLine wait;
        wait.kind = WaitKind::M191;
        wait.target = temps.chamber_wait_target_c();
        return wait;
    }
    return wait_line_;
}

// Which wait holds the queue decides how Cancel ends it, so a Cancel tapped
// while that is still being read waits for the answer, briefly.
bool ChamberStallPrompt::emergency_stop_confirm_open() {
    return g_estop_confirm && ModalStack::instance().backdrop_for(g_estop_confirm) != nullptr &&
           !ModalStack::instance().is_exiting(ModalStack::instance().backdrop_for(g_estop_confirm));
}

bool ChamberStallPrompt::confirm_emergency_stop() {
    auto& print = get_printer_state().print_state();
    const bool waiting = lv_subject_get_int(print.get_print_start_chamber_wait_subject()) != 0;
    if (!job_holds_machine() || !waiting) {
        spdlog::warn("[ChamberStall] Emergency stop not sent: the chamber wait already ended "
                     "(job={}, wait={})",
                     job_holds_machine(), waiting);
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("The chamber wait already ended; nothing was stopped."),
                                      5000);
        return false;
    }
    spdlog::warn("[ChamberStall] Emergency stop confirmed to end a chamber wait");
    AbortManager::instance().start_abort(AbortManager::Entry::EmergencyStop);
    return true;
}

void ChamberStallPrompt::on_cancel() {
    if (!wait_line_pending_ || get_printer_state().temperature_state().chamber_wait_active()) {
        cancel_print(current_wait().kind);
        return;
    }
    spdlog::info("[ChamberStall] Cancel waits for the waiting command to be read");
    pending_cancel_ = true;
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t*) {
            if (g_prompt && g_prompt->pending_cancel_) {
                spdlog::info("[ChamberStall] Waiting command not read in time; cancelling");
                g_prompt->cancel_wait_timer_.release(); // one-shot: LVGL deletes it
                g_prompt->run_pending_cancel();
            }
        },
        kCancelClassifyWaitMs, nullptr);
    lv_timer_set_repeat_count(timer, 1);
    cancel_wait_timer_.reset(timer);
}

void ChamberStallPrompt::on_try_again() {
    auto* controller = get_temperature_controller();
    const double target = retry_target_c(chamber_wait_target_c(current_wait()));
    if (!controller || target <= 0 || !controller->chamber_has_direct_route()) {
        note_ = lv_tr("The heater can't be changed while the printer is waiting.");
        refresh_content();
        return;
    }
    retry_busy_ = true;
    note_ = lv_tr("Trying again...");
    refresh_content();
    auto tok = lifetime_.token();
    controller->retry_chamber_target(target, [this, tok](const ChamberRouteReply& reply) {
        if (tok.expired()) {
            return;
        }
        retry_busy_ = false;
        switch (reply.outcome) {
        case ChamberRouteOutcome::Accepted:
        case ChamberRouteOutcome::Superseded:
            note_ = lv_tr("The heater took the target.");
            break;
        case ChamberRouteOutcome::Pending:
            note_ = lv_tr("The heater hasn't answered yet. Still trying.");
            break;
        case ChamberRouteOutcome::PrinterShutdown:
            note_ = lv_tr("The printer is shut down.");
            break;
        case ChamberRouteOutcome::Refused:
            // The status frames bring the reason; the sentence above it says it.
            note_.clear();
            break;
        case ChamberRouteOutcome::Unavailable:
            note_ = lv_tr("The heater can't be changed while the printer is waiting.");
            break;
        }
        refresh_content();
    });
}

void ChamberStallPrompt::evaluate() {
    auto& state = get_printer_state();
    auto& temps = state.temperature_state();
    ChamberStallInputs in;
    in.job_active = job_holds_machine();
    in.chamber_wait =
        lv_subject_get_int(state.print_state().get_print_start_chamber_wait_subject()) != 0;
    in.rejected = lv_subject_get_int(temps.get_chamber_heater_command_rejected_subject()) != 0;
    in.rejection_seq = lv_subject_get_int(temps.get_chamber_heater_rejection_seq_subject());
    in.chamber_target_deci = lv_subject_get_int(temps.get_chamber_target_subject());
    // A later chamber wait in the same job may be another command.
    if (!in.chamber_wait && wait_line_probed_ && !pending_cancel_) {
        reset_wait_line();
    }
    if (ChamberStallArbiter::should_close(in)) {
        if (ChamberStallModal::showing()) {
            spdlog::info("[ChamberStall] Closing the prompt: job={} wait={} rejected={} "
                         "target={}",
                         in.job_active, in.chamber_wait, in.rejected, in.chamber_target_deci);
            Modal::hide(ChamberStallModal::open_dialog());
        }
        // An emergency stop asked for to end a wait that has since ended
        // would stop whatever the printer is doing now.
        if (emergency_stop_confirm_open()) {
            spdlog::info("[ChamberStall] Withdrawing the emergency-stop confirmation");
            Modal::hide(g_estop_confirm);
            g_estop_confirm = nullptr;
        }
    }
    switch (arbiter_.decide(in)) {
    case ChamberStallAction::Prompt:
        show_prompt();
        break;
    case ChamberStallAction::Toast: {
        const std::string text =
            fmt::format(fmt::runtime(lv_tr("Chamber heater didn't start. {}")),
                        lv_subject_get_string(temps.get_chamber_heater_rejection_text_subject()));
        ToastManager::instance().show(ToastSeverity::WARNING, text.c_str(), 6000);
        break;
    }
    case ChamberStallAction::None:
        if (ChamberStallModal::showing()) {
            refresh_content();
        }
        break;
    }
}

void ChamberStallPrompt::refresh_content() {
    auto& temps = get_printer_state().temperature_state();
    const auto* controller = get_temperature_controller();
    const WaitLine wait = current_wait();
    const double wait_target = chamber_wait_target_c(wait);
    const double wait_c = wait_target > 0 ? wait_target : retry_target_c(wait_target);
    const bool can_retry =
        retry_target_c(wait_target) > 0 && controller && controller->chamber_has_direct_route();

    std::string message =
        wait_c > 0 ? fmt::format(fmt::runtime(lv_tr("The print is waiting for the chamber to "
                                                    "reach {}°C, but the heater is not heating.")),
                                 static_cast<int>(std::lround(wait_c)))
                   : std::string(lv_tr("The print is waiting for the chamber, but the heater is "
                                       "not heating."));
    const char* reason = lv_subject_get_string(temps.get_chamber_heater_rejection_text_subject());
    if (reason && reason[0]) {
        message += "\n";
        message += reason;
    }
    if (!can_retry) {
        message += "\n";
        message += lv_tr("The heater can't be changed while the printer is waiting.");
    } else if (!note_.empty()) {
        message += "\n";
        message += note_;
    }

    std::string hint;
    switch (chamber_cancel_path(wait.kind)) {
    case ChamberCancelPath::ClearTargetThenCancel:
        hint = lv_tr("Canceling turns the chamber target off so the wait ends, then stops the "
                     "print.");
        break;
    case ChamberCancelPath::EmergencyStop:
        hint = lv_tr("This wait ends only at its temperature: canceling takes an emergency stop "
                     "and a firmware restart.");
        break;
    }
    ChamberStallModal::set_content(message, hint, can_retry, retry_busy_);
}

// The command holding the queue decides how Cancel ends it. Read the line the
// print is executing: the one starting at virtual_sdcard.file_position.
void ChamberStallPrompt::probe_wait_line() {
    if (wait_line_probed_) {
        return;
    }
    wait_line_probed_ = true;
    IMoonrakerAPI* api = get_moonraker_api();
    const char* file =
        lv_subject_get_string(get_printer_state().print_state().get_print_filename_subject());
    if (!api || !file || !file[0] || !api->transfers().supports_range_reads()) {
        spdlog::info("[ChamberStall] Wait line not read: no api, file or ranged reads");
        return;
    }
    const std::string filename = file;
    const uint32_t gen = ++wait_line_gen_;
    wait_line_pending_ = true;
    auto tok = lifetime_.token();
    // Every path out of the read lands in on_wait_line() on the main thread,
    // so a Cancel waiting on the classification always gets an answer.
    auto give_up = [tok, gen](const char* what, const MoonrakerError& e) {
        spdlog::info("[ChamberStall] {} not read: {}", what, e.message);
        tok.defer("ChamberStallPrompt::wait_line_failed", [gen]() {
            if (g_prompt) {
                g_prompt->on_wait_line(gen, std::nullopt);
            }
        });
    };
    api->get_client().send_jsonrpc(
        "printer.objects.query", {{"objects", {{"virtual_sdcard", {"file_position"}}}}},
        [tok, filename, gen, give_up](const nlohmann::json& resp) {
            // WebSocket thread: parse only; the download starts on the main thread.
            const std::optional<uint64_t> position = virtual_sdcard_file_position(resp);
            if (!position) {
                give_up("virtual_sdcard.file_position", MoonrakerError{});
                return;
            }
            const uint64_t pos = *position;
            tok.defer("ChamberStallPrompt::read_wait_line", [tok, filename, pos, gen, give_up]() {
                IMoonrakerAPI* api = get_moonraker_api();
                if (!api) {
                    if (g_prompt) {
                        g_prompt->on_wait_line(gen, std::nullopt);
                    }
                    return;
                }
                read_wait_line_at(
                    api->transfers(), filename, pos,
                    [tok, gen](const WaitLine& line) {
                        tok.defer("ChamberStallPrompt::wait_line", [line, gen]() {
                            if (g_prompt) {
                                g_prompt->on_wait_line(gen, line);
                            }
                        });
                    },
                    [give_up](const MoonrakerError& e) { give_up("Wait line", e); });
            });
        },
        [give_up](const MoonrakerError& e) { give_up("virtual_sdcard", e); });
}

void ChamberStallPrompt::on_wait_line(uint32_t gen, std::optional<WaitLine> line) {
    if (gen != wait_line_gen_) {
        return; // a read for a wait that has since ended
    }
    wait_line_pending_ = false;
    if (line) {
        spdlog::info("[ChamberStall] Waiting command: kind {}, {}°C", static_cast<int>(line->kind),
                     chamber_wait_target_c(*line));
        wait_line_ = *line;
    }
    if (ChamberStallModal::showing()) {
        refresh_content();
    }
    if (pending_cancel_) {
        run_pending_cancel();
    }
}

void ChamberStallPrompt::reset_wait_line() {
    wait_line_ = {};
    wait_line_probed_ = false;
    wait_line_pending_ = false;
    ++wait_line_gen_;
}

void ChamberStallPrompt::run_pending_cancel() {
    pending_cancel_ = false;
    cancel_wait_timer_.reset();
    cancel_print(current_wait().kind);
}

void ChamberStallPrompt::show_prompt() {
    note_.clear();
    retry_busy_ = false;
    probe_wait_line();
    refresh_content();
    spdlog::warn("[ChamberStall] Prompting: wait kind {}, retry target {}°C",
                 static_cast<int>(current_wait().kind),
                 retry_target_c(chamber_wait_target_c(current_wait())));
    // A prompt already up shows the new rejection through its bound text.
    if (ChamberStallModal::showing()) {
        return;
    }
    auto modal = std::make_unique<ChamberStallModal>();
    if (!modal->show(lv_screen_active())) {
        spdlog::error("[ChamberStall] chamber_stall_modal failed to create");
        return;
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
}

} // namespace ui
} // namespace helix
