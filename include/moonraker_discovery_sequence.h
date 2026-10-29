// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helix_shared_callback.h"
#include "json_fwd.h"
#include "printer_discovery.h"
#include "webcam_selection.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace helix {

class MoonrakerClient; // Forward declaration

/**
 * @brief Owns the multi-step async printer discovery flow
 *
 * Discovery timeline:
 * 1. server.connection.identify → identified
 * 2. server.info → klippy_state gate (abort if not ready/shutdown)
 * 3. printer.objects.list → parse_objects() → on_hardware_discovered (silent)
 * 4. server.info → Moonraker version, Spoolman/webcam detection
 * 5. printer.info → hostname, software_version
 * 6. MCU queries → firmware versions
 * 7. printer.objects.subscribe → initial state dispatched
 * 8. on_discovery_complete
 */
class MoonrakerDiscoverySequence {
  public:
    explicit MoonrakerDiscoverySequence(MoonrakerClient& client);

    // Non-copyable (has mutex, references)
    MoonrakerDiscoverySequence(const MoonrakerDiscoverySequence&) = delete;
    MoonrakerDiscoverySequence& operator=(const MoonrakerDiscoverySequence&) = delete;

    /**
     * @brief Start the discovery sequence
     *
     * Begins with server.connection.identify, then checks Klippy readiness
     * via server.info gate before chaining through
     * objects.list → server.info → printer.info → MCU queries → subscribe.
     *
     * @param on_complete Called when discovery finishes successfully
     * @param on_error Called if discovery fails (e.g., Klippy not connected)
     */
    void start(helix::SharedCallback<void()> on_complete,
               helix::SharedCallback<void(const std::string& reason)> on_error = nullptr);

    /**
     * @brief Parse Klipper object list into typed hardware vectors
     *
     * Categorizes objects into heaters, sensors, fans, LEDs, steppers,
     * filament sensors and MCUs.
     *
     * @param objects JSON array of object name strings
     */
    void parse_objects(const json& objects);

    /**
     * @brief Forward bed mesh data to registered callback
     * @param bed_mesh JSON from bed_mesh subscription
     */
    void parse_bed_mesh(const json& bed_mesh);

    /** @brief Reset identification state (call on disconnect) */
    void reset_identified() {
        identified_.store(false);
    }

    /**
     * @brief Reset the per-connection completion flag.
     *
     * Must be called on every disconnect so that the next connection's
     * notify_klippy_* handlers correctly retry discovery if the gate rejected
     * the initial attempt. Paired with reset_identified().
     */
    void reset_completion() {
        discovery_completed_.store(false);
        reset_subscription_state();
    }

    /// See IMoonrakerClient::set_subscription_extras_provider.
    void set_extras_provider(std::function<json()> provider);

    /// See IMoonrakerClient::refresh_subscription.
    void refresh_subscription();

    /** @brief Check if identified to Moonraker */
    [[nodiscard]] bool is_identified() const {
        return identified_.load();
    }

    /**
     * @brief Whether a discovery has successfully completed on the current connection.
     *
     * Used by the Klippy-state notification handlers to decide whether a
     * transition to ready/shutdown should trigger a retry.
     */
    [[nodiscard]] bool is_completed() const {
        return discovery_completed_.load();
    }

    /** @brief Clear all cached discovery data (vectors + hardware) */
    void clear_cache();

    /** @brief Get a thread-safe snapshot of discovered hardware data */
    [[nodiscard]] PrinterDiscovery hardware() const {
        std::lock_guard<std::mutex> lock(hardware_mutex_);
        return hardware_;
    }

    /** @brief Mutate hardware_ under lock (for kinematics update from BG thread, etc.) */
    template <typename Fn> void modify_hardware(Fn&& fn) {
        std::lock_guard<std::mutex> lock(hardware_mutex_);
        fn(hardware_);
    }

