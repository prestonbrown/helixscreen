# Remote Display Backend (RFB Server) Implementation Plan — Phase 1b

> 🚧 **Work in flight (as of 2026-08-09) - coordinate before executing.**
>
> Unchecked boxes here do NOT mean "not started". This is the host-side half of the ESP32
> display work, whose device-side sources live in the untracked `firmware/` directory with
> the active branch at `esp32/port-4-app`. A grep of the tracked tree finds none of the
> `rfb_*` symbols this plan prescribes and will wrongly suggest nothing has happened. Check
> the branch before concluding anything, and ask before picking up a task.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fourth display backend (`DisplayBackendType::REMOTE`) that renders HelixScreen headless and serves the screen to one client over an RFB (VNC) 3.8 protocol subset, with touch coming back as pointer events — per Phase 1b of `docs/devel/plans/2026-06-10-esp32-display-device-design.md`.

**Architecture:** Three layers. (1) `rfb_protocol` — pure, socket-free protocol framing + Raw/Hextile encoders + ARGB8888→RGB565 conversion, fully unit-tested against golden bytes. (2) `RfbServer` — libhv `hv::TcpServer` composition owning the shadow framebuffer, dirty-rect accumulator, per-client state machine, and a pointer-event queue; protocol work stays on libhv's event-loop thread, shared state is mutex-guarded plain data (no LVGL calls off main thread). (3) `DisplayBackendRemote` — implements the 5 pure virtuals; its LVGL flush callback (main thread) copies dirty rects into the shadow framebuffer; its lv_indev read callback (main thread) drains the pointer queue.

**Tech Stack:** libhv TcpServer (already compiled into `build/lib/libhv.a` — server symbols included), LVGL 9.5 (`lv_display_create` + `LV_DISPLAY_RENDER_MODE_PARTIAL`), Catch2, optional Python integration client in `tests/python/`.

**Verified facts an implementer needs:**
- Pure virtuals to implement (`include/display_backend.h:184-459`): `create_display(int,int)`, `create_input_pointer()`, `type() const`, `name() const`, `is_available() const`. Everything else has safe defaults (`create_input_keyboard`→nullptr, `blank/unblank`→false, calibration→none needed since pointer events arrive in screen coordinates).
- `DisplayBackendType` enum at `include/display_backend.h:28-33`; string mapping in `display_backend_type_to_string` (:50-63); factory `create()`/`create_auto()` in `src/api/display_backend.cpp` (env override `HELIX_DISPLAY_BACKEND` parsed at :201-240 — accepts "drm"/"fbdev"/"fb"/"sdl" today).
- Headless display precedent: `tests/lvgl_test_fixture.cpp:74-88` — `lv_display_create(w,h)` + `lv_display_set_buffers(..., LV_DISPLAY_RENDER_MODE_PARTIAL)` + `lv_display_set_flush_cb(...)`; flush cb signature `void(lv_display_t*, const lv_area_t*, uint8_t*)`; must call `lv_display_flush_ready(disp)`.
- Build: `mk/display-lib.mk` uses an EXPLICIT source list (`DISPLAY_API_SRCS`, lines 15-42) — new backend file must be added there. Guard with `#ifdef HELIX_DISPLAY_REMOTE`.
- libhv server (`lib/libhv/include/hv/TcpServer.h`): `hv::TcpServer`; `createsocket(port, host)`; `onConnection`/`onMessage` (`std::function<void(const SocketChannelPtr&, Buffer*)>`) run on **libhv event-loop threads, never main**; `channel->write(data, size)` is thread-safe; `setMaxConnectionNum`, `stop()`. No WS framing needed — raw TCP.
- Host builds render `LV_COLOR_DEPTH 32` (ARGB8888, `lv_conf.h:29-40`); embedded 16. The encoder converts to RGB565 at encode time, so the backend works on both.
- zlib is already linked everywhere (`-lz`, Makefile:568 etc.) — NOT needed for v1 (Raw+Hextile only) but available for ZRLE later.
- mDNS: embedded `mdns/mdns.h` library with a discovery client at `src/network/mdns_discovery.cpp`; no advertising code exists yet.
- Threading rules: anything touching LVGL/subjects from libhv threads must go through `helix::ui::queue_update()` / `AsyncLifetimeGuard::bg_cb` (`include/ui_update_queue.h`, `include/async_lifetime_guard.h`). This design deliberately avoids needing them on the hot path: flush (main) and encode/send (libhv) meet only at a mutex-guarded byte buffer; pointer events meet at a mutex-guarded queue drained by the lv_indev read callback (main).

**Design deviation from the spec, intentional:** the spec suggested a pseudo-encoding identity rect; this plan instead carries identity in the standard ServerInit `name` field (`"HelixScreen <version>"`). Same information, zero protocol extension, works with every viewer. Update the spec doc's open-items note when this plan ships.

---

## RFB 3.8 wire reference (the subset we implement)

