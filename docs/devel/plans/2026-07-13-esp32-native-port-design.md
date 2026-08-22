# ESP32 Native Port ("helixscreen" ESP32 target) — Phase 2 Design

**Date:** 2026-07-13
**Status:** Approved (brainstorm complete)
**Parent spec:** `2026-06-10-esp32-display-device-design.md` (Phase 2, greenlit on the audit's yellow verdict)
**Audit:** `docs/devel/plans/ESP32_NATIVE_AUDIT.md` — measurements, feature gates, structural rules
**Hardware:** BTT K-Touch (ESP32-S3R8, 8MB octal PSRAM, 16MB flash, 800×480 RGB, GT911 touch)

## Decisions (Preston, 2026-07-13)

1. **Native-first.** The RFB terminal (Phases 1b/1c) is deferred, not cancelled;
   nothing blocks on it. `helix-terminal` stays reserved as its name.
2. **v1 feature cut: Core + AMS.** Multi-material is the differentiator and
   ships in v1 despite being the largest post-gcode RAM consumer.
3. **OTA A/B from day one.** The image diet is committed up front; retrofitting
   partition tables on fielded units is a USB-reflash breaking change.
4. **Unified branding.** No separate product name — it's HelixScreen built for
   another target, like Pi or AD5M. Project dir: `firmware/helixscreen-esp32/`.
5. **Architecture: seams-first hybrid** (approach C). The audit's ESP-IDF
   component structure for the build; the handful of audit-proven abstractions
   land properly in the main tree, pulled by need; CI cross-build prevents
   drift.

## Repo layout

```
firmware/helixscreen-esp32/          # ESP-IDF project (product quality)
├── main/                            # entry, board bring-up, DisplayBackendEspLcd glue
├── components/
│   ├── helixcore/                   # lv_conf.h + LVGL + helix-xml (from audit, kept)
│   ├── helixapp/                    # curated per-subsystem source lists from ../../src
│   └── helixnet/                    # NEW: esp_websocket_client / esp_http_client
│                                    #      impls of IMoonrakerClient / IMoonrakerAPI
├── shim/                            # spdlog→esp_log, hv/ include aliases
├── boards/ktouch/                   # pin/timing/touch board table (config, not fork)
└── partitions.csv                   # OTA A/B: 2×6.0MB app + 3.75MB storage + NVS
```

*(Partition sizing corrected during planning: storage must hold ui_xml — 1.6MB
top-level + 412KB components + 1.8MB per-language translations (the 884KB
merged translations.xml and micro/ layouts don't ship: translation_loader.cpp
loads per-language files only; 800×480 is the _medium breakpoint). Build-time
XML minification (~30%) makes 3.75MB comfortable. App slots therefore 6.0MB,
image budget ≤5.8MB.)*

The audit tree (`firmware/native-audit/`) remains as frozen reference;
the product tree starts clean and pulls proven pieces from it deliberately.

### Main-tree seams (each serves desktop too, or is neutral)

| Seam | Change | Desktop effect |
|---|---|---|
| Asset root | `helix_paths` grows an asset-root concept (`/littlefs` vs install dir) | Neutral (replaces audit's theme_manager override) |
| Token table | Build-time generator scans `ui_xml/` → emits token registration table; theme_manager consumes when present, falls back to runtime scan | **Faster desktop boot** |
| Settings storage | Small storage-backend interface under `SettingsManager` (JSON file today; NVS/LittleFS impl firmware-side) | Neutral |
| WiFi | `esp_wifi` backend behind the existing WiFi settings backend seam | Neutral (wpa_cli stays Linux-only) |
| Network | ESP impls of `IMoonrakerClient`/`IMoonrakerAPI` (new files in firmware tree) | None — the mock-drift interfaces were built to be this seam |
| Compile gates | Continue `HELIX_HAS_*` pattern; new gates only where the audit's D-bucket demands (BlueZ, dlfcn, camera) | Neutral |

## v1 port surface (Core + AMS)

| Ships in v1 | Gated off in v1 |
|---|---|
| Home, temps, motion, macros/console | Camera, 3D/2D gcode render |
| Print select (capped file list + thumbnails) + print status | Label printers, Bluetooth |
| Settings incl. WiFi provisioning (SoftAP captive portal on first boot, per terminal design) | Plugins, Spoolman, timelapse |
| AMS panel + backends (same WS pipeline; RAM budget reserved) | Debug-bundle heavy paths (light variant streams to socket) |
| OTA A/B via `esp_https_ota` + manifest URL (BTT-pluggable) | Local temp-file features — no local materialization; gcode transforms printer-side or native-remap printers only |
| CJK via compiled per-tier XIP subsets (5 faces, ~0.9MB flash, zero RAM) | Input shaper charts (CSV-heavy; evaluate post-v1) |

## Threading & data flow (audit-derived hard rules)

- Every task that can touch app code is **pthread-created**; LVGL + app run on
  one main pthread (32KB stack) — same single-UI-thread model as desktop.
- `esp_websocket_client` callbacks are background threads: the existing
  `ui_queue_update()` / `tok.defer()` discipline applies unchanged.
- HttpExecutor: 1+1 workers, small stacks. **No raw `std::thread`** anywhere.
- Heap telemetry is O(1)-only while the display is live (no
  `largest_free_block` / `get_info` / integrity checks on a timer — measured:
  one corrupted frame per call at product heap sizes).
- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`; DMA/WiFi-critical allocations take
  explicit `MALLOC_CAP_INTERNAL`.
- JSON from Moonraker is parsed streaming/capped per the audit gates; no
  unbounded buffering of notifications.

## Budgets (CI/HIL-enforced, not aspirational)

| Budget | Limit | Enforcement |
|---|---|---|
| App image | ≤5.8MB (6.0MB slot − margin) | CI fails build over budget |
| PSRAM steady-state free | ≥1.5MB with shell + live Moonraker + AMS | HIL boot-log assertion |
| Internal SRAM free | ≥100KB steady state | HIL boot-log assertion |
| Boot to rendered UI | ≤10s | HIL serial-marker timing |

Known levers: per-tier fonts (−1.7MB, measured), audit-tree slack, token table
(boot), lazy panel lifecycle (~1MB PSRAM, reserve lever).

## Failure behavior

- WiFi drop / Moonraker restart → existing disconnected-state UI; auto-reconnect
  with backoff.
- OTA: A/B with rollback — `esp_ota` confirm-on-healthy-boot; failed boot
  reverts to the previous slot.
- Crash: coredump partition + next-boot minimal text report through the
  existing crash-reporter pipeline (no bundle).
- LittleFS corruption: out of scope v1 — document USB reflash.

## Testing & CI

- **Host-side first:** ported code compiles Linux-side where possible;
  ESP-specific code sits behind interfaces with Catch2 unit tests + mocks.
- **CI cross-build:** GitHub Actions job builds the ESP32 target on every PR
  (ESP-IDF docker image) + the image-size budget gate. This—not architecture—
  is the drift prevention.
- **HIL on the K-Touch:** scripted flash + serial-marker assertions (boot time,
  RAM watermarks, subject round-trip), building on the audit's capture tooling.
- Mock-printer mode works on-device via the existing mock client — UI testable
  without a printer.

## Out of scope for this design

- RFB terminal (deferred; parent spec Phase 1b/1c unchanged).
- Non-K-Touch boards (board-table structure anticipates them; none targeted).
- BTT OTA infrastructure specifics (manifest URL is pluggable; their call).
- LVGL version upgrades (pinned 9.5.0 per existing decision).
