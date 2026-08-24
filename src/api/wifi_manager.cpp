// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file wifi_manager.cpp
 * @brief High-level WiFi operations manager wrapping platform backends
 *
 * @pattern Manager with weak self_ reference for callback safety
 * @threading Backend callbacks may run on background thread
 * @gotchas Uses fprintf in destructor instead of spdlog; clears callbacks BEFORE stopping backend
 *
 * @see wifi_backend.cpp
 */

#include "wifi_manager.h"

#include "ui_error_reporting.h"
#include "ui_timer_guard.h"
#include "ui_update_queue.h"

#include "http_executor.h"
#include "log_redact.h"
#include "lvgl/lvgl.h"
#include "safe_log.h"
#include "spdlog/spdlog.h"
#include "system_settings_manager.h"
#include "wifi_interface.h"
#include "wifi_ui_utils.h"

#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)
// NetworkManager/wpa_supplicant fallback (handle_init_failed, below) is a
// Linux-desktop-only concern — ESP32 has a single esp_wifi backend with no
// fallback path (see wifi_backend_esp.cpp).
#include "wifi_backend_networkmanager.h"
#include "wifi_backend_wpa_supplicant.h"
#endif

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

using namespace helix;

// Default OS-level link probe: true when the kernel reports a wireless iface
// with an up link. Overridable in tests via WiFiManagerTestAccess.
std::function<bool()> WiFiManager::os_link_probe_ = []() {
    return helix::ui::wifi::probe_os_wifi_link().has_link;
};

bool WiFiManager::os_link_up() {
    return os_link_probe_ && os_link_probe_();
}

void WiFiManager::mark_association_change() {
    last_association_change_ = std::chrono::steady_clock::now();
}

bool WiFiManager::in_association_grace() const {
    if (last_association_change_.time_since_epoch().count() == 0)
        return false;
    return (std::chrono::steady_clock::now() - last_association_change_) < ASSOCIATION_GRACE;
}

// Overridable in tests via WiFiManagerTestAccess so has_non_wifi_network_path()
// can be pointed at a fixture tree instead of the real /sys.
std::string WiFiManager::sys_root_ = "/sys";

// ============================================================================
// Constructor / Destructor
// ============================================================================

WiFiManager::WiFiManager(bool silent) : scan_timer_(nullptr), scan_pending_(false) {
    spdlog::debug("[WiFiManager] Initializing with backend system{}",
                  silent ? " (silent mode)" : "");

    // Create platform-appropriate backend. Factory returns immediately —
    // backend is NOT yet initialized. We register our event handlers first,
    // then kick off the deferred init via start_async() so we never miss a
    // READY / INIT_FAILED event.
    backend_ = WifiBackend::create(silent);
    if (!backend_) {
        if (!silent) {
            NOTIFY_ERROR_MODAL("WiFi Unavailable",
                               "Could not initialize WiFi hardware. Check system configuration.");
        } else {
            spdlog::debug("[WiFiManager] WiFi unavailable (silent mode - no modal)");
        }
        return;
    }

    // Register event callbacks BEFORE kicking off async init
    register_backend_callbacks(silent);

    // Kick off deferred initialization on a worker thread — this returns
    // immediately so the UI thread isn't blocked on subprocess probing.
    backend_->start_async();
}

WiFiManager::WiFiManager(std::unique_ptr<WifiBackend> backend, bool silent)
    : scan_timer_(nullptr), scan_pending_(false) {
    spdlog::debug("[WiFiManager] Initializing with injected backend{}",
                  silent ? " (silent mode)" : "");

    backend_ = std::move(backend);
    if (!backend_) {
        if (!silent) {
            NOTIFY_ERROR_MODAL("WiFi Unavailable",
                               "Could not initialize WiFi hardware. Check system configuration.");
        } else {
            spdlog::debug("[WiFiManager] WiFi unavailable (silent mode - no modal)");
        }
        return;
    }

    // Same bringup sequence as the platform-selecting constructor: register
    // handlers before kicking off async init so no READY / INIT_FAILED event
    // is missed.
    register_backend_callbacks(silent);
    backend_->start_async();
}

