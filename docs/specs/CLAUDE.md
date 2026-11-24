# docs/specs/CLAUDE.md

This directory holds **public, vendor-neutral specifications** that describe
conventions HelixScreen participates in alongside other tools. They are
written for third-party adopters (firmware vendors, slicers, scales,
spool-tracking tools), not as HelixScreen implementation docs. In-flight
design work does not live here — `docs/devel/plans/` is the home for that;
this directory is published conventions only.

## Current specs

- [`filament_slots.md`](filament_slots.md) — the `lane_data` Moonraker-DB
  filament slot metadata convention. Written by AFC, Happy Hare, and
  HelixScreen; read by OrcaSlicer 2.3.2 and later (verified at 2.5.2;
  2.5.0 and later also read `filament_id`). Happy Hare writes `lane_data`
  directly via its Moonraker component (`push_lane_data`), and OrcaSlicer
  prefers that over the live `mmu` object. Since v2.0 it follows the Lane Status Surface RFC: one writer per
  machine, records carrying `writer` / `lss_version` / `lane_name` / `loaded`,
  and `null` for unknown values.

## Style

Specs in this directory should:

- Credit originators and cite authoritative references
- Avoid HelixScreen-internal implementation details (link to `docs/devel/` for those)
- Document schemas, field semantics, and conformance expectations
- Use JSON examples liberally
- Include a changelog section
