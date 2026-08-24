// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "esp_moonraker_client.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "helix_version.h" // HELIX_VERSION for server.connection.identify

#include <algorithm>
#include <utility>
#include <vector>

namespace helix {

namespace {
constexpr const char* TAG = "helixnet";

int64_t now_us() {
    return esp_timer_get_time();
}

// WS text/continuation opcodes we reassemble; everything else (binary, ping,
// pong, close) is handled by the component or ignored.
constexpr uint8_t OP_TEXT = 0x01;
constexpr uint8_t OP_CONTINUATION = 0x00;
} // namespace

EspMoonrakerClient::EspMoonrakerClient() {
    const esp_timer_create_args_t targs = {
        .callback = &EspMoonrakerClient::housekeeping_trampoline,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "helixnet_hk",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&targs, &housekeeping_timer_) == ESP_OK) {
        esp_timer_start_periodic(housekeeping_timer_, HOUSEKEEPING_PERIOD_US);
    } else {
        ESP_LOGE(TAG, "failed to create housekeeping timer");
        housekeeping_timer_ = nullptr;
    }
}

EspMoonrakerClient::~EspMoonrakerClient() {
    // Destruction order is safety-critical (desktop precedent). Flip alive FIRST
    // so any event already queued on either task early-outs before touching
    // members.
    alive_.store(false);

    // Tear the housekeeping timer down BEFORE the transport. esp_timer_stop()/
    // esp_timer_delete() prevent future dispatches but do NOT join a callback
    // that is already running on the ESP_TIMER_TASK — so after deleting we spin
    // until the in-flight tick (if any) clears timer_in_flight_. Without this the
    // WS stop + member teardown below could race process_timeouts() mid-walk of
    // the tracker map (UAF on a destroyed mutex/map).
    if (housekeeping_timer_) {
        esp_timer_stop(housekeeping_timer_);
        esp_timer_delete(housekeeping_timer_);
        housekeeping_timer_ = nullptr;
    }
    while (timer_in_flight_.load()) {
        vTaskDelay(1);
    }

    // Now stop the transport (blocks until the WS task drains); callback maps /
    // tracker are freed last by the member dtors.
    if (ws_) {
        esp_websocket_client_stop(ws_);
        esp_websocket_client_destroy(ws_);
        ws_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

int EspMoonrakerClient::connect(const char* url, std::function<void()> on_connected,
                                std::function<void()> on_disconnected) {
    if (!url) {
        ESP_LOGE(TAG, "connect: null url");
        return -1;
    }
    url_ = url;
    // v1 is plain LAN ws:// only; TLS is a later cert-bundle decision.
    if (url_.rfind("wss://", 0) == 0) {
        ESP_LOGE(TAG, "connect: wss:// not supported in v1 (%s)", url_.c_str());
        return -1;
    }

    on_connected_ = std::move(on_connected);
    on_disconnected_ = std::move(on_disconnected);

    // F5: disarm BEFORE tearing down any prior client. If the stop() below
    // synchronously emits a DISCONNECTED event for the client we're about to
    // replace, on_ws_disconnected() must see auto_reconnect_ == false so it
    // doesn't arm a reconnect for a connection we're discarding on purpose.
    auto_reconnect_.store(false);
    reconnect_pending_.store(false);

    // A prior connect()/probe may have left a live client or a pending
    // reconnect intent. Start clean.
    //
    // Invariant: ws_ may only be destroyed while no housekeeping pass is in
    // flight. process_timeouts() runs on the ESP_TIMER_TASK as well as on the
    // LVGL pump, and its execute_reconnect() path dereferences ws_ across a
    // stop()/start() pair — freeing the handle underneath a timer-task pass is
    // a UAF on the transport. The LVGL-side pump cannot race us (same thread as
    // connect()), so quiescing the timer is what closes the window. Same shape
    // as the dtor: stop the timer so no further tick can begin, then spin until
    // the current one (if any) clears the flag. The stop is what bounds the
    // spin — esp_timer_delete()/esp_timer_stop() prevent future dispatches but
    // do not join a callback already running.
    if (ws_) {
        if (housekeeping_timer_) {
            esp_timer_stop(housekeeping_timer_);
        }
        while (timer_in_flight_.load()) {
            vTaskDelay(1);
        }
        esp_websocket_client_handle_t old_ws = ws_.exchange(nullptr);
        esp_websocket_client_stop(old_ws);
        esp_websocket_client_destroy(old_ws);
        if (housekeeping_timer_) {
            // A silent restart failure would kill the housekeeping heartbeat:
            // no reconnects, no request timeouts, ever again.
            esp_err_t err = esp_timer_start_periodic(housekeeping_timer_, HOUSEKEEPING_PERIOD_US);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to restart housekeeping timer: %s", esp_err_to_name(err));
            }
        }
    }

    // R3: this is a new connection attempt — bump the generation so any
    // discovery chain still finishing from a previous connection abandons in
    // place, and force-clear the in-flight guard so it can't be stuck true
    // from that abandoned chain (see discovery_in_flight_ comment).
    connection_generation_.fetch_add(1);
    discovery_in_flight_.store(false);

    // Re-arm reconnection for the new connection — a transient
    // set_auto_reconnect(false) is reset here by contract.
    auto_reconnect_.store(true);
    next_reconnect_delay_ms_ = reconnect_min_delay_ms_;
    was_connected_ = false;

    esp_websocket_client_config_t cfg = {};
    cfg.uri = url_.c_str();
    // 4096 default is too small once nlohmann is on the callback path.
    // 8192 is measurement-backed: discovery (the deepest json work on this
    // task) peaks at ~6.5KB, deterministic across 44 forced-reconnect
    // cycles — see WS_TASK_STACK_BYTES and the watermark log in
    // discovery_subscribe, which reports the live margin every cycle. A
    // stack-overflow hypothesis for the one soak heap-corruption abort was
    // REFUTED by that measurement (esp32p4-task-9-report.md, Investigation).
    cfg.task_stack = WS_TASK_STACK_BYTES;
    // Bounds the per-DATA-event chunk, not the message; we reassemble.
    cfg.buffer_size = 32768;
    // Capped so a stop() from the LVGL thread can never wait out a full
    // unreachable-host connect attempt — see MAX_NETWORK_TIMEOUT_MS (which also
    // documents what the cap does not cover: DNS resolution ahead of it).
    cfg.network_timeout_ms =
        static_cast<int>(std::min(connection_timeout_ms_, MAX_NETWORK_TIMEOUT_MS));
    if (connection_timeout_ms_ > MAX_NETWORK_TIMEOUT_MS) {
        // Say so once per connect: otherwise a user who configured 8s and sees
        // a probe fail on a slow link has nothing in the log explaining why the
        // transport gave up early.
        ESP_LOGI(TAG, "network timeout capped to %ums (configured %ums) to bound UI-thread stalls",
                 MAX_NETWORK_TIMEOUT_MS, connection_timeout_ms_);
    }
    cfg.ping_interval_sec = 10;
    // Defect 1 (Task 9 confirm soak): we never set this, so the component
    // defaulted to its own WEBSOCKET_PINGPONG_TIMEOUT_SEC = 120s — LONGER
    // than our own 60s default_request_timeout_ms_. A connection that goes
    // silent (no observable read error — no FIN/RST reaches this task's read
    // loop, which can happen transiently) was therefore NEVER caught by
    // ping/pong at all: our own slower 60s request timeout always lost that
    // race and fired first, so the printer screen sat dataless for up to a
    // minute with discovery_in_flight_ stuck true, and no "Connection lost"
    // ever appeared in between. Set explicitly here, safely under 60s, so a
    // silent connection is instead caught by ping/pong first — turning that
    // wait into a normal disconnect + auto-reconnect. Two ping intervals'
    // worth of missed PONGs before giving up (not one) to avoid flagging a
    // single delayed pong under ordinary WiFi jitter as a dead connection.
    cfg.pingpong_timeout_sec = PING_PONG_TIMEOUT_SEC;
    // F8: the component's own auto-reconnect tears down/restarts transport
    // structures from inside its own websocket task, which is the root cause
    // of the spinlock_acquire assert seen on server-side disconnect (Plan 3
    // finding F8). We disable it entirely and drive reconnection ourselves —
    // on_ws_disconnected() only records intent; process_timeouts() (a
    // different task) executes the actual stop()/start().
    cfg.disable_auto_reconnect = true;

    ws_ = esp_websocket_client_init(&cfg);
    if (!ws_) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return -1;
    }

    esp_websocket_register_events(ws_, WEBSOCKET_EVENT_ANY,
                                  &EspMoonrakerClient::ws_event_trampoline, this);

    set_state(ConnectionState::CONNECTING);

    esp_err_t err = esp_websocket_client_start(ws_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_websocket_client_start failed: %s", esp_err_to_name(err));
        set_state(ConnectionState::FAILED);
        return -1;
    }
    return 0;
}

