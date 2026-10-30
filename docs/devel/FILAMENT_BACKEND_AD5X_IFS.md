# AD5X IFS (FlashForge Adventurer 5X) Filament Backend

The FlashForge Adventurer 5X 4-lane Intelligent Filament Switching (IFS) system runs on
a separate STM32 MCU and is supported on two firmwares: ZMOD (v1.7.0+) and the
standalone IFS module extracted from it (ships in Forge-X — see "The standalone IFS
module" below). Topology is
`PathTopology::LINEAR` - the four ports merge at a single combiner before the toolhead.

## AD5X IFS (FlashForge Adventurer 5X)

> **Status: TESTING** — This backend is functional but not yet fully supported. It is available for user testing and feedback. Please report issues via GitHub.

The AD5X has a 4-lane Intelligent Filament Switching (IFS) system controlled by a separate STM32 MCU. HelixScreen supports it through ZMOD firmware (ghzserg's Klipper mod for FlashForge printers).

> **Required firmware**: [ZMOD open-source firmware](https://github.com/ghzserg/zmod) **v1.7.0 or newer** (v1.7.0, Mar 2026, is the first release with explicit HelixScreen integration via `DISPLAY_OFF HELIX=1`). Hard minimum: v1.6.2 (Oct 2025), when the `less_waste_*` `save_variables` plumbing first appeared via the bambufy plugin — older versions are missing the slot-color/material surface we read.
>
> Note: this is ZMOD's own version, not FlashForge stock firmware. ZMOD supports a discrete list of stock AD5X main versions (1.1.7 through 1.2.3, 3.0.3, 3.0.9, 3.1.0 ceiling) and requires a factory restore to a listed image before install — new units ship above the ceiling. Details and commissioning traps: `printers/FLASHFORGE_AD5X_SUPPORT.md` § "Firmware Quirks and Operating Rules".

### Detection

IFS is detected via `filament_switch_sensor _ifs_port_sensor_{1-4}` or `filament_motion_sensor _ifs_motion_sensor_{1-4}` in `printer.objects.list`. The leading space in sensor names is intentional — it's a Klipper object naming convention.

Detection is gated by `!has_mmu_` — if Happy Hare or AFC is already detected, IFS sensors are ignored (priority: HH > AFC > IFS).

### The standalone IFS module (Forge-X)

The IFS logic was extracted from zmod into a drop-in module (Python plugins plus
one gcode file; ships in Forge-X, runs on any klipper host). It speaks a
first-class Moonraker surface, and the backend switches to it wholesale the
moment a status frame carries either object (`ifs_module_live_`):

| Source | Data |
|--------|------|
| `ifs` object | `connected` / `error` / `activity` (`polling`/`ready`/`clamped`/`loading`/`unclamping`/`unloading`/`driver_error`) / `active_channel` (the F13 chan — same stickiness as zmod's Chan) / `loaded_channels` (silk bitmask decoded; the presence authority) / `pending_insert_channels` / `params` |
| `ifs_materials` object | `slots` dict — STRING keys, `#RRGGBB`/`#RGB` colours, per-material handling `temp` — a view onto the same Adventurer5M.json `zmod_color` reads, re-read on mtime change and pushed as a diff |
| `save_variables.ifs_loaded` | The module's own success-confirmed lane-in-nozzle record (written at the end of `IFS_LOAD`/`IFS_UNLOAD`, survives restarts). Stronger than `active_channel`, which also tracks cold ejects — the client just reads it |
| `filament_switch_sensor toolhead` | The module's ADC-classified toolhead switch, registered under a stock name — the head-presence authority (feeds the same `parse_head_sensor` path as zmod's `head_switch_sensor`) |

`loaded_channels` and `active_channel` are folded into the same
`ZColorSilentResult` the zmod objects and macro responses use, so every seated-
channel guard, presence corroboration, and external-edit baseline applies
unchanged. `ifs_loaded` overwrites `seated_chan_` directly, and once the module has
published it, it stays the seated lane on every module frame (`active_channel` included),
unless a lessWaste/bambufy plugin runs beside the module: its swaps need not run
`IFS_LOAD`, so that rig keeps following `active_channel`.
The remembered seated lane (the `lane_data` DB `seated` key) mirrors it instead of
feeding the cold-boot floor: on the module, `active_channel=0` with a loaded head is the
normal idle state, not a firmware that forgot its lane (#1897).

**Ops and writes route to the module's macros** once live:

| Action | Command |
|--------|---------|
| Load | `IFS_LOAD SLOT={n}` (resolves its own heat from the slot's material; self-unloads the outgoing lane) |
| Toolhead unload | `IFS_UNLOAD SLOT={n}`, or bare when a live active slot exists (the macro defaults `SLOT=` from `ifs_loaded`). With no active slot the backend names the lane itself or asks the user; see "Unload with no lane named" below |
| Lane eject | `IFS_EJECT SLOT={n}` (replaces the zmod `IFS_F24`/`F11`/`F39` sequence). Ejecting the lane `ifs_loaded` names also clears the extruder, which needs a temperature: an unlabelled lane gets `TEMP=` (the nozzle target from 150C, else 220C), a labelled one leaves it to the module's material |
| Tool change | `T{n}` — the module maps T0..T3 to slots 1..4 itself, so the identity tool map is firmware truth |
| Material/colour edit | `IFS_SET_MATERIAL SLOT={n} TYPE=… COLOR=7EC8E3` (bare hex — klipper's parser eats `#`; the module re-prefixes) |
| Fault recovery / abort | `IFS_RESET_DRIVER` / `IFS_STOP` |

**Both ZMOD polls stand down** on the module (this is the #1344 stand-down,
shipped here because `ifs_loaded` carries what `FFMInfo.channel` was missing on
the zmod object path): no `Adventurer5M.json` HTTP poll, no `GET_ZCOLOR` /
`IFS_STATUS` round trips — neither command exists on that firmware, and
`ifs_materials` pushes file changes by subscription.

The board's `activity` refines `system_info_.action` for ops HelixScreen did
NOT start (a slicer's `T1`, a console `IFS_LOAD`): `loading`/`unloading` map
up from IDLE, `ready` settles an untracked busy op back to IDLE, and
`driver_error` raises ERROR with the module's own error text. Ops we DID start
are excluded (`phase_tracker_` gate) — the macro-ack finalize and the tracker
own those, and a polling `ready` frame must never erase a tracked op's HEATING.

### State Sources

Stock zMod owns two Klipper objects — `zmod_ifs` and `zmod_color` — that hold the authoritative per-channel state, but their rich APIs are `printer.lookup_object()`-only (no `get_status()`), so Moonraker cannot see them. What Moonraker actually exposes depends on whether the lessWaste / bambufy plugins are installed.

**Shared (stock zMod and plugins both provide):**

| Source | Data | Notes |
|--------|------|-------|
| `filament_switch_sensor head_switch_sensor` | Toolhead filament presence | Authoritative NOZZLE/TOOLHEAD indicator |
| `filament_motion_sensor ifs_motion_sensor` | Filament moving **post-hub**, inside the IFS | Single boolean on stock zMod. Maps to `OUTPUT` segment — **not** the toolhead. Replaced by per-port sensors when plugins are installed. |
| `Adventurer5M.json` (Moonraker file API) | Per-channel color + material type | Polled + re-read on sensor edges / gcode responses. Moonraker sends no push for it; the HelixPrint Moonraker plugin can (below). Poll cadence 5 s idle / 30 s while printing — see "Polling caution" below. |

**Plugin-only (lessWaste / bambufy) — the Moonraker-visible export of `zmod_ifs` / `zmod_color`:**

| Source | Data | Plugin delta over stock zMod |
|--------|------|------------------------------|
| `filament_switch_sensor _ifs_port_sensor_{1-4}` | Per-port HUB presence (4 booleans) | Wraps `zmod_ifs.ifs_data.get_port(port)` — invisible to Moonraker otherwise |
| `save_variables.<prefix>_colors` / `_types` | Atomic per-port color + material | Subscribable; stock requires json polling |
| `save_variables.<prefix>_tools` | 16-element tool→port map | Not exposed on stock zMod |
| `save_variables.<prefix>_current_tool` | Active tool index (-1 or 0-15) | Stock: `zmod_color.get_current_channel()` (lookup-only) |
| `save_variables.<prefix>_external` | Bypass / external mode flag | Stock: `zmod_color.get_printer_data_detail().indepMatlInfo` (lookup-only) |
| `_IFS_VARS` gcode macro | Atomic writes of the above | Stock lacks this — can't persist UI-side changes |

Prefix is `less_waste` (the lessWaste plugin) or `bambufy` (the bambufy plugin); the schema is identical. Auto-detected from whichever keys are present. **Neither prefix comes from stock zMod** — the table above is the plugin-only column, and the `less_waste_*` plumbing first appeared in zMod v1.6.2 *via* the bambufy plugin framework, not in the firmware itself. A stock-zMod machine has no `save_variables` rows under either prefix.

**Forge-X port (`[ifs]` module):** the second firmware stack this backend serves. Machines on our Forge-X port carry a first-class Klipper module (`[ifs]` in printer.cfg, `printer.ifs` over Moonraker) that owns the slicer-tool → lane table and echoes it live:

| Source | Data | Notes |
|--------|------|-------|
| `printer.ifs.tool_map` | The live tool→lane table | `{"0": 1, "1": 2, "2": 3, "3": 4}` — keys are STRING 0-based tool numbers (the `T<n>` a slicer writes; a JSON object's keys are strings whatever Python held), values are INT 1-based lanes (the number `IFS_LOAD SLOT=` speaks). Published from construction, complete table per frame; entries a frame omits unmap. Presence of this field is one of the two remap signals — see "Tool remapping" under G-code Commands |

The module's map is **per-print state**: identity until `IFS_MAP_TOOL` rewrites an entry, kept across pause/resume, cleared when the print ends, never persisted (a klippy restart loses it). `firmware_default_routing()` therefore answers identity on a wire-table rig — a per-print table is not a standing default. ZMOD machines never publish this object; their remap path is the `_IFS_VARS` contract in the tables above.

> **Upstream wishlist:** add `get_status()` to `zmod_ifs` and `zmod_color` in stock zMod. That would close the plugin gap entirely and let HelixScreen drop the `Adventurer5M.json` polling path. Until then, users without a plugin see a degraded UI (no per-port HUB presence, no live tool map, no bypass flag, no atomic color updates).

> **Sensor-location correction:** the `ifs_motion_sensor` sits **inside the IFS immediately after the hub**, not at the toolhead. The current backend routes it through `parse_head_sensor()` as a simplification; a proper fix would map it to `PathSegment::OUTPUT` and require the toolhead switch for `filament_loaded` / load-complete detection.

#### The two data sources, and which one owns what

On native ZMOD (no lessWaste / bambufy plugin) the backend reconciles **two** independent reads. They answer different questions and must not be confused — conflating them is the root of the resurrection bug documented below.

| Source | Transport | Question it answers | Code |
|--------|-----------|---------------------|------|
| `Adventurer5M.json` `FFMInfo` | Moonraker `download_file("config", "Adventurer5M.json")`, 5s content-compare poll, or the HelixPrint plugin's push | What **color / material** is *assigned* to each channel (persisted metadata) | `parse_adventurer_json()`, `poll_adventurer_json()`, `apply_json_content()`, `note_json_content()` |
| `GET_ZCOLOR SILENT=1` (and, future, `IFS_STATUS`) | gcode console, on-demand | Which lanes **physically have filament** (RS-485 silk sensor) + the active lane | `query_zcolor_silent()`, `src/printer/ad5x_status_parse.cpp#parse_zcolor_silent`, `apply_zcolor_result()` |

**`Adventurer5M.json` `FFMInfo` has NO per-channel presence field.** It carries `ffmColor{1-4}` / `ffmType{1-4}` plus an active `channel`, and those colors **persist across unload/eject** — zmod never blanks `ffmColorN` when a lane is emptied. So a non-empty `ffmColorN` means "this channel was *assigned* this color", **not** "filament is loaded here". (Field-proven on raza616's hardware: he ejected *and* unloaded channel 1, yet `ffmColor1` stayed populated; a live `IFS_STATUS` reported `Ports:[F,T,T,T]` while the JSON still had `ffmColor1` set — the JSON simply does not track presence.)

**`GET_ZCOLOR SILENT=1`** is the silk-sensor truth. Text format (every line `// `-prefixed), parsed by `src/printer/ad5x_status_parse.cpp#parse_zcolor_silent`:

```
// Extruder: 3: PLA/2750E0 | IFS: True   <- summary: active lane, its mat/hex, IFS-mode flag
// 1: PLA/FFFFFF                          <- one row per LOADED slot (silk-detected)
// 3: PLA/2750E0
```

- Summary `Extruder: None (N)` = nothing at the hotend; `Extruder: N: MAT/HEX` = slot N is feeding the head. The `(N)` paren form carries the current channel.
- A **missing slot number = empty** — zmod filters slot rows by `hasFilament` from the RS-485 `silk_state` bitmask, so an absent row is the presence signal for "this lane is physically empty".
- Slot body is `MATERIAL`, `MATERIAL/HEX`, or `MATERIAL/NAME/HEX`. Material is everything before the first `/`; hex is everything after the **last** `/`. A response with slot rows but no `/HEX` is flagged `is_old_format` (pre-zmod-`ad2802ab`, Apr 2026) — presence only, colors still come from JSON.
- A response whose lines contain `action:prompt_` means **old zmod returned the interactive dialog instead of silent text** → `is_prompt_fallback` (see presence-ownership rule below).

**`IFS_STATUS`** (zmod_ifs.py `cmd_IFS_STATUS` → `ifs_data.get_values()`) is a cleaner, structured alternative that ships in zmod 1.7.1 but is **not yet consumed** by HelixScreen. It returns clean JSON:

```json
{"State": 4, "Ports": [false, true, true, true], "Silk": 14,
 "Chan": 4, "Insert": 0, "NeedInsert": false, "Stall": false, "stall_state": 0}
```

`Ports[i]` is `(silk_state >> i) & 1` — the same RS-485 bits `GET_ZCOLOR` filters on, but already decoded to booleans. `Chan` is the active port. **This is the future presence source**: it would let the backend drop both the `GET_ZCOLOR` text-scrape *and* the 5s JSON poll. Tracked as the firmware-integration headline; the upstream wishlist below (`get_status()` on `zmod_ifs`) would close the gap entirely.

#### Presence ownership rule (and the resurrection bug)

> **Presence is owned SOLELY by `GET_ZCOLOR` on modern zmod.** `parse_adventurer_json()` must NOT infer presence from `ffmColorN`. This is the fix for the channel-resurrection bug (commits `35dfcb765`, `2081e5757`).

**The bug.** Earlier code in `parse_adventurer_json()` treated a non-empty `ffmColorN` as `port_presence_[idx] = true` with no guard. Because zmod persists `ffmColorN` across unload/eject, an emptied lane was **resurrected as loaded on every content-changed poll**. The exact field report (raza616, v0.99.78): he externally unloaded channel 1 (Helix failed to clear it), then a `FIRMWARE_RESTART` fixed it (a fresh `GET_ZCOLOR` set `port_presence_[0]=false`), but then **editing channel 4's color in zmod changed the JSON content → triggered a reparse → the persisted `ffmColor1` resurrected channel 1 with its stale color**. One edit resurrected an unrelated emptied lane.

**The fix.** On modern zmod (where `GET_ZCOLOR SILENT=1` works), `parse_adventurer_json()` refreshes `colors_[]` / `materials_[]` only and leaves `port_presence_` untouched. `apply_zcolor_result()` (the silk-sensor read) is the sole presence authority, and it now also drives the `present→absent` override-clear (`clear_override_locked`) that used to ride on the JSON inference. Every JSON content change already schedules a `GET_ZCOLOR` immediately after the parse (`poll_adventurer_json()` calls `schedule_zcolor_query()`), so silk-truth presence re-establishes on the same event — the JSON setting presence was both **wrong and redundant**.

**The pre-SILENT regression (caught in review → commit `2081e5757`).** Making `GET_ZCOLOR` the sole authority breaks presence *entirely* on **old zmod**, where `GET_ZCOLOR SILENT=1` returns a prompt dialog instead of silent text. There, `apply_zcolor_result()` sees `is_prompt_fallback`, latches `zcolor_silent_supported_ = false`, and every subsequent `schedule_zcolor_query()` / `query_zcolor_silent()` no-ops forever. With JSON inference removed, every channel would be stuck EMPTY. The fix **gates the legacy `ffmColorN` inference on `!zcolor_silent_supported_`** (`parse_adventurer_json()`, the `if (!has_per_port_sensors_ && !zcolor_silent_supported_.load())` block):

- **Modern zmod** (`SILENT` works): `GET_ZCOLOR` owns presence; JSON never touches it → resurrection fixed.
- **Pre-SILENT zmod** (`zcolor_silent_supported_` latched false): no silk query exists, so JSON inference is the only fallback — `non-empty color == present`, `empty color while IDLE == eject + override-clear`. The resurrection bug can't bite here because no `GET_ZCOLOR` competes for ownership.

> **Known minor edge (accepted):** on modern zmod, an external color edit that arrives via the JSON poll *before* `GET_ZCOLOR` has confirmed presence won't sync to `lane_data` (the baseline moves without syncing). Rare, and far preferable to the constant resurrection. Confirmed acceptable in review.

**save_variables keys** (all prefixed `less_waste_`):

| Key | Type | Example |
|-----|------|---------|
| `less_waste_colors` | string[] | `['FF0000', '00FF00', '0000FF', 'FFFFFF']` |
| `less_waste_types` | string[] | `['PLA', 'PETG', 'ABS', 'TPU']` |
| `less_waste_tools` | int[16] | `[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]` |
| `less_waste_current_tool` | int | `0` (T0), `-1` (none) |
| `less_waste_external` | int | `0` (IFS mode), `1` (bypass/external) |

Tool mapping: array index = tool number (T0-T15), value = physical port (1-4, 5=unmapped).

#### Polling caution: IFS contention and print-time backoff

Two upstream-documented platform properties bound how hard HelixScreen may lean on the
IFS ([upstream-doc]; full write-up incl. attribution in
`printers/FLASHFORGE_AD5X_SUPPORT.md` § "Firmware Quirks and Operating Rules"):

- **The IFS is shared with the native display.** Errors result when the native screen
  and a mod access IFS simultaneously; most IFS settings only work with the native
  screen disabled (`DISPLAY_OFF`), and `DISPLAY_OFF_TIMEOUT=10` mitigates. HelixScreen
  runs in alternative-screen mode (`DISPLAY_OFF HELIX=1`), which is the supported
  configuration — but the constraint is why this backend polls where it can and never
  drives the IFS from two paths at once.
- **Host work during toolhead motion is hair-trigger.** Forge-X's own regression
  harness had to stop screenshotting during motion to avoid 'Timer too close'
  shutdowns (Forge-X commit `46c75cb`, 2026-08-12) — the same TTC cluster that keeps
  the AD5X MCU on Klipper 12.

That is the external rationale for the print-time poll backoff: the
`Adventurer5M.json` freshness poll runs at `kJsonPollIdle` = 5 s but slows to
`kJsonPollPrinting` = 30 s while PRINTING (`include/ams_backend_ad5x_ifs.h`; each
poll is a loopback HTTP GET on a 2-core board that is simultaneously feeding the MCU
step queue). PAUSED deliberately keeps the 5 s cadence — a pause is when a user
actually swaps a spool and relabels it.

#### Pushed by the HelixPrint plugin

Where the HelixPrint Moonraker plugin's `vendor_ad5x` feature is ready
(`moonraker-plugin/helix_vendor_ad5x.py`), the plugin stats the file once a second on the
printer and broadcasts its text as `notify_helix_vendor_state` (vendor `ad5x`, payload
`{"file", "content"}`) only when it changed and parses as JSON; the feature is ready only
once a read has parsed. The periodic poll then drops to a 30 s backstop
(`kVendorPushBackstop`, `should_poll_json(..., pushing)`), which removes most of the
loopback GETs the caution above is about while bounding how long a silent push can leave
the colours stale.

- One function applies a body either way: `apply_json_content(content, reason)` runs
  `note_json_content()`, then `parse_adventurer_json()` and `schedule_zcolor_query()` on a
  change. The poll calls it with `"json_poll"`, a push (`#on_vendor_state`) with
  `"json_push"`, so a push gets the same GET_ZCOLOR / IFS_STATUS follow-up.
- The poll's stand-downs hold for a push too: nothing is applied while the standalone module
  is live, or while the backend polls.
- `handle_status` re-reads the plugin's state each frame (`#refresh_vendor_push`). A plugin
  probe on the current connection that finds the feature ready owes one read of the file (a
  reconnect can have missed a change); anything else, no probe yet since a reconnect, a
  plugin uninstalled or too old for the contract, puts the poll back on its cadence at once.
  `on_started` / `on_stopping` reset the tracker. The rule is `vendor_state_source()`
  (`include/vendor_state_push.h`), shared with the ACE backend.
- The event-driven re-reads (`schedule_json_reread()` on sensor edges and klippy ready)
  are unchanged: they are rare, and they answer "did that edit land" sooner than any push.

#### Unattended runout detection (#1250, reported as #1247)

**The hole this fills.** `detect_load_unload_completion()` only reacts to a head-sensor transition while the action is `LOADING` or `UNLOADING`, and `check_action_timeout()` only runs during an operation phase. A head drop at `AmsAction::IDLE` with no phase tracking therefore produced **nothing at all** — the print sat paused with an empty toolhead and HelixScreen said nothing, while the reporter waited for a backup-spool switch that was never going to happen (no plugin installed).

**The authority is the switch pair, never `head_filament_`.** `parse_head_sensor()` writes `head_filament_` from *both* the toolhead switch and `ifs_motion_sensor`, and the motion sensor is device-confirmed to read `filament_detected=false` on a lane that is loaded but idle. The detector uses `head_switch_seen_ && !head_switch_present_` — the same pair as the #1065 row 28 seated head-gate.

**The predicate** (`evaluate_runout_locked()`, run from `handle_status_update()` right after `check_action_timeout()`), all of which must hold:

| Condition | Why |
|-----------|-----|
| A genuine `head_switch_present_` **true → false edge** was seen (`head_empty_since_`) | An edge, not a level: a printer that boots into a paused job with an empty toolhead has no runout to report |
| The edge was armed while nothing was in flight | `note_head_switch_reading_locked()` refuses to arm during a tracked op or a non-IDLE action |
| Print state is **PAUSED** | A real runout stops the job. While PRINTING, the same empty head is the middle of a firmware `A_CHANGE_FILAMENT`. Klipper queues a `PAUSE` behind the running macro, so a swap cannot make the job read PAUSED with the head still empty |
| `!phase_tracker_.active` **and** `action == IDLE` | The tracker covers load/unload; the action covers `do_change_tool()`, which sets `LOADING` without arming the tracker |
| `now - last_filament_op_dispatch_ >= 30 s` | `eject_lane()` and `do_unload_filament()`'s three early returns leave the backend IDLE and armless — the dispatch stamp is the only thing that sees them |
| The head has been empty for the confirm dwell | 30 s normally; **180 s** when a plugin with `variable_backup` on is installed, because that plugin's own switchover pauses, unloads and loads a replacement lane and must not be talked over |

On a raise: `runout_active_ = true`, `system_info_.filament_runout = true`, and `system_info_.action = AmsAction::ERROR`. **ERROR is not decorative** — it is the only edge `AmsErrorBridge` watches, so it is the only route to `current_error()` and the recovery modal. `check_action_timeout()` early-returns on ERROR, so the fault cannot be re-timed-out on top of itself. It clears when filament returns to the switch, when the print leaves PAUSED (which is what dismisses the modal), or via `recover()` / `reset()` / `cancel()`.

**Recovery actions** (`build_recovery_actions()` branches on `runout_active_`): `RESUME` (primary, hot), a plain `M83` + `G1 E50 F600` purge (hot), and `IFS_UNLOCK` (danger, cold-safe). The operation-timeout fault keeps its historical lone `IFS_UNLOCK`.

> **There is deliberately NO "Load slot N" recovery button**, even though a runout is exactly when the user wants one. Every AD5X load path runs `INSERT_PRUTOK_IFS`, whose macro homes itself and then moves the toolhead on its own authority (`_GOTO_TRASH`, `_SBROS_TRASH`, `_CLEAR_REZINA` nozzle wipe) — this is what `filament_ops_self_home()` is about. On the loadcell-Z AD5X that motion reaches **down into the part**; with a job owning the toolhead it trips ZMOD's `ZCONTROL_AUTO` and shuts Klipper down, recoverable only by a firmware restart (bundle `XWPBR2DX`, commit `329e731e9`). A runout state is PAUSED by construction, so the button would fire straight into that. Note the leading `_G28` is *conditional* on `homed_axes` (see `FLASHFORGE_AD5X_IFS_ANALYSIS.md` §12) and usually no-ops mid-print — that is not a reason to relax this: `homed_axes` is cleared by a Klipper error, an `M84`, or a cold resume, and the trash/wipe moves happen either way. `refuse_if_printing()` protects `load_filament()`; it does **not** protect a recovery button, which hands its gcode directly to `MoonrakerAPI::execute_gcode`, and the `_G28` is buried inside the macro where `reject_homing_during_active_print()` never sees it. The purge is a bare extruder move for the same reason — no homing, so it cannot reach the `_G28`. If a verified non-homing load-to-toolhead command ever turns up, that is the time to add the button.

> **Unverified, flagged rather than assumed:** whether a firmware tool change can make the job read PAUSED with the head still empty. The reasoning above (Klipper queues `PAUSE` behind the running macro) is first-principles, not a device observation, and the fleet's AD5X rig (commissioned 2026-08-23) has not yet been exercised for this case, nor is there an `ad5x` mock profile. If a false runout ever shows up mid-swap, the fix is to lengthen `kRunoutConfirmDelay` past a full swap (~2 min measured in bundle `NJB2U558`), not to loosen the PAUSED gate.

#### Auto-switchover plugin visibility

The `has_ifs_vars_` / `ifs_macro_confirmed_missing_` machinery distinguishes stock zMod from
the lessWaste / bambufy plugin path. #1250 surfaces it because the user needs to know which
system will handle a runout.

**All three modes have automatic slot-to-slot switchover** — verified from source
(`zmod_ifs.py:cmd_ANALOG_PRUTOK`, `bambufy.cfg:_RUNOUT_HEAD`, `lesswaste_src.cfg:_RUNOUT_HEAD`)
and corroborated on-device by raza616 and ninjamida. The original #1247 claim that "stock zMod
has no backup-spool switching at all" was wrong; zmod's own user-facing name for it is
**"Infinite Spool Mode"**.

| Mode | Trigger | Enable flag | Default |
|------|---------|-------------|---------|
| Stock zMod (`!has_ifs_vars_`) | `head_switch_sensor` runout_gcode calls `ANALOG_PRUTOK` (`ad5x_display_off.cfg`) | none — always on | on |
| bambufy | `_RUNOUT_HEAD` (plugin overrides the sensor's runout_gcode) | `variable_backup` (`bambufy.cfg:_IFS_VARS`) | **on** (`variable_backup: 1`) |
| lessWaste | `_RUNOUT_HEAD` (same shape; lessWaste is a fork of bambufy V1.2.10) | `variable_backup` (`lesswaste_src.cfg`) | off (`variable_backup: 0`) |

The match rule is identical across all three: same `ffmType` AND same `ffmColor` AND the
candidate port's presence sensor reads filament. None of the three disables switchover in
multicolor — a report that "bambufy doesn't support multicolor" describes the *de facto*
outcome of multicolor prints typically loading one spool per colour (so no same-colour backup
exists), not a code restriction.

| Getter | Values |
|--------|--------|
| `AmsBackendAd5xIfs::get_plugin()` | `IfsPlugin::None` / `LessWaste` / `Bambufy`. `None` whenever `has_ifs_vars_` is false, so stale `less_waste_*` rows left behind by an uninstalled plugin never read as installed |
| `AmsBackendAd5xIfs::plugin_backup_enabled()` | `std::optional<bool>` — `nullopt` means the macro dict never carried the key (or no plugin is installed), which is **not** the same as off |
| `AmsBackendAd5xIfs::backup_state_locked()` | The live switchover state as a tri-state, `kBackupUnknown` (-1) / `kBackupOff` (0) / `kBackupOn` (1). Stock zMod reports `kBackupOn` (ANALOG_PRUTOK is always-on); the plugin path mirrors `variable_backup` with `kBackupUnknown` when the key was never read. Feeds both the runout warning log and `get_endless_spool_capabilities()`' `enabled` axis, so the number in the log and the sentence on screen cannot disagree |

**There are no AD5X-specific XML subjects.** `ams_ifs_plugin` and `ams_ifs_backup_enabled`
existed for one release as this backend's own publication path and are gone: they never
acquired a reader, and a per-firmware subject can only ever describe one printer's answer.
The state reaches the UI through `get_endless_spool_capabilities()`, which `AmsState` turns
into the backend-neutral `ams_endless_state` / `ams_endless_text` subjects for every backend
— see [Endless Spool](FILAMENT_MANAGEMENT.md#endless-spool-shared-model) § "The status line".

**`variable_backup`.** `gcode_macro _ifs_vars`'s `get_status()` dict used to be reduced to a single "does the macro exist" bool at the `on_started()` probe and thrown away. It now flows into `parse_ifs_vars_macro_locked()`, which reads `variable_backup` (accepting the jinja int form and a bool). Note this object is **not** in the standing `objects.subscribe` set — the `on_started()` query and `recheck_ifs_vars_macro()` (fired on `notify_klippy_ready`) are the only two places it ever reaches us.

Per `printers/FLASHFORGE_AD5X_SUPPORT.md` § "lessWaste-Specific Variables" and the source
variable dumps in `printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md`, lessWaste ships
`variable_backup` defaulting **off** (`lesswaste_src.cfg`) and bambufy ships it defaulting
**on** (`bambufy.cfg:_IFS_VARS`). **Neither has been observed on a device by us** — the
defaults are source-reads, not device observations. Nothing branches on the value except the
wording, the runout-warning log, and the longer confirm delay.

**The matching rule the hint text promises is strict and must stay strict**: a backup port qualifies only when its filament **type** and **colour** both equal the active spool's *and* its own port sensor reads filament present (`find_backup_slot_locked()`). This mirrors exactly what `ANALOG_PRUTOK` (zmod_ifs.py:663-667) and `_RUNOUT_HEAD` enforce on the device.

> **PAUSE-reason follow-up, not implemented:** bambufy and lessWaste both emit `PAUSE REASON=` with one of `jam`, `broken`, `runout`, `empty`, `backup`, `loading`, `nobackup` (the last on a backup-enabled runout with no same-type+colour match — bambufy-only; verified from `bambufy.cfg`). That is a direct, unambiguous runout signal — but only on the plugin path, which is precisely the case the sensor-based detector above is *not* needed for. Parsing it would let the plugin path skip the dwell entirely. The
zmod analogue would be parsing `_PRINT_HEAD INFO=0` (its "no backup match" respond_raw
marker) as the same kind of direct signal.

### G-code Commands

| Command | Action |
|---------|--------|
| `INSERT_PRUTOK_IFS PRUTOK={port}` | Load filament from port (looks up temp from config) |
| `IFS_REMOVE_PRUTOK` | **Bare, with no `PRUTOK=`: a guaranteed no-op.** `cmd_IFS_REMOVE_PRUTOK` defaults `PRUTOK=0` and returns immediately on `prutok == 0` (zmod_ifs.py:1113). Given an explicit `PRUTOK=N` it forwards to the firmware's `_IFS_REMOVE_PRUTOK` macro for lane N, which is how `IFS_REMOVE_CURRENT_PRUTOK` calls it internally. HelixScreen never sends it, bare or otherwise |
| `REMOVE_PRUTOK_IFS PRUTOK={port}` | Toolhead unload (heat + retract the currently-loaded filament). **Not** a per-port jog — `PRUTOK=N` does not eject an idle lane; see note below |
| `IFS_F11 PRUTOK={port} LEN={mm} SPEED={s} CHECK=0` | Cold per-lane retract — reverse one idle lane's feed motor toward the spool; no heat, no presence guard. Used for idle-lane recovery (#996) |
| `A_CHANGE_FILAMENT CHANNEL={port}` | Full tool change |
| `SET_EXTRUDER_SLOT SLOT={port}` | Select slot without loading |
| `IFS_UNLOCK` | Reset IFS driver state machine |
| `IFS_MAP_TOOL TOOL={n} SLOT={m}` | Aim slicer tool T`n` at lane `m` for this print only — `n` 0-based, `m` 1-based. Refused for a lane with no filament; the refusal surfaces as a normal gcode error (HelixScreen does not pre-validate). A hand-typed `IFS_SELECT SLOT=` is never remapped |
| `IFS_MAP_TOOL RESET=1` | Restore the identity tool map |
| `_IFS_VARS key=value SHOW=0` | Persist color/type/external/tool changes (the ZMOD plugin contract; on a Forge-X port machine tool mapping goes through `IFS_MAP_TOOL` instead) |

**Tool remapping** is served by two adapters, one per firmware stack, each keeping its own contract end to end — they share only the slot model the UI reads. `remap_ready()` / `owns_tool_mapping_table()` answer whether EITHER signal was detected; with neither, the UI shows remapping as unsupported and a caller that writes anyway gets a local-only write the firmware ignores.

- **Forge-X port:** the `[ifs]` module's wire table (see State Sources). `set_tool_mapping()` validates the parameters and emits `IFS_MAP_TOOL`; it does **not** write the map locally — the module owns the table and the `printer.ifs.tool_map` echo is what promotes a mapping, so a refused write (a lane with no filament) leaves `get_tool_mapping()` reporting what the printer actually has. `reset_tool_mappings()` emits `IFS_MAP_TOOL RESET=1` the same way.
- **ZMOD + lessWaste/bambufy:** the plugin's `save_variables.<prefix>_tools` table. `set_tool_mapping()` applies the mapping locally and persists it with `_IFS_VARS tools=…` (`build_tool_map_value()` emits the whole 16-entry array — the macro replaces, not merges). This is the long-standing path, unchanged. The plugin contract has no reset verb; `reset_tool_mappings()` answers not supported there.

When both signals exist (stale plugin rows surviving a firmware swap), the wire table wins — it is live firmware state — and the detection logs that precedence rather than silently dropping the plugin signal.

**Variable persistence**: Use `_IFS_VARS` macro (not raw `SAVE_VARIABLE`) to persist slot data. `_IFS_VARS` updates both in-memory gcode variables AND `save_variables` with the correct prefix (`less_waste_*` for lessWaste, `bambufy_*` for bambufy). `SHOW=0` suppresses the interactive dialog. Example: `_IFS_VARS colors="['FF0000', '00FF00']" SHOW=0`. **The macro ships with those two plugins only** — stock zMod does not define `_IFS_VARS` at all, which is why HelixScreen cannot persist UI-side slot edits there (see the "Stock lacks this" row above).

**The `colors=`/`types=` arrays are indexed differently per plugin (#1247)** — `_IFS_VARS` *replaces* them wholesale, so the payload shape must match the consumer. bambufy keeps 4-entry, **port-indexed** lists (`_RUNOUT_HEAD` iterates `ifs.types` and reads `ifs.colors[port-1]`). lessWaste keeps 16-entry, **tool-indexed** lists projected through `variable_tools` (`_IFS_VARS colors=…` at `build_ifs_list_value()`); its `_RUNOUT_HEAD` scans all 16 tool slots comparing `ifs.colors[ifs.current_tool] == ifs.colors[index]`, so a 4-entry port-indexed push truncates the arrays and no backup lane can ever match — the "filament backup fails to switch the spool" failure. Because `SAVE_VARIABLE` persists the truncation across reboots, `parse_save_variables()` also detects a short lessWaste array and `dispatch_ifs_vars_repair()` pushes a correctly-shaped replacement once (the boot-time self-heal for installs damaged by older builds).

**Plugin compatibility**: HelixScreen auto-detects the variable prefix from whichever `save_variables` are present on the printer. Both lessWaste and bambufy use the same schema, just different prefixes.

**Unload is toolhead-oriented, not per-lane**: `REMOVE_PRUTOK_IFS PRUTOK={port}` runs the toolhead unload sequence — it heats the hotend and retracts whatever filament is currently loaded to the toolhead. The `PRUTOK={port}` argument does **not** select an idle lane to jog independently; observed on a real AD5X (native ZMOD), `REMOVE_PRUTOK_IFS PRUTOK=N` unloaded the currently-loaded filament (in a different slot) and ignored port N. (Bare `IFS_REMOVE_PRUTOK` is not a third way to do this: it is a firmware no-op, see the table above.) An older note here claimed the command "can error `No filament N in IFS`". **It cannot.** That string lives in `print_result()` on `RET_SILK` (zmod_ifs.py:789), which is reached only from the load paths; the error the unload chain actually raises is `"Failed to extract filament from extruder"` (zmod_ifs.py:1140), when the extruder sensor is still tripped after the retract. A cold per-lane retract, by contrast, **is** available at the gcode layer via `IFS_F11 PRUTOK={n} LEN={mm} SPEED={s} CHECK=0` (core ZMOD — a thin wrapper over raw serial `F11 C{port}…` with no heating and, with `CHECK=0`, no presence guard). This is why HelixScreen keeps the currently-loaded slot unloadable after runout (#995); and #996 implements HelixScreen calling `IFS_F11` directly for idle-lane recovery (e.g. a snapped chunk stuck in a lane's feed path — no hot nozzle involved). See `printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md` §12.

#### Per-lane eject (`eject_lane()`)

A real per-lane eject — pulling a whole spool's worth of filament back out of an idle lane so the user can remove it by hand — is **not** a single `IFS_F11`. A bare `IFS_F11` defaults to `LEN=90` (zmod_ifs.py `cmd_IFS_F11`, `gcmd.get_int('LEN', 90)`), which barely moves the filament, and an **unclamped** gear doesn't grip at all. `eject_lane()` (commit `dfcc83c0f`) mirrors zmod's own `_REMOVE_PRUTOK_IFS` macro with a three-command sequence:

```
IFS_F24 PRUTOK={port}                          # clamp — the gear now grips
IFS_F11 PRUTOK={port} LEN={tube} SPEED={speed} # cold retract the FULL tube length (no heat, no home)
IFS_F39 PRUTOK={port}                          # unclamp — filament is free to pull out by hand
```

- **`LEN` / `SPEED` are per-material**, resolved from zmod's /mod_data/filament.json keyed by the lane's material type. Fetched once at startup by `fetch_filament_json()` (mirrors `read_adventurer_json` threading; 404 on non-zmod is silent), parsed by `parse_filament_json()` into `filament_eject_params_`. Structure:

  ```json
  { "default": { "filament_tube_length": 1000, "filament_ifs_speed": 1200, ... },
    "PLA":     { "filament_tube_length": 1000, "filament_ifs_speed": 1200, ... },
    "PETG":    { "filament_tube_length": 650,  "filament_ifs_speed": 1200, ... } }
  ```

  Resolution order: per-material entry → file's `default` entry → hardcoded **1000 / 1200** (`filament_eject_default_`). `filament_tube_length` is the PTFE-tube length from the IFS module to the extruder (zmod default 1000 mm) — users with non-stock tubes get the right distance automatically. (Field-confirmed on raza616: `LEN=1000` ran the full retract and ended free after `F39`; his PETG tube is 650.)

- **Eject refuses the toolhead-loaded active lane.** If `system_info_.current_slot == slot_index` and the toolhead is not empty, `eject_lane()` returns `WRONG_STATE` ("Lane is loaded in toolhead / Unload from toolhead first") — a cold backward retract would fight the loaded filament. Unload it from the toolhead first. "Not empty" is `!head_empty_for_unload_routing_locked()`, the same predicate the unload router uses (below), *not* a bare `head_filament_` — the two must agree or the router's empty-head eject gets bounced by this refusal.

#### Unload routing: heated toolhead cut vs. cold lane eject

`do_unload_filament()` picks between `_IFS_REMOVE_CURRENT_PRUTOK` (heat, cut, retract the seated lane) and `eject_lane()` (cold `IFS_F24`/`IFS_F11`/`IFS_F39` on one lane). `slot_unloads_to_toolhead()` mirrors the same decision for the context-menu label, and a unit test pins the two together across the whole authority matrix. Order matters — the slot-identity guards run *before* the head test:

| # | Condition | Route | Why |
|---|-----------|-------|-----|
| 1 | Active slot known, tapped slot is neither it nor the IFS_STATUS-seated one | cold eject of the tapped lane | That lane's filament is in the lane, not the nozzle. Otherwise "unload channel 1" heats and backs out channel 3 (raza616 `HKHZFYB2`) |
| 2 | Active pointer lost, seated channel known, tapped slot is not it | cold eject of the tapped lane | Chan is the seated authority; the alternative is a wrong-lane heat+cut (`5HR3HHS6`) |
| 3 | Toolhead reads empty | cold eject (of the tapped slot, else the seated one, else the active one; hard error if none) | `_IFS_REMOVE_CURRENT_PRUTOK` early-returns on an empty extruder sensor, so the cut would home and do nothing (`7AC4SDEX`) |
| 4 | otherwise | heated toolhead cut | Includes the unknown-origin recovery case (both authorities lost, head loaded) |

**Row 4's unknown-origin case is the UI's only door in that state.** When both authorities are lost
(`current_slot == -1` with the head switch present: the power-cycle state, and exactly what
`toolhead_filament_unaccounted()` keys on), the lane context menus withdraw their Unload/Eject
entirely: the lane a menu names may not be the seated one, and a cold eject of the seated lane
grinds un-cut filament. The AMS sidebar's panel-level Unload and the Filament panel's Unload stay
live instead, dispatching `unload_filament(-1)`; `do_unload_filament()` takes the row-4 heated
cut, so the firmware resolves the real channel, not the screen.

Native ZMOD's `_IFS_REMOVE_CURRENT_PRUTOK` finds the channel itself only when
`FFMInfo.channel` names one: with it 0, `IFS_REMOVE_PRUTOK PRUTOK=0` returns at once, so the
printer homes and moves nothing. The standalone module's `IFS_UNLOAD` needs a lane too. The
next section covers how both find it.

#### Unload with no lane named (#1324)

When the head holds filament and `current_slot` is -1, a slotless unload has to find its
lane before it dispatches. `src/printer/ams_backend_ad5x_ifs.cpp#decide_unload_lane` is the
pure rule, over the firmware kind, the records, the per-lane presence bits and the head
sensor; `unload_lane_decision_locked()` feeds it the live state. Top-down:

| # | State | Verdict | What happens |
|---|---|---|---|
| 1 | A record names the lane: the active slot, the seated channel (`ifs_loaded` on the module, Chan/`FFMInfo.channel` on ZMOD), ZMOD's `FFMInfo.channel` alone, or the remembered lane (`remembered_unload_lane_locked()`, the `lane_data` DB `seated` key, only while that port reads present; on native ZMOD only once Adventurer5M.json has been read) | `UnloadLane` | Unload it, no question |
| 2 | Module only: `ifs_at_hub = n` and port n reads present | `UnloadLane` | `IFS_UNLOAD SLOT=n`, no question. Only one lane fits in the shared path, and `IFS_LOAD` claims it before the feed, so a load that failed after the feed leaves exactly this record |
| 3 | Exactly one lane reads present | `ConfirmLane` | The lane picker with that one lane. Never dispatched on the inference alone: a runout tail or filament fed from outside the IFS breaks it |
| 4 | Several lanes read present | `PickLane` | The lane picker |
| 5 | No lane reads present (a runout tail) | `ConfirmClear` | A dialog asks to confirm the extruder-only clear |

Only row 2 skips a question it could not otherwise answer. Rows 3-5 hold on both firmwares.
`module_at_hub_` is read from every `save_variables` frame, because that frame can arrive
before the one that latches the module, and is consulted only while the module is live.

`do_unload_filament()` acts on the verdict. Anything only the user can answer (rows 3-5) returns
`WRONG_STATE` before setting `HEATING`, so nothing warms up for an unload that cannot run.
On the module a lane always goes out as `IFS_UNLOAD SLOT={n}`, including the active slot: a
bare `IFS_UNLOAD` falls back on the module's own records, which can disagree with the screen.

On native ZMOD with no active slot, `_IFS_REMOVE_CURRENT_PRUTOK` unloads the lane in
`FFMInfo.channel` and does nothing when that is 0. `ffm_file_read_` says the file has been read and
`ffm_file_channel_` holds the channel exactly as it said it (nullopt before the first read or
with no integer channel), apart from `ffm_channel_`,
Helix's working copy, which `clear_seated_if_ejected_locked()` zeroes on an eject while the file
keeps the ejected lane. `native_lane_dispatch()` decides:

| File says | Dispatch |
|---|---|
| not read yet | refused ("Still reading the printer's lane record"), and a re-read is requested |
| read, but no integer `FFMInfo.channel` (or no `FFMInfo`) | no lane can be named to the firmware: refused with "The printer reports no lane record, so only clearing the extruder can unload it.", and `unload_lane_question()` offers only the clear, with that sentence as the confirmation's text (`UnloadLaneQuestion::reason`) |
| 0 | `SET_EXTRUDER_SLOT SLOT={n}` then `_IFS_REMOVE_CURRENT_PRUTOK`, one script. `SET_EXTRUDER_SLOT` is ZMOD's `ad5x.cfg` wrapper over `cmd_SET_EXTRUDER_SLOT` in ZMOD's zmod_color.py, which writes `FFMInfo.channel` and moves nothing |
| lane n | `_IFS_REMOVE_CURRENT_PRUTOK` |
| another lane | refused ("The printer records lane {} in the toolhead, not this one"): declaring would overwrite the firmware's record, and the plain macro would unload the other lane |

The extruder-only clear (`kClearToolheadSlot`) is `clear_toolhead_gcode()`. No lane names a
material, so its temperature `t` is the nozzle target when that is at least 150C, else 220C.

- **module:** `_IFS_SENSOR_RESUME` then `_IFS_CLEAR_EXTRUDER TEMP={t}` with no `SLOT=`: cut,
  wipe and the extruder withdraw, clamping and retracting no lane. The private macro does not
  restore a toolhead sensor an earlier failed op left muted; every public entry point does that
  first, so Helix does too.
- **native ZMOD:** `_REMOVE_PRUTOK_IFS PRUTOK=0 TEMP={t}`, the `prutok == 0` branch of ZMOD's
  `_REMOVE_PRUTOK_IFS` in `ad5x_display_off.cfg`: home, heat, cut, drop the stub in the trash,
  withdraw 60 mm, touching no lane. The public `REMOVE_PRUTOK_IFS` cannot be told a temperature
  (it fills TEMP from the lane's config, and lane 0 has none), and preheating first does not
  help: the macro sets its own TEMP and waits within -2/+4 of it.

Both refuse a clear while the head reads empty, by the same switch-pair predicate as the eject
routing, so a unit whose toolhead switch has published is gated by it. A unit whose switch reads
present with an empty head (row 7 of the research table) cannot be told apart; the clear still
waits for the user's confirmation there. A clear retracts no lane, so the strand that came from
one stays at the hub with no record naming it, and a load of a different lane can feed into
it. That is the price of not guessing: retracting the wrong lane leaves the same strand behind
and also writes records saying the path is clear.

The module's own `IFS_UNLOAD` falls back the same way (ad5m-forgex `macros/ifs.cfg`):
`SLOT=`, then `ifs_loaded`, then `ifs_at_hub` while the toolhead sensor reads filament and that
lane reads present (clearing both records on success), then the extruder-only clear with a
`!!` warning. A bare `IFS_UNLOAD` from Fluidd takes rows 2 and 5 itself. HelixScreen never sends
it bare, so it works on a module without that fallback too.

The UI asks before any of this. `AmsBackend::unload_lane_question()` asks nothing by default;
on this backend it returns the lanes to offer (rows 3-4) or `confirm_clear_toolhead` (row 5).
The AMS sidebar's Unload (`src/ui/ui_ams_sidebar.cpp#"void AmsOperationSidebar::handle_unload()"`)
and the Filament panel's Unload (`src/ui/ui_panel_filament.cpp#handle_unload_button`) both call
`src/ui/ui_unload_lane_choice.cpp#choose_unload_lane` first. Every other slotless unload asks
through `src/ui/ui_unload_lane_choice.cpp#ask_lane_then_unload`, which sends the answer straight
to `execute_filament_unload()`: the runout dialog's Unload
(`src/ui/ui_filament_runout_handler.cpp#dispatch_unload`) and the filament sensor tile's
(`src/ui/panel_widgets/filament_sensor_widget.cpp#dispatch_unload`) when the runout names no
lane, and bed drying's unload-first (`src/ui/ui_bed_drying_modal.cpp#unload_before_drying`),
where a Cancel leaves the drying unstarted. The lane context menus name their lane and withdraw
Unload in this state; the bypass toggle and the external spool unload only with a lane active
or for the bypass spool, neither of which reaches the question. Lanes open the slot picker titled
"Which lane is in the toolhead?" anchored to the Unload button: one row per lane, a "Clear
extruder only" row (`FilamentSlotPicker::Options::extra_choice`, answers `kClearToolheadSlot`)
and a "Cancel" row (`Options::cancel_choice`), even for a single lane, so the harmless way out
is always an explicit row. Row 5 opens a `modal_confirm` "Clear the extruder?" with Clear and
Cancel. Cancel, a backdrop tap and ESC send nothing. The answer re-enters the same handler as an
explicit slot: the sidebar passes `lane_in_toolhead`, so the stepper treats that lane as the
loaded one, and the Filament panel holds it in `chosen_unload_lane_` through its preheat.
`plan_unload()` dispatches `kClearToolheadSlot` under the same unaccounted-toolhead condition
as `kActiveHeadSlot`. With no question, the handler goes on to `unload_filament(-1)`, which
handles rows 1 and 2 itself.

The `ifs-module` mock can stage every row: `HELIX_MOCK_IFS_UNACCOUNTED`, `HELIX_MOCK_IFS_LANES`
and `HELIX_MOCK_IFS_AT_HUB` (`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`). Unit coverage: the
decision table, the module cases and the no-bare-`IFS_UNLOAD` sweep in
`tests/unit/test_ams_ad5x_ifs_module.cpp`, the native ZMOD cases (file read, eject) in
`tests/unit/test_ams_backend_ad5x_ifs.cpp`, the picker, dialog and sidebar dispatch in
`tests/unit/test_unload_lane_choice.cpp`, the Filament panel route in
`tests/unit/test_filament_panel_op_slot.cpp`, and the mock knobs in
`tests/unit/test_mock_ifs_module_env.cpp`.

**"Empty" for row 3 is the switch pair, not `head_filament_`** (`head_empty_for_unload_routing_locked()`):

```
head_switch_seen_ ? !head_switch_present_ : !head_filament_
```

Positive switch evidence is required to claim empty, because the errors are not symmetric. A false *empty* cold-ejects seated, un-cut filament and grinds it (raza616 #981). A false *loaded* only reaches a firmware no-op. `head_filament_`'s known failure mode — `parse_head_sensor()` also writes it from `ifs_motion_sensor`, which reads `filament_detected=false` on a loaded-but-idle lane — produces the dangerous direction, so it can no longer claim empty on its own. Motion-only firmware never sets `head_switch_seen_`, so it falls back to the historical `!head_filament_` unchanged.

`can_unload_from_toolhead()` deliberately does **not** move onto the switch pair: it only decides whether the Unload affordance is offered, and its harmful direction is the opposite one (a false empty would hide the #995 recovery affordance for filament that is physically seated).

> **The switch pair is a proxy for a sensor we do not read.** The firmware's actual gate is `get_extruder_sensor()` (zmod_ifs.py:1149), an ADC read of `temperature_sensor filamentValue` (`result = value >= 0.72` when `value > 0.3`, `True` otherwise — a missing reading counts as loaded, zmod_ifs.py:353-361). HelixScreen subscribes to it nowhere. Subscribing is the proper fix; it needs a session on the fleet's AD5X rig (commissioned 2026-08-23) to confirm the object is published — no `ad5x` mock profile exists.

#### External-change triggers (the gcode-response listener)

`register_zcolor_listener()` subscribes to `notify_gcode_response`; `on_gcode_response_line()` schedules a `GET_ZCOLOR` re-read when it sees an externally-driven change (commit `cafcff3ad` added the bare-`Extruder:` trigger). The watched signals:

| Token in stream | Why it fires | Notes |
|-----------------|--------------|-------|
| `RUN_ZCOLOR` / `CHANGE_ZCOLOR` | Deliberate external color/material edit (AD5X LCD, Mainsail, zmod COLOR macro). HelixScreen persists colors by writing `Adventurer5M.json` directly and **never** emits these — so they can only be external. | `CHANGE_ZCOLOR SLOT=N` carrying a real locked override also clears that stale override so the new firmware color wins (#981). |
| bare `Extruder: <N>` | zmod's `_SET_EXTRUDER_SLOT` (zmod_color.py `cmd_SET_EXTRUDER_SLOT`) emits `Extruder: {zslot}` via `respond_raw` at the channel-commit step near the **end** of an operation. This is the marker that catches **external unloads/loads done via zmod's own color macro**, where the stream carries no `RUN_ZCOLOR`/`CHANGE_ZCOLOR`. | Matched by a **strict** regex (`^\s*(?://\s*)?Extruder:\s*\d+\s*$`). |

**Why `IN_ZCOLOR` is NOT watched.** An external unload via zmod's color macro emits `IN_ZCOLOR SLOT=N NAPR=0/1` (load/unload) — but the literal `IN_ZCOLOR` token only appears in the **dialog button *definition* echo** (`action:prompt_button Unload|IN_ZCOLOR SLOT=N NAPR=1…`) at *prompt-render* time, **not** when the unload actually runs. Watching it would false-fire on dialog-open and still miss the real unload. The bare `Extruder: <N>` channel-commit marker is the reliable terminal signal instead.

**Why the bare-`Extruder:` regex is strict.** It must NOT match the `GET_ZCOLOR SILENT` summary (`Extruder: ... | IFS: True`), the interactive prompt (`action:prompt_text Extruder: ... | IFS:`), or per-slot rows (`3: PLA/HEX`) — all of which carry a ` | IFS:` suffix or a different shape. Combined with the `zcolor_query_active_` early-return guard (which buffers our own in-flight `GET_ZCOLOR` response echoes), this keeps the v0.99.51 **self-feedback spam loop** closed: `GET_ZCOLOR` never emits a bare `Extruder: N`, so a re-read can't re-trigger itself. (HelixScreen's own `SET_EXTRUDER_SLOT` *does* emit a bare `Extruder: N`, but a re-read after a Helix-initiated tool change is harmless — `schedule_zcolor_query()` is debounced + idempotent.) The bg-side pre-filter in `register_zcolor_listener()` admits the cheap `Extruder:` substring; the strict regex runs on the main thread in `on_gcode_response_line()`. **If you add a new trigger, update both the bg-side admit filter and the main-thread branch** — otherwise the new token is silently dropped on busy print streams.

#### zmod IFS command reference

The raw IFS commands (zmod_ifs.py registrations + docs/en/AD5X.md). `F##` numbers are thin wrappers over raw serial `F## C{port}…`. Not every raw `F##` the IFS firmware accepts is exposed as a zmod gcode macro — e.g. `F19` is used at the raw-serial layer but has no `IFS_F19` command (community knowledge, ninjamida's multi-IFS project); only the subset ZMOD actually drives is wrapped:

| Command | Action |
|---------|--------|
| `IFS_F10` | Insert filament (`F10 C{port} L{len} S{speed}`) |
| `IFS_F11 [LEN=mm] [SPEED=s] [CHECK=0/1]` | Remove/retract filament. **`LEN` defaults to 90** (barely moves) — pass the tube length for a full eject |
| `IFS_F13` | Query IFS state |
| `IFS_F24 PRUTOK=N` | Clamp the lane (gear grips) |
| `IFS_F39 PRUTOK=N` | Unclamp one lane (filament free to pull) |
| `IFS_F18` | Unclamp **all** lanes at once — no `PRUTOK` needed. ⚠️ zmod's docs/en/AD5X.md mistranslates this as "Filament purge everywhere"; the actual handler (`cmd_IFS_F18`) responds *"Unlocking all filaments"*. Handy for recovery when you don't know which lane is clamped (community tip, ninjamida) |
| `IFS_F112` | Stop filament feed |
| `IFS_STATUS` | Structured JSON state (`State`/`Ports`/`Silk`/`Chan`/…) — clean future presence source |
| `GET_ZCOLOR SILENT=1` | Per-slot loaded state + active lane as `// `-prefixed text (silk-sensor truth) |
| `REMOVE_PRUTOK_IFS PRUTOK=N` | Toolhead unload (heat + retract); **not** a per-lane jog |
| `IN_ZCOLOR SLOT=N NAPR=0/1` | zmod color-macro load (`NAPR=0`) / unload (`NAPR=1`) — emitted on the AD5X LCD / Mainsail path |

### Path Topology

```
  Port 1 ──┐
  Port 2 ──┤
            ├── Combiner ── Toolhead
  Port 3 ──┤
  Port 4 ──┘
```

`PathTopology::LINEAR` — 4 independent lanes merge at a single combiner before the toolhead.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | `Available` in every mode (stock zMod `FirmwareManaged` / plugin `PluginReadOnly`) | `ReadOnly` always - see below |
| Tool Mapping | Yes — `printer.ifs.tool_map` (Forge-X port) or `_IFS_VARS` (ZMOD + lessWaste/bambufy) | Forge-X: `IFS_MAP_TOOL` (one entry per module channel; AD5X: T0–T3 → 4 lanes). ZMOD: `_IFS_VARS tools=` (16-entry table). Neither signal: unsupported |
| Bypass Mode | Yes | Via `less_waste_external` |
| Spoolman | Optional | Works if configured |
| Auto-Heat on Load | No | -- |
| Dryer | No | -- |
| Device Actions | No | -- |
| Runout detection | Yes | Sensor-derived, HelixScreen-side — see "Unattended runout detection" above |
| Backup-spool switchover | Firmware-only | `variable_backup` on the `_ifs_vars` macro, read the same way whichever plugin is detected. HelixScreen reports the state, it does not perform the swap |

**Endless spool on IFS is read-only on purpose.** `get_endless_spool_capabilities()`
reports `Available` + `FirmwareManaged` + `provider="zmod"` + `enabled=On` while
`has_ifs_vars_` is false, because stock zMod's `ANALOG_PRUTOK` runs always-on with no toggle
- the [#1247](https://github.com/prestonbrown/helixscreen/issues/1247) reporter's original
misexpectation ("stock zMod has no switchover") was refuted by a source read of
`zmod_ifs.py:cmd_ANALOG_PRUTOK` plus on-device confirmation from raza616. Once `_IFS_VARS`
answers, availability stays `Available`, `provider` names the plugin (`"lessWaste"` or
`"bambufy"`, from the detected variable prefix), `restriction` becomes `PluginReadOnly`, and
`enabled` mirrors `variable_backup` - including a genuine `Unknown` when the key was never
read, since flattening that to `Off` would promise the user that no swap will happen when we
simply did not read the setting. Editability stays `ReadOnly` + `PluginReadOnly`: `backup` is
never written. The only write path would be `write_ifs_var("backup", …)`, which is a bare
`_IFS_VARS` G-code whose failure surfaces only as the console "Unknown command" latch that
demotes `has_ifs_vars_` for the session - not something to drive a user-facing toggle from.

There is no per-slot relation either, so no backup dropdown appears. What the plugin *will*
switch to is answered instead by `endless_spool_backup_eligibility()`, which IFS overrides
with the rule the firmware enforces: exact material **and** exact colour **and** the port
reporting filament present. It shares `backup_eligible_locked()` with
`find_backup_slot_locked()`, so the runout detail text and the eligibility answer cannot
drift apart.

These capabilities are the ONLY path this state takes to the UI. The AD5X-specific
`ams_ifs_plugin` / `ams_ifs_backup_enabled` subjects have been retired in favour of
`ams_endless_state` / `ams_endless_text`, which `AmsState` publishes from
`get_endless_spool_capabilities()` for every backend.

### Open Issues & Debugging Notes

> **Test-device history.** Until Aug 2026 HelixScreen shipped IFS support **blind** — no AD5X in the test fleet — and every IFS fix from that era was field-validated through users, primarily **raza616** (the most active AD5X/IFS reporter); treat that field evidence as ground truth for the fixes it validated. Our own rig (commissioned 2026-08-23: ZMOD 1.7.2 on stock base 1.1.7, stock Klipper 12 MCU — see `printers/FLASHFORGE_AD5X_SUPPORT.md` § "Firmware Quirks and Operating Rules") covers going-forward verification. Prefer regression tests + the mock backend (`HELIX_MOCK_AMS=ifs`) for anything that can't be exercised on hardware.

**Stuck-purge on load — KNOWN OPEN, UNRESOLVED.** Loading a lane via the multi-filament screen can leave HelixScreen stuck displaying "purging" indefinitely. **No confirmed cause; do not ship a speculative fix.** Dead ends already ruled out:

- *Head-sensor-clobber theory* — **disproved.** raza's live `QUERY_FILAMENT_SENSOR` showed both `head_switch_sensor` and `ifs_motion_sensor` detecting filament when loaded. (Bundle snapshots show `head_switch=false` in all three captures, but that reconciles to "nothing at the head *at capture time*" — raza had already unloaded — not a sensor inversion.)
- *`BlockingIOError [Errno 11]` at `gcode.py:459 _respond_raw`* — **red herring.** It's present in the *not*-stuck bundle too, in a bed-mesh-dump context. It's console-flood backpressure (drops echoed text, not state).

To actually crack it, capture — **while stuck**:

1. A debug bundle whose `log_tail` is **verified fresh** (its last timestamp == the bundle timestamp; see caveat below).
2. A simultaneous live `QUERY_FILAMENT_SENSOR` for both `head_switch_sensor` and `ifs_motion_sensor`.
3. A live `IFS_STATUS`.
4. zmod's own `/var/log/messages` — **the "did the load actually finish inside zmod" answer lives here, NOT in the bundle.**

> **Debug-bundle caveat (cost a whole investigation):** the bundle's `log_tail` can be a **stale ring buffer that predates the incident**. In the stuck-purge bundles the `log_tail` ended *before* the reported event, so the `RUN_ZCOLOR` / `IN_ZCOLOR` / `ActionPrompt` lines "read from the log" were an **old session**, and `klipper_log` was just a 74-second idle window (config dump + bed-mesh table). **Always confirm the `log_tail`'s last timestamp matches the bundle timestamp before trusting any "from the log" claim.** When the bundle is stale, the only valid runtime evidence is live console pastes.

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend_ad5x_ifs.h` | Backend class declaration |
| `src/printer/ams_backend_ad5x_ifs.cpp` | Full implementation |
| `tests/unit/test_ams_backend_ad5x_ifs.cpp` | Unit tests |
| `docs/devel/printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md` | Protocol research |

### Automatic Setup

AD5X users running ZMOD firmware get automatic detection — no configuration needed. When HelixScreen connects to a Moonraker instance with IFS sensors, it:

1. Detects `filament_switch_sensor _ifs_port_sensor_*` in object list
2. Sets `AmsType::AD5X_IFS`
3. Subscribes to `save_variables` for filament state
4. Creates `AmsBackendAd5xIfs` backend
5. Queries initial state via `printer.objects.query`

Existing beta testers upgrading to a version with IFS support will see the filament panel populate automatically on next connection.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
