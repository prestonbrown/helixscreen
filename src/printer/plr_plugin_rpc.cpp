// SPDX-License-Identifier: GPL-3.0-or-later
#include "plr_plugin_rpc.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "moonraker_db_item.h"
#include "moonraker_error.h"
#include "plr_profile.h"
#include "printer_state.h"
#include "standing_print_options.h"

#include <spdlog/spdlog.h>

#include <utility>

#include "hv/json.hpp"

namespace helix::plr_rpc {

namespace {

using json = nlohmann::json;

const json& result_of(const json& response) {
    static const json kEmpty = json::object();
    const json* result = json_util::find_member(response, "result");
    return result != nullptr ? *result : kEmpty;
}

/// A call whose reply carries nothing the caller reads beyond success.
void send_action(IMoonrakerAPI& api, const char* method, json params, uint32_t timeout_ms,
                 helix::SharedCallback<void()> on_ok, ErrorCallback on_error) {
    api.get_client().send_jsonrpc(
        method, params,
        [on_ok = std::move(on_ok), method](const json& /*response*/) {
            spdlog::info("[PLR] {} accepted", method);
            if (on_ok) {
                on_ok();
            }
        },
        std::move(on_error), timeout_ms);
}

} // namespace

void request_status(IMoonrakerAPI& api, helix::SharedCallback<void(const PlrPluginStatus&)> on_ok,
                    ErrorCallback on_error) {
    api.get_client().send_jsonrpc(
        kPlrStatusRpc, json::object(),
        [on_ok = std::move(on_ok)](const json& response) {
            if (on_ok) {
                on_ok(plr_parse_plugin_status(result_of(response)));
            }
        },
        std::move(on_error), 0, /*silent=*/true);
}

void request_plan(IMoonrakerAPI& api, helix::SharedCallback<void(const PlrPlanReply&)> on_ok,
                  ErrorCallback on_error) {
    api.get_client().send_jsonrpc(
        kPlrPlanRpc, json::object(),
        [on_ok = std::move(on_ok), on_error](const json& response) {
            std::optional<PlrPlanReply> plan = plr_parse_plan_reply(result_of(response));
            if (!plan) {
                if (on_error) {
                    on_error(MoonrakerError::parse_error("no plan in the reply", kPlrPlanRpc));
                }
                return;
            }
            if (on_ok) {
                on_ok(*plan);
            }
        },
        on_error, kPlanTimeoutMs);
}

void prepare(IMoonrakerAPI& api, const std::string& plan_id, bool skip_chamber_wait,
             helix::SharedCallback<void()> on_ok, ErrorCallback on_error) {
    send_action(api, kPlrPrepareRpc,
                json{{"plan_id", plan_id}, {"skip_chamber_wait", skip_chamber_wait}},
                kPrepareTimeoutMs, std::move(on_ok), std::move(on_error));
}

void commit(IMoonrakerAPI& api, const std::string& plan_id,
            const std::vector<PlrConfirmation>& given, int tier,
            helix::SharedCallback<void()> on_ok, ErrorCallback on_error) {
    // The plugin starts the resumed job once it has heated and purged.
    screen_start_ledger().note_any(ScreenStartLedger::Clock::now(),
                                   std::chrono::milliseconds(kCommitTimeoutMs));
    send_action(api, kPlrCommitRpc, plr_commit_params(plan_id, given, tier), kCommitTimeoutMs,
                std::move(on_ok), std::move(on_error));
}

void abort(IMoonrakerAPI& api, const std::string& plan_id, helix::SharedCallback<void()> on_ok,
           ErrorCallback on_error) {
    send_action(api, kPlrAbortRpc, json{{"plan_id", plan_id}}, 0, std::move(on_ok),
                std::move(on_error));
}

void discard(IMoonrakerAPI& api, helix::SharedCallback<void()> on_ok, ErrorCallback on_error) {
    send_action(api, kPlrDiscardRpc, json::object(), 0, std::move(on_ok), std::move(on_error));
}

void request_recovery_probe(IMoonrakerAPI& api,
                            helix::SharedCallback<void(const PlrDetectResult&)> on_result,
                            ErrorCallback on_error) {
    // Logged at info: this is a side-effectful, at-most-once-per-connection
    // call, so a second line in a session's log is itself a bug report.
    spdlog::info("[PLR] Recovery probe -> {}", kCrealityDetectRpc);
    api.get_client().send_jsonrpc(
        kCrealityDetectRpc, json::object(),
        [on_result = std::move(on_result)](const json& response) {
            PlrDetectResult result;
            if (!plr_parse_check_continue_response(response, result)) {
                spdlog::warn("[PLR] Unusable recovery probe response: {}",
                             json_util::safe_dump(response));
            } else {
                spdlog::info("[PLR] Recovery probe: file_state={} eeprom_state={}",
                             result.file_state, result.eeprom_state);
            }
            if (on_result) {
                on_result(result);
            }
        },
        std::move(on_error));
}

void discard_with_rpc(IMoonrakerAPI& api, const PlrRecoveryPlan& plan,
                      helix::SharedCallback<void()> on_ok, ErrorCallback on_error) {
    if (plan.discard_rpc_method == kPlrDiscardRpc) {
        discard(api, std::move(on_ok), std::move(on_error));
        return;
    }
    // The method is copied: the reply can outlive the plan it came from.
    api.get_client().send_jsonrpc(
        plan.discard_rpc_method, json::object(),
        [on_ok = std::move(on_ok), method = plan.discard_rpc_method](const json& /*response*/) {
            spdlog::info("[PLR] {} accepted", method);
            if (on_ok) {
                on_ok();
            }
        },
        std::move(on_error));
}

void request_mesh_include_status(IMoonrakerAPI& api,
                                 helix::SharedCallback<void(bool present)> on_ok,
                                 ErrorCallback on_error) {
    api.get_client().send_jsonrpc(
        kPlrMeshIncludeRpc, json::object(),
        [on_ok = std::move(on_ok)](const json& response) {
            if (on_ok) {
                on_ok(json_util::safe_bool(result_of(response), "present"));
            }
        },
        std::move(on_error), 0, /*silent=*/true);
}

void mesh_include_action(IMoonrakerAPI& api, bool add,
                         helix::SharedCallback<void(bool present, bool changed)> on_ok,
                         ErrorCallback on_error) {
    api.get_client().send_jsonrpc(
        kPlrMeshIncludeRpc, json{{"action", add ? "add" : "remove"}},
        [on_ok = std::move(on_ok), method = kPlrMeshIncludeRpc](const json& response) {
            const json& result = result_of(response);
            spdlog::info("[PLR] {} accepted", method);
            if (on_ok) {
                on_ok(json_util::safe_bool(result, "present"),
                      json_util::safe_bool(result, "changed"));
            }
        },
        std::move(on_error));
}

void write_profile(IMoonrakerAPI& api, const json& profile, helix::SharedCallback<void()> on_stored,
                   ErrorCallback on_error) {
    post_item_if_changed(api, kPlrDbNamespace, kPlrProfileKey, profile, on_stored,
                         std::move(on_error), on_stored);
}

void write_user(IMoonrakerAPI& api, bool enabled, helix::SharedCallback<void()> on_ok,
                ErrorCallback on_error) {
    api.database_post_item(kPlrDbNamespace, kPlrUserKey, plr_user_json(enabled), std::move(on_ok),
                           std::move(on_error));
}

void refresh_features(IMoonrakerAPI& api, FeatureAnnounce announce) {
    // set_helix_plugin_features defers to the main thread itself. An absent
    // plugin answers with an empty map, which reads as Absent.
    api.job().check_helix_plugin(
        [announce](const HelixPluginInfo& info) {
            get_printer_state().set_helix_plugin_features(info.features, announce);
        },
        [](const MoonrakerError&) {});
}

} // namespace helix::plr_rpc
