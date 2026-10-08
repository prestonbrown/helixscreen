// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_snapshot.h"

#include "app_boot.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_glue.h"
#include "mbedtls/base64.h"
#include "miniz.h"
#include "touch_input.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "serial_snapshot";
static atomic_bool s_requested;
static atomic_bool s_notes_requested;
static atomic_int s_resend_line = -1;

// Reads the console UART through its driver: without one, nothing delivers
// received bytes. Log output keeps writing the same UART as before.
static void reader_task(void* arg) {
    (void)arg;
    char line[24];
    size_t len = 0;
    for (;;) {
        uint8_t byte;
        if (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &byte, 1, pdMS_TO_TICKS(200)) != 1) {
            continue;
        }
        const int c = byte;
        if (c == '\r' || c == '\n') {
            line[len] = '\0';
            int x = 0;
            int y = 0;
            int seq = 0;
            if (strcmp(line, "snap") == 0) {
                atomic_store(&s_requested, true);
            } else if (strcmp(line, "notes") == 0) {
                atomic_store(&s_notes_requested, true);
            } else if (sscanf(line, "snapline %d", &seq) == 1) {
                atomic_store(&s_resend_line, seq);
            } else if (sscanf(line, "tdown %d %d", &x, &y) == 2) {
                extern void touch_input_inject_hold(int down, int x, int y);
                touch_input_inject_hold(1, x, y);
            } else if (sscanf(line, "tmove %d %d", &x, &y) == 2) {
                extern void touch_input_inject_hold(int down, int x, int y);
                touch_input_inject_hold(1, x, y);
            } else if (sscanf(line, "tup %d %d", &x, &y) == 2) {
                extern void touch_input_inject_hold(int down, int x, int y);
                touch_input_inject_hold(0, x, y);
            } else if (sscanf(line, "tap %d %d", &x, &y) == 2) {
                touch_input_inject_tap(x, y);
            }
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

void serial_snapshot_start(void) {
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM) &&
        uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "console UART driver unavailable; snapshots unavailable");
        return;
    }
    if (xTaskCreate(reader_task, "snap_rx", 2560, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the console reader; snapshots unavailable");
    }
}

// The last dump stays in PSRAM so "snapline N" can resend one line of it.
// Task-context output (esp_log, printf) shares one locked stdout, so it never
// lands inside a line, though a log written in pieces (the WiFi driver's) can
// leave its prefix in front of one. ROM/ISR output such as a task_wdt report
// can split a line; its crc catches that.
#define SNAP_LINE_BYTES 57 // -> one 76-char base64 line

static unsigned char* s_dump;
static size_t s_dump_len;
static size_t s_dump_cap;

// Grown in fixed steps, so the buffer stays close to the compressed size.
#define SNAP_GROW_BYTES (16 * 1024)

static mz_bool put_buf(const void* buf, int len, void* user) {
    (void)user;
    if (s_dump_len + (size_t)len > s_dump_cap) {
        const size_t need = s_dump_len + (size_t)len;
        const size_t cap = (need + SNAP_GROW_BYTES - 1) / SNAP_GROW_BYTES * SNAP_GROW_BYTES;
        unsigned char* grown = heap_caps_realloc(s_dump, cap, MALLOC_CAP_SPIRAM);
        if (!grown) {
            return MZ_FALSE;
        }
        s_dump = grown;
        s_dump_cap = cap;
    }
    memcpy(s_dump + s_dump_len, buf, (size_t)len);
    s_dump_len += (size_t)len;
    return MZ_TRUE;
}

static void print_line(size_t seq) {
    const unsigned char* raw = s_dump + seq * SNAP_LINE_BYTES;
    size_t len = s_dump_len - seq * SNAP_LINE_BYTES;
    if (len > SNAP_LINE_BYTES) {
        len = SNAP_LINE_BYTES;
    }
    unsigned char b64[80];
    size_t n = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &n, raw, len);
    // The crc covers "<seq> " too, so a damaged sequence number fails the line.
    char seq_text[12];
    const int seq_len = snprintf(seq_text, sizeof(seq_text), "%u ", (unsigned)seq);
    const mz_ulong crc = mz_crc32(mz_crc32(0, (const unsigned char*)seq_text, seq_len), raw, len);
    printf("SNAP:%s%08lx %.*s\n", seq_text, (unsigned long)crc, (int)n, b64);
}

static size_t line_count(void) {
    return (s_dump_len + SNAP_LINE_BYTES - 1) / SNAP_LINE_BYTES;
}

void serial_snapshot_poll(void) {
    if (atomic_exchange(&s_notes_requested, false)) {
        app_boot_print_notifications();
    }
    const int resend = atomic_exchange(&s_resend_line, -1);
    if (resend >= 0 && (size_t)resend < line_count()) {
        print_line((size_t)resend);
    }
    if (!atomic_exchange(&s_requested, false)) {
        return;
    }
    // Rows are compressed straight from the frame the panel shows, so the only
    // large allocations are the compressor and the compressed output.
    uint32_t w = 0;
    uint32_t h = 0;
    size_t stride = 0;
    const uint8_t* frame = lvgl_glue_frame(&w, &h, &stride);
    tdefl_compressor* comp = heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    if (!comp) {
        printf("\n=====HELIX-SNAP-ERROR no memory for the compressor (%u bytes)\n",
               (unsigned)sizeof(tdefl_compressor));
        return;
    }
    s_dump_len = 0;
    tdefl_init(comp, put_buf, NULL, 128);
    tdefl_status status = TDEFL_STATUS_OKAY;
    for (uint32_t y = 0; y < h && status == TDEFL_STATUS_OKAY; ++y) {
        status = tdefl_compress_buffer(comp, frame + y * stride, w * 2,
                                       y + 1 == h ? TDEFL_FINISH : TDEFL_NO_FLUSH);
    }
    heap_caps_free(comp);
    if (status != TDEFL_STATUS_DONE) {
        s_dump_len = 0;
        printf("\n=====HELIX-SNAP-ERROR no memory for the compressed image\n");
        return;
    }

    // Streaming at 115200 baud takes seconds; yielding every line keeps this
    // CPU's idle task, and so the task watchdog, fed meanwhile.
    const size_t lines = line_count();
    printf("\n=====HELIX-SNAP %lu %lu RGB565 DEFLATE %u %08lx %u\n", (unsigned long)w,
           (unsigned long)h, (unsigned)s_dump_len, (unsigned long)mz_crc32(0, s_dump, s_dump_len),
           (unsigned)lines);
    for (size_t seq = 0; seq < lines; ++seq) {
        print_line(seq);
        vTaskDelay(1);
    }
    printf("=====HELIX-SNAP-END %u\n", (unsigned)s_dump_len);
    // The host repeats "snap" until a dump starts; those repeats are answered
    // by this dump, and another would replace the lines it may still resend.
    atomic_store(&s_requested, false);
}
