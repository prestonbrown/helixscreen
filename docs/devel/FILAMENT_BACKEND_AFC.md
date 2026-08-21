# AFC (Armored Turtle / Box Turtle) Filament Backend

AFC-Klipper-Add-On drives Box Turtle, OpenAMS, and Toolchanger hardware, mixable within
one installation. Topology is per unit - `PathTopology::HUB` when a unit's lanes share one
extruder, `PARALLEL` when each lane feeds its own toolhead - inferred from the unit's extruder
count. Box Turtle and OpenAMS units carry 4 lanes each; the roster comes from the unit objects.

## AFC (Armored Turtle / Box Turtle)

AFC-Klipper-Add-On supports multiple hardware types (Box Turtle, OpenAMS) with different topologies. A single AFC installation can mix hardware types — e.g., a Box Turtle feeding 4 toolheads alongside two OpenAMS units each feeding 1 toolhead.

### Hardware Types and Klipper Objects

AFC hardware types register as different Klipper object prefixes:

| Hardware | Klipper Object Prefix | Lane Object | Hub Object | Topology |
|----------|----------------------|-------------|------------|----------|
| Box Turtle | `AFC_BoxTurtle {name}` | `AFC_stepper lane{N}` | `AFC_hub {name}` or none | HUB (standard) or PARALLEL (toolchanger) |
| OpenAMS | `AFC_OpenAMS {name}` | `AFC_lane lane{N}` | `AFC_hub Hub_{N}` | HUB (always) |
| Toolchanger | `AFC_Toolchanger {name}` | — | — | Container only |

**Critical: OpenAMS uses `AFC_lane`, not `AFC_stepper`.** Both have the same JSON schema and are parsed through the same `parse_afc_stepper()` function. We subscribe to both object types.

### Unit Object Structure (Real Production Data)

Each unit-type object provides lane/extruder/hub/buffer membership. This data comes from Klipper, not from individual lane queries.

**Box Turtle in toolchanger mode** (`AFC_BoxTurtle Turtle_1`):
```json
{
    "lanes": ["lane0", "lane1", "lane2", "lane3"],
    "extruders": ["extruder", "extruder1", "extruder2", "extruder3"],
    "hubs": [],
    "buffers": ["TN", "TN1", "TN2", "TN3"]
}
```
- 4 extruders → PARALLEL topology (each lane feeds its own toolhead)
- No hubs (lanes use `hub: "direct_load"` — direct connection to extruder)
- TurtleNeck buffers per lane

**OpenAMS** (`AFC_OpenAMS AMS_1`):
```json
{
    "lanes": ["lane4", "lane5", "lane6", "lane7"],
    "extruders": ["extruder4"],
    "hubs": ["Hub_1", "Hub_2", "Hub_3", "Hub_4"],
    "buffers": []
}
```
- 1 extruder → HUB topology (all 4 lanes converge to 1 toolhead)
- Per-lane hubs: each lane has its own hub (Hub_1 for lane4, Hub_2 for lane5, etc.)
- Hub names do NOT match unit names (Hub_1 ≠ AMS_1)
- No buffers (no TurtleNeck needed)

### Topology Determination

AFC topology is inferred from the extruder count per unit:
- **1 extruder** → `PathTopology::HUB` (all lanes merge to one toolhead)
- **N extruders (N == lane count)** → `PathTopology::PARALLEL` (1:1 lane-to-tool mapping)

This is stored per-unit in `unit_topologies_[]` and queried via `get_unit_topology(unit_index)`.

### The `map` Field Problem

AFC assigns each lane a virtual tool number via the `map` field (e.g., `"T4"`). **For HUB units, AFC gives each lane a unique map value even though all lanes physically feed the same extruder.**

Real production data from a 6-toolhead mixed system:

| Lane | Unit | Hub | Extruder | map | Physical Tool |
|------|------|-----|----------|-----|---------------|
| lane0 | Turtle_1 | direct_load | extruder | T0 | T0 |
| lane1 | Turtle_1 | direct_load | extruder1 | T1 | T1 |
| lane2 | Turtle_1 | direct_load | extruder2 | T2 | T2 |
| lane3 | Turtle_1 | direct_load | extruder3 | T3 | T3 |
| lane4 | AMS_1 | Hub_1 | extruder4 | T4 | T4 |
| lane5 | AMS_1 | Hub_2 | extruder4 | T5 | **T4** (same physical nozzle) |
| lane6 | AMS_1 | Hub_3 | extruder4 | T6 | **T4** (same physical nozzle) |
| lane7 | AMS_1 | Hub_4 | extruder4 | T7 | **T4** (same physical nozzle) |
| lane8 | AMS_2 | Hub_5 | extruder5 | T8 | T5 |
| lane9 | AMS_2 | Hub_6 | extruder5 | T9 | **T5** (same physical nozzle) |
| lane10 | AMS_2 | Hub_7 | extruder5 | T10 | **T5** (same physical nozzle) |
| lane11 | AMS_2 | Hub_8 | extruder5 | T11 | **T5** (same physical nozzle) |

**The `map` field represents virtual tool numbers for AFC's internal routing, not physical toolheads.** The UI must use topology to determine physical tool count for drawing nozzles:
- PARALLEL: `tool_count = max_tool - min_tool + 1` (each map value = different nozzle)
- HUB: `tool_count = 1` (all map values = same nozzle)

### Hub Sensor Propagation

Standard Box Turtle: hub name matches unit name (e.g., hub "Turtle_1" for unit "Turtle_1"), so `hub_name == unit.name` works.

OpenAMS: hub names are per-lane (Hub_1, Hub_2, ..., Hub_8) and do NOT match the unit name (AMS_1, AMS_2). Hub sensor state must be propagated by looking up which unit owns the hub via the `unit_infos_` hub membership lists.

The code uses a two-strategy approach:
1. Check `unit_infos_[].hubs` to find the parent unit (handles OpenAMS)
2. Fallback: direct `hub_name == unit.name` match (handles standard Box Turtle)

If ANY hub in a unit is triggered, `unit.hub_sensor_triggered = true`.

### AFC Lane Status Values

Status values observed in production and their mapping to `SlotStatus`:

| AFC Status | `tool_loaded` | Meaning | SlotStatus |
|------------|---------------|---------|------------|
| `"Tooled"` | any | Actively loaded in toolhead (OpenAMS) | LOADED |
| `"Loaded"` | true | Filament loaded to toolhead | LOADED |
| `"Loaded"` | false | Filament loaded to hub (not toolhead) | AVAILABLE |
| `"Ready"` | false | Filament present, sensors triggered | AVAILABLE |
| `"None"` | false | No filament, no sensors | EMPTY |
| `"Error"` | any | Lane error | AVAILABLE + SlotError |
| `""` (empty) | false | No data yet | EMPTY |

