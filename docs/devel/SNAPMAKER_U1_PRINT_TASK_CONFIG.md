<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Snapmaker U1 — `print_task_config` Native Filament/Tool-Mapping Command API

> **Correction note.** Earlier HelixScreen research wrongly concluded that the Snapmaker U1
> has *no* native runtime filament-remap mechanism and that remapping a print to a different
> physical head "requires rewriting the gcode." **That is false.** The U1's firmware ships a
> Klipper extra, print_task_config.py, that registers ~14 gcode commands the stock Snapmaker
> screen used for exactly this: per-slot filament metadata, **logical→physical extruder
> remapping** (`SET_PRINT_EXTRUDER_MAP`), and **per-head "used" gating** (`SET_PRINT_USED_EXTRUDERS`)
> that the print macros consult to decide whether to auto-feed a head. This document is the
> authoritative reference for that native API, derived from reading the full 1351-line extra,
> the live U1 device (`192.168.30.103`), the stock UI binary (`/usr/bin/gui`), and the
> slicer-baked print macros.

**Sources read for this document**
- /home/lava/klipper/klippy/extras/print_task_config.py (1351 lines — the extra itself)
- Live device, read-only: SSH `root@192.168.30.103` + Moonraker `http://192.168.30.103:7125`
- `/usr/bin/gui` (stock UI binary) — `strings` for exact command format strings
- Klipper config macros via `printer/objects/query?configfile=settings`
- /home/lava/klipper/klippy/toolhead.py + kinematics/extruder.py (Tn routing)
- A real Orca-sliced file: `/home/lava/printer_data/gcodes/lid_PLA_6m28s.gcode`

---

## 1. Command Reference

All commands are registered in `PrintTaskConfig.__init__` (lines 111–137). Constants:
`LOGICAL_EXTRUDER_NUM = 32`, `PHYSICAL_EXTRUDER_NUM = 4`.

| Command | Signature | Purpose | Blocked while printing? |
|---|---|---|---|
| `SET_PRINT_EXTRUDER_MAP` | `CONFIG_EXTRUDER=<0..31> MAP_EXTRUDER=<0..3>` | Remap one logical tool → physical head | **Yes** (id 531, code 15) |
| `GET_PRINT_EXTRUDER_MAP` | *(none)* | Dump `Tn -> Tm` map table to console | No |
| `SET_PRINT_FILAMENT_CONFIG` | `CONFIG_EXTRUDER=<0..3>` + filament fields (below) | Set per-physical-head filament vendor/type/color | No (but rejects official RFID unless `FORCE=1`) |
| `GET_PRINT_TASK_CONFIG` | *(none)* | Dump the entire `print_task_config` dict | No |
| `SAVE_CURRENT_PRINT_TASK_CONFIG` | *(none)* | Persist current config to print_task.json | No |
| `RESET_PRINT_TASK_CONFIG` | *(none)* | Reset whole config to defaults + persist | No |
| `LOAD_PRINT_TASK_CONFIG` | *(none)* | Reload config from disk | No |
| `SET_PRINT_PREFERENCES` | many flags (below) | Bed-level / flow-calib / timelapse / replenish / entangle prefs | Partially (id 531, code 16) |
| `SET_PRINT_USED_EXTRUDERS` | `EXTRUDERS=<csv of 0..3>` | Mark which physical heads this task uses | **Yes** (id 531, code 16) |
| `SET_PRINT_TASK_PARAMETERS` | bulk (map + prefs + per-tool gcode params) | One-shot "full task setup" the slicer/cloud uses | **Yes** (id 531, code 16/17) |
| `INNER_CHECK_AND_RELOAD_FILAMENT_INFO` | `EXTRUDER=<0..3> IS_RUNOUT=<0/1>` | Internal: restore filament metadata after runout | n/a (internal) |
| `INNER_AUTO_REPLENISH_FILAMENT` | `EXTRUDER=<0..3>` | Internal: find a matching head and continue print | n/a (internal, paused only) |
| `INNER_PRINT_END` | *(none)* | Internal: sets `is_exec_print_end_action=True` | n/a (internal) |
| webhook `print_task_config/set_print_preferences` | JSON-RPC params | Moonraker endpoint mirroring a subset of prefs | n/a |

