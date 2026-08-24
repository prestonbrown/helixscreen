// SPDX-License-Identifier: GPL-3.0-or-later
#include "lvgl_glue.h"

#include "app_boot.h"
#include "board_display.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ktouch.h"
#include "lvgl.h"
#include "ota_health.h"
#include "src/xml/lv_xml.h"
#include "touch_input.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

static const char* TAG = "lvgl_glue";
static esp_lcd_panel_handle_t s_panel;
static void (*s_ui_build)(void);
static void (*s_ui_tick)(void);

// ---------------------------------------------------------------------------
// Shadow framebuffer + vsync presenter (D2 tear fix), with a two-hop blit.
//
// The panel framebuffer is scanned out continuously by the RGB bounce ISR. Any
// write into the live FB races the scan beam: a copy that STARTS mid-frame is
// overtaken and tears (recovered by RESTART_IN_VSYNC as a visible glitch). HIL
// diag settled the parameters: vsync is per-frame (32.3Hz, confirmed valid beam
// clock), no scan-out desync, and the beam sweeps ~17.7 lines/ms. So the fix is
// to make the FB have exactly ONE writer that always starts at the vsync top:
//
//   - LVGL renders into its INTERNAL partial draw buffers as before (render
//     speed untouched) and flush_cb stages each chunk into a full-size PSRAM
//     SHADOW buffer, then returns immediately — LVGL never blocks on the panel.
//   - A dedicated high-priority PRESENTER task blocks on the vsync semaphore and,
//     on each frame it has a completed cycle to show, blits the shadow's dirty
//     band into the FB starting AT the vsync boundary, top-to-bottom.
//   - The FB is written ONLY by the presenter, only from frame top. No writer
//     ever races the beam, so scan-out tearing is structurally impossible.
//   - Frames present ATOMICALLY (whole dirty band per vsync), which also removes
//     the progressive chunk-by-chunk paint without rendering into PSRAM.
//
// TWO-HOP BLIT: the earlier direct shadow(PSRAM)->FB(PSRAM) blit measured only
// ~13 lines/ms — BELOW the beam — because a PSRAM->PSRAM stream thrashes the
// shared external-mem cache (shadow-read vs FB-write mutual eviction) and pays
// octal-PSRAM read<->write turnaround on every cache miss, on top of contending
// with the beam's own scan-out reads. So the presenter copies each band in two
// SEQUENTIAL same-direction passes: shadow(PSRAM) -> s_band (INTERNAL) via
// memcpy, then s_band -> FB(PSRAM) via draw_bitmap (a cache-buffered write, the
// fast direction). Same-direction sequential access avoids the turnaround +
// eviction thrash, recovering enough throughput to stay ahead of the beam for
// the dirty band. Band-sized (UI_BAND_LINES) to keep the internal buffer small.
//
// num_fbs stays 1 (double-FB + bounce desyncs scan-out — see board_display.c
// DO-NOT-RETRY). Shadow is 768KB PSRAM. Writer/presenter may touch overlapping
// shadow rows (benign: a one-frame blend of two near-identical consecutive
// renders — NOT scan-out garbage).
#define FB_BPP ((size_t)sizeof(lv_color16_t))
#define FB_STRIDE ((size_t)BOARD_LCD_H_RES * FB_BPP)
#define SHADOW_BYTES ((size_t)BOARD_LCD_V_RES * FB_STRIDE)

// Two-hop staging band, INTERNAL DRAM. UI_BAND_LINES full-width rows (mirrors
// the 10-line RGB bounce granularity). Allocated at display init AFTER the two
// boot heap gates (48KB UI stack, 32KB bounce) have already passed, so it can't
// threaten them; a failed alloc falls back to the direct PSRAM->PSRAM blit.
// Drop to 8 lines if internal DRAM proves tight.
#define UI_BAND_LINES 10
#define UI_BAND_BYTES ((size_t)UI_BAND_LINES * FB_STRIDE)

static uint8_t* s_shadow; // full-frame PSRAM shadow (LVGL chunks land here)
static uint8_t* s_band;   // internal-DRAM two-hop staging band (NULL => direct blit)

