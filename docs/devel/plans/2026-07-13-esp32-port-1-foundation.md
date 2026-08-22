# ESP32 Port — Plan 1: Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Product-quality ESP-IDF project at `firmware/helixscreen-esp32/` that boots the K-Touch to a touch-responsive LVGL screen from an OTA A/B partition layout, with CI cross-build and an enforced image-size budget.

**Architecture:** Seams-first hybrid (approved spec `docs/devel/plans/2026-07-13-esp32-native-port-design.md`). This plan is firmware-tree only — no main-tree changes. The audit tree `firmware/native-audit/` is a frozen reference; specific proven files are copied from it deliberately, never symlinked.

**Tech Stack:** ESP-IDF v5.5 (`~/Code/esp-idf`, `source export.sh`), LVGL 9.5 (repo submodule + patches, unmodified), `lib/helix-xml` (unmodified), `esp_lcd` RGB panel, `esp_lcd_touch_gt911` managed component, GitHub Actions + `espressif/idf:release-v5.5` docker.

## Program sequence (this is Plan 1 of 5)

| Plan | Deliverable | Status |
|---|---|---|
| **1. Foundation** (this plan) | Touch-responsive LVGL on K-Touch, OTA A/B partitions, CI + size gate | — |
| 2. Main-tree seams | Asset root, build-time token table, settings storage backend, WiFi backend seam | not written |
| 3. Network | `helixnet`: esp_websocket/http impls of `IMoonrakerClient`/`IMoonrakerAPI`, live Moonraker on-device | not written |
| 4. App integration | Full shell + Core+AMS panels + provisioning + NVS settings + mock mode on device | not written |
| 5. Product hardening | OTA end-to-end, crash reporting, CJK fonts, image diet to budget, HIL suite | not written |

Later plans are written as their predecessors land. Do not start Plan N+1 work inside Plan N.

## Global Constraints

- Target: ESP32-S3R8, 8MB octal PSRAM, 16MB flash, 800×480 RGB, GT911 touch (BTT K-Touch only).
- Image budget: **≤5.8MB** (6.0MB OTA slot − margin). CI-enforced from this plan onward.
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`; DMA/WiFi-critical allocs use explicit `MALLOC_CAP_INTERNAL`.
- Every task that can touch app code is pthread-created (audit rule; enforced from Task 3 on).
- No periodic `heap_caps_get_largest_free_block()`/`heap_caps_get_info()`/integrity checks while the display is live — steady-state telemetry uses `heap_caps_get_free_size()` only.
- All builds/flashes run in background (`run_in_background`); serial console 115200 via `firmware/native-audit/capture_serial.py` (`--baud 115200`, logs contain NULs → always `grep -a`); flash at 460800 via `sg dialout -c`.
- HIL steps require the physical K-Touch and (where marked) user visual confirmation — one HIL cycle at a time.
- Commit after every task; `audit(esp32)` prefix is retired — use `feat(esp32-fw)`.

## File Structure (end state of this plan)

```
firmware/helixscreen-esp32/
├── CMakeLists.txt                   # project(helixscreen_esp32)
├── partitions.csv                   # OTA A/B layout (Task 1)
├── sdkconfig.defaults               # Task 1
├── size_budget.json                 # Task 6
├── .gitignore                       # build/, sdkconfig, managed_components/, dependencies.lock
├── boards/
│   └── ktouch.h                     # pin/timing board table (Task 2)
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml            # esp_lcd_touch_gt911 dependency (Task 4)
│   ├── app_main.c                   # entry: board → lvgl → touch → ota-health → ui task
│   ├── board_display.c/.h           # panel + backlight bring-up (Task 2)
│   ├── lvgl_glue.c/.h               # lv init, buffers, flush, tick, ui pthread (Task 3)
│   ├── touch_input.c/.h             # GT911 → lv_indev (Task 4)
│   └── ota_health.c/.h              # rollback confirm + partition logging (Task 5)
└── components/
    └── helixcore/                   # copied from audit (Task 3): lv_conf.h + LVGL + helix-xml
