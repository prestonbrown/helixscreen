# Flashforge AD5X Platform Notes (ZMOD firmware)

Captured 2026-04-27 from a live AD5X (diehardave's printer, root@ via SSH).
Complements `FLASHFORGE_AD5X_RESEARCH.md` (general research) and
`FLASHFORGE_AD5X_IFS_ANALYSIS.md` (IFS-specific). This file documents the
runtime environment we deploy into.

## Hardware

| Component | Detail |
|-----------|--------|
| SoC | Ingenic XBurst II V0.0 (x2600), 2 cores |
| ISA | MIPS32 r2/r5 with MSA SIMD (mipsisa32r2el-linux-gnu, FPU V0.0) |
| RAM | 485 MB total, ~354 MB available, ~132 MB free at idle |
| Display | 800x480 visible, 800x960 virtual framebuffer (double-buffered), 32bpp ARGB8888 |
| Touchscreen | TSC2007 resistive (i2c, `/dev/input/event2`), abs single-touch, **needs affine calibration** |
| Backlight | Sysfs `backlight_gpio0`, max=1 (on/off only — no brightness control) |
| Camera | USB Sonix SN9C200 (USB ID `0c45:6366`), `/dev/video{0,1,2}` |
| WiFi | Realtek RTL8821CU USB dongle (`8821cu.ko` out-of-tree module) |
| Storage | `/dev/mmcblk0p6` (973 MB ext4 → `/usr/prog`), `/dev/mmcblk0p7` (5.7 GB ext4 → `/usr/data`) |
| Root FS | squashfs at `/dev/root` (12.5 MB, **read-only, 100 % full** — relevant for installer disk-check bug) |
| USB | pl2303, cp210x, ch341, cdc_acm modules loaded |

## OS / Userspace

| Item | Value |
|------|-------|
| Distro | Buildroot 2020.02.1 (`/etc/issue`: "Welcome to flashforge") |
| Kernel | Linux 5.10.186+ #126 SMP PREEMPT (Ingenic GCC 12.1 Release 6.0.1, built Oct 2024) |
| libc | glibc 2.33 (`/lib/libc-2.33.so`, `mipsisa32r2el-linux-gnu`) |
| Shell | BusyBox v1.31.1 (no bash, `/bin/sh → busybox`) |
| Init | SysV (BusyBox `/etc/init.d/Sxx` scripts) |
| Watchdog | kernel `watchdogd` |
| Logger | syslog → `/var/log/messages` (no journald) |

**Missing on AD5X:** systemd, journalctl, MALLOC_CHECK_, ASAN, openssl CLI,
sftp-server (use `scp -O`), curl, bash. wget present but compiled
**without TLS** — HTTPS downloads fail. openssl 1.1 *libraries* exist at
`/usr/lib/libssl.so.1.1` but no CLI.

## ZMOD Firmware (Custom Mod)

| Item | Value |
|------|-------|
| Project | ZMOD by ghzserg — https://zmod.link/, https://github.com/ghzserg/zmod |
| Marker file | `/ZMOD` (5 bytes) |
| Chroot root | `/usr/data/.mod/.zmod/` (overlays nearly all critical paths) |
| Chroot translation | `/srv/helixscreen` (inside) ↔ `/usr/data/.mod/.zmod/srv/helixscreen` (outside) |
| Klipper config | `/opt/config/` (also `/usr/data/config/` — same FS) |
| ZMOD shell scripts | `/opt/config/mod/.shell/` |
| ZMOD variables | `/opt/config/mod_data/variables.cfg` (key: `helix = 1`, `display_off_timeout = 20`) |
| Process env (running helix) | `HOME=/`, `PWD=/srv/helixscreen`, **no `HELIX_CONFIG_DIR`** |

The chroot is set up via bind mounts visible in `/proc/mounts` —
`/dev`, `/proc`, `/sys`, `/tmp`, `/run`, `/opt/config`, `/usr/data/config`,
`/usr/data/logs`, `/usr/data/gcodes`, `/usr/prog/config`, `/usr/prog/klipper`
all bind-mounted under `/usr/data/.mod/.zmod/...`.

**Operating from outside the chroot:** prefix anything with
`chroot /usr/data/.mod/.zmod ...`. Working examples:

```sh
# Restart helix-screen from an outside-chroot SSH session:
chroot /usr/data/.mod/.zmod /etc/init.d/S80helixscreen start

# Read settings as helix sees them:
cat /usr/data/.mod/.zmod/srv/helixscreen/config/settings.json
```

## helix-screen Install Layout (in chroot view)

```
/srv/helixscreen/
├── bin/
│   ├── helix-screen          (15.5 MB stripped, MIPS)
│   ├── helix-splash
│   ├── helix-watchdog
│   └── helix-launcher.sh
├── config/                   (writable; settings + telemetry land here)
│   ├── .helix-screen.lock
│   ├── .crash_restart_count  (watchdog crash counter)
│   ├── settings.json
│   ├── telemetry_device.json
│   ├── telemetry_queue.json
│   ├── tool_spools.json
│   ├── helixscreen.env       (env vars sourced by launcher)
│   ├── helixscreen.init      (SysV init script template)
│   ├── helixscreen.service   (systemd unit — unused on AD5X)
│   └── ... (printer_database.d, themes, custom_images, etc.)
├── assets/, ui_xml/, scripts/
├── install.sh
└── release_info.json
```

`/etc/init.d/S80helixscreen` (in chroot) is the active init script with
`DAEMON_DIR="/srv/helixscreen"` substituted by the installer.
`LOGFILE="/opt/config/mod_data/log/helixscreen.log"`.

## Log Locations

Two distinct streams — do not conflate them. `/opt/config` is a bind-mount of
the durable mod config dir, so every file below has two path spellings plus a
third view from inside the chroot.

| Source | Path |
|--------|------|
| **helix-screen app log** (spdlog rotating file sink) | `/opt/config/mod_data/log/helix.log` |
| (same, alt views) | `/usr/data/config/mod_data/log/helix.log`, `/usr/data/.mod/.zmod/opt/config/mod_data/log/helix.log` |
| **Launcher stream** (`[helix-launcher]` echoes + crash/glibc stderr) | `/opt/config/mod_data/log/helixscreen.log` |
| (same, alt views) | `/usr/data/config/mod_data/log/helixscreen.log`, `/usr/data/.mod/.zmod/opt/config/mod_data/log/helixscreen.log` |
| Klipper | `/usr/data/printer_data/logs/klippy.log` |
| System (syslog) | `/var/log/messages` |
| Kernel | `dmesg` |

The launcher stream exists because ghzserg's `/etc/init.d/S80helixscreen`
hardcodes `LOGFILE="/opt/config/mod_data/log/helixscreen.log"` and redirects the
launcher subshell `>> "$LOGFILE" 2>&1`. It carries **only** the wrapper's own
output. The app log is not in it: making stdout a regular file is exactly the
case `should_add_console()` refuses to attach a console sink for (see
`include/logging_init.h`), since a console sink there would double-log every
line into the same file the structured sink writes.

The app log's path is set by `platform_pre_start` in
`assets/config/platform/hooks-ad5m-zmod.sh` (`HELIX_LOG_DEST=file` +
`HELIX_LOG_FILE`), which is also the AD5X hook. It lives under `/opt/config`
rather than `/data` because ZMOD's `TAR_CONFIG` archiver collects
`/opt/config/ /usr/prog/config/ /usr/data/logs/ /usr/prog/app_startup.sh
/tmp/*.txt` on AD5X and `/opt/config/ /data/logFiles/ /tmp/*.txt` on AD5M —
`/data/helixscreen/` is in neither, so an app log written there never reached a
support archive (issue #1249). Auto-detection is **not** what puts the log in a
file: `detect_best_target()` returns `Syslog` on Linux and never `File`, so
without the hook's `HELIX_LOG_DEST=file` the app log goes to `/var/log/messages`.

The `helixscreen.env` does NOT set `HELIX_CONFIG_DIR`, and both
`HELIX_LOG_LEVEL` and `HELIX_LOG_FILE=/tmp/helixscreen.log` ship **commented
out** (`config/helixscreen.env`) — there is no default `HELIX_LOG_LEVEL=info`;
unset means the normal precedence applies (`settings.json` `/log_level`, else
`warn`). The `log_collector` cascade includes both ZMOD filenames under both
path spellings (`src/system/log_collector.cpp`).

## ZMOD Display Lifecycle (CRITICAL — affects helix stability)

ZMOD ships a Klipper macro chain that **kills helix-screen ~20 s after every
Klipper start** and respawns it. This is by design — it's how ZMOD hands
the framebuffer between the native FlashForge UI and the alternate UI
(helix or guppy) — but every kill triggers our `Application::shutdown()`
which is fragile in the teardown path.

The chain (in `/opt/config/mod/base_display_off.cfg`):

```ini
[delayed_gcode _PREPARE_DISPLAY_OFF]
initial_duration: 1
gcode:
    {% set display_off_timeout = printer.save_variables.variables['display_off_timeout']|default(20) | int %}
    UPDATE_DELAYED_GCODE ID=_TEST_DISPLAY_OFF DURATION={display_off_timeout}

[delayed_gcode _TEST_DISPLAY_OFF]
initial_duration: 0
gcode:
    RUN_SHELL_COMMAND CMD=zdisplay PARAMS="test"
```

`zdisplay.sh test` (lives at `/opt/config/mod/.shell/zdisplay.sh`):

```sh
if [ $1 = "test" ] && grep -q display_off.cfg /opt/config/printer.cfg; then
    killall firmwareExe helix-watchdog helix-screen helix-splash
    sleep 1
    if grep -q "guppy = 1" .../variables.cfg || grep -q "helix = 1" .../variables.cfg ; then
        /opt/config/mod/.shell/zguppy.sh up   # respawns helix
    else
        xzcat .../screen_off.raw.xz > /dev/fb0
    fi
```

Other arg paths (`off`, `guppy`, `helix`) all also `killall ... helix-screen
helix-splash` then either repaint the framebuffer or call zguppy.sh.

**Implication for helix:** `Application::shutdown()` runs on every SIGTERM.
If shutdown teardown crashes (the v0.99.46–48 family of L081-related
SIGBUS during `lv_deinit` / static destructors), the watchdog burns through
restart credits and eventually gives up, leaving the device stuck on a
crash dialog or blank screen. Fix #2 (`crash_handler::uninstall()` moved
from first → last in `shutdown()`) is the load-bearing fix for AD5X
stability — without it, the ZMOD respawn loop is unsurvivable.

`display_off_timeout` is in **seconds**, default 20, minimum-clamped to 5
in the macro. There is no helix-side configuration that prevents this kill.

## v4l2 / H.264 Codec — BROKEN in AD5X kernel

The AD5X firmware kernel ships a broken H.264 codec module. Any
`v4l2_open()` against `/dev/video*` traps the kernel:

```
Process v4l2-ctl (pid: …)
Call Trace:
  dma_coherent_mem_available+0xc/0xcc   ← BadVA: 0000000c (NULL+12 deref)
  av_mallocz+0x18/0x48
  h264_decode_init+0xf4/0x1bc
  fops_vcodec_open+0x88/0x298
  v4l2_open+0xd0/0x164
  ...
```

The faulting process gets killed. Other processes mmap'd into the same
v4l2 region take SIGBUS on next access. We cannot work around this from
userspace; the fix would be a kernel module rebuild and FlashForge isn't
shipping one.

**Mitigation in helix:**

- `HELIX_HAS_CAMERA=0` is already set for `HELIX_PLATFORM_AD5X` in
  `lv_conf.h:856-863` — the camera widget, `CameraStream`, QR scanner
  overlay, and camera-config modal are all compile-time excluded.
- Pending fix: gate the webcam *discovery* code in
  `moonraker_discovery_sequence.cpp` on `HELIX_HAS_CAMERA` so we don't
  even probe `server.webcams.list` on platforms that can't render it.
  (We've never directly opened `/dev/video*` from helix, so the discovery
  call alone doesn't trigger the kernel bug — but it's still wasted I/O.)

## SSH Access

- Default credentials: `root` / `root` (dropbear SSH on port 22)
- No sftp-server — use `scp -O` (legacy SCP protocol)
- Use `sshpass -p root ssh ...` for non-interactive automation
- Working from outside the chroot is fine for SSH; use `chroot /usr/data/.mod/.zmod ...` for any command that needs the helix-screen view

## Resource Limits (`ulimit -a`)

| Limit | Value |
|-------|-------|
| core file size | **0** (no core dumps allowed by default) |
| open files | 1024 |
| max user processes | 3787 |
| pending signals | 3787 |
| max locked memory | 64 KB |
| stack size | 8192 KB |
| data seg / virtual memory | unlimited |

The `core file size = 0` means **no automatic core dumps** for crash analysis
— rely on our crash handler writing `crash.txt`. (See
`crash_handler.cpp` and the path-fallback discussion in
`docs/devel/CRASH_REPORTER.md`.)

## Cross-Compilation Notes

- Toolchain: Ingenic MIPS gcc 12.1 Release 6.0.1 (xburst2 target, glibc 2.33)
- Vendor toolchain naming: `mips-linux-gnu-gcc`
- Build flag: `-DHELIX_PLATFORM_AD5X`
- Release asset: `helixscreen-ad5x.zip` (per `release_info.json`)
- Manifest currently omits the `.zip` for K1/AD5X — revert by 1.0

## Installer Caveats

`scripts/install.sh` `check_disk_space()` runs `df` on
`dirname(INSTALL_DIR)`. On AD5X (`INSTALL_DIR=/srv/helixscreen`), that's
`/srv` — which on this overlay setup either doesn't exist as its own mount
or inherits from the **read-only `/dev/root` (12.5 MB, 100 % used)**.
Result: installer aborts with "Insufficient disk space (Required: 50MB,
Available: 0MB)" even though `/usr/data` (where the binary actually
extracts) has gigabytes free.

