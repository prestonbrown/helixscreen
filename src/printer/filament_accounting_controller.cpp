// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_accounting_controller.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_globals.h"
#include "connection_state.h"
#include "core/observer_factory.h"
#include "fallback_sender.h"
#include "filament_database.h"
#include "i_moonraker_api.h"
#include "macro_param_cache.h"
#include "moonraker_db_item.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix {

using ui::observe;

namespace {

/// Grams one millimetre of @p material weighs at the printer's filament
/// diameter; 0 when the material is not known.
float grams_per_mm(const std::string& material) {
    if (material.empty()) {
        return 0.0f;
    }
    const auto info = filament::find_material(material);
    if (!info || info->density_g_cm3 <= 0.0f) {
        return 0.0f;
    }
    return filament::length_to_weight_g(1.0f, info->density_g_cm3,
                                        get_printer_state().get_discovery().filament_diameter_mm());
}

FeedEntry feed_for_slot(const AmsBackend& backend, int slot) {
    const SlotInfo info = backend.get_slot_info(slot);
    return {info.mapped_tool, slot, std::max(info.spoolman_id, 0), grams_per_mm(info.material)};
}

bool printer_switches() {
    return printer_switches_active_spool(get_printer_state().get_discovery());
}

std::vector<int> mapped_spools(const std::vector<FeedSignal>& signals) {
    std::vector<int> spools;
    for (const FeedSignal& sig : signals) {
        for (const auto& value : sig.values) {
            const int id = value.second.spool_id;
            if (id > 0 && std::find(spools.begin(), spools.end(), id) == spools.end()) {
                spools.push_back(id);
            }
        }
    }
    return spools;
}

} // namespace

ActiveSpoolOwner active_spool_owner_now(bool spoolman, bool backend_manages_active_spool) {
    return active_spool_owner(spoolman, backend_manages_active_spool, printer_switches(),
                              FilamentAccountingController::macros_set_active_spool());
}

bool helix_sets_active_spool(const AmsBackend* backend) {
    return active_spool_owner_now(/*spoolman=*/true,
                                  backend != nullptr && backend->manages_active_spool()) ==
           ActiveSpoolOwner::Helix;
}

int spool_to_activate(const AmsBackend* backend, int slot) {
    if (backend == nullptr || slot < 0 || !helix_sets_active_spool(backend)) {
        return 0;
    }
    return std::max(backend->get_slot_info(slot).spoolman_id, 0);
}

bool helix_sets_active_spool_on_edit(const AmsBackend* backend, bool external_spool) {
    const bool manages = backend != nullptr && backend->manages_active_spool();
    return app_sets_spool_on_edit(active_spool_owner_now(/*spoolman=*/true, manages),
                                  !external_spool && backend != nullptr &&
                                      backend->sets_active_spool_on_link());
}

FilamentAccountingController::FilamentAccountingController() {
    auto& ps = get_printer_state();
    probe_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_probe_seq_subject(), this,
        [](FilamentAccountingController* self, int seq) {
            if (seq > 0) {
                self->on_plugin_probed();
            }
        },
        ps.get_subjects_lifetime());
    // The probe can answer before the plugin reads as installed on a first
    // connect, and the map is written only once it does.
    installed_observer_ = observe<int>(
        ps.plugin_status_state().get_helix_plugin_installed_subject(), this,
        [](FilamentAccountingController* self, int) { self->refresh_record(); },
        ps.get_subjects_lifetime());
    connection_observer_ = observe<int>(
        ps.network_state().get_printer_connection_state_subject(), this,
        [](FilamentAccountingController* self, int state) { self->on_connection_changed(state); },
        ps.get_subjects_lifetime());

    // The map's inputs: which backends exist and what they last reported, and
    // the tools with the spools assigned to them.
    auto& ams = AmsState::instance();
    backend_count_observer_ = observe<int>(
        ams.get_backend_count_subject(), this,
        [](FilamentAccountingController* self, int) { self->refresh_record(); },
        ams.get_subjects_lifetime());
    data_revision_observer_ = observe<int>(
        ams.get_ams_data_revision_subject(), this,
        [](FilamentAccountingController* self, int) { self->refresh_record(); },
        ams.get_subjects_lifetime());
    auto& tools = ToolState::instance();
    tools_version_observer_ = observe<int>(
        tools.get_tools_version_subject(), this,
        [](FilamentAccountingController* self, int) { self->refresh_record(); },
        tools.get_subjects_lifetime());
    active_tool_observer_ = observe<int>(
        tools.get_active_tool_subject(), this,
        [](FilamentAccountingController* self, int tool) { self->on_active_tool_changed(tool); },
        tools.get_subjects_lifetime());
}