### 1.1 `SET_PRINT_EXTRUDER_MAP` — the remap primitive
```python
def cmd_SET_PRINT_EXTRUDER_MAP(self, gcmd):
    config_extruder = gcmd.get_int("CONFIG_EXTRUDER", None)   # logical, 0..31
    map_extruder    = gcmd.get_int("MAP_EXTRUDER", None)      # physical, 0..3
    ...
    if print_stats.state in ['printing', 'paused']:
        raise gcmd.error(message="...not allowed to set extruder map during printing!",
                         id=531, index=0, code=15, oneshot=1, level=1)
    if (config_extruder < 0 or config_extruder >= LOGICAL_EXTRUDER_NUM) or \
            (map_extruder < 0 or map_extruder >= PHYSICAL_EXTRUDER_NUM):
        raise gcmd.error("...invalid extruder index!!!")
    tmp_map_table[config_extruder] = map_extruder
    tmp_reprint_info['extruder_map_table'][config_extruder] = map_extruder
```
- `CONFIG_EXTRUDER` is the **logical/slicer tool index** (0–31); `MAP_EXTRUDER` is the **physical head** (0–3).
- Writes both the live `extruder_map_table` **and** `reprint_info.extruder_map_table` (so a re-print keeps the remap).
- **Does NOT auto-persist to disk** — it only mutates the in-memory dict. The stock screen calls
  `SAVE_CURRENT_PRINT_TASK_CONFIG` (or the bulk `SET_PRINT_TASK_PARAMETERS`, which does persist) to commit.
- Rejected during `printing`/`paused`.

### 1.2 `SET_PRINT_USED_EXTRUDERS` — the feed-gate primitive (★ key to the bug fix)
```python
def cmd_SET_PRINT_USED_EXTRUDERS(self, gcmd):
    extruders_str = gcmd.get('EXTRUDERS', None)          # e.g. "0,2"
    if print_stats.state in ['printing', 'paused']:
        raise gcmd.error(message="...not allow to set used_extruders during printing!",
                         id=531, index=0, code=16, oneshot=1, level=1)
    tmp_extruders_used = [False] * PHYSICAL_EXTRUDER_NUM
    used_extruders = [int(value) for value in extruders_str.split(',')]
    for i in range(min(len(used_extruders), LOGICAL_EXTRUDER_NUM)):
        tmp_extruders_used[used_extruders[i]] = True
        tmp_reprint_info['extruders_used'][used_extruders[i]] = True
    self.print_task_config['extruders_used'] = tmp_extruders_used
    ...  # persists to disk
```
- `EXTRUDERS` is a **comma-separated list of PHYSICAL head indices** (0–3) — the heads that are
  actually used by this task. Anything not listed becomes `extruders_used[i] = False`.
- **This is the flag the prestart feed/preheat macros consult** (§3). Setting it persists to disk.
- Rejected during `printing`/`paused`.

### 1.3 `SET_PRINT_FILAMENT_CONFIG` — per-head filament metadata
```python
config_extruder        = gcmd.get_int('CONFIG_EXTRUDER')      # physical 0..3 (NOT logical)
filament_vendor        = gcmd.get('VENDOR', None)
filament_type          = gcmd.get('FILAMENT_TYPE', None)
filament_sub_type      = gcmd.get('FILAMENT_SUBTYPE', None)
filament_soft          = gcmd.get_int('SOFT', None)
filament_color         = gcmd.get_int('FILAMENT_COLOR', None)        # 0xAARRGGBB int
filament_color_rgba    = gcmd.get('FILAMENT_COLOR_RGBA', None)       # "RRGGBB" or "RRGGBBAA"
filament_alpha         = gcmd.get_int('ALPHA', None, minval=0, maxval=255)
filament_color_nums    = gcmd.get_int('COLOR_NUMS', None, minval=1, maxval=5)
filament_colors_str    = gcmd.get('COLORS', None)                    # csv of RRGGBB, multicolor
filament_color_multi_mode = gcmd.get_int('MULTI_MODE', 0, minval=0, maxval=255)
force                  = gcmd.get_int('FORCE', False)
```
- `CONFIG_EXTRUDER` here is a **physical head** (0–3), validated against `PHYSICAL_EXTRUDER_NUM`.
- If the slot currently holds an **official (RFID) filament**, the call is rejected unless `FORCE=1`
  (`filament_official[config_extruder] and bool(force) == False` → error).