void WiFiManager::register_backend_callbacks(bool silent) {
    backend_->register_event_callback(
        "SCAN_COMPLETE", [this](const std::string& data) { handle_scan_complete(data); });
    backend_->register_event_callback("CONNECTED",
                                      [this](const std::string& data) { handle_connected(data); });
    backend_->register_event_callback(
        "DISCONNECTED", [this](const std::string& data) { handle_disconnected(data); });
    backend_->register_event_callback(
        "AUTH_FAILED", [this](const std::string& data) { handle_auth_failed(data); });
    backend_->register_event_callback(
        "INIT_FAILED", [this, silent](const std::string& msg) { handle_init_failed(silent, msg); });
    backend_->register_event_callback("READY", [this](const std::string&) {
        spdlog::debug("[WiFiManager] Backend READY event received");
        // Wake UI consumers that queried status before async init landed —
        // NetworkWidget in particular attaches synchronously during home-panel
        // load, races the backend's worker thread, and gets an empty STATUS
        // response that pins it on 'Disconnected' until this event lands.
        notify_state_observers();

        // A radio the user switched off must not come back on because the
        // process restarted. Reassert the stored choice once the backend can
        // act on it. READY fires on a background thread (the backend's init
        // worker — see the INIT_FAILED handling above), and both
        // get_wifi_enabled() (reads an lv_subject_t) and set_radio_enabled()
        // must not run there, so defer through async_lifetime_ the same way
        // the NetworkManager fallback above does.
        //
        // CC1 incident (Task 15): on a device whose only network path is this
        // radio, reasserting "off" is a one-way door — a reboot clears the
        // rfkill soft-block, but the watchdog restarts HelixScreen, which reads
        // the stored setting and blocks the radio again. The device can never
        // be reached remotely again. Only reassert "off" when a non-WiFi
        // network path (wired or otherwise) is actually up. When resolution of
        // the WiFi interface itself is inconclusive, fail safe: treat it the
        // same as "no known wired fallback" and do not disable the radio.
        async_lifetime_.defer("WiFiManager::reassert_stored_radio_state", [this]() {
            const bool want_on = SystemSettingsManager::instance().get_wifi_enabled();
            if (!want_on && backend_) {
                const auto iface = backend_->resolved_interface();
                const bool safe_to_disable =
                    iface.has_value() &&
                    helix::wifi::detail::has_non_wifi_network_path(sys_root_, iface->netdev);

                if (safe_to_disable) {
                    spdlog::info("[WiFiManager] Stored setting is WiFi off — reasserting");
                    backend_->set_radio_enabled(false);
                } else {
                    // Radio stays on. Leaving wifi_enabled at its stored
                    // false would show "off" in the UI over a working
                    // connection — exactly the class of state lie this whole
                    // branch exists to eliminate. Correct the stored value to
                    // match reality rather than silently preserving a choice
                    // we have deliberately declined to apply.
                    spdlog::warn(
                        "[WiFiManager] Stored setting is WiFi off, but no non-WiFi network "
                        "path was found (interface resolution {}) — refusing to disable the "
                        "radio to avoid stranding the device. Correcting stored setting to on.",
                        iface.has_value() ? "succeeded" : "inconclusive");
                    SystemSettingsManager::instance().set_wifi_enabled(true);
                    if (!backend_->is_radio_enabled()) {
                        backend_->set_radio_enabled(true);
                    }
                }
            } else if (want_on && backend_ && !backend_->is_radio_enabled()) {
                // Mirror image of the off-reassert above: the stored setting
                // is on, but the radio itself is soft-blocked — e.g. a stale
                // rfkill soft-block from a previous run, or one this same
                // process created before commit 8aaac4e78 made a soft block
                // non-fatal at startup instead of aborting init entirely.
                // is_radio_enabled() is now seeded from hardware
                // (<rfkill>/soft) during resolve_and_store_interface(), so it
                // reflects reality here rather than a hopeful default. A
                // device left in this state before that fix stays radio-dead
                // until someone physically taps the touchscreen (Task 15,
                // CC1) — clear the stale block automatically instead.
                spdlog::info("[WiFiManager] Stored setting is WiFi on, but the radio was "
                             "soft-blocked — clearing the stale block to match the stored "
                             "preference");
                backend_->set_radio_enabled(true);
            }
        });
    });
}

void WiFiManager::handle_init_failed(bool silent, const std::string& msg) {
    // On Linux, if the NetworkManager backend fails (e.g. nmcli binary present
    // but NM daemon masked/dead), transparently fall back to wpa_supplicant
    // so users aren't left WiFi-less because of a dormant NM install. Guarded
    // by tried_fallback_ to avoid infinite loops if wpa_supplicant also fails.
#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)
    if (!tried_fallback_ && backend_ && backend_->is_network_manager()) {
        tried_fallback_ = true;
        spdlog::warn("[WiFiManager] NetworkManager backend INIT_FAILED ({}); "
                     "falling back to wpa_supplicant",
                     msg);
        // CRITICAL: INIT_FAILED fires from inside the NM backend's init worker
        // thread. Calling backend_->stop() here would invoke
        // init_thread_.join() on the currently-executing thread, producing
        // std::system_error(resource_deadlock_would_occur). Defer the swap to
        // the main/UI thread via UpdateQueue so the init thread can unwind
        // before stop() joins it. The shared instance is owned by
        // the never-freed global holder for the life of the process, but tests build
        // their own on the stack, so the swap is routed through the guard rather
        // than relying on that (#1165).
        async_lifetime_.defer("WiFiManager::fallback_to_wpa_supplicant", [this, silent]() {
            if (!backend_) {
                return;
            }
            backend_->stop();
            backend_.reset();
            backend_ = std::make_unique<WifiBackendWpaSupplicant>();
            backend_->set_silent(silent);
            register_backend_callbacks(silent);
            backend_->start_async();
        });
        return;
    }
#endif
    // Backend initialization failed asynchronously - notify user (unless silent)
    if (!silent) {
        NOTIFY_ERROR("WiFi initialization failed: {}", msg);
    } else {
        spdlog::debug("[WiFiManager] WiFi init failed (silent): {}", msg);
    }
}

void WiFiManager::init_self_reference(std::shared_ptr<WiFiManager> self) {
    self_ = self;
    spdlog::debug("[WiFiManager] Self-reference initialized for async callback safety");
}

bool WiFiManager::has_non_wifi_fallback() {
    if (!backend_) {
        return false;
    }
    const auto iface = backend_->resolved_interface();
    return iface.has_value() &&
           helix::wifi::detail::has_non_wifi_network_path(sys_root_, iface->netdev);
}

WiFiManager::~WiFiManager() {
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[WiFiManager] Destructor called\n");

    // A set_enabled_async() worker is sitting inside backend_->set_radio_enabled().
    // Nothing below (least of all backend_.reset()) may run while it is.
    wait_for_radio_ops();

    // Clean up scanning
    stop_scan();

    // Clean up any pending auth-failure grace timer (helixscreen#1050)
    if (auth_fail_grace_timer_ && lv_is_initialized()) {
        lv_timer_delete(auth_fail_grace_timer_);
    }
    auth_fail_grace_timer_ = nullptr;

    // Same for the connect watchdog: StaticPanelRegistry::destroy_all() runs before
    // lv_deinit(), so an armed timer would still be in LVGL's list holding a freed `this`.
    cancel_connect_timeout();

    // Clear callbacks BEFORE stopping backend
    // Pending lv_async_call operations check for null callbacks before invoking
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_callback_ = nullptr;
        connect_callback_ = nullptr;
    }

    // Stop backend (this stops backend threads)
    if (backend_) {
        backend_->stop();
    }
}

// ============================================================================
// Network Scanning
// ============================================================================

std::vector<WiFiNetwork> WiFiManager::scan_once() {
    if (!backend_) {
        LOG_WARN_INTERNAL("No backend available for scan");
        return {};
    }

    spdlog::debug("[WiFiManager] Performing single scan");

    // Trigger scan and wait briefly for results
    WiFiError scan_result = backend_->trigger_scan();
    if (!scan_result.success()) {
        LOG_WARN_INTERNAL("Failed to trigger scan: {}", scan_result.technical_msg);
        return {};
    }

    // For synchronous scan, we need to get existing results
    // Note: This may not include the just-triggered scan results immediately
    std::vector<WiFiNetwork> networks;
    WiFiError get_result = backend_->get_scan_results(networks);
    if (!get_result.success()) {
        LOG_WARN_INTERNAL("Failed to get scan results: {}", get_result.technical_msg);
        return {};
    }

    return networks;
}

