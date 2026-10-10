# FlashForge Adventurer 5M / 5M Pro Support

HelixScreen runs on the AD5M's built-in 4.3" 800x480 screen, replacing GuppyScreen on Forge-X,
KlipperScreen on Klipper Mod, or the stock UI under ZMOD. User install steps live in
[`../../user/guide/install-ad5m.md`](../../user/guide/install-ad5m.md); this page covers the
build targets, deploy loop and the platform quirks the code works around.

## Hardware

| Spec | Value |
|------|-------|
| CPU | Allwinner, Cortex-A7 (armv7-a hard-float, NEON VFPv4) |
| RAM | 110 MB, over a disk-backed swap file |
| C library | glibc 2.25 |
| Display | 800x480 32bpp framebuffer (`fbdev`), evdev touch |
| Backlight | Allwinner DISP2 ioctls on `/dev/disp`, no `/sys/class/backlight` |
| Init | BusyBox SysV init |

## Firmware variants

The installer detects the firmware mod (`detect_mod_flavor` in
`scripts/lib/installer/platform.sh`) and picks the install directory, init script and platform
hook from it. `scripts/device-profile.sh` runs that same detection on a live device, so the
deploy targets and the installer can never disagree.

| Firmware | Replaces | Install dir | Init script | Platform hook |
|----------|----------|-------------|-------------|---------------|
| Forge-X | GuppyScreen | `/opt/helixscreen` | `/etc/init.d/S90helixscreen` | `hooks-ad5m-forgex.sh` |
| Klipper Mod | KlipperScreen | `/opt/helixscreen` (v00.06+), `/root/printer_software/helixscreen` (older) | `/etc/init.d/S80helixscreen` | `hooks-ad5m-kmod.sh` |
| ZMOD | its own screen | `/srv/helixscreen` | `/etc/init.d/S80helixscreen` | `hooks-ad5m-zmod.sh` |

Writable state (config, cache) lives under `/data/.helixscreen/`. The dot keeps it out of the
print-file list, because the vendor firmware exposes all of `/data` as Moonraker's gcodes root.

