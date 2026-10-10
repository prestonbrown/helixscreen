# ESP32 Display Device — Design

**Date:** 2026-06-10
**Status:** Approved (brainstorm complete)
**Scope:** Sequenced program — remote display terminal first, native ESP32 firmware contingent on a feasibility audit.

## Context & Motivation

Two converging use cases:

1. **Cheap primary screen** — printers with no usable display (or remote-only stock panels, e.g. QIDI Plus 4 / TJC serial screens) get a ~$15–30 ESP32-S3 + touch panel running HelixScreen's UI, driven by the printer's existing Linux host.
2. **Standalone product (BTT interest)** — BigTreeTech is interested in a HelixScreen-class UI running entirely on an ESP32-S3-class MCU, talking to Moonraker over WiFi, with no display SBC at all. Klipper/Moonraker still requires a Linux host somewhere; "replace the SBC" means the *display* no longer needs its own.

Hardware target: **ESP32-S3 class** — 240MHz dual Xtensa, 512KB SRAM, 8–16MB octal PSRAM, 16MB flash typical, 2.4GHz WiFi (~10–20Mbps practical TCP).

Functional ambition for the native variant: **near-parity** with HelixScreen (cuts only where hardware forces it — camera, 3D gcode viewer). Explicitly held as a *hypothesis* until the Phase-0 audit reports, not a commitment.

Code-sharing strategy for the native variant: **port the real codebase** — ESP-IDF becomes another cross target of this repo behind a platform abstraction layer. One UI, one truth. Fallback if the audit fails: shared XML engine + slim new app layer.

## Decision: Sequenced Architecture (C)

The two modes share all ESP32 plumbing (board bring-up, esp_lcd + touch, WiFi provisioning, OTA, mDNS). Sequence:

1. **Phase 1 — Remote display terminal** ships first. Low risk (~1.5–2.5 months to polished v1), works with every Klipper printer that has a host, sellable early.
2. **Phase 0 — Native feasibility audit** runs in parallel (timeboxed 2–4 weeks). Replaces faith with link-size and RAM numbers.
3. **Phase 2 — Native firmware** proceeds per the audit's decision gate. The terminal firmware's provisioning/OTA/display layers carry over wholesale.

Rejected alternatives:
- *Remote-only:* UX ceiling is "very good VNC"; doesn't serve the BTT standalone product.
- *Native-first:* months of medium-high risk before anything ships; near-parity-on-S3 unproven.
- *Sister firmware repo / from-scratch slim UI:* maintaining two divergent near-parity codebases is a treadmill.

---

## Phase 1a: Ghost Scroll Buttons (independently shippable)

A HelixScreen UI feature with zero dependency on protocol or firmware — ships to existing users first; remote mode inherits it.

- Reusable overlay: two semi-transparent up/down buttons floating over the right edge of scrollable views (file list, settings, macros, console — the major scroll surfaces). Ignore layout, align right. Tap scrolls ~80% of viewport height.
- Smart visibility: hidden entirely when content fits; up button fades at top of scroll range, down at bottom.
- Styled with design tokens. Press feedback via **opacity, not transform_scale** — transparent containers don't scale-feedback children (L078).
- **Setting:** `Scroll buttons: Auto / On / Off`, default **Auto** = on when remote backend active, off on local hardware. On = first-class accessibility option for local users (gloves, flaky resistive panels, preference for discrete controls).
- Why it matters for remote: a flick-scroll is ~50Hz of pointer events plus continuous large dirty areas — the protocol's pathological case. A scroll-button tap is one event, one repaint.

## Phase 1b: Host — `DisplayBackendRemote`

A fourth display backend (`DisplayBackendType::REMOTE`), alongside SDL/FBDEV/DRM, selected via `HELIX_DISPLAY_BACKEND=remote` or config. HelixScreen runs headless, renders normally, serves the screen over an **RFB (VNC) protocol subset**.