Workaround for end users: skip the installer and manually `tar xzf` the
release tarball into `/srv/helixscreen` (after stopping helix). Real fix:
have `check_disk_space()` `df` the install dir itself if it exists, only
walk up to its parent if it doesn't. (Pending fix.)

## Operational Cheatsheet

```sh
# Restart helix from outside chroot:
chroot /usr/data/.mod/.zmod /etc/init.d/S80helixscreen start

# Watch the helix log:
tail -f /opt/config/mod_data/log/helixscreen.log

# Find the running helix-screen PID:
pgrep -af helix-screen

# Look at the active config dir as helix sees it:
ls /usr/data/.mod/.zmod/srv/helixscreen/config/

# Check ZMOD lifecycle vars:
cat /opt/config/mod_data/variables.cfg | grep -E '^(helix|guppy|display_off)'

# Manually trigger the display-handoff macro (will kill helix):
echo "RUN_SHELL_COMMAND CMD=zdisplay PARAMS=test" >> /tmp/some-gcode

# Check kernel for v4l2 panics:
dmesg | grep -iE 'sigbus|h264|v4l|bus error|alignment'
```

## Known Issues

| ID | Summary |
|----|---------|
| #874-class | Teardown SIGBUS during ZMOD-triggered SIGTERM (Fix #2 in flight: uninstall handler last) |
| installer disk-check | "0MB free" false negative on overlay rootfs (Fix pending) |
| `HELIX_CONFIG_DIR` unset | Falls back to relative `"config"` — works only because helix-screen CWD is `/srv/helixscreen` |
| no core dumps | `ulimit -c = 0` by default; rely on `crash.txt` only |
| no ASAN | Insufficient memory for ASAN-instrumented build; can't reproduce here |
| webcam discovery probes | Wasted RPC even though widget is gated (fix pending) |