- Three mutually-exclusive color forms: multicolor (`COLOR_NUMS`+`COLORS`), `FILAMENT_COLOR_RGBA`, or `FILAMENT_COLOR` int.
- Persists to disk and runs `FLOW_RESET_K EXTRUDER=<n>`.
- **Not** blocked during printing (no `print_stats` guard).

### 1.4 `SET_PRINT_PREFERENCES`
```python
BED_LEVEL=<0/1>  FLOW_CALIBRATE=<0/1>  FLOW_CALIBRATE_EXTRUDERS=<csv>  SHAPER_CALIBRATE=<0/1>
TIME_LAPSE_CAMERA=<0/1>  AUTO_REPLENISH_FILAMENT=<0/1>  REPLENISH_IGNORE_COLOR=<0/1>
FILAMENT_ENTANGLE_DETECT=<0/1>  FILAMENT_ENTANGLE_SEN=<low|medium|high>
END_LED_TURN_OFF=<0/1>  END_UNLOAD_FILAMENT=<python-list-literal>  FORCE=<0/1>
```
- During `printing`/`paused`, setting `BED_LEVEL`/`FLOW_CALIBRATE`/`SHAPER_CALIBRATE`/`TIME_LAPSE_CAMERA`/`END_UNLOAD_FILAMENT`
  is rejected (id 531, code 16) **unless** `FORCE=1`. The replenish/entangle/LED prefs are always allowed.
- `END_UNLOAD_FILAMENT` is parsed with `ast.literal_eval` and must be a Python list (e.g. `[1,0,1,0]`).

### 1.5 `SET_PRINT_TASK_PARAMETERS` — the bulk one-shot
The "do everything" command. Accepts a superset:
```python
MAP_TABLE=<list of [logical,physical] pairs>   # e.g. "[[0,0],[1,2]]"
BED_LEVEL  FLOW_CALIBRATE  FLOW_CALIBRATE_EXTRUDERS  SHAPER_CALIBRATE  TIME_LAPSE_CAMERA
END_UNLOAD_FILAMENT
LINE_WIDTH  LAYER_HEIGHT  OUTER_WALL_SPEED
NOZZLE_DIAMETER_LIST  NOZZLE_TEMP  FILAMENT_TYPE  FILAMENT_FLOW_RATIO  FILAMENT_MAX_VOL_SPEED
FILAMENT_USED_G  FILAMENT_USED_MM
```
- `MAP_TABLE` is a list of `[logical, physical]` pairs; each pair updates `extruder_map_table[logical]=physical`.
- **`extruders_used` is derived automatically** here (lines 1299–1301):
  ```python
  for i in range(LOGICAL_EXTRUDER_NUM):
      if filament_used_g[i] > 0.0001 or filament_used_mm[i] > 0.0001:
          extruders_used[ extruder_map_table[i] ] = True
  ```
  i.e. any logical tool with nonzero filament usage marks its **mapped physical head** as used.
- Enforces a nozzle-diameter match per used head (code 14) and a flow-calibrate-allowed check (code 18).
- Rejected during `printing`/`paused` (code 16; otherwise code 17 on generic error).
- Persists both print_task.json and print_task_2.json.

### 1.6 Internal / runout commands
- `INNER_CHECK_AND_RELOAD_FILAMENT_INFO EXTRUDER=<0..3> IS_RUNOUT=<0/1>` — on runout, restores the
  pre-runout filament metadata for that head from `filament_info_backup`; if the head still has no
  filament type it raises a **pause action** error (`action='pause', id=523, index=<extruder>, code=39`).
