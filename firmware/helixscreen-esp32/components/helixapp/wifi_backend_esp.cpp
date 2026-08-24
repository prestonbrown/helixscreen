// SPDX-License-Identifier: GPL-3.0-or-later
//
// Task 13 — WifiBackend over esp_wifi. Implements helix::create_platform_
// wifi_backend() (declared in include/wifi_backend.h) so the shared
// WiFiManager (src/api/wifi_manager.cpp) drives the real K-Touch radio the
// same way it drives NetworkManager/wpa_supplicant on desktop: register event
// callbacks, then start_async(); scan/connect/status all go through the
// WifiBackend contract.
//
// Credential handling (R2): NVS namespace "wifi" (keys "ssid"/"psk") is the
// runtime source of truth. connect_network() is the only writer. On first
// boot (namespace has no "ssid" key at all — never seeded, never user-set),
// start() seeds ONCE from CONFIG_HELIX_HIL_WIFI_SSID/PASSWORD (git-ignored
// sdkconfig.local), matching Task 12 R2's Moonraker-host seed pattern
// exactly. The password is never logged at any level, masked or not — the
// simplest way to satisfy "PSK NEVER, not even length-masked at INFO" is to
// emit zero psk diagnostics.
//
// Hardware bring-up gating: see wifi_backend_esp.h. WiFiManager is a process
// singleton (get_wifi_manager()) shared between the boot connect path
// (app_boot.cpp's app_net_start(), Task 13 R3) and the Settings > Network UI.
// Whichever caller constructs it FIRST (in practice NetworkWidget, during the
// heavy build_shell() boot phase) must not trigger esp_wifi_init on that
// thread — hardware bring-up is deferred until app_net_start()'s dedicated
// pthread opens the gate and calls WiFiManager::retry_async(), preserving
// Task 8's THE PATTERN (>=32KB internal alloc claimed only on that pthread,
// after the boot's internal-DRAM gates clear).
//
// R4 carry-forward: an assoc-timeout esp_timer (kAssocTimeoutMs) aborts a
// stalled connect attempt (the historical ~1/20-boot 75s association stall)
// by forcing esp_wifi_disconnect(), which lands on the normal disconnected
// handler's bounded-backoff retry path — never a second, competing connect
// attempt. A second esp_timer (kRetryBackoff*) retries after each disconnect
// with exponential backoff capped at kRetryBackoffCapMs, so a never-
// associating network retries forever in the background instead of parking
// any thread.

#include "wifi_backend_esp.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "log_redact.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_backend.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr char NVS_NAMESPACE[] = "wifi";
constexpr char NVS_KEY_SSID[] = "ssid";
constexpr char NVS_KEY_PSK[] = "psk";

// R4: assoc-timeout — historical flake stalled association ~75s on some
// boots. 15s comfortably bounds a stalled attempt (successful assoc is
// typically <5s) while giving weak-signal joins real room before we abort
// and retry.
constexpr uint64_t ASSOC_TIMEOUT_US = 15'000'000; // 15s

// R4: bounded backoff between reconnect attempts after a disconnect. Doubles
// from the floor up to the cap, then holds — never a permanent giving-up
// (graceful degradation keeps retrying in the background indefinitely).
constexpr uint64_t RETRY_BACKOFF_FLOOR_US = 2'000'000; // 2s
constexpr uint64_t RETRY_BACKOFF_CAP_US = 30'000'000;  // 30s
constexpr int RETRY_BACKOFF_MAX_SHIFT = 4;             // 2s * 2^4 = 32s (clamped to cap)

// Bounds internal RAM for scan-result caching regardless of how many APs are
// in range (R constraint: "no unbounded scan-result accumulation").
constexpr uint16_t MAX_SCAN_RESULTS = 24;

int rssi_to_percent(int8_t rssi) {
    // Common linear mapping: -100dBm floor -> 0%, -50dBm ceiling -> 100%.
    int pct = 2 * (static_cast<int>(rssi) + 100);
    return std::max(0, std::min(100, pct));
}

int channel_to_mhz(uint8_t channel) {
    if (channel == 14) {
        return 2484;
    }
    if (channel >= 1 && channel <= 13) {
        return 2407 + channel * 5;
    }
    return 0; // unknown
}

