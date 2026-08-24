// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file moonraker_rest_api.cpp
 * @brief Generic REST endpoint and WLED control operations for Moonraker extensions
 *
 * Provides HTTP GET/POST methods for communicating with Moonraker extension
 * plugins that expose REST APIs (e.g., ValgACE at /server/ace/).
 * Also provides WLED control operations via Moonraker's WLED bridge.
 *
 * Thread safety: Callbacks are invoked from background threads. Callers must
 * ensure their callback captures remain valid for the duration of the request.
 * During MoonrakerRestAPI destruction, pending threads are joined, so callbacks
 * will complete before the API object is destroyed.
 */

#include "moonraker_rest_api.h"

#include "http_executor.h"
#include "hv/requests.h"
#include "moonraker_error.h"
#include "spdlog/spdlog.h"

namespace {

/**
 * @brief Validate REST endpoint path for safety
 *
 * Rejects endpoints containing:
 * - Directory traversal (..)
 * - Control characters (newlines, carriage returns, null bytes)
 * - Empty strings
 *
 * @param endpoint Endpoint path to validate
 * @return true if safe, false otherwise
 */
bool is_safe_endpoint(const std::string& endpoint) {
    if (endpoint.empty()) {
        return false;
    }

    // Reject directory traversal
    if (endpoint.find("..") != std::string::npos) {
        return false;
    }

    // Reject control characters that could enable CRLF injection
    for (char c : endpoint) {
        if (c == '\n' || c == '\r' || c == '\0') {
            return false;
        }
    }

    return true;
}

} // namespace

// ============================================================================
// MoonrakerRestAPI Implementation
// ============================================================================

MoonrakerRestAPI::MoonrakerRestAPI(helix::IMoonrakerClient& client,
                                   const std::string& http_base_url)
    : client_(client), http_base_url_(http_base_url) {}

MoonrakerRestAPI::~MoonrakerRestAPI() = default;

// ============================================================================
// Generic REST Endpoint Operations
// ============================================================================

void MoonrakerRestAPI::call_rest_get(const std::string& endpoint, RestCallback on_complete) {
    // Validate endpoint for injection attacks
    if (!is_safe_endpoint(endpoint)) {
        spdlog::error("[MoonrakerRestAPI] call_rest_get: invalid endpoint '{}'", endpoint);
        if (on_complete) {
            RestResponse resp;
            resp.success = false;
            resp.error = "Invalid endpoint - contains unsafe characters";
            on_complete(resp);
        }
        return;
    }

    if (http_base_url_.empty()) {
        spdlog::error("[MoonrakerRestAPI] call_rest_get: HTTP base URL not configured");
        if (on_complete) {
            RestResponse resp;
            resp.success = false;
            resp.error = "HTTP base URL not configured - call set_http_base_url first";
            on_complete(resp);
        }
        return;
    }

    // Build URL - endpoint should start with /
    std::string url = http_base_url_;
    if (!endpoint.empty() && endpoint[0] != '/') {
        url += "/";
    }
    url += endpoint;

    spdlog::debug("[MoonrakerRestAPI] REST GET: {}", url);

    // Run HTTP request in a tracked thread
    helix::http::HttpExecutor::fast().submit([url, endpoint, on_complete]() {
        RestResponse result;

        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_GET;
        req->url = url;
        req->timeout = 30; // 30 second timeout

        auto resp = requests::request(req);

        if (!resp) {
            spdlog::error("[MoonrakerRestAPI] REST GET failed (no response): {}", url);
            result.success = false;
            result.error = "HTTP request failed - no response";
            if (on_complete) {
                on_complete(result);
            }
            return;
        }

        result.status_code = static_cast<int>(resp->status_code);

        // Success is 2xx status code
        if (resp->status_code >= 200 && resp->status_code < 300) {
            result.success = true;

            // Try to parse response body as JSON
            if (!resp->body.empty()) {
                try {
                    result.data = json::parse(resp->body);
                } catch (const json::parse_error& e) {
                    spdlog::trace("[MoonrakerRestAPI] REST GET response is not JSON: {}", e.what());
                    result.data = json::object();
                    result.data["_raw_body"] = resp->body;
                }
            }

            spdlog::debug("[MoonrakerRestAPI] REST GET {} succeeded (HTTP {})", endpoint,
                          result.status_code);
        } else {
            result.success = false;
            result.error =
                "HTTP " + std::to_string(result.status_code) + ": " + resp->status_message();

            // Try to extract error details from response body
            if (!resp->body.empty()) {
                try {
                    auto error_json = json::parse(resp->body);
                    if (error_json.contains("error") && error_json["error"].is_string()) {
                        result.error = error_json["error"].get<std::string>();
                    } else if (error_json.contains("message") &&
                               error_json["message"].is_string()) {
                        result.error = error_json["message"].get<std::string>();
                    }
                } catch (const std::exception& e) {
                    spdlog::trace("[MoonrakerRestAPI] Error response parsing failed: {}", e.what());
                }
            }

            // 404 = feature not configured/available, not an error worth warning about
            if (result.status_code == 404) {
                spdlog::debug("[MoonrakerRestAPI] REST GET {} failed: {}", endpoint, result.error);
            } else {
                spdlog::warn("[MoonrakerRestAPI] REST GET {} failed: {}", endpoint, result.error);
            }
        }

        if (on_complete) {
            on_complete(result);
        }
    });
}

