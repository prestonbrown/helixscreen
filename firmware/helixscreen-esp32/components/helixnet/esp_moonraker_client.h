// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// helixnet — the ESP32 implementation of helix::IMoonrakerClient over
// esp_websocket_client. Mirrors the desktop MoonrakerClient's consumer contract
// (src/api/moonraker_client.cpp) but replaces libhv with the ESP-IDF managed
// esp_websocket_client component. See docs task-9-brief for the full contract.
//
// Threading model: esp_websocket_client owns a FreeRTOS task ("websocket_task").
// All WEBSOCKET_EVENT_* callbacks, and therefore message dispatch and every user
// callback we invoke, run ON that task. This IS the "background thread" contract
// desktop consumers expect: they hop to the LVGL main thread via ui_queue_update
// exactly as they do behind libhv. The request tracker and callback maps are
// therefore mutex-guarded with two-phase locking (copy the callback out under the
// lock, invoke it outside).

#pragma once

#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "i_moonraker_client.h"
#include "reconnect_backoff.h"
#include "rpc_error_policy.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace helix {

class EspMoonrakerClient final : public IMoonrakerClient {
  public:
    EspMoonrakerClient();
    ~EspMoonrakerClient() override;

    EspMoonrakerClient(const EspMoonrakerClient&) = delete;
    EspMoonrakerClient& operator=(const EspMoonrakerClient&) = delete;

    // --- Connection lifecycle ---
    int connect(const char* url, std::function<void()> on_connected,
                std::function<void()> on_disconnected) override;
    void disconnect() override;
    const std::string& get_last_url() const override {
        return url_;
    }
    void set_auto_reconnect(bool enabled) override;

    // --- JSON-RPC protocol ---
    int send_jsonrpc(const std::string& method) override;
    int send_jsonrpc(const std::string& method, const json& params) override;
    RequestId send_jsonrpc(const std::string& method, const json& params,
                           std::function<void(const json&)> cb) override;
    RequestId
    send_jsonrpc(const std::string& method, const json& params,
                 std::function<void(const json&)> success_cb,
                 std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
                 bool silent = false,
                 std::optional<rpc_error_policy::CallerIntent> intent = std::nullopt) override;
    int gcode_script(const std::string& gcode) override;
    void get_gcode_store(int count,
                         std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                         std::function<void(const MoonrakerError&)> on_error) override;
    void get_temperature_store(std::function<void(const TemperatureStore&)> on_success,
                               std::function<void(const MoonrakerError&)> on_error) override;

    // --- Discovery (Plan 4 fills the real sequence) ---
    void
    discover_printer(std::function<void()> on_complete,
                     std::function<void(const std::string& reason)> on_error = nullptr) override;
    PrinterDiscovery hardware() const override;
    void parse_objects(const json& objects) override;
    void clear_discovery_cache() override;
    void
    set_on_hardware_discovered(std::function<void(const helix::PrinterDiscovery&)> cb) override;
    void set_on_discovery_complete(
        std::function<void(const helix::PrinterDiscovery&, const json& initial_status)> cb)
        override;
    void set_bed_mesh_callback(std::function<void(const json&)> callback) override;

    // --- Subscriptions & method callbacks ---
    SubscriptionId register_notify_update(std::function<void(const json&)> cb) override;
    bool unsubscribe_notify_update(SubscriptionId id) override;
    void register_method_callback(const std::string& method, const std::string& handler_name,
                                  std::function<void(const json&)> cb) override;
    bool unregister_method_callback(const std::string& method,
                                    const std::string& handler_name) override;
    void dispatch_status_update(const json& status, bool from_cached_snapshot = false) override;

    // --- Connection state & observers ---
    ConnectionState get_connection_state() const override;
    void add_connected_observer(const std::string& handler_name, std::function<void()> cb) override;
    bool remove_connected_observer(const std::string& handler_name) override;
    void force_reconnect() override;

    // --- Events & modal suppression ---
    void register_event_handler(MoonrakerEventCallback cb) override;
    void suppress_disconnect_modal(uint32_t duration_ms = 10000) override;
    bool is_disconnect_modal_suppressed() const override;

