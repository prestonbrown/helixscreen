# Anycubic ACE Pro / ACE 2 Pro: Klipper Driver Landscape

Which community projects drive an ACE Pro or ACE 2 Pro on Klipper, how they are related,
what each one publishes to Moonraker, and which HelixScreen backend reads it. Surveyed
2026-11-18 from each project's source at the commit listed. Star counts and activity are
from that day and age quickly; the lineage and status surfaces do not.

The backend side (how HelixScreen parses each surface) lives in
[`FILAMENT_BACKEND_ACE.md`](../FILAMENT_BACKEND_ACE.md),
[`FILAMENT_BACKEND_MULTIACE.md`](../FILAMENT_BACKEND_MULTIACE.md) and the ACE section of
[`FILAMENT_BACKEND_AFC.md`](../FILAMENT_BACKEND_AFC.md). This page is the map of the
driver projects behind them.

## The hardware in one paragraph

Both units are four-lane feeders with a dryer and no internal combiner: lanes merge in an
external hub before the toolhead. The **ACE Pro** (gen 1) speaks a JSON protocol over USB
serial at 115200 baud and daisy-chains. The **ACE 2 Pro** speaks a binary, sequence-numbered,
protobuf-style framing over **RS-485** at 230400 baud and adds a humidity sensor. On a
non-Anycubic host it needs an RS485-to-USB adapter; the drivers name the Anycubic KS1/K3
conversion cable, which enumerates as a CH343 (`usb-1a86_USB_Single_Serial_*`). That
cable (labeled "For K3/K3M/S1") carries the converter in its boot, powered from the ACE: its
6-pin end goes into the ACE's 6-pin socket, and its 4-pin end presents plain USB in the gen-1
layout. Measured on a unit (face view of the 4-pin plug, latch up): top-right D+ (3.3 V
pull-up with the ACE on), top-left D-, bottom-right GND (passes through to the 6-pin side),
bottom-left unconnected. Host VBUS stays unwired. An ACE 2
**clamps the filament** when its link drops or when the extruder retracts against it, which
is why the drivers that support it carry disconnect-pause and "rollback assist" logic. The
ACE 2 has no firmware-side tangle detection during feed assist; the host has to watch for it.

The projects disagree on whether several ACE 2 units can share one RS-485 bus:
Kobra-S1/ACEPRO implements a shared-bus session (UID discovery and device-id assignment),
while multiACE and ace2k both say one unit per USB connection and no daisy chain. Treat
multi-unit ACE 2 on one bus as unproven.

## Lineage

```
DuckACE (utkabobr, abandoned)
  ├─ ValgACE (agrloki) / BunnyACE ............ Path 2: `ace` + /server/ace REST
  └─ ACEPROSV08 (szkrisz)
       └─ Kobra-S1/ACEPRO .................... Path 3: `ace` manager + `ace_instance_N`
            └─ ducati1198/acepro-any-klipper . same surface, plus toolchange fields
                 └─ Yheffe/KLIPPACE .......... same surface, plus a Happy Hare `mmu` shim

SnapACE (BlackFrogKok)
  └─ decay71/multiACE ........................ Path 4: `ace` with nested `aces[]` (U1)

AFC (ArmoredTurtle), DEV branch .............. `AFC_ACE` / `AFC_ACE2` units inside AFC
  ACE Pro code adapted from Kobra-S1/ACEPRO; ACE 2 wire protocol adapted from multiACE

tobecwb/ace2k ................................ replaces ACE 2 firmware: `[mcu ace2k]`
Rinkhals / Anycubic GoKlipper ................ Path 1: `filament_hub` (stock Anycubic)
```

ducati and KLIPPACE branched from Kobra-S1 at the same commit (683542a, 2026-03-29) and have
not merged upstream since; Kobra-S1 was 40 commits ahead of that point at survey time.

## The projects

### Kobra-S1/ACEPRO (upstream of the Python driver family)

