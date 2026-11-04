// SPDX-License-Identifier: GPL-3.0-or-later
#include "event_journal_recorder.h"

#include "app_globals.h"
#include "core/data_root_resolver.h"
#include "core/observer_factory.h"
#include "gcode_response_lines.h"
#include "http_executor.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "system/debug_bundle_collector.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

namespace {

constexpr const char* kHandlerName = "event_journal";
constexpr int kGcodeStoreCount = 1000;
constexpr int kHistoryBackfillLimit = 50;
/// klippy.log read on a reconnect whose gap held a shutdown. Klipper logs a
/// Stats line a second while printing, so this spans about 15 minutes of a
/// print and far longer idle.
constexpr int kKlippyLogWindowBytes = 256 * 1024;

} // namespace

EventJournalRecorder::EventJournalRecorder(IMoonrakerClient* client, LocalEventJournal& journal,
                                           std::string journal_path)
    : client_(client), journal_(journal), path_(std::move(journal_path)) {
    if (path_.empty()) {
        path_ = get_user_config_dir() + "/event_journal.json";
    }
    journal_.load_file(path_, wall_now_s());

    // RAW_PRINT_STATE_OK: the journal records the pause Klipper reported.
    print_state_observer_ = ui::observe_print_state(
        get_printer_state().print_state().get_print_state_enum_subject(), this,
        [](EventJournalRecorder* self, PrintJobState state) {
            self->on_print_state(static_cast<int>(state));
        },
        get_printer_state().get_subjects_lifetime());

    if (!client_) {
        return;
    }
    // Every registration delivers on the WebSocket thread; bg_cb moves the
    // body to the main thread, where the plugin state and printer subjects
    // are read, and drops it once this recorder is gone.
    client_->register_method_callback(
        "notify_gcode_response", kHandlerName,
        lifetime_.bg_cb("EventJournal::gcode_response",
                        [this](const nlohmann::json& msg) { on_gcode_response(msg); }));
    client_->register_method_callback(
        "notify_klippy_shutdown", kHandlerName,
        lifetime_.bg_cb("EventJournal::klippy_shutdown",
                        [this](const nlohmann::json&) { on_klippy_shutdown(); }));
    client_->register_method_callback(
        "notify_klippy_disconnected", kHandlerName,
        lifetime_.bg_cb("EventJournal::klippy_disconnected", [this](const nlohmann::json&) {
            record(JournalEntry{0, 0.0, journal_kind::kDisconnect});
        }));
    client_->register_method_callback(
        "notify_klippy_ready", kHandlerName,
        lifetime_.bg_cb("EventJournal::klippy_ready", [this](const nlohmann::json&) {
            record(JournalEntry{0, 0.0, journal_kind::kReady});
        }));
    client_->register_method_callback(
        "notify_history_changed", kHandlerName,
        lifetime_.bg_cb("EventJournal::history_changed",
                        [this](const nlohmann::json& msg) { on_history_changed(msg); }));
    client_->add_connected_observer(
        kHandlerName, lifetime_.bg_cb("EventJournal::connected", [this]() { on_connected(); }));
}

EventJournalRecorder::~EventJournalRecorder() {
    if (client_) {
        for (const char* method :
             {"notify_gcode_response", "notify_klippy_shutdown", "notify_klippy_disconnected",
              "notify_klippy_ready", "notify_history_changed"}) {
            client_->unregister_method_callback(method, kHandlerName);
        }
        client_->remove_connected_observer(kHandlerName);
    }
}

bool EventJournalRecorder::recording() const {
    return decide_journal_source(get_printer_state().plugin_status_state().plugin_feature(
                                     kJournalFeatureName, kJournalFeatureMinVersion))
        .record_locally;
}

void EventJournalRecorder::record(JournalEntry e) {
    if (!recording()) {
        return;
    }
    if (e.time <= 0.0) {
        e.time = wall_now_s();
    }
    if (e.job_id.empty() && e.filename.empty() && e.kind != journal_kind::kJobEnd) {
        e.job_id = job_id_;
        e.filename = filename_;
    }
    const double now = wall_now_s();
    if (journal_.append(std::move(e), now)) {
        save();
    }
}

void EventJournalRecorder::save() const {
    journal_.save_file(path_);
}

void EventJournalRecorder::on_gcode_response(const nlohmann::json& msg) {
    for_each_gcode_response_line(msg, [this](const std::string& line) {
        if (parse_gcode_error_line(line).prefix != GcodeErrorPrefix::Bang) {
            return;
        }
        JournalEntry e;
        e.kind = journal_kind::kGcodeError;
        e.text = journal_error_text(line);
        e.cls = classify_journal_error(line);
        record(std::move(e));
    });
}

void EventJournalRecorder::on_klippy_shutdown() {
    if (!recording() || !client_) {
        return;
    }
    // The reason is Klipper's state_message, which the notification does not
    // carry. The job is the one open now: history may finish it before the
    // query answers.
    JournalEntry pending;
    pending.kind = journal_kind::kShutdown;
    pending.time = wall_now_s();
    pending.job_id = job_id_;
    pending.filename = filename_;
    const nlohmann::json params = {{"objects", {{"webhooks", {"state_message"}}}}};
    client_->send_jsonrpc("printer.objects.query", params,
                          lifetime_.bg_cb("EventJournal::shutdown_reason",
                                          [this, pending](const nlohmann::json& response) {
                                              on_shutdown_reason(response, pending);
                                          }),
                          lifetime_.bg_cb("EventJournal::shutdown_reason_error",
                                          [this, pending](const MoonrakerError&) {
                                              on_shutdown_reason(nlohmann::json::object(), pending);
                                          }),
                          0, /*silent=*/true);
}

