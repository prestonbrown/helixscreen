---
paths:
  - "src/printer/ams_*"
  - "include/ams_*"
  - "src/printer/filament_*"
  - "include/filament_*"
---
# Filament Systems

Read `docs/devel/FILAMENT_MANAGEMENT.md` before touching a backend: the multi-backend
architecture, slot metadata, filament-op dispatch, endless spool, mock mode, and the
add-a-backend guide. One leaf doc per backend (`docs/devel/FILAMENT_BACKEND_*.md`)
carries its protocol, data sources, G-code commands, topology and capability table.

- `AmsState` never names a backend; vendor knowledge stays inside its `AmsBackend*`
  class (`.claude/rules/vendor-abstraction.md`).
- **CFS on K1:** read `docs/devel/CREALITY_CFS_INTERNALS.md` first. `BOX_*` command
  semantics, deferred-failure and resume traps, staged loading, serial timeouts.
- **MedusaHC is not a backend.** It is a klipper-toolchanger printer plus dock sensors
  that outrank `toolchanger.tool_number` and a servo feeder:
  `docs/devel/FILAMENT_BACKEND_MEDUSAHC.md` with `docs/devel/FILAMENT_BACKEND_TOOLCHANGER.md`.
- Slot metadata store: `docs/devel/FILAMENT_SLOT_METADATA.md`. The public wire format
  is `docs/specs/filament_slots.md`.
