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
| `status` | string | "ready", "changing", "error", "uninitialized" |
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

### Path Topology

`PathTopology::PARALLEL` -- Each slot has its own independent path to a separate toolhead. No converging path visualization needed.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | `Unsupported` | No override; inherits the base default |
| Tool Mapping | No | Fixed (tools ARE slots) |
| Bypass Mode | No | Not applicable - each tool is its own path. [The force override](FILAMENT_MANAGEMENT.md#bypass-visibility-and-the-force-override) shows the external spool for tracking only |
| Spoolman | No | -- |
| Auto-Heat on Load | No | -- |
| Dryer | No | -- |
| Device Actions | No | -- |

### Discovery Sequence

Tool names must be provided via `set_discovered_tools()` before calling `start()`. The caller (typically `AmsState::init_backend_from_hardware()`) extracts tool names from `PrinterDiscovery::get_tool_names()`.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
