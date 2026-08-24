# HelixScreen Safety Audit

**Date:** 2026-02-09
**Scope:** User-facing safety — ways a user could damage their printer, harm themselves, or be misled about printer state. Not a cybersecurity audit.

---

## Overall Assessment

The codebase is well-architected for safety — API-level validation on all commands, no raw GCode input, thread-safe state management, and proper shell injection prevention. The main gaps are around **"what happens when things go wrong"** (crash, disconnect, display sleep) and **"can the user do dangerous things during a print"** (jog, motor disable, fan off).

Klipper provides a strong safety backstop for most operations, but the UI should not rely solely on firmware-level rejection — users deserve clear feedback and prevention at the UI layer.

---

## CRITICAL — Could cause physical damage or fire

### C1. No wake-on-critical-error when display is sleeping

If the display is asleep and Klipper enters SHUTDOWN (thermal runaway, heater fault, etc.), the screen stays black. In an enclosure, this could mean minutes before the user notices a safety event.

- **Location:** `src/application/display_manager.cpp`
- **Root cause:** No hook from `klippy_state_subject` to wake display
- **Fix:** Watch `klippy_state_subject` for SHUTDOWN/ERROR transitions → force display wake. Optionally flash LED or trigger audible alert if hardware supports it.
- **Effort:** Small — add observer on klippy state subject in DisplayManager

### C2. XY jog movement allowed during active print

The motion panel does not check print state. A user could accidentally jog X/Y during a print, crashing the nozzle into the part or dragging it across the build plate.

- **Location:** `src/ui/ui_panel_motion.cpp:488` — `jog()` has no `is_printing` guard
- **Fix:** Disable XY jog buttons during PRINTING/PAUSED states. Z adjustment should remain enabled (intentional live tuning).
- **Effort:** Small — add state observer to motion panel, bind hidden flag on XY jog buttons

### C3. Motor disable (M84) allowed during print

The "Disable Motors" button has a confirmation dialog but does not check if a print is active. Sending M84 mid-print causes instant failure — the toolhead drops, potentially crashing into a hot part.

- **Location:** `src/ui/ui_panel_controls.cpp:1333-1373`
- **Fix:** Grey out the button when `PrintJobState::PRINTING` or `PAUSED`. The confirmation dialog text could also warn about consequences.
- **Effort:** Small — add state check before showing confirmation

---

## HIGH — Misleading state or missing safeguards

### H1. No homing enforcement before jog

Motion panel allows jogging unhomed axes. Whether Klipper rejects this depends on the printer's `printer.cfg` configuration. If it doesn't reject, the printer moves without knowing its position, risking crashes into endstops or the bed.

- **Location:** `src/ui/ui_panel_motion.cpp:488` — no check against `homed_axes_` state
- **Fix:** Disable jog buttons for unhomed axes. Show a prompt like "Home X/Y/Z first" with a home button.
- **Effort:** Medium — need to track per-axis homing state and bind to jog button visibility

### H2. Heaters stay on after app crash

If HelixScreen crashes, Klipper keeps heaters on indefinitely. The watchdog recovery dialog appears but does not send cooldown commands. A user might not realize the nozzle is still at 250°C while staring at the recovery screen.

- **Location:** `src/helix_watchdog.cpp:680-743` (recovery dialog)
- **Fix options:**
  - Recovery dialog sends `M104 S0` / `M140 S0` to Moonraker on appearance (aggressive)
  - Recovery dialog shows warning: "Heaters may still be on! Check printer before restarting." (conservative)
  - Add a "Cool Down" button to the recovery dialog
- **Effort:** Medium — watchdog needs Moonraker connection capability or at minimum better messaging

### H3. Z-offset babystep has no cumulative limit

Users can keep tapping the Z-offset adjust button during a print with no limit. Repeated taps in the wrong direction could crash the nozzle into the bed or lift it so far the print detaches.

- **Location:** `src/ui/ui_print_tune_overlay.cpp:475` — `SET_GCODE_OFFSET Z_ADJUST=<delta>`
- **Step sizes available:** 0.025mm, 0.05mm, 0.1mm, 0.2mm
- **Fix:** Track cumulative Z-offset change during a print session. Clamp to a configurable range (e.g., +/-2mm). Show a warning if approaching the limit.
- **Effort:** Medium — need to track cumulative offset and reset on print start