    /** @brief Set callback for early hardware discovery phase (after parse_objects) */
    void set_on_hardware_discovered(std::function<void(const PrinterDiscovery&)> cb) {
        on_hardware_discovered_ = std::move(cb);
    }

    /**
     * @brief Set callback for discovery completion (after subscription)
     *
     * The callback receives the discovered hardware AND the initial status from the
     * subscription response. Callers MUST dispatch the initial status AFTER initializing
     * subsystems (e.g., init_fans) to avoid race conditions where status updates are
     * processed before fan/sensor subjects exist.
     */
    void set_on_discovery_complete(
        std::function<void(const PrinterDiscovery&, const nlohmann::json& initial_status)> cb) {
        on_discovery_complete_ = std::move(cb);
    }

    /** @brief Set callback for bed mesh updates */
    void set_bed_mesh_callback(helix::SharedCallback<void(const json&)> cb) {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        bed_mesh_callback_ = std::move(cb);
    }

    // ======== Callback invocation (for mock to trigger discovery callbacks) ========

    /** @brief Invoke the on_hardware_discovered callback with current hardware */
    void invoke_hardware_discovered() {
        if (on_hardware_discovered_) {
            PrinterDiscovery snapshot;
            {
                std::lock_guard<std::mutex> lock(hardware_mutex_);
                snapshot = hardware_;
            }
            on_hardware_discovered_(snapshot);
        }
    }

    /** @brief Invoke the on_discovery_complete callback with current hardware (mock use) */
    void invoke_discovery_complete();

    // ======== Mock-owned hardware lists (real discovery reads PrinterDiscovery) ========
    // Thread safety: mutable accessors must only be called before start() or
    // from the same thread as discovery callbacks. Not safe for concurrent use.

    std::vector<std::string>& heaters() {
        return heaters_;
    }
    std::vector<std::string>& sensors() {
        return sensors_;
    }
    std::vector<std::string>& fans() {
        return fans_;
    }
    std::vector<std::string>& leds() {
        return leds_;
    }
    std::vector<std::string>& filament_sensors() {
        return filament_sensors_;
    }

    const std::vector<std::string>& heaters() const {
        return heaters_;
    }
    const std::vector<std::string>& sensors() const {
        return sensors_;
    }
    const std::vector<std::string>& fans() const {
        return fans_;
    }
    const std::vector<std::string>& leds() const {
        return leds_;
    }
    const std::vector<std::string>& filament_sensors() const {
        return filament_sensors_;
    }

  private:
    /**
     * @brief Check if the current discovery sequence is stale
     *
     * Returns true if the connection has been recycled since start() was called,
     * meaning any in-flight callbacks should be silently dropped.
     */
    bool is_stale() const;

    /** @brief Check if a captured sequence number matches the current discovery */
    bool is_current_sequence(uint64_t seq) const;

    /// The identify failed because Moonraker refused the login: every later request would
    /// fail the same way, so discovery stops and the refusal is reported once. False for
    /// any other error, which discovery carries on past.
    bool stop_for_refused_login(const MoonrakerError& err);

    /**
     * @brief Continue discovery after server.connection.identify
     *
     * Calls server.info to check klippy_state (gate). If Klippy is ready
     * or shutdown, proceeds to continue_discovery_objects(). Otherwise aborts.
     */
    void continue_discovery(uint64_t seq);

    /**
     * @brief Discover Moonraker-managed power devices (fire-and-forget)
     *
     * Queries machine.device_power.devices and populates PowerDeviceState.
     * Called both during full discovery and as partial discovery when
     * Klippy is not ready (power devices only need Moonraker, not Klipper).
     */
    void discover_power_devices();

    /**
     * @brief Discover Moonraker sensors (fire-and-forget)
     *
     * Queries server.sensors.list and populates SensorState with initial values.
     * Called alongside discover_power_devices() during both partial and full discovery.
     */
    void discover_sensors();