.github/workflows/esp32-build.yml    # Task 6
scripts/check_esp32_size.py          # Task 6
```

---

### Task 1: Project skeleton, partitions, sdkconfig

**Files:**
- Create: `firmware/helixscreen-esp32/CMakeLists.txt`
- Create: `firmware/helixscreen-esp32/partitions.csv`
- Create: `firmware/helixscreen-esp32/sdkconfig.defaults`
- Create: `firmware/helixscreen-esp32/.gitignore`
- Create: `firmware/helixscreen-esp32/main/CMakeLists.txt`
- Create: `firmware/helixscreen-esp32/main/app_main.c`

**Interfaces:**
- Produces: bootable minimal app; partition layout every later task flashes into; `PROJECT_NAME helixscreen_esp32` → `build/helixscreen_esp32.bin`.

- [ ] **Step 1: Create the project files**

`firmware/helixscreen-esp32/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(helixscreen_esp32)
```

`firmware/helixscreen-esp32/partitions.csv` — OTA A/B, no factory slot (approved spec; sizing note in spec):
```csv
# 16MB flash. OTA A/B from day one: 2x 6.0MB app slots + 3.75MB LittleFS
# storage (minified ui_xml + runtime data) + NVS. 128KB spare at end.
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xf000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
ota_0,    app,  ota_0,   0x20000,  0x600000,
ota_1,    app,  ota_1,   0x620000, 0x600000,
storage,  data, spiffs,  0xc20000, 0x3c0000,
```

`firmware/helixscreen-esp32/sdkconfig.defaults` (audit-proven values; poisoning OFF — that was corruption-hunt instrumentation; rollback ON for Task 5):
```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536

CONFIG_COMPILER_OPTIMIZATION_SIZE=y
CONFIG_COMPILER_CXX_EXCEPTIONS=y
CONFIG_COMPILER_CXX_RTTI=y

CONFIG_FREERTOS_HZ=1000
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=8192

# XML parse (expat) recursion needs a real stack (audit trap #4).
CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y

# OTA A/B with rollback: new image must confirm healthy boot or the
# bootloader reverts to the previous slot.
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

# TLS to Moonraker is optional/LAN; the default cert bundle costs ~636KB.
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n
```

`firmware/helixscreen-esp32/.gitignore`:
```
build/
sdkconfig
sdkconfig.old
managed_components/
dependencies.lock
```

`firmware/helixscreen-esp32/main/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "app_main.c"
                       PRIV_REQUIRES esp_timer app_update
                       INCLUDE_DIRS "." "../boards")
```

`firmware/helixscreen-esp32/main/app_main.c`:
```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HelixScreen ESP32 target — entry point.
// Phase order: board display → LVGL → touch → OTA health → UI task.
// Plan 1 fills these in task by task; this skeleton proves the partition
// layout and toolchain.

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "helixscreen";

void app_main(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "helixscreen-esp32 booting from partition '%s' @ 0x%08" PRIx32,
             running->label, running->address);
}
```

- [ ] **Step 2: Build**

```bash
bash -c 'source ~/Code/esp-idf/export.sh && cd firmware/helixscreen-esp32 && idf.py build'
```
(run_in_background; verify via the output file, not exit code of a piped tail — check for `Generated .../helixscreen_esp32.bin`.)
Expected: build succeeds; `check_sizes.py` reports the app fits `ota_0` (0x600000).

- [ ] **Step 3: Flash + serial-verify boot partition marker (HIL)**

```bash
bash -c 'source ~/Code/esp-idf/export.sh && cd firmware/helixscreen-esp32 && sg dialout -c "idf.py -p /dev/ttyUSB0 -b 460800 flash"'
python firmware/native-audit/capture_serial.py --port /dev/ttyUSB0 --baud 115200 --seconds 15 --out /tmp/plan1-task1.log
grep -a "booting from partition 'ota_0'" /tmp/plan1-task1.log
```
Expected: the grep matches — the A/B table is live and the bootloader selected `ota_0`.

- [ ] **Step 4: Commit**

```bash
git add firmware/helixscreen-esp32
git commit -m "feat(esp32-fw): project skeleton with OTA A/B partition layout"
```

---

### Task 2: Board table + display bring-up

**Files:**
- Create: `firmware/helixscreen-esp32/boards/ktouch.h`
- Create: `firmware/helixscreen-esp32/main/board_display.h`
- Create: `firmware/helixscreen-esp32/main/board_display.c`
- Modify: `firmware/helixscreen-esp32/main/CMakeLists.txt` (add source + esp_lcd/driver deps)
- Modify: `firmware/helixscreen-esp32/main/app_main.c` (call board_display_init + test pattern)

**Interfaces:**
- Produces: `esp_lcd_panel_handle_t board_display_init(void);` and `void board_display_backlight(uint8_t percent);` — Task 3 consumes the panel handle for the LVGL flush path.

- [ ] **Step 1: Write the board table**

`firmware/helixscreen-esp32/boards/ktouch.h` — every value probe-verified on hardware (`docs/devel/printer-research/BTT_K_TOUCH_HARDWARE.md`):
```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// BTT K-Touch board table. Pin map verified on-device by firmware/ktouch-probe
// (color bars + GT911 product-ID read). New boards add a sibling header and a
// -DHELIX_BOARD=<name> selection; they are a config entry, not a fork.
#pragma once

