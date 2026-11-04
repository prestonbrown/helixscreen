// SPDX-License-Identifier: GPL-3.0-or-later

#include "standing_print_options_controller.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/observer_factory.h"
#include "fallback_sender.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "json_utils.h"
#include "moonraker_db_item.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "standing_print_options.h"

#include <spdlog/spdlog.h>

#include <ctime>
#include <functional>
#include <optional>
#include <vector>

namespace helix {

using ui::observe;

namespace {
constexpr const char* kHandlerName = "StandingPrintOptions";
} // namespace

StandingPrintOptionsController::StandingPrintOptionsController(IMoonrakerAPI* api) : api_(api) {
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](StandingPrintOptionsController* self, int seq) {
            if (seq > 0) {
                self->on_plugin_probed();
            }
        },
        ps.get_subjects_lifetime());

    // The inputs the backends render from: which backends exist, what they
    // last reported, and the user's unload-at-end choice.
    auto& ams = AmsState::instance();
    backend_count_observer_ = observe<int>(
        ams.get_backend_count_subject(), this,
        [](StandingPrintOptionsController* self, int) { self->refresh_record(); },
        ams.get_subjects_lifetime());
    data_revision_observer_ = observe<int>(
        ams.get_ams_data_revision_subject(), this,
        [](StandingPrintOptionsController* self, int) { self->refresh_record(); },
        ams.get_subjects_lifetime());
    auto& settings = SettingsManager::instance();
    unload_tools_observer_ = observe<int>(
        settings.subject_end_of_print_unload_tools(), this,
        [](StandingPrintOptionsController* self, int) { self->refresh_record(); },
        settings.get_subjects_lifetime());
}

std::string StandingPrintOptionsController::render_now() {
    auto& ams = AmsState::instance();
    std::vector<const AmsBackend*> backends;
    for (int i = 0; i < ams.backend_count(); ++i) {
        backends.push_back(ams.get_backend(i));
    }
    return render_standing_gcode(backends);
}

void StandingPrintOptionsController::on_plugin_probed() {
    auto& status = get_printer_state().plugin_status_state();
    screen_start_ledger().set_announce(
        announce_screen_start(status.plugin_feature(kPrintHooksFeature, kPrintHooksMinVersion)));
    if (api_ == nullptr) {
        return;
    }
    subscribe_history(*api_);
    load_opt_ins(*api_);
    written_.reset();
    considered_.reset();
    refresh_record();
    // A screen on the printer's host announces itself, so a remote one leaves
    // the fallback to it.
    if (status.screen_local() && !presence_timer_) {
        presence_timer_.reset(lv_timer_create(
            [](lv_timer_t* t) {
                static_cast<StandingPrintOptionsController*>(lv_timer_get_user_data(t))
                    ->announce_presence();
            },
            static_cast<uint32_t>(kLocalScreenPresenceIntervalS * 1000), this));
    }
    announce_presence();
}

void StandingPrintOptionsController::refresh_record() {
    // Only a plugin reads the record; without one there is nothing to keep.
    if (!get_printer_state().plugin_status_state().service_has_helix_plugin()) {
        return;
    }
    if (api_ == nullptr || !api_->is_connected()) {
        return;
    }
    // A rendering already weighed (written, or left to another screen) is not
    // weighed again: the inputs tick on every backend report.
    if (considered_ && *considered_ == render_now()) {
        return;
    }
    // The record is per printer but rendered from this screen's settings, so
    // only the screen that would send the fallback writes it.
    resolve_sender_role([this](FallbackSenderRole role) { write_record(role); });
}

void StandingPrintOptionsController::write_record(FallbackSenderRole role) {
    if (api_ == nullptr || !api_->is_connected()) {
        return;
    }
    std::string gcode = render_now();
    considered_ = gcode;
    if (!writes_standing_record(role, gcode)) {
        spdlog::debug("[StandingPrintOptions] Not writing the record ({})",
                      role == FallbackSenderRole::Standby ? "another screen owns it"
                                                          : "nothing rendered yet");
        return;
    }
    if (written_ && *written_ == gcode) {
        return;
    }
    written_ = gcode;
    spdlog::debug("[StandingPrintOptions] Record now: '{}'", gcode);
    post_item_if_changed(
        *api_, kPluginFeaturesNamespace, kStandingOptionsKey, standing_options_record(gcode),
        [] { spdlog::info("[StandingPrintOptions] Record written"); },
        [this, token = lifetime_.token()](const MoonrakerError& err) {
            spdlog::warn("[StandingPrintOptions] Writing the record failed: {}", err.message);
            // The next refresh writes it again.
            token.defer("StandingPrintOptions::write_failed", [this] {
                written_.reset();
                considered_.reset();
            });
        });
}