**Critical**: AFC's `"Loaded"` status means hub-loaded, NOT toolhead-loaded. The `tool_loaded` boolean is the authoritative indicator of toolhead presence. Only `tool_loaded: true` or `status: "Tooled"` maps to `SlotStatus::LOADED`. The `"Loaded"` status string alone (with `tool_loaded: false`) maps to `AVAILABLE`.

### Other AFC Lane Fields

Fields present in production `AFC_lane` data but not in `AFC_stepper`:
- `buffer: null` and `buffer_status: null` (OpenAMS has no buffers)
- `dist_hub: 60` (OpenAMS, short distance) vs `1940-2230` (Box Turtle, long bowden)
- `td1_td`, `td1_color`, `td1_scan_time` — TD1 filament tag detection sensor data (not currently used by HelixScreen)

### Detection

Klipper object `AFC` in `printer.objects.list` sets `AmsType::AFC`. Lane names come from `AFC_stepper lane*` and `AFC_lane lane*` objects, hub names from `AFC_hub *` objects. Unit-type objects (`AFC_BoxTurtle`, `AFC_OpenAMS`) provide the lane/extruder/hub/buffer membership that determines per-unit topology.

### Data Sources

AFC state comes from multiple Klipper objects:

**Per-lane state** (`AFC_stepper lane{N}` or `AFC_lane lane{N}`):

| Field | Type | Description |
|-------|------|-------------|
| `prep` | bool | Prep sensor triggered |
| `load` | bool | Load sensor triggered (AFC calls this `raw_load_state` internally) |
| `loaded_to_hub` | bool | **DO NOT USE — see "Fields that do not mean what they say"** |
| `tool_loaded` | bool | Filament loaded to toolhead |
| `status` | string | "Loaded", "Tooled", "Ready", "None", "Error" |
| `color` | string | Filament color hex (`#RRGGBB`) |
| `material` | string | Material type from Spoolman |
| `spool_id` | int | Spoolman spool ID |
| `weight` | float | Remaining weight in grams |
| `buffer_status` | string | Buffer state (e.g., "Advancing") |
| `filament_status` | string | Readiness (e.g., "Ready", "Not Ready") |
| `dist_hub` | float | Distance to hub in mm |

**Hub state** (`AFC_hub {name}`):

| Field | Type | Description |
|-------|------|-------------|
| `state` | bool | Hub sensor triggered. **One sensor per UNIT, shared by every lane on it** — it cannot say whose filament tripped it. Trustworthy, unlike `loaded_to_hub`. |
| `afc_bowden_length` | float | Bowden tube length from hub to toolhead (mm) |

**Extruder state** (`AFC_extruder extruder`):

| Field | Type | Description |
|-------|------|-------------|
| `tool_start_status` | bool | Toolhead entry sensor |
| `tool_end_status` | bool | Toolhead exit/nozzle sensor |
| `lane_loaded` | string | Currently loaded lane name |

**Global state** (`AFC`):

| Field | Type | Description |
|-------|------|-------------|
| `current_lane` | string | Lane AFC is working (or null). Null after a crash-interrupted toolchange — see below. |
| `current_load` | string | Lane being loaded (or null). Fallback when `current_lane` is null. |
| `current_state` | string | "Idle", "Loading", "Unloading", "Error", etc. |
| `error_state` | bool | **Not the error signal.** Measured `false` for an entire session while an error was queued. Use `message.type == "error"`. |
| `message` | object | `{message, type}` — **the HEAD of a FIFO queue, not a scalar.** See below. |
| `lanes[]` | string[] | List of lane names |
| `quiet_mode` | bool | Quiet mode state |
| `led_state` | bool | LED strip on/off |

### Fields that do not mean what they say

Established by measurement on a live BoxTurtle (2026-07-27) and by reading
`AFC-Klipper-Add-On` v1.2.0. Every one of these cost real debugging time; do not re-derive them.

**`AFC_stepper.<lane>.loaded_to_hub` is latched and inert.** It is set once at prep and never
updated. On a 4-lane unit it reads `true` on **all four lanes simultaneously** while the shared
hub sensor reads clear — physically impossible for one hub. It does not change when filament
actually transits the hub. Verified by pushing a lane 250 mm past the hub and retracting it:
`AFC_hub.state` tracked the move exactly, `loaded_to_hub` never moved.

*Use `AFC_hub.<hub>.state` for hub occupancy.* Resolve a lane's hub through the per-lane `hub`
field (`"Turtle_1"`, or the literal `"direct"` meaning no hub in that lane's path).

**`AFC.message` is a FIFO queue head.** Each `AFC_CLEAR_MESSAGE` pops exactly one entry;
Klipper's own help string reads *"clear error and warning message from AFC message queue"*. A
new error raised while an older one is unacknowledged is enqueued **behind** it and cannot
display until the earlier entry is popped. Observed depth 4 during one real failure, with a
slicer-deprecation warning at the head hiding the actionable load error behind it. Clearing an
already-empty queue is a harmless no-op.

*The queue only ever grows on its own.* One entry per `AFC_logger.error()` / `.warning()`
call — **not** one per line; the per-line loop in AFC_logger.py writes the log file, and the
`message_queue.append((message, ...))` that follows it sits outside that loop, so a five-line
`TOOL_LOAD` diagnostic is a single entry carrying embedded newlines. Nothing pops entries
implicitly: `reset_failure()` (AFC_error.py) and `AFC_RESUME` both leave `message_queue`
untouched, so entries accumulate across a whole session and anything left behind resurfaces as
the next session's stale error. (Verified against the add-on source on a live BoxTurtle,
2026-07-29.)

*A single clear is not enough.* `AmsBackendAfc::clear_fault()` drains **until the queue reports
empty**, bounded by a wall-clock deadline and by `MESSAGE_DRAIN_MAX_CLEARS` as a runaway guard
(not as the expected stopping point); see `message_drain_budget_` / `message_drain_deadline_`.

