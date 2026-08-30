# Filament Management (Developer Guide)

Multi-material system support in HelixScreen: architecture, backend implementations, mock testing, and extension guide.

**User-facing doc**: [docs/user/USER_GUIDE.md](../user/USER_GUIDE.md) (filament panel usage, slot operations, troubleshooting)

---

## Architecture Overview

HelixScreen uses a backend abstraction layer to support multiple multi-filament and multi-tool systems through a single UI. The `AmsBackend` interface hides all backend-specific protocols and exposes a uniform API for the UI layer.

```
                         ┌─────────────┐
                         │  AmsState   │  Singleton LVGL subject bridge
                         │ (ams_state) │  Thread-safe subject updates
                         └──────┬──────┘
                                │ owns backends_[] vector
              ┌─────────────────┼─────────────────┐
              ▼                 ▼                  ▼
       Backend 0 (primary)   Backend 1        Backend N
       flat slot subjects    BackendSlot-      BackendSlot-
       (backward compat)     Subjects          Subjects
              │                 │                  │
    ┌─────────▼─────────┐      │                  │
    │     AmsBackend     │  Abstract interface     │
    │  (ams_backend.h)   │  Factory: create() / create_mock()
    └─────────┬──────────┘                         │
     ┌────────┼─────────┬───────────┬──────────────┘
     ▼        ▼         ▼           ▼           ▼           ▼           ▼
  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────┐ ┌──────────┐ ┌────────┐ ┌──────────┐
  │Happy   │ │  AFC   │ │  ACE   │ │  Tool    │ │ AD5X IFS │ │  CFS   │ │  Mock    │
  │Hare    │ │Backend │ │Backend │ │ Changer  │ │ Backend  │ │Backend │ │ Backend  │
  └────────┘ └────────┘ └────────┘ └──────────┘ └──────────┘ └────────┘ └──────────┘
       │          │          │           │            │           │            │
  Moonraker  Moonraker   REST API   Moonraker   Moonraker  Moonraker    In-memory
  WebSocket  WebSocket   Polling    WebSocket   WebSocket  WebSocket    simulation

                         ┌─────────────┐
                         │  ToolState  │  Singleton: tool abstraction
                         │(tool_state) │  Maps tools ↔ AMS backends
                         └─────────────┘
```

### Key Files

