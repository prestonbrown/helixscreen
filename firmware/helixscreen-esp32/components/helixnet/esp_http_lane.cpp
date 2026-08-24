// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EspHttpLane implementation. See esp_http_lane.h for the contract.

#include "esp_http_lane.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <pthread.h>
#include <utility>

namespace helix::http {

namespace {
constexpr char TAG[] = "esp_http_lane";
// Lazily claimed on first submit_get() — NOT at boot. The boot internal-RAM
// gates are tight (THE PATTERN: no runtime internal-RAM allocation >=32KB
// after WiFi start); a post-boot 16KB stack claim mirrors app_net_start()'s
// late pthread spawn in app_boot.cpp.
constexpr size_t WORKER_STACK_BYTES = 16 * 1024;
constexpr int HTTP_TIMEOUT_MS = 15000;
// esp_http_client's own internal read-chunk buffer (config.buffer_size) —
// small and fine in internal RAM. Only the accumulation buffer built up in
// run_one() below needs to be PSRAM; that's the buffer the R3 "PSRAM buffer,
// capped" requirement is about.
constexpr size_t CLIENT_BUFFER_BYTES = 4096;
} // namespace

EspHttpLane& EspHttpLane::instance() {
    static EspHttpLane lane;
    return lane;
}

bool EspHttpLane::submit_get(std::string url, size_t range_max_bytes, FetchSuccessCb on_success,
                             FetchErrorCb on_error) {
    const size_t cap = clamp_fetch_cap(range_max_bytes);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!slots_.try_acquire()) {
            ESP_LOGW(TAG, "queue full (%u/%u) — rejecting %s", (unsigned)slots_.in_flight(),
                     (unsigned)slots_.max_depth(), url.c_str());
            return false;
        }
        queue_.push_back(Job{std::move(url), cap, std::move(on_success), std::move(on_error)});

        // The worker is the only thing that drains the queue and releases
        // slots. Without it the job sits forever and its slot is never
        // returned, so QUEUE_DEPTH failed submissions would wedge the lane for
        // the rest of the session. Undo the push and the acquire instead.
        if (!ensure_worker_started_locked()) {
            queue_.pop_back();
            slots_.release();
            return false;
        }
    }

    cv_.notify_one();
    return true;
}

bool EspHttpLane::ensure_worker_started_locked() {
    if (worker_started_) {
        return true;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK_BYTES);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t thread;
    int rc = pthread_create(&thread, &attr, &EspHttpLane::worker_main, this);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        ESP_LOGE(TAG, "pthread_create failed: %d — rejecting this submission", rc);
        return false; // worker_started_ stays false: a later submit_get() retries the spawn.
    }
    worker_started_ = true;
    return true;
}

void* EspHttpLane::worker_main(void* self) {
    static_cast<EspHttpLane*>(self)->worker_loop();
    return nullptr;
}

// Runs forever. No shutdown path: process lifetime = power cycle (same as
// MoonrakerManager / PrinterState — no code anywhere tears those down either).
// EspHttpLane::instance() is a function-local static that never goes out of
// scope, so nothing ever needs to join or cancel this thread; the loop simply
// outlives the (never-ending) process. Documented per R2 — not an oversight.
void EspHttpLane::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty(); });
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        run_one(job);

        std::lock_guard<std::mutex> lock(mutex_);
        slots_.release();
    }
}

void EspHttpLane::run_one(const Job& job) {
    esp_http_client_config_t config = {};
    config.url = job.url.c_str();
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.buffer_size = CLIENT_BUFFER_BYTES;
    config.method = HTTP_METHOD_GET;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        if (job.on_error) {
            job.on_error("esp_http_client_init failed");
        }
        return;
    }

    // Bounded prefix via Range — mirrors desktop's download_file_partial
    // (src/api/moonraker_file_transfer_api.cpp): "bytes=0-{cap-1}", inclusive.
    // Servers that ignore Range return 200 with the full body; the read loop
    // below enforces the cap regardless of what the server does with Range.
    std::string range = "bytes=0-" + std::to_string(job.cap - 1);
    esp_http_client_set_header(client, "Range", range.c_str());

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        if (job.on_error) {
            job.on_error(std::string("esp_http_client_open: ") + esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
        return;
    }

    // Informational only (some servers/chunked responses don't report a
    // reliable Content-Length) — the read loop + is_complete check below is
    // what actually enforces the cap.
    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (status != 200 && status != 206) {
        if (job.on_error) {
            job.on_error("HTTP " + std::to_string(status));
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // Accumulation buffer in PSRAM — this is the buffer the internal-RAM
    // budget cares about, not esp_http_client's own small read-chunk buffer
    // (config.buffer_size above, internal RAM, CLIENT_BUFFER_BYTES only).
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(job.cap, MALLOC_CAP_SPIRAM));
    if (!buf) {
        if (job.on_error) {
            job.on_error("PSRAM allocation failed");
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    size_t total = 0;
    while (total < job.cap) {
        int n = esp_http_client_read(client, reinterpret_cast<char*>(buf + total),
                                     static_cast<int>(job.cap - total));
        if (n < 0) {
            if (job.on_error) {
                job.on_error("esp_http_client_read failed");
            }
            heap_caps_free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return;
        }
        if (n == 0) {
            break; // response complete
        }
        total += static_cast<size_t>(n);
    }

    // Over-cap: the buffer filled and the server says there's more. Abort and
    // report an error — R3 hard constraint: never truncate-and-return.
    const bool over_cap = (total >= job.cap) && !esp_http_client_is_complete_data_received(client);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (over_cap) {
        ESP_LOGW(TAG, "response exceeds %u byte cap — aborting: %s", (unsigned)job.cap,
                 job.url.c_str());
        heap_caps_free(buf);
        if (job.on_error) {
            job.on_error("response exceeds size cap");
        }
        return;
    }

    if (job.on_success) {
        job.on_success(buf, total);
    }
    heap_caps_free(buf);
}

} // namespace helix::http
