# multiACE Filament Backend (Snapmaker U1 + ACE Pro)

[multiACE](https://github.com/decay71/multiACE) hangs 1-4 Anycubic ACE Pro units off a
Snapmaker U1's four heads. `AmsBackendMultiAce` (`include/ams_backend_multiace.h`) is the
Snapmaker U1 backend with those units added upstream of the heads: the U1 stays the printer,
the ACE units are inventory feeding it. Tracking issue: prestonbrown/helixscreen#1660.

> **Status: built from the source read, no hardware seen.** Written against multiACE 1.27b
> (`multiace/VERSION`) and its engine API document (ENGINE_API.md in its docs). The fixtures in
> `tests/fixtures/multiace/` are synthesised from its `get_status()`; a tester's capture
> replaces them. Load/unload/select, the dryer, coded errors, the path view, spool numbering
> and bay editing are wired; device actions and batch preload are not yet (see
> [Not yet](#not-yet); plan: `docs/devel/plans/2026-10-17-multiace-1660.md`).

## Detection

Discovery hands a U1 that also reports a plain `ace` object to `AmsType::SNAPMAKER`
(`src/printer/printer_discovery_parse.cpp#parse_objects`, #1426): no new `AmsType`, because
this is still a Snapmaker printer with all its pre-print, preferences and resume behaviour.
The variant is chosen in the factory: `AmsBackend::create()` takes the discovered hardware,
and `multiace::present(hw)` (`filament_detect` plus an object named exactly `ace`) builds
`AmsBackendMultiAce`, anything else the plain U1 backend. `ace_device` (DnG-Crafts U1-Ace),
`filament_hub` and `ace_instance_N` are other ACE drivers and do not match.
`AmsBackendSnapmaker::required_status_objects` subscribes `ace` under the same predicate.

The engine contract is checked at run time, from status: until a frame shows
`api_version == 1` and `mode` `multi` or `head`, the backend answers exactly as the plain U1
does (no ACE units, the stock feed path). `normal` mode swaps the stock firmware files back
and drives no ACE. A higher `api_version` is logged once and treated the same way. The mode
can change with no restart (`ACE_RUN_MODE_SWITCH`), so the units are rebuilt on every frame
that carries `mode`, the per-head flags, `head_source` or `aces`.

## Data model

| Global slot | Meaning | Unit |
|---|---|---|
| 0..3 | U1 head h, exactly as the U1 backend has it | unit 0, PARALLEL |
| `4 + 4k + s` | ACE unit k (`aces[k].idx`), bay s | unit k+1, "ACE k+1" |

`multiace::bay_global()` / `multiace::bay_of()` (`include/multiace_status.h`) are the only
translation. Every inherited per-head array is bounded to the four heads, so bay indices
never reach the U1 logic.

Which heads an ACE feeds (`multiace::head_feed`, mirroring multiACE's `head_uses_ace`): a
`head_manual` head never; a `head_feeder` head only outside head mode; every other head
always. Which bay can feed which head (`multiace::candidate_bays`, `head_for_bay`):

| Mode | ACE unit topology | Bays head h can load |
|---|---|---|
| head | HUB, `hub_tool_label` = the head wired to it (`head_ace`) | the four bays of `head_ace[h]`; the engine refuses any other unit (code 207) |
| multi | PARALLEL | bay h of every unit |

A bay's `mapped_tool` and `extruder_name` are the head it feeds. The head's routed bay is
`head_source[h]` (null: unloaded); that bay reads LOADED. The head lane's own colour and
material stay the U1's, from `print_task_config`, which multiACE stamps on every load.

## Status parse

`multiace::parse_ace_status` (`src/printer/multiace_status.cpp`) is pure and delta-shaped:
every field optional, an absent key unchanged, a present one replacing what was held.
Per-head dicts are keyed `"0".."3"`; any other key, or a value of the wrong type, leaves that
head at its default. `head_source` color is bare hex: `""` unknown, `"000000"` a declared
black. A bay's raw `[0,0,0]` is black only beside a material. Bay status words go through the
same reader as the ACE backend (`helix::ace::slot_status_from_string`), with `gate_status[s]`
answering presence when the word is unrecognised; the dryer goes through
`helix::ace::apply_dryer_state` with the ACE Pro's range (`include/ace_slot_parse.h`).

On an ACE-fed head the feed channel's `filament_detected` is the ACE gate (multiACE shadows
the stock filament_feed.py with its own filament_feed_ace.py), not a spool at the head's port, so the
base's `head_is_upstream_fed_locked()` hook turns off that head's PREP sensor, insert presence
and insert notice. The toolhead switch and the `load_finish` latch stay the head's truth.

## Commands

While the engine contract is in force every head goes through the engine's commands,
feeder and manual heads included: the engine owns the feed state machine for all four.

| Operation | ACE-fed head | Feeder / manual head |
|---|---|---|
| Load head lane h, head mode | `ACE_LOAD_HEAD HEAD=h` (the engine picks the wired unit and the head's last bay) | `ACE_LOAD_HEAD HEAD=h` (side feeder) |
| Load head lane h, multi mode | `ACE_LOAD_HEAD HEAD=h ACE=k SLOT=s` from `head_source[h]`; refused when none is recorded, since a bare load takes the globally active unit | `ACE_LOAD_HEAD HEAD=h` |
| Load bay (k,s) into its empty head h | `ACE_LOAD_HEAD HEAD=h ACE=k SLOT=s`, both always sent | n/a |
| Same, h holding filament | `ACE_UNLOAD_HEAD HEAD=h` alone; the load follows once a frame shows the head empty | n/a |
| Load a bay while a print is printing or paused | refused, nothing sent: the engine swaps mid-print as the sliced file asks | |
| Unload h, or its routed bay | `ACE_UNLOAD_HEAD HEAD=h` | `ACE_UNLOAD_HEAD HEAD=h` (tip-pull) |
| Select a bay | `T<h>` when it is h's routed bay, refused otherwise | |
| Batch unload covering every loaded head | `ACE_UNLOAD_ALL_HEADS` | |
| Batch unload, part of the loaded heads | `ACE_UNLOAD_HEAD` per head, one script | |

`ACE_UNLOAD_ALL_HEADS` is sent only when the selection is every loaded head, because the
engine's own loop (the _unload_all_heads command handler in its ace.py module) walks all
four heads and unloads every one whose motion sensor sees filament — ACE-fed, feeder and
manual alike, and it counts a feeder head's tip-pull as a successful unload. Feeder and
manual heads in the selection are therefore covered by the macro, not skipped by it.
| Batch load | refused when any selected head is ACE-fed | `ACE_LOAD_HEAD` per head, one script, after every head passes the single-load checks |
| Dry unit k | `ACE_DRY ACE=k TEMP=t DURATION=m`; stop `ACE_STOP_DRYING ACE=k` | |

`ACE_LOAD_HEAD` answers OK and feeds nothing into a head that already holds filament, and
returns silently for a bay whose gate is empty. So the backend refuses an empty bay before
sending anything, and swaps a bay into an occupied head in two steps: the unload goes
alone, and the load waits (`AmsBackendMultiAce::advance_pending_load`) for a frame showing
`head_source[h]` null and the head's load latch clear. The unload's OK is not that proof;
it opens a 15 s window, and a head that has not emptied when it closes drops the load, even
if the frame that finally shows it empty arrives later. So does the
engine leaving multi/head mode, or a print holding the heads by the time the head empties
(the shared print-active rule, so a start this screen is preparing counts before the printer
reports it);
each of those raises kEventError naming the bay. A failed unload, or another filament op
that actually dispatches (a tool change and a tool park included), drops it at once; an op
that is refused before sending changes nothing, the pending load included. A head loaded
with no recorded
`head_source` refuses a bay swap outright: the engine has no bay to return that strand to. `ACE_SWAP_HEAD` would do both in one
command, but its behaviour outside a print is unconfirmed. `needs_unload_before_load()` is
false for a bay: the screen's swap ladder would send `T<h>`, which loads no bay.

Resume after a failed load is held as on a stock U1 (`FILAMENT_BACKEND_SNAPMAKER_U1.md`,
"Runout and Resume"): an engine-loaded head ends at `load_finish` like a stock one, so that
gate applies unchanged, and the engine's own `head_source[h].load_failed` (rising edge)
marks the head's load unconfirmed too. While the engine is active Resume never loads a head
itself (`resume_may_refeed_locked()`): the stock `AUTO_FEEDING` is not the engine's load, so
a held head is refused with a pointer to the filament panel instead, and so is a runout,
which on a stock U1 refeeds
(`src/printer/ams_backend_multiace.cpp#resume_may_refeed_locked`).

An ACE head's unload ends at `preload_finish`, not `unload_finish`: multiACE's feed unload
leaves the channel there with the filament back in the ACE. The base's
`preload_finish_ends_unload_locked()` hook makes that state close the unload when the
channel's last in-progress state was an `unload_*` one, for the op lifecycle, the
unloaded-lane grace and the batch cursor alike. A feeder head still waits for
`unload_finish`.

The dryer is per unit: unit 0 (the heads) has none, each ACE unit has the ACE Pro's
(35-55 C, 12 h), with the range check and the stop-and-restart rule shared with the ACE
backend (`helix::ace::check_dryer_request`, `helix::ace::restart_request`).

## Errors

multiACE raises its own faults in the U1's coded form, id 525 with a 200-band code and the
head as index (multiACE's `_ace_error` in its ace.py), so they arrive as `!! LLLL-0525-000H-02CC <message>`.
`AmsBackendMultiAce::classify_error` files them as a filament fault on that head, with the
engine's own message. The engine also sends index 0 for a fault tied to no head (a malformed
command, code 200), so index 0 carries a toolhead label only when the message names head 1.
Idle, the fault is a refusal of the command the user just sent. Every other line goes to the U1 backend's classifier.

## Path view and numbering

The overview and the unit view both read one rule, generic in
`compute_system_tool_layout`: a lane or unit naming an extruder a placed nozzle already
belongs to shares that nozzle, whatever topology placed it. Unit 0 places the four head
nozzles from the heads' extruder names, so a head-mode ACE unit (HUB, all four bays naming
one head's extruder) draws onto its head's nozzle and a multi-mode unit (PARALLEL, bay s
naming head s) onto all four: one physical head is one nozzle everywhere (4 on this rig,
never 8 or 20).

A routed head lane's spool lives in its bay, so `upstream_slot(h)` answers the routed bay
and `spool_display_number()` resolves through it: a routed head wears its bay's number and
the badges on lanes holding their own spools run 1..N with no gap or repeat. The unit view
draws the upstream lane's name as a small caption on the fed head's column
(`helix::ui::upstream_lane_caption`, "ACE 2" + middle dot + bay number), on the row a prep
sensor would occupy - an ACE-fed head has no prep sensor of its own.

Filament use follows the same rule. `slot_for_extruder(h)` is `head_source_slot(h)`, so
what an ACE-fed nozzle extrudes comes off its routed bay's spool, re-read on every
delta so a mid-print swap charges the bay now feeding; a head with several bays wired and
none routed charges no slot. Bays appear with the first engine frame, after the backend
is registered, and `AmsBackendRegistry::ensure_sinks()` gives them consumption sinks then.

## Bay identity

A bay is edited the same way a head is: the editor opens on any bay, the edit
lands on that bay's `SlotInfo`, and Clear Spool clears it. The engine's own
table (`aces[k].slots[n]` in every frame) is the bay's machine reading — the
role a head's RFID tag plays — filed as a vendor-cache record on the bay's lane
and restated on every rebuild. A user edit files its declaration on the same
lane, which outranks the engine's reading for the fields it names, exactly as a
head's edit outranks its tag; after a restart the stored record (the shared
`lane_data` override store, keyed by global slot index) comes back as the
user's own word and still wins. Clearing a bay drops the override and the lane's
records and lets the engine's reading show again — the same clear semantics a
head with a tag has.

A bay edit also clears on the hardware event, the same rule a head's RFID tag
runs: every frame's bay identity (uid, else material and colour) goes through
the shared `SlotFingerprintTracker`, and a change the insert rule reads as a
different spool clears the override and lets the engine's table show through.
A bay entry stating no identity — an empty bay, or an entry the frame omits —
is no signal: the record waits for evidence, so a bay that empties and refills
with the same material and colour keeps the edit.

`AmsBackendMultiAce::apply_user_edit` / `sync_external_identity` /
`clear_slot_override` route by unit: a bay index resolves through
`multiace::bay_of()` into its own unit's `SlotInfo` and the shared override
machinery the other backends use; nothing is written to the engine, whose own
slot-overrides table never sees HelixScreen's edit (engine-side reconciliation
is a later phase). A head index keeps the base's path, which also writes the
head's firmware record. Temps an engine frame never carries are restated from
the stored record, the only place they live.

## Mock mode

`HELIX_MOCK_AMS=multiace` (pair with `HELIX_MOCK_PRINTER=snapmaker_u1`) is mock HARDWARE:
`MoonrakerClientMock` lists the U1 feed objects plus the plain `ace` object and serves
engine-shaped `ace` frames from `HELIX_MOCK_MULTIACE_MODE` (head default, multi) and
`HELIX_MOCK_MULTIACE_UNITS` (1-4), so real discovery builds this backend and runs against
them. In head mode the units wire the heads 1:1 (head h to unit h) and the heads past the
unit count are side-feeder heads; head 1 starts routed to ACE 1 bay 1. `ACE_LOAD_HEAD` /
`ACE_UNLOAD_HEAD` answer by routing (or clearing) and walking the head's channel_state, an
unload parking at `preload_finish`; a feeder head loads from its side port (no route) and
`ACE_UNLOAD_ALL_HEADS` unloads every head whose sensor sees filament, feeder heads
included. `tests/unit/test_ams_mock_multiace_parity.cpp` pins the frame to the real
parser and backend. Full knob table: `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` under
`HELIX_MOCK_AMS`.

## Not yet

- A print started from another client (Mainsail, Fluidd, the slicer) while a bay swap's
  unload runs: if its start has not reached a status frame by the frame that shows the head
  empty, the deferred load is still sent, and Klipper queues it behind the start. This screen
  has no signal for a start it did not make until the printer reports it.
- The engine's own spool table does not see a bay edit made here: reconciliation through
  multiACE's slot-overrides file (the Moonraker file API) is a later phase.
- The `multiace_event` console watcher (`multiace::parse_event_line` exists), the spool
  table and bindings, device actions (quad replenish, head rewiring) and `ACE_PRELOAD`
  batch loads.
