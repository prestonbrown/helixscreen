# Native OpenAMS Filament Backend

HelixScreen integrates with `klipper_openams` directly through the versioned
`oams_manager` status API. This backend has no AFC runtime, schema, object, or
G-code dependency. AFC's separately supported `AFC_OpenAMS` hardware remains
part of the AFC backend and is not used by this path.

## Discovery and precedence

`oams_manager` in `printer.objects.list` selects `AmsType::OPENAMS`. Discovery
applies this after scanning the complete list, so it wins deterministically if
stale or intentionally co-installed AFC objects are also present.

Moonraker subscribes only these `oams_manager` fields:

```text
api_version schema ready commands lanes units groups
```

The backend accepts API version 1 with schema `openams.manager` and fails
closed for missing or unsupported versions.

## Status model

OpenAMS publishes a full nested snapshot:

- `lanes[]` are independently operated FPS paths;
- `units[]` belong to a lane, carry a stable family `kind`, and declare a
  generic `topology` (`hub`, `linear`, `parallel`, or `mixed`);
- `units[].slots[]` carry globally unique remote IDs plus ready/loaded state;
- `groups[]` map tool/material names to candidate remote slot IDs.

HelixScreen flattens the slots into its global slot index while retaining the
remote slot ID, group, and lane for command dispatch. Rendering branches on
`topology`, never on a family name; this lets future feeder families reuse the
same API. A snapshot containing different unit topologies renders as mixed
topology. The parser supports one or several lanes and does not assume four
slots per unit. Unknown topology values fail closed.

## Operations

The backend uses command names advertised by the manager instead of device
primitives:

| Operation | v1 command |
|-----------|------------|
| Load slot | `OPENAMS_LOAD GROUP=<group> SLOT=<remote-id>` |
| Safe unload | `OPENAMS_UNLOAD` |
| Cancel | `OAMSM_LOAD_FILAMENT_CANCEL` |
| Reset/recover | `OAMSM_CLEAR_ERRORS` |

Load and unload remain pending until Moonraker completes the long-running
macro call. Callbacks defer state changes through the backend lifetime token;
background callbacks never update LVGL directly. The caller owns surfaced RPC
errors, while this backend logs the failure and clears its optimistic action.

## Capabilities

| Capability | Behavior |
|------------|----------|
| Per-slot loaded authority | Yes; `slots[].loaded` is authoritative |
| Multiple FPS lanes | Parsed; multiple simultaneous loaded lanes produce no single aggregate current slot |
| Mixed unit families | Yes; unit `kind` selects topology without a vendor-specific parser |
| Bypass | No |
| Tool mapping edits | No; `T<n>` group names provide the read-only map |
| Slot metadata edits | Local `FilamentSlotOverrideStore`, published through the shared `lane_data` format |
| Firmware slot identity | No v1 write API; local overrides win |

## Tests

`tests/unit/test_ams_backend_openams.cpp` covers AFC-independent discovery,
precedence when AFC objects coexist, mixed topology, advertised command use,
partial Moonraker subscription updates, and unsupported-version rejection.
