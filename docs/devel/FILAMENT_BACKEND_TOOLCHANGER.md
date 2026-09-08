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

