# Logging Guidelines

This document defines the logging standards for HelixScreen. All new code should follow these patterns; existing code will be migrated incrementally.

## Log Levels

| Level | CLI Flag | Purpose | Examples |
|-------|----------|---------|----------|
| **ERROR** | (always) | Unrecoverable failures | "Failed to connect", "NULL pointer" |
| **WARN** | (always) | Recoverable issues, guards | "Double init detected", "Fallback used" |
| **INFO** | `-v` | User-visible milestones | "Connected to Moonraker", "Setup complete" |
| **DEBUG** | `-vv` | Troubleshooting info | Component init summaries, state changes |
| **TRACE** | `-vvv` | Wire-level details | Per-item loops, JSON-RPC protocol, observer plumbing |

## When to Use Each Level

### ERROR
- Unrecoverable failures that require user action
- NULL pointer dereferences (guarded)
- Missing critical configuration
- Failed operations that cannot proceed

### WARN
- Recoverable issues with automatic fallbacks
- Guard checks (double initialization, missing optional components)
- Deprecated API usage
- Situations that might indicate a problem but don't prevent operation

### INFO
Use INFO sparingly for **milestones the user cares about**:
- System startup completion: "HelixScreen UI Prototype"
- Connection events: "✓ Connected to Moonraker"
- Discovery summaries: "Capabilities: QGL, bed_mesh, chamber_sensor"
- Panel setup completion: "[Home Panel] Setup complete!"
- Major operations: "File list updated: 126 G-code files"

**NOT INFO** (use DEBUG or TRACE instead):
- Navigation events: "Switched to panel 2" → DEBUG
- Mock backend operations: "Returning mock file list" → DEBUG
- Per-item updates: "Updated slot 0 info" → DEBUG
- Internal wiring: "Queuing switch to panel" → DEBUG

### DEBUG
Use DEBUG for **troubleshooting information**:
- Component initialization summaries: "Subjects initialized"
- Configuration details: "Target: 800x480, DPI: 160"
- State change summaries: "Printer connection state changed: Connected"
- Batch operation summaries: "Auto-registered 21 theme-aware color pairs"
- Discovery details: "Detected probe: probe"

**NOT DEBUG** (use TRACE instead):
- Per-widget XML apply: "Applied size preset: 64x32" → TRACE
- Per-file metadata: "Using cached thumbnail: X.png" → TRACE
- RPC method calls: "Mock send_jsonrpc: method" → TRACE
- File listing results: "Found 11 files", "Directory has X items" → TRACE
- UI state toggles: "Overlay backdrop visibility set to: true" → TRACE
- Spoolman lookups: "get_spoolman_spool(1) -> Polymaker PLA" → TRACE
- Theme file parsing: "Parsing X.json in legacy format" → TRACE

### TRACE
Use TRACE for **deep debugging only**:
- Per-item processing in loops: "Registering color graph_bg: selected=#2D2D2D"
- Wire protocol: "send_jsonrpc: {...}", "Registered request 5 for method X"
- Observer/callback registration: "Registering observer on X at 0x..."
- Subject value changes: "Subject value now: 2"
- Per-widget creation: "Created widget for slot 3"

## Message Format

### Prefix Standard: `[ComponentName]`

**Correct:**
```cpp
spdlog::debug("[Theme] Auto-registered 21 color pairs");
spdlog::info("[Home Panel] Setup complete!");
spdlog::trace("[Moonraker Client] send_jsonrpc: {}");
```

**Incorrect:**
```cpp
spdlog::debug("Theme: Auto-registered 21 color pairs");  // NO colon
spdlog::info("[Home Panel]: Setup complete!");           // NO double colon
spdlog::debug("Home Panel] Setup complete!");            // Missing bracket
```

### Prefix Naming Rules

| Pattern | Use For | Example |
|---------|---------|---------|
| `[ClassName]` | Classes | `[MoonrakerClient]`, `[PrinterState]` |
| `[Feature Name]` | Multi-word features | `[Home Panel]`, `[Print Select Panel]` |
| `[Subsystem]` | Subsystems | `[Theme]`, `[AMS AFC]` |

