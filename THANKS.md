# Thanks

HelixScreen is a passion project, but it has never been a solo one. The printers
it supports, the firmware quirks it works around, and the bugs it no longer has
are the work of people who showed up with hardware I don't own, logs I couldn't
capture, and patches I didn't have to write.

This page is for them. If you helped and you're not here, that's my mistake, not
a judgment — open an issue or ping me and I'll fix it.

*(For the machine-generated list of everyone with a commit, see
[`CONTRIBUTORS.txt`](CONTRIBUTORS.txt), which feeds the in-app credits screen.)*

---

## Code & features

People who sent patches and built features.

### Camden Winder — [@Mud](https://github.com/Camden-Winder)
Brought up **QIDI Q2** support — a printer I ship blind, with no unit on my
bench. Camden wrote the QIDI support docs (#948) and, more importantly, is the
Q2 field tester: drying control, Happy Hare paths, and box configs all got
proven on his hardware, not mine. That printer is supported because he showed up.

### Pierre Poissinger
The **micro breakpoint** system for tiny 480x272 screens, and the **chamber
temperature** support — refactoring heater gcode generation so chamber follows the
same pattern as nozzle and bed, with the tests to back it. Also the network-widget
WiFi re-detection fix (#819) and a pile of font work.

### Andrew Basson
Owns the **installer and self-update** machinery — `install.sh`,
`serve-local-update.sh`, and the hard parts: surviving `NoNewPrivileges`, systemd
cgroup kills mid-update, atomic `.old` swaps, and Moonraker extraction. The kind of
infrastructure nobody notices until it breaks, which it now doesn't.

### Timo V
**Responsive and tiny-screen UI** — the `_tiny` breakpoint tokens, responsive and
animated icons, fbdev input handling, and **Centauri Carbon (CC1) support**.

### Justin Hayes — [@justinh-rahb](https://github.com/justinh-rahb)
**Micro 480x272 layouts** — compact controls, theme preview, and display overlays,
plus micro-portrait detection. Also build-system fixes: ccache double-wrap, K1
static-linked libhv/OpenSSL on the MIPS Docker flow, and Ender-3 V3 KE detection.

### RNGIllSkillz — [@RNGIllSkillz](https://github.com/RNGIllSkillz)
Hardened the **release download path** (timeouts and speed limits), added `enP*`
network-interface naming support, and tuned SDL display performance (#116).

---

### Borillion
**Portrait layouts** - a stacked `print_status` for portrait screens, the temperature
graph moved above its control strip, and speed/flow stacked in `print_tune`. Portrait
machines got a layout of their own instead of a squeezed landscape one.

### Jacob10383 - [@Jacob10383](https://github.com/Jacob10383)
**Community K2 Box firmware for the CFS** - support for the community Box firmware,
gating the Fork dialect on the Box API, and routing Box profile clears explicitly.

### TheLegendTubaGuy
**QIDI Max 4 support**, and taught the box handling to cope with more than one QIDI
box at a time.

### Thomas Dixon
**Native QIDI 3MF print previews** - thumbnails read from the format QIDI actually
ships, rather than none at all.

### Gaston Alexis Garcia Carli - [@lelalexi](https://github.com/lelalexi)
**Snapmaker U1 touch ergonomics** - a collapsible history filter row with an
active-filter funnel for small screens, and a temperature tool selector sized for
fingers instead of cursors.

### physicsG
**LAN pairing** - answering the firmware's pairing prompt, so the screen stops being
the thing standing between you and the printer.

### DST - [@plandevida](https://github.com/plandevida)
**Network state events** - CONNECTED/DISCONNECTED emitted on status-poll transitions
(#1059), so the UI is told the link changed instead of inferring it.

### Henri van der Riet
Stopped a scroll registering as a click (`PRESS_LOCK`, #1074) - the kind of fix you
only find by using the thing on a real touchscreen.

### LIsennn - [@LIsennn](https://github.com/LIsennn)
**Translations to 100% coverage across all nine languages.**

### cubewhy
**CFS RFID** - probing a bay's RFID when a spool is inserted, plus documenting
`BOX_INFO_REFRESH` and the vender field states.

## Hardware & field testing

The people who put builds on printers I don't have, captured the logs, and stayed
in the loop while we chased fixes. This work is worth more, not less, because I
can't reproduce it myself.

### raza616 — AD5X / IFS
Extensive help with the Flashforge AD5X integrated filament system — debug bundles
and on-printer validation across many rounds of fixes. The IFS support is as solid
as it is because of this testing.

### Vger1700 — AD5X / IFS
Debugging help on the AD5X integrated filament system — captured the debug bundles
that pinned down the purge-timeout and loaded-status behavior (#1065), and
volunteered as a field tester across the fixes.

### npa62 — Niimbot B1
Help bringing up and testing Niimbot B1 label-printer support on a Voron setup.

### jacekruf — K2 Plus
Help with Creality K2 Plus support and filament-system integration.

### Lexanger — Android
Help testing the Android remote-control build across additional device sizes.

---

## Firmware & ecosystem we run on

HelixScreen runs on top of community firmware and rooting work. These projects
aren't HelixScreen code, but without them HelixScreen wouldn't boot on half the
printers it supports.

### Sergei Rozhkov — [ghzserg](https://github.com/ghzserg) / [ZMOD](https://zmod.link)
ZMOD firmware for the Flashforge AD5M / AD5X. The IFS support in HelixScreen is
built directly against ZMOD's behavior.

### Guilouz — [Creality Helper Script](https://github.com/Guilouz/Creality-Helper-Script)
The rooting / helper script that makes HelixScreen installable on the Creality
K1 / K1C / K1 Max.

### Jpe230 — [SonicPad-Debian](https://github.com/Jpe230/SonicPad-Debian)
The Debian firmware that turns the Creality Sonic Pad into a target HelixScreen
can run on.

### PAXX — [SnapmakerU1-Extended-Firmware](https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware)
Extended firmware enabling SSH (and thus HelixScreen) on the Snapmaker U1.

### Phil1988 — [FreeDi / FreeQIDI](https://github.com/Phil1988/FreeDi)
Community Klipper + Moonraker stack for QIDI printers, and improvements to
HelixScreen's own QIDI support docs (#949, #963).

---

## Bug reports, protocol work & Discord

Good bug reports with logs, reverse-engineering, and the people who help others get
HelixScreen running.

- **J0eB0l** ([@lindnjoe](https://github.com/lindnjoe)) — help with Snapmaker U1
  boot and overlay diagnosis.
- **ninjamida** — IFS protocol intel and multi-IFS testing on the AD5X.
- **DIEHARDave** — help diagnosing AD5X filament-system behavior.
- **Sib6019** — reverse-engineering help on QIDI's display protocol.
- **Thmsdmsk** ([@Thmsdmsk](https://github.com/Thmsdmsk)) — co-authored fix.
- **GhostTypes** ([@GhostTypes](https://github.com/GhostTypes)) — Discord support
  and co-authored fix.

---

## Also thank you to

Everyone who filed a clean bug report with logs, tested a build on a printer I
couldn't reach, captured firmware behavior, or hung out in Discord helping other
people get HelixScreen running. You're why this thing works on more than one
printer.
