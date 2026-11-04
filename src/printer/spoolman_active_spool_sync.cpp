// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_active_spool_sync.h"

#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "i_moonraker_client.h"
#include "i_moonraker_sub_apis.h"
#include "pa_tuning.h"
#include "printer_state.h"
#include "replace_method_callback.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"
#include "tool_state.h"
#include "tune_controller.h"

#include <spdlog/spdlog.h>

namespace helix::spoolman_sync {

namespace {
constexpr const char* kActiveSpoolMethod = "notify_active_spool_set";
constexpr const char* kActiveSpoolHandler = "external_spool_sync";
constexpr const char* kStatusMethod = "notify_spoolman_status_changed";
constexpr const char* kStatusHandler = "spoolman_status_sync";

/// The startup status query and its sync, also rerun when Spoolman reconnects.
void request_status_sync(ISpoolmanAPI& spoolman) {
    // The reply arrives on the WebSocket thread and sync_from_status() reads AmsState.
    spoolman.get_spoolman_status(
        [&spoolman](bool connected, int active_spool_id) {
            helix::ui::queue_update("spoolman_active_spool_sync::sync_from_status",
                                    [&spoolman, connected, active_spool_id]() {
                                        sync_from_status(spoolman, connected, active_spool_id);
                                    });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[Application] Spoolman status unavailable: {}", err.message);
        },
        true); // silent: Spoolman not configured is normal
}
} // namespace

void try_assign_active_spool_to_tool(const SpoolInfo& spool) {
    auto* backend = AmsState::instance().get_backend();
    if (!backend || !backend->supports_per_tool_spool_assignment())
        return;

    auto& tool_state = ToolState::instance();
    if (!tool_state.spool_assignments_loaded())
        return; // assignments haven't loaded yet — can't safely auto-assign

    int tool_idx = tool_state.active_tool_index();
    if (tool_idx < 0)
        return;

    const auto& tools = tool_state.tools();
    if (tool_idx >= static_cast<int>(tools.size()))
        return;

    if (tools[tool_idx].spoolman_id > 0)
        return; // already has an assignment

    tool_state.assign_spool(tool_idx, spool.id, spool.display_name(),
                            static_cast<float>(spool.remaining_weight_g),
                            static_cast<float>(spool.initial_weight_g));
    AmsState::instance().sync_from_backend();
    spdlog::info("[Application] Auto-assigned Spoolman spool {} to "
                 "toolchanger tool {}",
                 spool.id, tool_idx);
}

void sync_external_spool(const SpoolInfo& spool, std::string log_context) {
    helix::ui::queue_update("spoolman_active_spool_sync::sync_external_spool",
                            [spool, log_context = std::move(log_context)]() {
                                // Tool-changer auto-assign runs BEFORE the bypass gate, not
                                // after it. The gate passes only when
                                // active_spool_describes_bypass() is true, which is
                                // `no backend || any_bypass_active()` — and every backend that
                                // answers supports_per_tool_spool_assignment() (TOOL_CHANGER,
                                // SNAPMAKER) hardcodes is_bypass_active() to false. Downstream
                                // of the gate the assign therefore required "a backend exists"
                                // and "no backend exists" at once, so it never ran on a real
                                // changer. The two concerns are independent: the gate is about
                                // which slot owns the EXTERNAL spool record, this is about
                                // which spool is mounted on the active TOOL. Its own guards
                                // (per-tool support, assignments loaded, valid index, not
                                // already assigned) are what decide whether it acts.
                                try_assign_active_spool_to_tool(spool);

                                // An AMS slot assignment sets Moonraker's global active spool
                                // too, so mirroring it onto the bypass unconditionally used to
                                // overwrite the bypass with whichever lane was assigned last.
                                if (!AmsState::instance().active_spool_describes_bypass()) {
                                    spdlog::debug(
                                        "[Application] Active spool {} belongs to a lane, not the "
                                        "bypass — not syncing external spool",
                                        spool.id);
                                    return;
                                }
                                // This record is the freshest view of the spool we will get
                                // — it arrives from the startup sync and from every
                                // notify_active_spool_set. Refresh the identity side
                                // channel from it (invalidate first: cache_identity() is
                                // insert-if-absent, so a stale entry would win otherwise).
                                SpoolmanManager::invalidate_identity(spool.id);
                                if (SpoolmanManager::cache_identity(spool)) {
                                    // Tell the label consumers a name they could not resolve
                                    // before is available now (#1264).
                                    AmsState::instance().bump_slots_version();
                                }

                                SlotInfo slot;
                                slot.slot_index = -2;
                                slot.global_index = -2;
                                // apply_spool_to_slot() owns the whole identity copy,
                                // multi-colour included, so spool_name stays the bare filament
                                // name the lane_data schema, AFC and Happy Hare all read.
                                apply_spool_to_slot(slot, spool);
                                AmsState::instance().set_external_spool_info(slot);
                                spdlog::info("[Application] External spool {}: {} (id={})",
                                             log_context, slot.spool_name, slot.spoolman_id);
                            });
}

void apply_saved_pressure_advance(const SpoolInfo& spool) {
    auto& state = get_printer_state();
    const bool printing = job_holds_machine(state.print_state().get_print_lifecycle());
    if (!helix::pa::should_auto_apply_on_activation(printing, spool.pressure_advance)) {
        if (spool.pressure_advance) {
            spdlog::info("[Application] Spool {} has K={:.4f} saved; not applied during a print",
                         spool.id, *spool.pressure_advance);
        }
        return;
    }
    std::vector<helix::pa::SpoolOnExtruder> mounted;
    auto& tools = ToolState::instance();
    for (const auto& tool : tools.tools()) {
        if (tool.spoolman_id > 0) {
            mounted.push_back({tool.spoolman_id, tool.effective_heater()});
        }
    }
    const std::string extruder = helix::pa::extruder_for_spool(
        spool.id, mounted, state.temperature_state().active_extruder_name());
    spdlog::info("[Application] Applying spool {}'s saved pressure advance {:.4f} to {}", spool.id,
                 *spool.pressure_advance, extruder);
    helix::tune::set_pressure_advance(get_moonraker_api(), extruder, *spool.pressure_advance);
}

void sync_from_status(ISpoolmanAPI& spoolman, bool connected, int active_spool_id) {
    if (!connected || active_spool_id <= 0) {
        spdlog::debug("[Application] No active Spoolman spool to sync "
                      "(connected={}, spool_id={})",
                      connected, active_spool_id);
        return;
    }

    // Check if existing external spool already matches
    auto existing = AmsState::instance().get_external_spool_info();
    if (existing && existing->spoolman_id == active_spool_id) {
        spdlog::debug("[Application] External spool already matches active "
                      "Spoolman spool {}",
                      active_spool_id);
        return;
    }

    // Fetch spool details and populate external spool
    spdlog::info("[Application] Syncing external spool from Moonraker active "
                 "spool {}",
                 active_spool_id);
    spoolman.get_spoolman_spool(
        active_spool_id,
        [active_spool_id](const std::optional<SpoolInfo>& spool_opt) {
            if (!spool_opt) {
                spdlog::warn("[Application] Active spool {} not found in "
                             "Spoolman",
                             active_spool_id);
                return;
            }
            sync_external_spool(*spool_opt, "synced");
        },
        [active_spool_id](const MoonrakerError& err) {
            spdlog::warn("[Application] Failed to fetch active spool {}: {}", active_spool_id,
                         err.message);
        });
}

void on_active_spool_set(ISpoolmanAPI& spoolman, const nlohmann::json& data) {
    // Callback receives full JSON-RPC message — extract params
    const auto& params_arr = data.contains("params") ? data["params"] : data;
    int spool_id = 0;
    if (params_arr.is_array() && !params_arr.empty()) {
        const auto& params = params_arr[0];
        if (params.contains("spool_id") && !params["spool_id"].is_null()) {
            spool_id = params["spool_id"].get<int>();
        }
    }

    if (spool_id <= 0) {
        // Same global-vs-bypass confusion as the sync arm, with
        // a worse blast radius: clearing an AMS lane makes
        // commit_slot_edit post set_active_spool(0), which comes
        // straight back as this notification. Taken at face
        // value it erased the whole bypass record — one tap on a
        // lane's "Clear Spool" and the user's bypass assignment
        // was gone.
        helix::ui::queue_update("spoolman_active_spool_sync::on_active_spool_set", []() {
            auto& ams = AmsState::instance();
            if (!ams.active_spool_describes_bypass()) {
                spdlog::debug("[Application] Active spool cleared for a lane, "
                              "not the bypass — keeping external spool");
                return;
            }
            // A manual entry never set the server's active spool, so a clear
            // says nothing about it. Unlinking the bypass spool commits one and
            // then clears the server, whose echo lands here.
            const auto existing = ams.get_external_spool_info();
            if (existing && existing->spoolman_id <= 0) {
                spdlog::debug("[Application] Active spool cleared, bypass holds an "
                              "unlinked entry, keeping it");
                return;
            }
            spdlog::info("[Application] Active spool cleared via notification");
            ams.clear_external_spool_info();
        });
        return;
    }

    spdlog::info("[Application] Active spool changed to {} via notification", spool_id);
    spoolman.get_spoolman_spool(
        spool_id,
        [](const std::optional<SpoolInfo>& spool_opt) {
            if (!spool_opt)
                return;
            sync_external_spool(*spool_opt, "updated via notification");
            // Only on an activation, never on the startup sync: a reconnect
            // must not overwrite a K the user set by hand since.
            helix::ui::queue_update(
                "spoolman_active_spool_sync::apply_saved_pressure_advance",
                [spool = *spool_opt]() { apply_saved_pressure_advance(spool); });
        },
        [spool_id](const MoonrakerError& err) {
            spdlog::warn("[Application] Failed to fetch notified spool {}: {}", spool_id,
                         err.message);
        });
}

void on_spoolman_status_changed(ISpoolmanAPI& spoolman, const nlohmann::json& msg) {
    const auto& params = msg.contains("params") ? msg["params"] : msg;
    if (!params.is_array() || params.empty() || !params[0].is_object()) {
        return;
    }
    const auto& status = params[0];
    const auto it = status.find("spoolman_connected");
    if (it == status.end() || !it->is_boolean()) {
        return;
    }
    const bool connected = it->get<bool>();
    spdlog::info("[Application] Spoolman {} (status notification)",
                 connected ? "connected" : "disconnected");
    get_printer_state().capabilities_state().set_spoolman_available(connected);
    if (connected) {
        request_status_sync(spoolman);
    }
}

void attach(IMoonrakerClient& client, ISpoolmanAPI& spoolman) {
    // Sync external spool from Moonraker's active Spoolman spool. This ensures the
    // filament panel shows the correct spool on startup, even if the active spool was
    // changed via Spoolman's web UI or another client.
    request_status_sync(spoolman);

    // Listen for Moonraker active spool changes (user changes spool in Spoolman web
    // UI or another client while HelixScreen is running).
    replace_method_callback(
        client, kActiveSpoolMethod, kActiveSpoolHandler,
        [&spoolman](const nlohmann::json& data) { on_active_spool_set(spoolman, data); });

    // Moonraker's link to Spoolman comes and goes on its own (Spoolman starts after
    // Moonraker, the network drops), and discovery asks only once.
    replace_method_callback(
        client, kStatusMethod, kStatusHandler,
        [&spoolman](const nlohmann::json& data) { on_spoolman_status_changed(spoolman, data); });
}

void detach(IMoonrakerClient& client) {
    client.unregister_method_callback(kActiveSpoolMethod, kActiveSpoolHandler);
    client.unregister_method_callback(kStatusMethod, kStatusHandler);
}

} // namespace helix::spoolman_sync
