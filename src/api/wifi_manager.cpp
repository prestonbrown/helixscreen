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

#include "config.h"
#include "http_executor.h"
#include "log_redact.h"
#include "lvgl/lvgl.h"
#include "safe_log.h"
#include "spdlog/spdlog.h"
#include "system_settings_manager.h"
#include "wifi_country.h"
#include "wifi_interface.h"
#include "wifi_radio_toggle.h"
#include "wifi_ui_utils.h"

#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)
// The wpa_supplicant fallback (handle_init_failed, below) is a
// Linux-desktop-only concern — ESP32 has a single esp_wifi backend with no
// fallback path (see wifi_backend_esp.cpp).
#include "wifi_backend_networkmanager.h"
#include "wifi_backend_wpa_supplicant.h"
#endif

#include <algorithm>
#include <atomic>
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
    return (std::chrono::steady_clock::now() - last_association_change_) < kAssociationGrace;
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
            NOTIFY_ERROR_MODAL(
                lv_tr("WiFi Unavailable"), "{}",
                lv_tr("Could not initialize WiFi hardware. Check system configuration."));
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
            NOTIFY_ERROR_MODAL(
                lv_tr("WiFi Unavailable"), "{}",
                lv_tr("Could not initialize WiFi hardware. Check system configuration."));
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
    backend_->register_event_callback("REGDOM_CHANGE", [this](const std::string& data) {
        // Fired on the backend's event thread; scanning is LVGL-thread state.
        async_lifetime_.defer("WiFiManager::regdom_change",
                              [this, data]() { handle_regdom_change(data); });
    });
    backend_->register_event_callback("READY", [this](const std::string&) {
        spdlog::debug("[WiFiManager] Backend READY event received");
        // Wake UI consumers that queried status before async init landed —
        // NetworkWidget in particular attaches synchronously during home-panel
        // load, races the backend's worker thread, and gets an empty STATUS
        // response that pins it on 'Disconnected' until this event lands.
        notify_state_observers(/*country_may_have_changed=*/true);

        // A radio the user switched off must not come back on because the
        // process restarted. Reassert the stored choice once the backend can
        // act on it. READY fires on a background thread (the backend's init
        // worker — see the INIT_FAILED handling above), and both
        // get_wifi_enabled() (reads an lv_subject_t) and set_radio_enabled()
        // must not run there, so defer through async_lifetime_ the same way
        // the wpa_supplicant fallback above does.
        //
        // CC1 incident (Task 15): on a device whose only network path is this
        // radio, reasserting "off" is a one-way door — a reboot clears the
        // rfkill soft-block, but the watchdog restarts HelixScreen, which reads
        // the stored setting and blocks the radio again. The device can never
        // be reached remotely again. Only reassert "off" when a non-WiFi
        // network path (wired or otherwise) is actually up. When resolution of
        // the WiFi interface itself is inconclusive, fail safe: treat it the
        // same as "no known wired fallback" and do not disable the radio.
        async_lifetime_.defer("WiFiManager::apply_stored_country",
                              [this]() { apply_stored_country(); });
        async_lifetime_.defer("WiFiManager::reassert_stored_radio_state", [this]() {
            if (backend_ && !backend_->supports_radio_toggle()) {
                // The radio belongs to the printer's network daemon: there is
                // no "off" to reassert, and no interface resolution to make
                // the stranding check meaningful. Skipping the whole block
                // matters because of its ELSE branch — unable to apply a
                // stored "off", it corrects the stored value to "on" so the
                // UI cannot show a lie. On a radio nobody here controls that
                // correction is itself the lie, and it silently overwrites a
                // choice the user made under an older release on the first
                // boot after upgrading.
                spdlog::debug("[WiFiManager] Backend does not control the radio — leaving the "
                              "stored WiFi setting untouched");
                return;
            }
            const bool want_on = SystemSettingsManager::instance().get_wifi_enabled();
            // A soft-blocked radio on a machine where HelixScreen never
            // configured WiFi is the administrator's decision (rfkill block,
            // `nmcli radio wifi off`), not a stale block from a previous run.
            // wifi_expected is written only by the wizard and the
            // network-settings toggle, so false means no HelixScreen WiFi
            // configuration exists to justify clearing a block against
            // (#1697). This gates only the block-CLEARING direction; the
            // reassert-off and stranding refusal below are untouched —
            // declining to clear a block cannot strand a device.
            const bool wifi_configured = Config::get_instance()->is_wifi_expected();
            // The radio change itself is control traffic: it goes down the lane.
            auto set_radio = [this](bool on) {
                submit_control("reassert_radio", ControlKind::User,
                               [this, on]() { backend_->set_radio_enabled(on); });
            };
            if (!want_on && backend_) {
                const auto iface = backend_->resolved_interface();
                const bool safe_to_disable =
                    iface.has_value() &&
                    helix::wifi::detail::has_non_wifi_network_path(sys_root_, iface->netdev);

                if (safe_to_disable) {
                    spdlog::info("[WiFiManager] Stored setting is WiFi off — reasserting");
                    set_radio(false);
                } else if (!wifi_configured && !backend_->is_radio_enabled()) {
                    // No HelixScreen WiFi configuration and the radio is
                    // soft-blocked: correcting the stored setting to "on"
                    // over a blocked radio would be the same class of state
                    // lie the correction below exists to eliminate. Touch
                    // neither the radio nor the stored setting.
                    spdlog::info("[WiFiManager] Stored setting is WiFi off and the radio is "
                                 "soft-blocked, but WiFi was never configured in HelixScreen "
                                 "— leaving both untouched");
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
                        set_radio(true);
                    }
                }
            } else if (want_on && backend_ && !backend_->is_radio_enabled()) {
                if (!wifi_configured) {
                    // The stored default is on but WiFi was never configured
                    // here — the block belongs to the administrator, and
                    // HelixScreen must not clear it at every boot (#1697).
                    spdlog::info("[WiFiManager] Radio is soft-blocked but WiFi was never "
                                 "configured in HelixScreen — leaving the block in place");
                    return;
                }
                // Mirror image of the off-reassert above: the stored setting
                // is on, but the radio itself is soft-blocked — e.g. a stale
                // rfkill soft-block from a previous run, or one this same
                // process created, since a soft block found at startup does
                // not abort init. is_radio_enabled() is seeded from hardware
                // (<rfkill>/soft) during resolve_and_store_interface(), so it
                // reflects reality here rather than a hopeful default. Left
                // uncleared, the radio stays dead until someone physically
                // taps the touchscreen (Task 15, CC1) — clear the stale
                // block automatically instead.
                spdlog::info("[WiFiManager] Stored setting is WiFi on, but the radio was "
                             "soft-blocked — clearing the stale block to match the stored "
                             "preference");
                set_radio(true);
            }
        });
    });
}

