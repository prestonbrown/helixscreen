# HelixScreen image module

Installs HelixScreen into a Raspberry Pi OS image while it is built, so the printer boots into
HelixScreen with nobody running an installer. It follows the CustomPiOS module layout (`config`
plus `start_chroot_script`, run inside the image's chroot), and the same script also runs as a
numbered CustoPiZer script, which is how MainsailOS builds its images today.

The script downloads the release for the image's architecture (`pi` for arm64, `pi32` for armhf),
installs the libraries HelixScreen needs with `apt-get`, and runs the release's own
`install.sh --target-root / --platform <p> --local <archive>`. That installs HelixScreen into the
Klipper user's home, enables its service, and adds `[update_manager helixscreen]` to the image's
`moonraker.conf`. It starts nothing; the steps that need the real printer run once on first boot.
It needs HelixScreen 1.2 or newer, and Klipper and Moonraker already installed in the image.

## CustomPiOS

Copy this directory to your distribution's `src/modules/helixscreen`, and list it after the modules
that install Klipper and Moonraker:

```bash
export MODULES="base(network,klipper,moonraker,mainsail,helixscreen)"
```

## MainsailOS (CustoPiZer)

In a fork of MainsailOS, copy `start_chroot_script` to `modules/generic/56-helixscreen`, so it runs
after `51-moonraker`. Settings go in `modules/generic/files/00-config` as `export` lines; the script
reads that file when the build provides it.

## Settings

All optional.

| Variable | Default | Meaning |
|----------|---------|---------|
| `HELIXSCREEN_VERSION` | `latest` | Release tag to install, such as `v1.2.0` |
| `HELIXSCREEN_PLATFORM` | from the image | `pi` (64-bit) or `pi32` (32-bit) |
| `HELIXSCREEN_ARCHIVE` | none | A release archive already inside the image; nothing is downloaded |
| `HELIXSCREEN_SHA256` | none | The archive's sha256, checked before installing |
| `HELIXSCREEN_USER` | `BASE_USER` | The user that runs Klipper; when neither is set, the installer finds it from `klipper.service` |
| `HELIXSCREEN_OWN_DISPLAY` | `0` | `1` turns an enabled KlipperScreen off so HelixScreen owns the screen. Otherwise KlipperScreen keeps it and HelixScreen is installed but not started |
| `HELIXSCREEN_INSTALL_ARGS` | none | More `install.sh` options, such as `--skip-kiauh-registration` |

A pinned `HELIXSCREEN_VERSION` with its `HELIXSCREEN_SHA256` makes the build reproducible.

On the booted printer, HelixScreen updates through Moonraker like any install, and
`install.sh --uninstall` removes it and turns KlipperScreen back on if the module turned it off.

The image-install options themselves are described in the user guide, `docs/user/INSTALL.md`,
under "For Image Builders".