- `create_display()`: LVGL in partial render mode; flush callback copies each dirty rect into a host-maintained **shadow framebuffer** (late-joining clients get a full frame) and queues the rect for encoding.
- Wire format **RGB565** regardless of host color depth; encoder converts from ARGB8888 on 32-bit hosts. Halves bandwidth.
- Resolution dictated by host config (`HELIX_DISPLAY_REMOTE_SIZE=800x480`); v1 says "configure host to match panel," v2 may auto-match from client-reported size.
- Server: libhv server-side TCP (in-tree, first server-side use — isolate behind a small `RfbServer` class so it's swappable for raw sockets if libhv disappoints).
- Touch: RFB `PointerEvent`s fed into the lv_indev read callback via a thread-safe queue. Standard background-thread rules apply (WebSocket-callback-shaped territory: no direct subject writes, queue everything).
- Remote mode defaults panel-slide animations off (LVGL anim time 0) to keep dirty areas small.
- v1: single client, LAN-only, security type None (same trust model as Moonraker on LAN).
- Sizing: in the "cheap primary screen" case the headless instance is the *only* instance — no added load on AD5M-class hosts. Remote *in addition to* a local screen = second process, Pi-class hosts only.
- Dev superpower: any desktop VNC viewer connects to a headless HelixScreen — permanent hardware-free dev/debug tool.

### RFB subset (wire contract)

- RFB 3.8 handshake, port 5900, security type None.
- Pixel format negotiated RGB565 little-endian; also advertised as native so dumb viewers work.
- Server→client: `FramebufferUpdate` with **Raw + Hextile** encodings (no new deps; ZRLE later if zlib-grade compression wanted).
- Client→server: `SetPixelFormat`, `SetEncodings`, `FramebufferUpdateRequest`, `PointerEvent`. `KeyEvent` accepted and dropped.
- One HelixScreen extension via a standard pseudo-encoding: **server identity/capability rect** (name, resolution, UI version) so firmware can verify it's talking to HelixScreen and show a useful pairing screen. Desktop viewers ignore unknown pseudo-encodings.
- **Client-pull flow control is the gift:** client sends `FramebufferUpdateRequest` when ready, so a slow WiFi MCU naturally rate-limits the stream. No custom flow control.

### Bandwidth physics (800×480 RGB565, ~15Mbps real WiFi)

- Full frame raw: 768KB ≈ 400ms — unacceptable raw, hence Hextile.
- HelixScreen is flat-color UI: RLE-class compression typically 5–20× → panel transitions 20–80ms; small updates (temps, progress) in the noise.
- Touch round-trip adds 30–80ms. Acceptable for status UI; drag-scroll is the weak spot (mitigated by ghost scroll buttons + anim-off).

## Phase 1c: ESP32-S3 Terminal Firmware (`helix-terminal`)

ESP-IDF project at `firmware/helix-terminal/` in this repo (unified docs/CI/releases). Plain C, four modules:

1. **Board layer** — esp_lcd RGB-panel + touch (GT911/FT5x06) behind a board-config table; BTT or community boards are a config entry, not a fork. Dev target: a common 800×480 S3 dev board.
2. **Provisioning** — first boot (or button-hold): SoftAP + captive portal for WiFi credentials and host address; mDNS browse for `_helixscreen-rfb._tcp` (host advertises) so most users never type an IP. Credentials in NVS.
3. **RFB client** — negotiate RGB565, then: request update → decode rects straight into panel framebuffer (Hextile decodes tile-by-tile, no full-frame staging buffer) → send `PointerEvent`s from touch ISR queue. Pointer events sent immediately (not batched with frame requests), move events coalesced to ~50Hz.
4. **OTA** — `esp_https_ota` from a manifest URL, pluggable so BTT can point at their own infra. USB reflash fallback documented.

Failure behavior: WiFi drop / host restart → splash with last-known status, auto-reconnect with backoff. Host side: dropped client stops the encoder; LVGL keeps rendering into the shadow buffer; reconnect resumes with one full-frame update.

v1 firmware scope freeze: renders frames and a splash/provisioning screen. **No local fallback UI** — scope gravity is a named risk.

## Phase 0: Native Feasibility Audit (parallel, timeboxed 2–4 weeks)

Deliverable: written report in `docs/devel/` with go/no-go recommendation and three measurements:

1. **Compile audit** — ESP-IDF skeleton building `lib/helix-xml` unmodified, then a vertical slice of the app core: `PrinterState` + subjects + UpdateQueue + one panel's XML, with stubs behind the existing `IMoonrakerClient`/`IMoonrakerAPI` seams (built for mock drift-protection; now the porting seam) and a `spdlog→esp_log` shim header (fmtlib-core based). Every non-compiling file categorized: trivial shim / needs `#ifdef` / fundamentally Linux-bound.
2. **Link-size** — actual `-Os` size of the slice, extrapolated against a 16MB part's realistic app partition (~6–8MB with OTA A/B). The most likely killer; measured for hundreds of dollars instead of months.
3. **RAM profile** — boot the slice on real S3, render one panel at 800×480, measure SRAM/PSRAM watermarks; test CJK font viability (file-backed glyph loading from LittleFS vs Latin-only v1). Framebuffers ≈1.5MB PSRAM; the multi-MB runtime CJK .bin is the squeeze.

**Decision gates:**
- **Green** → Phase 2 ports the real codebase to S3.
- **Yellow** → Phase 2 proceeds on S3 **with explicit feature gates** (no camera/3D, capped lists, streamed JSON, Latin-first fonts); the audit report documents which gates.
- **Red** → fallback to "shared XML engine + slim new app layer"; communicated to BTT early.

*(Revised 2026-07-13: the original yellow gate included "steer BTT to ESP32-P4-class hardware." That option is off the table — BTT's goal is broader appeal for their existing S3 stock (K-Touch in hand, hardware recon verified in `docs/devel/printer-research/BTT_K_TOUCH_HARDWARE.md`), not a new panel. S3 + 8MB PSRAM + 16MB flash is the fixed target; feature gates are the expected design, not a fallback.)*

**⚑ PHASE 0 RESULT (2026-07-13): 🟡 YELLOW — Phase 2 proceeds on S3 with
explicit feature gates.** Full report + measurements + the gate list:
`docs/devel/ESP32_NATIVE_AUDIT.md`. Highlights: LVGL + helix-xml unmodified,
app core ~90% shim-portable (one real seam: libhv WS/HTTP), full 6-panel
shell renders on the K-Touch, 26-30 FPS realistic / 5 FPS full-screen worst
case, CJK viable via compiled per-tier XIP subsets (~0.9MB flash, zero RAM),
extrapolated product image ~7MB (single-slot fits; OTA A/B needs the
documented diet). **Communicating this result to BTT is Preston's
conversation — the audit report is the input, not the message.**

### Phase 2 porting seams (for the audit to validate)

- Network: ESP impl of `IMoonrakerClient`/`IMoonrakerAPI` over `esp_websocket_client`/`esp_http_client`; libhv stays home.
- UI: `lib/helix-xml` (LVGL-native C) compiles as-is; `ui_xml/` + design tokens on a LittleFS partition, runtime-loaded exactly like today.
- Threading: ESP-IDF pthreads; subjects/observers/UpdateQueue patterns portable. HttpExecutor pools shrink to 1+1 workers, small stacks.
- Logging: shim header mapping `spdlog::` onto fmtlib-core + `esp_log`.
- Settings: NVS or LittleFS JSON behind `SettingsManager`.
- Stays home: camera, 3D gcode viewer, BlueZ Bluetooth, wpa_cli WiFi (replaced by esp_wifi backend behind the same settings UI), debug-bundle heavy paths.
- Display: `DisplayBackendEspLcd` implementing the existing `DisplayBackend` interface.
- Rendering perf is the well-trodden part: 800×480 LVGL on S3 with octal PSRAM + DMA ≈ 25–35fps partial updates; flat aesthetic is the favorable case. Known gotcha: PSRAM bandwidth contention with WiFi.

## Testing Strategy (Phase 1)

- **Protocol/encoder:** pure unit tests in the existing Catch2 suite — Hextile encode/decode round-trips, dirty-rect coalescing, RFB framing vs golden byte sequences. No hardware, CI-able.
- **Integration without hardware:** `HELIX_DISPLAY_BACKEND=remote` + desktop VNC viewer is the daily dev loop; a small headless Python RFB client drives automated touch-injection tests against `--test` mock-printer mode.
- **Firmware:** hardware-in-loop manual at first; the RFB client decode core is written host-compilable so it runs in the same unit suite.
- **Ghost scroll buttons:** normal UI tests + manual on local hardware (setting in all three states).

## Risk Register

| Risk | Mitigation |
|------|------------|
| Scroll/animation feel over WiFi | Anim-off default, Hextile, ghost scroll buttons; budget tuning time. Flick-scrolling long lists is the UX acid test. |
| 2.4GHz congestion in print farms | Document expectations; don't promise farm-scale. |
| libhv server-side unexercised by us | Isolate behind `RfbServer` class; swappable for raw sockets. |
| Binary size on 16MB flash (Phase 2) | Exactly what Phase 0 measures before commitment. |
| CJK fonts in PSRAM (Phase 2) | File-backed glyph streaming or Latin-only v1; Phase 0 measures. |
| Near-parity-on-S3 overpromised to BTT | Framed as hypothesis; yellow gate = steer BTT to P4-class for native. |
| Scope gravity on terminal firmware | v1 = frames + splash only, frozen. |

## Open Items (deferred, not blockers)

- Product naming (`helix-terminal` is a working name).
- v2 resolution auto-match from client-reported panel size.
- Multi-client / read-only spectator support.
- ZRLE encoding (needs zlib) if Hextile proves insufficient.
- BTT OTA infrastructure integration details.
