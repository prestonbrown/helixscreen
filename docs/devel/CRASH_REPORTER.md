# Crash Reporter (Developer Guide)

How the crash reporter works end-to-end: crash detection, report collection, delivery pipeline, UI modal, and the Cloudflare Worker that creates GitHub issues.

**Key files:**

| File | Purpose |
|------|---------|
| `include/system/crash_reporter.h` | CrashReporter singleton API |
| `src/system/crash_reporter.cpp` | Core logic: parsing, formatting, sending |
| `include/ui_crash_report_modal.h` | Modal dialog class |
| `src/ui/ui_crash_report_modal.cpp` | Modal UI logic and delivery flow |
| `ui_xml/crash_report_modal.xml` | Modal layout (XML) |
| `server/crash-worker/src/index.ts` | Cloudflare Worker (creates GitHub issues) |
| `tests/unit/test_crash_reporter.cpp` | Unit tests (30 cases, 72 assertions) |

---

## Architecture Overview

```
crash_handler.cpp                  CrashReporter                    CrashReportModal
(signal handler)                   (next startup)                   (LVGL modal)
      |                                  |                                |
  writes crash.txt          has_crash_report() → true            show_modal(screen)
  (async-signal-safe)       collect_report()                     attempt_delivery()
                            ├── parse crash.txt                       |
                            ├── platform info                    ┌────┴────────┐
                            ├── hardware info                    |             |
                            └── log tail                    try_auto_send  show_qr_code
                                                                |             |
                                                          POST to worker  QR with GitHub URL
                                                                |
                                                          crash.helixscreen.org
                                                                |
                                                          GitHub Issue created
```

### Relationship to TelemetryManager

CrashReporter is **independent** of TelemetryManager. Both read crash.txt, but CrashReporter runs first and calls `consume_crash_file()` to delete it. TelemetryManager's `check_previous_crash()` then finds no file and silently skips. This means crash reports are sent even when telemetry is disabled.

---

## Crash File Format

Written by `crash_handler::install()` signal handler (async-signal-safe). Located at `~/helixscreen/config/crash.txt`:

```
signal:11
signal_name:SIGSEGV
app_version:0.9.16
timestamp:2026-02-12T22:31:58
uptime_sec:3600
backtrace:0x400abc
backtrace:0x400def
backtrace:0x401000
```

Key-value pairs, one per line. Multiple `backtrace:` lines for the call stack. Parsed by `crash_handler::read_crash_file()`.

---

## Delivery Pipeline

Priority order — first success wins:

### 1. Auto-Send (CF Worker)

`CrashReporter::try_auto_send()` POSTs JSON to `crash.helixscreen.org/v1/report`. The worker validates the API key and creates a GitHub issue with `crash` + `auto-reported` labels.

```
Device → POST crash.helixscreen.org/v1/report → CF Worker → GitHub Issues API
         (X-API-Key header)                      (GITHUB_TOKEN secret)
```

Uses the shared telemetry ingest key (`INGEST_API_KEY`). Not a true secret — it's baked into the binary to prevent casual spam, not to authenticate individual devices.

### 2. QR Code Fallback

If auto-send fails (no network), `generate_github_url()` builds a pre-filled GitHub issue URL and displays it as a QR code. The user scans with their phone to file the issue.

URL format: `https://github.com/prestonbrown/helixscreen/issues/new?title=...&body=...&labels=crash,auto-reported`

Truncated to <2000 chars for QR compatibility. Requires `LV_USE_QRCODE=1` in `lv_conf.h`.

### 3. File Fallback

Always writes `~/helixscreen/crash_report.txt` (human-readable). Users can SCP this file to the developer.

---

## Startup Integration

In `application.cpp`, the crash reporter runs after XML components are registered and the screen exists, but before the wizard check:

```cpp
// Write mock crash file if --mock-crash flag set (testing only)
if (get_runtime_config()->mock_crash) {
    crash_handler::write_mock_crash_file("config/crash.txt");
}
CrashReporter::instance().init("config");

// ... later, after init_ui() ...

if (CrashReporter::instance().has_crash_report()) {
    auto report = CrashReporter::instance().collect_report();
    auto* modal = new CrashReportModal();
    modal->set_report(report);
    modal->show_modal(lv_screen_active());
}
```

The modal self-deletes via `ui_queue_update()` in `on_hide()` — no external ownership needed.

### Startup Order

```
Phase 1:  parse_args (--mock-crash flag parsed)
Phase 2:  crash_handler::install("config/crash.txt")
          CrashReporter::init("config")
          mock crash file written (if --mock-crash)
Phase 8:  XML components registered (crash_report_modal.xml)
Phase 10: init_ui() — screen exists
          → Crash detection + modal show (HERE)
Phase 12: Wizard check
          TelemetryManager::init() — crash.txt already consumed
```