## Helix Rig Observations (2026-08-24, our own AD5X)

First-party rig commissioned 2026-08-23: ZMOD 1.7.2 (full variant) on a
factory-restored **stock base 1.1.7** (the unit shipped on 3.1.4 — above ZMOD's
discrete supported list, which tops out at 3.1.0; the required path is restore
DOWN, never update up). MCU is the **stock Klipper 12 blob**
(`stm32f103xe`, `20241125_172251`); `klipper13=0` in the ZMOD variables. Commissioning
traps (USB-stick re-trigger, empty `/opt`, port signature) are recorded in
`docs/devel/printers/FLASHFORGE_AD5X_SUPPORT.md`.

### Network / SSH

- Address **192.168.1.66** (main LAN). The earlier "SSH stalls at banner exchange"
  puzzle was **IOT-segment-specific** — from the main LAN the dropbear handshake
  completes in ~22 ms with key auth. Whatever mangles it lives on the 192.168.30.x
  VLAN, not the printer.
- Port sweep from the main LAN shows **all four ports open** (22, 80, 7125, 8899) —
  8899 open deviates from the "ZMOD closes 8899" signature claimed at commissioning
  time; do not treat 8899 as a stock-vs-ZMOD discriminator without re-checking.
- `scp` needs `-O` or `cat`-over-ssh (no `sftp-server`, confirmed).