### Message Content

- **Be specific**: Include relevant data (counts, states, identifiers)
- **Be consistent**: Use the same wording for similar events
- **Be actionable**: For errors/warnings, include what went wrong

**Good:**
```cpp
spdlog::info("[PrinterState] Initialized 6 fans (version 1)");
spdlog::debug("[Controls Panel] Populated 3 secondary fans");
spdlog::trace("[Theme] Registering spacing space_lg: selected=16");
```

**Bad:**
```cpp
spdlog::info("fans initialized");  // No context
spdlog::debug("Done");             // Not actionable
```

## Loop Logging Pattern

When logging inside loops, use TRACE for per-item and DEBUG for summaries:

```cpp
int registered = 0;
for (const auto& item : items) {
    spdlog::trace("[Theme] Registering {}: value={}", item.name, item.value);
    // ... registration logic ...
    registered++;
}
spdlog::debug("[Theme] Auto-registered {} items", registered);
```

## Common Patterns

### Initialization Guards
```cpp
if (initialized_) {
    spdlog::warn("[MyComponent] init_subjects() called twice - ignoring");
    return;
}
// ... initialization ...
spdlog::debug("[MyComponent] Subjects initialized");
```

### Error Handling
```cpp
if (!ptr) {
    spdlog::error("[MyComponent] Cannot process: NULL pointer");
    return;
}
```

### Milestone Completion
```cpp
// After significant setup work
spdlog::info("[MyComponent] Setup complete!");
```

### Wire Protocol
```cpp
spdlog::trace("[Moonraker Client] send_jsonrpc: {}", rpc.dump());
spdlog::trace("[Moonraker Client] Registered request {} for method {}", id, method);
```

## Implementation Notes

- Use `spdlog` exclusively (not `printf`, `std::cout`, or `LV_LOG_*`)
- Every log line carries the emitting **thread id** and (on console/file) an ms timestamp — see "Per-Sink Log Patterns" below
- For errors that should notify the user, use `NOTIFY_ERROR()` macro

---

# Log Destinations & Retrieval

There are **two independent log streams** on every deployment:

1. **Structured app log** — every `spdlog::*` call from helix-screen, helix-watchdog, helix-splash. Routed through spdlog sinks.
2. **Launcher subshell capture** — `helix-launcher.sh` echoes, libc/glibc abort output, anything written to stdout/stderr before `init_logging()` runs. Captured by the SysV init script's `>> "$LOGFILE" 2>&1` redirect. Never sees per-line spdlog output (see "Console sink" below).

These two streams do **not** overlap. spdlog never writes to the launcher file (in daemon mode); the shell redirect never sees structured spdlog calls. They are complementary diagnostics, not redundant.

## spdlog Sinks & Auto-Detection

Logging is brought up in **two phases**, both implemented in `src/system/logging_init.cpp`:

1. `helix::logging::init_early()`, called at the top of `Application::run()` before any
   startup phase, so nothing can log into a void.
2. `helix::logging::init(config)`, called from `Application::init_logging()` in **Phase 3**,
   once CLI args (Phase 1) and `settings.json` (Phase 2) have been read. This builds the
   full sink set for the resolved target.

Everything below the "Target Resolution" heading describes phase 2. Phase 1 and the handoff
between them are in "Ring-Buffer Sink Lifecycle".

### Target Resolution

The runtime target is set by precedence (highest → lowest):