**`AFC.error_state` is not the *detection* signal.** It stayed `false` for a whole session while
`message` held an error, so `message.type == "error"` is what tells you a fault exists. But
`error_state_` is far from inert: besides `error_segment_` and the `classify_error` catch-all, it
is the entire gate on `AmsBackendAfc::current_error()`, which returns `nullopt` unless it is set.
That is the whole status-driven fault channel for AFC; see
§ [Two error channels](FILAMENT_MANAGEMENT.md#two-error-channels).

**The hub sensor cannot attribute a strand to a lane.** One sensor per unit, shared. When a
strand is stuck past the hub, every lane on that unit looks identical — during a live failure
lanes 1 and 4 both read `prep=True load=True loaded_to_hub=True` and only lane 4's filament was
actually in the hub. `AFC.current_lane` is the only attribution signal, and it is null after a
Klipper crash mid-toolchange. **Software cannot determine this from sensors.** See
`active_load_lane_` and `can_recover_lane_position()`.

This is not a signal that is merely unwired. It does not exist. The full measured state during
that failure:

```
lane1  prep=True  load=True  loaded_to_hub=True
lane4  prep=True  load=True  loaded_to_hub=True
AFC_hub Turtle_1.state = True   (one sensor, shared by all four lanes)
AFC.current_lane = None
```

Console history pointed at lanes 1 and 2. The answer was lane 4, and only looking at the machine
established it. Anything that claims to pick the stranded lane out of sensor data is guessing,
and it will be wrong three times in four on a 4-lane unit.

**A failed `AFC_LANE_RESET` names the wrong lane, it does not report a failure.**
`cmd_AFC_LANE_RESET` retracts the named lane until the hub clears, bailing if *that lane's* own
switch opens first:

```
"'{lane}' failed to reset to hub, load switch became false during reset"   → wrong lane
"'{lane}' failed to reset to hub, prep switch became false during reset"   → wrong lane
"'{lane}' failed to reset to hub"  (no switch named)                       → nothing owns it
```

The first two also mean **that lane has now been retracted past its own switch** and will fail
its next load with "LOAD TRIGGER NOT TRIGGERED" until advanced forward again. The third means
the retract ran the full bowden without clearing — most likely a snapped fragment in the hub,
which no lane reset can ever clear.

**A wrong lane guess is destructive, not free.** The retract loop runs until *that lane's* own
switch opens, so the guess always ends with the lane pulled back behind its load sensor. In the
observed instance a guess at lane 1 left it `load=False`; a forward lane move of 20 mm restored
the switch (driven that night through BoxTurtle's `BT_LANE_MOVE` wrapper; the portable command
is `LANE_MOVE`), after which `T0` loaded normally. Until that forward move the lane is unusable,
and tapping the lane reset again only drags it further back.

*Automatic sequential retry is therefore rejected, deliberately.* Walking the roster on the
user's behalf leaves every lane it eliminates de-seated: four lanes tried, three working lanes
broken, to reach an answer a person standing at the machine can read off it directly. Do not add
it later as a convenience. The only defensible way to spend a guess is one at a time, with the
resulting de-seat undone before the next.

**When every lane on a hub has been eliminated, the hub holds a broken fragment.** Each
wrong-lane diagnostic rules out one candidate. Once the whole roster routed to that hub has
returned it, nothing on that unit owns the obstruction, no lane reset can ever clear it, and it
comes out by hand. AFC reaches the same conclusion on the load path: AFC.py raises *"Hub not
clear when trying to load. Please check that hub does not contain broken filament and is
clear"*. This case is not exotic; it occurred twice in one evening on the `.112` rig. A recovery
flow modelled only on "which lane is it" never terminates here.

**`AFC_LANE_RESET`'s toolhead guard does not actually stop it.** In v1.2.0 (`a06f14d`) the
hub-clear guard has a `return`; the toolhead guard does not:

```python
if not CUR_HUB.state:
    ...AFC_error("Hub is already clear while trying to reset '{lane}'")
    return                                  # returns

if (tool_load := self.get_current_lane_obj()) is not None:
    ...AFC_error("Toolhead is loaded with '{name}'...")
                                            # NO return — falls through and moves filament
```

So AFC logs the refusal and then retracts the lane anyway, while the extruder still grips the
filament. Reported as [AFCProject/AFC-Klipper-Add-On#803](https://github.com/AFCProject/AFC-Klipper-Add-On/issues/803),
open as of 2026-07-28.

*`can_recover_lane_position()`'s `filament_loaded` check is therefore load-bearing safety, not a
politeness mirror of an upstream guard.* Do not remove it as redundant.

**A filament swap resets lane identity when `remember_spool` is false.** AFC re-applies
`[afc] default_material_type` and `full_weight`, discarding material, colour and weight. Lanes
carrying a Spoolman `spool_id` survive; lanes without one silently revert. HelixScreen's
`FilamentSlotOverrideStore` (private AFC namespace) exists to preserve identity across this.

**Moonraker database** (AFC namespace, `lane_data` key -- v1.0.32+; outer keys
are `T(n)` per mapping on the virtual-tools firmware, `laneN` before):

```json
{
  "T0": {"color": "FF0000", "material": "PLA", "loaded": false},
  "T1": {"color": "00FF00", "material": "PETG", "loaded": true}
}
```

### G-code Commands

Verified against `AFC-Klipper-Add-On` v1.2.0. Two kinds exist and the distinction matters:
**Python** commands are registered by AFC's extras modules and are always present; **config
macro** entries ship in AFC's `config/` templates and can be absent, renamed or edited on a
given machine. `BT_*` macros are BoxTurtle-specific and do not exist on other unit types.

| Command | Kind | Action |
|---------|------|--------|
| `CHANGE_TOOL LANE={name}` / `T{n}` | Python | Tool change (unload + load) |
| `TOOL_LOAD LANE={name}` | Python | Load a lane into the toolhead |
| `TOOL_UNLOAD` | Python | Unload the toolhead |
| `LANE_UNLOAD LANE={name}` | Python | Eject a lane's filament back to the spool |
| `LANE_MOVE LANE={name} DISTANCE={float}` | Python | Manual lane move. Negative retracts. **Refuses while printing** unless `FORCE=1`. Zero distance is an error. See the note below on `DISTANCE`'s type. |
| `HUB_LOAD LANE={name}` | Python | Advance a lane to its hub |
| `AFC_LANE_RESET LANE={name}` | Python | Retract a lane from the bowden back to its hub. Requires hub occupied + toolhead free. |
| `AFC_RESET` | Python | **Opens a lane-picker prompt**, not a system reset. Lists lanes with `raw_load_state` true and dispatches `AFC_LANE_RESET` for the chosen one. With no candidates: *"No lanes are loaded, a lane must be loaded to be reset"*. |
| `RESET_FAILURE` | Python | Clear AFC's failure state |
| `AFC_CLEAR_MESSAGE` | Python | Pop **one** entry from the message queue |
| `SET_LANE_LOADED LANE={name}` | Python | Mark a lane as toolhead-loaded without moving filament |
| `UNSET_LANE_LOADED` | Python | Clear the toolhead-loaded marker |
| `SET_MAP LANE={name} MAP=T{n}` | Python | Set lane-to-tool mapping |
| `SET_MATERIAL LANE={name} MATERIAL={type}` | Python | Set a lane's material |
| `SET_COLOR LANE={name} COLOR={hex}` | Python | Set a lane's colour |
| `SET_WEIGHT LANE={name} WEIGHT={g}` | Python | Set a lane's remaining weight |
| `SET_SPOOL_ID LANE={name} SPOOL_ID={id}` | Python | Link a lane to a Spoolman spool |
| `SET_BOWDEN_LENGTH HUB={hub} LENGTH={mm}` | Python | Set bowden length (mux keyed on `HUB`) |
| `SET_RUNOUT LANE={name} RUNOUT={backup_lane}` | Python | Set endless spool backup |
| `RESET_AFC_MAPPING RUNOUT=no` | Python | Reset tool mappings only. **Renamed `AFC_RESET_MAPPING` by the virtual-tools firmware (Klipper-Add-On #832), which deregistered the old name**; HelixScreen picks per firmware via the `multiple_tool_mapping` status flag |
| `AFC_CALIBRATION` | Python | Run calibration wizard |
| `AFC_RESET_MOTOR_TIME LANE={name}` | Python | Reset motor run-time counter |
| `AFC_QUIET_MODE` | Python | Toggle quiet mode |
| `TURN_ON_AFC_LED` / `TURN_OFF_AFC_LED` | Python | Toggle LED strip |
| `AFC_CUT` / `AFC_PARK` / `AFC_BRUSH` / `AFC_POOP` / `AFC_KICK` | config macro | Toolhead servicing. Ship in AFC's config templates; may be absent or edited. |
| `BT_LANE_MOVE` / `BT_LANE_EJECT` / `BT_TOOL_UNLOAD` / `BT_CHANGE_TOOL` / `BT_PREP` | config macro | **BoxTurtle only.** Thin wrappers over the Python commands above — prefer the Python command. |

**`LANE_MOVE`'s `DISTANCE` is a float, and AFC's own metadata says otherwise.** The
`cmd_LANE_MOVE_options` dict (extras/AFC.py:1010) labels it `{"type": "int"}`, but nothing
consumes that dict for parsing — the command body does `gcmd.get_float('DISTANCE', 0)`. Read the
function body, not the options metadata, when documenting any AFC command; the metadata is
descriptive and can be wrong about its own command. (This exact mistake was made and caught
while writing this section.)

`LANE_MOVE` also returns early with *"Cannot move lane while printer is printing"* unless
`FORCE=1`, and rejects a zero distance. Anything automating a lane move during a paused print
needs to account for both.

**Commands that do NOT exist.** These appeared in earlier revisions of this document and were
never real — verified absent from both AFC's Python registrations and its shipped config macros.
Do not reintroduce them:

| Fiction | Use instead |
|---------|-------------|
| `AFC_HOME` | Nothing homes AFC. `AFC_RESET` opens a lane picker; `reset()`/`recover()` both send `AFC_RESET`. |
| `AFC_LOAD` | `TOOL_LOAD LANE={name}` or `CHANGE_TOOL LANE={name}` |
| `AFC_UNLOAD` | `TOOL_UNLOAD` (toolhead) or `LANE_UNLOAD LANE={name}` (lane to spool) |
| `AFC_LANE_MOVE` | `LANE_MOVE` — the `AFC_` prefix is not real |

### AFC console response contract

AFC narrates its operations over `notify_gcode_response`, and the toolchange step bar is driven
entirely by matching those strings — there is no structured field for "which phase am I in". This
is the undocumented string contract of #1153; the shapes below were captured verbatim from a live
12-toolchange print on the BoxTurtle rig via `server/gcode_store`
(`N` = a digit run; the verbatim strings live in `tests/unit/test_afc_console_corpus.cpp`). Tests drive the exact
strings: `tests/unit/test_afc_console_corpus.cpp`.

**AFC emits narration on two different channels, and they need different matchers.**

#### Channel 1 — bare lines (no `//`, no `!!`)

`AmsBackendAfc::match_bare_narration_phase()`. These carry the *semantically important* half of a
toolchange. Klipper's `respond_raw` gives them no prefix at all, so before this was split out the
router's `//`-only filter discarded every one of them and the step bar could only ever advance on
the decorative cut/brush lines.

| Shape | Phase | Source |
|-------|-------|--------|
| `Loading laneN` | `feed` | AFC.py `TOOL_LOAD` |
| `Unloading laneN` | `unload` | AFC.py `TOOL_UNLOAD` |
| `laneN is now loaded in toolhead t:N` | `load` | load complete (`t:N` absent on pre-toolchanger builds) |
| `Lane laneN unload done t:N` | `unload` | unload complete |
| `Tool Change - laneN -> laneN`, `Tool Change - None -> laneN` | *(none)* | toolchange banner — no phase in the template |
| `Total change time: t:N` | *(none)* | toolchange end — no phase in the template |
| `laneN already loaded` | *(none)* | CHANGE_TOOL no-op (#1183). Must **not** read as a completed load |

**Bare lines are matched by anchored shape, never by substring.** The unprefixed channel is the
printer's open console: the same stream carries `B:N /N TN:N /N` temperature reports, `echo:` output
from user macros, `Rotation distance reset : N`, an HTML `<span class=warning--text>…</span>`
deprecation notice — and `File opened: <name>.gcode Size: N`, where the filename is
**user-controlled**. A loose `has("cut")` needle turns anyone's `haircut.gcode` into a Cut-tip step.
So each shape is pinned on fixed words in fixed positions plus a token count: `Loading laneN` matches
only as exactly two whitespace-separated tokens, and the load-complete line needs all five of
`is now loaded in toolhead` in sequence.

> **Residual exposure.** `Loading <one-word>` is the weakest shape — a user macro doing
> `M118 Loading mesh` during an active toolchange would advance the bar one step. Tightening it
> further would mean validating the second token against the configured lane names, which the
> matcher deliberately avoids: it is a pure function today, and reading the lane registry would put
> a lock into a per-console-line path for a cosmetic-only gain.

#### Channel 2 — `//` lines

`AmsBackendAfc::match_narration_phase()`. A `//` body came from a macro's own `respond_info`, so
upstream owns the wording and the matcher is deliberately loose: it normalizes to lowercase words
and substring-matches, so `AFC_Brush: Clean Nozzle`, `AFC Brush - Clean nozzle` and
`[AFC_Brush] Clean Nozzle!` all land on `brush`.

| Shape | Phase |
|-------|-------|
| `// AFC_Cut: …` (`Cut Filament`, `Moving to cutter pin`, `Retract Filament for Cut`, `Cut Move…`, `Final Cut…`, `Push cut tip back into hotend`, `Clearing cutter pin`) | `cut` |
| `// AFC_Brush: …` (`Clean Nozzle`, `Move to Brush.`, `Y Brush Moves`, `X Brush Moves`) | `brush` |
| `// AFC_Poop: …` (`Starting poop`, `Move To Purge Location`) | `poop` |
| `// AFC_Kick: …` | `kick` |
| `// AFC_Park: Park Toolhead` | *(none)* — AFC's park has no step in the template, so it stays unmatched rather than borrowing a neighbour. Adding it would mean adding a real phase. |
| `// Smart Park location: N,N.`, `// Moving filament tip N.Nmms`, `// DESCRIBE_COLOR: …`, `// TOOLCHANGE: filament …`, `// Run Current: …`, `// pressure_advance: N`, `//      Change N out of N` | *(none)* |

`// KAMP purge is not using firmware retraction…` does match `poop` via the loose `purg` needle.
That is accepted: KAMP's advisory only appears around the purge anyway, so the phase it lands on is
the right one.

#### `// Unknown command:"X"` — an aborted macro that still returns `ok`

Klipper reports a macro referencing an undefined command through `respond_info` as
`// Unknown command:"STATUS_PURGING"` — **not** `!!` — and Moonraker still returns `ok` for the
enclosing script. Nothing else in the stack can distinguish "the macro ran" from "the macro died on
line 4", so the operation's success callback fires and the button shows a green checkmark for a
macro that did nothing. (Observed four times in the captured window: a `purge_filament` macro
aborting because the user's LED config has no `STATUS_PURGING`.)

`GcodeNarrationRouter` claims the line before either matcher sees it — `parse_unknown_command()`,
anchored at the start of the body — and hands the command name to
`FilamentPanel::fail_op_on_unknown_command()`, which fails the visibly-running operation and names
the missing command in the toast. Claiming it early also stops the error message itself from driving
the step bar: `has("purg")` reads `STATUS_PURGING` as a real purge phase.

**Correlation is best-effort.** Klipper does not tie a response line to the RPC that provoked it, so
the only handle is "an operation is showing its spinner". An unknown-command line raised by another
client while a filament op happens to be running will fail that op. That is the lesser harm — a
checkmark for a macro that never ran is what sends users hunting the wrong problem.

#### Drift hints

When neither matcher claims a line, `AmsBackend::is_narration_drift_candidate()` decides whether it
is worth a deduped `debug` log. AFC's answer is deliberately looser than its matchers (any line
naming `afc` or a `lane` — the hint exists to catch *rewording*, which by definition no matcher
recognizes) minus the lines it emits every toolchange that have no phase by design (`tool change`,
`already loaded`, `total change time`, `rotation distance reset`). Grep `[GcodeNarration] no phase
matched` after an AFC upgrade.

#### Channel 3 — `!!` lane faults, and the position diagram welded to them

Five AFC error sites append a monospace position diagram to their sentence. Verbatim, exhaustively
(read off a live BoxTurtle, #1184):

```python
AFC.py:1294  'filament did not trigger hub sensor, CHECK FILAMENT PATH\n||=====||==>--||-----||\nTRG   LOAD   HUB   TOOL.'
AFC.py:1345  'filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH\n||=====||====||==>--||\nTRG   LOAD   HUB   TOOL'
AFC.py:1370  'filament failed to trigger post extruder gear toolhead sensor, CHECK FILAMENT PATH\n||=====||====||==>--||\nTRG   LOAD   HUB   TOOL'
AFC.py:1469  'Current lane not loaded, LOAD TRIGGER NOT TRIGGERED\n||==>--||----||-----||\nTRG   LOAD   HUB   TOOL'
AFC_BoxTurtle.py:527  ' FAILED TO LOAD, CHECK FILAMENT AT TRIGGER\n||==>--||----||------||\nTRG   LOAD   HUB    TOOL'
```

**The art is a hardcoded literal per error site, not a rendering of live sensor state**, so
parsing it buys nothing and costs precision: `:1345` (**pre** extruder gear) and `:1370` (**post**
extruder gear) emit byte-identical bars for two faults with different remedies, and
AFC_BoxTurtle.py writes `||------||` where AFC.py writes `||-----||`. We therefore map the
**message text**, and strip the art.

`helix::afc::afc_fault_position()` (`include/afc_fault_position.h`) — a pure function, no LVGL, no
printer state:

| Message fragment | Filament reached | `PathSegment` |
|---|---|---|
| `LOAD TRIGGER NOT TRIGGERED` | short of the lane trigger | `SPOOL` |
| `CHECK FILAMENT AT TRIGGER` | short of the lane trigger | `SPOOL` |
| `did not trigger hub sensor` | past lane, short of the hub | `HUB` |
| `pre extruder gear toolhead sensor` | past hub, short of the toolhead | `OUTPUT` |
| `post extruder gear toolhead sensor` | at toolhead, short of the extruder gears | `TOOLHEAD` |

Matching is case-insensitive and **anchored on word boundaries** — the same open-console hazard as
Channel 1 applies, and `File opened: check filament at triggering.gcode` must not resolve to a
position. Anything else returns `std::nullopt`, and `afc_strip_position_diagram()` is gated on that
optional: an unrecognised message is returned byte-for-byte, so upstream rewording degrades to the
plain-text rendering we had before rather than mangling the sentence. Tests drive the exact strings:
`tests/unit/test_afc_fault_position.cpp`.

**Where it surfaces.** Both modals that can show an AFC lane fault route their text through
`helix::ui::afc_fault_path_apply()` (`include/ui_afc_fault_path.h`), which publishes the stop point
to the int subject `afc_fault_segment` and returns the stripped text:

| Path | Modal | Call site |
|---|---|---|
| `!!` -> `GcodeErrorRouter` -> `RecoveryModalPresenter` | `ActionPromptModal` | `recovery_modal_presenter.cpp` `present()` |
| `AmsAction::ERROR` rising edge -> `AmsErrorBridge` -> `backend->current_error()` -> `RecoveryModalPresenter` | `ActionPromptModal` | same call site as the row above, reached with no `!!` line involved |
| `printer.AFC.message` -> `AmsAction::ERROR` -> `AmsPanel` | `AmsLoadingErrorModal` | `ui_panel_ams.cpp` `show_loading_error_modal()` |

All three can fire for the same fault. The graphic itself is `ui_xml/components/afc_fault_path.xml` —
four labelled checkpoints joined by the three **gaps** between them, all bound to
`afc_fault_segment` alone; 0 (`PathSegment::NONE`) hides the whole component.

**The gap, not the checkpoint, is what gets marked**, and that is not cosmetic. AFC's own art is
three sections under four labels (`Spool`→`Lane`, `Lane`→`Hub`, `Hub`→`Toolhead`), which is why it
is so often misread as being off by one. The source settles it: `did not trigger hub sensor` fires
*after* `cur_lane.loaded_to_hub = True`, and `pre extruder gear toolhead sensor` fires while homing
down the bowden past an already-cleared hub. Both are failures *between* checkpoints. Colouring the
checkpoint red would tell the user the hub failed, about a hub the filament passed cleanly. The one
exception is `post extruder gear toolhead sensor`, which genuinely fails *at* the toolhead — the
filament cleared the sensor and jammed in the extruder gears — so `TOOLHEAD` marks the node itself.

Position alone is not enough on its own, though: it says which element differs from its
neighbours, not that the difference means failure, and red-against-green is precisely the pair a
colourblind user cannot separate (#1196). So the component also renders one of four captions —
*Stopped between Hub and Toolhead*, and so on — bound to the same subject and mutually exclusive
on it. The caption is also the only thing that can express `TOOLHEAD`, which fails at a node
rather than in a gap.

Every caller must go through `afc_fault_path_apply()` even when the message is not AFC's, or a
previous fault's marker stays on screen.

#### Maintaining the contract across AFC versions

Everything above is a contract with a project that never agreed to one. AFC's console wording is
not an API, is not versioned, and moves when a maintainer improves a sentence. Neither side breaks
loudly when it does: the step bar simply stops advancing, or a lane fault renders as plain text.
This subsection is the maintenance half of #1153 — what we depend on, where it lives on our side,
and what to do on an AFC version bump.

**What the narration actually drives.** A matched phase id is looked up in the *active operation's*
phase template (`AmsBackendAfc::toolchange_phase_template()`), and the step bar advances to that
index. A phase id the running operation's template does not contain is matched and then dropped —
`GcodeNarrationRouter::process_line()` leaves the step subject untouched — so a needle is only ever
as useful as the template it feeds:

| Operation | Phase ids, in order (opt = optional: stays Pending when never narrated) |
|---|---|
| `LOAD_SWAP` (toolchange) | `heat`, `cut` (opt), `unload`, `feed`, `poop` (opt), `brush` (opt), `kick` (opt), `load` |
| `LOAD_FRESH` | `heat`, `feed`, `poop` (opt), `brush` (opt), `kick` (opt), `load` |
| `UNLOAD` | `heat`, `cut` (opt), `unload` |

There is no `park` and no `clean` distinct from `brush` — AFC has exactly one purge macro and one
wipe macro, so adding either needle without first adding the phase would be dead code.

**Our whole side of the contract is four matchers, in two files.** Nothing else needs touching
when upstream rewords:

| File | Owns |
|---|---|
| `src/printer/ams_backend_afc.cpp` `match_narration_phase()` | Channel 2 needles — loose, normalized substring |
| `src/printer/ams_backend_afc.cpp` `match_bare_narration_phase()` | Channel 1 shapes — anchored words plus token count |
| `include/afc_fault_position.h` (impl in `src/printer/afc_fault_position.cpp`) | Channel 3 fault-position fragments |
| `src/printer/ams_backend_afc.cpp` `is_narration_drift_candidate()` | which unmatched lines are worth a drift hint |

The literals to grep upstream for, exhaustively. Channel 2 is matched after collapsing everything
non-alphanumeric to single spaces and lowercasing, so grep case-insensitively and ignore
punctuation:

| Needle(s) | Phase | Emitted by |
|---|---|---|
| `is now loaded in toolhead`, `load complete`, `loaded in toolhead` | `load` | extras/AFC.py |
| `unload` | `unload` | extras/AFC.py |
| `clean nozzle`, `cleaning nozzle`, `brush` | `brush` | `config/macros/Brush.cfg` (`AFC_BRUSH`) |
| `purg`, `poop` | `poop` | `AFC_POOP`, in AFC's shipped `config/macros/` |
| `kick` | `kick` | `AFC_KICK`, same |
| `cut` | `cut` | `AFC_CUT`, same |
| `retract` | `unload` | `AFC_CUT`'s retract step (#1046) |
| `to hub`, `feed`, `loading lane` | `feed` | extras/AFC_functions.py, extras/AFC_BoxTurtle.py |
| `heat` | `heat` | toolhead heat-up narration |

**Order is load-bearing in both matchers** and is not an implementation detail: `unload` must be
tested before the `feed` needles, because normalized `unloading lane1` contains `loading lane`;
`cut` must be tested before `retract`, because `AFC_Cut` says *Retract Filament for Cut*. Reordering
the `if` chain silently reassigns phases.

**Channel 2's sources are config macros, not Python.** `AFC_BRUSH`, `AFC_POOP`, `AFC_CUT` and
`AFC_KICK` ship as templates under AFC's `config/macros/` and the user's copy is theirs to edit. A user who
renames a `RESPOND` in their own macro breaks their own step bar and no upstream release is
involved. That is also why Channel 2 is deliberately loose while Channel 1, which runs on the open
console, is anchored.

**On an AFC version bump:**

1. Grep the new AFC tree for each literal in the table above. Anything that has moved needs the
   needle updated *and* the verbatim new string added to `tests/unit/test_afc_console_corpus.cpp`.
2. Re-check Channel 3's five error sites in extras/AFC.py and extras/AFC_BoxTurtle.py; those
   are matched on message text, not on the position art, so a reworded *sentence* is what breaks
   them, not a redrawn bar.
3. Run `./build/bin/helix-tests "[afc][narration][corpus]" "[narration][router]" "[afc][fault]"`.
4. Drive a real toolchange with `-vv` and grep the log for `[GcodeNarration] no phase matched`.
   That line is deduped and is the only automatic signal that a string moved; a *silent* log with
   a stalled step bar means the wording changed to something `is_narration_drift_candidate()`
   does not recognise as AFC's either, which is the worst case and needs the hint widened too.

**The permanent fix is upstream, not here.** AFC's macros already know which step they are on —
they are the ones emitting the `RESPOND` — so publishing that step as a status field would let
every UI drop string scraping. Until then, this section is load-bearing: four separate features
(step bar, terminating responses #1183, position art #1184, failure classification #1182) all
scrape the same console because there is no structured channel to read.

### Path Topology

`PathTopology::HUB` -- Multiple lanes merge into a common hub/merger. Sensor-based position inference:

```
No sensors            -> SPOOL (filament present but not advanced)
prep only             -> HUB (past prep, approaching hub)
prep + hub            -> TOOLHEAD (past hub, approaching toolhead)
prep + hub + toolhead -> NOZZLE (fully loaded)
```

See `path_segment_from_afc_sensors()` in `ams_types.h`.

### AFC-Specific Features

#### Hub Bowden Length

The bowden tube length from hub to toolhead is read from `AFC_hub.afc_bowden_length` and exposed as a slider in the device actions UI. Adjustable via `SET_BOWDEN_LENGTH LENGTH={mm}` G-code.

#### Per-Lane Stepper Fields

Each `AFC_stepper` object provides sensor states (`prep`, `load`, `loaded_to_hub`), buffer state (`buffer_status`), filament readiness (`filament_status`), and distance to hub (`dist_hub`). These are cached in the `LaneSensors` struct per lane (up to 16 lanes).

#### Buffer Objects

Each lane's `AFC_stepper`/`AFC_lane` object carries a `buffer_status` string naming the
current buffer operation (e.g. "Advancing"). The buffer's own health lives on separate
`AFC_buffer {name}` objects, discovered from the Klipper object list.

**Buffer health is per buffer, attributed to a unit.** `parse_afc_buffer()` accumulates
each frame into `buffer_health_[buffer_name]` - Moonraker forwards only changed keys, so
absent fields must leave the previous reading alone - and `apply_buffer_health_to_units()`
then derives `AmsUnit::buffer_health` from `buffer_health_` plus `buffer_lane_names_`,
resolving the first lane that maps to a unit. Two invariants make that work:

- **Unit objects must be parsed before anything that resolves a lane to a unit.**
  `handle_status_update()` runs `AFC_BoxTurtle`/`AFC_OpenAMS`/`AFC_vivid` first, then
  `AFC_buffer`. Those unit objects are what build the multi-unit layout
  (`parse_afc_unit_object` → `rebuild_unit_map_from_klipper` → `reorganize_slots`). With
  buffers parsed first, every buffer in the first frame resolved against the synthetic
  single unit `initialize_slots()` creates, and a five-unit rig put all five buffers on
  unit 0, each read-modify-writing the previous one's fields (bundle XGVDYEB5).
- **`reorganize_slots()` preserves nothing.** It clears `system_info_.units` and rebuilds
  every `AmsUnit` from scratch, so any field not re-derived in or after that function is
  gone. Sensors and topology are re-derived inside the loop; buffer health is re-derived
  after it by calling `apply_buffer_health_to_units()` again. A field whose only writer is
  a status-delta parser can never be "preserved" across a rebuild, because that parser may
  not run again for minutes - which is exactly why the reading used to go blank and stay
  blank until AFC happened to push a changed buffer field.

Attribution is logged only when it **changes** (`buffer_unit_attribution_`).
`apply_buffer_health_to_units()` runs on every frame carrying a buffer or a unit object, so
an unconditional line there is per-buffer-per-unit spam that pushes the incident window out
of the debug-bundle ring - and this is precisely the line that has to survive in a bundle,
since a buffer landing on the wrong unit is what it exists to show.

#### Global State

The `AFC` Klipper object provides global state: `current_lane`, `current_state`, `error_state`, `quiet_mode`, and `led_state`. These drive the UI status display and device action toggles.

#### Maintenance Mode

The device operations overlay exposes AFC maintenance actions:

| Action | G-code | Description |
|--------|--------|-------------|
| Test All Lanes | `AFC_TEST_LANES` | Run test sequence on all lanes |
| Change Blade | `AFC_CHANGE_BLADE` | Initiate blade change procedure |
| Park | `AFC_PARK` | Park the AFC system |
| Clean Brush | `AFC_BRUSH` | Run nozzle cleaning brush cycle |
| Reset Motor Timer | `AFC_RESET_MOTOR_TIME LANE={name}` | Reset motor run-time counter. The command is **per-lane**, so `execute_device_action("reset_motor")` loops every configured lane and sends one `AFC_RESET_MOTOR_TIME LANE=<name>` each, aborting on the first failure. No lanes configured is a `not_supported` error, not a silent success |

#### LED Toggle

The LED toggle sends `TURN_ON_AFC_LED` or `TURN_OFF_AFC_LED` based on the current `afc_led_state_`. The button label and icon dynamically reflect the current state.

#### Quiet Mode

Quiet mode reduces motor noise at the cost of speed. Toggled via `AFC_QUIET_MODE` G-code. The current state is tracked via `afc_quiet_mode_` from the `AFC.quiet_mode` printer object field.

#### Fault Clear vs Lane-Position Recovery

AFC does not have a genuine per-lane reset. What used to be called `reset_lane()` was
actually two unrelated operations that happened to share one name:

- **Fault clear** (`clear_fault(slot_index)`) is bookkeeping only — it never moves
  filament. AFC has no per-lane fault clear, so `slot_index` is ignored: it sends
  `RESET_FAILURE` followed by `AFC_CLEAR_MESSAGE` and arms a drain of
  `printer.AFC.message`, which is a FIFO queue — a second queued error is not visible
  until the first is popped, so a single clear only pops one entry. The drain runs until
  the queue empties rather than for a fixed count — nothing but `AFC_CLEAR_MESSAGE` ever
  pops an entry, so depth is a function of the whole session, not of the current fault.
- **Lane-position recovery** (`recover_lane_position(slot_index)`) is a physical
  retract: it sends `AFC_LANE_RESET LANE={name}` to pull filament stranded in the
  bowden back to its lane. AFC's firmware refuses this unless that lane's hub sensor
  is actually triggered, so `can_recover_lane_position(slot_index)` gates the UI on
  the live `AFC_hub.<hub>.state` field — **not** `AFC_stepper.<lane>.loaded_to_hub`,
  which is latched once at prep time and never updated afterward, so it cannot be
  used as a hub-occupancy signal.

Separately, **Reset** (`reset()`) and **Recover** (`recover()`) both send `AFC_RESET`
today — `reset()` after the usual busy-state preconditions, `recover()` skipping them
so it still works while the system is stuck. Neither homes the system; `AFC_HOME` is
not sent by either.

`can_recover_lane_position()` ends with `lane_name == active_load_lane_ &&
recovery_attribution_valid_unlocked()`, so the targeted per-lane action is offered only when AFC
itself names the lane. There is no all-lanes fallback: an unattributed strand deliberately offers
nothing per lane, and the route out is the sidebar Reset, which dispatches `AFC_RESET` and lets
AFC's own picker list the candidates. That picker's list is built from the firmware's view of its
hardware and is a better answer than anything derivable from a shared hub sensor.

**Known gaps in wrong-lane handling.** The wrong-lane diagnostic described under "Fields that do
not mean what they say" is understood but only partly acted on. Each of the following is verified
absent from `src/` and `include/`, and is recorded here so the reasoning is not re-derived:

- **The diagnostic is not classified.** `AmsBackendAfc::classify_error()` has exactly two
  AFC-owned branches: a toolhead-jam match, and a `ctx.is_paused && error_state_` catch-all
  titled "Filament System Error". `"'<lane>' failed to reset to hub, load switch became false
  during reset"` matches neither on its own. It therefore renders as the generic title when the
  print happens to be paused and AFC is in an error state, and otherwise falls through to the
  generic classifier untouched, since AFC raises it with `pause=False`. The one useful fact in
  the line, the name of a lane now ruled out and de-seated, never reaches the user.
- **There is no elimination set.** Nothing records which lanes have already returned the
  diagnostic for the strand currently in a hub, so the broken-fragment conclusion cannot be
  drawn and the UI keeps inviting another guess. Any such set has to be keyed per hub, because a
  multi-unit machine has independent hubs, and it has to clear when that hub's sensor goes false
  or one session's eliminations permanently suppress recovery for later strands.
- **There is no re-seat action.** Nothing undoes the retract that a wrong guess causes.
  `AmsBackend::clear_fault()` is bookkeeping and moves no filament, and
  `recover_lane_position()` sends `AFC_LANE_RESET`, which retracts *toward* the hub, the opposite
  direction from what a de-seated lane needs. The user is left to work the forward `LANE_MOVE`
  out themselves. A re-seat would have to advance in bounded steps and stop the moment
  `raw_load_state` returns true, never move a fixed distance, since overshoot pushes filament
  back at a hub that is still blocked. `LANE_MOVE`'s printing guard is not an obstacle for the
  common case: AFC_functions.py's `is_printing()` compares `print_stats.state` against
  `"printing"` only, so a paused print does not trip it and no `FORCE=1` is needed.

### Capabilities

| Feature | Supported | Editable |
|---------|-----------|----------|
| Endless Spool | `Available`, always `On` | `PerSlot` via `SET_RUNOUT` (see [Endless Spool](FILAMENT_MANAGEMENT.md#endless-spool-shared-model)) |
| Tool Mapping | Yes | Yes (via `SET_MAP`) |
| Bypass Mode | Yes | Hardware sensor (auto-detect on Box Turtle) |
| Spoolman | Yes | -- |
| Auto-Heat on Load | Yes | AFC uses `default_material_temps` from config |
| Dryer | No | -- |
| Device Actions | Yes | Setup, Speed, Toolhead, Maintenance, Hub & Cutter, Tip Forming, Purge & Wipe (see [Device Operations Overlay](FILAMENT_MANAGEMENT.md#device-operations-overlay)) |

`recovers_filament_on_resume()` is **not** overridden (default `false`), so an AFC runout
gets the dialog with manual **Load** kept prominent, because Resume alone does not re-feed.
`supports_per_tool_spool_assignment()` is not overridden either; it falls through to
`is_tool_changer(get_type())`, which is false for AFC.

**Homing delegation.** With `[AFC] auto_home: True` in AFC.cfg, AFC's
macros home-if-needed themselves. `AmsBackendAfc` surfaces this via
`AmsBackend::delegates_homing_to_printer()` (false until AFC.cfg has
loaded), and all three home-first prompt sites — the AMS sidebar, the
filament panel, and `ensure_homed_then()` — skip both the prompt and the
synthesized G28. Distinct from `filament_ops_self_home()`, which governs
paused-print refusal.

### AFC Version Reporting

`afc_version_` is **display and diagnostics only. Never gate behavior on it.** AFC has no
trustworthy version signal:

- The `afc-install` database namespace has been an orphan since AFC's `7d20db7` (mid-2025),
  so `detect_afc_version()` finds nothing on any current install.
- `AFC_VERSION` is a hand-bumped literal that sat at `1.1.37` through the whole v1.2.0 release.
- v1.2.0's own `get_status()` publishes no version key at all (upstream #807 is an open PR).
- A live BoxTurtle reported `"1.0.0"` while running v1.1.0.

Capabilities therefore come from **feature detection**, not comparison:

| Hook | What it inspects |
|------|------------------|
| `AmsBackendAfc::status_has_modern_fields()` | `filament_name` / `multi_color_hexes` / `initial_weight` on a lane status. All three ship from one `if not save_to_file:` block in `AFC_lane.get_status()`, so any one proves the whole block. Only meaningful on a **complete** status object, the subscription's first baseline frame; every later frame is a delta where an absent key means "unchanged". |
| `AmsBackendAfc::probe_feature_level()` | Queries one lane object directly (never a status frame) to obtain that baseline. |

**What a pre-v1.2.0 install actually loses depends on Spoolman, and the advisory says so.**
Only one of the three missing capabilities is unavailable by any other route:

| | pre-1.2.0 **with** Spoolman | pre-1.2.0 **without** |
|---|---|---|
| Filament name / vendor | **Available** — `SpoolmanManager::find_identity()` resolves them from the lane's `spool_id`, and `resolve_filament_label()` (`ams_state.cpp`) already consumes it. Bundle L53W5PKG rendered "LDO Industry Blue" on a pre-1.2.0 lane. | Needs the upgrade |
| Weights | Available — the Spoolman poll updates slot weights directly | Needs the upgrade |
| Multi-colour swatch | **Needs the upgrade.** The automatic lane sync only ever takes `multi_color_hexes` from `lane_data`; `apply_spool_to_slot()`, the one path that copies Spoolman's copy of it, serves manual external-spool assignment rather than the AFC lane refresh. | Needs the upgrade |

So the toast leads with multi-colour and qualifies names as needing Spoolman otherwise. It is
deliberately **not** branched on `is_spoolman_available()`: the feature probe and Spoolman
discovery both land during startup with no ordering guarantee, and the notice is latched to
fire once ever, so a mis-timed read would pin the wrong variant permanently.

The `AFC` / `lane_data` database query follows the same rule: `on_started()` calls
`query_lane_data()` **unconditionally**, because there is no reliable flag to gate on.
AFC's `lane_data_enabled` reports whether Moonraker has the (now unused) `[lane_data]`
section, not whether the namespace holds data; `send_lane_data()` writes regardless. A
live BoxTurtle on 2026-07-26 had `lane_data_enabled=false` with a fully populated
namespace. Lanes are initialized from `PrinterCapabilities` discovery first, so the query
only ever supplements colours / materials / spool ids; a missing namespace just errors and
the probe stays silent.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the shared architecture, slot metadata, and endless spool model.