- `INNER_AUTO_REPLENISH_FILAMENT EXTRUDER=<0..3>` — when paused on a runout and `auto_replenish_filament`
  is on, searches the other heads for a colour/type match, **rewrites `extruder_map_table` so every logical
  tool that pointed at the runout head now points at the replacement**, flips `extruders_used`, records
  `extruders_replenished[old]=new`, then `RESUME REPLENISH=1 REPLENISH_EXTRUDER=<new>`. This is the firmware's
  own live-remap-on-runout path and is the clearest proof the map is the routing authority.
- `INNER_PRINT_END` — sets `is_exec_print_end_action=True`.

### 1.7 Error-id convention
Blocked-during-print errors all use `id=531` with a distinguishing `code` (15 = set map, 16 = used/prefs/params,
17 = params generic, 14 = nozzle mismatch, 18 = flow-calib not allowed). The runout "no filament edited"
pause uses `id=523, code=39`. These ids match the U1 exception-object scheme documented in
`project_991_u1_pause_signals` (523/532 families).

---

## 2. Data Model

`DEFAULT_PRINT_TASK_CONFIG` (lines 23–61). Per-**physical-head** arrays are length 4
(`PHYSICAL_EXTRUDER_NUM`); the map table is length 32 (`LOGICAL_EXTRUDER_NUM`).

| Field | Shape | Meaning |
|---|---|---|
| `extruder_map_table` | `int[32]` | **logical tool index → physical head index.** Default identity: `[0,1,2,3, 0,0,…]` (indices 4–31 default to 0) |
| `extruders_used` | `bool[4]` | which physical heads this task uses (gates feed/preheat/switch-check) |
| `extruders_replenished` | `int[4]` | per-head: which head replaced it after auto-replenish (default identity) |
| `filament_exist` | `bool[4]` | sensor-derived: filament physically present at head |
| `filament_vendor/type/sub_type` | `str[4]` | filament metadata per head (`'NONE'` = unset) |
| `filament_color` | `int[4]` | `0xAARRGGBB` |
| `filament_color_rgba` | `str[4]` | 8-char `"RRGGBBAA"` |
| `filament_color_multi` | `dict[4]` | `{nums, alpha, mode, colors:[…]}` for multicolor spools |
| `filament_official` | `bool[4]` | true = Snapmaker RFID spool (locks `SET_PRINT_FILAMENT_CONFIG` w/o FORCE) |
| `filament_sku`, `filament_soft`, `filament_edit` | per-head | SKU id; "soft" flag; UI-editable flag |
| `flow_calibrate`, `flow_calib_extruders[4]`, `auto_bed_leveling`, `time_lapse_camera`, `shaper_calibrate` | prefs | task prefs |
| `auto_replenish_filament`, `replenish_ignore_color` | prefs | runout auto-replenish behaviour |
| `filament_entangle_detect`, `filament_entangle_sen` | prefs | tangle detection |
| `end_led_turn_off`, `end_unload_filament[4]` | prefs | end-of-print actions |
| `reprint_info` | dict | snapshot of `{extruder_map_table, extruders_used, flow_*, time_lapse, bed_level, end_unload}` for re-print |

A second file print_task_2.json (`DEFAULT_PRINT_TASK_CONFIG_2`) holds **per-logical-tool gcode params**
(`nozzle_temp[32]`, `nozzle_diameter[32]`, `filament_used_g[32]`, etc.) used for validation in
`SET_PRINT_TASK_PARAMETERS`.

`reset_print_info()` (called at construction / new task) resets `extruder_map_table`, `extruders_used`,
`extruders_replenished`, and prefs to identity/defaults — so **each new print starts with an identity map
and all-False `extruders_used` until the screen sets them.** (Verified live: an idle U1 reports
`extruders_used: [False,False,False,False]`, `extruder_map_table[0:8]: [0,1,2,3,0,0,0,0]`.)

The whole struct is exported as a Klipper status object: `printer.print_task_config` (queried via
Moonraker `printer/objects/query?print_task_config`), which is exactly what the macros read.

---