void StandingPrintOptionsController::load_opt_ins(IMoonrakerAPI& api) {
    api.database_get_item(
        kPluginFeaturesNamespace, kPluginFeaturesKey,
        [token = lifetime_.token()](const nlohmann::json& record) {
            token.defer("StandingPrintOptions::opt_ins", [record] {
                get_printer_state().plugin_status_state().set_stored_opt_ins(record);
            });
        },
        [token = lifetime_.token()](const MoonrakerError& err) {
            std::optional<nlohmann::json> record = opt_ins_after_failed_read(err);
            if (!record) {
                spdlog::debug("[StandingPrintOptions] Keeping the last opt-ins: {}", err.message);
                return;
            }
            token.defer("StandingPrintOptions::opt_ins_absent", [record = std::move(*record)] {
                get_printer_state().plugin_status_state().set_stored_opt_ins(record);
            });
        });
}

void StandingPrintOptionsController::subscribe_history(IMoonrakerAPI& api) {
    history_events_.subscribe(api, kHandlerName, lifetime_.token(),
                              [this](const HistoryJobEvent& event) {
                                  if (event.action == "added") {
                                      on_job_added(event.filename);
                                  }
                              });
}

void StandingPrintOptionsController::on_job_added(const std::string& filename) {
    // Claimed now: the ledger entry is this job's whatever the decision.
    const bool screen_started = screen_start_ledger().claim(filename);
    auto& status = get_printer_state().plugin_status_state();
    const PluginFeatureState hooks =
        status.plugin_feature(kPrintHooksFeature, kPrintHooksMinVersion);
    const bool opted_in =
        plugin_feature_opted_in(status.stored_opt_ins(), kPrintHooksFeature, /*acts=*/true);
    std::string gcode = render_now();
    auto decide = [this, hooks, opted_in, screen_started, filename,
                   gcode](FallbackSenderRole role) {
        const StandingOptionsSender sender =
            standing_options_sender(hooks, opted_in, screen_started, !gcode.empty(), role);
        if (sender == StandingOptionsSender::App) {
            send_options(filename, gcode);
            return;
        }
        spdlog::debug("[StandingPrintOptions] Job '{}' started: {}", filename,
                      sender == StandingOptionsSender::Plugin ? "the plugin sends the options"
                      : role == FallbackSenderRole::Standby   ? "a local screen sends them"
                                                              : "no options to send");
    };
    // Only a send this instance would make needs to know who else is here.
    if (standing_options_sender(hooks, opted_in, screen_started, !gcode.empty(),
                                FallbackSenderRole::Sender) != StandingOptionsSender::App) {
        decide(FallbackSenderRole::Sender);
        return;
    }
    resolve_sender_role(decide);
}

void StandingPrintOptionsController::resolve_sender_role(
    std::function<void(FallbackSenderRole)> then) {
    resolve_fallback_sender_role(lifetime_.token(), std::move(then));
}

void StandingPrintOptionsController::send_options(const std::string& filename,
                                                  const std::string& gcode) {
    if (api_ == nullptr) {
        return;
    }
    spdlog::info("[StandingPrintOptions] Job '{}' started elsewhere, sending:\n{}", filename,
                 gcode);
    api_->execute_gcode(
        gcode, [] {},
        [](const MoonrakerError& err) {
            spdlog::warn("[StandingPrintOptions] Sending the options failed: {}", err.message);
        },
        0, /*silent=*/true, nullptr, /*caller_surfaces_errors=*/false);
}

void StandingPrintOptionsController::announce_presence() {
    if (api_ == nullptr || !api_->is_connected() ||
        !get_printer_state().plugin_status_state().screen_local()) {
        return;
    }
    api_->database_post_item(kPluginFeaturesNamespace, kLocalScreenPresenceKey,
                             local_screen_presence_record(static_cast<int64_t>(std::time(nullptr))),
                             nullptr, [](const MoonrakerError& err) {
                                 spdlog::debug("[StandingPrintOptions] Presence not written: {}",
                                               err.message);
                             });
}

} // namespace helix