void EspMoonrakerClient::disconnect() {
    // F5: disarm BEFORE stop(). esp_websocket_client_stop() can emit a
    // DISCONNECTED event; if auto_reconnect_ were still true when that event
    // lands, on_ws_disconnected() would arm a zombie reconnect right after an
    // intentional disconnect. Also drop any reconnect intent a PRIOR
    // disconnect may have already scheduled — an intentional disconnect must
    // win over it.
    auto_reconnect_.store(false);
    reconnect_pending_.store(false);
    if (ws_) {
        esp_websocket_client_stop(ws_);
    }
    set_state(ConnectionState::DISCONNECTED);
}

bool EspMoonrakerClient::is_connected() const {
    // Single load: two separate reads of ws_ could straddle a concurrent
    // exchange(nullptr) in connect().
    esp_websocket_client_handle_t ws = ws_;
    return ws && esp_websocket_client_is_connected(ws);
}

void EspMoonrakerClient::arm_reconnect_intent() {
    const int delay_ms = next_reconnect_delay_ms_;
    reconnect_deadline_us_.store(now_us() + static_cast<int64_t>(delay_ms) * 1000);
    reconnect_generation_.store(connection_generation_.load());
    reconnect_pending_.store(true);
    // Manual exponential backoff: this attempt uses delay_ms; double up to the
    // cap for the NEXT one.
    next_reconnect_delay_ms_ = helix::next_backoff_delay_ms(delay_ms, reconnect_max_delay_ms_);
}

void EspMoonrakerClient::execute_reconnect() {
    if (!ws_) {
        return;
    }

    // R3: this is a new connection attempt — bump the generation and force-clear
    // the in-flight guard, same as connect() (see discovery_in_flight_).
    connection_generation_.fetch_add(1);
    discovery_in_flight_.store(false);

    // F5 ordering: hold auto-reconnect off across the teardown so a DISCONNECTED
    // event the stop() below emits cannot arm a second intent on top of the
    // attempt being executed right here.
    auto_reconnect_.store(false);

    // cfg.disable_auto_reconnect means the component's own abort path already
    // cleared client->run and let the websocket task exit, so by the time a
    // deferred intent drains, stop() usually returns ESP_FAIL ("Client was not
    // started"). That is the ordinary case here and must not abort the restart —
    // only a failed start() leaves nothing running at all.
    const esp_err_t stop_err = esp_websocket_client_stop(ws_);
    if (stop_err != ESP_OK) {
        ESP_LOGD(TAG, "reconnect: stop returned %s (already stopped is normal here)",
                 esp_err_to_name(stop_err));
    }
    // Drop any intent the stop above armed, then re-arm auto-reconnect for the
    // connection we are about to start.
    reconnect_pending_.store(false);
    auto_reconnect_.store(true);

    const esp_err_t start_err = esp_websocket_client_start(ws_);
    if (start_err != ESP_OK) {
        // Discarding this return was a dead end: nothing is running and no
        // disconnect event will ever arrive to schedule another try, so the
        // device stayed offline until a power cycle. Behave like a disconnect
        // instead — FAILED plus a fresh intent on the same backoff ladder, so
        // the next tick retries and keeps retrying.
        ESP_LOGE(TAG, "reconnect: start failed (%s) — retrying in %dms", esp_err_to_name(start_err),
                 next_reconnect_delay_ms_);
        set_state(ConnectionState::FAILED);
        arm_reconnect_intent();
    }
}

// ---------------------------------------------------------------------------
// State + events
// ---------------------------------------------------------------------------

void EspMoonrakerClient::set_state(ConnectionState next) {
    ConnectionState prev;
    std::function<void(ConnectionState, ConnectionState)> cb;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == next) {
            return;
        }
        prev = state_;
        state_ = next;
        cb = state_change_cb_;
        if (next == ConnectionState::RECONNECTING) {
            reconnecting_since_us_ = now_us();
        }
    }
    if (cb) {
        try {
            cb(prev, next);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "state_change_cb threw: %s", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "state_change_cb threw unknown");
        }
    }
}

ConnectionState EspMoonrakerClient::get_connection_state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

void EspMoonrakerClient::emit_event(MoonrakerEventType type, const std::string& message,
                                    bool is_error, const std::string& details) {
    MoonrakerEventCallback handler;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        handler = event_handler_;
    }
    if (!handler) {
        return;
    }
    MoonrakerEvent ev{type, message, details, is_error};
    try {
        handler(ev);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "event handler threw: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "event handler threw unknown");
    }
}

// ---------------------------------------------------------------------------
// WebSocket event handling (runs on the websocket_task)
// ---------------------------------------------------------------------------

void EspMoonrakerClient::ws_event_trampoline(void* arg, esp_event_base_t /*base*/, int32_t event_id,
                                             void* event_data) {
    auto* self = static_cast<EspMoonrakerClient*>(arg);
    if (!self || !self->alive_.load()) {
        return;
    }
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        self->on_ws_connected();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        self->on_ws_disconnected();
        break;
    case WEBSOCKET_EVENT_DATA:
        self->on_ws_data(static_cast<const esp_websocket_event_data_t*>(event_data));
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "websocket transport error");
        break;
    default:
        break;
    }
}

void EspMoonrakerClient::on_ws_connected() {
    ESP_LOGI(TAG, "connected to %s", url_.c_str());
    // Reset exponential backoff for the next disconnect.
    next_reconnect_delay_ms_ = reconnect_min_delay_ms_;
    // shrink the reassembly buffer back down after a session's peak.
    rx_buf_.clear();
    rx_buf_.shrink_to_fit();
    rx_skip_ = false;

    set_state(ConnectionState::CONNECTED);
    // Only a genuine reconnection emits RECONNECTED; the first-ever connect is
    // silent (desktop was_connected_ guard, moonraker_client.cpp:483-485).
    if (was_connected_) {
        emit_event(MoonrakerEventType::RECONNECTED, "Connected to Moonraker", false);
    }
    was_connected_ = true;

    if (on_connected_) {
        try {
            on_connected_();
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "on_connected threw: %s", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "on_connected threw unknown");
        }
    }

    std::vector<std::pair<std::string, std::function<void()>>> observers;
    {
        std::lock_guard<std::mutex> lock(observers_mutex_);
        observers.reserve(connected_observers_.size());
        for (const auto& [name, fn] : connected_observers_) {
            observers.emplace_back(name, fn);
        }
    }
    for (auto& [name, fn] : observers) {
        if (!fn) {
            continue;
        }
        try {
            fn();
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "connected observer '%s' threw: %s", name.c_str(), e.what());
        } catch (...) {
            ESP_LOGE(TAG, "connected observer '%s' threw unknown", name.c_str());
        }
    }
}

