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

#include <algorithm>
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

    const auto action_it = helix::json_util::read_field<std::string>(event, "action");
    if (!action_it) {
        spdlog::debug("[TimelapseState] Event missing or invalid 'action' field");
        return;
    }

    const std::string& action = *action_it;

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

namespace timelapse {

namespace {
std::string bool_word(bool v) {
    return v ? "True" : "False";
}

/// The plugin's value of @p key in @p s, in the form a write sends it.
std::string setting_value(const TimelapseSettings& s, const std::string& key) {
    if (key == "mode") {
        return s.mode;
    }
    if (key == "parkhead") {
        return bool_word(s.parkhead);
    }
    if (key == "enabled") {
        return bool_word(s.enabled);
    }
    return {};
}
} // namespace

std::string encode_owed(const std::vector<OwedSetting>& owed) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& o : owed) {
        j[o.key] = nlohmann::json::array({o.owed, o.switched});
    }
    return owed.empty() ? std::string{} : j.dump();
}

std::vector<OwedSetting> decode_owed(const std::string& record) {
    std::vector<OwedSetting> owed;
    const nlohmann::json j = nlohmann::json::parse(record, nullptr, false);
    if (!j.is_object()) {
        return owed;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const auto& pair = it.value();
        if (pair.is_array() && pair.size() == 2 && pair[0].is_string() && pair[1].is_string()) {
            owed.push_back({it.key(), pair[0].get<std::string>(), pair[1].get<std::string>()});
        }
    }
    return owed;
}

std::vector<OwedSetting> merge_owed(std::vector<OwedSetting> existing,
                                    const std::vector<OwedSetting>& added) {
    for (const auto& a : added) {
        auto it = std::find_if(existing.begin(), existing.end(),
                               [&a](const OwedSetting& e) { return e.key == a.key; });
        if (it == existing.end()) {
            existing.push_back(a);
        } else {
            it->switched = a.switched;
        }
    }
    return existing;
}

std::vector<OwedSetting> hyperlapse_switch(const TimelapseSettings& current) {
    std::vector<OwedSetting> change;
    if (current.mode != "hyperlapse") {
        change.push_back({"mode", current.mode, "hyperlapse"});
    }
    if (current.parkhead) {
        change.push_back({"parkhead", bool_word(true), bool_word(false)});
    }
    return change;
}

std::vector<OwedSetting> timelapse_off_switch() {
    return {{"enabled", bool_word(true), bool_word(false)}};
}

std::vector<std::pair<std::string, std::string>>
settings_to_restore(const std::vector<OwedSetting>& owed, const TimelapseSettings& current) {
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto& o : owed) {
        if (setting_value(current, o.key) == o.switched) {
            values.emplace_back(o.key, o.owed);
        }
    }
    return values;
}

} // namespace timelapse

void TimelapseState::queue_plugin_op(std::function<void(std::function<void()>)> op) {
    plugin_ops_.push_back(std::move(op));
    run_next_plugin_op();
}

void TimelapseState::run_next_plugin_op() {
    if (plugin_op_in_flight_ || plugin_ops_.empty()) {
        return;
    }
    auto op = std::move(plugin_ops_.front());
    plugin_ops_.pop_front();
    plugin_op_in_flight_ = true;
    op([this]() {
        plugin_op_in_flight_ = false;
        run_next_plugin_op();
    });
}

void TimelapseState::post_settings(ITimelapseAPI& api,
                                   std::vector<std::pair<std::string, std::string>> values,
                                   std::function<void()> on_done) {
    queue_plugin_op([this, &api, values = std::move(values),
                     on_done = std::move(on_done)](std::function<void()> next) {
        auto tok = async_lifetime_.token();
        api.set_timelapse_values(
            values,
            [tok, on_done, next]() {
                tok.defer("TimelapseState::settings_written", [on_done, next]() {
                    if (on_done) {
                        on_done();
                    }
                    next();
                });
            },
            [tok, values, next](const MoonrakerError& err) {
                spdlog::error("[TimelapseState] Timelapse settings write failed: {}", err.message);
                tok.defer("TimelapseState::settings_write_failed", [values, next]() {
                    const bool switching =
                        std::any_of(values.begin(), values.end(), [](const auto& v) {
                            return v.first == "mode" && v.second == "hyperlapse";
                        });
                    if (switching) {
                        NOTIFY_WARNING(lv_tr("Could not switch timelapse to Hyperlapse"));
                    } else {
                        NOTIFY_WARNING(lv_tr("Timelapse is still in Hyperlapse mode. Switch it "
                                             "back in Settings > Printing > Timelapse."));
                    }
                    next();
                });
            });
    });
}

void TimelapseState::owe(const std::vector<timelapse::OwedSetting>& added) {
    const auto owed = timelapse::merge_owed(
        timelapse::decode_owed(pending_restore::read(timelapse::kModeRestoreKey)), added);
    pending_restore::write(timelapse::kModeRestoreKey, timelapse::encode_owed(owed));
}