void auth_mode_to_security(wifi_auth_mode_t mode, bool& secured, std::string& type) {
    switch (mode) {
    case WIFI_AUTH_OPEN:
        secured = false;
        type = "Open";
        break;
    case WIFI_AUTH_WEP:
        secured = true;
        type = "WEP";
        break;
    case WIFI_AUTH_WPA_PSK:
        secured = true;
        type = "WPA";
        break;
    case WIFI_AUTH_WPA2_PSK:
        secured = true;
        type = "WPA2";
        break;
    case WIFI_AUTH_WPA_WPA2_PSK:
        secured = true;
        type = "WPA/WPA2";
        break;
    case WIFI_AUTH_WPA3_PSK:
        secured = true;
        type = "WPA3";
        break;
    case WIFI_AUTH_WPA2_WPA3_PSK:
        secured = true;
        type = "WPA2/WPA3";
        break;
    case WIFI_AUTH_WPA2_ENTERPRISE:
        secured = true;
        type = "WPA2-Enterprise";
        break;
    case WIFI_AUTH_OWE:
        secured = true;
        type = "OWE";
        break;
    default:
        secured = true;
        type = "Unknown";
        break;
    }
}

// Best-effort NVS string read. Returns ESP_ERR_NVS_NOT_FOUND if the key was
// never written (distinguishes "never seeded" from "seeded empty").
esp_err_t nvs_read_string(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &len);
    if (err != ESP_OK) {
        return err;
    }
    std::vector<char> buf(len);
    err = nvs_get_str(h, key, buf.data(), &len);
    if (err == ESP_OK) {
        out.assign(buf.data());
    }
    return err;
}

} // namespace

namespace helix {

// Hardware bring-up gate — see wifi_backend_esp.h. Closed by default; the
// boot net thread opens it once, right before retry_async().
static std::atomic<bool> s_hw_bringup_allowed{false};

void wifi_backend_esp_allow_hardware_bringup() {
    s_hw_bringup_allowed.store(true);
}

// Internal to this TU (not in the header) — only WifiBackendEsp::start()
// below needs it.
bool wifi_backend_esp_hw_bringup_allowed() {
    return s_hw_bringup_allowed.load();
}

// Task 14 — see wifi_backend_esp.h for the self-healing rationale. Mirrors
// nvs_write_creds()'s open/write/commit/close shape exactly, erasing instead
// of writing; kept in this TU so it stays the ONLY thing (besides
// connect_network()) that ever opens the "wifi" namespace for writing.
void wifi_backend_esp_clear_stored_credentials() {
    nvs_handle_t h;
    esp_err_t rc = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (rc != ESP_OK) {
        spdlog::warn("[WifiBackend] esp32: nvs_open('wifi') failed clearing credentials: {}",
                     esp_err_to_name(rc));
        return;
    }
    // ESP_ERR_NVS_NOT_FOUND on either key is fine (already absent) — no
    // separate check needed, nvs_commit() below is what actually matters.
    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PSK);
    esp_err_t crc = nvs_commit(h);
    if (crc != ESP_OK) {
        spdlog::warn("[WifiBackend] esp32: nvs_commit failed clearing credentials: {}",
                     esp_err_to_name(crc));
    }
    nvs_close(h);
    spdlog::info("[WifiBackend] esp32: cleared stored WiFi credentials (provisioning rollback)");
}

} // namespace helix

namespace {

static const char* TAG = "wifi_backend_esp";

class WifiBackendEsp : public WifiBackend {
  public:
    WifiBackendEsp() = default;

    ~WifiBackendEsp() override {
        stop();
    }