    /**
     * @brief Read Moonraker's version from update_manager (fire-and-forget)
     *
     * For a server.info that reported a placeholder version: machine.update.status
     * still knows the checkout's tag or commit. Publishes to hardware_ (for the
     * discovery snapshot) and PrinterState (for a reply landing after it).
     */
    void fetch_moonraker_version_fallback(uint64_t seq);

    /**
     * @brief Record the hardware fields carried by a machine.system_info reply
     *
     * OS distribution name, CPU architecture, and the model string a vendor
     * firmware may publish as `system_info.machine_name`. The same reply also
     * carries the camera `service_state` detect_webcam() cross-checks, which is
     * why one query feeds both.
     */
    void parse_system_info(const json& sys_response);

    /**
     * @brief Publish the printer's identity string
     *
     * `printer.info` reports the host's own name, which on a stock vendor image
     * is the distro default and names nothing. A firmware that publishes
     * `machine_name` is naming the machine itself, so that wins whenever it is
     * present. Both replies are in flight at once and either may land first, so
     * precedence lives here rather than in whichever handler ran last.
     *
     * Caller must hold hardware_mutex_.
     */
    void publish_identity_locked();

    /**
     * @brief Detect a usable webcam and publish the answer (fire-and-forget)
     *
     * Queries server.webcams.list, cross-checks that the service serving the
     * chosen entry is actually running, probes an absolute snapshot URL for
     * reachability, and falls back to probing local camera endpoints.
     *
     * @param service_state `service_state` out of machine.system_info, or an
     *        empty object when that query failed — which reads as "assume
     *        available" (prestonbrown/helixscreen#1351).
     * @param probe_local_endpoints Fall back to probing loopback camera ports
     *        when the registry has nothing usable; see
     *        helix::board::Info::local_camera_probe_safe for where that is allowed.
     */
    void detect_webcam(json service_state, bool probe_local_endpoints);

    /**
     * @brief Continue discovery after Klippy readiness gate passes
     *
     * Chains: objects.list → server.info → printer.info → MCU queries → subscribe
     */
    void continue_discovery_objects(uint64_t seq);

    /**
     * @brief Complete discovery by subscribing to printer objects
     *
     * Builds subscription JSON from discovered objects, subscribes,
     * dispatches initial state to all registered callbacks.
     */
    void complete_discovery_subscription(uint64_t seq);

    /// Send the discovery subscribe for @p app merged with @p extras. A failure that carried
    /// extras re-sends @p app alone; only a failure of the app objects themselves is reported.
    void send_discovery_subscribe(uint64_t seq, const PrinterDiscovery& hw, const json& app,
                                  const json& extras);

    /// The discovery subscribe's response: initial state, discovery completion.
    void finish_discovery_subscription(const PrinterDiscovery& hw, size_t num_subscribed,
                                       const json& sub_response);

    /// The provider's current extras, or an empty object when there is no provider or its
    /// result is the one Moonraker last refused.
    json current_extras();

    /// Remember that Moonraker refused @p extras so they are not sent again unchanged.
    void reject_extras(const json& extras);

    void reset_subscription_state();

    /// Run a refresh that was asked for while none could be sent.
    void drain_pending_refresh();

  public:
    /**
     * @brief Pure helper: build the `printer.objects.subscribe` objects map
     *
     * Translates the discovered hardware into the JSON-RPC `objects` argument,
     * narrowing each object to just the fields HelixScreen parsers actually
     * read. Exposed publicly so unit tests can lock the field lists against
     * regression — production code calls it from
     * complete_discovery_subscription() with the discovery snapshot.
     *
     * Every object PrinterDiscovery classified is subscribed with
     * hw::status_fields(kind). Pure aside from the active print-start profile
     * lookup; no member access.
     */
    static nlohmann::json build_subscription_objects(const PrinterDiscovery& hw);

  private:
    MoonrakerClient& client_;

