# Tool Changer Filament Backend

viesturz/klipper-toolchanger swaps complete toolheads on the carriage - each "slot" is a
toolhead with its own extruder, not a filament lane. Topology is
`PathTopology::PARALLEL` (each tool is its own path; no hub or selector); the slot count
is the number of discovered `tool T*` Klipper objects.

## Tool Changer (viesturz/klipper-toolchanger)

Physical tool changers have multiple complete toolheads that are swapped on the carriage, fundamentally different from filament-switching systems.

### Detection

Tool counts come from three sources, in descending authority
(`include/printer_discovery.h#PrinterDiscovery/parse_objects`):

1. **`tool T*` objects** - created by klipper-toolchanger, and authoritative: a
   klipper-toolchanger name is arbitrary and `ASSIGN_TOOL` can remap it, so real objects
   are never overwritten by anything below.
2. **The changer extra's own count** - for a changer running *without*
   klipper-toolchanger. Only MedusaHC exposes one today
   (`docs/devel/FILAMENT_BACKEND_MEDUSAHC.md`).
3. **Extruder heaters** - the fallback for a machine presenting one heater and one
   extruder motor per tool. Names become the G-code tool numbers `T0`, `T1`, ... Counted
   through `helix::is_extruder_name()`, so `extruder_stepper` sections do not inflate it
   and a Chimera/cyclops mixing hotend stays one tool.

`AmsType::TOOL_CHANGER` is registered whenever the printer has tools and either the
`toolchanger` object or more than one of them - a multi-extruder machine with no filament
system is a parallel-topology multi-tool printer whether or not anything calls itself a
changer, and registering it is what gives it slots, per-tool spool identity and the
filament panel's tool selector (prestonbrown/helixscreen#1350). A real MMU always wins:
the tool-changer arm is last in the detection chain.

Swap commands follow from the same fact. `SELECT_TOOL`/`UNSELECT_TOOL` exist only where
klipper-toolchanger does; without it the swap is plain `T<n>`, which Klipper maps to
`ACTIVATE_EXTRUDER` and a changer extra overrides with its own. `toolchanger_addon::
resolve_tool_commands()` answers this, and the provider table only has to name what
differs - the unmount, which a machine whose tools are plain extruders does not have.

### Key Differences from Filament Systems

- Each "slot" is a complete toolhead with its own extruder
- No hub/selector -- path topology is `PARALLEL`
- "Loading" means mounting the tool to the carriage
- No bypass mode (each tool IS the path)
- Tool mapping is fixed (tools ARE slots)

### Klipper Objects

**Global** (`toolchanger`):

| Variable | Type | Description |
|----------|------|-------------|
| `status` | string | "ready", "changing", "error", "uninitialized", "initializing" |
| `tool` | string | Current tool name ("T0") or null |
| `tool_number` | int | Current tool number (-1 if none) |
| `tool_numbers` | int[] | All tool numbers [0, 1, 2] |
| `tool_names` | string[] | All tool names ["T0", "T1", "T2"] |

**Per-tool** (`tool T{n}`):

| Variable | Type | Description |
|----------|------|-------------|
| `active` | bool | Is this tool selected? |
| `mounted` | bool | Is this tool mounted on carriage? |
| `gcode_x_offset` | float | X offset — settable at runtime with `SET_TOOL_PARAMETER T=<n> PARAMETER=gcode_x_offset VALUE=<mm>`, persisted with `SAVE_TOOL_PARAMETER T=<n> PARAMETER=gcode_x_offset` + `SAVE_CONFIG` (see `include/tool_offsets.h`) |
| `gcode_y_offset` | float | Y offset — same, `PARAMETER=gcode_y_offset` |
| `gcode_z_offset` | float | Z offset — same, `PARAMETER=gcode_z_offset` |
| `extruder` | string | Associated extruder name |
| `fan` | string | Associated fan name |

### G-code Commands

| Command | Action |
|---------|--------|
| `SELECT_TOOL TOOL=T{n}` | Mount specified tool |
| `UNSELECT_TOOL` | Unmount current tool (park it) |
| `T{n}` | Tool change macro |

### Initialization states

`initialize_on` (klipper-toolchanger config) decides what a cold boot does, and the two
non-settled states are easy to get wrong:

- `uninitialized` -> `AmsAction::RESETTING`, which `is_busy()`, so the precondition gate
  refuses a tool tap. That refusal is imperfect on the default `initialize_on: first-use`,
  where `select_tool()` would have auto-initialized and the tap is what would have cleared
  the state. It is kept anyway because letting the tap through is worse: on
  `initialize_on: manual` (what MedusaHC ships) Klipper raises "Cannot select tool,
  toolchanger status is uninitialized", that rejection reaches `execute_gcode()`'s error
  callback which only logs, `on_complete` never fires, and `execute_gcode()` returns
  success so the `if (!result)` net misses it too. The optimistic `SELECTING` would latch
  forever, and Moonraker only republishes CHANGED fields so no second `uninitialized`
  frame arrives to reset it. Refusing is recoverable (Reset sends
  `INITIALIZE_TOOLCHANGER`); a latched `SELECTING` is not. Fixing it properly means
  unwinding the dispatch from the gcode error callback, which is shared with AFC / Happy
  Hare / CFS and wants its own change.