### Moonraker surface (live-verified)

- The WebSocket endpoint is **`ws://<host>:7125/websocket` only**. Port 80 is
  `Zmod httpd/1.1.0` and serves the Fluidd SPA for **every** path — a WS handshake
  sent to port 80 gets `200 OK` + the SPA HTML, not a protocol error.
- **`objects/query` returns success-with-empty-status for objects that do not
  exist** (`mod_params`, `zmod_ifs`, `zmod_color` all "answer" while absent from
  `objects/list`). A query response is therefore never proof of object presence;
  `objects/list` is the presence check. Matters for any capability-detection code.
- 269 objects registered. Present and confirmed: `SAVE_ZMOD_DATA` (macro),
  `SET_EXTRUDER_SLOT` (macro), `zmod_ifs_switch_sensor head_switch_sensor`,
  `zmod_ifs_motion_sensor ifs_motion_sensor`, `fan_generic fanM106`,
  `filament_switch_sensor head_switch_sensor`. Absent: `SET_MOD` (Forge-X-only —
  our Forge-X provider row cannot misfire here).
- `save_variables.variables` is ZMOD's **flat mod-param dict** (41 keys observed:
  `klipper13`, `load_zoffset`, `helix`, `guppy`, `fix_e0011`, `fix_e0017`,
  `china_cloud`, `display_off_timeout`, …). The `gcode_offsets.z` key our
  z-offset provider reads appears **only after a z-offset has actually been
  saved** — a freshly commissioned rig legitimately has none.
