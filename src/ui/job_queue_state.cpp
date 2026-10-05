// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "job_queue_state.h"

#include "ui_filename_utils.h"

#include "connection_staleness.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "job_queue_start.h"
#include "queued_job_options.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

JobQueueState::JobQueueState(IMoonrakerAPI* api, helix::IMoonrakerClient* client)
    : api_(api), client_(client) {
    std::memset(state_buffer_, 0, sizeof(state_buffer_));
    std::memset(summary_buffer_, 0, sizeof(summary_buffer_));
    std::memset(up_next_text_buffer_, 0, sizeof(up_next_text_buffer_));
    std::memset(start_next_text_buffer_, 0, sizeof(start_next_text_buffer_));

    subscribe_to_notifications();
    watch_connection_state();
    spdlog::debug("[JobQueueState] Created");
}

void JobQueueState::invalidate() {
    is_loaded_ = false;
    // A dropped socket may hide a Moonraker restart, and a restart is the
    // only thing that changes server config: re-read it on the next connect.
    automatic_transition_loaded_ = false;
    // The prune re-arms with it: an extra prune only repeats the
    // store-then-queue read, so collecting a reconnect's orphans costs one
    // round trip.
    pruned_this_connect_ = false;
}

void JobQueueState::watch_connection_state() {
    if (!api_) {
        return;
    }
    // api_->printer_state() rather than the global accessor: it is the state
    // this owner's API already reads and writes, which keeps the wiring honest
    // under test.
    connection_observer_ =
        helix::observe_connection_staleness(api_->printer_state(), this, "JobQueueState");
}

JobQueueState::~JobQueueState() {
    connection_observer_.reset();

    // lifetime_'s destructor invalidates its own tokens automatically. The
    // subjects_' manager member dies with the object and flips its death
    // signal there.

    if (client_) {
        client_->unregister_method_callback("notify_job_queue_changed", "JobQueueState");
    }

    spdlog::debug("[JobQueueState] Destroyed");
}