---

## CrashReport Struct

Data collected at startup:

| Field | Source | Example |
|-------|--------|---------|
| `signal` | crash.txt | `11` |
| `signal_name` | crash.txt | `"SIGSEGV"` |
| `app_version` | crash.txt | `"0.9.16"` |
| `timestamp` | crash.txt | `"2026-02-12T22:31:58"` |
| `uptime_sec` | crash.txt | `3600` |
| `backtrace` | crash.txt | `["0x400abc", "0x400def"]` |
| `platform` | `UpdateChecker::get_platform_key()` | `"pi4"` |
| `ram_total_mb` | `PlatformCapabilities::detect()` | `4096` |
| `cpu_cores` | `PlatformCapabilities::detect()` | `4` |
| `log_tail` | Pre-crash log lines (up to ~500; post-crash lines filtered out) | `"[2026-02-12 ...] ..."` |
| `printer_model` | Empty (Moonraker not connected yet) | `""` |
| `klipper_version` | Empty (Moonraker not connected yet) | `""` |
| `display_info` | Empty (populated later if needed) | `""` |

Note: `printer_model`, `klipper_version`, and `display_info` are empty at startup because Moonraker isn't connected yet. Future enhancement: populate these if connection is available.

---

## JSON Schema (Worker Payload)

`report_to_json()` produces this structure for the CF Worker:

```json
{
  "signal": 11,
  "signal_name": "SIGSEGV",
  "app_version": "0.9.16",
  "timestamp": "2026-02-12T22:31:58",
  "uptime_seconds": 3600,
  "backtrace": ["0x400abc", "0x400def", "0x401000"],
  "platform": "pi4",
  "printer_model": "",
  "klipper_version": "",
  "display_backend": "",
  "ram_mb": 4096,
  "cpu_cores": 4,
  "log_tail": ["[2026-02-12 22:31:55.123] ...", "..."]
}
```

Field naming follows the **worker's expectations** (not the C++ struct names). Key mappings:

| C++ struct field | JSON field | Note |
|-----------------|------------|------|
| `uptime_sec` | `uptime_seconds` | |
| `ram_total_mb` | `ram_mb` | |
| `display_info` | `display_backend` | |
| `cpu_cores` | `cpu_cores` | Worker formats as "N cores" |
| `log_tail` | `log_tail` | String in C++, split to array in JSON |

---

## CF Worker (`server/crash-worker/`)