#define BOARD_NAME            "btt-ktouch"

#define BOARD_LCD_H_RES       800
#define BOARD_LCD_V_RES       480
#define BOARD_LCD_PCLK_HZ     14800000
#define BOARD_LCD_HSYNC_PW    4
#define BOARD_LCD_HSYNC_BP    16
#define BOARD_LCD_HSYNC_FP    16
#define BOARD_LCD_VSYNC_PW    4
#define BOARD_LCD_VSYNC_BP    32
#define BOARD_LCD_VSYNC_FP    32

#define BOARD_LCD_PIN_RESET   46
#define BOARD_LCD_PIN_DE      38
#define BOARD_LCD_PIN_PCLK    5
// R0-R4, G0-G5, B0-B4 (16-bit RGB565 parallel)
#define BOARD_LCD_DATA_PINS   {17, 18, 48, 47, 39, 11, 12, 13, 14, 15, 16, 6, 7, 8, 9, 10}

#define BOARD_BACKLIGHT_PIN   21

#define BOARD_TOUCH_I2C_SCL   1
#define BOARD_TOUCH_I2C_SDA   2
#define BOARD_TOUCH_PIN_IRQ   40
#define BOARD_TOUCH_PIN_RST   41
// GT911 at 0x5D (IRQ held low through reset); only device on the bus.
```

- [ ] **Step 2: Write the display driver**

`firmware/helixscreen-esp32/main/board_display.h`:
```c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esp_lcd_panel_ops.h"

// Brings up the RGB panel (reset, esp_lcd config, bounce buffers) and the
// LEDC backlight at full brightness. Aborts on failure (nothing to fall
// back to on a display device).
esp_lcd_panel_handle_t board_display_init(void);

// 0-100. LEDC 11-bit duty under the hood.
void board_display_backlight(uint8_t percent);
```

`firmware/helixscreen-esp32/main/board_display.c` (bring-up logic proven in the audit; 10-line bounce buffers are REQUIRED — direct PSRAM scanout desyncs):
```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "board_display.h"
#include "ktouch.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_rgb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_lcd_panel_handle_t board_display_init(void) {
    gpio_config_t rst = {.pin_bit_mask = 1ULL << BOARD_LCD_PIN_RESET,
                         .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&rst));
    gpio_set_level(BOARD_LCD_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_LCD_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
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
        .num_fbs = 1,
        // 10-line bounce buffers: direct PSRAM scanout visibly desyncs when
        // redraw traffic competes for PSRAM bandwidth (audit Task 1 trap).
        .bounce_buffer_size_px = 10 * BOARD_LCD_H_RES,
        .hsync_gpio_num = -1,
        .vsync_gpio_num = -1,
        .de_gpio_num = BOARD_LCD_PIN_DE,
        .pclk_gpio_num = BOARD_LCD_PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = BOARD_LCD_DATA_PINS,
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
        .gpio_num = BOARD_BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
        .duty = (1 << 11) - 1,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    return panel;
}