- `initializing` -> `AmsAction::RESETTING`. It homes and moves the carriage, so it is
  genuinely busy. This was unmapped until recently and fell through to `IDLE`, so a tap
  could land mid-initialization.

### Path Topology

`PathTopology::PARALLEL` -- Each slot has its own independent path to a separate toolhead. No converging path visualization needed.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | `Unsupported` | No override; inherits the base default |
| Tool Mapping | Yes | `RemapStrategy::Native` - `set_tool_mapping()` emits `ASSIGN_TOOL TOOL=T{n} N={tool}`, so a G-code T-number can point at any physical tool. `parse_toolchanger_state()` resolves `tool_number` back through the forward map |
| Bypass Mode | No | Not applicable - each tool is its own path. [The force override](FILAMENT_MANAGEMENT.md#bypass-visibility-and-the-force-override) shows the external spool for tracking only |
| Spoolman | Fields only | `spoolman_id`/`spoolman_vendor_id` persist in slot overrides; no toolchanger-specific Spoolman wiring exists in the backend |
| Slot metadata | Yes | Persisted via `FilamentSlotOverrideStore` (`lane_data`, `T<n>` keys). The firmware supplies none of it, so the store is the sole source - see [FILAMENT_SLOT_METADATA.md](FILAMENT_SLOT_METADATA.md#tool-changer-is-the-odd-one-out) |
| Auto-Heat on Load | No | -- |
| Dryer | No | -- |
| Device Actions | Machine-dependent | None by default. A tool changer with an add-on feeder exposes `open_feeder`/`close_feeder` - see [FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md) |
| Operation step bar | Machine-dependent | Suppressed unless the machine reports phases. A plain klipper-toolchanger has only `toolchanger.status == "changing"`, which is not a sequence, so it renders no step bar rather than the legacy Heat/Feed/Purge one - nothing heats, feeds or purges when the whole hot end is swapped. A changer with an add-on phase source gets a real bar; see [FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md) § "The operation step bar" |

### Discovery Sequence

Tool names must be provided via `set_discovered_tools()` before calling `start()`. The caller (typically `AmsState::init_backend_from_hardware()`) extracts tool names from `PrinterDiscovery::get_tool_names()`.

---

Hotend changers (MedusaHC) run on this backend with add-ons layered on top - see
[FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md).

## Z-Mod on the Creator 5 Pro

A Creator 5 Pro on Z-Mod firmware ([ghzserg/z_c5pro](https://github.com/ghzserg/z_c5pro))
is the same backend speaking a different dialect: FlashForge's own Klipper fork, no
klipper-toolchanger, and Z-Mod's `zmod_color` extra driving the changer. From the UI it is
a 4-tool changer like any other; the row in `src/printer/toolchanger_addon.cpp#providers`
is what supplies everything below.

### Detection

`zmod_color` **and** `gcode_button extruder_grab1` in `printer.objects.list`
(`src/printer/toolchanger_addon.cpp#zmod_c5_detect`). Both halves are required: the AD5X
Z-Mod also publishes `zmod_color` but has no carriage grab buttons, so it cannot match,
and `zmod_color` alone would not say which machine this is. Reforge, the other C5 Pro
firmware, needs none of this: it matches no provider row and takes the plain
klipper-toolchanger path described above.

### Tool reading

`zmod_color.active_tool_id` maps one-to-one onto `ToolReading::current_tool`
(`include/toolchanger_addon.h#ToolReading`): 0..3 mounted, -1 nothing on the carriage, -2
dock and carriage buttons disagree. A frame without the field is no news, never a clear
(Moonraker republishes only what changed). The -2 rules are the shared ones: the last
known tool is held, the unit shows a sensor error, and the fault is withdrawn by the next
agreeing frame (`src/printer/ams_backend_toolchanger.cpp#apply_tool_sensor_locked`; a -2
mid-swap is transitional, per `sensor_error_is_fault()`).

Z-Mod only recomputes `active_tool_id` inside its own commands. On a release without
[ghzserg/z_c5pro#1](https://github.com/ghzserg/z_c5pro/pull/1) the field reads a stale -2
after every restart, so the unit boots showing a dock sensor error until the first
`_T_IN` / `_T_OUT` / `GET_ZCOLOR` runs. Accepted as-is: the fix is upstream (the PR
computes the field live from the dock and carriage buttons), not a second copy of Z-Mod's
button rule here.

### Commands

| Action | G-code |
|--------|--------|
| Mount | `_T_IN T=<n>` (0-based tool number; the provider's `select_prefix`) |
| Unmount | `_T_OUT` |

Completion is the gcode ack plus the next `active_tool_id`, as for MedusaHC. A refused
`_T_IN` / `_T_OUT` (Z-Mod raises `gcmd.error`) surfaces like a failed `SELECT_TOOL`; tool
state does not move because `active_tool_id` does not.

### Firmware material source

Z-Mod stores each head's material and colour in firmwareRes/config/filament.json on the
printer and exports them through `zmod_color.get_status()`. `read_materials()`
(`src/printer/toolchanger_addon.cpp#read_materials`, parsing in
`include/zmod_color_status.h`) turns a frame into per-slot material/hex readings plus
`valid_types` and `palette`; `apply_material_reading_locked()` files each as an
`ObservationSource::VendorCache` observation through `helix::ams::ingest()`. A Spoolman
link still outranks it.

Once a frame has carried both `slots` and `palette`, the firmware is the only store for
colour and material (`src/printer/ams_backend_toolchanger.cpp#firmware_stores_color_and_material`).
A user edit sends the writer's gcode and files **no** colour or material declaration;
brand, spool name, Spoolman link and weights are declared as usual. The screen updates
from the firmware's echo one status frame later, so a rejected write never shows. Before
the first such frame (a Z-Mod build without ghzserg/z_c5pro#1) the hook answers "none" and
edits stay local, as on any tool changer.

- **Types:** `zmod_color.valid_types` becomes `get_supported_materials()`, so the edit
  dropdown offers only those. `AmsBackend::normalize_material()` maps anything else by
  compat group before sending, and a type the firmware lists but that is unsafe on a
  gcode line (`IMoonrakerAPI::is_safe_material_param()`) is refused with an error and
  never sent.
- **Colours:** filament.json stores a colour as an index into Z-Mod's 24-entry
  `COLOR_MAPPING`; a hex outside it is saved as index 0, white. The writer snaps the
  picked colour to the nearest palette entry (squared RGB distance,
  `include/color_utils.h#nearest_palette_key`, the same rule QIDI Box uses) before
  sending.
- **SILENT=1:** `CHANGE_ZCOLOR` with `HEX` and `TYPE` runs `GET_ZCOLOR` on the same
  command, which opens a Mainsail/Fluidd prompt unless `SILENT=1` is set. The command
  needs both `HEX` and `TYPE` to write without a prompt, so a colour-only edit sends the
  slot's current type and a material-only edit sends its current (snapped) colour.

### Mock

`HELIX_MOCK_PRINTER=creator5_zmod` exercises this path against mock hardware: the persona
publishes Z-Mod's objects and status and implies `--real-ams`, so real discovery builds
this backend under `--test` (see `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`). Hardware
verification on a real Z-Mod C5 is tracked as prestonbrown/helixscreen#1714 follow-up
work.

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.

## Automatic tool offset calibration

klipper-toolchanger's example config (`examples/calibrate-offsets.cfg`) defines a
`CALIBRATE_TOOL_OFFSETS` macro that measures every tool on a nozzle-contact sensor and writes
the result with `SET_TOOL_PARAMETER` + `SAVE_TOOL_PARAMETER` on `gcode_x/y/z_offset`. HelixScreen
runs that macro and follows it; it does not drive the individual `TOOL_LOCATE_SENSOR` /
`TOOL_CALIBRATE_TOOL_OFFSET` steps itself, and it makes no assumption about which tool the
macro measures against, how it heats, or where the sensor is — all of that is the macro's.

| Piece | Where |
|-------|-------|
| Capability, gcode | `include/tool_offset_calibration.h` (`helix::tool_offset_calibration`) — capability is `has_tool_changer()` + the macro; the only gcode is the bare macro |
| Capability subject for XML | `printer_has_tool_offset_cal` (`PrinterCapabilitiesState`) |
| Screen | `include/ui_panel_calibration_tool_offset.h` — Controls ▸ Tool Offsets, and the Advanced row |
| Results | never parsed off the console: the macro's `SET_TOOL_PARAMETER` writes land on the `tool T<n>` objects and reach `ToolState` through `helix::tool_offsets`, exactly as a manual adjustment would |
| Progress | the rpc's completion only: `printer.gcode.script` answers when the macro finishes. The rows fill in as the macro writes, since `ToolState` publishes every offset change, but no row says which tool is under the probe: `toolchanger.tool_number` says which tool is mounted, and the tool the macro measures the others against is mounted without being probed, so mounted and probed differ. Per-tool progress needs the macro to report it |
| Save | the macro persists nothing; `SAVE_TOOL_PARAMETER` only stages. The panel's Save is the shared `save_dirty_offsets()` path (`src/ui/z_offset_utils.cpp`): re-send SET+SAVE for every dirty axis, then one `SAVE_CONFIG` through `SaveConfigWatch` |
| Stop | the macro blocks Klipper's gcode queue, so Stop is M112 + `FIRMWARE_RESTART` with the disconnect suppressed as expected; the restart discards the offsets measured so far |
| Timeout | the rpc ceiling is the panel's own `CALIBRATION_TIMEOUT_MS` (15 min; ~30 s to measure plus heating from cold, per tool, sequentially). Moonraker never times out `printer.gcode.script`, so an expiry while `idle_timeout` still reads Printing is a macro that is still running: the run completes on the busy→idle edge, with one more ceiling as the backstop - `PrintPreparationManager`'s rule for a pre-start macro that outlives its ceiling |
| Mock | `HELIX_MOCK_AMS=toolchanger` advertises the macro and simulates the run (see `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`) |

