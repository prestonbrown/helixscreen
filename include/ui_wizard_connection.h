// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lan_sweep.h"
#include "lvgl/lvgl.h"
#include "mdns_discovery.h"
#include "utils/network_validation.h"
#include "wizard_step.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/**
 * @file ui_wizard_connection.h
 * @brief Wizard Moonraker connection step - WebSocket configuration and testing
 *
 * Handles Moonraker WebSocket configuration during first-run wizard:
 * - IP address or hostname entry
 * - Port number configuration (default: 7125)
 * - Connection testing with async feedback
 * - Auto-discovery trigger on success
 * - Configuration persistence
 *
 * ## Class-Based Architecture (Phase 6)
 *
 * This step has been migrated from function-based to class-based design:
 * - Instance members instead of static globals
 * - Async WebSocket callbacks with captured instance reference
 * - Static trampolines for LVGL event callbacks
 * - Global singleton getter for backwards compatibility
 *
 * ## Subject Bindings (6 total):
 *
 * - connection_ip (string) - IP address or hostname
 * - connection_port (string) - Port number (default "7125")
 * - connection_status_icon (string) - MDI icon (check/xmark/empty)
 * - connection_status_text (string) - Status message text
 * - connection_testing (int) - 0=idle, 1=testing (disables button)
 * - connection_discovering (int) - 0=not discovering, 1=discovering (shows spinner)
 *
 * ## External Subject:
 *
 * - connection_test_passed (extern) - Controls wizard Next button globally
 *
 * Initialization Order (CRITICAL):
 *   1. Register XML component (wizard_connection.xml)
 *   2. init_subjects()
 *   3. register_callbacks()
 *   4. create(parent)
 */

/**
 * @class WizardConnectionStep
 * @brief Moonraker WebSocket connection step for the first-run wizard
 *
 * Allows user to enter Moonraker IP/port and test the connection.
 * On success, triggers hardware discovery for subsequent wizard steps.
 */
class WizardConnectionStep : public helix::wizard::Step {
  public:
    // helix::wizard::Step interface
    helix::wizard::StepId id() const override {
        return helix::wizard::StepId::Connection;
    }
    const char* component_name() const override {
        return "wizard_connection";
    }
    const char* log_name() const override {
        return "Wizard Connection";
    }
    bool should_skip([[maybe_unused]] const helix::wizard::StepContext& ctx) const override {
        return false;
    }

    /// Auto-probe state for localhost detection
    enum class AutoProbeState {
        IDLE,        ///< Never probed or probe complete
        IN_PROGRESS, ///< Currently probing localhost
        SUCCEEDED,   ///< Found printer at localhost
        FAILED       ///< No printer at localhost (silent failure)
    };

  public:
    WizardConnectionStep();
    ~WizardConnectionStep();

    // Non-copyable, non-movable (singleton with lv_subject_t members that
    // contain internal linked lists — moving corrupts observer pointers)
    WizardConnectionStep(const WizardConnectionStep&) = delete;
    WizardConnectionStep& operator=(const WizardConnectionStep&) = delete;
    WizardConnectionStep(WizardConnectionStep&&) = delete;
    WizardConnectionStep& operator=(WizardConnectionStep&&) = delete;

    /**
     * @brief Initialize reactive subjects
     *
     * Creates and registers 5 subjects. Loads existing values from config.
     * Sets connection_test_passed to 0 (disabled) for this step.
     */
    void init_subjects() override;

    /**
     * @brief Register event callbacks with lv_xml system
     *
     * Registers callbacks:
     * - on_test_connection_clicked
     * - on_ip_input_changed
     * - on_port_input_changed
     */
    void register_callbacks() override;

    /**
     * @brief Create the connection UI from XML
     *
     * @param parent Parent container (wizard_content)
     * @return Root object of the step, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Cleanup resources
     *
     * Cancels any ongoing connection test and resets UI references.
     */
    void cleanup() override;

    /**
     * @brief Get the configured Moonraker URL
     *
     * @param buffer Output buffer for the URL
     * @param size Size of the output buffer
     * @return true if URL was successfully constructed
     */
    bool get_url(char* buffer, size_t size) const;

    /**
     * @brief Check if connection has been successfully tested
     *
     * @return true if a successful connection test has been performed
     */
    bool is_validated() const override;

    /**
     * @brief Get step name for logging
     */
    const char* get_name() const {
        return "Wizard Connection";
    }

    /**
     * @brief Inject an mDNS discovery implementation
     *
     * Allows tests to inject a MockMdnsDiscovery to avoid network I/O
     * and background threads. Must be called before create().
     *
     * @param discovery The mDNS discovery implementation to use
     */
    void set_mdns_discovery(std::unique_ptr<helix::IMdnsDiscovery> discovery);

