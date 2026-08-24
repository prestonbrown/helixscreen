// SPDX-License-Identifier: GPL-3.0-or-later
//
// net_hil — Plan 3 Task 10 network hardware-in-the-loop scenario. Test-only:
// brings up the real WifiBackend (Task 13, wifi_backend_esp.cpp — over
// esp_wifi directly, not the shared WiFiManager singleton, since this build
// never runs app_boot.cpp's app_net_start() and so never races it) and drives
// a live Moonraker WebSocket session through EspMoonrakerClient to validate
// the transport on real K-Touch hardware while the display stays up.
// Entirely gated behind CONFIG_HELIX_NET_HIL (default n) — this translation
// unit compiles to nothing when the option is off. See
// .superpowers/sdd/task-10-brief.md for the exact scenario contract.

#include "sdkconfig.h"

#if CONFIG_HELIX_NET_HIL

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_moonraker_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_backend.h"
#include "wifi_backend_esp.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <pthread.h>

extern "C" void net_hil_start(void);

namespace {

constexpr char TAG[] = "net_hil";
// esp-idf#14918: a TX can block 10s+ behind an in-progress RX without the
// separate TX lock (sdkconfig.defaults sets CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK).
// Anything above this during the probe means that class of contention is back.
constexpr int64_t TX_LOCK_FAIL_THRESHOLD_MS = 2000;
constexpr int64_t SCENARIO_WINDOW_MS = 60000;
constexpr int64_t PING_CADENCE_MS = 15000;
constexpr int64_t PROBE_AT_MS = 30000;
constexpr int PROBE_BURST_COUNT = 5;
constexpr uint32_t HEAP_FLAT_TOLERANCE_BYTES = 8192;

// Counters updated from the WS task (notify callback, RPC error callbacks) and
// read from the HIL thread — plain atomics, no ordering requirements beyond
// visibility.
std::atomic<uint32_t> s_msgs{0};
std::atomic<uint32_t> s_drops{0};
std::atomic<uint32_t> s_max_msg{0};

// Process-lifetime singleton: this scenario never tears the client down, so
// the dtor's quiesce path is deliberately not exercised here (out of scope —
// Plan 4 owns real client lifecycle). Leaked on purpose.
helix::IMoonrakerClient* s_client = nullptr;

// Task 13: process-lifetime backend instance, owned directly (not through
// the shared WiFiManager singleton) — this scenario never runs alongside
// app_boot.cpp's app_net_start() (CONFIG_HELIX_NET_HIL gates it off there),
// so there's no second owner to collide with. Leaked on purpose, same
// rationale as s_client above.
std::unique_ptr<WifiBackend> s_wifi_backend;

// Test-only WiFi station bring-up over the real WifiBackend (Task 13,
// wifi_backend_esp.cpp). Explicitly connects with the Kconfig SSID/password
// (the "HelixScreen Network" menu) rather than relying on NVS/first-boot-seed
// timing, so the scenario always joins the configured HIL network regardless
// of whatever credentials a prior production-path boot may have stored.
void wifi_init_station(void) {
    helix::wifi_backend_esp_allow_hardware_bringup();
    s_wifi_backend = helix::create_platform_wifi_backend(/*silent=*/false);

    SemaphoreHandle_t connected_sem = xSemaphoreCreateBinary();
    s_wifi_backend->register_event_callback(
        "CONNECTED", [connected_sem](const std::string&) { xSemaphoreGive(connected_sem); });

    WiFiError start_err = s_wifi_backend->start();
    if (!start_err.success()) {
        ESP_LOGE(TAG, "wifi backend start failed: %s", start_err.technical_msg.c_str());
    }
    WiFiError connect_err =
        s_wifi_backend->connect_network(CONFIG_HELIX_HIL_WIFI_SSID, CONFIG_HELIX_HIL_WIFI_PASS);
    if (!connect_err.success()) {
        ESP_LOGE(TAG, "wifi backend connect_network failed: %s", connect_err.technical_msg.c_str());
    }

    ESP_LOGI(TAG, "waiting for wifi connection (ssid=\"%s\")...", CONFIG_HELIX_HIL_WIFI_SSID);
    xSemaphoreTake(connected_sem, portMAX_DELAY);
    vSemaphoreDelete(connected_sem);
    ESP_LOGI(TAG, "wifi up");
}

// Synchronous printer.info round trip: blocks the HIL thread on a binary
// semaphore given from the WS task's success/error callback, and logs the
// round-trip latency. Returns the latency in milliseconds.
int64_t timed_printer_info() {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    int64_t t0 = esp_timer_get_time();

    s_client->send_jsonrpc(
        "printer.info", json::object(), [done](const json&) { xSemaphoreGive(done); },
        [done](const MoonrakerError& err) {
            ESP_LOGW(TAG, "printer.info error: %s", err.message.c_str());
            s_drops.fetch_add(1, std::memory_order_relaxed);
            xSemaphoreGive(done);
        });

    // Bounded wait: a truly hung transport must not wedge this thread forever.
    if (xSemaphoreTake(done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "printer.info timed out waiting for reply");
        s_drops.fetch_add(1, std::memory_order_relaxed);
    }
    vSemaphoreDelete(done);

    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "printer.info rtt=%lldms", static_cast<long long>(dt_ms));
    if (dt_ms > TX_LOCK_FAIL_THRESHOLD_MS) {
        ESP_LOGE(TAG, "FAIL tx-lock latency=%lldms", static_cast<long long>(dt_ms));
    }
    return dt_ms;
}

