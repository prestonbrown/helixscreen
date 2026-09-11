# Filament Environment Zones (Developer Guide)

How HelixScreen models the heated and passive enclosures a filament system puts spools in: the
`EnvironmentZone` type, the three paths a zone is discovered by, the folding rules that turn
per-gate firmware states into one answer per zone, and the presentation split that decides
whether the user gets a tab strip or a list.

**Vocabulary.** **Box** is the word that ships in the UI. The live translation keys are
`View all boxes`, `Units: %d   Boxes: %d   Dryers: %d` and `Waiting for another box to finish.`
**Zone** is the internal type name and reaches no screen. Use "box" in anything a user reads,
and "zone" in code, comments, tests and this doc. There is no "zone" spelling in the catalogs to
fall back on.

**Two other things in this tree are called "humidity" and neither is this.** The home-screen
humidity widget (`src/ui/panel_widgets/humidity_widget.cpp`) reports the printer **chamber**
through `HumiditySensorManager` and binds `chamber_humidity_text`; it has no connection to the
filament system. `helix::config::EnvironmentConfig` (`include/environment_config.h`) is a generic
environment-**variable** parser and shares nothing but the word. Neither belongs in a search for
zone code.

---

## Architecture Overview

```
Backend hardware state
  |
  |  AmsBackend::get_environment_zones(unit)   (include/ams_backend.h)
  |    default: walk AmsUnit::environment / SlotInfo::environment + get_dryer_info()
  |    Happy Hare: derive_environment_zones() over its per-gate lists
  |    Mock: default walk, plus a simulated cap in "capped" mode
  v
std::vector<EnvironmentZone>          (include/ams_environment_zone.h)
  |    id / label / heater_name / sensor_name / gates / unit_index
  |    env (EnvironmentData)   +   dryer (DryerInfo)   +   state (ZoneDryingState)
  |
  |  select_zone_presentation(zones)  ->  Single | Tabs | List
  v
open_environment_for_unit(unit)       (src/ui/ui_ams_environment_overlay.cpp)
  |
  +-> AmsZoneOverviewOverlay  (List)     one zone_row per zone, drill into detail
  |     ui_xml/ams_zone_overview_overlay.xml + ui_xml/components/zone_row.xml
  |
  +-> AmsEnvironmentOverlay   (Tabs / Single)
        ui_xml/ams_environment_overlay.xml + ui_xml/components/zone_tab.xml
        readouts, material comfort strip, dryer controls, queued banner
          |
          v
        AmsBackend::start_drying / stop_drying (unit = the shown zone's unit_index)
```

The invariant: **a zone is what the UI selects, renders and controls.** Nothing above the
backend border asks which vendor produced a zone, how many units the printer has, or whether a
heater is shared or per-lane.

---

## Key Files

