// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_state.h"

#include "ui_error_reporting.h"
#include "ui_format_utils.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "core/update_queue.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "replace_method_callback.h"
#include "state/subject_macros.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix {

TimelapseState& TimelapseState::instance() {
    static TimelapseState instance;
    return instance;
}

namespace {
constexpr const char* kTimelapseEventMethod = "notify_timelapse_event";
constexpr const char* kTimelapseHandler = "timelapse_state";
} // namespace

void TimelapseState::attach(IMoonrakerClient& client) {
    replace_method_callback(client, kTimelapseEventMethod, kTimelapseHandler,
                            [](const nlohmann::json& data) {
                                TimelapseState::instance().handle_timelapse_event(data);
                            });
}

void TimelapseState::detach(IMoonrakerClient& client) {
    client.unregister_method_callback(kTimelapseEventMethod, kTimelapseHandler);
}

void TimelapseState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[TimelapseState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[TimelapseState] Initializing subjects (register_xml={})", register_xml);

    std::memset(timelapse_render_status_buf_, 0, sizeof(timelapse_render_status_buf_));
    std::strncpy(timelapse_render_status_buf_, lv_tr("idle"),
                 sizeof(timelapse_render_status_buf_) - 1);
    std::memset(timelapse_capture_info_buf_, 0, sizeof(timelapse_capture_info_buf_));
    {
        // A fresh subject set starts with no finished render, like the subjects it pairs with.
        std::lock_guard<std::mutex> lock(render_mutex_);
        last_rendered_filename_.clear();
    }

    INIT_SUBJECT_INT(timelapse_render_progress, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(timelapse_render_status, lv_tr("idle"), subjects_, register_xml);
    INIT_SUBJECT_INT(timelapse_frame_count, 0, subjects_, register_xml);
    INIT_SUBJECT_STRING(timelapse_capture_info, "", subjects_, register_xml);

    subjects_initialized_ = true;

    // RAW_PRINT_STATE_OK: the plugin captures frames only while the printer
    // runs a job, so the wire state is what decides where one print ends.
    PrinterState& printer = get_printer_state();
    print_in_progress_ = false;
    print_ended_ = false;
    print_state_obs_ = helix::ui::observe_print_state<TimelapseState>(
        printer.print_state().get_print_state_enum_subject(), this,
        [](TimelapseState* self, PrintJobState state) { self->on_print_state_changed(state); },
        printer.get_subjects_lifetime());

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "TimelapseState", []() { TimelapseState::instance().deinit_subjects(); });
}

void TimelapseState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[TimelapseState] Deinitializing subjects");

    // Expire any event/reset callbacks still queued on the UpdateQueue. They
    // capture `this` and write the subjects torn down below; without this the
    // next drain writes into a deinited subject and lv_subject_notify walks a
    // stale observer list (#1165, #1146).
    async_lifetime_.invalidate();
    print_state_obs_.reset();

    subjects_.deinit_all();
    subjects_initialized_ = false;
    last_notified_progress_ = -1;
}

void TimelapseState::on_print_state_changed(PrintJobState state) {
    // RAW_PRINT_STATE_OK: wire state by design, see init_subjects().
    switch (state) {
    case PrintJobState::PRINTING:
        // PAUSED -> PRINTING is the same print resuming, and a print running
        // before anything was seen to end is one this app joined; only a print
        // that follows an ended one starts the count over.
        if (!print_in_progress_ && print_ended_) {
            reset();
        }
        print_in_progress_ = true;
        print_ended_ = false;
        break;
    case PrintJobState::PAUSED:
        print_in_progress_ = true;
        break;
    // RAW_PRINT_STATE_OK: STANDBY after a job is the wire's report that it ended.
    case PrintJobState::STANDBY:
        print_ended_ = print_ended_ || print_in_progress_;
        print_in_progress_ = false;
        break;
    default:
        print_ended_ = true;
        print_in_progress_ = false;
        break;
    }
}