FeedEntry FilamentAccountingController::feed_for_tool(const ToolInfo& tool, int tool_index) {
    if (tool.backend_index >= 0 && tool.backend_slot >= 0) {
        if (const AmsBackend* backend = AmsState::instance().get_backend(tool.backend_index)) {
            FeedEntry feed = feed_for_slot(*backend, tool.backend_slot);
            feed.tool = tool_index;
            return feed;
        }
    }
    FeedEntry feed;
    feed.tool = tool_index;
    feed.spool_id = std::max(tool.spoolman_id, 0);
    if (const auto identity = SpoolmanManager::find_identity(tool.spoolman_id)) {
        feed.grams_per_mm = grams_per_mm(identity->material);
    }
    return feed;
}

std::vector<FeedSignal> FilamentAccountingController::signals_now() {
    std::vector<FeedSignal> signals;
    auto& ams = AmsState::instance();
    for (int i = 0; i < ams.backend_count(); ++i) {
        const AmsBackend* backend = ams.get_backend(i);
        if (backend == nullptr) {
            continue;
        }
        for (const AmsBackend::FeedSlotSignal& source : backend->feed_slot_signals()) {
            FeedSignal sig{source.object, source.path, {}};
            for (const auto& [value, slot] : source.value_slots) {
                sig.values.emplace_back(value, feed_for_slot(*backend, slot));
            }
            if (!sig.values.empty()) {
                signals.push_back(std::move(sig));
            }
        }
    }
    // One tool has nothing to switch between.
    const auto& tools = ToolState::instance().tools();
    if (tools.size() > 1) {
        std::vector<std::string> extruders;
        std::vector<FeedEntry> feeds;
        for (size_t i = 0; i < tools.size(); ++i) {
            extruders.push_back(tools[i].extruder_name.value_or(""));
            feeds.push_back(feed_for_tool(tools[i], static_cast<int>(i)));
        }
        if (auto sig = extruder_feed_signal(extruders, feeds)) {
            signals.push_back(std::move(*sig));
        }
    }
    return signals;
}

bool FilamentAccountingController::backends_manage_active_spool() {
    auto& ams = AmsState::instance();
    for (int i = 0; i < ams.backend_count(); ++i) {
        const AmsBackend* backend = ams.get_backend(i);
        if (backend != nullptr && backend->manages_active_spool()) {
            return true;
        }
    }
    return false;
}

bool FilamentAccountingController::macros_set_active_spool() {
    const auto& cache = MacroParamCache::instance();
    return tool_macros_set_active_spool(
        get_printer_state().get_discovery().macros(),
        [&cache](const std::string& name) { return cache.get(name).sets_active_spool; });
}

void FilamentAccountingController::on_plugin_probed() {
    // Discovery has read the printer: the active tool now is this
    // connection's, and only a change from it is a tool change.
    armed_ = true;
    baseline_tool_ = ToolState::instance().active_tool_index();
    written_.reset();
    considered_.reset();
    role_pending_ = false;
    refresh_record();
}

void FilamentAccountingController::on_connection_changed(int state) {
    if (state != static_cast<int>(ConnectionState::CONNECTED)) {
        armed_ = false;
    }
}

void FilamentAccountingController::refresh_record() {
    // Only a plugin reads the map; without one there is nothing to keep.
    if (!get_printer_state().plugin_status_state().service_has_helix_plugin()) {
        return;
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr || !api->is_connected()) {
        return;
    }
    const std::string rendered =
        filament_spools_record(backends_manage_active_spool(), printer_switches(),
                               macros_set_active_spool(), signals_now())
            .dump();
    // The inputs tick on every backend report; a rendering already weighed is
    // not weighed again.
    if (considered_ && *considered_ == rendered) {
        return;
    }
    // One presence read at a time: the inputs tick faster than it answers,
    // and write_record() renders whatever is current when it does.
    if (role_pending_) {
        return;
    }
    role_pending_ = true;
    resolve_fallback_sender_role(lifetime_.token(), [this](FallbackSenderRole role) {
        role_pending_ = false;
        write_record(role);
    });
}