    // Discovery sequence callbacks (stored once in start(), used by chained methods)
    helix::SharedCallback<void()> on_complete_discovery_;
    helix::SharedCallback<void(const std::string&)> on_error_discovery_;
    uint64_t discovery_generation_{0};
    uint64_t discovery_sequence_{0}; // Monotonic counter to invalidate prior discoveries

    // Hardware vectors
    std::vector<std::string> heaters_;
    std::vector<std::string> sensors_;
    std::vector<std::string> fans_;
    std::vector<std::string> leds_;
    std::vector<std::string> filament_sensors_;

    PrinterDiscovery hardware_;
    mutable std::mutex hardware_mutex_; // Protects hardware_ from concurrent read/write (#777)
    // Identity strings as each reply reports them, kept apart from the resolved
    // hostname so a late reply cannot undo the precedence rule.
    std::string reported_hostname_;     // printer.info "hostname"
    std::string reported_machine_name_; // machine.system_info "machine_name"
    std::atomic<bool> identified_{false};
    std::atomic<bool> discovery_completed_{false};

    // Plugin objects merged into the union subscription. The provider is called while
    // extras_mutex_ is held, so clearing it waits out a call in progress.
    std::mutex extras_mutex_;
    std::function<json()> extras_provider_;
    std::string rejected_extras_; // dump of the extras Moonraker last refused

    // Per-connection subscription state, under subscription_mutex_.
    std::mutex subscription_mutex_;
    bool subscribed_{false};         // the discovery subscribe of this connection succeeded
    json last_app_objects_;          // build_subscription_objects() result of this connection
    json last_sent_objects_;         // objects map of the last subscribe Moonraker accepted
    uint64_t subscription_epoch_{0}; // moves on with every disconnect and discovery subscribe
    bool refresh_in_flight_{false};
    bool refresh_pending_{false};

    // Callbacks
    std::function<void(const PrinterDiscovery&)> on_hardware_discovered_;
    std::function<void(const PrinterDiscovery&, const nlohmann::json& initial_status)>
        on_discovery_complete_;
    helix::SharedCallback<void(const json&)> bed_mesh_callback_;
    std::mutex callbacks_mutex_;
};

/// Seconds allowed for the TCP connect phase of a snapshot reachability probe.
/// This is the budget that decides "is anything listening at this address" —
/// the stale-DHCP / wrong-subnet case the probe exists to catch.
inline constexpr int kSnapshotProbeConnectTimeoutSec = 2;

/// Seconds allowed for the whole snapshot reachability probe.
/// Deliberately longer than the connect budget: a go2rtc-style endpoint that
/// transcodes H.264 must wait for the next keyframe before it can emit a JPEG,
/// measured at up to ~2.9s on a Pi 5. Raising this does not slow the dead-address
/// case down — that one loses on the connect budget above.
inline constexpr int kSnapshotProbeTotalTimeoutSec = 6;

/**
 * @brief Probe an absolute snapshot URL to decide whether the webcam is live.
 *
 * Guards against a stale ABSOLUTE webcam URL — e.g. an install-time-detected LAN
 * IP that has since changed via DHCP, or an IOT-subnet address unreachable from
 * here. A registered-but-dead entry otherwise wins over (and suppresses) the
 * local camera probe, leaving the camera silently broken.
 *
 * Two separate budgets, because "nothing is listening" and "listening but slow"
 * are different answers. A single total timeout conflates them: libhv clamps the
 * connect phase to MIN(connect_timeout, timeout), so one short budget covers both
 * phases and a live endpoint that needs three seconds to produce a frame is
 * rejected exactly like a dead host (prestonbrown/helixscreen#1205).
 *
 * @param snapshot_url Absolute http(s) snapshot URL to probe.
 * @return true if the endpoint answered 200 within the budget.
 */
[[nodiscard]] bool probe_snapshot_reachable(const std::string& snapshot_url);

} // namespace helix
