<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# HelixScreen Privacy Policy

**Effective date**: July 2026
**Last updated**: July 2026

---

## 1. Data Controller

356C LLC ("we", "us", "our") is the data controller for information collected through the HelixScreen telemetry system.

**Contact**: privacy@helixscreen.org

---

## 2. Overview

HelixScreen is open-source touchscreen software for Klipper 3D printers. It includes an optional telemetry system that collects anonymous usage data to help us improve the software. This privacy policy explains what data is collected, how it is processed, and what rights you have.

The key points:

- Telemetry is **opt-in only** (disabled by default)
- All data is **fully anonymized** before transmission
- You can **view, disable, and delete** your local data at any time
- No personally identifiable information is collected

---

## 3. Legal Basis for Processing

We process telemetry data under **consent** (GDPR Article 6(1)(a)). Telemetry is disabled by default and only activated when you explicitly enable it via the Settings panel. You may withdraw consent at any time by disabling the telemetry toggle.

Because the data we collect is fully anonymized through irreversible cryptographic hashing (see Section 6), the transmitted data qualifies as anonymous information under **GDPR Recital 26** and is no longer considered personal data. Article 11 of the GDPR applies: where we do not require identification of data subjects, we are not obligated to maintain, acquire, or process additional information to identify them.

---

## 4. Data Collected

When telemetry is enabled, HelixScreen collects the categories of events described below. The three most detailed categories (Session, Print Outcome, Crash) have full field tables here; the remaining diagnostic and usage events are summarized in Section 4.4, with complete field-by-field detail in the user-facing [Telemetry documentation](TELEMETRY.md).

### 4.1 Session Events

Recorded once per application launch.

| Field | Type | Description |
|-------|------|-------------|
| `schema_version` | integer | Event schema version (currently `2`) |
| `event` | string | Always `"session"` |
| `device_id` | string | Double-hashed anonymous device identifier (64-char hex) |
| `timestamp` | string | ISO 8601 UTC timestamp (e.g., `"2026-02-08T14:30:00Z"`) |
| `app.version` | string | HelixScreen software version (e.g., `"1.0.0"`) |
| `app.platform` | string | Hardware platform (e.g., `"rpi4"`, `"rpi5"`, `"x86_64"`) |
| `app.display` | string | Display resolution (e.g., `"800x480"`) |

### 4.2 Print Outcome Events

Recorded when a print job reaches a terminal state (completion, failure, or cancellation).

| Field | Type | Description |
|-------|------|-------------|
| `schema_version` | integer | Event schema version (currently `2`) |
| `event` | string | Always `"print_outcome"` |
| `device_id` | string | Double-hashed anonymous device identifier |
| `timestamp` | string | ISO 8601 UTC timestamp |
| `outcome` | string | `"success"`, `"failure"`, or `"cancelled"` |
| `duration_sec` | integer | Print duration in seconds |
| `phases_completed` | integer | Number of print start phases completed (0-10) |
| `filament_used_mm` | float | Filament consumed in millimeters |
| `filament_type` | string | Filament material type (e.g., `"PLA"`, `"PETG"`, `"ABS"`) |
| `nozzle_temp` | integer | Target nozzle temperature in degrees Celsius |
| `bed_temp` | integer | Target bed temperature in degrees Celsius |

### 4.3 Crash Reports

Written to local storage at crash time using async-signal-safe functions. Picked up and enqueued on the next application startup.

| Field | Type | Description |
|-------|------|-------------|
| `schema_version` | integer | Event schema version (currently `2`) |
| `event` | string | Always `"crash"` |
| `device_id` | string | Double-hashed anonymous device identifier |
| `timestamp` | string | ISO 8601 UTC timestamp |
| `signal` | integer | POSIX signal number (e.g., `11` for SIGSEGV) |
| `signal_name` | string | Signal name (e.g., `"SIGSEGV"`, `"SIGABRT"`, `"SIGBUS"`, `"SIGFPE"`) |
| `app_version` | string | HelixScreen version at time of crash |
| `uptime_sec` | integer | Application uptime in seconds at time of crash |
| `backtrace` | array of strings | Stack frame addresses in hexadecimal |

### 4.4 Diagnostic and Usage Events