void JobQueueState::init_subjects() {
    if (subjects_initialized_)
        return;

    lv_subject_init_string(&job_queue_state_subject_, state_buffer_, nullptr, sizeof(state_buffer_),
                           "Ready");
    subjects_.publish("job_queue_state_text", &job_queue_state_subject_);

    lv_subject_init_string(&job_queue_summary_subject_, summary_buffer_, nullptr,
                           sizeof(summary_buffer_), "Queue empty");
    subjects_.publish("job_queue_summary_text", &job_queue_summary_subject_);

    lv_subject_init_int(&job_queue_count_subject_, 0);
    subjects_.publish("job_queue_count", &job_queue_count_subject_);

    lv_subject_init_string(&job_queue_up_next_text_subject_, up_next_text_buffer_, nullptr,
                           sizeof(up_next_text_buffer_), "");
    subjects_.publish("job_queue_up_next_text", &job_queue_up_next_text_subject_);

    lv_subject_init_string(&job_queue_start_next_text_subject_, start_next_text_buffer_, nullptr,
                           sizeof(start_next_text_buffer_), "");
    subjects_.publish("job_queue_start_next_text", &job_queue_start_next_text_subject_);

    lv_subject_init_int(&job_queue_automatic_transition_subject_, automatic_transition_ ? 1 : 0);
    subjects_.publish("job_queue_automatic_transition", &job_queue_automatic_transition_subject_);

    SubjectDebugRegistry::instance().register_subject(&job_queue_state_subject_,
                                                      "job_queue_state_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&job_queue_summary_subject_,
                                                      "job_queue_summary_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&job_queue_count_subject_, "job_queue_count",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&job_queue_up_next_text_subject_,
                                                      "job_queue_up_next_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&job_queue_start_next_text_subject_,
                                                      "job_queue_start_next_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    subjects_initialized_ = true;

    // The "Up next" rows and the completion modal's "Start next" secondary
    // resolve these callback names; registration must precede any XML that
    // references them, and this init runs before panel creation. Idempotent,
    // so the many per-test JobQueueState instances register once per process.
    helix::register_job_queue_start_callbacks();

    // Co-locate cleanup registration with init (CLAUDE.md mandate)
    StaticSubjectRegistry::instance().register_deinit("JobQueueState",
                                                      [this]() { deinit_subjects(); });

    spdlog::debug("[JobQueueState] Subjects initialized");
}

void JobQueueState::deinit_subjects() {
    if (!subjects_initialized_)
        return;

    // deinit_all() flips the death signal first, then withdraws each XML
    // name and frees the subject.
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[JobQueueState] Subjects deinitialized");
}

void JobQueueState::fetch() {
    fetch_automatic_transition();

    if (!api_)
        return;
    bool expected = false;
    if (!is_fetching_.compare_exchange_strong(expected, true))
        return;

    auto token = lifetime_.token();
    api_->queue().get_queue_status(
        [this, token](const JobQueueStatus& status) {
            is_fetching_.store(false);
            token.defer("JobQueueState::on_queue_fetched",
                        [this, status]() { on_queue_fetched(status); });
        },
        [this, token](const MoonrakerError& err) {
            is_fetching_.store(false);
            // spdlog is thread-safe; expired() guard would have suppressed the
            // log on a destroyed manager but the log is informational and
            // harmless either way. Dropping the bare expired() check silences
            // the bg_tok_expired_check detector for this site (3XNZQB2R audit).
            spdlog::warn("[JobQueueState] Fetch failed: {}", err.message);
        });
}

void JobQueueState::on_queue_fetched(const JobQueueStatus& status) {
    // Main-thread only: fetch()'s success callback wraps this call in
    // tok.defer(), so everything below may touch subjects and LVGL state.
    cached_jobs_ = status.queued_jobs;
    queue_state_ = status.queue_state;
    is_loaded_ = true;
    // Prune once per connect, on the FIRST fetch. Which fetch triggers it is
    // irrelevant to safety: the prune reads the option store first and the
    // queue second, so an entry can only be deleted against a queue read
    // taken after the store read. The latch just avoids repeating the
    // two-read round trip on every fetch.
    if (!pruned_this_connect_) {
        pruned_this_connect_ = true;
        prune_option_store();
    }
    update_subjects();
    spdlog::debug("[JobQueueState] Updated: state={}, jobs={}", queue_state_, cached_jobs_.size());
}

void JobQueueState::fetch_automatic_transition() {
    if (!client_ || automatic_transition_loaded_)
        return;
    // Latch before issuing: a failed read must not re-fire on every widget
    // activation, and unreadable already means false.
    automatic_transition_loaded_ = true;

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "server.config", json::object(),
        [this, token](const json& response) {
            // Parsed here so the deferred body carries a bool, not the config DOM.
            token.defer("JobQueueState::on_server_config",
                        [this, automatic = helix::parse_automatic_transition(response)]() {
                            automatic_transition_ = automatic;
                            if (subjects_initialized_) {
                                lv_subject_set_int(&job_queue_automatic_transition_subject_,
                                                   automatic_transition_ ? 1 : 0);
                            }
                            spdlog::debug("[JobQueueState] job_queue automatic_transition={}",
                                          automatic_transition_);
                        });
        },
        [token](const MoonrakerError& err) {
            spdlog::debug("[JobQueueState] server.config read failed: {}", err.message);
        },
        0, true);
}

void JobQueueState::prune_option_store() {
    helix::queue::prune_stored_queued_job_options(lifetime_, api_);
}