void EspMoonrakerClient::on_ws_disconnected() {
    ESP_LOGW(TAG, "disconnected from %s", url_.c_str());

    // Any discovery chain in flight is now invalid: its pending requests are about
    // to be failed with connection_lost below. Clear the guard so the next
    // on_connected → discover_printer() can start fresh. The connection_generation_
    // guard (R3) additionally makes that chain's own continuations no-ops if any
    // of them still land after this point.
    discovery_in_flight_.store(false);

    if (auto_reconnect_.load()) {
        // F8: record intent ONLY — never call esp_websocket_client_stop()/
        // start() from here. This handler runs on the websocket_task; touching
        // the transport from within its own event dispatch is the root cause
        // of the spinlock_acquire assert (Plan 3 finding F8). The actual
        // reconnect executes later from process_timeouts() (housekeeping
        // esp_timer + main-thread app_boot_tick pump — never this task).
        arm_reconnect_intent();
        set_state(ConnectionState::RECONNECTING);
    } else {
        // Reconnection suspended (probe flow): report a terminal DISCONNECTED
        // and leave no reconnect intent behind.
        reconnect_pending_.store(false);
        set_state(ConnectionState::DISCONNECTED);
    }
    emit_event(MoonrakerEventType::CONNECTION_LOST, "Connection to Moonraker lost", true);

    // Fail every in-flight request with connection_lost (two-phase).
    std::vector<std::function<void()>> cleanup;
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        cleanup.reserve(pending_.size());
        for (auto& [id, req] : pending_) {
            if (req.error_cb) {
                MoonrakerError err = MoonrakerError::connection_lost(req.method);
                auto cb = req.error_cb;
                cleanup.emplace_back([cb, err]() {
                    try {
                        cb(err);
                    } catch (...) {
                    }
                });
            }
        }
        pending_.clear();
    }
    for (auto& fn : cleanup) {
        fn();
    }

    if (on_disconnected_) {
        try {
            on_disconnected_();
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "on_disconnected threw: %s", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "on_disconnected threw unknown");
        }
    }
}

void EspMoonrakerClient::on_ws_data(const esp_websocket_event_data_t* d) {
    if (!d) {
        return;
    }
    // Only text (0x01) and its continuation frames (0x00) carry JSON-RPC.
    if (d->op_code != OP_TEXT && d->op_code != OP_CONTINUATION) {
        return;
    }

    if (d->payload_offset == 0) {
        rx_buf_.clear();
        rx_skip_ = (static_cast<size_t>(d->payload_len) > MAX_MESSAGE_BYTES);
        if (rx_skip_) {
            ESP_LOGE(TAG, "dropping %d-byte message (cap 256KB)", d->payload_len);
        } else {
            rx_buf_.reserve(std::min(static_cast<size_t>(d->payload_len), MAX_MESSAGE_BYTES));
        }
    }

    if (!rx_skip_ && d->data_ptr && d->data_len > 0) {
        rx_buf_.append(d->data_ptr, static_cast<size_t>(d->data_len));
    }

    // Message complete when this chunk reaches the declared payload length.
    const bool complete = (d->payload_offset + d->data_len) >= d->payload_len;
    if (complete) {
        if (!rx_skip_ && !rx_buf_.empty()) {
            dispatch_message(rx_buf_.data(), rx_buf_.size());
        }
        rx_buf_.clear();
        rx_skip_ = false;
    }
}

void EspMoonrakerClient::dispatch_message(const char* buf, size_t len) {
    json msg = json::parse(buf, buf + len, nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded()) {
        ESP_LOGW(TAG, "dropping unparseable %zu-byte message", len);
        return;
    }

    // Response (has "id") → request tracker.
    if (msg.contains("id") && msg["id"].is_number_integer()) {
        uint64_t id = msg["id"].get<uint64_t>();
        std::function<void(const json&)> success_cb;
        std::function<void(const MoonrakerError&)> error_cb;
        std::string method;
        bool silent = false;
        bool found = false;
        bool has_error = msg.contains("error");
        {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                found = true;
                method = it->second.method;
                silent = it->second.silent;
                if (has_error) {
                    error_cb = it->second.error_cb;
                } else {
                    success_cb = it->second.success_cb;
                }
                pending_.erase(it);
            }
        }
        if (found) {
            if (has_error) {
                MoonrakerError err = MoonrakerError::from_json_rpc(msg["error"], method);
                if (!silent && !error_cb) {
                    emit_event(MoonrakerEventType::RPC_ERROR,
                               "Printer command '" + method + "' failed: " + err.message, true,
                               method);
                }
                if (error_cb) {
                    try {
                        error_cb(err);
                    } catch (const std::exception& e) {
                        ESP_LOGE(TAG, "error cb for '%s' threw: %s", method.c_str(), e.what());
                    } catch (...) {
                    }
                }
            } else if (success_cb) {
                try {
                    success_cb(msg);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "success cb for '%s' threw: %s", method.c_str(), e.what());
                } catch (...) {
                }
            }
        }
    }

    // Notification (has "method") → notify + method + bed-mesh callbacks.
    if (msg.contains("method") && msg["method"].is_string()) {
        dispatch_notification(msg, /*include_method_callbacks=*/true);
    }
}

void EspMoonrakerClient::dispatch_notification(const json& msg, bool include_method_callbacks) {
    if (!msg.contains("method") || !msg["method"].is_string()) {
        return;
    }
    std::string method = msg["method"].get<std::string>();

    std::vector<std::function<void(const json&)>> to_invoke;
    std::function<void(const json&)> bed_mesh_cb;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (method == "notify_status_update" || method == "notify_filelist_changed") {
            to_invoke.reserve(notify_callbacks_.size());
            for (const auto& [id, cb] : notify_callbacks_) {
                to_invoke.push_back(cb);
            }
        }
        // Method-specific handlers fire only for genuine inbound notifications;
        // the synthetic dispatch_status_update path is notify-only (desktop parity).
        if (include_method_callbacks) {
            auto it = method_callbacks_.find(method);
            if (it != method_callbacks_.end()) {
                for (const auto& [handler, cb] : it->second) {
                    to_invoke.push_back(cb);
                }
            }
        }
        bed_mesh_cb = bed_mesh_callback_;
    }

    // Extract bed mesh before user callbacks (mirrors desktop ordering): a
    // notify_status_update carries params[0].bed_mesh on the containing object.
    if (bed_mesh_cb && method == "notify_status_update" && msg.contains("params") &&
        msg["params"].is_array() && !msg["params"].empty()) {
        const json& params0 = msg["params"][0];
        if (params0.contains("bed_mesh") && params0["bed_mesh"].is_object()) {
            try {
                bed_mesh_cb(params0["bed_mesh"]);
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "bed_mesh callback threw: %s", e.what());
            } catch (...) {
            }
        }
    }

    for (auto& cb : to_invoke) {
        if (!cb) {
            continue;
        }
        try {
            cb(msg);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "callback for '%s' threw: %s", method.c_str(), e.what());
        } catch (...) {
        }
    }
}

// ---------------------------------------------------------------------------
// Housekeeping timer (timeouts + informational FAILED transition)
// ---------------------------------------------------------------------------

void EspMoonrakerClient::housekeeping_trampoline(void* arg) {
    auto* self = static_cast<EspMoonrakerClient*>(arg);
    if (!self) {
        return;
    }
    // Mark the tick in-flight BEFORE reading any member so the dtor's quiesce
    // loop (which runs after esp_timer_delete) always observes an overlapping
    // callback and waits it out.
    self->timer_in_flight_.store(true);
    if (!self->alive_.load()) {
        self->timer_in_flight_.store(false);
        return;
    }

    self->process_timeouts();

    // 60s of RECONNECTING → FAILED (purely informational; reconnect continues).
    bool to_failed = false;
    {
        std::lock_guard<std::mutex> lock(self->state_mutex_);
        if (self->state_ == ConnectionState::RECONNECTING &&
            (now_us() - self->reconnecting_since_us_) > RECONNECTING_TO_FAILED_US) {
            to_failed = true;
        }
    }
    if (to_failed) {
        self->set_state(ConnectionState::FAILED);
        self->emit_event(MoonrakerEventType::CONNECTION_FAILED, "Reconnection has not succeeded",
                         true);
    }

    self->timer_in_flight_.store(false);
}

