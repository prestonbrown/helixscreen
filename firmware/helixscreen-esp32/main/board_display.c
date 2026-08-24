// SPDX-License-Identifier: GPL-3.0-or-later
#include "board_display.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ktouch.h"

static const char* TAG = "board_display";
static bool s_backlight_ok;

esp_lcd_panel_handle_t board_display_init(void) {
    gpio_config_t rst = {.pin_bit_mask = 1ULL << BOARD_LCD_PIN_RESET, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(BOARD_LCD_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_LCD_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings =
            {
                .pclk_hz = BOARD_LCD_PCLK_HZ,
                .h_res = BOARD_LCD_H_RES,
                .v_res = BOARD_LCD_V_RES,
                .hsync_pulse_width = BOARD_LCD_HSYNC_PW,
                .hsync_back_porch = BOARD_LCD_HSYNC_BP,
                .hsync_front_porch = BOARD_LCD_HSYNC_FP,
                .vsync_pulse_width = BOARD_LCD_VSYNC_PW,
                .vsync_back_porch = BOARD_LCD_VSYNC_BP,
                .vsync_front_porch = BOARD_LCD_VSYNC_FP,
                .flags = {.pclk_active_neg = 1},
            },
        .data_width = 16,
        .bits_per_pixel = 16,
        // DO NOT set num_fbs > 1 with this bounce-buffer config. Tried it (LVGL
        // double-FB DIRECT mode, D2 tear fix attempt): num_fbs=2 + bounce_buffer
        // + RESTART_IN_VSYNC = SCAN-OUT DEATH — the panel drops into its BIST
        // color-cycle test pattern (white/red/green/blue/blank) while the app
        // keeps running. The bounce ISR streams FB->bounce->panel continuously; a
        // second FB plus a draw_bitmap "flip" gives the driver two conflicting
        // ideas of which FB feeds the bounce, and the vsync DMA restart desyncs
        // permanently. The esp_lcd/esp_lvgl_port double-FB examples all use DIRECT
        // PSRAM scan-out with NO bounce — but this panel REQUIRES bounce (direct
        // scan-out desyncs under redraw PSRAM-bandwidth contention, the Task-1
        // trap below). So double-FB is closed on this hardware; tearing is handled
        // in software instead (per-refresh-cycle vsync-gated flush, lvgl_glue.c).
        .num_fbs = 1,
        // 20-line bounce buffers (two 32KB internal buffers). Sizing is
        // measured, not guessed — A/B'd on-device with the [scanout] underrun
        // tripwire (lvgl_glue.c) at verified 80MHz octal PSRAM:
        //   10 lines: PSRAM-heavy bursts (deferred panel builds, WiFi bring-up)
        //     starve the refill ISR's 565us deadline -> 741 late refills + 34
        //     coalesced EOFs per boot = the one-frame downward-ghost glitches
        //     RESTART_IN_VSYNC then recovers.
        //   20 lines: same bursts -> 2 late + 8 hard (those 8 are the flash-
        //     cache-off NVS windows no bounce size can absorb). Idle is clean
        //     under both.
        // The doubled per-refill window (1130us vs a ~600us copy) is what buys
        // the headroom. CAUTION: only valid at PSRAM 80MHz — at 40MHz the copy
        // itself exceeds any window and bigger buffers make it WORSE (measured:
        // continuous idle underruns). Verify "esp_psram: Speed: 80MHz" in the
        // boot log before trusting any scan-out measurement.
        .bounce_buffer_size_px = 20 * BOARD_LCD_H_RES,
        .hsync_gpio_num = -1,
        .vsync_gpio_num = -1,
        .de_gpio_num = BOARD_LCD_PIN_DE,
        .pclk_gpio_num = BOARD_LCD_PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = BOARD_LCD_DATA_PINS,
        .flags = {.fb_in_psram = 1},
    };
    esp_lcd_panel_handle_t panel = NULL;
    // Allocation gate #2 (one-shot, every boot): esp_lcd_new_rgb_panel allocates
    // the RGB bounce buffer in INTERNAL DMA-capable SRAM. If internal DRAM is
    // exhausted this aborts ("no mem for bounce buffer") into a reset loop, so
    // the deciding heap number is logged here — the counterpart to gate #1 (the
    // UI pthread stack in lvgl_glue.c).
    ESP_LOGI(TAG, "internal heap before rgb panel: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
    // Backlight bring-up is non-fatal, unlike the panel init above: a board
    // whose LEDC setup fails still has a working panel, and a backlight stuck
    // at its power-on level is a far better outcome than refusing to boot.
    // board_display_backlight() checks s_backlight_ok so it does not spam
    // errors against an unconfigured channel.
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight timer config failed: %s - brightness control disabled",
                 esp_err_to_name(err));
        return panel;
    }
    ledc_channel_config_t channel = {
        .gpio_num = BOARD_BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
        .duty = (1 << 11) - 1,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight channel config failed: %s - brightness control disabled",
                 esp_err_to_name(err));
        return panel;
    }
    s_backlight_ok = true;
    return panel;
}

void board_display_backlight(uint8_t percent) {
    if (!s_backlight_ok)
        return;
    if (percent > 100)
        percent = 100;
    uint32_t duty = ((uint32_t)percent * ((1 << 11) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