void FilamentAccountingController::write_record(FallbackSenderRole role) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr || !api->is_connected()) {
        return;
    }
    const std::vector<FeedSignal> signals = signals_now();
    const nlohmann::json record = filament_spools_record(
        backends_manage_active_spool(), printer_switches(), macros_set_active_spool(), signals);
    std::string rendered = record.dump();
    considered_ = rendered;
    if (!writes_filament_spools_record(role, signals)) {
        spdlog::debug("[FilamentAccounting] Not writing the spool map ({})",
                      role == FallbackSenderRole::Standby ? "another screen owns it"
                                                          : "no feed signal yet");
        return;
    }
    if (written_ && *written_ == rendered) {
        return;
    }
    written_ = rendered;
    spdlog::debug("[FilamentAccounting] Spool map now: {}", rendered);
    post_item_if_changed(
        *api, kPluginFeaturesNamespace, kFilamentSpoolsKey, record,
        [] { spdlog::info("[FilamentAccounting] Spool map written"); },
        [this, token = lifetime_.token()](const MoonrakerError& err) {
            spdlog::warn("[FilamentAccounting] Writing the spool map failed: {}", err.message);
            token.defer("FilamentAccounting::write_failed", [this] {
                written_.reset();
                considered_.reset();
            });
        });
}

void FilamentAccountingController::on_active_tool_changed(int tool_index) {
    if (!armed_ || tool_index == baseline_tool_) {
        return;
    }
    baseline_tool_ = tool_index;
    auto& ps = get_printer_state();
    if (lv_subject_get_int(ps.print_state().get_print_active_subject()) == 0) {
        return;
    }
    const auto& tools = ToolState::instance().tools();
    if (tool_index < 0 || tool_index >= static_cast<int>(tools.size())) {
        return;
    }
    const ToolInfo& tool = tools[static_cast<size_t>(tool_index)];
    // A lane a backend feeds is switched on its load (AmsState).
    if (tool.backend_index >= 0) {
        return;
    }
    auto& status = ps.plugin_status_state();
    const PluginFeatureState state =
        status.plugin_feature(kFilamentAccountingFeature, kFilamentAccountingMinVersion);
    const bool opted_in =
        plugin_feature_opted_in(status.stored_opt_ins(), kFilamentAccountingFeature, /*acts=*/true);
    const ActiveSpoolOwner owner =
        active_spool_owner_now(ps.is_spoolman_available(), backends_manage_active_spool());
    // Only a switch this instance would make needs to know who else is here.
    if (filament_accounting_sender(state, opted_in, owner, FallbackSenderRole::Sender) !=
        FilamentAccountingSender::App) {
        return;
    }
    FeedEntry feed = feed_for_tool(tool, tool_index);
    std::vector<int> mapped = mapped_spools(signals_now());
    resolve_fallback_sender_role(
        lifetime_.token(), [this, state, opted_in, owner, feed, mapped](FallbackSenderRole role) {
            if (filament_accounting_sender(state, opted_in, owner, role) !=
                FilamentAccountingSender::App) {
                spdlog::debug("[FilamentAccounting] A local screen switches the spool");
                return;
            }
            switch_active_spool(feed, mapped);
        });
}

void FilamentAccountingController::switch_active_spool(const FeedEntry& feed,
                                                       const std::vector<int>& mapped) {
    IMoonrakerAPI* api = get_moonraker_api();
    if (api == nullptr) {
        return;
    }
    api->spoolman().get_spoolman_status(
        [token = lifetime_.token(), feed, mapped](bool /*connected*/, int active) {
            token.defer("FilamentAccounting::active_spool", [feed, mapped, active] {
                const SpoolSwitch decision = decide_spool_switch(&feed, active, mapped);
                if (decision.action == SpoolSwitch::Action::None) {
                    return;
                }
                IMoonrakerAPI* api = get_moonraker_api();
                if (api == nullptr) {
                    return;
                }
                const int spool =
                    decision.action == SpoolSwitch::Action::Set ? decision.spool_id : 0;
                spdlog::info("[FilamentAccounting] Tool {} feeding: active spool {} -> {}",
                             feed.tool, active, spool);
                api->spoolman().set_active_spool(
                    spool, [] {},
                    [](const MoonrakerError& err) {
                        spdlog::warn("[FilamentAccounting] Setting the active spool failed: {}",
                                     err.message);
                    });
            });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[FilamentAccounting] Spoolman status unavailable: {}", err.message);
        },
        /*silent=*/true);
}

} // namespace helix
