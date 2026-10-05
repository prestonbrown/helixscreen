# GOD-4: splitting PrintStatusPanel

Audit finding GOD-4 (`2026-09-30-architecture-audit.html#GOD-4`). `src/ui/ui_panel_print_status.cpp`
is 4,193 lines doing six jobs: preview pipeline (~850), measured layout (~530), tree cache and
lifecycle (~650), progress text (~500), terminal/completion flow (~420), print-scoped objects
(~200). Delete this file when the tranche ships.

## Seams

- `GcodePreviewFetcher` (`src/ui/gcode_preview_fetcher.{h,cpp}`): acquisition only, no LVGL. The
  Thumbnail Only and 3D gates, cache path, QIDI `.temp` shadow, metadata, streaming-safe check,
  `preview_cache_is_current`, download. `fetch(filename, on_ready, on_unavailable)` plus
  `cancel()`, its own `AsyncLifetimeGuard`. The print-select detail view uses its in-flight join.
- `PrintPreviewController`, owned by the panel: fetcher, debounce timer, both displayed markers,
  `ensure_preview_current`, viewer hand-off, tool colors, PSRAM thumbnail. `detach_widgets()` from
  `on_ui_destroyed()`.
- `PrintStatusLayoutFitter` (measured layout) and `PrintProgressText` (progress subjects and
  formatting).
- Stays: exclude-object glue (already behind managers), the tune button, the completion flow
  (`on_print_state_changed` orchestrates everything). Camera label moves into
  `PrintLightTimelapseControls`.

Panel ends near 2.4K lines; the audit's 1.5K also needs the lifecycle and completion flow moved,
deferred until the first steps prove out.

## Hazards in today's code (fixed by this tranche)

1. Close mid-download (destroy-on-close) and reopen: a second download starts into the same temp
   file while the first still writes. The panel's lifetime is only invalidated in its destructor.
2. The viewer load callback checks the shared `gcode_load_filename_`, i.e. the most recent load,
   so overlapping loads cross-check names.
3. The cache key is the filename hash only: two printers with the same file name and size get a
   false cache hit.

## Lifetime rules for the new classes

- Every metadata/list/download callback captures a fetcher token and continues only through
  `tok.defer`; no background thread reads `this`.
- `cancel()` (generation bump) when a fetch starts for a different file, from
  `on_ui_destroyed()`, and in the destructor.
- One transfer per path: a fetch for a file already downloading joins it. A cancelled transfer can
  still finish writing; the size check stops a partial file being trusted.
- The viewer callback's user data is the controller; `detach_widgets()` nulls the viewer before the
  tree is deleted.
- The debounce timer is deleted in `cancel()`, `on_deactivating()` and the destructor.

## Commits (pinning test first)

0. Panel-level preview tests: a download landing after tree destroy is dropped; a size-mismatched
   cache re-downloads; a metadata error keeps a rendered preview.
1. Extract the fetcher (pure move).
2. `cancel()` and the in-flight join (behaviour change; close and reopen mid-download holds one
   transfer). `make mutate-diff`.
3. Move the controller; zeus ASAN on the teardown-UAF tests at the end.
4. Layout fitter (move). 5. Progress text (move).
6. Detail view uses the fetcher's join (~-80).
7. Host in the cache key (behaviour change, test with two hosts). `make mutate-diff`.

## Risks and checks

- The #906 family (background callbacks touching LVGL): grep each moved lambda for `defer`.
- The command-line render-mode precedence in `on_activate` stays in the panel.
- Mock (`HELIX_MOCK_AUTO_PRINT=1 --test --sim-speed 6`, pinned socket): one "Streamed G-code to
  disk"; leave and return mid-deferral and mid-download, still one transfer; Thumbnail Only
  mid-print starts no fetch; at `--sim-speed 50`, complete and reprint drops the stale print.