void WiFiManager::start_scan(
    std::function<void(const std::vector<WiFiNetwork>&)> on_networks_updated) {
    if (!backend_) {
        NOTIFY_ERROR("WiFi unavailable. Cannot scan for networks.");
        return;
    }

    spdlog::debug("[WiFiManager] start_scan ENTRY, callback is {}",
                  on_networks_updated ? "NOT NULL" : "NULL");

    // Stop existing timer if running (also clears old callback)
    stop_scan();

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_callback_ = on_networks_updated;
    }
    spdlog::debug("[WiFiManager] Scan callback registered");

    spdlog::info("[WiFiManager] Starting periodic network scan (interval backs off {}ms-{}ms)",
                 helix::wifi::ScanScheduler::BASE_INTERVAL_MS,
                 helix::wifi::ScanScheduler::MAX_INTERVAL_MS);

    // A fresh scan session (e.g. the user opening network settings) is a
    // manual refresh: clear any suppression/backoff left over from a prior
    // session and start this one's timer at the base interval. Touching
    // scan_scheduler_ directly is safe because start_scan() is LVGL-thread-only
    // — it creates the scan lv_timer below, and stop_scan() deletes it. Callers
    // on any other thread must marshal via helix::ui::queue_update().
    scan_scheduler_.on_user_refresh();

    // Create timer for periodic scanning
    scan_timer_ =
        lv_timer_create(scan_timer_callback, helix::wifi::ScanScheduler::BASE_INTERVAL_MS, this);
    spdlog::debug("[WiFiManager] Timer created: {}", (void*)scan_timer_);

    // Trigger immediate scan
    spdlog::debug("[WiFiManager] About to trigger initial scan");
    scan_scheduler_.on_scan_started();
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_pending_ = true; // Mark scan as pending - cleared after first SCAN_COMPLETE processed
    }
    WiFiError scan_result = backend_->trigger_scan();
    if (!scan_result.success()) {
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            scan_pending_ = false;
        }
        // trigger_scan() failed synchronously — no SCAN_COMPLETE event will
        // ever arrive for this attempt, so resolve the scheduler now instead
        // of leaving scan_outstanding_ stuck true forever, which would
        // permanently block should_trigger(). Deliberately on_scan_failed(),
        // NOT on_scan_complete(0, ...): a failed trigger is not evidence the
        // network stopped changing, and feeding it through on_scan_complete
        // would let repeated failures (e.g. a wedged control socket while
        // still associated) drive unchanged_streak_ to 2 and suppress
        // scanning permanently — exactly backwards for the case this exists
        // to diagnose.
        scan_scheduler_.on_scan_failed();
        // Keep the backend's own account of the failure. NOTIFY_WARNING logs
        // only the user-facing string, so before this the actual wpa_supplicant
        // reply never reached the log — the AD5X bundles carry the toast with no
        // way to tell FAIL-BUSY from a wedged control socket.
        spdlog::warn("[WiFiManager] Scan trigger failed: {}", scan_result.technical_msg);
        // If the OS reports the wireless link is actually up, the managed
        // backend simply can't reach its control socket (the link is system-
        // managed and live). Nagging the user with a failure toast is wrong —
        // demote to a debug log (helixscreen#1059).
        if (os_link_up()) {
            spdlog::debug("[WiFiManager] Scan trigger failed but OS link is up "
                          "(system-managed) — suppressing user warning");
        } else if (in_association_grace()) {
            spdlog::debug("[WiFiManager] Scan trigger failed within {}s of an association change "
                          "we initiated — suppressing user warning",
                          ASSOCIATION_GRACE.count());
        } else {
            NOTIFY_WARNING("WiFi scan failed. Try again.");
        }
    } else {
        spdlog::debug("[WiFiManager] Initial scan triggered successfully");
    }
}

void WiFiManager::stop_scan() {
    if (scan_timer_ && lv_is_initialized()) {
        lv_timer_delete(scan_timer_);
        scan_timer_ = nullptr;
        spdlog::info("[WiFiManager] Stopped network scanning");
    }
    // Clear callback to prevent stale callbacks firing after the caller
    // deactivates/destroys (the callback captures a raw overlay pointer).
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_callback_ = nullptr;
    }
}

void WiFiManager::scan_timer_callback(lv_timer_t* timer) {
    WiFiManager* manager = static_cast<WiFiManager*>(lv_timer_get_user_data(timer));
    if (manager && manager->backend_) {
        // Ask the scheduler whether this tick should actually scan: a scan
        // already in flight, or the results having gone stable while
        // connected, both mean "not yet". This runs on the main/LVGL
        // thread (the lv_timer callback), same as scan_scheduler_'s owner.
        if (!manager->scan_scheduler_.should_trigger()) {
            spdlog::trace("[WiFiManager] Periodic scan tick skipped (suppressed={})",
                          manager->scan_scheduler_.suppressed());
            return;
        }
        manager->scan_scheduler_.on_scan_started();

        // Trigger scan - results will arrive via SCAN_COMPLETE event
        {
            std::lock_guard<std::mutex> lock(manager->callback_mutex_);
            manager->scan_pending_ = true; // Mark scan as pending
        }
        WiFiError result = manager->backend_->trigger_scan();
        if (!result.success()) {
            {
                std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                manager->scan_pending_ = false;
            }
            // As in start_scan(): a synchronous trigger failure gets no
            // SCAN_COMPLETE, so resolve the scheduler now (on_scan_failed(),
            // not on_scan_complete(0, ...) — see start_scan()'s comment) or
            // should_trigger() stays false forever.
            manager->scan_scheduler_.on_scan_failed();
            if (manager->scan_timer_) {
                lv_timer_set_period(manager->scan_timer_,
                                    manager->scan_scheduler_.next_interval_ms());
            }
            LOG_WARN_INTERNAL("Periodic scan failed: {}", result.technical_msg);
            // Self-heal: a NOT_INITIALIZED scan means the backend never came up
            // (typically a fresh-boot race where wpa_supplicant's control socket
            // wasn't ready when the constructor first probed). Re-attempt bringup
            // here so the backend recovers on its own once the socket appears,
            // instead of looping "Backend not started" forever (helixscreen#1036).
            if (result.result == WiFiResult::NOT_INITIALIZED) {
                spdlog::info("[WiFiManager] Backend not started — re-attempting bringup");
                manager->backend_->start_async();
            }
        }
    }
}

