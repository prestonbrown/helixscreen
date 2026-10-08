// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// EspHttpLane implementation. See esp_http_lane.h for the contract.

#include "esp_http_lane.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "psram_thread_stack.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <strings.h>
#include <utility>

namespace helix::http {

namespace {
constexpr char TAG[] = "esp_http_lane";
// Lazily claimed on first submit_get(), from PSRAM (see
// ensure_worker_started_locked).
constexpr size_t WORKER_STACK_BYTES = 16 * 1024;
// Connecting and reading the headers: a busy Moonraker on a weak link can take
// seconds to answer, and one timeout here fails the request.
constexpr int HTTP_TIMEOUT_MS = 15000;
// Each socket read of the body: short, so a stalled read comes back to
// read_capped_body() to be timed against BODY_DEADLINES rather than blocking.
constexpr int BODY_READ_TIMEOUT_MS = 5000;
// A thumbnail is a few KB: a body still arriving after 30 s, or silent for
// 10 s, is a link that has stopped, and the lane has other cards waiting.
constexpr LaneDeadlines BODY_DEADLINES{30000, 10000};
// esp_http_client's own internal read-chunk buffer (config.buffer_size) —
// small and fine in internal RAM. Only the accumulation buffer built up in
// run_one() below needs to be PSRAM; that's the buffer the R3 "PSRAM buffer,
// capped" requirement is about.
constexpr size_t CLIENT_BUFFER_BYTES = 4096;

std::atomic<EspHttpLane::DateHeaderHook> s_date_hook{nullptr};
std::atomic<EspHttpLane::WorkerStartHook> s_worker_start_hook{nullptr};

esp_err_t on_http_event(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Date") == 0) {
        if (auto hook = s_date_hook.load()) {
            hook(evt->header_value);
        }
    }
    return ESP_OK;
}
} // namespace

void EspHttpLane::set_date_header_hook(DateHeaderHook hook) {
    s_date_hook.store(hook);
}

void EspHttpLane::set_worker_start_hook(WorkerStartHook hook) {
    s_worker_start_hook.store(hook);
}

EspHttpLane& EspHttpLane::instance() {
    static EspHttpLane lane;
    return lane;
}

size_t EspHttpLane::free_slots() {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.max_depth() - slots_.in_flight();
}

bool EspHttpLane::submit_get(std::string url, size_t range_max_bytes, FetchSuccessCb on_success,
                             FetchErrorCb on_error, FetchCancelFlag cancelled) {
    const size_t cap = clamp_fetch_cap(range_max_bytes);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!slots_.try_acquire()) {
            ESP_LOGW(TAG, "queue full (%u/%u) — rejecting %s", (unsigned)slots_.in_flight(),
                     (unsigned)slots_.max_depth(), url.c_str());
            return false;
        }
        queue_.push_back(Job{std::move(url), cap, std::move(on_success), std::move(on_error),
                             std::move(cancelled)});

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

    // The stack goes in PSRAM: after WiFi is up the internal heap's largest
    // block can be smaller than the stack, and what it has is WiFi/lwIP headroom.
    // The worker-start hook bars the thread from storage (psram_thread_stack.h).
    pthread_t thread;
    int rc;
    {
        helix::PsramThreadStackScope psram_stack("http_lane", WORKER_STACK_BYTES);
        rc = pthread_create(&thread, &attr, &EspHttpLane::worker_main, this);
    }
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        ESP_LOGE(TAG, "pthread_create failed: %d — rejecting this submission", rc);
        return false; // worker_started_ stays false: a later submit_get() retries the spawn.
    }
    worker_started_ = true;
    return true;
}

void* EspHttpLane::worker_main(void* self) {
    if (auto hook = s_worker_start_hook.load()) {
        hook();
    }
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

        if (job.cancelled && job.cancelled->load()) {
            if (job.on_error) {
                job.on_error("cancelled");
            }
        } else {
            run_one(job);
        }

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
    config.event_handler = &on_http_event;

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

    // Content-Length sizes the buffer; for a Range request it is the length of
    // the range. The read loop and is_complete check below enforce the cap.
    const int64_t content_length = esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    if (status != 200 && status != 206) {
        if (job.on_error) {
            job.on_error("HTTP " + std::to_string(status));
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // Accumulation buffer, in PSRAM: this is the buffer the RAM budget cares
    // about, not esp_http_client's own small read-chunk buffer
    // (config.buffer_size above, CLIENT_BUFFER_BYTES).
    esp_http_client_set_timeout_ms(client, BODY_READ_TIMEOUT_MS);
    struct Transport {
        esp_http_client_handle_t client;
        int read(char* buf, int len) {
            const int n = esp_http_client_read(client, buf, len);
            return n == -ESP_ERR_HTTP_EAGAIN ? TRANSPORT_AGAIN : n;
        }
        bool complete() const {
            return esp_http_client_is_complete_data_received(client);
        }
    } transport{client};
    std::string body;
    const BodyRead read = read_capped_body(
        transport, job.cap, content_length, body, [] { return esp_timer_get_time() / 1000; },
        BODY_DEADLINES, job.cancelled.get());

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read != BodyRead::Ok) {
        char why[64];
        if (read == BodyRead::Stalled) {
            snprintf(why, sizeof(why), "stalled: no data for %lld ms",
                     static_cast<long long>(BODY_DEADLINES.stall_ms));
        } else if (read == BodyRead::TimedOut) {
            snprintf(why, sizeof(why), "timed out after %lld ms",
                     static_cast<long long>(BODY_DEADLINES.total_ms));
        } else {
            snprintf(why, sizeof(why), "%s",
                     read == BodyRead::AllocFailed  ? "PSRAM allocation failed"
                     : read == BodyRead::ReadFailed ? "esp_http_client_read failed"
                     : read == BodyRead::OverCap    ? "response exceeds size cap"
                                                    : "cancelled");
        }
        if (read != BodyRead::Cancelled) {
            ESP_LOGW(TAG, "%s: %s", why, job.url.c_str());
        }
        if (job.on_error) {
            job.on_error(why);
        }
        return;
    }

    if (job.on_success) {
        job.on_success(body);
    }
}

} // namespace helix::http