void WiFiManager::handle_init_failed(bool silent, const std::string& msg) {
    // On Linux, a backend that cannot initialize is worth replacing with
    // wpa_supplicant only when wpa_supplicant can actually take the hardware
    // over. That is the backend's question to answer, never this function's:
    // a dormant NetworkManager install holds nothing, while a printer network
    // daemon holds the radio, the supplicant and DHCP even when our client
    // connection to it fails. Starting wpa_supplicant against the second case
    // puts two clients on one radio, which is worse than no WiFi.
    // tried_fallback_ bounds this to a single swap, so a wpa_supplicant that
    // also fails cannot loop.
#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)
    if (!tried_fallback_ && backend_ && backend_->supports_wpa_supplicant_fallback()) {
        tried_fallback_ = true;
        spdlog::warn("[WiFiManager] WiFi backend INIT_FAILED ({}); "
                     "falling back to wpa_supplicant",
                     msg);
        // CRITICAL: INIT_FAILED fires from inside the backend's init worker
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
            // Calls still queued were meant for the failed backend: drop them
            // after the one in progress rather than make the swap wait on them.
            {
                std::lock_guard<std::mutex> lock(control_mutex_);
                control_paused_ = true;
            }
            swap_to_wpa_supplicant(silent);
        });
        return;
    }
#endif
    // Backend initialization failed asynchronously - notify user (unless silent)
    if (!silent) {
        // INIT_FAILED fires on the backend's init worker; the toast is main-thread only.
        helix::ui::queue_update("WiFiManager::init_failed_toast", [msg]() {
            NOTIFY_ERROR(lv_tr("WiFi initialization failed: {}"), msg);
        });
    } else {
        spdlog::debug("[WiFiManager] WiFi init failed (silent): {}", msg);
    }
}

void WiFiManager::swap_to_wpa_supplicant(bool silent) {
#if !defined(__APPLE__) && !defined(__ANDROID__) && !defined(ESP_PLATFORM)
    // Never wait here: this is the LVGL thread, and a call in progress on a
    // wedged daemon does not return. Look again later instead.
    if (!wait_for_radio_ops_for(std::chrono::milliseconds(0))) {
        after_ms(100, [this, silent]() { swap_to_wpa_supplicant(silent); });
        return;
    }
    backend_->stop();
    // The stopped backend can never deliver the SCAN_COMPLETE a
    // successful trigger_scan() owed, so resolve the scheduler now —
    // the same resolution the synchronous-failure paths in start_scan()
    // and scan_timer_callback() use. Without it, a scan outstanding at
    // the swap latches should_trigger() false for the rest of this
    // scan session (prestonbrown/helixscreen#1405). The contract and
    // its one exception (stop) are stated on WifiBackend::trigger_scan.
    scan_scheduler_.on_scan_failed();
    backend_.reset();
    backend_ = std::make_unique<WifiBackendWpaSupplicant>();
    backend_->set_silent(silent);
    {
        // The strikes belong to the backend that failed; the new one starts
        // trusted, or an open breaker would refuse every user op against it.
        std::lock_guard<std::mutex> lock(breaker_mutex_);
        breaker_ = wifi::ControlBreaker(breaker_.config());
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        control_paused_ = false;
    }
    register_backend_callbacks(silent);
    backend_->start_async();
#else
    (void)silent;
#endif
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

    // A lane worker may be inside a backend call. Nothing below (least of all
    // backend_.reset()) may run while it is, and what is still queued behind
    // it is dropped rather than run against a manager on its way out. The wait
    // is unbounded: the worker uses this object until its call returns. Only
    // tests destroy a WiFiManager; the app's one instance is never freed
    // (get_wifi_manager(), and the owner gate in tests/shell/test_code_lint.bats).
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        control_shutdown_ = true;
    }
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
    helix::SharedCallback<void(const std::vector<WiFiNetwork>&)> on_networks_updated) {
    if (!backend_) {
        NOTIFY_ERROR("{}", lv_tr("WiFi unavailable. Cannot scan for networks."));
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
                 helix::wifi::ScanScheduler::kBaseIntervalMs,
                 helix::wifi::ScanScheduler::kMaxIntervalMs);

    // A fresh scan session (e.g. the user opening network settings) is a
    // manual refresh: clear any suppression/backoff left over from a prior
    // session and start this one's timer at the base interval. Touching
    // scan_scheduler_ directly is safe because start_scan() is LVGL-thread-only
    // — it creates the scan lv_timer below, and stop_scan() deletes it. Callers
    // on any other thread must marshal via helix::ui::queue_update().
    scan_scheduler_.on_user_refresh();

    // Create timer for periodic scanning
    scan_timer_ =
        lv_timer_create(scan_timer_callback, helix::wifi::ScanScheduler::kBaseIntervalMs, this);
    spdlog::debug("[WiFiManager] Timer created: {}", (void*)scan_timer_);

    // Trigger immediate scan. A trigger still in flight from before stop_scan()
    // serves this session too: its SCAN_COMPLETE reaches the callback just set.
    // The caller is waiting on its callback to clear a "scanning" state, so it
    // is answered whatever becomes of the trigger.
    user_scan_waiting_ = true;
    const uint64_t generation = ++scan_wait_generation_;
    if (request_scan(/*user_refresh=*/true) == ScanRequest::Refused) {
        async_lifetime_.defer("WiFiManager::scan_refused", [this]() { answer_scan_waiter(); });
    } else {
        watch_scan_waiter(generation);
    }
}

void WiFiManager::answer_scan_waiter() {
    if (!user_scan_waiting_) {
        return;
    }
    user_scan_waiting_ = false;
    ++scan_wait_generation_;
    deliver_last_scan_results();
}

void WiFiManager::deliver_last_scan_results() {
    helix::SharedCallback<void(const std::vector<WiFiNetwork>&)> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = scan_callback_;
    }
    // The last list, not an empty one: nothing new was learned, and an empty
    // list would wipe the networks the user is looking at.
    if (cb) {
        cb(last_scan_results_);
    }
}

void WiFiManager::watch_scan_waiter(uint64_t generation) {
    uint32_t poll_ms;
    {
        std::lock_guard<std::mutex> lock(breaker_mutex_);
        poll_ms = std::min<uint32_t>(1000, breaker_.config().slow_call_ms);
    }
    after_ms(poll_ms, [this, generation]() {
        // A trigger that left the lane is answered by SCAN_COMPLETE or by
        // apply_scan_trigger_result().
        if (generation != scan_wait_generation_ || !user_scan_waiting_ || !scan_trigger_inflight_) {
            return;
        }
        if (control_stalled()) {
            spdlog::debug("[WiFiManager] Scan trigger stuck behind a stalled WiFi control call — "
                          "answering with the last results");
            answer_scan_waiter();
            return;
        }
        watch_scan_waiter(generation);
    });
}