## 3. Routing + Feed Mechanism (drives the bug fix)

### 3.1 How `Tn` resolves to a physical head
Two distinct paths:

**`T0`–`T3` (physical-range tool commands).** Registered by kinematics/extruder.py as each extruder's
`gcode_id`, dispatching to `cmd_SWITCH_EXTRUDER_ADVANCED`:
```python
def cmd_SWITCH_EXTRUDER_ADVANCED(self, gcmd):
    extruder_map = gcmd.get_int('A', 1, minval=0)        # A defaults to 1 = "apply map"
    if extruder_map != 0 and self.print_config is not None:
        index = int(self.gcode_id.split('T')[1])
        index = self.print_config.get_extruder_map_index(index)   # ← map lookup
        section = 'extruder' if not index else 'extruder%d' % index
        extruder = self.printer.lookup_object(section, None)
        extruder.cmd_SWITCH_EXTRUDER(gcmd)
    else:
        self.cmd_SWITCH_EXTRUDER(gcmd)                    # A=0 → direct, NO map
```
So a bare `T2` **does** route through `extruder_map_table[2]` (because `A` defaults to 1). A `T2 A0`
selects physical head 2 directly, bypassing the map. **The Snapmaker print macros consistently use
`A0`** for deterministic physical addressing (e.g. `SM_PRINT_CHECK_SWITCH_EXTRUDER` emits `T{i} A0`,
preheat emits `M104 ... T{i} A0`).

**`T4`–`T31` (extended/logical tool commands).** These are *macros* — each is literally
`SWITCH_OF_EXTENDED_EXTRUDER INDEX=n`. Implemented in toolhead.py:
```python
def cmd_SWITCH_OF_EXTENDED_EXTRUDER(self, gcmd):
    index = gcmd.get_int('INDEX')                                  # 4..31
    extruder_index = print_task_config.get_extruder_map_index(index)   # ← map lookup
    extruder = lookup('extruder' if extruder_index==0 else 'extruder%d'%extruder_index)
    gcmd._params['A'] = '0'                                        # then direct-select physical
    extruder.cmd_SWITCH_EXTRUDER_ADVANCED(gcmd)
```
So **`T4`–`T31` ALWAYS route through the map** (`get_extruder_map_index`) and then physical-select.

**Net:** `extruder_map_table` is the single routing authority for both ranges (T0–T3 via the default
`A=1`, T4–T31 unconditionally). Remapping with `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=c MAP_EXTRUDER=p`
genuinely redirects body tool-changes to a different physical head.

### 3.2 How the prestart feed/preheat works — and the empty-head bug
The Orca slicer bakes an **unconditional** prestart block for **all four heads** into every file. From
the real file `lid_PLA_6m28s.gcode`:
```
PRINT_START
T0
SM_PRINT_CHECK_SWITCH_EXTRUDER
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=0
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=2 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=1
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=3 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=2
SM_PRINT_AUTO_FEED EXTRUDER=3
```
The `.gcode` file itself contains **no** `SET_PRINT_USED_EXTRUDERS` / `SET_PRINT_EXTRUDER_MAP` (grep-confirmed).
Those are sent **by the screen/cloud BEFORE the file plays** — and they are what make the baked block selective.

**The feed macro gates on `extruders_used`.** `gcode_macro sm_print_auto_feed`:
```jinja
{% set extruder = params.EXTRUDER | default(999) | int %}
{% if filament_feed_vars != {} and extruder >= 0
      and extruder < printer.configfile.settings.printer.max_physical_extruder_num %}
  {% if printer.print_task_config['extruders_used'][extruder] %}          ← THE GATE
    {% set feed_module_seq  = filament_feed_vars.module_sequence[extruder] %}
    {% set feed_channel_seq = filament_feed_vars.channel_sequence[extruder] %}
    FEED_AUTO MODULE={feed_module_seq} CHANNEL={feed_channel_seq} LOAD=1 PRINTING=1 {rawparams}
  {% endif %}
{% else %} … {% endif %}
```
`sm_print_extruder_preheat` has the **same gate**:
```jinja
{% if extruder >= 0 and extruder < max_physical_extruder_num
      and printer.print_task_config['extruders_used'][extruder] %}
  M104 S{temp} T{extruder} A0
{% endif %}
```
And `sm_print_check_switch_extruder` only switches to heads that are used:
```jinja
{% for i in range(max_physical_extruder_num) %}
  {% if printer.print_task_config['extruders_used'][i] %}
    T{i} A0
  {% endif %}
{% endfor %}
```