In addition to the three categories above, HelixScreen records the following diagnostic and usage events. Each contains only the data described; none contains filenames, G-code content, credentials, or free-text you have entered. Full field tables are in the [Telemetry documentation](TELEMETRY.md).

| Event | When recorded | What it contains |
|-------|---------------|------------------|
| Update events | When the in-app updater runs | Whether an update attempt succeeded or failed, and version/platform context |
| Memory snapshots | Hourly and at startup | Resident/virtual memory usage and uptime (for leak detection) |
| Memory warnings | When memory pressure is detected | The same memory-usage figures at the moment of a warning |
| Hardware profile | Once per session | Detected printer model, kinematics, MCU, extruder/fan counts, capability flags, and Klipper configuration identifiers (see Section 4.5) |
| Settings snapshot | Once per session | A subset of your preferences (theme, brightness, locale, animations on/off) |
| Settings changes | Batched over 30 seconds after a change | The name of a changed setting and its old/new value — enumerated category values only, never free text |
| Panel usage | Every 4 hours and at close | Time spent on and visit counts for each UI panel (no actions or content) |
| Performance snapshots | Every 4 hours | Frame-render timing percentiles and dropped-frame counts (no user content) |
| Feature adoption | 5 minutes after startup | Boolean flags for which built-in features have been used |
| Connection stability | Every 4 hours and at close | Connect/disconnect counts and total connected time |
| Print start context | When a print begins | Print source, whether the file has a thumbnail, a bucketed file-size range, and slicer name — never the filename |
| Error events | When a non-fatal error occurs (rate-limited) | An error category, code, and the operation being attempted |

### 4.5 Klipper Configuration Identifiers

The Hardware Profile event (Section 4.4) includes **firmware-defined object names** from your Klipper configuration, so we can accurately detect printer models and understand which hardware and features are in use. Specifically, it may include:

- The auto-detected printer model (e.g., `"Voron 2.4"`)
- G-code macro names (e.g., `"PRINT_START"`, `"CLEAN_NOZZLE"`) — up to 200
- Stepper motor names (e.g., `"stepper_x"`)
- Temperature sensor and filament sensor names
- LED / neopixel names
- The list of active Klipper objects (e.g., `"toolhead"`, `"bed_mesh"`)

These are **configuration identifiers defined by your printer's firmware and config files**, not personal data. We do **not** collect the *contents* of your configuration (parameter values, macro bodies, or G-code), and these identifiers are not serial numbers, hostnames, or network identifiers. They are transmitted alongside the anonymized `device_id` and cannot be linked back to you (see Section 6).

---

## 5. Data NOT Collected

HelixScreen **does not** collect, process, or transmit any of the following:

- File names, file paths, or directory structures
- G-code content, sliced files, or print job metadata beyond what is listed above
- IP addresses (local or public)
- MAC addresses or other hardware network identifiers
- Hostnames, printer names, or Klipper instance names
- Usernames, passwords, or authentication credentials
- Moonraker API keys or tokens
- Camera images, video, or webcam streams
- WiFi SSIDs, network topology, or connection details
- Device serial numbers (printer, control board, display)
- The **contents** of your Klipper configuration — parameter values, macro bodies, or G-code (only firmware-defined object *names* are collected, as disclosed in Section 4.5)
- Email addresses or other contact information
- Location data or timezone information
- Browsing history or usage patterns outside of HelixScreen

---

## 6. Anonymization Method

Each HelixScreen installation generates two random values on first launch:

1. A **UUID v4** (128-bit random identifier)
2. A **salt** (also a UUID v4, used as a cryptographic salt)

Both values are stored locally on the device and are **never transmitted**.

The device identifier included in telemetry events is computed as:

```
device_id = SHA-256( SHA-256(uuid) + salt )
```

This double-hash construction ensures:

- **Irreversibility**: SHA-256 is a one-way cryptographic hash function. The original UUID cannot be recovered from the `device_id`.
- **Salt uniqueness**: Each device uses a unique, randomly generated salt. Even identical UUIDs (astronomically unlikely) would produce different `device_id` values.
- **No cross-referencing**: Without access to the device's local files, there is no way to link a `device_id` to a physical device, user, or network.

