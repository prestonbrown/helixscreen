// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "env_knobs.h"
#include "http_executor.h"
#include "json_utils.h"
#include "mock_disk_usage.h"
#include "moonraker_client_mock_internal.h"
#include "pa_calibration.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Moonraker lists `spoolman` in server.info only when the component is
/// configured. Reporting it on the WIRE — rather than short-circuiting the
/// capability flag after discovery — is what lets the real discovery sequence's
/// spoolman branch be exercised at all.
nlohmann::json mock_server_components(bool with_spoolman) {
    nlohmann::json comps = nlohmann::json::array({"file_manager", "database", "machine", "history",
                                                  "announcements", "job_queue", "update_manager"});
    if (with_spoolman) {
        comps.push_back("spoolman");
    }
    return comps;
}

} // namespace

namespace mock_internal {

namespace {

/// Splits "a.b.c" into segments; a dotless key yields one segment.
std::vector<std::string> split_key(const std::string& key) {
    std::vector<std::string> segments;
    size_t start = 0;
    while (true) {
        const size_t dot = key.find('.', start);
        segments.push_back(
            key.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return segments;
}

/// The record at a dotted key, or null when the namespace, any intermediate
/// record, or the final member is absent.
json* find_db_value(json& db, const std::string& ns, const std::string& key) {
    auto ns_it = db.find(ns);
    if (ns_it == db.end() || !ns_it->is_object()) {
        return nullptr;
    }
    json* node = &ns_it.value();
    for (const auto& segment : split_key(key)) {
        auto member = node->find(segment);
        if (member == node->end()) {
            return nullptr;
        }
        node = &member.value();
    }
    return node;
}

/// Writes @p value at a dotted key the way Moonraker's post_item does: "a.b"
/// writes member b of record a, creating a when absent.
void store_db_value(json& db, const std::string& ns, const std::string& key, const json& value) {
    const std::vector<std::string> segments = split_key(key);
    json& root = db[ns];
    if (!root.is_object()) {
        root = json::object();
    }
    json* node = &root;
    for (size_t i = 0; i + 1 < segments.size(); ++i) {
        if (!node->contains(segments[i]) || !(*node)[segments[i]].is_object()) {
            (*node)[segments[i]] = json::object();
        }
        node = &(*node)[segments[i]];
    }
    (*node)[segments.back()] = value;
}

/// Moonraker's database raises ServerError(..., 404), which its JSON-RPC layer
/// sends as code -32601 with the message kept.
MoonrakerError db_not_found(const std::string& method, const std::string& what) {
    return MoonrakerError::from_json_rpc({{"code", -32601}, {"message", what + " not found"}},
                                         method);
}

/// Moonraker's answer for a method it has not registered, as when the
/// spoolman component is not configured.
MoonrakerError method_not_found(const std::string& method) {
    return MoonrakerError::from_json_rpc({{"code", -32601}, {"message", "Method not found"}},
                                         method);
}

} // namespace

void register_server_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    // server.config - Moonraker's own configuration.
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-server-configuration
    // job_queue.automatic_transition defaults to false the way Moonraker's
    // does; HELIX_MOCK_JOB_QUEUE_AUTOMATIC_TRANSITION=1 flips it so the
    // true-branch UI behaviour is reachable in a --test run.
    registry["server.config"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           [[maybe_unused]] helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        const bool automatic_transition =
            helix::env_flag("HELIX_MOCK_JOB_QUEUE_AUTOMATIC_TRANSITION");

        json response = {
            {"jsonrpc", "2.0"},
            {"result",
             {{"config", {{"job_queue", {{"automatic_transition", automatic_transition}}}}}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.database.get_item - read one key from the mock database, or the
    // whole namespace when no key is given. A missing key or namespace answers
    // the JSON-RPC 404 the real server does, which is the signal callers treat
    // as "nothing stored yet".
    registry["server.database.get_item"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        if (!params.contains("namespace") || !params["namespace"].is_string() ||
            (params.contains("key") && !params["key"].is_string())) {
            if (error_cb) {
                error_cb(MoonrakerError::validation_error("server.database.get_item",
                                                          "get_item: 'namespace' is required"));
            }
            return true;
        }
        std::string ns = params["namespace"].get<std::string>();
        json& db = self->mock_db();
        if (!params.contains("key")) {
            auto ns_it = db.find(ns);
            if (ns_it == db.end()) {
                if (error_cb) {
                    error_cb(db_not_found("server.database.get_item", "Namespace " + ns));
                }
                return true;
            }
            if (success_cb) {
                success_cb(
                    json{{"result", {{"namespace", ns}, {"key", nullptr}, {"value", *ns_it}}}});
            }
            return true;
        }
        std::string key = params["key"].get<std::string>();
        json* value = find_db_value(db, ns, key);
        if (value == nullptr) {
            if (error_cb) {
                error_cb(db_not_found("server.database.get_item",
                                      "Key '" + key + "' in namespace '" + ns + "'"));
            }
            return true;
        }
        if (success_cb) {
            success_cb(json{{"result", {{"namespace", ns}, {"key", key}, {"value", *value}}}});
        }
        return true;
    };

    // server.database.post_item - write one key to the mock database.
    registry["server.database.post_item"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        if (!params.contains("namespace") || !params["namespace"].is_string() ||
            !params.contains("key") || !params["key"].is_string() || !params.contains("value")) {
            if (error_cb) {
                error_cb(MoonrakerError::validation_error(
                    "server.database.post_item",
                    "post_item: 'namespace', 'key' and 'value' are required"));
            }
            return true;
        }
        std::string ns = params["namespace"].get<std::string>();
        std::string key = params["key"].get<std::string>();
        store_db_value(self->mock_db(), ns, key, params["value"]);
        spdlog::debug("[MoonrakerClientMock] database post_item: {}/{}", ns, key);
        if (success_cb) {
            success_cb(
                json{{"result", {{"namespace", ns}, {"key", key}, {"value", params["value"]}}}});
        }
        return true;
    };

    // server.database.delete_item - delete one (possibly dotted) key from the
    // mock database. A missing key answers the JSON-RPC 404 the real server
    // does, which MoonrakerAPI::database_delete_item normalizes to success —
    // leaving this unregistered would freeze any caller waiting on either
    // callback, because unimplemented methods invoke neither.
    registry["server.database.delete_item"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        if (!params.contains("namespace") || !params["namespace"].is_string() ||
            !params.contains("key") || !params["key"].is_string()) {
            if (error_cb) {
                error_cb(MoonrakerError::validation_error(
                    "server.database.delete_item",
                    "delete_item: 'namespace' and 'key' are required"));
            }
            return true;
        }
        std::string ns = params["namespace"].get<std::string>();
        std::string key = params["key"].get<std::string>();
        const std::vector<std::string> segments = split_key(key);
        // The PARENT record is what erases the child: the namespace object for
        // a dotless key, the intermediate record for a dotted one.
        json& db = self->mock_db();
        json* parent = nullptr;
        if (segments.size() > 1) {
            parent = find_db_value(db, ns, key.substr(0, key.rfind('.')));
        } else {
            auto ns_it = db.find(ns);
            if (ns_it != db.end() && ns_it->is_object()) {
                parent = &ns_it.value();
            }
        }
        if (parent == nullptr || parent->find(segments.back()) == parent->end()) {
            if (error_cb) {
                error_cb(db_not_found("server.database.delete_item",
                                      "Key '" + key + "' in namespace '" + ns + "'"));
            }
            return true;
        }
        parent->erase(segments.back());
        spdlog::debug("[MoonrakerClientMock] database delete_item: {}/{}", ns, key);
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", json::object()}});
        }
        return true;
    };
    // server.connection.identify - Identify client to Moonraker for notifications
    // https://moonraker.readthedocs.io/en/latest/web_api/#identify-connection
    registry["server.connection.identify"] =
        [](MoonrakerClientMock* /*self*/, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        // Log the identification for debugging
        std::string client_name = params.value("client_name", "unknown");
        std::string version = params.value("version", "unknown");
        std::string type = params.value("type", "unknown");

        spdlog::debug("[MoonrakerClientMock] server.connection.identify: {} v{} ({})", client_name,
                      version, type);

        // Return a successful response with mock connection_id
        // This matches the real Moonraker response format
        static std::atomic<int> connection_counter{1000};
        json response = {{"jsonrpc", "2.0"},
                         {"result", {{"connection_id", connection_counter.fetch_add(1)}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.spoolman.status - Spoolman connectivity, queried by the real
    // discovery sequence whenever the component appears in server.info.
    registry["server.spoolman.status"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        const int active = self->spoolman_mock().get_mock_active_spool_id();
        json response = {{"jsonrpc", "2.0"},
                         {"result",
                          {{"spoolman_connected", self->is_mock_spoolman_enabled()},
                           {"pending_reports", json::array()},
                           {"spool_id", active > 0 ? json(active) : json(nullptr)}}}};
        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.spoolman.post_spool_id - set the active spool.
    registry["server.spoolman.post_spool_id"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        if (!self->is_mock_spoolman_enabled()) {
            if (error_cb) {
                error_cb(method_not_found("server.spoolman.post_spool_id"));
            }
            return true;
        }
        const int id = params.contains("spool_id") && params["spool_id"].is_number_integer()
                           ? params["spool_id"].get<int>()
                           : 0;
        self->spoolman_mock().set_active_spool_id(id);
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"},
                            {"result", {{"spool_id", id > 0 ? json(id) : json(nullptr)}}}});
        }
        return true;
    };

    // server.spoolman.proxy - Spoolman's REST API, answered by the mock
    // Spoolman server with the JSON Spoolman returns.
    registry["server.spoolman.proxy"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        if (!self->is_mock_spoolman_enabled()) {
            if (error_cb) {
                error_cb(method_not_found("server.spoolman.proxy"));
            }
            return true;
        }
        json result;
        MoonrakerError err;
        const bool ok = self->spoolman_mock().proxy(params, result, err);
        const int latency_ms = self->spoolman_mock().external_search_latency_ms();
        const std::string path = params.contains("path") && params["path"].is_string()
                                     ? params["path"].get<std::string>()
                                     : std::string();
        if (latency_ms > 0 && path.rfind("/v1/external/filament/search", 0) == 0) {
            // Answer from a worker thread after the delay, as a real response
            // arrives on the WebSocket thread.
            helix::http::HttpExecutor::fast().submit(
                [ok, latency_ms, result = std::move(result), err, success_cb, error_cb]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms));
                    if (!ok) {
                        if (error_cb) {
                            error_cb(err);
                        }
                    } else if (success_cb) {
                        success_cb(json{{"jsonrpc", "2.0"}, {"result", std::move(result)}});
                    }
                });
            return true;
        }
        if (!ok) {
            if (error_cb) {
                error_cb(err);
            }
            return true;
        }
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", std::move(result)}});
        }
        return true;
    };

    // server.webcams.list - HELIX_MOCK_WEBCAMS, or one unnamed MJPEG feed
    registry["server.webcams.list"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        json webcams = json::array();
        for (const auto& cam : self->mock_webcams()) {
            webcams.push_back({{"name", cam.name},
                               {"service", cam.service},
                               {"stream_url", cam.stream_url},
                               {"snapshot_url", cam.snapshot_url},
                               {"enabled", true}});
        }
        if (webcams.empty()) {
            webcams.push_back({{"name", ""},
                               {"service", "mjpegstreamer"},
                               {"stream_url", "/webcam/?action=stream"},
                               {"snapshot_url", "/webcam/?action=snapshot"},
                               {"enabled", true}});
        }
        {
            auto& posted = self->posted_webcams();
            std::lock_guard<std::mutex> lock(posted.mutex);
            for (const auto& item : posted.items) {
                webcams.push_back(item);
            }
        }
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", {{"webcams", webcams}}}});
        }
        return true;
    };

    // server.webcams.post_item / delete_item, keyed as Moonraker keys them: by uid when
    // the request carries one (404 when no entry has it), else by name.
    auto webcam_str = [](const json& j, const char* key) {
        auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    auto find_webcam = [webcam_str](std::vector<json>& items, const json& params) {
        const std::string uid = webcam_str(params, "uid");
        const char* key = uid.empty() ? "name" : "uid";
        const std::string want = uid.empty() ? webcam_str(params, "name") : uid;
        return std::find_if(items.begin(), items.end(),
                            [&](const json& e) { return webcam_str(e, key) == want; });
    };
    // Moonraker's JSON-RPC layer reports its 404 as -32601 with the message kept.
    auto not_found = [](const helix::SharedCallback<void(const MoonrakerError&)>& error_cb,
                        const char* method) {
        if (error_cb) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::JSON_RPC_ERROR;
            err.code = -32601;
            err.message = "Webcam not found";
            err.method = method;
            error_cb(err);
        }
        return true;
    };
    registry["server.webcams.post_item"] =
        [find_webcam,
         not_found](MoonrakerClientMock* self, const json& params,
                    helix::SharedCallback<void(const json&)> success_cb,
                    helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        json stored;
        {
            auto& posted = self->posted_webcams();
            std::lock_guard<std::mutex> lock(posted.mutex);
            auto& items = posted.items;
            auto it = find_webcam(items, params);
            if (it == items.end()) {
                if (params.contains("uid")) {
                    return not_found(error_cb, "server.webcams.post_item");
                }
                json item = params;
                item["enabled"] = params.contains("enabled") ? params["enabled"] : json(true);
                item["uid"] = "mock-webcam-" + std::to_string(++posted.next_uid);
                items.push_back(item);
                stored = item;
            } else {
                for (const auto& [key, value] : params.items()) {
                    if (key != "uid") {
                        (*it)[key] = value;
                    }
                }
                stored = *it;
            }
        }
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", {{"webcam", stored}}}});
        }
        return true;
    };
    registry["server.webcams.delete_item"] =
        [find_webcam,
         not_found](MoonrakerClientMock* self, const json& params,
                    helix::SharedCallback<void(const json&)> success_cb,
                    helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        json removed;
        {
            auto& posted = self->posted_webcams();
            std::lock_guard<std::mutex> lock(posted.mutex);
            auto& items = posted.items;
            auto it = find_webcam(items, params);
            if (it == items.end()) {
                return not_found(error_cb, "server.webcams.delete_item");
            }
            removed = *it;
            items.erase(it);
        }
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", {{"webcam", removed}}}});
        }
        return true;
    };