WiFiManager::ScanRequest WiFiManager::request_scan(bool user_refresh) {
    if (control_stalled()) {
        spdlog::debug("[WiFiManager] WiFi control call still waiting on the backend — scan not "
                      "sent");
        return ScanRequest::Refused;
    }
    if (scan_trigger_inflight_) {
        spdlog::debug("[WiFiManager] Scan trigger already in flight — reusing it");
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_pending_ = true;
        return ScanRequest::Coalesced;
    }
    if (!breaker_allows_call()) {
        spdlog::debug("[WiFiManager] WiFi control is unresponsive — scan not sent");
        return ScanRequest::Refused;
    }

    scan_scheduler_.on_scan_started();
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_pending_ = true; // Mark scan as pending - cleared after first SCAN_COMPLETE processed
    }
    scan_trigger_inflight_ = true;
    submit_control(
        "trigger_scan", ControlKind::Health,
        [this, tok = async_lifetime_.token(), user_refresh]() {
            const auto started = std::chrono::steady_clock::now();
            WiFiError result = backend_->trigger_scan();
            record_health_call(started);
            tok.defer("WiFiManager::scan_trigger_result", [this, result, user_refresh]() {
                apply_scan_trigger_result(result, user_refresh);
            });
        },
        [this, tok = async_lifetime_.token()]() {
            breaker_abandon_probe();
            tok.defer("WiFiManager::scan_trigger_dropped", [this]() {
                apply_scan_trigger_result(
                    WiFiError(WiFiResult::BACKEND_ERROR, "Scan trigger dropped unrun", ""),
                    /*user_refresh=*/false);
            });
        });
    return ScanRequest::Sent;
}

void WiFiManager::apply_scan_trigger_result(const WiFiError& result, bool user_refresh) {
    scan_trigger_inflight_ = false;
    if (result.success()) {
        spdlog::debug("[WiFiManager] Scan triggered successfully");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        scan_pending_ = false;
    }
    // A failed trigger gets no SCAN_COMPLETE, so resolve the scheduler now
    // instead of leaving scan_outstanding_ stuck true forever, which would
    // permanently block should_trigger(). Deliberately on_scan_failed(), NOT
    // on_scan_complete(0, ...): a failed trigger is not evidence the network
    // stopped changing, and feeding it through on_scan_complete would let
    // repeated failures (e.g. a wedged control socket while still associated)
    // drive unchanged_streak_ to 2 and suppress scanning permanently, exactly
    // backwards for the case this exists to diagnose.
    scan_scheduler_.on_scan_failed();
    if (scan_timer_) {
        lv_timer_set_period(scan_timer_, scan_scheduler_.next_interval_ms());
    }
    // A refresh coalesced onto this trigger waits on it whichever kind it was.
    answer_scan_waiter();

    if (!user_refresh) {
        LOG_WARN_INTERNAL("Periodic scan failed: {}", result.technical_msg);
        // Self-heal: a NOT_INITIALIZED scan means the backend never came up
        // (typically a fresh-boot race where wpa_supplicant's control socket
        // wasn't ready when the constructor first probed). Re-attempt bringup
        // here so the backend recovers on its own once the socket appears,
        // instead of looping "Backend not started" forever (helixscreen#1036).
        if (result.result == WiFiResult::NOT_INITIALIZED && backend_) {
            spdlog::info("[WiFiManager] Backend not started — re-attempting bringup");
            backend_->start_async();
        }
        return;
    }

    // Keep the backend's own account of the failure: NOTIFY_WARNING logs only
    // the user-facing string, which cannot tell FAIL-BUSY from a wedged
    // control socket.
    spdlog::warn("[WiFiManager] Scan trigger failed: {}", result.technical_msg);
    // If the OS reports the wireless link is actually up, the managed backend
    // simply can't reach its control socket (the link is system-managed and
    // live). Nagging the user with a failure toast is wrong (helixscreen#1059).
    if (os_link_up()) {
        spdlog::debug("[WiFiManager] Scan trigger failed but OS link is up "
                      "(system-managed) — suppressing user warning");
    } else if (in_association_grace()) {
        spdlog::debug("[WiFiManager] Scan trigger failed within {}s of an association change "
                      "we initiated — suppressing user warning",
                      kAssociationGrace.count());
    } else if (breaker_open()) {
        spdlog::debug("[WiFiManager] Scan trigger failed while WiFi control is unresponsive — "
                      "the WiFi status already says so");
    } else {
        NOTIFY_WARNING("{}", lv_tr("WiFi scan failed. Try again."));
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
        // Results will arrive via the SCAN_COMPLETE event; a failed trigger
        // comes back through apply_scan_trigger_result().
        manager->request_scan(/*user_refresh=*/false);
    }
}

// ============================================================================
// Connection Management
// ============================================================================

namespace helix {

std::string connect_failure_message(WiFiResult result, const std::string& reason,
                                    const std::string& auth_message) {
    if (result == WiFiResult::AUTHENTICATION_FAILED) {
        return auth_message;
    }
    if (reason.empty()) {
        return lv_tr("Connection failed");
    }
    return reason;
}

} // namespace helix

void WiFiManager::connect(const std::string& ssid, const std::string& password,
                          ConnectCallback on_complete, bool is_hidden) {
    if (!backend_) {
        NOTIFY_ERROR("{}", lv_tr("WiFi unavailable. Cannot connect to network."));
        if (on_complete) {
            on_complete(false, "No WiFi backend available", WiFiResult::NOT_INITIALIZED);
        }
        return;
    }

    spdlog::info("[WiFiManager] Connecting to '{}'", helix::redact::ssid(ssid));

    // A single-transport backend answers this join by taking the link off the
    // wired interface, so a user reachable on the wired address is about to
    // lose it. Say so NOW rather than leaving the only feedback to whatever
    // the daemon does next, and send the join anyway: the daemon's own events
    // are the only trustworthy verdict on whether it can be completed, and
    // refusing here would make Ethernet -> Wi-Fi impossible from the UI while
    // hiding a wedged radio behind a local answer.
    if (backend_->join_displaces_wired_link()) {
        NOTIFY_WARNING("{}", lv_tr("Joining Wi-Fi disconnects the wired network."));
    }

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

    const uint64_t generation = ++connect_generation_;
    // The deadline is the connect watchdog's first leg: it covers a join stuck
    // behind another call, and apply_connect_result() re-arms the watchdog for
    // the events once the backend accepts the join.
    submit_user_op(
        "connect_network", user_op_timeouts_.connect_ms,
        [this, generation, ssid, password, is_hidden]() -> helix::Callback<void()> {
            WiFiError result = backend_->connect_network(ssid, password, is_hidden);
            return [this, generation, ssid, result]() {
                apply_connect_result(generation, ssid, result);
            };
        },
        [this, generation, ssid]() {
            apply_connect_result(generation, ssid, unresponsive_error());
        });
}