void EventJournalRecorder::on_shutdown_reason(const nlohmann::json& response,
                                              JournalEntry pending) {
    using namespace json_util;
    const nlohmann::json* result = find_member(response, "result");
    const nlohmann::json* status = result ? find_member(*result, "status") : nullptr;
    const nlohmann::json* webhooks = status ? find_member(*status, "webhooks") : nullptr;
    const std::string message = webhooks ? safe_string(*webhooks, "state_message") : "";
    pending.text = journal_error_text(message);
    if (!pending.text.empty()) {
        pending.cls = classify_journal_error(pending.text);
        if (is_mcu_disconnect(pending.text)) {
            pending.kind = journal_kind::kMcuDisconnect;
        }
    }
    record(std::move(pending));
}

void EventJournalRecorder::on_history_changed(const nlohmann::json& msg) {
    using namespace json_util;
    const nlohmann::json* payload = notification_payload(msg);
    const nlohmann::json* job = payload ? find_member(*payload, "job") : nullptr;
    if (job == nullptr || !job->is_object()) {
        return;
    }
    const std::string action = safe_string(*payload, "action");
    std::string job_id;
    if (const nlohmann::json* id = find_member(*job, "job_id")) {
        job_id = as_string(*id, "", /*accept_number=*/true);
    }
    const std::string filename = safe_string(*job, "filename");
    if (action == "added") {
        job_id_ = job_id;
        filename_ = filename;
    } else if (action == "finished") {
        job_id_.clear();
        filename_.clear();
        JournalEntry e;
        e.kind = journal_kind::kJobEnd;
        e.detail = safe_string(*job, "status");
        e.job_id = job_id;
        e.filename = filename;
        record(std::move(e));
    }
}

void EventJournalRecorder::on_print_state(int state) {
    const int previous = last_print_state_;
    last_print_state_ = state;
    // The first value is where the printer already was, not a transition.
    // RAW_PRINT_STATE_OK: Klipper's own paused state, as the plugin records it.
    if (previous < 0 || state != static_cast<int>(PrintJobState::PAUSED) ||
        previous == static_cast<int>(PrintJobState::PAUSED)) {
        return;
    }
    JournalEntry e;
    e.kind = journal_kind::kPause;
    e.time = wall_now_s();
    e.detail = journal_pause_cause(journal_.snapshot(), e.time);
    record(std::move(e));
}

void EventJournalRecorder::on_connected() {
    if (!recording() || !client_ || backfill_in_flight_) {
        return;
    }
    backfill_in_flight_ = true;
    const double since = std::max(journal_.latest_time(), wall_now_s() - kJournalMaxAgeS);
    lv_subject_t* klippy = get_printer_state().network_state().get_klippy_state_subject();
    const bool shutdown_now =
        klippy != nullptr && lv_subject_get_int(klippy) == static_cast<int>(KlippyState::SHUTDOWN);

    client_->get_gcode_store(
        kGcodeStoreCount,
        lifetime_.bg_cb("EventJournal::gcode_store_backfill",
                        [this, since](const std::vector<GcodeStoreEntry>& store) {
                            merge(backfill_from_gcode_store(store, since));
                        }),
        [](const MoonrakerError& err) {
            spdlog::debug("[EventJournal] gcode_store backfill failed: {}", err.message);
        });

    const nlohmann::json params = {{"limit", kHistoryBackfillLimit}, {"since", since}};
    client_->send_jsonrpc(
        "server.history.list", params,
        lifetime_.bg_cb("EventJournal::history_backfill",
                        [this, since, shutdown_now](const nlohmann::json& response) {
                            on_history_backfill(response, since, shutdown_now);
                        }),
        lifetime_.bg_cb("EventJournal::history_backfill_error",
                        [this, since, shutdown_now](const MoonrakerError&) {
                            on_history_backfill(nlohmann::json::object(), since, shutdown_now);
                        }),
        0, /*silent=*/true);
}

void EventJournalRecorder::on_history_backfill(const nlohmann::json& response, double since,
                                               bool klippy_shutdown_now) {
    const nlohmann::json* result = json_util::find_member(response, "result");
    std::vector<JournalEntry> jobs =
        result ? backfill_from_history(*result, since) : std::vector<JournalEntry>{};
    const bool read_log = backfill_needs_klippy_log(klippy_shutdown_now, jobs);
    merge(std::move(jobs));
    if (!read_log) {
        backfill_in_flight_ = false;
        return;
    }
    auto deliver =
        lifetime_.bg_cb("EventJournal::klippy_log_backfill", [this, since](const std::string& log) {
            merge(backfill_from_klippy_log(log, since));
            backfill_in_flight_ = false;
        });
    http::HttpExecutor::fast().submit([deliver]() mutable {
        deliver(DebugBundleCollector::fetch_klipper_log_window(kKlippyLogWindowBytes));
    });
}

void EventJournalRecorder::merge(std::vector<JournalEntry> entries) {
    if (entries.empty() || !recording()) {
        return;
    }
    const std::size_t added = journal_.merge_backfill(std::move(entries), wall_now_s());
    if (added > 0) {
        spdlog::info("[EventJournal] Recovered {} event(s) from the reconnect gap", added);
        save();
    }
}

} // namespace helix