    /// Test-only: expose a lifetime token for verifying callback safety
    helix::LifetimeToken lifetime_token_for_test() {
        return lifetime_.token();
    }

  private:
    // Screen instance
    lv_obj_t* screen_root_ = nullptr;

    // Subjects
    lv_subject_t connection_ip_{};
    lv_subject_t connection_port_{};
    lv_subject_t connection_status_icon_{};
    lv_subject_t connection_status_text_{};
    lv_subject_t connection_testing_{};
    lv_subject_t connection_discovering_{};
    /// Moonraker API key, for a printer whose Moonraker requires a login.
    lv_subject_t connection_api_key_{};
    /// 1 shows the API key field: the printer refused a test for want of one, or has one saved.
    lv_subject_t connection_login_required_{};

    // String buffers (must be persistent)
    char connection_ip_buffer_[128];
    char connection_port_buffer_[8];
    char connection_status_icon_buffer_[8];
    char connection_status_text_buffer_[256];
    char connection_api_key_buffer_[80];

    // State tracking (main thread only)
    bool connection_validated_ = false;
    bool subjects_initialized_ = false;

    // Async callback safety — invalidated on cleanup AND on each new connection
    // attempt, replacing both cleanup_called_ and connection_generation_ (#193, #827)
    helix::AsyncLifetimeGuard lifetime_;

    // Auto-probe state for localhost detection (atomic for cross-thread access)
    std::atomic<AutoProbeState> auto_probe_state_{AutoProbeState::IDLE};
    bool auto_probe_attempted_ = false; // Main thread only
    lv_timer_t* auto_probe_timer_ = nullptr;

    /// Upper bound on the discovery spinner (#1161). A healthy discovery is a
    /// handful of chained JSON-RPCs that settle in a few seconds even on an
    /// AD5M/CC1 over wifi, so this is ~6x headroom; it is also deliberately
    /// under MoonrakerRequestTracker's 60 s per-request timeout, which would
    /// otherwise stack across identify → server.info → printer.objects.list.
    static constexpr uint32_t kDiscoveryWatchdogMs = 30000;

    // Watchdog state — main thread only (created, fired and cancelled there)
    uint32_t discovery_watchdog_ms_ = kDiscoveryWatchdogMs;
    lv_timer_t* discovery_watchdog_timer_ = nullptr;
    bool discovery_in_flight_ = false;

    // Saved values for async callback - protected by mutex for thread-safe access
    mutable std::mutex saved_values_mutex_;
    std::string saved_ip_;      // Protected by saved_values_mutex_
    std::string saved_port_;    // Protected by saved_values_mutex_
    std::string saved_api_key_; // Protected by saved_values_mutex_

    // Where the port field's value came from (main thread only); moonraker_ports_to_try()
    // decides from it. Reset to FilledIn by cleanup().
    helix::PortOrigin port_origin_ = helix::PortOrigin::FilledIn;
    /// Set while the app writes the port field, so the write is not taken for typing.
    bool port_set_by_app_ = false;
    /// The ports the last test tried, for the failure message.
    std::vector<int> tried_ports_;
    /// Test Connection's HTTP check, and the auto-probe's.
    helix::MoonrakerConnectionTest connection_test_;

    /// Write @p port to the port subject and field without marking it typed.
    void set_port_field(const std::string& port);

    /// A probe found Moonraker on @p port: record it and show it in the port field.
    void adopt_probed_port(int port);

    /// The API key field, trimmed; blank means the printer takes none.
    std::string entered_api_key();

    /// Moonraker at @p ip answered the test but refused it: show the key field and why.
    void show_login_required(const std::string& ip,
                             const helix::MoonrakerConnectionTest::Result& result);

    /// Writes the tested address and API key to the active printer's settings.
    void save_connection(const std::string& ip, const std::string& port);

    // Event handler implementations
    void handle_test_connection_clicked();
    void handle_ip_input_changed();
    void handle_port_input_changed();

    // Async callback handlers (called from WebSocket callbacks, marshal via tok.defer)
    void on_connection_success(const helix::LifetimeToken& tok);
    void on_connection_failure(const helix::LifetimeToken& tok);
    /// Shows a failed test for @p kind with the ports tried, and keeps Next shut.
    void show_connection_failure(helix::ConnectFailureKind kind);
    /// Connects the live client to saved_ip_:saved_port_, the address a test confirmed,
    /// for the hardware discovery the next steps need.
    void connect_confirmed(const helix::LifetimeToken& tok, bool auto_probe, uint32_t timeout_ms);