// ============================================================================
// Connection Management
// ============================================================================

void WiFiManager::connect(const std::string& ssid, const std::string& password,
                          std::function<void(bool success, const std::string& error)> on_complete) {
    if (!backend_) {
        NOTIFY_ERROR("WiFi unavailable. Cannot connect to network.");
        if (on_complete) {
            on_complete(false, "No WiFi backend available");
        }
        return;
    }

    spdlog::info("[WiFiManager] Connecting to '{}'", helix::redact::ssid(ssid));
    // Selecting a network disassociates from the current one; any scan trigger
    // that lands in the gap is collateral, not a user-actionable failure.
    mark_association_change();

    // Drop any grace timer left over from a prior attempt so it can't deliver a stale
    // failure against this new connect (helixscreen#1050). Same for the watchdog: a
    // leftover one would time out this attempt on the previous attempt's schedule.
    cancel_auth_fail_grace();
    cancel_connect_timeout();

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connect_callback_ = on_complete;
        connecting_in_progress_ = true; // Ignore DISCONNECTED events during connection attempt
    }
    spdlog::debug("[WiFiManager] Connect callback registered for '{}'", helix::redact::ssid(ssid));

    // Use backend's connect method
    WiFiError result = backend_->connect_network(ssid, password);
    if (!result.success()) {
        NOTIFY_ERROR("Failed to connect to WiFi network '{}'", helix::redact::ssid(ssid));
        // Clear in-progress + take the callback under the lock, then invoke the
        // local copy OUTSIDE the lock (the callback may re-enter WiFiManager).
        std::function<void(bool, const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            connecting_in_progress_ = false; // Clear on sync failure
            cb = std::move(connect_callback_);
            connect_callback_ = nullptr;
        }
        if (cb) {
            cb(false, result.user_msg.empty() ? result.technical_msg : result.user_msg);
        }
        return; // the callback is already delivered — nothing left for the watchdog to guard
    }
    // Success/failure will be reported via CONNECTED/AUTH_FAILED events — or, when the
    // backend never produces either, by the watchdog armed here.
    start_connect_timeout();
}

void WiFiManager::disconnect() {
    if (!backend_) {
        LOG_WARN_INTERNAL("No backend available for disconnect");
        return;
    }

    spdlog::info("[WiFiManager] Disconnecting");
    mark_association_change();

    // An explicit disconnect aborts whatever connect is in flight. Leaving
    // connecting_in_progress_ set would make handle_disconnected() swallow the very
    // DISCONNECTED this call is about to produce, latching the state machine with no
    // event left that can clear it. The user cancelled deliberately, so the pending
    // callback is dropped rather than invoked with a failure. Both pending resolvers go
    // with it: an armed grace window would otherwise still fire ~4s later against an
    // attempt the user already abandoned.
    cancel_connect_timeout();
    cancel_auth_fail_grace();
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connecting_in_progress_ = false;
        connect_callback_ = nullptr;
    }

    WiFiError result = backend_->disconnect_network();
    if (!result.success()) {
        NOTIFY_WARNING("Could not disconnect from WiFi");
    }
}

void WiFiManager::forget(const std::string& ssid,
                         std::function<void(bool success, const std::string& error)> on_complete) {
    if (!backend_) {
        NOTIFY_ERROR("WiFi unavailable. Cannot forget network.");
        if (on_complete) {
            on_complete(false, "No WiFi backend available");
        }
        return;
    }

    spdlog::info("[WiFiManager] Forgetting '{}'", helix::redact::ssid(ssid));
    // Forgetting the CONNECTED network disassociates as a side effect, and the
    // overlay restarts scanning immediately afterwards.
    mark_association_change();

    WiFiError result = backend_->forget_network(ssid);
    if (!result.success()) {
        // NETWORK_NOT_FOUND is not a failure the user caused — nothing was
        // there to forget, so it does not warrant an error toast the way a
        // genuine backend failure does.
        if (result.result != WiFiResult::NETWORK_NOT_FOUND) {
            // NOTIFY_ERROR ultimately reaches spdlog::error, which is persisted
            // and swept into debug bundles — redact the SSID the same as every
            // other log line in this file.
            NOTIFY_ERROR("Failed to forget WiFi network '{}'", helix::redact::ssid(ssid));
        }
        if (on_complete) {
            on_complete(false, result.user_msg.empty() ? result.technical_msg : result.user_msg);
        }
        return;
    }

    if (on_complete) {
        on_complete(true, "");
    }
}

// ============================================================================
// Status Queries
// ============================================================================

bool WiFiManager::is_connected() {
    if (!backend_)
        return false;

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.connected;
}

std::string WiFiManager::get_connected_ssid() {
    if (!backend_)
        return "";

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.ssid;
}

std::string WiFiManager::get_ip_address() {
    if (!backend_)
        return "";

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.ip_address;
}

std::string WiFiManager::get_mac_address() {
    if (!backend_)
        return "";

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.mac_address;
}

int WiFiManager::get_signal_strength() {
    if (!backend_)
        return 0;

    WifiBackend::ConnectionStatus status = backend_->get_status();
    return status.signal_strength;
}

bool WiFiManager::supports_5ghz() {
    if (!backend_)
        return false;

    return backend_->supports_5ghz();
}

// ============================================================================
// Hardware Detection (Legacy Compatibility)
// ============================================================================

bool WiFiManager::has_hardware() {
    // Backend creation handles hardware availability
    return (backend_ != nullptr);
}

bool WiFiManager::is_enabled() {
    if (!backend_)
        return false;
    return backend_->is_running() && backend_->is_radio_enabled();
}

