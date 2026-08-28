<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Telemetry

HelixScreen includes an optional, anonymous telemetry system that helps us understand how the software is used in the real world. This page explains exactly what is collected, how your privacy is protected, and how to control it.

## TL;DR

- Telemetry is **OFF by default**. You must opt in.
- Data is **fully anonymous**. We cannot identify you or your printer.
- You can **view, disable, and delete** your data at any time.
- No filenames, G-code, IP addresses, or personal information is ever collected.

---

## What Is Collected

When telemetry is enabled, HelixScreen collects the following types of events:

### Session Events

Recorded once per application launch. Helps us understand the hardware landscape.

| Field | Description | Example |
|-------|-------------|---------|
| `schema_version` | Event schema version | `2` |
| `event` | Event type identifier | `"session"` |
| `device_id` | Anonymized device identifier (see below) | `"a3f8c1..."` (64-char hex) |
| `timestamp` | ISO 8601 UTC timestamp | `"2026-02-08T14:30:00Z"` |
| `app.version` | HelixScreen version | `"1.0.0"` |
| `app.platform` | Hardware platform | `"rpi4"`, `"rpi5"`, `"x86_64"` |
| `app.display` | Display resolution | `"800x480"` |

### Print Outcome Events

Recorded when a print finishes (success, failure, or cancellation). Helps us track print success rates and understand failure patterns.

| Field | Description | Example |
|-------|-------------|---------|
| `schema_version` | Event schema version | `2` |
| `event` | Event type identifier | `"print_outcome"` |
| `device_id` | Anonymized device identifier | `"a3f8c1..."` (64-char hex) |
| `timestamp` | ISO 8601 UTC timestamp | `"2026-02-08T16:45:00Z"` |
| `outcome` | Print result | `"success"`, `"failure"`, `"cancelled"` |
| `duration_sec` | Total print duration in seconds | `3600` |
| `phases_completed` | Print start phases completed (0-10) | `10` |
| `filament_used_mm` | Filament consumed in millimeters | `1500.0` |
| `filament_type` | Filament material | `"PLA"`, `"PETG"`, `"ABS"` |
| `nozzle_temp` | Target nozzle temperature in degrees C | `215` |
| `bed_temp` | Target bed temperature in degrees C | `60` |

### Crash Reports

Recorded automatically when HelixScreen crashes. Picked up on the next startup. Helps us catch and fix regressions.

| Field | Description | Example |
|-------|-------------|---------|
| `schema_version` | Event schema version | `2` |
| `event` | Event type identifier | `"crash"` |
| `device_id` | Anonymized device identifier | `"a3f8c1..."` (64-char hex) |
| `timestamp` | ISO 8601 UTC timestamp | `"2026-02-08T12:00:00Z"` |
| `signal` | POSIX signal number | `11` |
| `signal_name` | Signal name | `"SIGSEGV"`, `"SIGABRT"`, `"SIGBUS"`, `"SIGFPE"` |
| `app_version` | HelixScreen version at time of crash | `"1.0.0"` |
| `uptime_sec` | Seconds since application started | `3600` |
| `backtrace` | Stack frame addresses (hex) | `["0x0040abcd", "0x0040ef01"]` |

### Update Events

Recorded when the in-app updater runs. Tracks whether updates succeed or fail, helping us identify update reliability issues across different platforms.

### Memory Snapshots

Recorded periodically (hourly) and at startup. Helps detect memory leaks on different platforms, especially resource-constrained devices.

| Field | Description | Example |
|-------|-------------|---------|
| `trigger` | What caused the snapshot | `"hourly"`, `"session_start"` |
| `uptime_sec` | Seconds since app started | `7200` |
| `rss_kb` | Physical memory used (KB) | `48320` |
| `vm_size_kb` | Virtual memory mapped (KB) | `142000` |

### Hardware Profile

Recorded once per session. Helps us prioritize feature development and hardware support.

| Field | Description | Example |
|-------|-------------|---------|
| `printer.detected_model` | Auto-detected printer model | `"Voron 2.4"` |
| `printer.kinematics` | Motion system type | `"corexy"` |
| `mcus.primary` | Main MCU chip | `"stm32h723xx"` |
| `extruders.count` | Number of extruders | `1` |
| `fans.total` | Total fan count | `5` |
| `fans.names` | Fan object names | `["fan", "heater_fan hotend_fan"]` |
| `sensors.temperature_names` | Temperature sensor names | `["chamber", "mcu_temp"]` |
| `leds.names` | LED/neopixel names | `["neopixel sb_leds"]` |
| `steppers.names` | Stepper motor names | `["stepper_x", "stepper_y"]` |
| `macros.names` | G-code macro names | `["PRINT_START", "CLEAN_NOZZLE"]` |
| `printer_objects` | Klipper object list | `["toolhead", "bed_mesh"]` |
| `capabilities.*` | Boolean feature flags | `true`/`false` |

