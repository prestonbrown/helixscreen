# HelixScreen Installation System

Developer guide for the HelixScreen installer infrastructure: modular shell scripts, platform detection, KIAUH integration, Moonraker updater, and the bats test suite.

**User-facing install instructions**: See the project README for quick-start installation commands.

---

## Architecture Overview

The installer is a **modular POSIX shell system** with 10 library modules that get bundled into monolithic scripts for end-user distribution. All shell code targets `/bin/sh` for maximum compatibility, including BusyBox on embedded platforms (AD5M, K1).

```
scripts/
  install-dev.sh              # Development installer (sources modules at runtime)
  install.sh                  # Bundled installer (auto-generated, committed)
  uninstall.sh                # Bundled uninstaller (auto-generated, committed)
  bundle-installer.sh         # Generates install.sh from modules
  bundle-uninstaller.sh       # Generates uninstall.sh from modules
  helix-launcher.sh           # Runtime launcher with watchdog supervision
  lib/installer/
    common.sh                 # Logging, colors, error handler, process killing
    platform.sh               # Platform/firmware detection, install paths, tmp dir
    permissions.sh             # Root/sudo checks
    requirements.sh            # Pre-flight: commands, deps, disk space, init system
    forgex.sh                  # ForgeX-specific: display config, screen.sh patching
    competing_uis.sh           # Stop GuppyScreen, KlipperScreen, Xorg, stock UI
    release.sh                 # Download from R2 CDN/GitHub, extract, validate arch
    service.sh                 # systemd/SysV service install, platform hooks
    moonraker.sh               # Moonraker update_manager configuration
    uninstall.sh               # Uninstall, clean, re-enable previous UIs
    kiauh.sh                   # KIAUH extension auto-detection and install
  kiauh/helixscreen/
    __init__.py                # KIAUH extension constants and install dir detection
    helixscreen_extension.py   # KIAUH BaseExtension implementation
    metadata.json              # KIAUH extension metadata (display name, description)
```

### Module Dependencies

Modules use source guards (`_HELIX_*_SOURCED`) to prevent double-sourcing. The load order in `install-dev.sh` matters -- `uninstall.sh` must be last because it uses functions from other modules.

### Bundled vs Development Installer

| | `install-dev.sh` | `install.sh` |
|---|---|---|
| **Usage** | Development, from repo checkout | End-user, via `curl \| sh` |
| **Modules** | Sources from `lib/installer/` at runtime | All modules inlined by `bundle-installer.sh` |
| **Guard** | Checks `_HELIX_BUNDLED_INSTALLER` is unset | Sets `_HELIX_BUNDLED_INSTALLER=1` |
| **Regeneration** | N/A | `./scripts/bundle-installer.sh -o scripts/install.sh` |

When modifying any module in `lib/installer/`, you must regenerate the bundled scripts:

```bash
./scripts/bundle-installer.sh -o scripts/install.sh
./scripts/bundle-uninstaller.sh -o scripts/uninstall.sh
```

The bundler uses `awk` to strip shebangs, SPDX headers, and source guards from each module, then concatenates them with the main orchestration code.

---

## Installation Methods

### 1. One-Line Install (curl)

The primary end-user method. Downloads and runs the bundled `install.sh`:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

**Options:**

| Flag | Description |
|------|-------------|
| `--update` | Update existing installation (preserves config) |
| `--uninstall` | Remove HelixScreen |
| `--clean` | Remove old installation completely, then fresh install |
| `--version VER` | Install specific version (e.g., `--version v1.1.0`) |
| `--local FILE` | Install from a local tarball (skip download) |

### 2. Local Tarball Install

For devices without HTTPS support (e.g., AD5M with BusyBox wget):

```bash
# On your computer: download the release
# On the device:
sh /data/install.sh --local /data/helixscreen-ad5m-v1.2.0.tar.gz
```

The installer auto-detects when HTTPS is unavailable and prints manual download instructions.

### 3. KIAUH Extension