void MoonrakerRestAPI::call_rest_post(const std::string& endpoint, const json& params,
                                      RestCallback on_complete) {
    // Validate endpoint for injection attacks
    if (!is_safe_endpoint(endpoint)) {
        spdlog::error("[MoonrakerRestAPI] call_rest_post: invalid endpoint '{}'", endpoint);
        if (on_complete) {
            RestResponse resp;
            resp.success = false;
            resp.error = "Invalid endpoint - contains unsafe characters";
            on_complete(resp);
        }
        return;
    }

    if (http_base_url_.empty()) {
        spdlog::error("[MoonrakerRestAPI] call_rest_post: HTTP base URL not configured");
        if (on_complete) {
            RestResponse resp;
            resp.success = false;
            resp.error = "HTTP base URL not configured - call set_http_base_url first";
            on_complete(resp);
        }
        return;
    }

    // Build URL - endpoint should start with /
    std::string url = http_base_url_;
    if (!endpoint.empty() && endpoint[0] != '/') {
        url += "/";
    }
    url += endpoint;

    // Serialize params to JSON string
    std::string body = params.dump();

    // Log without body content to avoid exposing sensitive data
    spdlog::debug("[MoonrakerRestAPI] REST POST: {} ({} bytes)", url, body.size());

    // Run HTTP request in a tracked thread
    helix::http::HttpExecutor::fast().submit([url, endpoint, body, on_complete]() {
        RestResponse result;

        auto req = std::make_shared<HttpRequest>();
        req->method = HTTP_POST;
        req->url = url;
        req->timeout = 30; // 30 second timeout
        req->content_type = APPLICATION_JSON;
        req->body = body;

        auto resp = requests::request(req);

        if (!resp) {
            spdlog::error("[MoonrakerRestAPI] REST POST failed (no response): {}", url);
            result.success = false;
            result.error = "HTTP request failed - no response";
            if (on_complete) {
                on_complete(result);
            }
            return;
        }

        result.status_code = static_cast<int>(resp->status_code);

        // Success is 2xx status code
        if (resp->status_code >= 200 && resp->status_code < 300) {
            result.success = true;

            // Try to parse response body as JSON
            if (!resp->body.empty()) {
                try {
                    result.data = json::parse(resp->body);
                } catch (const json::parse_error& e) {
                    spdlog::trace("[MoonrakerRestAPI] REST POST response is not JSON: {}",
                                  e.what());
                    result.data = json::object();
                    result.data["_raw_body"] = resp->body;
                }
            }

            spdlog::debug("[MoonrakerRestAPI] REST POST {} succeeded (HTTP {})", endpoint,
                          result.status_code);
        } else {
            result.success = false;
            result.error =
                "HTTP " + std::to_string(result.status_code) + ": " + resp->status_message();

            // Try to extract error details from response body
            if (!resp->body.empty()) {
                try {
                    auto error_json = json::parse(resp->body);
                    if (error_json.contains("error") && error_json["error"].is_string()) {
                        result.error = error_json["error"].get<std::string>();
                    } else if (error_json.contains("message") &&
                               error_json["message"].is_string()) {
                        result.error = error_json["message"].get<std::string>();
                    }
                } catch (const std::exception& e) {
                    spdlog::trace("[MoonrakerRestAPI] Error response parsing failed: {}", e.what());
                }
            }

            if (result.status_code == 404) {
                spdlog::debug("[MoonrakerRestAPI] REST POST {} failed: {}", endpoint, result.error);
            } else {
                spdlog::warn("[MoonrakerRestAPI] REST POST {} failed: {}", endpoint, result.error);
            }
        }

        if (on_complete) {
            on_complete(result);
        }
    });
}