Hardware names (fans, sensors, macros, etc.) are firmware-defined identifiers from your Klipper configuration — not personally identifiable. No serial numbers, hostnames, or unique identifiers are collected.

### Settings Snapshot

Recorded once per session. Helps us understand user preferences so we can choose better default settings.

| Field | Description | Example |
|-------|-------------|---------|
| `theme` | Light or dark theme | `"dark"` |
| `brightness_pct` | Display brightness | `80` |
| `locale` | Language setting | `"en"` |
| `animations_enabled` | Whether animations are on | `true` |

### Panel Usage

Recorded every 4 hours and when HelixScreen closes. Shows which screens are used most so we can focus improvement efforts. No specific actions or content are tracked. Periodic snapshots ensure data isn't lost if the application crashes.

| Field | Description | Example |
|-------|-------------|---------|
| `session_duration_sec` | Total time HelixScreen was open | `14400` |
| `panel_time_sec` | Time spent on each panel (seconds) | `{"home": 8000, "controls": 3200}` |
| `panel_visits` | Visit count per panel | `{"home": 42, "controls": 18}` |

### Performance Snapshots

Recorded every 4 hours alongside panel usage. Measures UI responsiveness so we can identify and fix slow panels across different hardware.

| Field | Description | Example |
|-------|-------------|---------|
| `frame_time_p50_ms` | Median frame render time (ms) | `8` |
| `frame_time_p95_ms` | 95th percentile frame time (ms) | `16` |
| `frame_time_p99_ms` | 99th percentile frame time (ms) | `28` |
| `dropped_frame_count` | Frames slower than 33ms | `42` |
| `total_frame_count` | Total frames rendered | `432000` |
| `worst_panel` | Panel with highest frame times | `"temperature"` |

No user content is included — just timing data about how smoothly the UI renders.

### Feature Adoption

Recorded once per session (5 minutes after startup). Tells us which built-in features are actually used, so we know where to invest development effort.

| Field | Description | Example |
|-------|-------------|---------|
| `features` | Boolean flags for each feature | `{"macros": true, "camera": false, ...}` |

Features tracked: macros, filament management, camera, console, bed mesh, input shaper, manual probe, spoolman, LED control, power devices, multi-printer, themes, timelapse, favorites, PID calibration, firmware retraction.

### Settings Changes

Recorded when you change a setting, batched over a 30-second window. Helps us understand which default settings people change most, so we can pick better defaults.

| Field | Description | Example |
|-------|-------------|---------|
| `setting` | Setting name | `"theme"`, `"brightness"` |
| `old_value` | Previous value | `"dark"` |
| `new_value` | New value | `"light"` |

Only enumerated category values are recorded (e.g., theme names, brightness percentages). No free-text or user-entered content.

### Connection Stability

Recorded every 4 hours and when HelixScreen closes. Helps us understand connection reliability across different setups.

| Field | Description | Example |
|-------|-------------|---------|
| `connect_count` | Times connected to printer | `3` |
| `disconnect_count` | Times disconnected | `2` |
| `total_connected_sec` | Total connected time | `13800` |
| `klippy_error_count` | Klippy error events | `1` |

### Print Start Context

Recorded when a print begins. Helps us understand slicer usage and file characteristics.

| Field | Description | Example |
|-------|-------------|---------|
| `source` | Print file source | `"local"` |
| `has_thumbnail` | Whether file has thumbnail | `true` |
| `file_size_bucket` | File size range | `"1-10MB"` |
| `slicer` | Slicer software used | `"PrusaSlicer"` |

No filenames are collected. File size is bucketed, not exact.

### Error Events

Recorded when a non-fatal error occurs (rate-limited to avoid excessive data). Helps us identify common issues that users may never report.

| Field | Description | Example |
|-------|-------------|---------|
| `category` | Error category | `"moonraker_api"` |
| `code` | Error code | `"timeout"` |
| `context` | What was being attempted | `"get_printer_objects"` |

---

## What Is NOT Collected

HelixScreen **never** collects any of the following:

- **Filenames** or file paths (print jobs, G-code files, thumbnails)
- **G-code content** (sliced files, macros, custom commands)
- **IP addresses** (local or public)
- **MAC addresses** or other network identifiers
- **Hostnames** (printer name, device name, Klipper instance name)
- **Usernames** or account credentials
- **Camera data** (images, video, webcam streams)
- **WiFi SSIDs** or network topology
- **Serial numbers** (printer, board, display)
- The **contents** of your configuration — parameter values, macro bodies, or G-code (only firmware-defined object *names* are collected, as shown in the Hardware Profile section above)
- **Moonraker API keys** or authentication tokens
- **Email addresses** or contact information