void EspMoonrakerClient::process_timeouts() {
    struct TimedOut {
        std::string method;
        bool silent;
        std::function<void(const MoonrakerError&)> cb;
        MoonrakerError err;
    };
    std::vector<TimedOut> timed_out;
    const int64_t now = now_us();
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        // Defense in depth: bail under the lock if teardown began after the
        // trampoline's entry check (the dtor's quiesce loop still waits on us).
        if (!alive_.load()) {
            return;
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            const int64_t age_us = now - it->second.sent_us;
            if (age_us > static_cast<int64_t>(it->second.timeout_ms) * 1000) {
                TimedOut t;
                t.method = it->second.method;
                t.silent = it->second.silent;
                t.cb = it->second.error_cb;
                t.err = MoonrakerError::timeout(it->second.method, it->second.timeout_ms);
                timed_out.push_back(std::move(t));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& t : timed_out) {
        if (!t.silent) {
            emit_event(MoonrakerEventType::REQUEST_TIMEOUT,
                       "Printer command '" + t.method + "' timed out", false, t.method);
        }
        if (t.cb) {
            try {
                t.cb(t.err);
            } catch (...) {
            }
        }
    }

    // F8: execute any pending auto-reconnect intent recorded by
    // on_ws_disconnected(). process_timeouts() is driven from the
    // housekeeping esp_timer (ESP_TIMER_TASK) and the main-thread
    // app_boot_tick pump — NEVER the websocket task — so the stop()/start()
    // execute_reconnect() performs cannot race the websocket task's own event
    // dispatch, unlike the component's built-in auto-reconnect this replaces.
    //
    // The claim itself MUST still be atomic across those two pump contexts:
    // a plain load()-then-store(false) lets both tasks observe pending==true
    // before either clears it, so both would call stop()+start() on the same
    // ws_ concurrently — the exact cross-task transport race F8 exists to
    // eliminate, just relocated here (code review finding). exchange(false)
    // makes exactly one caller win: only the task whose exchange() call
    // observes the prior value as true proceeds; the loser sees false and
    // does nothing. Deadline is checked first (a plain load, no claim) so a
    // not-yet-due intent is left untouched for the next tick.
    if (now_us() >= reconnect_deadline_us_.load() && reconnect_pending_.exchange(false)) {
        // R3: if a manual connect()/force_reconnect() bumped the generation
        // since this intent was scheduled, it belongs to a connection nothing
        // is waiting on anymore — drop it instead of restarting on top of
        // whatever the manual path already did.
        const bool current = (reconnect_generation_.load() == connection_generation_.load());
        if (current && auto_reconnect_.load() && ws_) {
            ESP_LOGI(TAG, "auto-reconnect: restarting transport");
            execute_reconnect();
        }
    }
}

// ---------------------------------------------------------------------------
// JSON-RPC send path
// ---------------------------------------------------------------------------

int EspMoonrakerClient::send_envelope(const json& envelope) {
    if (!is_connected()) {
        return -1;
    }
    std::string payload = envelope.dump();
    int sent = esp_websocket_client_send_text(ws_, payload.data(), static_cast<int>(payload.size()),
                                              pdMS_TO_TICKS(SEND_TIMEOUT_MS));
    return sent;
}

int EspMoonrakerClient::send_jsonrpc(const std::string& method) {
    return send_jsonrpc(method, json());
}

int EspMoonrakerClient::send_jsonrpc(const std::string& method, const json& params) {
    if (!is_connected()) {
        return -1;
    }
    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;
    rpc["id"] = next_request_id_.fetch_add(1) + 1;
    if (!params.is_null() && !params.empty()) {
        rpc["params"] = params;
    }
    int result = send_envelope(rpc);
    return result < 0 ? result : 0;
}

RequestId EspMoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                           std::function<void(const json&)> cb) {
    return send_jsonrpc(method, params, std::move(cb), nullptr, 0, false);
}

RequestId EspMoonrakerClient::send_jsonrpc(const std::string& method, const json& params,
                                           std::function<void(const json&)> success_cb,
                                           std::function<void(const MoonrakerError&)> error_cb,
                                           uint32_t timeout_ms, bool silent,
                                           std::optional<rpc_error_policy::CallerIntent> intent) {
    // CallerIntent governs who surfaces a failed request's error (caller
    // callback vs the tracker's generic toast) on the desktop client, where
    // both surfaces exist. The shim has exactly one surface — error_cb — so
    // every intent resolves to "the caller owns the report", which is what
    // track_and_send() already does. Accepted for signature parity with
    // IMoonrakerClient (esp32 CI static-asserts the full override set in
    // coverage_assert.cpp) and deliberately not consulted beyond that.
    (void)intent;
    if (!is_connected()) {
        // Fail fast so callers don't wait on a request that never times out.
        if (error_cb) {
            try {
                error_cb(MoonrakerError::connection_lost(method));
            } catch (...) {
            }
        }
        return INVALID_REQUEST_ID;
    }
    return track_and_send(method, params, std::move(success_cb), std::move(error_cb), timeout_ms,
                          silent);
}

RequestId EspMoonrakerClient::track_and_send(const std::string& method, const json& params,
                                             std::function<void(const json&)> success_cb,
                                             std::function<void(const MoonrakerError&)> error_cb,
                                             uint32_t timeout_ms, bool silent) {
    RequestId id = next_request_id_.fetch_add(1) + 1;

    bool queue_full = false;
    {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        if (pending_.size() >= MAX_PENDING_REQUESTS) {
            queue_full = true;
        } else {
            Pending req;
            req.method = method;
            req.success_cb = success_cb;
            req.error_cb = error_cb;
            req.sent_us = now_us();
            req.timeout_ms = (timeout_ms > 0) ? timeout_ms : default_request_timeout_ms_;
            req.silent = silent;
            pending_.emplace(id, std::move(req));
        }
    }

    if (queue_full) {
        ESP_LOGW(TAG, "request queue full (%zu), rejecting %s", MAX_PENDING_REQUESTS,
                 method.c_str());
        if (error_cb) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::CONNECTION_LOST;
            err.method = method;
            err.message = "Request queue full — too many pending requests";
            try {
                error_cb(err);
            } catch (...) {
            }
        }
        return INVALID_REQUEST_ID;
    }

    json rpc;
    rpc["jsonrpc"] = "2.0";
    rpc["method"] = method;
    rpc["id"] = id;
    if (!params.is_null() && !params.empty()) {
        rpc["params"] = params;
    }

    int result = send_envelope(rpc);
    if (result < 0) {
        // Send failed — drop the pending entry and report connection_lost.
        std::function<void(const MoonrakerError&)> cb_copy;
        {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                cb_copy = it->second.error_cb;
                pending_.erase(it);
            }
        }
        if (cb_copy) {
            try {
                cb_copy(MoonrakerError::connection_lost(method));
            } catch (...) {
            }
        }
        return INVALID_REQUEST_ID;
    }
    return id;
}

int EspMoonrakerClient::gcode_script(const std::string& gcode) {
    json params = {{"script", gcode}};
    int result = send_jsonrpc("printer.gcode.script", params);
    return result < 0 ? result : 0;
}

void EspMoonrakerClient::get_gcode_store(
    int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
    std::function<void(const MoonrakerError&)> on_error) {
    json params = {{"count", count}};
    send_jsonrpc(
        "server.gcode_store", params,
        [on_success](const json& response) {
            std::vector<GcodeStoreEntry> entries;
            if (response.contains("result") && response["result"].contains("gcode_store")) {
                const auto& store = response["result"]["gcode_store"];
                entries.reserve(store.size());
                for (const auto& item : store) {
                    GcodeStoreEntry entry;
                    entry.message = item.value("message", "");
                    entry.time = item.value("time", 0.0);
                    entry.type = item.value("type", "response");
                    entries.push_back(std::move(entry));
                }
            }
            if (on_success) {
                on_success(entries);
            }
        },
        on_error);
}