// vsync semaphore: given by the on_vsync ISR every frame, taken by the presenter
// to align each blit to a fresh frame top.
static SemaphoreHandle_t s_vsync_sem;

// Presenter handoff: the completed-cycle dirty Y-union, published by flush_cb on
// flush_is_last and consumed by the presenter. Guarded by a portMUX critical
// section (a few int assignments — never blocks). Full width; band = [y1, y2].
static portMUX_TYPE s_present_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_frame_pending;
static int32_t s_pub_y1;
static int32_t s_pub_y2;

// Dirty Y-union accumulated across the current (in-progress) refresh cycle.
static bool s_cur_valid;
static int32_t s_cur_y1;
static int32_t s_cur_y2;

// Scan-out underrun tripwire. In bounce mode the driver streams the PSRAM FB
// through the bounce ring via an EOF-ISR memcpy with a hard per-buffer
// deadline; when PSRAM bus contention makes that copy miss, the DMA re-sends
// stale lines (the one-frame downward-ghost glitch RESTART_IN_VSYNC then
// recovers). Public-API detector: on_frame_buf_complete fires once per frame
// ONLY when bounce_pos wraps the full FB — a frame whose EOFs coalesced never
// wraps, so (vsyncs - frame_completes) counts hard-underrun frames. Refills
// late by less than a full buffer period glitch identically but keep the wrap
// count, so this UNDERCOUNTS mild contention — a nonzero reading is proof, a
// zero is not exoneration (measured 741 late refills vs 34 counted here in one
// 10-line-bounce boot).
static volatile uint32_t s_vsync_n;
static volatile uint32_t s_frame_complete_n;

