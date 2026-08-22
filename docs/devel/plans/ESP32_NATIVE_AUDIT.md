# ESP32 Native Port Feasibility Audit — Results

**Plan:** `docs/devel/plans/2026-06-10-esp32-native-audit.md` (Phase 0 of the ESP32 display program)
**Hardware:** BTT K-Touch (ESP32-S3R8, 8MB octal PSRAM, 16MB flash, 800×480 RGB panel — see `printer-research/BTT_K_TOUCH_HARDWARE.md`)
**Toolchain:** ESP-IDF v5.5 (release branch), `-Os`, C++ exceptions on
**Constraint (2026-07-13):** S3 is the fixed target — no P4 escape hatch. BTT wants broader appeal for existing stock. Feature gates are the expected design.

## Verdict: 🟡 YELLOW — proceed to Phase 2 on S3, with explicit feature gates

Everything the near-parity thesis actually depended on survived contact with
hardware: LVGL 9.5 and `lib/helix-xml` compile and run **unmodified**; the app
core is ~90% shim-portable with exactly one real porting seam (libhv WS/HTTP);
the full desktop shell (navbar + all six panels, real XML, real subject
pipeline) boots and renders on the K-Touch with flat heap; CJK is affordable
(compiled per-tier XIP subsets, zero RAM); and every render-integrity issue
found was root-caused to something fixable (missing `lv_xml_init`, stride
align, bounce buffers, heap-walk instrumentation) — none to the pipeline
itself. What makes this yellow instead of green is arithmetic, not
architecture: the audit image is 8.64MB against a ~6.5MB/slot OTA A/B budget
(per-tier fonts recover ~1.7MB of a measured 2.8MB font payload; a product
build lands ~7MB → single-slot fits, A/B needs the documented diet), and the
full shell leaves ~3.3MB PSRAM for data features — enough for the status-UI
mission, not for desktop-scale caches. Those are precisely the feature gates
the yellow gate anticipated. No red findings.

## Measurements