void EspMoonrakerClient::get_temperature_store(
    std::function<void(const TemperatureStore&)> on_success,
    std::function<void(const MoonrakerError&)> on_error) {
    json params = {{"include_monitors", false}};
    send_jsonrpc(
        "server.temperature_store", params,
        [on_success](const json& response) {
            TemperatureStore store;
            if (response.contains("result") && response["result"].is_object()) {
                for (const auto& [key, series_json] : response["result"].items()) {
                    if (!series_json.is_object()) {
                        continue;
                    }
                    TemperatureStoreSeries series;
                    auto load = [&series_json](const char* field, std::vector<float>& out) {
                        if (series_json.contains(field) && series_json[field].is_array()) {
                            for (const auto& v : series_json[field]) {
                                if (v.is_number()) {
                                    out.push_back(v.get<float>());
                                }
                            }
                        }
                    };
                    load("temperatures", series.temperatures);
                    load("targets", series.targets);
                    load("powers", series.powers);
                    store.emplace(key, std::move(series));
                }
            }
            if (on_success) {
                on_success(store);
            }
        },
        on_error);
}

// ---------------------------------------------------------------------------
// Discovery
//
// A trimmed reimplementation of the desktop MoonrakerDiscoverySequence chain
// (src/api/moonraker_discovery_sequence.cpp) over the client's own send_jsonrpc.
// The desktop sequence is NOT reused: it is hard-coupled to the concrete
// helix::MoonrakerClient (holds MoonrakerClient&, calls connection_generation()
// + emit_event(), and makes a sync libhv webcam probe) and is deliberately
// excluded from the ESP image (helixapp/app_srcs.txt). The one portable piece —
// PrinterDiscovery::parse_objects() — is reused as-is.
//
// v1 (Core+AMS) drops the desktop steps that only feed the About screen / webcam
// / power UI: 2nd server.info (Moonraker version, Spoolman, webcam), printer.info
// hostname, machine.system_info OS/arch, MCU last_stats queries, and
// machine.device_power / server.sensors probes.
// ---------------------------------------------------------------------------

namespace {

// Pure subscription-payload builder over a discovered PrinterDiscovery snapshot.
// Mirrors the object-class selection of the desktop
// MoonrakerDiscoverySequence::build_subscription_objects, trimmed to the v1
// Core+AMS cut (no MCU last_stats / webcam / power / spoolman entries). Each
// object's field list is narrowed to what the HelixScreen parsers actually read,
// so Klipper does not stream every internal field on each motion step. Depends
// only on PrinterDiscovery accessors + nlohmann::json — host-testable in
// isolation.
json build_subscription_objects(const PrinterDiscovery& hw) {
    json objs;

    // --- Core objects (always subscribed) ---
    objs["print_stats"] = json::array({"state", "filename", "filament_used", "print_duration",
                                       "total_duration", "estimated_time", "info", "message"});
    objs["virtual_sdcard"] = json::array({"progress", "layer", "layer_count", "is_active"});
    objs["toolhead"] = json::array({"position", "homed_axes", "kinematics", "extruder",
                                    "max_velocity", "axis_minimum", "axis_maximum"});
    objs["gcode_move"] =
        json::array({"gcode_position", "speed", "speed_factor", "extrude_factor", "homing_origin"});
    objs["motion_report"] = json::array({"live_extruder_velocity"});
    objs["display_status"] = json::array({"message", "progress"});
    objs["webhooks"] = json::array({"state", "state_message"});
    objs["pause_resume"] = json::array({"is_paused"});
    objs["bed_mesh"] = json::array(
        {"profile_name", "probed_matrix", "mesh_min", "mesh_max", "mesh_params", "profiles"});
    objs["exclude_object"] = json::array({"objects", "excluded_objects", "current_object"});
    objs["manual_probe"] = json::array({"is_active", "z_position"});
    objs["stepper_enable"] = json::array({"steppers"});
    objs["idle_timeout"] = json::array({"state"});

    // --- Heaters (extruders, heater_bed, heater_generic) ---
    static const json heater_fields = json::array({"temperature", "target"});
    for (const auto& h : hw.heaters()) {
        objs[h] = heater_fields;
    }

    // --- Sensors (temperature_sensor, temperature_fan, tmc*). temperature_fan is
    // overwritten in the fans loop below with the union of its fields. ---
    static const json sensor_fields = json::array({"temperature", "humidity"});
    for (const auto& s : hw.sensors()) {
        objs[s] = sensor_fields;
    }

    // --- Fans. Field shape varies by object type. ---
    static const json fan_speed_fields = json::array({"speed"});
    static const json temp_fan_fields = json::array({"temperature", "target", "speed"});
    static const json output_pin_value_fields = json::array({"value"});
    for (const auto& f : hw.fans()) {
        if (f.rfind("temperature_fan ", 0) == 0) {
            objs[f] = temp_fan_fields;
        } else if (f.rfind("output_pin ", 0) == 0) {
            objs[f] = output_pin_value_fields;
        } else {
            objs[f] = fan_speed_fields;
        }
    }
    if (hw.has_fan_feedback()) {
        objs["fan_feedback"] =
            json::array({"fan0_speed", "fan1_speed", "fan2_speed", "fan3_speed", "fan4_speed",
                         "fan5_speed", "fan6_speed", "fan7_speed", "fan8_speed", "fan9_speed"});
    }

    // --- LEDs + led_effect ---
    static const json led_color_fields = json::array({"color_data"});
    for (const auto& l : hw.leds()) {
        if (l.rfind("output_pin ", 0) == 0) {
            objs[l] = output_pin_value_fields;
        } else {
            objs[l] = led_color_fields;
        }
    }
    for (const auto& e : hw.led_effects()) {
        objs[e] = json::array({"enabled"});
    }

    // --- Firmware retraction ---
    if (hw.has_firmware_retraction()) {
        objs["firmware_retraction"] = json::array(
            {"retract_length", "retract_speed", "unretract_extra_length", "unretract_speed"});
    }

    // --- Filament sensors ---
    static const json filament_sensor_fields =
        json::array({"filament_detected", "enabled", "detection_count"});
    for (const auto& s : hw.filament_sensor_names()) {
        objs[s] = filament_sensor_fields;
    }

    // --- Width sensors ---
    if (hw.has_width_sensors()) {
        static const json width_fields = json::array({"Diameter", "Raw"});
        for (const auto& s : hw.width_sensor_objects()) {
            objs[s] = width_fields;
        }
    }

    // --- Print-start detection macros ---
    objs["gcode_macro _START_PRINT"] = json::array({"print_started"});
    objs["gcode_macro START_PRINT"] = json::array({"preparation_done"});
    objs["gcode_macro _HELIX_STATE"] = json::array({"print_started"});

    // --- AMS / filament systems (v1 Core+AMS) ---
    if (hw.has_mmu()) {
        // Happy Hare mmu object — narrowed to the fields the AMS backends read
        // (nullptr would flood notifications, #388).
        objs["mmu"] = json::array({"gate",
                                   "tool",
                                   "filament",
                                   "action",
                                   "reason_for_pause",
                                   "filament_pos",
                                   "gate_status",
                                   "gate_color_rgb",
                                   "gate_color",
                                   "gate_material",
                                   "gate_name",
                                   "gate_filament_name",
                                   "gate_spool_id",
                                   "gate_temperature",
                                   "has_bypass",
                                   "num_units",
                                   "num_gates",
                                   "unit_gate_counts",
                                   "unit",
                                   "ttg_map",
                                   "endless_spool_groups",
                                   "sensors",
                                   "bowden_progress",
                                   "clog_detection_enabled",
                                   "encoder",
                                   "flowguard",
                                   "drying_state",
                                   "sync_feedback_state",
                                   "sync_feedback_bias_modelled",
                                   "sync_feedback_bias_raw",
                                   "sync_feedback_flow_rate",
                                   "sync_drive",
                                   "spoolman_support",
                                   "pending_spool_id",
                                   "espooler_active",
                                   "num_toolchanges",
                                   "slicer_tool_map",
                                   "toolchange_purge_volume",
                                   "leds"});
    }

    // AFC objects come from the raw printer-object list — the typed PrinterDiscovery
    // accessors don't expose the full AFC_* set. parse_objects() populates
    // printer_objects() before this runs. Field lists mirror the desktop
    // AmsBackendAfc parsers.
    static const json afc_state_fields = json::array({"connected",
                                                      "bypass_state",
                                                      "quiet_mode",
                                                      "current_load",
                                                      "current_lane",
                                                      "current_state",
                                                      "current_tool",
                                                      "current_toolchange",
                                                      "error_state",
                                                      "filament_loaded",
                                                      "lane_loaded",
                                                      "led_state",
                                                      "message",
                                                      "name",
                                                      "number_of_toolchanges",
                                                      "num_extruders",
                                                      "status",
                                                      "system",
                                                      "tool_sensor_after_extruder",
                                                      "tool_stn",
                                                      "tool_stn_unload",
                                                      "type",
                                                      "units",
                                                      "lanes",
                                                      "hubs",
                                                      "extruders",
                                                      "buffers"});
    static const json afc_stepper_fields =
        json::array({"buffer_status", "color", "dist_hub", "extruder", "filament_status", "hub",
                     "load", "loaded_to_hub", "map", "material", "prep", "runout_lane", "spool_id",
                     "status", "tool_loaded", "weight"});
    static const json afc_hub_fields = json::array({"state", "afc_bowden_length"});
    static const json afc_buffer_fields = json::array(
        {"state", "distance_to_fault", "error_sensitivity", "fault_detection_enabled", "lanes"});
    static const json afc_extruder_fields =
        json::array({"lane_loaded", "tool_end_status", "tool_start_status"});
    static const json afc_unit_fields = json::array({"lanes", "extruders", "hubs", "buffers"});
    for (const auto& o : hw.printer_objects()) {
        if (o == "AFC" || o == "afc") {
            objs[o] = afc_state_fields;
        } else if (o.rfind("AFC_stepper ", 0) == 0 || o.rfind("AFC_lane ", 0) == 0) {
            objs[o] = afc_stepper_fields;
        } else if (o.rfind("AFC_hub ", 0) == 0) {
            objs[o] = afc_hub_fields;
        } else if (o.rfind("AFC_buffer ", 0) == 0) {
            objs[o] = afc_buffer_fields;
        } else if (o.rfind("AFC_extruder ", 0) == 0) {
            objs[o] = afc_extruder_fields;
        } else if (o.rfind("AFC_led ", 0) == 0) {
            continue; // never parsed by HelixScreen
        } else if (o.rfind("AFC_", 0) == 0) {
            objs[o] = afc_unit_fields; // unit-level object (BoxTurtle/OpenAMS/ViViD/...)
        }
    }

    // Backend-specific companion objects (nullptr = all fields; these are small).
    switch (hw.mmu_type()) {
    case AmsType::AD5X_IFS:
        objs["save_variables"] = nullptr;
        break;
    case AmsType::ACE:
        objs["ace"] = nullptr;
        break;
    case AmsType::CFS:
        objs["box"] = nullptr;
        objs["motor_control"] = nullptr;
        break;
    case AmsType::SNAPMAKER:
        objs["filament_detect"] = nullptr;
        objs["filament_feed left"] = nullptr;
        objs["filament_feed right"] = nullptr;
        objs["print_task_config"] = nullptr;
        objs["machine_state_manager"] = nullptr;
        for (int i = 0; i < 4; ++i) {
            objs[std::string("filament_motion_sensor e") + std::to_string(i) + "_filament"] =
                nullptr;
        }
        break;
    case AmsType::QIDI_BOX:
        objs["box_extras"] = nullptr;
        objs["save_variables"] = nullptr;
        break;
    default:
        break;
    }

    // --- Toolchanger + per-tool objects ---
    if (hw.has_tool_changer()) {
        objs["toolchanger"] = json::array({"status", "tool_number", "tool_numbers"});
        static const json tool_fields =
            json::array({"active", "mounted", "detect_state", "gcode_x_offset", "gcode_y_offset",
                         "gcode_z_offset", "extruder", "fan"});
        for (const auto& t : hw.tool_names()) {
            objs["tool " + t] = tool_fields;
        }
    }

    return objs;
}

} // namespace

