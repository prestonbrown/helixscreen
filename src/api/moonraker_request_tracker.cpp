// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_request_tracker.h"

#include "hv/WebSocketClient.h"
#include "rpc_error_correlation.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "hv/json.hpp"

namespace helix {

RequestId MoonrakerRequestTracker::send(hv::WebSocketClient& ws, const std::string& method,
                                        const json& params,
                                        std::function<void(const json&)> success_cb,
                                        std::function<void(const MoonrakerError&)> error_cb,
                                        uint32_t timeout_ms, bool silent,
                                        std::optional<rpc_error_policy::CallerIntent> intent) {
    // Atomically fetch and increment to avoid race condition in concurrent calls
    // Note: request_id_ starts at 0, but we increment FIRST, so actual IDs start at 1
    // This ensures we never return 0 (INVALID_REQUEST_ID) for a valid request
    RequestId id = request_id_.fetch_add(1) + 1;

    // Create pending request
    PendingRequest request;
    request.id = id;
    request.method = method;
    request.success_callback = success_cb;
    request.error_callback = error_cb;
    request.timestamp = std::chrono::steady_clock::now();
    request.timeout_ms = (timeout_ms > 0) ? timeout_ms : default_request_timeout_ms_;
    // Callers that know their own intent state it; everyone else gets it inferred
    // from the two signals available here. The inference treats "supplied an
    // error_cb" as "will report this itself", which holds for the direct
    // send_jsonrpc callers — non-gcode RPCs whose errors reach the user through
    // no other channel. The gcode paths, which DO have Klipper's `!!` broadcast
    // as a second channel, pass their intent explicitly instead.
    request.intent = intent.value_or(rpc_error_policy::CallerIntent{silent, error_cb != nullptr});

    // Register request — check queue capacity first
    bool queue_full = false;
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);

        if (pending_requests_.size() >= MAX_PENDING_REQUESTS) {
            spdlog::warn("[Request Tracker] Pending request queue full ({} requests), "
                         "rejecting {} (id={})",
                         pending_requests_.size(), method, id);
            queue_full = true;
        } else {
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                spdlog::error("[INTERNAL] "
                              "[Request Tracker] Request ID {} already has a registered callback",
                              id);
                return INVALID_REQUEST_ID;
            }
            pending_requests_.insert({id, request});
            spdlog::trace(
                "[Request Tracker] Registered request {} for method {}, total pending: {}", id,
                method, pending_requests_.size());
        }
    }

    if (queue_full) {
        if (error_cb) {
            MoonrakerError err = MoonrakerError::connection_lost(
                method, "Request queue full — too many pending requests");
            try {
                error_cb(err);
            } catch (const std::exception& e) {
                spdlog::error("[Request Tracker] Queue-full error callback threw: {}", e.what());
            }
        }
        return INVALID_REQUEST_ID;
    }

    // Build and send JSON-RPC message with the registered ID
    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;
    rpc["id"] = id;

    // Only include params if not null or empty
    if (!params.is_null() && !params.empty()) {
        rpc["params"] = params;
    }

    spdlog::trace("[Request Tracker] send: {}", rpc.dump());
    int result = ws.send(rpc.dump());
    spdlog::trace("[Request Tracker] send({}) returned {}", method, result);

    // Return the request ID on success, or INVALID_REQUEST_ID on send failure
    if (result < 0) {
        // Send failed - remove pending request and invoke error callback
        std::function<void(const MoonrakerError&)> error_callback_copy;
        std::string method_name;
        {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            auto it = pending_requests_.find(id);
            if (it != pending_requests_.end()) {
                error_callback_copy = it->second.error_callback;
                method_name = it->second.method;
                pending_requests_.erase(it);
            }
        }
        spdlog::error("[Request Tracker] Failed to send request {} ({}), removed from pending", id,
                      method_name.empty() ? "unknown" : method_name);

        // Invoke error callback outside lock (prevents deadlock if callback sends new request)
        if (error_callback_copy) {
            try {
                error_callback_copy(MoonrakerError::connection_lost(method_name));
            } catch (const std::exception& e) {
                spdlog::error("[Request Tracker] Error callback threw exception: {}", e.what());
            }
        }
        return INVALID_REQUEST_ID;
    }

    return id;
}

int MoonrakerRequestTracker::send_fire_and_forget(hv::WebSocketClient& ws,
                                                  const std::string& method, const json& params) {
    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;
    rpc["id"] = request_id_.fetch_add(1) + 1;

    // Only include params if not null or empty
    if (!params.is_null() && !params.empty()) {
        rpc["params"] = params;
    }

    spdlog::trace("[Request Tracker] send_fire_and_forget: {}", rpc.dump());
    int result = ws.send(rpc.dump());
    return result < 0 ? result : 0;
}