void WiFiManager::apply_connect_result(uint64_t generation, const std::string& ssid,
                                       const WiFiError& result) {
    if (generation != connect_generation_) {
        spdlog::debug("[WiFiManager] Connect result for '{}' arrived after the attempt was "
                      "superseded — dropped",
                      helix::redact::ssid(ssid));
        return;
    }
    if (!result.success()) {
        const std::string reason = result.user_msg.empty() ? result.technical_msg : result.user_msg;
        // The reason belongs in the toast too: "Failed to connect" alone
        // leaves the user with nothing to act on.
        if (is_unresponsive_error(result)) {
            notify_unresponsive();
        } else {
            NOTIFY_ERROR(lv_tr("Failed to connect to WiFi network '{}': {}"),
                         helix::redact::ssid(ssid), reason);
        }
        // Clear in-progress + take the callback under the lock, then invoke the
        // local copy OUTSIDE the lock (the callback may re-enter WiFiManager).
        ConnectCallback cb;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            connecting_in_progress_ = false;
            cb = std::move(connect_callback_);
            connect_callback_ = nullptr;
        }
        if (cb) {
            cb(false, reason, result.result);
        }
        return; // the callback is already delivered — nothing left for the watchdog to guard
    }
    // Success/failure will be reported via CONNECTED/AUTH_FAILED events — or, when the
    // backend never produces either, by the watchdog armed here. Either event may
    // already have landed while the join ran on the lane; the watchdog then finds
    // nothing pending when it fires and delivers nothing.
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
    ++connect_generation_;

    submit_user_op(
        "disconnect_network", user_op_timeouts_.other_ms,
        [this]() -> helix::Callback<void()> {
            if (backend_->disconnect_network().success()) {
                return nullptr;
            }
            return []() { NOTIFY_WARNING("{}", lv_tr("Could not disconnect from WiFi")); };
        },
        []() { notify_unresponsive(); });
}

void WiFiManager::forget(
    const std::string& ssid,
    helix::SharedCallback<void(bool success, const std::string& error)> on_complete) {
    if (!backend_) {
        NOTIFY_ERROR("{}", lv_tr("WiFi unavailable. Cannot forget network."));
        if (on_complete) {
            on_complete(false, "No WiFi backend available");
        }
        return;
    }

    spdlog::info("[WiFiManager] Forgetting '{}'", helix::redact::ssid(ssid));
    // Forgetting the CONNECTED network disassociates as a side effect, and the
    // overlay restarts scanning immediately afterwards.
    mark_association_change();

    submit_user_op(
        "forget_network", user_op_timeouts_.other_ms,
        [this, ssid, on_complete]() -> helix::Callback<void()> {
            WiFiError result = backend_->forget_network(ssid);
            return
                [ssid, result, on_complete]() { report_forget_result(ssid, result, on_complete); };
        },
        [ssid, on_complete]() { report_forget_result(ssid, unresponsive_error(), on_complete); });
}