Hooks live in `assets/config/platform/`. Presets for each variant are in
`assets/config/presets/ad5m*.json`. A ready-made Forge-X fork with HelixScreen preinstalled is
published at [prestonbrown/ff5m](https://github.com/prestonbrown/ff5m).

ZMOD stores the z-offset outside `gcode_move`; see [`../Z_OFFSET_PERSISTENCE.md`](../Z_OFFSET_PERSISTENCE.md).

## Build targets

| Target | Toolchain | Linking | Use |
|--------|-----------|---------|-----|
| `ad5m` | ARM GCC 10.3 in Docker (`make ad5m-docker`) | fully static | Release tarballs, sideload onto Forge-X / Klipper Mod / ZMOD |
| `ad5m-br` | caller-provided (buildroot) | dynamic | Packaged by an outer build system such as Klipper Mod's buildroot |

Both build the same armv7-a binary with `-DHELIX_PLATFORM_AD5M`, `-Os -flto`, the `medium` and
`large` font tiers. The `HELIX_HAS_*` gates in `mk/cross.mk` compile out the label printer
(`HELIX_HAS_LABEL_PRINTER := 0`) and the vendor filament backends CFS, IFS, ACE, QIDI and Snapmaker
(`HELIX_HAS_CFS := 0` and friends). AFC, Happy Hare, OpenAMS and the tool changer have no gate and
are always built. The `ad5m` target links statically because the toolchain sysroot
carries glibc 2.33 symbols (`stat`, `fcntl`, `pow`) that the printer's glibc 2.25 lacks. The
toolchain survey behind that choice is in
[`../printer-research/FLASHFORGE_AD5M_TOOLCHAIN_NOTES.md`](../printer-research/FLASHFORGE_AD5M_TOOLCHAIN_NOTES.md).

```bash
make ad5m-docker                          # build/ad5m/bin/helix-screen + helix-splash
make deploy-ad5m AD5M_HOST=<ip>           # default host: ad5m.local
make deploy-ad5m-fg AD5M_HOST=<ip>        # run in the foreground with debug logging
make deploy-ad5m-bin AD5M_HOST=<ip>       # binaries only, fast iteration
make package-ad5m                         # release tarball
```

The printer's BusyBox has no sftp-server or rsync, so `deploy-ad5m` streams files with
`cat | ssh` and `tar | ssh`. It also refreshes whichever `S80`/`S90` init script is present and
installs the platform hook the device profile selects.

### `ad5m-br`: building inside Klipper Mod

[xblax/flashforge_ad5m_klipper_mod](https://github.com/xblax/flashforge_ad5m_klipper_mod) can
build HelixScreen as a native variant beside GuppyScreen and KlipperScreen. The caller provides:

- `CROSS_COMPILE` and `CC`/`CXX`/`AR`/`RANLIB`/`STRIP` for an armv7-a hard-float glibc toolchain
  (buildroot: `arm-buildroot-linux-gnueabihf-`).
- A sysroot with `libssl`, `libcrypto`, `libstdc++`, `libpthread`, `libm`, `libdl`, `libz`, and
  `libevdev` (`BR2_PACKAGE_LIBEVDEV=y`).

`libhv`, `spdlog`, `libnl` and `libwpa_client` are always built in-tree from their pinned,
patched submodules.

```make
# buildroot package (helixscreen.mk)
define HELIXSCREEN_BUILD_CMDS
    $(MAKE) $(TARGET_CONFIGURE_OPTS) PLATFORM_TARGET=ad5m-br -C $(@D)
endef

define HELIXSCREEN_INSTALL_TARGET_CMDS
    $(MAKE) $(TARGET_CONFIGURE_OPTS) PLATFORM_TARGET=ad5m-br \
        install DESTDIR=$(TARGET_DIR) -C $(@D)
endef
```

`make install DESTDIR=...` (the `install` target in `Makefile`) writes `opt/helixscreen/` with
`bin/` (`helix-screen`, plus `helix-splash` and `helix-watchdog` when built), `ui_xml/`,
`assets/` (fonts, images, sounds, config including every preset and platform hook) and, after a
Docker build, `certs/ca-certificates.crt`. Writable state is not installed.
`src/application/data_root_resolver.cpp`
strips `/bin` from the binary's path to find `/opt/helixscreen/`, so no environment variables
are needed.

To check the install layout locally, build inside the `helixscreen/toolchain-ad5m` image
(`make docker-toolchain-ad5m`) with `PLATFORM_TARGET=ad5m-br`, the `arm-none-linux-gnueabihf-`
tools passed as `CC`/`CXX`/etc., and `SKIP_OPTIONAL_DEPS=1`, then run
`make install PLATFORM_TARGET=ad5m-br DESTDIR=build/ad5m-br-staging`. From a worktree, also
bind-mount the main repo at its own absolute path so the `lib/` symlinks resolve.

## What the installer changes on Forge-X

The install-time calls are made from `configure_platform` in `scripts/lib/installer/main.sh`, and
the functions they call live in `scripts/lib/installer/forgex.sh`. `uninstall_forgex` in
`forgex.sh` reverses the changes:

- `disable_stock_firmware_ui` comments out `ffstartup-arm` in the printer's /opt/auto_run.sh. It runs
  before the Forge-X case and no-ops where those files are absent.
- `configure_forgex_display` switches Forge-X's display mode in `variables.cfg`, records the previous
  mode for uninstall, and de-execs the `S80guppyscreen`, `S35tslib` and `guppyscreen` launchers. A
  failure here aborts the install.
- `dismiss_forgex_feather_promo` sets `show_feather_promo = 0` in `variables.cfg`, so the Feather
  display offer is never composed. Uninstall does not restore it.
- `patch_forgex_screen_sh` patches the `backlight` case of Forge-X's /opt/config/mod/.shell/screen.sh
  (see below).
- `patch_forgex_screen_drawing` guards each draw command in screen.sh so nothing else writes the
  framebuffer while HelixScreen runs.
- `install_forgex_logged_wrapper` wraps `.bin/exec/logged` so `--send-to-screen` (direct writes to
  /dev/fb0) is stripped while HelixScreen is active.

`uninstall_forgex` undoes the screen.sh patches with `unpatch_forgex_screen_sh` and
`unpatch_forgex_screen_drawing`, removes the wrapper with `uninstall_forgex_logged_wrapper`, restores
the recorded display mode, and re-enables the vendor launchers.

## Backlight

`BacklightBackendAllwinner` (`src/api/backlight_backend.cpp`) drives `/dev/disp` with the DISP2
ioctls `SET_BRIGHTNESS` (0x102), `GET_BRIGHTNESS` (0x103), `BACKLIGHT_ENABLE` (0x104) and
`BACKLIGHT_DISABLE` (0x105), range 0-255. Two quirks shape it:

- **Inverted PWM.** The DISP2 driver can land in a state where higher values are dimmer. The
  backend's constructor cycles DISABLE, then ENABLE plus full brightness, which clears it. The
  reset is skipped on the Sonic Pad (R818), where it destabilises the display, and when
  `/display/backlight_enable_ioctl` is false (the Centauri Carbon's opt-out).
  By hand: `chroot /data/.mod/.forge-x /root/printer_data/py/backlight.py 0`, then `90`.
- **Forge-X dims the screen behind our back.** Its `S99root` needs a `backlight 100` to bring
  the panel up, and Klipper's `reset_screen` delayed_gcode later dims to 10%. The patched
  screen script exits early for any value other than 100 while `/tmp/helixscreen_active` exists
  (the Forge-X and ZMOD platform hooks create it on start and remove it on stop). As a second
  line, `DisplayManager` re-applies the user's brightness 20 s after startup on the Allwinner
  backend, after the delayed_gcode has fired.

Boot order, then: the Forge-X splash, the platform hook creates the flag, `S99root`'s
`backlight 100` passes, HelixScreen starts and resets the driver, Klipper becomes ready and its
dim is blocked, and the 20 s timer restores the configured brightness.

## Files

| Path | Role |
|------|------|
| `mk/cross.mk` | `ad5m` / `ad5m-br` target blocks, `deploy-ad5m*`, `package-ad5m` |
| `config/helixscreen.init` | SysV init script (installed as `S80` or `S90`) |
| `scripts/lib/installer/main.sh` | `configure_platform`: the install-time order of the Forge-X steps |
| `scripts/lib/installer/forgex.sh` | Forge-X display, launcher and screen-script changes, and `uninstall_forgex` |
| `assets/config/platform/hooks-ad5m-*.sh` | Per-firmware platform hooks |
| `src/api/backlight_backend.cpp` | `BacklightBackendAllwinner` |
| `src/application/display_manager.cpp` | Startup brightness and the 20 s override |

## Related

- [`FLASHFORGE_AD5X_SUPPORT.md`](FLASHFORGE_AD5X_SUPPORT.md): the AD5X is a different SoC
  (MIPS) on ZMOD only; its "Differences from AD5M" section compares the two.
- [Forge-X](https://github.com/DrA1ex/ff5m), [ZMOD](https://github.com/ghzserg/zmod),
  [Klipper Mod](https://github.com/xblax/flashforge_ad5m_klipper_mod)
