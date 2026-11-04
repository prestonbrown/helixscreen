# Bondtech INDX Filament Backend

Bondtech INDX is a tool changer built around one **Smart Head** on the gantry and up to ten
**passive tools** parked in a dock (prestonbrown/helixscreen#1333). A passive tool is a
nozzle and a filament path, nothing else: the Smart Head carries the only extruder (the DX
drive), the only heater (an induction coil read by an IR sensor) and a load cell. Each tool
carries its own filament from its own spool, and no purge is needed between tools.

To the UI it is a tool changer: `AmsType::INDX`, `is_tool_changer()` true, one slot per
tool, a change is a swap, each tool owns its filament and spool. To Klipper it is a printer
with one `[extruder]`: `heater_pin: indx:heater`, `sensor_type: indx`.

| Piece | Where |
|---|---|
| Macro contract (what is read and sent) | `include/indx_contract.h`, `src/printer/indx_contract.cpp` |
| Backend | `include/ams_backend_indx.h`, `src/printer/ams_backend_indx.cpp` (derives `AmsBackendToolChanger`) |
| Detection | `src/printer/printer_discovery_parse.cpp#parse_objects`, `#parse_config_keys` |
| Shared heater in ToolState | `ToolTopology::heads_share_heater`, `include/tool_state.h#ToolState/names_mounted_nozzle` |
| Heat gate | `AmsBackend::nozzle_heat_blocked_reason()`, enforced in `src/ui/controls/temperature_controller.cpp#"void TemperatureController::set_target(HeaterType type"` |
| Mock | `HELIX_MOCK_AMS=indx`, `src/api/moonraker_client_mock_indx.cpp` |

## The macro contract

INDX publishes no tool-changer status object. Tool state lives in `save_variables` and in
variables of Bondtech's upstream macros ([BondtechAB/INDX](https://github.com/BondtechAB/INDX)
`macros/indx.cfg`, `macros/indx-tc-macros.cfg`, `macros/indx-cal.cfg`). Those macros are the
contract. Nothing outside these two tables is read or sent.

### Read

| What | Where | Meaning |
|---|---|---|
| Tool on the head | `save_variables.variables.active_tool` | `0..N-1`, `-1` empty head. Absent until `INIT_STATE` runs: no news, not "empty" |
| Half-seated fault | `save_variables.variables.seat_suspect` | `>= 0` names a tool that may be half on the head; changes, parks and `G28 Z` refuse until `MANUAL_TOOL_REMOVE` / `MANUAL_TOOL_SEAT` clear it |
| Toolchange count | `save_variables.variables.toolchange_count` | Diagnostic |
| Latch | `gcode_macro _TOOLCHANGE_VARS.toolhead_state` | `0` locked, `2` open; diagnostic |
| Speed mode | `gcode_macro _TOOLCHANGE_VARS.mode` | `stealth`, `normal`, `sport` in any case; any other value selects nothing in the Speed dropdown |
| Tool offsets | `save_variables.variables.t<n>_offset_x\|y\|z`, `.global_z_offset` | Millimeters, written by INDX's own calibration. Read-only here (the `INDX save_variables` row of `src/printer/tool_offsets.cpp`): absence is no news, never zero |
| Tool count | `gcode_macro TOOL_POSITIONS.tool_count` | Slot count. `indx::config_tool_count()` also reads `variable_tool_count` from the macro's config (`CachedMacroInfo::variables`) so the slots exist before the first frame. An `active_tool` past the count is treated as unknown |
| Tools with no hotend | `gcode_macro TOOL_POSITIONS.no_heat_tools` | A pen or cutter: never heated, no filament ops |
| Docks measured | `gcode_macro TOOL_POSITIONS.docks_calibrated` | `False` refuses every change and park |
| Load presets | `gcode_macro LOAD_FILAMENT.filament_presets` | `TYPE=` names for Load, sent as the printer spells them |
| Per-tool spool | `gcode_macro T<n>.spool_id` | Not upstream: read where a macro set declares it, filed as the lane's Spoolman id. Never written |