1. `--log-dest=<dest>` CLI flag (passed through by `helix-launcher.sh`)
2. `HELIX_LOG_DEST` env var — read **twice, independently**: `helix-launcher.sh` translates it into `--log-dest=`, and `Application::init_logging()` also reads it directly via `helix::logging::log_env_override()`. The direct read is what makes the variable work under a systemd unit's `Environment=`, a hand-run binary, or a third-party init script that execs helix-screen without our launcher (#1249). An unrecognized value warns and is ignored — never fatal, because a typo in `helixscreen.env` must not crash-loop an appliance.
3. `/log_dest` in `settings.json`
4. `LogTarget::Auto` (default) → resolved by `detect_best_target()`:
   - **`HELIX_PLATFORM_ANDROID`** → `Android` (`__android_log_print`)
   - **Linux with `HELIX_HAS_SYSTEMD` AND `/run/systemd/journal/socket` exists** → `Journal` (`systemd_sink_mt`)
   - **Other Linux** → `Syslog` (`syslog_sink_mt` via `libc syslog()`)
   - **macOS/other** → `Console` (`stdout_color_sink_mt`)

Valid `LogTarget` values: `auto`, `journal`, `syslog`, `file`, `console`, `android`.

### Per-Sink Log Patterns

Each sink gets its **own** spdlog pattern (applied via `sink->set_pattern()` right after construction — not a global `spdlog::set_pattern()`, since the formats differ). The pattern strings come from the pure helper `helix::logging::pattern_for_sink(SinkKind)` in `logging_init.h`, so the format decision is unit-testable without constructing real sinks (`tests/unit/test_log_pattern.cpp`, tag `[logging][pattern]`).

| Sink (`SinkKind`) | Pattern | Notes |
|---|---|---|
| Console (`stdout_color_sink`) | `[%H:%M:%S.%e] [%^%l%$] [%t] %v` | ms timestamp, colored level, thread id |
| File (`rotating_file_sink`) | `[%H:%M:%S.%e] [%^%l%$] [%t] %v` | same string — `%^…%$` are no-ops on the non-color file sink |
| journald (`systemd_sink`) | `[%l] [%t] %v` | **no time token** — journald stamps its own time |
| syslog (`syslog_sink`) | `[%l] [%t] %v` | **no time token** — syslog stamps its own; `%l` kept for grep-ability of `/var/log/messages` |
| Android (`android_sink`) | `[%t] %v` | logcat adds its own timestamp/level/tag metadata |
| Crash breadcrumb (`CrashErrorLogSink`) | `[%H:%M:%S.%e] [%l] [%t] %v` | feeds crash context; the ring actually stores `msg.payload`, so this pattern is for any other consumer of the stream |

**Why the thread id (`%t`) is on every sink:** the worst crash family in this codebase (L081 / async-delete) is about main-thread-vs-background-thread (WebSocket / HTTP worker) confusion. Knowing which thread emitted a line is the single highest-value field for diagnosing it. The `[logging][pattern]` test fails if `%t` is dropped from any sink or if a time token is added to the system sinks (which would double-stamp the journal/syslog clock).

A console/file line now looks like:

```
[14:32:07.918] [debug] [140351827234560] [PrinterState] Initialized 6 fans (version 1)
```

### Ring-Buffer Sink Lifecycle

The in-memory ring (`MonotonicRingSink`, process-global `g_ring_sink`) is what the debug
bundle's `log_tail` and `helix-screen ctl log` read. It is installed by **`init_early()`**,
and `init()` **adopts that same instance** rather than constructing a new one.

That ordering is the whole point. `Application::run()` does Phase 2 `init_config()` before
Phase 3 `init_logging()`, so the entire config diagnostic trail - corrupt `settings.json`,
restore-from-backup, parse failures, migration output - is emitted while only the early
logger exists. With the ring created in `init()`, every one of those lines was already gone
by the time a ring existed, and no bundle could ever show them. Bundle XGVDYEB5 is the case:
a user-visible "settings were corrupted, restored from backup" toast, and not one line of
the config trail anywhere in a 20,000-line `log_tail`.

| | `init_early()` | `init(config)` |
|---|---|---|
| Console sink level | Pinned at WARN (`EARLY_CONSOLE_LEVEL`) | `config.level` |
| Ring sink level | `debug`, or WARN when `HELIX_BUNDLE_LOG_DEBUG=0` | `debug`, or `config.level` when `HELIX_BUNDLE_LOG_DEBUG=0` |
| Logger floor | The more verbose of the two above | Same rule |
| Ring instance | Created | Adopted if one exists, else created |

