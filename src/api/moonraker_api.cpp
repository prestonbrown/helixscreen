// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_api.h"

#include "bed_mesh_presence.h"
#include "core/update_queue.h"
#include "json_utils.h"
#include "moonraker_api_internal.h"
#include "runtime_config.h"
#include "spdlog/spdlog.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace helix;

using namespace moonraker_internal;

// ============================================================================
// MoonrakerAPI Implementation
// ============================================================================

MoonrakerAPI::MoonrakerAPI(IMoonrakerClient& client, PrinterState& state)
    : client_(client), state_(state) {
    // Create sub-APIs
    advanced_api_ = std::make_unique<MoonrakerAdvancedAPI>(client, *this);
    file_api_ = std::make_unique<MoonrakerFileAPI>(client);
    file_transfer_api_ = std::make_unique<MoonrakerFileTransferAPI>(client, http_base_url_);
    history_api_ = std::make_unique<MoonrakerHistoryAPI>(client);
    job_api_ = std::make_unique<MoonrakerJobAPI>(client, &state);
    motion_api_ = std::make_unique<MoonrakerMotionAPI>(client, state, safety_limits_);
    queue_api_ = std::make_unique<MoonrakerQueueAPI>(client, &state);
    rest_api_ = std::make_unique<MoonrakerRestAPI>(client, http_base_url_);
    spoolman_api_ = std::make_unique<MoonrakerSpoolmanAPI>(client);
    timelapse_api_ = std::make_unique<MoonrakerTimelapseAPI>(client, http_base_url_);

    // Initialize build_volume_version subject for change notifications
    lv_subject_init_int(&build_volume_version_, 0);

    // Wire up bed mesh callback: Client pushes data to advanced API when it arrives from WebSocket
    client_.set_bed_mesh_callback([this](const json& bed_mesh) {
        this->advanced_api_->update_bed_mesh(bed_mesh);
        // Presence verdicts only when the update actually speaks about
        // probed_matrix; partial updates carry no information about presence. Copy the observer
        // under the mutex: it is set on the main thread and read here on the WebSocket thread, and
        // on weakly-ordered targets an unsynchronized read can stay stale-null for the process
        // lifetime.
        std::function<void(bool)> observer;
        {
            std::lock_guard<std::mutex> lock(bed_mesh_presence_mutex_);
            observer = bed_mesh_presence_observer_;
        }
        if (bed_mesh.contains("probed_matrix")) {
            const bool present = helix::probed_matrix_has_mesh(bed_mesh["probed_matrix"]);
            if (observer) {
                observer(present);
            } else {
                // Diagnostic for the on-device flap silence: distinguishes
                // "observer never wired/replaced" from downstream gating.
                spdlog::debug("[MoonrakerAPI] bed-mesh presence verdict dropped: no observer "
                              "(present={})",
                              present);
            }
        }
    });
}

MoonrakerAPI::~MoonrakerAPI() {
    // Expire the deferred subject writes queued by notify_build_volume_changed()
    // before the subject they touch goes away. Without this a callback still
    // pending on the UpdateQueue is drained by whatever runs next and calls
    // lv_subject_set_int() on freed memory (#1165, #1146).
    async_lifetime_.invalidate();

    // Deinit LVGL subject before destruction to prevent dangling observer crashes
    // (same pattern as StaticSubjectRegistry — observers must be disconnected before lv_deinit)
    lv_subject_deinit(&build_volume_version_);

    // HTTP work runs on process-wide HttpExecutor::fast()/slow() singletons,
    // which are stopped in Application::shutdown() after MoonrakerManager is
    // destroyed.
}

bool MoonrakerAPI::ensure_http_base_url() {
    if (!http_base_url_.empty()) {
        return true;
    }

    // Never derive a real HTTP endpoint from a simulated connection. The mock
    // records the URL it was "connected" to so topology consumers can read it,
    // but deriving an HTTP base from it would point file transfers at the
    // operator's actual printer from a --test run that is supposed to touch
    // nothing. An explicitly configured base URL (above) still wins.
    if (get_runtime_config()->should_mock_moonraker()) {
        spdlog::debug("[Moonraker API] Mock Moonraker: not deriving HTTP base URL");
        return false;
    }

    // Try to derive from WebSocket URL
    const std::string& ws_url = client_.get_last_url();
    if (!ws_url.empty() && ws_url.find("ws://") == 0) {
        // Convert ws://host:port/websocket -> http://host:port
        std::string host_port = ws_url.substr(5); // Skip "ws://"
        auto slash_pos = host_port.find('/');
        if (slash_pos != std::string::npos) {
            host_port = host_port.substr(0, slash_pos);
        }
        http_base_url_ = "http://" + host_port;
        spdlog::info("[Moonraker API] Auto-derived HTTP base URL from WebSocket: {}",
                     http_base_url_);
        return true;
    }

    spdlog::error("[Moonraker API] HTTP base URL not configured and cannot derive from WebSocket");
    return false;
}

void MoonrakerAPI::notify_build_volume_changed() {
    // Increment version counter to notify observers
    // Counter is atomic since it's incremented from background thread and read from UI thread
    int new_version = ++build_volume_version_counter_;
    // Queue the subject update for the UI thread — lv_subject_set_int() is not thread-safe
    async_lifetime_.defer("MoonrakerAPI::notify_build_volume_changed", [this, new_version]() {
        lv_subject_set_int(&build_volume_version_, new_version);
    });
    spdlog::debug("[MoonrakerAPI] Build volume changed, version={}", new_version);
}