bool MoonrakerRequestTracker::route_response(
    const json& msg,
    std::function<void(MoonrakerEventType, const std::string&, bool, const std::string&)>
        emit_event,
    std::function<bool()> suppress_error_toast) {
    // Check if this is a response message (has "id" field)
    if (!msg.contains("id")) {
        return false;
    }

    // Validate 'id' field type
    if (!msg["id"].is_number_integer()) {
        spdlog::error("[INTERNAL] [Request Tracker] Invalid 'id' type in response: {}",
                      msg["id"].type_name());
        return false;
    }

    uint64_t id = msg["id"].get<uint64_t>();

    spdlog::trace("[Request Tracker] Got response for id={}", id);

    // Copy callbacks out before invoking to avoid deadlock
    std::function<void(const json&)> success_cb;
    std::function<void(const MoonrakerError&)> error_cb;
    std::string method_name;
    bool has_error = false;
    rpc_error_policy::CallerIntent request_intent;
    bool found = false;
    MoonrakerError error;

    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        auto it = pending_requests_.find(id);
        if (it != pending_requests_.end()) {
            found = true;
            PendingRequest& request = it->second;
            method_name = request.method;
            request_intent = request.intent;

            // Check for JSON-RPC error. Only the *classification* happens here —
            // parsing the error payload is deliberately deferred until after the
            // erase below, so a malformed payload cannot strand the entry.
            if (msg.contains("error")) {
                has_error = true;
                error_cb = request.error_callback;
            } else {
                success_cb = request.success_callback;
            }

            pending_requests_.erase(it);
        }
    } // Lock released here

    if (!found) {
        return false;
    }

    // Parse the error payload only after the pending entry is gone and the lock
    // is released. Everything it needs (method_name, msg) is already copied or
    // outlives us, so a throw in here cannot leave the request pending until the
    // 60s timeout fires a bogus "timed out" toast at the user.
    if (has_error) {
        error = MoonrakerError::from_json_rpc(msg["error"], method_name);
    }

    // Invoke callbacks outside the lock to avoid deadlock
    if (has_error) {
        // Check if error toasts should be suppressed (e.g., during shutdown)
        bool suppress_toast = suppress_error_toast && suppress_error_toast();

        // Who reports this error is one decision with one implementation —
        // helix::rpc_error_policy::decide(), shared with the mock's inline gcode
        // path and the gcode APIs that build the intent. Emitting the generic
        // "Printer command failed" toast on top of a caller's own report would
        // double-report a single Klipper rejection (the K2 chamber key69
        // produced three stacked toasts).
        const rpc_error_policy::Decision d = rpc_error_policy::decide(
            request_intent, rpc_error_policy::RequestFacts{
                                /*has_broadcast_channel=*/
                                rpc_error_policy::method_has_broadcast_channel(method_name),
                                /*suppress_all=*/suppress_toast});

        if (d.emit_generic_toast) {
            spdlog::error("[Request Tracker] Request {} failed: {}", method_name, error.message);

            // Emit RPC error event only when no caller will surface it
            emit_event(MoonrakerEventType::RPC_ERROR,
                       fmt::format("Printer command '{}' failed: {}", method_name, error.message),
                       true, method_name);
        } else if (suppress_toast) {
            spdlog::debug("[Request Tracker] Request {} failed during shutdown (suppressed): {}",
                          method_name, error.message);
        } else {
            spdlog::debug("[Request Tracker] Request {} failed; caller handles UI: {}", method_name,
                          error.message);
        }

        // Record the exact Klipper-supplied error whenever someone is definitely
        // reporting it — the caller's own UI, or the generic fallback just
        // emitted above — so the GcodeError `!!` broadcast for the same root
        // cause (which arrives through a different transport channel) suppresses
        // its independent toast; see include/rpc_error_correlation.h. Deliberately
        // outside the branches above: the policy authorises recording alongside
        // the generic toast, not only alongside a caller's own report. A merely
        // `silent` caller records nothing — nothing was shown, so the `!!` copy
        // is the user's only remaining signal.
        if (d.record_for_dedup) {
            helix::rpc_error_correlation::record_caller_handled(error.message);
        }

        if (error_cb) {
            try {
                error_cb(error);
            } catch (const std::exception& e) {
                spdlog::error(
                    "[INTERNAL] [Request Tracker] Error callback for '{}' threw exception: {}",
                    method_name, e.what());
                // Symmetric with success_cb below: never re-throw out of
                // libhv's event loop. Pre-#931 a throwing callback unwound
                // through main_loop and exited 134.
            }
        }
    } else if (success_cb) {
        try {
            success_cb(msg);
        } catch (const std::exception& e) {
            spdlog::error(
                "[INTERNAL] [Request Tracker] Success callback for '{}' threw exception: {}",
                method_name, e.what());
            // Do NOT re-throw: stack unwinding between here and the outer
            // handler can leave libhv's event loop in a corrupt state,
            // leading to SIGSEGV on the next message cycle.
        }
    }

    return true;
}