| Metric | Value | Gate | Status |
|---|---|---|---|
| helix-xml compile | 42/42 clean, zero source changes | all | ✅ |
| LVGL 9.5 compile (repo patches incl.) | all ~600 files clean | all | ✅ |
| App-core sweep (pass 2, 468 files) | A:48 B:372 C:35 D:13 | D<20 | ✅ |
| Slice image (-Os, RTTI+EH on, 486 srcs, 34 fonts) | 8.64MB (audit+experiments build: 8.79MB) | — | — |
| Extrapolated product image | ~7MB (−1.7MB per-tier fonts, +WS/HTTP/WiFi port, +0.9MB CJK) | <6 / <9 | 🟡 |
| PSRAM watermark (full 6-panel shell) | ~4.7MB used / 3.29MB free of 8MB | fits with headroom | 🟡 |
| Internal SRAM low-water | 160KB free (`ALWAYSINTERNAL=0`; 63KB at default) | informational | — |
| CJK strategy | compiled per-tier XIP subsets (5 faces, ~0.9MB flash, zero RAM) | workable | ✅ |
| Render FPS, realistic case (live animation in full shell, partial redraws) | 26-30 FPS @ 12-15ms render, 3-6% CPU (perf-monitor overlay, user-read) | ~25+ | ✅ |
| Render FPS, worst case (forced full-screen invalidation, 100× `lv_refr_now`) | 5.0 FPS (200ms/frame) — avoid continuous full-screen animation | informational | — |
| C++ exceptions + RTTI | on in every measurement above (never A/B'd off) | informational | — |
| Boot to rendered shell | ~81s (LittleFS XML + token scan; build-time token table planned) | informational | — |
| Heap over steady state | flat (all builds, 80s+ windows) | no leaks | ✅ |

## What blocks what (bucket C and D detail)

- **The one real seam:** `include/moonraker_client.h` → `hv/WebSocketClient.h`
  transitively blocks 149 files (via `observer_factory.h`/`app_globals.h`).
  Phase 2 grows the audit's skeletal `shim/hv_stub/` into an
  `esp_websocket_client`/`esp_http_client` implementation behind the existing
  `IMoonrakerClient`/`IMoonrakerAPI` interfaces — the mock-drift seams are the
  porting seams, as designed.
- **C bucket (35):** 23 RTTI (`typeid` via `panel_widget_manager.h`; already
  flipped on with `CONFIG_COMPILER_CXX_RTTI=y`), 4 Xtensa `int32_t`=`long`
  `std::min/max` mismatches (one-line casts), 8 misc POSIX one-liners.
- **D bucket (13):** 8× `hv/requests.h` HTTP users (→ esp_http_client port),
  2 BlueZ Bluetooth (gate off; NimBLE is a later option), `dlfcn`/`ucontext`/
  `hlog` (gate off — plugin loading and crash-handler paths don't apply).

## Recommended Phase 2 shape

- **Keep LVGL + helix-xml unmodified** (lv_conf deltas from Task 1: color
  depth 16, `LV_DRAW_BUF_STRIDE_ALIGN 1`, `LV_OS_PTHREAD`, `lv_xml_init()`
  after `lv_init()`).
- **Port surface = libhv only**, behind `IMoonrakerClient`/`IMoonrakerAPI`.
- **Structural rules discovered on-device:** every app-touching task is
  pthread-created (32KB+ stack if it touches XML); asset-root abstraction
  (no cwd on ESP-IDF VFS); build-time token table (kills the 25-pass XML scan
  and most of the 81s boot); heap telemetry O(1)-only while the display is
  live; `ALWAYSINTERNAL=0` + explicit `heap_caps` internal allocs.
- **Feature gates (the yellow list):** no camera, no 3D/2D gcode rendering
  (already compile-gated: `HELIX_HAS_GCODE_VIEWER=0` / `HELIX_HAS_BED_MESH_3D=0`),
  capped file lists + streamed JSON parsing, **no local temp-file
  materialization** (gcode transforms printer-side or native-remap printers
  only; debug bundles stream to socket; self-update = native `esp_ota` A/B),
  per-tier fonts with compiled CJK subsets.
- **RAM headroom lever if needed:** lazy panel lifecycle (create-on-navigate)
  recovers most of the 0.97MB six-resident-panel cost.
- **Partitioning:** start single-slot factory (+ recovery-lite) at ~7MB;
  move to OTA A/B (2×6.5MB) once the image diet gets under 6.5MB.

## Threats to validity

- The slice runs **mock printer data** — no live Moonraker WebSocket traffic.
  Real JSON churn (temp updates, file lists, AMS state) is the biggest
  unmeasured RAM/CPU variable; it's also exactly what the streamed-JSON gate
  exists to cap.
- **Shim fidelity:** spdlog→esp_log and the hv stub compile the code but
  don't exercise it; the 8 D-bucket HTTP files have never been compiled for
  Xtensa. New blockers may surface during the real port.
- **Boot time** (81s) reflects the audit's LittleFS layout and the known
  pathological token scan; the build-time token table is designed but unbuilt.
- **FPS**: the realistic-case number comes from a continuously-animating
  element in the shell (partial redraws) — scrolling and touch-driven
  full-panel churn are not profiled; GT911 touch is verified at probe level
  but not wired into the audit firmware. The 5 FPS full-screen worst case
  means transition animations need care (one-shot 200ms panel switches are
  acceptable; sustained full-screen animation is not). Untested lever:
  internal-RAM draw buffers were ruled out for the flicker bug but never
  A/B'd for FPS — PSRAM traffic is ~3× per frame with both buffers and the
  framebuffer in PSRAM.
- Single device, bench power, no thermal soak.

## Communicating the result

**The BTT conversation is Preston's** — this report is the input, not the
message. Suggested framing: the S3 K-Touch can run a real HelixScreen-family
firmware (same XML engine, same declarative UI, same look) with a documented
feature-gate list; it will not run the desktop feature set 1:1.

## Task 1 — helix-xml + LVGL compile/link/boot ✅ PASS (2026-07-13)

`firmware/native-audit/` compiles the repo's **LVGL 9.5 submodule (with our patches) and `lib/helix-xml` completely unmodified**, via relative paths, against ESP-IDF. Verified end-to-end on a physical K-Touch: XML component with design-token consts, styled nested widgets, and live `bind_text`/`bind_value` subject bindings rendering at 800×480, visually confirmed stable and artifact-free.

### Numbers

| Measurement | Value |
|---|---|
| App image (LVGL + helix-xml + esp_lcd + IDF runtime, -Os) | **733KB** (674KB before heap-poisoning debug config) |
| helix-xml files compiling clean | 42/42 (zero source changes) |
| LVGL files compiling clean | all (~600, internal xml/expat excluded as in Makefile) |
| `lv_xml_register_component_from_data` | 2.1ms |
| `lv_xml_create` (card + 2 labels + bar, bindings) | 10.3ms |
| Internal RAM free, full stack up | 295KB |
| PSRAM free, full stack up (incl. 768KB FB + 2×128KB draw buffers) | 7.36MB |
| Heap over 10s of 1Hz subject updates | flat (no leaks) |

### Adaptations required (all config/boundary — zero source edits)

1. `lv_conf.h` copy: `LV_COLOR_DEPTH 16` (same as embedded Linux targets); assert handler → default (app-layer header); default font → built-in Montserrat 14 (app fonts are Makefile-compiled objects).
2. `-DLV_KCONFIG_IGNORE` — helix-xml ships `lv_conf_internal.h` without the sibling `lv_conf_kconfig.h`; we configure via lv_conf.h, not Kconfig.
3. `anomaly_stub.c` — one-function stub for the `helix_lvgl_anomaly` telemetry hook our LVGL patches reference (app provides it on Linux).
4. `LV_DRAW_BUF_STRIDE_ALIGN 1` — the repo's 16 is an ARM-SIMD tuning; `esp_lcd_panel_draw_bitmap` expects packed rows. With 16, partial redraws of non-multiple-of-8px areas skew per-row (trapezoid artifacts, confirmed on device).
5. `LV_USE_OS = LV_OS_PTHREAD` kept **unchanged** and works on ESP-IDF pthreads — deliberate: this is the seam the app's std::thread usage rides on in Phase 2.

### Traps discovered (worth their weight for Phase 2)

- **`lv_init()` does NOT call `lv_xml_init()`** — helix-xml is external to LVGL; the app calls it during startup. Skipping it leaves `component_scope_ll` uninitialized, which presents as heap-corruption-shaped TLSF asserts several calls later (three different crash signatures before root-causing). Any ESP32 entry point must call `lv_xml_init()` right after `lv_init()`.
- **Registering subjects requires the "globals" scope** (created by `lv_xml_init`); `lv_xml_register_component_from_data("globals", …)` *dereferences* the existing scope and crashes if init was skipped.
- **RGB panel needs bounce buffers** (10 lines, as stock firmware and PandaTouch reference use). Direct PSRAM scanout visibly desyncs — whole-frame jumping/wrapping — whenever redraw traffic competes for PSRAM bandwidth.
- **ESP-IDF main task stack (3.5KB default) is far too small** for the XML parse path; 32KB works. Any task calling into helix-xml registration/creation needs a real stack — relevant to the plan's "HttpExecutor pools shrink to small stacks" note: *not* for XML-touching tasks.
- CH340 UART: flash/monitor at 460800 max (921600 corrupts). RTS pulse = programmatic reset; `idf.py monitor` unavailable workflow-wise, use raw pyserial capture.

### RAM trim opportunities for the port (Preston 2026-07-13: "we know the actual resolution — cut fonts, cut images, etc.")

Fixed 800×480 RGB565 and a known product surface let us cut aggressively in Phase 2:

- **Draw buffers:** 2×80-line partial buffers = 256KB PSRAM today; 2×40-line = 128KB is likely fine at this PCLK. Measure tearing/fps trade.
- **Fonts:** compile in only the sizes the 800×480 layout actually uses (the app's responsive breakpoints collapse to one profile). No runtime TTF, no unused Montserrat sizes (each compiled-in size costs flash, glyph caches cost RAM). CJK → deferred/file-backed or Latin-first v1.
- **`LV_USE_ASSERT_STYLE` off** in release (LVGL warns it costs RAM; it's a debug aid).
- **Image caches:** `LV_CACHE_DEF_SIZE` / image header cache sized to the tiny on-device asset set; no PNG decode cache for assets we pre-convert to raw RGB565 `.bin` at build time (also removes lodepng from the hot path).
- **No 32-bit anything:** all assets/canvases RGB565; no ARGB8888 intermediates (halves every pixel buffer).
- **Subjects/observers are cheap** (bytes each) — the reactive layer is not a RAM concern; caps go on *data* (file lists paginated, thumbnail streaming, JSON parsed with SAX/streaming instead of nlohmann DOM for large Moonraker payloads).
- **Heap poisoning off** in release builds (audit keeps it on for diagnostics; costs a few % of heap + CPU).

### Calibration vs. stock firmware

BTT's stock K-Touch app: 2.25MB image in 4.5MB OTA A/B slots + 7MB SPIFFS assets, built on ESP-IDF 5.1.1. Our entire UI engine at 733KB leaves generous room; the Phase 2 question is the C++ app core on top (PrinterState, subjects glue, Moonraker client), which Task 2 (compile sweep) and Task 3 (link size of the vertical slice) measure next.

## Task 2 — Shim layer + app-core compile sweep ✅ DONE (2026-07-13)

Per-file compile of all 468 `.cpp` under `src/printer/ src/system/ src/ui/` (plus
`src/application/static_subject_registry.cpp` for the vertical slice) against the
ESP-IDF v5.5 Xtensa toolchain, `-std=gnu++17`, exceptions on, RTTI off (IDF default).
Runner: `firmware/native-audit/sweep.py`; raw data: `audit_sweep_results.csv` (pass 1)
and `audit_sweep_results_pass2.csv` (pass 2). Zero unclassified rows in either pass.

**Headline: the app core is ~90% shim-portable.** 420/468 files compile with nothing
but the spdlog→esp_log and json include-path shims once ONE header seam is carved
(below). No fundamental-blocker category comes anywhere near the ~20-file gate line;
the largest is libhv's HTTP client at 8 files.

### Method

- **Flags:** the exact g++ command ESP-IDF generates for its own C++ TUs
  (from build/compile_commands.json: sysroot, `-mlongcalls`, `-fno-rtti`,
  `-fexceptions`), plus the app side mirrored from the Linux Makefile compile line
  — same include order, same `-include include/lvgl_pch.h`, same feature defines
  minus Linux-only ones (`HELIX_DISPLAY_SDL`, `HELIX_HAS_SYSTEMD/ALSA/SOUND/TRACKER`,
  `HELIX_HAS_LIBUSB`, `ENABLE_GLES_3D` off; `HELIX_HAS_CFS/IFS/LABEL_PRINTER=1`,
  `HELIX_ENABLE_MOCKS/SCREENSAVER` on — the configuration an ESP32 build would use,
  with portable feature logic measured rather than #ifdef'd out).
- **Bucket A nuance:** `include/lvgl_pch.h` unconditionally includes spdlog + fmt +
  `hv/json.hpp` into every TU, so a literal no-shim compile of ANY file is impossible.
  A is therefore "compiles clean AND the file itself never references spdlog/fmt/json"
  (usage scan); B = compiles and uses the shimmed surface.
- **Two passes.** Pass 1 exposed that a single header — `include/moonraker_client.h`
  (→ `hv/Event.h`, `hv/WebSocketClient.h`) — transitively blocks 149 files, because
  `observer_factory.h`/`app_globals.h` pull it into nearly everything. That wall left
  a third of the codebase uncategorized, so pass 2 carves it with skeletal audit-only
  stand-ins (`shim/hv_stub/hv/{Event.h,WebSocketClient.h}` — `hv::TimerID`, an empty
  `WebSocketClient` base with the three callback members and `setReconnect()`; nothing
  in `moonraker_client.h`'s inline code calls the base, so a declaration suffices for
  compile-only categorization).

### Results

Pass 1 (spdlog/json shims only):

| dir | A | B | C | D | total |
|---|---|---|---|---|---|
| src/application | 0 | 1 | 0 | 0 | 1 |
| src/printer | 9 | 23 | 1 | 28 | 61 |
| src/system | 9 | 69 | 6 | 19 | 103 |
| src/ui | 29 | 151 | 9 | 114 | 303 |
| **TOTAL** | **47** | **244** | **16** | **161** | **468** |

→ 149 of the 161 D rows share one blocker: `moonraker_client.h` → libhv. That seam is
already designed for — `IMoonrakerClient` (`include/i_moonraker_client.h`) exists
precisely so a stub/port can satisfy it (Task 3 does exactly that).

Pass 2 (seam carved — the real distribution underneath):

| dir | A | B | C | D | total |
|---|---|---|---|---|---|
| src/application | 0 | 1 | 0 | 0 | 1 |
| src/printer | 9 | 48 | 4 | 0 | 61 |
| src/system | 9 | 75 | 7 | 12 | 103 |
| src/ui | 30 | 248 | 24 | 1 | 303 |
| **TOTAL** | **48** | **372** | **35** | **13** | **468** |

### Bucket C detail (pass 2, 35 files — all small #ifdef / config-level)

| Blocker | Files | Fix shape |
|---|---|---|
| `typeid` with `-fno-rtti` | 23 (19 via `include/panel_widget_manager.h:40` alone) | `CONFIG_COMPILER_CXX_RTTI=y` (IDF supports it, costs flash — measure in Task 3) or replace that one header's typeid with static type tags |
| `std::min/max/clamp` arg mix | 4 | Xtensa newlib defines `int32_t` as `long`; mixed `int32_t`/`int` args break template deduction — cast-level fixes |
| sys/statvfs.h ×2, ifaddrs.h, sys/utsname.h | 4 | POSIX headers newlib lacks; disk-space/hostname probes — #ifdef with esp_vfs/esp_netif equivalents |
| `spdlog/sinks/base_sink.h` | 1 (+1 via `crash_error_log_sink.h`) | log-backend setup; replaced wholesale by esp_log on a port |
| POSIX signals (`SA_*`), `timegm()`, missing `<thread>` include | 3 | one-liners |

### Bucket D detail (pass 2, 13 files — genuinely Linux-bound)

| Blocker | Files | Notes |
|---|---|---|
| `hv/requests.h` (libhv HTTP client) | 8 | camera_stream, crash_reporter, debug_bundle_collector, ipp_printer, snapshot_qr_scanner, telemetry_manager, update_checker, ui_spoolman_overlay — the biggest real porting surface beyond the WS client; ESP-IDF's esp_http_client is the natural target |
| BlueZ/RFCOMM Bluetooth | 2 | bt_print_utils, makeid_bt_printer (label printing) — feature-gate off for v1 |
| dlfcn.h, ucontext.h, `hv/hlog.h` | 3 | bluetooth_loader (dlopen), crash_handler (signal-context dumps → esp coredump instead), logging_init (log backend) |

Direct libhv API leakage outside the client seam is tiny: two UI files call
`setReconnect()` on the client; everything else reaches libhv only through
`MoonrakerClient`/`MoonrakerAPI`.

### Shim/stub categorization table (one row per shim, per plan)

| Shim | What it does | Fidelity |
|---|---|---|
| `shim/spdlog_shim.h` | spdlog call surface → `helix_shim_log()` → esp_log; formats with the repo's OWN bundled fmt (`lib/spdlog/.../fmt/bundled`, header-only) — probed clean on Xtensa GCC 14, so format-string/arg type checking is semantically identical to the Linux build | Real formatting; naive fallback kept behind `HELIX_SHIM_NAIVE_FMT`, unused |
| `shim/include/spdlog/{spdlog,common}.h`, `spdlog/fmt/fmt.h` | include-path aliases onto the shim | — |
| `shim/hv_json_shim.h` + `shim/include/hv/json.hpp` | `hv/json.hpp` IS nlohmann 3.12.0 verbatim (zero libhv deps) — alias to the repo's copy, no second vendored header | Identical header |
| `shim/include/json.hpp` | bare `"json.hpp"` form (via `include/unit_conversions.h`; Linux resolves it through `-isystem lib/libhv/cpputil`) | Identical header |
| `shim/platform_stubs.{h,cpp}` | `helix_shim_log(level,msg)` → `esp_log_write` funnel (keeps esp_log macros out of app TUs); compiled in the component build | Real |
| `shim/hv_stub/hv/{Event.h,WebSocketClient.h}` | **pass-2 only, never linked**: `hv::TimerID`, skeletal `WebSocketClient` (3 callback members, `setReconnect`) to carve the moonraker seam | Declaration-only by design |

### Threats to validity

- Compile-only (`-c`, no link): missing symbols, static-init order, and section/size
  issues are invisible until Task 3.
- `-O0 -w`: warnings and optimizer-dependent diagnostics suppressed by design.
- A file that compiles is not a file that WORKS — filesystem paths, `/proc` reads,
  POSIX sockets inside function bodies compile fine against newlib+lwip headers and
  fail at runtime. The sweep measures compile viability, which is what Phase 0 gates.
- Bucket counts depend on the chosen define set (documented above); flipping
  `HELIX_HAS_LABEL_PRINTER=0` would move the 2 BT files out of the denominator.
- `hv_stub` fidelity is declaration-level; pass-2 B files still need the real
  seam satisfied (Task 3 stub or Phase 2 esp_websocket_client port) to link.

## Task 3 — Vertical slice links, boots, and renders ✅ PASS (2026-07-13)

The full app-core slice — **486 repo sources** (PrinterState + all printer state
classes, UpdateQueue, SubjectInitializer, theme system, all custom widgets, the
home panel and its widget manager, settings/translation/tips managers) plus 34
tier-6 fonts — links against ESP-IDF, boots on the K-Touch, and **renders the
real home panel from the real `ui_xml/` tree served off LittleFS**. Visually
confirmed on device (Nord theme fallback, image assets absent by design).
Renderer compile-out gates (`HELIX_HAS_GCODE_VIEWER=0`,
`HELIX_HAS_BED_MESH_3D=0`, added to the main Makefile as label-printer-style
flags) and `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n` are in effect.

### Numbers

| Measurement | Value |
|---|---|
| App image (-Os, RTTI + exceptions ON, 486 sources, 34 fonts) | **9,064,576 bytes (8.64MB)** |
| …same before cert-bundle off + last 2 renderer TUs gated | 9,229,616 (8.80MB) |
| Core-slice-only milestone (before fonts/panels grew the list) | 1.06MB (helixapp archive alone: 90KB) |
| Fonts in flash (largest single cost) | ~2.7MB |
| `ui_xml/` tree on LittleFS partition | 4,936KB used / 6,464KB |
| Boot to Task-1 card | ~3.4s |
| Boot to home panel created + finalized | **~60s** (see seams below) |
| Internal RAM free: boot → LVGL up → slice up | 245KB → 161KB → **63KB** |
| PSRAM free: boot → LVGL up → slice up | 8.39MB → 7.36MB → **4.35MB** |
| Heap over 80s steady render loop | flat (internal and PSRAM both stable) |

Size notes: `esp_idf_size --files` shows ~630KB attributed to
`esp_app_desc.c.obj` — an attribution artifact (the real `.rodata_desc` is
0x100 bytes; the tool lumps unattributed rodata/exception tables at the segment
start into it). Treat per-file numbers as approximate; the map file is the
truth (`x509_crt_bundle*`, `gcode_*`, `bed_mesh_*` symbols verified absent).
The 9.5MB factory partition is measurement-only; a product build must fit an
OTA A/B scheme, and fonts remain the headline Phase 2 trim.

### Runtime seams found (each cost a boot-debug cycle; all have audit-tree fixes)

1. **`pthread_self()` asserts on raw FreeRTOS tasks.** The app core calls
   `std::this_thread::get_id()` (`ui_notification_init`, main-thread
   detectors, spdlog); ESP-IDF's gthread shim routes it to `pthread_self()`,
   which `assert()`s from any task not created via pthread — including the
   `main` task. Fix: `audit_main.c` runs the whole app phase (init + render
   loop) on a pthread with a 32KB stack. **Phase 2 rule: every task that can
   touch app code must be pthread-created.**
2. **No working directory on ESP-IDF VFS → every relative asset path fails.**
   Desktop assumes cwd = install root. `theme_manager`'s responsive-token
   auto-discovery (`opendir("ui_xml")`) silently found nothing → all `space_*`
   / `font_*` / `nav_width` tokens unregistered → `ui_text` hard-aborts on
   missing `font_small` (boot loop). Config-dir creation fails the same way
   (harmless here). Fix: `overrides/theme_manager.cpp` points discovery at
   `/littlefs/ui_xml`. **Phase 2 needs a real asset-root abstraction.**
3. **Token discovery I/O is pathological on flash filesystems.** ~25 scan
   passes (px/string/color × up to 7 breakpoint suffixes), each re-reading
   every top-level XML file (~1.26MB across ~150 files) via `std::ifstream`:
   watchdog-observed **150s+ and still not done** on LittleFS. Fix: override
   caches file bytes (~1.3MB, PSRAM) so flash is read once → theme init ~50s
   total. **Phase 2: single-pass discovery or a build-time token table.**
4. **Fonts must be XML-registered before theme init.** The slice initially
   omitted `AssetManager::register_all()`; the responsive font registrar
   verifies each face is linked and drops the token otherwise → same fatal
   `font_small` path as (2). Also: 12 faces referenced by `asset_manager.cpp`
   aren't in the tier-6 audit set (`source_code_pro` family, `mdi_icons_80/96/128`,
   `noto_sans_8` — `mdi_icons_128.c` alone is 7.2MB of source); they're
   alias-stubbed to `noto_sans_16` in `audit_stubs.cpp`. Only
   `source_code_pro_10..16` can actually be selected at 480px and render as
   the alias face — acceptable for a structure audit.
5. **800×480 selects breakpoint tier `_medium`** (480 > `UI_BREAKPOINT_SMALL_MAX`),
   not `_small` — pins exactly which font sizes a K-Touch build must ship.
6. **Graceful degradation works.** Missing subjects from out-of-slice
   subsystems (`job_queue_count`, `led_*`), a missing event callback, missing
   `assets/images/*` on LittleFS, and missing translations all warn-and-continue.
   The only hard-abort paths found were the two font-token ones above.

### Slice mechanics (delta over Task 2)

- `link_loop.py` grew `app_srcs.txt` to 486 sources using a Linux-build symbol
  index (regen command in `resolve_undefined.py` header); stubs documented in
  `audit_{stubs,moonraker_stub,platform_stubs2,fake_typeinfo,stb_impl}.*`.
- `overrides/` audit-tree copies (never `src/` edits): 10 files — Xtensa
  `int32_t`=long casts, `timegm`, `statvfs`, `ifaddrs`, `<thread>`, and now
  `theme_manager.cpp` (VFS paths + content cache).
- LittleFS via joltwallet component; `littlefs_create_partition_image` of
  `ui_xml/`; `LV_FS_POSIX_PATH "/littlefs/"` maps the app's `A:` paths.
- `SubjectInitializer::init_panels(nullptr, rc)` — panels tolerate a null API
  at init; the audit drives state via PrinterState setters (like `--test`
  minus libhv). No null-API crash observed through full panel construction.
- Task watchdog warnings during init are cosmetic (main-thread init doesn't
  yield for tens of seconds; steady-state loop is clean).

### Threats to validity

- No exceptions-off delta measured (RTTI+exceptions ON throughout Task 3).
- 9.5MB factory partition is for measurement, not shippable (no OTA A/B).
- Boot time includes audit logging and first-boot cache population; not a
  Phase 2 boot-time prediction, but the LittleFS read cost it exposes is real.
- Aliased mono/micro fonts mean glyph fidelity is unverified for those faces.
- Home panel renders alongside the Task-1 card (intentional); no full-screen
  visual-diff against a Linux screenshot was performed.
- Slice has no Moonraker/WebSocket/network layer — the biggest unlinked seam.

### Task 4 follow-up — full app shell + RAM routing experiment (2026-07-13)

The slice was then upgraded from home-panel-only to the REAL desktop shell:
`app_layout.xml` (navbar + all six panels resident-and-hidden, the desktop
memory model), `NavigationManager::set_app_layout` + `wire_events`,
`PanelFactory::find_panels/setup_panels`. Boots clean, navbar renders,
user-confirmed on device. RAM measurements across configs:

| Config | Internal free | PSRAM free |
|---|---|---|
| home panel only, `SPIRAM_MALLOC_ALWAYSINTERNAL=4096` | 63,155 | 4.35MB |
| full shell (6 panels + navbar), `=512` | 63,171 | 3.39MB |
| full shell (6 panels + navbar), `=0` | **159,967** | 3.29MB |

Findings:

- **The six-resident-panel desktop model costs ~0.97MB PSRAM** and fits with
  3.3MB headroom — not a day-one blocker on 8MB. Lazy panel lifecycle
  (create-on-navigate, the overlay pattern the codebase already uses for
  print_status) remains the biggest lever to buy headroom back for data-heavy
  features (gcode metadata, thumbnails, Moonraker payloads).
- **Internal RAM pressure was entirely sub-512B allocations** (LVGL widget
  structs are ~200-300B): threshold 512 reclaimed nothing, threshold 0
  reclaimed ~97KB, moving internal from 63KB to 160KB free (of ~250KB).
  RTOS/WiFi/DMA still get internal via explicit `heap_caps` flags. Boot time
  unchanged (+2.4s, noise-level); steady-state heap flat in all configs.
  No visible render degradation with all widget data in octal PSRAM.
- Code size is a non-issue for RAM: XIP demand-paging from flash is
  hardware-managed overlaying — only ~16KB of IRAM-pinned hot paths occupy RAM.

**RESOLVED — periodic transient frame corruption (~10-15s cadence):** brief
whole/partial-frame corruption that self-resolved, observed on the thr-0 full-
shell build. Root cause: the render loop's 10s `[heap:steady]` log called
`heap_caps_get_largest_free_block()`, which runs `multi_heap_get_info()` — a
walk of **every block in the heap inside an interrupt-disabling critical
section**. At ALWAYSINTERNAL=0 the PSRAM heap holds thousands of small widget
blocks, so the walk keeps interrupts off (and PSRAM busy) long enough for the
RGB bounce-buffer refill ISR to miss its deadline → one corrupted frame per
call. Proven by a differential build splitting the log into a pure UART line
every 3s and a silent heap walk every 20s: user-observed flicker followed the
20s walk exactly. Fix: steady-state logging uses `heap_caps_get_free_size()`
only (O(1) tracked counter, no walk); full walks stay in one-shot stage
watermarks. This also explains why the home-panel-only build showed none
(small PSRAM heap = fast walk) — the trigger was heap block count, not the
thr-0 routing itself. Ruled OUT along the way: LVGL compositing bandwidth —
internal draw buffers (2×32-line, MALLOC_CAP_INTERNAL) did not change it
(reverted to PSRAM).

**Phase 2 rule from this:** on RGB-panel ESP32 targets, never call
`heap_caps_get_largest_free_block()` / `heap_caps_get_info()` /
`heap_caps_check_integrity*()` periodically while the display is live. Any
ESP32 port of MemoryMonitor must sample with O(1) free-size counters only.

**Phase 2 constraint — temp-file / scratch storage (flagged by Preston
2026-07-13):** the desktop app materializes multi-MB temp files in several
paths, and the ESP32 has nowhere to put them — LittleFS storage is ~6.6MB
total (and mostly full of ui_xml + fonts), there is no /tmp, and PSRAM can't
absorb 10-100MB gcode. `StreamingPolicy`'s existing "stream" answer is
*download to disk*, which doesn't exist here — the port needs a third policy
mode: **no local materialization**. Inventory of desktop temp-file users and
their ESP32 disposition:
- `GCodeFileModifier` (print-prep download→modify→re-upload, e.g. spool remap
  without native mapping): must go printer-side (Moonraker-side transform, or
  restrict to printers with native remap à la U1 `SET_PRINT_EXTRUDER_MAP`) or
  be feature-gated off.
- 2D gcode viewer full-file download: already compiled out
  (`HELIX_HAS_GCODE_VIEWER=0`, `f71f10341`).
- Thumbnails (`thumbnail_processor`): small (tens of KB) — cap + PSRAM-only,
  no disk cache.
- Debug bundles (`log_collector`): multi-MB tarball — ESP32 variant must
  stream straight to the upload socket or ship a reduced bundle.
- Self-update (`update_checker` downloads the new binary): replaced wholesale
  by native `esp_ota` A/B streaming from HTTP — never touches a filesystem.
- `input_shaper_cache`, config writes: KB-scale, fine on LittleFS.

**CJK font viability (Task 4, final piece — MEASURED on-device 2026-07-13):**
two implementations of the same 1203-codepoint zh+ja translation subset
(16px/4bpp, Noto Sans CJK SC+JP), rendered side by side on the K-Touch
(user-confirmed correct glyphs, identical quality):

| | runtime `.bin` (desktop CjkFontManager path) | compiled-in C array (XIP) |
|---|---|---|
| heap cost | **135.9KB PSRAM** (1.10× file size), 0 internal | **zero** (internal + PSRAM byte-identical) |
| load time | **1361ms** from LittleFS (~90KB/s) | none (rodata) |
| flash cost | 123KB on LittleFS | +135KB app image |

Extrapolation: the desktop model loads ~22 bins for a CJK language even at
the AD5M font tier ≈ **~3.0MB PSRAM of the ~3.15MB remaining + ~15s LittleFS
load — not viable**. The compiled XIP route costs zero RAM — and the target
build only needs the faces its breakpoint tier actually maps (Preston's
callout): the 800×480 **_medium** tier uses exactly five text-font tokens
(globals.xml:267-303) → CJK companions `noto_sans_26` (heading),
`noto_sans_bold_28` (xl), `noto_sans_18` (body), `noto_sans_light_16`
(small), `noto_sans_light_12` (xs). Summing those five bins × the measured
1.10 compiled/bin ratio = **~0.9MB flash, zero RAM** (font_mono has no CJK
fallback even on desktop). **Verdict: CJK is VIABLE on the S3 — Phase 2
keeps CjkFontManager's `->fallback` wiring but points it at const compiled
fonts (no lv_binfont_create, no load/unload, no heap).** The subset is baked
at firmware build time from the translation YAMLs — same regen trigger the
desktop uses, different output format (`--format lvgl` vs `bin`). Test
scaffolding: `main/noto_sans_cjk_16_compiled.c` (generated via lv_font_conv
with the manifest codepoints) + `cjk_experiment()` in audit_main.c.

## Remaining tasks

- **Task 4 [HW]: COMPLETE** — RAM watermarks + routing (160KB internal /
  3.29MB PSRAM free, full shell), flicker root-caused (heap-walk critical
  section), CJK viability measured (compiled XIP = zero RAM, viable).
- **Task 5:** final report + go/no-go. Gates revised 2026-07-13: yellow = S3 + explicit feature gates (P4 hatch removed).
