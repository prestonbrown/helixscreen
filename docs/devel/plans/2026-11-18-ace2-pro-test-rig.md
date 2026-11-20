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

What came in the box: two signal cables (one labeled "For K3/K3M/S1" with a larger boot, one
plain), each with a 6-pin end and a 4-pin end, both male; and the return detection module
(RDM), a filament sensor with its own 6-pin cable. The ACE 2 has two sockets at the back,
lower left: a 6-pin above a 4-pin.

**The K3/K3M/S1 cable is an RS485-to-USB converter**, measured 2026-11-18 with its 6-pin end
in the ACE's 6-pin socket (the S1 manual page: "4-pin signal cable into the printer's back
panel ... 6-pin cable to the port ... of the ACE 2 Pro"). Its 4-pin end, face view, latch up:

| Pin | Measured | Wire to USB-A |
|---|---|---|
| TR | 3.3 V to BR with the ACE on (full-speed pull-up) | D+ |
| TL | about 0 V; diode 0.52 V from BR | D- |
| BR | passes straight through to the 6-pin bottom-center | GND |
| BL | OL to every pin on both ends | none |

The converter is powered from the ACE side, so host VBUS stays unwired. This matches the
gen-1 pinout in Kobra-S1/ACEPRO's README. The ACE's 4-pin socket is unused by every manual
page seen. The BMCU RS-485 adapter is not needed.

- [x] Identify the cables and the ACE sockets; meter the K3 cable's printer end.
- [x] USB-A adapter (D+, D-, GND). On thelio 2026-11-19 it enumerated as `1a86:55d3`
      "USB Single Serial" (CH343, `cdc_acm`, `ttyACM`), by-id
      `usb-1a86_USB_Single_Serial_5B61000190-if00`. AFC's ACE 2 protocol code, run
      standalone at 230400 baud, got answers to discover (UID), `get_info` (`ACE 2 Pro`,
      firmware `V1.1.22`, boot `V1.0.1`) and `get_status` (four empty slots, dryer `stop`,
      20 C, 43 % humidity). Link proven on stock firmware.
- [ ] Crimp a proper 2x2 receptacle once it enumerates.
- [ ] RDM pinout (its 6-pin cable goes to the printer per the manual; Kobra-S1 wires it as a
      `filament_switch_sensor`, optionally a `filament_tracker` with an encoder line).
      Deferred: the first layout below needs no RDM.

**Hub layout for the first run:** a printed passive 4-in-1 placed right at the V0's existing
foot runout sensor (`gpio16`), so the sensor sits at the hub's outlet. AFC's ACE 2 template
ships a virtual hub (`switch_pin: virtual`, the ACE 2 has no hub switch), so the sensor is an
addition: point `[AFC_hub]` `switch_pin` at the foot sensor's pin and delete
`[filament_switch_sensor filament_runout_sensor]`, which claims the same pin and calls
`PAUSE` on runout. A real hub switch on an ACE unit is untested in AFC. Each lane's
`dist_hub` must park its tip short of the hub's merge point (the template's rule), and
`AFC_ACE_CALIBRATE` measures `afc_bowden_length` from there to the toolhead sensor.

- [ ] Print the 4-in-1; measure each ACE outlet's tube length to the merge point.
- [ ] Optional: an automatic bypass, a 2-into-1 hub before the toolhead whose switch sits on
      the bypass input only, configured as `[filament_switch_sensor bypass]`. A hand-fed spool
      then turns AFC's bypass on by itself, which settles the "filament at the toolhead, unit
      empty" state the V0 is in today (`FILAMENT_BACKEND_AFC.md`, bypass paragraph).
- [ ] Mount the ACE and the hub; route one PTFE run from hub to the V0 extruder; measure it.
- [ ] Hub sensor: if it is a plain switch, wire it to a free Pico or SHT36 input (or reuse
      `gpio16` if the runout switch moves to the hub).

## Phase 1: AFC DEV on the V0 (remote; each install or restart confirmed with Preston)

- [x] Backed up `printer_data/config` and the Moonraker database to thelio
      `~/printer-backups/voronv0/printer_data-config-database-20261119-1828-before-afc.tgz`.
- [x] AFC DEV `1e400e2` installed 2026-11-19 at `~/AFC-Klipper-Add-On` by running AFC's own
      `install_afc()` non-interactively (no sudo): unit ACE `V2_Pro` `Ace2_1`, buffer
      Internal, `pin_tool_start: ^sht36:LIMIT_1`, tip forming on, cutter/poop/kick/wipe/park
      macros off. `conf.d/toolhead.cfg`'s `[filament_switch_sensor toolhead_sensor]` is
      commented out (AFC owns the pin). `moonraker.conf` update_manager entry uses
      `primary_branch: DEV`. Virtual hub for now; stock ACE 2 firmware V1.1.22.
- [x] Klipper ready, `AFC_ACE2 Ace2_1` reports `ace_connected: true`; HelixScreen detects AFC.
- [ ] Hub at the foot sensor: `[AFC_hub]` `switch_pin: gpio16`, delete
      `[filament_switch_sensor Filament_Runout_Sensor]` in `printer.cfg`, per-lane `dist_hub`.
- [ ] `AFC_ACE_CALIBRATE` per lane.
- [ ] Tip forming tuned until a swap unloads cleanly past the hub.
- [ ] One two-color print end to end.

## Phase 2: HelixScreen under AFC

Found and fixed on the rig so far (main 7933d35598): a phantom "ASA" on an empty AFC lane
(HelixScreen's own external-spool record under the 1-based `lane1` key read as AFC's
0-based `lane1`), and the "Filament in the toolhead, no lane loaded" prompt
(`FILAMENT_MANAGEMENT.md`). The V0 runs that build.

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
