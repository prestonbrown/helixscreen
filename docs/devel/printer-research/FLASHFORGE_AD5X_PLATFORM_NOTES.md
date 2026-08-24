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