- Klippy `Stats` lines are **single-line with inline `mcu:` / `eboard:`
  sections** — per-MCU lines do not start at column 0 (`grep '^mcu @'` finds
  nothing; parse inline). Spool-weight data shows up as Stats fields
  (`filamentValue: temp=`, `cutValue: temp=`, `weightValue: temp=840.0` — grams
  masquerading as a temperature) but there is **no `weightValue` object** in
  `objects/list`, so the printer-DB heuristic keyed on it will not match;
  the higher-confidence signals (`zmod_ifs`, hostname, `SET_EXTRUDER_SLOT`)
  carry detection.

### Klipper-12 retransmit baseline (control arm for the TTC investigation)

163 Stats lines over a ~70-minute idle session:

| MCU | bytes_retransmit | Notes |
|-----|------------------|-------|
| `mcu` | flat **9** | boot-time only, never moved |
| `eboard` | **≤ 9** | 103 samples at 0, 60 at 9 after one blip |

Compare the incident rigs (both on **forced Klipper 13**, one of them cold-start
idle at 581 s uptime): `eboard bytes_retransmit=50, retransmit_seq=542` against
`mcu=9` — the eboard 5x noisier than the main MCU. On Klipper 12 the eboard
matches or beats the mcu. The noisy-eboard signature appears **only on
Klipper 13**; this is the strongest evidence short of deliberately forcing 13
on the rig (deliberately not done).

### Gcode transport test (wedge check)

The commissioning-session report "any gcode macro POSTed to
`/printer/gcode/script` wedges Moonraker ~40 s" **did not reproduce** on the
rig's current state (post config-reload, main LAN, 2026-08-24):

| Phase | Command RTT | Worst `server.info` RTT after |
|-------|-------------|-------------------------------|
| WS macro `GET_ZCOLOR SILENT=1` | 20 ms | **7.4 ms** over 60 s |
| WS plain `M105` | 14.5 ms | 1374 ms — one blip ~16 s after, recovered in ~6 s, unattributed |
| HTTP POST macro | 0.0 s (immediate) | 14 ms over 75 s |

Baseline RTT was 4-7 ms throughout. Nothing approaches a 40 s wedge on either
transport. The `IFS_STATUS` follow-up (the large-`respond_info` macro our IFS
backend polls) was also clean: worst 10 ms over WS, 8 ms during the HTTP phase.
Treat the 40 s wedge as **unconfirmed on current firmware state**, not as an
operating assumption. Probable root cause of the original report: the commissioning
observations were made over the **wifi path (192.168.30.254), which drops ~21.7%
of packets** (ethernet: 0%) — tens of seconds of TCP retransmit stall on a lossy
link presents exactly like a "wedged" Moonraker. Same failure family as the
IOT-segment SSH banner stall: segment, not printer.

### Restart mechanics + get_status patch validation (2026-08-24)

**Moonraker's `POST /printer/restart` returns `{"result":"ok"}` but does NOT
restart klippy on this rig** — klippy is PPID 1 (orphaned by the FlashForge boot
chain, not Moonraker-supervised) and continues running with old bytecode.
The only verified restart is a full `reboot` over SSH (a few seconds of grace,
then ~40 s down, klippy ready ~40 s later; verify by PID change — 1664 → 1669 —
never by the endpoint response). Also: `python3 -m py_compile` **writes
`__pycache__` itself**, so pyc mtimes after your own compile prove nothing about
what klippy imported. Post-reboot, SSH key auth was dropped (password `root`
via sshpass works).

**get_status() patch validated on-rig** (upstream handoff candidate; the mod
source has no public git — these files ship only in zmod release tarballs, so
the patch is handed to ghzserg directly; **filed 2026-08-24 as
ghzserg/zmod#699** with the patch inline, from Preston's account). Patch adds `get_status()` to
`zmod_ifs` + `zmod_color` (extras install as symlinks from
`klippy/extras/` to `/usr/data/config/mod/.shell/`; deploy = replace the .shell
targets; backups `/usr/data/*.pre-getstatus.bak`). After a real reboot:
`objects/list` includes both; `objects/query` returns full live status
(`zmod_ifs`: available/color_limit/RawData/State/Ports/Silk/Chan/Insert/
NeedInsert/Stall/stall_state; `zmod_color`: ifs/display/color_limit/
valid_types/extruder_sensor/channel/slots[]); `objects/subscribe` delivers
correct initial snapshots and Moonraker's diff-based updates behave (no frames
on an idle rig — expected). 40 s soak at 4 Hz subscription: zero errors. The
patch also fixes an upstream boot-window bug: `IfsData.__init__` wrote
`LastResponseRaw` while `get_values()` reads `lastResponseRaw`, so `IFS_STATUS`
raised `AttributeError` before the first F13 poll on stock 1.7.2. Note: a ZMOD
update overwrites these files; the rig carries the patch until then.