static bool flush_on_vsync(esp_lcd_panel_handle_t panel,
                           const esp_lcd_rgb_panel_event_data_t* edata, void* user_ctx) {
    (void)panel;
    (void)edata;
    (void)user_ctx;
    s_vsync_n++;
    BaseType_t high_task_woken = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static bool on_frame_buf_complete(esp_lcd_panel_handle_t panel,
                                  const esp_lcd_rgb_panel_event_data_t* edata, void* user_ctx) {
    (void)panel;
    (void)edata;
    (void)user_ctx;
    s_frame_complete_n++;
    return false;
}

// LVGL draw buffers — INTERNAL DRAM, PARTIAL mode (unchanged from the working
// bring-up; the shadow+presenter is layered on top of this, LVGL still renders
// into these). Static (link-time reserved, no runtime fragmentation lottery).
// Internal (not PSRAM) so LVGL's blends don't contend with scan-out. 12-line
// pair (2x19.2KB) double-buffers render N+1 while N is staged, at half the
// internal cost of the 24-line pair — the 48KB UI stack and 32KB RGB bounce DMA
// must still find contiguous internal blocks (see the boot heap-gate logs).
/* One 24-line buffer instead of the earlier 12-line double-buffer pair — SAME
 * 38.4KB internal total. Rationale: every chunk re-walks the widget tree and
 * re-resolves styles for each widget it intersects, so chunk count is a direct
 * multiplier on non-blend render overhead (measured: a 30-key buttonmatrix
 * rendered ~3x slower per pixel across 26 chunks than plain content). Halving
 * the chunks (40 -> 20 full-screen) buys more than double-buffering did: our
 * flush_cb is a fast synchronous memcpy into the PSRAM shadow (~1ms/chunk),
 * so render/stage overlap was worth almost nothing. */
#define UI_DRAW_BUF_LINES 24
#define UI_DRAW_BUF_BYTES (BOARD_LCD_H_RES * UI_DRAW_BUF_LINES * (int)FB_BPP)
LV_ATTRIBUTE_MEM_ALIGN static uint8_t s_draw_buf1[UI_DRAW_BUF_BYTES];

// XML/expat parsing recurses deeply during component registration and layout;
// the audit ran the full app slice on a 32KB pthread stack. 48KB gives margin
// for the real bring-up + panel construction. Kept INTERNAL (not PSRAM) — the
// UI thread does settings→flash writes, which cannot run from a PSRAM stack.
#define UI_THREAD_STACK_BYTES (48 * 1024)
// Presenter: tiny body (union read + banded blit). 4KB internal stack, high
// priority so it preempts to blit at the vsync boundary. No affinity — the
// external-RAM cache is shared across cores, so shadow reads are coherent
// wherever it lands; priority, not pinning, is what keeps it on the vsync edge.
#define PRESENT_STACK_BYTES 4096
#define PRESENT_TASK_PRIO 10

// Refresh-cycle cost tripwire: chunk count / staged px / first-to-last-chunk ms
// for each completed LVGL refresh cycle, logged only when the cycle is
// expensive (UI-thread stalls starve the polled touch indev — taps during a
// long cycle are dropped, so slow cycles ARE user-visible input loss). Cheap:
// three counters and one esp_timer read per chunk.
static uint32_t s_cyc_chunks;
static uint64_t s_cyc_px;
static int64_t s_cyc_t0_us;
#define CYCLE_LOG_MS 100

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    // Stage this chunk into the PSRAM shadow at its screen offset (internal
    // partial buffer -> PSRAM, row by row: the chunk is w*h tightly packed, the
    // shadow is full-width). Then extend the cycle's dirty union and return —
    // LVGL is free to reuse px_map immediately.
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    if (s_cyc_chunks == 0) {
        s_cyc_t0_us = esp_timer_get_time();
    }
    s_cyc_chunks++;
    s_cyc_px += (uint64_t)w * (uint64_t)h;
    size_t row_bytes = (size_t)w * FB_BPP;
    for (int32_t r = 0; r < h; r++) {
        uint8_t* dst = s_shadow + (size_t)(area->y1 + r) * FB_STRIDE + (size_t)area->x1 * FB_BPP;
        const uint8_t* src = px_map + (size_t)r * row_bytes;
        memcpy(dst, src, row_bytes);
    }

    if (!s_cur_valid) {
        s_cur_y1 = area->y1;
        s_cur_y2 = area->y2;
        s_cur_valid = true;
    } else {
        if (area->y1 < s_cur_y1)
            s_cur_y1 = area->y1;
        if (area->y2 > s_cur_y2)
            s_cur_y2 = area->y2;
    }

    if (lv_display_flush_is_last(disp)) {
        // Publish the completed cycle to the presenter. If the presenter hasn't
        // consumed the previous publish yet, MERGE unions — both cycles' renders
        // are already in the shadow, so a merged blit presents both atomically
        // and nothing is dropped.
        portENTER_CRITICAL(&s_present_mux);
        if (s_frame_pending) {
            if (s_cur_y1 < s_pub_y1)
                s_pub_y1 = s_cur_y1;
            if (s_cur_y2 > s_pub_y2)
                s_pub_y2 = s_cur_y2;
        } else {
            s_pub_y1 = s_cur_y1;
            s_pub_y2 = s_cur_y2;
            s_frame_pending = true;
        }
        portEXIT_CRITICAL(&s_present_mux);
        s_cur_valid = false;

        int64_t cyc_ms = (esp_timer_get_time() - s_cyc_t0_us) / 1000;
        if (cyc_ms >= CYCLE_LOG_MS) {
            ESP_LOGW(TAG, "slow refresh cycle: %ldms, %lu chunks, %llu px, band y[%ld..%ld]",
                     (long)cyc_ms, (unsigned long)s_cyc_chunks, (unsigned long long)s_cyc_px,
                     (long)s_pub_y1, (long)s_pub_y2);
        }
        s_cyc_chunks = 0;
        s_cyc_px = 0;
    }

    lv_display_flush_ready(disp); // immediate — LVGL never blocks on the panel
}

// Blit shadow rows [y1, y2] (full width) to the FB. Two-hop through the internal
// band buffer when available (see the s_band comment); direct otherwise.
static void present_blit(int32_t y1, int32_t y2) {
    if (s_band) {
        for (int32_t by = y1; by <= y2; by += UI_BAND_LINES) {
            int32_t bh = y2 - by + 1;
            if (bh > UI_BAND_LINES)
                bh = UI_BAND_LINES;
            // hop 1: shadow(PSRAM) -> internal band (sequential read)
            memcpy(s_band, s_shadow + (size_t)by * FB_STRIDE, (size_t)bh * FB_STRIDE);
            // hop 2: internal band -> FB(PSRAM) (cache-buffered write)
            esp_lcd_panel_draw_bitmap(s_panel, 0, by, BOARD_LCD_H_RES, by + bh, s_band);
        }
    } else {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y1, BOARD_LCD_H_RES, y2 + 1,
                                  s_shadow + (size_t)y1 * FB_STRIDE);
    }
}