[github.com/Kobra-S1/ACEPRO](https://github.com/Kobra-S1/ACEPRO), surveyed at its 2026-11-08
head. A Klipper extras package (`extras/ace/`, about 18.5k lines) plus an `ace_status.py`
Moonraker component and a browser dashboard. 79 test files.

- **Hardware:** ACE Pro and ACE 2 Pro, mixed on one printer, multi-unit (tested with three).
  Instances are numbered in USB topology order, ACE Pro units first.
- **Targets:** Kobra S1, Kobra 3 and K3 Max on vanilla Klipper or Kalico, usually on an
  external Pi over USB-OTG; Kobra X (alpha) behind the Kobra Klipper fork's toolchanger.
  Other printers work with hand-adapted feed lengths and cut/wipe macros.
- **Notable:** ACE 2 firmware update over the link (`ACE_FIRMWARE_UPDATE`), rollback assist,
  fast disconnect pause, tangle detection from the firmware's `cont_assist_time`, feed
  assist paused at idle timeout, feed fail-fast on the firmware's own error verdicts, RFID
  inventory sync, endless spool with exact / material / next-ready matching, `lane_data`
  sync for OrcaSlicer.
- **Surface:** manager `ace` (also `ace_state`) plus one `ace_instance_N` per unit. The
  field tables are in `FILAMENT_BACKEND_ACE.md` § Path 3.
- **HelixScreen:** Path 3 of `AmsBackendAce`. Confirmed on a live gen-1 rig (#1069).

### ducati1198/acepro-any-klipper

[github.com/ducati1198/acepro-any-klipper](https://github.com/ducati1198/acepro-any-klipper),
surveyed at its 2026-11-14 head. Kobra-S1 repackaged for non-Anycubic printers; tuned on a
Creality K1 Max with two ACE Pro units. 64 of its 105 commits since the fork point are
authored "Claude".

- **Adds:** an installer with Klipper, Kalico and Simple AF profiles and `--undo`; a patched
  Fluidd 1.37.7 ACE panel; an OrcaSlicer spool-to-toolhead mapper plugin; tool-change,
  cut and purge counters; a CFS purge-chute variant (untested).
- **Drops** these Kobra-S1 modules: `idle_assist`, `connection_pause`, `rollback_assist`,
  `print_tools`, `monitor_events`, `version`. Kobra-S1's safety fixes since March are absent.
- **Surface:** instance `get_status` keys identical to Kobra-S1. The manager adds
  `toolchange_phase`, `toolchange_steps`, `stats` and `endless_chain`. HelixScreen does not
  read them; `toolchange_phase`/`toolchange_steps` are the only new information any fork
  publishes and could feed a tool-change progress display.
- **HelixScreen:** Path 3, by the shared surface. Not run against a live rig.

### Yheffe/KLIPPACE

[github.com/Yheffe/KLIPPACE](https://github.com/Yheffe/KLIPPACE), forked from ducati, surveyed
at its 2026-11-01 head. In practice one person's Voron 2.4 build (EBB36, Blobifier, A4T
cutter, the author's own `printer.cfg` in the repo) around a trimmed driver (about 14.5k
lines). Multi-unit code exists; only one unit has been tested.

- **Adds:** sensor-terminated feeding (the ACE stops when the toolhead entry sensor trips),
  differential push speeds, a manual slot editor, and a **Happy Hare compatibility shim**:
  it registers `mmu` and `mmu_machine` Klipper objects (`gate_status`, `gate_color`,
  `gate_material`, `ttg_map`, ...) and an `MMU_*` command set so stock Fluidd and Mainsail
  cards render it.
- **Drops** also `ace2_ota` and `intake_gated`; instance status lacks `park_state` and
  `max_dryer_temperature`.
- **HelixScreen: misdetected, read from the code, not run.** In
  `src/printer/printer_discovery_parse.cpp#parse_objects` the `mmu` branch sets
  `mmu_type_ = HAPPY_HARE` without the `!has_mmu_` guard the ACE branch has, so the shim
  claims the printer whatever order the objects are listed in. The Happy Hare backend then
  drives a shim, ignores the ACE-only state (dryer, RFID, `ace_instance_N`), and any `MMU_*`
  command the shim lacks is logged and skipped by Klipper.

### decay71/multiACE

[github.com/decay71/multiACE](https://github.com/decay71/multiACE), v1.27b, pushed
2026-11-18. By far the largest user base of these projects (220 stars). It began as a
SnapACE fork and is now mostly its own code.

- **Topology:** built for the **Snapmaker U1**: up to four ACE units (16 colors) feeding the
  U1's four toolheads. Its model is which spool feeds which head, with parked swaps and a
  browser **preflight** that re-plans a print so one color can sit on two heads to cut swaps.
- **Hardware:** ACE Pro and ACE 2 Pro. Community firmwares flash from its web UI:
  ACE2-Open (Simon-CR) for the ACE 2 and a community build for the ACE Pro, both adding tag
  read and write beyond Anycubic tags.
- **Other printers:** supported only recently; a Voron was the first non-U1 machine.
- **Distribution:** being packaged into paxx's Extended Firmware for the U1, so U1 users
  will get it by default.
- **Surface:** `ace` with units nested under `aces[]`. **HelixScreen:** Path 4,
  `AmsBackendMultiAce` beside the Snapmaker backend, U1 only
  ([`FILAMENT_BACKEND_MULTIACE.md`](../FILAMENT_BACKEND_MULTIACE.md)). A multiACE install on a
  non-U1 printer has no `filament_detect`, so detection would hand it to `AmsBackendAce`,
  which expects a top-level `slots` array. Unhandled today.

### AFC (ArmoredTurtle), DEV branch

[AFC-Klipper-Add-On](https://github.com/ArmoredTurtle/AFC-Klipper-Add-On), `DEV` branch,
surveyed at 1e400e2 (2026-11-15). ACE support arrived in AFC #879 (2026-10-30), with fixes in
#895 (ACE fighting the toolhead extruder on unload) and #904 (Kalico). Not yet on `main`.

- **Units:** `[AFC_ACE <name>]` for the ACE Pro and `[AFC_ACE2 <name>]` for the ACE 2 Pro.
  Lanes are plain `[AFC_lane]` sections (the ACE moves filament itself, so no steppers or
  pins). Modes: combined (one toolhead through a hub) or direct (one tool per lane).
- **Requires** a toolhead sensor in the `[AFC_extruder]`'s `pin_tool_start` (or an FPS/PSF
  buffer): a load feeds `afc_bowden_length`, then pulses until that sensor trips.
  `AFC_ACE_CALIBRATE` measures the bowden length.
- **Firmware:** everything runs on stock ACE 2 firmware except AFC's own RFID reader and feed
  speeds above 100 mm/s. Those need the AFC build (`firmwares/ACE2/AFC_ACE2PRO.bin`: stock
  app plus a register passthrough and a speed patch), flashed over the same serial link and
  recoverable from the IAP loader.
- **Code lineage:** `AFC_ACE.py` adapts Kobra-S1/ACEPRO's commands; `AFC_ACE2.py` adapts
  multiACE's wire protocol and reuses the gen-1 unit logic unchanged.
- **HelixScreen:** `AmsBackendAfc` reads the `AFC_ACE`/`AFC_ACE2` objects for dryer, climate
  and tags (`include/afc_status_parse.h` `AceUnitDelta`) and drives `AFC_ACE_DRY`. Written
  against the DEV source; not yet run on hardware.

### tobecwb/ace2k

[github.com/tobecwb/ace2k](https://github.com/tobecwb/ace2k), v0.15.0 (2026-11-13). Not a
driver: open firmware for the **ACE 2 Pro only** that turns the unit into a native Klipper
MCU (`[mcu ace2k]` plus an `[ace2k]` host module, about 13k lines of host Python and C
firmware, 71 test files, ADRs and CI).

- **Design:** feed, dryer and RFID transport run in the MCU; brand tag decoding runs on the
  host (Anycubic, Bambu Lab and Snapmaker today; Elegoo and Creality written but off).
- **Target:** the Snapmaker U1 via the separate `ace2k-u1` adapter. On another printer the
  user writes macros over the `ACEK_*` commands.
- **Risks the project states:** tested on one unit, one U1, 127 V / 60 Hz only. The optional
  humidity mode (`unattended_drying: True`) starts the mains heater with nobody present.
  Each unit is a Klipper MCU, so losing any unit's USB link shuts Klipper down.
- **Surface:** `ace2k`, `ace2k_feed`, `ace2k_rfid`, `ace2k_dryer`, `ace2k_dryer_mode`, none
  of which HelixScreen matches. A new backend, or a `lane_status` producer
  ([`FILAMENT_BACKEND_LANE_STATUS.md`](../FILAMENT_BACKEND_LANE_STATUS.md)) on its side.

### Happy Hare

No ACE support. Feature request moggieuk/Happy-Hare#786 is open; the maintainer has said a PR
needs someone with the hardware. Happy Hare's NFC tag parser does decode the Anycubic ACE tag
format.

## Coverage matrix

| Driver | ACE Pro | ACE 2 | Klipper objects | HelixScreen reads it through | Verified on hardware |
|---|---|---|---|---|---|
| Anycubic GoKlipper (Rinkhals) | yes | yes | `filament_hub` | `AmsBackendAce` Path 1 | yes (user rigs) |
| ValgACE / BunnyACE | yes | no claim | `ace` + REST | `AmsBackendAce` Path 2 | user rigs |
| Kobra-S1/ACEPRO | yes | yes | `ace`, `ace_instance_N` | `AmsBackendAce` Path 3 | gen 1 only (#1069) |
| ducati any-klipper | yes | untested | same as Kobra-S1 | `AmsBackendAce` Path 3 | no |
| KLIPPACE | yes | untested | same plus `mmu`, `mmu_machine` | **Happy Hare (wrong)** | no |
| multiACE on a U1 | yes | yes | `ace` with `aces[]` | `AmsBackendMultiAce` | user rigs |
| multiACE elsewhere | yes | yes | `ace` with `aces[]` | none | no |
| AFC DEV | yes | yes | `AFC_ACE`, `AFC_ACE2` | `AmsBackendAfc` | no |
| ace2k | no | yes (reflashed) | `ace2k*` | none | no |
