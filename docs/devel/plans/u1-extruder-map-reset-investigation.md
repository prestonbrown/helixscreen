# U1 `extruder_map_table` reset between prints - investigation

Date: 2026-08-27. Device: Snapmaker U1 at 192.168.30.103 (Buildroot 2024.02, klipper under
`/home/lava/klipper`). Investigation only - nothing was changed on the printer, no gcode sent.

## Question

`print_task_config.extruder_map_table` returns to identity `[0,1,2,3]` between prints, after both
a completed and a cancelled print. No caller had been found for the reset.

## Answer

The reset is **`print_task_config.reset_print_info()`**
(`klippy/extras/print_task_config.py:378`), called from **`print_stats._note_finish()`**
(`klippy/extras/print_stats.py:167`).

`_note_finish()` is the single shared terminal path for all three print outcomes:

| entry point | line | reaches `_note_finish` |
|---|---|---|
| `note_complete()` | print_stats.py:155 | `_note_finish("complete")` |
| `note_error()` | print_stats.py:158 | `_note_finish("error", msg)` |
| `note_cancel()` | print_stats.py:160 | `_note_finish("cancelled")` |

`reset_print_info()` is a **partial** reset, not the full wipe. It restores exactly these fields
from `DEFAULT_PRINT_TASK_CONFIG` and leaves filament identity alone:

`extruder_map_table`, `extruders_used`, `extruders_replenished`, `flow_calibrate`,
`flow_calib_extruders`, `auto_bed_leveling`, `time_lapse_camera`, `end_unload_filament`

It explicitly does **not** touch `reprint_info`, and it persists the result to
`/home/lava/printer_data/config/snapmaker/print_task.json`.

Its three callers:

- `print_stats.py:167` - `_note_finish()`, the print-end reset. **This is the observed one.**
- `print_task_config.py:110` - `__init__`, so every klippy start also resets.
- `virtual_sdcard.py:1888` - power-loss-recovery restore failure path only.

## Evidence

**1. Live klippy log** - the live log is `/oem/klippylogs/klippy.log` (the `printer_data/logs/`
copy is stale, last written Aug 22). The reset logs `[print_task_config] reset print info`:

```
# COMPLETED print - 1ms gap
08-27 18:50:34.381: Finished SD card print, file: .../calicat_PLA_13m17s.gcode
08-27 18:50:34.382: [print_task_config] reset print info

# CANCELLED print - 1ms gap
08-27 19:18:51.125: [pause_resume] request cancel
08-27 19:18:51.730: Exiting SD card print, lines=17386, current_line_gcode=M400
08-27 19:18:51.731: [print_task_config] reset print info

# CANCELLED print, matches print_task.json mtime 20:32
08-27 20:32:56.504: [pause_resume] request cancel
08-27 20:32:58.540: Exiting SD card print, lines=7595
08-27 20:32:58.541: [print_task_config] reset print info
```

**2. Full causal chain in one session:**

```
20:25:21  SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=2
20:25:21  SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=2 MAP_EXTRUDER=0
20:32:58  [print_task_config] reset print info
```

**3. On-disk state afterwards** (`print_task.json`, mtime Aug 27 20:32) shows the exact
field split `reset_print_info()` produces - live reset, `reprint_info` preserved:

```
extruder_map_table  : [0, 1, 2, 3, 0, 0, 0, 0]      <- reset to identity
extruders_used      : [False, False, False, False]  <- reset
extruders_replenished: [0, 1, 2, 3]                 <- reset
reprint_info.extruder_map_table : [2, 1, 0, 3, ...] <- the real job map, kept
reprint_info.extruders_used     : [True, False, True, False]
```

## Why it was not found before

Two independent traps, both of which produce a confident zero-hit result:

1. **BusyBox grep silently rejects `--include`.** `/usr/bin/grep` on the U1 is BusyBox v1.36.1.
   `grep -rn --include=*.py PATTERN /home/lava` prints a usage error to stderr and matches
   nothing. With stderr suppressed or skimmed, that reads as "no hits anywhere". Plain
   `grep -rl PATTERN <dir>` works and immediately surfaces `print_stats.py` and
   `virtual_sdcard.py`.
