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

### Fields on every event

Every event starts with the same few fields, so we can tell which release, platform and board it came from:

| Field | Description | Example |
|-------|-------------|---------|
| `schema_version` | Format version of the event | `3` |
| `event` | Event type identifier | `"session"` |
| `device_id` | Anonymized device identifier (see below) | `"a3f8c1..."` (64-char hex) |
| `timestamp` | ISO 8601 UTC timestamp | `"2026-02-08T14:30:00Z"` |
| `app_version` | HelixScreen version that sent it | `"1.1.0"` |
| `app_platform` | Hardware platform | `"pi"`, `"mips"` |
| `app_board` | Printer board, where HelixScreen can tell | `"ad5x"` |
| `boot_id` | Random ID for this run of HelixScreen | `"9f2c41d07ab3e855"` |

`boot_id` is 16 random characters generated each time HelixScreen starts. It is never saved on your device, so it cannot be traced across restarts or back to you. It only lets us tell two reports from the same run apart from reports of different runs.

The tables below list each event's own fields; these shared fields are not repeated.

### Session Events

Recorded once per application launch. Helps us understand the hardware landscape.

| Field | Description | Example |
|-------|-------------|---------|
| `app.display` | Display resolution | `"800x480"` |
| `app.locale` | Language setting | `"en"` |
| `app.install_source` | Channel that installed HelixScreen, from a fixed list (`helix-installer`, `kiauh`, `helper-script`, `curl`, `other`, `unknown`) | `"kiauh"` |
| `app.host_mod` | Firmware mod running on the printer, from a fixed list (`forge_x`, `zmod`, `klipper_mod`, `cosmos`, `paxx`, `simple_af`, `rinkhals`, `none`) | `"none"` |
| `printer.detected_model` | Auto-detected printer model | `"Voron 2.4"` |

### Print Outcome Events

Recorded when a print finishes (success, failure, or cancellation). Helps us track print success rates and understand failure patterns.

| Field | Description | Example |
|-------|-------------|---------|
| `outcome` | Print result | `"success"`, `"failure"`, `"cancelled"` |
| `duration_sec` | Total print duration in seconds | `3600` |
| `phases_completed` | The furthest print start phase reached, as its number in that build (13 = complete) | `13` |
| `filament_used_mm` | Filament consumed in millimeters | `1500.0` |
| `filament_type` | Filament material | `"PLA"`, `"PETG"`, `"ABS"` |
| `nozzle_temp` | Target nozzle temperature in degrees C | `215` |
| `bed_temp` | Target bed temperature in degrees C | `60` |

### Crash Reports

Recorded automatically when HelixScreen crashes. Picked up on the next startup. Helps us catch and fix regressions.

| Field | Description | Example |
|-------|-------------|---------|
| `signal` | POSIX signal number | `11` |
| `signal_name` | Signal name | `"SIGSEGV"`, `"SIGABRT"`, `"SIGBUS"`, `"SIGFPE"` |
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
| `spoolman_configured` | Whether a Spoolman server is connected | `true` |
| `theme_changed` | Whether you picked a theme other than the default | `false` |
| `multi_printer` | Whether more than one printer is set up | `false` |
| `screensaver_type` | The screensaver you picked, or `off` | `"maze"` |

`spoolman_configured`, `theme_changed` and `multi_printer` are yes/no answers only. They tell us how many people use these features without needing a screen visit to prove it.

### Panel Usage

Recorded every 4 hours and when HelixScreen closes. Shows which screens are used most so we can focus improvement efforts. No specific actions or content are tracked.

Each report covers only the time since the previous one, not a running total, so a report never repeats activity we already received. If HelixScreen is interrupted (a power cut, for instance), the activity that had not been sent yet is saved on your device and sent the next time it starts.

| Field | Description | Example |
|-------|-------------|---------|
| `snapshot_seq` | Report number within this run, starting at 0 | `3` |
| `session_duration_sec` | Length of the period this report covers | `14400` |
| `panel_time_sec` | Time spent on each panel in that period (seconds) | `{"home": 8000, "controls": 3200}` |
| `panel_visits` | Visit count per panel | `{"home": 42, "controls": 18}` |
| `overlay_visits` | How many times each pop-up screen was opened | `{"settings_sound_overlay": 2}` |
| `widget_interactions` | How many times each home-screen widget was tapped | `{"temperature": 5}` |

Only screen and widget names are recorded, never what you did on them.

### Screensaver Usage

Recorded every 4 hours and when HelixScreen closes, only for a period in which a screensaver ran. Tells us which screensavers people keep, and which ones are too heavy for their printer so we can make them lighter. Like Panel Usage, each report covers only the time since the previous one. A preview from the Test button in Display settings is not counted.

