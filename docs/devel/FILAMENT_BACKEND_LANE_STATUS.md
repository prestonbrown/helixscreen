# Lane Status Surface backend

`AmsBackendLaneStatus` (`include/ams_backend_lane_status.h`) reads any filament
system that publishes the Lane Status Surface, lane-status-spec RFC 0001
(draft 0.1, [prestonbrown/lane-status-spec](https://github.com/prestonbrown/lane-status-spec),
branch `rfc-0001`). It needs no knowledge of the producer: a firmware mod that
adopts the spec gets a filament panel here with no backend of its own
(prestonbrown/helixscreen#1908).

The field reading is pure and lives in `include/lane_status_parse.h`
(`helix::lane_status`); the backend holds the subscription, the record poll and
the commands.

## Detection and ranking

Discovery records a `lane_status` object in `printer.objects.list`
(`PrinterDiscovery::has_lane_status()`). It claims the printer only in
`register_detected_ams_systems()`, after every native backend and fallback
(Snapmaker, INDX) and before the bare multi-extruder tool-changer arm. So:

- a vendor backend whose objects are present always wins (AFC, Happy Hare,
  OpenAMS, ACE, CFS, IFS, QIDI);
- a status-only producer that wraps a system read natively (a U1, say) leaves
  it to that backend, which is what RFC 0001 §4 asks of a consumer;
- `lane_status` is the reader for a producer nothing else knows.

`kind` and `wraps` are read but do not move the ranking, because discovery runs
on the object list, before any status exists.

## Data sources

| Source | Read | Gives |
|---|---|---|
| `lane_status` object | subscribed by name, the 12 keys of SPEC.md §2.1 | lanes and their order, current lane, state, error, producer name and version, kind, capabilities |
| `lane_data` namespace | `database_get_namespace`, at start and every 10 s | per-lane identity and presence |

Status updates carry only the keys that changed, so they are merged over the
last full view before parsing. A key that is absent, null or the wrong type
reads as its empty value; keys the spec does not define are ignored. A
`spec_version` whose major is not `0` is not read at all: no lanes, no
commands.

Lane names must match `[A-Za-z0-9_]+` (SPEC.md §2.2); one that does not, or a
repeat, is dropped, which is also what makes every lane name safe as a G-code
value. `lane_status.units` names units but not which lanes each holds, so the
lanes draw as one unit, named after the only unit when there is one.

## Records

A record joins a lane by its `lane_name`; the outer key is advisory and
`lane` is the slicer's index, which RFC 0001 lets follow the tool number
rather than the display order. The writer rule is Part B §8's: a record whose
`writer` names another producer is skipped, a legacy record (no `writer`) is
read only for a lane the producer wrote nothing for and then joins by `lane`,
and a record with an unknown `lss_version` major is skipped.

| Record field | Lands on |
|---|---|
| `loaded` | presence (Sensed): AVAILABLE / EMPTY, UNKNOWN when unstated |
| `material` (or `filament_type`) | `material` |
| `color` | `color_rgb` (`#RRGGBB`, `RRGGBB`, `RRGGBBAA`) |
| `vendor_name`, `name` | `brand`, `spool_name` |
| `nozzle_temp`, `bed_temp` | the slot's temperatures |
| `spool_id` | `spoolman_id` |
| `remaining_weight_g`, `total_weight_g` | the weights |
| `tool` (`"T3"`) | `mapped_tool` |

The record's identity is filed as the lane's Firmware source and presence as
Sensed, then laid back on the slot through `apply_resolved_lane()`.
`current_lane` makes its lane LOADED. `filament_id` and `extruder_index` are
not used: nothing on screen shows them.

## Commands

| Action | Sent | Offered when |
|---|---|---|
| Load | `CHANGE_TOOL LANE=<lane>` | `kind` is `controller` and `capabilities` lists `load` |
| Unload | `TOOL_UNLOAD LANE=<lane>` | `kind` is `controller` and `capabilities` lists `unload` |

Everything else (select, tool change, reset, recover, cancel, bypass, tool
mapping) is refused as not supported.

**The producer owns its records** ("one writer per machine", SPEC.md §5), so
this backend writes nothing to `lane_data`: edits and Clear are refused, and a
weight or Spoolman update is not persisted.

## Mock

`HELIX_MOCK_AMS=lane_status` (`src/mock/moonraker_client_mock_lane_status.cpp`)
publishes a producer and its records; `HELIX_MOCK_LANE_STATUS_KIND=status_only`
turns it into a status-only producer. See `MOCK_ENVIRONMENT_VARIABLES.md`.

## Not yet read

The `afc` compatibility profile's `lane_status_kind` marker (D3), the
`SET_COLOR` / `SET_MATERIAL` / `SET_MAP` / `LANE_UNLOAD` capabilities, and
unit membership, which the spec does not publish.