### 3.3 Answer to the critical question
**Yes — `SET_PRINT_USED_EXTRUDERS` (sent before `print_start`) is exactly the command that prevents the
firmware from auto-feeding an unused/empty head.** The baked `SM_PRINT_AUTO_FEED EXTRUDER=n` and
`SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=n` lines are no-ops for any head `n` whose `extruders_used[n]` is
False. For the reporter's case — a file whose body uses heads 0+2 but whose baked prestart feeds 0,1,2,3 —
sending `SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2` before the print causes the macros to **skip** the feed
and preheat for heads 1 and 3, so the empty head never trips a runout.

**Important nuance — which command, and the map interaction:**
- The **physical-head index** in `extruders_used` is what gates the feed. So you must list the *physical*
  heads actually used, after applying any remap.
- `SET_PRINT_EXTRUDER_MAP` alone does **not** set `extruders_used` (it only writes the map table), so a
  remap by itself will not silence the feed of a now-unused head. You need `SET_PRINT_USED_EXTRUDERS` too
  (or the bulk `SET_PRINT_TASK_PARAMETERS`, which derives `extruders_used` from per-tool filament usage
  through the map — §1.5).
- The stock flow uses both: set the map, then set used-extruders to the mapped physical set.

**Confidence:** High from static reading — all three prestart macros explicitly index
`printer.print_task_config['extruders_used'][extruder]`, the gcode file carries the unconditional baked
block, and the file does not self-set the flags. The one thing **not** verified live (read-only constraint
forbids starting a print or sending `SET_*`) is the end-to-end timing: that the screen's
`SET_PRINT_USED_EXTRUDERS` lands and persists into `printer.print_task_config` *before* `PRINT_START`
executes the baked block. This is consistent with the code (the commands are rejected during print, so they
*must* precede it) but should be confirmed on-printer with a 0+2 file before we rely on it in production.

---

## 4. How the Stock Screen Sequenced It

The orchestration lives in the **local UI binary `/usr/bin/gui`**, not in snapmakercloud.py (grep of the
cloud component for `extruder_map` / `used_extruder` / `MAP_EXTRUDER` returns nothing — the cloud submits
tasks, the local gui issues the gcode). Exact format strings extracted from `/usr/bin/gui`:

```c
"SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=%d MAP_EXTRUDER=%d\n"
"SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=%u MAP_EXTRUDER=%u"
"SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=%u VENDOR=%s FILAMENT_TYPE=%s FILAMENT_SUBTYPE='%s'"
"SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=%u FILAMENT_COLOR_RGBA=%02X%02X%02X%02X"
"SET_PRINT_USED_EXTRUDERS EXTRUDERS=%s"
"SET_PRINT_PREFERENCES BED_LEVEL=%d FLOW_CALIBRATE=%d SHAPER_CALIBRATE=%d TIME_LAPSE_CAMERA=%d"
"SET_PRINT_PREFERENCES FLOW_CALIBRATE_EXTRUDERS=%s"
```
Plus the JSON-RPC preference mirror (via the `print_task_config/set_print_preferences` webhook):
```
{"jsonrpc":"2.0","method":"printer.print_task_config.set_print_preferences",
 "params":{"auto_replenish_filament":%d}, "id":%u}
   (also: end_led_turn_off, filament_entangle_detect, filament_entangle_sen)
```

**Reconstructed pre-print sequence** (from the command semantics + format strings; the gui sends these
before issuing the print, since all the map/used/params commands are rejected once `print_stats.state`
is `printing`/`paused`):
1. Per edited slot: `SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=<phys> VENDOR=… FILAMENT_TYPE=… FILAMENT_SUBTYPE='…'`
   (and a colour variant) — sets the per-head filament metadata.