bool MoonrakerRequestTracker::cancel(RequestId id) {
    if (id == INVALID_REQUEST_ID) {
        return false;
    }

    std::lock_guard<std::mutex> lock(requests_mutex_);
    auto it = pending_requests_.find(id);
    if (it != pending_requests_.end()) {
        spdlog::debug("[Request Tracker] Cancelled request {} ({})", id, it->second.method);
        pending_requests_.erase(it);
        return true;
    }

    spdlog::debug("[Request Tracker] Cancel failed: request {} not found (already completed?)", id);
    return false;
}

void MoonrakerRequestTracker::check_timeouts(
    std::function<void(MoonrakerEventType, const std::string&, bool, const std::string&)>
        emit_event) {
    // Two-phase pattern: collect events and callbacks under lock, invoke outside lock
    // This prevents deadlock if event handler or callback tries to send new request
    struct TimeoutInfo {
        std::string method_name;
        uint32_t timeout_ms;
        bool silent;
        std::function<void()> error_callback;
    };
    std::vector<TimeoutInfo> timed_out;

    // Phase 1: Find timed out requests and copy data (under lock)
    size_t pending_count = 0;
    uint32_t oldest_age_ms = 0;
    std::string oldest_method;
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        std::vector<uint64_t> timed_out_ids;

        // Visibility instrumentation for #909 — surface the size of
        // pending_requests_ + the oldest age periodically. If a request is
        // silently lost (libhv state-race, malformed WS frame dropped by
        // Moonraker), it sits here until is_timed_out() trips. Operators can
        // grep this line in debug bundles to confirm whether the timer
        // machinery is ticking and whether the queue is draining.
        pending_count = pending_requests_.size();
        for (const auto& [id, request] : pending_requests_) {
            uint32_t age = request.get_elapsed_ms();
            if (age > oldest_age_ms) {
                oldest_age_ms = age;
                oldest_method = request.method;
            }
        }

        for (auto& [id, request] : pending_requests_) {
            if (request.is_timed_out()) {
                spdlog::warn("[Request Tracker] Request {} ({}) timed out after {}ms", id,
                             request.method, request.get_elapsed_ms());

                TimeoutInfo info;
                info.method_name = request.method;
                info.timeout_ms = request.timeout_ms;
                info.silent = request.intent.silent;

                // Capture error callback in lambda if present
                if (request.error_callback) {
                    MoonrakerError error =
                        MoonrakerError::timeout(request.method, request.timeout_ms);
                    std::string method_name = request.method;
                    info.error_callback = [cb = request.error_callback, error, method_name]() {
                        try {
                            cb(error);
                        } catch (const std::exception& e) {
                            spdlog::error(
                                "[INTERNAL] [Request Tracker] Timeout error callback for {} "
                                "threw exception: {}",
                                method_name, e.what());
                        } catch (...) {
                            spdlog::error(
                                "[INTERNAL] [Request Tracker] Timeout error callback for {} "
                                "threw unknown exception",
                                method_name);
                        }
                    };
                }

                timed_out.push_back(std::move(info));
                timed_out_ids.push_back(id);
            }
        }

        // Remove timed out requests while still holding lock
        for (uint64_t id : timed_out_ids) {
            pending_requests_.erase(id);
        }
    } // Lock released here

    // Periodic pending-count log when the queue is non-empty — debug at any
    // age, escalated to warn if a single request has been pending longer than
    // 30 s (well past the 60 s default but a leading indicator that the
    // timer isn't draining and the queue is stuck — #909).
    //
    // Throttled to log on change: a single stuck request previously logged on
    // every monitor tick (~4 lines/sec) for its whole lifetime. Emit only when
    // the pending signature (count + oldest method + warn level) changes;
    // re-emit a still-stuck warn at most once per 10 s so the queue-stuck
    // indicator stays visible without flooding.
    //
    // The signature throttle alone does not quiet a queue that is draining
    // normally: every count decrement is a fresh signature, so one healthy poll
    // burst emits a line per in-flight request. On a Spoolman printer that ran
    // to 1275 of the 2000 lines in a debug bundle's ring buffer, capping its
    // reach at ~3.5 h of mostly nothing (bundle 3Q2GB74K). So the debug line
    // additionally requires the oldest request to have aged past
    // PENDING_LOG_MIN_AGE_MS — below that the queue is working, not stuck. The
    // warn path is unaffected: a request past its warn age always logs.
    //
    // The warn age is per-method, because "old" is not one number. A blocking
    // G-code script is bounded by the macro it is running, not by the network:
    // an AFC toolchange over a 2 m bowden takes over a minute of perfectly
    // healthy waiting, which at 30 s made the tracker the single loudest thing
    // in an AFC user's log (77 warnings in one bundle, every one of them normal).
    if (pending_count > 0) {
        const uint32_t warn_age = oldest_method == "printer.gcode.script"
                                      ? PENDING_WARN_AGE_GCODE_MS
                                      : PENDING_WARN_AGE_MS;
        const bool warn = oldest_age_ms > warn_age;
        const auto now = std::chrono::steady_clock::now();
        const bool signature_changed = pending_count != last_logged_pending_count_ ||
                                       oldest_method != last_logged_oldest_method_ ||
                                       warn != last_logged_was_warn_;
        const bool warn_refresh = warn && now - last_warn_log_ >= std::chrono::seconds(10);
        const bool old_enough = warn || oldest_age_ms >= PENDING_LOG_MIN_AGE_MS;

        if (old_enough && (signature_changed || warn_refresh)) {
            if (warn) {
                spdlog::warn("[Request Tracker] {} pending request(s); oldest '{}' age {}ms",
                             pending_count, oldest_method, oldest_age_ms);
                last_warn_log_ = now;
            } else {
                spdlog::debug("[Request Tracker] {} pending request(s); oldest '{}' age {}ms",
                              pending_count, oldest_method, oldest_age_ms);
            }
            last_logged_pending_count_ = pending_count;
            last_logged_oldest_method_ = oldest_method;
            last_logged_was_warn_ = warn;
        }
    } else {
        last_logged_pending_count_ = 0;
        last_logged_oldest_method_.clear();
        last_logged_was_warn_ = false;
    }

    // Phase 2: Emit events and invoke callbacks outside lock
    // (safe - event handlers and callbacks can call send without deadlock)
    for (auto& info : timed_out) {
        // Silent requests suppress the user-facing REQUEST_TIMEOUT event — callers that opt
        // into silent mode (e.g. EXCLUDE_OBJECT, which can legitimately sit queued for
        // minutes during pre-print heating) handle their own error UX via the error callback.
        if (!info.silent) {
            emit_event(MoonrakerEventType::REQUEST_TIMEOUT,
                       fmt::format("Printer command '{}' timed out after {}ms", info.method_name,
                                   info.timeout_ms),
                       false, info.method_name);
        }

        if (info.error_callback) {
            info.error_callback();
        }
    }
}

