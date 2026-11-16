// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sound_backend.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hv/json.hpp" // JSON_HPP_OK: json values held by value

namespace helix::plugin {

using json = nlohmann::json;

struct RpcResult {
    bool ok = false;
    json value;
    std::string error;
};

/// May run on any thread.
using RpcCallback = std::function<void(RpcResult)>;

/// Everything Lua bindings may ask of the app. Bindings never reach Moonraker or the network
/// any other way, which is what lets tests substitute a fake.
struct PluginBackend {
    std::function<void(const std::string& script, RpcCallback)> gcode;
    std::function<void(const std::string& method, const json& params, RpcCallback)> call;
    /// `root` is `gcodes` or `config`.
    std::function<void(const std::string& root, const std::string& path, const std::string& content,
                       RpcCallback)>
        upload;
    /// `root` is `gcodes` or `config`; `value` is a JSON string holding the file content.
    /// `max_bytes` caps the transfer: the fetch is stopped once that much body has arrived.
    std::function<void(const std::string& root, const std::string& path, size_t max_bytes,
                       RpcCallback)>
        download;
    /// `value` is `{"status": int, "body": string}`; `ok` is false only when no response
    /// arrived. `max_body` caps the response body; the connection is cut once that much
    /// has arrived.
    std::function<void(const std::string& method, const std::string& url, const std::string& body,
                       const json& headers, uint32_t timeout_ms, size_t max_body, RpcCallback)>
        http;
    /// Registers a handler for a Moonraker notification method (it receives the whole
    /// message) and returns the function that unregisters it.
    std::function<std::function<void()>(const std::string& notify_method,
                                        std::function<void(const json& msg)>)>
        on_notify;
    /// Registers a handler for every notify_status_update the app's own status parsers
    /// receive: the live deltas and the client's whole-object snapshots (discovery after a
    /// connect or Klipper restart, subscription refreshes), which carry
    /// helix::kWholeObjectsMarker. Runs on whichever thread dispatched it. Returns the
    /// function that unregisters it.
    std::function<std::function<void()>(std::function<void(const json& notification)>)> on_status;
    /// Publishes `objects`, the plugin's merged printer.objects.subscribe map (object name
    /// to null for every field or an array of field names). An empty object clears the
    /// plugin's set. Main thread only.
    std::function<void(const std::string& plugin_id, const json& objects)> set_plugin_objects;
    /// Plays a named cue of the active sound theme through the app's sound manager, which
    /// applies the user's sound settings. Main thread only.
    std::function<void(const std::string& name, SoundPriority priority)> play_sound;
};

PluginBackend make_app_backend();

/// Sets one plugin's entry in the process-wide object registry behind
/// make_app_backend's set_plugin_objects. An empty `objects` clears the entry. Both
/// functions are thread-safe and never touch the Moonraker client.
void publish_plugin_objects(const std::string& plugin_id, const json& objects);

/// The union of every plugin's objects, merged with merge_subscription_objects. This is
/// what the app installs as the client's subscription extras provider: it runs on the
/// WebSocket thread while the client holds its internal mutex, so it only takes the
/// registry's own lock and returns a copy.
json plugin_objects_union();

/// Where a plugin HTTP request may connect.
struct HttpTarget {
    bool ok = false;
    std::string error;
    std::string connect_url; ///< the URL to request; for http, its host is the checked address
    std::string host_header; ///< the original Host for a pinned http connect; empty for https
};

/// Parses `url` exactly as libhv's client does, resolves its host, and refuses the request
/// when any address is loopback, unspecified, or in `forbidden_ips` (this machine's own
/// interfaces and the printer host): those carry the control the gcode and moonraker_write
/// permissions gate. Blocks on DNS, so call it off the main thread.
HttpTarget plan_http_target(const std::string& url, const std::vector<std::string>& forbidden_ips);

} // namespace helix::plugin
