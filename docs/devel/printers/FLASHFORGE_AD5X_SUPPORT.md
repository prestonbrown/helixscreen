<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# FlashForge Adventurer 5X (AD5X) Support

HelixScreen has a dedicated cross-compilation target for the FlashForge Adventurer 5X. The AD5X is a multi-color 3D printer with a 4-channel IFS (Intelligent Filament System). It has its own toolchain and Docker image (`Dockerfile.ad5x`), separate from the Creality K1 series.

**Status: Active testing** — prebuilt binaries are included in releases. See [issue #203](https://github.com/prestonbrown/helixscreen/issues/203).

## Hardware

| Spec | Value |
|------|-------|
| SoC | Ingenic X2600 (XBurst2, MIPS32 R5 compatible) |
| CPU | Dual-core XBurst2 (MIPS) + Victory0 (RISC-V real-time co-processor) |
| Display | 4.3" touch, 800x480, fbdev at `/dev/fb0`, 32bpp BGRA |
| Touch | Resistive, evdev at `/dev/input/eventN` (device name "Touchscreen") |
| RAM | Unconfirmed |
| OS | Custom Linux (BusyBox), not Buildroot |
| Init System | sysvinit (BusyBox), NOT systemd |
| Moonraker | Via ZMOD, port 7125 |
| Multi-Material | IFS (Intelligent Filament System), 4 spools, auto-switching |
| SSH | `root@<ip>` (via ZMOD) |

### Buzzer

There is no host audio hardware and no sysfs PWM: the X2600's PWM2 IP is driven
by the out-of-tree `soc_pwm.ko`, which registers the misc char device
`/dev/jz_pwm` and never a `pwmchip`, so the PWM sysfs backend cannot probe (or
steal backend selection) on this platform. The buzzer (pc12) is a host GPIO,
unreachable from Klipper as configured — both MCUs are serial controller boards
with no linux MCU — and the stock config set has no `[output_pin]` or beep
section at all, so no output_pin object ever appears in objects/list.

The working path is gcode to a macro that reaches the host GPIO from Klipper:
ZMOD's AD5X config defines `[gcode_macro M300]` → `RUN_SHELL_COMMAND
CMD=audio_freq`; Forge-X's macros/base.cfg ships `[gcode_macro M300]` wrapping
the fork's native TONE command (whose tone_player backend is being ported to
the cmd_pwm-compatible `fx-pwm` helper for this platform). HelixScreen's
speaker detection therefore keys on the M300 macro (`parse_objects`,
printer_discovery.h), and sound goes out as `M300 S<Hz> P<ms>` via the M300
backend — S0 is the backend's silence form and is a no-op on both firmwares.
Note for rigs: Forge-X's TONE wrapper drops every tone when the printer's
`mod_params` `sound` variable is off — that mutes HelixScreen too, and looks
identical to a broken sound path. A printer whose firmware handles M300
without a macro and without an output_pin can be brought in by forcing the
`speaker` capability override to `enable` in settings.json — that override
installs the M300 backend too.

## Filesystem Layout

The AD5X uses a FlashForge-specific layout, distinct from the Creality K1:

| Path | Purpose |
|------|---------|
| `/usr/data/` | User data partition |
| `/usr/prog/` | FlashForge programs and tools — **key AD5X indicator** |
| `/usr/data/config/` | Klipper/Moonraker config |
| `/usr/data/config/mod/` | ZMOD installation |
| `/usr/data/config/mod_data/` | ZMOD data, logs, database |
| `/opt/config/` | Symlink or bind-mount to `/usr/data/config/` |

The presence of `/usr/prog/` is used for runtime platform detection (K1 vs AD5X).

## Build Target

The AD5X has a dedicated build target and toolchain, separate from the Creality K1:

```bash
make ad5x-docker          # Build AD5X binary (dedicated MIPS32r5 glibc toolchain)
make release-ad5x         # Package as helixscreen-ad5x.zip (AD5X release_info.json)
```

**Toolchain**: Dedicated Docker image (`Dockerfile.ad5x`) with MIPS32r5 glibc cross-compiler, distinct from the K1's musl-based toolchain.

**Platform define**: `-DHELIX_PLATFORM_MIPS`. Runtime detection via `/usr/prog` presence determines platform key (`ad5x` vs `k1`) for update manager asset selection.

## ZMOD Integration

The AD5X runs HelixScreen through the [ZMOD](https://github.com/ghzserg/zmod) firmware modification. ZMOD handles:

- Klipper/Moonraker installation and management
- Display initialization (fbdev, touch via tslib env vars)
- App lifecycle (init.d service script `S80guppyscreen`)
- Update management via Moonraker update manager
- Z-offset storage — see below

### ZMOD z-offset storage

ZMOD does **not** treat Klipper's live `gcode_move.homing_origin[2]` as the
authoritative z-offset. Its `SET_GCODE_OFFSET` override writes every adjustment to
the `gcode_offsets` save-variable, `END_PRINT` / `CANCEL_PRINT` reset the live
offset to 0, and `START_PRINT` re-applies the stored value via `LOAD_GCODE_OFFSET`
(gated on the `load_zoffset` save-variable, off by default).

Consequences, all handled in `include/z_offset_persistence.h`:

| Effect | Handling |
|--------|----------|
| Live offset reads 0.000 whenever idle | Display the stored value instead while idle; the live one is authoritative only mid-print |
| Stored value lives in `save_variables.variables.gcode_offsets.z` (mm) | `save_variables` is subscribed for any printer exposing `SAVE_ZMOD_DATA`, independently of the IFS subscription |
| Relative `SET_GCODE_OFFSET Z_ADJUST=` resolves against the zeroed live offset, and the override persists the result | Idle adjustments send an absolute `Z=` computed from the displayed base |
| Reload at print start ships off | `SAVE_ZMOD_DATA LOAD_ZOFFSET=1` sent once per session, while idle |

ZMOD's own docs describe the idle reading as "for reference only". Note also that
the native-screen and screenless offsets are stored separately — `LOAD_ZOFFSET_NATIVE`
copies one to the other, and HelixScreen does not call it.

### Moonraker Update Manager

ZMOD configures Moonraker to check for HelixScreen updates. The release_info.json file tells Moonraker which release asset to download:

```json
{
    "project_name": "helixscreen",
    "project_owner": "prestonbrown",
    "version": "v0.13.1",
    "asset_name": "helixscreen-ad5x.zip"
}
```

### Launch Environment

The ZMOD init script sets up touch input via tslib environment variables, but HelixScreen uses LVGL's built-in evdev driver instead. The relevant environment on the AD5X:

- Touch device: auto-detected from `/dev/input/eventN`
- Framebuffer: `/dev/fb0` (800x480, 32bpp)
- Backlight: `FBIOBLANK` ioctl (standard Linux fbdev)

## Sound (2026-09-01 rig sessions)

**Decision: UI sounds only.** The piezo is driven natively by
`JzPwmSoundBackend` (behind `HELIX_HAS_JZ_PWM`, ad5x builds): theme steps
render in-process with the full NoteEvent synthesis - chords, ADSR,
sweeps - duty-encoded at the tuner-calibrated 385 MHz DMA rate and played
by the long-lived `fx-pwm serve` daemon over `/tmp/fx-pwm.sock` (the
backend spawns it on first sound; it idle-exits after 30 s so klippy's
per-tone fx-pwm one-shots keep working between UI sounds). Audibility
floor measured at 10 ms; buffers capped at 2.5 s.

Why no music, with numbers: the driver's dma_update copies buffer words
at ~30 µs each (measured 2026-09-01: 3,504 words = 106 ms, 14,024 =
429 ms, linear). Upload time therefore equals playback time at the
32 kHz ultrasonic carrier - a hard 50% duty ceiling for continuous
audio; ~80% is reachable only by dropping the carrier into the audible
band, which whines. The tracker phrase path
(`jz_pwm_render_phrase`) stays in the tree, tested but unused: mods do
not ship on ad5x. The M300 path remains for remote-UI installs. PCM
streaming on this engine is impossible with numbers: buffer swaps are
refused while a loop is armed and the legal chunk cycle costs a fixed
~500 ms of silence per chunk.

Daemon hardening (all learned from rig incidents, see SOUND_SYSTEM.md for
the protocol): SIGPIPE ignored (a client close must not kill the daemon);
per-frame release/re-request claim dance (second dma_init otherwise
EPERMs) - and that dance can itself D-wedge pwm2_release, so never loop
it hot; hold loops use absolute deadlines (poll timeout restarts on a
readable socket = infinite phrase loop otherwise); the app's sender
worker is the only socket writer (250 KB bodies block for seconds).

## Firmware Quirks and Operating Rules (verified 2026-08)

Recorded during the 2026-08 research pass that accompanied commissioning our own AD5X
rig (2026-08-23: ZMOD 1.7.2 on stock base 1.1.7, stock Klipper 12 MCU), so nobody
re-derives these from a dead printer. Every claim is tagged:

- **[upstream-doc]** — ZMOD AD5X wiki, tracker, or release notes
- **[community]** — consensus user reporting, not vendor-confirmed
- **[rig-verified]** — observed on our own AD5X

### Stock-base compatibility: restore DOWN to a listed image

ZMOD supports a discrete list of AD5X stock main versions — 1.1.7 through 1.2.3, 3.0.3,
3.0.9, and 3.1.0 (the ceiling) — and a **factory restore is mandatory before install**.
New units ship above the ceiling: ours arrived on 3.1.4 and had to be restored *down*
to `AD5X-1.1.7-1.1.0-3.0.6-20250912-Factory.tgz` first. Never update up before
installing ZMOD — there is no install path from an unlisted base. [rig-verified]

### Timer-too-close: the AD5X's signature failure

`MCU shutdown: Timer too close` recurs on AD5X+ZMOD across the tracker's whole life:
#185, #195, #230, #475, #551, #698 (Oct 2025 - Aug 2026); #561 is verbatim "AD5X
repeatedly fails with MCU 'eboard' shutdown: Timer too close". Community consensus
names Klipper 13 on the AD5X MCU as the trigger ("stay on 12 for the AD5X"). ZMOD
defaults the AD5X MCU to Klipper 12; `UPDATE_MCU FORCE=13` opts in and requires host
and MCU versions to match. **Our rig intentionally stays on the stock Klipper 12 MCU
blob.** [community + upstream-doc]

Two avoidance rules come out of the same host-starvation mechanism:

- **Slicer "Exclude Models" output gcode is a documented Klipper crash trigger** —
  uncheck it (slicer: Process Profile -> Other -> Output G-code -> Exclude Models).
  [upstream-doc]
- **Host work during toolhead motion is hair-trigger.** Forge-X's own regression
  harness had to stop screenshotting during motion to avoid TTC (Forge-X commit
  `46c75cb`, 2026-08-12). This is the external rationale for HelixScreen's print-time
  IFS poll backoff (5 s → 30 s while printing) —
  `docs/devel/FILAMENT_BACKEND_AD5X_IFS.md` § "Polling caution". [upstream-doc,
  Forge-X repo]

### ZMOD behavior differences on AD5X

All [upstream-doc]:

- `NEW_SAVE_CONFIG` does **not** function on AD5X — use plain `SAVE_CONFIG`
- The dialog flag is `FAST_CLOSE_DIALOGS`, not `CLOSE_DIALOGS`
- No Entware on AD5X
- `CAMERA_ON VIDEO=video3|video0|video99`
- The `IFS_F10` / `IFS_F11` / `IFS_F13` / `IFS_F15` macro family exists for filament
  management — command reference in `docs/devel/FILAMENT_BACKEND_AD5X_IFS.md`
  § "zmod IFS command reference"

### IFS contention with the native screen

Errors result when the native display and a mod access IFS simultaneously; most IFS
settings only work with the native screen disabled (`DISPLAY_OFF`), and
`DISPLAY_OFF_TIMEOUT=10` mitigates. HelixScreen runs in alternative-screen mode
(`DISPLAY_OFF HELIX=1`) — the supported configuration — but the constraint is why the
IFS backend treats polling as a guest privilege, not a right:
`docs/devel/FILAMENT_BACKEND_AD5X_IFS.md` § "Polling caution". [upstream-doc]

### RAM: what alternative-screen mode buys

Native screen ~23 MB vs GuppyScreen ~9 MB — alternative-screen mode frees ~14 MB on
this small-RAM host. Figures are community-measured, not vendor specs. [community]

### Commissioning traps

All [rig-verified]:

- **Remove the USB stick after ZMOD install.** Left in, the installer re-triggers on
  the next boot, dies on a busy mount, and the failure looks exactly like a broken
  install (no 80/7125, empty /opt).
- **`/opt` is empty on AD5X ZMOD installs.** "No /opt/zmod" is not evidence of a
  failed install.
- **Port signature:** stock = 22 + 8899; ZMOD = 22 + 80 + 7125 with 8899 closed.
  Fluidd is served on 80; nginx does **not** proxy Moonraker — talk to `:7125`
  directly.
- **First boot takes ~140 s to reach port 80**, then runs calibration and input
  shaping autonomously. Do not poll the printer during that window (see TTC above).

### Stock OTA and ZMOD

A stock OTA update **disables** ZMOD while preserving data; re-enable afterwards via
`AD5X-ENABLE-zmod.tgz`. Never take a stock OTA over a mod install. [upstream-doc]

### Ecosystem context

The AD5X has exactly one mod: ZMOD (ghzserg). Forge-X (DrA1ex/ff5m) is AD5M/Pro-only
and states AD5X support is unlikely ever; xblax's klipper-mod
(`xblax/flashforge_ad5m_klipper_mod` — see `docs/devel/AD5M_KMOD_VARIANT.md`) has been
dormant since 2025-09. The three variants are mutually incompatible at the macro and
binary level. FlashForge stock firmware has shipped no changelogs since mid-2025, and
there is no AD5X GPL source drop. [upstream-doc]

## Display & Touch

### Display Backend

The AD5X uses the **fbdev** display backend (same as AD5M and K1). No DRM support.

- Resolution: 800x480 (auto-detected from framebuffer)
- Color depth: 32bpp ARGB8888
- Sleep: `FBIOBLANK` / `FB_BLANK_NORMAL` for blanking, `FB_BLANK_UNBLANK` for wake

### Known Issue: Random solid colors during sleep (unresolved)

Some AD5X users report a solid red/green/blue panel fill when the display
goes to sleep, instead of a blank screen. The color appears to be random per
sleep cycle. Not reproducible on our internal hardware so there is no code
fix in place yet.

**Probable root cause:** interaction between `FBIOBLANK FB_BLANK_NORMAL` and
the Allwinner DE/TCON pipeline — when the framebuffer source is detached
from the display engine, the DE appears to emit whatever color is latched in
its default-fill register rather than going fully dark. Because
`backlight_enable_ioctl` is `false` on AD5X (PWM polarity inversion quirk,
#95 / #235), the backlight is dimmed via `SET_BRIGHTNESS(0)` only — and on
some AD5X units that does not fully kill the LEDs, so the garbage fill
stays visible.

Affected cohorts:

| Cohort | Config | Sleep path |
|--------|--------|-----------|
| Fresh install / post-#431 preset | `hardware_blank=1`, `sleep_backlight_off=true` | FBIOBLANK + backlight off — DE may emit solid color |
| Post-wizard (pre-fix) | `hardware_blank=0`, `sleep_backlight_off=false` | Software overlay + backlight on — last frame held |

Both have produced user reports of the RGB symptom. The underlying driver
behavior is the same — LVGL stops invalidating after the overlay draws or
the FBIOBLANK fires, and the DE pipeline goes quiet.

**Wizard/preset disagreement (pending fix):** `ui_wizard_printer_identify.cpp`
force-writes `hardware_blank=0`, `sleep_backlight_off=false`,
`backlight_enable_ioctl=false` on wizard confirmation for AD5X and CC1,
overriding the AD5X preset. The wizard should not be doing hardware
manipulation — this block needs to be removed and a config migration added
for users who already ran it. See prestonbrown/helixscreen#235,
prestonbrown/helixscreen#431, prestonbrown/helixscreen#303 for history.

**User workarounds** (documented in `docs/user/TROUBLESHOOTING.md`):

1. Settings → Display → Sleep → **Never** (`sleep_sec = 0`)
2. Edit helixconfig.json: set `display.sleep_backlight_off = false` to
   keep the backlight on during sleep

**Investigation TODO:**

- Collect debug bundles from affected users to confirm cohort split
- Try reordering in `enter_sleep()`: backlight → 0 *before* `FBIOBLANK`
- Add `clear_framebuffer(0x00000000)` before creating the software overlay
- Force `lv_refr_now()` after `create_sleep_overlay()` so there is no
  window between overlay creation and flush

### Touch Input

HelixScreen uses LVGL's built-in evdev input driver. The ZMOD ecosystem historically used tslib for touch calibration, but our built-in calibration system handles this natively.

If touch input requires calibration (resistive panel with non-linear mapping), the calibration wizard will handle it automatically on first launch.

## IFS (Intelligent Filament System)

The AD5X's 4-channel IFS is its distinguishing feature. HelixScreen has a dedicated AMS backend (`AmsBackendAd5xIfs`) that fully integrates with the IFS.

> **Required firmware**: [ZMOD open-source firmware](https://github.com/ghzserg/zmod) **v1.7.0 or newer**. v1.7.0 (Mar 2026) is the first release with explicit HelixScreen integration (`DISPLAY_OFF HELIX=1`). Hard minimum: v1.6.2 (Oct 2025), when the `less_waste_*` `save_variables` first appeared via the bambufy plugin.
>
> ZMOD has its own versioning, distinct from FlashForge stock firmware. AD5X stock firmware uses a tri-versioned scheme like `AD5X-1.1.6-1.1.0-3.0.6-20250729` (main / sub / screen / date) — the `3.0.6` is screen-firmware version, not a major printer-firmware bump. ZMOD supports a discrete list of stock main versions — 1.1.7 through 1.2.3, 3.0.3, 3.0.9, and 3.1.0 (ceiling) — and a factory restore to a listed image is mandatory before install; see § "Firmware Quirks and Operating Rules" below.
>
> ##### ZMOD IFS feature timeline
>
> | ZMOD release | Date | What landed |
> |---|---|---|
> | v1.4.1 | Mar 2025 | Alpha AD5X support |
> | v1.5.1 | Apr 2025 | MCU IFS update path |
> | v1.5.4 | Jun 2025 | AD5X filament-presence sensor working |
> | v1.6.1 | Sep 2025 | Headless IFS — works without native screen |
> | v1.6.2 | Oct 2025 | Plugin framework + bambufy + nopoop (`less_waste_*` plumbing) |
> | v1.7.0 | Mar 2026 | First-class HelixScreen integration (`DISPLAY_OFF HELIX=1`, NoPoop 2) |
> | v1.7.2 | Aug 2026 | Current release (2026-08-06); the version running on our rig |

### Supported Features

- 4 filament slots with load/unload/select operations
- Per-slot color and material tracking (via `save_variables`)
- Filament presence detection (per-port switch sensors)
- Tool-to-port mapping (T0-T15 → physical ports 1-4)
- External spool mode (bypass IFS)
- Spoolman integration for filament assignment

### Architecture

IFS state is stored in Klipper `save_variables` (not Moonraker database). HelixScreen subscribes to these and sends G-code commands for operations:

| Variable | Contents |
|----------|----------|
| `less_waste_colors` | Hex color strings per slot (no `#` prefix) |
| `less_waste_types` | Material name strings per slot |
| `less_waste_tools` | Tool→port mapping array (index=tool, value=1-4 or 5=unmapped) |
| `less_waste_current_tool` | Active tool number (-1 = none) |
| `less_waste_external` | External spool mode (0/1) |

### Macro Ecosystem: bambufy vs lessWaste

Two major IFS macro packages exist for ZMOD. Both use the same `save_variables` schema and are compatible with HelixScreen:

| | **bambufy** | **lessWaste** |
|---|-----------|-------------|
| **Repo** | [function3d/bambufy](https://github.com/function3d/bambufy) | [Hrybmo/lessWaste](https://github.com/Hrybmo/lesswaste) |
| **Status** | Original, widely used | Fork of bambufy V1.2.10 with enhancements |
| **Tool macros** | T0-T3 (4 tools) | T0-T15 (16 virtual tools) |
| **Backup/failover** | Yes (`variable_backup`, **default on**) | Yes (`variable_backup`, default off) |
| **Virtual channels** | No | Yes — map >4 slicer tools to 4 physical ports |
| **Purge control** | Basic | Advanced — in-tower or out-the-back, per-material feedrates |
| **Same-filament purge** | Always purges | Configurable skip (`same_filament_purge`) |
| **Recovery** | Basic | Auto-recovery (head sensor, consume leftover, filament check) |
| **Start UI** | No | Dialog-based tool-to-port assignment at print start |

**Backup/failover** in both plugins (and in stock zMod — see below) requires a candidate slot
whose material type AND colour both exactly match the spent slot's, and whose own port sensor
reads filament present. In a typical multicolor print each slicer tool maps to a distinct
colour, so switchover silently falls through to "no match → pause" unless the user has loaded
a same-colour duplicate spool. That is the user-config outcome ninjamida's report described as
"bambufy does not support multicolor" — no plugin (nor stock zMod) disables switchover in
multicolor by code.

> **Stock zMod has its own switchover.** Before any plugin is installed, `ANALOG_PRUTOK`
> (`zmod_ifs.py:cmd_ANALOG_PRUTOK`) is wired to `head_switch_sensor`'s `runout_gcode`
> (`ad5x_display_off.cfg:39-44`). zmod's user-facing name for this is **"Infinite Spool
> Mode"**. Same type+colour+present match rule as the plugins. Always on, no toggle.
> Confirmed from zmod 1.7.1 source and on-device by raza616.

Both packages use **1-based port numbering** for hardware (ports 1-4) and define the same G-code commands (`IFS_F10`, `IFS_F11`, `IFS_F24`, `IFS_F39`, `SET_EXTRUDER_SLOT`).

### lessWaste-Specific Variables

Mostly unused by HelixScreen; `variable_backup` is the exception.

| Variable | Purpose |
|----------|---------|
| `variable_backup` | Enable/disable automatic filament backup on runout (bambufy defaults it **on**, lessWaste defaults it **off**). **Read by HelixScreen** (#1250): quoted in the runout dialog (`build_runout_detail_locked()`) and mapped onto `EndlessSpoolCapabilities::enabled`, which reaches the AMS panel and slot context menu as the backend-neutral `ams_endless_state` / `ams_endless_text` subjects — so a user is told plainly whether the printer will switch spools by itself. Absent key = unknown, never reported as off. There is no AD5X-specific subject for this; the short-lived `ams_ifs_backup_enabled` was retired in favour of the cross-backend pair. See `docs/devel/FILAMENT_MANAGEMENT.md` § "Auto-switchover plugin visibility" and § "The status line" |
| `variable_is_virtual_mode` | Virtual channel mode active (>4 tools mapped to 4 ports) |
| `variable_same_filament_purge` | Skip start purge if same filament in hotend |
| `variable_e_feedrates` | Per-tool extrusion feedrates |
| `variable_kamp` | KAMP (adaptive bed mesh) enabled |
| `variable_line_purge` | Purge line at print start |
| `PAUSE REASON=` values | `jam`, `broken`, `runout`, `empty`, `backup`, `nobackup`, `loading` (the `nobackup` reason is bambufy-only, emitted on a backup-enabled runout with no same-type+colour match — `bambufy.cfg:149`) |

### Known Issue: Zmod Slot Renumbering

Zmod has an option to rename slots from 0-indexed (0,1,2,3) to 1-indexed (1,2,3,4). This is a **slicer ↔ macro configuration issue**, not a HelixScreen issue. When enabled, the slicer sends T1-T4 instead of T0-T3, causing the wrong port to be selected.

**HelixScreen is not affected** because we read the `less_waste_tools` mapping array which maps logical tool numbers to physical ports. The mapping is set by the macro package's start-of-print UI and is always consistent regardless of the slot naming scheme. The off-by-one only affects users whose slicer sends tool numbers that don't match the macro package's expectations.

### Future Enhancements

- Parse `PAUSE REASON=` for specific filament error UI (jam, runout, empty, nobackup). Would let the plugin path skip the sensor-derived runout detector's confirm dwell entirely — see `docs/devel/FILAMENT_MANAGEMENT.md` § "Unattended runout detection"
- Support virtual channel visualization (>4 tools mapped to 4 physical ports)
- Expose `same_filament_purge` toggle in settings

## Differences from AD5M

| Aspect | AD5M | AD5X |
|--------|------|------|
| Architecture | ARM (armv7l, Cortex-A7) | MIPS (Ingenic X2600 XBurst2) |
| Display | 800x480 fbdev | 800x480 fbdev |
| Backlight | `/dev/disp` ioctl (Allwinner sunxi) | `FBIOBLANK` (standard Linux) |
| Config path | `/opt/helixscreen/` | `/usr/data/helixscreen/` |
| Multi-material | No (single extruder) | IFS (4 spools) |
| Build target | `PLATFORM_TARGET=ad5m` | `PLATFORM_TARGET=ad5x` |
| Binary | ARM static (glibc sysroot) | MIPS static (glibc) |

## Known Limitations

- **No inotify**: AD5X kernel may lack inotify support (same as AD5M) — XML hot reload may not work
- **No WiFi management**: wpa_supplicant present but may not have usable interfaces