void JobQueueState::update_subjects() {
    if (!subjects_initialized_)
        return;

    int count = static_cast<int>(cached_jobs_.size());

    // State text: capitalize first letter for display
    std::string state_display = queue_state_;
    if (!state_display.empty()) {
        state_display[0] =
            static_cast<char>(std::toupper(static_cast<unsigned char>(state_display[0])));
    }
    std::snprintf(state_buffer_, sizeof(state_buffer_), "%s", state_display.c_str());
    lv_subject_copy_string(&job_queue_state_subject_, state_buffer_);

    // Summary text
    if (count == 0) {
        std::snprintf(summary_buffer_, sizeof(summary_buffer_), "Queue empty");
    } else if (count == 1) {
        std::snprintf(summary_buffer_, sizeof(summary_buffer_), "1 job queued");
    } else {
        std::snprintf(summary_buffer_, sizeof(summary_buffer_), "%d jobs queued", count);
    }
    lv_subject_copy_string(&job_queue_summary_subject_, summary_buffer_);

    // Next-job display name, empty when the queue is empty, feeds both
    // composed strings. Same settled-before-count rule as the two subjects
    // above: count observers re-read these.
    const std::string next_display =
        cached_jobs_.empty() ? std::string{}
                             : helix::gcode::get_display_filename(cached_jobs_.front().filename);

    // Both surfaces bind their visibility to the count published last.
    std::snprintf(up_next_text_buffer_, sizeof(up_next_text_buffer_), "%s",
                  helix::format_up_next_text(next_display, count).c_str());
    lv_subject_copy_string(&job_queue_up_next_text_subject_, up_next_text_buffer_);

    std::snprintf(start_next_text_buffer_, sizeof(start_next_text_buffer_), "%s",
                  helix::format_start_next_text(next_display, count).c_str());
    lv_subject_copy_string(&job_queue_start_next_text_subject_, start_next_text_buffer_);

    lv_subject_set_int(&job_queue_automatic_transition_subject_, automatic_transition_ ? 1 : 0);

    // Count goes LAST, after cached_jobs_ and both text subjects are settled.
    // It is the rebuild trigger the queue surfaces observe, and PrintStatusWidget's
    // observer runs synchronously (observe<int>) — publishing it first would
    // let that handler re-read a half-updated state. Main-thread only: the sole
    // caller is on_queue_fetched(), which fetch() reaches through tok.defer().
    lv_subject_set_int(&job_queue_count_subject_, count);
}

void JobQueueState::subscribe_to_notifications() {
    if (!client_)
        return;

    auto token = lifetime_.token();
    client_->register_method_callback(
        "notify_job_queue_changed", "JobQueueState", [this, token](const nlohmann::json& /*data*/) {
            token.defer("JobQueueState::notify_changed", [this]() { fetch(); });
        });

    spdlog::debug("[JobQueueState] Subscribed to notify_job_queue_changed");
}

namespace helix {

std::string format_up_next_text(const std::string& display_name, int queued_count) {
    if (queued_count <= 0 || display_name.empty()) {
        return {};
    }
    // Whole-format translation keys: a translator must be able to move or
    // drop the colon and the count suffix, not have it welded on after the
    // fact around a translated fragment.
    if (queued_count > 1) {
        return fmt::format(lv_tr("Up next: {} (+{})"), display_name, queued_count - 1);
    }
    return fmt::format(lv_tr("Up next: {}"), display_name);
}

std::string format_start_next_text(const std::string& display_name, int queued_count) {
    if (queued_count <= 0 || display_name.empty()) {
        return {};
    }
    return fmt::format(lv_tr("Start next: {}"), display_name);
}

bool parse_automatic_transition(const json& rpc_response) {
    // Defensive walk, no throwing value() lookups: server.config is foreign
    // input and a malformed answer reads as Moonraker's own default (false).
    if (!rpc_response.is_object() || !rpc_response.contains("result")) {
        return false;
    }
    const json& result = rpc_response["result"];
    if (!result.is_object() || !result.contains("config")) {
        return false;
    }
    const json& config = result["config"];
    if (!config.is_object() || !config.contains("job_queue")) {
        return false;
    }
    const json& job_queue = config["job_queue"];
    if (!job_queue.is_object() || !job_queue.contains("automatic_transition")) {
        return false;
    }
    const json& value = job_queue["automatic_transition"];
    return value.is_boolean() ? value.get<bool>() : false;
}

} // namespace helix