void EspMoonrakerClient::discover_printer(std::function<void()> on_complete,
                                          std::function<void(const std::string&)> on_error) {
    if (!is_connected()) {
        if (on_error) {
            on_error("not connected");
        }
        return;
    }

    // Re-entrancy: discovery_in_flight_ collapses a second discover_printer()
    // while one is running for THIS generation — cleared on disconnect
    // (on_ws_disconnected) and at chain end, and force-cleared whenever a new
    // connection attempt begins (connect(), force_reconnect(), the
    // auto-reconnect executor), so it can't stick true across a generation
    // change even if the old chain never reaches a terminal callback.
    if (discovery_in_flight_.exchange(true)) {
        spdlog::debug("[helixnet] discover_printer: already in flight, ignoring re-entrant call");
        return;
    }

    // R3: snapshot the generation this chain belongs to. Every async
    // continuation below re-checks it before touching hardware_,
    // discovery_in_flight_, or the user callbacks — a reconnect landing
    // mid-chain bumps connection_generation_, which makes every subsequent
    // callback in THIS chain a no-op instead of applying stale results or
    // double-running discovery for the new connection.
    const uint64_t generation = connection_generation_.load();

    // Share the user callbacks across the nested chain; every terminal path clears
    // discovery_in_flight_ exactly once (discovery_fail, or the subscribe success).
    auto done = std::make_shared<std::function<void()>>(std::move(on_complete));
    auto fail = std::make_shared<std::function<void(const std::string&)>>(std::move(on_error));

    json identify_params = {{"client_name", "HelixScreen"},
                            {"version", HELIX_VERSION},
                            {"type", "display"},
                            {"url", "https://github.com/prestonbrown/helixscreen"}};

    // Step a — server.connection.identify (best-effort; older Moonraker may lack
    // it). Either outcome continues to the Klippy-readiness gate.
    send_jsonrpc(
        "server.connection.identify", identify_params,
        [this, done, fail, generation](const json& resp) {
            if (helix::is_stale_generation(generation, connection_generation_.load())) {
                return;
            }
            if (resp.contains("result")) {
                spdlog::info("[helixnet] identified to Moonraker (connection_id: {})",
                             resp["result"].value("connection_id", 0));
            }
            discovery_gate_klippy(done, fail, generation);
        },
        [this, done, fail, generation](const MoonrakerError& err) {
            if (helix::is_stale_generation(generation, connection_generation_.load())) {
                return;
            }
            spdlog::warn("[helixnet] identify failed (continuing): {}", err.message);
            discovery_gate_klippy(done, fail, generation);
        });
}

void EspMoonrakerClient::discovery_gate_klippy(DiscoveryDone done, DiscoveryFail fail,
                                               uint64_t generation) {
    // Step b — server.info Klippy-readiness gate. printer.objects.list returns
    // JSON-RPC -32601 while Klippy is in STARTUP; gate here so we defer (retryable)
    // instead of surfacing a confusing error.
    send_jsonrpc(
        "server.info", json(),
        [this, done, fail, generation](const json& resp) {
            if (helix::is_stale_generation(generation, connection_generation_.load())) {
                return;
            }
            std::string klippy_state = "unknown";
            if (resp.contains("result") && resp["result"].contains("klippy_state") &&
                resp["result"]["klippy_state"].is_string()) {
                klippy_state = resp["result"]["klippy_state"].get<std::string>();
            }
            spdlog::debug("[helixnet] Klippy state gate: {}", klippy_state);
            // "ready" and "shutdown" both expose valid Klipper objects; anything
            // else (startup/error/unknown) defers.
            if (klippy_state != "ready" && klippy_state != "shutdown") {
                std::string reason = "Klippy not ready (state: " + klippy_state + ")";
                spdlog::warn("[helixnet] {}", reason);
                discovery_fail(fail, MoonrakerEventType::DISCOVERY_DEFERRED, reason, generation);
                return;
            }
            discovery_query_objects(std::move(done), std::move(fail), generation);
        },
        [this, done, fail, generation](const MoonrakerError& err) {
            if (helix::is_stale_generation(generation, connection_generation_.load())) {
                return;
            }
            spdlog::error("[helixnet] server.info failed: {}", err.message);
            discovery_fail(fail, MoonrakerEventType::DISCOVERY_FAILED, err.message, generation);
        });
}