| File | Purpose |
|------|---------|
| `include/ams_environment_zone.h` | `EnvironmentZone`, `ZoneDryingState`, `ZonePresentation`, `kMaxZoneTabs`, and the four pure functions |
| `src/printer/ams_environment_zone.cpp` | `derive_environment_zones()`, `fold_zone_drying_state()`, `queue_zones_waiting_for_the_cap()`, `select_zone_presentation()` |
| `include/ui_zone_presentation.h` | `ZoneVerdict`, the humidity thresholds, `zone_display_label()`, `zone_slot_text()`, `zones_span_units()` |
| `src/ui/ui_zone_presentation.cpp` | Their implementations. LVGL-free and translation-free: callers pass translated words in |
| `include/ams_backend.h` | `get_environment_zones()` - the sixth dryer-family virtual - and the five it sits beside |
| `src/printer/ams_backend.cpp` | `AmsBackend::get_environment_zones` - the default walk every backend gets for free |
| `src/printer/ams_backend_happy_hare.cpp` | The one hardware override, plus `gates_suffix_for_unit()` and the `MMU_HEATER` command shapes |
| `src/printer/ams_backend_mock.cpp` | Zone-shape rigs for the six `HELIX_MOCK_AMS_ENV` modes; the only caller of `queue_zones_waiting_for_the_cap()` |
| `include/ui_ams_environment_overlay.h`, `src/ui/ui_ams_environment_overlay.cpp` | Detail overlay + tab selector, `open_environment_for_unit()`, the dryer control call sites |
| `include/ui_ams_zone_overview_overlay.h`, `src/ui/ui_ams_zone_overview_overlay.cpp` | The zone list, its status text and unit grouping |
| `ui_xml/ams_environment_overlay.xml`, `ui_xml/components/zone_tab.xml` | Detail layout and the segmented selector |
| `ui_xml/ams_zone_overview_overlay.xml`, `ui_xml/components/zone_row.xml` | List layout and one row |
| `tests/unit/test_ams_environment_zones.cpp` | The pure functions - `[ams][dryer][zones]` |
| `tests/unit/test_ams_environment_zone_source.cpp` | The default walk and backend overrides - `[ams][zones][source]` |
| `tests/unit/test_zone_presentation.cpp` | Verdicts, labels, presentation choice - `[ams][zones][presentation]` |
| `tests/unit/test_ams_environment_overlay_zones.cpp` | Overlay behavior over a zone set - `[ams][zones][overlay]` |

---

## The model

### Why the zone, and not the unit

Hardware does not divide environments the way it divides units:

- A **QuattroBox** is one heated enclosure over four gates. One heater, one sensor, four lanes.
- An **EMU** rig gives every lane its own sealed box with its own sensor, and possibly its own
  heater.
- A **QIDI** rig is one box per unit, so unit and enclosure happen to coincide.

The zone is the granularity all three share. A caller that reasoned in units would have to ask
which of the three shapes it was looking at before it could render anything; a caller that
reasons in zones renders the same way for all of them.

### `EnvironmentZone`

Declared in `include/ams_environment_zone.h`.

| Field | Meaning |
|-------|---------|
| `id` | Stable across polls, derived from the objects behind the zone. The overlays re-match a refreshed fetch against a held set by this |
| `label` | What the backend calls this zone. Empty is normal; `zone_display_label()` has fallbacks |
| `heater_name` | Klipper object, empty when the zone is passive |
| `sensor_name` | Klipper object, empty when nothing is monitored |
| `gates` | Global gate indices, ascending |
| `unit_index` | The unit the gates fall in, `-1` when they span several or match none |
| `env` | `EnvironmentData` - live temperature, humidity, `has_humidity` |
| `dryer` | `DryerInfo` - `supported == false` on a passive zone |
| `state` | `ZoneDryingState`, folded from the zone's gates |

**`env` and `dryer` are independent.** A passive EMU lane reports humidity with
`dryer.supported == false`: something to watch, nothing to drive. The reverse also happens - the
QIDI box has a heater and no humidity sensor, so `dryer.supported` is true while
`env.has_humidity` is false, and the detail card hides both the humidity readout and the material
comfort strip rather than printing a fabricated all-clear.

`unit_index == -1` is not a display problem, it is a **control** problem. A read that only shapes
a display value may treat it as "no data for this unit"; the start/stop call sites refuse rather
than guess which box to command (`src/ui/ui_ams_environment_overlay.cpp#acting_unit_index`, and
the two `unit < 0` branches in `on_start_stop_clicked`).

### `ZoneDryingState`

```
Idle = 0   Queued = 1   Active = 2   Complete = 3   Cancelled = 4
```

`Queued` is its own answer rather than a shade of `Active`. Firmware that caps simultaneous
heaters leaves later boxes waiting and says so per gate, and a waiting box drawn as idle reads as
a control that did nothing when the user pressed it. `Cancelled` and `Complete` likewise differ:
one answers "someone stopped it", the other "it finished".

The integer values are the contract with XML. `zone_tab.xml` picks one of four state icons by
comparing `env_zone_tab_state_${i}` against 1..4, and `ams_environment_overlay.xml` shows the
queued banner on `env_zone_state == 1` and hides the progress bar on the same value.

