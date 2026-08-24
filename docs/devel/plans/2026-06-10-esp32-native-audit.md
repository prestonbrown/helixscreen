# ESP32 Native Port Feasibility Audit Plan — Phase 0

> 🚧 **Work in flight (as of 2026-08-09) - coordinate before executing.**
>
> Unchecked boxes here do NOT mean "not started". The ESP-IDF sources and build output live
> in `firmware/` at the repo root, which is deliberately untracked, and the active work is on
> the `esp32/port-4-app` branch. A grep of the tracked tree finds none of the symbols this
> plan prescribes and will wrongly suggest nothing has happened. Check the branch and
> `firmware/` before concluding anything, and ask before picking up a task.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Nature of this plan:** This is an AUDIT, not a feature. The deliverable is a measurement report with a go/no-go recommendation — `docs/devel/plans/ESP32_NATIVE_AUDIT.md` — per Phase 0 of `docs/devel/plans/2026-06-10-esp32-display-device-design.md`. Timebox: 2-4 weeks. Audit scaffolding lives in `firmware/native-audit/` and is throwaway-quality by design (committed for reproducibility, never shipped). **Do not fix the app to make it compile on ESP32 in this phase** — categorize and count; fixing is Phase 2's job.
>
> **Hardware:** any ESP32-S3 module with 16MB flash + 8/16MB octal PSRAM (the Phase 1c dev board works). Steps marked **[HW]** need it; the compile/link measurements (the most decisive ones) do not.

**Goal:** Replace "near-parity HelixScreen on ESP32-S3" faith with three numbers: (1) what fraction of the app core compiles against ESP-IDF and what's blocking the rest, (2) actual `-Os` link size of a vertical slice vs. a realistic ~6-8MB app partition, (3) SRAM/PSRAM watermarks rendering one real panel at 800×480, including CJK font viability.

**The three decision gates (from the spec, restated as measurable thresholds):**
- **GREEN** — slice binary extrapolates to <6MB app partition AND PSRAM watermark <12MB AND no fundamental-blocker category exceeds ~20 files → Phase 2 ports the real codebase to S3.
- **YELLOW** — size 6-9MB extrapolated OR PSRAM 12-15MB OR CJK unworkable → Phase 2 proceeds on ESP32-P4-class hardware (steer BTT) or with explicit feature gates; document which gates.
- **RED** — size >9MB extrapolated OR core architecture (subjects/UpdateQueue/XML engine) fundamentally incompatible → fall back to "shared XML engine + slim new app layer"; tell BTT early.

---

### Task 1: Audit skeleton + helix-xml compile (the cheap decisive test)

**Files:** `firmware/native-audit/{CMakeLists.txt,sdkconfig.defaults,partitions.csv,main/audit_main.c,components/helixcore/CMakeLists.txt}`

`lib/helix-xml` is LVGL-native C and the foundation of the whole near-parity thesis — if it doesn't compile, everything changes.

- [ ] **Step 1:** ESP-IDF project skeleton (same `sdkconfig.defaults` baseline as Phase 1c: octal PSRAM, 16MB flash, `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`, plus `CONFIG_COMPILER_CXX_EXCEPTIONS=y` — the app uses try/catch; note its cost in the report). Component `helixcore` whose CMakeLists compiles, via relative paths into the main repo checkout:
  - LVGL: use the ESP-IDF managed component `lvgl/lvgl` pinned to 9.x FIRST; only if version drift breaks helix-xml, point at the repo's `lib/lvgl` submodule. Record which was used.
  - `lib/helix-xml/**/*.c` — unmodified.
  - A copy of the repo's `lv_conf.h` adapted: force `LV_COLOR_DEPTH 16`, stub the platform `#ifdef`s.
