# Happy Hare Filament Backend

Happy Hare is a Klipper add-on for ERCF, Tradrack, and other selector-based
multi-filament systems (MMUs) - one shared extruder fed by a moving selector.
Topology is `PathTopology::LINEAR`: the selector picks one gate from the spools;
slot count is the gate count from `[mmu_machine]` (multi-unit EMU rigs appear as multiple units).

## Happy Hare (MMU)

Happy Hare is a Klipper add-on for ERCF, Tradrack, and other selector-based multi-filament systems.

### Detection

Klipper object `mmu` in `printer.objects.list` sets `AmsType::HAPPY_HARE`.

### Moonraker Variables

| Variable | Type | Description |
|----------|------|-------------|
| `printer.mmu.gate` | int | Current gate (-1=none, -2=bypass) |
| `printer.mmu.tool` | int | Current tool number |
| `printer.mmu.filament` | string | "Loaded" or "Unloaded" |
| `printer.mmu.action` | string | "Idle", "Loading", "Unloading", "Forming Tip", etc. |
| `printer.mmu.gate_status` | int[] | Per-gate: -1=unknown, 0=empty, 1=available, 2=from_buffer |
| `printer.mmu.gate_color_rgb` | int[] | Per-gate RGB colors (0xRRGGBB) |
| `printer.mmu.gate_material` | string[] | Per-gate material names |
| `printer.mmu.filament_pos` | int | 0-8 filament position for path visualization |
| `printer.mmu.sync_feedback_bias_modelled` | float | -1 (tight) to +1 (loose); `AmsSystemInfo::sync_feedback_bias`, the one buffer reading Happy Hare gives (`sync_feedback_bias_raw` is the raw sensor value). `helix::buffer_reading()` draws it as the "Sync" buffer on every buffer surface |

