# Tool Changer Filament Backend

viesturz/klipper-toolchanger swaps complete toolheads on the carriage - each "slot" is a
toolhead with its own extruder, not a filament lane. Topology is
`PathTopology::PARALLEL` (each tool is its own path; no hub or selector); the slot count
is the number of discovered `tool T*` Klipper objects.

## Tool Changer (viesturz/klipper-toolchanger)

Physical tool changers have multiple complete toolheads that are swapped on the carriage, fundamentally different from filament-switching systems.

### Detection

Klipper object `toolchanger` in `printer.objects.list` sets `AmsType::TOOL_CHANGER`. Individual tool names come from `tool T*` objects (e.g., `tool T0`, `tool T1`).

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
| `gcode_x_offset` | float | X offset |
| `gcode_y_offset` | float | Y offset |
| `gcode_z_offset` | float | Z offset |
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
