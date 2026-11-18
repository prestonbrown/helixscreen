# ACE 2 Pro Test Rig on the Voron V0

**Goal:** put a real ACE 2 Pro on a fleet printer and use it to verify, and then fix,
HelixScreen's ACE support. Until now every ACE path has been checked against user captures
or driver source; the AFC ACE parse (f52867aab3) has never seen hardware at all.

**Driver choice:** AFC's DEV branch first (Preston's preference, and the only one of
AFC / Happy Hare / a new backend that exists today). Kobra-S1/ACEPRO second, on the same
rig, to cover `AmsBackendAce` Path 3 with an ACE 2. Background on every driver:
[`printer-research/ANYCUBIC_ACE_DRIVER_LANDSCAPE.md`](../printer-research/ANYCUBIC_ACE_DRIVER_LANDSCAPE.md).

**Printer:** the Voron V0 (192.168.1.133, Pi 4, Kalico, SHT36 over CAN; fleet notes in
memory `reference_voron_v0_test_device`). It has no filament system today, and it has the
toolhead sensor AFC requires. The AD5M Pro is a possible second host later; Forge-X runs
FlashForge's Klipper fork and the root partition is nearly full, so AFC there is its own
experiment.

## What the V0 has (read over Moonraker, 2026-11-18)

| Object | Pin | Today |
|---|---|---|
| `filament_switch_sensor toolhead_sensor` | `^sht36:LIMIT_1` | at the extruder; `pause_on_runout: False`. Becomes AFC's `pin_tool_start` |
| `filament_switch_sensor filament_runout_sensor` | `gpio16` (SKR Pico) | pauses on runout. Physical position to confirm; candidate hub or post-combiner sensor |
| `extruder` | `sht36:EXT_STEP` | `rotation_distance: 21.9`, `max_extrude_only_distance: 450` |

No cutter, so swaps rely on tip forming (`AFC_form_tip`).

## Phase 0: hardware (Preston's hands)

- [ ] Photo of the hub's connectors and cable ends. Settle whether the hub cable carries
      the ACE's RS-485 through to the printer, or only the hub sensor. No driver surveyed
      mentions a hub device on the ACE 2 bus.
- [ ] Multimeter the ACE 2 connector against Kobra-S1/ACEPRO's `img/connector.png`, and find
      out whether it carries 24 V, before the BMCU RS485-to-USB adapter goes on it.
- [ ] Mount the ACE and the hub; route one PTFE run from hub to the V0 extruder; measure it.
- [ ] Hub sensor: if it is a plain switch, wire it to a free Pico or SHT36 input (or reuse
      `gpio16` if the runout switch moves to the hub).

## Phase 1: AFC DEV on the V0 (remote; each install or restart confirmed with Preston)

- [ ] Back up `printer_data/config` and the Moonraker database (memory
      `feedback_preserve_moonraker_db_on_printer_reverts`).
- [ ] Install AFC from `DEV` with its installer, unit type ACE, `V2 Pro`. sudo needs
      Preston's password.
- [ ] `[AFC_ACE2 Ace2_1]`: combined mode, `serial_port` pinned by-id rather than `auto`
      (the BMCU adapter may not be a CH343), `[AFC_extruder]` `pin_tool_start` = the SHT36
      sensor. Stock ACE 2 firmware.
- [ ] `AFC_ACE_STATUS UNIT=Ace2_1`, then `AFC_ACE_CALIBRATE` per lane.
- [ ] Tip forming tuned until a swap unloads cleanly past the hub.
- [ ] One two-color print end to end.

## Phase 2: HelixScreen under AFC

A dev build with the ctl server (`make deploy-pi PI_HOST=192.168.1.133 PI_USER=pbrown`; the
packaged install has none). Capture each `AFC_ACE2` status frame we see into
`printer-research/` as fixtures for tests.

- [ ] Detection picks `AmsBackendAfc`; the unit draws as the ACE (`ams_unit_shell.cpp` maps
      AFC `ace2` to `ace_pro`).
- [ ] Lanes, colors and materials, from Anycubic RFID tags and from manual edits.
- [ ] Dryer start and stop (`AFC_ACE_DRY` / `AFC_ACE_DRY_STOP`), target, remaining time,
      box temperature and humidity on screen.
- [ ] Load, unload and a tool change from the UI; a swap mid-print; a runout.
- [ ] Whatever breaks becomes a fix with a test pinned to the captured frame.

## Phase 3: Kobra-S1/ACEPRO on the same rig (Path 3 with an ACE 2)

Swap AFC out, install Kobra-S1/ACEPRO, then restore AFC afterwards.

- [ ] `ace_instance_0` reports `protocol: ace2_proto` and `humidity`; capture frames.
- [ ] Seat signal (`current_index`, `target_index`), `park_state`, dryer, `ACE_CHANGE_TOOL`.
- [ ] Optionally ducati's fork for one session, to confirm its surface matches.

## Phase 4: fixes that need no hardware

- [ ] Detection: `ace_instance_*` or `ace_state` present means ACE wins over a Happy Hare
      `mmu` object (KLIPPACE's shim). Unit test on an objects list with both.
- [ ] Detection: multiACE on a non-U1 printer (`ace` with `aces[]`, no `filament_detect`)
      currently lands on `AmsBackendAce`. Decide where it belongs.
- [ ] Optional: show ducati's `toolchange_phase` / `toolchange_steps` as swap progress.

## Later, optional

- AFC's ACE 2 firmware build (RFID passthrough, 140 mm/s), flashed and recoverable over the
  same link.
- ACE 2 on the U1 (192.168.30.103) under multiACE, to test `AmsBackendMultiAce` with an ACE 2.
- ace2k, if a backend for it is wanted; the dryer's mains risk is in the landscape doc.

## Open

- Hub wiring (Phase 0 photo).
- AFC DEV is a moving branch; pin the commit installed and record it here.