void EspMoonrakerClient::discovery_query_objects(DiscoveryDone done, DiscoveryFail fail,
                                                 uint64_t generation) {
    // Step c — printer.objects.list → parse_objects() → fire on_hardware_discovered_.
    // silent=true suppresses the error toast if Klippy vanished between the gate and
    // this call.
    send_jsonrpc(
        "printer.objects.list", json(),
        [this, done, fail, generation](const json& resp) {
            if (helix::is_stale_generation(generation, connection_generation_.load())) {
                return;
            }
            if (!resp.contains("result") || !resp["result"].contains("objects")) {
                std::string reason = "Failed to query printer objects";
                if (resp.contains("error") && resp["error"].contains("message") &&
                    resp["error"]["message"].is_string()) {
                    reason = resp["error"]["message"].get<std::string>();
                }
                spdlog::error("[helixnet] printer.objects.list failed: {}", reason);
                discovery_fail(fail, MoonrakerEventType::DISCOVERY_FAILED, reason, generation);
                return;
            }

            parse_objects(resp["result"]["objects"]); // locks hardware_mutex_

            // Snapshot for the early hardware-discovered callback (AMS/MMU backends
            // init before the subscribe response arrives). Copy under lock (#562,
            // #777) — the callback runs outside the lock.
            std::function<void(const helix::PrinterDiscovery&)> hw_cb;
            {
                std::lock_guard<std::mutex> lock(callbacks_mutex_);
                hw_cb = on_hardware_discovered_;
            }
            PrinterDiscovery snapshot;
            {
                std::lock_guard<std::mutex> lock(hardware_mutex_);
                snapshot = hardware_;
            }
            spdlog::info("[helixnet] discovered {} heaters, {} sensors, {} fans, {} leds, {} "
                         "filament sensors",
                         snapshot.heaters().size(), snapshot.sensors().size(),
                         snapshot.fans().size(), snapshot.leds().size(),
                         snapshot.filament_sensor_names().size());
            if (hw_cb) {
                try {
                    hw_cb(snapshot);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "on_hardware_discovered threw: %s", e.what());
                } catch (...) {
                }
            }

            discovery_subscribe(std::move(done), std::move(fail), generation);
        },
        [this, done, fail, generation](const MoonrakerError& err) {
            if (helix::is_stale_generation(generation, connection_generation_.load())) {
                return;
            }
            spdlog::error("[helixnet] printer.objects.list request failed: {}", err.message);
            discovery_fail(fail, MoonrakerEventType::DISCOVERY_FAILED, err.message, generation);
        },
        0,     // default timeout
        true); // silent
}

void EspMoonrakerClient::discovery_subscribe(DiscoveryDone done, DiscoveryFail fail,
                                             uint64_t generation) {
    // Step d — printer.objects.subscribe. Build the payload from the discovered
    // snapshot, then fire on_discovery_complete_(hardware, initial_status) with the
    // status returned in the subscribe response, and finally on_complete().
    PrinterDiscovery snapshot;
    {
        std::lock_guard<std::mutex> lock(hardware_mutex_);
        snapshot = hardware_;
    }
    json subscription_objects = build_subscription_objects(snapshot);
    json params = {{"objects", subscription_objects}};
    size_t num = subscription_objects.size();

    send_jsonrpc(
        "printer.objects.subscribe", params,
        [this, done, fail, num, generation](const json& resp) {
            if (helix::is_stale_generation(generation, connection_generation_.load())) {
                return;
            }
            if (resp.contains("result")) {
                spdlog::info("[helixnet] subscription complete: {} objects subscribed", num);
            } else if (resp.contains("error")) {
                // Subscribe returned an error object but the request itself
                // succeeded — desktop treats this as non-fatal; discovery still
                // completes so the UI can come up.
                spdlog::error("[helixnet] subscribe returned error: {}", resp["error"].dump());
                emit_event(MoonrakerEventType::DISCOVERY_FAILED,
                           "Failed to subscribe to printer updates", false);
            }

            json initial_status;
            if (resp.contains("result") && resp["result"].contains("status")) {
                initial_status = resp["result"]["status"];
            }

            std::function<void(const helix::PrinterDiscovery&, const json&)> done_cb;
            {
                std::lock_guard<std::mutex> lock(callbacks_mutex_);
                done_cb = on_discovery_complete_;
            }
            PrinterDiscovery snap;
            {
                std::lock_guard<std::mutex> lock(hardware_mutex_);
                snap = hardware_;
            }
            if (done_cb) {
                try {
                    done_cb(snap, initial_status);
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "on_discovery_complete threw: %s", e.what());
                } catch (...) {
                }
            }

            // Permanent telemetry, not scaffolding: uxTaskGetStackHighWaterMark
            // reports the MINIMUM free stack space this (this reconnect
            // cycle's freshly created — see connect()'s F8 comment) websocket
            // task has ever had since it started, in bytes. Logged here
            // because discovery is the heaviest, most deeply-nested json
            // work this task does per cycle (identify/gate/query_objects/
            // subscribe, ending here). If this ever reports < 8192 the old
            // (pre-fix) budget would have overflowed — confirming the
            // heap-corruption soak hypothesis retroactively; if it stays
            // comfortably high across churn, that hypothesis is wrong and
            // this line is the tripwire evidence for whatever the hunt
            // turns to next.
            spdlog::info("[helixnet] ws task stack watermark: {} bytes free of {}",
                         uxTaskGetStackHighWaterMark(nullptr), WS_TASK_STACK_BYTES);

            discovery_in_flight_.store(false);
            if (*done) {
                try {
                    (*done)();
                } catch (...) {
                }
            }
        },
        [this, done, fail, generation](const MoonrakerError& err) {
            if (helix::is_stale_generation(generation, connection_generation_.load())) {
                return;
            }
            spdlog::error("[helixnet] subscribe request failed: {}", err.message);
            discovery_fail(fail, MoonrakerEventType::DISCOVERY_FAILED, err.message, generation);
        });
}

void EspMoonrakerClient::discovery_fail(const DiscoveryFail& fail, MoonrakerEventType ev,
                                        const std::string& reason, uint64_t generation) {
    // Defense in depth: every call site above already checks staleness before
    // calling in, but discovery_fail is the one place discovery_in_flight_
    // gets cleared on the failure path, so re-check here too (R3) — a stale
    // caller must not clear the flag on behalf of a newer generation's chain.
    if (helix::is_stale_generation(generation, connection_generation_.load())) {
        return;
    }
    emit_event(ev, reason, true);
    discovery_in_flight_.store(false);
    if (fail && *fail) {
        try {
            (*fail)(reason);
        } catch (...) {
        }
    }
}

PrinterDiscovery EspMoonrakerClient::hardware() const {
    std::lock_guard<std::mutex> lock(hardware_mutex_);
    return hardware_;
}

void EspMoonrakerClient::parse_objects(const json& objects) {
    std::lock_guard<std::mutex> lock(hardware_mutex_);
    hardware_.parse_objects(objects);
    // parse_objects() clears printer_objects_, so repopulate the raw name list
    // AFTER it — build_subscription_objects derives AFC/unit objects from it.
    std::vector<std::string> names;
    if (objects.is_array()) {
        names.reserve(objects.size());
        for (const auto& o : objects) {
            if (o.is_string()) {
                names.push_back(o.get<std::string>());
            }
        }
    }
    hardware_.set_printer_objects(names);
}

