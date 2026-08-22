# AD5X IFS (FlashForge Adventurer 5X) Filament Backend

The FlashForge Adventurer 5X 4-lane Intelligent Filament Switching (IFS) system runs on
a separate STM32 MCU and is supported through ZMOD firmware (v1.7.0+). Topology is
`PathTopology::LINEAR` - the four ports merge at a single combiner before the toolhead.

## AD5X IFS (FlashForge Adventurer 5X)

> **Status: TESTING** — This backend is functional but not yet fully supported. It is available for user testing and feedback. Please report issues via GitHub.

The AD5X has a 4-lane Intelligent Filament Switching (IFS) system controlled by a separate STM32 MCU. HelixScreen supports it through ZMOD firmware (ghzserg's Klipper mod for FlashForge printers).

> **Required firmware**: [ZMOD open-source firmware](https://github.com/ghzserg/zmod) **v1.7.0 or newer** (v1.7.0, Mar 2026, is the first release with explicit HelixScreen integration via `DISPLAY_OFF HELIX=1`). Hard minimum: v1.6.2 (Oct 2025), when the `less_waste_*` `save_variables` plumbing first appeared via the bambufy plugin — older versions are missing the slot-color/material surface we read.
>
> Note: this is ZMOD's own version, not FlashForge stock firmware. ZMOD supports stock AD5X bases from v1.0.2 (Jan 2025) onward; no specific FF stock version is required.

### Detection

IFS is detected via `filament_switch_sensor _ifs_port_sensor_{1-4}` or `filament_motion_sensor _ifs_motion_sensor_{1-4}` in `printer.objects.list`. The leading space in sensor names is intentional — it's a Klipper object naming convention.

Detection is gated by `!has_mmu_` — if Happy Hare or AFC is already detected, IFS sensors are ignored (priority: HH > AFC > IFS).

### State Sources

Stock zMod owns two Klipper objects — `zmod_ifs` and `zmod_color` — that hold the authoritative per-channel state, but their rich APIs are `printer.lookup_object()`-only (no `get_status()`), so Moonraker cannot see them. What Moonraker actually exposes depends on whether the lessWaste / bambufy plugins are installed.

**Shared (stock zMod and plugins both provide):**

| Source | Data | Notes |
|--------|------|-------|
| `filament_switch_sensor head_switch_sensor` | Toolhead filament presence | Authoritative NOZZLE/TOOLHEAD indicator |
| `filament_motion_sensor ifs_motion_sensor` | Filament moving **post-hub**, inside the IFS | Single boolean on stock zMod. Maps to `OUTPUT` segment — **not** the toolhead. Replaced by per-port sensors when plugins are installed. |
| `Adventurer5M.json` (Moonraker file API) | Per-channel color + material type | Polled + re-read on sensor edges / gcode responses. No push notifications. |

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

> **Upstream wishlist:** add `get_status()` to `zmod_ifs` and `zmod_color` in stock zMod. That would close the plugin gap entirely and let HelixScreen drop the `Adventurer5M.json` polling path. Until then, users without a plugin see a degraded UI (no per-port HUB presence, no live tool map, no bypass flag, no atomic color updates).

> **Sensor-location correction:** the `ifs_motion_sensor` sits **inside the IFS immediately after the hub**, not at the toolhead. The current backend routes it through `parse_head_sensor()` as a simplification; a proper fix would map it to `PathSegment::OUTPUT` and require the toolhead switch for `filament_loaded` / load-complete detection.

#### The two data sources, and which one owns what

On native ZMOD (no lessWaste / bambufy plugin) the backend reconciles **two** independent reads. They answer different questions and must not be confused — conflating them is the root of the resurrection bug documented below.

| Source | Transport | Question it answers | Code |
|--------|-----------|---------------------|------|
| `Adventurer5M.json` `FFMInfo` | Moonraker `download_file("config", "Adventurer5M.json")`, 5s content-compare poll | What **color / material** is *assigned* to each channel (persisted metadata) | `parse_adventurer_json()`, `poll_adventurer_json()`, `note_json_content()` |
| `GET_ZCOLOR SILENT=1` (and, future, `IFS_STATUS`) | gcode console, on-demand | Which lanes **physically have filament** (RS-485 silk sensor) + the active lane | `query_zcolor_silent()`, `parse_zcolor_silent()`, `apply_zcolor_result()` |

**`Adventurer5M.json` `FFMInfo` has NO per-channel presence field.** It carries `ffmColor{1-4}` / `ffmType{1-4}` plus an active `channel`, and those colors **persist across unload/eject** — zmod never blanks `ffmColorN` when a lane is emptied. So a non-empty `ffmColorN` means "this channel was *assigned* this color", **not** "filament is loaded here". (Field-proven on raza616's hardware: he ejected *and* unloaded channel 1, yet `ffmColor1` stayed populated; a live `IFS_STATUS` reported `Ports:[F,T,T,T]` while the JSON still had `ffmColor1` set — the JSON simply does not track presence.)

**`GET_ZCOLOR SILENT=1`** is the silk-sensor truth. Text format (every line `// `-prefixed), parsed by `parse_zcolor_silent()`:

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

> **Unverified, flagged rather than assumed:** whether a firmware tool change can make the job read PAUSED with the head still empty. The reasoning above (Klipper queues `PAUSE` behind the running macro) is first-principles, not a device observation, and there is no AD5X in the fleet and no `ad5x` mock profile to test it on. If a false runout ever shows up mid-swap, the fix is to lengthen `RUNOUT_CONFIRM_DELAY` past a full swap (~2 min measured in bundle `NJB2U558`), not to loosen the PAUSED gate.

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
| Stock zMod (`!has_ifs_vars_`) | `head_switch_sensor` runout_gcode calls `ANALOG_PRUTOK` (`ad5x_display_off.cfg:39-44`) | none — always on | on |
| bambufy | `_RUNOUT_HEAD` (plugin overrides the sensor's runout_gcode) | `variable_backup` (`bambufy.cfg:_IFS_VARS`) | **on** (`variable_backup: 1`) |
| lessWaste | `_RUNOUT_HEAD` (same shape; lessWaste is a fork of bambufy V1.2.10) | `variable_backup` (`lesswaste_src.cfg:969`) | off (`variable_backup: 0`) |

The match rule is identical across all three: same `ffmType` AND same `ffmColor` AND the
candidate port's presence sensor reads filament. None of the three disables switchover in
multicolor — a report that "bambufy doesn't support multicolor" describes the *de facto*
outcome of multicolor prints typically loading one spool per colour (so no same-colour backup
exists), not a code restriction.

| Getter | Values |
|--------|--------|
| `AmsBackendAd5xIfs::get_plugin()` | `IfsPlugin::None` / `LessWaste` / `Bambufy`. `None` whenever `has_ifs_vars_` is false, so stale `less_waste_*` rows left behind by an uninstalled plugin never read as installed |
| `AmsBackendAd5xIfs::plugin_backup_enabled()` | `std::optional<bool>` — `nullopt` means the macro dict never carried the key (or no plugin is installed), which is **not** the same as off |
| `AmsBackendAd5xIfs::backup_state_locked()` | The live switchover state as a tri-state, `BACKUP_UNKNOWN` (-1) / `BACKUP_OFF` (0) / `BACKUP_ON` (1). Stock zMod reports `BACKUP_ON` (ANALOG_PRUTOK is always-on); the plugin path mirrors `variable_backup` with `BACKUP_UNKNOWN` when the key was never read. Feeds both the runout warning log and `get_endless_spool_capabilities()`' `enabled` axis, so the number in the log and the sentence on screen cannot disagree |

**There are no AD5X-specific XML subjects.** `ams_ifs_plugin` and `ams_ifs_backup_enabled`
existed for one release as this backend's own publication path and are gone: they never
acquired a reader, and a per-firmware subject can only ever describe one printer's answer.
The state reaches the UI through `get_endless_spool_capabilities()`, which `AmsState` turns
into the backend-neutral `ams_endless_state` / `ams_endless_text` subjects for every backend
— see [Endless Spool](FILAMENT_MANAGEMENT.md#endless-spool-shared-model) § "The status line".

**`variable_backup`.** `gcode_macro _ifs_vars`'s `get_status()` dict used to be reduced to a single "does the macro exist" bool at the `on_started()` probe and thrown away. It now flows into `parse_ifs_vars_macro_locked()`, which reads `variable_backup` (accepting the jinja int form and a bool). Note this object is **not** in the standing `objects.subscribe` set — the `on_started()` query and `recheck_ifs_vars_macro()` (fired on `notify_klippy_ready`) are the only two places it ever reaches us.

Per `printers/FLASHFORGE_AD5X_SUPPORT.md` § "lessWaste-Specific Variables" and the source
variable dumps in `printer-research/FLASHFORGE_AD5X_IFS_ANALYSIS.md`, lessWaste ships
`variable_backup` defaulting **off** (`lesswaste_src.cfg:969`) and bambufy ships it defaulting
**on** (`bambufy.cfg:_IFS_VARS`). **Neither has been observed on a device by us** — the
defaults are source-reads, not device observations. Nothing branches on the value except the
wording, the runout-warning log, and the longer confirm delay.

**The matching rule the hint text promises is strict and must stay strict**: a backup port qualifies only when its filament **type** and **colour** both equal the active spool's *and* its own port sensor reads filament present (`find_backup_slot_locked()`). This mirrors exactly what `ANALOG_PRUTOK` (`zmod_ifs.py:663-667`) and `_RUNOUT_HEAD` enforce on the device.

> **PAUSE-reason follow-up, not implemented:** bambufy and lessWaste both emit `PAUSE REASON=` with one of `jam`, `broken`, `runout`, `empty`, `backup`, `loading`, `nobackup` (the last on a backup-enabled runout with no same-type+colour match — bambufy-only; verified from `bambufy.cfg:149`). That is a direct, unambiguous runout signal — but only on the plugin path, which is precisely the case the sensor-based detector above is *not* needed for. Parsing it would let the plugin path skip the dwell entirely.

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
| `_IFS_VARS key=value SHOW=0` | Persist color/type/tool/external changes |

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

**"Empty" for row 3 is the switch pair, not `head_filament_`** (`head_empty_for_unload_routing_locked()`):

```
head_switch_seen_ ? !head_switch_present_ : !head_filament_
```

Positive switch evidence is required to claim empty, because the errors are not symmetric. A false *empty* cold-ejects seated, un-cut filament and grinds it (raza616 #981). A false *loaded* only reaches a firmware no-op. `head_filament_`'s known failure mode — `parse_head_sensor()` also writes it from `ifs_motion_sensor`, which reads `filament_detected=false` on a loaded-but-idle lane — produces the dangerous direction, so it can no longer claim empty on its own. Motion-only firmware never sets `head_switch_seen_`, so it falls back to the historical `!head_filament_` unchanged.

`can_unload_from_toolhead()` deliberately does **not** move onto the switch pair: it only decides whether the Unload affordance is offered, and its harmful direction is the opposite one (a false empty would hide the #995 recovery affordance for filament that is physically seated).

> **The switch pair is a proxy for a sensor we do not read.** The firmware's actual gate is `get_extruder_sensor()` (zmod_ifs.py:1149), an ADC read of `temperature_sensor filamentValue` (`result = value >= 0.72` when `value > 0.3`, `True` otherwise — a missing reading counts as loaded, `zmod_ifs.py:353-361`). HelixScreen subscribes to it nowhere. Subscribing is the proper fix; it needs a real AD5X to confirm the object is published, and there is no AD5X in the fleet and no `ad5x` mock profile.

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
| Tool Mapping | Yes | Yes (16 tools → 4 ports) |
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

> **No AD5X test device.** HelixScreen ships IFS support **blind** — there is no AD5X in the test fleet. Every IFS fix is field-validated through users, primarily **raza616** (the most active AD5X/IFS reporter). Treat live Discord console pastes and freshly-captured debug bundles as the ground truth, and prefer regression tests + the mock backend (`HELIX_MOCK_AMS=ifs`) for anything that can't be exercised on hardware.

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
| `tests/unit/test_ams_backend_ad5x_ifs.cpp` | Unit tests (16 cases, 100+ assertions) |
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