void on_temp_notify(const json& msg) {
    // notify_status_update carries the full JSON-RPC envelope; approximate the
    // WS message size from it since the client doesn't expose one (brief note:
    // callback-side approximation, not the exact wire byte count).
    size_t approx_size = msg.dump().size();
    uint32_t prev_max = s_max_msg.load(std::memory_order_relaxed);
    while (approx_size > prev_max &&
           !s_max_msg.compare_exchange_weak(prev_max, static_cast<uint32_t>(approx_size),
                                            std::memory_order_relaxed)) {
    }

    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }
    const json& status = msg["params"][0];

    // notify_status_update carries DELTAS: a given update usually contains ONLY
    // extruder OR heater_bed, not both. Carry last-known values so the log always
    // shows real temps — defaulting the absent field to 0.0 produced fake
    // "extruder=28.7 bed=0.0" / "extruder=0.0 bed=28.0" flapping every delta.
    static double s_last_extruder = 0.0;
    static double s_last_bed = 0.0;
    bool has_temp = false;
    if (status.contains("extruder") && status["extruder"].contains("temperature")) {
        s_last_extruder = status["extruder"]["temperature"].get<double>();
        has_temp = true;
    }
    if (status.contains("heater_bed") && status["heater_bed"].contains("temperature")) {
        s_last_bed = status["heater_bed"]["temperature"].get<double>();
        has_temp = true;
    }
    if (!has_temp) {
        return;
    }
    s_msgs.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGI(TAG, "extruder=%.1f bed=%.1f", s_last_extruder, s_last_bed);
}