    WiFiError start() override {
        // Serializes against a concurrent start_async() re-entry — e.g. the
        // boot net thread's explicit retry_async() landing at the same time
        // as WiFiManager's own scan-timer NOT_INITIALIZED retry (helixscreen
        // #1036 pattern) firing from the UI/main thread. Mirrors
        // WifiBackendNetworkManager::start()'s start_mutex_ (desktop).
        std::lock_guard<std::mutex> start_lock(start_mutex_);

        if (running_) {
            return WiFiErrorHelper::success();
        }
        if (!helix::wifi_backend_esp_hw_bringup_allowed()) {
            spdlog::debug("[WifiBackend] esp32: hardware bring-up deferred (gate closed) — "
                          "waiting for the boot net thread");
            return WiFiError(WiFiResult::NOT_INITIALIZED,
                             "esp32 wifi hardware bring-up deferred to boot net thread",
                             "WiFi not ready yet");
        }

        esp_err_t nvs_rc = nvs_flash_init();
        if (nvs_rc == ESP_ERR_NVS_NO_FREE_PAGES || nvs_rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_rc = nvs_flash_init();
        }
        if (nvs_rc != ESP_OK) {
            spdlog::warn("[WifiBackend] esp32: nvs_flash_init: {} (continuing)",
                         esp_err_to_name(nvs_rc));
        }

        esp_err_t netif_rc = esp_netif_init();
        if (netif_rc != ESP_OK && netif_rc != ESP_ERR_INVALID_STATE) {
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             std::string("esp_netif_init: ") + esp_err_to_name(netif_rc),
                             "WiFi hardware init failed");
        }
        esp_err_t loop_rc = esp_event_loop_create_default();
        if (loop_rc != ESP_OK && loop_rc != ESP_ERR_INVALID_STATE) {
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             std::string("esp_event_loop_create_default: ") +
                                 esp_err_to_name(loop_rc),
                             "WiFi hardware init failed");
        }
        if (!netif_) {
            netif_ = esp_netif_create_default_wifi_sta();
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t init_rc = esp_wifi_init(&cfg);
        if (init_rc != ESP_OK && init_rc != ESP_ERR_WIFI_INIT_STATE) {
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             std::string("esp_wifi_init: ") + esp_err_to_name(init_rc),
                             "WiFi hardware init failed");
        }

