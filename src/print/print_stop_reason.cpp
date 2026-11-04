// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_stop_reason.h"

#include "core/observer_factory.h"
#include "error_event.h"
#include "firmware_error_envelope.h"
#include "gcode_response_lines.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix {

namespace {

// RAW_PRINT_STATE_OK: the printer's own pause, cancel and error are what a
// reason explains.
bool is_stop(PrintJobState state) {
    return state == PrintJobState::PAUSED || state == PrintJobState::CANCELLED ||
           state == PrintJobState::ERROR;
}

} // namespace

std::string gcode_error_reason_text(const std::string& line) {
    std::string text = journal_error_text(line);
    if (const auto nl = text.find_first_of("\r\n"); nl != std::string::npos) {
        text = journal_error_text(text.substr(0, nl));
    }
    return decode_firmware_error_text(text);
}

std::optional<std::string> last_gcode_error_text(const nlohmann::json& msg) {
    std::optional<std::string> out;
    for_each_gcode_response_line(msg, [&out](const std::string& line) {
        if (parse_gcode_error_line(line).prefix == GcodeErrorPrefix::Bang) {
            out = gcode_error_reason_text(line);
        }
    });
    if (out && out->empty()) {
        return std::nullopt;
    }
    return out;
}

std::string recent_gcode_error(const PrinterPrintState& state, double now) {
    return is_recent_cause(state.last_gcode_error_time(), now) ? state.last_gcode_error()
                                                               : std::string();
}

std::string decide_print_stop_reason(PrintJobState stop, const std::string& print_message,
                                     const std::vector<JournalEntry>& entries, double stop_time) {
    if (!is_stop(stop)) {
        return {};
    }
    if (!print_message.empty()) {
        return print_message;
    }
    const JournalEntry* newest = nullptr;
    for (const auto& e : entries) {
        if (e.kind != journal_kind::kGcodeError || e.text.empty() ||
            !is_recent_cause(e.time, stop_time)) {
            continue;
        }
        if (newest == nullptr || e.time >= newest->time) {
            newest = &e;
        }
    }
    return newest ? newest->text : std::string();
}

std::optional<double> gcode_store_clock_shift(const std::vector<GcodeStoreEntry>& store,
                                              const std::string& line, double seen_at) {
    const GcodeStoreEntry* match = nullptr;
    for (const auto& item : store) {
        if (item.type == "response" && item.message == line &&
            (match == nullptr || item.time >= match->time)) {
            match = &item;
        }
    }
    if (match == nullptr) {
        return std::nullopt;
    }
    return seen_at - match->time;
}

PrintStopReasonTracker::PrintStopReasonTracker(IMoonrakerClient& client, PrinterState& state,
                                               std::function<double()> now)
    : client_(client), state_(state), now_(now ? std::move(now) : wall_now_s),
      stop_(PrintJobState::STANDBY) { // RAW_PRINT_STATE_OK: no stop yet
    // The tap runs on the WebSocket thread, and the client may call it after
    // the tracker is gone: it reads only what it holds by value, and only the
    // deferred body, behind the token, reaches the tracker. Only a frame that
    // carries an error reaches the update queue.
    client_.register_method_callback(
        "notify_gcode_response", kHandlerName,
        [self = this, token = lifetime_.token(), clock = now_,
         live = live_line_](const nlohmann::json& msg) {
            if (token.expired_no_lvgl()) { // L081_OK: no LVGL here; the tracker only via defer
                return;
            }
            const double now = clock();
            for_each_gcode_response_line(msg, [&](const std::string& line) {
                std::lock_guard<std::mutex> lock(live->mutex);
                live->text = line;
                live->time = now;
            });
            auto text = last_gcode_error_text(msg);
            if (!text) {
                return;
            }
            JournalEntry e;
            e.kind = journal_kind::kGcodeError;
            e.time = now;
            e.text = std::move(*text);
            token.defer("PrintStopReason::gcode_error",
                        [self, e = std::move(e)]() mutable { self->on_gcode_error(std::move(e)); });
        });

    auto& ps = state_.print_state();
    // Immediate: the reason is in place before the lifecycle publishes the stop,
    // so the overlay, toast and modal it raises can read it.
    // RAW_PRINT_STATE_OK: a pause is the printer's own state, and the reason
    // explains what the printer did.
    job_state_observer_ = ui::observe<int>(
        ps.get_print_state_enum_subject(), this,
        [](PrintStopReasonTracker* self, int) { self->on_state(); }, state_.get_subjects_lifetime(),
        ui::Dispatch::Immediate);
    outcome_observer_ = ui::observe<int>(
        ps.get_print_outcome_subject(), this,
        [](PrintStopReasonTracker* self, int) { self->on_state(); }, state_.get_subjects_lifetime(),
        ui::Dispatch::Immediate);
    message_observer_ = ui::observe<const char*>(
        ps.get_print_message_subject(), this,
        [](PrintStopReasonTracker* self, const char*) { self->publish(); },
        state_.get_subjects_lifetime());
}