    // Helper to set status icon and text imperatively with appropriate colors
    enum class StatusVariant { None, Success, Warning, Danger };
    void set_status(const char* icon_name, StatusVariant variant, const char* text);

  public:
    /**
     * @brief Hardware discovery failed even though Moonraker answered
     *
     * Reached only from the connected path, so the address the user entered is
     * correct and only Klipper is unusable (bad printer.cfg, MCU fault, or
     * still starting). Enables Next with an explicit warning instead of
     * blocking: the wizard is a full-screen overlay with no Skip button on
     * this step, so gating here left the single most common first-boot state
     * (Moonraker up, Klipper in `error`) with no way forward, backward, or
     * into the app's own Klipper error surface.
     */
    void allow_continue_without_klipper();

    /**
     * @brief Arm the bounded window that discovery must finish inside
     *
     * Called on the main thread immediately before discover_printer(). Cancels
     * any previously armed watchdog, so a retry never leaves two running.
     *
     * discover_printer() is not guaranteed to invoke either of its callbacks:
     * MoonrakerDiscoverySequence drops its RPC replies whenever the connection
     * generation or the discovery sequence number moved on (is_stale() /
     * is_current_sequence()), and the error path in continue_discovery() nulls
     * on_complete_discovery_ before handing off. The sequence therefore cannot
     * own this timeout — the step must.
     */
    void start_discovery_watchdog();

    /**
     * @brief The discovery window closed with neither callback having fired
     *
     * Unblocks Next the same way allow_continue_without_klipper() does, with a
     * status that does not claim to know why: from here we only know discovery
     * went quiet, not whether Klipper is down. Idempotent — a fire that races a
     * completion is a no-op because the completion cancelled the watchdog.
     */
    void discovery_watchdog_expired();

    /// Test-only: shorten the watchdog window so a test can observe it fire.
    void set_discovery_watchdog_ms_for_test(uint32_t ms) {
        discovery_watchdog_ms_ = ms;
    }

    /// Test-only: arm the one-shot auto-probe timer without building the screen,
    /// so the teardown paths that must cancel it can be exercised directly.
    void arm_auto_probe_timer_for_test();

    /// Test-only: the armed auto-probe timer, or nullptr when none is armed.
    lv_timer_t* auto_probe_timer_for_test() const {
        return auto_probe_timer_;
    }

  private:
    /// Cancel an armed watchdog. Safe to call when none is armed.
    void cancel_discovery_watchdog();

    /// Cancel a pending auto-probe timer. Safe to call when none is armed.
    void cancel_auto_probe_timer();

    /// Shared tail of allow_continue_without_klipper() / discovery_watchdog_expired()
    void unblock_after_incomplete_discovery(const char* message);

    // Auto-probe methods for localhost detection
    bool should_auto_probe() const;
    void attempt_auto_probe();
    void on_auto_probe_success(const helix::LifetimeToken& tok);
    void on_auto_probe_failure(const helix::LifetimeToken& tok);

    // Static trampolines for LVGL callbacks
    static void on_test_connection_clicked_static(lv_event_t* e);
    static void on_ip_input_changed_static(lv_event_t* e);
    static void on_port_input_changed_static(lv_event_t* e);
    static void on_api_key_input_changed_cb(lv_event_t* e);
    static void auto_probe_timer_cb(lv_timer_t* timer);
    static void discovery_watchdog_timer_cb(lv_timer_t* timer);

    /// Gates discovery callbacks. Separate from lifetime_, which every connection attempt
    /// invalidates while discovery keeps running.
    helix::AsyncLifetimeGuard discovery_lifetime_;

    // mDNS discovery (injectable for testing)
    std::unique_ptr<helix::IMdnsDiscovery> mdns_discovery_;
    std::vector<helix::DiscoveredPrinter> discovered_printers_;

    // Subjects for mDNS UI
    lv_subject_t mdns_status_{};      ///< "Scanning..." / "Found N printer(s)"
    lv_subject_t discovery_active_{}; ///< 1 while printer discovery runs (shows the section)
    char mdns_status_buffer_[64];

    // mDNS callbacks
    void on_printers_discovered(const std::vector<helix::DiscoveredPrinter>& printers);
    static void on_printer_selected_cb(lv_event_t* e);
};

// ============================================================================
// Global Instance Access
// ============================================================================

/**
 * @brief Get the global WizardConnectionStep instance
 *
 * Creates the instance on first call. Used by wizard framework.
 */
WizardConnectionStep* get_wizard_connection_step();
