# Elegoo Centauri Carbon: Yocto Build (OpenCentauri COSMOS)

The Centauri Carbon 1 (Allwinner R528, Cortex-A7 hard-float NEON VFPv4, 480x272 fbdev, 112 MB
RAM) has two build targets:

| Target | What it is |
|--------|------------|
| `cc1` | The release build: fully static, built in Docker (`make cc1-docker`, `make package-cc1`), installed onto COSMOS by the regular installer into `/user-resource/helixscreen/`. Deploy with `make deploy-cc1 CC1_HOST=<ip>` (`-fg`, `-bin` variants as for other printers). User steps: [`../../user/guide/install-cc1.md`](../../user/guide/install-cc1.md) |
| `yocto` | Built by bitbake as a recipe inside [OpenCentauri/cosmos](https://github.com/OpenCentauri/cosmos) (`meta-opencentauri/recipes-apps/helixscreen/`). Bitbake supplies `CC`/`CXX`/`CFLAGS`/`LDFLAGS` and every library through the recipe's `DEPENDS`, so `YOCTO_BUILD := yes` skips all in-tree submodule builds |

The rest of this page is the local dev loop for the `yocto` target, which lets you iterate on
the recipe and the Makefile without pushing commits to GitHub each time. The sunxi BSP lives in
cosmos's `meta-sunxi` + `meta-opencentauri`.

## One-time setup

Requires Docker, git, and ~40GB free disk.

```bash
# 1. Clone cosmos (~1.5GB including submodules)
git clone --recurse-submodules --jobs=8 \
    https://github.com/OpenCentauri/cosmos.git ~/Code/Printing/yocto-cosmos

# 2. Pull the official poky dev container
docker pull crops/poky:ubuntu-22.04

# 3. Drop a dev-only auto.conf into cosmos's build/conf/ (see below)
cat > ~/Code/Printing/yocto-cosmos/build/conf/auto.conf <<'EOF'
# Public Yocto sstate mirror: pulls prebuilt gcc/glibc/pkgconfig-native/etc.
# for the scarthgap release, saving hours on the first build.
SSTATE_MIRRORS ?= "file://.* http://sstate.yoctoproject.org/all/PATH;downloadfilename=PATH"

# Build helixscreen from our live worktree mounted at /workdir/helixscreen
# (no push-to-github per iteration).
INHERIT += "externalsrc"
EXTERNALSRC:pn-helixscreen = "/workdir/helixscreen"
# B = S so the Makefile actually runs. Our Makefile builds into ./build/
# which is gitignored.
EXTERNALSRC_BUILD:pn-helixscreen = "/workdir/helixscreen"

BB_NUMBER_THREADS = "8"
PARALLEL_MAKE = "-j 8"
EOF
```

The `auto.conf` stays *outside* this repo: it is a user-local dev override
for the cosmos tree.

## Iteration loop

From anywhere inside the helixscreen repo (including a worktree):

```bash
./scripts/yocto-docker.sh                           # full build
./scripts/yocto-docker.sh helixscreen -c compile -f # force-recompile only (fastest)
./scripts/yocto-docker.sh -e helixscreen | less     # dump recipe env
./scripts/yocto-docker.sh bash                      # interactive poky shell
```

The script mounts whichever tree it was invoked from (via `$HELIX_SRC`, auto-
detected) at `/workdir/helixscreen`. Edit Makefile / code locally → rerun →
see errors in seconds-to-minutes.

First build will also pull sstate from yoctoproject.org's public mirror for
gcc-cross, glibc, pkgconfig-native, etc. Expect 10-30 minutes. Subsequent
incremental builds take seconds.

## Recipe location

```
~/Code/Printing/yocto-cosmos/meta-opencentauri/recipes-apps/helixscreen/helixscreen_0.1.bb
```

When iterating on recipe changes, edit that file directly. When it's right,
we open a PR against `OpenCentauri/cosmos` with the updated recipe.

## Moonraker on COSMOS

COSMOS has no systemd and no `sudo`: it boots with SysV init (`/etc/rc6.d/K04moonraker`
stops Moonraker through `start-stop-daemon` on reboot), `/sbin/reboot` is sysvinit's `halt`,
and HelixScreen runs as root. Three Moonraker setups exist on these printers, and they differ
in what HelixScreen can rely on:

| Setup | `machine.reboot` / `machine.shutdown` | Python components (HelixPrint plugin, timelapse) |
|---|---|---|
| COSMOS stable (26.09.0 and earlier): Python Moonraker, `[machine] provider: none` | Fails: shells out to `sudo systemctl reboot`, no sudo | Load |
| COSMOS main (since 2026-10-06, not in a release yet): Python Moonraker with a `sysvinit` machine provider | Works | Load |
| [Moonruster](https://github.com/shawn-makes-stuff/moonruster): third-party Rust port of Moonraker, installed by the user over SSH, not shipped by COSMOS | Fails the same way under `provider: none` (logs `sudo reboot`) | **Do not load.** Its components are a fixed compiled list; any other `moonraker.conf` section logs `ModuleNotFoundError: No module named 'moonraker.components.<name>'` and becomes a startup warning |

What this means for HelixScreen on a CC1:

- **Power actions do not use Moonraker.** When Moonraker is on this host and the host does
  not run systemd (`helix::host_runs_systemd()`), the shutdown dialog calls `SystemPower`
  directly (`src/ui/panel_widgets/shutdown_widget.cpp#plan_shutdown_dialog`), so all three
  setups reboot through `/sbin/reboot` with no Moonraker error (#1869). Mainsail and Fluidd
  reboot buttons still go through `machine.reboot` and fail on stable COSMOS and Moonruster.
- **The framebuffer is cleared on the way down.** Once a local reboot or shutdown is
  dispatched, LVGL stops rendering and fb0 is written to opaque black, and the SIGTERM fast
  exit (`include/fbdev_signal_clear.h`) writes it again, so the last frame does not show
  through the next boot. Every SIGTERM stop blanks the panel this way, including the
  gui-switcher hand-off to another UI and COSMOS's GUI stop before resonance calibration.
- **If local power fails while Moonraker is connected**, HelixScreen retries through
  `machine.reboot` / `machine.shutdown` before showing an error, for setups where the systemd
  check reads wrong (a container without `/run/systemd`, a localhost tunnel).
- **Under Moonruster the HelixPrint plugin is absent.** HelixScreen detects no plugin and runs
  its no-plugin paths. On the printer it reads the process table and skips the install prompt
  (`diag::moonraker_loads_python_addons`); a remote screen cannot tell, because Moonruster reports
  itself as Moonraker, so it still offers. The plugin installer's `[helix_print]` section only adds a Moonruster
  startup warning. Moonruster ports Moonraker's extension (agent) API, so an out-of-process
  plugin could work there, but the plugin is a Python component today and that would be a
  redesign. Moonruster measured about 10 MB more `MemAvailable` on a 115 MB CC1, so expect COSMOS users to try it.
- **Plain API clients do not care which server answers.** Moonruster reports itself as
  Moonraker `v0.11.0-3-g9e676eb`, API 1.5.0, and HelixScreen talks to it like any Moonraker.
  Its `Server` header reads `Moonruster/<version>` if a bug report needs telling them apart.

## Notes

- `scripts/yocto-docker.sh` reads the cosmos checkout from `$YOCTO_COSMOS` (default
  `~/Code/Printing/yocto-cosmos`) and the helixscreen tree from `$HELIX_SRC` (default: the
  script's own repo). From a worktree it also mounts the main tree so `lib/` symlinks resolve.
- `-Wno-psabi` is on for every build (top of `Makefile`), so ARM psABI-transition notes do not
  trip `-Werror`.