WiFiError WiFiManager::apply_radio_enabled(bool enabled) {
    if (!backend_) {
        return WiFiError(WiFiResult::NOT_INITIALIZED, "No WiFi backend", "WiFi system not ready");
    }

    // Radio on/off, NOT backend start()/stop(). stop() tears down the control
    // connection to wpa_supplicant entirely, so a subsequent STATUS has
    // nothing to query — every later call logs "send_command called but not
    // connected to wpa_supplicant" and the UI can't even report the radio is
    // off, let alone turn it back on. set_radio_enabled() actually asserts
    // the interface down (or blocks it via rfkill) while keeping the control
    // socket alive.
    return backend_->set_radio_enabled(enabled);
}

void WiFiManager::report_radio_result(bool enabled, const WiFiError& result) {
    if (result.success()) {
        spdlog::debug("[WiFiManager] WiFi radio {}", enabled ? "enabled" : "disabled");
        return;
    }

    // A live system-managed link (printer reachable by IP) means the
    // backend just can't reach wpa_supplicant's control socket; the
    // radio is not actually off. Don't surface a hard error toast for
    // an unmanageable-but-up link (helixscreen#1059).
    if (os_link_up()) {
        spdlog::debug("[WiFiManager] Radio {} failed but OS link is up "
                      "(system-managed) — suppressing user error: {}",
                      enabled ? "enable" : "disable",
                      result.user_msg.empty() ? result.technical_msg : result.user_msg);
    } else {
        NOTIFY_ERROR("Failed to {} WiFi: {}", enabled ? "enable" : "disable",
                     result.user_msg.empty() ? result.technical_msg : result.user_msg);
    }
}

bool WiFiManager::set_enabled(bool enabled) {
    if (!backend_)
        return false;

    spdlog::debug("[WiFiManager] set_enabled({})", enabled);

    if (!enabled) {
        // Stop scanning — the timer must not keep firing trigger_scan()
        // against a radio we just turned off.
        stop_scan();
    }

    WiFiError result = apply_radio_enabled(enabled);
    report_radio_result(enabled, result);
    return result.success();
}

void WiFiManager::set_enabled_async(bool enabled, helix::LifetimeToken token,
                                    std::function<void(bool, bool)> on_complete) {
    spdlog::debug("[WiFiManager] set_enabled_async({})", enabled);

    if (!backend_) {
        // Still answer off the call stack, so callers see one async contract.
        if (on_complete) {
            token.defer("WiFiManager::set_enabled_async_no_backend",
                        [cb = std::move(on_complete)]() { cb(false, false); });
        }
        return;
    }

    // stop_scan() deletes an lv_timer_t and mutates the ScanScheduler, both of
    // which are main-thread-only state. Do it here (this method is called from
    // the UI thread) rather than on the worker.
    if (!enabled) {
        stop_scan();
    }

    // Idempotent; covers unit tests and any call site that runs before
    // Application calls HttpExecutor::start_all().
    helix::http::HttpExecutor::fast().start();

    // Guards the toast below against the manager outliving neither more nor
    // less than it should: taken on the main thread, where `this` is valid.
    auto mgr_token = async_lifetime_.token();

    {
        std::lock_guard<std::mutex> lock(radio_op_mutex_);
        ++radio_ops_inflight_;
    }

    // Route through HttpExecutor::fast() (bounded 4-worker pool) rather than a
    // detached std::thread — per-call spawns fail with pthread EAGAIN under
    // thread exhaustion on memory-constrained ARM devices (#724).
    helix::http::HttpExecutor::fast().submit(
        [this, enabled, token, mgr_token, cb = std::move(on_complete)]() mutable {
            // `this` is valid for the whole body: ~WiFiManager blocks on
            // radio_op_cv_ until radio_ops_inflight_ drains, before it touches a
            // single member.
            WiFiError result = apply_radio_enabled(enabled);
            const bool success = result.success();
            const bool actual = backend_->is_running() && backend_->is_radio_enabled();

            // Neither deferred body dereferences `this`: report_radio_result is
            // static, and the caller's lambda carries its own captures. That keeps
            // both safe even if the manager is destroyed between here and the next
            // UpdateQueue tick.
            mgr_token.defer("WiFiManager::report_radio_result",
                            [enabled, result]() { report_radio_result(enabled, result); });
            if (cb) {
                token.defer("WiFiManager::set_enabled_async",
                            [cb = std::move(cb), success, actual]() { cb(success, actual); });
            }

            {
                // Notify under the lock, not after it. wait_for_radio_ops() wakes
                // as soon as the count reaches zero, and ~WiFiManager() destroys
                // radio_op_cv_ right after it returns -- which would be while this
                // thread was still inside notify_all(). Holding the mutex across
                // the notify keeps the waiter blocked on reacquiring it until we
                // are done touching the condition variable.
                std::lock_guard<std::mutex> lock(radio_op_mutex_);
                --radio_ops_inflight_;
                radio_op_cv_.notify_all();
            }
        });
}

void WiFiManager::wait_for_radio_ops() {
    std::unique_lock<std::mutex> lock(radio_op_mutex_);
    if (radio_ops_inflight_ == 0) {
        return;
    }
    spdlog::debug("[WiFiManager] Waiting for {} in-flight radio op(s) before teardown",
                  radio_ops_inflight_);
    radio_op_cv_.wait(lock, [this] { return radio_ops_inflight_ == 0; });
}

void WiFiManager::retry_async() {
    if (!backend_) {
        return;
    }
    spdlog::debug("[WiFiManager] Re-attempting backend bringup (retry_async)");
    backend_->start_async();
}

// ============================================================================
// Event Handling
// ============================================================================

// Helper struct for async callback dispatch
// Uses weak_ptr to safely handle manager destruction before callback executes
struct ScanCallbackData {
    std::weak_ptr<WiFiManager> manager;
    std::vector<WiFiNetwork> networks;
};