---

## Discovery: three paths

### `derive_environment_zones()` - the pure collapse

`src/printer/ams_environment_zone.cpp#derive_environment_zones` takes a per-gate heater list, a
per-gate sensor list, a gate count and (optionally) a per-gate firmware state list, and returns
zones. It is the whole vendor-independent half of discovery, with no locks, no backend and no
LVGL.

- **Gates naming the same (heater, sensor) pair are one zone.** That is what makes a shared
  enclosure come back as a single zone even though its one heater name has to be repeated once
  per gate. Pass a shared enclosure as lists repeating the one name and both the shared and the
  per-gate shape take this single path.
- **The key is `heater + '|' + sensor`.** Klipper object names are unique and cannot contain
  `|`, so the pair separates unambiguously and two boxes can never collide on the key.
- **Gates naming neither a heater nor a sensor are dropped.** They have no environment, so they
  form no zone rather than accumulating into an anonymous one.
- **Ordering is by lowest gate, for free.** The loop runs ascending, so first appearance is
  already lowest gate and no sort is needed to satisfy the ordering contract.
- **A short list reads as empty.** A backend sizes its lists to the lanes it configured, which
  need not reach every gate the system reports (`at_or_empty`).
- `dryer.supported` is set from `!heater.empty()`. Everything else on `dryer` is the caller's to
  fill in.
- Each zone's `state` is folded as part of the walk. The gate-to-zone mapping the fold needs
  exists only here; a caller holding the finished zone cannot rebuild it.

### The default backend walk

`src/printer/ams_backend.cpp#get_environment_zones` is what every backend gets without writing a
line. It walks `get_system_info().units`, and for each unit (or only the requested one when
`unit >= 0`):

- **Per-slot sensors win.** If any slot in the unit carries `SlotInfo::environment`, each such
  slot becomes its own zone, `gates = { first_slot_global_index + slot_index }`, `id` of the form
  `<unit name>#<global gate>`. Slots with no sensor of their own are skipped.
- **Otherwise the unit is one zone**, if it has `AmsUnit::environment` or a supported dryer.
  Its gates are the unit's whole slot range.
- **A unit with neither a sensor nor a dryer contributes nothing.**
- Every zone on the unit carries that unit's `DryerInfo`, so a unit-level dryer is claimed by
  each of its per-slot zones.
- The state is `dryer.active ? Active : Idle`. Only a running cycle is visible at this level;
  `Queued` needs a firmware report the base class does not receive.

The unit zone's `id` falls back to `"unit-" + index` when `AmsUnit::name` is empty. `AmsUnit::name`
carries no non-empty invariant, and two empty names would otherwise both produce `id == ""`,
which lets the overlays' id-matched refresh bind one zone's fresh values onto the other. (The
per-slot form needs no such fallback: the global gate number is already unique.)

**The default cannot express a per-lane heater.** It reads one `DryerInfo` per unit, so a rig
whose lanes each have their own heater has to override.

### Backend overrides

Two backends override `get_environment_zones()`. Everything else takes the default walk.

`AmsBackendHappyHare` (`src/printer/ams_backend_happy_hare.cpp#get_environment_zones`) snapshots
`filament_heaters_`, `environment_sensors_`, `gate_drying_states_` and `heater_temp_` under its
mutex, then:

- Falls straight through to `AmsBackend::get_environment_zones(unit)` when both per-gate lists
  are empty. The scalar `filament_heater` / `environment_sensor` form names one heater and one
  sensor for the whole machine, which the generic walk already models correctly.
- Otherwise runs `derive_environment_zones()` over the lists and fills in what the pure collapse
  cannot know: `unit_index` from `unit_index_for_gate()`, and the `DryerInfo` behind each heater
  (with `active` taken per zone from its folded state, and `current_temp_c` from that heater's
  own live reading).
- Filters to the requested unit last, so every zone has a resolved `unit_index` by the time the
  filter reads it.