// Presenter task: blits completed shadow frames to the FB, each aligned to a
// fresh vsync so the copy starts at frame top and outruns the beam.
static void present_task(void* arg) {
    (void)arg;
    while (true) {
        portENTER_CRITICAL(&s_present_mux);
        bool have = s_frame_pending;
        portEXIT_CRITICAL(&s_present_mux);

        if (!have) {
            // No completed frame — wait for a vsync (or 50ms) then re-check.
            // Tolerates empty vsyncs.
            xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(50));
            continue;
        }

        // Align the blit to a FRESH frame top: drop any stale token, then wait
        // the next vsync (bounded so a stalled panel can't wedge the presenter).
        xSemaphoreTake(s_vsync_sem, 0);
        xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(100));

        // Consume the pending union (may have merged more since we peeked).
        int32_t y1, y2;
        portENTER_CRITICAL(&s_present_mux);
        y1 = s_pub_y1;
        y2 = s_pub_y2;
        s_frame_pending = false;
        portEXIT_CRITICAL(&s_present_mux);

        if (y1 < 0)
            y1 = 0;
        if (y2 > BOARD_LCD_V_RES - 1)
            y2 = BOARD_LCD_V_RES - 1;
        if (y2 >= y1) {
            present_blit(y1, y2);
        }
    }
}