void WiFiManager::handle_scan_complete(const std::string& event_data) {
    (void)event_data; // Unused for now

    spdlog::debug("[WiFiManager] handle_scan_complete ENTRY (backend thread)");

    // Debounce: wpa_supplicant can emit duplicate SCAN_RESULTS events
    // Only process the first one per scan cycle. This runs on the backend thread,
    // so the flag/callback reads are serialized against the main thread via
    // callback_mutex_.
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (!scan_pending_) {
            spdlog::trace("[WiFiManager] Ignoring duplicate SCAN_COMPLETE (already processed)");
            return;
        }
        scan_pending_ = false; // Clear flag - subsequent events for this scan will be ignored

        if (!scan_callback_) {
            LOG_WARN_INTERNAL("Scan complete but no callback registered");
            return;
        }
    }

    // CRITICAL: This is called from backend thread - must dispatch to LVGL thread!
    spdlog::debug("[WiFiManager] Scan callback is registered, fetching results");
    std::vector<WiFiNetwork> networks;
    WiFiError result = backend_->get_scan_results(networks);

    if (result.success()) {
        spdlog::debug("[WiFiManager] Got {} scan results, dispatching to LVGL thread",
                      networks.size());

        // Use RAII-safe async callback wrapper
        helix::ui::queue_update<ScanCallbackData>(
            std::make_unique<ScanCallbackData>(ScanCallbackData{self_, networks}),
            [](ScanCallbackData* data) {
                spdlog::debug("[WiFiManager] async_call executing in LVGL thread with {} networks",
                              data->networks.size());

                // Safely check if manager still exists
                if (auto manager = data->manager.lock()) {
                    // ScanScheduler is main/LVGL-thread-only state; this lambda
                    // runs there (dispatched via queue_update), so touching it
                    // directly is safe. Feed the scan cadence policy the result
                    // count and current connection state, then apply whatever
                    // interval it decides on to the live timer.
                    manager->scan_scheduler_.on_scan_complete(data->networks.size(),
                                                              manager->is_connected());
                    if (manager->scan_timer_) {
                        lv_timer_set_period(manager->scan_timer_,
                                            manager->scan_scheduler_.next_interval_ms());
                    }

                    std::function<void(const std::vector<WiFiNetwork>&)> cb;
                    {
                        std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                        cb = manager->scan_callback_;
                    }
                    if (cb) {
                        cb(data->networks);
                        spdlog::debug("[WiFiManager] scan_callback_ completed successfully");
                    } else {
                        spdlog::warn(
                            "[WiFiManager] scan_callback_ was cleared before async dispatch");
                    }
                } else {
                    spdlog::debug(
                        "[WiFiManager] Manager destroyed before async callback - safely ignored");
                }
            });

    } else {
        LOG_WARN_INTERNAL("Failed to get scan results: {}", result.technical_msg);

        // Use RAII-safe async callback wrapper
        helix::ui::queue_update<ScanCallbackData>(
            std::make_unique<ScanCallbackData>(ScanCallbackData{self_, {}}),
            [](ScanCallbackData* data) {
                LOG_WARN_INTERNAL("async_call: calling callback with empty results");
                if (auto manager = data->manager.lock()) {
                    // Couldn't fetch results even though the trigger/scan
                    // itself succeeded — still an unresolved attempt, not
                    // evidence the network is stable. on_scan_failed(), not
                    // on_scan_complete(0, ...) — see start_scan()'s comment
                    // for why folding this into a zero-result complete would
                    // corrupt the suppression state machine.
                    manager->scan_scheduler_.on_scan_failed();
                    if (manager->scan_timer_) {
                        lv_timer_set_period(manager->scan_timer_,
                                            manager->scan_scheduler_.next_interval_ms());
                    }

                    std::function<void(const std::vector<WiFiNetwork>&)> cb;
                    {
                        std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                        cb = manager->scan_callback_;
                    }
                    if (cb) {
                        cb({});
                    }
                } else {
                    spdlog::debug(
                        "[WiFiManager] Manager destroyed before async callback - safely ignored");
                }
            });
    }

    spdlog::debug("[WiFiManager] handle_scan_complete EXIT (dispatch queued)");
}

// Helper struct for connection callback dispatch
// Uses weak_ptr to safely handle manager destruction before callback executes
struct ConnectCallbackData {
    std::weak_ptr<WiFiManager> manager;
    bool success;
    std::string error;
};

void WiFiManager::handle_connected(const std::string& event_data) {
    (void)event_data; // Could parse IP address from event data

    spdlog::debug("[WiFiManager] Connected event received (backend thread)");

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connecting_in_progress_ = false; // Connection complete
    }

    // Fan out to passive UI observers regardless of whether there's an active
    // connect() callback — the home-panel network widget depends on this to
    // learn the initial post-boot connection state.
    notify_state_observers();

    bool has_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        has_callback = static_cast<bool>(connect_callback_);
    }
    if (!has_callback) {
        spdlog::debug(
            "[WiFiManager] Connected event but no callback registered (normal on startup)");
        return;
    }

    // Use RAII-safe async callback wrapper
    helix::ui::queue_update<ConnectCallbackData>(
        std::make_unique<ConnectCallbackData>(ConnectCallbackData{self_, true, ""}),
        [](ConnectCallbackData* d) {
            if (auto manager = d->manager.lock()) {
                // A transient AUTH_FAILED may be sitting in the grace window — this
                // CONNECTED is the real outcome, so cancel it before delivering success
                // (helixscreen#1050).
                manager->cancel_auth_fail_grace();
                // The attempt resolved — disarm the watchdog before delivering success.
                manager->cancel_connect_timeout();
                std::function<void(bool, const std::string&)> cb;
                {
                    std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                    cb = std::move(manager->connect_callback_);
                    manager->connect_callback_ = nullptr;
                }
                if (cb) {
                    cb(d->success, d->error);
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before connect callback - safely ignored");
            }
        });
}

