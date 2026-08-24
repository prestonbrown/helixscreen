// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 0 audit, Task 1: lib/helix-xml + repo LVGL 9.5 on ESP32-S3 (K-Touch).
//
// Measures the near-parity thesis's foundation: the unmodified XML engine
// parsing a component with design-token-style consts and a reactive subject
// binding, rendering on the real 800x480 panel. Logs heap watermarks at each
// stage for the audit report.
//
// Board bring-up (pins/timings) verified by firmware/ktouch-probe.

#include <pthread.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "lvgl_private.h"  // audit instrumentation: inspect lv_obj_t style state
#include "src/xml/lv_xml.h"
#include "src/xml/lv_xml_component.h"

#include "audit_app.h"  // Task 3 slice driver (helixapp component)
#include "esp_littlefs.h"

static const char *TAG = "native_audit";

static void *app_phase(void *arg);

#define LCD_H_RES 800
#define LCD_V_RES 480

static void check_heap(const char *stage) {
    bool ok = heap_caps_check_integrity_all(true);
    ESP_LOGI(TAG, "[integrity:%s] %s", stage, ok ? "OK" : "CORRUPT");
    if (!ok) abort();
}

// Full watermark including largest free block. ONE-SHOT stages only:
// heap_caps_get_largest_free_block() walks every heap block inside an
// interrupt-disabling critical section. With the full shell at
// ALWAYSINTERNAL=0 the PSRAM heap holds thousands of small widget blocks,
// so the walk keeps interrupts off long enough for the RGB bounce-buffer
// refill ISR to miss its deadline — one visibly corrupted frame per call.
// Never call this from the steady-state render loop; use log_heap_fast().
static void log_heap(const char *stage) {
    ESP_LOGI(TAG, "[heap:%s] internal free=%u largest=%u | psram free=%u largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

// Steady-state-safe watermark: heap_caps_get_free_size() reads a tracked
// counter (O(1), no block walk) so it cannot stall the scanout ISRs.
static void log_heap_fast(const char *stage) {
    ESP_LOGI(TAG, "[heap:%s] internal free=%u | psram free=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// ---- Panel bring-up (verified pin map, see ktouch-probe) ----

static esp_lcd_panel_handle_t s_panel;

static esp_lcd_panel_handle_t panel_init(void) {
    gpio_config_t rst = {.pin_bit_mask = 1ULL << 46, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(46, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(46, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 14800000,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 16,
            .hsync_front_porch = 16,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 32,
            .vsync_front_porch = 32,
            .flags = {.pclk_active_neg = 1},
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        // 10-line bounce buffers (matches stock/PandaTouch). Direct PSRAM scanout
        // visibly desyncs: the image jumps/wraps whenever redraw traffic competes
        // for PSRAM bandwidth. Bounce buffers were ruled out as the corruption
        // source (that was the missing lv_xml_init).
        .bounce_buffer_size_px = 10 * LCD_H_RES,
        .hsync_gpio_num = -1,
        .vsync_gpio_num = -1,
        .de_gpio_num = 38,
        .pclk_gpio_num = 5,
        .disp_gpio_num = -1,
        .data_gpio_nums = {17, 18, 48, 47, 39, 11, 12, 13, 14, 15, 16, 6, 7, 8, 9, 10},
        .flags = {.fb_in_psram = 1},
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 30000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {
        .gpio_num = 21,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
        .duty = (1 << 11) - 1,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    return panel;
}

// ---- LVGL glue ----

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                              px_map);
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- The audit component: consts, styles, nested widgets, a subject binding ----

static const char *AUDIT_XML =
    "<component>"
    "  <consts>"
    "    <color name=\"card_bg\" value=\"0x1a2332\"/>"
    "    <color name=\"accent\" value=\"0x4fc3f7\"/>"
    "    <int name=\"space_md\" value=\"12\"/>"
    "  </consts>"
    "  <view extends=\"lv_obj\" width=\"400\" height=\"200\" style_bg_color=\"#card_bg\""
    "        style_radius=\"12\" style_pad_all=\"#space_md\" style_border_width=\"0\">"
    "    <lv_label text=\"helix-xml on ESP32-S3\" style_text_color=\"#accent\"/>"
    "    <lv_label name=\"counter\" text=\"waiting...\" y=\"40\" style_text_color=\"0xffffff\""
    "              bind_text=\"audit_counter\"/>"
    "    <lv_bar name=\"bar\" y=\"90\" width=\"370\" height=\"16\" bind_value=\"audit_progress\"/>"
    "  </view>"
    "</component>";

static lv_subject_t s_counter_subject;
static lv_subject_t s_progress_subject;
static char s_counter_buf[64];
static char s_counter_prev[64];

void app_main(void) {
    ESP_LOGI(TAG, "=== Phase 0 audit Task 1: helix-xml on K-Touch ===");
    log_heap("boot");

    s_panel = panel_init();
    log_heap("panel");
    check_heap("panel");

    lv_init();
    lv_tick_set_cb(tick_cb);
    // lv_init() does NOT call lv_xml_init() — helix-xml is external to LVGL and
    // the app calls this itself at startup. Skipping it leaves component_scope_ll
    // uninitialized -> heap-corruption-shaped crashes on first registration.
    lv_xml_init();

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    // LV_DRAW_BUF_ALIGN is 16 in the repo config; plain heap_caps_malloc is 8-aligned.
    // Draw buffers stay in PSRAM: compositing bandwidth is not a problem — an
    // internal-buffer experiment (2x32-line, MALLOC_CAP_INTERNAL) rendered
    // identically, so there's no reason to spend 100KB of internal RAM here.
    // (2x64KB internal is not even allocatable: largest internal block at this
    // stage is ~118KB.) The periodic frame corruption once blamed on bandwidth
    // was the steady-state heap walk; see log_heap() vs log_heap_fast().
    size_t buf_px = LCD_H_RES * 80;
    lv_color_t *buf1 = heap_caps_aligned_alloc(16, buf_px * sizeof(lv_color16_t),
                                               MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_aligned_alloc(16, buf_px * sizeof(lv_color16_t),
                                               MALLOC_CAP_SPIRAM);
    assert(buf1 && buf2);
    lv_display_set_buffers(disp, buf1, buf2, buf_px * sizeof(lv_color16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    log_heap("lvgl");
    check_heap("lvgl");

    // Differential test: exercise plain LVGL styles BEFORE any helix-xml call.
    lv_obj_t *scr0 = lv_screen_active();
    ESP_LOGI(TAG, "pre-xml: scr=%p style_cnt=%u styles=%p", (void *)scr0,
             scr0 ? (unsigned)scr0->style_cnt : 0, scr0 ? (void *)scr0->styles : NULL);
    lv_obj_set_style_bg_color(scr0, lv_color_hex(0x101010), 0);
    ESP_LOGI(TAG, "pre-xml: plain LVGL style write OK");
    check_heap("style-write");

    // Subjects first, then component registration, then create — the app's
    // init order (CLAUDE.md "Subject init order").
    lv_subject_init_string(&s_counter_subject, s_counter_buf, s_counter_prev,
                           sizeof(s_counter_buf), "init");
    lv_subject_init_int(&s_progress_subject, 0);
    lv_xml_register_subject(NULL, "audit_counter", &s_counter_subject);
    lv_xml_register_subject(NULL, "audit_progress", &s_progress_subject);
    check_heap("subjects");

    int64_t t0 = esp_timer_get_time();
    lv_result_t reg = lv_xml_register_component_from_data("audit_card", AUDIT_XML);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "lv_xml_register_component_from_data: %s in %lldus",
             reg == LV_RESULT_OK ? "OK" : "FAILED", t1 - t0);
    check_heap("post-register");
    if (reg != LV_RESULT_OK) {
        ESP_LOGE(TAG, "=== AUDIT FAILED at component registration ===");
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        ESP_LOGE(TAG, "lv_screen_active() is NULL (default display=%p)",
                 (void *)lv_display_get_default());
        ESP_LOGE(TAG, "=== AUDIT FAILED: no active screen ===");
        return;
    }
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), 0);
    t0 = esp_timer_get_time();
    lv_obj_t *card = lv_xml_create(scr, "audit_card", NULL);
    t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "lv_xml_create: %s in %lldus", card ? "OK" : "FAILED", t1 - t0);
    if (!card) {
        ESP_LOGE(TAG, "=== AUDIT FAILED at component instantiation ===");
        return;
    }
    lv_obj_center(card);
    log_heap("xml-created");

    ESP_LOGI(TAG, "=== AUDIT TASK 1 PASS: XML component live, entering render loop ===");

    // Task 3: mount the ui_xml tree, then bring up the real app-core slice
    // (PrinterState subject pipeline + home panel from its real XML).
    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    esp_err_t fs_err = esp_vfs_littlefs_register(&fs_conf);
    ESP_LOGI(TAG, "littlefs mount: %s", esp_err_to_name(fs_err));
    if (fs_err == ESP_OK) {
        size_t fs_total = 0, fs_used = 0;
        esp_littlefs_info("storage", &fs_total, &fs_used);
        ESP_LOGI(TAG, "littlefs: %u/%u KB used", (unsigned)(fs_used / 1024),
                 (unsigned)(fs_total / 1024));
    }
    // The app phase must run on a pthread-created task: ESP-IDF's pthread_self()
    // asserts from raw FreeRTOS tasks, and the app core calls
    // std::this_thread::get_id() (main-thread detectors, spdlog). The main task
    // just joins; all LVGL access stays sequential (Task 1 code above finished
    // before this thread starts, and only app_phase touches LVGL afterwards).
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32768);
    pthread_t app_thread;
    int perr = pthread_create(&app_thread, &attr, app_phase, NULL);
    if (perr != 0) {
        ESP_LOGE(TAG, "app_phase pthread_create failed: %d", perr);
        return;
    }
    pthread_join(app_thread, NULL);
}

// ---- Task 4: CJK font viability ----
//
// Test A: the desktop CjkFontManager path — lv_binfont_create() of a runtime
// .bin subset (1203 codepoints from the zh+ja translations, 16px/4bpp, 123KB
// on LittleFS). Measures actual heap cost and LittleFS load time.
// Test B: the same subset compiled in as a C array — lives in .rodata, XIP'd
// from flash, zero heap by construction. Both render side by side; the label
// text is taken from translations/{zh,ja}.yml so every glyph is in-subset.
extern const lv_font_t noto_sans_cjk_16_compiled;

static void cjk_experiment(void) {
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int64_t t0 = esp_timer_get_time();
    lv_font_t *binfont = lv_binfont_create("A:assets/fonts/cjk/noto_sans_cjk_16.bin");
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG,
             "[cjk:binfont] %s in %lldms | psram cost=%d internal cost=%d",
             binfont ? "OK" : "FAILED", (t1 - t0) / 1000,
             (int)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             (int)(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

    lv_obj_t *card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(card, 520, 110);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2332), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);

    // White = Test A (runtime binfont). If font load failed the label falls
    // back to the default Latin font and shows missing-glyph boxes.
    lv_obj_t *l1 = lv_label_create(card);
    if (binfont) lv_obj_set_style_text_font(l1, binfont, 0);
    lv_label_set_text(l1, "打印机归位并探测热床、印刷完了後に保存してください");
    lv_obj_set_style_text_color(l1, lv_color_hex(0xffffff), 0);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 0);

    // Blue = Test B (compiled-in subset, XIP from flash).
    lv_obj_t *l2 = lv_label_create(card);
    lv_obj_set_style_text_font(l2, &noto_sans_cjk_16_compiled, 0);
    lv_label_set_text(l2, "打印机归位并探测热床、印刷完了後に保存してください");
    lv_obj_set_style_text_color(l2, lv_color_hex(0x4fc3f7), 0);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, 36);
}

// Task 5 FPS gate: sustained full-screen redraw rate through the real
// pipeline (full shell composited via 80-line PSRAM draw buffers +
// esp_lcd draw_bitmap into the PSRAM framebuffer). lv_refr_now() renders
// synchronously, so timing N forced full refreshes measures pure render
// throughput with no task-loop sleep in the number. The LV_USE_PERF_MONITOR
// overlay (bottom right) additionally shows live FPS/CPU during normal use.
static void fps_benchmark(void) {
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *scr = lv_screen_active();
    const int N = 100;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; i++) {
        lv_obj_invalidate(scr);
        lv_refr_now(disp);
    }
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "[fps] %d full-screen refreshes in %lldms = %.1f fps sustained",
             N, (t1 - t0) / 1000, (double)N * 1000000.0 / (double)(t1 - t0));
}

static void *app_phase(void *arg) {
    (void)arg;
    audit_app_run();
    log_heap("app-slice-up");
    cjk_experiment();
    log_heap("post-cjk");
    fps_benchmark();

    uint32_t n = 0;
    int64_t last_update = 0;
    while (true) {
        uint32_t delay = lv_timer_handler();
        int64_t now = esp_timer_get_time();
        if (now - last_update > 1000000) {  // 1Hz subject updates through the binding
            last_update = now;
            n++;
            char tmp[64];  // must not write subject's own buffer: change detection compares against it
            lv_snprintf(tmp, sizeof(tmp), "subject update #%u", (unsigned)n);
            lv_subject_copy_string(&s_counter_subject, tmp);
            lv_subject_set_int(&s_progress_subject, (int)(n * 10 % 110));
            if (n % 10 == 0) log_heap_fast("steady");
        }
        vTaskDelay(pdMS_TO_TICKS(delay < 5 ? 5 : delay > 50 ? 50 : delay));
    }
    return NULL;
}