---

## How Anonymization Works

Each HelixScreen installation generates a random UUID v4 identifier on first launch. This UUID is used **only** to correlate events from the same device across sessions (so we can track things like "devices that crash also tend to have shorter print times"). The UUID itself **never leaves your device**.

Instead, before including the device identifier in any event, HelixScreen computes a **double SHA-256 hash**:

```
device_id = SHA-256( SHA-256(uuid) + device_local_salt )
```

The salt is a second random UUID v4, also generated locally and stored alongside the original UUID. Both values are stored in your local config directory and never transmitted.

This double-hash design means:

- The transmitted `device_id` cannot be reversed to recover the original UUID
- Even if the telemetry server were compromised, your device identity remains protected
- Two devices with different salts will always produce different device IDs, even if they somehow generated the same UUID
- **Re-identification is impossible** without access to the device's local files

---

## Default State

Telemetry is **OFF by default**. No data is collected, queued, or transmitted until you explicitly enable it through the Settings panel.

---

## How to Enable or Disable

1. Navigate to **Settings** on the HelixScreen home panel
2. Find the **Telemetry** section
3. Toggle **Share Usage Data** on or off

When you disable telemetry:
- No new events are recorded
- No queued events are transmitted
- Previously queued events remain in the local queue (use "Clear All Events" to delete them)

When you re-enable telemetry:
- A new session event is recorded on the next app launch
- Print outcomes and crashes resume being recorded
- Any previously queued events will be included in the next transmission

---

## How to View Your Data

You can inspect exactly what HelixScreen has queued for transmission:

1. Navigate to **Settings**
2. Tap **Telemetry**
3. Tap **View Telemetry Data**

This opens an overlay showing all queued events in their raw JSON format, which is exactly what would be sent to the server.

---

## How to Clear Your Data

To delete all locally queued telemetry events:

1. Navigate to **Settings** > **Telemetry** > **View Telemetry Data**
2. Tap **Clear All Events**

This permanently removes all queued events from your device. Events that have already been transmitted to the server cannot be individually deleted (see the [Privacy Policy](PRIVACY_POLICY.md) for details on why server-side data is inherently anonymous).

---

## Data Retention and Transmission

### Local Queue
- Events are stored locally in `telemetry_queue.json` in your config directory
- Maximum **100 events** in the queue; oldest events are dropped when the limit is reached
- Queue persists across application restarts

### Transmission
- Events are sent via **HTTPS POST** to `https://telemetry.helixscreen.org/v1/events`
- Batched in groups of up to **20 events** per request
- Transmission is attempted every **24 hours** when telemetry is enabled
- On failure, exponential backoff is applied (doubling interval, capped at 7 days)
- On success, sent events are removed from the local queue
- User-Agent header includes the HelixScreen version (e.g., `HelixScreen/1.0.0`)

---

## Why We Collect This Data

Telemetry helps us make HelixScreen better in specific, measurable ways:

- **Hardware landscape**: Knowing which platforms and display resolutions are most common helps us prioritize testing and optimization
- **Print success rates**: Understanding failure patterns across the user base helps us identify and fix issues that affect real prints
- **Crash regressions**: Crash reports with backtraces let us catch and fix bugs that might only appear on specific hardware or under specific conditions
- **Memory profiling**: Detect memory issues on resource-constrained devices like Pi Zero
- **Hardware census**: Understand what printers and features are in use to prioritize development
- **UI engagement**: Know which features are actually used to focus improvement efforts
- **UI performance**: Identify janky panels and frame drops across different hardware
- **Feature adoption**: Understand which features users have never discovered
- **Settings insight**: Learn which defaults work and which get changed immediately
- **Connection reliability**: Identify and fix connectivity issues across different setups
- **Error patterns**: Find and fix silent errors that users may never report
- **Development priorities**: Aggregate usage data helps us focus engineering effort where it matters most

We believe in earning trust through transparency. That is why the telemetry system is opt-in, the data is viewable, and this document exists.

---

## Technical Details

For developers and the technically curious:

- **Implementation**: handled by the telemetry and crash-handling modules built into HelixScreen itself — no external agents, scripts, or third-party trackers are involved
- **Crash handling**: written to keep working even while the app is crashing, so a crash is still captured and queued for sending
- **Schema version**: `2` (all events include `schema_version` for forward compatibility)
- **Identity files**: `telemetry_device.json` (UUID + salt), `telemetry_config.json` (enabled state), `telemetry_queue.json` (event queue)
- **Privacy policy**: [PRIVACY_POLICY.md](PRIVACY_POLICY.md)