void TimelapseState::record_print_as_hyperlapse(ITimelapseAPI& api, lv_subject_t* lifecycle,
                                                const SubjectLifetime& lifetime) {
    restore_api_ = &api;
    const uint64_t generation = ++restore_generation_;
    // Watched from now, so even a short print's end is seen.
    watch_print_end(lifecycle, lifetime, false);
    queue_plugin_op([this, &api, generation](std::function<void()> next) {
        auto tok = async_lifetime_.token();
        api.get_timelapse_settings(
            [this, tok, &api, generation, next](const TimelapseSettings& settings) {
                tok.defer("TimelapseState::hyperlapse_read",
                          [this, &api, generation, next, settings]() {
                              // Superseded, or the print is already over: switch nothing.
                              if (generation != restore_generation_ || !watching_print_end_) {
                                  next();
                                  return;
                              }
                              const auto change = timelapse::hyperlapse_switch(settings);
                              if (!change.empty()) {
                                  // Owed before it is sent: a restart from here on still puts it
                                  // back.
                                  owe(change);
                                  std::vector<std::pair<std::string, std::string>> values;
                                  for (const auto& c : change) {
                                      values.emplace_back(c.key, c.switched);
                                  }
                                  post_settings(api, std::move(values), [this]() {
                                      spdlog::info("[TimelapseState] Hyperlapse for this print");
                                      layer_mode_.store(false);
                                  });
                              }
                              next();
                          });
            },
            [tok, next](const MoonrakerError& err) {
                spdlog::error("[TimelapseState] Could not read timelapse settings: {}",
                              err.message);
                tok.defer("TimelapseState::hyperlapse_read_failed", [next]() {
                    NOTIFY_WARNING(lv_tr("Could not switch timelapse to Hyperlapse"));
                    next();
                });
            });
    });
}

void TimelapseState::begin_print_without_timelapse(ITimelapseAPI& api, lv_subject_t* lifecycle,
                                                   const SubjectLifetime& lifetime) {
    restore_api_ = &api;
    ++restore_generation_;
    owe(timelapse::timelapse_off_switch());
    watch_print_end(lifecycle, lifetime, false);
}

void TimelapseState::start_ended_without_print() {
    if (!watching_print_end_ || print_end_latch_.seen_running()) {
        return;
    }
    stop_watching_print_end();
    settle_mode_restore(restore_api_, ReportedPrintActivity::Idle);
}

void TimelapseState::watch_print_end(lv_subject_t* lifecycle, const SubjectLifetime& lifetime,
                                     bool already_running) {
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
                self->settle_mode_restore(self->restore_api_, ReportedPrintActivity::Idle);
            });
        },
        lifetime);
}

void TimelapseState::stop_watching_print_end() {
    watching_print_end_ = false;
    print_end_observer_.reset();
}

void TimelapseState::settle_mode_restore(ReportedPrintActivity activity) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        restore_api_ = &api->timelapse();
    }
    if (!watching_print_end_ && activity != ReportedPrintActivity::Idle &&
        !pending_restore::read(timelapse::kModeRestoreKey).empty()) {
        watch_print_end(get_printer_state().print_state().get_print_lifecycle_subject(),
                        get_printer_state().get_subjects_lifetime(),
                        activity == ReportedPrintActivity::Active);
    }
    settle_mode_restore(restore_api_, activity);
}

void TimelapseState::settle_mode_restore(ITimelapseAPI* api, ReportedPrintActivity activity) {
    const bool pending = !pending_restore::read(timelapse::kModeRestoreKey).empty();
    // A watch that has not seen its print run vouches for nothing: the
    // printer's own report decides.
    const bool watching = watching_print_end_ && print_end_latch_.seen_running();
    if (!api || timelapse::mode_restore_step(pending, watching, activity) !=
                    timelapse::ModeRestoreStep::FetchSettings) {
        return;
    }
    if (watching_print_end_) {
        stop_watching_print_end();
    }
    const uint64_t generation = restore_generation_;
    queue_plugin_op([this, api, generation](std::function<void()> next) {
        auto tok = async_lifetime_.token();
        api->get_timelapse_settings(
            [this, tok, api, generation, next](const TimelapseSettings& settings) {
                tok.defer("TimelapseState::restore_read", [this, api, generation, next,
                                                           settings]() {
                    if (generation != restore_generation_) {
                        next();
                        return;
                    }
                    const auto values = timelapse::settings_to_restore(
                        timelapse::decode_owed(pending_restore::read(timelapse::kModeRestoreKey)),
                        settings);
                    if (values.empty()) {
                        spdlog::info("[TimelapseState] Timelapse settings changed since the "
                                     "switch: nothing to put back");
                        pending_restore::write(timelapse::kModeRestoreKey, {});
                        set_mode(settings.mode);
                        next();
                        return;
                    }
                    spdlog::info("[TimelapseState] Print over: putting {} timelapse setting(s) "
                                 "back",
                                 values.size());
                    post_settings(*api, values, [this, generation, values]() {
                        for (const auto& [key, value] : values) {
                            if (key == "mode") {
                                set_mode(value);
                            }
                        }
                        // A switch since this read owns the record now.
                        if (generation == restore_generation_) {
                            pending_restore::write(timelapse::kModeRestoreKey, {});
                        }
                    });
                    next();
                });
            },
            [tok, next](const MoonrakerError& err) {
                spdlog::warn("[TimelapseState] Could not read timelapse settings to restore "
                             "them: {}",
                             err.message);
                tok.defer("TimelapseState::restore_read_failed", [next]() { next(); });
            });
    });
}

void TimelapseState::forget_mode_restore() {
    ++restore_generation_;
    restore_api_ = nullptr;
    pending_restore::write(timelapse::kModeRestoreKey, {});
    stop_watching_print_end();
}

} // namespace helix