Three things fall out of this that are easy to get wrong:

- **The early console sink must pin its level explicitly.** The logger floor now drops to
  debug so the ring can capture it, and spdlog gates at the logger *before* any sink sees a
  message - a sink's own default level is trace. Without the explicit pin the early console
  would start echoing debug to stdout, which a daemonized launch redirects straight into the
  journal or the log file. Stdout volume is unchanged from before the ring moved.
- **`HELIX_BUNDLE_LOG_DEBUG=0` still works, with one wrinkle.** Before `init()` there is no
  configured level to fall back to, so the early ring matches the console at WARN. That
  leaves the early phase behaving exactly as it did when no early ring existed at all.
- **Adoption is one-shot.** The flag is cleared on adoption, so a *later* `init()` rebuilds
  the ring exactly as it always did. Production calls `init()` once
  (`Application::init_logging`, `helix_watchdog`), but tests re-initialize the logger
  constantly and rely on each `init()` handing them a clean buffer. The watchdog build has
  no early phase at all, so `init()` creates the ring there.

Adopting rather than replacing also keeps `set_runtime_level()`'s identity check against
`g_ring_sink` valid, keeps `tail_ring_buffer()`'s `shared_ptr` copy pointing at live data
across the handoff, and keeps the sink's clock-step detector's memory continuous.

Capacity (`HELIX_LOG_RING_LINES`, else scaled from total RAM) is resolved in `init_early()`.
That is safe this early: `resolve_ring_capacity()` reads only the env var and
`/proc/meminfo`, neither of which needs Config. The cost is one extra
`PlatformCapabilities::detect()` at startup, not extra memory - there is still exactly one
ring.

### Console Sink (Stdout) — When It's Attached

The console sink is **opt-in by detection**. The whole decision is the pure function `helix::logging::should_add_console(target, enable_console, force_console, test_mode, stdout_kind)` (`logging_init.cpp`), which `init()` calls with `classify_stdout()` — the only part that touches the real fd. `classify_stdout()` distinguishes **Tty / Pipe / File / Socket / Other**, which is more than `isatty()` can tell you and is what the gate below turns on.

| Resolved target | Console sink? |
|---|---|
| `Console` | Always — it's the only sink |
| `Android` | Never — stdout is invisible to logcat, and `test_mode` does not override this |
| `Syslog` / `Journal` / `File` | `test_mode` (`--test`) → **any** stdout kind. Otherwise: a **Tty** always; a **Pipe** only with `force_console`; a regular **File**, **Socket**, or **Other** → never |

`enable_console = false` vetoes every row.

`force_console` is set by an explicit `-v`/`--log-level`, or by `HELIX_LOG_LEVEL` (which the launcher also translates into `--log-level=`, so it has always reached this flag on a launcher-started device).

What that yields in practice:
- `./build/bin/helix-screen --test -vv` from a shell → console sink on, colored output to terminal.
- `ssh -t pi 'helix-screen ...'` → tty allocated → console sink on.
- `helix-screen -vv | tee run.log` → pipe + `force_console` → console sink **on**. A human is watching through `tee`; discarding the output there was #1105.
- `helix-screen --test > out.log` → regular file, but `test_mode` → console sink **on**.
- `helix-screen -vv > out.log` **without** `--test` → regular file, no test mode → console sink **off**. A plain redirect is indistinguishable from the daemon redirect, so this is an accepted tradeoff; use `| tee` instead.
- SysV daemon launch where stdout is redirected to a file → console sink **off**, even though the launcher passed `--log-level`.
- systemd daemon launch where stdout is a journald socket → console sink off (journald already has the structured stream via the journal sink).