### H4. Firmware restart has no confirmation dialog

`FIRMWARE_RESTART` is available from the Advanced panel with a single tap. Accidental activation kills all motors (position lost), turns off heaters, and aborts any active print.

- **Location:** `src/ui/ui_emergency_stop.cpp:313-336`, Advanced panel at line 393-396
- **Fix:** Add confirmation dialog: "This will restart printer firmware. Motors will lose position and any active print will be aborted. Continue?"
- **Note:** In the recovery dialog (post-SHUTDOWN), firmware restart is expected and a confirmation dialog would be annoying. Consider only adding confirmation for the Advanced panel path.
- **Effort:** Small

### H5. PID tuning available during print

No state check before starting `PID_CALIBRATE`. Running PID tuning mid-print would cause temperature instability and likely ruin the print.

- **Location:** `src/ui/ui_panel_calibration_pid.cpp`, header at `include/ui_panel_calibration_pid.h:177-183`
- **Fix:** Disable PID tuning start button when `is_printing()`. Show "Cannot tune PID during a print."
- **Effort:** Small

---

## MEDIUM — Edge cases and potential confusion

### M1. Part cooling fan can be disabled during print with no warning

Users can set the part cooling fan to 0% at any time via the controls panel slider. With high-temp materials (ABS, PETG, nylon) this could cause heat creep or, in extreme cases, thermal issues.

- **Location:** `src/ui/ui_panel_controls.cpp:1305-1327`, `src/ui/ui_fan_control_overlay.cpp:175-176`
- **Suggestion:** Show a one-time warning if setting fan to 0% while nozzle temp > 230°C: "Part cooling fan disabled while nozzle is hot. This may affect print quality."
- **Effort:** Small

### M2. Safety limits in config lack validation

`settings.json` has user-editable safety limits (max temp, max distance, max feedrate). Nothing prevents setting nonsensical values like `max_temperature_celsius: 99999` or `max_temp < min_temp`.

- **Location:** `config/settings.json.template:67-78`, `src/system/config.cpp:288`
- **Fix:** Validate on load:
  - `max > min` for all limit pairs
  - Temperature: max ≤ 500°C (no consumer printer needs more)
  - Distance: max ≤ 2000mm
  - Log warnings for unusual values
- **Effort:** Small

### M3. Print start profile corruption could mislead users

Print start profiles (`config/print_start_profiles/*.json`) are user-modifiable JSON. A misconfigured profile could show wrong phase progress — user thinks the printer is idle or complete when it's still heating, or vice versa.

- **Location:** `config/print_start_profiles/default.json`, `config/print_start_profiles/forge_x.json`
- **Fix options:**
  - JSON schema validation on profile load
  - Cross-check reported phase against actual Moonraker temperature/position state
  - Fall back to default.json if custom profile fails validation
- **Effort:** Medium

### M4. Multi-client conflicts show generic errors

If another client (Mainsail, Fluidd, OctoPrint) changes printer state while HelixScreen is active (e.g., cancels a print), the HelixScreen user gets a vague "not ready" error when they try to interact.

- **Location:** `src/api/moonraker_api_print.cpp:17-34`
- **Suggestion:** Parse Moonraker error responses for common state conflicts and show actionable messages: "Print was cancelled" rather than "Printer not ready."
- **Effort:** Medium

### M5. No explicit Klipper state guard on motion/temp commands

`home_axes()`, `move_axis()`, and `set_temperature()` don't check if Klipper is in SHUTDOWN before sending commands. Klipper will reject them, but the user gets a confusing error.

- **Location:** `src/api/moonraker_api_motion.cpp:23-100`
- **Fix:** Early return with clear message if `klippy_state != READY`: "Printer is in shutdown state. Restart Klipper to continue."
- **Effort:** Small — add guard at top of each API method

### M6. Cancel print has no confirmation dialog

`cancel_print()` in `moonraker_api_print.cpp:60-70` sends cancel immediately with no confirmation. An accidental tap on cancel during a long print = hours of work lost.