void MoonrakerRequestTracker::cleanup_all() {
    // Two-phase pattern: collect callbacks under lock, invoke outside lock
    // This prevents deadlock if callback tries to send new request
    std::vector<std::function<void()>> cleanup_callbacks;

    // Phase 1: Copy callbacks and clear map (under lock)
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);

        if (!pending_requests_.empty()) {
            spdlog::debug("[Request Tracker] Cleaning up {} pending requests due to disconnect",
                          pending_requests_.size());

            // Capture callbacks in lambdas
            for (auto& [id, request] : pending_requests_) {
                if (request.error_callback) {
                    MoonrakerError error = MoonrakerError::connection_lost(request.method);
                    std::string method_name = request.method;
                    cleanup_callbacks.push_back(
                        [cb = request.error_callback, error, method_name]() {
                            try {
                                cb(error);
                            } catch (const std::exception& e) {
                                spdlog::error(
                                    "[INTERNAL] [Request Tracker] Cleanup error callback for {} "
                                    "threw exception: {}",
                                    method_name, e.what());
                            } catch (...) {
                                spdlog::error(
                                    "[INTERNAL] [Request Tracker] Cleanup error callback for {} "
                                    "threw unknown exception",
                                    method_name);
                            }
                        });
                }
            }

            pending_requests_.clear();
        }
    } // Lock released here

    // Phase 2: Invoke callbacks outside lock (safe - callbacks can call send)
    for (auto& callback : cleanup_callbacks) {
        callback();
    }
}

} // namespace helix