| Field | Description | Example |
|-------|-------------|---------|
| `snapshot_seq` | Report number within this run, starting at 0 | `3` |
| `saver_shown_sec` | How long each screensaver was on screen in that period (seconds) | `{"maze": 5400}` |
| `saver_starts` | How many times each one started | `{"maze": 3}` |
| `saver_woke_by_touch` | How many times a touch ended it | `{"maze": 2}` |
| `saver_stopped_other` | How many times something else ended it: the screen going to sleep, a print finishing, a switch to another screensaver | `{"maze": 1}` |
| `saver_too_heavy` | How many times it was too heavy for your printer even at its lightest setting, so another one took over | `{"maze": 0}` |
| `saver_settled_levels` | How many runs ended at each setting, as name and level: 0 is the full effect, higher numbers are lighter | `{"maze:1": 3}` |

When every screensaver is too heavy for your printer, a plain black screen shows instead and is counted as `black_screen`.

### Performance Snapshots

Recorded every hour, and once more when HelixScreen closes. Measures UI responsiveness so we can identify and fix slow panels across different hardware.

| Field | Description | Example |
|-------|-------------|---------|
| `frame_time_p50_ms` | Median frame render time (ms) | `8` |
| `frame_time_p95_ms` | 95th percentile frame time (ms) | `16` |
| `frame_time_p99_ms` | 99th percentile frame time (ms) | `28` |
| `dropped_frame_count` | Frames slower than 33ms | `42` |
| `total_frame_count` | Frames sampled in the same window as the dropped count | `1024` |
| `worst_panel` | Panel with highest frame times | `"temperature"` |

No user content is included — just timing data about how smoothly the UI renders.

### Feature Usage

There is no separate feature report. Which built-in features are used is worked out from the screens and widgets listed in Panel Usage (a feature counts as used on a device when its screen was opened or its widget tapped during a reporting period), plus the three yes/no answers in the Settings Snapshot.

### Settings Changes

Recorded when you change a setting, batched over a 30-second window. Helps us understand which default settings people change most, so we can pick better defaults.

| Field | Description | Example |
|-------|-------------|---------|
| `setting` | Setting name | `"theme"`, `"brightness"` |
| `old_value` | Previous value | `"dark"` |
| `new_value` | New value | `"light"` |

Only enumerated category values are recorded (e.g., theme names, brightness percentages). No free-text or user-entered content.

### Connection Stability

Recorded every 4 hours and when HelixScreen closes. Helps us understand connection reliability across different setups. Like Panel Usage, each report covers only the time since the previous one.

| Field | Description | Example |
|-------|-------------|---------|
| `snapshot_seq` | Report number within this run | `3` |
| `session_duration_sec` | Length of the period this report covers | `14400` |
| `connect_count` | Times connected to printer | `3` |
| `disconnect_count` | Times disconnected | `2` |
| `total_connected_sec` | Connected time | `13800` |
| `total_disconnected_sec` | Disconnected time | `600` |
| `longest_disconnect_sec` | Longest single outage (an outage that spans two reports is measured from its real start) | `420` |
| `klippy_error_count` | Klippy error events | `1` |
| `klippy_shutdown_count` | Klippy shutdowns | `0` |

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

1. Go to **Settings > System**
2. Find the **Share Usage Data** toggle
3. Toggle it on or off

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

1. Go to **Settings > System**
2. Tap **View Telemetry Data** (visible when sharing is on)

This opens an overlay showing all queued events in their raw JSON format, which is exactly what would be sent to the server.

---

## How to Clear Your Data

To delete all locally queued telemetry events:

1. Go to **Settings > System > View Telemetry Data**
2. Tap **Clear All Events**

This permanently removes all queued events from your device. Events that have already been transmitted to the server cannot be individually deleted (see the [Privacy Policy](PRIVACY_POLICY.md) for details on why server-side data is inherently anonymous).

---

## Data Retention and Transmission

### Local Queue
- Events are stored locally in telemetry_queue.json in your config directory
- Maximum **100 events** in the queue; when the limit is reached, a new usage or connection report is first combined with a report of the same kind already waiting, and otherwise the oldest other event is dropped to make room. Only if the queue holds nothing else that can be dropped is the new report discarded
- Queue persists across application restarts
- Activity that has been counted but not yet put in a report is saved to telemetry_snapshot.json in your config directory about once an hour and deleted when HelixScreen closes normally

### Transmission
- Events are sent via **HTTPS POST** to `https://telemetry.helixscreen.org/v1/events`
- Batched in groups of up to **20 events** per request
- Transmission is attempted every **24 hours** when telemetry is enabled
- On failure, exponential backoff is applied (doubling interval, capped at 7 days)
- On success, sent events are removed from the local queue
- User-Agent header includes the HelixScreen version (e.g., `HelixScreen/0.99.111`)

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
- **Feature usage**: Understand which features users have never discovered
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
- **Schema version**: `3` (all events include `schema_version` for forward compatibility)
- **Local files**: telemetry_device.json (UUID + salt) in HelixScreen's state directory, kept out of the printer config folder that web UIs show and config backups upload (on a Raspberry Pi `/var/lib/helixscreen`, on printers the `helixscreen-state` directory beside the install); telemetry_queue.json (event queue) and telemetry_snapshot.json (usage not yet reported) in the config directory; the enabled state is a setting in settings.json
- **Privacy policy**: [PRIVACY_POLICY.md](PRIVACY_POLICY.md)