void board_display_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;
    uint32_t duty = ((uint32_t)percent * ((1 << 11) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
```

- [ ] **Step 3: Wire into app_main with a test pattern**

Modify `main/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "app_main.c" "board_display.c"
                       PRIV_REQUIRES esp_timer app_update esp_lcd driver
                       INCLUDE_DIRS "." "../boards")
```

In `app_main.c`, after the partition log line, add:
```c
    esp_lcd_panel_handle_t panel = board_display_init();
    // Test pattern: three horizontal color bands drawn via draw_bitmap —
    // proves data-pin order and scanout before LVGL enters the picture.
    static uint16_t line[BOARD_LCD_H_RES];
    for (int y = 0; y < BOARD_LCD_V_RES; y++) {
        uint16_t c = (y < 160) ? 0xF800 : (y < 320) ? 0x07E0 : 0x001F;
        for (int x = 0; x < BOARD_LCD_H_RES; x++) line[x] = c;
        esp_lcd_panel_draw_bitmap(panel, 0, y, BOARD_LCD_H_RES, y + 1, line);
    }
    ESP_LOGI(TAG, "display: test pattern up");
```
(add `#include "board_display.h"` and `#include "ktouch.h"` at the top.)

- [ ] **Step 4: Build, flash, HIL-verify (user confirmation required)**

Build + flash as in Task 1. Ask the user: screen must show three solid bands — red / green / blue, top to bottom, no jitter. Serial shows `display: test pattern up`.

- [ ] **Step 5: Commit**

```bash
git add firmware/helixscreen-esp32
git commit -m "feat(esp32-fw): ktouch board table + RGB panel bring-up with test pattern"
```

---

### Task 3: helixcore + LVGL glue on a UI pthread

**Files:**
- Copy: `firmware/native-audit/components/helixcore/` → `firmware/helixscreen-esp32/components/helixcore/` (then remove `shim/` — app shims arrive in Plan 3/4; keep `lv_conf.h`, `anomaly_stub.c`, CMakeLists trimmed accordingly)
- Create: `firmware/helixscreen-esp32/main/lvgl_glue.h`
- Create: `firmware/helixscreen-esp32/main/lvgl_glue.c`
- Modify: `firmware/helixscreen-esp32/main/CMakeLists.txt`, `app_main.c`

**Interfaces:**
- Consumes: `board_display_init()` panel handle (Task 2).
- Produces: `void lvgl_glue_start(esp_lcd_panel_handle_t panel, void (*ui_build)(void));` — creates the display, buffers, tick, then runs `ui_build` + the `lv_timer_handler` loop on a 32KB pthread. Tasks 4/5 and all later plans build UI inside a `ui_build` callback.

- [ ] **Step 1: Copy helixcore and trim**

```bash
cp -r firmware/native-audit/components/helixcore firmware/helixscreen-esp32/components/
rm -rf firmware/helixscreen-esp32/components/helixcore/shim
```
Edit the copied `CMakeLists.txt`: remove `shim/platform_stubs.cpp` from SRCS and the two shim include dirs. Keep `lv_conf.h` byte-identical (LV_COLOR_DEPTH 16, LV_DRAW_BUF_STRIDE_ALIGN 1, LV_OS_PTHREAD, LV_USE_SYSMON/PERF_MONITOR back to 0). `REPO_ROOT` in the copied file resolves via `../../../..` — verify it still points at the repo root from the new location (same depth as the audit tree: firmware/X/components/helixcore → 4 up).

- [ ] **Step 2: Write the LVGL glue**

`firmware/helixscreen-esp32/main/lvgl_glue.h`:
```c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esp_lcd_panel_ops.h"

// Initializes LVGL (lv_init + lv_xml_init — LVGL does NOT call the latter;
// skipping it is heap-corruption-shaped TLSF crashes), creates the display
// with 2x80-line PSRAM draw buffers, then spawns the UI pthread: ui_build()
// once, then the lv_timer_handler loop. Everything that touches LVGL or app
// code after this call happens on that pthread (ESP-IDF pthread_self()
// asserts from raw FreeRTOS tasks).
void lvgl_glue_start(esp_lcd_panel_handle_t panel, void (*ui_build)(void));
```

`firmware/helixscreen-esp32/main/lvgl_glue.c`:
```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lvgl_glue.h"
#include "ktouch.h"

#include <pthread.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/xml/lv_xml.h"

static const char *TAG = "lvgl_glue";
static esp_lcd_panel_handle_t s_panel;
static void (*s_ui_build)(void);

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1,
                              area->y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void *ui_thread(void *arg) {
    (void)arg;
    s_ui_build();
    ESP_LOGI(TAG, "ui: built, entering render loop");
    while (true) {
        uint32_t delay = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(delay < 5 ? 5 : delay > 50 ? 50 : delay));
    }
    return NULL;
}

void lvgl_glue_start(esp_lcd_panel_handle_t panel, void (*ui_build)(void)) {
    s_panel = panel;
    s_ui_build = ui_build;

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_xml_init();

    lv_display_t *disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    size_t buf_px = BOARD_LCD_H_RES * 80;
    // LV_DRAW_BUF_ALIGN is 16; heap_caps_malloc is only 8-aligned.
    lv_color_t *buf1 = heap_caps_aligned_alloc(16, buf_px * sizeof(lv_color16_t),
                                               MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_aligned_alloc(16, buf_px * sizeof(lv_color16_t),
                                               MALLOC_CAP_SPIRAM);
    assert(buf1 && buf2);
    lv_display_set_buffers(disp, buf1, buf2, buf_px * sizeof(lv_color16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32768);
    pthread_t t;
    int err = pthread_create(&t, &attr, ui_thread, NULL);
    if (err != 0) {
        ESP_LOGE(TAG, "ui pthread_create failed: %d", err);
        abort();
    }
    pthread_join(t, NULL);
}
```

- [ ] **Step 3: Wire into app_main**

Replace the Task 2 test-pattern block in `app_main.c` with:
```c
    esp_lcd_panel_handle_t panel = board_display_init();
    lvgl_glue_start(panel, ui_build_hello);
```
and add above `app_main`:
```c
#include "lvgl_glue.h"
#include "lvgl.h"

static void ui_build_hello(void) {
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x0d1117), 0);
    lv_obj_t *card = lv_obj_create(lv_screen_active());
    lv_obj_set_size(card, 400, 120);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2332), 0);
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, "helixscreen-esp32 foundation");
    lv_obj_set_style_text_color(label, lv_color_hex(0x4fc3f7), 0);
    lv_obj_center(label);
}
```
Update `main/CMakeLists.txt` SRCS to add `lvgl_glue.c` and PRIV_REQUIRES to add `helixcore pthread`.

- [ ] **Step 4: Build, flash, HIL-verify (user confirmation required)**

Screen shows the dark background + centered card + label, stable (no flicker/jitter). Serial shows `ui: built, entering render loop`.

- [ ] **Step 5: Commit**

```bash
git add firmware/helixscreen-esp32
git commit -m "feat(esp32-fw): helixcore component + LVGL on UI pthread renders"
```

---

### Task 4: GT911 touch → lv_indev

**Files:**
- Create: `firmware/helixscreen-esp32/main/idf_component.yml`
- Create: `firmware/helixscreen-esp32/main/touch_input.h`
- Create: `firmware/helixscreen-esp32/main/touch_input.c`
- Modify: `main/CMakeLists.txt`, `app_main.c` (hello UI gains a tap-counter button)

**Interfaces:**
- Consumes: `lvgl_glue_start` (touch init is called from inside `ui_build`, on the UI pthread, after `lv_init`).
- Produces: `void touch_input_init(void);` — registers an `lv_indev`. All later plans get touch for free.

- [ ] **Step 1: Add the managed component**

`firmware/helixscreen-esp32/main/idf_component.yml`:
```yaml
dependencies:
  espressif/esp_lcd_touch_gt911: "^1"
```

- [ ] **Step 2: Write the touch driver**

`firmware/helixscreen-esp32/main/touch_input.h`:
```c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// I2C bus + GT911 + LVGL pointer indev. Call on the UI pthread after lv_init.
void touch_input_init(void);
```

`firmware/helixscreen-esp32/main/touch_input.c`:
```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_input.h"
#include "ktouch.h"

#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_touch;

static void indev_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    esp_lcd_touch_read_data(s_touch);
    uint16_t x, y;
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_coordinates(s_touch, &x, &y, NULL, &cnt, 1) && cnt) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void touch_input_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .scl_io_num = BOARD_TOUCH_I2C_SCL,
        .sda_io_num = BOARD_TOUCH_I2C_SDA,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = BOARD_TOUCH_PIN_RST,
        .int_gpio_num = BOARD_TOUCH_PIN_IRQ,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch));

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read);
    ESP_LOGI(TAG, "GT911 indev registered");
}
```

- [ ] **Step 3: Tap-counter button in the hello UI**

In `app_main.c`: make `touch_input_init();` the first line of `ui_build_hello`, add a named callback above it, and create the button below the label:
```c
static void tap_cb(lv_event_t *e) {
    static int taps = 0;
    lv_obj_t *btn_label = lv_event_get_user_data(e);
    lv_label_set_text_fmt(btn_label, "tap me: %d", ++taps);
    LV_LOG_USER("tap %d", taps);
}
```
```c
    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn, 200, 60);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "tap me: 0");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn, tap_cb, LV_EVENT_CLICKED, btn_label);
```
(Note: raw `lv_obj_add_event_cb` is banned in app code by CLAUDE.md declarative-UI rules — those rules bind when the XML engine arrives in Plan 4. Foundation bring-up predates XML and uses plain LVGL deliberately.)

- [ ] **Step 4: Build, flash, HIL-verify (user confirmation required)**

Ask the user to tap the button several places on it: counter increments, touch tracks finger position. This is the FIRST touch test on this device — if coordinates are mirrored/rotated, set `tp_cfg.flags` (`swap_xy` / `mirror_x` / `mirror_y`) accordingly and re-flash before concluding the driver is wrong.

- [ ] **Step 5: Commit**

```bash
git add firmware/helixscreen-esp32
git commit -m "feat(esp32-fw): GT911 touch via esp_lcd_touch_gt911 wired to lv_indev"
```

---

### Task 5: OTA health + rollback confirm

**Files:**
- Create: `firmware/helixscreen-esp32/main/ota_health.h`
- Create: `firmware/helixscreen-esp32/main/ota_health.c`
- Modify: `main/CMakeLists.txt` (add source), `app_main.c`

**Interfaces:**
- Consumes: runs on the UI pthread after `ui_build` (a healthy boot = UI actually built).
- Produces: `void ota_health_confirm(void);` — logs running slot + app version, and (when the image is in `ESP_OTA_IMG_PENDING_VERIFY` after a real OTA) calls `esp_ota_mark_app_valid_cancel_rollback()`. Plan 5's full OTA flow depends on this contract existing from day one.

- [ ] **Step 1: Write ota_health**

`firmware/helixscreen-esp32/main/ota_health.h`:
```c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Call once after the UI is up. On a normal boot: logs slot + version.
// On the first boot after an OTA (PENDING_VERIFY): marks the image valid so
// the bootloader won't roll back. "UI built" is the v1 health criterion;
// Plan 5 extends it (WiFi up, Moonraker reachable) before the confirm.
void ota_health_confirm(void);
```

`firmware/helixscreen-esp32/main/ota_health.c`:
```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ota_health.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";

void ota_health_confirm(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "running slot=%s version=%s", running->label, desc->version);

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "OTA image confirmed valid (rollback cancelled)");
    }
}
```

- [ ] **Step 2: Call it after ui_build in the UI thread**

In `lvgl_glue.c` `ui_thread`, after `s_ui_build();` add `ota_health_confirm();` (+ include). Add `ota_health.c` to SRCS.

- [ ] **Step 3: Build, flash, serial-verify**

```bash
grep -a "running slot=ota_0 version=" /tmp/plan1-task5.log
```
Expected: slot and a git-describe-shaped version string (IDF embeds `git describe` automatically). Rollback branch stays unexercised until Plan 5 performs a real esp_https_ota — that's correct for this plan.

- [ ] **Step 4: Commit**

```bash
git add firmware/helixscreen-esp32
git commit -m "feat(esp32-fw): OTA health confirm + rollback-cancel plumbing"
```

---

### Task 6: CI cross-build + size budget gate

**Files:**
- Create: `scripts/check_esp32_size.py`
- Create: `firmware/helixscreen-esp32/size_budget.json`
- Create: `.github/workflows/esp32-build.yml`

**Interfaces:**
- Produces: CI job `esp32-build` red/green on every PR; `check_esp32_size.py <bin> <budget.json>` exit 0/1. Plans 2-5 inherit drift protection and the budget ratchet.

- [ ] **Step 1: Budget file + checker**

`firmware/helixscreen-esp32/size_budget.json`:
```json
{
  "app_max_bytes": 6081741,
  "comment": "5.8MB = 6.0MB OTA slot minus margin. Spec: 2026-07-13-esp32-native-port-design.md. Raise only with a spec change."
}
```

`scripts/check_esp32_size.py`:
```python
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if the ESP32 app image exceeds its budget. Usage:
   check_esp32_size.py build/helixscreen_esp32.bin firmware/helixscreen-esp32/size_budget.json"""
import json
import os
import sys

def main() -> int:
    bin_path, budget_path = sys.argv[1], sys.argv[2]
    size = os.path.getsize(bin_path)
    budget = json.load(open(budget_path))["app_max_bytes"]
    pct = 100.0 * size / budget
    print(f"esp32 image: {size} bytes / budget {budget} ({pct:.1f}%)")
    if size > budget:
        print("FAIL: image exceeds budget", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Verify the checker locally, both directions**

```bash
python3 scripts/check_esp32_size.py firmware/helixscreen-esp32/build/helixscreen_esp32.bin firmware/helixscreen-esp32/size_budget.json; echo "exit=$?"
# expect exit=0 and a low percentage
python3 - <<'EOF'
import json, subprocess, tempfile, os
tiny = {"app_max_bytes": 1}
with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
    json.dump(tiny, f); path = f.name
rc = subprocess.call(["python3", "scripts/check_esp32_size.py",
                      "firmware/helixscreen-esp32/build/helixscreen_esp32.bin", path])
os.unlink(path)
assert rc == 1, "budget gate failed to fail"
print("over-budget correctly fails")
EOF
```
Expected: `exit=0` then `over-budget correctly fails`.

- [ ] **Step 3: CI workflow**

`.github/workflows/esp32-build.yml`:
```yaml
name: esp32-build
on:
  pull_request:
  push:
    branches: [main]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - name: Build firmware
        uses: espressif/esp-idf-ci-action@v1
        with:
          esp_idf_version: v5.5
          target: esp32s3
          path: firmware/helixscreen-esp32
      - name: Size budget gate
        run: |
          python3 scripts/check_esp32_size.py \
            firmware/helixscreen-esp32/build/helixscreen_esp32.bin \
            firmware/helixscreen-esp32/size_budget.json
```
A fresh CI checkout has UNPATCHED submodules — desktop builds apply `patches/*.patch` via `mk/patches.mk`, and helix-xml is paired with the patched LVGL. The workflow must run the repo's own target before the firmware build (insert between checkout and "Build firmware"):
```yaml
      - name: Apply submodule patches
        run: make apply-patches
```

- [ ] **Step 4: Verify CI on a PR**

Push the branch, open a draft PR, confirm the `esp32-build` job goes green and prints the size line. (CI is the verification — there is no local substitute for the fresh-checkout property.)

- [ ] **Step 5: Commit**

```bash
git add scripts/check_esp32_size.py firmware/helixscreen-esp32/size_budget.json .github/workflows/esp32-build.yml
git commit -m "ci(esp32-fw): cross-build workflow + image size budget gate (5.8MB)"
```

---

## Definition of done (Plan 1)

- K-Touch boots from `ota_0` to a stable LVGL screen with working touch (user-verified).
- `ota_health_confirm` logs slot + version on every boot.
- CI builds the firmware from a fresh checkout and enforces the 5.8MB budget.
- Zero changes outside `firmware/helixscreen-esp32/`, `scripts/check_esp32_size.py`, `.github/workflows/esp32-build.yml`.