### Endpoints

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/` | None | Health check |
| POST | `/v1/report` | `X-API-Key` | Submit crash report |

### Secrets

Set via `wrangler secret put`:

| Secret | Purpose |
|--------|---------|
| `INGEST_API_KEY` | Shared with telemetry worker — validates device requests |
| `GITHUB_TOKEN` | GitHub PAT with Issues: Read and write scope |

`GITHUB_REPO` is set as a plain var in `wrangler.toml` (not a secret).

### Release-Version Gate

`INGEST_API_KEY` is compiled into every binary and the repo is public, so anything
built from source — a fork, a local build, a private branch — posts to the same
endpoint. Those reports can never be symbolicated: no `.sym` for that build exists
in R2 or on a GitHub release, and none ever will.

Two checks run before an issue is filed:

1. **Shape** — `app_version` must be semver-shaped (`isVersionShapeValid`). Failures
   get `400`. This also bounds the string that lands in the issue title.
2. **Published release** — `isKnownRelease()` asks GitHub whether tag `v<version>`
   exists. If it definitively does not, the report is accepted with `202
   {"status": "ignored"}` and logged, but no issue is created.

The tag check uses `git/matching-refs` (not a direct ref lookup) because "no such
tag" comes back as `200` + an empty array, which is distinguishable from a
permission or transport failure. Every other outcome **fails open** and files the
issue — a GitHub outage must never silently drop a real crash.

Prompted by #1240: a `v0.1.4` report from a Pi. `VERSION.txt` has never held that
value (history runs 1.1.0 → 0.9.0 … 0.13.x → 0.95.x … 0.99.x), and the reported
27 MB text segment doesn't match any release build, so it came from an
independently compiled binary.

### GitHub Issue Format

The worker creates issues with:
- **Title**: `Crash: SIGSEGV in v0.9.16`
- **Labels**: `crash`, `auto-reported`
- **Body**: Markdown with Crash Summary table, System Info table, Backtrace code block, Log Tail (collapsed)

### Deployment

```bash
cd server/crash-worker
npm install
wrangler secret put INGEST_API_KEY   # same key as telemetry
wrangler secret put GITHUB_TOKEN     # GitHub PAT
wrangler deploy
```

Custom domain: `crash.helixscreen.org` — configured via `[[routes]]` in `wrangler.toml`. Requires AAAA DNS record (`100::`) with Cloudflare proxy enabled on the `helixscreen.org` zone.

---

## Modal UI

### XML Component

`ui_xml/crash_report_modal.xml` — extends `ui_dialog` with:
- Warning icon + "HelixScreen Crashed" title
- Crash details bound to `crash_report_details` subject
- Status text bound to `crash_report_status` subject
- QR container (hidden via `crash_report_show_qr` subject, shown when QR is needed)
- Dismiss / Send Report buttons

### Subject Bindings

| Subject | Type | Purpose |
|---------|------|---------|
| `crash_report_details` | string | Signal, version, uptime display |
| `crash_report_status` | string | Status messages ("Sending...", "Sent!", etc.) |
| `crash_report_show_qr` | int | QR container visibility (0=hidden, 1=visible) |

### Callback Registration

Follows the standard modal pattern with static callbacks dispatching to `active_instance_`:

```cpp
lv_xml_register_event_cb(nullptr, "on_crash_report_send", on_send_cb);
lv_xml_register_event_cb(nullptr, "on_crash_report_dismiss", on_dismiss_cb);
```

### Lifecycle

1. `show_modal()` → registers callbacks, inits subjects, populates crash details, calls `Modal::show()`
2. User clicks "Send Report" → `attempt_delivery()` tries auto-send → QR → file
3. User clicks "Dismiss" → `consume_crash_file()` + `hide()`
4. `on_hide()` → nulls `active_instance_`, schedules `delete this` via `ui_queue_update()`

---

## Testing

### Running Tests

```bash
make test-run                              # All tests
./build/bin/helix-tests "[crash_reporter]" # Just crash reporter
```

### Interactive Testing

```bash
./build/bin/helix-screen --test --mock-crash -vv
```

The `--mock-crash` flag writes a synthetic crash.txt with a fake SIGSEGV before crash detection runs. Requires `--test` mode. This lets you test the full UI flow without actually crashing.

### Test Coverage

30 test cases across 7 categories:

| Category | Tests | What's Covered |
|----------|-------|----------------|
| Detection | 3 | `has_crash_report()`, `consume_crash_file()` |
| Report Collection | 6 | Signal parsing, backtrace, platform, hardware info |
| Log Tail | 4 | Last N lines, short files, missing/empty files |
| Report Formatting | 6 | JSON fields, log_tail array conversion, text output |
| GitHub URL | 4 | Valid URL, <2000 chars, truncation |
| File Save | 4 | Write, content match, success/failure returns |
| Singleton Lifecycle | 3 | Init, re-init, shutdown |

### Test Fixture

Tests use `CrashReporterTestFixture` which creates a temp directory, resets the singleton, and cleans up on destruction. Helper methods `write_crash_file()` and `write_log_file()` create synthetic test data.

---

## Resolving Backtraces

The backtrace addresses in crash reports are raw pointers. To resolve them to file/line:

```bash
scripts/resolve-backtrace.sh <version> <platform> <addr1> <addr2> ...
```

This downloads `.sym` files from the releases R2 bucket and uses `addr2line` to resolve. Symbols are cached in `~/.cache/helixscreen/symbols/`.

### Shared Library Addresses

Not all backtrace frames come from the main binary — some are in shared libraries (libc, libpthread, etc.). The crash-worker's symbol resolver (`server/crash-worker/src/symbol-resolver.ts`) detects these automatically:

- After computing `file_offset = address - load_base`, if the offset is negative or exceeds the last known symbol address (+ 1MB margin), the frame is labeled **`<shared library>`** instead of being resolved against the main binary's symbol table.
- This prevents garbage symbol names that previously appeared when libc addresses (e.g., `0x7fff80012345`) were force-matched to the nearest binary symbol.
- The Python telemetry tool (`scripts/telemetry-crashes.py`) has equivalent logic via `is_shared_lib_addr()`.

Shared library frames are expected in crash backtraces — they indicate the crash unwind passed through system code (e.g., `__libc_start_main`, signal handlers).

---

## Future Enhancements

- **~~Rate limiting~~**: Done — Worker uses CF Rate Limiting binding (5 requests/IP/60s).
- **Deduplication**: Hash signal+version+backtrace to detect duplicate crashes and comment on existing issues instead of creating new ones.
- **Printer/Klipper info**: Populate `printer_model` and `klipper_version` if Moonraker connection is available at crash report time.
- **Input sanitization**: Validate/truncate crash data fields in the worker before creating GitHub issues.