// ============================================================================
// WLED Control Operations
// ============================================================================

void MoonrakerRestAPI::wled_get_strips(RestCallback on_success, ErrorCallback on_error) {
    spdlog::debug("[MoonrakerRestAPI] Fetching WLED strips");

    call_rest_get("/machine/wled/strips", [on_success, on_error](const RestResponse& resp) {
        if (resp.success) {
            if (on_success) {
                on_success(resp);
            }
        } else {
            spdlog::debug("[MoonrakerRestAPI] WLED get_strips failed: {}", resp.error);
            if (on_error) {
                on_error(MoonrakerError::unknown(resp.error, "wled"));
            }
        }
    });
}

void MoonrakerRestAPI::wled_get_status(RestCallback on_success, ErrorCallback on_error) {
    spdlog::debug("[MoonrakerRestAPI] Fetching WLED status");

    call_rest_get("/machine/wled/strips", [on_success, on_error](const RestResponse& resp) {
        if (resp.success) {
            if (on_success) {
                on_success(resp);
            }
        } else {
            spdlog::debug("[MoonrakerRestAPI] WLED get_status failed: {}", resp.error);
            if (on_error) {
                on_error(MoonrakerError::unknown(resp.error, "wled"));
            }
        }
    });
}

void MoonrakerRestAPI::get_server_config(RestCallback on_success, ErrorCallback on_error) {
    spdlog::debug("[MoonrakerRestAPI] Fetching server config");

    call_rest_get("/server/config", [on_success, on_error](const RestResponse& resp) {
        if (resp.success) {
            if (on_success) {
                on_success(resp);
            }
        } else {
            spdlog::warn("[MoonrakerRestAPI] get_server_config failed: {}", resp.error);
            if (on_error) {
                on_error(MoonrakerError::unknown(resp.error, "server_config"));
            }
        }
    });
}

void MoonrakerRestAPI::wled_set_strip(const std::string& strip, const std::string& action,
                                      int brightness, int preset, SuccessCallback on_success,
                                      ErrorCallback on_error) {
    json body;
    body["strip"] = strip;
    body["action"] = action;

    if (brightness >= 0) {
        body["brightness"] = brightness;
    }
    if (preset >= 0) {
        body["preset"] = preset;
    }

    spdlog::debug("[MoonrakerRestAPI] WLED set_strip: strip={} action={} brightness={} preset={}",
                  strip, action, brightness, preset);

    call_rest_post("/machine/wled/strip", body,
                   [on_success, on_error, strip](const RestResponse& resp) {
                       if (resp.success) {
                           if (on_success) {
                               on_success();
                           }
                       } else {
                           spdlog::warn("[MoonrakerRestAPI] WLED set_strip '{}' failed: {}", strip,
                                        resp.error);
                           if (on_error) {
                               on_error(MoonrakerError::unknown(resp.error, "wled"));
                           }
                       }
                   });
}