2. Per remap: `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<logical> MAP_EXTRUDER=<physical>` — redirect tools.
3. `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv of physical heads in use>` — gate feed/preheat.
4. `SET_PRINT_PREFERENCES …` (bed-level / flow-calib / timelapse / flow-calib-extruders).
5. Start the print (the baked `PRINT_START` block then feeds/preheats only the used heads).

The slicer/cloud path can instead use the single `SET_PRINT_TASK_PARAMETERS MAP_TABLE=… FILAMENT_USED_G=… …`,
which sets the map and derives `extruders_used` from per-tool usage in one command.

---

## 5. Implications for HelixScreen

1. **Use the native API for U1 filament remap.** Replace any "rewrite the gcode" plan with the
   `print_task_config` command set. To remap logical tool *c* to physical head *p*:
   `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=c MAP_EXTRUDER=p`, then commit with
   `SAVE_CURRENT_PRINT_TASK_CONFIG` (or use `SET_PRINT_TASK_PARAMETERS`, which persists).
2. **Fix the spurious-feed bug with `SET_PRINT_USED_EXTRUDERS`.** Before starting a U1 print, compute the
   set of *physical heads the body actually uses* (after any remap) and send
   `SET_PRINT_USED_EXTRUDERS EXTRUDERS=<csv>`. This makes the slicer's unconditional
   `SM_PRINT_AUTO_FEED`/`SM_PRINT_EXTRUDER_PREHEAT` block skip empty/unused heads, eliminating the false
   runout from feeding an unloaded head. This is the firmware's own intended mechanism — not a workaround.
3. **Timing constraint is hard.** Every map/used/params command raises an error (`id=531`) if
   `print_stats.state` is `printing` or `paused`. HelixScreen MUST send these **before** issuing
   `print_start` / `SDCARD_PRINT_FILE`. Sending them mid-print is not just ineffective — it errors and
   surfaces a U1 exception modal.
4. **Read current state from `printer.print_task_config`.** It's a standard Klipper status object; query
   via Moonraker `printer/objects/query?print_task_config` (and subscribe). Fields of interest:
   `extruder_map_table`, `extruders_used`, `filament_exist`, `filament_type`/`vendor`/`sub_type`,
   `filament_color_rgba`, `filament_official`.
5. **Respect `filament_official`.** A slot holding a Snapmaker RFID spool rejects
   `SET_PRINT_FILAMENT_CONFIG` unless we pass `FORCE=1`. Don't blindly overwrite official slots.
6. **Indices: mind logical vs physical.** `extruder_map_table` / `SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER`
   are **logical** (0–31). `MAP_EXTRUDER`, `SET_PRINT_USED_EXTRUDERS EXTRUDERS=`, and
   `SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER` are **physical** (0–3).

### Still UNCERTAIN / needs on-printer confirmation (do not start prints to test casually)
- **End-to-end timing of the feed-skip** — that a pre-print `SET_PRINT_USED_EXTRUDERS EXTRUDERS=0,2` is
  reflected in `printer.print_task_config['extruders_used']` by the time the baked `PRINT_START` block runs
  (consistent with the code, but unverified live). Confirm with a real 0+2 file.
- **Whether `SM_PRINT_FLOW_CALIBRATE` is defined** in shipping firmware — it appears in the slicer-baked
  block but is **not** present as a `gcode_macro` in this device's live config (no
  `gcode_macro sm_print_flow_calibrate`). It may be a no-op / unknown-command-tolerant path or defined in a
  build variant. Doesn't affect feed/remap, but note it before relying on flow-calib behaviour.
- **Exact persistence semantics of `SET_PRINT_EXTRUDER_MAP`** (in-memory only) vs whether the stock screen
  always follows it with a save — we infer a save step but did not capture the gui's exact call order live.
- **Interaction with HelixScreen's own AMS/tool-mapping model** for the U1 backend — needs a design pass to
  decide whether we drive `extruder_map_table` directly or keep our own mapping and translate at print time.