// ============================================================================
// Connection and Subscription Proxies
// ============================================================================

bool MoonrakerAPI::is_connected() const {
    return client_.get_connection_state() == ConnectionState::CONNECTED;
}

ConnectionState MoonrakerAPI::get_connection_state() const {
    return client_.get_connection_state();
}

std::string MoonrakerAPI::get_websocket_url() const {
    return client_.get_last_url();
}

SubscriptionId
MoonrakerAPI::subscribe_notifications(helix::SharedCallback<void(const json&)> callback) {
    return client_.register_notify_update(std::move(callback));
}

bool MoonrakerAPI::unsubscribe_notifications(SubscriptionId id) {
    return client_.unsubscribe_notify_update(id);
}

std::weak_ptr<bool> MoonrakerAPI::client_lifetime_weak() const {
    return client_.lifetime_weak();
}

void MoonrakerAPI::register_method_callback(const std::string& method, const std::string& name,
                                            helix::SharedCallback<void(const json&)> callback) {
    client_.register_method_callback(method, name, std::move(callback));
}

bool MoonrakerAPI::unregister_method_callback(const std::string& method, const std::string& name) {
    return client_.unregister_method_callback(method, name);
}

void MoonrakerAPI::suppress_disconnect_modal(uint32_t duration_ms) {
    client_.suppress_disconnect_modal(duration_ms);
}

void MoonrakerAPI::get_gcode_store(
    int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
    helix::SharedCallback<void(const MoonrakerError&)> on_error) {
    client_.get_gcode_store(count, std::move(on_success), std::move(on_error));
}

// ============================================================================
// Database Operations
// ============================================================================

void MoonrakerAPI::database_get_item(const std::string& namespace_name, const std::string& key,
                                     helix::SharedCallback<void(const json&)> on_success,
                                     ErrorCallback on_error) {
    json params = {{"namespace", namespace_name}, {"key", key}};
    // Silent: missing keys are expected (first-time reads before any save).
    // Callers handle errors via their error callback — no need for a toast.
    client_.send_jsonrpc(
        "server.database.get_item", params,
        [on_success](const json& response) {
            // send_jsonrpc passes the full JSON-RPC message (see
            // moonraker_request_tracker.cpp:217). Unwrap response["result"]
            // before extracting the "value" field — the shape is
            // {"jsonrpc","id","result":{"namespace","key","value": <payload>}}.
            if (on_success) {
                const json& result =
                    response.contains("result") ? response["result"] : json::object();
                const json* value = json_util::find_member(result, "value");
                on_success(value ? *value : json{});
            }
        },
        [on_error](const MoonrakerError& err) {
            if (on_error)
                on_error(err);
        },
        0,     // timeout_ms: use default
        true); // silent: suppress RPC_ERROR toast
}

void MoonrakerAPI::database_post_item(const std::string& namespace_name, const std::string& key,
                                      const json& value, helix::SharedCallback<void()> on_success,
                                      ErrorCallback on_error) {
    json params = {{"namespace", namespace_name}, {"key", key}, {"value", value}};
    client_.send_jsonrpc(
        "server.database.post_item", params,
        [on_success](const json&) {
            if (on_success)
                on_success();
        },
        [on_error](const MoonrakerError& err) {
            if (on_error)
                on_error(err);
        });
}

void MoonrakerAPI::database_get_namespace(const std::string& namespace_name,
                                          helix::SharedCallback<void(const json&)> on_success,
                                          ErrorCallback on_error) {
    // Moonraker's server.database.get_item accepts just a namespace (no key)
    // and returns the full namespace contents as a JSON object.
    json params = {{"namespace", namespace_name}};
    client_.send_jsonrpc(
        "server.database.get_item", params,
        [on_success](const json& response) {
            // See database_get_item above: unwrap response["result"] first.
            if (on_success) {
                const json& result =
                    response.contains("result") ? response["result"] : json::object();
                const json* value = json_util::find_member(result, "value");
                on_success(value ? *value : json::object());
            }
        },
        [on_error](const MoonrakerError& err) {
            if (on_error)
                on_error(err);
        },
        0,     // timeout_ms: use default
        true); // silent: suppress RPC_ERROR toast
}

void MoonrakerAPI::database_delete_item(const std::string& namespace_name, const std::string& key,
                                        helix::SharedCallback<void()> on_success,
                                        ErrorCallback on_error) {
    json params = {{"namespace", namespace_name}, {"key", key}};
    client_.send_jsonrpc(
        "server.database.delete_item", params,
        [on_success](const json&) {
            // Moonraker returns the deleted value on success; we don't care.
            if (on_success)
                on_success();
        },
        [namespace_name, key, on_success, on_error](const MoonrakerError& err) {
            // Deleting a missing key answers not-found ("Key '<k>' in namespace
            // '<n>' not found"); clear_async treats that as success
            // (idempotency). A missing method is not that answer.
            if (err.is_not_found()) {
                spdlog::debug("[MoonrakerAPI] database_delete_item({}/{}): missing-key "
                              "error treated as success (code={}, msg=\"{}\")",
                              namespace_name, key, err.code, err.message);
                if (on_success)
                    on_success();
                return;
            }
            if (on_error)
                on_error(err);
        },
        0,     // timeout_ms: use default
        true); // silent: suppress RPC_ERROR toast (missing-key is benign)
}