2. **The search string was the gcode name, not the log string.** The reset never mentions
   `RESET_PRINT_TASK_CONFIG`; it logs `[print_task_config] reset print info`. Searching the
   logs for the gcode name correctly finds nothing.

## Status of the five prior claims

| # | Claim | Verdict |
|---|---|---|
| 1 | `_reset_print_task_config()` has exactly one caller, the `RESET_PRINT_TASK_CONFIG` handler | **HELD** (`print_task_config.py:829`, handler at :828) - but it is the wrong function. The observed behavior is `reset_print_info()`, a different and partial reset. |
| 2 | The module has no print-end hook, only `klippy:ready` | **HELD** (`print_task_config.py:143` is the only `register_event_handler`) - but misleading. The module does not subscribe; `print_stats.py` calls into it directly. |
| 3 | The sliced file's `machine_start_gcode` does not contain it | **HELD.** 0 hits for both `RESET_PRINT_TASK_CONFIG` and `SET_PRINT_EXTRUDER_MAP` across the whole 2 MB gcode file. |
| 4 | HelixScreen does not send it | **HELD.** No hits in `src/`, `include/`, `ui_xml/`, `assets/`. |
| 5 | Stock UI disabled; `klippy.log` has zero hits | **FALSE - this is where the answer was.** Wrong file (stale copy) and wrong search string. The live log has 9 `reset print info` lines. |

**"It might be a read artifact rather than a reset" - ruled out.** `get_status()`
(`print_task_config.py:505`) returns `dict(self.print_task_config)` with no re-derivation, and
the reset is persisted to disk, where the identity value is directly observable.

## Second question: where the firmware consumes `reprint_info.extruder_map_table`

Two consumers, **both on the power-loss-recovery path only**:

1. **`virtual_sdcard.py:2118`**, in `GCodeStateTracker.update_position()` (class at :1903,
   method at :2030; called from :1166 and :1275). While tracking gcode to reconstruct machine
   state, on a `T<n>` tool-change command it maps the logical index through
   `reprint_info["extruder_map_table"]` and stores `self.extruder_gcode_id = f"T{mapped}"`.
   That value is consumed on resume at :1670
   (`run_script_from_command("{} A0".format(gcode_tracker.extruder_gcode_id))`) and :1629/:1639
   to reactivate the correct physical extruder.
2. **`print_task_config.apply_reprint_info()`** (:420), called only from `virtual_sdcard.py:1842`
   during power-loss restore. It copies `reprint_info` back over the live fields.

`apply reprint info` appears **0 times** in the live log, so neither path fires in normal
operation. `reprint_info` is written by `SET_PRINT_EXTRUDER_MAP` (:537-538, which writes the live
table and `reprint_info` together), by `set_reprint_info()` (:433, via `set_new_print_info()` at
:452, called from `virtual_sdcard.py:510` at print start), and by the auto-replenish path
(:1025-1027).

So the model is: **live fields = working state for the current job, cleared at job end;
`reprint_info` = the durable record of what the last job was**, used to reconstruct a job after
power loss and to back a "reprint" that reuses the previous routing.

## Relevance to HelixScreen

No live bug, and the current gate is coherent rather than lucky.
`AmsBackendSnapmaker::get_tool_mapping()` (`src/printer/ams_backend_snapmaker.cpp:1926`) gates on
`extruders_used` being non-empty. Because `reset_print_info()` clears `extruders_used` and
`extruder_map_table` in the same function, in the same tick, and persists them in one write, the
two can never be observed disagreeing - a stale non-identity map with a live `extruders_used`, or
the reverse, is not a reachable state. The gate holds by construction.

One nuance worth recording: the comment at `ams_backend_snapmaker.cpp:1966` already asserts that
`reset_print_info()` does not run between our `SET_PRINT_EXTRUDER_MAP` send and the print. That is
confirmed - it runs at print *end*, from `_note_finish()`.

## Open

- Whether the stock Snapmaker UI, when enabled, additionally issues `RESET_PRINT_TASK_CONFIG`.
  Not tested; the UI is disabled on this unit and the gcode name appears nowhere in the logs.
- `extruders_replenished` is reset alongside the map but HelixScreen does not read it. Not
  investigated further.