`AmsBackendMock` (`src/printer/ams_backend_mock.cpp#get_environment_zones`) runs the default walk
and, in `"capped"` mode only, applies `queue_zones_waiting_for_the_cap()`. Its six
`HELIX_MOCK_AMS_ENV` modes exist to rig one zone shape each - see
[MOCK_ENVIRONMENT_VARIABLES.md](MOCK_ENVIRONMENT_VARIABLES.md) § `HELIX_MOCK_AMS_ENV` for the
mode-to-presentation table.

---

## Folding and the cap

### `fold_zone_drying_state()`

Reduces a zone's gates to one state. Precedence is **Active > Queued > Cancelled > Complete >
Idle**: live states outrank finished ones because they describe what is happening now, and
`Cancelled` outranks `Complete` so a partly-cancelled zone does not report as cleanly finished.

That precedence lives in its own table (`precedence()` in the anonymous namespace), deliberately
not the enum's own ordering - the enum's integer values are a storage and XML contract and stay
put.

Gates outside `per_gate_state`, negative gates, and states the firmware does not define all read
as `Idle`.

`state_from_firmware()` accepts **both** `"canceled"` and `"cancelled"`. Happy Hare emits the
one-l spelling while its own header documents the two-l one; taking both keeps an upstream
correction from silently reading as `Idle`.

### `queue_zones_waiting_for_the_cap()`

**Real backends do not run this.** On hardware, `Queued` arrives from the firmware's own per-gate
drying state and reaches a zone through `fold_zone_drying_state()` - Happy Hare writes the literal
string `"queued"` into `gate_drying_states_`, and the fold does the rest. Nothing in the backend
layer infers a cap.

The helper exists so the mock can **simulate** one. It marks every zone that can dry but is not
the one running as `Queued`, and is a no-op when nothing is `Active` - idle heaters with no cycle
running are not waiting on anything. `AmsBackendMock` calls it in `"capped"` mode and is the only
production caller; `tests/unit/test_ams_environment_zones.cpp` is the other.

Reach for it only if a real backend turns up whose sole signal is a global "something is drying"
flag with no per-gate detail behind it. None of the ones here are in that position.

The overlay names the blocker rather than saying "waiting". When the shown zone is `Queued`, it
searches everything the backend reports - not just the zones on screen - for the `Active` one,
because the cap is a printer-wide resource and a badge opened on the queued unit alone never has
the running unit's zone in its own set.

---

## Presentation

### `select_zone_presentation()`

```
zones.size() <= 1                       -> Single
any zone disagrees on dryer.supported   -> List
zones.size() > kMaxZoneTabs (4)         -> List
otherwise                               -> Tabs
```

Count alone would be the wrong rule. Zones that differ in whether they can dry are not
interchangeable, and a tab hides a heated box and a passive box behind a selection the user has
to make before they can see that the two differ at all. So any capability split goes to the list
whatever the count, and only the count question is left for `kMaxZoneTabs` - which is a width
constraint: above four, a segmented selector stops fitting the narrow breakpoints.

`open_environment_for_unit()` (`src/ui/ui_ams_environment_overlay.cpp#open_environment_for_unit`)
is the one caller. The branch lives there rather than inside either overlay, so neither overlay
has to decide that the other one should have been opened.

`AmsEnvironmentOverlay::show_zone()` re-applies the tab ceiling itself: `with_selector_` is only
true when the caller asked for a selector **and** the set is 2..`kMaxZoneTabs`, so an oversized
set handed in with `with_selector = true` still renders without tabs rather than overflowing the
strip.

### `ui_zone_presentation.h`

Pure classification for display. The overview row, the selector tab and the detail header all
need the same answers about a zone and render them differently, so what is shared is the
**decision** and never the drawing. LVGL-free and translation-free: callers pass translated
words in.

**`ZoneVerdict`** - `Ok` / `Marginal` / `TooHumid` / `Unknown`, with the bands as shared
constants so the row and the detail strip cannot disagree:

| Verdict | Band | Reading |
|---------|------|---------|
| `Ok` | `<= kZoneHumidityOkMax` (35%) | Dry enough for everything the user is likely storing |
| `Marginal` | `<= kZoneHumidityMarginalMax` (55%) | Fine for PLA, not for ASA or ABS |
| `TooHumid` | above that | Above threshold for everything |
| `Unknown` | `!env.has_humidity` | No sensor, so no verdict |

A zone with no humidity sensor is `Unknown` and never `Ok`. An unmeasured box is not a dry box,
and the passive rigs this serves exist to answer exactly that question.

**The verdict is not the word on the row.** `zone_status()` classifies a row once and returns
both halves of what it renders - a `ZoneStatusKind` for the word and a `ZoneVerdict` for the
color band - so a row cannot name one state while coloring for another:

1. `dryer.active` -> `Drying`, severity `Ok`. An active cycle is the machine already acting on
   the humidity, so a wet reading mid-cycle is the cycle working rather than something to flag.
2. `!dryer.supported` -> `Passive`, severity from `zone_verdict()`. A box nobody can drive is
   exactly the one whose color is the only signal the row gives, so it keeps its verdict.
3. Otherwise `Verdict`, severity from `zone_verdict()`.

`zone_status_text()` in the overview (`src/ui/ui_ams_zone_overview_overlay.cpp`) turns the kind
into a word: "Drying", "passive", or the verdict itself as "OK" / "Marginal" / "Too humid", with
`Unknown` rendering `--`. The severity feeds `verdict_pool_` and the `bind_style_if_eq` rules in
`zone_row.xml`, which style only `Marginal` (1) and `TooHumid` (2). So a heated box with no
humidity sensor shows a neutral `--`, and a damp passive box shows "passive" in the danger color
because that is the one thing the row can still tell you.

**`zone_display_label(zone, unit_word, slot_word, type_name)`** - three cases, in order:

1. A zone the backend named uses that name.
2. A single-gate zone with no name is one slot, `slot_word + " " + (gate + 1)` (one-based on the
   global gate index).
3. Anything else is `type_name + " " + unit_word + " " + (unit_index + 1)`.

Case 1 matters on rigs that name their boxes ("Box Turtle 1", "Night Owl"): an ordinal against
the system type cannot tell one from another on the very screen where you pick between them.

**`zone_slot_text(zone, plural_word, singular_word)`** - the slots a zone covers as the user
reads them: one-based, contracted to a range (`"Slots 1-4"`, or `"Slot 3"` when first == last).
Slots are contiguous within a zone on every shape we serve, so a range is honest. An empty zone
gets an empty string rather than a range of nothing.

**`zones_span_units(zones)`** - whether a set covers more than one unit, which is when the
overview's grouping headers earn their space.

### The two overlays

`AmsEnvironmentOverlay` is the detail view: readouts, the material comfort strip, dryer controls
and the queued banner, with an optional tab strip above them. It holds the zone set it was shown
with (`zones_`) and a selected index.

- `select_zone(index)` changes only values. The widget tree stays exactly as it was built; the
  tab pools and the detail subjects are re-published.
- `refresh()` re-fetches **every** zone on the printer and matches by `id` into the held set.
  Re-deriving by a single unit would narrow a cross-unit set to one, and `on_activate()` calls
  `refresh()` in the tick the overlay opens. A zone that has gone away keeps its last values
  rather than shifting the view under whoever is looking at it.
- Its own `ams_env_overlay_humidity_visible` subject gates the humidity readout and the comfort
  strip. Binding a per-unit indicator subject here would mean picking a unit index at
  XML-authoring time, which answers for whichever unit that index names and not the one on
  screen.
- Three `IndexedSubjectPool`s back the tab strip (`env_zone_tab_label/state/active`), reclaimed
  in `on_ui_destroyed()` rather than `on_deactivate()`.
- The cross-unit affordance ("View all boxes") appears only when the printer reports more zones
  than this view is showing **and** no overview already sits beneath the overlay - reached from
  the list, Back is already the way there, so a second route would stack a duplicate.