**Because of this patch, the rig answers capability questions that NO user's
printer answers** (`zmod_ifs`/`zmod_color` in `objects/list`, queryable status
where stock returns nothing). Any capability claim verified on this rig MUST be
checked in BOTH states - patched, and restored from the `.pre-getstatus.bak`
backups - before it becomes a claim about AD5X behavior. A finding that passes
only on this box is a patched-rig artifact and will fail for every real owner.
This matters doubly for the IFS tool-mapping echo question, where our shipped
finding is that AD5X does NOT echo the mapping - the patch could silently
invert that. **Executed 2026-08-24 for the three core claims** - with stock
extras restored (md5-verified pristine, fresh boot), auto-detection still
matched and persisted `FlashForge Adventurer 5X`, the ZMOD z-offset provider
still activated (`[ZOffset] Enabling firmware z-offset persistence (ZMOD)`),
and the AD5X IFS backend still created and started. All three hold on stock;
the patched state was then restored (md5-verified, fresh boot, objects live).
The tool-mapping echo question has NOT yet been dual-state checked - it needs
loaded spools to test meaningfully.

### HelixScreen end-to-end on the rig (2026-08-24)

Desktop build (SDL dummy, isolated `HELIX_CONFIG_DIR`, pinned `--remote-socket`)
against the live printer — the first time AD5X support has run on real hardware:

- **Auto-detection works**: with the printer type unset, the heuristic chain
  matched and persisted `FlashForge Adventurer 5X` (zmod_ifs sensors, hostname
  `flashforge`, `SET_EXTRUDER_SLOT`, `temperature_sensor weightValue`, mips).
- **ZMOD z-offset provider activates on hardware**: `[ZOffset] Enabling firmware
  z-offset persistence (ZMOD)` at discovery.
- **AD5X IFS backend runs live**: backend created + subscribed, slots 0-3
  initialized, head-sensor events flowing, the `Adventurer5M.json` +
  `GET_ZCOLOR SILENT=1` color-truth path executing, remote-Moonraker upload-path
  distinction applied.
- **Resilience exercised for free**: the rig rebooted mid-session (cause
  unresolved — see below); the app's retry loop reconnected, auto-closed the
  Connection Failed modal, re-ran discovery, and re-initialized the IFS backend.
- Six minutes of logs, four warn/error lines, all benign (isolated-config
  backup path, stale seeded sensor config, expected `Method not found` for
  plugins this Moonraker lacks). The rig's gcodes dir contains a bare `.3mf`
  (`FlashForge-TestModel-01.3mf`) that Moonraker cannot metascan — our
  thumbnail fallback chain degrades gracefully.
- Desktop runs log to **syslog** (not stdout): read via
  `journalctl --user | grep <pid>`.
- The rig rebooted several times this session; only two were this validation's
  (fresh-import restarts). The rest were **other agents working the shared rig
  concurrently** — initially misread here as unexplained reboots. The AD5X rig is
  a shared test device: coordinate reboots/state changes across sessions, and
  treat any unexpected rig state change as a peer's action before suspecting
  the hardware. Note also: the on-rig helix-screen is a peer session's
  `feat/ad5x-oobe` build of 0.99.115 (original at
  `bin/helix-screen.0.99.107.bak`), not stock — verify which build produced any
  on-rig UI evidence. The full 2026-08-24 reboot/outage timeline reconciles as
  peer actions: 12:21 + 12:47 = this validation's reboots, 12:33 = Preston's
  bench power-cycle, ~12:36 = a peer's helix-screen service restart.
- **The "unreachable while the screen kept rendering" event is SOLVED, and it
  was not a driver or hardware fault**: wlan0 (rtl8821cu) was administratively
  down the entire time (and does not come back with `ip link set wlan0 up` as
  root), so eth0 (stmmaceth) was the only live interface — and it was holding
  an address from a manual one-shot `udhcpc -n -q` with no renewal daemon.
  When that lease expired, eth0 kept carrier and silently lost its address:
  the box stayed up and rendering, unreachable from the network. The
  post-reboot state runs the firmware's own persistent
  `/sbin/udhcpc -i eth0 -p /var/run/udhcpc.pid`, which is why it has been
  stable since. **Durable fix worth doing: a DHCP reservation or static IP for
  this rig** — multiple agents drive it remotely and any manual network
  bring-up is a silent time bomb. Never use one-shot DHCP flags on it.