Happy Hare has both a buffer reading (sync feedback, above) and clog detection: the
encoder (`clog_detection` 1 or 2) and FlowGuard (`flowguard.enabled`) feed the
`clog_meter_*` subjects. The two stay separate; see
[Filament buffer reading](FILAMENT_MANAGEMENT.md#filament-buffer-reading).

### G-code Commands

| Command | Action |
|---------|--------|
| `MMU_LOAD GATE={n}` | Load filament from gate |
| `MMU_UNLOAD` | Unload current filament |
| `MMU_SELECT GATE={n}` | Select gate without loading |
| `T{n}` | Tool change (unload + load) |
| `MMU_HOME` | Home the selector (reset). Unloads first unless already unloaded — see Reset vs Recover |
| `MMU_RECOVER [GATE=g \| BYPASS=1] [LOADED=0\|1]` | Re-sync HH's tracked state; only what the user asserted is named, and never `TOOL` - see Reset vs Recover |
| `MMU_PRELOAD GATE={n}` | Park a gate's filament ready for a later load (slot menu **Preload**). HH refuses it while printing, and so does `preload_lane()`. HH also refuses it while any filament is loaded, reporting only to its console, so the menu row is disabled then |
| `MMU_LOAD EXTRUDER_ONLY=1` / `MMU_UNLOAD EXTRUDER_ONLY=1` | Load / unload only the extruder (Maintenance **Load Extruder** / **Unload Extruder**). HH heats the nozzle itself but has no print check on either, so the backend refuses them while a print owns the toolhead |
| `MMU_MOTORS_ON` / `MMU_MOTORS_OFF` | Maintenance **Motors** toggle. Present in v3 and v4 |
| `MMU_SPOOLMAN REFRESH=1` | Accessories **Refresh Spoolman**. Disabled while `printer.mmu.spoolman_support` is `off`, which HH refuses outright |
| `MMU_CHECK_GATE` | Probe every gate sensor (sidebar **Check slots**). Physical: parks the toolhead and unloads/reloads each gate, and Happy Hare does not refuse it mid-print, so the sidebar gates it |
| `MMU_TTG_MAP TOOL={n} GATE={g}` | Set tool-to-gate mapping |
| `MMU_GATE_MAP GATE={n} [COLOR=..] [MATERIAL=..] [SPOOLID=..]` | Persist a slot edit to the gate map (`mmu_vars.cfg`). Omitted params keep their current value, so a field is only cleared by naming it with an explicit empty value. In Spoolman pull mode nothing is sent - see [Clear Spool](#clear-spool) |
| `MMU_GATE_MAP GATE={n} MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1` | **Clear Spool**: wipe every field the gate map holds for one gate - see [Clear Spool](#clear-spool) |
| `MMU_SELECT_BYPASS` | Select bypass position |

### Path Topology

`PathTopology::LINEAR` -- Selector picks one input from multiple gates. Filament path: `SPOOL -> PREP -> LANE -> HUB (selector) -> OUTPUT (bowden) -> TOOLHEAD -> NOZZLE`.

Happy Hare's `filament_pos` (0-8) maps to `PathSegment` via `path_segment_from_happy_hare_pos()`.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | `Available` | `Group` on a single-unit MMU; `ReadOnly` + `MultiUnit` on multi-unit, `ReadOnly` + `NotReady` before the gate registry initialises (see [Endless Spool](FILAMENT_MANAGEMENT.md#endless-spool-shared-model)) |
| Tool Mapping | Yes | Yes (via `MMU_TTG_MAP`) |
| Bypass Mode | Yes | Yes (selector position -2), when `[mmu_machine] has_bypass` is set (v4: any unit's `has_bypass`). `has_bypass: 0` hides the UI but `MMU_SELECT_BYPASS` still works - see [the force override](FILAMENT_MANAGEMENT.md#bypass-visibility-and-the-force-override) |
| Spoolman | Yes | -- (pull mode: the gate map is Spoolman's - Clear Spool clears HelixScreen's copy only and reports partial failure) |
| Auto-Heat on Load | No | UI manages preheat |
| Dryer | Yes | `MMU_HEATER` (see [Happy Hare Specifics](FILAMENT_MANAGEMENT.md#happy-hare-specifics)) |
| Lane Eject | Yes | `supports_lane_eject()` + `eject_lane()` |

Happy Hare's endless spool is group-based and settable at runtime, not a config-file
read: `apply_endless_spool_backup()` builds a full `GROUPS=` array (one non-negative group
id per gate) and sends `MMU_ENDLESS_SPOOL QUIET=1 GROUPS=<csv>`.
`get_endless_spool_capabilities()` reports `Group` editability only when
`system_info_.units.size() <= 1`: `MMU_ENDLESS_SPOOL` has no `UNIT=` parameter and acts on
the currently-selected unit, so a client cannot reliably target one unit's groups on a
multi-unit (EMU) rig. `get_endless_spool_config()` returns the gate group as one unordered
`EndlessSpoolGroup` per group id; flattening it to per-slot arrows is the renderer's job
(see [Endless Spool](FILAMENT_MANAGEMENT.md#endless-spool-shared-model)).

**`ENABLE=` on edit vs on reset.** An edit sends **no** `ENABLE=`, and
`apply_endless_spool_backup()` refuses with `WRONG_STATE` when
`mmu.endless_spool_enabled` is false: `cmd_MMU_ENDLESS_SPOOL` ignores `GROUPS` while the
feature is off, so the write would fail silently. An unconditional `ENABLE=1` is not the
fix - it turns the feature **on**, persistently via `mmu_state_enable_endless_spool`, as a
side effect of setting one backup gate. `reset_endless_spool()` does keep
`MMU_ENDLESS_SPOOL ENABLE=1 RESET=1 QUIET=1`, because the handler early-returns before
honouring `RESET` while disabled, and `_reset_endless_spool()` then assigns *and* persists
`default_endless_spool_enabled` over the momentary enable - so there it is not a lasting
side effect.

`endless_spool_enabled` is in the `mmu` subscription field list
(`src/api/moonraker_discovery_sequence.cpp`) alongside `endless_spool_groups`. Happy Hare
publishes the bit under two keys, `endless_spool_enabled` and `endless_spool`, both tagged
DEPRECATED in mmu.py's `get_status()` with no replacement shipped, so the parse reads the
newer spelling and falls back to the older one; if a future Happy Hare drops both, the flag
keeps its last value instead of silently flipping to off. It lands in
`AmsSystemInfo::endless_spool_enabled`, which is what `caps.enabled` is derived from. Before
the first frame the flag is still false, which is why the uninitialised-registry branch
reports `Unknown` + `NotReady` rather than `Off`.

`recovers_filament_on_resume()` is left at its `kTraits` default (`false`), so a Happy
Hare runout gets the dialog with manual **Load** kept prominent, because Resume alone does not
re-feed. `supports_per_tool_spool_assignment()` is not overridden either; it falls through
to `is_tool_changer(get_type())`, which is false for an MMU.

### Clear Spool

Clear Spool empties everything the printer remembers about the gate, not just the
spool link. Happy Hare keeps omitted `MMU_GATE_MAP` params at their current value, so
`apply_user_edit()` detects the funnel's all-blank `SlotInfo` (and a gate that held
something) and sends one wipe:

```
MMU_GATE_MAP GATE={n} MATERIAL= COLOR= NAME= VENDOR= SPOOLID=-1 QUIET=1
```

Every writable field is named with an explicit empty value; v2/v3 ignore params they do
not fetch, and `VENDOR` is v4-only. Three params are permanently off this command:

- `RESET=1` - on v2/v3 it ignores `GATE` and wipes **every** gate.
- `TEMP=0` - falsy values mean "keep the current value".
- `AVAILABLE=0` - that marks the gate EMPTY, not unknown.

Two gates on the send itself:

- **Spoolman pull mode** (`printer.mmu.spoolman_support == "pull"`, parsed into
  `AmsSystemInfo::spoolman_mode`): Happy Hare refuses local writes to material, colour,
  name, vendor and spool id, and logs the refusal in its own console rather than
  returning an error the client could read. The backend clears HelixScreen's layer,
  sends nothing, and returns a partial failure naming Spoolman as the owner of the gate
  map. An ordinary slot edit gets the same treatment in `apply_user_edit()`: the
  `MMU_GATE_MAP` send is skipped with the same partial failure (HelixScreen keeps its
  own copy), while a tool remap still goes out - `MMU_TTG_MAP` is not a gate-map field
  and Happy Hare takes it in pull mode.
- **Mid-print backstop.** The print UI refuses clears while a job holds the machine. If
  one reaches the backend anyway for the gate the job is printing from, the firmware
  write is skipped with a warning and a partial failure; HelixScreen's own layer is
  still cleared.

The wipe only fires when the gate held something. An all-blank edit on an already-blank
gate (a tool-map-only save) sends no `MMU_GATE_MAP` at all, so an ordinary edit on an
empty gate stays silent.

### Happy Hare 4

The connect-time query reads `configfile.settings` and the live `mmu_machine` object
together, and `happy_hare::read_machine_layout()` (`include/happy_hare_status_parse.h`)
decides which layout they describe. v4 is recognised by `happy_hare_version` on
`mmu_machine` (the live object, else configfile's `[mmu_machine]`); v3 never publishes it
there. The version lands in `AmsSystemInfo::version`, and its number picks the parameter names
`happy_hare::param_name()` hands out (`MachineLayout::version_number`). Every v3/v4 difference
below asks that one layout; a status frame carrying `tangle_prevention`, which only v4
publishes, sets its v4 flag before the query answers.

| What | v3 | v4 |
|------|----|----|
| Machine fields (selector type, heaters, env sensors) | live `mmu_machine.unit_N` on v3.4, else `configfile.settings.mmu_machine` | live `mmu_machine.unit_N` |
| Version (`AmsSystemInfo::version`) | `[mmu] happy_hare_version` (a number such as 3.42) | `mmu_machine.happy_hare_version` |
| Tunables (tip macro, speeds, toolhead distances, `heater_max_temp`, `sync_to_extruder`) | `[mmu]` | `[mmu_parameters]`, `[mmu_unit_parameters <unit>]`, `[mmu_toolhead <toolhead of that unit>]`; `happy_hare::find_config_param()` owns which |
| `MMU_TEST_CONFIG` gear speeds | `GEAR_FROM_SPOOL_SPEED`, `GEAR_FROM_BUFFER_SPEED`; `GEAR_UNLOAD_SPEED` from 3.10 | `GEAR_LOAD_SPEED`, `GEAR_FROM_FILAMENT_BUFFER_SPEED`, `GEAR_UNLOAD_SPEED` |
| Bypass support | `printer.mmu.has_bypass` | any `mmu_machine.unit_N.has_bypass` (`printer.mmu.has_bypass` is a constant true) |
| Per-gate pre-gate sensors | `printer.mmu.sensors.mmu_pre_gate_N` | `filament_switch_sensor mmu_entry_N` objects; `printer.mmu.sensors` covers only the selected gate |
| eSpooler | `espooler_active` | per-gate `espooler` list, the selected gate's entry shown |
| Calibrate gates | `MMU_CALIBRATE_GATES` | `MMU_CALIBRATE_GATE ALL=1` |
| Clog detection mode (0 off, 1 manual, 2 auto) | read from `clog_detection_enabled`; written as `ENABLE_CLOG_DETECTION` (+ `MMU_CALIBRATION_CLOG_LENGTH` in manual) before 3.42, `FLOWGUARD_ENCODER_MODE` (+ `FLOWGUARD_ENCODER_MAX_MOTION`) on 3.42 | read from `flowguard.encoder_mode` only (`clog_detection_enabled` is a constant false; `encoder.detection_mode` is runtime state); written as `flowguard_encoder_mode` (+ `flowguard_encoder_max_motion`) on a unit with an encoder |

**`UNIT=` on a multi-unit v4.** v4 refuses its per-unit commands without `UNIT=` once
`mmu_machine.num_units` is above 1. `unit_suffix_locked()` builds it, and only there:
`MMU_HOME` and `MMU_MOTORS_ON/OFF` take `UNIT=ALL`. `MMU_HEATER` takes the unit being dried;
a whole-machine start or stop sends one `MMU_HEATER` per unit with a heater, because
`UNIT=ALL` stops at the first unit without one. `MMU_SERVO`, `MMU_TEST_GRIP`,
`MMU_CALIBRATE_GATE ALL=1` and unit-scoped `MMU_TEST_CONFIG` parameters take the selected unit
(`printer.mmu.unit`), or, when it lacks the hardware, the first unit that has it. v3's
`MMU_TEST_CONFIG` rejects an unknown `UNIT`, so it is never sent there.

**Per-unit guards.** v4 applies the parameters a unit takes and answers the rest of an
`MMU_TEST_CONFIG` with an error, and v3 refuses the whole command over any name that is not
one of its own attributes, so `happy_hare::unit_supports()` answers, from `mmu_machine.unit_N` and
the unit's `[mmu_unit <name>] encoder`: `MMU_SERVO` needs a servo selector,
`selector_move_speed` a moving selector, the encoder mode and gate calibration an encoder,
`sync_to_extruder` a unit that does not keep its filament gripped, and
`gear_from_filament_buffer_speed` a filament buffer. The two toolhead distances tuned against a
sensor need it fitted and enabled (its key in `printer.mmu.sensors`, not null). v3 checks
only names, so there only `selector_move_speed` is filtered, by selector. A device action no
unit takes is disabled, and the override reapply sends one `MMU_TEST_CONFIG` per parameter on
every version, leaving out any no unit takes, so one refusal cannot sink the rest.

**Units.** `happy_hare::read_machine_units()` returns every `mmu_machine.unit_N` (v3.4 and v4
both publish them; an older v3 falls back to configfile's `[mmu_machine]` as one unit). Each
unit's `num_gates` sets the slot split in preference to `printer.mmu`'s counts or an even
split; when it arrives after the first `gate_status` frame, `initialize_slots()` re-splits
the registry with `SlotRegistry::reorganize()` and keeps every gate's state. Each unit gets
its own topology from its `selector_type` (a mixed ERCF + Box Turtle rig is LINEAR then HUB)
and its `display_name` (v4) as its name. Enclosure heaters and sensors stay one shared name
when every unit uses the same one, else become one entry per gate across all units
(`happy_hare::collect_unit_objects()`). `mmu_machine.unit_N.is_homed` is a boot-time snapshot
on v4 and is never read.

v4 sends `encoder`, `flowguard`, `tangle_prevention` and the `sync_feedback_*` fields as
JSON null while the selected unit lacks that hardware. A null `sync_feedback_bias_*` returns
the bias to the -2 "unavailable" sentinel, and a null `flowguard` or `encoder` clears
`flowguard_info.enabled` / `encoder_info.enabled`; other fields keep their last value. An
encoder-only unit publishes `flowguard` as `{active, enabled, encoder_mode}`, which is the
encoder's detection rather than buffer FlowGuard, so `flowguard_info.enabled` follows only a
`flowguard` carrying `level`, `trigger` or `max_clog`. Until the units are known, v4's constant
`has_bypass` shows no bypass, and a v4 `espooler` list is read per gate (v3's covers only the
gates fitted with one, so it is read per gate only when it covers every gate).

The clog config modal asks `AmsBackend::clog_detection_mode_gcode()` for its command and
pre-fills its length from `clog_detection_length_setting()`: the configured
`flowguard_encoder_max_motion` from 3.42 on, else the live encoder length. Save sends the
length only when the user moved it. `MMU_FLOWGUARD ENABLE=` switches buffer FlowGuard as a whole; no HelixScreen control
sends it. Golden payloads: `tests/fixtures/happy_hare_v4_*.json`.

### Reset vs Recover

- **Reset** (`reset()`) sends `MMU_HOME` to home the selector. Used for general state reset.
  It is not state-only: bare `MMU_HOME` carries no `FORCE_UNLOAD`, and Happy Hare's selector
  `home()` runs its unload sequence whenever `filament_pos` is not `UNLOADED` before homing.
  Happy Hare's own `check_if_printing()` guard is on `MMU_PRELOAD` alone, so the firmware
  accepts this mid-print. Happy Hare is therefore the one backend answering
  `reset_moves_filament()` true, which is what lets the AMS sidebar grey its Reset button
  while a job owns the machine (`include/filament_op_slot_resolver.h#compute_machine_op_gating`).
- **Recover** (`recover()`) sends `MMU_RECOVER` to attempt error recovery without full re-homing.
- **Recover with state** (`recover_with_state(request)`) is what the Recover buttons in Device
  Operations and the selector menu open: `AmsRecoverStateModal` asks for the true gate (or
  bypass), pre-filled from live state, and the filament position, which starts at "Detect
  automatically". `AmsBackendHappyHare::build_recover_command()` names only what the user
  set: "Keep current" omits `GATE`, `BYPASS=1` replaces it (HH forces tool and gate to
  bypass), and "Detect automatically" omits `LOADED` so HH reads its sensors. It never names
  `TOOL`: HH's tool branch remaps the tool onto the gate and rewrites the gate's status, and
  with `LOADED=0` wipes that gate's material, colour and spool id. Tool remapping is the tool
  map's job. The error popup's Recover action is bare `MMU_RECOVER`: our loaded flag reads
  false for every position HH reports as Unknown, so asserting it would tell HH "unloaded"
  about filament stuck mid-bowden.
- **Clear fault** (`clear_fault(slot_index)`) is a third, gate-scoped door onto the same command.
  Happy Hare overrides the base default (which forwards to `cancel()`): `slot_index >= 0` sends
  `MMU_RECOVER GATE=<n>`, and `slot_index < 0` (what both UI callers pass whenever nothing is
  loaded, which is the state Reset is pressed in) drops the parameter and sends bare
  `MMU_RECOVER`, re-syncing the whole selector.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