void WiFiManager::handle_disconnected(const std::string& event_data) {
    (void)event_data; // Could parse reason from event data

    spdlog::debug("[WiFiManager] Disconnected event received (backend thread)");

    // During a connection attempt, wpa_supplicant fires DISCONNECTED before CONNECTED
    // when switching networks. Ignore DISCONNECTED during connection - only AUTH_FAILED
    // or subsequent CONNECTED should determine success/failure.
    bool in_progress;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        in_progress = connecting_in_progress_;
    }
    if (in_progress) {
        spdlog::debug("[WiFiManager] Ignoring DISCONNECTED during connection attempt");
        return;
    }

    // Genuine disconnect — wake passive observers so they can refresh UI state.
    notify_state_observers();

    // A new network (or no network) is a new environment worth re-learning,
    // so drop any backoff/suppression from the last one. ScanScheduler is
    // main/LVGL-thread-only state and handle_disconnected() runs on the
    // backend thread, so dispatch rather than touching it here.
    //
    // Only bother if self_ is wired (set once by init_self_reference() and
    // never mutated again, so reading it here unsynchronized is the same
    // established pattern ScanCallbackData/ConnectCallbackData already rely
    // on). Without it the queued lambda can never resolve to a live manager
    // anyway — and skipping the enqueue matters concretely: backend_->stop()
    // in the destructor fires this same DISCONNECTED path synchronously,
    // with scan_timer_ already torn down and self_ either never set (tests that
    // construct WiFiManager directly) or already expired (the weak self-
    // reference goes flat the moment refcount hits zero), so an unconditional
    // enqueue there outlives the test with nothing left to drain it — an
    // UpdateQueue isolation leak.
    // std::weak_ptr::expired(), not LifetimeToken::expired(), so there is no
    // TOCTOU on `this`: the backend thread running this handler is joined by
    // backend_->stop() before ~WiFiManager returns, and the member access the
    // gate sees is inside the queued lambda's weak_self.lock() on the main thread.
    if (!self_.expired()) { // L081_OK: std::weak_ptr expiry, not a LifetimeToken
        std::weak_ptr<WiFiManager> weak_self = self_;
        helix::ui::queue_update("WiFiManager::handle_disconnected(scan_scheduler)", [weak_self]() {
            if (auto manager = weak_self.lock()) {
                manager->scan_scheduler_.on_disconnected();
                if (manager->scan_timer_) {
                    lv_timer_set_period(manager->scan_timer_,
                                        manager->scan_scheduler_.next_interval_ms());
                }
            }
        });
    }

    bool has_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        has_callback = static_cast<bool>(connect_callback_);
    }
    if (!has_callback) {
        spdlog::debug("[WiFiManager] Disconnected event but no callback registered (normal)");
        return;
    }

    // Use RAII-safe async callback wrapper
    helix::ui::queue_update<ConnectCallbackData>(
        std::make_unique<ConnectCallbackData>(ConnectCallbackData{self_, false, "Disconnected"}),
        [](ConnectCallbackData* d) {
            if (auto manager = d->manager.lock()) {
                std::function<void(bool, const std::string&)> cb;
                {
                    std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                    cb = std::move(manager->connect_callback_);
                    manager->connect_callback_ = nullptr;
                }
                if (cb) {
                    cb(d->success, d->error);
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before disconnect callback - safely ignored");
            }
        });
}

void WiFiManager::handle_auth_failed(const std::string& event_data) {
    spdlog::warn("[WiFiManager] Authentication failed event received (backend thread)");

    // Do NOT clear connecting_in_progress_ or deliver the failure synchronously here.
    // wpa_supplicant emits a transient CTRL-EVENT-SSID-TEMP-DISABLED/WRONG_KEY mid-handshake
    // on some adapters even when the connect ultimately succeeds — a CONNECTED follows ~1-3s
    // later (helixscreen#1050). Staying "in progress" keeps any interleaved DISCONNECTED
    // ignored, and the failure is armed on a grace timer that a CONNECTED can preempt.
    bool has_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        has_callback = static_cast<bool>(connect_callback_);
    }
    if (!has_callback) {
        LOG_WARN_INTERNAL("Auth failed event but no callback registered");
        return;
    }

    // Pass through backend detail if provided, otherwise generic message
    std::string error_msg = event_data.empty() ? "Authentication failed" : event_data;

    // Use RAII-safe async callback wrapper
    helix::ui::queue_update<ConnectCallbackData>(
        std::make_unique<ConnectCallbackData>(
            ConnectCallbackData{self_, false, std::move(error_msg)}),
        [](ConnectCallbackData* d) {
            if (auto manager = d->manager.lock()) {
                // Only arm the grace window if a connect is still pending; a CONNECTED that
                // already resolved it nulls connect_callback_.
                bool still_pending;
                {
                    std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                    still_pending = static_cast<bool>(manager->connect_callback_);
                }
                if (still_pending) {
                    manager->start_auth_fail_grace(d->error);
                }
            } else {
                spdlog::debug(
                    "[WiFiManager] Manager destroyed before auth_failed callback - safely ignored");
            }
        });
}

// ----------------------------------------------------------------------------
// Auth-failure debounce (helixscreen#1050) — UI thread only
// ----------------------------------------------------------------------------

namespace {
// Window to wait after a transient AUTH_FAILED for a CONNECTED to arrive. wpa_supplicant's
// TEMP-DISABLED -> CONNECTED gap is typically 1-3s; allow margin for slow embedded adapters.
// A genuine wrong password (no CONNECTED) surfaces the failure after this delay.
constexpr uint32_t AUTH_FAIL_GRACE_MS = 4000;
} // namespace

void WiFiManager::start_auth_fail_grace(const std::string& error) {
    pending_auth_error_ = error;
    // The grace window now owns resolving this attempt (either a CONNECTED preempts it or
    // deliver_auth_failure() reports the failure), so the watchdog must stand down.
    cancel_connect_timeout();
    if (auth_fail_grace_timer_) {
        lv_timer_delete(auth_fail_grace_timer_);
        auth_fail_grace_timer_ = nullptr;
    }
    auth_fail_grace_timer_ = lv_timer_create(auth_fail_grace_timer_cb, AUTH_FAIL_GRACE_MS, this);
    lv_timer_set_repeat_count(auth_fail_grace_timer_, 1); // one-shot
    spdlog::debug("[WiFiManager] AUTH_FAILED deferred {}ms pending possible CONNECTED",
                  AUTH_FAIL_GRACE_MS);
}

void WiFiManager::cancel_auth_fail_grace() {
    if (auth_fail_grace_timer_) {
        // Reached from handle_connected() (a CONNECTED preempted the failure, so it was
        // a transient handshake error), from connect() (a new attempt supersedes it), and
        // from disconnect() (the user abandoned the attempt). Kept neutral for all three.
        spdlog::debug("[WiFiManager] Pending AUTH_FAILED grace window cancelled");
        lv_timer_delete(auth_fail_grace_timer_);
        auth_fail_grace_timer_ = nullptr;
    }
    pending_auth_error_.clear();
}