- **ZMOD config bug that surfaces in our error UI**: `base_display_off.cfg:68`
  hardcodes `BED_MESH_PROFILE LOAD=auto`, but this printer's saved profile is
  `MESH_DATA` — every `DISPLAY_OFF` (i.e. every boot, via the display-handoff
  chain) throws two gcode errors that land in the on-device error UI. This is
  the source of the "bed_mesh: Unknown profile [auto]" lines in klippy startup
  logs; it is not a missing-profile condition on our side.

### Bed mesh on a freshly-modded AD5X: why `auto`, and what we do about it

Expanding the `LOAD=auto` entry above with the mechanism, because the remedy and
the open risk both follow from it.

**Where the name comes from.** `auto` is not arbitrary — it is the default
`PROFILE` of ZMOD's own `AUTO_FULL_BED_LEVEL` macro (documented in ZMOD's
Calibrations page). So `_PREPARE_DISPLAY_OFF` is not asking for a magic profile,
it is reloading the mesh it assumes ZMOD's leveling created. The assumption holds
for a printer that has been levelled with ZMOD's macro and fails for one that has
not — ours carries `MESH_DATA` from stock leveling performed before ZMOD existed
on the box. Any freshly-modded printer is in that state until its first
`AUTO_FULL_BED_LEVEL`.

**User-side remedy: run `AUTO_FULL_BED_LEVEL` once after installing ZMOD.** It
saves to `auto` by default and creates exactly what the handoff path reloads.
Copying the stock profile across with `BED_MESH_PROFILE SAVE=auto` also silences
the errors and costs no probe cycle, but hands ZMOD a mesh produced by a
different routine without its nozzle clean — and ZMOD's `MESH_TEST` validates the
saved mesh against a fresh centre probe and re-levels on a >=0.3 mm discrepancy.
Prefer letting ZMOD generate the mesh it expects to own.

There is no setting to point the handoff path at a different profile name;
`AUTO_FULL_BED_LEVEL` takes `PROFILE` as a parameter but `_PREPARE_DISPLAY_OFF`
hardcodes `auto`. Creating the profile is the only user-side fix, which is why
this is worth an upstream report rather than a local workaround.

**UNVERIFIED, and it decides how much this matters.** The macro runs
`BED_MESH_CLEAR` *before* `BED_MESH_PROFILE LOAD=auto`. If the clear succeeds and
the load fails, the printer may be left with no active mesh after every screen
handover — which happens on every boot in alt-screen mode. Mitigating: `_START_PRINT`
issues its own `BED_MESH_PROFILE LOAD={mesh}` from a configured variable, so the
print path probably reloads something, and `PRINT_LEVELING` defaults to 0 (no
re-mesh per print) meaning it leans on that saved profile. Nobody has checked
`printer.bed_mesh.profile_name` after a handoff. Two red lines in the error UI and
a silently unmeshed bed are very different bugs; this one query separates them.

**VERIFIED 2026-08-24 (rig, read-only + one handover): the handover itself is
harmless-to-idle — but the print-start path is worse than assumed.**
- Baseline (idle, before any handover): `printer.bed_mesh.profile_name` is already
  `''` with `MESH_DATA` saved — no mesh is ACTIVE at idle on this firmware,
  handover or not. So `BED_MESH_CLEAR` + failing `LOAD=auto` clears nothing that
  was in use; the "silently unmeshed bed after handover" worst case does not
  materialize at idle. klippy survives the handover (same PID); Moonraker's
  brief unresponsiveness right after `DISPLAY_OFF` is transient.