void TimelapseState::handle_timelapse_event(const nlohmann::json& event) {
    if (!subjects_initialized_) {
        spdlog::warn("[TimelapseState] Event received before subjects initialized");
        return;
    }

    // Safely extract action
    auto action_it = event.find("action");
    if (action_it == event.end() || !action_it->is_string()) {
        spdlog::debug("[TimelapseState] Event missing or invalid 'action' field");
        return;
    }

    const std::string& action = action_it->get_ref<const std::string&>();

    if (action == "newframe") {
        // Increment frame count — read+write both inside ui_queue_update
        // since lv_subject_get_int must be called from the UI thread
        async_lifetime_.defer("TimelapseState::newframe", [this]() {
            int current = lv_subject_get_int(&timelapse_frame_count_);
            lv_subject_set_int(&timelapse_frame_count_, current + 1);

            // On first frame, set capture info with print filename and timestamp
            if (current == 0) {
                auto* fn_subject = get_printer_state().print_state().get_print_filename_subject();
                const char* filename = fn_subject ? lv_subject_get_string(fn_subject) : "";
                auto date_str = helix::ui::format_short_date(std::time(nullptr));
                std::string info;
                if (filename && filename[0] != '\0') {
                    info = std::string(filename) + " \xC2\xB7 " + date_str;
                } else {
                    info = date_str;
                }
                lv_subject_copy_string(&timelapse_capture_info_, info.c_str());
            }
        });

        spdlog::debug("[TimelapseState] New frame captured");

    } else if (action == "render") {
        // Extract render status and progress
        std::string status;
        int progress = 0;
        std::string error_msg;
        std::string filename;

        if (event.contains("status") && event["status"].is_string()) {
            status = event["status"].get<std::string>();
        }
        if (event.contains("progress") && event["progress"].is_number()) {
            progress = event["progress"].get<int>();
        }
        if (event.contains("msg") && event["msg"].is_string()) {
            error_msg = event["msg"].get<std::string>();
        }
        if (event.contains("filename") && event["filename"].is_string()) {
            filename = event["filename"].get<std::string>();
        }

        if (status == "running") {
            // Show start toast on first progress event
            if (last_notified_progress_ < 0) {
                ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Rendering timelapse..."),
                                              3000);
            }
            last_notified_progress_ = progress;

            async_lifetime_.defer("TimelapseState::render_progress", [this, progress]() {
                lv_subject_set_int(&timelapse_render_progress_, progress);
                lv_subject_copy_string(&timelapse_render_status_, lv_tr("rendering"));
            });

            spdlog::debug("[TimelapseState] Render progress: {}%", progress);

        } else if (status == "success") {
            RenderCompleteCallback cb;
            {
                std::lock_guard<std::mutex> lock(render_mutex_);
                last_rendered_filename_ = filename;
                cb = on_render_complete_;
            }

            async_lifetime_.defer(
                "TimelapseState::render_complete", [this, filename, cb = std::move(cb)]() {
                    lv_subject_set_int(&timelapse_render_progress_, 0);
                    lv_subject_set_int(&timelapse_frame_count_, 0);
                    lv_subject_copy_string(&timelapse_render_status_, lv_tr("complete"));
                    lv_subject_copy_string(&timelapse_capture_info_, "");
                    if (cb) {
                        cb(filename);
                    }
                });

            last_notified_progress_ = -1;
            ToastManager::instance().show(ToastSeverity::SUCCESS,
                                          lv_tr("Timelapse rendered successfully"), 5000);

            spdlog::info("[TimelapseState] Render complete: {}", filename);

        } else if (status == "error") {
            async_lifetime_.defer("TimelapseState::render_error", [this]() {
                lv_subject_copy_string(&timelapse_render_status_, lv_tr("error"));
            });

            last_notified_progress_ = -1;
            if (!error_msg.empty()) {
                NOTIFY_ERROR(lv_tr("Timelapse render failed: {}"), error_msg);
            } else {
                NOTIFY_ERROR(lv_tr("Timelapse render failed"));
            }

            spdlog::error("[TimelapseState] Render error: {}", error_msg);
        }

    } else {
        spdlog::debug("[TimelapseState] Unknown action: {}", action);
    }
}

void TimelapseState::reset() {
    if (!subjects_initialized_) {
        return;
    }

    async_lifetime_.defer("TimelapseState::reset", [this]() {
        lv_subject_set_int(&timelapse_frame_count_, 0);
        lv_subject_set_int(&timelapse_render_progress_, 0);
        lv_subject_copy_string(&timelapse_render_status_, lv_tr("idle"));
        lv_subject_copy_string(&timelapse_capture_info_, "");
    });

    last_notified_progress_ = -1;
    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        last_rendered_filename_.clear();
    }
    spdlog::debug("[TimelapseState] State reset");
}

} // namespace helix