Status keys are matched case-insensitively and subscribed under the configured case
(`PrinterDiscovery::macro_config_name`).

### Send

| Command | Used for |
|---|---|
| `T<n>` when `gcode_macro T<n>` exists, else `CHANGE_TOOL TOOL=<n>` | Change tool. `T<n>` runs a user's wrapper exactly as a slicer change does. The macro homes an unhomed printer, turns the coil off before docking and `M109`s the new tool to the outgoing target |
| `PARK_TOOL` | Park, leaving the head empty |
| `LOAD_FILAMENT TOOL=<n> TYPE=<preset>`, or `TEMP=<c>` when the material names no preset (`helix::match_material`: case, then filament family) or is unsafe on a gcode line | Load: picks the tool up, sets the heater's filament model, heats, feeds and primes. Leaves the tool mounted and hot |
| The same, one line per tool, as one script | Batch Load, in the order chosen; the last tool stays mounted and hot. The rpc timeout is one operation's per tool |
| `MODE_STEALTH`, `MODE_NORMAL`, `MODE_SPORT` | Speed dropdown |
| `TC_CYCLE_ALL` | Cycle every tool: each is picked up and parked; the head ends empty |
| `MANUAL_TOOL_SEAT TOOL=<n>` | Seat a tool held on the head by hand (seat-fault recovery, and seat by hand for a first Z home) |
| `MANUAL_TOOL_REMOVE` | Open the latch so the half-seated tool can be taken off (seat-fault recovery) |
| `INDX_CALIBRATE`, `INDX_FAN_CALIBRATE` | The heater and part-fan fits. `[indx]` module commands with no `desc=`, so they cannot be probed and are offered whenever the contract is recognized |
| `SET_TOOL_FILAMENT TOOL=<n> DENSITY=<d> HEAT_CAPACITY=<hc>` | The heater-model update offered after a material edit |
| `SAVE_CONFIG` | After a calibration, through `helix::ui::SaveConfigWatch` only |

Optional macros are used only when listed: `MODE_*` (all three), `TC_CYCLE_ALL`,
`MANUAL_TOOL_SEAT` with `MANUAL_TOOL_REMOVE`, `SET_TOOL_FILAMENT` (`indx::Contract::has_*`).

### Recognized, or not

Required in `printer.objects.list`: `save_variables`, `gcode_macro TOOL_POSITIONS`,
`gcode_macro _TOOLCHANGE_VARS`, `gcode_macro CHANGE_TOOL`, `gcode_macro PARK_TOOL`. With any
missing, the backend still registers but reports **INDX macros not recognized**: no slots, a
`current_error()` naming what is missing, every operation refused, nothing sent.
`LOAD_FILAMENT` is optional; without it Load is Manual.

