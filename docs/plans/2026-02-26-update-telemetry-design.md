# Update Telemetry Events

Track in-app update outcomes (success and failure) via the existing anonymous, opt-in telemetry system.

## Problem

The in-app updater has 10+ failure paths with zero telemetry. Failures are logged to spdlog but invisible to us unless a user reports them. We can't measure update success rates or identify systemic issues (e.g., corrupt downloads on a specific platform).

## Event Schemas

### `update_failed`

Fired immediately from each failure path in `do_download()` / `do_install()`.

```json
{
  "schema_version": 2,
  "event": "update_failed",
  "device_id": "...",
  "timestamp": "...",
  "reason": "corrupt_download",
  "version": "0.14.0",
  "from_version": "0.13.4",
  "platform": "ad5m",
  "http_code": 200,
  "file_size": 1048576,
  "exit_code": null
}
```

**Reason values** (short, enum-like strings):

| Reason | Failure path |
|--------|-------------|
| `print_in_progress` | Tried to update while printing |
| `no_cached_update` | No update info available |
| `no_disk_space` | No writable dir with space |
| `download_failed` | libhv returned 0 |
| `file_too_small` | <1MB |
| `file_too_large` | >150MB |
| `corrupt_download` | gunzip -t failed |
| `wrong_architecture` | ELF mismatch |
| `installer_not_found` | install.sh missing |
| `install_failed` | install.sh non-zero exit |
| `install_timeout` | install.sh exceeded 120s |

Fields `http_code`, `file_size`, `exit_code` included only when relevant (null otherwise).

### `update_success`

Recorded on next boot via persist-and-detect (same pattern as crash reporting).

```json
{
  "schema_version": 2,
  "event": "update_success",
  "device_id": "...",
  "timestamp": "...",
  "version": "0.14.0",
  "from_version": "0.13.4",
  "platform": "ad5m"
}
```

## Implementation

### TelemetryManager

New public methods:
- `record_update_failure(reason, version, platform, http_code, file_size, exit_code)` — builds JSON, calls `enqueue_event()`
- `record_update_success(version, from_version, platform)` — builds JSON, calls `enqueue_event()`
- `check_previous_update()` — called from `init()` after `check_previous_crash()`, reads/deletes flag file

New private helpers:
- `build_update_failed_event(...)` — returns JSON
- `build_update_success_event(...)` — returns JSON

### UpdateChecker

- Each `report_download_status(Error, ...)` gets a matching `record_update_failure()` call
- Before `_exit(0)` on success: write `config_dir/update_success.json` with `{version, from_version, platform, timestamp}`

### telemetry-analyze.py

Add update event section: counts by reason, success rate calculation.

### No backend changes

The telemetry endpoint accepts arbitrary event types (no whitelist).

## Constraints

- All events respect `is_enabled()` — no-op when telemetry is off
- `schema_version` stays at 2
- No user-visible strings added (internal telemetry only)
- Flag file cleaned up on read (same as crash.txt pattern)