void WiFiManager::report_forget_result(
    const std::string& ssid, const WiFiError& result,
    const helix::SharedCallback<void(bool success, const std::string& error)>& on_complete) {
    if (!result.success()) {
        // Two of these are not failures the user caused, and neither may
        // reach the red "Failed to forget" toast:
        //
        // NETWORK_NOT_FOUND — nothing was there to forget. Silent: the
        //   desired state already holds.
        // NOT_SUPPORTED — this platform has no forget at all (the printer's
        //   network daemon owns the credential store, so there is nothing
        //   HelixScreen persists to remove). The user did tap a button and is
        //   owed an answer, so this one speaks — but as "not here", not as a
        //   failure. Saying nothing would leave a silent dead control.
        if (is_unresponsive_error(result)) {
            notify_unresponsive();
        } else if (result.result == WiFiResult::NOT_SUPPORTED) {
            NOTIFY_INFO("{}", lv_tr("This printer's network service manages saved WiFi networks"));
        } else if (result.result != WiFiResult::NETWORK_NOT_FOUND) {
            // NOTIFY_ERROR ultimately reaches spdlog::error, which is persisted
            // and swept into debug bundles — redact the SSID the same as every
            // other log line in this file.
            NOTIFY_ERROR(lv_tr("Failed to forget WiFi network '{}'"), helix::redact::ssid(ssid));
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

bool WiFiManager::reports_frequency() {
    return backend_ && backend_->reports_frequency();
}

std::string WiFiManager::netdev_name() {
    const auto iface = backend_ ? backend_->resolved_interface() : std::nullopt;
    return iface ? iface->netdev : std::string();
}

bool WiFiManager::supports_5ghz() {
    if (!backend_)
        return false;

    return backend_->supports_5ghz();
}

bool WiFiManager::supports_forget() {
    return backend_ && backend_->supports_forget();
}

bool WiFiManager::supports_radio_toggle() {
    return backend_ && backend_->supports_radio_toggle();
}

// ============================================================================
// Regulatory Country
// ============================================================================

bool WiFiManager::supports_country() {
    return backend_ && backend_->supports_country();
}

void WiFiManager::get_country_async(helix::LifetimeToken token,
                                    helix::SharedCallback<void(const CountryState&)> on_done) {
    if (!on_done) {
        return;
    }
    CountryState unread;
    unread.stored = helix::wifi::country::normalize(
        helix::SystemSettingsManager::instance().get_wifi_country());
    unread.last_seen = helix::wifi::country::normalize(
        helix::SystemSettingsManager::instance().get_wifi_country_last_seen());
    unread.display = unread.stored;
    if (!supports_country()) {
        on_done(unread);
        return;
    }
    submit_user_op(
        "get_country", user_op_timeouts_.other_ms,
        [this, token, on_done, unread]() -> helix::Callback<void()> {
            CountryState state = unread;
            state.read_ok = backend_->get_country(state.system).success();
            if (state.read_ok) {
                remember_radio_country(state.system);
            }
            state.display =
                helix::wifi::country::plan(state.stored, state.system, state.last_seen).display;
            return [token, on_done, state]() {
                if (!token.expired()) {
                    on_done(state);
                }
            };
        },
        [token, on_done, unread]() {
            if (!token.expired()) {
                on_done(unread);
            }
        });
}

void WiFiManager::set_country_async(const std::string& code, helix::LifetimeToken token,
                                    helix::SharedCallback<void(bool success)> on_done) {
    const std::string clean = helix::wifi::country::normalize(code);
    auto answer = [token, on_done](bool success) {
        if (on_done && !token.expired()) {
            on_done(success);
        }
    };
    if (clean.empty()) {
        helix::SystemSettingsManager::instance().set_wifi_country(clean);
        spdlog::info("[WiFiManager] WiFi country cleared — the system's own setting applies");
        answer(true);
        return;
    }
    if (!supports_country()) {
        answer(false);
        return;
    }
    spdlog::info("[WiFiManager] Setting WiFi country to {}", clean);
    submit_user_op(
        "set_country", user_op_timeouts_.other_ms,
        [this, clean, answer]() -> helix::Callback<void()> {
            const WiFiError result = backend_->set_country(clean);
            if (result.success()) {
                // Saved only once the radio took it, since a choice it refuses
                // would fail again at every start. Deferred on its own rather
                // than in the answer: the answer is dropped when it comes after
                // the deadline, and the radio has the country all the same.
                async_lifetime_.defer("WiFiManager::save_country", [clean]() {
                    helix::SystemSettingsManager::instance().set_wifi_country(clean);
                });
            }
            return [clean, result, answer]() {
                if (!result.success()) {
                    spdlog::warn("[WiFiManager] Could not set WiFi country {}: {}", clean,
                                 result.technical_msg);
                    NOTIFY_ERROR("{}", lv_tr("Could not set the WiFi country"));
                }
                answer(result.success());
            };
        },
        [answer]() {
            notify_unresponsive();
            answer(false);
        });
}

void WiFiManager::apply_stored_country() {
    if (!supports_country()) {
        return;
    }
    const auto& settings = helix::SystemSettingsManager::instance();
    const std::string stored = helix::wifi::country::normalize(settings.get_wifi_country());
    const std::string last_seen =
        helix::wifi::country::normalize(settings.get_wifi_country_last_seen());
    // Submitted even with nothing saved, so the radio's own country is
    // remembered for the start that finds it wiped.
    submit_control("apply_country", ControlKind::User, [this, stored, last_seen]() {
        std::string system;
        const bool read_ok = backend_->get_country(system).success();
        if (!read_ok) {
            system.clear();
            if (stored.empty()) {
                // An unreadable radio may hold a country; never overwrite it.
                spdlog::debug("[WiFiManager] Could not read the WiFi country — leaving it");
                return;
            }
        }
        remember_radio_country(system);
        const auto plan = helix::wifi::country::plan(stored, system, last_seen);
        if (plan.apply.empty()) {
            spdlog::debug("[WiFiManager] Leaving the WiFi country at {}",
                          system.empty() ? "world" : system);
            return;
        }
        const WiFiError result = backend_->set_country(plan.apply);
        if (!result.success()) {
            spdlog::warn("[WiFiManager] Could not apply {} WiFi country {}: {}",
                         stored.empty() ? "last reported" : "saved", plan.apply,
                         result.technical_msg);
        } else if (stored.empty()) {
            spdlog::info("[WiFiManager] Restored WiFi country {}: the radio came up at world "
                         "with no saved choice, and {} is the last country it reported",
                         plan.apply, plan.apply);
        } else {
            spdlog::info("[WiFiManager] Applied saved WiFi country {} (radio had '{}')", plan.apply,
                         system.empty() ? "world" : system);
        }
    });
}

void WiFiManager::remember_radio_country(const std::string& system) {
    const std::string clean = helix::wifi::country::normalize(system);
    if (clean.empty()) {
        return;
    }
    async_lifetime_.defer("WiFiManager::remember_country", [clean]() {
        helix::SystemSettingsManager::instance().set_wifi_country_last_seen(clean);
    });
}

void WiFiManager::handle_regdom_change(const std::string& event_data) {
    const std::string alpha2 = helix::wifi::country::parse_regdom_change_alpha2(event_data);
    spdlog::info("[WiFiManager] Regulatory domain changed (alpha2={}){}",
                 alpha2.empty() ? "?" : alpha2, scan_timer_ ? " — rescanning" : "");
    remember_radio_country(alpha2);
    notify_state_observers(/*country_may_have_changed=*/true);
    if (scan_timer_) {
        request_scan(/*user_refresh=*/false);
    }
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

void WiFiManager::report_radio_result(bool enabled, const WiFiError& result,
                                      bool has_wired_fallback) {
    if (result.success()) {
        spdlog::debug("[WiFiManager] WiFi radio {}", enabled ? "enabled" : "disabled");
        return;
    }

    // The user tapped the switch and is owed this even over a live link.
    if (is_unresponsive_error(result)) {
        notify_unresponsive();
        return;
    }

    if (result.result == WiFiResult::NOT_SUPPORTED) {
        // Absent, not broken — and the ONE case the suppression below must
        // not swallow. The switch flips optimistically and
        // reconcile_radio_toggle() snaps it back the moment the real state
        // reads unchanged, so with no message the control just refuses to
        // move for no stated reason. A working network path is the normal
        // condition here (the daemon owns a radio that is up), which is
        // exactly what os_link_up() would suppress on.
        NOTIFY_INFO("{}", lv_tr("This printer's network service controls the WiFi radio"));
        return;
    }

    // A live system-managed link (printer reachable by IP) means the
    // backend just can't reach wpa_supplicant's control socket; the
    // radio is not actually off. Don't surface a hard error toast for
    // an unmanageable-but-up link (helixscreen#1059).
    // Same reasoning for a wired path: on a printer whose Ethernet is carrying
    // the connection, the radio genuinely may not come up (the driver refuses to
    // raise the interface), but the user has working networking and a modal that
    // says only "Failed to enable WiFi" is alarming and unactionable.
    if (os_link_up() || has_wired_fallback) {
        spdlog::debug("[WiFiManager] Radio {} failed but a network path is up "
                      "({}) — suppressing user error: {}",
                      enabled ? "enable" : "disable", os_link_up() ? "wireless" : "wired",
                      result.user_msg.empty() ? result.technical_msg : result.user_msg);
    } else {
        NOTIFY_ERROR(enabled ? lv_tr("Failed to enable WiFi: {}")
                             : lv_tr("Failed to disable WiFi: {}"),
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
    report_radio_result(enabled, result, has_non_wifi_fallback());
    return result.success();
}

void WiFiManager::persist_radio_expectation(bool requested, bool success, bool actual) {
    // reconcile_radio_toggle owns what "the radio's state" means; reading it
    // back here keeps the stored value and the value the UI reconciles its
    // switch to from being two rules that agree only by convention.
    const auto outcome = helix::wifi::reconcile_radio_toggle(requested, success, actual);
    auto* config = Config::get_instance();
    // Staged, not saved: a toggle must not put a synchronous disk write on
    // the main thread, and whichever surface started it saves on its own
    // schedule.
    config->set_wifi_expected(outcome.enabled);
}

void WiFiManager::set_enabled_async(bool enabled, helix::LifetimeToken token,
                                    helix::SharedCallback<void(bool, bool)> on_complete) {
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
    // Application calls HttpExecutor::start_all(). A pool stopped at exit stays
    // stopped.
    if (!lane_pooled_) {
        helix::http::HttpExecutor::fast().start();
    }

    // The outcome is recorded under the manager's lifetime (the user-op
    // answer), and handed to the caller only while its token lives.
    auto answer = [token, cb = std::move(on_complete)](bool success, bool actual) {
        if (cb && !token.expired()) {
            cb(success, actual);
        }
    };

    submit_user_op(
        "set_radio_enabled", user_op_timeouts_.other_ms,
        [this, enabled, answer]() -> helix::Callback<void()> {
            WiFiError result = apply_radio_enabled(enabled);
            const bool success = result.success();
            const bool actual = backend_->is_running() && backend_->is_radio_enabled();
            const bool wired_fallback = has_non_wifi_fallback();
            return [enabled, result, wired_fallback, success, actual, answer]() {
                report_radio_result(enabled, result, wired_fallback);
                // Ahead of the caller's callback, so a caller that saves in
                // that callback flushes this write with it rather than
                // costing a second save.
                persist_radio_expectation(enabled, success, actual);
                answer(success, actual);
            };
        },
        [this, enabled, answer]() {
            // Nothing is known to have reached the radio, so nothing is persisted.
            report_radio_result(enabled, unresponsive_error(), has_non_wifi_fallback());
            answer(false, is_enabled());
        });
}

void WiFiManager::get_status_async(
    helix::LifetimeToken token,
    helix::SharedCallback<void(const WifiBackend::ConnectionStatus&)> on_done) {
    if (!on_done) {
        return;
    }
    if (!helix::http::HttpExecutor::fast().running()) {
        if (!lane_pooled_) {
            on_done(backend_ ? backend_->get_status() : WifiBackend::ConnectionStatus{});
            return;
        }
        // The pool has stopped (app exit): no read may run on this thread.
        WifiBackend::ConnectionStatus cached;
        {
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            cached = cached_status_;
        }
        cached.unresponsive = true;
        on_done(cached);
        return;
    }
    const bool stalled = control_stalled();
    {
        std::lock_guard<std::mutex> lock(status_waiters_mutex_);
        if (status_read_inflight_ && !stalled) {
            status_waiters_.push_back({std::move(token), std::move(on_done)});
            return;
        }
        if (stalled || !breaker_allows_call()) {
            WifiBackend::ConnectionStatus cached = cached_status_;
            cached.unresponsive = true;
            token.defer("WiFiManager::get_status_async(unresponsive)",
                        [cb = std::move(on_done), cached]() { cb(cached); });
            return;
        }
        status_waiters_.push_back({std::move(token), std::move(on_done)});
        status_read_inflight_ = true;
    }
    // A read dropped unrun clears the in-flight mark, so the next caller
    // starts a read that also answers everyone still waiting.
    submit_control(
        "get_status", ControlKind::Health, [this]() { run_status_reads(); },
        [this]() {
            breaker_abandon_probe();
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            status_read_inflight_ = false;
        });
}

void WiFiManager::run_status_reads() {
    // `this` is valid for the whole body: see radio_op_mutex_.
    // get_status_async() admitted the first read; each later one asks again.
    bool first = true;
    for (;;) {
        std::vector<StatusWaiter> batch;
        {
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            if (status_waiters_.empty()) {
                status_read_inflight_ = false;
                return;
            }
            batch.swap(status_waiters_);
        }
        const bool admitted = first || breaker_allows_call();
        first = false;
        WifiBackend::ConnectionStatus status;
        if (admitted) {
            // Every waiter in the batch asked before this read began.
            const auto started = std::chrono::steady_clock::now();
            status = backend_ ? backend_->get_status() : WifiBackend::ConnectionStatus{};
            const bool answered_in_time = record_health_call(started);
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            if (answered_in_time) {
                cached_status_ = status;
            } else {
                // A read that ran out the reply deadline parsed an empty reply,
                // which says "disconnected" about a link nobody could ask about.
                status = cached_status_;
                status.unresponsive = breaker_open();
            }
        } else {
            std::lock_guard<std::mutex> lock(status_waiters_mutex_);
            status = cached_status_;
            status.unresponsive = true;
        }
        for (auto& w : batch) {
            w.token.defer("WiFiManager::get_status_async",
                          [cb = std::move(w.on_done), status]() { cb(status); });
        }
    }
}

bool WiFiManager::cached_connected() {
    std::lock_guard<std::mutex> lock(status_waiters_mutex_);
    return cached_status_.connected;
}

// ============================================================================
// Control lane
// ============================================================================

void WiFiManager::submit_control(const char* tag, ControlKind kind, helix::Callback<void()> work,
                                 helix::Callback<void()> on_dropped) {
    if (!helix::http::HttpExecutor::fast().running()) {
        if (!lane_pooled_) {
            // No pool to run on (ESP32): its backend answers from memory.
            work();
            return;
        }
        spdlog::debug("[WiFiManager] Control lane: '{}' dropped, the worker pool has stopped", tag);
        if (on_dropped) {
            on_dropped();
        }
        return;
    }
    lane_pooled_ = true;
    {
        std::unique_lock<std::mutex> lock(control_mutex_);
        if (control_shutdown_ || control_paused_) {
            lock.unlock();
            spdlog::debug("[WiFiManager] Control lane: '{}' dropped, the lane is closed", tag);
            if (on_dropped) {
                on_dropped();
            }
            return;
        }
        control_queue_.push_back({tag, kind, std::move(work), std::move(on_dropped)});
        if (control_lane_active_) {
            return;
        }
        control_lane_active_ = true;
    }
    // One owner, so the two steps run in this order however the work item's
    // captures are destroyed: a drain dropped unrun releases what it would
    // have run, and only then ends the radio op, after which ~WiFiManager may
    // proceed.
    begin_radio_op();
    auto ran = std::shared_ptr<std::atomic<bool>>(new std::atomic<bool>(false),
                                                  [this](std::atomic<bool>* r) {
                                                      if (!r->load()) {
                                                          drop_control_ops();
                                                      }
                                                      delete r;
                                                      end_radio_op();
                                                  });
    helix::http::HttpExecutor::fast().submit([this, ran = std::move(ran)]() {
        ran->store(true);
        run_control_ops();
    });
}

void WiFiManager::run_control_ops() {
    // `this` is valid for the whole body: see radio_op_mutex_.
    for (;;) {
        ControlOp op;
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            // Going idle in the same critical section that found the queue
            // empty, so a submit racing it either lands here or starts a drain.
            if (control_queue_.empty()) {
                control_lane_active_ = false;
                return;
            }
            if (control_shutdown_ || control_paused_) {
                break;
            }
            op = std::move(control_queue_.front());
            control_queue_.pop_front();
        }
        spdlog::trace("[WiFiManager] Control lane: {}", op.tag);
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            control_op_started_ = std::chrono::steady_clock::now();
            control_op_kind_ = op.kind;
        }
        op.work();
        {
            std::lock_guard<std::mutex> lock(control_mutex_);
            control_op_started_.reset();
        }
    }
    drop_control_ops();
}

void WiFiManager::drop_control_ops() {
    std::deque<ControlOp> dropped;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        dropped.swap(control_queue_);
        control_lane_active_ = false;
    }
    for (auto& op : dropped) {
        spdlog::debug("[WiFiManager] Control lane: '{}' dropped unrun", op.tag);
        if (op.on_dropped) {
            op.on_dropped();
        }
    }
}

uint64_t WiFiManager::monotonic_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

bool WiFiManager::record_health_call(std::chrono::steady_clock::time_point started) {
    const auto elapsed_ms =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count());
    helix::wifi::ControlBreaker::Change change;
    helix::wifi::ControlBreaker::Config config;
    {
        std::lock_guard<std::mutex> lock(breaker_mutex_);
        change = breaker_.record(elapsed_ms, monotonic_ms());
        config = breaker_.config();
    }
    if (change == helix::wifi::ControlBreaker::Change::Tripped) {
        spdlog::warn("[WiFiManager] WiFi control unresponsive: {} consecutive calls took {}ms or "
                     "more — pausing scans and status polls, probing from {}ms",
                     config.trip_after, config.slow_call_ms, config.first_probe_ms);
        notify_state_observers();
    } else if (change == helix::wifi::ControlBreaker::Change::Recovered) {
        spdlog::info("[WiFiManager] WiFi control answering again ({}ms) — resuming", elapsed_ms);
        notify_state_observers();
    }
    return elapsed_ms < config.slow_call_ms;
}

bool WiFiManager::breaker_allows_call() {
    std::lock_guard<std::mutex> lock(breaker_mutex_);
    return breaker_.allow_call(monotonic_ms());
}

bool WiFiManager::breaker_open() {
    std::lock_guard<std::mutex> lock(breaker_mutex_);
    return breaker_.open();
}

void WiFiManager::breaker_abandon_probe() {
    std::lock_guard<std::mutex> lock(breaker_mutex_);
    breaker_.abandon_probe();
}

bool WiFiManager::control_stalled() {
    uint32_t slow_ms;
    {
        std::lock_guard<std::mutex> lock(breaker_mutex_);
        slow_ms = breaker_.config().slow_call_ms;
    }
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!control_op_started_) {
        return false;
    }
    const uint32_t limit_ms =
        control_op_kind_ == ControlKind::Health ? slow_ms : user_op_timeouts_.stall_ms;
    return std::chrono::steady_clock::now() - *control_op_started_ >=
           std::chrono::milliseconds(limit_ms);
}

bool WiFiManager::control_unresponsive() {
    return control_stalled() || breaker_open();
}

namespace {
constexpr const char* kUnresponsiveTechnicalMsg = "WiFi control call did not return in time";
} // namespace

WiFiError WiFiManager::unresponsive_error() {
    return WiFiError(WiFiResult::TIMEOUT, kUnresponsiveTechnicalMsg,
                     lv_tr("WiFi isn't responding"));
}

bool WiFiManager::is_unresponsive_error(const WiFiError& error) {
    return error.result == WiFiResult::TIMEOUT && error.technical_msg == kUnresponsiveTechnicalMsg;
}

void WiFiManager::notify_unresponsive() {
    NOTIFY_WARNING("{}", lv_tr("WiFi isn't responding"));
}

void WiFiManager::submit_user_op(const char* tag, uint32_t deadline_ms,
                                 helix::Callback<helix::Callback<void()>()> work,
                                 helix::SharedCallback<void()> on_unanswered) {
    if (control_unresponsive()) {
        spdlog::warn("[WiFiManager] WiFi control is not answering — '{}' not sent", tag);
        async_lifetime_.defer(tag, std::move(on_unanswered));
        return;
    }

    // Exactly one of the result, the deadline and the drop answers the
    // caller; whichever moves the op to Answered first.
    enum : int { Queued, Running, Answered };
    auto phase = std::make_shared<std::atomic<int>>(Queued);
    auto answer_unanswered = [phase, on_unanswered]() {
        if (phase->exchange(Answered) != Answered) {
            on_unanswered();
        }
    };

    after_ms(deadline_ms, [tag, deadline_ms, phase, answer_unanswered]() {
        if (phase->load() != Answered) {
            spdlog::warn("[WiFiManager] '{}' got no answer within {}ms", tag, deadline_ms);
        }
        answer_unanswered();
    });

    auto tok = async_lifetime_.token();
    submit_control(
        tag, ControlKind::User,
        [this, tok, tag, phase, work = std::move(work)]() {
            int expected = Queued;
            if (!phase->compare_exchange_strong(expected, Running)) {
                return; // the caller was already told it failed; do not act now
            }
            helix::Callback<void()> apply = work();
            tok.defer(tag, [this, tag, phase, apply = std::move(apply)]() {
                if (phase->exchange(Answered) != Answered) {
                    if (apply) {
                        apply();
                    }
                    return;
                }
                // The caller was told this failed at its deadline, but it ran:
                // the radio or link may have moved, so let the UI re-read it.
                spdlog::info("[WiFiManager] '{}' finished after its deadline — refreshing", tag);
                notify_state_observers();
            });
        },
        [tok, tag, answer_unanswered]() { tok.defer(tag, answer_unanswered); });
}

void WiFiManager::after_ms(uint32_t ms, helix::Callback<void()> fn) {
    if (!lv_is_initialized()) {
        return;
    }
    struct Pending {
        helix::LifetimeToken token;
        helix::Callback<void()> fn;
    };
    // TIMER_DTOR_OK: the timer owns its context and checks the manager's token
    // before acting, so one outliving the manager runs nothing.
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            std::unique_ptr<Pending> pending(static_cast<Pending*>(lv_timer_get_user_data(t)));
            if (!pending->token.expired()) {
                pending->fn();
            }
        },
        ms, new Pending{async_lifetime_.token(), std::move(fn)});
    lv_timer_set_repeat_count(timer, 1); // one-shot: LVGL deletes it after the callback
}

