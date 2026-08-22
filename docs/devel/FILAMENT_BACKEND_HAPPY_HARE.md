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

### G-code Commands

| Command | Action |
|---------|--------|
| `MMU_LOAD GATE={n}` | Load filament from gate |
| `MMU_UNLOAD` | Unload current filament |
| `MMU_SELECT GATE={n}` | Select gate without loading |
| `T{n}` | Tool change (unload + load) |
| `MMU_HOME` | Home the selector (reset) |
| `MMU_RECOVER` | Attempt error recovery |
| `MMU_TTG_MAP TOOL={n} GATE={g}` | Set tool-to-gate mapping |
| `MMU_SELECT_BYPASS` | Select bypass position |

### Path Topology

`PathTopology::LINEAR` -- Selector picks one input from multiple gates. Filament path: `SPOOL -> PREP -> LANE -> HUB (selector) -> OUTPUT (bowden) -> TOOLHEAD -> NOZZLE`.

Happy Hare's `filament_pos` (0-8) maps to `PathSegment` via `path_segment_from_happy_hare_pos()`.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | `Available` | `Group` on a single-unit MMU; `ReadOnly` + `MultiUnit` on multi-unit, `ReadOnly` + `NotReady` before the gate registry initialises (see [Endless Spool](FILAMENT_MANAGEMENT.md#endless-spool-shared-model)) |
| Tool Mapping | Yes | Yes (via `MMU_TTG_MAP`) |
| Bypass Mode | Yes | Yes (selector position -2), when `[mmu_machine] has_bypass` is set. `has_bypass: 0` hides the UI but `MMU_SELECT_BYPASS` still works - see [the force override](FILAMENT_MANAGEMENT.md#bypass-visibility-and-the-force-override) |
| Spoolman | Yes | -- |
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

`recovers_filament_on_resume()` is **not** overridden here (default `false`), so a Happy
Hare runout gets the dialog with manual **Load** kept prominent, because Resume alone does not
re-feed. `supports_per_tool_spool_assignment()` is not overridden either; it falls through
to `is_tool_changer(get_type())`, which is false for an MMU.

### Reset vs Recover

- **Reset** (`reset()`) sends `MMU_HOME` to home the selector. Used for general state reset.
- **Recover** (`recover()`) sends `MMU_RECOVER` to attempt error recovery without full re-homing.
- **Clear fault** (`clear_fault(slot_index)`) is a third, gate-scoped door onto the same command.
  Happy Hare overrides the base default (which forwards to `cancel()`): `slot_index >= 0` sends
  `MMU_RECOVER GATE=<n>`, and `slot_index < 0` (what both UI callers pass whenever nothing is
  loaded, which is the state Reset is pressed in) drops the parameter and sends bare
  `MMU_RECOVER`, re-syncing the whole selector.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