    // machine.device_power.devices - four devices, none with MOCK_EMPTY_POWER
    registry["machine.device_power.devices"] =
        [](MoonrakerClientMock* /*self*/, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        json devices = json::array();
        if (!helix::env_flag("MOCK_EMPTY_POWER")) {
            auto device = [](const char* name, const char* type, const char* status, bool locked) {
                return json{{"device", name},
                            {"type", type},
                            {"status", status},
                            {"locked_while_printing", locked}};
            };
            devices = {device("printer_psu", "gpio", "on", false),
                       device("chamber_light", "klipper_device", "on", true),
                       device("exhaust_fan", "klipper_device", "off", false),
                       device("led_strip", "gpio", "on", false)};
        }
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", {{"devices", devices}}}});
        }
        return true;
    };

    // server.helix.status - HelixPrint plugin presence.
    // Absent by default, which is the state a fresh printer is in and the one
    // the Advanced panel's Install row is bound to. HELIX_MOCK_HELIX_PLUGIN=1
    // reports it installed, =disabled installed with `enabled: False`. Leaving
    // this method unregistered is not the same
    // thing: an unimplemented method invokes NEITHER callback, so the plugin
    // subject stays at its -1 unknown and every surface gated on it is
    // unreachable in a mock run.
    registry["server.helix.status"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        const char* mode = std::getenv("HELIX_MOCK_HELIX_PLUGIN");
        const bool disabled = mode && std::string(mode) == "disabled";
        const bool installed = disabled || helix::env_flag("HELIX_MOCK_HELIX_PLUGIN");
        if (!installed) {
            // Moonraker answers an unknown endpoint with a JSON-RPC error, and
            // that error is what tells the app the plugin is absent rather than
            // merely unprobed.
            if (error_cb) {
                error_cb(MoonrakerError::unknown("Method not found", "server.helix.status"));
            }
            return true;
        }
        // HELIX_MOCK_HELIX_PLUGIN_LINK_DIR names the plugin's link directory
        // (`symlink_dir`), as a plugin installed on a printer whose Klipper
        // skips dot-directories reports a visible one.
        const char* link_dir = std::getenv("HELIX_MOCK_HELIX_PLUGIN_LINK_DIR");
        // The features map in the plugin's status shape (api 2). convert_3mf
        // owns its switch; a plugin set to enabled: False starts no converter,
        // so it reports the feature unavailable, as the real plugin does.
        json convert_3mf = {{"version", 1},
                            {"available", !disabled},
                            {"enabled", !disabled},
                            {"reason", disabled ? json("plugin_disabled") : json(nullptr)}};
        json response = {{"jsonrpc", "2.0"},
                         {"result",
                          {{"enabled", !disabled},
                           {"version", "1.0.1"},
                           {"symlink_dir", link_dir && *link_dir ? link_dir : ".helix_print"},
                           {"api", 2},
                           {"features", {{"convert_3mf", convert_3mf}}}}}};
        response["result"]["features"]["event_journal"] = convert_3mf;
        if (json plr = self->mock_plr_feature_entry(); !disabled && !plr.is_null()) {
            response["result"]["features"]["plr"] = std::move(plr);
        }
        if (!disabled) {
            // print_hooks acts on prints, so it is off until opted into, as
            // the plugin reads it: helix-screen/plugin_features.print_hooks.
            // HELIX_MOCK_PRINT_HOOKS=unavailable reports it unable to run, as a
            // plugin whose hooks module failed to load does.
            const char* hooks_mode = std::getenv("HELIX_MOCK_PRINT_HOOKS");
            const bool hooks_unavailable = hooks_mode && std::string(hooks_mode) == "unavailable";
            const json opt_in = self->mock_db_get("helix-screen", "plugin_features.print_hooks");
            response["result"]["features"]["print_hooks"] = {
                {"version", 1},
                {"available", !hooks_unavailable},
                {"enabled", !hooks_unavailable && opt_in.is_boolean() && opt_in.get<bool>()},
                {"reason", hooks_unavailable ? json("load_failed") : json(nullptr)}};
            // Automatic storage cleanup acts on files, so it is off until opted into.
            const json auto_cleanup =
                self->mock_db_get("helix-screen", "plugin_features.storage_auto_cleanup");
            response["result"]["features"]["storage_auto_cleanup"] = {
                {"version", 1},
                {"available", true},
                {"enabled", auto_cleanup.is_boolean() && auto_cleanup.get<bool>()},
                {"reason", nullptr}};
            // filament_accounting acts on spools: off until opted into, as
            // the plugin reads helix-screen/plugin_features.
            const json filament_opt_in =
                self->mock_db_get("helix-screen", "plugin_features.filament_accounting");
            response["result"]["features"]["filament_accounting"] = {
                {"version", 1},
                {"available", true},
                {"enabled", filament_opt_in.is_boolean() && filament_opt_in.get<bool>()},
                {"reason", nullptr}};
            // photos only observes, so it is on unless opted out.
            const json photos_opt_in = self->mock_db_get("helix-screen", "plugin_features.photos");
            // HELIX_MOCK_PHOTOS=unavailable reports it unable to reach the camera.
            const char* photos_mode = std::getenv("HELIX_MOCK_PHOTOS");
            const bool photos_unavailable =
                photos_mode && std::string(photos_mode) == "unavailable";
            response["result"]["features"]["photos"] = {
                {"version", 1},
                {"available", !photos_unavailable},
                {"enabled", !photos_unavailable &&
                                !(photos_opt_in.is_boolean() && !photos_opt_in.get<bool>())},
                {"reason", photos_unavailable
                               ? json("snapshot_unreachable: /webcam/?action=snapshot")
                               : json(nullptr)}};
            // fit_check_blocks_autostart acts on prints the same way: off until
            // opted into.
            const json fit_opt_in =
                self->mock_db_get("helix-screen", "plugin_features.fit_check_blocks_autostart");
            response["result"]["features"]["fit_check_blocks_autostart"] = {
                {"version", 1},
                {"available", true},
                {"enabled", fit_opt_in.is_boolean() && fit_opt_in.get<bool>()},
                {"reason", nullptr}};
            // pa_gauge heats and moves, so it is off until opted into, and the
            // plugin only offers it on a printer with a nozzle force sensor.
            const json gauge_opt_in = self->mock_db_get("helix-screen", "plugin_features.pa_gauge");
            const bool has_sensor = helix::pacal::can_add_measuring(self->hardware());
            response["result"]["features"]["pa_gauge"] = {
                {"version", 1},
                {"available", has_sensor},
                {"enabled", has_sensor && gauge_opt_in.is_boolean() && gauge_opt_in.get<bool>()},
                {"reason", has_sensor ? json(nullptr) : json("no_force_sensor")}};
            response["result"]["features"]["klipper_companions"] = {
                {"version", 1}, {"available", true}, {"enabled", true}, {"reason", nullptr}};
            // The rest of what the plugin ships, each on and running.
            for (const char* name : {"file_analysis", "server_edits", "config_edit", "storage"}) {
                response["result"]["features"][name] = {
                    {"version", 1}, {"available", true}, {"enabled", true}, {"reason", nullptr}};
            }
        }
        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.helix.analysis - the plugin's file analysis record. A file a test
    // gave a reply to (set_mock_file_analysis) answers it; any other file is
    // pending, as one the plugin has not reached yet.
    registry["server.helix.analysis"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        const std::string filename = helix::json_util::safe_string(params, "filename");
        json result = self->mock_file_analysis(filename);
        if (result.is_null()) {
            result = {{"filename", filename}, {"status", "pending"}};
        }
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", std::move(result)}});
        }
        return true;
    };

    // server.helix.filament_usage - the plugin's per-spool record of a job,
    // present with the plugin (HELIX_MOCK_HELIX_PLUGIN): every job used two
    // spools from two tools.
    registry["server.helix.filament_usage"] =
        [](MoonrakerClientMock* /*self*/, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        const char* mode = std::getenv("HELIX_MOCK_HELIX_PLUGIN");
        if (!(mode && std::string(mode) == "disabled") &&
            !helix::env_flag("HELIX_MOCK_HELIX_PLUGIN")) {
            if (error_cb) {
                error_cb(
                    MoonrakerError::unknown("Method not found", "server.helix.filament_usage"));
            }
            return true;
        }
        const std::string job_id = helix::json_util::safe_string(params, "job_id");
        json spools = json::array({
            {{"tool", 0}, {"lane", 0}, {"spool_id", 1}, {"mm", 4180.2}, {"grams", 12.47}},
            {{"tool", 1}, {"lane", 1}, {"spool_id", 2}, {"mm", 1530.0}, {"grams", 4.56}},
        });
        json record = {{"schema", 1},
                       {"job_id", job_id},
                       {"status", "completed"},
                       {"continues", nullptr},
                       {"spools", std::move(spools)}};
        if (success_cb) {
            success_cb(
                json{{"jsonrpc", "2.0"},
                     {"result", {{"job_id", job_id}, {"enabled", true}, {"record", record}}}});
        }
        return true;
    };

    // server.helix.journal - the plugin's printer event journal, present with
    // the plugin (HELIX_MOCK_HELIX_PLUGIN): a few events of the last hour, one a
    // `!!` line counted twelve times.
    registry["server.helix.journal"] =
        [](MoonrakerClientMock* /*self*/, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        const char* mode = std::getenv("HELIX_MOCK_HELIX_PLUGIN");
        if (!(mode && std::string(mode) == "disabled") &&
            !helix::env_flag("HELIX_MOCK_HELIX_PLUGIN")) {
            if (error_cb) {
                error_cb(MoonrakerError::unknown("Method not found", "server.helix.journal"));
            }
            return true;
        }
        const double now = static_cast<double>(std::time(nullptr));
        const json job = {{"job_id", "00002A"}, {"filename", "3DBenchy.gcode"}};
        json entries = json::array({
            {{"id", 1}, {"time", now - 3600}, {"kind", "klippy_ready"}, {"job", nullptr}},
            {{"id", 2},
             {"time", now - 1800},
             {"kind", "runout"},
             {"detail", "runout_sensor"},
             {"job", job}},
            {{"id", 3},
             {"time", now - 1795},
             {"kind", "pause"},
             {"detail", "runout"},
             {"job", job}},
            {{"id", 4},
             {"time", now - 900},
             {"kind", "gcode_error"},
             {"text", "Unknown command:\"PURGE_LINE\""},
             {"class", "command"},
             {"count", 12},
             {"last_time", now - 860},
             {"job", job}},
            {{"id", 6},
             {"time", now - 600},
             {"kind", "klippy_shutdown"},
             {"text", "MCU 'mcu' shutdown: Timer too close"},
             {"class", "mcu"},
             {"job", job}},
            {{"id", 7},
             {"time", now - 599},
             {"kind", "job_end"},
             {"detail", "klippy_shutdown"},
             {"job", job}},
        });
        if (success_cb) {
            success_cb(
                json{{"jsonrpc", "2.0"},
                     {"result", {{"entries", entries}, {"truncated", false}, {"latest_id", 7}}}});
        }
        return true;
    };

    // server.helix.storage - the plugin's storage report, present with the
    // plugin (HELIX_MOCK_HELIX_PLUGIN): a fixed breakdown on the mock's gcodes
    // disk, its print copies shrinking to nothing once cleaned up.
    registry["server.helix.storage"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        if (!helix::env_flag("HELIX_MOCK_HELIX_PLUGIN")) {
            if (error_cb) {
                error_cb(MoonrakerError::unknown("Method not found", "server.helix.storage"));
            }
            return true;
        }
        constexpr uint64_t kMb = 1024ULL * 1024ULL;
        const helix::mock::DiskMb disk = helix::mock::gcodes_disk_mb();
        const bool cleaned = self->mock_db_get("helix-mock", "storage_cleaned").is_boolean();
        auto consumer = [](const char* name, const char* root, uint64_t bytes, uint64_t files,
                           bool cleanable) {
            return json{{"name", name},   {"root", root},      {"bytes", bytes},
                        {"files", files}, {"available", true}, {"cleanable", cleanable}};
        };
        json consumers = json::array({
            consumer("helix_temp", "gcodes", cleaned ? 0 : 412 * kMb, cleaned ? 0 : 6, true),
            consumer("3mf_outputs", "gcodes", 96 * kMb, 3, false),
            consumer("analysis_sidecars", "gcodes", 18 * kMb, 41, true),
            consumer("photos", "gcodes", 230 * kMb, 88, true),
            consumer("timelapse", "timelapse", 1450 * kMb, 12, true),
            consumer("logs", "logs", 64 * kMb, 9, true),
            consumer("thumbnails", "gcodes", 31 * kMb, 164, false),
            consumer("user_gcode", "gcodes", 3820 * kMb, 41, false),
            consumer("plr", "data", 2 * kMb, 2, false),
        });
        const json opt_in =
            self->mock_db_get("helix-screen", "plugin_features.storage_auto_cleanup");
        json result = {
            {"volume",
             {{"total", disk.total * kMb},
              {"used", (disk.total - disk.free) * kMb},
              {"free", disk.free * kMb}}},
            {"floor", 100 * kMb},
            {"consumers", std::move(consumers)},
            {"auto_cleanup",
             {{"enabled", opt_in.is_boolean() && opt_in.get<bool>()}, {"last_run", nullptr}}}};
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"}, {"result", std::move(result)}});
        }
        return true;
    };

    // server.helix.storage_cleanup - frees the mock's print copies; any other
    // consumer reports nothing removed.
    registry["server.helix.storage_cleanup"] =
        [](MoonrakerClientMock* self, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        const std::string consumer = helix::json_util::safe_string(params, "consumer");
        uint64_t freed = 0;
        if (consumer == "helix_temp") {
            self->mock_db_set("helix-mock", "storage_cleaned", true);
            freed = 412ULL * 1024ULL * 1024ULL;
        }
        if (success_cb) {
            success_cb(json{{"jsonrpc", "2.0"},
                            {"result",
                             {{"consumer", consumer},
                              {"removed", freed > 0 ? 6 : 0},
                              {"freed", freed},
                              {"errors", 0}}}});
        }
        return true;
    };

    // server.helix.config - the plugin's parsed config read. The mock has no
    // config files to parse; Moonraker's answer for a method it lacks sends
    // the config editor down its file-API route, which the mock serves.
    registry["server.helix.config"] =
        [](MoonrakerClientMock* /*self*/, const json& /*params*/,
           helix::SharedCallback<void(const json&)> /*success_cb*/,
           helix::SharedCallback<void(const MoonrakerError&)> error_cb) -> bool {
        if (error_cb) {
            error_cb(MoonrakerError::unknown("Method not found", "server.helix.config"));
        }
        return true;
    };

    // server.helix.print_hooks.screen_start - the screen announcing its own
    // start to the plugin's print hooks; the mock has no hooks to skip.
    registry["server.helix.print_hooks.screen_start"] =
        [](MoonrakerClientMock* /*self*/, const json& params,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        if (success_cb) {
            success_cb(json{{"result", {{"filename", params.value("filename", "")}}}});
        }
        return true;
    };

    // server.info - Get Moonraker server information
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-server-info
    registry["server.info"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        // Map KlippyState enum to string
        std::string klippy_state_str;
        bool klippy_connected = false;
        switch (self->get_klippy_state()) {
        case MoonrakerClientMock::KlippyState::READY:
            klippy_state_str = "ready";
            klippy_connected = true;
            break;
        case MoonrakerClientMock::KlippyState::STARTUP:
            klippy_state_str = "startup";
            klippy_connected = false;
            break;
        case MoonrakerClientMock::KlippyState::SHUTDOWN:
            klippy_state_str = "shutdown";
            klippy_connected = true;
            break;
        case MoonrakerClientMock::KlippyState::ERROR:
            klippy_state_str = "error";
            klippy_connected = true;
            break;
        }

        spdlog::debug("[MoonrakerClientMock] server.info: klippy_state={}, connected={}",
                      klippy_state_str, klippy_connected);

        // Recent enough that no --test run trips Application's too-old-Moonraker
        // warning. HELIX_MOCK_MOONRAKER_VERSION drives the other side of that
        // gate, which is otherwise unreachable in mock.
        const char* version_env = std::getenv("HELIX_MOCK_MOONRAKER_VERSION");
        const std::string moonraker_version = version_env != nullptr ? version_env : "v0.9.3-mock";

        json response = {{"jsonrpc", "2.0"},
                         {"result",
                          {{"klippy_connected", klippy_connected},
                           {"klippy_state", klippy_state_str},
                           {"moonraker_version", moonraker_version},
                           {"api_version", json::array({1, 5, 0})},
                           {"api_version_string", "1.5.0"},
                           // Spoolman is reported on the WIRE, the way Moonraker reports it,
                           // rather than by short-circuiting the capability flag after
                           // discovery. The short-circuit is why the real sequence's
                           // spoolman branch went untested and shipped a flag flap.
                           {"components", mock_server_components(self->is_mock_spoolman_enabled())},
                           {"failed_components", json::array()},
                           {"registered_directories", json::array({"gcodes", "config", "logs"})},
                           {"warnings", json::array()},
                           {"websocket_count", 1}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // printer.info - Get Klipper printer information
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-printer-info
    registry["printer.info"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        // Map KlippyState enum to string and state message
        std::string state_str;
        std::string state_message;
        switch (self->get_klippy_state()) {
        case MoonrakerClientMock::KlippyState::READY:
            state_str = "ready";
            state_message = "Printer is ready";
            break;
        case MoonrakerClientMock::KlippyState::STARTUP:
            state_str = "startup";
            state_message = "Printer is starting up";
            break;
        case MoonrakerClientMock::KlippyState::SHUTDOWN:
            state_str = "shutdown";
            state_message = "Printer has been shut down";
            break;
        case MoonrakerClientMock::KlippyState::ERROR:
            state_str = "error";
            state_message = "Printer is in error state";
            break;
        }

        spdlog::debug("[MoonrakerClientMock] printer.info: state={}", state_str);

        // Detect HELIX_MOCK_KALICO env var for Kalico firmware simulation
        bool mock_kalico = helix::env_flag("HELIX_MOCK_KALICO");
        std::string app_name = mock_kalico ? "Kalico" : "Klipper";
        const char* version_env = std::getenv("HELIX_MOCK_KLIPPER_VERSION");
        const std::string software_version = version_env != nullptr ? version_env : "v0.12.0-mock";

        // The persona's hostname, which PrinterDetector's hostname heuristic
        // scores (an "ad5m" hostname is what loads the AD5M pre-print options).
        const std::string hostname(helix::mock::descriptor(self->get_printer_type()).hostname);

        json response = {{"jsonrpc", "2.0"},
                         {"result",
                          {{"state", state_str},
                           {"state_message", state_message},
                           {"hostname", hostname},
                           {"app", app_name},
                           {"software_version", software_version},
                           {"klipper_path", "/home/pi/klipper"},
                           {"python_path", "/home/pi/klippy-env/bin/python"},
                           {"log_file", "/home/pi/printer_data/logs/klippy.log"}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // machine.system_info - Get OS/system information
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-system-info
    registry["machine.system_info"] =
        [](MoonrakerClientMock* /*self*/, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        spdlog::debug("[MoonrakerClientMock] machine.system_info");

        json response = {
            {"jsonrpc", "2.0"},
            {"result",
             {{"system_info",
               {{"cpu_info",
                 {{"cpu_count", 4},
                  {"total_memory", 3906644},
                  {"memory_units", "kB"},
                  {"processor", "ARMv7 Processor rev 5 (v7l)"}}},
                {"distribution",
                 {{"name", "Ubuntu 22.04 LTS (mock)"},
                  {"id", "ubuntu"},
                  {"version", "22.04"},
                  {"version_parts", {{"major", "22"}, {"minor", "04"}, {"build_number", ""}}},
                  {"like", "debian"},
                  {"codename", "jammy"}}}}}}}};

        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // machine.update.status - update_manager state; only `busy` is modelled
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-update-status
    registry["machine.update.status"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        json response = {{"jsonrpc", "2.0"},
                         {"result",
                          {{"busy", self->update_manager_busy()},
                           {"github_rate_limit", 60},
                           {"version_info", json::object()}}}};
        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.restart - Restart Moonraker service
    // https://moonraker.readthedocs.io/en/latest/web_api/#restart-server
    registry["server.restart"] =
        [](MoonrakerClientMock* /*self*/, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        spdlog::info("[MoonrakerClientMock] server.restart (mock — no-op)");

        json response = {{"jsonrpc", "2.0"}, {"result", "ok"}};
        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    // server.temperature_store - Cached per-sensor temperature history
    // https://moonraker.readthedocs.io/en/latest/web_api/#get-cached-temperature-data
    // Mock returns a realistic heat/hold/cool curve so the connect-time seed
    // (#944) populates the graphs in mock mode for visual verification.
    registry["server.temperature_store"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           helix::SharedCallback<void(const json&)> success_cb,
           helix::SharedCallback<void(const MoonrakerError&)> /*error_cb*/) -> bool {
        TemperatureStore store = self->build_historical_temperature_store();

        json result = json::object();
        for (const auto& [key, series] : store) {
            json entry = json::object();
            entry["temperatures"] = series.temperatures;
            if (!series.targets.empty()) {
                entry["targets"] = series.targets;
            }
            if (!series.powers.empty()) {
                entry["powers"] = series.powers;
            }
            result[key] = std::move(entry);
        }
        spdlog::debug("[MoonrakerClientMock] server.temperature_store (mock — {} keys)",
                      result.size());

        json response = {{"jsonrpc", "2.0"}, {"result", std::move(result)}};
        if (success_cb) {
            success_cb(response);
        }
        return true;
    };

    spdlog::debug("[MoonrakerClientMock] Registered {} server method handlers", 12);
}

} // namespace mock_internal

void MoonrakerClientMock::mock_db_set(const std::string& ns, const std::string& key,
                                      const json& value) {
    mock_internal::store_db_value(mock_db_, ns, key, value);
}

json MoonrakerClientMock::mock_db_get(const std::string& ns, const std::string& key) {
    json* value = mock_internal::find_db_value(mock_db_, ns, key);
    return value ? *value : json();
}