- The alt-screen config (`ad5x_config_off.cfg`) contains **no
  `BED_MESH_PROFILE LOAD` site at all** — its only BED_MESH mentions are
  settings-menu text. The mitigation assumed above ("`_START_PRINT` reloads from
  a configured variable") holds only for the NATIVE-screen config path
  (`ad5x.cfg:540`, `LOAD={printer.bed_mesh.profile_name}` — which at runtime is
  the empty name). With the default `print_leveling=0` ("don't build bed mesh on
  each print"), an alt-screen AD5X neither builds nor loads a mesh at print
  start unless the SLICER emits `BED_MESH_PROFILE LOAD` in the file.
  Unverified: the rig carries only `.3mf` packages (no greppable `.gcode`), so
  whether FlashForge/Orca profiles emit the load is unconfirmed. **If they do
  not, default alt-screen prints run without mesh compensation** — that is the
  question worth asking ghzserg, and it is bigger than the `LOAD=auto` error.

**Our side: capability question identified, deliberately NOT built yet.** If we
ever act on this, it belongs behind the `z_offset_persistence.h` shape — a
provider table keyed on a detection predicate, vendor names confined to one .cpp,
answering one question:

```cpp
/// The mesh profile this firmware expects to exist, or "" when it has no such
/// convention.
std::string firmware_expected_mesh_profile(const PrinterDiscovery& hw);
```

The bed-mesh UI could then offer to create a missing expected profile instead of
surfacing a raw gcode error. It is currently vendor-free (no zmod/forge/creality
mentions anywhere in `ui_panel_bed_mesh.cpp` or `ui_bed_mesh.cpp`) and this would
keep it that way.

Not built, for three reasons: the consequence above is unverified and only one
version of it justifies the machinery; an upstream fix would make it dead code
encoding a convention that no longer exists; and it is one printer's observation.
Trigger for revisiting: upstream declines, or a second reporter hits it.

### RESOLVED (2026-08-24): the missing step is `SAVE_CONFIG`, and `LOAD=auto` is not a bug

Ran `AUTO_FULL_BED_LEVEL` on the rig and measured each stage. This supersedes the
open questions in the two sections above.

| stage | active profile | live mesh | new errors per handover |
|---|---|---|---|
| before any leveling | `''` | none | 2 |
| after `AUTO_FULL_BED_LEVEL` alone | `''` | none | 2 |
| after `AUTO_FULL_BED_LEVEL` + `SAVE_CONFIG` | `auto` | 25 pts | 0 |

**`AUTO_FULL_BED_LEVEL` alone does not persist the profile.** Klipper's
`BED_MESH_PROFILE SAVE` stages into the pending config; only `SAVE_CONFIG` writes
it to `printer.cfg`. Measured directly: `auto` was present in `bed_mesh.profiles`
after the macro and absent after the next handover, so a staged-but-unsaved
profile does not survive the transition. Note ZMOD's AD5X page:
`NEW_SAVE_CONFIG` does NOT work on this model, plain `SAVE_CONFIG` only.

*Mechanism not established.* An earlier draft of this section said the handover
restarts klippy and that is what discards it. That was an inference, not a
measurement — the klippy restart actually observed here was `SAVE_CONFIG`'s own
(HTTP 503, ~50s to ready), a different event. A separate measurement on this rig
found klippy surviving a `DISPLAY_OFF` handover **same-PID**, which argues against
the restart explanation; the handover's own `BED_MESH_CLEAR` combined with
staged-profile limbo is the likelier route. The table below holds either way — it
records what was measured at each stage, not why.

**This is the nastiest part of the whole thing.** `AUTO_FULL_BED_LEVEL` alone
*looks* like it worked — the profile appears in `bed_mesh.profiles`,
`profile_name` reads `auto`, every observable is correct. It exists in live state
only and disappears at the next restart, with no error naming the cause. Our first
handover test failed with `auto` apparently present; it had already been lost.

**With the profile persisted, `LOAD=auto` is ghzserg's mesh-loading mechanism for
alt-screen mode and it works.** After a handover: `profile_name == 'auto'`, a live
25-point mesh, error count flat across the transition. So the earlier framing —
"alt-screen prints run unmeshed" — is wrong once bootstrapped. The handover IS the
alt-screen mesh loader.

**The real defect is narrow: the fresh-install bootstrap.** A newly-modded AD5X has
no `auto` profile, so it is both noisy and unmeshed until its owner runs
`AUTO_FULL_BED_LEVEL` **and** `SAVE_CONFIG` — and nothing tells them the second
step exists. Upstream ask: gate the `LOAD` on profile existence, or surface a
bootstrap hint. Much smaller and more defensible than "your mesh loading is broken".

**Probe sanity check.** The fresh mesh is consistent with the stock one, which
rules out a probe fault and calibrates expectations for anyone reading these
numbers later:

```
MESH_DATA  min=-5.044 max=-3.825 spread=1.219mm  5x5   (stock leveling)
auto       min=-4.938 max=-3.778 spread=1.159mm  5x5   (ours)
```

The ~-4mm absolute offset looks alarming against a typical +/-0.2mm mesh but is
this machine's probe-offset convention, not a fault — stock sits in the same place.
Bed flatness spread is what matters and the two agree to within 0.06mm.

**Our side: still not building `firmware_expected_mesh_profile()`.** The reasoning
above stands and is stronger now — the upstream behaviour is correct once
bootstrapped, so there is no capability for us to detect. If anything is ever worth
doing here it is a first-run hint, not a provider table.
