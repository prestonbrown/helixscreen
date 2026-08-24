// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "printer_discovery.h"

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
    void start(std::function<void()> on_complete,
               std::function<void(const std::string& reason)> on_error = nullptr);

    /**
     * @brief Parse Klipper object list into typed hardware vectors
     *
     * Categorizes objects into heaters, sensors, fans, LEDs, steppers,
     * AFC objects, and filament sensors.
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
    }

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
    void set_bed_mesh_callback(std::function<void(const json&)> cb) {
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

    // ======== Hardware vector accessors (for mock to populate directly) ========
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
    std::vector<std::string>& steppers() {
        return steppers_;
    }
    std::vector<std::string>& afc_objects() {
        return afc_objects_;
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
    const std::vector<std::string>& steppers() const {
        return steppers_;
    }
    const std::vector<std::string>& afc_objects() const {
        return afc_objects_;
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

  public:
    /**
     * @brief Pure helper: build the `printer.objects.subscribe` objects map
     *
     * Translates the discovered hardware into the JSON-RPC `objects` argument,
     * narrowing each object to just the fields HelixScreen parsers actually
     * read. Exposed publicly so unit tests can lock the field lists against
     * regression — production code calls it from
     * complete_discovery_subscription() with the live discovery vectors.
     *
     * Pure (no I/O, no logging, no member access) — depends only on its args.
     */
    static nlohmann::json build_subscription_objects(
        const PrinterDiscovery& hw, const std::vector<std::string>& heaters,
        const std::vector<std::string>& sensors, const std::vector<std::string>& fans,
        const std::vector<std::string>& leds, const std::vector<std::string>& afc_objects,
        const std::vector<std::string>& filament_sensors, const std::vector<std::string>& mcus);

  private:
    MoonrakerClient& client_;

    // Discovery sequence callbacks (stored once in start(), used by chained methods)
    std::function<void()> on_complete_discovery_;
    std::function<void(const std::string&)> on_error_discovery_;
    uint64_t discovery_generation_{0};
    uint64_t discovery_sequence_{0}; // Monotonic counter to invalidate prior discoveries

    // Hardware vectors
    std::vector<std::string> heaters_;
    std::vector<std::string> sensors_;
    std::vector<std::string> fans_;
    std::vector<std::string> leds_;
    std::vector<std::string> steppers_;
    std::vector<std::string> afc_objects_;
    std::vector<std::string> filament_sensors_;
    // MCUs (matches "mcu" and "mcu <name>" — e.g. "mcu host", "mcu e0").
    // Subscribed alongside the rest so PerformanceState gets live
    // last_stats/bytes_retransmit via the single union subscription. Moonraker
    // replaces the subscription on every printer.objects.subscribe call, so
    // PerformanceSource cannot subscribe separately without wiping ours.
    std::vector<std::string> mcus_;

    PrinterDiscovery hardware_;
    mutable std::mutex hardware_mutex_; // Protects hardware_ from concurrent read/write (#777)
    std::atomic<bool> identified_{false};
    std::atomic<bool> discovery_completed_{false};

    // Callbacks
    std::function<void(const PrinterDiscovery&)> on_hardware_discovered_;
    std::function<void(const PrinterDiscovery&, const nlohmann::json& initial_status)>
        on_discovery_complete_;
    std::function<void(const json&)> bed_mesh_callback_;
    std::mutex callbacks_mutex_;
};

/**
 * @brief Decide whether a webcam snapshot_url is a usable image endpoint.
 *
 * Moonraker webcam entries advertise a snapshot_url, but some "services"
 * (e.g. the Creality K2's "iframe" service) point it at an HTML WebRTC viewer
 * page such as "/snapshot.html" or "/camera.html" instead of a real JPEG/PNG.
 * CameraStream would poll such a URL forever and never decode a frame.
 *
 * Accept a URL when it looks like a genuine image/snapshot endpoint:
 *   - contains "action=snapshot" (mjpeg-streamer style), or
 *   - ends in .jpg / .jpeg / .png (case-insensitive)
 * Reject when the path ends in ".html" (case-insensitive) — an HTML page, not
 * an image. (A bare-host or query-only URL with none of these markers is treated
 * conservatively as usable, preserving prior behavior for atypical real endpoints.)
 *
 * @param snapshot_url The snapshot_url from a Moonraker webcam entry.
 * @return true if the URL is a plausible image endpoint, false if it's an HTML page.
 */
[[nodiscard]] bool is_usable_snapshot_url(const std::string& snapshot_url);

/// Seconds allowed for the TCP connect phase of a snapshot reachability probe.
/// This is the budget that decides "is anything listening at this address" —
/// the stale-DHCP / wrong-subnet case the probe exists to catch.
inline constexpr int SNAPSHOT_PROBE_CONNECT_TIMEOUT_SEC = 2;

/// Seconds allowed for the whole snapshot reachability probe.
/// Deliberately longer than the connect budget: a go2rtc-style endpoint that
/// transcodes H.264 must wait for the next keyframe before it can emit a JPEG,
/// measured at up to ~2.9s on a Pi 5. Raising this does not slow the dead-address
/// case down — that one loses on the connect budget above.
inline constexpr int SNAPSHOT_PROBE_TOTAL_TIMEOUT_SEC = 6;

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