static uint32_t tick_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// The UI thread body. Runs on the pthread created in lvgl_glue_start. Owns the
// ENTIRE display + LVGL bring-up (panel, lv_init, buffers, touch) plus the app
// shell and render loop. board_display_init() allocates the 32KB internal RGB
// bounce DMA buffer here — it's the SECOND internal-DRAM allocation gate (the
// first is the pthread stack), so its pre-alloc heap is logged inside
// board_display_init. Keeping it all on one thread also keeps LVGL access
// sequential (LV_OS_NONE, no locking).
static void* ui_thread_main(void* arg) {
    (void)arg;

    // Panel first (RGB init + bounce buffers). Thread-agnostic hardware setup.
    s_panel = board_display_init();

    // vsync semaphore (drives the presenter). Register before any flush can run.
    // on_vsync is not IRAM-safe here (the bounce ISR runs with cache on —
    // LCD_RGB_ISR_IRAM_SAFE is off), so no IRAM_ATTR and no logging inside it.
    s_vsync_sem = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t lcd_cbs = {.on_vsync = flush_on_vsync,
                                                   .on_frame_buf_complete = on_frame_buf_complete};
    esp_lcd_rgb_panel_register_event_callbacks(s_panel, &lcd_cbs, NULL);

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_xml_init();

    // Full-frame PSRAM shadow. 64-byte aligned for PSRAM cache-line coherency
    // when esp_lcd reads it during the blit. Zeroed so no garbage can reach the
    // FB before the first full-screen render overwrites it.
    s_shadow = heap_caps_aligned_alloc(64, SHADOW_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_shadow) {
        ESP_LOGE(TAG, "FATAL: no PSRAM shadow %uB (largest=%u)", (unsigned)SHADOW_BYTES,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        abort();
    }
    memset(s_shadow, 0, SHADOW_BYTES);

    // Two-hop blit staging band (INTERNAL DRAM). Allocated HERE — after the boot
    // heap gates (48KB UI stack, 32KB bounce) have already passed — so it cannot
    // push them over. Non-fatal: on failure present_blit falls back to the direct
    // PSRAM->PSRAM blit (slower, but correct).
    s_band = heap_caps_aligned_alloc(16, UI_BAND_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_band) {
        ESP_LOGW(TAG, "no internal for %uB band; direct blit (largest=%u)", (unsigned)UI_BAND_BYTES,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    lv_display_t* disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_display_set_buffers(disp, s_draw_buf1, NULL, UI_DRAW_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    // Touch indev registration must run on the UI thread after lv_init. A
    // failed probe is non-fatal (no indev, display-only); report it to the app
    // layer here, which runs before s_ui_build() below, so app_boot_ui() can
    // enqueue the user-visible warning before it drains the warning queue.
    app_boot_set_touch_available(touch_input_init());

    // Presenter task — the only writer of the panel FB.
    if (xTaskCreate(present_task, "present", PRESENT_STACK_BYTES, NULL, PRESENT_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "FATAL: no presenter task");
        abort();
    }

    s_ui_build();
    ota_health_confirm();
    ESP_LOGI(TAG, "ui: render loop");
    int64_t underrun_log_us = esp_timer_get_time();
    int32_t underrun_prev_drift = 0;
    while (true) {
        // Scan-out underrun tripwire: drift = glitched frames since boot (see
        // the comment above flush_on_vsync). Checked every 10s, logged ONLY
        // when the window saw new underruns, so a healthy panel stays silent
        // and a contention burst is attributable to whatever the log shows in
        // the same window.
        int64_t now_us = esp_timer_get_time();
        if (now_us - underrun_log_us >= 10 * 1000 * 1000) {
            underrun_log_us = now_us;
            uint32_t v = s_vsync_n;
            uint32_t f = s_frame_complete_n;
            // The wrap callback leads the vsync ISR within a frame, so drift can
            // transiently read -1; signed math keeps that from exploding.
            int32_t drift = (int32_t)(v - f);
            if (drift - underrun_prev_drift > 0) {
                ESP_LOGW(TAG, "[scanout] vsync=%lu frame_complete=%lu underruns=%ld (%+ld/10s)",
                         (unsigned long)v, (unsigned long)f, (long)drift,
                         (long)(drift - underrun_prev_drift));
            }
            underrun_prev_drift = drift;
        }
        // Cycle-time tripwire: a long lv_timer_handler starves the polled touch
        // indev — taps during the window are silently dropped. Pairs with the
        // flush_cb slow-refresh log to split "render slow" from "handler slow".
        int64_t t0 = esp_timer_get_time();
        uint32_t delay = lv_timer_handler();
        int64_t handler_ms = (esp_timer_get_time() - t0) / 1000;
        if (handler_ms >= CYCLE_LOG_MS) {
            ESP_LOGW(TAG, "slow ui cycle: lv_timer_handler %ldms", (long)handler_ms);
        }
        if (s_ui_tick) {
            s_ui_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(delay < 5 ? 5 : delay > 50 ? 50 : delay));
    }
    return NULL;
}

void lvgl_glue_start(void (*ui_build)(void), void (*ui_tick)(void)) {
    s_ui_build = ui_build;
    s_ui_tick = ui_tick;

    // Create the UI pthread as the FIRST sizeable heap allocation of the boot —
    // before the thread body's board_display_init/lv_init and before the net
    // task app_main spawns next. esp_pthread_set_cfg applies to the next
    // pthread_create only. Detached: runs the render loop for the process
    // lifetime, never joined.
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = UI_THREAD_STACK_BYTES;
    cfg.prio = 5;
    cfg.thread_name = "ui";
    esp_err_t cfg_err = esp_pthread_set_cfg(&cfg);
    if (cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pthread_set_cfg failed: %s", esp_err_to_name(cfg_err));
    }

    // Allocation gate #1 (one-shot, every boot): the 48KB UI stack must fit in
    // `largest`. Below ~48KB, pthread_create fails with ENOMEM (errno 12). The
    // matching gate #2 (RGB bounce DMA) logs inside board_display_init.
    ESP_LOGI(TAG, "heap before pthread: free=%u largest=%u need=%u",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), UI_THREAD_STACK_BYTES);

    pthread_t ui_thread;
    int rc = pthread_create(&ui_thread, NULL, ui_thread_main, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ui pthread_create failed: %d — no UI", rc);
        return;
    }
    pthread_detach(ui_thread);
}
