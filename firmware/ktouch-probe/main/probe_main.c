// SPDX-License-Identifier: GPL-3.0-or-later
//
// BTT K-Touch hardware verification probe.
//
// Verifies the pin map borrowed from bigtreetech/PandaTouch_PlatformIO against
// real K-Touch hardware (see docs/devel/printer-research/BTT_K_TOUCH_HARDWARE.md):
//   1. RGB panel bring-up -> draws color bars + white border (visual pass/fail)
//   2. Full I2C bus scan  -> finds GT911 and any battery gauge / other devices
//   3. GT911 product-ID read + touch coordinate logging over UART
//
// Intentionally NO LVGL: raw esp_lcd + raw I2C so a failure points at exactly
// one layer. Throwaway-quality by design, committed for reproducibility.

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char* TAG = "ktouch_probe";

// ---- Pin map under test (PandaTouch_PlatformIO pt_board.h) ----
#define LCD_PCLK_PIN 5
#define LCD_DE_PIN 38
#define LCD_RESET_PIN 46
#define LCD_BL_PIN 21
// data_gpio_nums order per esp_lcd HAL: B3-B7, G2-G7, R3-R7
static const int LCD_DATA_PINS[16] = {17, 18, 48, 47, 39,     // B3-B7
                                      11, 12, 13, 14, 15, 16, // G2-G7
                                      6,  7,  8,  9,  10};    // R3-R7

#define LCD_H_RES 800
#define LCD_V_RES 480
#define LCD_PCLK_HZ 14800000
#define LCD_HSYNC_PW 4
#define LCD_HSYNC_BP 16
#define LCD_HSYNC_FP 16
#define LCD_VSYNC_PW 4
#define LCD_VSYNC_BP 32
#define LCD_VSYNC_FP 32

#define I2C_SCL_PIN 1
#define I2C_SDA_PIN 2
#define GT911_IRQ_PIN 40
#define GT911_RST_PIN 41
#define GT911_ADDR 0x5D // selected by holding IRQ low through reset

// GT911 registers (16-bit big-endian addressing)
#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINT0 0x814F

static esp_err_t gt911_read(uint16_t reg, uint8_t* buf, size_t len) {
    uint8_t addr[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_write_read_device(I2C_NUM_0, GT911_ADDR, addr, 2, buf, len,
                                        pdMS_TO_TICKS(100));
}

static esp_err_t gt911_write_u8(uint16_t reg, uint8_t val) {
    uint8_t frame[3] = {reg >> 8, reg & 0xFF, val};
    return i2c_master_write_to_device(I2C_NUM_0, GT911_ADDR, frame, 3, pdMS_TO_TICKS(100));
}

static void backlight_on(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_11_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 30000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {
        .gpio_num = LCD_BL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
        .duty = (1 << 11) - 1, // 100%
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    ESP_LOGI(TAG, "backlight: LEDC 30kHz 11-bit on GPIO%d -> 100%%", LCD_BL_PIN);
}

static esp_lcd_panel_handle_t panel_init(void) {
    // Reset dance from PandaTouch reference: low 100ms, high 10ms
    gpio_config_t rst = {.pin_bit_mask = 1ULL << LCD_RESET_PIN, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(LCD_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LCD_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings =
            {
                .pclk_hz = LCD_PCLK_HZ,
                .h_res = LCD_H_RES,
                .v_res = LCD_V_RES,
                .hsync_pulse_width = LCD_HSYNC_PW,
                .hsync_back_porch = LCD_HSYNC_BP,
                .hsync_front_porch = LCD_HSYNC_FP,
                .vsync_pulse_width = LCD_VSYNC_PW,
                .vsync_back_porch = LCD_VSYNC_BP,
                .vsync_front_porch = LCD_VSYNC_FP,
                .flags = {.pclk_active_neg = 1},
            },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = 10 * LCD_H_RES, // matches stock; eases PSRAM/WiFi contention
        .hsync_gpio_num = -1,                    // DE-only mode, HSYNC/VSYNC not wired
        .vsync_gpio_num = -1,
        .de_gpio_num = LCD_DE_PIN,
        .pclk_gpio_num = LCD_PCLK_PIN,
        .disp_gpio_num = -1,
        .flags = {.fb_in_psram = 1},
    };
    memcpy(cfg.data_gpio_nums, LCD_DATA_PINS, sizeof(LCD_DATA_PINS));

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_LOGI(TAG, "RGB panel up: %dx%d @ %.1fMHz PCLK, DE-only", LCD_H_RES, LCD_V_RES,
             LCD_PCLK_HZ / 1e6);
    return panel;
}

static void draw_test_pattern(esp_lcd_panel_handle_t panel) {
    void* fb = NULL;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb));
    uint16_t* px = fb;

    // 5 vertical bars: red, green, blue, white, black (RGB565)
    const uint16_t bars[5] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            px[y * LCD_H_RES + x] = bars[x * 5 / LCD_H_RES];
        }
    }
    // 4px white border to expose porch/timing misalignment at the edges
    for (int x = 0; x < LCD_H_RES; x++) {
        for (int y = 0; y < 4; y++) {
            px[y * LCD_H_RES + x] = 0xFFFF;
            px[(LCD_V_RES - 1 - y) * LCD_H_RES + x] = 0xFFFF;
        }
    }
    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < 4; x++) {
            px[y * LCD_H_RES + x] = 0xFFFF;
            px[y * LCD_H_RES + (LCD_H_RES - 1 - x)] = 0xFFFF;
        }
    }
    ESP_LOGI(TAG, "test pattern drawn: R/G/B/W/K bars + white border");
}