| File | Purpose |
|------|---------|
| `include/ams_backend.h` | Abstract interface with factory methods |
| `include/ams_types.h` | Shared types: `AmsType`, `SlotInfo`, `AmsAction`, `PathTopology`, etc. |
| `include/ams_error.h` | Error types with user-friendly messages |
| `include/ams_state.h` | LVGL subject bridge (singleton) |
| `include/slot_registry.h` | SlotRegistry: single source of truth for per-slot state |
| `src/printer/slot_registry.cpp` | SlotRegistry implementation (name/index mapping, reorganize, tool map) |
| `include/ams_backend_happy_hare.h` | Happy Hare MMU implementation |
| `include/ams_backend_afc.h` | AFC (Armored Turtle / Box Turtle) implementation |
| `include/ams_backend_ace.h` | ACE (Anycubic ACE Pro) implementation |
| `include/ams_backend_toolchanger.h` | Physical tool changer (viesturz/klipper-toolchanger) |
| `include/ams_backend_ad5x_ifs.h` | FlashForge AD5X IFS (Intelligent Filament Switching) |
| `include/ams_backend_cfs.h` | Creality Filament System (K2 series, RS-485) |
| `include/ams_backend_mock.h` | Mock backend for development and testing |
| `src/printer/ams_backend.cpp` | Factory methods, plus the shared endless-spool validation / reset / eligibility base |
| `src/printer/ams_endless_spool.cpp` | Backend-agnostic endless-spool model: restriction text, group builders, the one group-to-edge projection ([§ Endless Spool](#endless-spool-shared-model)) |
| `include/printer_discovery.h` | Hardware detection from Klipper object list |
| `include/tool_state.h` | Tool abstraction: `ToolInfo`, `ToolState` singleton, tool-backend mapping |
| `include/printer_temperature_state.h` | `ExtruderInfo` struct, multi-extruder dynamic subjects |
| `include/preflight_validator.h` | Pre-print tool-vs-slot validation; takes bypass as a required input ([§ Bypass suppresses the pre-print filament gates](#bypass-suppresses-the-pre-print-filament-gates)) |
| `include/print_start_checks.h` | Print-start gate pipeline pure core (context, rules, ordered gate list) |
| `include/ui_ams_context_menu.h` | Slot context menu (load, unload, edit, spoolman) |
| `include/ui_ams_device_operations_overlay.h` | Device operations overlay (home, recover, bypass, etc.) |

### Data Flow

1. **Discovery**: `PrinterDiscovery::parse_objects()` scans Klipper's `printer.objects.list` for `mmu`, `AFC`, `toolchanger`, `ace`, `AFC_stepper lane*`, `AFC_hub *`, `tool T*`, and `filament_switch_sensor _ifs_port_sensor_*` objects.
2. **Backend Creation**: `AmsState::init_backend_from_hardware()` calls `AmsBackend::create()` with the detected `AmsType` and Moonraker dependencies.
3. **Slot State**: Each backend stores per-slot state in its `SlotRegistry` instance (`slots_`), which provides indexed access, name lookup, and multi-unit reorganization. Moonraker status updates write to the registry under the backend's mutex.
4. **State Sync**: Backend emits events (`STATE_CHANGED`, `SLOT_CHANGED`, etc.) which `AmsState` translates to LVGL subject updates.
5. **UI Binding**: XML widgets bind to subjects (`ams_type`, `ams_action`, `current_slot`, `slots_version`, etc.) for reactive updates.

### SlotRegistry (Per-Slot State)

Each backend owns a `helix::printer::SlotRegistry` instance (`slots_`) that serves as the single source of truth for all per-slot indexed state. Before SlotRegistry, backends maintained parallel vectors (`lane_names_`, `lane_sensors_`, `gate_sensors_`, etc.) that had to be kept in sync manually -- a frequent source of index mismatch bugs.

**What SlotRegistry manages:**
- Slot names and bidirectional name-to-index lookup
- Per-slot sensor states (prep, load, loaded_to_hub, tool_loaded)
- Per-slot error and buffer health
- Per-slot filament weight tracking
- Tool-to-slot mapping
- Multi-unit reorganization (preserving slot data when unit topology changes)

**How backends use it:**

```cpp
// Initialize (once, during startup or first data arrival)
slots_.initialize("AFC Box Turtle", lane_names);   // AFC
slots_.initialize("Happy Hare MMU", gate_count);    // Happy Hare

// Read state
int idx = slots_.index_of("lane3");         // Name -> index
std::string name = slots_.name_of(2);       // Index -> name
const auto* entry = slots_.get(idx);        // Read-only access
auto info = slots_.build_slot_info(idx);    // Build SlotInfo for API

// Write state (under backend mutex)
auto* entry = slots_.get_mut(idx);
entry->sensors.prep = true;
entry->info.color_rgb = 0xFF0000;

// Multi-unit reorganization (AFC multi-unit topology changes)
slots_.reorganize(unit_lane_map);           // Preserves slot data across layout changes
```

**Key design decisions:**
- SlotRegistry does NOT hold a mutex -- the owning backend's mutex protects all access
- `build_slot_info()` constructs a `SlotInfo` snapshot, avoiding shared mutable state
- `reorganize()` takes an ordered vector of unit/lane pairs — caller controls unit ordering
- Slot names remain backend-specific ("lane1" for AFC, "Gate 0" for Happy Hare) -- SlotRegistry is agnostic

### Per-Slot Load Authority

Two `AmsBackend` predicates answer "is *this* slot loaded?" and everything the user can
tap about a slot is derived from them: the active-lane highlight, the Load/Unload gate on
the filament panel, and the context menu's Unload/Eject/Recover choice.

| Predicate | Question | Default |
|-----------|----------|---------|
| `slot_is_actively_loaded(i)` | Firmware considers this slot seated at the toolhead | see below |
| `slot_has_filament_at_toolhead(i)` | A per-slot toolhead sensor is tripped | `false` |
| `can_unload_from_toolhead(i)` | Offer Unload (and suppress Load) | `status == LOADED`, or `is_present()` on PARALLEL |
| `slot_unloads_to_toolhead(i, hint)` | The unload is a heated toolhead unload, not a cold eject | `hint` |

`slot_is_actively_loaded()` has **two** rules, chosen by
`has_per_slot_loaded_authority()` (default `false`):

- **`false`** — derive from the aggregate pair `get_current_slot() + is_filament_loaded()`.
- **`true`** — read the slot's own `SlotStatus::LOADED`.

The aggregate rule is only as good as our tracking of a single active-slot pointer. When
that pointer names the wrong slot or lags a toolchange, every affordance above inherits
the wrong answer — that was #1194, which surfaced as Load staying enabled on an AFC lane
the firmware had already seated (#1183) and as Recover being offered on a lane that only
reached the hub.

**Opting a backend in is not free.** The per-slot rule believes `get_slot_info(i).status`,
so a backend that never stamps `LOADED` on its seated slot would report *every* slot
unloaded and blank the active-lane highlight. Before flipping a backend to `true`, confirm
its parse sets `SlotStatus::LOADED` on the seated slot on **every** path that also sets the
aggregate, and add a test that fails if it stops.

| Backend | Authority | Basis |
|---------|-----------|-------|
| AFC | `true` | `AFC_stepper.<lane>.tool_loaded` (#1194) |
| Snapmaker | overrides outright | returns `status == LOADED` verbatim |
| AD5X IFS | `true` | firmware active-lane pointer + head sensor (#1199) |
| QIDI Box | `true` | `save_variables slot<N> == 2` (#1199) |
| CFS | `true` | `T{n}.filament` letter + toolhead switch (#1199) |
| ACE | `true` | arbitrated seated slot, stamped every parse path (#1199) |
| Happy Hare | `false` | `mmu.gate` / `mmu.filament` *are* the firmware truth |
| Toolchanger | `false` | `toolchanger.tool_number` *is* the firmware truth; no per-tool filament signal exists |

Every backend that opted in derives its stamp from the same inputs the aggregate pair is
assigned from, so the per-slot and aggregate rules cannot disagree. That is deliberate: it
makes "believing the per-slot status blanks the highlight" structurally impossible rather
than merely tested against. The value of opting in is not divergence-fixing but that
`can_unload_from_toolhead()` — which keys on `status == LOADED` for serial topologies —
finally reads true on a seated slot.

AFC's opt-in rests on `AFC_stepper.<lane>.tool_loaded`, which upstream's `set_loaded()` /
`set_unloaded()` assign in lockstep with `AFC.current_load` and
`AFC_extruder.lane_loaded`. Note that AFC's lane `status == "Loaded"` means *loaded to
hub*, not to the toolhead — only `tool_loaded` answers the toolhead question, which is why
`parse_afc_stepper` maps `"Loaded"` to `AVAILABLE`.

AD5X IFS derives its stamp from the same two inputs `system_info_.filament_loaded` is
assigned from, so the two cannot disagree — but only after dropping the lane's own port
sensor from the condition. A runout clears `port_presence_` while the filament that lane
fed is still at the toolhead (#995, the state `can_unload_from_toolhead()` keeps the unload
gate open for); requiring the port sensor demoted the lane to `EMPTY` at exactly the moment
the user needs to recover it.

QIDI Box is the opposite shape: `slot<N> == 2` is the Box's own per-slot statement and
needs no active-slot pointer, while the aggregate pair is written *only* from
`last_load_slot` — so a Box that never writes that variable reported nothing loaded at all.
`parse_save_variables()` reconciles the stamp against the aggregate at the end of every
pass, since the `slot<N>` loop runs before the `last_load_slot` block and would otherwise
demote the seated slot on a payload that repeated one without the other.

Happy Hare deliberately stays on the aggregate rule. `mmu.gate` and `mmu.filament` are
Happy Hare's own values parsed verbatim from one object, so the aggregate is already
firmware truth; `gate_status` carries fill state, not seating, so the per-gate stamp is
derived *from* the aggregate and believing it back would only add staleness. It would also
drop the highlight on a gate that ran out (`gate_status 0`) while its filament is still at
the toolhead. The stamp itself is re-derived on every `printer.mmu` frame
(`refresh_gate_statuses_locked()`) because `gate_status`, `gate` and `filament` arrive in
independent deltas — a toolchange typically carries the latter two alone.

Toolchanger stays on the aggregate rule too, and for a reason none of the others have: it
carries **no filament signal at all**. `get_slot_filament_segment()` returns `NOZZLE`
unconditionally, no per-tool switch is read, and `is_filament_loaded()` is nothing more than
`tool_number >= 0`. The only fact the parse can state is *which tool is on the carriage*,
which is single-valued — precisely what the aggregate pair encodes, assigned verbatim from
klipper-toolchanger's own `toolchanger.tool_number`. Being `PARALLEL` does not change that:
the topology describes independent filament paths, but this backend cannot see filament in
any of them, and its load/unload verbs are `SELECT_TOOL` / `UNSELECT_TOOL` — mount and
unmount, of which exactly one tool at a time is the subject.

`tool <name>.mounted` is emphatically *not* that authority. It arrives on a separate
Moonraker object from the one that writes the aggregate, and an all-tools-mounted payload is
a shape HelixScreen emits itself in mock mode (`moonraker_client_mock_objects.cpp` gives
every `tool T<n>` `mounted: true`). The parse used to write `mounted ? LOADED : AVAILABLE`
straight into `slot.status` from that object alone, so such a payload marked every tool
`LOADED` — the exact state that would make an opt-in report every tool as the active one.
`refresh_slot_statuses_locked()` now derives the stamp from the carriage tool on every parse
path, so the two writers cannot disagree; opting in afterwards would be safe but pointless,
since the stamp is derived *from* the aggregate.

The `PARALLEL` arm of `can_unload_from_toolhead()` is the part that did need fixing.
`is_present()` is true for every toolchanger slot forever — a slot here is a physical
toolhead, never `EMPTY` or `UNKNOWN` — so it read true everywhere. Through
`decide_can_load()`'s inverted `!toolhead_unload` factor that left **Load disabled on every
tool**, and through `decide_unload_mode()` it offered Unload (`UNSELECT_TOOL T=<n>`) on
tools parked in their docks. The backend overrides it with `slot_index == current_tool`.
`slot_unloads_to_toolhead()` stays on the base rule: an unmount *is* a toolhead operation,
and with no lane eject or lane recovery a docked tool correctly lands on
`UnloadMode::Unavailable`.

CFS earns it differently, and the difference is worth naming: its firmware publishes no
per-slot loaded flag at all. The seated bay is the intersection of two signals that arrive
on separate notifications — the per-unit `T{n}.filament` letter ("A".."D") naming the
engaged lane, and `filament_switch_sensor filament_sensor.filament_detected` at the
toolhead. `handle_status_update()` derives `SlotStatus::LOADED` from that pair at the end
of every frame, so the per-slot status can never disagree with the aggregate rather than
being independently authoritative. That still buys the real fix: before it, CFS wrote only
`AVAILABLE`/`EMPTY`, so `can_unload_from_toolhead()` — `status == LOADED` on a HUB
backend — was false on every CFS slot and the panel never offered Unload (#1199). The
stamp is applied even over a bay firmware calls `EMPTY`: a spool pulled while still
threaded leaves filament at the toolhead the user has to be able to unload. Removing it
restores the status the parse wrote, not a guessed `AVAILABLE`.

ACE derives it the same way, and its opt-in is a case study in *not* believing a firmware
string. The per-slot `"loaded"` token that its `slot_status_from_string()` maps to
`AVAILABLE` exists only in the community ValgACE dialect, where it sits in the same
enumeration as `"available"` and `"ready"` — the same slot-local trap as AFC's `"Loaded"`
meaning loaded-to-hub. Native Anycubic GoKlipper has no per-slot `"loaded"` at all; its
vocabulary is `empty`/`ready`/`preload`/`running`/`runout` and it answers the seated
question with the separate top-level `current_filament` (`"<unitId>-<localIndex>"`). So the
vocabulary map is left alone. Instead `apply_seated_slot_stamp_locked()` stamps whichever
slot the parse *arbitrated* to — from the ValgACE `"loaded"` scan, `loaded_slot`, or
`current_filament`, in that precedence — and a HUB backend has exactly one. The REST
fallback needs both ends of the stamp because `/status` owns `loaded_slot` while `/slots`
owns the slot vector: without it, each `/slots` poll would demote the seated slot and
report a spurious change every 500 ms.

`slot_has_filament_at_toolhead()` stays at its `false` default unless the sensor genuinely
exists *and* is attributable to one slot. AFC's `AFC_extruder` carries
`tool_start_status` / `tool_end_status` plus the `lane_loaded` that owns them; a trip with
no owning lane reads `false` rather than being blamed on an arbitrary lane.

### Threading Model

All Moonraker/libhv callbacks arrive on a background thread. Backends update internal state under mutex, then `AmsState` posts subject updates to the LVGL thread via `lv_async_call()`. The UI never directly accesses backend state.

---

## Multi-Backend Architecture

Some printers have multiple filament management systems simultaneously (e.g., a tool changer where each toolhead has its own AFC unit). AmsState supports multiple concurrent backends via a `backends_` vector that replaces the former single `backend_` pointer.

### Backend Storage

```cpp
// AmsState private members
std::vector<std::unique_ptr<AmsBackend>> backends_;       // All backends
std::vector<BackendSlotSubjects> secondary_slot_subjects_; // Per-backend subjects (index 1+)
```

- **Primary backend (index 0)** uses the existing flat `slot_colors_[MAX_SLOTS]` and `slot_statuses_[MAX_SLOTS]` subject arrays. This preserves backward compatibility with all existing XML bindings and single-backend printers.
- **Secondary backends (index 1+)** each get a `BackendSlotSubjects` struct with dynamically allocated `lv_subject_t` vectors:

```cpp
struct BackendSlotSubjects {
    std::vector<lv_subject_t> colors;
    std::vector<lv_subject_t> statuses;
    std::vector<lv_subject_t> fills;  // int: fill percent 0-100, -1 = unknown
    int slot_count = 0;
    // Lifetime token shared by every subject in this struct: these subjects are
    // DYNAMIC (destroyed in deinit() on backend rediscovery), so any observer
    // bound to them MUST hold a copy of this token. deinit() invalidates it.
    SubjectLifetime lifetime;
    void init(int count);   // Allocate and init subjects
    void deinit();          // Deinit subjects, invalidate lifetime
};
```

### Discovery of Multiple Systems

`PrinterDiscovery::parse_objects()` collects all detected AMS/filament systems into a `detected_ams_systems_` vector of `DetectedAmsSystem` structs:

```cpp
struct DetectedAmsSystem {
    AmsType type = AmsType::NONE;
    std::string name;  // "Happy Hare", "AFC", "Tool Changer"
};
```

A printer with both a tool changer and an AFC unit will have two entries. The `init_backends_from_hardware()` method iterates this list and creates a backend for each detected system.

### Backend Selection

Two new subjects track backend selection:

| Subject | Type | Description |
|---------|------|-------------|
| `backend_count_` | int | Number of registered backends |
| `active_backend_` | int | Index of the currently selected backend |

The AMS panel UI shows a backend selector when `backend_count > 1`, allowing users to switch between systems. API:

- `active_backend_index()` -- returns the currently selected backend index
- `set_active_backend(int)` -- switches the active backend (bounds-checked)

### Per-Backend Event Routing

When backends are added via `add_backend()`, each backend's event callback captures its backend index at registration time:

```
Backend 0 emits STATE_CHANGED  -->  on_backend_event(0, "STATE_CHANGED", ...)
Backend 1 emits SLOT_CHANGED   -->  on_backend_event(1, "SLOT_CHANGED", ...)
```

The `on_backend_event()` handler routes to `sync_backend(int)` or `update_slot_for_backend(int, int)` which update the correct set of subjects. All subject updates are posted via `ui_queue_update()` for thread safety.

### Per-Backend Subject Access

Two-argument overloads of `get_slot_color_subject()` and `get_slot_status_subject()` route to the correct subject storage:

```cpp
// Backend 0: flat arrays (backward compat)
lv_subject_t* get_slot_color_subject(0, slot_index);  // -> slot_colors_[slot_index]

// Backend 1+: per-backend storage
lv_subject_t* get_slot_color_subject(1, slot_index);  // -> secondary_slot_subjects_[0].colors[slot_index]
```

### Tool-Backend Integration

The `ToolState` singleton (see `tool_state.h`) maps tools to specific AMS backends via two fields on `ToolInfo`:

```cpp
struct ToolInfo {
    int backend_index = -1;  // Which AMS backend feeds this tool (-1 = direct drive)
    int backend_slot = -1;   // Fixed slot in that backend (-1 = any/dynamic)
    // ... other fields
};
```

- `backend_index = -1` means the tool uses direct-drive filament (no AMS).
- `backend_index >= 0` maps the tool to a specific AMS backend. For example, on a dual-toolhead printer where each head has its own AFC unit, T0 might map to backend 0 and T1 to backend 1.
- `backend_slot` pins the tool to a specific slot within that backend, or `-1` for dynamic slot selection (e.g., Happy Hare tool-to-gate mapping).

`ToolState` and `AmsState` coordinate through `PrinterDiscovery`: tools are discovered from `tool T*` Klipper objects, and the mapping between tools and AMS backends is established during `init_backends_from_hardware()`.

---

## Persistence of slot metadata

HelixScreen writes user-edited slot metadata (brand, spool name, Spoolman
link, weights, color/material) to the Moonraker `lane_data` namespace,
following the AFC-originated convention. This is the same namespace
OrcaSlicer 2.3.2+ reads for filament sync, so user edits automatically
flow to the slicer on Moonraker-based printers. The flow is one-directional:
HelixScreen writes, OrcaSlicer reads (it never writes `lane_data` back), so
"round-trip" here means the user's edit reaching the slicer's filament
panel — not a slicer-to-printer write.

- **Wire-format spec (public):** [`../specs/filament_slots.md`](../specs/filament_slots.md)
- **Implementation notes (internal):** [`FILAMENT_SLOT_METADATA.md`](FILAMENT_SLOT_METADATA.md)

### Material names as G-code parameter values

Some backends persist the material by putting the name on a G-code line rather
than writing a database record, so it has to survive Klipper's argument parser.
Two validators exist and they are **not** interchangeable:

| Helper | Charset | Use for |
|--------|---------|---------|
| `IMoonrakerAPI::is_safe_gcode_param()` (`is_safe_identifier()`) | `[A-Za-z0-9_ ]` | Lane names, macro names, object names - identifiers Klipper itself constrains |
| `IMoonrakerAPI::is_safe_material_param()` | alphanumeric plus `+ - _ . ( ) /` and space | Material names |

`is_safe_material_param()` is deliberately the wider of the two. `PLA+`, `PA6-CF`,
`PETG-CF`, `PC-ABS` and `Silk PLA` all ship in `include/filament_database.h`, so
gating a material send on the identifier charset made the app offer materials its
own persistence layer refused to send: the write was dropped, a warning nobody
reads was logged, and the save reported success.

What it still rejects, and why each one matters: CR/LF (Moonraker runs a
`printer.gcode.script` body line by line, so a newline is the one real
multi-command vector), `;` and `#` (Klipper's comment characters - they truncate
the rest of the command silently), `*` (checksum separator), `"` and `\` (both
meaningful to the quoting pass below, so letting them through would let a value
escape its own quotes), `=`, control characters, and non-ASCII bytes.

**Quoting is mandatory, not cosmetic.** Klipper parses an extended command's
arguments - anything that is not a bare `G`/`M` code, which `SET_MATERIAL` and
`MMU_GATE_MAP` both are - with `shlex.shlex(posix=True, whitespace_split=True,
commenters="#;")`, then splits each token on its first `=` (verified in Kalico
klippy/gcode.py, the extended-command parameter parser). An unquoted `MATERIAL=Silk PLA`
therefore arrives as two tokens, the second with no `=` anywhere in it, and
Klipper answers "Malformed command" - the value never reaches firmware at all, so
a space in a material name was never merely dropped, it broke the whole command.
`IMoonrakerAPI::gcode_param_value()` quotes a value only when it contains
whitespace, so every value that worked before goes out byte-for-byte unchanged.
Its precondition is a validator that rejects `"` and `\`; pair it with
`is_safe_material_param()`, never use it on unvalidated input.

Per backend:

| Backend | Command | Protection |
|---------|---------|------------|
| AFC | `SET_MATERIAL LANE={lane} MATERIAL={material}` | `is_safe_material_param()` + `gcode_param_value()` |
| Happy Hare | `MMU_GATE_MAP GATE={n} … MATERIAL={material}` | same pair |
| CFS | `_BOX_SLOT_SET SLOT={n} MATERIAL=… BRAND=… NAME=…` | local `quote_gcode_param()` (`ams_backend_cfs.cpp`) - always quotes, escapes `\` and `"`, folds CR/LF to a space. It escapes rather than rejects because `BRAND`/`NAME` are free-form strings arriving from RFID, not values the user picked from a list |
| AD5X IFS | `_IFS_VARS types="['PLA', …]"` | `build_type_list_value()` emits an already-quoted Python list literal. This is a mirror to the lessWaste/bambufy plugin, not the primary persistence path |

Backends that persist through `FilamentSlotOverrideStore` (IFS, Snapmaker, ACE)
or through a numeric id (QIDI Box's `SAVE_VARIABLE VARIABLE=filament_slot{n}`)
never put the string on a G-code line and need neither helper.

The same shlex mechanism bites `SDCARD_PRINT_FILE FILENAME=` on the power-loss
resume path, which needs a *blocklist* rather than an allowlist because a slicer
filename legitimately carries characters no material name would - see
[`POWER_LOSS_RECOVERY.md`](POWER_LOSS_RECOVERY.md). Do not merge the two.

**A rejected material is an error, not a silent skip.** `AmsBackendAfc::set_slot_info()`
and `AmsBackendHappyHare::set_slot_info()` issue every *other* write first, then
return `AmsResult::COMMAND_FAILED` naming the material. The user keeps the color,
weight and Spoolman link and is told which part did not land; returning success
for a write that never happened is what made this invisible for so long.

### Gcode Tool Remapper

`helix::GcodeToolRemapper` (`src/rendering/gcode_tool_remapper.cpp`, `include/gcode_tool_remapper.h`) rewrites the tool parameters inside a sliced G-code file when a logical tool must print from a different physical head. It stays generic — pure text in, changed lines out, no `AmsBackend`, Moonraker, or LVGL dependency — because the problem lives at the file level, not the backend level: a slicer bakes tool numbers into three command families, and a remap that moves one but not the others runs the right filament at the wrong temperature:

1. Prestart macros — `SM_PRINT_AUTO_FEED` / `SM_PRINT_EXTRUDER_PREHEAT` / `SM_PRINT_FLOW_CALIBRATE` with `EXTRUDER=<n>` (the Snapmaker U1 family the remapper was built for)
2. Body — bare `T<n>` toolchange lines
3. Temperatures — `M104` / `M109` lines carrying a `T<n>` token

Each line is transformed from its ORIGINAL text in a single pass, so a swap (`1<->2`) cannot chain; comment lines never match. Two entry points split tests from production: `apply_to_string()` returns the whole rewritten file; `build_line_replacements()` returns only the changed lines, which is what the streaming path consumes.

Backends are consumers, not implementers: `AmsBackend::get_remap_strategy()` routes (`include/ams_backend.h`). `Native` (Happy Hare, AFC, CFS, AD5X IFS, Tool Changer, QIDI Box) and `SnapmakerNative` (the U1 emits firmware-native `print_task_config` gcode — no file rewrite) never reach the remapper. `GcodeRewrite` is the generic fallback for firmware with no internal routing table: no backend ships it today — ACE will adopt it once its `ACE_CHANGE_TOOL` family is implemented and validated, and until then ACE stays `None` (`include/ams_backend_ace.h`). When it runs, the production path is `PrintSelectPanel::apply_remap()` -> `PrintPreparationManager::modify_and_print_with_remap()` (`src/ui/ui_print_preparation_manager.cpp`): download the file, compute the changed lines, apply them through the streaming `GCodeFileModifier`, and print the modified copy via the HelixPrint plugin under the ORIGINAL filename — an identity remap prints the original directly, no copy. The strategy is guarded in `open_remap_modal()` (`include/ui_panel_print_select.h`): GcodeRewrite without the plugin shows an actionable alert instead of a picker whose Done would silently fail. `tests/unit/test_gcode_tool_remapper.cpp` pins the three families and the swap collision-safety.

### AD5X IFS material/color reconcile (locks, insert, #1065/#1071)

Native ZMOD has **no per-port RFID or spool identity** — the only per-lane
signals are a color (`ffmColor`) and a material type (`ffmType`), read via
`GET_ZCOLOR` / `IFS_STATUS`, plus a presence bit from `IFS_STATUS Ports`. Because
there's no identity, a lane's `FilamentSlotOverride` bundles two conceptually
different kinds of data, and they follow different rules:

- **Display data** — `color_rgb` + `material`. Should track what's physically loaded.
- **Identity data** — `spoolman_id`, `brand`, `spool_name`, weights. Attached by
  the user; firmware knows nothing about it. Retained across an eject/insert
  cycle so a re-inserted same spool keeps its assignment (**#1071**).

The `user_locked_color` / `user_locked_material` flags gate whether the
`OverwriteAlways` auto-mirror (`mirror_firmware_to_lane_data`) may refresh the
display fields from firmware truth. A locked field is **never** auto-refreshed —
this exists to protect a deliberate user choice from the AD5X post-print
`FFMInfo` revert, which re-emits the *old* type after a print (**#965**).

**The reconcile detectors** live in `ams_backend_ad5x_ifs.cpp`:
`check_external_color_change` and `check_external_type_change`, both called from
`update_slot_from_state`. Each keeps a per-slot baseline (`last_firmware_color_`
/ `last_firmware_material_`); a baseline≠observed delta on a *present* lane fires
`sync_override_to_firmware_locked`, which runs the auto-mirror.

Two footguns this area has repeatedly hit (fixed in #1065; keep them fixed):

1. **Baseline swallow on presence lag.** On modern ZMOD the firmware
   color/type can surface one parse frame *before* `IFS_STATUS Ports` flips the
   slot present. The detectors must **hold** the baseline while the slot reads
   not-present (advancing it only for a genuine empty-lane/`""` eject reading).
   If the baseline advances during the lag, the delta is consumed while the sync
   is skipped, and when presence catches up there's no delta left — the change is
   swallowed (classic symptom: *color updated on screen, material stuck*).

2. **Insert can't clear a lock, so the display sticks.** The only thing that
   clears a lock is an external `CHANGE_ZCOLOR` in the gcode stream (**#981**,
   emitted by the ZMOD COLOR macro / LCD). A **physical insert emits no
   `CHANGE_ZCOLOR`**, so a lane whose material was locked — either by a menu
   type-set (`set_slot_info`) or by the pessimistic `!material.empty()` load
   default in `from_lane_data_record` — keeps painting the *previous* spool's
   type after a new spool goes in. This is why "change type via the COLOR macro"
   worked while "insert a new spool and change its type" did not.

   Fix: `unlock_auto_tracked_override_on_insert_locked()` runs on the
   empty→present edge (both `apply_zcolor_result` presence sites). It drops the
   two lock flags **only when the lane has no real Spoolman binding**
   (`spoolman_id <= 0`) — an auto-tracked material is a guess that a fresh insert
   invalidates, so firmware truth should win. `brand` / `spool_name` /
   `spoolman_id` / weights are never touched, so a retained binding still paints.

   **Why gate on the Spoolman binding.** On insert we can't tell "same spool back
   after maintenance" from "brand-new spool" — there's no identity signal. The
   two want opposite things for the identity fields, so we don't guess: a lane
    with a deliberate Spoolman binding is left entirely alone (**#1071** retains
    it), and only auto-tracked lanes (no binding) refresh material/color from
    firmware. A lane whose firmware later reports a *different* spool id drops
    the binding entirely (`merge_override` re-bind rule, **#1281**) — the
    residual stale-binding case is now only the no-signal backends. On AFC and
    Happy Hare the eject signal itself is user-configurable ("Keep Spool Info
    on Eject", AMS Management overlay; default on).

    **Precedence: firmware retention beats the toggle.** With AFC's per-lane
    `remember_spool = true` on every lane, AFC itself repopulates lanes on
    eject and keeps reporting the spool id — so neither the merge's eject rule
    nor the re-assert push ever fires and the toggle has no observable effect
    in either position. Rather than let it silently lie, the overlay disables
    it with a note: `AmsBackend::printer_retains_spool_info()` (ALL-lane
    semantics; a mixed config leaves the toggle governing the `false` lanes)
    drives the `ams_device_ops_printer_retains_spool_info` subject, which the row's
    `disabled` prop and the note bind to (#1281 follow-up).

### OrcaSlicer compatibility — by backend

All HelixScreen-managed AMS backends write the AFC-standard `lane_data`
record on edit, so every one of them round-trips to OrcaSlicer with no
additional configuration. **Verified against OrcaSlicer upstream/main
(post-2.4.0-beta nightly)**, source MoonrakerPrinterAgent.cpp
`fetch_moonraker_filament_data()`.

| Backend | Writer | Key style | How OrcaSlicer picks it up |
|---------|--------|-----------|----------------------------|
| AD5X IFS | HelixScreen (`FilamentSlotOverrideStore`) | `laneN` (1-based) | `lane_data` namespace |
| Snapmaker U1 | HelixScreen (`FilamentSlotOverrideStore`) | `T<n>` (0-based) — tool changer | `lane_data` namespace |
| ACE (Anycubic ACE Pro) | HelixScreen (`FilamentSlotOverrideStore`) | `laneN` (1-based) | `lane_data` namespace |
| CFS (Creality K2) | HelixScreen (`FilamentSlotOverrideStore`) | `laneN` (1-based) | `lane_data` namespace |
| AFC / Box Turtle | AFC's own Klipper plugin | `T(n)` per mapping (virtual-tools firmware, #832); `laneN` (1-based) before | `lane_data` namespace (AFC is the originator) |
| Happy Hare | Happy Hare's own Klipper plugin (components/mmu_server.py `push_lane_data`) | `laneN` (1-based) | `lane_data` namespace — Orca prefers it over the live `mmu` object |
| Tool Changer | (not applicable — no per-slot metadata) | — | N/A |

The key style is derived from the AMS type (`lane_key_style_for(get_type())`),
not hardcoded per backend: tool changers (Snapmaker U1, generic
klipper-toolchanger) write `T<n>`, filament systems write `laneN`. See the
interoperability subsection below.

IFS, Snapmaker, ACE, and CFS share the `FilamentSlotOverrideStore`
infrastructure and publish to `lane_data`; AFC and Happy Hare each write
`lane_data` via their own Klipper plugins. **HelixScreen never writes
`lane_data` for the AFC or Happy Hare backends** — those plugins own their
records, and HelixScreen's AFC/HH backends route user edits through G-code
(`SET_COLOR`/`SET_MATERIAL`, `MMU_GATE_MAP`) only. The reason is stronger than
clobber risk: AFC deletes every key in the namespace on each Klipper boot
(AFC.py `delete_lane_data()`) and rebuilds it lane by lane as PREP advances,
so a record we wrote there would vanish on reboot, and a *read* landing in that
window sees a partial namespace. Treat `lane_data` as neither durable nor
atomic for AFC. User overrides go to a private namespace instead (#1158).
(Earlier docs said HH reached Orca solely via the live `mmu` Klipper object.
That is outdated: HH's `push_lane_data` now writes the namespace directly and
Orca prefers it; the `mmu` object is the fallback.)

**AFC moved its outer key from `laneN` to `T<n>`** (shipped on upstream `DEV`
2026-08-16, Klipper-Add-On #832 — the 1.3 release line). One lane can answer
to several `T` commands under virtual tools, which a lane-name key cannot
express, so each mapping gets its own record and the record carries no lane
identity. Two HelixScreen changes followed: the live AFC reader
(`parse_lane_data` in `ams_backend_afc.cpp`) joins `T(n)` keys through the
firmware-asserted tool mapping (parking a payload that lands before any
mapping and replaying it once one arrives — the DB query is one-shot), and
`reset_tool_mappings()` sends `AFC_RESET_MAPPING` on firmware that reports
`multiple_tool_mapping` (#832 deregistered the old name). The override-store
reader needed nothing: it is key-agnostic and HelixScreen authors no records
on AFC printers. Full analysis, including the new key-space overlap with
Mainsail, is in
[`../specs/filament_slots.md` § "Shipped: AFC moved from `laneN` to
`T<n>`"](../specs/filament_slots.md#shipped-afc-moved-from-lanen-to-tn-virtual-tools-firmware).

#### Schema lineage and what Orca actually matches on

- **AFC pioneered the base schema** — keys `color` / `material` / `bed_temp` /
  `nozzle_temp` / `scan_time` / `td` / `lane` / `spool_id`, DB key 1-based
  (`lane1`…), inner `lane` field 0-based. Older AFC emits **no** vendor field.
- **Happy Hare extended it** with `vendor_name`, `name`, and `filament_id`
  (`push_lane_data`, authored by `ammmze`). HH's `filament_id` is a **Spoolman**
  DB id, not an OrcaSlicer preset id.
- **AFC then adopted HH's two keys verbatim**, plus `initial_weight`
  ([AFCProject/AFC-Klipper-Add-On#833](https://github.com/AFCProject/AFC-Klipper-Add-On/pull/833),
  closing #808). `lane_data` is a shared namespace, so the deliberate call was
  one spelling per value across backends rather than each writer using its own
  attribute names. **`lane_data` and AFC's `get_status` therefore disagree on
  purpose**: status reports the same brand as `spool_vendor` (and the name as
  `filament_name`), because that dict mirrors `AFCLane`'s attributes and is
  AFC's own surface, not a shared one.
- **OrcaSlicer reads only `lane` / `material` / `color` / `bed_temp` /
  `nozzle_temp`**, and matches a lane to a filament preset **by the `material`
  type string alone** (`filament_id_by_type`, falling back to generic
  OrcaFilamentLibrary ids like `OGFL99`). It ignores `vendor_name` and
  `filament_id` today, and never writes `lane_data` back. So brand has no
  effect on the slicer's preset pick — "Generic PLA" and "Elegoo PLA+" both
  resolve to a generic PLA preset. Emit canonical material strings (`PLA`,
  `PETG`, `ABS`…); marketing names won't match. (Vendor-aware matching for the
  generic Moonraker sync is proposed upstream on
  `feat/moonraker-vendor-aware-filament-match`; until it lands, assume the
  type-only behaviour above.)

#### Two-string identity: `material` (Orca wire) vs `helix_material` (HelixScreen)

A lane's display type and its Orca match string are **not the same string**.
HelixScreen stores the precise identity the user chose — `ASA-GF`, `PLA Silk`,
`PPS-CF` — but Orca can only match a type string its own library carries. Writing
the precise string verbatim is what caused the original bug: OrcaSlicer resolves an
unmatched `material` to **the first library preset whose name contains "PLA"**
(Preset.cpp:3300), and because that bogus id then resolves cleanly it
**short-circuits the similarity search** that would otherwise have found a closer
type (PresetBundle.cpp:3320-3346). So `ASA-GF` synced as *Generic PLA* — PLA
temperatures on a glass-filled ASA — while the color came through untouched.
(Verified against the pinned OrcaSlicer source, not secondhand docs.)

`to_lane_data_record()` therefore emits two keys:

- **`material`** — the Orca wire string, derived by `filament::orca_match_type()`
  (`filament_variants.cpp`): explicit `orca_type_overrides` entry → the type itself
  if Orca's library carries it → `extract_base_material()` base polymer if the
  library carries *that* → otherwise **omitted entirely** (better an empty tray in
  Orca than a confident wrong match). The library-type set and the override table
  are generated into `assets/filaments.json` (`orca_library_types`,
  `orca_type_overrides`) by `scripts/import_orca_filaments.py`.
- **`helix_material`** — the precise identity, written unconditionally. Orca ignores
  it; HelixScreen's reader (`from_lane_data_record()`) prefers it over `material`,
  so the on-device AMS screen still shows `ASA-GF` even though the same lane synced
  to Orca as `ASA`.

**Healing existing installs.** Records written before this split carry an
unmatchable `material` and no `helix_material`. `load_blocking()` rewrites
helix-authored records — proven by a `helix_locked_*` key, never a foreign
co-author's — in place: `helix_material` = the precise identity, `material` =
`orca_match_type()` of it (or dropped if nothing matches). Mutating in place
preserves `scan_time` and any co-author's fields. The heal is gated on
`orca_tables_available()` — a missing or stale `assets/filaments.json` would
otherwise strip `material` from every lane in one pass — and it re-runs on **drift**
(a later library regeneration that drops a type we used to match), converging once
`orca_match_type(material) == material`. The tables are pre-warmed on the main
thread at startup (`filament::warm_orca_tables()`, called from
`SubjectInitializer`) so the first match never parses the asset on a WebSocket
background thread.

#### Vendor / product-name aliases (`vendor_name` / `name`)

Brand and product name have **one agreed spelling in `lane_data`** — HH's
`vendor_name` / `name`, which AFC adopted in #833 — plus the legacy keys we
emitted before that settled:

| Value | `lane_data` (AFC + HH) | HelixScreen legacy | AFC `get_status` only |
|-------|------------------------|--------------------|-----------------------|
| Brand | `vendor_name` | `vendor` | `spool_vendor` |
| Product name | `name` | `spool_name` | `filament_name` |

The third column is **not** a `lane_data` spelling — it is what AFC's status
dict calls the same values, and `AmsBackendAfc` meets it only on the status
path.

HelixScreen's writer (`to_lane_data_record()` in
`filament_slot_override_store.cpp`) emits **both** the shared key and our legacy
one per value, so a reader of this namespace finds our overrides under the key
it already looks for: a zero-cost hedge, since every consumer ignores unknown
keys. Its reader (`from_lane_data_record()`) prefers our key and falls back to
the shared one, so round-trips of our own records stay exact while foreign
records still read. `AmsBackendAfc::read_vendor()` runs a wider ladder because
it serves both surfaces: `vendor_name` (lane_data), `spool_vendor` (status),
then `vendor` / `brand` defensively. **Do not add
a HelixScreen-side `filament_id` resolver:** Orca reads the field from nowhere,
there is no deterministic (vendor, material) → Orca `setting_id` catalog (the
ids number in the hundreds and churn across releases), and we do not ship a
forked OrcaSlicer that could add the read path.

### `lane_data` interoperability (outer-key contract)

`lane_data` is a **shared namespace with multiple writers and multiple
readers**. The authoritative, source-verified contract lives in the public
spec — [`../specs/filament_slots.md` § "Interoperating readers and
writers"](../specs/filament_slots.md#8-interoperating-readers-and-writers).
Read that section before touching key formatting, the load filter, or the
migration. The summary:

- **Writers and their key style**: HelixScreen (`T<n>` on tool changers,
  `laneN` otherwise), AFC (`T(n)` per mapping since the virtual-tools firmware,
  `laneN` before), Happy Hare (`laneN`), Mainsail #2510
  (`T<n>` on Spoolman + tool changer).
- **Readers**: OrcaSlicer is **key-opaque** (reads the inner `lane` field, never
  the outer key — MoonrakerPrinterAgent.cpp:780), requires the inner `lane`
  to be a JSON **string**, and does **no deduplication**. HelixScreen's reader
  is **key-agnostic** and prefers the canonical key for its own style on
  duplicates (`load_blocking` in `filament_slot_override_store.cpp`).
- **The collision hazard is not a wrong outer key** — it is the **same inner
  `lane` under two different outer keys**, which Orca renders as two trays for
  one slot. A tool changer converges on `T<n>` (matching Mainsail) and migrates
  its own stale `laneN` records to `T<n>` on load to avoid exactly this.

**Lesson (recorded inline so we don't re-derive it):** verify wire-format
claims against the tools' **source**, not their PR or release text. Mainsail
#2510's companion PR broadened an AFC `map` TypeScript type to `string[]`,
which looked like a schema change but was speculative — upstream AFC still
emits a scalar `map`. Confirming against MoonrakerPrinterAgent.cpp (Orca) and
the AFC plugin source, not the PR descriptions, is what kept this change
correct. Cite exact source lines in the spec so a future reader re-verifies the
same way.

---

## Filament Catalog (`filaments.json`)

HelixScreen ships a single generated catalog of **branded** filament products —
`assets/filaments.json` — that unifies what used to be two disconnected data
sources: the generic material-**type** table in `include/filament_database.h`
(PLA, ABS, PETG, … — untouched, still `constexpr`, still the source of
physical truth) and the old CFS-only assets/cfs_materials.json (renamed and
superseded). The catalog is generic infrastructure — not CFS-specific — even
though the CFS backend is currently its only consumer.

### Schema

Each entry in the `filaments` array is one branded product:

```json
{
  "id": "creality-cr-abs",     // stable slug; user overrides target this
  "brand": "Creality",
  "name": "CR-ABS",            // display = "{brand} {name}"
  "type": "ABS",               // resolves to a filament_database.h type
  "nozzle": 260,                // recommended nozzle temp (°C)
  "bed": 60,                    // recommended bed temp (°C)
  "nozzle_min": 240,            // OPTIONAL — only emitted when it differs from the type's range
  "nozzle_max": 280,            // OPTIONAL
  "density": 1.24,              // OPTIONAL — else inherit type
  "codes": { "cfs": "07001" },  // OPTIONAL, open scheme-keyed map (see below)
  "orca_id": "OGF...",          // provenance: OrcaSlicer filament_id (NOT a CFS code)
  "source": "orca"              // provenance: orca | cfs-seed | user
}
```

Most Orca-derived entries are **thin** — just `id, brand, name, type, nozzle,
bed, source`. Everything else (nozzle range, bed if unset, chamber temp, dry
temp/time, `compat_group`, density) **inherits from the base `type`**, the
same way a product does at runtime (see `EffectiveFilament` below). A field is
only written to the file when it *differs* from what the type would already
supply — keeps the catalog small and keeps regen diffs meaningful.

### Type inheritance

Products don't duplicate physical data — they carry deltas over their base
material type:

```
EffectiveFilament = filament::find_material(product.type)   // type defaults
                     ◀ product's own JSON fields              // product overrides
                     ◀ user overlay entry (same id), if any    // user overrides
```

`nozzle_min` / `nozzle_max` / `bed` / `density` / `chamber_temp_c` /
`dry_temp_c` / `dry_time_min` / `compat_group` all come from the type unless
the product JSON explicitly sets them. A product whose `type` string doesn't
resolve in `filament_database.h` (an Orca material HelixScreen doesn't map
yet) is only valid if it's self-sufficient — i.e. the importer emitted
explicit `nozzle_min`/`nozzle_max` for it directly; see the data-integrity
lint in `tests/unit/test_filaments_data.cpp`.

### The `codes` map (scheme-keyed, open-ended)

Hardware/RFID codes live in a scheme-keyed map so multiple, possibly-colliding
namespaces coexist and a new decoder drops in with **no schema change**:

| scheme | meaning | status |
|--------|---------|--------|
| `cfs` | Creality Filament System numeric hardware code | **live** — decodes CFS box-reported material codes |
| `rfid` | future vendor-neutral / generic RFID standard | reserved (not populated) |
| `snapmaker` | Snapmaker U1 `filament_sku` | reserved — U1 exposes a real per-material SKU (`print_task_config.filament_sku`), but no seed table exists yet |
| `bambu` | Bambu `filament_id`/RFID (`GFA00`…) | reserved — could auto-derive from `orca_id` later |

Each scheme is indexed independently (`by_code[scheme][code] -> product`), so
a `cfs` code can never collide with an `rfid` or `snapmaker` code that happens
to share the same digits.

### `FilamentCatalog` — transient, on-demand access layer

`include/filament_catalog.h` / `src/printer/filament_catalog.cpp`. **No
`::instance()` singleton** — unlike the rest of the printer-state layer, this
is a scoped value type: construct it, query it, let it fall out of scope. Idle
RAM footprint is zero; nothing is parsed until something asks for it.

```cpp
// Small slice: only products carrying a code in one scheme. Used by CFS decode —
// built once at the top of a box-state enrichment pass, destroyed at the end.
auto cat = FilamentCatalog::load_codes("cfs");
const EffectiveFilament* mat = cat.resolve_code("cfs", mat_id);

// Whole catalog + user overlay merged in. For a future offline picker (Phase 2);
// transient for the lifetime of a picker session, not resident otherwise.
auto full = FilamentCatalog::load_full();
```

Other query methods: `resolve_id(id)`, `products_for_type(type)`,
`products_for_brand(brand)`, `all_brands()`, `all_products()`. The tradeoff
(accepted): CFS re-parses its small coded slice on every poll rather than
caching — worth it for zero idle RAM on memory-constrained devices (AD5M,
K1). A debounce cache is a future escape hatch only if profiling ever shows
the re-parse cost matters.

**Today's only consumer** is `AmsBackendCfs` (`src/printer/ams_backend_cfs.cpp`),
which replaced the old `CfsMaterialDb` JSON table with
`FilamentCatalog::load_codes("cfs").resolve_code("cfs", mat_id)`. Behavior is
unchanged for CFS users — same slot fields get filled — the catalog is just
richer and no longer CFS-gated. A user-editable overlay
(config/user_filaments.json, read-write, merged by `load_with_overlay()`)
exists at the load-path level today; the UI to author it is Phase 3 (out of
scope here).

### User overlay format

config/user_filaments.json is the on-disk shape for everything a user
contributes about filaments — product entries (override/add to the built-in
catalog) and Orca-type hints (so a display name not in our snapshot resolves
correctly in OrcaSlicer without waiting for a HelixScreen release). The file
does not exist by default; it is created the first time the Phase 3 edit UI
writes a change. The on-disk format is an internal concern — users interact
through the UI and never see JSON.

```jsonc
{
  "filaments": [
    // Product entries: override built-ins by id, or add new ones. Merged by
    // FilamentCatalog::load_with_overlay(). See the "effective filament"
    // structure in include/filament_catalog.h for the full field set.
    {"id": "polymaker-abs-pro", "nozzle_min": 265, "nozzle_max": 285, "source": "user"},
    {"id": "acme-custom-petg", "brand": "Acme", "name": "Custom PETG",
     "type": "PETG", "nozzle": 240, "source": "user"}
  ],
  "orca_type_map": {
    // Helix display name -> Orca wire string. Single map by design — users
    // contribute *overrides*, not library-type membership, which stays a
    // shipped-asset concept (assets/filaments.json's `orca_library_types`).
    // Resolution at orca_match_type() step 1 makes user entries always win
    // over shipped ones. An empty-string value is the documented "suppress"
    // case: emit nothing for this type rather than a wrong match. See the
    // spec's § Drift for the safety rationale.
    "PLA-BioTough": "PLA",
    "WeirdResin": "",
    "CustomASA": "ASA"
  }
}
```

The two sections are independent: a user can carry only `filaments`, only
`orca_type_map`, both, or neither. The shipped asset
(`assets/filaments.json`) keeps its own split between `orca_library_types`
(list) and `orca_type_overrides` (map) because the importer generates those
two differently — that distinction does not propagate to the user overlay.

**Wiring.** `SubjectInitializer::init_core_and_state()` warms the Orca tables
on the main thread (`warm_orca_tables()`), then immediately calls
`FilamentCatalog::load_user_orca_type_map()` and feeds the result to
`filament::merge_user_orca_overrides()`. The merge runs under
`g_orca_mutex`, so it is safe against concurrent `orca_match_type()` callers.
User entries land in `g_orca_overrides`, where resolution step 1 picks them
up before any shipped lookup. An empty `orca_type_map` (the common case when
no user overlay exists) is a no-op.

**Writing the overlay.** `FilamentCatalog::save_user_products(products)`
replaces the `filaments` section via a temp-file + `rename` (POSIX rename is
atomic within a filesystem, so a **process** crash mid-write never leaves a
partial overlay — the rename either fully happens or doesn't). It does **not**
`fsync`, so this is not a power-loss durability guarantee; on the rare power
cut mid-save a filesystem could still surface a truncated file. That trade is
deliberate: the overlay is written only on user filament edits, and the
original is never modified until the rename succeeds. It performs
read-modify-write to preserve any existing `orca_type_map`, migrates legacy
bare-array overlays to object form on first save, recovers from a corrupt
existing file rather than blocking the save (preserving the unparseable
original as `<path>.bak` for hand-recovery), and creates missing parent
directories. On a fresh install where no overlay exists yet, the write target
falls back to the canonical config/user_filaments.json so the first save can
create the file. The caller supplies pre-built
`nlohmann::json` product objects (one per entry, minimum field `id`) —
typically the modal's form-handler builds these. `orca_type_map` has no
write API today: contributing Orca-type hints is a power-user hand-edit
concern (see issue #1120 and the design spec's § Drift for the rationale —
a UI that invites "add Orca type" misleads users into thinking HelixScreen
can teach Orca new presets, which it cannot; Orca only matches against
types already in its own library).

### Regenerating the catalog

```bash
make regen-filaments ORCA_TAG=v2.4.1     # ORCA_TAG defaults to a pinned tag in mk/filaments.mk
```

This shallow-clones OrcaSlicer's `resources/profiles` at the pinned tag into
`build/orca-profiles` (sparse checkout, blob-filtered), runs
`scripts/import_orca_filaments.py` to resolve `inherits` chains, extract
facts, and union them with the preserved CFS-code seed
(`scripts/fixtures/cfs_seed.json`), writes `assets/filaments.json`, mirrors it
to android/app/src/main/assets/assets/filaments.json, and discards the
cloned Orca checkout. Nothing from the Orca clone is committed — only the
derived output. Bump `ORCA_TAG` to refresh against newer Orca data.

`assets/filaments.json` is **generated but committed** (same pattern as fonts
and translations) — cross-compiled targets need the file present without
running Python/git-clone during the build.

### Licensing and attribution

OrcaSlicer is AGPL-3.0; HelixScreen is GPL-3.0-or-later. HelixScreen never
ships OrcaSlicer's profile files — the importer clones them into scratch,
derives **facts** (nozzle/bed temps, density — not copyrightable expression),
and discards the clone. `filaments.json` carries a top-level `_attribution`
field naming OrcaSlicer, its repo URL, the pinned tag, and its license, e.g.:

```json
"_attribution": "Factual filament data derived from OrcaSlicer (github.com/SoftFever/OrcaSlicer, tag v2.4.1, AGPL-3.0). No OrcaSlicer profile files are shipped."
```

---

## How a lane presents itself

Every surface that draws an AMS lane answers the same question first: does this
lane have filament, did it keep an identity after being ejected, or is it simply
unused. That question has one implementation — `classify_lane()` in
`include/ams_lane_state.h` — and three answers:

| `LaneState` | Meaning | Spool rendering | Bar rendering |
|-------------|---------|-----------------|---------------|
| `Present` | has filament | spool at fill level | bar at fill level |
| `Ghosted` | ejected, identity retained (#1071) | whole cell dimmed, last known fill | same |
| `Empty` | no filament, no identity | placeholder + "Empty" | nothing — the gap is the signal |

Two things about this are deliberate and easy to undo by accident:

**Ghosting dims the whole cell, never one element.** The spool, the material
label and the percent fade together, applied with `lv_obj_set_style_opa()` on the
widget root using the `ghost_opacity` token. A per-element opacity produces a
ghost too faint to read — a bar's fill is only a pixel or two tall at the sizes
bar mode actually runs at, so the dimming has to be what carries the signal.

**A ghosted lane shows its last known fill.** That reverses `a106413f6`, where an
emptied lane rendered a full-strength 75% bar and read as loaded. It is safe only
because the whole cell is dimmed: the dimming is the disclaimer. Do not reuse
`lane_fill_level()`'s ghosted value on a surface that does not dim.

`UNKNOWN` is classified exactly as `EMPTY`, so it inherits the identity split. It
is not a steady state on any backend — every `SlotStatus::UNKNOWN` assignment is
skeleton construction before firmware data lands — so treating it as `Present`
would briefly show filament in a lane that has none.

Loaded-ness and error are **decorations** layered over a base state, from their
own subjects. A blocked lane still has filament; an active lane is still
`Present`.

The classification is published per lane as `ams_slot_<n>_lane_state`;
`ams_lane_bar` consumes it. Converting the remaining surfaces
(`ui_ams_mini_status` bar and spool modes, `ui_panel_ams_overview` mini-bars,
`ams_slot`) is tracked in prestonbrown/helixscreen#1368, which also carries the
open question of how much of the per-lane loop can be expressed in XML.

---

## UI Panels

### AMS Panel (`ui_panel_ams`)

The detail panel showing slots, path visualization, hub sensors, and the currently loaded filament for a single backend. Opened as an overlay from the Filament nav panel or from the AMS Overview Panel.

Key features:
- Slot grid with overlap layout for >4 slots (shared via `ui_ams_slot_layout.h`)
- Path canvas showing filament routing from slots through hub to toolhead
- Backend selector (shown when `backend_count > 1`)
- Unit scoping: can display a subset of slots for a single unit within a multi-unit backend

### AMS Overview Panel (`ui_panel_ams_overview`)

Grid of unit cards showing all units across the system. Each card is a miniature visualization of the unit's slots. Clicking a card transitions inline to a detail view of that unit's slots.

Key files:
| File | Purpose |
|------|---------|
| `include/ui_panel_ams_overview.h` | Class with detail view state |
| `src/ui/ui_panel_ams_overview.cpp` | Card creation, inline detail view, slot layout |
| `ui_xml/ams_overview_panel.xml` | Two-column layout: cards/detail left, loaded info right |
| `ui_xml/ams_unit_card.xml` | Mini unit card with slot bars and hub sensor dot |

**Current scope**: The overview panel queries `get_backend(0)` and displays all units from that single backend's `AmsSystemInfo`. This covers the common case of a single multi-unit AMS system (e.g., AFC with multiple Box Turtle units).

**Future: multi-backend aggregation**: When multiple backends are active simultaneously (e.g., an AFC system on one toolhead + a Happy Hare on another), the overview panel should iterate all backends via `AmsState::get_backend(i)` for `i` in `0..backend_count` and aggregate their units into the card grid. The per-backend slot subject storage (`secondary_slot_subjects_`) and event routing already support this — the UI aggregation is the remaining integration point.

#### Per-unit environment subjects and the `MAX_UNITS` cap

Every unit card binds its temperature/humidity badge to a set of seven per-unit
subjects named `ams_env_ind_<unit>_{temp_text, humidity_text, humidity_status,
humidity_visible, visible, drying_active, drying_text}`. `AmsState` allocates
those statically, one set per unit, up to `AmsState::MAX_UNITS` - **8**, matching
the widest rig the AMS system-path canvas draws, so every unit the path shows also
has a badge to bind.

Cards are created for **every** unit the backend reports, cap or no cap.
`AmsState::env_indicator_subject_names(unit_index)` is the single place that
decides which names a card gets: the unit's own set below the cap, and the
always-off placeholders `ams_env_ind_off_flag` / `ams_env_ind_off_text` at or
above it. Past the cap the badge is simply hidden - slots, hub dot and error badge
all still render - and `create_unit_cards()` logs one line naming the cap.

**Do not expand the `ams_env_ind_%d_*` names at the call site.** That is what the
panel used to do, and a rig with more units than the cap then bound seven names
nothing had registered per excess card: seven `No subject was found` parser
warnings each, plus a permanently dark badge with nothing in the log explaining
it. `AmsState` owns the cap and the registrations, so it owns the naming as well;
raising `MAX_UNITS` stays a one-constant change.

### Error State Visualization

Per-slot error indicators and per-unit error badges, driven by `SlotInfo.error` and `AmsUnit::buffer_health` from the backend layer. (The original 2026-02-15 error-state-visualization design doc is no longer in-tree; the data model below is the surviving summary.)

**Data model** (`ams_types.h`):
- `SlotError` — message + severity (INFO/WARNING/ERROR), `std::optional` on `SlotInfo`
- `BufferHealth` - AFC buffer fault proximity data, `std::optional` on **`AmsUnit`**, not `SlotInfo`. A TurtleNeck sits between the hub and the toolhead, so it belongs to the unit; there is one per unit and it cannot say which lane it is regulating except through its own `active_lane` field
- `AmsUnit::has_any_error()` — rolls up per-slot errors for overview badge

**Detail view** (`ui_ams_slot.cpp`):
- 14px error badge at top-right of spool (red for ERROR, yellow for WARNING), pulled from `SlotInfo` during refresh (same pattern as material/tool badge)

**Path canvas** (`ui_ams_detail.cpp`):
- Buffer fault tint on the hub, from the *displayed unit's* `buffer_health` (green / yellow / red by `distance_to_fault`), with Happy Hare's `sync_feedback_bias` feeding the same three states when there is no AFC buffer
- Buffer presence and compressed/tension state, from that same `buffer_health.state`

**Overview view** (`ui_panel_ams_overview.cpp`):
- 12px error badge at top-right of unit card (worst severity across slots)
- Mini-bar status lines colored by error severity

**Backend integration**:
- AFC: per-lane error from `status` field + buffer health from `AFC_buffer` objects, attributed to units by `apply_buffer_health_to_units()` (see the AFC section)
- Happy Hare: system-level error mapped to `current_slot` via `reason_for_pause`
- Mock: `set_slot_error()` / `set_unit_buffer_health()` + pre-populated errors in AFC mode

### Two error channels

A backend fault reaches the user through one of **two independent channels**. They are not alternatives and not a fallback pair — they are fed by different transports, fire at different moments, and a backend may implement either, both, or neither. Getting this wrong is how a fault double-surfaces or vanishes.

| | **Channel A — line driven** | **Channel B — status driven** |
|---|---|---|
| Hook | `AmsBackend::classify_error(raw_line, ctx)` | `AmsBackend::current_error()` |
| Transport | Moonraker `notify_gcode_response` | Moonraker `notify_status_update` |
| Dispatched from | `GcodeErrorRouter::process_line()`, `src/application/gcode_error_router.cpp:474` — **exactly once per line**, before the generic `error_classify::classify()` | `AmsErrorBridge::on_action_changed()`, `src/application/ams_error_bridge.cpp:68` — **only on the rising edge** into `AmsAction::ERROR` |
| Pre-filtering | **None.** Every response line is handed to every backend. Each override gates itself | The `AmsAction::ERROR` edge is the entire gate. A backend that never assigns that action is never asked, even if it overrides the hook |
| Presentation | `decide_presentation()` → toast / modal / `MODAL_WITH_RECOVER` | `RecoveryModalPresenter::present()` directly |
| Returning `nullopt` | Defers to `error_classify::classify()` | Falls through to the bridge's last-resort toast (`surface_unhandled_error()`) |

**Per-backend gates and recovery sets:**

| Backend | Channel A gate | Channel B gate | Recovery actions (`build_recovery_actions()`) |
|---------|----------------|----------------|-----------------------------------------------|
| **AFC** | `is_bang_line()`, then a `tool_end` jam/break/runout signature, else any pausing `!!` while `error_state_` | `error_state_` set (the stuck-action latch returns `nullopt` — that fault is ours, not AFC's) | Resume (primary, hot) · Unload (hot) *or* Eject lane (cold) depending on `tool_start_sensor_` · AFC_RESET (danger) |
| **Happy Hare** | `is_bang_line()`, then paused **and** (`AmsAction::ERROR` or a recognized cause in `reason_for_pause_`) | — | Backend-derived; title is "Filament runout" when the detail says runout |
| **AD5X IFS** | — | `AmsAction::ERROR`, raised by `evaluate_runout_locked()` or by an operation timeout | Runout: Resume (primary, hot) · Purge `M83`+`G1 E` (hot) · `IFS_UNLOCK` (danger, cold). Timeout: `IFS_UNLOCK` alone. **No "Load slot N"** — every IFS load path self-homes and trash-moves into the part |
| **CFS** | **inverted** — `is_bang_line()` returns `nullopt`, so `!!` `key8xx` codes stay with the generic classifier. Claims only paused `respond_info` lines matching the auto-refill give-up wording | — (never assigns `AmsAction::ERROR`) | Resume (primary, hot) · Reset CFS = `BOX_ERROR_CLEAR` (danger, cold) |
| **QIDI Box** | — | `AmsAction::ERROR`, raised by a negative `slot<N>` state word (blocked lane) | Lone dismiss — recovery gcode unknown, clearance is manual (#1041) |
| **ACE**, **Tool changer**, **Snapmaker** | — | — | — (generic runout modal owns these; see below) |

**There is a third channel, and it decides whether Channel A speaks at all.** When the fault
originated in a macro *we* sent, the same rejection also comes back as the JSON-RPC error reply
to our `printer.gcode.script` request — a third transport, arriving milliseconds before or
after the `!!` line. `MoonrakerRequestTracker` decides on that reply who owns the report, and
if the answer is "the caller's own UI", it records the message and `GcodeErrorRouter` skips its
`!!` toast — Channel A never runs its presentation. This is why a backend's dispatch path must
declare `caller_surfaces_errors` honestly: a log-only `on_error` that claims the report
silences Channel A for a fault nobody saw. See `RPC_ERROR_OWNERSHIP.md`.

**Why CFS inverts the usual gate.** Creality's box reports coded faults as `!!` lines carrying a `key8xx` JSON payload, which `error_classify::classify()` already decodes into a CRITICAL event (and a "Reset CFS" button for `key840`). Claiming those in `classify_error()` would either duplicate that path or silently replace it. The runout give-up messages ride the *other* half of the same channel — plain `respond_info` output that no classifier looks at — so taking non-`!!` lines and only non-`!!` lines is what keeps the two from colliding.

**Cross-channel dedup — there are TWO ledgers, and they guard different pairs.** Both are exact-string, both prune on read, and confusing them leads to debugging the wrong window.

| Ledger | Window | Guards | Recorded by | Checked by |
|--------|--------|--------|-------------|------------|
| `rpc_error_correlation` (`src/api/rpc_error_correlation.cpp`) | 1.5 s | JSON-RPC error reply ↔ the `!!` broadcast of the same rejection | `MoonrakerRequestTracker::route_response()`, only when `rpc_error_policy::decide()` says someone is definitely reporting it | `GcodeErrorRouter::already_reported_via_rpc()` (`gcode_error_router.cpp:173-177`), including a re-check when the deferred toast timer fires |
| `fault_surface_correlation` (`src/application/fault_surface_correlation.cpp`) | 3 s | Channel A ↔ Channel B | `GcodeErrorRouter` records every detail it surfaces | `AmsErrorBridge`'s fallback toast, and the router's own toast arms (`gcode_error_router.cpp:413-418`) |

A merely-`silent` request records **nothing** in the RPC ledger. `silent` means "no automatic toast from us", not "the user was told" — recording on it would mute the `!!` copy for a failure that reached no one. Only a caller that declared `caller_surfaces_errors`, or the generic fallback actually firing, earns a record. See `RPC_ERROR_OWNERSHIP.md`.

`RecoveryModalPresenter` separately dedups on `detail` **plus** the action set — the action set is part of the identity because AFC legitimately emits byte-identical text on both channels with different affordances (#1171). Backends should populate `ErrorEvent::raw_detail` with the firmware's untranslated wording when `detail` has been rewritten, or the ledger has nothing the other channel can match.

**Who owns the runout surface.** Both channels compete with a third, older surface: the generic sensor-driven modal (`FilamentRunoutHandler` on the pause edge, `PrintStatusWidget` when idle), gated by `RuntimeConfig::should_show_runout_modal()`. The rule is **one surface per printer**: that predicate returns false exactly for the backends in the table above that raise their own runout fault (AFC, Happy Hare, AD5X IFS, CFS), and true for hub backends that raise nothing (ACE, QIDI Box) — which the old blanket "is it a hub AMS" test silenced with nothing put in its place (#1250).

Note that for AFC, Happy Hare, AD5X IFS and CFS the generic surface is *also* structurally blind: each claims its own sensors through `owns_filament_sensor()`, so `PrinterHardware::is_ams_sensor()` hides them from the wizard's sensor picker, they never get a `FilamentSensorRole`, and `FilamentSensorManager::has_real_runout()` skips them. The suppression above is belt-and-braces for the configs where an AMS lane sensor *does* carry a role (AFC's `...eN_filament` naming is the case `has_real_runout()`'s lane-mapping branch exists for).

---

## Filament Op Dispatch: Which Surface Owns What

More than one screen can start a Load. Every time one of them grew its own answer to
"what do I do when there is no AMS backend?", the answers diverged: a full three-tier
fallback on the Filament panel, a silent return in the AMS sidebar, and a navigate-away in
both runout dialogs. The already-mounted guard existed only in the sidebar, so the same
firmware no-op that the sidebar refused left the Filament panel's Load button spinning for
the full 120 s guard timeout (bundle 9KRXZ62P). On Snapmaker U1 the two surfaces sent
*different G-code for the same button label* — `T{n}`, which seats the carriage and feeds
nothing, versus `AUTO_FEEDING EXTRUDER={n} LOAD=1`.

The decision is now one shared, display-free layer; the surfaces own only how the answer is
presented.

| Header | Owns |
|--------|------|
| `include/filament_op_dispatch.h` | `plan_load()` / `plan_unload()` — which tier, which backend call, or which refusal. Also `unload_target_is_loaded()`. Header-only, takes plain values (`AmsSystemInfo` + `BackendCaps`), no `AmsBackend*` |
| `include/filament_op_slot_resolver.h` | `resolve_op_button_slot()` — which slot a tool's buttons act on; `compute_op_button_gating()` — whether Load/Unload are enabled |
| `src/ui/filament_op_router.{h,cpp}` | Tiers 2 and 3: `dispatch_filament_macro()` with its `ParamPolicy`, the shared `MacroParamModal`, and `filament_load_fallback_gcode()` / `filament_unload_fallback_gcode()` |

Tier 1 deliberately stays with the callers — the backend call is inseparable from each
surface's own guard, stepper, and spinner bookkeeping.

### The four dispatch surfaces

| Surface | Entry point | Raised by | Dispatches? |
|---------|-------------|-----------|-------------|
| Filament panel | `FilamentPanel::execute_load()` / `execute_unload()` | The Load / Unload buttons on the Filament nav panel | Yes — full ladder, `ParamPolicy::Prompt` |
| AMS operation sidebar | `AmsOperationSidebar::handle_load_with_preheat(slot)` / `handle_unload(slot)` | Slot grid + context menu on the AMS panel and the AMS Overview panel (both own a `unique_ptr` to one) | Yes — full ladder, `ParamPolicy::Prompt` |
| Mid-print runout dialog | `FilamentRunoutHandler::dispatch_load()` | `RunoutGuidanceModal`'s Load button during a print or runout pause | Yes — full ladder, `ParamPolicy::Suppress` |
| Idle runout dialog | `PrintStatusWidget::show_idle_runout_modal()` | A real runout detected while STANDBY / COMPLETE / CANCELLED | **No** — hands off to the Filament panel |

The idle dialog is the one surviving "navigate away", and it is correct *because* it never
dispatches: with the printer idle the Filament panel is reachable, so `set_active(PanelId::
Filament)` inherits that panel's routing instead of forking a fourth answer. That is only
true while it stays a pure hand-off. The moment it wants to load without leaving the modal,
it goes through `plan_load()` like the other three.

### The dispatch ladder

| Order | What runs | Chosen when |
|-------|-----------|-------------|
| 0 `FilamentTier::Macro` (**user override**) | The macro the user assigned in Settings > Macro Buttons, via `dispatch_filament_macro()` | `StandardMacroInfo::get_source() == MacroSource::CONFIGURED`. Outranks everything, on load and unload alike |
| 1 `FilamentTier::AmsBackend` | `load_filament()`, `unload_filament()`, or `change_tool()` — carried in `FilamentOpPlan::ams_call` / `ams_arg` | A backend owns the operation (see the asymmetry below) |
| 2 `FilamentTier::Macro` (auto-detected) | The `StandardMacroSlot::LoadFilament` / `UnloadFilament` we pattern-matched, or a `HELIX_*` fallback | No tier 1, and the slot is non-empty |
| 3 `FilamentTier::RawGcode` | `filament_load_fallback_gcode()` (fast bowden move, then a slow push into the melt zone) or `filament_unload_fallback_gcode()` (tip-shape, then a long retract) | Nothing else is configured |
| — `FilamentTier::Refused` | Nothing. `FilamentOpPlan::refusal` says why | See the refusal table |

**Order 0 keys on the SOURCE of the macro, not its presence.** `plan_load()` and
`plan_unload()` take `macro_available` and `macro_user_configured` separately, and only the
latter jumps the backend. The distinction is load-bearing: the auto-detector matches
`QUIT_MATERIAL` for `UnloadFilament`, so on a CFS printer a presence-based rule would hand
every bypass unload to a vendor macro that cannot finish the job (see
[§ CFS bypass: why the two vendor macros are not symmetric](#cfs-bypass-why-the-two-vendor-macros-are-not-symmetric)).

An override **replaces** the backend call rather than running alongside it, so the backend's
bookkeeping goes with it — on AFC that means `TOOL_UNLOAD` does not run, and lane state and
shuttle parking become the macro's responsibility. That is the intended meaning of the
setting: a user with extra steps to run gets to own the whole operation. It is documented for
users in `docs/user/guide/filament.md`.

`AmsCall::ChangeTool` carries a **tool number**, not a slot index — it comes from the target
slot's `mapped_tool`. Every other call takes the slot.

| Refusal | Meaning | Reached from |
|---------|---------|--------------|
| `SelectSlot` | The backend wants a slot and none resolved | Load only |
| `AlreadyMounted` | The requested tool is already on the carriage. `SELECT_TOOL` on it is a firmware no-op (9KRXZ62P) | Load only, tool changers only |
| `NothingLoaded` | No slot resolved, or nothing at that slot worth pulling | Unload only — its *only* refusal |

### One deliberate asymmetry between load and unload

**Bypass falls through on load and stays on the backend for unload.**

`plan_load()` gates tier 1 on `caps.present && caps.requires_slot_selection_for_load`, not on
the backend merely existing. `AmsBackend::requires_slot_selection_for_load()` defaults to
`!is_bypass_active()`, so an active bypass drops past the backend to an auto-detected
`LOAD_FILAMENT` macro — on a stock Creality printer that is `LOAD_MATERIAL`, and it is how a
bypass spool loads at all.

`plan_unload()` gates tier 1 on `caps.present` alone, because the CFS backend has to own the
bypass unload: the vendor's `QUIT_MATERIAL` does not finish it. See the next section.

There used to be a second reason recorded here — that AFC runs the user's unload macro as part
of its own unload, so tier 2 would run it twice. That hazard belonged to a design where both
fired; order 0 above *replaces* the backend call, so an override runs exactly once.

Load/unload dispatch is not the only thing bypass reaches — it also suppresses both pre-print
filament gates. See [§ Bypass suppresses the pre-print filament gates](#bypass-suppresses-the-pre-print-filament-gates).

### CFS bypass: why the two vendor macros are not symmetric

Creality ships an external-spool pair alongside the `BOX_*` family, and only half of it is
self-sufficient. Both verified on a K2 Plus, 2026-08-19, by watching the extruder axis:

| Macro | What it does | Extruder delta measured |
|-------|--------------|-------------------------|
| `LOAD_MATERIAL` | `BOX_GO_TO_EXTRUDE_POS` / `FILAMENT_RACK_SAVE_FAN` / `FILAMENT_RACK_PRE_FLUSH` / `FILAMENT_RACK_SET_TEMP` / `FILAMENT_RACK_FLUSH` / park + `SET_COOL_TEMP`. Feed and purge both gated on the toolhead switch | **+370 mm** — a complete load |
| `QUIT_MATERIAL` | `BOX_GO_TO_EXTRUDE_POS` / `FILAMENT_RACK_SET_TEMP` / `BOX_MOVE_TO_CUT` / `G0 E-10` / park + `SET_COOL_TEMP` | **-13.99 mm** — filament still gripped by the gears |

The asymmetry is in `[box]` itself: `tn_extrude = 140` against `tn_retrude = -10`. A bay unload
only needs the extruder to break its grip because the box's own feeder motors reel the
remaining ~130 mm back down the tube. **A bypass spool has no feeder**, so the extruder has to
cover the whole path alone.

`AmsBackendCfs::bypass_unload_gcode()` therefore emits `QUIT_MATERIAL` plus an 80 mm retract of
its own; `bypass_load_gcode()` emits `LOAD_MATERIAL` bare, because nothing is missing there.
80 mm because the release point measured near 64 mm (`QUIT_MATERIAL`'s 14 plus 50 more by hand
before the filament came free), and it is the length `filament_unload_fallback_gcode()` already
uses for the same physical job.

This is also why an auto-detected `QUIT_MATERIAL` must not outrank the backend: it is a real
unload macro, it matches the detector, and on bypass it silently leaves filament in the
extruder.

### Load-vs-swap and already-mounted exist only on the load side

A machine with filament already seated cannot simply feed another lane, so when
`needs_unload_before_load(info)` is true and the target slot has a `mapped_tool`, `plan_load()`
rewrites the call to `change_tool(mapped_tool)`. Centralized so the UI and the backend agree
(#968). A target with **no** tool mapping falls through to a plain `load_filament()` rather
than synthesising an unload: every backend that arm could reach already chains the unload
inside its own load (ACE's `change_tool()` *is* `load_filament()`; QIDI prepends the unload
itself; AFC's `CHANGE_TOOL` is the toolchange verb), and Happy Hare — the one backend whose
`load_filament()` is a bare `MMU_LOAD GATE={n}` — is precisely the backend the UI is
forbidden to help (`allows_implicit_chaining()` is false, #1229). Unload asks none of this.

Neither asymmetry is visible in `plan_unload()`'s signature, which is why both call sites
carry a comment saying so. Read `include/filament_op_dispatch.h` before "fixing" either.

### Shared policy vs per-surface presentation

**Shared — one answer, in the planner.** A second answer here is a user-visible bug.

| Question | Answered by |
|----------|-------------|
| Which tier does this operation take? | `plan_load()` / `plan_unload()` |
| Is this a fresh load or a swap? | `plan_load()` via `needs_unload_before_load()` -> `AmsCall::ChangeTool` |
| Is the requested tool already mounted? | `plan_load()` -> `FilamentRefusal::AlreadyMounted` |
| Is there anything at this slot to unload? | `unload_target_is_loaded()` — actively loaded, **or** filament at the toolhead, **or** it is the current slot (the runout-recovery case, #995 / #1199) |
| Which slot do this tool's buttons act on? | `resolve_op_button_slot()` |
| Are Load / Unload enabled right now? | `compute_op_button_gating()` — load state *and* print state |

**Per-surface — presentation, and correctly different.**

| Surface | Owns |
|---------|------|
| `FilamentPanel` | `begin_operation_guard()` / `operation_guard_`, the `backend_op_active_` gate on `ams_action_observer_`, the on-button spinner (`op_started` / `op_succeeded` / `op_failed`), and `navigate_to_ams_panel()` on `SelectSlot` |
| `AmsOperationSidebar` | The step model (`start_operation(StepOperationType::LOAD_FRESH / LOAD_SWAP / UNLOAD)`) and the preheat state machine (`get_load_temp_for_slot()`, `pending_load_slot_`, `check_pending_load()`, `ui_initiated_heat_`) |
| `FilamentRunoutHandler` | Staying put. Every outcome is a toast; navigating would tear down the dialog the user is standing in |
| All three | Toast copy, and whether to toast at all. On a *dispatch* failure that is not purely presentational: the send's `caller_surfaces_errors` says whether this surface's `on_error` really shows the user something, and a surface that claims it silences `GcodeErrorRouter`'s `!!` report of the same rejection. A surface that only logs must pass `false` — see `RPC_ERROR_OWNERSHIP.md` |

Two consequences worth naming, because they look like bugs and are not:

- **The sidebar is silent on a refusal; the panel toasts.** The AMS grid already highlights
  the mounted slot and greys the unpickable ones, so a toast there narrates what the user
  can see. On the Filament panel the button is the only feedback there is.
- **Tool changers skip the sidebar's preheat entirely.** `SELECT_TOOL` owns its own heat
  sequence and the backend sets `SELECTING` at dispatch, resolving on the macro ack (#1183);
  an optimistic `HEATING` stepper would fight it. Only the *decision* is shared.

Two more where the surface deliberately does **not** use the plan's value:

- The sidebar **re-plans after preheat** (`check_pending_load()`) instead of replaying the
  plan it computed before heating — the firmware may have picked up or dropped a tool while
  the nozzle came up, which flips load-vs-swap.
- The sidebar passes its caller's raw `slot_index` to `unload_filament()`, **not**
  `plan.ams_arg`: its own Unload button means "whatever is active" and passes `-1`, which the
  AD5X IFS backend keys on to send `IFS_REMOVE_CURRENT_PRUTOK`. The Filament panel does the
  opposite and passes its resolved slot explicitly, because re-resolving `current_slot` inside
  the backend was the U1 wrong-tool unload bug.

### The lifetime hazard in tier 2

`get_filament_param_modal()` returns a **function-local static** — one `MacroParamModal` for
the whole process. `MacroParamModal` stores its `on_execute_` callback and **does not clear it
on dismiss**; only the next `show_for_*()` overwrites it. A callback handed to that modal can
therefore fire arbitrarily later, long after the object that built it is gone.

| Surface | Lifetime | What tier 2 must capture |
|---------|----------|--------------------------|
| `FilamentPanel` | Immortal singleton | Bare `[this]` is safe, annotated `[L012]` |
| `AmsOperationSidebar` | `unique_ptr` on the AMS / AMS Overview panel — destroyed when the panel closes | **Must** capture `lifetime_.token()` and re-enter through `token.defer(tag, ...)`, which re-checks the generation on the main thread. A bare `this` here is a live use-after-free |
| `FilamentRunoutHandler` | Owned by the print-status panel | Uses `ParamPolicy::Suppress`, so `run` fires synchronously inside `dispatch_filament_macro()` and is never retained |

`ParamPolicy::Suppress` is not only a lifetime dodge — it is required for any surface that
already owns a dialog. A `MacroParamModal` raised from the runout dialog would stack on top of
a live modal whose observers keep firing underneath it.

`dispatch_filament_macro()` returns **true when a prompt was raised**, which is exactly the
"your callback outlived this call" signal: `false` means `run` already executed with an empty
`MacroParamResult`. Tests reach the prompt branch without a screen via
`set_filament_param_prompter()`; pass a default-constructed `ParamPrompter` to restore the
shared modal.

### Rules for contributors

**Adding a fifth dispatch surface.** Do not write another ladder.

1. Read the backend's answers into a `BackendCaps` (`present`,
   `requires_slot_selection_for_load()`, `needs_unload_before_load(info)`, `get_type() ==
   AmsType::TOOL_CHANGER`) — the existing surfaces do this in three or four lines each.
2. Call `plan_load()` / `plan_unload()` and `switch` on `plan.tier`. Handle all four arms,
   including `Refused`.
3. Tier 1 is yours (the backend call sits inside your own guard/stepper bookkeeping). Tiers 2
   and 3 come from `dispatch_filament_macro()` and the two fallback-G-code helpers — do not
   re-derive either.
4. Pick a `ParamPolicy`: `Suppress` if your surface already owns a dialog, `Prompt`
   otherwise. If you pick `Prompt` and you are not immortal, capture a lifetime token.
5. Add a case to `tests/unit/test_filament_dispatch_surfaces.cpp` — its whole point is that
   all surfaces answer the same question the same way.

**Adding a new backend.** Do not add a UI branch for it. The plan is driven entirely by
`requires_slot_selection_for_load()`, `needs_unload_before_load()`, `is_bypass_active()`,
`get_type()`, `slot_is_actively_loaded()`, and `slot_has_filament_at_toolhead()`. If the plan
is wrong for your hardware, the fix is in one of those predicates or in
`filament_op_dispatch.h` — never in a surface. See also "Per-Slot Load Authority" and
"Developer Guide: Adding a New Backend".

**Deciding whether a new question is shared policy or presentation.** In order:

1. *Would two surfaces answering it differently be a bug the user could see?* Yes -> shared.
   The four divergences above all failed this test.
2. *Does the answer depend on the printer, the firmware, or the backend — or on which screen
   the user is standing on?* Printer -> shared. Screen -> presentation.
3. *Does answering it need a widget, a timer, a stepper, or `this`?* If yes it cannot live in
   the planner, which takes plain values by design so the whole decision compiles and runs in
   a binary with no printer and no display (`tests/unit/test_filament_op_dispatch.cpp`,
   `test_filament_op_slot_resolver.cpp`). If a question fails 3 but passes 1, split it: the
   *rule* goes in the planner, the *effect* stays in the surface. That split is exactly what
   `plan.ams_call` is.

---

## Swap Preheat: Hold Previous Filament Temp

When a user switches filament, the nozzle must stay hot enough to purge the material already in the melt zone. Dropping straight to the new material's temperature (e.g. ABS 250 → TPU 230) leaves un-purged high-temp filament clogging the path.

**The rule.** A "switching material" send floors the nozzle target at:

```
load_target = max(new_material_temp, last_nonzero_nozzle_target, current_actual_nozzle_temp)
```

- `new_material_temp` — what the tapped preset / load op requested.
- `last_nonzero_nozzle_target` — an **in-session latch** of the last non-zero nozzle target. It **survives the target cooling to 0**, so even a cold swap reheats to the old material's temp to purge it. Latched in `PrinterTemperatureState::update_from_status()` (per-`ExtruderInfo.last_nonzero_target`, per-extruder).
- `current_actual_nozzle_temp` — covers a physically-hot nozzle whose target was already cleared.

**Latch lifecycle.**
- **Set:** every status update with `target > 0` (per extruder).
- **Survives:** cooldown to 0 (the whole point).
- **Reset:** on **unload only** — the filament is physically pulled, so nothing is left to purge. `FilamentPanel::execute_unload()` and `AmsOperationSidebar::handle_unload()` call `PrinterState::clear_nozzle_load_latch()`.
- **Not persisted** across restart — a power cycle means a cold printer that reheats anyway, and persistence is where staleness would bite.

**Where the guard lives.** `TemperatureController::set_target(HeaterType, celsius, opts)` applies the floor when `opts.keep_previous_hot` is set. Nozzle only — bed/chamber and any send without the flag are untouched, so cooldown-to-0 and deliberate manual keypad lowers still work.

**Which calls set `keep_previous_hot`.**
| Call site | Flag | Rationale |
|-----------|------|-----------|
| Material preset tap (`handle_preset_button`, `handle_spool_preset_button`) | ✅ on | "I'm switching material" |
| Op preheat (`start_preheat_for_op` — load/extrude/purge/etc.) | ✅ on | controller computes the max; replaced the old target-only check |
| AMS load-with-preheat (`handle_load_with_preheat`) | ✅ on | skip/wait decision also uses `max(actual, latch)` so a cooled nozzle still reheats to purge |
| Manual keypad entry (`handle_custom_nozzle_confirmed`) | ❌ off | deliberate override |
| Cooldown-to-0 | ❌ off | must still reach 0 |

**User feedback.** When (and only when) the guard raises the target above the request, an info toast fires: *"Holding nozzle at N°C to purge previous filament."* (plus an `spdlog::info` line). No toast when the request already clears the floor.

---

## Supported Backends

Each backend has its own leaf doc covering the protocol, data sources, G-code commands, topology, and capability table. This hub keeps what they share: the dispatch ladder, slot metadata, the endless spool model, UI panels, dryer control, device operations, mock mode, and the add-a-backend guide.

| Backend | Hardware | Leaf doc |
|---------|----------|----------|
| Happy Hare | ERCF / Tradrack and other selector-based MMUs (Klipper add-on) | [FILAMENT_BACKEND_HAPPY_HARE.md](FILAMENT_BACKEND_HAPPY_HARE.md) |
| AFC | Box Turtle, OpenAMS, Toolchanger units (AFC-Klipper-Add-On) | [FILAMENT_BACKEND_AFC.md](FILAMENT_BACKEND_AFC.md) |
| ACE | Anycubic ACE Pro 4-slot hub (native GoKlipper/Rinkhals; ValgACE REST fallback) | [FILAMENT_BACKEND_ACE.md](FILAMENT_BACKEND_ACE.md) |
| Tool Changer | viesturz/klipper-toolchanger physical toolheads | [FILAMENT_BACKEND_TOOLCHANGER.md](FILAMENT_BACKEND_TOOLCHANGER.md) |
| AD5X IFS | FlashForge Adventurer 5X Intelligent Filament Switching via ZMOD | [FILAMENT_BACKEND_AD5X_IFS.md](FILAMENT_BACKEND_AD5X_IFS.md) |
| CFS | Creality Filament System (K2 built-in; K1/K1C/K1 Max upgrade) | [FILAMENT_BACKEND_CFS.md](FILAMENT_BACKEND_CFS.md) |
| QIDI Box | QIDI PLUS4 / Q2 / MAX4 RFID hub, chainable to 4 boxes | [FILAMENT_BACKEND_QIDI_BOX.md](FILAMENT_BACKEND_QIDI_BOX.md) |
| Snapmaker U1 | SnapSwap 4-toolhead parallel toolchanger with per-channel RFID | [FILAMENT_BACKEND_SNAPMAKER_U1.md](FILAMENT_BACKEND_SNAPMAKER_U1.md) |

### AmsType Enum

```cpp
enum class AmsType {
    NONE = 0,         // No AMS detected
    HAPPY_HARE = 1,   // Happy Hare MMU (mmu object in Moonraker)
    AFC = 2,          // AFC-Klipper-Add-On (AFC object, lane_data database)
    ACE = 3,          // AnyCubic ACE Pro (ValgACE/BunnyACE/DuckACE Klipper drivers)
    TOOL_CHANGER = 4, // Physical tool changer (viesturz/klipper-toolchanger)
    AD5X_IFS = 5,     // FlashForge AD5X IFS (Intelligent Filament Switching)
    CFS = 6,          // Creality Filament System (K2 series, RS-485)
    SNAPMAKER = 7,    // Snapmaker U1 SnapSwap toolchanger
    QIDI_BOX = 8      // QIDI Box (PLUS4 / Q2 / MAX4, hub-style, 4 slots chainable to 16)
};
```

Helper functions: `is_tool_changer()` and `is_filament_system()` distinguish between the two categories.

---

## Endless Spool (shared model)

Every backend means the same thing by "endless spool" - when a slot runs dry mid-print,
something switches to another slot that can stand in for it - but each firmware answers a
different set of questions about it. The shared model is three things:

| Piece | Where |
|-------|-------|
| `EndlessSpoolCapabilities` - what a backend can do, on three axes | `include/ams_types.h` § "Endless Spool Types" |
| `EndlessSpoolConfig` / `EndlessSpoolGroup` - the relation itself | same |
| Restriction text, the two config builders, the one projection | `src/printer/ams_endless_spool.cpp` |

Nothing in `ams_endless_spool.cpp` touches a backend, a mutex or LVGL, so it is directly
unit-testable (`tests/unit/test_ams_endless_spool.cpp`).

### Three axes, not one bool

| Axis | Type | Values |
|------|------|--------|
| Availability | `EndlessSpoolAvailability` | `Unsupported` / `RequiresPlugin` / `Available` |
| Enablement | `EndlessSpoolEnabled` | `Unknown` / `Off` / `On` / `OnWithoutBackup` |
| Editability | `EndlessSpoolEditability` | `ReadOnly` / `PerSlot` / `Group` |

The axes are independent because real backends occupy the corners. CFS is
available-and-read-only whether auto-refill is on or off, so a single `supported` bool
rendered both states identically. `RequiresPlugin` is retained in the enum for a future
backend whose package genuinely can be missing; no backend currently uses it, since the
AD5X stock-zMod path moved to `Available`/`FirmwareManaged` once source-read of
`ANALOG_PRUTOK` established that switchover is always-on there. `Unknown` is not `Off`: only
`Off` justifies telling the user that no automatic switchover will happen. `OnWithoutBackup`
(CFS, #1391) is `On` plus a grouping-derived negative: the setting is on, but no two lanes
currently group, so a runout would stop the print anyway.

Editability carries a shape, not just a yes/no, because the write shape matters to the UI: a
`PerSlot` write touches one slot (AFC `SET_RUNOUT`), while a `Group` write can move other
slots' relations as a side effect because the transport rewrites the whole partition (Happy
Hare `GROUPS=<csv>`).

`EndlessSpoolRestriction` says **why** editing is restricted, as an enum: `None`,
`MultiUnit`, `FirmwareManaged`, `NotReady`, `PluginMissing`, `PluginReadOnly`. Display text
comes from `endless_spool_restriction_text()`, which is the only place `lv_tr()` is involved.
The struct's one free-text field is `provider`, the proper noun of the package implementing
the feature (`"lessWaste"`, `"bambufy"`), empty when the backend or firmware implements it
natively - a product name is never translated, which is why it is allowed to be free text.
`available()` and `editable()` are the convenience predicates callers use; `editable()`
implies `available()`.

This replaced `{bool supported; bool editable; std::string description;}`, where
`description` was never displayed yet carried load-bearing state as untranslated English
("Auto-refill enabled", "...read-only on multi-unit") that no UI could safely show in any
language but ours.

### Groups, and the single projection

`get_endless_spool_config()` returns **one** `EndlessSpoolConfig` for the whole system,
holding `std::vector<EndlessSpoolGroup>`. A group has `id` (the backend's own group number,
or -1 for one we synthesised), `members` (global slot indices) and `ordered`:

- `ordered = true` - `members[i]` hands off to `members[i+1]`; the last member has no
  successor. An AFC `SET_RUNOUT` edge is a two-member ordered group. Overlapping ordered
  groups are legal: AFC permits 0->2 and 1->2, which is two pairs sharing slot 2.
- `ordered = false` - any member substitutes for any other. Happy Hare's gate group is one
  undirected group of arbitrary size, and an unordered relation is a partition.

Two builders construct it. `endless_spool_config_from_edges(edges)` takes per-slot directed
backups (`-1` or a self-edge is skipped) and emits one two-member ordered group per edge.
`endless_spool_config_from_groups(group_ids)` takes per-slot group ids - Happy Hare's shape -
and emits one unordered group per id, dropping any group with fewer than two members: a group
of one backs nothing up, and emitting it would make "is grouped" and "has a backup" disagree,
which matters because Happy Hare gives every ungrouped gate its own standalone id.

Anything that needs one successor per slot calls the projection and never re-derives it:
`endless_spool_backup_edges(cfg, slot_count)` for a whole system,
`endless_spool_backup_for(cfg, slot)` for one slot. Ordered groups project along their order.
Unordered groups project onto a **ring**: `members[i] -> members[i+1]`, last back to first.
Both entry points agree that the first group to give a slot a successor wins, so they cannot
disagree on a hand-built config with two successors for one slot. The two production callers
are `AmsPanel::update_endless_arrows_from_backend()` (`src/ui/ui_panel_ams.cpp`) and
`AmsContextMenu::get_current_backup_for_slot()` (`src/ui/ui_ams_context_menu.cpp`), so the
arrows and the dropdown cannot disagree about a Happy Hare group.

**Why a ring and not "the first other member".** The projection originally pointed every
member at the first *other* member, reproducing the arrow set Happy Hare's backend computed
inline with a `// Use first match` loop. For a 4-gate group that draws 0->1, 1->0, 2->0, 3->0
— a picture that says "gate 1 is everyone's backup", which is not what a clique means. A ring
gives every member exactly one successor and visits the whole group, which is the closest a
one-target-per-source edge view can get to "any member substitutes for any other".

**What the arrow widget cannot express, and is not asked to.** `ui_endless_spool_arrows`
(`src/ui/ui_endless_spool_arrows.cpp`) takes `backup_slots[source] = target`: one target per
source, at most 16 slots, drawn as a directed dashed up-over-down line with an arrowhead at
the target. It has no primitive for a pool — no bracket, no shared container, no undirected
edge — so an N-member clique genuinely cannot be drawn as a clique, and drawing all N*(N-1)
directed arrows would be unreadable at 480x272 even if it were correct. The ring is the honest
fallback, not a claim to be the whole relation. The widget does now clamp its stacked route
heights to the canvas: an N-member group projects to N mutually-overlapping arrows, and the
unclamped height ladder used to walk past the bottom edge and draw the "vertical" segments
inverted, outside the widget.

### The status line

Capabilities are only worth having if the user can see them. `endless_spool_status(caps)`
(`src/printer/ams_endless_spool.cpp`) is the one place the enums become a sentence. It returns
`{EndlessSpoolStatusKind kind, std::string text}`; `AmsState::sync_endless_spool_from_backend()`
publishes those on two backend-neutral XML subjects, from `sync_from_backend()` — main thread,
because the `EVENT_STATE_CHANGED` handler already marshals through `helix::ui::queue_update()`.

| Subject | Type | Meaning |
|---------|------|---------|
| `ams_endless_state` | int, `EndlessSpoolStatusKind` | `Hidden` 0 / `On` 1 / `Off` 2 / `Unknown` 3 / `NeedsPlugin` 4. A UI contract — append, never renumber. `Hidden` is 0 so one `bind_flag_if_eq ref_value="0"` hides the row |
| `ams_endless_text` | string | The translated sentence, possibly with an embedded newline. Bind to a `long_mode="wrap"` label |

Wording rules, all pinned by tests in `test_ams_endless_spool.cpp`:

- `Unsupported` renders **nothing**. Not "off": a printer with no such mechanism is not a
  printer with the mechanism switched off, and the row disappears rather than asserting
  something about a feature that does not exist.
- `Unknown` is phrased as unknown. Only `Off` says "nothing will switch" — that is the whole
  reason enablement is tri-state, and flattening `Unknown` to `Off` is a promise we cannot
  keep.
- `RequiresPlugin` names `provider` when the backend knows the package
  ("Needs the lessWaste package to switch spools") and otherwise falls back to the restriction
  text. **No backend populates `provider` in that state today**: AD5X cannot know whether the
  user would install lessWaste or bambufy, so the generic
  "No automatic backup-spool package is installed" is what actually renders on stock zMod.
- A non-`None` restriction is appended on its own line, from
  `endless_spool_restriction_text()` and never a second copy of that prose. "It will not
  switch" and "and here is why you cannot change that from here" are two different facts.
- A non-empty `provider` is attributed parenthetically ("… on runout (bambufy)"). A proper
  noun needs no translation, so this costs no string.

**Where it renders, and where it deliberately does not.** Two homes, one component
(`ui_xml/components/ams_endless_status.xml`, registered in `src/xml_registration.cpp` ahead of
`filament_panel.xml` because the AMS panel registers itself lazily). The component is entirely
subject-driven and needs no C++ of its own.

| Surface | Why |
|---------|-----|
| AMS panel, inside `slot_area` under the slots | It explains the arrows at the top of that same container. Growth is absorbed by `path_container`, which is `flex_grow="1"` and whose canvas scales. Note it goes in `slot_area`, not directly in `ams_unit_card` — the card has no `flex_flow`, so a second child there stacks *on top of* the slots |
| Slot context menu, under `backup_dropdown_row` | Where the disabled-or-absent backup dropdown actually is, and the only surface a CFS user reaches at all: CFS hides the dropdown row entirely (no per-slot relation), so without this line tapping a slot said nothing about runout behaviour |

**Not on the filament panel**, despite it being the obvious second home. Its `left_column` is a
fixed height budget with `temp_graph_card` as the flexible remainder
(`FilamentPanel::apply_left_column_sizing()`), so any row added to `spool_card` is paid for
entirely by the temperature graph. Measured with a two-line status: 172 -> 76 px at LARGE,
118 -> 30 px at MEDIUM; and at MICRO it does not fit at all (`spool_card` is 74 px there, 48 of
it `ams_manage_row`).

**Measured at 480x272 (MICRO).** Label width 259 px in the AMS card, 15 px per line. Every
headline fits one line in all nine languages. Four of the five restriction texts need two lines
in `ru` and `es`, two do in `fr`, two in `pt`, one in `de`; `en`, `it` and `zh` fit all five on
one line, `ja` needs two for one. Worst total is therefore 3 lines / 45 px, which takes
`path_canvas` from 112 to 97 px with `scroll_bottom` staying negative — nothing clips, because
`long_mode="wrap"` cannot clip. The Russian `Unknown` headline was shortened to
"Резервная катушка: состояние неизвестно" precisely to hold that 3-line ceiling; at its
original length the worst case was 4 lines / 60 px and `path_canvas` fell to 82 px.

### What the base owns, what a backend supplies

`AmsBackend::set_endless_spool_backup()` is **deliberately not virtual**
(`src/printer/ams_backend.cpp` § "Endless Spool - shared validation"). It owns every
rejection, in order: feature unavailable; feature read-only (carrying the translated
restriction reason); `endless_spool_slot_count() <= 0`, reported as `NotReady`; `slot_index`
out of range; `backup_slot` out of range; `backup_slot == slot_index`. Three backends used to
write those same guards with three different phrasings of the self-backup error.

A backend supplies only these:

| Hook | Responsibility |
|------|----------------|
| `apply_endless_spool_backup(slot, backup)` (protected virtual) | Transport only. Reached **after** the base accepted the write, so it must not re-check availability, editability, ranges or self-backup - and must not update a local mirror of the mapping before its transport has accepted the command. |
| `endless_spool_slot_count()` (protected virtual) | How many slots the relation spans; drives range validation and the reset loop. Default `get_system_info().total_slots`. Override when the transport's slot space differs, or to report 0 while not ready. |
| `endless_spool_backup_eligibility(slot, backup)` | May this lane stand in for that one? Returns `BackupEligibility` (`Eligible` / `GradeDiffers` / `Incompatible`), not a bool. The base default asks two questions in order: `filament::materials_compatible()` for the polymer, then `filament::grades_match()` for the grade, so a filled variant of the right polymer answers `GradeDiffers` - tagged in the dropdown, still selectable, because a swap that keeps the print alive beats a print that dies at a runout. An unknown material on either side stays `Eligible` rather than blocking a slot the user simply has not labelled. AD5X IFS overrides it with the rule its firmware actually enforces - exact material **and** exact colour **and** the port reporting filament present - sharing `backup_eligible_locked()` with `find_backup_slot_locked()` so its runout hint text and its eligibility answer cannot diverge, and answering only `Eligible`/`Incompatible` because a lane its firmware will not select is not a choice worth offering. |

`reset_endless_spool()` has a real base implementation: walk
`set_endless_spool_backup(slot, -1)` over every slot, continue past failures so it clears as
many as it can, return the first error. That loop was AFC's private implementation; AFC
deleted its copy and every editable backend gets it now. Happy Hare overrides it because its
firmware has an actual primitive.

`get_endless_spool_capabilities()` and `get_endless_spool_config()` overrides take the
backend's own `mutex_`, so callers must not hold it. `set_endless_spool_backup()` holds no
lock and hands off to the hook with no lock held.

### `endless_spool_enabled` is a carrier, not a second answer

`AmsSystemInfo::endless_spool_enabled` is the ENABLE axis only. It exists because the
WebSocket parse builds an `AmsSystemInfo` off the main thread and commits it under the
backend mutex, so the parsed bit needs a home in that struct: CFS `box.auto_refill` (stock) /
`box.runout_swap_enabled` (flat fork), Happy Hare `mmu.endless_spool_enabled`, AD5X
`variable_backup` from the `_ifs_vars` macro's status dict.
`get_endless_spool_capabilities()` is the single source of truth for all three axes and
**derives** `caps.enabled` rather than answering independently, so the two cannot diverge.
Read the capabilities, not the field.

CFS, Happy Hare and the mock read the field directly. Two backends are one step removed and
say so at the site: AD5X IFS keeps a `std::optional<bool>` (`ifs_backup_variable_`) as its
source of truth because a plain bool cannot express `Unknown`, and mirrors it into the field
so `get_system_info()` agrees; AFC has no enable bit to read at all and reports `On`
unconditionally, its field seeded once from `afc_default_capabilities()`.

The field replaced `AmsSystemInfo::supports_endless_spool`, which answered the *availability*
question a second time and provably disagreed with `get_endless_spool_capabilities()` on CFS
whenever auto-refill was off.

### Per-backend state

| Backend | Availability | `enabled` derived from | Editability | Restriction | Per-slot relation? |
|---------|--------------|------------------------|-------------|-------------|--------------------|
| AFC | `Available` | Hardcoded `On` - a lane either names a runout lane or it does not, so there is no on/off switch to read | `PerSlot` (`SET_RUNOUT LANE= RUNOUT=`) | `None` | Yes - `endless_spool_config_from_edges(slots_.backup_edges())` |
| Happy Hare | `Available` | `mmu.endless_spool_enabled`; forced to `Unknown` before `slots_` is initialised | `Group` on a single unit, `ReadOnly` otherwise | `None`; `MultiUnit` on a multi-unit rig; `NotReady` before the registry initialises | Yes - `endless_spool_config_from_groups()` over each gate's `endless_spool_group` |
| CFS | `Available` | `box.auto_refill` / `box.runout_swap_enabled` | `ReadOnly` | `FirmwareManaged` | **No, deliberately** - see below |
| AD5X IFS | `Available` in all three modes (stock zMod, bambufy, lessWaste) | stock zMod: always `On` (ANALOG_PRUTOK has no toggle); plugin path: `variable_backup`, with a genuine `Unknown` when it was never read | `ReadOnly` | `FirmwareManaged` on stock zMod; `PluginReadOnly` on the plugin path | No |
| ACE, QIDI Box, Snapmaker U1, Tool Changer | `Unsupported` (base default; no override at all) | -- | `ReadOnly` | `None` | No |
| Mock | `set_endless_spool_supported()` | `system_info_.endless_spool_enabled` | `PerSlot` when `set_endless_spool_editable(true)`, else `ReadOnly` | `FirmwareManaged` when read-only | Yes - edges from its `SlotRegistry` |

CFS, and AD5X IFS in every mode (stock zMod, bambufy, lessWaste), report `Available` while
leaving `get_endless_spool_config()` unoverridden. That is the truthful answer, not an
omission: the firmware picks the backup itself and exposes no per-slot mapping to read, so
the base's empty relation is correct, and it is what keeps the context menu from drawing a
dropdown that could only ever read "None" (see [Context Menu Actions](#context-menu-actions)).

---

## Dryer / Box-Heater Control

Some AMS backends include an integrated filament dryer — a heated chamber that removes moisture from hygroscopic filaments (Nylon, PA-CF, TPU, PETG, etc.) before or during a print. HelixScreen exposes a common dryer control UI across all backends that support it.

### Data Flow

```
AmsBackend::get_dryer_info()       populates DryerInfo (ams_types.h)
        │
        ▼
AmsState::sync_dryer_from_backend()  bridges to LVGL subjects
        │
        ▼
AmsEnvironmentOverlay              control UI (ui_ams_environment_overlay.cpp
  + ui_xml/ams_environment_overlay.xml)  target temp, duration, start/stop
```

`DryerInfo` (declared in `include/ams_types.h`) carries:

| Field | Type | Description |
|-------|------|-------------|
| `supported` | bool | Whether this backend has a dryer at all |
| `active` | bool | Dryer is currently running |
| `current_temp` | float | Current chamber temperature (°C) |
| `target_temp` | float | Target setpoint (°C) |
| `remaining_minutes` | int | Countdown to end of session (-1 = no timer) |
| `max_temp` | float | Hardware maximum for the target slider |

> **Humidity is not on `DryerInfo`.** It lives on `EnvironmentData` (per-unit, `AmsUnit::environment`) alongside the box temperature, because humidity is a per-enclosure reading from an environment sensor, not a property of the global dryer session. The dryer overlay and the AMS panel environment indicator both read `AmsUnit::environment`.

`AmsEnvironmentOverlay` is opened for one specific unit (`unit_index_`), so it owns its
own `ams_env_overlay_humidity_visible` subject rather than binding the unit cards'
`ams_env_ind_<i>_humidity_visible`. Both the humidity readout and the Material Comfort
strip gate on it, and both need a real reading to say anything true. Binding a
per-unit-indicator subject here means picking an index at XML-authoring time, which
answers for whichever unit that index names and not the one on screen - the overlay was
hard-wired to unit 0's flag, so opening unit 1's environment showed unit 0's humidity
availability. The overlay's copy is set from the same rule the badge uses: the unit
reports an environment **and** that environment has a humidity sensor.

### Backend Virtual Interface

Declared in `include/ams_backend.h`. Default implementations return `supported=false` / `not_supported` so existing backends that don't have a dryer need no changes.

| Virtual | Default | Description |
|---------|---------|-------------|
| `get_dryer_info()` | `DryerInfo{.supported=false}` | Read current dryer state |
| `start_drying(temp, minutes)` | NOT_SUPPORTED | Begin a drying session |
| `stop_drying()` | NOT_SUPPORTED | End the active session |
| `update_drying(temp, minutes)` | NOT_SUPPORTED | Change temp/time mid-session |
| `get_drying_presets()` | empty vector | Return material-preset list |

### Backend Support Matrix

| Backend | Drying | Command |
|---------|--------|---------|
| ACE (Anycubic ACE Pro) | ✅ | `ACE_START_DRYING TEMP=<t> DURATION=<m>` / `ACE_STOP_DRYING` |
| Happy Hare | ✅ | `MMU_HEATER DRY=1 TEMP=<t> TIMER=<mins>` / `MMU_HEATER STOP=1` (TIMER is minutes; see [Happy Hare Specifics](#happy-hare-specifics)) |
| QIDI Box | ✅ | `ENABLE_BOX_DRY BOX=<n> TEMP=<t> END_TIME=<h>` / `DISABLE_BOX_DRY BOX=<n>`, with `SET_HEATER_TEMPERATURE` fallback when `box_extras` is absent. Write-path always enabled (commands verified vs QIDI firmware, #1030). |
| CFS (Creality K2) | ❌ | Not supported — CFS has no drying hardware |
| AFC (Box Turtle / OpenAMS) | ❌ | Not supported |
| AD5X IFS | ❌ | Not supported |
| Snapmaker U1 (SnapSwap) | ❌ | Not supported |
| Tool Changer | ❌ | Not applicable |

### QIDI Box Specifics

The QIDI Box dryer uses the printer's standard `heater_generic heater_box<N>` Klipper object — the same safety system (temperature limits, watchdog) that applies to any Klipper heater. The active session timer is tracked via `box_extras.box_drying_state.box<N>` (fields `dry_state` and `end_time`). Remaining time is computed as `(end_time - now) / 60` since there is no native remaining-minutes field.

See [QIDI_BOX_HEATER.md](QIDI_BOX_HEATER.md) for full reverse-engineering details: Klipper object schema, firmware command variants, config key spellings, and per-material drying tables.

### Happy Hare Specifics

Happy Hare's filament dryer is driven by the `MMU_HEATER` command and configured under `[mmu_machine]`. HelixScreen reads the dryer's object names from `configfile.settings` once at connect (`query_heater_config_from_config`), then tracks live state over the normal subscription push — **there is no polling**.

**What HelixScreen supports today:**

- **Commands.** `MMU_HEATER DRY=1 TEMP=<°C> TIMER=<minutes>` to start, `MMU_HEATER STOP=1` to stop. `TIMER` is **minutes** (float, `minval=0`), so sub-hour cycles are valid — the duration field in the overlay is a minutes field for this reason.
- **Box temperature.** Read from the `filament_heater` `heater_generic` object's live `temperature`.
- **Box humidity.** Happy Hare's `mmu` object deliberately does **not** republish temp/humidity (`mmu_environment_manager.get_status()` omits them by design); the client must read the environment sensor object directly. Humidity therefore comes from the **backing humidity chip** — `bme280` / `htu21d` / `sht3x` / `aht10` `<name>`, where `<name>` is the bare second token of the `environment_sensor` value (e.g. `temperature_sensor box` → `htu21d box`). Discovery subscribes these chips and requests the `humidity` field on all temperature sensors (only objects present in `objects.list` are subscribed, so this is safe for printers without them).
- **Per-unit / multi-MMU resolution.** Happy Hare has two mutually-exclusive enclosure forms: a **shared** enclosure (scalar `filament_heater` / `environment_sensor`) or **per-gate** hardware (plural `filament_heaters` / `environment_sensors`, one entry per gate, distributed across units for multi-MMU). `get_system_info()` resolves **each unit's** heater + sensor — the scalar form applies to every unit; the per-gate lists map each unit to the object at its first gate (`first_slot_global_index`). So a multi-MMU rig with distinct box sensors shows the correct temp/humidity per unit in the panel indicator and overlay.

**What is NOT yet supported (boundary):**

- **Per-gate / per-slot / per-lane *drying control*.** The control surface (`AmsEnvironmentOverlay`) and the `DryerInfo` model are **single-dryer-global**: start/stop drives the default heater (no `GATES=` selector), and the per-gate `drying_state` **array** is collapsed to a single "any gate active" boolean in `parse_mmu_state`. Independently drying specific gates (`MMU_HEATER … GATES=g1,g2`), per-gate countdowns, and the `HUMIDITY=` termination target are tracked post-1.0 in **#1026**.

> Note: the per-unit environment *readout* (above) is unverified on per-gate/EMU hardware — it is unit-tested (scalar + 2-unit per-gate) but we own no EMU rig. The QIDI Box (the common Happy-Hare-on-a-box case) is a single shared sensor.

### Adding Dryer Support to a New Backend

Override the five dryer virtuals in your `AmsBackend` subclass. At minimum implement `get_dryer_info()` — the UI polls this to drive the display. Implement `start_drying()` and `stop_drying()` to make the controls functional. `update_drying()` and `get_drying_presets()` are optional enhancements.

Dryer status flows into `AmsState::sync_dryer_from_backend()` the same way slot state does — no additional wiring is required in `AmsState`.

---

## Context Menu Actions

The `AmsContextMenu` (`ui_ams_context_menu.h`) provides per-slot operations:

| Action | Description | Availability |
|--------|-------------|------------|
| **Load** | Load filament from this slot | When slot has filament and not at toolhead |
| **Unload** | Unload filament from extruder | When filament is loaded to extruder |
| **Eject** | Eject filament from hub back to spool | When hub-loaded but not at toolhead, and `supports_lane_eject()` is true (AFC and Happy Hare) |
| **Spool Info** | View/edit slot properties (color, material, brand) | When slot has filament |
| **Spoolman** | Assign a Spoolman spool to this slot | Always |

The context menu also includes inline dropdowns for:

- **Tool Mapping**: Assign which tool number maps to this slot (if backend supports it)
- **Endless Spool Backup**: Set backup slot for runout (if backend supports it)

The tool dropdown is populated from `backend->get_tool_mapping()`.

The backup dropdown is populated from `backend->get_endless_spool_config()`, which is a
*group* relation, projected to this slot's single successor with
`helix::printer::endless_spool_backup_for()` - the same projection the panel's arrow renderer
uses (see [Endless Spool](#endless-spool-shared-model)). Its row visibility is the pure
predicate `AmsContextMenu::decide_show_backup_row(caps, has_relation)`:

- Not `available()` - hidden.
- `editable()` - shown, because there is something to write even before anything is set.
- Read-only **and** there is a relation to display - shown, but the dropdown gets
  `LV_STATE_DISABLED`.
- Read-only with no relation - hidden. This is the CFS and AD5X IFS case: the firmware picks
  the backup and publishes no mapping, so a visible dropdown could only ever read "None".

Backup options are tagged from `AmsBackend::endless_spool_backup_eligibility()`, so a backend
that tightens the rule (AD5X IFS) tightens the label too. The verdict is tri-state and the two
non-empty answers are tagged differently, because they mean different things to the user:

| Verdict | Suffix | Selectable? |
|---------|--------|-------------|
| `Eligible` | none | yes |
| `GradeDiffers` | `(different grade)` | **yes** - same polymer, filled variant; it will print |
| `Incompatible` | `(incompatible)` | no - `decide_backup_refused()` bounces it with a toast |

`GradeDiffers` is the only verdict where the label and the refusal deliberately disagree, and
it is why the enum exists: refusing a PLA-CF backup for a PLA lane would leave a print dead at
a runout with a usable spool one lane over, while waving it through silently hides that the
swap brings an abrasive, slower filament mid-print with nobody watching.

---

## Device Operations Overlay

The `AmsDeviceOperationsOverlay` (`ui_ams_device_operations_overlay.h`) consolidates device-specific controls:

### Fixed Actions (all backends)

| Action | G-code (varies by backend) | Description |
|--------|---------------------------|-------------|
| Home | `MMU_HOME` / `AFC_RESET` | Reset to home position (label follows `reset_button_label()`; AFC sends `AFC_RESET`, not `AFC_HOME`) |
| Recover | `MMU_RECOVER` / `AFC_RESET` | Attempt error recovery |
| Abort | `cancel()` | Cancel current operation |
| Bypass Toggle | `enable_bypass()` / `disable_bypass()` | Toggle bypass mode (if supported) |

### The Bypass Toggle row: one shared policy for all three surfaces

The table's last row names the backend calls, not the UI path. Three surfaces flip bypass,
and all three route through one shared policy object, `BypassToggleController`
(`src/ui/ui_bypass_toggle_controller.cpp`, extracted from the sidebar's handler in
03f784219): the AMS sidebar's toggle (`include/ui_ams_sidebar.h:195`), the home-panel
Bypass tile (`src/ui/panel_widgets/bypass_widget.h:33`), and this overlay's own switch
(`include/ui_ams_device_operations_overlay.h`). Each owns an instance and forwards its
click to `toggle()`, which runs every guard before touching the backend:

- **Print guard** (`:26-36`) — "Bypass cannot be changed while printing". Asked of the
  print lifecycle via `job_holds_machine()`, not `print_stats.state`: Preparing and Paused
  count too, because filament is already staged mid-path and a pre-start block may be
  homing.
- **Hardware-sensor refusal** (`:44-49`) — `has_hardware_bypass_sensor` means firmware owns
  bypass; the toggle only reports it.
- **Unload-first chaining** (`:62-79`) — enabling while a lane is loaded, on a backend that
  allows implicit chaining, first unloads the active lane and arms an `ams_action`
  observer; the UNLOADING→IDLE edge issues the `enable_bypass()`, an ERROR edge disarms
  without enabling. The controller observes the action subject itself while the chain is
  armed — before the extraction only the sidebar fed that subject, so a chain started from
  the home tile never saw its completing edge.

The overlay switch was the last holdout: its handler called `enable_bypass()` /
`disable_bypass()` directly, with its own hardware-sensor check but neither the print guard
nor the unload chain. On a backend whose `enable_bypass()` carries no filament-loaded
refusal of its own — AD5X IFS — a tap mid-print therefore reached the firmware, and on the
rest it stranded the loaded lane's filament behind the external feed.

Both switch surfaces (this one and the sidebar's) additionally re-publish
`ams_bypass_active` after `toggle()` returns. A `ui_switch` flips its own CHECKED state
*before* the handler runs, so a refusal leaves the widget claiming a state the backend
never entered — and `lv_subject_set_int()` does not notify when the value is unchanged,
which is exactly the refusal case, hence the explicit `lv_subject_notify()`. Both also bind
`disabled` to `job_holds_machine`, matching the home tile: the binding dims them, the
controller refuses regardless (`helix-screen ctl click` reaches a disabled widget's handler,
and so does a stale tap mid-transition). The switch's checked state follows the live
`ams_bypass_active` subject rather than an overlay-local snapshot, so an enable that
completes asynchronously behind the unload chain shows up when the backend actually gets
there instead of when the gcode was queued.

### Bypass visibility and the force override

Two pure predicates decide whether any bypass UI exists. Both had been inlined at four render
sites, where three of the four had already drifted apart, so neither may be re-derived locally:

| Predicate | Header | Rule |
|-----------|--------|------|
| `bypass_available(supports_bypass, force_override)` | `ams_bypass_policy.h` | `supports_bypass \|\| force_override` - folds the user's override into the firmware's report |
| `bypass_node_visible(supports_bypass, bypass_active, is_afc, always_show)` | `ui_bypass_spool_widget.h` | Additionally hides AFC's *virtual* bypass sensor while disengaged (#1229) unless `always_show` |

`bypass_available_for(bool)` and `bypass_node_visible_for(const AmsBackend*)` gather the live
inputs from `SettingsManager` and the backend; the render sites call the `_for` variants. The
firmware's own `supports_bypass` is never overwritten, so switching the override off restores
reality without a re-parse.

Settings keys (per-printer, under `df() + "ams/"`): `force_bypass_controls`,
`always_show_bypass_spool`. The **Enable Bypass Controls** row in `ams_device_operations.xml`
binds `hidden` to `ams_device_ops_fw_supports_bypass == 1`, so it self-hides on hardware that
already reports a bypass. Flipping it calls `AmsState::sync_from_backend()` +
`update_from_backend()` because both gating subjects are recomputed from the backend, not from
the setting, and neither moves on its own.

Where the override lands, by backend:

| Backend | `supports_bypass` | Override row shown | `enable_bypass()` with override on |
|---------|-------------------|--------------------|------------------------------------|
| AFC | `afc_defaults` caps, default `true` | no | Consults `bypass_available_for()` |
| AD5X IFS | `true` (`ams_backend_ad5x_ifs.cpp:135`) | no | Real command via `less_waste_external` |
| Happy Hare | Runtime from `[mmu_machine] has_bypass`; `false` until first status | Only when `has_bypass: 0` | Consults `bypass_available_for()`; `MMU_SELECT_BYPASS` runs |
| CFS | Converges on first full box frame: true (Fork: + payload `external` entry) | no | Consults `bypass_available_for()` — real `T<external>` on Fork, sensor-derived declaration on stock |
| ACE | Hardcoded `false` (`ams_backend_ace.cpp:44`) | yes | `not_supported` |
| Snapmaker | Hardcoded `false` (`ams_backend_snapmaker.cpp:240`) | yes | `not_supported` |
| Tool Changer | Hardcoded `false` (`:31`) | yes | `not_supported` |
| QIDI Box | Hardcoded `false` (`:193`) | yes | `not_supported` |

Happy Hare is the one backend where the override changes machine behavior rather than only the
UI: `cmd_MMU_SELECT_BYPASS` never checks `has_bypass`, it deselects the gear steppers and
reports gate -2 either way, while `has_bypass` defaults to `0` for `mmu_vendor: Other` (a QIDI
Box driven through Happy Hare reports exactly that) and is ANDed with the calibrated bypass
offset on type-A selectors.

### Bypass suppresses the pre-print filament gates

An engaged bypass feeds the extruder without passing through any slot, and
`AmsState::collect_available_slots()` deliberately does not emit one for it. Anything reasoning
over that vector therefore concludes that every tool in the file is unfed. Two gates sit on the
same Print tap and both did exactly that, so a bypass print was blocked by
`"T<n> has no filament loaded — this print will run out."` and then, once past it, by the
`"Color Mismatch"` dialog:

| Gate | Where | Input |
|------|-------|-------|
| Pre-flight empty-slot block | `PreflightValidator::validate()`, 4th parameter | `PrintSelectDetailView::recompute_preflight()` |
| Unresolved-tool / color-mismatch dialog | `PrintStartController::unresolved_tools_for()`, 3rd parameter | `PrintStartController::find_unresolved_tools()` |

Both take the flag as a **required** parameter rather than reading `AmsState` themselves, so
they stay pure and unit-testable and a new caller cannot silently omit it.

The input is `AmsState::any_bypass_active()`, which polls every backend's `is_bypass_active()`.
Two things it is deliberately **not**:

- **Not `AmsSystemInfo::current_slot == -2`.** The AFC backend sets that at
  `ams_backend_afc.cpp:2239` while parsing `bypass_state`, but nine later writes in the same
  file can overwrite it — including the mount-state derivation from #1229, which is
  intentionally unguarded so it cannot re-latch. `is_bypass_active()` returns the firmware's own
  report and is stable.
- **Not the `ams_bypass_active` subject**, which is derived from that same `current_slot == -2`
  and inherits the problem.

Engaging bypass moves no slot, so the per-slot delta scan never bumps `slots_version` — and
`slots_version` is the only thing that re-runs the detail view's cached pre-flight result.
`sync_from_backend()` therefore bumps it on the `any_bypass_active()` edge (both directions), or
engaging bypass with a file already open would leave the stale block in place. Verified on a
Voron/BoxTurtle with AFC's `virtual_bypass`: the log shows
`Bypass -> true, bumping slots_version` followed immediately by a recompute from `block=true` to
`block=false`.

Only the backends whose `is_bypass_active()` can return `true` reach any of this — the five
display-and-tracking rows below never suppress anything.

On the bottom five rows the override is display-and-tracking only. Their `is_bypass_active()`
returns a literal `false`, so `bypass_node_visible()` reaches the `!is_afc` branch and renders
the node; tapping it opens `show_external_spool_menu()`, which writes HelixScreen-side slot
metadata (`AmsState::set_external_spool_info()`) and sends nothing to the printer. The sidebar
toggle, however, is gated on the same `ams_supports_bypass` subject, so it appears too and then
fails with the backend's `not_supported`.

> **Adding a backend:** if the override is meant to engage a command the firmware can actually
> run, guard `enable_bypass()` with `bypass_available_for(system_info_.supports_bypass)` rather
> than `system_info_.supports_bypass` directly - that is what AFC, Happy Hare and the mock do.
> A hardcoded `not_supported` is the correct answer only when no command exists at all.

**Where the at-tap chain lives now.** The dialog half of the table above is the print-start
gate pipeline: `PrintStartController::run_gates_from()` iterates
`default_print_start_gates()` (`include/print_start_checks.h`) — six pure gates over a single
`gather_print_start_context()` snapshot, in order:

1. `insufficient_spool_weight` — spoolman remaining weight vs. the file's need
2. `bypass_engaged_lane_print` — bypass engaged **and** the file uses more than one tool;
   a single-tool bypass print is the legitimate bypass use and stays silent. "Single-tool"
   is the gcode-scan's used-tool count, not the filament palette size — slicers emit a
   full-profile palette (e.g. `PLA;ASA-GF;ASA-GF;PLA`) even for a file extruding from one
   tool, which made the palette-count form of this gate nag every such file (K2 CFS case).
   How that scan runs — the footer-read fast path and the persistent tools-used cache —
   is covered in `architecture/16-gcode-pipeline.md`
3. `unaccounted_toolhead_filament` — filament in the toolhead that no lane accounts for
4. `required_filament_present` — the empty-lane / runout dialog (at-tap sibling of the
   pre-flight block above); a single-tool bypass print skips lane truth entirely — its
   mapped lanes describe filament that is not being printed with
5. `unresolved_tools` — the color-mismatch dialog above
6. `material_compatibility` — file material vs. loaded spool, in two passes. First
   `FilamentMapper::materials_match()` decides whether the **polymer** is right; anything it
   rejects is a material mismatch and shows the "Material Mismatch" dialog. What it accepts
   then goes to `filament::grades_match()`, which decides whether the **grade** is right, and
   a difference there shows the separate "Filament Grade Mismatch" dialog
   (`MaterialMismatchDetail::grade_only`). Under an engaged bypass on a single-tool file both
   passes run against the **external spool** rather than the mapped lanes.

   The grade axis is whether the filament carries solid particles, not what the marketing
   calls it. `VARIANT_AFFIXES[]` in `src/printer/filament_variants.cpp` carries a `filled`
   column: CF, GF, AERO, LW, Wood, Marble, Metal and Glow are filled (abrasive and/or
   flow-altering); `+`, Silk, Matte, HS, HF and HT are not. So a file sliced ASA-GF against a
   loaded ASA spool warns, PLA against PLA+ does not. The dialog wording is directional -
   filled filament on an unfilled profile names the hardened-nozzle risk, the reverse only
   notes it will run slower and hotter than needed. Both are click-through warnings, and
   neither changes what the mapper ROUTES: `materials_match()` still treats the two grades as
   interchangeable when picking a lane

The two bypass suppressions this section describes are entries in that list: gate 4
(`required_filament_present`) and gate 5 (`unresolved_tools`, whose `unresolved_tools_in()`
keeps the `ctx.any_bypass_active` early-out). A gate that warns shows one dialog, and "Start
Anyway" resumes at the next entry — the old proceed-callback chain, flattened.

Gate 2 is the multi-color bypass case the old chain never covered: firmware (AFC's
`_check_bypass`, verified) refuses a lane load while bypass filament sits in the toolhead,
so a multi-color file with bypass engaged gets the "Bypass Is Active" warning instead of a
firmware error mid-print.

Gate 3 asks every backend the `toolhead_filament_unaccounted()` capability question. The
`AmsBackend` default returns `nullopt` — cannot determine — and the gate stays silent rather
than guess. AFC, Happy Hare, CFS, and AD5X IFS override it with verified signals; the AD5X
override trusts the extruder switch pair (`head_switch_seen_` / `head_switch_present_`) and
never `head_filament_` alone, which is conflated with the motion sensor and reads false on a
loaded-but-idle lane (see the warning in `include/ams_backend_ad5x_ifs.h`). With no bypass
engaged, any backend answering `true` produces the "Filament In The Toolhead" warning —
pull the stray filament or confirm to start anyway. Drive the scenario against the mock
with `HELIX_MOCK_AMS_STATE=unaccounted` under `--test`.

### Bypass companions: runout arming and the external lane

Two cross-cutting behaviors ride the same `any_bypass_active()` edge in
`AmsState::sync_from_backend()`. Both live in their own abstraction layers — AmsState only
notifies; per the vendor/layer rules no backend implements either one itself.

**Runout arming** (`FilamentSensorManager::on_bypass_active_changed`): filament fed through
the bypass passes no backend lane, so mid-print runout protection falls to the toolhead
sensor alone — and on firmwares that leave that sensor disabled outside their own filament
system (Creality's macros toggle it around every CFS operation; the K2 sits at
`enabled: false` between sequences), a bypass print would run with no protection at all.
Engaging bypass arms every RUNOUT-role sensor the firmware holds disabled
(`SET_FILAMENT_SENSOR SENSOR=<name> ENABLE=1`, bare name, same form the vendor macros use);
disengaging restores exactly what we armed. Firmware reports of a sensor being disabled
behind our back (vendor macro ran mid-bypass) drop it from the armed set, so the restore
never sends a command for state we no longer own. The user's monitoring switches (master
enable, per-sensor enable) gate the arming — it is a temporary firmware-state change, not a
settings change.

**External lane publish** (`AmsBackend::publish_external_spool_lane` +
`helix::ams::publish_external_lane`): the external spool is published as the lane one past
the last physical slot in the shared `lane_data` namespace, so OrcaSlicer's
MoonrakerPrinterAgent can select it as the "next tool over" (T4 beside T0–T3). Readers key
off the inner 0-based `lane` field (`filament_slots.md` §4), which is the slot count.
Triggered on bypass engage and on every external-spool identity change
(`AmsState::apply_external_spool_store`), on every backend — each override decides for
itself:

| Backend | Publishes | Namespace owner / source-verified caveat |
|---------|-----------|------------------------------------------|
| CFS | `lane{N+1}` via its own mirror store | ZMOD/stock never writes lane_data; the namespace is ours |
| AD5X IFS | `lane{NUM_PORTS+1}` via its own mirror store | same — ours alone |
| AFC | `T{N}` via a dedicated shared-namespace store (lazy, from `api_`) | AFC's plugin never publishes extern (`AFC_lane.send_lane_data` runs only for lanes with a tool mapping) and **deletes the whole namespace at boot** (its `delete_lane_data()`); our entry dies at AFC restart and is re-published on the next trigger |
| Happy Hare | `lane{N+1}` via a dedicated shared-namespace store | HH's plugin publishes gates only (`push_lane_data` in its Moonraker component), and its **boot-time cleanup deletes records with `lane >= num_gates`**; same die-at-restart, re-publish-on-trigger cycle |
| ACE / Snapmaker / QIDI / Tool Changer | no (default no-op) | `supports_bypass` is false — there is no external spool to publish |

The identity rule is shared in `publish_external_lane()`: a null or identity-less record
(no Spoolman id, no material, default-gray color) **clears** the lane rather than
publishing a phantom tray; pure black is a real pick and publishes.

### Dynamic Actions (backend-specific)

Each backend can expose dynamic device actions via `get_device_sections()` and `get_device_actions()`. The UI renders them as buttons, toggles, sliders, or dropdowns based on `ActionType`.

The section lists live in `src/printer/afc_defaults.cpp` (`afc_default_sections()`) and
`src/printer/hh_defaults.cpp` (`hh_default_sections()`):

| Backend | Sections |
|---------|----------|
| AFC | **Setup**, **Speed Settings**, **Toolhead**, **Maintenance**, **Hub & Cutter**, **Tip Forming**, **Purge & Wipe** (7) |
| Happy Hare | **Setup**, **Speed**, **Toolhead**, **Accessories**, **Maintenance** (5) |

There is no separate "Calibration" or "LED & Modes" section on AFC. The calibration
wizard, bowden length, LED toggles and quiet mode all live under **Setup**.
`AmsBackendAfc::get_device_sections()` drops **Tip Forming** whenever
`system_info_.tip_method != TipMethod::TIP_FORM`, which is the common case (the default
capability set is `TipMethod::CUT`), so a stock Box Turtle shows six.

Action counts are not fixed either. `afc_default_actions()` returns 26 static actions, and
`AmsBackendAfc::get_device_actions()` then adds one **Hub Distance** slider per lane and,
on a multi-extruder rig, swaps the single bowden / toolhead / toolhead-LED entries for
per-extruder ones. See the [AFC-Specific Features](FILAMENT_BACKEND_AFC.md#afc-specific-features) section for
details.

---

## Mock Mode for Testing

The `AmsBackendMock` simulates any of the supported backend types for UI development and testing.

### Activation

Mock mode is activated when `RuntimeConfig::should_mock_ams()` returns true (typically via the `--test` CLI flag). The factory method `AmsBackend::create()` automatically returns a mock backend in this case.

Pass `--real-ams` alongside `--test` to opt back out and drive a real backend (e.g. `AmsBackendHappyHare`) against the mock Moonraker client instead of `AmsBackendMock`. This is what makes backend-specific chokepoints reachable under `--test` — for example `AmsSubscriptionBackend::ensure_homed_then()`'s "Home printer first?" confirmation, which `AmsBackendMock` never goes near since it doesn't inherit `AmsSubscriptionBackend`. The mock Moonraker client only simulates a minimal, static `mmu` status for Happy Hare (`moonraker_client_mock_objects.cpp`'s `get_mock_mmu_status()`: 4 gates, a mix of loaded/empty, no operation state machine) — it is a plumbing harness for exercising backend code paths, not a UI development tool. Use plain `--test` + `HELIX_MOCK_AMS` (below) for that.

**`--real-ams` seeds Happy Hare only and does not compose with `HELIX_MOCK_AMS`.** The backend comes from mock hardware discovery, not from `HELIX_MOCK_AMS` — that variable is read inside `AmsBackend::create()`'s mock branch (`src/printer/ams_backend.cpp`), which `--real-ams` bypasses entirely. So `HELIX_MOCK_AMS=toolchanger` combined with `--real-ams` still swaps in a real `AmsBackendToolChanger`, but with zero seeded state — a silently empty panel, not a toolchanger simulation.

The seed also dispatches from the main thread (inside an `UpdateQueue` drain), while production delivers the same `mmu` payload from the libhv WebSocket event-loop thread. A threading bug in a backend's `handle_status_update` will not reproduce under `--real-ams`.

```bash
./build/bin/helix-screen --test --real-ams -vv
```

### Environment Variables

| Variable | Values | Default | Description |
|----------|--------|---------|-------------|
| `HELIX_AMS_GATES` | 1-16 | 4 | Number of simulated slots |
| `HELIX_MOCK_AMS` | `afc`, `box_turtle`, `boxturtle`, `toolchanger`, `tool_changer`, `tc`, `mixed`, `multi`, `torture`, `vivid`, `ifs`, `ad5x`, `ad5x_ifs`, `htlf_toolchanger`, `htlf_tc`, `htlf`, `snapmaker`, `snapswap`, `u1` | Happy Hare | AMS type to simulate |
| `HELIX_MOCK_AMS_STATE` | `idle`, `loading`, `error`, `bypass`, `unaccounted`, `grade` | `idle` | Visual scenario to simulate |
| `HELIX_MOCK_DRYER` | `1`, `true` | Disabled | Simulate integrated dryer |
| `HELIX_MOCK_DRYER_SPEED` | Integer | 60 | Dryer speed multiplier (60 = 1 real sec = 1 sim min) |

### Mock AFC Mode

```bash
HELIX_MOCK_AMS=afc ./build/bin/helix-screen --test
```

When AFC mock mode is enabled:

- Reports `AmsType::AFC` with type name "AFC (Mock)"
- Uses `PathTopology::HUB` (4 lanes merge through hub)
- Configures 4 lanes with realistic filament data (PLA, PETG, ABS, ASA)
- Sets AFC-specific device sections: Calibration, Maintenance, Speed Settings, LEDs & Modes
- Includes mock device actions: calibration wizard, bowden length slider, speed multipliers, lane tests, blade change, park, brush, motor reset, LED toggle, quiet mode toggle
- Uses `TipMethod::CUT`
- Editable endless spool with pre-configured backup mapping
- Supports auto-heat on load

### Mock Mixed Topology Mode

```bash
HELIX_MOCK_AMS=mixed ./build/bin/helix-screen --test
```

Simulates a real-world 6-toolhead toolchanger with mixed AFC hardware (based on production data):

- **Unit 0**: Box Turtle "Turtle_1" — 4 lanes, PARALLEL, lanes 0-3 → T0-T3, TurtleNeck buffers, no hub sensor
- **Unit 1**: OpenAMS "AMS_1" — 4 lanes, HUB, lanes 4-7 all → T4, per-lane hubs (Hub_1-Hub_4), no buffers
- **Unit 2**: OpenAMS "AMS_2" — 4 lanes, HUB, lanes 8-11 all → T5, per-lane hubs (Hub_5-Hub_8), no buffers
- Total: 12 slots, 6 physical toolheads
- Per-unit topology via `get_unit_topology()`
- 23 regression tests in `tests/unit/test_ams_mock_mixed_topology.cpp` validate this setup

### Mock Tool Changer Mode

```bash
HELIX_MOCK_AMS=toolchanger ./build/bin/helix-screen --test
```

- Reports `AmsType::TOOL_CHANGER`
- Uses `PathTopology::PARALLEL`
- Disables bypass mode
- Labels slots as "T0", "T1", etc.

### Mock AD5X IFS Mode

```bash
HELIX_MOCK_AMS=ifs ./build/bin/helix-screen --test
```

- Reports `AmsType::AD5X_IFS` with type name "AD5X IFS"
- Uses `PathTopology::LINEAR`
- 4 slots with bypass support
- Tool mapping enabled; endless spool `Unsupported` - the scenario clears
  `endless_spool_supported_` / `endless_spool_editable_`, not just
  `system_info_.endless_spool_enabled`. Clearing only the bit left the mock AD5X with an
  editable backup dropdown and endless-spool arrows the real backend does not have. The
  Snapmaker scenario clears the same pair for the same reason.

### Mock Realistic Mode

```bash
HELIX_MOCK_AMS_STATE=loading ./build/bin/helix-screen --test
```

Enables multi-phase operation simulation with realistic timing:

- **Load**: HEATING -> LOADING (segment animation) -> IDLE
- **Unload**: HEATING -> CUTTING -> UNLOADING (animation) -> IDLE
- Timing respects `--sim-speed` flag with +/-20-30% variance

### Mock-Specific Test Methods

The mock backend exposes additional methods for unit testing:

| Method | Description |
|--------|-------------|
| `simulate_error(AmsResult)` | Trigger a specific error condition |
| `simulate_pause()` | Set PAUSED state (user intervention required) |
| `resume()` | Resume from PAUSED state |
| `set_operation_delay(ms)` | Set simulated operation delay |
| `force_slot_status(slot, status)` | Force a specific slot status |
| `set_has_hardware_bypass_sensor(bool)` | Toggle hardware vs virtual bypass sensor |
| `set_endless_spool_supported(bool)` | Availability: `Available` vs `Unsupported`. Also sets `system_info_.endless_spool_enabled`, which is what `caps.enabled` reads |
| `set_endless_spool_editable(bool)` | Editability: `PerSlot` vs `ReadOnly` + `FirmwareManaged` (the shape CFS and a multi-unit MMU have). When read-only, `set_endless_spool_backup()` is rejected by the base with the translated restriction reason, not by the mock |
| `set_device_sections(sections)` | Set custom device sections for testing |
| `set_device_actions(actions)` | Set custom device actions for testing |

---

## Developer Guide: Adding a New Backend

### 1. Define the AmsType

Add a new value to `AmsType` in `ams_types.h`:

```cpp
enum class AmsType {
    // ... existing values ...
    MY_SYSTEM = 5  // New system type
};
```

Update `ams_type_to_string()`, `ams_type_from_string()`, and the `is_filament_system()` / `is_tool_changer()` helpers as appropriate.

### 1b. Declare the firmware default routing (only if it is not lane-per-tool)

`AmsBackend::firmware_default_routing()` answers which physical head a logical
tool routes to with **no remap applied** - the firmware's own default map. The
base implementation is lane-per-tool (tool N owns lane N), which is correct for
AFC, Happy Hare, klipper-toolchanger, CFS, QIDI and ACE, so most backends
override nothing.

Override only when the hardware genuinely disagrees:

```cpp
// Snapmaker U1: four physical heads, up to 32 logical tools -> [0,1,2,3,0,0,...]
[[nodiscard]] helix::FirmwareRouting firmware_default_routing() const override {
    return helix::FirmwareRouting::fixed_heads(NUM_TOOLS, 0);
}
```

`AmsBackendAd5xIfs` is the third shape: it publishes an arbitrary 16-entry
tool -> port table, so it builds `FirmwareRouting::head_for_tool` directly (ports
are 1-based there and `5` is the unmapped sentinel).

**This is not the live map.** `AmsSystemInfo::tool_to_slot_map` carries what the
firmware is doing right now, and it is not uniformly available - an AFC tracks it,
a U1 freezes `mapped_tool` at 1:1 while its real map lives in
`print_task_config.extruder_map_table`. Seeding from the live map therefore means
different things per backend; seed from the default map instead.

Three consumers read this and they must all agree: the mapping-card seed
(`FilamentMapper::use_current_assignments`), the wire filter
(`FilamentMapper::identity_filtered_remap`), and the runout lane scan
(`FilamentSensorManager`). Answering them differently is a silent bug - a
lane-per-tool identity read as a genuine remap, or a runout watch on lane 0.

### 2. Add Detection in PrinterDiscovery

In `printer_discovery.h`, add detection logic in `parse_objects()`:

```cpp
else if (name == "my_system") {
    has_mmu_ = true;
    mmu_type_ = AmsType::MY_SYSTEM;
}
```

Add any component discovery (lane names, tool names, etc.) as needed.

### 3. Implement the Backend Class

Create include/ams_backend_mysystem.h and src/printer/ams_backend_mysystem.cpp. Implement all pure virtual methods from `AmsBackend`:

**Required overrides:**

- `start()`, `stop()`, `is_running()` -- Lifecycle
- `set_event_callback()` -- Event registration
- `get_system_info()`, `get_type()`, `get_slot_info()`, `get_current_action()`, `get_current_tool()`, `get_current_slot()`, `is_filament_loaded()` -- State queries
- `get_topology()`, `get_filament_segment()`, `get_slot_filament_segment()`, `infer_error_segment()` -- Path visualization
- `load_filament()`, `unload_filament()`, `select_slot()`, `change_tool()` -- Operations
- `recover()`, `reset()`, `cancel()` -- Recovery
- `set_slot_info()`, `set_tool_mapping()` -- Configuration
- `enable_bypass()`, `disable_bypass()`, `is_bypass_active()` -- Bypass mode

**Optional overrides (with default implementations):**

- `clear_fault()` -- Clear a latched fault, bookkeeping only (default: forwards to `cancel()`)
- `recover_lane_position()` -- Physical retract of a stranded lane (default: NOT_SUPPORTED)
- `get_dryer_info()`, `start_drying()`, `stop_drying()`, `update_drying()` -- Dryer control
- `get_endless_spool_capabilities()`, `get_endless_spool_config()` -- Endless spool state. `set_endless_spool_backup()` is **not** an override point: it is non-virtual and owns every rejection. Supply `apply_endless_spool_backup()` (protected, transport only), `endless_spool_slot_count()` (protected, only if `total_slots` is wrong for you), and `endless_spool_backup_eligibility()` (only to tighten the default polymer-plus-grade rule; return `Eligible`/`Incompatible` only, unless your firmware genuinely has a soft case). `reset_endless_spool()` already works for any editable backend by looping the setter with -1 - override it only if your firmware has a real reset primitive. See § [Endless Spool](#endless-spool-shared-model).
- `get_remap_strategy()`, `remap_ready()`, `owns_tool_mapping_table()`, `get_tool_mapping()` -- Tool mapping. **Three questions, one spelling each.** `get_remap_strategy()` says HOW a user's tool->lane pick is carried out (`Native` writes your table, `GcodeRewrite` rewrites the job, `SnapmakerNative` is a firmware pre-print send, `None` means it cannot be). `remap_ready()` says whether that route is usable YET -- default true, override only where discovery gates it, as AD5X IFS does on `_IFS_VARS`. `owns_tool_mapping_table()` says whether you hold a tool->slot table for `ToolState` to adopt; the Snapmaker U1 answers **no** and still honors every pick, through its pre-print send, which is why this is not the same question as the first two. Ask them through `helix::printer::can_remap()` and `remap_is_persistent()` in `ams_remap.h` -- never by combining them at a call site, which is how one question came to have six answers that could disagree.
- `get_device_sections()`, `get_device_actions()`, `execute_device_action()` -- Device-specific actions
- `set_discovered_lanes()`, `set_discovered_tools()` -- Discovery configuration
- `supports_auto_heat_on_load()` -- Auto-heat capability
- `supports_lane_eject()` + `eject_lane()` -- Cold retract of a lane's filament back to the spool. Without the predicate the context menu never offers Eject, whatever `eject_lane()` does.
- `has_per_slot_loaded_authority()` -- Return true only when the firmware reports load state **per slot**. Leave it false when your per-slot answer is derived from an aggregate "current slot" pointer, or a mid-toolchange null will drop the highlight.
- `reset_button_label()` -- Sidebar Reset button text (default `"Reset"`; Happy Hare uses `"Home"`)

**The error seam.** All optional, all defaulted to "nothing", and a backend that skips the
whole group gets **no error dialog at all**, silently. Read § [Two error channels](#two-error-channels)
before implementing any of them:

- `classify_error()` -- Channel A: claim one gcode-response line and return an `ErrorEvent`. The router applies **no line filtering**, so every override must gate itself (AFC and Happy Hare take only `!!` lines via `helix::is_bang_line`; CFS deliberately takes only non-`!!` lines). Return `nullopt` to defer to the generic classifier.
- `current_error()` -- Channel B: the current actionable fault derived from backend **status**, consulted only by `AmsErrorBridge` on the rising edge into `AmsAction::ERROR`. Independent of channel A, not an alternative to it: AFC overrides both.
- `build_recovery_actions()` (protected) -- The buttons the user can tap for the current fault. **The caller already holds `mutex_`**, which is non-recursive: an override that locks deadlocks. The base returns an empty vector deliberately: `decide_presentation()` keys off `recovery_actions.empty()` to pick MODAL vs MODAL_WITH_RECOVER, so recovery is strictly opt-in.
- **Your dispatch path must pass `caller_surfaces_errors=false` when its `on_error` only logs.** `AmsSubscriptionBackend::ensure_homed_then()` and `dispatch_payload()` (`include/ams_subscription_backend.h`) take it as `std::optional<bool>`; unset means "derive from `on_error`". That derivation is wrong whenever the callback ends up in `handle_dispatch_error()`'s default -- log the message and reset the action to `AmsAction::IDLE`, which no user ever sees. Claiming the report there records the rejection for RPC dedup and silences Channel A's `!!` copy, so a failed macro reaches nobody. `scripts/check_gcode_error_ownership.py` gates the log-only shape at zero; the escape hatch is `// ERROR_OWNERSHIP_OK: <reason>`. See `RPC_ERROR_OWNERSHIP.md`.

**The toolchange narration seam.** Also optional; leaving it empty falls back to the
sidebar's legacy `AmsAction`-driven hardcoded step list, which is a valid choice:

- `toolchange_phase_template(op)` -- The ordered phase list per `StepOperationType`. Order it by **first narration**, not by macro name: a phase whose line fires twice re-reports the earlier index, and the step bar has no notion of a repeated step.
- `match_narration_phase()` -- Map a `//` narration body to a phase id. Loose substring needles are fine here; the text came from a macro's own `respond_info`.
- `match_bare_narration_phase()` -- Map an **unprefixed** console line to a phase id. Must match anchored line shapes, never loose substrings. The open console carries user-controlled gcode filenames, so a `cut` needle fires on `haircut.gcode`.

**Runout and spool-assignment routing.** Both default to something reasonable; override
only if your hardware model diverges:

- `recovers_filament_on_resume()` -- True when Resume itself re-feeds filament (Snapmaker U1 runs `AUTO_FEEDING` then `RESUME`). Such backends present Resume as the runout dialog's primary action and demote manual Load/Unload/Purge. Default false, which keeps Load prominent. That is correct for AFC, Happy Hare, and every basic runout sensor.
- `supports_per_tool_spool_assignment()` -- Whether each tool owns its own spool assignment. Default is `is_tool_changer(get_type())`; no backend currently overrides it.

### 4. Wire into the Factory

In `src/printer/ams_backend.cpp`, add cases to both `create()` overloads:

```cpp
case AmsType::MY_SYSTEM:
    return std::make_unique<AmsBackendMySystem>(api, client);
```

### 5. Add Mock Support

In `src/printer/ams_backend.cpp`, extend the `HELIX_MOCK_AMS` environment variable handling:

```cpp
if (type_str == "mysystem") {
    mock->set_my_system_mode(true);
}
```

Add corresponding `set_my_system_mode()` to `AmsBackendMock` if the new system has unique UI characteristics that need simulation.

### 6. Update AmsState (if needed)

If the new backend has special discovery requirements, update `AmsState::init_backend_from_hardware()` accordingly. For example, ACE supports both object-list detection (`ace` in `printer.objects.list`) and a REST probe fallback.

### 7. Add Tests

Write tests for:
- State parsing from Moonraker JSON
- G-code command generation
- Error handling and recovery
- Tool/slot mapping
- Path segment computation

See `tests/unit/test_ams_backend_happy_hare.cpp`, `test_ams_tool_mapping.cpp`, `test_ams_endless_spool.cpp`, and `test_ams_device_actions.cpp` for patterns.

---

## Spoolman Management & Spool Wizard

Beyond slot assignment, HelixScreen provides full Spoolman spool management:

- **SpoolmanPanel overlay** — Browse, search, edit, and delete spools with virtualized list (20-row pool)
- **New Spool Wizard** — 3-step guided creation: Vendor → Filament → Spool Details
- **Context menu** — Per-spool actions: Set Active, Edit, Delete
- **Edit modal** — Update weight, price, lot number, notes via PATCH

### Spool Wizard Architecture

The wizard (`SpoolWizardOverlay`) is a 3-step overlay:

1. **Step 0 — Select Vendor**: Search/filter vendors from Spoolman server, or create a new one via modal (`create_vendor_modal.xml`)
2. **Step 1 — Select Filament**: Filter filaments by selected vendor (`vendor.id` API param), or create a new one via modal (`create_filament_modal.xml`) with material from `filament::MATERIALS[]` database, color picker, temp ranges, weight
3. **Step 2 — Spool Details**: Remaining weight, price, lot number, notes — compact 2-column layout

Key patterns:
- **Modal forms** for vendor/filament creation (not inline) — keeps list scroll area maximized
- **Vendor filtering**: Filament API uses `vendor.id=X` (Spoolman's dot-notation filter syntax)
- **Color picker**: HSV picker + preset swatches, launched from filament creation modal
- **Atomic creation**: Creates vendor → filament → spool in sequence with best-effort rollback on failure
- **Row selection**: `LV_STATE_CHECKED` with `selected_style` (primary left border + elevated bg)

### Key Files

| File | Purpose |
|------|---------|
| `include/ui_spool_wizard.h` | Wizard overlay class declaration |
| `src/ui/ui_spool_wizard.cpp` | Wizard logic, API calls, callbacks |
| `ui_xml/spool_wizard.xml` | 3-step wizard layout |
| `ui_xml/create_vendor_modal.xml` | New vendor modal form |
| `ui_xml/create_filament_modal.xml` | New filament modal form |
| `ui_xml/wizard_vendor_row.xml` | Selectable vendor row (lv_button with checked style) |
| `ui_xml/wizard_filament_row.xml` | Selectable filament row (lv_button with checked style) |
| `src/ui/ui_color_picker.cpp` | Color picker modal (used by filament creation) |

The original spool-wizard visual test plan (2026-02-15) is no longer in-tree; treat the wizard steps above as the reference.

---

## Troubleshooting

### Common Issues by Backend

#### Happy Hare

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No multi-filament system detected" | `mmu` object not in Klipper | Verify Happy Hare is installed and `[mmu]` section exists in printer.cfg |
| Gate status all "Unknown" | Subscription not receiving updates | Check Moonraker connection, verify `printer.mmu` is subscribable |
| Tool mapping not updating | Stale TTG map | Try reset tool mappings (sends 1:1 mapping for all tools) |
| Bypass button disabled | Hardware bypass sensor detected | System auto-detects bypass via sensor, manual toggle not available |

#### AFC

| Symptom | Cause | Fix |
|---------|-------|-----|
| "No multi-filament system detected" | `AFC` object not in Klipper | Verify AFC-Klipper-Add-On is installed |
| Lane count wrong | Discovery mismatch | Check for both `AFC_stepper lane*` and `AFC_lane lane*` objects in `printer.objects.list` (OpenAMS uses `AFC_lane`) |
| Too many nozzles drawn | HUB unit map values treated as separate tools | Verify topology detection — HUB units always have tool_count=1 regardless of `map` field values |
| Hub sensors not updating | Hub name doesn't match unit name | OpenAMS uses per-lane hubs (Hub_1..Hub_N) — check hub-to-unit ownership in `unit_infos_` |
| No filament colors/materials | AFC version too old or no Spoolman | `lane_data` database requires v1.0.32+; assign spools in Spoolman |
| Device actions missing | Backend not returning sections | Verify AFC backend is connected (not mock) |
| Bowden length slider shows wrong range | Default 450mm being used | Hub data may not be received yet; wait for state sync |
| Quiet mode not toggling | G-code not recognized | Verify AFC firmware supports `AFC_QUIET_MODE` command |

#### ACE (Anycubic ACE Pro)

| Symptom | Cause | Fix |
|---------|-------|-----|
| ACE Pro not detected | Object not in list + REST probe failed | Verify a ValgACE/BunnyACE/DuckACE driver is installed; check `ace` in `printer.objects.list` and `/server/ace/info` endpoint |
| Stale state | Polling interval | ACE polls at 500ms; state may lag slightly |
| Dryer not controllable | Missing REST bridge | BunnyACE/DuckACE users must install ValgACE's `ace_status.py` Moonraker component |

#### Tool Changer

| Symptom | Cause | Fix |
|---------|-------|-----|
| No tools shown | `toolchanger` object missing | Verify klipper-toolchanger is installed |
| Wrong tool count | Discovery mismatch | Check that `tool T*` objects appear in `printer.objects.list` |
| "Uninitialized" status | Tools not homed | Run `T0` or `SELECT_TOOL TOOL=T0` to initialize |

#### AD5X IFS

| Symptom | Cause | Fix |
|---------|-------|-----|
| IFS not detected | Missing or outdated ZMOD firmware | Install ZMOD v1.7.0+ (v1.6.2 hard minimum). Verify zmod_ifs.py is installed and `_ifs_port_sensor_*` sensors appear in `printer.objects.list` |
| Colors/materials empty | `save_variables` not populated | Run IFS calibration wizard in ZMOD to initialize `less_waste_*` variables |
| Slots all EMPTY | Port sensors not subscribed | Check that `filament_switch_sensor _ifs_port_sensor_{1-4}` are present |
| Tool mapping wrong | Stale `less_waste_tools` | Check `save_variables.variables.less_waste_tools` — ports are 1-based, 5=unmapped |
| Bypass stuck on | `less_waste_external` = 1 | Set via ZMOD UI or `SAVE_VARIABLE VARIABLE=less_waste_external VALUE=0` |

### Debug Logging

Run with `-vv` (DEBUG) or `-vvv` (TRACE) to see backend-specific logging:

```bash
./build/bin/helix-screen --test -vv
```

All backends log with prefixes:

| Prefix | Backend |
|--------|---------|
| `[AMS Backend]` | Factory/creation |
| `[AMS Happy Hare]` / `[AmsBackendHappyHare]` | Happy Hare |
| `[AMS AFC]` | AFC |
| `[AMS ACE]` | ACE (Anycubic ACE Pro) |
| `[AMS ToolChanger]` | Tool Changer |
| `[AMS AD5X-IFS]` | AD5X IFS |
| `[AmsBackendMock]` | Mock |

### Error Result Codes

See `ams_error.h` for the full `AmsResult` enum. Key results:

| Result | Recoverable | Typical Cause |
|--------|-------------|---------------|
| `FILAMENT_JAM` | Yes | Filament stuck in path |
| `SLOT_BLOCKED` | Yes | Slot obstructed |
| `EXTRUDER_COLD` | Yes | Nozzle below load temp |
| `LOAD_FAILED` | Yes | Load did not complete |
| `UNLOAD_FAILED` | Yes | Unload did not complete |
| `BUSY` | No (wait) | Another operation in progress |
| `NOT_SUPPORTED` | No | Feature not available on this backend |
| `HOMING_FAILED` | Yes | Selector home failed |

`AmsErrorHelper` provides factory methods for creating user-friendly error messages with suggestions for each error type.