PrintStopReasonTracker::~PrintStopReasonTracker() {
    lifetime_.invalidate();
    client_.unregister_method_callback("notify_gcode_response", kHandlerName);
}

PrintJobState PrintStopReasonTracker::current_stop() const {
    auto& ps = state_.print_state();
    const auto outcome =
        static_cast<PrintOutcome>(lv_subject_get_int(ps.get_print_outcome_subject()));
    if (outcome == PrintOutcome::CANCELLED) {
        return PrintJobState::CANCELLED;
    }
    if (outcome == PrintOutcome::ERROR) {
        return PrintJobState::ERROR;
    }
    // RAW_PRINT_STATE_OK: see the job-state observer.
    if (ps.get_print_job_state() == PrintJobState::PAUSED) {
        return PrintJobState::PAUSED;
    }
    return PrintJobState::STANDBY;
}

bool PrintStopReasonTracker::stopped() const {
    return is_stop(stop_);
}

void PrintStopReasonTracker::on_state() {
    const PrintJobState stop = current_stop();
    if (stop == stop_) {
        return;
    }
    // RAW_PRINT_STATE_OK: the printer's own pause.
    const bool was_paused = stop_ == PrintJobState::PAUSED;
    const std::string previous =
        lv_subject_get_string(state_.print_state().get_print_stop_reason_subject());
    stop_ = stop;
    recovered_.clear();
    stop_message_.clear();
    // Canceling a paused print ends it for the reason it paused.
    carried_reason_ = was_paused && stopped() ? previous : std::string();
    if (!stopped()) {
        publish();
        return;
    }
    stop_time_ = now_();
    const std::uint32_t stop_id = ++stop_id_;
    publish();
    if (lv_subject_get_int(state_.print_state().get_print_stop_reason_visible_subject()) != 0) {
        return;
    }
    // Nothing seen live explains the stop: the error may have come before this
    // screen connected, or before the tap was registered.
    client_.get_gcode_store(
        kGcodeStoreCount,
        lifetime_.bg_cb("PrintStopReason::gcode_store",
                        [this, stop_id](const std::vector<GcodeStoreEntry>& store) {
                            on_gcode_store(store, stop_id);
                        }),
        [](const MoonrakerError& err) {
            spdlog::debug("[PrintStopReason] gcode_store read failed: {}", err.message);
        });
}

void PrintStopReasonTracker::on_gcode_error(JournalEntry e) {
    state_.print_state().set_last_gcode_error(e.text, e.time);
    last_error_ = std::move(e);
    if (stopped()) {
        publish();
    }
}

void PrintStopReasonTracker::on_gcode_store(const std::vector<GcodeStoreEntry>& store,
                                            std::uint32_t stop_id) {
    if (stop_id != stop_id_ || !stopped() || store.empty()) {
        return;
    }
    std::string line;
    double seen_at = 0.0;
    {
        std::lock_guard<std::mutex> lock(live_line_->mutex);
        line = live_line_->text;
        seen_at = live_line_->time;
    }
    // Moonraker stamps the store with its own clock. A store holding no line
    // seen live cannot be placed against the stop, and its newest line can be
    // long before it on a printer that printed nothing as it stopped.
    const auto shift = line.empty() ? std::nullopt : gcode_store_clock_shift(store, line, seen_at);
    if (!shift) {
        spdlog::debug("[PrintStopReason] gcode_store holds no line seen live; not read");
        return;
    }
    recovered_ = backfill_from_gcode_store(store, stop_time_ - *shift - kEventCauseWindowS -
                                                      kEventCauseLateS);
    for (auto& e : recovered_) {
        e.time += *shift;
        e.text = gcode_error_reason_text(e.text);
    }
    publish();
}

void PrintStopReasonTracker::publish() {
    std::vector<JournalEntry> entries = recovered_;
    if (last_error_) {
        entries.push_back(*last_error_);
    }
    const char* message = lv_subject_get_string(state_.print_state().get_print_message_subject());
    if (stopped() && message && *message) {
        stop_message_ = message;
    }
    std::string reason = decide_print_stop_reason(stop_, stop_message_, entries, stop_time_);
    if (reason.empty()) {
        reason = carried_reason_;
    }
    if (!reason.empty()) {
        spdlog::info("[PrintStopReason] {}", reason);
    }
    state_.print_state().set_print_stop_reason(reason);
}

} // namespace helix
