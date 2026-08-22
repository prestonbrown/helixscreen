# ESP32-S3 Terminal Firmware (helix-terminal) Implementation Plan — Phase 1c

> 🚧 **Work in flight (as of 2026-08-09) - coordinate before executing.**
>
> Unchecked boxes here do NOT mean "not started". The ESP-IDF sources and build output live
> in `firmware/` at the repo root, which is deliberately untracked, and the active work is on
> the `esp32/port-4-app` branch. A grep of the tracked tree finds none of the symbols this
> plan prescribes (`board.h`, `ota.h`, `provisioning.h`) and will wrongly suggest nothing has
> happened. Check the branch and `firmware/` before concluding anything, and ask before
> picking up a task.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Prerequisite:** Phase 1b (`2026-06-10-remote-display-backend.md`) must be implemented first — this firmware is its client. Hardware-in-loop steps require an ESP32-S3 dev board with an 800×480 RGB panel + capacitive touch (e.g. Waveshare ESP32-S3-Touch-LCD-7 or an equivalent Guition/Elecrow 7" S3 board) and ESP-IDF v5.2+ installed (`idf.py` on PATH). Steps marked **[HW]** can only be verified on the device; everything else builds and tests on the host.

**Goal:** ESP32-S3 firmware that connects over WiFi to a HelixScreen RFB server, displays the streamed UI, and sends touch back — a dumb terminal with provisioning and OTA, per Phase 1c of `docs/devel/plans/2026-06-10-esp32-display-device-design.md`.

**Architecture:** ESP-IDF project at `firmware/helix-terminal/` with four components: `board` (esp_lcd RGB panel + touch behind a board-config table), `provisioning` (SoftAP captive portal + NVS + mDNS browse), `rfb_client` (transport-free protocol core in plain C, host-compilable and unit-tested in the main repo's Catch2 suite, plus a thin esp socket task), and `ota` (esp_https_ota from a manifest URL). v1 scope freeze: frames + splash/provisioning screens only — no local fallback UI.

**Tech Stack:** ESP-IDF v5.2+ (esp_lcd, esp_lcd_touch_gt911, esp_wifi, esp_http_server, mdns, esp_https_ota, NVS, LittleFS optional), plain C. Host tests: Catch2 in the main repo (`tests/unit/test_rfb_client_core.cpp` `#include`s the component's C file directly — zero build-system surgery).

**Protocol contract:** the RFB 3.8 subset defined in `2026-06-10-remote-display-backend.md` (wire reference table there is normative): security None, RGB565-LE pixel format, encodings Raw(0) + Hextile(5) where Hextile tiles arrive only as subencoding 1 (raw) or 2 (background-specified solid) from our server — but the decoder MUST handle full Hextile (4/8/16 bits) since desktop-grade servers may be used for testing. Identity arrives in the ServerInit name (expect prefix `"HelixScreen"` to gate the pairing UI, but never refuse other servers — they're useful for testing).

---

## File structure

```text
firmware/helix-terminal/
├── CMakeLists.txt                  # idf project; EXTRA_COMPONENT_DIRS=components
├── sdkconfig.defaults              # PSRAM octal, 16MB flash, partition table, FreeRTOS hz
├── partitions.csv                  # nvs, otadata, ota_0, ota_1, storage
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.c                  # boot: nvs→board→provisioning-or-connect→rfb task
│   └── splash.c / splash.h         # status text on framebuffer (no LVGL in v1)
├── components/
│   ├── board/
│   │   ├── CMakeLists.txt
│   │   ├── include/board.h         # board_init(), board_fb(), board_touch_read()
│   │   ├── board.c                 # esp_lcd RGB panel + touch init from board_config
│   │   └── boards/ws_s3_lcd7.h     # pin/timing table for the dev board (one entry per supported board)
│   ├── provisioning/
│   │   ├── CMakeLists.txt
│   │   ├── include/provisioning.h  # prov_needed(), prov_run_portal(), prov_get_config()
│   │   └── provisioning.c          # SoftAP + esp_http_server captive portal + NVS + mDNS browse
│   ├── rfb_client/
│   │   ├── CMakeLists.txt
│   │   ├── include/rfb_client_core.h   # transport-free: handshake builder, message parser, hextile/raw decoder
│   │   ├── rfb_client_core.c           # PLAIN C, NO esp includes — host-compilable
│   │   ├── include/rfb_client_task.h
│   │   └── rfb_client_task.c           # esp socket loop: connect, pump core, blit, touch tx
│   └── ota/
│       ├── CMakeLists.txt
│       ├── include/ota.h
│       └── ota.c                   # esp_https_ota from manifest URL in NVS
└── README.md                       # build/flash/monitor + provisioning walkthrough

helixscreen repo (host tests):
└── tests/unit/test_rfb_client_core.cpp   # #includes ../../firmware/helix-terminal/components/rfb_client/rfb_client_core.c
```

---

### Task 1: rfb_client_core — transport-free decoder, tested on host (TDD)

**Files:**
- Create: `firmware/helix-terminal/components/rfb_client/include/rfb_client_core.h`
- Create: `firmware/helix-terminal/components/rfb_client/rfb_client_core.c`
- Test: `tests/unit/test_rfb_client_core.cpp` (main repo, auto-discovered by mk/tests.mk)

This is the load-bearing module — write it first, on the host, before any hardware exists.

- [ ] **Step 1: Failing tests.** `tests/unit/test_rfb_client_core.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// Host-compiles the firmware's transport-free RFB client core.
extern "C" {
#include "../../firmware/helix-terminal/components/rfb_client/rfb_client_core.c"
}
#include "../catch_amalgamated.hpp"
#include <cstring>
#include <vector>

TEST_CASE("client handshake state machine", "[rfb_client][quick]") {
    rfbc_t c;
    uint16_t fb[64 * 64];
    rfbc_init(&c, fb, 64, 64);

    // Server greeting → client replies with same version
    uint8_t out[64];
    size_t out_len = 0;
    REQUIRE(rfbc_feed(&c, (const uint8_t*)"RFB 003.008\n", 12, out, sizeof(out),
                      &out_len) == RFBC_OK);
    REQUIRE(out_len == 12);
    REQUIRE(memcmp(out, "RFB 003.008\n", 12) == 0);

    // Security types {1, None} → client chooses None
    const uint8_t sec[] = {1, 1};
    REQUIRE(rfbc_feed(&c, sec, 2, out, sizeof(out), &out_len) == RFBC_OK);
    REQUIRE(out_len == 1);
    REQUIRE(out[0] == 1);

    // SecurityResult OK → client sends ClientInit(shared=1)
    const uint8_t ok[] = {0, 0, 0, 0};
    REQUIRE(rfbc_feed(&c, ok, 4, out, sizeof(out), &out_len) == RFBC_OK);
    REQUIRE(out_len == 1);

    // ServerInit 800x480 RGB565 name "HelixScreen" → client sends
    // SetPixelFormat(RGB565) + SetEncodings + first full update request
    std::vector<uint8_t> si = {
        0x03, 0x20, 0x01, 0xE0,                          // 800, 480
        16, 16, 0, 1, 0, 31, 0, 63, 0, 31, 11, 5, 0,     // pixel format
        0, 0, 0,                                          // pad
        0, 0, 0, 11, 'H','e','l','i','x','S','c','r','e','e','n'};
    uint8_t out2[128];
    REQUIRE(rfbc_feed(&c, si.data(), si.size(), out2, sizeof(out2), &out_len) == RFBC_OK);
    REQUIRE(c.state == RFBC_STATE_READY);
    REQUIRE(c.server_w == 800);
    REQUIRE(c.server_h == 480);
    REQUIRE(out_len > 0);  // SetPixelFormat + SetEncodings + FBUpdateRequest
}

static rfbc_t make_ready_client(uint16_t* fb, int w, int h) {
    rfbc_t c;
    rfbc_init(&c, fb, w, h);
    rfbc_force_ready_for_test(&c, w, h);
    return c;
}

TEST_CASE("decode raw rect into framebuffer", "[rfb_client][quick]") {
    uint16_t fb[8 * 8] = {};
    rfbc_t c = make_ready_client(fb, 8, 8);
    // FramebufferUpdate, 1 rect: (1,1,2,1) Raw, pixels {0xF800, 0x07E0} LE
    const uint8_t msg[] = {0, 0, 0, 1,
                           0, 1, 0, 1, 0, 2, 0, 1,  0, 0, 0, 0,
                           0x00, 0xF8, 0xE0, 0x07};
    uint8_t out[8]; size_t out_len = 0;
    REQUIRE(rfbc_feed(&c, msg, sizeof(msg), out, sizeof(out), &out_len) == RFBC_OK);
    REQUIRE(fb[1 * 8 + 1] == 0xF800);
    REQUIRE(fb[1 * 8 + 2] == 0x07E0);
    REQUIRE(c.frames_completed == 1);
}

TEST_CASE("decode hextile solid + raw tiles", "[rfb_client][quick]") {
    uint16_t fb[32 * 16] = {};
    rfbc_t c = make_ready_client(fb, 32, 16);
    // 1 rect (0,0,32,16) Hextile = two 16x16 tiles:
    //   tile0: subenc 2 (bg specified) bg=0x001F → solid blue
    //   tile1: subenc 1 (raw) 256 px of 0xF800
    std::vector<uint8_t> msg = {0, 0, 0, 1,
                                0, 0, 0, 0, 0, 32, 0, 16,  0, 0, 0, 5,
                                2, 0x1F, 0x00,
                                1};
    for (int i = 0; i < 256; ++i) { msg.push_back(0x00); msg.push_back(0xF8); }
    uint8_t out[8]; size_t out_len = 0;
    REQUIRE(rfbc_feed(&c, msg.data(), msg.size(), out, sizeof(out), &out_len) == RFBC_OK);
    REQUIRE(fb[0] == 0x001F);
    REQUIRE(fb[5 * 32 + 5] == 0x001F);
    REQUIRE(fb[0 * 32 + 16] == 0xF800);
    REQUIRE(fb[15 * 32 + 31] == 0xF800);
}

TEST_CASE("decode hextile with subrects (full spec)", "[rfb_client]") {
    uint16_t fb[16 * 16] = {};
    rfbc_t c = make_ready_client(fb, 16, 16);
    // tile: bg specified (2) + any-subrects (8) + coloured (16) = 26
    // bg=0x0000, 1 subrect: colour 0xFFFF at x=2,y=3 w=4,h=2
    // subrect encoding: [colour(2B LE)] [xy: x<<4|y] [wh: (w-1)<<4|(h-1)]
    const uint8_t msg[] = {0, 0, 0, 1,
                           0, 0, 0, 0, 0, 16, 0, 16,  0, 0, 0, 5,
                           26, 0x00, 0x00, 1,
                           0xFF, 0xFF, 0x23, 0x31};
    uint8_t out[8]; size_t out_len = 0;
    REQUIRE(rfbc_feed(&c, msg, sizeof(msg), out, sizeof(out), &out_len) == RFBC_OK);
    REQUIRE(fb[3 * 16 + 2] == 0xFFFF);
    REQUIRE(fb[4 * 16 + 5] == 0xFFFF);
    REQUIRE(fb[0] == 0x0000);
    REQUIRE(fb[3 * 16 + 6] == 0x0000);
}

TEST_CASE("partial feed across tcp segment boundaries", "[rfb_client][quick]") {
    uint16_t fb[8 * 8] = {};
    rfbc_t c = make_ready_client(fb, 8, 8);
    const uint8_t msg[] = {0, 0, 0, 1,
                           0, 1, 0, 1, 0, 2, 0, 1,  0, 0, 0, 0,
                           0x00, 0xF8, 0xE0, 0x07};
    uint8_t out[8]; size_t out_len = 0;
    // Feed one byte at a time — must never error, must complete at the end
    for (size_t i = 0; i < sizeof(msg); ++i) {
        REQUIRE(rfbc_feed(&c, &msg[i], 1, out, sizeof(out), &out_len) == RFBC_OK);
    }
    REQUIRE(fb[1 * 8 + 1] == 0xF800);
}

TEST_CASE("pointer event encoder", "[rfb_client][quick]") {
    uint8_t out[8];
    size_t n = rfbc_build_pointer_event(out, /*pressed=*/true, 100, 200);
    REQUIRE(n == 6);
    REQUIRE(out[0] == 5);
    REQUIRE(out[1] == 1);
    REQUIRE(out[2] == 0x00); REQUIRE(out[3] == 0x64);
    REQUIRE(out[4] == 0x00); REQUIRE(out[5] == 0xC8);
}

TEST_CASE("update request builder", "[rfb_client][quick]") {
    uint8_t out[16];
    size_t n = rfbc_build_update_request(out, /*incremental=*/true, 0, 0, 800, 480);
    REQUIRE(n == 10);
    REQUIRE(out[0] == 3);
    REQUIRE(out[1] == 1);
}
```

- [ ] **Step 2:** `make test` → fails (files don't exist).
- [ ] **Step 3: Implement** `rfb_client_core.h` / `.c`. Header API (plain C, no ESP includes, no malloc — caller provides the framebuffer):

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum { RFBC_OK = 0, RFBC_ERR_PROTOCOL = -1, RFBC_ERR_OVERFLOW = -2 } rfbc_result_t;
typedef enum { RFBC_STATE_AWAIT_VERSION, RFBC_STATE_AWAIT_SECURITY,
               RFBC_STATE_AWAIT_SECRESULT, RFBC_STATE_AWAIT_SERVERINIT,
               RFBC_STATE_READY, RFBC_STATE_FAILED } rfbc_state_t;

#define RFBC_INBUF_SIZE 2048      /* protocol headers only; pixel data streams */

typedef struct {
    rfbc_state_t state;
    uint16_t* fb;                  /* caller-owned RGB565 framebuffer */
    uint16_t fb_w, fb_h;
    uint16_t server_w, server_h;
    char server_name[64];
    uint32_t frames_completed;
    /* internal: header accumulation + streaming rect decode cursor */
    uint8_t inbuf[RFBC_INBUF_SIZE];
    size_t inbuf_len;
    /* current rect being streamed (Raw) / tile cursor (Hextile) */
    int rect_active;
    uint16_t rx, ry, rw, rh; int32_t rencoding;
    uint16_t rects_remaining;
    size_t rect_bytes_done;        /* raw streaming progress */
    int tile_col, tile_row;        /* hextile cursor */
} rfbc_t;

void rfbc_init(rfbc_t* c, uint16_t* fb, uint16_t fb_w, uint16_t fb_h);
void rfbc_force_ready_for_test(rfbc_t* c, uint16_t w, uint16_t h);

/// Feed received bytes. Response bytes the caller must transmit are written
/// to out (handshake replies + the automatic post-frame incremental
/// FramebufferUpdateRequest). Never blocks; never allocates.
rfbc_result_t rfbc_feed(rfbc_t* c, const uint8_t* data, size_t len,
                        uint8_t* out, size_t out_cap, size_t* out_len);

size_t rfbc_build_pointer_event(uint8_t* out, int pressed, uint16_t x, uint16_t y);
size_t rfbc_build_update_request(uint8_t* out, int incremental,
                                 uint16_t x, uint16_t y, uint16_t w, uint16_t h);
```

Implementation requirements (the executor writes the .c against the tests):
- **Streaming decode** — Raw rect pixel data is written into `fb` as it arrives (track `rect_bytes_done`), NEVER buffered whole (a full frame is 768KB; `inbuf` is 2KB and only ever holds headers). Hextile decodes tile-by-tile; a tile (≤513 bytes worst case) may be header-buffered.
- Full Hextile per spec: subencoding bits 1/2/4/8/16, foreground colour persistence across tiles within a rect, coloured + same-colour subrect forms.
- After the last rect of an update completes, auto-emit an incremental `FramebufferUpdateRequest` into `out` (continuous pull loop) and bump `frames_completed`.
- Pixel format on the wire is whatever the client requested = RGB565-LE; `fb` stores native RGB565 (no conversion — memcpy/assign).

- [ ] **Step 4:** `make test && ./build/bin/helix-tests "[rfb_client]"` → PASS.
- [ ] **Step 5: Cross-check against the real server** (Phase 1b must be merged): run `HELIX_DISPLAY_BACKEND=remote ./build/bin/helix-screen --test` and extend `tests/python/rfb_probe.py` usage docs if anything in the contract surprised you. **Commit:**

```bash
git add firmware/helix-terminal/components/rfb_client/include/rfb_client_core.h firmware/helix-terminal/components/rfb_client/rfb_client_core.c tests/unit/test_rfb_client_core.cpp
git commit -m "feat(helix-terminal): host-tested transport-free RFB client core"
```

---

### Task 2: ESP-IDF project skeleton + board bring-up **[HW]**

**Files:** `firmware/helix-terminal/{CMakeLists.txt,sdkconfig.defaults,partitions.csv,main/*,components/board/*}` per the file-structure table.

- [ ] **Step 1:** `idf.py create-project`-style skeleton with the layout above; `sdkconfig.defaults`: `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_120M=y` (fall back to 80M if the board errata requires), `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`, `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`. `partitions.csv`: nvs(24K), otadata(8K), ota_0(6M), ota_1(6M), storage/littlefs(remainder) — adjust to the actual flash map.
- [ ] **Step 2:** `board` component: `board_init()` brings up the RGB panel via `esp_lcd_new_rgb_panel` with timings from `boards/ws_s3_lcd7.h` (pin table, pclk, porches — **transcribe from the actual board's schematic/vendor demo; do not trust generated guesses**) and double `fb` in PSRAM via `esp_lcd_rgb_panel_get_frame_buffer`; touch via `esp_lcd_touch_new_i2c_gt911`. API: `uint16_t* board_fb(void)`, `bool board_touch_read(uint16_t* x, uint16_t* y, bool* pressed)`, `void board_flush(int x, int y, int w, int h)` (no-op for direct-scanout RGB panels; seam kept for SPI panels later).
- [ ] **Step 3:** `main/app_main.c` v0: init NVS + board, fill framebuffer with a test pattern (color bars + corner markers), print heap/PSRAM stats. `idf.py build flash monitor` → **[HW]** verify panel shows the pattern, touch coordinates log on tap, no PSRAM errors. This validates the board table before any protocol work.
- [ ] **Step 4: Commit** (whole firmware skeleton):

```bash
git add firmware/helix-terminal
git commit -m "feat(helix-terminal): ESP-IDF skeleton + S3 board bring-up (RGB panel, GT911)"
```

---

### Task 3: WiFi provisioning + host discovery **[HW]**

**Files:** `components/provisioning/*`, `main/app_main.c`, `main/splash.c`

- [ ] **Step 1:** NVS schema: namespace `helixterm`, keys `wifi_ssid`, `wifi_pass`, `host`, `port` (u16, default 5900), `ota_url`. `prov_needed()` = no `wifi_ssid` OR boot-button held 3s (GPIO0).
- [ ] **Step 2:** Portal: SoftAP `HelixTerminal-XXXX` (MAC suffix), `esp_http_server` serving one embedded HTML page (form: SSID picker from `esp_wifi_scan`, password, host [blank = auto-discover], port) + a `/save` POST that writes NVS and reboots. DNS-hijack captive portal redirect (standard ESP-IDF captive portal example pattern).
- [ ] **Step 3:** Discovery: after WiFi connects (STA), if `host` empty → `mdns_query_ptr("_helixscreen-rfb", "_tcp", ...)` (ESP-IDF mdns component); first responder wins, cache in RAM (not NVS — host IPs change).
- [ ] **Step 4:** `splash.c`: direct-to-framebuffer status text (embed an 8x16 bitmap font, ~4KB): "Connecting to <ssid>…", "Searching for HelixScreen…", "Connected — waiting for frames", error states. No LVGL.
- [ ] **Step 5: [HW]** Full provisioning walkthrough on the device: fresh flash → portal appears → join, configure → reboots → splash shows connection progress. Document the walkthrough with photos in `firmware/helix-terminal/README.md`. **Commit.**

```bash
git add firmware/helix-terminal
git commit -m "feat(helix-terminal): SoftAP provisioning portal + mDNS host discovery + splash"
```

---

### Task 4: RFB client task — frames on glass **[HW]**

**Files:** `components/rfb_client/rfb_client_task.{h,c}`, `main/app_main.c`

- [ ] **Step 1:** `rfb_client_task.c`: FreeRTOS task (8KB stack, core 1): resolve host → blocking `connect()` → `rfbc_init(&c, board_fb(), w, h)` → loop `recv()` into a 4KB buffer → `rfbc_feed` → `send()` any out bytes → on frame completion call `board_flush(...)`. Socket options: `TCP_NODELAY` on, `SO_RCVTIMEO` 5s (timeout → reconnect). Disconnect/error → splash "Reconnecting…" + exponential backoff (1s→2s→4s→max 15s) → reconnect.
- [ ] **Step 2:** Touch TX: a second small task (or tick in the same loop at recv timeout granularity): poll `board_touch_read` at ~50Hz; on change (press/move/release) `rfbc_build_pointer_event` → `send()`. Move events coalesced to the 50Hz poll naturally; send immediately, never batched behind frame requests (spec requirement).
- [ ] **Step 3:** Resolution mismatch policy (v1): if ServerInit WxH ≠ panel WxH, letterbox if server is smaller, else show persistent splash error "Host is 800x480, panel is WxH — set HELIX_DISPLAY_REMOTE_SIZE on the host." (Matches the spec: v1 says configure host to match.)
- [ ] **Step 4: [HW] The milestone:** `HELIX_DISPLAY_BACKEND=remote ./build/bin/helix-screen --test` on the dev machine; ESP32 on same LAN discovers it and renders HelixScreen; tapping buttons works. Measure and record in README: full-frame time, panel-transition latency, touch-to-response latency (rough phone-camera numbers fine). **Commit.**

```bash
git add firmware/helix-terminal
git commit -m "feat(helix-terminal): RFB client task - frames + touch on hardware"
```

---

### Task 5: OTA + soak

**Files:** `components/ota/*`

- [ ] **Step 1:** `ota.c`: on boot (after WiFi, before RFB) and then every 24h: GET `ota_url` (JSON manifest `{"version": "x.y.z", "url": "https://.../helix-terminal.bin", "sha256": "..."}`); if version > running (`esp_app_get_description()->version`), `esp_https_ota` with hash check; reboot on success. `ota_url` empty (default) = OTA disabled — BTT or users opt in by setting it in the portal. Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, mark-valid only after first successful RFB frame.
- [ ] **Step 2: [HW]** Exercise one OTA cycle against a local manifest (python http.server). Verify rollback by flashing a build that fails before first frame.
- [ ] **Step 3: [HW] Soak:** overnight run against a `--test` host; record reconnect count, heap low-water (`esp_get_minimum_free_heap_size`), any watchdog events in README. **Commit.**

```bash
git add firmware/helix-terminal
git commit -m "feat(helix-terminal): manifest OTA with rollback + soak fixes"
```

---

### Task 6: Repo integration + docs

- [ ] **Step 1:** `firmware/helix-terminal/README.md` complete: prerequisites, `idf.py build flash monitor`, provisioning walkthrough, supported-boards table, measured performance numbers, troubleshooting (panel timing, PSRAM, WiFi).
- [ ] **Step 2:** Root docs: add a row to the docs index in `docs/devel/CLAUDE.md` table if conventions allow, and link from `docs/devel/REMOTE_DISPLAY.md` (created by Phase 1b plan).
- [ ] **Step 3:** Confirm the main repo build/tests are untouched-green: `make -j && make test-run` (the only main-repo file this plan adds is `tests/unit/test_rfb_client_core.cpp`).
- [ ] **Step 4:** Final commit; verify series hygiene (`git log --stat`, each commit scoped).

## Out of Scope (YAGNI)

- Local fallback UI on the terminal (scope-gravity risk named in the spec — frames + splash only).
- SPI-panel boards, non-GT911 touch (the board-table seam exists; entries come later).
- ZRLE decode (server sends Raw/Hextile only; revisit with the server).
- Resolution auto-match (v2, spec open item).
- BTT-specific OTA infrastructure (manifest URL is the pluggable seam).
- Keyboard/ClientCutText support.