void EspMoonrakerClient::clear_discovery_cache() {
    std::lock_guard<std::mutex> lock(hardware_mutex_);
    hardware_ = PrinterDiscovery{};
}

void EspMoonrakerClient::set_on_hardware_discovered(
    std::function<void(const helix::PrinterDiscovery&)> cb) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_hardware_discovered_ = std::move(cb);
}

void EspMoonrakerClient::set_on_discovery_complete(
    std::function<void(const helix::PrinterDiscovery&, const json&)> cb) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_discovery_complete_ = std::move(cb);
}

void EspMoonrakerClient::set_bed_mesh_callback(std::function<void(const json&)> callback) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    bed_mesh_callback_ = std::move(callback);
}

// ---------------------------------------------------------------------------
// Subscriptions & method callbacks
// ---------------------------------------------------------------------------

SubscriptionId EspMoonrakerClient::register_notify_update(std::function<void(const json&)> cb) {
    if (!cb) {
        return INVALID_SUBSCRIPTION_ID;
    }
    SubscriptionId id = next_subscription_id_.fetch_add(1) + 1;
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    notify_callbacks_.emplace(id, std::move(cb));
    return id;
}

bool EspMoonrakerClient::unsubscribe_notify_update(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    return notify_callbacks_.erase(id) > 0;
}

void EspMoonrakerClient::register_method_callback(const std::string& method,
                                                  const std::string& handler_name,
                                                  std::function<void(const json&)> cb) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    method_callbacks_[method][handler_name] = std::move(cb);
}

bool EspMoonrakerClient::unregister_method_callback(const std::string& method,
                                                    const std::string& handler_name) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    auto it = method_callbacks_.find(method);
    if (it == method_callbacks_.end()) {
        return false;
    }
    bool removed = it->second.erase(handler_name) > 0;
    if (it->second.empty()) {
        method_callbacks_.erase(it);
    }
    return removed;
}

void EspMoonrakerClient::dispatch_status_update(const json& status, bool from_cached_snapshot) {
    // Wrap raw status into the notify_status_update envelope and route it through
    // the same fan-out an incoming WS notification would take. [status, eventtime].
    json wrapped;
    wrapped["method"] = "notify_status_update";
    wrapped["params"] = json::array({status, 0.0});
    if (from_cached_snapshot) {
        wrapped[CACHED_SNAPSHOT_MARKER] = true;
    }
    // Notify-only fan-out (no method_callbacks_), matching desktop
    // dispatch_status_update semantics.
    dispatch_notification(wrapped, /*include_method_callbacks=*/false);
}

// ---------------------------------------------------------------------------
// Observers & reconnection
// ---------------------------------------------------------------------------

void EspMoonrakerClient::add_connected_observer(const std::string& handler_name,
                                                std::function<void()> cb) {
    bool fire_now = false;
    std::function<void()> immediate;
    {
        std::lock_guard<std::mutex> lock(observers_mutex_);
        connected_observers_[handler_name] = cb;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == ConnectionState::CONNECTED) {
            fire_now = true;
            immediate = cb;
        }
    }
    if (fire_now && immediate) {
        try {
            immediate();
        } catch (...) {
        }
    }
}

bool EspMoonrakerClient::remove_connected_observer(const std::string& handler_name) {
    std::lock_guard<std::mutex> lock(observers_mutex_);
    return connected_observers_.erase(handler_name) > 0;
}

void EspMoonrakerClient::set_auto_reconnect(bool enabled) {
    auto_reconnect_.store(enabled);
    if (enabled) {
        // Re-arm: restore the exponential-backoff floor for the next
        // disconnect. No transport call needed — reconnection is entirely
        // driven by our own reconnect_pending_/deadline state (F8), not the
        // esp_websocket_client component's built-in reconnect (disabled at
        // connect() time). This just lets a future on_ws_disconnected() arm
        // the next intent.
        next_reconnect_delay_ms_ = reconnect_min_delay_ms_;
    } else {
        // Suspend background reconnection WITHOUT dropping the current
        // connection (mirrors desktop setReconnect(nullptr)). Cancel anything
        // already scheduled by a prior disconnect — suspending must win over
        // an intent recorded before the caller asked to suspend it.
        reconnect_pending_.store(false);
    }
}

void EspMoonrakerClient::force_reconnect() {
    if (!ws_) {
        return;
    }
    // F5 ordering: disarm before the stop inside execute_reconnect() so a
    // DISCONNECTED event it emits can't schedule a redundant auto-reconnect
    // intent on top of this manual one; drop anything already scheduled too.
    auto_reconnect_.store(false);
    reconnect_pending_.store(false);

    // A manual reconnect restarts the ladder from the shortest delay.
    next_reconnect_delay_ms_ = reconnect_min_delay_ms_;

    set_state(ConnectionState::CONNECTING);
    execute_reconnect();
}

// ---------------------------------------------------------------------------
// Events & modal suppression
// ---------------------------------------------------------------------------

void EspMoonrakerClient::register_event_handler(MoonrakerEventCallback cb) {
    std::lock_guard<std::mutex> lock(event_mutex_);
    event_handler_ = std::move(cb);
}

void EspMoonrakerClient::suppress_disconnect_modal(uint32_t duration_ms) {
    suppress_modal_until_us_.store(now_us() + static_cast<int64_t>(duration_ms) * 1000);
}

bool EspMoonrakerClient::is_disconnect_modal_suppressed() const {
    return now_us() < suppress_modal_until_us_.load();
}

// ---------------------------------------------------------------------------
// Request management & configuration
// ---------------------------------------------------------------------------

bool EspMoonrakerClient::cancel_request(RequestId id) {
    if (id == INVALID_REQUEST_ID) {
        return false;
    }
    std::lock_guard<std::mutex> lock(requests_mutex_);
    return pending_.erase(id) > 0;
}

void EspMoonrakerClient::set_state_change_callback(
    std::function<void(ConnectionState, ConnectionState)> cb) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_change_cb_ = std::move(cb);
}

void EspMoonrakerClient::set_connection_timeout(uint32_t timeout_ms) {
    connection_timeout_ms_ = timeout_ms;
}

void EspMoonrakerClient::set_default_request_timeout(uint32_t timeout_ms) {
    default_request_timeout_ms_ = timeout_ms;
}

void EspMoonrakerClient::configure_timeouts(uint32_t connection_timeout_ms,
                                            uint32_t request_timeout_ms,
                                            uint32_t /*keepalive_interval_ms*/,
                                            uint32_t reconnect_min_delay_ms,
                                            uint32_t reconnect_max_delay_ms) {
    connection_timeout_ms_ = connection_timeout_ms;
    default_request_timeout_ms_ = request_timeout_ms;
    reconnect_min_delay_ms_ = static_cast<int>(reconnect_min_delay_ms);
    reconnect_max_delay_ms_ = static_cast<int>(reconnect_max_delay_ms);
    next_reconnect_delay_ms_ = reconnect_min_delay_ms_;
}

void EspMoonrakerClient::toggle_filament_runout_simulation() {
    // Simulation hook is a no-op in production (mirrors desktop).
}

std::weak_ptr<bool> EspMoonrakerClient::lifetime_weak() const {
    return lifetime_;
}

// ---------------------------------------------------------------------------
// Platform factory
// ---------------------------------------------------------------------------

std::unique_ptr<IMoonrakerClient> create_platform_moonraker_client() {
    return std::make_unique<EspMoonrakerClient>();
}

} // namespace helix

// Force-link probe: keeps the client in the image so the size gate accounts for
// its real cost even before Task 10 wires it into app_main. Never executed.
extern "C" void helixnet_link_probe(void) {
    auto client = helix::create_platform_moonraker_client();
    (void)client;
}