    // --- Request management ---
    bool cancel_request(RequestId id) override;

    // --- Owner wiring & configuration ---
    void
    set_state_change_callback(std::function<void(ConnectionState, ConnectionState)> cb) override;
    void set_connection_timeout(uint32_t timeout_ms) override;
    void set_default_request_timeout(uint32_t timeout_ms) override;
    void configure_timeouts(uint32_t connection_timeout_ms, uint32_t request_timeout_ms,
                            uint32_t keepalive_interval_ms, uint32_t reconnect_min_delay_ms,
                            uint32_t reconnect_max_delay_ms) override;
    void process_timeouts() override;

    // --- Simulation hooks ---
    void toggle_filament_runout_simulation() override;

    // --- Lifetime guard ---
    std::weak_ptr<bool> lifetime_weak() const override;

  private:
    // Reassembly cap: a single WS message larger than this is dropped whole.
    // Moonraker does not chunk at the protocol level, so an oversized response's
    // RPC will simply time out (see brief). 256 KiB.
    static constexpr size_t MAX_MESSAGE_BYTES = 262144;
    // Own request-tracker cap — far below desktop's 500 (RAM-bound). On overflow
    // the error callback fires synchronously with a CONNECTION_LOST error.
    static constexpr size_t MAX_PENDING_REQUESTS = 64;
    static constexpr uint32_t DEFAULT_REQUEST_TIMEOUT_MS = 60000;
    // The two bounds below both exist for one reason: no transport call may
    // stall the LVGL thread long enough for the screen to look dead. The task
    // watchdog is NOT the constraint — it watches the idle tasks only and
    // CONFIG_ESP_TASK_WDT_PANIC is off, so it neither fires for a stalled LVGL
    // thread nor reboots if it did. The whole cost is a frozen screen. 3s is a
    // UX judgement: long enough that an ordinary LAN operation never trips it,
    // short enough that a tap on an unreachable printer reads as slow rather
    // than crashed.
    static constexpr uint32_t UI_STALL_BUDGET_MS = 3000;

    // How long esp_websocket_client_send_text() may block the CALLING task
    // waiting for the transport lock and the socket write. Every UI event
    // handler that issues a gcode or JSON-RPC reaches this on the LVGL thread.
    // Deliberately NOT connection_timeout_ms_, which
    // MoonrakerManager::configure_timeouts() sets to 10s from
    // moonraker_connection_timeout_ms.
    static constexpr uint32_t SEND_TIMEOUT_MS = UI_STALL_BUDGET_MS;