The file/socket exclusions prevent the "double-log" mode that caused the Snapmaker U1 print failure where spdlog at trace wrote ~35 lines/sec to stdout, the init script captured stdout to a tmpfs file, and 498 MB filled `/tmp`. They are also why the ZMOD AD5X launcher redirect (`>> /opt/config/mod_data/log/helixscreen.log 2>&1`) contains no app log: stdout there is a regular file.

### Reading `--test` Logs When stdout Isn't a TTY

`--test` sets `LogConfig::test_mode`, and `should_add_console()` attaches the console sink for **any** stdout kind under test mode — pipe, regular file, socket, background run, non-interactive agent shell. So `./build/bin/helix-screen --test -vv > run.log 2>&1 &` captures the logs in `run.log`, and `| tee` / `| grep` work too. That is safe because `--test` never runs in production: no systemd unit, init script, procd shim, or launcher passes it, so test mode cannot reach a daemonized double-log path.

Without `--test` the narrower gate applies (see the table above): a pipe needs `-v`/`--log-level` to force the sink, and a plain `> file` redirect gets nothing. Read the resolved system sink instead:

```bash
HELIX_MOCK_PRINTER=ad5m ./build/bin/helix-screen -v &   # note: no --test
sleep 6 && kill %1
journalctl --since "1 min ago" | grep helix          # all lines
journalctl --since "1 min ago" | grep '\[PrinterDetector\]'   # one subsystem
```

`--log-dest=file` is a workable dev escape hatch too. With no `--log-file`, `resolve_log_file_path()` probes `/var/log` for writability and falls back to `$XDG_DATA_HOME/helix-screen/helix.log` when it is not writable, so a non-root run lands in `~/.local/share/helix-screen/helix.log` rather than failing. An explicit `--log-file` at an unopenable path is not fatal either: the sink construction is caught, and the platform's normal system sink takes over with a warning.

Verbosity still applies (`-v`=info, `-vv`=debug, `-vvv`=trace). Detection lines such as `[PrinterState] Printer type set to: '…'` are **info-level**, so `-v` is enough to see them.

### Per-Platform Routing Summary

| Platform | spdlog target (default) | Where structured logs land | How to read |
|---|---|---|---|
| Raspberry Pi (systemd) | Journal | systemd journal | `journalctl -u helixscreen -f` |
| x86/x86_64 (systemd) | Journal | systemd journal | `journalctl -u helixscreen -f` |
| Snapmaker U1 (Debian Trixie, SysV) | Syslog | `/var/log/messages` (rsyslogd persists on overlay) | `grep helix-screen /var/log/messages` or `journalctl -t helix-screen` if journald-only |
| AD5M Forge-X/KMod (BusyBox SysV) | **File** (hook) | `/data/helixscreen/logs/helix.log` | `tail -f /data/helixscreen/logs/helix.log` |
| AD5M ZMOD / AD5X (ZMOD MIPS, BusyBox) | **File** (hook) | `/opt/config/mod_data/log/helix.log` — under `/opt/config` so ZMOD's `TAR_CONFIG` archives it (#1249) | `tail -f /opt/config/mod_data/log/helix.log` |
| Creality K1/K1C (BusyBox, in-memory syslog) | **File** (hook) | `/usr/data/helixscreen/logs/helix.log` | `tail -f /usr/data/helixscreen/logs/helix.log` |
| Creality K2 (BusyBox procd) | **File** (hook) | `/mnt/UDISK/helixscreen/logs/helix.log` | `tail -f /mnt/UDISK/helixscreen/logs/helix.log` |
| Elegoo CC1 / COSMOS (BusyBox) | **File** (hook) | `/user-resource/helixscreen/logs/helix.log` | `tail -f /user-resource/helixscreen/logs/helix.log` |
| SonicPad (Debian) | Syslog | `/var/log/syslog` | `grep helix-screen /var/log/syslog` |
| Android | Android | logcat | `adb logcat -s HelixScreen` |
| Dev workstation (macOS / interactive Linux) | Console | stdout in terminal | visible directly |

