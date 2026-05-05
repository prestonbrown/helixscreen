---
name: spdlog
description: >
  spdlog logging library knowledge for HelixScreen C++ development.
  Use when adding log statements, configuring log sinks, working with log levels,
  formatting log messages, or any logging-related code in HelixScreen.
  Includes HelixScreen-specific logging rules (mandatory spdlog usage, level
  semantics, prefix format, systemd journal integration, hlog sync).
  Trigger on any spdlog API call, log statement, or logging configuration.
---

# spdlog for HelixScreen

HelixScreen uses spdlog exclusively for all logging. **Never use printf, std::cout, or LV_LOG_***.

## Build Integration

- **Header-only mode**: spdlog used as header-only library from `lib/spdlog/`
- **System fallback**: Makefile checks for system spdlog first (`/usr/include`, `/usr/local/include`, `/opt/homebrew/include`)
- **Include**: `#include <spdlog/spdlog.h>` (+ specific sinks as needed)
- **Flags**: SPDLOG_FMT_EXTERNAL=0 (uses bundled fmt)

## HelixScreen Log Level Rules

| Level | CLI Flag | When to Use |
|-------|----------|-------------|
| `spdlog::error` | (always) | Unrecoverable failures, NULL pointers |
| `spdlog::warn` | (always) | Recoverable issues, guards, fallbacks |
| `spdlog::info` | `-v` | User-visible milestones only |
| `spdlog::debug` | `-vv` | Troubleshooting summaries |
| `spdlog::trace` | `-vvv` | Per-item loops, wire protocol, observer plumbing |

## Message Format

**MANDATORY**: `[ComponentName]` prefix in brackets:

```cpp
// Correct
spdlog::info("[Home Panel] Setup complete!");
spdlog::debug("[Theme] Auto-registered {} color pairs", count);
spdlog::trace("[Moonraker Client] send_jsonrpc: {}", rpc.dump());

// WRONG - never use these patterns:
spdlog::debug("Theme: Auto-registered");  // colon, not brackets
spdlog::info("[Home Panel]: Setup");       // double colon
spdlog::debug("setup done");               // no prefix
```

### Prefix Rules
- `[ClassName]` for classes: `[MoonrakerClient]`, `[AbortManager]`
- `[Feature Name]` for multi-word: `[Home Panel]`, `[Print Select Panel]`
- `[Subsystem]` for subsystems: `[Theme]`, `[AMS AFC]`

## Loop Logging Pattern

```cpp
int registered = 0;
for (const auto& item : items) {
    spdlog::trace("[Theme] Registering {}: value={}", item.name, item.value);
    registered++;
}
spdlog::debug("[Theme] Auto-registered {} items", registered);
```

## Sink Configuration (logging_init.cpp)

HelixScreen configures sinks based on platform at runtime:

| Platform | Default Sink | Fallback |
|----------|-------------|----------|
| Linux + systemd | `systemd_sink_mt` (journal) | `syslog_sink_mt` |
| Linux (no systemd) | `syslog_sink_mt` | Console |
| Android | `android_sink_mt` (logcat) | Console |
| macOS/other | `stdout_color_sink_mt` | Console |

- **File logging**: `rotating_file_sink_mt` — 5MB max, 3 rotated files
- **Log path**: `/var/log/helix-screen.log` → `~/.local/share/helix-screen/helix.log` → `/tmp`
- **Early init**: `init_early()` creates WARN-level console logger before full init
- **LVGL assert handler**: Bridges LVGL assertions to spdlog with backtrace dump

## Common Patterns

### Initialization Guards
```cpp
if (initialized_) {
    spdlog::warn("[MyComponent] init_subjects() called twice - ignoring");
    return;
}
```

### Error Handling
```cpp
if (!ptr) {
    spdlog::error("[MyComponent] Cannot process: NULL pointer");
    return;
}
```

### NOT INFO Examples
- Navigation events → DEBUG
- Per-item updates → DEBUG or TRACE
- Internal wiring → TRACE

## Reference Files

| Topic | File |
|-------|------|
| Quick start guide | `references/QuickStart.md` |
| Available sinks | `references/Sinks.md` |
| Custom formatting | `references/Custom-formatting.md` |
| Async logging | `references/Asynchronous-logging.md` |
| Configuration tweaks | `references/Tweaking.md` |
| Thread safety | `references/Thread-Safety.md` |
| FAQ | `references/FAQ.md` |
