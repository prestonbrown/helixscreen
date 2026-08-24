// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_rest_api.h
 * @brief Generic REST endpoint and WLED control operations via Moonraker
 *
 * Extracted from MoonrakerAPI to encapsulate all REST endpoint operations
 * and WLED control functionality in a dedicated class. Uses HTTP for
 * Moonraker extension plugins (e.g., ACE backend via ValgACE, WLED bridge).
 */

#pragma once

#include "i_moonraker_sub_apis.h"
#include "moonraker_error.h"
#include "moonraker_types.h"

#include <functional>
#include <string>

#include "hv/json.hpp"

// Forward declarations
namespace helix {
class IMoonrakerClient;
} // namespace helix

/**
 * @brief REST Endpoint and WLED Control API operations via Moonraker
 *
 * Provides HTTP GET/POST methods for communicating with Moonraker extension
 * plugins that expose REST APIs (e.g., ACE backend at /server/ace/ via ValgACE, WLED bridge
 * at /machine/wled/).
 *
 * These methods differ from the standard MoonrakerClient JSON-RPC methods:
 * - JSON-RPC (MoonrakerClient): Uses WebSocket, for standard Moonraker APIs
 * - REST (these methods): Uses HTTP, for extension plugins
 *
 * Thread safety: Callbacks are invoked from background threads. Callers must
 * ensure their callback captures remain valid for the duration of the request.
 *
 * Usage:
 *   MoonrakerRestAPI rest(client, http_base_url);
 *   rest.call_rest_get("/server/ace/status",
 *       [](const RestResponse& resp) { ... });
 */
class MoonrakerRestAPI : public IRestAPI {
  public:
    using SuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const MoonrakerError&)>;
    using RestCallback = std::function<void(const RestResponse&)>;

    /**
     * @brief Constructor
     *
     * @param client MoonrakerClient instance (must remain valid during API lifetime)
     * @param http_base_url Reference to HTTP base URL string (owned by MoonrakerAPI)
     */
    MoonrakerRestAPI(helix::IMoonrakerClient& client, const std::string& http_base_url);
    virtual ~MoonrakerRestAPI();

    // ========================================================================
    // Generic REST Endpoint Operations (for Moonraker extensions)
    // ========================================================================

    /**
     * @brief Call a Moonraker extension REST endpoint with GET
     *
     * Makes an HTTP GET request to a Moonraker extension endpoint.
     * Used for plugins like ACE (via ValgACE's Moonraker bridge) that expose REST APIs at
     * /server/xxx/.
     *
     * Example: call_rest_get("/server/ace/status", callback)
     *
     * @param endpoint REST endpoint path (e.g., "/server/ace/status")
     * @param on_complete Callback with response (success or failure)
     */
    void call_rest_get(const std::string& endpoint, RestCallback on_complete) override;

    /**
     * @brief Call a Moonraker extension REST endpoint with POST
     *
     * Makes an HTTP POST request to a Moonraker extension endpoint.
     * Used for plugins like ACE (via ValgACE's Moonraker bridge) that accept commands via REST.
     *
     * Example: call_rest_post("/server/ace/command", {"action": "load"}, callback)
     *
     * @param endpoint REST endpoint path (e.g., "/server/ace/command")
     * @param params JSON parameters to POST
     * @param on_complete Callback with response (success or failure)
     */
    void call_rest_post(const std::string& endpoint, const json& params,
                        RestCallback on_complete) override;

    // ========================================================================
    // WLED Control Operations (Moonraker WLED Bridge)
    // ========================================================================

    /**
     * @brief Get list of discovered WLED strips via Moonraker bridge
     *
     * GET /machine/wled/strips - Returns WLED devices configured in moonraker.conf.
     *
     * @param on_success Callback with RestResponse containing strip data
     * @param on_error Error callback
     */
    void wled_get_strips(RestCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Control a WLED strip via Moonraker bridge
     *
     * POST /machine/wled/strip with JSON body containing strip name and action.
     * Brightness and preset are optional (-1 means omit from request).
     *
     * @param strip WLED strip name (as configured in moonraker.conf)
     * @param action Action: "on", "off", or "toggle"
     * @param brightness Brightness 0-255 (-1 to omit)
     * @param preset WLED preset ID (-1 to omit)
     * @param on_success Success callback
     * @param on_error Error callback
     */
    void wled_set_strip(const std::string& strip, const std::string& action, int brightness,
                        int preset, SuccessCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Get WLED strip status via Moonraker bridge
     *
     * GET /machine/wled/strips - Returns current state of all WLED strips
     * including on/off status, brightness, and active preset.
     *
     * @param on_success Callback with RestResponse containing status data
     * @param on_error Error callback
     */
    void wled_get_status(RestCallback on_success, ErrorCallback on_error) override;

    /**
     * @brief Fetch server configuration from Moonraker
     *
     * GET /server/config - Returns the full server configuration including
     * WLED device addresses configured in moonraker.conf.
     *
     * @param on_success Callback with RestResponse containing config data
     * @param on_error Error callback
     */
    void get_server_config(RestCallback on_success, ErrorCallback on_error) override;

  protected:
    helix::IMoonrakerClient& client_;
    const std::string& http_base_url_;
};
