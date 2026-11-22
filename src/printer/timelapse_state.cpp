// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timelapse_state.h"

#include "app_globals.h"
#include "core/error_reporting.h"
#include "core/observer_factory.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "display_format.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "notice.h"
#include "printer_pending_restore.h"
#include "printer_state.h"
#include "replace_method_callback.h"
#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

#include <cstring>

#include "hv/json.hpp"

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

            if (current == 0) {
                auto* fn_subject = get_printer_state().print_state().get_print_filename_subject();
                const char* filename = fn_subject ? lv_subject_get_string(fn_subject) : "";
                auto date_str = helix::display_format::format_short_date(std::time(nullptr));
                std::string info;
                if (filename && filename[0] != '\0') {
                    info = std::string(filename) + " \xC2\xB7 " + date_str;
                } else {
                    info = date_str;
                }
                lv_subject_set_string(&timelapse_capture_info_, info.c_str());
            }
        });

        spdlog::debug("[TimelapseState] New frame captured");

    } else if (action == "render") {
        const std::string status = json_util::read_field<std::string>(event, "status").value_or("");
        const int progress = json_util::read_field<int>(event, "progress").value_or(0);
        const std::string error_msg = json_util::read_field<std::string>(event, "msg").value_or("");
        const std::string filename =
            json_util::read_field<std::string>(event, "filename").value_or("");

        if (status == "running") {
            if (last_notified_progress_ < 0) {
                publish_notice({NoticeSeverity::INFO, lv_tr("Rendering timelapse..."), 3000});
            }
            last_notified_progress_ = progress;

            async_lifetime_.defer("TimelapseState::render_progress", [this, progress]() {
                lv_subject_set_int(&timelapse_render_progress_, progress);
                lv_subject_set_string(&timelapse_render_status_, lv_tr("rendering"));
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
                    lv_subject_set_string(&timelapse_render_status_, lv_tr("complete"));
                    lv_subject_set_string(&timelapse_capture_info_, "");
                    if (cb) {
                        cb(filename);
                    }
                });

            last_notified_progress_ = -1;
            publish_notice(
                {NoticeSeverity::SUCCESS, lv_tr("Timelapse rendered successfully"), 5000});

            spdlog::info("[TimelapseState] Render complete: {}", filename);

        } else if (status == "error") {
            async_lifetime_.defer("TimelapseState::render_error", [this]() {
                lv_subject_set_string(&timelapse_render_status_, lv_tr("error"));
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
        lv_subject_set_string(&timelapse_render_status_, lv_tr("idle"));
        lv_subject_set_string(&timelapse_capture_info_, "");
    });

    last_notified_progress_ = -1;
    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        last_rendered_filename_.clear();
    }
    spdlog::debug("[TimelapseState] State reset");
}

void TimelapseState::record_print_as_hyperlapse() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[TimelapseState] Cannot switch this print to Hyperlapse: no printer");
        return;
    }
    // Owed before it is sent: a restart from here on still puts Layer mode back.
    pending_restore::write(timelapse::kModeRestoreKey, "layermacro");
    watch_print_end(false);
    api->timelapse().set_timelapse_mode(
        "hyperlapse",
        [this]() {
            spdlog::info("[TimelapseState] Hyperlapse for this print");
            layer_mode_.store(false);
        },
        [](const MoonrakerError& err) {
            spdlog::error("[TimelapseState] Hyperlapse switch failed: {}", err.message);
            helix::ui::queue_update("TimelapseState::hyperlapse_failed", []() {
                NOTIFY_WARNING(lv_tr("Could not switch timelapse to Hyperlapse"));
            });
        });
}

void TimelapseState::watch_print_end(bool already_running) {
    auto* lifecycle = get_printer_state().print_state().get_print_lifecycle_subject();
    if (!lifecycle) {
        return;
    }
    print_end_latch_ = timelapse::PrintEndLatch{};
    if (already_running) {
        (void)print_end_latch_.ended_by(PrintState::Printing);
    }
    watching_print_end_ = true;
    print_end_observer_ = helix::ui::observe_print_lifecycle<TimelapseState>(
        lifecycle, this,
        [](TimelapseState* self, PrintState state) {
            if (!self->print_end_latch_.ended_by(state)) {
                return;
            }
            self->watching_print_end_ = false;
            // Deferred: this runs inside the observer it releases.
            self->async_lifetime_.defer("TimelapseState::print_ended", [self]() {
                self->print_end_observer_.reset();
                self->settle_mode_restore(ReportedPrintActivity::Idle);
            });
        },
        get_printer_state().get_subjects_lifetime());
}

void TimelapseState::settle_mode_restore(ReportedPrintActivity activity) {
    if (!watching_print_end_ && activity != ReportedPrintActivity::Idle &&
        !pending_restore::read(timelapse::kModeRestoreKey).empty()) {
        watch_print_end(activity == ReportedPrintActivity::Active);
    }
    IMoonrakerAPI* api = get_moonraker_api();
    settle_mode_restore(api ? &api->timelapse() : nullptr, activity);
}

void TimelapseState::settle_mode_restore(ITimelapseAPI* api, ReportedPrintActivity activity) {
    const bool pending = !pending_restore::read(timelapse::kModeRestoreKey).empty();
    if (timelapse::mode_restore_step(pending, watching_print_end_, activity, nullptr) !=
            timelapse::ModeRestoreStep::FetchMode ||
        !api) {
        return;
    }
    auto tok = async_lifetime_.token();
    api->get_timelapse_settings(
        [this, tok, api](const TimelapseSettings& settings) {
            tok.defer("TimelapseState::mode_restore_read", [this, tok, api, settings]() {
                const std::string owed = pending_restore::read(timelapse::kModeRestoreKey);
                switch (timelapse::mode_restore_step(!owed.empty(), watching_print_end_,
                                                     ReportedPrintActivity::Idle, &settings.mode)) {
                case timelapse::ModeRestoreStep::Keep:
                case timelapse::ModeRestoreStep::FetchMode:
                    return;
                case timelapse::ModeRestoreStep::Clear:
                    spdlog::info("[TimelapseState] Timelapse mode is {}: no restore owed",
                                 settings.mode);
                    pending_restore::write(timelapse::kModeRestoreKey, {});
                    set_mode(settings.mode);
                    return;
                case timelapse::ModeRestoreStep::Send:
                    break;
                }
                spdlog::info("[TimelapseState] Print over: timelapse back to {}", owed);
                api->set_timelapse_mode(
                    owed,
                    [this, tok, owed]() {
                        tok.defer("TimelapseState::mode_restored", [this, owed]() {
                            pending_restore::write(timelapse::kModeRestoreKey, {});
                            set_mode(owed);
                        });
                    },
                    [](const MoonrakerError& err) {
                        spdlog::error("[TimelapseState] Mode restore failed: {}", err.message);
                        helix::ui::queue_update("TimelapseState::restore_failed", []() {
                            NOTIFY_WARNING(lv_tr("Timelapse is still in Hyperlapse mode. Switch it "
                                                 "back in Settings > Printing > Timelapse."));
                        });
                    });
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[TimelapseState] Could not read the timelapse mode to restore it: {}",
                         err.message);
        });
}

void TimelapseState::forget_mode_restore() {
    pending_restore::write(timelapse::kModeRestoreKey, {});
    watching_print_end_ = false;
    print_end_observer_.reset();
}

} // namespace helix