All multi-byte integers BIG-ENDIAN on the wire (pixel data itself follows the negotiated pixel format's endianness — we use little-endian RGB565).

```text
Handshake:
  S→C: "RFB 003.008\n"                      (12 bytes)
  C→S: "RFB 003.008\n"                      (12 bytes; accept 003.003/003.007 by closing politely in v1)
  S→C: u8 nTypes=1, u8 types[]={1}          (security: None)
  C→S: u8 chosen=1
  S→C: u32 SecurityResult=0                 (OK)
  C→S: u8 shared                            (ClientInit; ignore value)
  S→C: ServerInit:
        u16 fb_width, u16 fb_height,
        PIXEL_FORMAT(16 bytes), u32 name_len, name bytes

PIXEL_FORMAT (16 bytes):
  u8 bits_per_pixel, u8 depth, u8 big_endian, u8 true_colour,
  u16 red_max, u16 green_max, u16 blue_max,
  u8 red_shift, u8 green_shift, u8 blue_shift, u8 pad[3]
  RGB565-LE: bpp=16 depth=16 be=0 tc=1 rmax=31 gmax=63 bmax=31 rsh=11 gsh=5 bsh=0

Client→Server messages (first byte = type):
  0 SetPixelFormat:      u8 pad[3], PIXEL_FORMAT
  2 SetEncodings:        u8 pad, u16 count, s32 encodings[count]
  3 FramebufferUpdateRequest: u8 incremental, u16 x, u16 y, u16 w, u16 h
  4 KeyEvent:            u8 down, u16 pad, u32 keysym          (accept + drop)
  5 PointerEvent:        u8 button_mask, u16 x, u16 y          (bit0 = left/touch)
  6 ClientCutText:       u8 pad[3], u32 len, bytes[len]        (accept + drop)

Server→Client:
  0 FramebufferUpdate:   u8 type=0, u8 pad, u16 nRects, then per rect:
                         u16 x, u16 y, u16 w, u16 h, s32 encoding, payload
  Encodings: Raw=0 (w*h*2 bytes RGB565), Hextile=5

Hextile (encoding 5): rect split into 16x16 tiles, row-major; last row/col may
be smaller. Per tile: u8 subencoding mask — 1=Raw, 2=BackgroundSpecified,
4=ForegroundSpecified, 8=AnySubrects, 16=SubrectsColoured.
v1 encoder emits only two tile forms:
  - solid tile:  subencoding=2, followed by 1 bg pixel (2 bytes)
  - mixed tile:  subencoding=1, followed by raw tile pixels (w*h*2)
This is fully spec-compliant (decoders must handle all forms) and captures the
dominant win for flat-color UI.
```

---

### Task 1: rfb_protocol — types, builders, parsers (pure, TDD)

**Files:**
- Create: `include/rfb_protocol.h`
- Create: `src/network/rfb_protocol.cpp`
- Test: `tests/unit/test_rfb_protocol.cpp`

- [ ] **Step 1: Write failing tests** — golden-byte tests, no fixture needed (pure functions on `std::vector<uint8_t>`):

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rfb_protocol.h"
#include "../catch_amalgamated.hpp"
#include <cstring>

using namespace helix::rfb;

TEST_CASE("version string is RFB 3.8", "[rfb][quick]") {
    REQUIRE(std::memcmp(kVersionString, "RFB 003.008\n", 12) == 0);
}

TEST_CASE("server_init builds correct bytes", "[rfb][quick]") {
    auto buf = build_server_init(800, 480, "HelixScreen");
    // u16 width BE
    REQUIRE(buf[0] == 0x03); REQUIRE(buf[1] == 0x20);  // 800
    REQUIRE(buf[2] == 0x01); REQUIRE(buf[3] == 0xE0);  // 480
    // pixel format: bpp16 depth16 le truecolour
    REQUIRE(buf[4] == 16); REQUIRE(buf[5] == 16);
    REQUIRE(buf[6] == 0);  REQUIRE(buf[7] == 1);
    REQUIRE(buf[8] == 0);  REQUIRE(buf[9] == 31);      // red_max 31 BE
    REQUIRE(buf[10] == 0); REQUIRE(buf[11] == 63);     // green_max
    REQUIRE(buf[12] == 0); REQUIRE(buf[13] == 31);     // blue_max
    REQUIRE(buf[14] == 11); REQUIRE(buf[15] == 5); REQUIRE(buf[16] == 0);
    // name_len at offset 20 (after 3 pad bytes)
    REQUIRE(buf[20] == 0); REQUIRE(buf[21] == 0); REQUIRE(buf[22] == 0);
    REQUIRE(buf[23] == 11);
    REQUIRE(std::memcmp(&buf[24], "HelixScreen", 11) == 0);
    REQUIRE(buf.size() == 24 + 11);
}

TEST_CASE("parse PointerEvent", "[rfb][quick]") {
    // type 5, mask 1 (pressed), x=100, y=200
    const uint8_t bytes[] = {5, 1, 0x00, 0x64, 0x00, 0xC8};
    ClientMessage msg{};
    size_t consumed = 0;
    REQUIRE(parse_client_message(bytes, sizeof(bytes), msg, consumed) == ParseResult::Ok);
    REQUIRE(consumed == 6);
    REQUIRE(msg.type == ClientMessageType::PointerEvent);
    REQUIRE(msg.pointer.button_mask == 1);
    REQUIRE(msg.pointer.x == 100);
    REQUIRE(msg.pointer.y == 200);
}

TEST_CASE("parse FramebufferUpdateRequest", "[rfb][quick]") {
    const uint8_t bytes[] = {3, 1, 0, 0, 0, 0, 0x03, 0x20, 0x01, 0xE0};
    ClientMessage msg{};
    size_t consumed = 0;
    REQUIRE(parse_client_message(bytes, sizeof(bytes), msg, consumed) == ParseResult::Ok);
    REQUIRE(consumed == 10);
    REQUIRE(msg.type == ClientMessageType::FramebufferUpdateRequest);
    REQUIRE(msg.fb_request.incremental == 1);
    REQUIRE(msg.fb_request.w == 800);
    REQUIRE(msg.fb_request.h == 480);
}

TEST_CASE("parse needs more data on short buffer", "[rfb][quick]") {
    const uint8_t bytes[] = {5, 1, 0x00};  // truncated PointerEvent
    ClientMessage msg{};
    size_t consumed = 0;
    REQUIRE(parse_client_message(bytes, sizeof(bytes), msg, consumed) ==
            ParseResult::NeedMoreData);
    REQUIRE(consumed == 0);
}

TEST_CASE("parse skips KeyEvent and ClientCutText", "[rfb][quick]") {
    const uint8_t key[] = {4, 1, 0, 0, 0, 0, 0, 0x61};  // 'a' down
    ClientMessage msg{};
    size_t consumed = 0;
    REQUIRE(parse_client_message(key, sizeof(key), msg, consumed) == ParseResult::Ok);
    REQUIRE(consumed == 8);
    REQUIRE(msg.type == ClientMessageType::Ignored);

    const uint8_t cut[] = {6, 0, 0, 0, 0, 0, 0, 2, 'h', 'i'};
    REQUIRE(parse_client_message(cut, sizeof(cut), msg, consumed) == ParseResult::Ok);
    REQUIRE(consumed == 10);
    REQUIRE(msg.type == ClientMessageType::Ignored);
}

TEST_CASE("parse SetEncodings records hextile support", "[rfb][quick]") {
    const uint8_t bytes[] = {2, 0, 0, 2, 0, 0, 0, 5, 0, 0, 0, 0};  // [Hextile, Raw]
    ClientMessage msg{};
    size_t consumed = 0;
    REQUIRE(parse_client_message(bytes, sizeof(bytes), msg, consumed) == ParseResult::Ok);
    REQUIRE(consumed == 12);
    REQUIRE(msg.type == ClientMessageType::SetEncodings);
    REQUIRE(msg.encodings.supports_hextile == true);
}
```

- [ ] **Step 2: Run to verify compile failure** — `make test` → `rfb_protocol.h` not found.
- [ ] **Step 3: Implement.** `include/rfb_protocol.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace helix::rfb {

inline constexpr char kVersionString[] = "RFB 003.008\n"; // 12 bytes, no NUL on wire
inline constexpr int32_t kEncodingRaw = 0;
inline constexpr int32_t kEncodingHextile = 5;

enum class ClientMessageType { SetPixelFormat, SetEncodings,
                               FramebufferUpdateRequest, PointerEvent, Ignored };
enum class ParseResult { Ok, NeedMoreData, ProtocolError };

struct PointerEvent { uint8_t button_mask; uint16_t x; uint16_t y; };
struct FbUpdateRequest { uint8_t incremental; uint16_t x, y, w, h; };
struct EncodingsInfo { bool supports_hextile; };

struct ClientMessage {
    ClientMessageType type;
    PointerEvent pointer;
    FbUpdateRequest fb_request;
    EncodingsInfo encodings;
};

/// Parse one client message from buf. On Ok, `consumed` = bytes eaten.
/// NeedMoreData leaves consumed=0 (caller buffers and retries).
ParseResult parse_client_message(const uint8_t* buf, size_t len,
                                 ClientMessage& out, size_t& consumed);

/// ServerInit body (width, height, RGB565-LE pixel format, name).
std::vector<uint8_t> build_server_init(uint16_t width, uint16_t height,
                                       const std::string& name);

/// Security handshake fragments (3.8, type None).
std::vector<uint8_t> build_security_types();   // {1, 1}
std::vector<uint8_t> build_security_result_ok(); // u32 0

/// FramebufferUpdate header for `n_rects` rectangles.
std::vector<uint8_t> build_fb_update_header(uint16_t n_rects);

/// Rectangle header (x, y, w, h, encoding) appended to `out`.
void append_rect_header(std::vector<uint8_t>& out, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h, int32_t encoding);

} // namespace helix::rfb
```

`src/network/rfb_protocol.cpp` — big-endian helpers + straightforward switch over message type. Complete the parser exactly per the wire reference table above (lengths: SetPixelFormat=20, SetEncodings=4+4n, FbUpdateRequest=10, KeyEvent=8, PointerEvent=6, ClientCutText=8+len). Unknown type → `ProtocolError`. Use only `<cstdint>/<vector>` — no LVGL, no libhv.

- [ ] **Step 4: Add `src/network/rfb_protocol.cpp` to the build.** Check how other `src/network/*.cpp` files are compiled (`grep -rn "src/network" Makefile mk/*.mk | head`) — follow the same mechanism (explicit list or glob).
- [ ] **Step 5: `make test && ./build/bin/helix-tests "[rfb]"` → PASS. Commit:**

```bash
git add include/rfb_protocol.h src/network/rfb_protocol.cpp tests/unit/test_rfb_protocol.cpp
git commit -m "feat(remote-display): RFB 3.8 protocol framing + parsers"
```

---

### Task 2: Pixel conversion + Raw/Hextile encoders (pure, TDD)

**Files:**
- Create: `include/rfb_encoder.h`, `src/network/rfb_encoder.cpp`
- Test: `tests/unit/test_rfb_encoder.cpp`

- [ ] **Step 1: Failing tests:**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rfb_encoder.h"
#include "../catch_amalgamated.hpp"

using namespace helix::rfb;

TEST_CASE("argb8888 to rgb565", "[rfb][encoder][quick]") {
    REQUIRE(argb8888_to_rgb565(0xFFFF0000) == 0xF800); // red
    REQUIRE(argb8888_to_rgb565(0xFF00FF00) == 0x07E0); // green
    REQUIRE(argb8888_to_rgb565(0xFF0000FF) == 0x001F); // blue
    REQUIRE(argb8888_to_rgb565(0xFFFFFFFF) == 0xFFFF);
    REQUIRE(argb8888_to_rgb565(0xFF000000) == 0x0000);
}

TEST_CASE("raw encoding of a 2x2 rgb565 region", "[rfb][encoder][quick]") {
    // Source framebuffer 4x4 RGB565, encode the 2x2 at (1,1)
    uint16_t fb[16] = {};
    fb[1 * 4 + 1] = 0xF800; fb[1 * 4 + 2] = 0x07E0;
    fb[2 * 4 + 1] = 0x001F; fb[2 * 4 + 2] = 0xFFFF;
    std::vector<uint8_t> out;
    encode_raw(fb, 4, 1, 1, 2, 2, out);
    REQUIRE(out.size() == 8); // 4 px * 2 bytes, little-endian
    REQUIRE(out[0] == 0x00); REQUIRE(out[1] == 0xF8);
    REQUIRE(out[2] == 0xE0); REQUIRE(out[3] == 0x07);
    REQUIRE(out[4] == 0x1F); REQUIRE(out[5] == 0x00);
    REQUIRE(out[6] == 0xFF); REQUIRE(out[7] == 0xFF);
}

TEST_CASE("hextile solid tile emits background-specified", "[rfb][encoder][quick]") {
    // 16x16 solid red region
    std::vector<uint16_t> fb(16 * 16, 0xF800);
    std::vector<uint8_t> out;
    encode_hextile(fb.data(), 16, 0, 0, 16, 16, out);
    REQUIRE(out.size() == 3);          // subencoding + 1 bg pixel
    REQUIRE(out[0] == 2);              // BackgroundSpecified
    REQUIRE(out[1] == 0x00); REQUIRE(out[2] == 0xF8);
}

TEST_CASE("hextile mixed tile falls back to raw", "[rfb][encoder][quick]") {
    std::vector<uint16_t> fb(16 * 16, 0xF800);
    fb[5] = 0x07E0; // one differing pixel
    std::vector<uint8_t> out;
    encode_hextile(fb.data(), 16, 0, 0, 16, 16, out);
    REQUIRE(out.size() == 1 + 16 * 16 * 2);
    REQUIRE(out[0] == 1);              // Raw subencoding
}

TEST_CASE("hextile handles non-multiple-of-16 edges", "[rfb][encoder][quick]") {
    // 20x20 solid region → tiles: 16x16, 4x16, 16x4, 4x4 (all solid)
    std::vector<uint16_t> fb(20 * 20, 0x001F);
    std::vector<uint8_t> out;
    encode_hextile(fb.data(), 20, 0, 0, 20, 20, out);
    REQUIRE(out.size() == 4 * 3);      // four solid tiles, 3 bytes each
}

TEST_CASE("hextile compresses solid regions well", "[rfb][encoder]") {
    std::vector<uint16_t> fb(800 * 480, 0x2104); // typical flat bg
    std::vector<uint8_t> out;
    encode_hextile(fb.data(), 800, 0, 0, 800, 480, out);
    REQUIRE(out.size() < 800 * 480 * 2 / 50);  // >50x on solid
}
```

- [ ] **Step 2: verify fail.** `make test` → header missing.
- [ ] **Step 3: Implement** `include/rfb_encoder.h` / `src/network/rfb_encoder.cpp`:

```cpp
// include/rfb_encoder.h
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace helix::rfb {

inline uint16_t argb8888_to_rgb565(uint32_t argb) {
    return static_cast<uint16_t>(((argb >> 8) & 0xF800) |  // r: bits 23..19 → 15..11
                                 ((argb >> 5) & 0x07E0) |  // g: bits 15..10 → 10..5
                                 ((argb >> 3) & 0x001F));  // b: bits 7..3  → 4..0
}

/// Append little-endian RGB565 raw pixels for the (x,y,w,h) sub-region of a
/// framebuffer with `fb_stride_px` pixels per row.
void encode_raw(const uint16_t* fb, int fb_stride_px, int x, int y, int w, int h,
                std::vector<uint8_t>& out);

/// Hextile-encode the sub-region (16x16 tiles; solid tiles → subencoding 2 +
/// bg pixel; mixed tiles → subencoding 1 + raw pixels).
void encode_hextile(const uint16_t* fb, int fb_stride_px, int x, int y, int w, int h,
                    std::vector<uint8_t>& out);

} // namespace helix::rfb
```

The `.cpp` is two nested loops; for hextile iterate `ty` then `tx` in steps of 16, clamp tile w/h at edges, scan tile for uniformity, emit accordingly. Pixel bytes little-endian (low byte first).

- [ ] **Step 4:** `make test && ./build/bin/helix-tests "[encoder]"` → PASS. **Commit:**

```bash
git add include/rfb_encoder.h src/network/rfb_encoder.cpp tests/unit/test_rfb_encoder.cpp
git commit -m "feat(remote-display): RGB565 conversion + Raw/Hextile encoders"
```

---

### Task 3: RfbServer — connection state machine + shadow framebuffer

**Files:**
- Create: `include/rfb_server.h`, `src/network/rfb_server.cpp`
- Test: `tests/unit/test_rfb_server.cpp` (state machine via injected byte streams — no real sockets)

**Design:** `RfbServer` separates the protocol session (`RfbSession` — feed bytes in, get bytes out; testable without sockets) from the libhv transport glue.

- [ ] **Step 1: Failing tests for `RfbSession`** (handshake sequence, update-request → encoded rect, pointer event surfacing):

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rfb_server.h"
#include "../catch_amalgamated.hpp"
#include <cstring>

using namespace helix::rfb;

static std::vector<uint8_t> drain(RfbSession& s) {
    std::vector<uint8_t> out;
    s.take_pending_output(out);
    return out;
}

TEST_CASE("session handshake to ready", "[rfb][session]") {
    SharedFramebuffer fb(64, 64);
    RfbSession s(&fb, "HelixScreen");

    auto greeting = drain(s);
    REQUIRE(greeting.size() == 12);
    REQUIRE(std::memcmp(greeting.data(), "RFB 003.008\n", 12) == 0);

    s.feed(reinterpret_cast<const uint8_t*>("RFB 003.008\n"), 12);
    auto sec = drain(s);                       // security types {1, 1}
    REQUIRE(sec == std::vector<uint8_t>{1, 1});

    const uint8_t choose_none[] = {1};
    s.feed(choose_none, 1);
    auto result = drain(s);                    // SecurityResult OK
    REQUIRE(result == std::vector<uint8_t>{0, 0, 0, 0});

    const uint8_t client_init[] = {1};
    s.feed(client_init, 1);
    auto server_init = drain(s);
    REQUIRE(server_init.size() >= 24);
    REQUIRE(s.state() == RfbSession::State::Ready);
}

TEST_CASE("update request returns full-frame rect when dirty", "[rfb][session]") {
    SharedFramebuffer fb(64, 64);
    fb.fill(0x2104);
    fb.mark_dirty(0, 0, 64, 64);
    RfbSession s(&fb, "HelixScreen");
    s.force_ready_for_test();

    // incremental=0 full request
    const uint8_t req[] = {3, 0, 0, 0, 0, 0, 0, 64, 0, 64};
    s.feed(req, sizeof(req));
    auto out = drain(s);
    REQUIRE(out.size() >= 4 + 12);             // header + ≥1 rect header
    REQUIRE(out[0] == 0);                      // FramebufferUpdate
    uint16_t n_rects = static_cast<uint16_t>((out[2] << 8) | out[3]);
    REQUIRE(n_rects >= 1);
}

TEST_CASE("incremental request with no dirty region defers", "[rfb][session]") {
    SharedFramebuffer fb(64, 64);
    RfbSession s(&fb, "HelixScreen");
    s.force_ready_for_test();
    fb.clear_dirty();

    const uint8_t req[] = {3, 1, 0, 0, 0, 0, 0, 64, 0, 64};
    s.feed(req, sizeof(req));
    REQUIRE(drain(s).empty());                 // nothing to send yet
    REQUIRE(s.has_pending_update_request());

    fb.fill(0xF800);
    fb.mark_dirty(0, 0, 8, 8);
    s.notify_dirty();                          // server pump calls this after flush
    auto out = drain(s);
    REQUIRE_FALSE(out.empty());
    REQUIRE_FALSE(s.has_pending_update_request());
}

TEST_CASE("pointer events surface through the queue", "[rfb][session]") {
    SharedFramebuffer fb(64, 64);
    RfbSession s(&fb, "HelixScreen");
    s.force_ready_for_test();

    const uint8_t press[] = {5, 1, 0, 10, 0, 20};
    const uint8_t release[] = {5, 0, 0, 10, 0, 20};
    s.feed(press, sizeof(press));
    s.feed(release, sizeof(release));

    PointerEvent ev{};
    REQUIRE(s.pop_pointer_event(ev));
    REQUIRE(ev.button_mask == 1);
    REQUIRE(ev.x == 10);
    REQUIRE(ev.y == 20);
    REQUIRE(s.pop_pointer_event(ev));
    REQUIRE(ev.button_mask == 0);
    REQUIRE_FALSE(s.pop_pointer_event(ev));
}
```

- [ ] **Step 2: verify fail**, then **Step 3: implement** `include/rfb_server.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rfb_protocol.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hv { class TcpServer; }

namespace helix::rfb {

/// Shadow framebuffer shared between the LVGL flush callback (main thread,
/// writer) and the RFB session encoder (libhv thread, reader). RGB565.
/// All access internally mutex-guarded; no LVGL types cross this boundary.
class SharedFramebuffer {
  public:
    SharedFramebuffer(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }

    /// Copy a dirty rect of RGB565 pixels in (stride = rect width) and grow
    /// the dirty bounding box. Called from the LVGL flush cb (main thread).
    void write_rect(int x, int y, int w, int h, const uint16_t* pixels);

    /// Encode-side: snapshot + clear the dirty bounding box. Returns false if
    /// nothing dirty. Encoder then reads pixels under the same lock via
    /// with_pixels().
    bool take_dirty(int& x, int& y, int& w, int& h);
    void with_pixels(const std::function<void(const uint16_t* fb, int stride_px)>& fn) const;

    // Test helpers
    void fill(uint16_t rgb565);
    void mark_dirty(int x, int y, int w, int h);
    void clear_dirty();

  private:
    int width_, height_;
    std::vector<uint16_t> pixels_;
    mutable std::mutex mutex_;
    bool dirty_ = false;
    int dx1_ = 0, dy1_ = 0, dx2_ = 0, dy2_ = 0; // dirty bounding box
};

/// One client's protocol state machine. Transport-agnostic: feed() bytes in,
/// take_pending_output() bytes out. Owned by RfbServer; runs on the libhv
/// event-loop thread (single-threaded per connection).
class RfbSession {
  public:
    enum class State { AwaitVersion, AwaitSecurityChoice, AwaitClientInit, Ready, Failed };

    RfbSession(SharedFramebuffer* fb, std::string server_name);

    void feed(const uint8_t* data, size_t len);
    void take_pending_output(std::vector<uint8_t>& out);
    State state() const { return state_; }

    /// FramebufferUpdateRequest bookkeeping (client-pull flow control).
    bool has_pending_update_request() const { return pending_request_; }
    /// Called when new dirty content exists; emits an update if a request is
    /// pending.
    void notify_dirty();

    /// Pointer events parsed from the client (drained by the lv_indev read
    /// callback on the main thread — internally locked).
    bool pop_pointer_event(PointerEvent& ev);

    void force_ready_for_test() { state_ = State::Ready; }

  private:
    void handle_message(const ClientMessage& msg);
    void emit_update();                        // encode dirty box → output buffer

    SharedFramebuffer* fb_;
    std::string server_name_;
    State state_ = State::AwaitVersion;
    std::vector<uint8_t> inbuf_;
    std::vector<uint8_t> outbuf_;
    bool pending_request_ = false;
    bool client_supports_hextile_ = false;     // until SetEncodings says so → Raw
    std::mutex pointer_mutex_;
    std::deque<PointerEvent> pointer_queue_;
};

/// TCP transport: binds the port, owns ≤1 RfbSession, pumps session output to
/// the socket. All libhv callbacks stay on libhv threads and touch only
/// RfbSession/SharedFramebuffer (never LVGL).
class RfbServer {
  public:
    RfbServer(SharedFramebuffer* fb, int port, std::string server_name);
    ~RfbServer();
    bool start();
    void stop();
    bool has_client() const;

    /// Main thread, after each LVGL flush: wake the session if a client is
    /// waiting on a FramebufferUpdateRequest.
    void notify_dirty();

    /// Main thread (lv_indev read cb): drain one pointer event.
    bool pop_pointer_event(PointerEvent& ev);

  private:
    SharedFramebuffer* fb_;
    int port_;
    std::string server_name_;
    std::unique_ptr<hv::TcpServer> tcp_;
    std::mutex session_mutex_;
    std::shared_ptr<RfbSession> session_;      // single client v1
};

} // namespace helix::rfb
```

`src/network/rfb_server.cpp` implementation notes (complete in execution):
- `RfbSession::feed`: append to `inbuf_`; in handshake states consume fixed-size chunks per the wire table; in `Ready` loop `parse_client_message` until `NeedMoreData`, erasing consumed bytes.
- `emit_update()`: `take_dirty` → clamp to fb bounds → `build_fb_update_header(1)` + `append_rect_header(...)` + `encode_hextile` (or `encode_raw` if client never sent SetEncodings listing hextile) under `with_pixels`.
- `RfbServer` libhv glue (follow `hv::TcpServer` API from `lib/libhv/include/hv/TcpServer.h`): `createsocket(port_)`, `setMaxConnectionNum(2)` (second connect → close politely), `onConnection` creates/destroys the session and pushes greeting bytes via `channel->write`; `onMessage` calls `session_->feed` then writes `take_pending_output`. Store the channel; `notify_dirty()` posts to the loop (`tcp_->loop()->runInLoop(...)`) so encoding stays on the libhv thread.
- Per project threading rules: these callbacks never touch LVGL/subjects — only `RfbSession`/`SharedFramebuffer` (mutex-guarded plain data), so `queue_update` is NOT needed here. Document this in a file-top comment; future maintainers WILL be tempted.

- [ ] **Step 4:** `make test && ./build/bin/helix-tests "[session]"` → PASS. **Commit:**

```bash
git add include/rfb_server.h src/network/rfb_server.cpp tests/unit/test_rfb_server.cpp
git commit -m "feat(remote-display): RfbServer session state machine + shared framebuffer"
```

---

### Task 4: DisplayBackendRemote + enum/factory/build wiring

**Files:**
- Create: `include/display_backend_remote.h`, `src/api/display_backend_remote.cpp`
- Modify: `include/display_backend.h` (enum :28-33 — add `REMOTE`; `display_backend_type_to_string` :50-63)
- Modify: `src/api/display_backend.cpp` (factory `create()` ~:115-140 — add REMOTE case; `create_auto()` env parsing ~:201-240 — accept `"remote"`)
- Modify: `mk/display-lib.mk` (add `src/api/display_backend_remote.cpp` + `src/network/rfb_*.cpp` for Linux AND Darwin — remote works on both; define `HELIX_DISPLAY_REMOTE` in `DISPLAY_CXXFLAGS`)

Backend skeleton (`display_backend_remote.h`):

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef HELIX_DISPLAY_REMOTE

#include "display_backend.h"
#include "rfb_server.h"

#include <memory>
#include <vector>

class DisplayBackendRemote : public DisplayBackend {
  public:
    DisplayBackendRemote() = default;
    ~DisplayBackendRemote() override;

    lv_display_t* create_display(int width, int height) override;
    lv_indev_t* create_input_pointer() override;
    DisplayBackendType type() const override { return DisplayBackendType::REMOTE; }
    const char* name() const override { return "Remote/RFB"; }
    bool is_available() const override { return true; }
    DetectedResolution detect_resolution() const override; // HELIX_DISPLAY_REMOTE_SIZE or 800x480

  private:
    static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
    static void pointer_read_cb(lv_indev_t* indev, lv_indev_data_t* data);

    lv_display_t* display_ = nullptr;
    lv_indev_t* pointer_ = nullptr;
    std::unique_ptr<helix::rfb::SharedFramebuffer> shadow_fb_;
    std::unique_ptr<helix::rfb::RfbServer> server_;
    std::vector<uint8_t> draw_buf_;            // LVGL partial render buffer
    std::vector<uint16_t> convert_buf_;        // per-flush RGB565 staging
    helix::rfb::PointerEvent last_pointer_{};  // sticky state for lv_indev
};

#endif // HELIX_DISPLAY_REMOTE
```

Key implementation points for `display_backend_remote.cpp`:
- `create_display`: resolution from `detect_resolution()` (parse `HELIX_DISPLAY_REMOTE_SIZE` "WxH", default 800x480); `lv_display_create(w, h)`; partial draw buffer = `w * 60 rows * 4` bytes (host ARGB8888); `lv_display_set_buffers(display_, draw_buf_.data(), nullptr, draw_buf_.size(), LV_DISPLAY_RENDER_MODE_PARTIAL)`; `lv_display_set_user_data(display_, this)`; `lv_display_set_flush_cb(display_, flush_cb)`; construct `shadow_fb_` and `server_` (port from `HELIX_RFB_PORT`, default 5900), `server_->start()`.
- `flush_cb` (main thread): convert the dirty area's pixels to RGB565 (`argb8888_to_rgb565` per pixel when `LV_COLOR_DEPTH==32`; straight memcpy rows when 16 — `#if LV_COLOR_DEPTH == 32` both paths), `shadow_fb_->write_rect(...)`, `server_->notify_dirty()`, `lv_display_flush_ready(disp)`.
- `pointer_read_cb` (main thread): `lv_indev_create` + `lv_indev_set_type(LV_INDEV_TYPE_POINTER)` + `lv_indev_set_read_cb` in `create_input_pointer`; the read cb pops one event per call (`server_->pop_pointer_event`); if none, repeat `last_pointer_` state (LVGL polls — sticky last state is the standard pattern, see `touch_calibration_wrapper.h` for read-cb chaining precedent); map `button_mask & 1` → `LV_INDEV_STATE_PRESSED/RELEASED`, clamp x/y to display bounds.
- Factory: in `create()` add `case DisplayBackendType::REMOTE: return std::make_unique<DisplayBackendRemote>();` under `#ifdef HELIX_DISPLAY_REMOTE`; in `create_auto()` env parsing add `"remote"` → REMOTE. REMOTE is never auto-selected — explicit only.

- [ ] **Step 1:** Enum + string + factory changes; build (`make -j`) stays green with backend unimplemented (`#ifdef` not yet defined).
- [ ] **Step 2:** mk/display-lib.mk additions + `-DHELIX_DISPLAY_REMOTE` for desktop/Linux builds; implement the backend; `make -j` green.
- [ ] **Step 3: Smoke test headless with a real VNC viewer:**

```bash
HELIX_DISPLAY_BACKEND=remote HELIX_DISPLAY_REMOTE_SIZE=800x480 ./build/bin/helix-screen --test -vv
# In another terminal: open a VNC viewer to localhost:5900
# (macOS: open vnc://localhost:5900 ; or any RFB 3.8 viewer)
```

Expected: viewer shows the HelixScreen home panel; clicking in the viewer presses buttons. This step requires a human eyeball — report findings, capture a screenshot of the viewer.

- [ ] **Step 4: Commit:**

```bash
git add include/display_backend_remote.h src/api/display_backend_remote.cpp include/display_backend.h src/api/display_backend.cpp mk/display-lib.mk
git commit -m "feat(remote-display): DisplayBackendRemote RFB backend (HELIX_DISPLAY_BACKEND=remote)"
```

---

### Task 5: Python integration probe (automated, no GUI viewer needed)

**Files:**
- Create: `tests/python/rfb_probe.py`

A ~100-line stdlib-only client: connect, full handshake (assert version/security/ServerInit fields), `SetEncodings [Hextile, Raw]`, full `FramebufferUpdateRequest`, assert ≥1 rect arrives and decodes to the right pixel count, send a `PointerEvent` press/release at a known button location, request again, assert a new update arrives (screen changed). Exit 0/1.

- [ ] **Step 1:** Write the probe (argparse: `--host localhost --port 5900 --width 800 --height 480`). Follow existing `tests/python/` conventions (check for a runner/README first).
- [ ] **Step 2:** Manual loop: start `HELIX_DISPLAY_BACKEND=remote ./build/bin/helix-screen --test` in background, run `python3 tests/python/rfb_probe.py`, expect exit 0. Document the two-command recipe at the top of the probe file. (CI wiring is a follow-up — note it, don't build it.)
- [ ] **Step 3: Commit:**

```bash
git add tests/python/rfb_probe.py
git commit -m "test(remote-display): RFB handshake/update/input integration probe"
```

---

### Task 6: Remote-mode polish — animations off, scroll buttons Auto, mDNS advertise

- [ ] **Step 1: Animations off in remote mode (non-persistent).** Find the animations apply path (`grep -n "handle_animations_changed\|animations" src/ui/ui_panel_settings.cpp src/system/display_settings_manager.cpp`). In `Application` init, after backend creation: if `backend()->type() == DisplayBackendType::REMOTE`, apply animations-disabled at runtime WITHOUT persisting (call the apply function, not the setting setter). Add a log line stating why.
- [ ] **Step 2: Ghost scroll buttons Auto mode.** ⚠️ **STALE - do not execute as written.** This step is built on the Phase 1a ghost-scroll design, which never shipped. `src/ui/ui_scroll_buttons.cpp`, its `remote_backend_active()` stub, and `tests/unit/test_ui_scroll_buttons.cpp` are all zero-hit greps - the file never existed. What shipped is `src/ui/page_scroll_auto_inject.cpp` + `src/ui/page_scroll_controller.cpp` behind a **boolean** `DisplaySettingsManager::get/set_page_scroll_buttons()`, so there is no "Auto mode" to teach about the remote backend; see `docs/devel/PAGE_SCROLL_BUTTONS.md`. If remote-mode-specific behavior is still wanted, re-scope it against that code first. Original text: In `src/ui/ui_scroll_buttons.cpp`, replace the `remote_backend_active()` stub (returns false, planted by the Phase 1a plan) with a real check via `DisplayManager::instance()->backend()->type() == DisplayBackendType::REMOTE` (guard nulls; see `include/display_manager.h:134-139`). Extend `tests/unit/test_ui_scroll_buttons.cpp` only if a test seam exists without DisplayManager bootstrapping — otherwise the pure-function tests still cover the policy and this stays a 3-line integration.
- [ ] **Step 3: mDNS advertising.** New `src/network/rfb_advertiser.{h,cpp}` using the embedded `mdns/mdns.h` (client precedent: `src/network/mdns_discovery.cpp` — PIMPL + background thread + `queue_update` for callbacks). Advertise `_helixscreen-rfb._tcp` with the RFB port + TXT records `version`, `resolution`. Started/stopped by `RfbServer::start()/stop()`. If the embedded mdns.h lacks responder support (verify: `grep -n "mdns_announce\|MDNS_RECORDTYPE_PTR" lib/mdns/mdns.h src/network/mdns_discovery.cpp`), descope to a follow-up issue and note it — do NOT hand-roll an mDNS responder in this plan.
- [ ] **Step 4:** `make -j && make test-run` green; re-run Task 4 Step 3 viewer smoke test confirming animations are off (panel switches snap instead of slide). **Commit:**

```bash
git add src/application/application.cpp src/ui/ui_scroll_buttons.cpp src/network/rfb_advertiser.h src/network/rfb_advertiser.cpp src/network/rfb_server.cpp mk/display-lib.mk
git diff --staged --stat   # adjust list to what actually changed
git commit -m "feat(remote-display): remote-mode polish - anim off, scroll buttons auto, mdns advertise"
```

---

### Task 7: Docs + final verification

- [ ] **Step 1:** Add `docs/devel/REMOTE_DISPLAY.md`: how to run (`HELIX_DISPLAY_BACKEND=remote`, `HELIX_DISPLAY_REMOTE_SIZE`, `HELIX_RFB_PORT`), protocol subset table (copy from this plan), threading model diagram (flush/main vs encode/libhv vs indev/main), known limits (single client, no auth, LAN-only, animations forced off). Add the env vars to `docs/devel/ENVIRONMENT_VARIABLES.md`.
- [ ] **Step 2:** Full pass: `make -j && make test-run && ./build/bin/helix-tests "[rfb]"` — all green, output captured.
- [ ] **Step 3:** Update the spec (`docs/devel/plans/2026-06-10-esp32-display-device-design.md`): mark the ServerInit-name identity deviation in the Open Items section.
- [ ] **Step 4:** Commit docs; verify each commit in the series contains only its files (`git log --stat`).

## Out of Scope (YAGNI)

- ZRLE encoding (zlib is linked and ready; add only if Hextile proves insufficient on real WiFi).
- Multi-client / spectator mode; auth; TLS.
- Resolution auto-match from client (v2 — see spec open items).
- CI wiring for the Python probe.
- KeyEvent → LVGL keyboard injection.