`AmsZoneOverviewOverlay` is the list: one `zone_row` per zone, a subtitle counting units, boxes
and dryers, and a unit grouping header on the first row of each unit (suppressed entirely when
the set sits inside one unit). Its status column is `zone_status()`, whose three-way order is
above. It re-pulls and re-matches by `id` in `on_activate()`, because a row that was drilled
into can go stale while the list sits paused underneath.

Both overlays register their repeated component (`zone_tab`, `zone_row`) before their own XML is
parsed, and both self-register their overlay XML so they are safe to open without first visiting
a host panel.

---

## Backend matrix

| Backend | `get_environment_zones()` | Zones produced |
|---------|---------------------------|----------------|
| Happy Hare | **Override** | Heated per-gate zones from `filament_heaters` / `environment_sensors`; passive zones for gates with a sensor and no heater; falls back to the default walk on the scalar form |
| Mock | **Override** (default walk, plus a simulated cap in `"capped"` mode) | Whatever `HELIX_MOCK_AMS_ENV` rigs: passive, per-slot, EMU, mixed or capped |
| QIDI Box | Default walk | One heated zone per box - per-unit `environment` plus per-unit `DryerInfo` |
| ACE | Default walk | One heated zone. Temperature always; humidity only on firmware variants that publish a top-level `humidity` |
| CFS | Default walk | Passive zones, one per box. The backend never touches `DryerInfo`, so `dryer.supported` stays false on every zone |
| AFC | Default walk | None. No environment data, no dryer |
| Tool Changer | Default walk | None |
| AD5X IFS | Default walk | None |
| Snapmaker U1 | Default walk | None |

A backend in the "None" rows costs nothing: the default walk finds neither a sensor nor a dryer
on any unit and returns an empty vector, which `open_environment_for_unit()` renders as the
overview's empty state.

---

## Happy Hare specifics

Happy Hare is the only shipping backend that overrides `get_environment_zones()`, and the only
one with per-gate firmware state to fold. (The mock overrides too, to rig shapes no real backend
here produces.)

**Zones are per-gate; the dryer clock is not.** `target_temp_c`, `duration_min` and
`remaining_min` stay global on every zone the override produces. Happy Hare tracks one setpoint
and one end-of-cycle clock, not one per box, so a per-zone value for those would be invented
rather than reported. Only `current_temp_c` and `active` are resolved per zone - the first from
that heater's own live reading, the second from the zone's folded state. The temperature
**ceiling** is likewise one global `heater_max_temp`, so a Happy Hare rig's zones share a
ceiling even though `env_temp_range` is published per zone.

**`gates_suffix_for_unit(unit)`** builds the `GATES=` selector appended to every `MMU_HEATER`
command:

- `unit < 0`, or a single-unit MMU, returns the empty string, so Happy Hare targets all non-empty
  gates - the whole-MMU behavior.
- Otherwise it returns `" GATES=g,g,g"` listing **every** gate on that unit, empty gates
  included. Per-gate occupancy filtering is a refinement we cannot verify: nobody here owns an
  EMU rig.

**The commands**, all in `src/printer/ams_backend_happy_hare.cpp`:

| Operation | Command |
|-----------|---------|
| `start_drying` | `MMU_HEATER DRY=1 TEMP=<t> TIMER=<minutes>` + gates suffix |
| `stop_drying` | `MMU_HEATER STOP=1` + gates suffix |
| `update_drying`, temperature only | `MMU_HEATER TEMP=<t>` + gates suffix |
| `update_drying`, any duration change | `stop_drying()` then `start_drying()` |

`TIMER` is minutes (float, `minval=0`), which is why the overlay's duration field is a minutes
field and sub-hour cycles are valid. There is no `FAN` parameter, so `fan_pct` is ignored.

`TIMER` is read only on the `DRY=1` path and `DRY=1` mid-cycle is refused, so moving the clock
means a fresh cycle. That is exactly what `DryerInfo::supports_live_temp = true` /
`supports_live_duration = false` encode, and both are set the moment a heater is named - on the
scalar form and the per-gate form alike.