See the [KIAUH Integration](#kiauh-integration) section below.

### 4. Development Install

From a repo checkout, the modular installer sources modules directly:

```bash
./scripts/install-dev.sh
./scripts/install-dev.sh --update
./scripts/install-dev.sh --uninstall
```

---

## Installation Flow

The `main()` function orchestrates this sequence:

1. **Platform detection** -- `detect_platform()` returns `ad5m`, `k1`, `pi`, `pi32`, or `unsupported`
2. **Firmware detection** -- AD5M: `klipper_mod` or `forge_x`; K1: `simple_af` or `stock_klipper`
3. **Path configuration** -- `set_install_paths()` sets `INSTALL_DIR`, `INIT_SCRIPT_DEST`, `PREVIOUS_UI_SCRIPT`, `TMP_DIR`
4. **Permission check** -- Root required on AD5M/K1; sudo on Pi
5. **Pre-flight checks** -- Required commands, runtime deps (libdrm2/libinput10 on Pi), disk space, init system detection
6. **Klipper ecosystem check** -- Verifies Klipper/Moonraker running (AD5M/K1 only, warns if missing)
7. **Platform configuration** -- ForgeX: display mode, screen.sh patching, logged wrapper
8. **Stop competing UIs** -- GuppyScreen, KlipperScreen, Xorg, stock FlashForge UI
9. **Download release** -- R2 CDN primary (`releases.helixscreen.org`), GitHub Releases fallback
10. **Extract with atomic swap** -- Validates ELF architecture, backs up config, `mv` old to `.old`, rollback on failure
11. **Platform hooks** -- Deploys `hooks-{platform}.sh` to `$INSTALL_DIR/platform/hooks.sh`
12. **Install service** -- systemd unit or SysV init script (templated with `@@HELIX_USER@@`, etc.)
13. **Moonraker integration** -- Adds `[update_manager helixscreen]` section, writes `release_info.json`
14. **KIAUH extension** -- Auto-installs if KIAUH detected. **Note:** The `kiauh.sh` module exists but is not currently integrated into the installer flow. KIAUH extension files are installed manually or via the KIAUH UI.
15. **Config symlink** -- `printer_data/config/helixscreen` symlink for Mainsail/Fluidd access
16. **Start service** -- Waits up to 5 seconds for startup confirmation
17. **Cleanup** -- Remove temp files, remove `.old` backup

### Error Handling

The installer uses `trap 'error_handler $LINENO' ERR` to catch failures. On error:
- Reports the failing line number and exit code
- Cleans up temp files
- Restores backed-up configuration if the install was partially complete
- Prints help resources

### Atomic Extraction with Rollback

The `extract_release()` function in `release.sh` implements a safe upgrade path:

1. Extract tarball to a temp directory
2. Validate the `helix-screen` binary exists and has correct ELF architecture
3. Move existing `$INSTALL_DIR` to `$INSTALL_DIR.old`
4. Move extracted content to `$INSTALL_DIR`
5. Restore user config from backup
6. If step 4 fails, automatically roll back from `.old`

### `NoNewPrivileges` and Self-Update on Pi (systemd)

When helix-screen performs a **self-update** (user presses "Check for Updates", the binary downloads a new tarball and spawns `install.sh`), the installer runs as a subprocess of the helix-screen systemd service.

The Pi systemd unit includes:

```ini
[Service]
NoNewPrivileges=true
```

This is a hardening flag that prevents any process in the service's cgroup — including child processes — from gaining new privileges. Concretely: **`sudo` is completely non-functional** inside `install.sh` when spawned by helix-screen.

**Affected operations and how they are handled:**

| Operation | Old behavior | Fixed behavior |
|-----------|-------------|----------------|
| `fix_install_ownership()` — chown files to klipper user | `sudo chown` → fatal exit | Warns and continues; not critical for self-update |
| Remove stale `$INSTALL_DIR.old` | `sudo rm -rf` | Try plain `rm -rf` first; fall back to timestamped name (`*.old.TIMESTAMP`) |
| Cleanup `.old*` dirs post-install | `sudo rm -rf` | Try plain `rm -rf` first; warn and skip if blocked |

**Root-owned `.old` directory** — a common scenario after a manual root-level install leaves behind a root-owned `helixscreen.old/`. The `pi` user cannot remove it even with `sudo` blocked by `NoNewPrivileges`. The installer detects this and creates a timestamped fallback (`helixscreen.old.1234567890`). A one-time manual cleanup is needed on the Pi:

```bash
# Diagnose: find files not owned by the current user
find ~/helixscreen* ! -user "$(id -un)" 2>/dev/null

# Fix: remove root-owned stale backup
sudo rm -rf ~/helixscreen.old
```

**Design principle:** Under `NoNewPrivileges`, the installer must complete the core swap (`mv old → .old`, `mv new → INSTALL_DIR`, restore config) without `sudo`. Anything that requires `sudo` must be either non-fatal or deferred to a manual step.

---

## Platform-Specific Installation

### Raspberry Pi (`pi`, `pi32`)

| Setting | Value |
|---------|-------|
| **Detection** | `/etc/os-release` contains Debian/Raspbian, or `/home/pi`, `/home/biqu`, `/home/mks` exists |
| **32/64-bit** | `getconf LONG_BIT` determines userspace bitness (64-bit kernel with 32-bit userspace is common) |
| **Install dir** | Auto-detected based on Klipper ecosystem: `~/helixscreen` if klipper/moonraker/printer_data found, else `/opt/helixscreen` |
| **Klipper user** | Detected via systemd service owner, process table, printer_data scan, or well-known users (biqu, pi, mks) |
| **Init system** | systemd (service template with `@@HELIX_USER@@` substitution) |
| **Runtime deps** | `libdrm2`, `libinput10` installed via apt |
| **Config symlink** | `~/printer_data/config/helixscreen` -> `$INSTALL_DIR/config` for web UI access |

### FlashForge Adventurer 5M -- Forge-X Firmware (`ad5m`, `forge_x`)

| Setting | Value |
|---------|-------|
| **Detection** | `armv7l` + kernel contains `ad5m` or `5.4.61` |
| **Firmware** | Forge-X detected by `/opt/config/mod/.root` directory |
| **Install dir** | `/opt/helixscreen` |
| **Init script** | `/etc/init.d/S90helixscreen` |
| **Previous UI** | `/opt/config/mod/.root/S80guppyscreen` |

**ForgeX-specific patches (all reversible on uninstall):**

- **Display mode**: Sets `variables.cfg` display to `GUPPY` mode (required for backlight)
- **GuppyScreen disable**: `chmod -x` on `/opt/config/mod/.root/S80guppyscreen`
- **tslib disable**: `chmod -x` on `/opt/config/mod/.root/S35tslib`
- **Stock UI disable**: Comments out `ffstartup-arm` in `/opt/auto_run.sh`
- **screen.sh backlight patch**: Blocks non-100 backlight changes when HelixScreen active (allows S99root init cycle)
- **screen.sh drawing patch**: Skips `draw_splash`, `draw_loading`, `boot_message` when HelixScreen active
- **logged wrapper**: Wraps `/opt/config/mod/.bin/exec/logged` to strip `--send-to-screen` flag (prevents direct framebuffer writes)

### FlashForge Adventurer 5M -- Klipper Mod (`ad5m`, `klipper_mod`)

| Setting | Value |
|---------|-------|
| **Firmware** | Detected by `/root/printer_software` or `/mnt/data/.klipper_mod` |
| **Install dir** | `/root/printer_software/helixscreen` |
| **Init script** | `/etc/init.d/S80helixscreen` |
| **Previous UI** | `/etc/init.d/S80klipperscreen` |
| **Xorg** | Stopped and disabled (`S40xorg`) since HelixScreen uses fbdev directly |

### Creality K1 Series -- Simple AF (`k1`, `simple_af`)

| Setting | Value |
|---------|-------|
| **Detection** | Buildroot OS + `/usr/data` + 2+ K1 indicators (pellcorp, printer_data, get_sn_mac.sh, etc.) |
| **Install dir** | `/usr/data/helixscreen` |
| **Init script** | `/etc/init.d/S99helixscreen` |
| **Previous UI** | `/etc/init.d/S99guppyscreen` |

### K2 / Other Platforms

K2 support exists in the `release_info.json` asset naming (`helixscreen-k2.zip`) but platform detection is not yet implemented in `detect_platform()`.

---

## KIAUH Integration

[KIAUH](https://github.com/dw-0/kiauh) (Klipper Installation And Update Helper) is the standard tool for managing Klipper ecosystem components.

### Extension Structure

The KIAUH extension lives in `scripts/kiauh/helixscreen/` and consists of three files:

**`metadata.json`** -- Extension metadata for KIAUH's menu system:
```json
{
  "metadata": {
    "index": 14,
    "module": "helixscreen_extension",
    "maintained_by": "prestonbrown",
    "display_name": "HelixScreen",
    "description": ["Modern touchscreen interface for Klipper..."],
    "repo": "https://github.com/prestonbrown/helixscreen",
    "updates": true
  }
}
```

**`__init__.py`** -- Constants and install directory detection:
- `HELIXSCREEN_INSTALLER_URL` -- URL to the bundled `install.sh`
- `find_install_dir()` -- Scans platform-dependent paths for existing installation

**`helixscreen_extension.py`** -- `BaseExtension` subclass with three operations:
- `install_extension()` -- Downloads and runs `install.sh`
- `update_extension()` -- Runs `install.sh --update`
- `remove_extension()` -- Runs `install.sh --uninstall`

### How the Extension Gets Installed

During installation, `install_kiauh_extension()` in `lib/installer/kiauh.sh`:

1. Calls `detect_kiauh_dir()` to find `~/kiauh/kiauh/extensions/` or `/home/*/kiauh/kiauh/extensions/`
2. If KIAUH is found and extension source files exist in the release package (`$INSTALL_DIR/scripts/kiauh/helixscreen/`)
3. Copies `__init__.py`, `helixscreen_extension.py`, and `metadata.json` to the KIAUH extensions directory
4. On new installs, prompts interactively (or uses `--kiauh yes/no`)
5. On updates, silently updates the extension files

### Updating the KIAUH Extension

When modifying the extension:

1. Edit files in `scripts/kiauh/helixscreen/`
2. The extension files are included in release tarballs and auto-updated during `--update`
3. Run the KIAUH extension bats tests to verify structural correctness

### Important: metadata.json Structure

The `metadata` top-level key is **required** (GitHub issue #3 was caused by this being missing). The bats tests validate this structure to prevent regressions.

---

## Moonraker Update Manager Integration

The installer configures Moonraker to enable one-click updates from Mainsail/Fluidd web UIs.

### What Gets Configured

1. **`[update_manager helixscreen]` section** appended to `moonraker.conf`:
   ```ini
   [update_manager helixscreen]
   type: web
   channel: stable
   repo: prestonbrown/helixscreen
   path: /home/biqu/helixscreen
   persistent_files:
       config/helixconfig.json
       config/helixscreen.env
       config/.disabled_services
   ```

2. **`release_info.json`** written to `$INSTALL_DIR/` -- Moonraker `type:web` needs this to detect the installed version

3. **`moonraker.asvc`** -- HelixScreen added to Moonraker's service allowlist so it can restart the service after updates

### Config Survival on Updates

Moonraker's `type: web` updater wipes the install directory (`shutil.rmtree`) before extracting each update. Config is preserved via three layers:

1. **`persistent_files`** in moonraker.conf -- Moonraker backs up listed files before rmtree and restores them after extraction
2. **Rolling backups** -- `Config::save()` maintains backups in `/var/lib/helixscreen/` (systemd `StateDirectory`) and `$HOME/.helixscreen/` (fallback). `Config::init()` auto-restores from these if the config file is missing after an update.
3. **SysV init script** -- On BusyBox systems (K1, AD5M) where systemd isn't available, the init script exports `HOME=/root` and creates `/var/lib/helixscreen/` so backup paths are persistent (not volatile `/tmp/`).

The installer also runs `ensure_persistent_files()` on every upgrade to add `persistent_files` to existing Moonraker configs that predate this feature.

### Migration

The installer detects old `type: git_repo` and `type: zip` configurations and auto-migrates them to `type: web`, cleaning up the sparse clone directory.

### moonraker.conf Discovery

The `find_moonraker_conf()` function searches in this order:
1. `$KLIPPER_HOME/printer_data/config/moonraker.conf` (detected user)
2. Static fallbacks: `/home/pi/...`, `/home/biqu/...`, `/home/mks/...`, `/root/...`, `/opt/config/...`, `/usr/data/...`

### Skipped Platforms

Moonraker update_manager is skipped on AD5M (typically no Mainsail/Fluidd web UI).

---

## Uninstaller

The uninstaller (`scripts/uninstall.sh`) reverses the installation:

1. **Stop service** -- systemd or SysV, plus kill remaining processes (watchdog first to prevent crash dialog)
2. **Remove service** -- Delete systemd unit or init script
3. **Re-enable disabled services** -- Reads `config/.disabled_services` state file and re-enables each recorded entry
4. **Remove installation** -- Checks all known paths: `/opt/helixscreen`, `/root/printer_software/helixscreen`, `/usr/data/helixscreen`
5. **Restore previous UI** -- Platform-specific:
   - Klipper Mod: Re-enable Xorg and KlipperScreen
   - K1: Re-enable GuppyScreen
   - ForgeX: Full cleanup via `uninstall_forgex()` (restore display mode, unpatch screen.sh, remove logged wrapper, re-enable GuppyScreen/tslib)
6. **Remove caches** -- Thumbnail caches, temp files, PID files, log files
7. **Remove Moonraker section** -- Strips `[update_manager helixscreen]` from moonraker.conf

### Disabled Services State File

The installer tracks what it disabled in `$INSTALL_DIR/config/.disabled_services`:
```
systemd:KlipperScreen
sysv-chmod:/etc/init.d/S80klipperscreen
sysv-chmod:/etc/init.d/S40xorg
```

The uninstaller reads this file and reverses each action (systemd enable, chmod +x). This is listed in `persistent_files` in the Moonraker config so it survives zip updates.

---

## Release Download System

### R2 CDN (Primary)

Downloads go through `releases.helixscreen.org` (Cloudflare R2 bucket):

1. Fetch `stable/manifest.json` for latest version and per-platform download URLs
2. Download the platform-specific tarball from R2

### GitHub Releases (Fallback)

If R2 is unavailable or returns a corrupt file:

1. Query `api.github.com/repos/.../releases/latest` for the tag name
2. Download from `github.com/.../releases/download/{version}/{filename}`

### HTTPS Capability Check

On embedded platforms (AD5M), BusyBox wget does not support HTTPS. The installer:
1. Tests curl HTTPS, then wget HTTPS
2. If neither works, prints step-by-step manual install instructions with `scp` commands

### Architecture Validation

After extraction, `validate_binary_architecture()` reads the ELF header (first 20 bytes) to verify:
- ELF magic bytes
- ELF class (32-bit vs 64-bit)
- Machine type (ARM vs AARCH64)

This prevents installing a Pi binary on AD5M or vice versa.

---

## Shell Test Infrastructure (bats)

The installer has **543 test cases** across **30 bats files** (~6700 lines of test code), making it one of the most thoroughly tested shell installer systems for 3D printer firmware.

### Running Tests

```bash
# Run all shell tests
bats tests/shell/

# Run a specific test file
bats tests/shell/test_platform_detection.bats

# Run with verbose output
bats --verbose-run tests/shell/test_platform_detection.bats
```

### Test Organization

| Test File | Coverage |
|-----------|----------|
| `test_platform_detection.bats` | Pi 32/64-bit detection, AD5M/K1 identification |
| `test_platform_hooks.bats` | Platform hook deployment |
| `test_pi_install_path.bats` | Pi install directory auto-detection cascade |
| `test_user_detection.bats` | Klipper user detection (systemd, process, printer_data, well-known) |
| `test_forgex_boot.bats` | ForgeX boot patches, screen.sh, logged wrapper |
| `test_arch_validation.bats` | ELF header parsing, architecture mismatch detection |
| `test_download_validation.bats` | Tarball validation, HTTPS capability |
| `test_r2_installer.bats` | R2 CDN manifest parsing, fallback to GitHub |
| `test_extract_release.bats` | Extraction, atomic swap, rollback |
| `test_release_packaging.bats` | Release tarball structure |
| `test_service_install.bats` | systemd/SysV service installation |
| `test_service_template.bats` | Service template placeholder substitution |
| `test_moonraker_config.bats` | update_manager section add/remove/migrate |
| `test_moonraker_paths.bats` | moonraker.conf discovery across platforms |
| `test_config_symlink.bats` | printer_data config symlink creation |
| `test_uninstall.bats` | Full uninstall flow, cache cleanup, UI restore |
| `test_disabled_services.bats` | Service disable/re-enable state tracking |
| `test_requirements.bats` | Command checking, disk space, init system detection |
| `test_detect_tmp_dir.bats` | Temp directory selection with space checking |
| `test_kiauh_extension.bats` | KIAUH metadata.json structure, Python syntax |
| `test_kiauh_installer.bats` | KIAUH extension install/update logic |
| `test_klipper_check.bats` | Klipper/Moonraker ecosystem pre-flight |
| `test_monolithic_installer.bats` | Bundled install.sh/uninstall.sh structural checks |
| `test_helix_launcher.bats` | Launcher script, env file sourcing, watchdog |
| `test_generate_manifest.bats` | Release manifest generation |
| `test_no_echo_ansi.bats` | No raw ANSI in echo (BusyBox compat) |
| `test_code_lint.bats` | Shell code quality checks |
| `test_symbol_extraction.bats` | Debug symbol extraction for crash reporting |
| `test_telemetry_pull.bats` | Telemetry data pull scripts |
| `test_resolve_backtrace.bats` | Backtrace symbol resolution |

### Test Helpers

`tests/shell/helpers.bash` provides shared utilities:

- **`mock_command`** -- Create a mock executable that outputs specific text
- **`mock_command_fail`** -- Create a mock that exits non-zero
- **`mock_command_script`** -- Create a mock with custom shell logic
- **`setup_mock_pi`** -- Create temp directory structure mimicking a Pi system
- **`create_fake_elf`** / **`create_fake_arm32_elf`** / **`create_fake_aarch64_elf`** -- Generate minimal ELF headers for architecture validation tests
- **`SUDO=""`** -- Exported no-op for tests that call `$SUDO`
- Logging stubs (`log_info`, `log_warn`, etc.) suppressed during tests

### Writing New Tests

Pattern for a new test file:

```bash
#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Reset globals
    unset _HELIX_MYMODULE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/mymodule.sh"
}

@test "my function does the right thing" {
    result=$(my_function "arg")
    [ "$result" = "expected" ]
}

@test "my function handles errors" {
    run my_function "bad_arg"
    [ "$status" -ne 0 ]
}
```

Key patterns:
- Use `WORKTREE_ROOT` (not hardcoded paths) so tests work in git worktrees
- `unset _HELIX_*_SOURCED` before sourcing modules to reset source guards
- Use `$BATS_TEST_TMPDIR` for temp files (auto-created, auto-cleaned)
- Mock system commands by prepending `$BATS_TEST_TMPDIR/bin` to `$PATH`

---

## Developer Guide: Adding Platform Support

### Step 1: Platform Detection

Add a new case in `detect_platform()` in `scripts/lib/installer/platform.sh`. Detection must be reliable and specific -- avoid false positives on other ARM devices.

```sh
# In detect_platform():
if [ "$arch" = "aarch64" ] && is_my_platform; then
    echo "myplatform"
    return
fi
```

### Step 2: Install Paths

Add a case in `set_install_paths()`:

```sh
elif [ "$platform" = "myplatform" ]; then
    INSTALL_DIR="/path/to/helixscreen"
    INIT_SCRIPT_DEST="/etc/init.d/S90helixscreen"
    PREVIOUS_UI_SCRIPT="/path/to/previous/ui"
```

### Step 3: Platform Hooks (Optional)

If the platform needs runtime hooks (pre-start/post-start behavior), create `config/platform/hooks-myplatform.sh` in the release package and add the mapping in `install_platform_hooks()` in the bundled installer.

### Step 4: Firmware-Specific Module (Optional)

For platforms with complex setup (like ForgeX), create a dedicated module `lib/installer/myplatform.sh`:
- Add source guard
- Implement install-time and uninstall-time functions
- Source it in `install-dev.sh` and add to the `bundle-installer.sh` module list

### Step 5: Tests

Create `tests/shell/test_myplatform.bats` covering:
- Platform detection (positive and negative cases)
- Install path configuration
- Any firmware-specific patching
- Uninstall/restore behavior

### Step 6: Regenerate Bundles

```bash
./scripts/bundle-installer.sh -o scripts/install.sh
./scripts/bundle-uninstaller.sh -o scripts/uninstall.sh
```

### Step 7: Release Asset

Add the platform to the CI/CD build matrix so release tarballs are generated. Update the `write_release_info()` case statement in `moonraker.sh` with the asset name.

---

## Troubleshooting

### "HTTPS Download Not Available"

**Cause**: BusyBox wget on AD5M/K1 doesn't support HTTPS.

**Fix**: Download the tarball on another computer and use `--local`:
```bash
scp -O helixscreen-ad5m-v1.2.0.tar.gz root@printer-ip:/data/
ssh root@printer-ip "sh /data/install.sh --local /data/helixscreen-ad5m-v1.2.0.tar.gz"
```

### "Architecture mismatch"

**Cause**: Wrong release tarball for the platform (e.g., Pi binary on AD5M).

**Fix**: Ensure you download the correct platform variant. The installer validates ELF headers before proceeding.

### "Insufficient disk space"

**Cause**: The target filesystem needs at least 50MB free, plus temp space for extraction (~3x tarball size).

**Fix**: Free space, or override the temp directory: `TMP_DIR=/path/with/space sh install.sh`

### "Failed to extract tarball: no space left on device"

**Cause**: Temp directory ran out of space during extraction.

**Fix**: The installer tries multiple temp locations (`/data/`, `/mnt/data/`, `/var/tmp/`, `/tmp/`), picking the first with 100MB+ free. Override with `TMP_DIR=` env var.

### ForgeX: Screen flickers or goes blank after install

**Cause**: ForgeX display mode not set correctly, or screen.sh patches didn't apply.

**Fix**: Check display mode: `grep display /opt/config/mod_data/variables.cfg` -- should be `GUPPY`. Verify patches: `grep helixscreen_active /opt/config/mod/.shell/screen.sh`.

### Moonraker update_manager not working

**Cause**: Missing `release_info.json`, wrong section type, or service not in `moonraker.asvc`.

**Fix**:
1. Check `release_info.json` exists in install dir
2. Verify section is `type: zip` (not `git_repo`)
3. Ensure `helixscreen` is in `printer_data/moonraker.asvc`
4. Restart Moonraker: `systemctl restart moonraker`

### Uninstall doesn't restore previous UI

**Cause**: The previous UI init script wasn't found or `config/.disabled_services` was deleted.

**Fix**: Manually re-enable the previous UI:
```bash
# ForgeX
chmod +x /opt/config/mod/.root/S80guppyscreen

# K1
chmod +x /etc/init.d/S99guppyscreen

# Klipper Mod
chmod +x /etc/init.d/S40xorg
chmod +x /etc/init.d/S80klipperscreen
```

### KIAUH extension not showing up

**Cause**: Extension files not copied to KIAUH's extensions directory.

**Fix**: Manually copy:
```bash
cp -r /opt/helixscreen/scripts/kiauh/helixscreen ~/kiauh/kiauh/extensions/
```

### "sudo: The 'no new privileges' flag is set, which prevents sudo from running as root"

**Cause**: The helix-screen systemd service has `NoNewPrivileges=true`. When a self-update is triggered from the UI, `install.sh` runs as a child of the service and inherits this restriction — `sudo` is fully non-functional.

**What this affects:**
- Chowning files to the klipper user (non-fatal, a warning is logged)
- Removing a root-owned stale `helixscreen.old` backup from a prior manual install

**Fix for root-owned stale backup:** Manually clean it up on the Pi before or after an update:
```bash
# Check what's root-owned
find ~/helixscreen* ! -user "$(id -un)" 2>/dev/null

# Remove it
sudo rm -rf ~/helixscreen.old
```

The installer handles this gracefully: if it cannot remove the stale `.old` directory, it renames the new backup to `helixscreen.old.TIMESTAMP` so the atomic swap can still proceed.