static void i2c_bus_scan(void) {
    ESP_LOGI(TAG, "I2C scan on SCL=%d SDA=%d @400kHz:", I2C_SCL_PIN, I2C_SDA_PIN);
    int found = 0;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  device ACK at 0x%02X%s", a, a == 0x5D || a == 0x14 ? " (GT911)" : "");
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan done: %d device(s)", found);
}

static bool gt911_init(void) {
    // Address-select reset: hold IRQ low through RST rising edge -> 0x5D
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << GT911_IRQ_PIN) | (1ULL << GT911_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(GT911_IRQ_PIN, 0);
    gpio_set_level(GT911_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GT911_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_config_t in = {.pin_bit_mask = 1ULL << GT911_IRQ_PIN, .mode = GPIO_MODE_INPUT};
    ESP_ERROR_CHECK(gpio_config(&in));
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_config_t i2c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));

    i2c_bus_scan();

    uint8_t id[4] = {0};
    esp_err_t err = gt911_read(GT911_REG_PRODUCT_ID, id, 4);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 product-ID read FAILED at 0x%02X: %s", GT911_ADDR,
                 esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "GT911 product ID: '%c%c%c' (raw %02x %02x %02x %02x)", id[0], id[1], id[2],
             id[0], id[1], id[2], id[3]);
    return true;
}

static void touch_poll_task(void* arg) {
    (void)arg;
    while (true) {
        uint8_t status = 0;
        if (gt911_read(GT911_REG_STATUS, &status, 1) == ESP_OK && (status & 0x80)) {
            int n = status & 0x0F;
            for (int i = 0; i < n && i < 5; i++) {
                uint8_t p[8];
                if (gt911_read(GT911_REG_POINT0 + i * 8, p, 8) == ESP_OK) {
                    int x = p[1] | (p[2] << 8);
                    int y = p[3] | (p[4] << 8);
                    ESP_LOGI(TAG, "TOUCH[%d] id=%d x=%d y=%d", i, p[0], x, y);
                }
            }
            gt911_write_u8(GT911_REG_STATUS, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== K-Touch hardware probe ===");
    ESP_LOGI(TAG, "internal free: %u, PSRAM free: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_lcd_panel_handle_t panel = panel_init();
    draw_test_pattern(panel);
    backlight_on();

    bool touch_ok = gt911_init();
    ESP_LOGI(TAG, "=== probe summary: panel=UP touch=%s ===", touch_ok ? "OK" : "FAILED");

    if (touch_ok) {
        xTaskCreate(touch_poll_task, "touch_poll", 4096, NULL, 5, NULL);
    }
}