void* hil_thread_main(void*) {
    // CONFIG_HELIX_HIL_MOONRAKER_URL defaults to empty — the bench address it used
    // to carry is not something to ship. Without this check the connect below
    // never completes and the xSemaphoreTake(portMAX_DELAY) parks this thread for
    // good, which reads as a hung scenario rather than a missing setting.
    if (CONFIG_HELIX_HIL_MOONRAKER_URL[0] == '\0') {
        ESP_LOGE(TAG, "FAIL CONFIG_HELIX_HIL_MOONRAKER_URL is empty — set it in sdkconfig.local");
        return nullptr;
    }

    std::unique_ptr<helix::IMoonrakerClient> client = helix::create_platform_moonraker_client();
    s_client = client.release(); // process-lifetime singleton; see header comment

    SemaphoreHandle_t connected_sem = xSemaphoreCreateBinary();
    s_client->connect(
        CONFIG_HELIX_HIL_MOONRAKER_URL, [connected_sem]() { xSemaphoreGive(connected_sem); },
        []() { ESP_LOGW(TAG, "moonraker disconnected"); });

    ESP_LOGI(TAG, "waiting for moonraker connect (%s)...", CONFIG_HELIX_HIL_MOONRAKER_URL);
    xSemaphoreTake(connected_sem, portMAX_DELAY);
    vSemaphoreDelete(connected_sem);
    ESP_LOGI(TAG, "moonraker connected");

    {
        SemaphoreHandle_t info_done = xSemaphoreCreateBinary();
        s_client->send_jsonrpc("server.info", json::object(), [info_done](const json& resp) {
            std::string klippy_state =
                resp.value(json::json_pointer("/result/klippy_state"), std::string("?"));
            std::string moonraker_version =
                resp.value(json::json_pointer("/result/moonraker_version"), std::string("?"));
            ESP_LOGI(TAG, "server.info klippy_state=%s moonraker_version=%s", klippy_state.c_str(),
                     moonraker_version.c_str());
            xSemaphoreGive(info_done);
        });
        if (xSemaphoreTake(info_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
            ESP_LOGE(TAG, "server.info timed out");
        }
        vSemaphoreDelete(info_done);
    }

    // Exercise the real discovery chain (Task 7) end-to-end and log its results.
    // discover_printer() issues its own printer.objects.subscribe, which REPLACES
    // the per-connection subscription — so run it to completion here, BEFORE the
    // manual extruder+heater_bed subscribe below re-establishes the minimal set
    // the temp oracle (on_temp_notify) consumes. Nothing else invokes
    // discover_printer on this boot: MoonrakerManager's client doesn't connect
    // until Task 8, so this HIL scenario is its only caller.
    {
        s_client->set_on_hardware_discovered([](const helix::PrinterDiscovery& hw) {
            ESP_LOGI(TAG,
                     "discovery hardware: %u heaters, %u sensors, %u fans, %u leds, %u "
                     "filament-sensors",
                     static_cast<unsigned>(hw.heaters().size()),
                     static_cast<unsigned>(hw.sensors().size()),
                     static_cast<unsigned>(hw.fans().size()),
                     static_cast<unsigned>(hw.leds().size()),
                     static_cast<unsigned>(hw.filament_sensor_names().size()));
        });
        s_client->set_on_discovery_complete([](const helix::PrinterDiscovery& hw,
                                               const json& initial_status) {
            ESP_LOGI(TAG, "discovery complete: %u objects in initial status, mmu=%d toolchanger=%d",
                     static_cast<unsigned>(initial_status.is_object() ? initial_status.size() : 0),
                     static_cast<int>(hw.has_mmu()), static_cast<int>(hw.has_tool_changer()));
        });

        SemaphoreHandle_t disc_done = xSemaphoreCreateBinary();
        s_client->discover_printer(
            [disc_done]() {
                ESP_LOGI(TAG, "discover_printer done");
                xSemaphoreGive(disc_done);
            },
            [disc_done](const std::string& reason) {
                ESP_LOGW(TAG, "discover_printer error: %s", reason.c_str());
                xSemaphoreGive(disc_done);
            });
        if (xSemaphoreTake(disc_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGE(TAG, "discover_printer timed out");
        }
        vSemaphoreDelete(disc_done);
    }

    // Baseline heap AFTER connect + server.info so setup allocations (WS
    // buffers, request tracker) don't skew the 60s flatness check.
    uint32_t heap_baseline = esp_get_free_heap_size();
    uint32_t psram_baseline = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    json sub_params = {
        {"objects", {{"extruder", {"temperature"}}, {"heater_bed", {"temperature"}}}}};
    s_client->send_jsonrpc("printer.objects.subscribe", sub_params,
                           [](const json&) { ESP_LOGI(TAG, "subscribed extruder+heater_bed"); });
    s_client->register_notify_update(on_temp_notify);

    int64_t t_start_ms = esp_timer_get_time() / 1000;
    int64_t next_ping_ms = PING_CADENCE_MS;
    bool probe_done = false;

    while (true) {
        int64_t elapsed_ms = (esp_timer_get_time() / 1000) - t_start_ms;
        if (elapsed_ms >= SCENARIO_WINDOW_MS) {
            break;
        }
        if (!probe_done && elapsed_ms >= PROBE_AT_MS) {
            // TX-during-RX probe: 5 back-to-back printer.info RPCs while the
            // temp subscription is actively streaming (esp-idf#14918 class).
            ESP_LOGI(TAG, "tx-lock probe: %d back-to-back printer.info", PROBE_BURST_COUNT);
            for (int i = 0; i < PROBE_BURST_COUNT; ++i) {
                timed_printer_info();
            }
            probe_done = true;
            next_ping_ms = PROBE_AT_MS + PING_CADENCE_MS;
            continue;
        }
        if (elapsed_ms >= next_ping_ms) {
            timed_printer_info();
            next_ping_ms += PING_CADENCE_MS;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    uint32_t heap_now = esp_get_free_heap_size();
    uint32_t psram_now = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    int64_t heap_delta = static_cast<int64_t>(heap_now) - static_cast<int64_t>(heap_baseline);
    if (heap_delta < 0) {
        heap_delta = -heap_delta;
    }
    if (static_cast<uint32_t>(heap_delta) > HEAP_FLAT_TOLERANCE_BYTES) {
        ESP_LOGE(TAG, "FAIL heap drift=%lldB baseline=%u now=%u",
                 static_cast<long long>(heap_delta), heap_baseline, heap_now);
    } else {
        ESP_LOGI(TAG, "heap flat: baseline=%u now=%u delta=%lldB", heap_baseline, heap_now,
                 static_cast<long long>(heap_delta));
    }
    (void)psram_baseline;

    ESP_LOGI(TAG, "PASS msgs=%u drops=%u max_msg=%u heap_free=%u psram_free=%u",
             s_msgs.load(std::memory_order_relaxed), s_drops.load(std::memory_order_relaxed),
             s_max_msg.load(std::memory_order_relaxed), heap_now, psram_now);

    // Park forever: the subscription keeps streaming independently of this
    // thread, so serial capture past 60s still shows live temp notifications.
    // No exit-and-cleanup by design (client lifetime is process-scoped here).
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
    return nullptr;
}

} // namespace

extern "C" void net_hil_start(void) {
    // Blocks until WiFi has an IP, then spawns the HIL thread. Runs on its
    // own task (see app_main) so a slow or absent network can never hold the
    // display off — DHCP at weak RSSI was observed taking 75s+.
    wifi_init_station();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t thread;
    int rc = pthread_create(&thread, &attr, hil_thread_main, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to start HIL thread: %d", rc);
    }
}

#endif // CONFIG_HELIX_NET_HIL