---

## Per-backend `update_drying` differences

Three shapes sit behind one screen. `update_drying()` is not currently reached from any UI call
site, so these are the contract for whoever wires the first one.

| Backend | Behavior |
|---------|-----------|
| Happy Hare | Temperature alone re-sends the setpoint and leaves the cycle and its timer alone. Any duration change is a stop then a start, carrying the running target when only the duration moved |
| ACE | Always a stop then a start. When the duration was not specified it restarts on `remaining_min` rather than `duration_min`, so a temperature change does not silently hand back time already served |
| Every other backend | The `AmsBackend` default: `NOT_SUPPORTED` |

QIDI overrides `get_dryer_info`, `start_drying` and `stop_drying` but **not** `update_drying`, so
its adjustments fall to that default. Its input validation lives in `start_drying`, which range-
checks temperature and duration against the box's own reported limits and returns an `AmsError`
carrying a suggestion naming those limits ("Set temperature between N°C and M°C") rather than a
bare failure.

`DryerInfo::supports_live_temp` and `supports_live_duration` both default to **false** so a
backend that has not been checked against real hardware forces the stop-and-restart path rather
than silently dropping an adjustment.

---

## Adding environment support to a new backend

`get_environment_zones()` is the sixth virtual in the dryer family, beside `get_dryer_info()`,
`start_drying()`, `stop_drying()`, `update_drying()` and `get_drying_presets()`. Unlike the other
five it is **not** a "not supported" stub - it has a working default, so most backends should
never override it.

**To get a passive zone from the default walk**, publish an `EnvironmentData` on the unit
(`AmsUnit::environment`) or on each measured slot (`SlotInfo::environment`). Set `has_humidity`
only when a real humidity channel exists; leaving it false with `humidity_pct == 0` is what makes
the verdict `Unknown` instead of a fabricated `Ok`.

**To get a heated zone**, additionally return `DryerInfo{.supported = true, ...}` from
`get_dryer_info(unit)` and implement `start_drying()` / `stop_drying()`. Fill `min_temp_c`,
`max_temp_c` and `max_duration_min` from the hardware - the overlay clamps the keypad and the
Start command to exactly those, so a wrong ceiling shows the user a number the command will
silently replace. Set `supports_live_temp` / `supports_live_duration` only where the hardware
has been checked.

**Override `get_environment_zones()` only when the default cannot express your hardware.** In
practice that means a per-lane heater, or a firmware that reports a per-gate drying state you
want folded. If you do override, `derive_environment_zones()` does the collapse for you: build a
per-gate heater list and a per-gate sensor list (repeating one name across the gates of a shared
enclosure), then fill in `unit_index`, `label` and the `DryerInfo` fields the pure function
cannot know.

`AmsState::sync_dryer_from_backend()` mirrors one unit's `DryerInfo` into the scalar `dryer_*`
subjects other surfaces read; the detail overlay retargets that mirror to the shown zone's unit
via `AmsState::set_dryer_mirror_unit()`. A zone that spans units leaves the mirror on its last
target rather than following a bad guess. No other `AmsState` wiring is needed.

---

## Boundaries

- **Control is per unit, not per zone.** `start_drying` / `stop_drying` take a unit index, and on
  Happy Hare that becomes `GATES=` over every gate on that unit. On an EMU rig the zones are
  per-lane for display while a Start on any one of them commands the whole unit. Independently
  drying a subset of gates, per-gate countdowns, and the `HUMIDITY=` termination target are open
  in **#1026**.
- **Per-gate occupancy filtering is unwritten and unverifiable here.** `gates_suffix_for_unit()`
  lists empty gates along with loaded ones. We own no EMU rig; the per-gate paths are unit-tested
  against synthetic status frames only.
- **Nothing infers a concurrency cap.** `Queued` is only ever a firmware report folded by
  `fold_zone_drying_state()`. `queue_zones_waiting_for_the_cap()` exists to let the mock simulate
  a cap and has no production caller outside it.
