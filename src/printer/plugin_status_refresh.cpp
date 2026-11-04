// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_status_refresh.h"

#include "app_globals.h"
#include "core/observer_factory.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "plr_plugin_rpc.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix {

using ui::observe;

namespace {
constexpr const char* kHandlerName = "PluginStatusRefresh";
constexpr const char* kFeaturesChanged = "notify_helix_features_changed";
} // namespace

PluginStatusRefresh::PluginStatusRefresh() {
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](PluginStatusRefresh* self, int seq) {
            if (seq > 0) {
                self->on_probed();
            }
        },
        ps.get_subjects_lifetime());
}

void PluginStatusRefresh::on_probed() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    history_events_.subscribe(*api, kHandlerName, lifetime_.token(),
                              [this](const HistoryJobEvent& event) {
                                  if (event.action == "added") {
                                      refresh();
                                  }
                              });
    IMoonrakerClient& client = api->get_client();
    if (notified_client_ == &client) {
        return;
    }
    notified_client_ = &client;
    client.register_method_callback(
        kFeaturesChanged, kHandlerName, [token = lifetime_.token(), this](const nlohmann::json&) {
            token.defer("PluginStatusRefresh::changed", [this] { refresh(); });
        });
}

void PluginStatusRefresh::refresh() {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr || !get_printer_state().plugin_status_state().service_has_helix_plugin()) {
        return;
    }
    spdlog::debug("[PluginStatusRefresh] Reading the plugin's status again");
    // A job start that changes nothing re-runs no probe observer.
    plr_rpc::refresh_features(*api, FeatureAnnounce::IfChanged);
}

} // namespace helix