    // Ceiling applied to cfg.network_timeout_ms in connect(). The websocket
    // task spends that budget inside esp_transport_connect() when the address
    // is unreachable, and esp_websocket_client_stop() waits on STOPPED_BIT with
    // portMAX_DELAY for the task to come back — so a stop() issued from the
    // LVGL thread inherits the connect budget as a freeze. ChangeHostModal is
    // exactly that path: its connect() tears down the previous probe's client
    // while that one is still mid-connect to a bad address, which at the
    // configured 10s left the modal unresponsive for ten seconds.
    //
    // What this cap does and does NOT bound: network_timeout_ms reaches
    // esp-tls as cfg.timeout_ms, which governs the TCP connect and the TLS
    // handshake — i.e. everything AFTER the host is resolved. Resolution
    // itself happens first, in a bare getaddrinfo() that takes no timeout
    // (esp-tls/esp_tls.c:239) and is bounded only by lwIP's own DNS settings.
    // moonraker_host is routinely a hostname (.local mDNS names are the norm
    // on MainsailOS/Fluidd), so a stop() issued while a slow or failing
    // resolution is in progress can still exceed this cap by the DNS time.
    // The cap is a real improvement for the address case, not a guarantee.
    static constexpr uint32_t MAX_NETWORK_TIMEOUT_MS = UI_STALL_BUDGET_MS;
    // How long RECONNECTING persists before the informational FAILED transition.
    static constexpr int64_t RECONNECTING_TO_FAILED_US = 60LL * 1000 * 1000;
    // Period of the owned esp_timer that drives timeout + FAILED bookkeeping.
    static constexpr uint64_t HOUSEKEEPING_PERIOD_US = 5LL * 1000 * 1000;
    // esp_websocket_client's own task stack (connect()'s cfg.task_stack).
    // Named so the discovery-complete watermark log (discovery_subscribe)
    // reports headroom against the SAME value connect() actually configures.
    // Sized from measurement, not guesswork: 44 forced-reconnect cycles
    // showed a deterministic ~6.5KB peak (watermark 10,060 free of a trial
    // 16384), so 8192 carries ~1.7KB headroom — and this stack is INTERNAL
    // DRAM (a 16KB trial cost exactly 8KB of steady-state internal free,
    // breaking the >=100KB budget). The watermark log guards the margin.
    static constexpr uint32_t WS_TASK_STACK_BYTES = 8192;
    // connect()'s cfg.pingpong_timeout_sec (Defect 1, Task 9 confirm soak).
    // Must stay comfortably below DEFAULT_REQUEST_TIMEOUT_MS (60s) so a
    // silently-dead connection is caught by ping/pong first, not by the
    // slower per-request timeout — see connect()'s comment for the soak
    // evidence (5 separate requests across 3 different methods each sat the
    // full 60s with no disconnect ever observed in between).
    static constexpr int PING_PONG_TIMEOUT_SEC = 20;
    static_assert(PING_PONG_TIMEOUT_SEC * 1000u < DEFAULT_REQUEST_TIMEOUT_MS,
                  "ping/pong must detect a dead link before the per-request timeout fires — "
                  "otherwise silent connection death stalls requests for the full request "
                  "timeout with no disconnect in between (Task 9 confirm-soak defect)");

    struct Pending {
        std::string method;
        std::function<void(const json&)> success_cb;
        std::function<void(const MoonrakerError&)> error_cb;
        int64_t sent_us = 0;
        uint32_t timeout_ms = 0;
        bool silent = false;
    };

    // esp_websocket_client event trampoline → instance dispatch.
    static void ws_event_trampoline(void* arg, esp_event_base_t base, int32_t event_id,
                                    void* event_data);
    void on_ws_connected();
    void on_ws_disconnected();
    void on_ws_data(const esp_websocket_event_data_t* data);
    void dispatch_message(const char* buf, size_t len);
    // Fan a notify_status_update-shaped message out to the notify callbacks
    // (copy-then-invoke) and the bed-mesh callback. Shared by the WS dispatch
    // path and dispatch_status_update(). include_method_callbacks also fans out
    // to method_callbacks_ — true only for real inbound WS notifications; the
    // synthetic dispatch_status_update path is notify-only, matching desktop.
    void dispatch_notification(const json& msg, bool include_method_callbacks);

    // esp_timer trampoline → instance housekeeping (timeouts + FAILED).
    static void housekeeping_trampoline(void* arg);

    // Arm a deferred reconnect intent at the current backoff step and advance
    // the ladder for the next one. Shared by on_ws_disconnected() and the
    // failed-start path in execute_reconnect() so both walk the same ladder
    // instead of each carrying its own copy of the backoff math.
    void arm_reconnect_intent();

    // Stop and restart the existing transport handle: THE single execution
    // point for a reconnect. Both the deferred auto-reconnect intent drained by
    // process_timeouts() and the manual force_reconnect() route through it, so
    // a failed start is handled identically in both. Must never run on the
    // websocket task — esp_websocket_client_stop() refuses to stop a client
    // from its own task and returns ESP_FAIL without stopping anything.
    void execute_reconnect();

    void set_state(ConnectionState next);
    void emit_event(MoonrakerEventType type, const std::string& message, bool is_error,
                    const std::string& details = "");

    // Serialize + send a JSON-RPC envelope over the socket. Returns bytes sent
    // (>=0) or negative on failure. Safe to call from any task.
    int send_envelope(const json& envelope);
    RequestId track_and_send(const std::string& method, const json& params,
                             std::function<void(const json&)> success_cb,
                             std::function<void(const MoonrakerError&)> error_cb,
                             uint32_t timeout_ms, bool silent);
    bool is_connected() const;

