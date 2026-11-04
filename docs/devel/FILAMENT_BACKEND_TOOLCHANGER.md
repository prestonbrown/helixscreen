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
| `tool_numbers` | int[] | All tool numbers, ascending [0, 1, 2] |
| `tool_names` | string[] | Each tool's config section, in `tool_numbers` order ["tool T0", "tool T1", "tool T2"]. Discovery orders the slots by it (below) |

**Tool order.** A slot's index is its Klipper tool number: slot `n` is labelled "T`n`" and selected by
the name of its `[tool ...]` object, `SELECT_TOOL TOOL="tool <name>"`. The objects list names the tools but not their numbers, so discovery
sorts them naturally (T2 before T10) and then, once the subscription's first status
arrives, puts them in `toolchanger.tool_names` order
(`PrinterDiscovery::order_tools_by_number`). `[tool Right]` numbered 0 therefore takes
slot 0 ahead of `[tool Left]` numbered 1. Tools with no `tool_number` follow the numbered
ones. Numbers with gaps (0, 1, 5) still map slot by position: slot 2 mounts the third tool,
but is labelled `T2`.

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
| `SELECT_TOOL TOOL="tool <name>"` | Mount the slot's physical tool (what a slot tap sends). `TOOL=` takes the full object name, quoted for its space. `SELECT_TOOL T=<n>` would look `n` up in the table `ASSIGN_TOOL` writes, so a remap left from a mapped print would mount another hot end |
| `ASSIGN_TOOL TOOL="tool <name>" N=<n>` | Make G-code `T<n>` mount that physical tool (a remap). Keyed on the same full object name |
| `UNSELECT_TOOL` | Park the current tool (`park_active_tool()`) |
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
| Tool Mapping | Yes | With klipper-toolchanger: `RemapStrategy::Native` - `set_tool_mapping()` emits `ASSIGN_TOOL TOOL="tool T{n}" N={tool}`, so a G-code T-number can point at any physical tool. `parse_toolchanger_state()` resolves `tool_number` back through the forward map. Without it (`tool_commands_.present`, the machine swaps with its own `T<n>` commands): `RemapStrategy::GcodeRewrite` - the job file is rewritten before printing, which needs the HelixPrint plugin. See [the remapper](FILAMENT_MANAGEMENT.md#gcode-tool-remapper) for what that rewrite does and does not move |
| Bypass Mode | No | Not applicable - each tool is its own path. [The force override](FILAMENT_MANAGEMENT.md#bypass-visibility-and-the-force-override) shows the external spool for tracking only |
| Spoolman | Fields only | `spoolman_id`/`spoolman_vendor_id` persist in slot overrides; no toolchanger-specific Spoolman wiring exists in the backend |
| Slot metadata | Yes | Persisted via `FilamentSlotOverrideStore` (`lane_data`, `T<n>` keys). The firmware supplies none of it, so the store is the sole source - see [FILAMENT_SLOT_METADATA.md](FILAMENT_SLOT_METADATA.md#tool-changer-is-the-odd-one-out) |
| Auto-Heat on Load | No | -- |
| Dryer | No | -- |
| Device Actions | Machine-dependent | None by default. A tool changer with an add-on feeder exposes `open_feeder`/`close_feeder`, and one with a nozzle-clean command `clean_nozzle` - see [FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md) |
| Operation step bar | Machine-dependent | Suppressed unless the machine reports phases. A plain klipper-toolchanger has only `toolchanger.status == "changing"`, which is not a sequence, so it renders no step bar rather than the legacy Heat/Feed/Purge one - nothing heats, feeds or purges when the whole hot end is swapped. A changer with an add-on phase source gets a real bar; see [FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md) § "The operation step bar" |

### Discovery Sequence

Tool names must be provided via `set_discovered_tools()` before calling `start()`. `set_discovery()` does that from `PrinterDiscovery::tool_names()` when `AmsState::init_backends_from_hardware()` hands it the discovery snapshot.

---

Hotend changers (MedusaHC) run on this backend with add-ons layered on top - see
[FILAMENT_BACKEND_MEDUSAHC.md](FILAMENT_BACKEND_MEDUSAHC.md). A MedusaHC swaps with its own
`T<n>` / `DROP_TOOL` and maps tools by rewriting the file even where klipper-toolchanger
runs beside its controller: klipper-toolchanger only mirrors the docks' tool there.

## Z-Mod on the Creator 5 Pro

A Creator 5 Pro on Z-Mod firmware ([ghzserg/z_c5pro](https://github.com/ghzserg/z_c5pro))
is the same backend speaking a different dialect: FlashForge's own Klipper fork, no
klipper-toolchanger, and Z-Mod's `zmod_color` extra driving the changer. From the UI it is
a 4-tool changer like any other; the row in `src/printer/toolchanger_addon.cpp#providers`
is what supplies everything below.

### Detection

`zmod_color` **and** `gcode_button extruder_grab1` in `printer.objects.list`
(`src/printer/toolchanger_addon.cpp#zmod_c5_detect`, exported as `zmod_c5_present`). Both
halves are required: the AD5X Z-Mod also publishes `zmod_color` but has no carriage grab
buttons, so it cannot match, and `zmod_color` alone would not say which machine this is.
Reforge, the other C5 Pro firmware, needs none of this: it matches no provider row and
takes the plain klipper-toolchanger path described above.

Z-Mod 1.7.4 additionally publishes an **AFC shim** (`zmod_afclite` registering `AFC`,
`AFC_lane E0..E3`, `AFC_unit C5`) so Fluidd and Mainsail draw their AFC panel for the C5's
heads. Discovery keys on the bare `AFC` object, so `PrinterDiscovery::parse_objects`
yields that claim to this row whenever the shim's own object and the changer fingerprint
coexist — the same carve-out shape as the PAXX AFC-Lite rule, and for the same reason:
the shim reports no extruders and no hubs, so an AFC backend would infer a hub topology
for heads that are each their own path, and its op macros (`AFC_RESET`,
`AFC_LANE_RESET`, `SET_SPOOL_ID`, ...) do not exist on this firmware.

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

### Swap phases from console markers

Z-Mod publishes no phase word on a status object; its macros narrate the swap through
respond_info lines instead. The backend subscribes `notify_gcode_response` for this row
alone and classifies the lines (`toolchanger_addon::parse_zmod_marker`, marker text
field-verified in bundle `5WSPTZBL`):

| Marker | Phase word |
|--------|------------|
| `Grab attempt for T<n>: i of N...` | `picking` |
| `X/Y/Z-Offset: ... _T_IN` | `picking` |
| `Unload attempt for T<n>: i of N...` | `dropping` |
| `X/Y/Z-Offset: ... _T_OUT` | `dropping` |

Each maps onto the same picking/dropping vocabulary the MedusaHC controllers publish, so
the action, the direction latch and the step bar ride the existing machinery: a mount is
[Pick up tool], an unmount is [Dock tool], a change is [Dock tool, Pick up tool]. The
direction capability is latched from the provider row itself (`set_tool_sensor`), not
from a first marker, because the step bar snapshots the latch when it is built and a bar
built at dispatch time — before the macro's first marker — would otherwise pin a
suppressed sequence forever. A firmware build that stops echoing the markers degrades to
no step bar, never a wrong one; the marker text on a live swap is the one piece still
pending hardware verification (prestonbrown/helixscreen#1714).

### The afclite shim's status, and what is acted on

Z-Mod 1.7.4+ publishes `zmod_afclite` (aliased `AFC`), and the provider row subscribes
it. Of its fields, exactly two are acted on, both with semantics a boolean or the
firmware's own string fixes:

| Field | Treatment |
|-------|-----------|
| `error_state` + `message` | A fault the firmware itself reports: raises the ERROR action and surfaces through `current_error()` with the firmware's own wording; clears when the flag clears |
| `current_state` | **Logged, not acted on.** The vocabulary beyond "idle" is unverified on this firmware; the log line is what a field capture during a real load/unload reads, and the mapping that follows is written against that evidence |
| `lanes[].map` (via the `AFC_lane` objects) | Logged when it is not identity — a `SET_MAP` remap is exactly what a bundle should show. Nothing acts on it until a capture says what lane→tool means for head indices |

### T_INFO: the read-only carriage query

`T_INFO` answers where every tool sits and moves nothing (zmod's own doc). The backend
sends it once at start and again on `notify_klippy_ready`, and parses its rows through
the same console listener as the swap markers: `T<n>: HEAD` names the carriage tool,
`T<n>: home` seats a dock, `ERROR (both)` is the disagreeing-sensors fault, and the
trailing `Offset:` row closes the block — the only place "nothing is on the carriage"
can be stated, which is what clears the stale boot reading on firmware whose
`active_tool_id` only recomputes inside its own commands (the pre-1.7.4 condition).

### Per-head filament presence (Z-Mod 1.7.4+)

`zmod_color.slots[].hasFilament` says whether each head physically holds filament. It
stamps the slot EMPTY only in the docked arm — a docked head whose slot says no filament
reads Empty — and deliberately cannot outrank the carriage (a mounted head that ran dry
is the runout flow's question) or a dock-reported vacancy (the head itself is out of the
machine). `slot_status_tracks_filament()` stays false: the per-head `fd_ex` sensors keep
driving pre-print checks as before.

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

### Filament feed and unload

Z-Mod's own Load for a head is `CHANGE_TOOL LANE=E<n>`: with the afclite shim present
(Z-Mod 1.7.4+, `zmod_afclite`) that command heats only the selected head and feeds it,
verified on a Creator 5 Pro by a contributor (prestonbrown/helixscreen#1779). The Z-Mod
row builds exactly that command through `toolchanger_addon::resolve_tool_feed()`, and
`AmsBackendToolChanger::do_load_filament()` sends it. The shim runs `_T_IN_ZCOLOR` itself;
HelixScreen never calls `_T_IN_ZCOLOR` directly, because its `SLOT` base and `NAPR`
polarity are not confirmed for the C5 (the plugin is not published, and the AD5X source
reads the opposite way).

The head-to-lane relation is the **lane name**: head `n` is the shim's `AFC_lane E<n>`.
Discovery does not read any field that ties a lane to a head (the `lanes[].map` word is
logged only), so the name is the whole basis. A lane the shim did not publish yields no
command and the Load is refused; a lane name is never constructed for an object the
printer lacks.

Without the shim (Z-Mod older than 1.7.4, or no `zmod_afclite` object) there is no feed:
`head_filament_ops()` answers Load Manual, and Load refuses with that reason and sends
nothing.

With the feed present the backend answers capability questions the generic code asks:
`head_filament_ops()` (Load Automatic for every head the shim published a lane for),
`load_feeds_filament()` (Load is a feed, so a head that reads loaded still accepts it and
the planner does not swap another in first) and `supports_auto_heat_on_load()` (the shim
heats the right head, so the UI does not preheat, which would heat the active head).
Mounting a tool is never a filament op: it is the toolhead tap and the sidebar's Mount,
which send `_T_IN T=<n>` like every other tool change. Busy and printing refusals are
unchanged.

There is **no automatic unload** on the Creator 5: the user cuts the filament at the head
and pulls it out. The row answers `AmsBackend::unload_is_manual()` true, so
`head_filament_ops()` answers Unload Manual with that reason, and every UI control that
means "take the filament out" disables or, through `notify_if_filament_op_unavailable()`,
shows one info toast and sends nothing, heating included. **Park** (`park_active_tool()`)
is a tool operation: it still sends `_T_OUT`.

### Mock

`HELIX_MOCK_PRINTER=creator5_zmod` exercises this path against mock hardware: the persona
publishes Z-Mod's objects and status (1.7.4 shape, afclite shim included), answers
`_T_IN` / `_T_OUT` by narrating the swap markers, and implies `--real-ams`, so real
discovery builds this backend under `--test` (see
`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`). Hardware verification on a real Z-Mod C5 is
tracked as prestonbrown/helixscreen#1714 follow-up work.

### Endless spool

Z-Mod's C5 firmware switches to the next head whose colour AND type both match on runout
(its docs call this "endless spool mode"), always on with no flag to write. The backend
reports it exactly as the AD5X IFS backend reports stock zMod: Available /
FirmwareManaged / ReadOnly, provider `"zmod"` (`get_endless_spool_capabilities`, gated on
this provider row). Every other tool changer keeps the base default (Unsupported).

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
| Timeout | the rpc ceiling is the panel's own `kCalibrationTimeoutMs` (15 min; ~30 s to measure plus heating from cold, per tool, sequentially). Moonraker never times out `printer.gcode.script`, so an expiry while `idle_timeout` still reads Printing is a macro that is still running: the run completes on the busy→idle edge, with one more ceiling as the backstop - `PrintPreparationManager`'s rule for a pre-start macro that outlives its ceiling |
| Mock | `HELIX_MOCK_AMS=toolchanger` advertises the macro and simulates the run (see `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`) |

### Which procedure the entry runs

`tool_offset_calibration::resolve_procedure(hw)` is the one question
(`include/tool_offset_calibration.h`). It walks a table in
`src/printer/tool_offset_calibration.cpp`, first match wins, and returns
`Procedure { kind, name, gcode, flow_id, entry_description }`:

| Row | Detects | Kind | Runs |
|-----|---------|------|------|
| Snapmaker nozzle offset (builds with `HELIX_HAS_SNAPMAKER` only) | `snapmaker::nozzle_offset::supported` | `GuidedFlow` | the U1's guided flow |
| INDX seat by hand | `indx::supports_seat_by_hand` | `GuidedFlow` | the seat-by-hand flow |
| klipper-toolchanger | `has_tool_changer()` + `CALIBRATE_TOOL_OFFSETS` | `Panel` | the panel above |
| Changer firmware prompt | a tool changer in any dialect (`has_tool_changer()` or `toolchanger_addon::present()`) + `CALIBRATE_EXTRUDERS` | `PrinterPrompt` | the macro alone |

`supported(hw)` is `kind != None`; `printer_has_tool_offset_cal` and the Advanced row's
description come from the same struct. `open_tool_offset_cal_panel()`
(`src/ui/calibration/ui_panel_calibration_tool_offset.cpp#open_tool_offset_cal_panel`) switches on the
kind: a guided flow starts, the panel shows, and a `PrinterPrompt` goes through
`start_calibration_gcode()` (`src/printer/macro_executor.cpp#start_calibration_gcode`), which
refuses while `machine_motion_blocked` is set and reports failure the way every macro send
does.

**The printer prompt.** Z-Mod on the Creator 5 ships `CALIBRATE_EXTRUDERS`, which only opens
an action:prompt asking the user to remove the build plate; the prompt's OK runs
`_T_CALIBRATE_EXTRUDERS` for all four heads. Its rpc answers before anything moves, so the
panel cannot follow it: its completion and result rows would claim a finish that has not
happened. HelixScreen sends the macro, `ActionPromptManager` renders the printer's prompt,
and the firmware keeps its own measurements; no progress or rows are shown.

Tests: `[tool_offset_cal]` in `tests/unit/test_tool_offset_calibration.cpp`, including one
case that walks every row in precedence order. Adding a firmware is one row in that table.