"**File** (hook)" means `platform_pre_start` in that platform's
`assets/config/platform/hooks-*.sh` exports `HELIX_LOG_DEST=file` +
`HELIX_LOG_FILE` (plus a 1–2 MiB × 3 rotation cap). Without the hook these
targets would all fall back to `detect_best_target()` → `Syslog`, which on the
BusyBox boxes is an in-memory ring that dies with the reboot you are trying to
diagnose. `logread | grep helix-screen` still shows anything logged before the
hook takes effect, and remains the right command on Snapmaker U1 / SonicPad.

### Debugging the C / libhv Layer On-Device

spdlog is C++-only. When you need temporary instrumentation **inside a C
dependency** (libhv, the DNS resolver in `lib/libhv/base/`), do **not** rely on
`fprintf(stderr)` — the app manages its own stdout/stderr and a raw stderr write
may not be captured. Use **`syslog(3)`** (`#include <syslog.h>`,
`syslog(LOG_WARNING, "[TAG] ...")`); it lands in the platform's syslog
(`/var/log/messages`, `logread`, etc.) right alongside spdlog's syslog sink, with
no plumbing. spdlog's own warn/error (fd1/syslog) is reliably captured;
`--log-dest=console -vv` forces the console sink for redirected/non-tty runs.

**Trap:** before trusting *absence* of instrumentation output, confirm the code
is actually in the deployed binary: `strings <binary> | grep <MARKER>`. A patched
file compiled into a static `.a` (e.g. `hsocket.o` in `libhv.a`) can be a stale
cached object that never picked up your edit — "no log output" then means "not
compiled in," not "not reached." See BUILD_SYSTEM.md § Patch Gotchas.

## Launcher Subshell Capture (`launcher.log`)

`config/helixscreen.init` runs the launcher in a backgrounded subshell with `( ... exec "$LAUNCHER" ) >> "$LOGFILE" 2>&1 &`. Everything written to stdout/stderr inside that subshell ends up in `$LOGFILE`:

- `[helix-launcher] ...` shell echoes (binary selection, splash detection, log level, exit code)
- glibc abort output: `*** glibc detected ***`, `MALLOC_CHECK_=3` diagnostics, `std::terminate without active exception` — happens **after** spdlog is dead and can be the only diagnostic on a crash
- Early-init spdlog console output before `Application::init_logging()` swaps to the real logger (a handful of lines, warn-level)
- LVGL warnings emitted before the LVGL log handler is wired up

**It is NOT the structured app log.** For that, use the syslog/journal commands above.

### Path resolution

`LOGFILE` is chosen at script-start by `config/helixscreen.init`:

1. Probe `/var/log` — writable AND its backing filesystem (from `/proc/mounts`) isn't `tmpfs` or `ramfs` → `LOGFILE="/var/log/helixscreen/launcher.log"` (the FHS-conforming location).
2. Otherwise (tmpfs/ramfs `/var/log`, or `/var/log` not writable) → `LOGFILE="${DAEMON_DIR}/logs/launcher.log"` where `DAEMON_DIR` is the install directory (always persistent ext4 on supported targets).