The #1333 reporter's macros also define `INDX_LOCK`, `INDX_UNLOCK`, `INDX_FORCE_STATE`,
`VERIFY_TOOL_PRESENT` and `T<n>.spool_id`. None of them exists in the upstream repository,
so none is used. `T<n>.active` (for Mainsail/Fluidd's tool buttons) is recent upstream and
missing on older installs, so `active_tool` is the only source of the mounted tool.

## Detection

Either signal registers `AmsType::INDX`:

- `indx` in `printer.objects.list` (the mainline `indx_klipper` module has a `get_status`);
- an `indx` section in `configfile.config` (Kalico's module may not list as an object).
  `parse_config_keys()` runs after `parse_objects()` registered the filament systems, so it
  registers them again when it is the one that finds INDX.

The INDX arm sits after Snapmaker and before the generic tool-changer arm; a real MMU still
wins. Macros alone never detect INDX.

## One heater, several nozzles

Every tool prints through the one `extruder`, so the backend answers
`heads_share_heater()` true and `build_ams_topology()` carries it into `ToolState`:

- `physical_heads()` is one head per tool (the tool switcher and printer chips see six), while
  `heated_heads()` is one entry, standing for the mounted tool. `extruder_count()` is 1.
  `head_reads_heater()` is the one rule both use: only the mounted nozzle shows the heater's
  temperature, so the tool picker's docked cells show material only.
- `names_mounted_nozzle()` is true, so the label is the tool changer one: "Nozzle 3" while T2
  is mounted (or the tool's own name), plain "Nozzle" with the head empty. It drives
  `nozzle_label()`, the nozzle icon badge, the print status tool label and the nozzle temps
  widget's row name. The print status nozzle picker stays on `has_multiple_extruders()`:
  one heater, nothing to pin. The temp graph series stays "Nozzle": it is the heater's
  history across every change.
- `nozzle_heat_blocked_reason()` is "No tool on the head" or "Tool 4 (T3) has no hotend".
  HelixScreen heats with `SET_HEATER_TEMPERATURE`, which bypasses INDX's own `M104` guard,
  so `TemperatureController::set_target()` refuses a nozzle target above 0 while it is set
  and toasts the reason. `ams_nozzle_heat_blocked` dims the nozzle presets and Custom button
  (`styles.heat_blocked`), on the temperature overlay and the filament panel; a tap still
  reaches the controller, which says why. PA tuning and extruder PID/MPC calibration, which
  heat by name too, refuse to start with the same reason.
- `slot_for_extruder()` answers nothing, so filament usage is charged to the mounted slot.
- A change of mounted tool clears the swap preheat latch: the next nozzle is a different melt
  zone, so the previous tool's temperature has nothing to purge.

## Filament operations

| Op | Support |
|---|---|
| Load | Automatic through `LOAD_FILAMENT` (the backend auto-heats, the UI never preheats). Manual without the macro; Unavailable on a `no_heat_tools` tool |
| Unload | Manual: "Pick up the tool, heat it, and pull the filament out". Upstream has no unload macro |
| Change, park | `T<n>` / `CHANGE_TOOL`, `PARK_TOOL`; refused on an unrecognized contract, a half-seated tool, uncalibrated docks, or a paused print (resuming would print with another nozzle). Loading the mounted tool while paused is allowed; loading a docked one is a swap and is refused |

Batch Load sends one `LOAD_FILAMENT` per tool when the macro exists
(`max_heads_per_filament_op()` is the tool count). Every tool passes the single Load's checks
first (`AmsBackendIndx::load_refusal_locked`): one refusal, a no-hotend tool or a docked tool
under a paused print, refuses the whole batch, naming the reason, and nothing is sent. Unload
is never a batch.

A half-seated tool shows as an error with the macro's own instructions. With the `MANUAL_*`
macros it carries two recovery buttons, each behind a confirm
(`RecoveryAction::confirm_message`, see FILAMENT_MANAGEMENT.md): **Tool is seated** sends
`MANUAL_TOOL_SEAT TOOL=<n>` ("Hold Tool 3 (T2) fully on the head..."), **Remove tool** sends
`MANUAL_TOOL_REMOVE` ("The latch opens and Tool 3 (T2) can drop. Hold it..."). Without them the
dialog has no buttons.

## Device actions

Four sections on the device operations overlay, each listed only when it holds an action, and
nothing at all on an unrecognized contract:

| Section | Actions | Gates |
|---|---|---|
| Speed | Tool change speed dropdown (`MODE_*`) | Any time, printing included. An unknown `mode` selects nothing and sends nothing |
| Calibration | Calibrate heater, Calibrate part fan (guided flows), Seat a tool by hand | Calibrations move the machine (refused printing or paused) and need a mounted tool with a hotend; the heater fit also a nozzle target of 0 ("Turn the nozzle heater off first"). Seat by hand is never greyed by a seat fault, uncalibrated docks or an empty head: it is how those get unstuck |
| Maintenance | Cycle every tool (guided flow) | Refused printing or paused; greyed by a seat fault or uncalibrated docks |
| Tool Offsets | One read-only row per tool ("X 0.120  Y -0.045  Z 0.030 mm", an unknown axis `-`), and the global Z offset | None |

The flows live in `src/ui/filament/ui_indx_device_flows.cpp`: each asks first, then runs through
`run_device_action()` and keeps one status line under the section's buttons
(`ui_xml/components/indx_flow_status.xml`) from "Calibrating..." to the result. A calibration
that succeeds offers **Save & Restart** / **Later**; one that times out says the printer may
still be calibrating and offers no save. A dropped connection reads the same way and clears the
backend's busy state, though Klipper may still be calibrating.

The Tool Offsets entry (Controls calibration tile, Advanced row, printer manager chip) opens
the seat-by-hand flow on an INDX with `MANUAL_TOOL_SEAT`
(`src/printer/tool_offset_calibration.cpp#resolve_procedure`): pick the tool you are holding, confirm,
`MANUAL_TOOL_SEAT`. The Advanced row's description comes from `entry_description()`.

After a material edit on the mounted tool, or with the head empty, a material that names a
different preset offers **Update heater model?**, which sends `SET_TOOL_FILAMENT`. Never while
printing, never for a no-hotend tool, never for a docked tool while another is mounted:
`SET_TOOL_FILAMENT` loads the values into the live heater model, which belongs to the mounted
tool.

## Spoolman

`manages_active_spool()` is true when a `T<n>` or `CHANGE_TOOL` template calls
`SET_ACTIVE_SPOOL` or `spoolman_set_active_spool` (`CachedMacroInfo::sets_active_spool`).
Stock Bondtech macros do not, so HelixScreen posts the mounted tool's spool on each mount;
a macro set that does keeps it.

The one extruder never changes, so `feed_slot_signals()` names
`save_variables.variables.active_tool` (value `N` is slot `N`) as the field that says which
tool feeds. The HelixPrint plugin follows it to switch the active spool at every tool change,
including ones made while no screen is connected (`HELIXPRINT_PLUGIN.md`, filament
accounting).

## Mock

`HELIX_MOCK_AMS=indx` is mock hardware with the real backend (it implies `--real-ams`). The
mock publishes the objects and macro state above, only when they change; a change publishes
its park and its pickup before the gcode acks, as the real macro does; heat is refused with no
tool on the head or on a no-hotend tool, so a missing gate shows as a target that never moves.
It lists the phase-3 macros, publishes `mode` and per-tool offsets, and answers
`INDX_CALIBRATE`, `INDX_FAN_CALIBRATE` and `TC_CYCLE_ALL` a sim-speed-scaled minute later, so
the status line is visible at `--sim-speed 6`. Knobs in
`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`.

## Not yet hardware-verified

No INDX is in the test fleet; verification goes through the #1333 reporter. Open:

- Whether Kalico's `[indx]` lists as an object. Detection handles both.
- How the head engages a docked tool's filament, and whether a retract can unload one. Unload
  stays Manual until the reporter confirms; a built-in mount, heat and retract waits on that.
- What the induction heater does with no tool on the head. It is treated as unsafe because
  upstream refuses it.
- Where the reporter's `INDX_FORCE_STATE`, `VERIFY_TOOL_PRESENT` and `spool_id` macros come
  from, and how `spool_id` persists a spool. `T<n>.spool_id` stays read-only until then, so a
  spool assigned on the screen lives in HelixScreen's store.
- Whether a user's own `T<n>` without `variable_active` behaves differently from upstream's.
  Nothing here reads `active`.

Part of the filament system: see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) and the
base this backend extends, [FILAMENT_BACKEND_TOOLCHANGER.md](FILAMENT_BACKEND_TOOLCHANGER.md).