void WiFiManager::deliver_auth_failure() {
    // Grace window elapsed with no CONNECTED — the authentication failure is real.
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connecting_in_progress_ = false;
    }
    notify_state_observers();
    std::function<void(bool, const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = std::move(connect_callback_);
        connect_callback_ = nullptr;
    }
    if (cb) {
        cb(false, pending_auth_error_);
    }
    pending_auth_error_.clear();
}

void WiFiManager::auth_fail_grace_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<WiFiManager*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }
    // One-shot: LVGL deletes the timer after this callback returns, so just drop our handle.
    self->auth_fail_grace_timer_ = nullptr;
    self->deliver_auth_failure();
}

// ----------------------------------------------------------------------------
// Connect watchdog — UI thread only
// ----------------------------------------------------------------------------

namespace {
// Upper bound on how long a connect may stay pending with no terminal event. A
// wpa_supplicant association plus DHCP on a slow embedded radio can legitimately take
// ~30s, so anything tighter would abort connects that were about to succeed; 45s clears
// that comfortably without leaving the user staring at a spinner that never resolves.
constexpr uint32_t CONNECT_TIMEOUT_MS = 45000;
} // namespace

void WiFiManager::start_connect_timeout() {
    cancel_connect_timeout();
    connect_timeout_timer_ = lv_timer_create(connect_timeout_timer_cb, CONNECT_TIMEOUT_MS, this);
    lv_timer_set_repeat_count(connect_timeout_timer_, 1); // one-shot
    spdlog::debug("[WiFiManager] Connect watchdog armed ({}ms)", CONNECT_TIMEOUT_MS);
}

void WiFiManager::cancel_connect_timeout() {
    if (connect_timeout_timer_) {
        helix::ui::lv_timer_cancel_safe(connect_timeout_timer_);
        connect_timeout_timer_ = nullptr;
    }
}

void WiFiManager::deliver_connect_timeout() {
    // Neither CONNECTED nor AUTH_FAILED ever arrived. Resolve the attempt as a failure so
    // the caller stops waiting, and clear connecting_in_progress_ so a later DISCONNECTED
    // is no longer swallowed by handle_disconnected().
    bool was_pending = false;
    std::function<void(bool, const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        was_pending = connecting_in_progress_ || static_cast<bool>(connect_callback_);
        if (was_pending) {
            connecting_in_progress_ = false;
            cb = std::move(connect_callback_);
            connect_callback_ = nullptr;
        }
    }
    if (!was_pending) {
        return; // resolved between the timer firing and this callback running
    }

    LOG_WARN_INTERNAL("Connect produced no terminal event within {}ms — reporting timeout",
                      CONNECT_TIMEOUT_MS);
    notify_state_observers();
    // Invoke OUTSIDE callback_mutex_ — the callback re-enters WiFiManager.
    if (cb) {
        cb(false, lv_tr("Connection timeout"));
    }
}

void WiFiManager::connect_timeout_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<WiFiManager*>(lv_timer_get_user_data(timer));
    if (!self) {
        return;
    }
    // One-shot: LVGL deletes the timer after this callback returns, so just drop our handle.
    self->connect_timeout_timer_ = nullptr;
    self->deliver_connect_timeout();
}

// ============================================================================
// State Observers
// ============================================================================

void WiFiManager::add_state_observer(helix::LifetimeToken token, std::function<void()> on_change) {
    if (!on_change) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_observers_mutex_);
    state_observers_.push_back({std::move(token), std::move(on_change)});
}

void WiFiManager::notify_state_observers() {
    // Snapshot under lock, then invoke outside the lock so defer() — and any
    // work it kicks off on the UI thread — can't call back into us while we
    // hold state_observers_mutex_. Drop expired entries as we go; the backend
    // callback threads are where this runs, so a bit of cleanup here is fine.
    std::vector<StateObserver> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_observers_mutex_);
        // L081_OK: sweeping *observers'* tokens to drop dead entries, not
        // gating our own `this` access — structurally not Mechanism C, and
        // the backend fires READY from its init worker on nearly every
        // launch, which made this the single largest bg_tok_expired_check
        // emitter in the field.
        state_observers_.erase(
            std::remove_if(state_observers_.begin(), state_observers_.end(),
                           [](const StateObserver& obs) { return obs.token.expired_no_lvgl(); }),
            state_observers_.end());
        snapshot = state_observers_;
    }
    for (const auto& obs : snapshot) {
        obs.token.defer("WiFiManager::state_observer", obs.callback);
    }
}

// ============================================================================
// Shared Singleton Instance
// ============================================================================

// Global shared WiFiManager instance.
//
// DELIBERATELY NEVER DESTROYED. The holder is heap-allocated and never freed, so
// the manager it owns outlives static destruction instead of racing it. A plain
// static shared_ptr would release its last use from __run_exit_handlers, running
// ~WiFiManager -> backend_->stop() -> spdlog::info() after spdlog's own static
// sinks have been destroyed: a use-after-free that segfaults inside
// sink::should_log(). The manager is process-lifetime state that the app never
// tears down on purpose, so leaking it at exit is the intended trade -- the same
// reason the destructor reports through fprintf rather than spdlog.
//
// This is NOT a licence for the class to leak generally: a WiFiManager built by
// anyone else (every test fixture, for one) is still owned by its caller and
// destroyed normally, which is what self_ being a weak_ptr buys.
static std::shared_ptr<WiFiManager>& shared_wifi_manager() {
    static auto* holder = new std::shared_ptr<WiFiManager>();
    return *holder;
}
static std::mutex g_wifi_manager_mutex;

namespace helix {

std::shared_ptr<WiFiManager> get_wifi_manager() {
    std::lock_guard<std::mutex> lock(g_wifi_manager_mutex);

    auto& instance = shared_wifi_manager();
    if (!instance) {
        spdlog::debug("[WiFiManager] Creating global instance");
        // Use silent=true for global instance since it's used for passive status monitoring
        // (e.g., home panel WiFi icon). Avoids modal popup when WiFi hardware is unavailable
        // on development machines or when WiFi is simply turned off.
        instance = std::make_shared<WiFiManager>(/*silent=*/true);
        instance->init_self_reference(instance);
    }

    return instance;
}

} // namespace helix