| Platform | Resolved path |
|---|---|
| Raspberry Pi | `/var/log/helixscreen/launcher.log` (systemd platforms don't use this script — uses `ExecStart` via systemd journal instead) |
| Snapmaker U1 | `/var/log/helixscreen/launcher.log` (overlay's upperdir is persistent ext4) |
| AD5M Forge-X/KMod | `/opt/helixscreen/logs/launcher.log` (`/var/log` is tmpfs on BusyBox) |
| K1 / K1C | `/usr/data/helixscreen/logs/launcher.log` |
| K2 | `/usr/data/helixscreen/logs/launcher.log` |
| AD5X (ZMOD) | `/opt/config/mod_data/log/helixscreen.log` — ghzserg ships their own fork of the init script with `LOGFILE` hardcoded, so the `/var/log` probe above never runs. `INSTALL_DIR` is `/srv/helixscreen`, not `/usr/data/helixscreen`. The **app** log is a separate file, `/opt/config/mod_data/log/helix.log` (see below) |
| CC1 / COSMOS | `/user-resource/helixscreen/logs/launcher.log` |

Size is capped at 5 MB at every `start` (the init script truncates if larger). This is belt-and-suspenders; the file shouldn't grow that big now that the console sink is gated off in daemon mode.

### Pre-v0.99.62 (legacy) path

Older installs wrote to `/tmp/helixscreen.log` on every SysV target. Debug bundles still scan this path for backward compatibility (`src/system/log_collector.cpp:default_file_paths()`).

## Controlling Log Verbosity

### CLI

| Flag | Effect |
|---|---|
| `-v` | INFO and above |
| `-vv` | DEBUG and above |
| `-vvv` | TRACE and above |
| `--log-level=trace\|debug\|info\|warn\|error\|critical\|off` | Explicit level (overrides `-v`) |
| `--log-dest=auto\|journal\|syslog\|file\|console\|android` | Override destination |
| `--log-file=<path>` | Override path when `--log-dest=file` |

### Env / config

`helixscreen.env` (in `<install_dir>/config/`, but on Klipper platforms it's symlinked to `~/printer_data/config/helixscreen/helixscreen.env`):

```
HELIX_LOG_LEVEL=warn   # trace, debug, info, warn, error, critical, off
HELIX_LOG_DEST=auto    # auto, journal, syslog, file, console
HELIX_LOG_FILE=        # path; only used when HELIX_LOG_DEST=file
```

Precedence: `--log-*` CLI flag > `HELIX_LOG_*` env > `/log_*` in `settings.json` > defaults (production: `warn`; test mode: `debug`).

The env tier is enforced in C++ (`Application::init_logging()` → `helix::logging::log_env_override()` / `resolve_log_setting()`), not just by the launcher's flag translation, so it holds however the binary was started. Validation is shared with the CLI parser (`is_valid_log_target()` / `is_valid_log_level()` in `logging_init.h`) so the accepted sets cannot drift, but the two disagree on what to do with a bad value on purpose: a bad **flag** is fatal (the user is at a prompt and can retry), a bad **env value** logs a warning and falls through to the next tier (a typo in `helixscreen.env` must not turn every boot of an appliance into a crash-loop).

Note the platform hooks are the real source of `HELIX_LOG_DEST` / `HELIX_LOG_FILE` on most embedded targets — six of the seven `assets/config/platform/hooks-*.sh` export them from `platform_pre_start`. `helix-launcher.sh` therefore resolves `LOG_DEST`/`LOG_FILE`/`LOG_LEVEL`/`DEBUG_MODE` **after** sourcing the hooks; resolving before them read unset variables (#1249).

### Runtime (in-app)

The Settings → System → Log Level dropdown calls `helix::logging::set_runtime_level()`, which is `spdlog::set_level()` globally + persists to `settings.json`. This survives restart via the precedence chain (config-file level applies on next launch if no CLI/env override is set).

### Klipper-style helixscreen.env convention

On Klipper-based platforms (Pi, AD5M, K1, K2, Snapmaker U1, etc.), `setup_config_symlink()` in `scripts/lib/installer/platform.sh` creates the canonical env file at `~/printer_data/config/helixscreen/helixscreen.env` (visible/editable from Mainsail/Fluidd) and symlinks `<install_dir>/config/helixscreen.env` → that path. Edit either location; both point to the same file. The launcher only reads the install-dir copy, so a broken symlink silently makes user edits invisible — `make deploy-<platform>` now verifies and repairs the symlink during deploy.

## Debug Bundles

`DebugBundleCollector::collect()` (`src/system/debug_bundle_collector.cpp`) assembles a debug bundle on user request (Settings → About → Generate Debug Bundle, or the `helix_debug` Moonraker shell command), reading the on-disk log cascade through `src/system/log_collector.cpp`. It captures:

- `log_tail` - the structured app log, read from the in-memory ring rather than from disk (`src/system/debug_bundle_collector.cpp`), so it is always the live process and always fresh. Because the ring is installed in `init_early()` it reaches back past Phase 2 config load; see "Ring-Buffer Sink Lifecycle". The companion `log_meta` field records the active sink target, the level the persistent sinks were configured at (which the ring may have been more verbose than), and whether the tail came from the live ring or the on-disk fallback
- Last N lines from each candidate launcher-log path (`/var/log/helixscreen/launcher.log`, `${install_dir}/logs/launcher.log`, legacy `/tmp/helixscreen.log`, etc.)
- Last N lines of syslog (`/var/log/messages`, `/var/log/syslog`)
- systemd journal entries when available (`journalctl -u helixscreen`)
- Crash report (if recent)
- `settings.json` (sanitized)
- `printer_config` — Klipper's `printer.cfg` and every config it `[include]`s, fetched from
  `/server/files/config/<path>` and sanitized per line (see below)
- `filament_system.gcode_macros` — bare `gcode_macro` names from `/printer/objects/list`

### Why `printer_config` is its own field

Klipper re-dumps `printer.cfg` into `klippy.log` on every start, so the log tail used to carry it
for free. It no longer does: every line of a config is a unique shape, so it survived
`condense_klipper_log()`'s shape-collapse intact and spent the whole line budget on config —
84/63/58% of `klipper_log` on AD5X bundles `4QA7SZAM` / `LYGVE39Y` / `XSNN7PX5`. `ce4f21914` added
`strip_klipper_config_dumps()` to elide it so the incident window survives.

Fetching the files into a separate field gives the content back without putting it back in
competition with the incident, and beats the log copy anyway — that one arrives head-truncated
whenever the byte window slices through the dump.

Budget: `MAX_CONFIG_BYTES` (512 KB) and `MAX_CONFIG_FILES` (40). A full ZMOD AD5X config is ~250 KB
across its includes, so it fits whole. Hitting either cap records `printer_config.truncated` with
the reason rather than silently shortening.

### Sanitizing config bodies

Config bodies go through `sanitize_text_block()`, which applies `sanitize_value()` **per line**.
This is not interchangeable with calling `sanitize_value()` on the file: that function replaces any
single string over 4 KB with `[REDACTED_LONG_VALUE]`, so a whole-file call would redact every
config in the bundle. Line granularity still catches what actually turns up in a `printer.cfg` —
notification macros carrying Pushover/Telegram tokens, camera and Spoolman URLs with embedded
credentials, emails, MACs.

What it does **not** redact: filesystem paths, so an `[include /home/<user>/printer_data/…]` ships
the username. That is the same exposure `update.install_root` has always had, not a new class, but
it is worth knowing before pasting a config excerpt into a public issue. The existing rule applies
unchanged — scrub bundle excerpts before they go anywhere public.

Bundles are uploaded to `crash.helixscreen.org` with an 8-char uppercase alphanumeric share code
(e.g. `UK9QCFY3`, `CGR6C7PA`). The code is minted server-side; nothing in this repo validates its
length, so treat the server as the authority if that ever changes. Retrieval: `./scripts/debug-bundle.sh <CODE> --save`. Note that `--save` writes `debug-bundle-<code>.json` to the current working directory — run it from `/tmp` (`cd /tmp && …`) so bundles never land in the repo, and don't commit them.

---

## Related Documentation

- `DEVELOPMENT.md#contributing` - Code standards
- `DEVELOPMENT.md` - Build and debug workflow
- `CLAUDE.md` - AI assistant rules (includes verbosity flags)
- `ENVIRONMENT_VARIABLES.md` - All `HELIX_*` env vars including `HELIX_LOG_LEVEL`
- `CRASH_REPORTER.md` - How crashes are captured and shipped
- `../user/TROUBLESHOOTING.md` - User-facing guide to reading logs