    // --- Discovery chain (Task 7). discover_printer() is the entry point; each
    // step below runs on the websocket_task and threads the shared user callbacks
    // through. Split out of discover_printer for readability. ---
    using DiscoveryDone = std::shared_ptr<std::function<void()>>;
    using DiscoveryFail = std::shared_ptr<std::function<void(const std::string&)>>;
    // `generation` is the connection_generation_ snapshot captured at
    // discover_printer() entry (R3). Every step re-checks it as the first
    // thing it does; a mismatch means a reconnect landed mid-chain and this
    // continuation abandons in place without touching hardware_,
    // discovery_in_flight_, or the user callbacks.
    void discovery_gate_klippy(DiscoveryDone done, DiscoveryFail fail, uint64_t generation);
    void discovery_query_objects(DiscoveryDone done, DiscoveryFail fail, uint64_t generation);
    void discovery_subscribe(DiscoveryDone done, DiscoveryFail fail, uint64_t generation);
    // Emit `ev`, clear discovery_in_flight_, and invoke the error callback once
    // — but only if `generation` still matches (see above).
    void discovery_fail(const DiscoveryFail& fail, MoonrakerEventType ev, const std::string& reason,
                        uint64_t generation);

    // Written on the LVGL thread (connect()/destructor), read on the
    // ESP_TIMER_TASK housekeeping path. Atomic so the timer-task null checks
    // carry a real happens-before edge on the dual-core S3 — the quiesce in
    // connect() closes the reachable race window, but the esp_timer dispatch
    // handoff (list-unlock before callback entry) leaves a residual sliver
    // where a stale pass can start; it must observe the fresh nullptr.
    std::atomic<esp_websocket_client_handle_t> ws_{nullptr};
    esp_timer_handle_t housekeeping_timer_ = nullptr;
    std::string url_;

    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    // Flipped false in the dtor BEFORE esp_websocket_client_stop() so any event
    // still in flight on the WS task early-outs instead of touching torn-down
    // members. Mirrors desktop's destruction_guard_ ordering.
    std::atomic<bool> alive_{true};
    // Backing store for lifetime_weak(): identical to desktop's shared_ptr<bool>.
    std::shared_ptr<bool> lifetime_ = std::make_shared<bool>(true);
    // Set while a housekeeping tick is executing on the ESP_TIMER_TASK. The dtor
    // spins on this after esp_timer_delete() because that call does not join an
    // in-flight callback (see the dtor comment).
    std::atomic<bool> timer_in_flight_{false};

    mutable std::mutex state_mutex_;
    ConnectionState state_ = ConnectionState::DISCONNECTED;
    std::function<void(ConnectionState, ConnectionState)> state_change_cb_;
    int64_t reconnecting_since_us_ = 0;

    // Manual exponential backoff (the component's own auto-reconnect is fixed 10s).
    int reconnect_min_delay_ms_ = 200;
    int reconnect_max_delay_ms_ = 2000;
    int next_reconnect_delay_ms_ = 200;
    // When false, a disconnect goes straight to DISCONNECTED and background
    // reconnection is suspended (connection-test probe flows). Transient: every
    // connect() re-arms it to true.
    std::atomic<bool> auto_reconnect_{true};

    // F8: on_ws_disconnected() (websocket_task) records reconnect INTENT only —
    // it never calls esp_websocket_client_stop()/start() itself, because that
    // would tear down transport structures from the very task executing the
    // event that's disconnecting them (root cause of the spinlock_acquire
    // assert seen on server-side disconnect, Plan 3 finding F8). The actual
    // stop()/start() executes later from process_timeouts(), which runs on the
    // housekeeping esp_timer (ESP_TIMER_TASK) and the main-thread app_boot_tick
    // pump — never the websocket task.
    std::atomic<bool> reconnect_pending_{false};
    std::atomic<int64_t> reconnect_deadline_us_{0};
    // Generation the pending intent was scheduled against (R3). If a manual
    // connect()/force_reconnect() bumps connection_generation_ before the
    // deadline elapses, process_timeouts() drops the stale intent instead of
    // restarting a connection nothing is waiting on anymore.
    std::atomic<uint64_t> reconnect_generation_{0};