        if (!handlers_registered_) {
            ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       &WifiBackendEsp::wifi_event_handler, this));
            ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       &WifiBackendEsp::ip_event_handler, this));
            handlers_registered_ = true;
        }

        if (!assoc_timeout_timer_) {
            esp_timer_create_args_t assoc_args = {};
            assoc_args.callback = &WifiBackendEsp::assoc_timeout_cb;
            assoc_args.arg = this;
            assoc_args.name = "wifi_assoc_to";
            ESP_ERROR_CHECK(esp_timer_create(&assoc_args, &assoc_timeout_timer_));
        }
        if (!retry_timer_) {
            esp_timer_create_args_t retry_args = {};
            retry_args.callback = &WifiBackendEsp::retry_cb;
            retry_args.arg = this;
            retry_args.name = "wifi_retry";
            ESP_ERROR_CHECK(esp_timer_create(&retry_args, &retry_timer_));
        }

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        load_or_seed_credentials();
        apply_wifi_config_locked();

        esp_err_t start_rc = esp_wifi_start();
        if (start_rc != ESP_OK) {
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             std::string("esp_wifi_start: ") + esp_err_to_name(start_rc),
                             "WiFi hardware start failed");
        }

        running_ = true;
        spdlog::info("[WifiBackend] esp32: station started (ssid configured: {})",
                     !current_ssid_.empty());
        return WiFiErrorHelper::success();
    }

    void start_async() override {
        WiFiError result = start();
        if (result.success()) {
            fire_event("READY");
        } else if (result.result != WiFiResult::NOT_INITIALIZED) {
            // NOT_INITIALIZED here means "gate closed" (deferred, not a real
            // failure) — no INIT_FAILED noise for that; the retry_async()
            // after the gate opens is the real attempt.
            fire_event("INIT_FAILED", result.technical_msg);
        }
    }

    void stop() override {
        if (assoc_timeout_timer_) {
            esp_timer_stop(assoc_timeout_timer_); // no-op if not running
        }
        if (retry_timer_) {
            esp_timer_stop(retry_timer_);
        }
        if (!running_) {
            return;
        }
        esp_wifi_disconnect();
        esp_wifi_stop();
        running_ = false;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            cached_status_ = ConnectionStatus{};
        }
        spdlog::info("[WifiBackend] esp32: station stopped");
    }

    bool is_running() const override {
        return running_;
    }

    void register_event_callback(const std::string& name,
                                 std::function<void(const std::string&)> callback) override {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto it = callbacks_.find(name);
        if (it == callbacks_.end()) {
            callbacks_.insert({name, std::move(callback)});
        } else {
            spdlog::warn("[WifiBackend] esp32: callback '{}' already registered (not replacing)",
                         name);
        }
    }

    WiFiError trigger_scan() override {
        if (!running_) {
            return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                             "WiFi system not ready");
        }
        wifi_scan_config_t scan_cfg = {};
        scan_cfg.show_hidden = false;
        esp_err_t rc = esp_wifi_scan_start(&scan_cfg, false); // async — SCAN_DONE event follows
        if (rc != ESP_OK) {
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             std::string("esp_wifi_scan_start: ") + esp_err_to_name(rc),
                             "Could not start scan");
        }
        return WiFiErrorHelper::success();
    }

    WiFiError get_scan_results(std::vector<WiFiNetwork>& networks) override {
        if (!running_) {
            return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                             "WiFi system not ready");
        }
        std::lock_guard<std::mutex> lock(networks_mutex_);
        networks = cached_networks_;
        return WiFiErrorHelper::success();
    }

    WiFiError connect_network(const std::string& ssid, const std::string& password) override {
        if (!running_) {
            return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                             "WiFi system not ready");
        }
        if (ssid.empty() || ssid.size() >= 32 || password.size() >= 64) {
            return WiFiError(WiFiResult::INVALID_PARAMETERS, "SSID/password invalid or too long",
                             "Invalid network name or password");
        }

        spdlog::info("[WifiBackend] esp32: connecting to '{}'", helix::redact::ssid(ssid));

        {
            std::lock_guard<std::mutex> lock(cfg_mutex_);
            current_ssid_ = ssid;
            current_psk_ = password;
        }
        nvs_write_creds(ssid, password); // R2: connect_network() is the only NVS writer

        // Own-teardown suppression: if we're mid-association, tear it down
        // first. The resulting STA_DISCONNECTED is our own noise, not a real
        // failure — swallow exactly one via own_disconnect_pending_ before
        // arming the fresh attempt below.
        esp_err_t drc = esp_wifi_disconnect();
        if (drc == ESP_OK) {
            own_disconnect_pending_.store(true);
        }

        apply_wifi_config_locked();

        retry_count_ = 0;
        explicit_connect_pending_.store(true);
        arm_assoc_timeout();
        esp_err_t crc = esp_wifi_connect();
        if (crc != ESP_OK && crc != ESP_ERR_WIFI_CONN) {
            explicit_connect_pending_.store(false);
            cancel_assoc_timeout();
            return WiFiError(WiFiResult::BACKEND_ERROR,
                             std::string("esp_wifi_connect: ") + esp_err_to_name(crc),
                             "Could not start connection");
        }
        return WiFiErrorHelper::success();
    }

    WiFiError disconnect_network() override {
        if (!running_) {
            return WiFiError(WiFiResult::NOT_INITIALIZED, "Backend not started",
                             "WiFi system not ready");
        }
        spdlog::info("[WifiBackend] esp32: disconnecting");
        cancel_assoc_timeout();
        esp_timer_stop(retry_timer_);
        esp_wifi_disconnect();
        // Not own_disconnect_pending_: a user-initiated disconnect SHOULD
        // surface as a real DISCONNECTED event (matches desktop semantics —
        // nmcli device disconnect fires the same way).
        return WiFiErrorHelper::success();
    }

    ConnectionStatus get_status() override {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return cached_status_;
    }

    bool supports_5ghz() const override {
        return false; // S3 radio is 2.4GHz-only
    }

  private:
    std::atomic<bool> running_{false};
    std::mutex start_mutex_;
    esp_netif_t* netif_ = nullptr;
    bool handlers_registered_ = false;

    std::mutex callbacks_mutex_;
    std::map<std::string, std::function<void(const std::string&)>> callbacks_;

    std::mutex networks_mutex_;
    std::vector<WiFiNetwork> cached_networks_;

    std::mutex status_mutex_;
    ConnectionStatus cached_status_{};

    std::mutex cfg_mutex_;
    std::string current_ssid_;
    std::string current_psk_;

    // Resolves exactly one explicit connect_network() attempt with a
    // definitive CONNECTED/AUTH_FAILED — everything else (boot auto-connect,
    // background retries) reports plain DISCONNECTED so a passive observer
    // (home-panel icon) still tracks link state without spamming a "failed"
    // callback nobody is waiting on.
    std::atomic<bool> explicit_connect_pending_{false};
    // Swallows the single disconnected-event our OWN esp_wifi_disconnect()
    // (issued from connect_network() to tear down a prior session before
    // reconfiguring) produces, so it isn't misreported as a real failure of
    // the NEW attempt that follows immediately after.
    std::atomic<bool> own_disconnect_pending_{false};

    int retry_count_ = 0;
    esp_timer_handle_t assoc_timeout_timer_ = nullptr;
    esp_timer_handle_t retry_timer_ = nullptr;

    // ------------------------------------------------------------------
    // Credentials (R2)
    // ------------------------------------------------------------------

    // Populates current_ssid_/current_psk_ from NVS, seeding ONCE from
    // Kconfig if the namespace has never held a value. Never re-seeds over a
    // stored (possibly user-set) value.
    void load_or_seed_credentials() {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
            spdlog::warn("[WifiBackend] esp32: nvs_open('wifi') failed — no stored credentials");
            return;
        }

        std::string ssid, psk;
        esp_err_t ssid_rc = nvs_read_string(h, NVS_KEY_SSID, ssid);
        if (ssid_rc == ESP_ERR_NVS_NOT_FOUND) {
            // Never seeded, never user-set. Seed ONCE from Kconfig (may be
            // empty in a dev build without sdkconfig.local — that's fine,
            // just means no auto-connect target yet).
            ssid = CONFIG_HELIX_HIL_WIFI_SSID;
            psk = CONFIG_HELIX_HIL_WIFI_PASS;
            if (!ssid.empty()) {
                nvs_set_str(h, NVS_KEY_SSID, ssid.c_str());
                nvs_set_str(h, NVS_KEY_PSK, psk.c_str());
                nvs_commit(h);
                spdlog::info("[WifiBackend] esp32: seeded first-boot WiFi SSID from Kconfig "
                             "default ('{}')",
                             helix::redact::ssid(ssid));
            } else {
                spdlog::info("[WifiBackend] esp32: no stored WiFi credentials and no Kconfig "
                             "seed — station will wait for Settings > Network");
            }
        } else if (ssid_rc == ESP_OK) {
            nvs_read_string(h, NVS_KEY_PSK, psk); // best-effort; empty = open network
            spdlog::info("[WifiBackend] esp32: using stored WiFi SSID '{}' from NVS",
                         helix::redact::ssid(ssid));
        } else {
            spdlog::warn("[WifiBackend] esp32: nvs_get_str(ssid) failed: {}",
                         // PII_OK: an esp_err_t name, not the SSID itself
                         esp_err_to_name(ssid_rc));
        }
        nvs_close(h);

        std::lock_guard<std::mutex> lock(cfg_mutex_);
        current_ssid_ = ssid;
        current_psk_ = psk;
    }

    void nvs_write_creds(const std::string& ssid, const std::string& password) {
        nvs_handle_t h;
        esp_err_t rc = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
        if (rc != ESP_OK) {
            spdlog::warn("[WifiBackend] esp32: nvs_open('wifi') failed persisting credentials: {}",
                         esp_err_to_name(rc));
            return;
        }
        nvs_set_str(h, NVS_KEY_SSID, ssid.c_str());
        nvs_set_str(h, NVS_KEY_PSK, password.c_str());
        esp_err_t crc = nvs_commit(h);
        if (crc != ESP_OK) {
            spdlog::warn("[WifiBackend] esp32: nvs_commit failed persisting credentials: {}",
                         esp_err_to_name(crc));
        }
        nvs_close(h);
    }

    // Pushes current_ssid_/current_psk_ into the esp_wifi driver config.
    // Safe to call with an empty SSID (leaves the driver unconfigured; the
    // STA_START handler checks before auto-connecting).
    void apply_wifi_config_locked() {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        wifi_config_t wifi_config = {};
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), current_ssid_.c_str(),
                     sizeof(wifi_config.sta.ssid) - 1);
        std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), current_psk_.c_str(),
                     sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode =
            current_psk_.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        esp_err_t rc = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (rc != ESP_OK) {
            spdlog::warn("[WifiBackend] esp32: esp_wifi_set_config failed: {}",
                         esp_err_to_name(rc));
        }
    }

    bool has_ssid_configured() {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        return !current_ssid_.empty();
    }

    bool has_password_configured() {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        return !current_psk_.empty();
    }

    // ------------------------------------------------------------------
    // Event plumbing
    // ------------------------------------------------------------------

    void fire_event(const std::string& event_name, const std::string& data = "") {
        std::function<void(const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            auto it = callbacks_.find(event_name);
            if (it == callbacks_.end()) {
                return;
            }
            cb = it->second;
        }
        try {
            cb(data);
        } catch (const std::exception& e) {
            spdlog::error("[WifiBackend] esp32: exception in callback '{}': {}", event_name,
                          e.what());
        }
    }

    void arm_assoc_timeout() {
        if (!assoc_timeout_timer_) {
            return;
        }
        esp_timer_stop(assoc_timeout_timer_); // ignore ESP_ERR_INVALID_STATE (not running)
        esp_timer_start_once(assoc_timeout_timer_, ASSOC_TIMEOUT_US);
    }

    void cancel_assoc_timeout() {
        if (assoc_timeout_timer_) {
            esp_timer_stop(assoc_timeout_timer_);
        }
    }

    void schedule_retry() {
        if (!retry_timer_ || !has_ssid_configured()) {
            return;
        }
        int shift = std::min(retry_count_, RETRY_BACKOFF_MAX_SHIFT);
        uint64_t backoff_us =
            std::min<uint64_t>(RETRY_BACKOFF_FLOOR_US << shift, RETRY_BACKOFF_CAP_US);
        esp_timer_stop(retry_timer_);
        esp_timer_start_once(retry_timer_, backoff_us);
    }

    // WIFI_EVENT / IP_EVENT handlers run on the system event loop task — the
    // esp_wifi analogue of a background callback context (CLAUDE.md
    // threading rule). We do zero heap-heavy or teardown work here directly;
    // esp_wifi_connect()/disconnect() are lightweight driver calls (the
    // vendor's own reference pattern uses them directly from this context),
    // and fire_event() just invokes the registered WiFiManager callback,
    // which marshals real UI/subject work via ui_queue_update itself.
    static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
        auto* self = static_cast<WifiBackendEsp*>(arg);
        if (!self) {
            return;
        }
        if (base != WIFI_EVENT) {
            return;
        }
        if (id == WIFI_EVENT_STA_START) {
            if (self->has_ssid_configured()) {
                self->retry_count_ = 0;
                self->arm_assoc_timeout();
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            // Associated (pre-DHCP) — cancel the assoc-timeout; GOT_IP (or a
            // later disconnect) resolves the attempt from here.
            self->cancel_assoc_timeout();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            self->on_disconnected(data);
        } else if (id == WIFI_EVENT_SCAN_DONE) {
            self->on_scan_done();
        }
    }

    static void ip_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data) {
        auto* self = static_cast<WifiBackendEsp*>(arg);
        if (!self || base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) {
            return;
        }
        self->on_got_ip(static_cast<ip_event_got_ip_t*>(data));
    }

    static void assoc_timeout_cb(void* arg) {
        auto* self = static_cast<WifiBackendEsp*>(arg);
        if (!self) {
            return;
        }
        ESP_LOGW(TAG, "assoc timeout — aborting stalled attempt (retry via disconnect handler)");
        // Abort only; the resulting STA_DISCONNECTED drives the normal
        // bounded-backoff retry path below. No competing connect() call here.
        esp_wifi_disconnect();
    }

    static void retry_cb(void* arg) {
        auto* self = static_cast<WifiBackendEsp*>(arg);
        if (!self || !self->has_ssid_configured()) {
            return;
        }
        self->retry_count_++;
        self->arm_assoc_timeout();
        esp_wifi_connect();
    }

    void on_disconnected(void* event_data) {
        cancel_assoc_timeout();

        auto* info = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGI(TAG, "station disconnected (reason=%d)", info ? info->reason : -1);

        if (own_disconnect_pending_.exchange(false)) {
            // Our own teardown noise from connect_network()'s pre-reconnect
            // disconnect — the fresh attempt is already in flight, don't
            // report or retry against it.
            return;
        }

        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            cached_status_.connected = false;
        }

        bool was_explicit = explicit_connect_pending_.exchange(false);
        if (was_explicit && has_password_configured()) {
            fire_event("AUTH_FAILED", "Connection failed");
        } else {
            fire_event("DISCONNECTED");
        }

        schedule_retry();
    }

    void on_got_ip(ip_event_got_ip_t* event) {
        cancel_assoc_timeout();
        explicit_connect_pending_.store(false);
        retry_count_ = 0;

        char ip_str[16] = {};
        if (event) {
            std::snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        }

        wifi_ap_record_t ap_info = {};
        esp_err_t ap_rc = esp_wifi_sta_get_ap_info(&ap_info);

        {
            std::lock_guard<std::mutex> lock(cfg_mutex_);
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            cached_status_.connected = true;
            cached_status_.ssid = current_ssid_;
            cached_status_.ip_address = ip_str;
            if (ap_rc == ESP_OK) {
                cached_status_.signal_strength = rssi_to_percent(ap_info.rssi);
                cached_status_.frequency_mhz = channel_to_mhz(ap_info.primary);
                char bssid_str[18];
                std::snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                              ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                              ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
                cached_status_.bssid = bssid_str;
            }
        }

        uint8_t mac[6] = {};
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
            char mac_str[18];
            std::snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                          mac[2], mac[3], mac[4], mac[5]);
            std::lock_guard<std::mutex> lock(status_mutex_);
            cached_status_.mac_address = mac_str;
        }

        ESP_LOGI(TAG, "got ip: %s", ip_str);
        fire_event("CONNECTED");
    }

    void on_scan_done() {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        num = std::min(num, MAX_SCAN_RESULTS);

        std::vector<wifi_ap_record_t> records(num);
        if (num > 0) {
            esp_wifi_scan_get_ap_records(&num, records.data());
        }

        std::vector<WiFiNetwork> networks;
        networks.reserve(num);
        for (uint16_t i = 0; i < num; ++i) {
            const wifi_ap_record_t& rec = records[i];
            std::string ssid(reinterpret_cast<const char*>(rec.ssid));
            if (ssid.empty()) {
                continue; // skip hidden networks, matches desktop backends
            }
            bool secured = false;
            std::string sec_type;
            auth_mode_to_security(rec.authmode, secured, sec_type);
            networks.emplace_back(ssid, rssi_to_percent(rec.rssi), secured, sec_type,
                                  channel_to_mhz(rec.primary));
        }

        // Deduplicate by SSID (keep strongest signal), same as desktop backends.
        if (networks.size() > 1) {
            std::unordered_map<std::string, size_t> best_by_ssid;
            for (size_t i = 0; i < networks.size(); ++i) {
                auto it = best_by_ssid.find(networks[i].ssid);
                if (it == best_by_ssid.end()) {
                    best_by_ssid[networks[i].ssid] = i;
                } else if (networks[i].signal_strength > networks[it->second].signal_strength) {
                    it->second = i;
                }
            }
            if (best_by_ssid.size() < networks.size()) {
                std::vector<WiFiNetwork> deduped;
                deduped.reserve(best_by_ssid.size());
                for (const auto& [ssid, idx] : best_by_ssid) {
                    deduped.push_back(networks[idx]);
                }
                networks = std::move(deduped);
            }
        }

        {
            std::lock_guard<std::mutex> lock(networks_mutex_);
            cached_networks_ = std::move(networks);
        }
        ESP_LOGI(TAG, "scan complete: %u networks", static_cast<unsigned>(num));
        fire_event("SCAN_COMPLETE");
    }
};

} // namespace

namespace helix {

std::unique_ptr<WifiBackend> create_platform_wifi_backend(bool silent) {
    auto backend = std::make_unique<WifiBackendEsp>();
    backend->set_silent(silent);
    return backend;
}

} // namespace helix