bool WiFiManager::wait_for_radio_ops_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(radio_op_mutex_);
    return radio_op_cv_.wait_for(lock, timeout, [this] { return radio_ops_inflight_ == 0; });
}

void WiFiManager::begin_radio_op() {
    std::lock_guard<std::mutex> lock(radio_op_mutex_);
    ++radio_ops_inflight_;
}

void WiFiManager::end_radio_op() {
    // Notify under the lock, not after it. wait_for_radio_ops() wakes as soon as
    // the count reaches zero, and ~WiFiManager() destroys radio_op_cv_ right
    // after it returns -- which would be while this thread was still inside
    // notify_all(). Holding the mutex across the notify keeps the waiter blocked
    // on reacquiring it until we are done touching the condition variable.
    std::lock_guard<std::mutex> lock(radio_op_mutex_);
    --radio_ops_inflight_;
    radio_op_cv_.notify_all();
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
                    // count and the cached connection state (a fresh read is a
                    // backend round trip, which must not run on this thread),
                    // then apply whatever interval it decides on to the live timer.
                    manager->scan_scheduler_.on_scan_complete(data->networks.size(),
                                                              manager->cached_connected());
                    manager->last_scan_results_ = data->networks;
                    manager->user_scan_waiting_ = false;
                    ++manager->scan_wait_generation_;
                    if (manager->scan_timer_) {
                        lv_timer_set_period(manager->scan_timer_,
                                            manager->scan_scheduler_.next_interval_ms());
                    }

                    helix::SharedCallback<void(const std::vector<WiFiNetwork>&)> cb;
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
                LOG_WARN_INTERNAL("async_call: calling callback with the last results");
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
                    manager->user_scan_waiting_ = false;
                    ++manager->scan_wait_generation_;
                    manager->deliver_last_scan_results();
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
    WiFiResult result;
};