- [ ] **Step 2:** `main/audit_main.c` v0: `lv_init()`, create a display with a PSRAM buffer (`heap_caps_malloc(800*480*2, MALLOC_CAP_SPIRAM)`, `LV_DISPLAY_RENDER_MODE_PARTIAL`, no-op flush), register one trivial inline XML component string, `lv_xml_create` it, log success + heap stats. No real panel needed yet — this can run on any S3 module headless.
- [ ] **Step 3:** `idf.py build` → record in the report draft: compile result for every helix-xml file (expect: clean or near-clean — it's portable C), total component .a size from the map file.
- [ ] **Step 4: [HW]** Flash + run; capture the success log and `esp_get_free_heap_size()`/`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` numbers.
- [ ] **Step 5: Commit** the skeleton + start `docs/devel/plans/ESP32_NATIVE_AUDIT.md` with a Results table (template in Task 5).

```bash
git add firmware/native-audit docs/devel/plans/ESP32_NATIVE_AUDIT.md
git commit -m "audit(esp32): skeleton + helix-xml compile/link/boot measurements"
```

---

### Task 2: Shim layer + app-core compile sweep (categorize, don't fix)

**Files:** `firmware/native-audit/components/helixcore/shim/{spdlog_shim.h,hv_json_shim.h,platform_stubs.{h,cpp}}`, `components/helixcore/CMakeLists.txt`, audit script `firmware/native-audit/sweep.py`

- [ ] **Step 1: Shims** — just enough to let portable code compile, nothing more:
  - `shim/spdlog_shim.h`: a header injected via `-include` that maps the `spdlog::trace/debug/info/warn/error` call surface onto `esp_log` through a printf-style adapter. Try compiling real `fmt` core first (`managed component or vendored fmt with FMT_HEADER_ONLY`); if it explodes flash, fall back to a variadic-template adapter that stringifies `{}` placeholders naively — audit only needs it to COMPILE, fidelity is irrelevant. Record which worked and its size cost.
  - `shim/hv_json_shim.h`: `hv/json.hpp` is nlohmann — vendor nlohmann/json single-header and alias the include path (`-I` mapping `hv/json.hpp` → it). nlohmann compiles fine on ESP32; this is an include-path fix, not a port.
  - `platform_stubs.cpp`: empty/no-op definitions for anything link-missing that is legitimately platform-bound (wpa_cli wrappers, DBus, camera). Every stub added = one line in the report's categorization table.
- [ ] **Step 2: The sweep.** `sweep.py`: for each `.cpp` in a target list, attempt `idf.py`-environment compile (invoke the component build with the file added, or directly call the toolchain `xtensa-esp32s3-elf-g++` with the captured compile_commands flags); bucket the result:
  - **A: compiles clean**
  - **B: compiles with shim** (spdlog/json/include-path only)
  - **C: needs small #ifdef** (filesystem paths, `std::filesystem`, socket headers — record the specific symbol)
  - **D: fundamentally Linux-bound** (BlueZ, DRM, evdev, wpa_cli, camera, HttpExecutor internals)
  Target list = the vertical slice first (`src/printer/printer_state.cpp` + its transitive includes, `src/system/static_subject_registry.cpp`, UpdateQueue impl, `src/ui/ui_panel_home.cpp` or the smallest real panel), then broaden to all of `src/printer/ src/system/ src/ui/` as time allows.
- [ ] **Step 3:** Run the sweep; produce `audit_sweep_results.csv` (file, bucket, blocking symbols). Summarize counts per bucket per directory in the report.
- [ ] **Step 4: Commit.**

```bash
git add firmware/native-audit docs/devel/plans/ESP32_NATIVE_AUDIT.md
git commit -m "audit(esp32): shim layer + compile sweep with A/B/C/D categorization"
```

---

### Task 3: Vertical slice — link size measurement (the killer number)

- [ ] **Step 1:** Get the vertical slice LINKING: helix-xml + LVGL + `PrinterState` + subjects + UpdateQueue + one real panel's XML + the `IMoonrakerClient`/`IMoonrakerAPI` seam satisfied by a stub implementation (`audit_moonraker_stub.cpp`: hardcoded mock state pushed through the real subject pipeline — mirrors what `--test` mode does, minus libhv). Permitted hacks: `#ifdef ESP_AUDIT` carve-outs IN THE AUDIT TREE ONLY (copies/patches under `firmware/native-audit/overrides/`, never edits to `src/`).
- [ ] **Step 2:** `audit_main.c` v1: init subjects → create the panel from its real XML (XML + fonts on a LittleFS partition image built from `ui_xml/` + `assets/fonts/` subsets via `littlefs_create_partition_image`) → push fake printer state through `PrinterState` → confirm the panel renders (no-op flush; verify via `lv_obj_find_by_name` + a screenshot-to-RAM checksum, not eyeballs).
- [ ] **Step 3: Measure and record:**

```bash
idf.py size                       # total image
idf.py size-components            # per-archive breakdown
idf.py size-files | head -50      # biggest objects
```

Report: slice image size; breakdown attributing LVGL vs helix-xml vs app-core vs shims; **extrapolation** = slice app-core bytes × (total app-core SLOC ÷ slice SLOC compiled), stated with the obvious caveats, compared against 6MB/9MB gate lines. Also record C++ exceptions overhead (build once with `-fno-exceptions` on the slice for the delta).
- [ ] **Step 4: [HW] RAM watermarks:** boot the slice, render the panel, log `heap_caps_get_largest_free_block`/`get_free_size` for INTERNAL and SPIRAM at: post-lv_init, post-XML-create, steady-state with a 1Hz fake temp update. Record against the 12MB/15MB gates.
- [ ] **Step 5: Commit.**

```bash
git add firmware/native-audit docs/devel/plans/ESP32_NATIVE_AUDIT.md
git commit -m "audit(esp32): vertical slice links and boots - size + RAM watermarks"
```

---

### Task 4: CJK font viability + render throughput **[HW]**

- [ ] **Step 1: CJK:** put the runtime CJK `.bin` (`assets/fonts/cjk/`) on the LittleFS partition; `lv_binfont_create("L:cjk_xx.bin")`; measure load time + PSRAM cost. If whole-file load is unworkable (likely — multi-MB), test `lv_tiny_ttf` file-streaming as the alternative and record ITS numbers. Outcome feeds the gate: "CJK: whole-load / streaming / Latin-only-v1".
- [ ] **Step 2: Throughput:** with the slice rendering, drive a worst-case invalidation (full-screen `lv_obj_invalidate` at max rate, then a realistic case: animated arc + 4 label updates/s) and measure FPS via `lv_display` refresh timing logs over 60s. Record vs. the informal 25-35fps expectation from the spec. Note PSRAM/WiFi bandwidth contention risk qualitatively (WiFi off vs on if the module allows).
- [ ] **Step 3: Commit.**

```bash
git add firmware/native-audit docs/devel/plans/ESP32_NATIVE_AUDIT.md
git commit -m "audit(esp32): CJK font + render throughput measurements"
```

---

### Task 5: The report + recommendation

- [ ] **Step 1:** Complete `docs/devel/plans/ESP32_NATIVE_AUDIT.md` with this structure (started in Task 1, filled as measured):

```markdown
# ESP32-S3 Native Port Feasibility Audit — Results
Date / commit / ESP-IDF version / hardware used

## Verdict: GREEN | YELLOW | RED  (one paragraph why)

## Measurements
| Metric | Value | Gate | Status |
|---|---|---|---|
| helix-xml compile | n/N files clean | all | … |
| App-core sweep | A:n B:n C:n D:n (of N) | D<20 | … |
| Slice image size | n MB | — | — |
| Extrapolated app size | n MB | <6 / <9 | … |
| PSRAM watermark (slice) | n MB | <12 / <15 | … |
| Internal SRAM low-water | n KB | informational | — |
| CJK strategy | whole/stream/latin-only | workable | … |
| Render FPS (realistic case) | n | ~25+ | … |
| C++ exceptions cost | +n KB flash | informational | — |

## What blocks what (bucket C and D detail)
## Recommended Phase 2 shape (S3 / P4 / fallback architecture)
## Threats to validity (extrapolation caveats, shim fidelity, board specifics)
```

- [ ] **Step 2:** Update the project memory/spec: record the verdict in the spec's Phase 0 section and surface it to the user — **the user communicates the result to BTT; that conversation is theirs, flag it explicitly.**
- [ ] **Step 3: Final commit + cleanup check:** audit tree builds from a clean checkout (`idf.py fullclean && idf.py build`); zero modifications under `src/` or `lib/` in the main repo (`git status` clean outside `firmware/native-audit/` and `docs/`).

```bash
git add firmware/native-audit docs/devel/plans/ESP32_NATIVE_AUDIT.md docs/devel/plans/2026-06-10-esp32-display-device-design.md
git commit -m "audit(esp32): final feasibility report + go/no-go recommendation"
```

## Out of Scope (hard lines for an audit)

- Fixing app code to compile (categorize only; `#ifdef ESP_AUDIT` overrides live in the audit tree).
- Real Moonraker connectivity (stub behind the existing interfaces).
- Any UI beyond one panel; any input; OTA; provisioning.
- Performance optimization — measure what exists, don't tune.
- P4 hardware measurements (the YELLOW path cites public P4 specs; buying/bringing up P4 hardware is a Phase 2 decision).