- **Location:** `src/api/moonraker_api_print.cpp:60-70`
- **Fix:** Add confirmation dialog: "Cancel current print? This cannot be undone."
- **Note:** Need to verify this isn't already handled at the UI layer above the API call.
- **Effort:** Small

---

## LOW — Worth documenting or hardening

### L1. Config file permissions

`settings.json` is saved with default umask (typically 0644 = world-readable). Contains Moonraker IP address and potentially sensitive configuration.

- **Location:** `src/system/config.cpp:461-489`
- **Fix:** After save, `chmod(path, 0600)` to restrict to owner only.
- **Effort:** Trivial

### L2. Moonraker API key field exists but is unused

Config has a `moonraker_api_key` field but no code sends it. If someone enables Moonraker authentication, HelixScreen fails to connect with no helpful error.

- **Location:** `config/settings.json.template`
- **Fix:** Either implement API key support or remove the field and document that Moonraker auth is not supported.
- **Effort:** Small (to remove), Medium (to implement)

### L3. No TLS for Moonraker WebSocket

All communication is over unencrypted WebSocket (`ws://`). Expected for local network operation but worth documenting as a known limitation, especially for users running Moonraker over tailnet or similar.

### L4. 100mm jog distance available

The largest jog distance is 100mm. On small printers, repeated 100mm jogs could move the toolhead into endstops. Klipper's firmware limits should catch this, but a visual warning for large jogs could help.

- **Location:** `src/ui/ui_panel_motion.cpp`, `ui_xml/motion_panel.xml:114-127`
- **Suggestion:** Consider a confirmation for jog distances > 50mm, or scale max jog to printer dimensions from config.

### L5. Filament database preset values not clamped to SafetyLimits

Temperature presets come from the filament database. If the database were corrupted, presets could exceed SafetyLimits. Currently all bundled values are safe (max ABS = 255°C nozzle, 100°C bed).

- **Location:** `src/ui/ui_panel_temp_control.cpp:41-71`
- **Fix:** Clamp all filament database values to SafetyLimits on load.
- **Effort:** Trivial

---

## What's Already Good

These areas were audited and found to be well-implemented:

| Area | Notes |
|------|-------|
| **API-level validation** | All temps, distances, feedrates validated against `SafetyLimits` before sending |
| **Cold extrusion protection** | Load/unload/purge buttons disabled below 170°C with clear messaging |
| **Emergency stop** | State-aware visibility, two-stage confirmation option, recovery flow |
| **No raw GCode input** | Eliminates injection attack surface entirely |
| **Thread safety** | `ui_async_call()` pattern, `weak_ptr` lifetime guards in WebSocket callbacks |
| **Update integrity** | gzip verification + ELF architecture validation + `fork/exec` (no shell) |
| **Shell injection prevention** | `safe_exec()` throughout, no `popen()` or `system()` |
| **Settings validation** | Clamp/validate all inputs, `validate_timeout_option()` with fallback |
| **Reconnection state sync** | Full re-discovery from Moonraker on reconnect |
| **Klipper error display** | Red icon + recovery overlay, manual restart required (no auto-clear) |
| **Klipper config isolation** | HelixScreen never modifies `printer.cfg` |
| **Z-axis feedrate** | Hardcoded to 600 mm/min (10mm/s) vs 6000 for XY — safer by design |
| **Probe/calibration operations** | Pre-flight checks, timeout guards, modal dialogs |
| **Temperature keypad clamping** | Nozzle 0-350°C, bed 0-150°C, no decimals, no negatives |

---

## Priority Order for Implementation

If tackling these incrementally:

1. **C1** — Wake display on critical error (highest safety impact, small effort)
2. **C2** — Disable XY jog during print (high impact, small effort)
3. **C3** — Disable motor-off during print (high impact, small effort)
4. **H4** — Firmware restart confirmation (quick win)
5. **H5** — PID tuning state check (quick win)
6. **H1** — Homing enforcement (medium effort, high value)
7. **H3** — Z-offset cumulative limit (medium effort)
8. **M5** — Klipper state guards on API methods (small effort, good UX)
9. **M2** — Safety limits validation (small effort)
10. **H2** — Crash recovery heater warning (medium effort)
11. Everything else as time permits