void WiFiManager::handle_connected(const std::string& event_data) {
    (void)event_data; // Could parse IP address from event data

    spdlog::debug("[WiFiManager] Connected event received (backend thread)");

    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        connecting_in_progress_ = false; // Connection complete
    }
    {
        // The SSID and address this event joined are unknown here; the cached
        // ones may be another network's. Keep only the adapter's own MAC until
        // the next status read fills the rest.
        std::lock_guard<std::mutex> lock(status_waiters_mutex_);
        WifiBackend::ConnectionStatus fresh;
        fresh.connected = true;
        fresh.mac_address = cached_status_.mac_address;
        cached_status_ = fresh;
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
        std::make_unique<ConnectCallbackData>(
            ConnectCallbackData{self_, true, "", WiFiResult::SUCCESS}),
        [](ConnectCallbackData* d) {
            if (auto manager = d->manager.lock()) {
                // A transient AUTH_FAILED may be sitting in the grace window — this
                // CONNECTED is the real outcome, so cancel it before delivering success
                // (helixscreen#1050).
                manager->cancel_auth_fail_grace();
                // The attempt resolved — disarm the watchdog before delivering success.
                manager->cancel_connect_timeout();
                ConnectCallback cb;
                {
                    std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                    cb = std::move(manager->connect_callback_);
                    manager->connect_callback_ = nullptr;
                }
                if (cb) {
                    cb(d->success, d->error, d->result);
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

    {
        std::lock_guard<std::mutex> lock(status_waiters_mutex_);
        cached_status_ = WifiBackend::ConnectionStatus{};
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
        std::make_unique<ConnectCallbackData>(
            ConnectCallbackData{self_, false, "Disconnected", WiFiResult::CONNECTION_FAILED}),
        [](ConnectCallbackData* d) {
            if (auto manager = d->manager.lock()) {
                ConnectCallback cb;
                {
                    std::lock_guard<std::mutex> lock(manager->callback_mutex_);
                    cb = std::move(manager->connect_callback_);
                    manager->connect_callback_ = nullptr;
                }
                if (cb) {
                    cb(d->success, d->error, d->result);
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
        std::make_unique<ConnectCallbackData>(ConnectCallbackData{
            self_, false, std::move(error_msg), WiFiResult::AUTHENTICATION_FAILED}),
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
constexpr uint32_t kAuthFailGraceMs = 4000;
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
    auth_fail_grace_timer_ = lv_timer_create(auth_fail_grace_timer_cb, kAuthFailGraceMs, this);
    lv_timer_set_repeat_count(auth_fail_grace_timer_, 1); // one-shot
    spdlog::debug("[WiFiManager] AUTH_FAILED deferred {}ms pending possible CONNECTED",
                  kAuthFailGraceMs);
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
    ConnectCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = std::move(connect_callback_);
        connect_callback_ = nullptr;
    }
    if (cb) {
        cb(false, pending_auth_error_, WiFiResult::AUTHENTICATION_FAILED);
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
constexpr uint32_t kConnectTimeoutMs = 45000;
} // namespace

void WiFiManager::start_connect_timeout() {
    cancel_connect_timeout();
    connect_timeout_timer_ = lv_timer_create(connect_timeout_timer_cb, kConnectTimeoutMs, this);
    lv_timer_set_repeat_count(connect_timeout_timer_, 1); // one-shot
    spdlog::debug("[WiFiManager] Connect watchdog armed ({}ms)", kConnectTimeoutMs);
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
    ConnectCallback cb;
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
                      kConnectTimeoutMs);
    notify_state_observers();
    // Invoke OUTSIDE callback_mutex_ — the callback re-enters WiFiManager.
    if (cb) {
        cb(false, lv_tr("Connection timeout"), WiFiResult::TIMEOUT);
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

void WiFiManager::add_state_observer(helix::LifetimeToken token,
                                     helix::SharedCallback<void()> on_change) {
    if (!on_change) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_observers_mutex_);
    state_observers_.push_back({std::move(token), std::move(on_change)});
}

void WiFiManager::add_country_observer(helix::LifetimeToken token,
                                       helix::SharedCallback<void()> on_change) {
    if (!on_change) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_observers_mutex_);
    state_observers_.push_back({std::move(token), std::move(on_change), /*country_only=*/true});
}

void WiFiManager::notify_state_observers(bool country_may_have_changed) {
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
        if (obs.country_only && !country_may_have_changed) {
            continue;
        }
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
        // The one owner, never freed, so ~WiFiManager never waits in the app.
        instance = std::make_shared<WiFiManager>(/*silent=*/true); // WIFI_MANAGER_OWNER_OK
        instance->init_self_reference(instance);
    }

    return instance;
}

} // namespace helix