Under **GDPR Article 11**, because we cannot identify individual data subjects from the transmitted data, certain data subject rights (such as access, rectification, and server-side erasure) do not apply to the already-transmitted anonymous data.

---

## 7. Data Storage and Security

### 7.1 Local Storage

Before transmission, events are stored in a JSON file (`telemetry_queue.json`) in the HelixScreen configuration directory on the device. This file is protected by the device's filesystem permissions.

### 7.2 Transmission

Events are transmitted via **HTTPS** (TLS-encrypted) POST requests to:

```
https://telemetry.helixscreen.org/v1/events
```

Events are sent in batches of up to 20, with a minimum interval of 24 hours between transmission attempts. Failed transmissions use exponential backoff.

### 7.3 Server-Side Storage

Received events are stored with encryption at rest. Access to raw event data is restricted to authorized personnel within 356C LLC.

---

## 8. Data Retention

| Data Type | Retention Period |
|-----------|-----------------|
| Raw telemetry events | 365 days from receipt |
| Aggregated/statistical data | Indefinitely (no individual records) |
| Local queue on device | Until transmitted and acknowledged, or until user clears |

After the retention period, raw events are permanently deleted. Aggregated data (e.g., "60% of users are on Raspberry Pi 4", "average print success rate is 92%") is retained indefinitely as it contains no individual device records.

---

## 9. Your Rights

### 9.1 Right to Withdraw Consent

You may withdraw consent at any time by navigating to **Settings > System > Share Usage Data** and disabling the toggle. When disabled:

- No new events are recorded
- No queued events are transmitted
- The preference is persisted and survives application restarts

### 9.2 Right to Erasure of Local Data

You may delete all locally queued telemetry events by navigating to **Settings > System > Share Usage Data > View Telemetry Data** and tapping **Clear All Events**. This permanently removes all pending events from your device.

### 9.3 Server-Side Data and Article 11

Because all transmitted data is fully anonymized (see Section 6), we cannot identify which server-side records correspond to your device. Under **GDPR Article 11**, where the controller does not require identification of the data subject, individual access and erasure requests regarding server-side data are not applicable.

If you have concerns, you may:

1. Disable telemetry to prevent future data collection
2. Clear your local queue to remove unsent data
3. Contact us at privacy@helixscreen.org with any questions

### 9.4 Right to Information

You can inspect the exact data queued for transmission at any time via **Settings > System > Share Usage Data > View Telemetry Data**. The raw JSON shown is identical to what would be sent to the server.

### 9.5 Right to Lodge a Complaint

You have the right to lodge a complaint with a supervisory authority if you believe your data has been processed unlawfully.

---

## 10. Data Transfers

We prefer EU-region storage for telemetry data. If data is processed outside the EU/EEA, appropriate safeguards are in place in accordance with GDPR Chapter V.

---

## 11. Children's Privacy

HelixScreen does not knowingly collect data from children under 16. The telemetry system does not collect any personally identifiable information regardless of the user's age.

---

## 12. Third-Party Sharing

We do not sell, rent, or share raw telemetry data with third parties. Aggregated, anonymous statistics may be published in release notes, blog posts, or community discussions (e.g., "85% of HelixScreen users run on Raspberry Pi hardware").

---

## 13. Open Source Transparency

HelixScreen is open-source software. The complete telemetry implementation is available for public review:

- **Telemetry manager**: `src/system/telemetry_manager.cpp`
- **Crash handler**: `src/system/crash_handler.cpp`
- **Unit tests**: `tests/unit/test_telemetry_manager.cpp`
- **User documentation**: `docs/user/TELEMETRY.md`

You are welcome to audit the source code to verify that the telemetry system behaves exactly as described in this policy.

---

## 14. Changes to This Policy

We may update this privacy policy from time to time. Changes will be communicated through:

- Release notes accompanying the HelixScreen version that includes the change
- An updated "Last updated" date at the top of this document

Material changes that expand the scope of data collection or alter the anonymization method will be accompanied by a consent reset, requiring users to re-enable telemetry under the new terms.

---

## 15. Contact

For questions, concerns, or requests regarding this privacy policy or HelixScreen's telemetry system:

**Email**: privacy@helixscreen.org
**Project**: https://github.com/prestonbrown/helixscreen