    // R3: bumped on every new connection attempt — connect(), force_reconnect(),
    // and the deferred auto-reconnect executor in process_timeouts(). The
    // discovery chain snapshots this at discover_printer() entry and re-checks
    // it at every async continuation. Mirrors desktop
    // MoonrakerClient::connection_generation_ (include/moonraker_client.h),
    // trimmed to this client: same bump-on-attempt semantics, no separate
    // per-chain sequence counter (this client only ever runs one discovery
    // chain at a time via discovery_in_flight_).
    std::atomic<uint64_t> connection_generation_{0};

    // Fragment reassembly (grows to cap, shrinks on disconnect). WS-task only.
    std::string rx_buf_;
    bool rx_skip_ = false;
    // WS-task only. Gates the RECONNECTED event so the first-ever connect is
    // silent and only genuine reconnections emit (desktop was_connected_).
    bool was_connected_ = false;

    // Bounded request tracker.
    std::mutex requests_mutex_;
    std::map<uint64_t, Pending> pending_;
    std::atomic<uint64_t> next_request_id_{0};
    uint32_t default_request_timeout_ms_ = DEFAULT_REQUEST_TIMEOUT_MS;
    // Feeds cfg.network_timeout_ms only — the transport's per-operation budget,
    // spent on the websocket task — and is capped at MAX_NETWORK_TIMEOUT_MS on
    // the way in. MoonrakerManager::configure_timeouts() overwrites this from
    // moonraker_connection_timeout_ms at init, so the value here is only the
    // pre-configuration default. The UI-thread send wait is bounded separately
    // by SEND_TIMEOUT_MS.
    uint32_t connection_timeout_ms_ = 10000;

    // Callback maps.
    std::mutex callbacks_mutex_;
    std::map<SubscriptionId, std::function<void(const json&)>> notify_callbacks_;
    std::atomic<uint64_t> next_subscription_id_{0};
    std::map<std::string, std::map<std::string, std::function<void(const json&)>>>
        method_callbacks_;
    // Discovery / bed-mesh callbacks (guarded by callbacks_mutex_). In v1 these
    // pair with the discover_printer stub; Plan 4 drives them from real discovery.
    std::function<void(const helix::PrinterDiscovery&)> on_hardware_discovered_;
    std::function<void(const helix::PrinterDiscovery&, const json&)> on_discovery_complete_;
    std::function<void(const json&)> bed_mesh_callback_;

    std::mutex observers_mutex_;
    std::map<std::string, std::function<void()>> connected_observers_;

    std::mutex event_mutex_;
    MoonrakerEventCallback event_handler_;

    std::atomic<int64_t> suppress_modal_until_us_{0};

    // Guards hardware_: populated on the websocket_task during discovery, read by
    // consumers on the main thread via hardware(). Mirrors desktop's
    // MoonrakerDiscoverySequence::hardware_mutex_.
    mutable std::mutex hardware_mutex_;
    // Collapses a re-entrant discover_printer() while one is already running.
    // Reset on disconnect (on_ws_disconnected), at chain end, and — belt and
    // suspenders against it being stuck true by an abandoned stale chain —
    // whenever a new connection attempt begins (connect(), force_reconnect(),
    // the auto-reconnect executor). The connection_generation_ guard (R3)
    // handles the rest: a stale chain's own continuations never reach the
    // code that would touch this flag on the new generation's behalf.
    std::atomic<bool> discovery_in_flight_{false};

    PrinterDiscovery hardware_; // populated via discovery (guarded by hardware_mutex_)
};

} // namespace helix

// Platform factory (Plan 3 Task 8 consumes this). Returns the ESP32 transport as
// the polymorphic interface so the app layer stays platform-agnostic.
namespace helix {
std::unique_ptr<IMoonrakerClient> create_platform_moonraker_client();
} // namespace helix
