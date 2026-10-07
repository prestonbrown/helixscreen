# HelixScreen Installation System

Developer guide for the HelixScreen installer infrastructure: modular shell scripts, platform detection, KIAUH integration, Moonraker updater, and the bats test suite.

**User-facing install instructions**: See the project README for quick-start installation commands.

---

## Architecture Overview

The installer is a **modular POSIX shell system** with 18 library modules (in `scripts/lib/installer/`) that get bundled into monolithic scripts for end-user distribution. All shell code targets `/bin/sh` for maximum compatibility, including BusyBox on embedded platforms (AD5M, K1).

```
scripts/
  install-dev.sh              # Development installer (sources modules at runtime)
  install.sh                  # Bundled installer (auto-generated, committed)
  uninstall.sh                # Bundled uninstaller (auto-generated, committed)
  bundle-installer.sh         # Generates install.sh from modules
  bundle-uninstaller.sh       # Generates uninstall.sh from modules
  helix-launcher.sh           # Runtime launcher with watchdog supervision
  lib/installer/              # 17 modules (subset shown)
    main.sh                   # Orchestrator: arg parsing, install flow, KIAUH call
    common.sh                 # Logging, colors, error handler, process killing
    host_profile.sh           # Mod-host probe + mod-owned path guard (see the payload-contract section)
    platform.sh               # Platform/firmware detection, install paths, tmp dir
    permissions.sh             # Root/sudo checks
    requirements.sh            # Pre-flight: commands, deps, disk space, init system
    forgex.sh                  # ForgeX-specific: display config, screen.sh patching
    competing_uis.sh           # Stop GuppyScreen, KlipperScreen, Xorg, stock UI
    release.sh                 # Download from R2 CDN/GitHub, extract, validate arch
    service.sh                 # systemd/SysV service install, platform hooks
    moonraker.sh               # Moonraker update_manager configuration
    klipper_include.sh         # Klipper config include management
    printer_seed.sh            # Seed default printer config
    audio.sh                   # Audio device setup
    camera.sh                  # Camera setup
    recovery.sh                # Recovery/rollback support
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
| **Regeneration** | N/A | `make installer` (writes `build/installer/`) |

The bundles are generated, never committed. `make installer` writes `build/installer/install.sh` and `build/installer/uninstall.sh`, rebuilding only when a module or bundler changed. Every consumer takes them from there: the `release-*` and `deploy-*` targets, `release.yml` (which builds once and publishes that file as both the GitHub release asset and the one-line installer at https://releases.helixscreen.org/install.sh), and `scripts/dev-release.sh`. The bats suite builds its own copies once per run in `tests/shell/setup_suite.bash` (`$INSTALL_BUNDLE`, `$UNINSTALL_BUNDLE`).

The bundler uses `awk` to strip shebangs, SPDX headers, and source guards from each module, then concatenates them with the main orchestration code.

---

## Installation Methods

### 1. One-Line Install (curl)

The primary end-user method. Downloads and runs the bundled `install.sh`:

```bash
curl -sSL https://releases.helixscreen.org/install.sh | sh
```

**Options:**

| Flag | Description |
|------|-------------|
| `--update` | Update existing installation (preserves config) |
| `--uninstall` | Remove HelixScreen |
| `--clean` | Remove old installation completely, then fresh install |
| `--version VER` | Install specific version (e.g., `--version v1.1.0`) |
| `--local FILE` | Install from a local archive (.zip or .tar.gz, skip download) |

### 2. Local Archive Install

For devices without HTTPS support (e.g., AD5M with BusyBox wget):

```bash
# On your computer: download the release
# On the device:
sh /data/install.sh --local /data/helixscreen-ad5m.zip
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

`main()` (`scripts/lib/installer/main.sh#main`) is split at the confirm point (`scripts/lib/installer/plan.sh#confirm_point`). Everything before it only reads; everything after it, `apply_install()`, changes the machine. `tests/shell/test_installer_confirm_point.bats` holds every call before the confirm point to a read-only allowlist, so a new detection step has to be allowlisted there (its header says what is covered).

### Before the confirm point (read-only)

1. **Host probe and arguments** -- `host_profile_probe`, `parse_installer_args`, `mod_payload_autodetect`
2. **Guards** -- refuse an uninstall run from the install dir; refuse to install over a firmware-managed HelixScreen
3. **Platform detection** -- `detect_platform()` returns one of `cc1`, `k2`, `ad5m`, `ad5x`, `k1`, `snapmaker-u1`, `m1`, `pi`, `pi32`, `x86`, or `unsupported` (plus zmod/guilouz sub-detectors); `unsupported` exits
4. **Firmware detection** -- AD5M/AD5X: `detect_mod_flavor`; K1: `detect_k1_firmware`
5. **Path configuration** -- `set_install_paths()` sets `INSTALL_DIR`, `INIT_SCRIPT_DEST`, `PREVIOUS_UI_SCRIPT`, `TMP_DIR`; `mod_payload_mode_block` settles payload mode
6. **Permission check** -- Root required on AD5M/K1; sudo on Pi. `--uninstall` branches off here and exits, so it never reaches the confirm point; `parse_installer_args` refuses `--uninstall --dry-run`
7. **Pre-flight checks** -- `detect_missing_unzip`, `check_requirements`, `detect_missing_runtime_deps`, `check_disk_space`, `detect_init_system`, `check_klipper_ecosystem`
8. **Version and release** -- `resolve_update_channel` (update or existing install), then `--local` filename, `--version` or `get_latest_version`; `probe_release` HEADs the archive so the plan only offers a release that exists
9. **Detection for the plan** -- `detect_competing_uis` (only stock UIs that are running or would start at boot; one an earlier install already disabled is not listed), `detect_moonraker_integration`, `detect_kiauh` (no Add when the extension is already installed)

### The confirm point

`confirm_point` prints the logo banner and the plan screen (Printer, Found, Install or `Update A -> B`, Remove, Libraries, Disable, Add, Disk, sudo). Then:

- `--dry-run` prints "Dry run, nothing changed." and exits 0. A check that would stop the install has already exited non-zero before this.
- A fresh install with a terminal asks `Continue? [Y/n]`, read from `/dev/tty` (`HELIX_TTY_DEVICE` overrides it). EOF means no. `--yes`, no terminal and `--update` do not ask.
- `--clean` always asks, `Continue? [y/N]`, since that answer is the consent to delete what the Remove line lists. With no terminal it refuses unless `--yes` was given, before anything changes.
- If sudo is needed it asks for the password once, so later steps do not stall on a prompt. The plan's sudo row lists only the reasons that apply, and is left out under NoNewPrivileges, where sudo cannot run.
- It sets `HELIX_CONFIRMED=1`, counts the steps (`plan_count_steps`), closes the "Checking system" step and opens the log.

### After the confirm point (`apply_install`)

One numbered step per phase; a step with nothing to do is skipped under the same conditions that left it out of the count.

1. **Installing libraries** -- apt for unzip and runtime deps (libdrm2/libinput10 on Pi). If apt fails the step reads FAILED and the install continues; `verify_binary_deps` stops it later if a library the binary needs is missing. Under NoNewPrivileges detection plans no libraries and warns which to install by hand
2. **Downloading** (or **Unpacking local archive**) -- R2 CDN primary (`releases.helixscreen.org`), GitHub Releases fallback. This runs before anything touches the running printer, so a failed download leaves it as it was.
3. **Stopping the stock screen** -- `configure_platform` (ForgeX: display mode, screen.sh patching, logged wrapper), then `stop_competing_uis` (GuppyScreen, KlipperScreen, Xorg, stock FlashForge UI)
4. **Installing files** -- `--clean` removal, `stop_service` on update, state migration, then `extract_release`: validates ELF architecture, backs up config, `mv` old to `.old`, re-checks free space when the swap crosses filesystems, rollback on failure
5. **Setting up service** -- systemd unit or SysV init script (templated with `@@HELIX_USER@@`, etc.), `hooks-{platform}.sh` to `$INSTALL_DIR/platform/hooks.sh`, udev/polkit rules, KIAUH extension (`install_kiauh_extension`, honoring `--skip-kiauh-registration`; see "How the Extension Gets Installed"), K1 extras, `verify_binary_deps`
6. **Connecting to Moonraker** -- `printer_data/config/helixscreen` symlink for Mainsail/Fluidd, `[update_manager helixscreen]` section, the release info file
7. **Starting HelixScreen** -- recovery script, install-time printer detection (see [Install-Time Printer Detection](#install-time-printer-detection)), then `start_service`, which waits up to 5 seconds for startup confirmation. On a payload install, where the mod starts the UI at boot, the same step is titled **Finishing setup**.

Then `INSTALL_COMPLETE=1`, the old-install cleanups, `finalize_install_log`, `cleanup_on_success` and `print_summary`.

### Install-Time Printer Detection

After the release is in place but **before** the service starts, the installer tries to
recognize the printer and pre-seed `settings.json` so the first launch lands on (or near)
the right configuration without the user driving the whole wizard. The logic lives in
`scripts/lib/installer/printer_seed.sh`, orchestrated from `main.sh`:

```sh
seed_pid=$(detect_printer_model)          # Tier-1
if [ -n "$seed_pid" ]; then
    seed_settings_for_printer "$seed_pid"          # device-level seed
    install_klipper_include_for_printer "$seed_pid"
else
    seed_from_moonraker_detection || true # Tier-2
fi
```

It runs in two tiers, Tier-1 first and Tier-2 only as a fallback:

**Tier-1 — model fingerprint (`detect_printer_model`).** A filesystem-based binary
fingerprint: it looks for a stock-firmware artifact at a known path that uniquely
identifies a model. Currently the only fingerprint shipped is the Sovol SV06 Ace, keyed on
the presence of the stock `mksclient` binary (e.g. `/home/sovol/printer_data/build/mksclient`).
On a match it seeds the printer's **device-level** blocks (display/input) directly and
installs any Klipper include for that printer. This tier is intentionally narrow — it only
fires for signals strong enough to avoid false positives — so most installs fall through to
Tier-2.

**Tier-2 — Moonraker detection (`seed_from_moonraker_detection`).** When Tier-1 finds
nothing, the installer shells out to the freshly-installed binary:

```sh
helix-screen --detect-printer --host 127.0.0.1 --port 7125
```

That one-shot queries the local Moonraker over REST and prints a JSON verdict (see
[`--detect-printer`](DEVELOPMENT.md#printer-detection---detect-printer) in DEVELOPMENT.md for
the exact shape). Tier-2 parses `preset`, `confidence`, and `runner_up_confidence` from that
verdict. It is a no-op (returns success without seeding) when Moonraker is unreachable, the
verdict carries no `preset`, or the JSON is malformed.

#### The B/C confidence gate

Tier-2 decides what to seed using two numeric thresholds (overridable via environment):

| Variable | Default | Meaning |
|----------|---------|---------|
| `HELIX_DETECT_MIN_CONFIDENCE` | `85` | Minimum top-match confidence to auto-apply a full preset |
| `HELIX_DETECT_MIN_MARGIN` | `10` | Minimum lead over the runner-up (`confidence - runner_up_confidence`) |

```sh
margin=$(( conf - rconf ))
if [ "$conf" -ge "$HELIX_DETECT_MIN_CONFIDENCE" ] && [ "$margin" -ge "$HELIX_DETECT_MIN_MARGIN" ]; then
    # Detection B -> full preset
    seed_full_preset_for_printer "$preset"
else
    # Detection C -> device-level seed + localhost host
    seed_settings_for_printer "$preset"
    _seed_moonraker_host_localhost
fi
```

Note these are **not** lettered confidence levels (there is no A/D). Confidence is a
continuous 0-100 score; "B" and "C" are simply the two seeding *paths* the gate selects:

- **Path B (confident: `confidence >= 85` AND `margin >= 10`).** Auto-apply the full preset
  via `seed_full_preset_for_printer`. This writes the preset's `display` block, merges its
  `printer` block (heaters, fans, LEDs, filament sensors) into `printers["default"]`, sets
  the top-level `"preset"` marker so the app knows a preset is already applied, and sets
  `printers["default"]["wizard_completed"] = false` so the wizard still runs once for the
  user to verify rather than silently trusting the seed.

- **Path C (ambiguous: `confidence < 85` OR `margin < 10`).** Seed only the safe
  **device-level** blocks (`input`, `display`) via `seed_settings_for_printer`, then
  pre-fill `printers["default"]["moonraker_host"] = "127.0.0.1"` so the app can reach
  Moonraker on first launch. Crucially it does **not** write the `"preset"` marker — the app
  re-runs its own detection at startup and asks the user to confirm, rather than committing
  to an uncertain guess.

- **Skip (no-op).** Moonraker unreachable, no `preset` in the verdict, or malformed JSON:
  nothing is seeded and the installer continues.

Seeded printer ids are recorded to `${INSTALL_DIR}/config/.seeded_settings` (idempotently)
so uninstall can be seed-aware.

Because Path B's seed can be wrong on an ambiguous-but-just-over-threshold match, it is
recoverable: re-running the app with `--wizard` (see
[DEVELOPMENT.md](DEVELOPMENT.md#wizard-flags)) clears the `"preset"` marker and host, turning
the next launch back into a full wizard.

### Error Handling

The installer uses `trap 'error_handler $LINENO' ERR` to catch failures. On error:
- Reports the failing line number and exit code
- Cleans up temp files
- Restores backed-up configuration if the install was partially complete
- Prints help resources

### Atomic Extraction with Rollback

The `extract_release()` function in `release.sh` implements a safe upgrade path:

1. Extract archive to a temp directory
2. Validate the `helix-screen` binary exists and has correct ELF architecture
3. Move existing `$INSTALL_DIR` to `$INSTALL_DIR.old`
4. Re-check free space when the staging and install directories sit on different filesystems (`_check_swap_space`)
5. Move extracted content to `$INSTALL_DIR`
6. Restore user config from backup
7. If step 5 fails, automatically roll back from `.old`

**Cross-filesystem free-space re-check.** Step 4 exists because a `mv` within one
filesystem is a rename and needs no space, but across filesystems it is a
copy-then-delete that needs the whole tree's worth — and the staging dir is
routinely on a different partition now (the K2 stages on `/mnt/UDISK` and installs
to `/opt`, a ~240MB overlay). The earlier sizing pass measures *before* the old
install is moved aside, and its tight path then relocates the old install
off-partition assuming the freed space suffices — false whenever the new tree is
materially larger than the old one. `_check_swap_space()` re-measures against
real free space after the move-aside, so an impossible copy fails having written
nothing instead of dying half-way through with ENOSPC. Refusal is not a partial
state: `_restore_install_backup()` puts the previous install back first, then the
installer exits. Pinned by `tests/shell/test_install_swap_space.bats`.

### Archive Ownership

An installed tree must never carry the build machine's uid/gid. Two independent
guarantees, because either one alone leaves a hole:

**Packaging** — every release tarball is created with `$(TAR_OWNER_FLAGS)`
(`mk/cross.mk`), which zeroes owner and group. A bare `tar -czvf` stamps each entry with
whatever uid built it: 1001 on the GitHub Actions runner, 1000 on a local build host. The
flag spelling is probed once, because GNU tar wants `--owner=0 --group=0` and bsdtar wants
`--uid 0 --gid 0`, neither accepts the other's, and releases are packaged on both. The
`.zip` artifacts need nothing — neither `unzip` nor Python's `zipfile` restores ownership.

**Extraction** — every `tar` extract passes **`-o`** ("don't restore user:group"). Root
extracts with `--same-owner` by default on both GNU and BusyBox tar, so without this an
install lands owned by a uid with no `/etc/passwd` entry on the printer. This is not
redundant with the packaging fix: archives published before it still carry the old ids.

> `-o` is the **only** portable spelling. BusyBox documents `-o` but has no
> `--no-same-owner` long option, so the long form fails extraction outright on every
> Creality box. GNU tar accepts `-o` as an alias for `--no-same-owner` when extracting.
> Verified on BusyBox 1.29.3 (AD5M), 1.31.1 (K1), 1.33.2 (K2), 1.36.1 (CC1, U1).

**Repair** - `fix_install_ownership()` normalises the whole tree on every platform, root-run
ones (ad5m/ad5x/k1/k2/cc1/u1) included. Repair is what heals an install whose archive arrived
carrying foreign ids: a measured K2 had 890 of 915 files owned by uid 1001.

The `deploy-*` targets extract with `-o` on the device for the same reason - a dev deploy is
a second way for the build host's uid to reach a printer.

### `NoNewPrivileges` and Self-Update on Pi (systemd)

When helix-screen performs a **self-update** (user presses "Check for Updates", the binary downloads a new archive and spawns `install.sh`), the installer runs as a subprocess of the helix-screen systemd service.

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

## Output and logging

The output layer is in `scripts/lib/installer/common.sh` (steps, logging), `plan.sh` (plan, summary) and `logo.sh` (banner).

- **Banner and plan.** `print_banner` draws the logo from `scripts/lib/installer/logo.sh`, a generated file (`scripts/render-installer-logo.sh` with `scripts/installer_logo_art.py`, needs Pillow). On a color terminal it first asks the terminal what it can draw (a kitty graphics query, `CSI 16 t` for the cell size, then DA1, which ends the read) and shows a kitty, iTerm2 or sixel image with the logotype beside it; anything else gets half-block (`▀ ▄`) text art, which every terminal font has. `print_plan` (`plan.sh`) lists the rows `confirm_point` set with `plan_set`.
- **Steps.** `step "running title" ["done title"]` opens a step; `step_done [detail]`, `step_fail` and `step_skip` close it. On a terminal the open step is a spinner that becomes a check mark or cross (ASCII marks when the locale is not UTF-8). Without a terminal each step prints one line, `[n/N] <title> ... ok (<detail>)`. A run ends with `print_summary`.
- **Terminal probe.** `ui_detect` looks at stderr, where every log line goes. `HELIX_INSTALL_TTY=0|1` forces no-terminal or terminal.
- **`log_info` vs `log_note`.** `log_info`/`log_success` go to the log file and reach the screen only with `--verbose` or `HELIX_INSTALL_VERBOSE=1`. `log_warn` and `log_error` always show. `log_note` is always shown and always logged; use it for the rare line a regular user must see.
- **`run_logged cmd...`.** Runs a command with its output captured to a temp file and appended to the log. Silent on success; on failure it prints the command, its exit status and the last `RUN_LOGGED_TAIL` (15) lines. With verbose on, the output streams as it arrives. Use it for apt, systemctl and helper scripts instead of letting them write to the terminal.
- **Failure report.** The EXIT trap (`installer_exit_report`) runs only for a run that passed the confirm point. It prints the failed step, a state line from `install_state_line` saying what the run actually changed (nothing, what was stopped, the new install in place but not set up, rolled back), and `Full log: <path>`.
- **Log location.** The log is written to the scratch dir while the run is going, then `finalize_install_log` moves it to `install_log_dest`: `$KLIPPER_HOME/printer_data/logs/helixscreen-install.log` when `printer_data` exists (Mainsail and Fluidd list it), otherwise `logs/helixscreen-install.log` under `install_state_root` (outside the install dir, which an update replaces whole). The previous run's log is kept as `.1`.
- **Environment.** `HELIX_INSTALL_VERBOSE`, `HELIX_INSTALL_TTY`, `HELIX_INSTALL_LOGO=kitty|iterm|sixel|text` (skips the terminal query and forces that logo), and `HELIX_TTY_DEVICE` (the file the `Continue?` prompt and the logo query use; tests point it at a nonexistent path so no prompt can block).

## Mod-Managed Hosts: the Payload Contract (Forge-X)

Some Adventurer hosts run a firmware **mod** - a git-managed tree that owns the machine's
userland, its own service mechanism, and its own OTA. Forge-X (`DrA1ex/ff5m`) is the mod this
contract supports, on both boards it selects between (`armv7l` AD5M, `mips` AD5X); Z-Mod hosts
keep the standalone flow. On a mod host the roles invert: **the mod owns the UI service, the
boot path, and the update cadence** - the installer's job is to place and refresh a payload,
not to run a machine.

### The probe and the guard

`scripts/lib/installer/host_profile.sh` probes once, before any path is chosen, and answers
capability questions (`HOST_MOD_ROOT`, `HOST_MOD_CHROOT`, `HOST_CHROOT_STATE`,
`HOST_SERVICE_MECHANISM`, `HOST_PLATFORM_HOOK_KEY`, ...). The mod's own
.shell/platform.sh presence is the marker - their source of truth, robust to their
refactors - and the chroot each board carries (one derivation off its `DATA_MNT`: 
`/usr/data/.mod/.forge-x` on AD5X, `/data/.mod/.forge-x` on AD5M) is what qualifies a host
for the payload contract. A tree without a chroot is a half-installed mod: recognized,
guarded, never auto-armed.

No other module tests vendor markers. `host_path_is_mod_owned` recognizes the mod namespaces
**canonically** - marker-independent - so a half-uninstalled mod is still protected, and the
fatal guard (`host_refuse_mod_owned`) sits in front of every destructive step: update
backups, `rm` loops, Moonraker NetDeploy arming, uninstall sweeps, scratch-dir selection.
One exemption exists (`HELIX_MOD_PAYLOAD`, set only by the payload contract itself) and its
blast radius is the payload root's contents - never a `mv` or wholesale `rm` of the root,
never anything else under the mod's tree.

### What a payload install is

On a verified mod host a bare install is the payload contract, no flags:

- The payload root is `<mod-tree-parent>/mod_data/helixscreen` - a **sibling of the mod's
  git tree** (`/opt/config/mod_data/helixscreen` on AD5M, `/usr/data/config/mod_data/`
  `helixscreen` on AD5X), never inside it: Forge-X's OTA is a `git_repo` update_manager and
  a Feather "reset" runs `git reset --hard` + `git clean -fd`, which deletes every untracked
  path in the tree. `mod_data` is the one directory per layout that exists on the host and
  is chroot-visible on both boards (the `/opt/config` bind) yet outside that reach. Because
  the root is outside the mod namespaces, `host_path_is_mod_owned` does not match it - the
  guard protects the *mod's* files, and this root is ours.
- Contents are replaced **in place** - the root's inode never changes; `config/` and
  `platform/` survive every update; an existing `helixscreen.env` is preserved byte-identical
  (the incoming one lands beside it as `.env.new`).
- **No service is installed on the host.** The service lands inside the mod chroot's
  `/etc/init.d`, which the mod's own bootstrap (`<chroot>/.root/start.sh`) starts at boot; nothing
  host-side iterates an `/etc/init.d`, and the installer never starts the UI itself.
- **Nothing is written to any mod-owned Moonraker conf.** The mod's `moonraker.conf` is
  git-tracked - dirtying it breaks their OTA. The sanctioned include point is
  `mod_data/user.moonraker.conf`, and even that only gains an `[update_manager helixscreen]`
  stanza with `--auto-update` (refused while the root is mod-owned: a `type: web` updater
  replaces the path wholesale, violating the preservation contract).
- The binary is **linked against the mod chroot's glibc** and runs inside it; host-side
  execution failing is the expected state, warned not errored.

Flags are overrides, named for what they do: `--standalone` (self-managed install beside the
mod, with a warning that the mod will not start it), `--payload-root PATH` (where), and
`--auto-update` (the stanza opt-in). The uninstall direction mirrors this: `install.sh
--uninstall` auto-arms; the standalone `uninstall.sh` run bare refuses removal and requires
its explicit `--mod-payload` - removal must never be the destructive default of a bare
uninstaller.

Two lifecycle invariants hold across every entry point (one shared resolver,
`flag > recorded root > probed default`, every tier passing the same
last-component-`helixscreen` name gate that refuses `--payload-root /usr/data` outright):
after any successful install that leaves a payload in place, the record in `mod_data` names
that payload's root; and only terminating removals consume the record - `--clean` continues
into a fresh install and must not orphan the record the fresh install relies on.

### The scope boundary - and why it is safe everywhere else

Every behavior above keys on probe answers that are only set when a mod tree or mod chroot
exists. On any other host - Pi, CC1, Snapmaker U1, K1/K2, plain Linux - the probe answers
"no mod": guards are inert (nothing mod-owned exists), the contract cannot arm, services and
paths resolve exactly as before. The **architecture** layers are vendor-neutral on purpose -
one flavor detector (`forge_x | zmod | stock`), one C++ platform predicate
(`platform_info.h`'s `ad5x_mod_layout_present`, which also fixes Forge-X rigs'
self-update classification), zero vendor literals in generic code - so the next mod flavor
is one module touch, not a sweep. The **feature** activates only on the verified shape:
both Forge-X boards today; Z-Mod and everything else keep their existing flows.

### Legacy AD5M installs

AD5M Forge-X hosts that installed HelixScreen before this contract run a standalone root
(`/opt/helixscreen`) with an `S90helixscreen` service. The payload install detects that root
and offers **adopt-or-warn**: adopting makes the legacy root the payload root (outside the
mod's tree - also the OTA-durable answer - recorded like any payload root), and **keeps the
S90 service, which is then the payload's only boot path** (the mod's bootstrap only starts
its own tree; the payload contract installs no HOST service, and the adopted root keeps
its S90 as the only boot path; the S90 bakes `DAEMON_DIR` at
install time so it boots the adopted root's refreshed `bin/`). Declining proceeds at the
mod's default with the exact manual migration commands printed. Neither branch deletes or
de-execs anything; the operator executes those steps.

### Vendored-script edits are contained

The display takeover edits the mod's screen.sh (backlight and drawing guards). Every
rewrite is validated (`bash -n`) against a candidate before replacing the original, with
restore-on-failure, and the uninstall restore runs exactly once per process - a consumed
record never falls back to rewriting a display mode the rig did not have.

## Platform-Specific Installation

### Raspberry Pi (`pi`, `pi32`)

| Setting | Value |
|---------|-------|
| **Detection** | `/etc/os-release` contains Debian/Raspbian, or `/home/pi`, `/home/biqu`, `/home/mks` exists |
| **32/64-bit** | `getconf LONG_BIT` determines userspace bitness (64-bit kernel with 32-bit userspace is common) |
| **Install dir** | Auto-detected — see the cascade below. `~/helixscreen` in every case with a non-root service user; `/opt/helixscreen` only for root installs |
| **Klipper user** | Detected via systemd service owner, process table, printer_data scan, or well-known users (biqu, pi, mks) |
| **Init system** | systemd (service template with `@@HELIX_USER@@` substitution) |
| **Runtime deps** | `libdrm2`, `libinput10` installed via apt |
| **Config symlink** | Per-file: user-owned config lives in `~/printer_data/config/helixscreen/` and each `$INSTALL_DIR/config/<file>` is a symlink into it (`setup_config_symlink`). Per-file, not a directory symlink, because the install's `config/` also holds packaged files an update replaces - only `HELIX_USER_CONFIG_FILES` (settings.json, helixscreen.env, .disabled_services, tool_spools.json, crash_history.json) and `HELIX_USER_CONFIG_DIRS` (custom_images, themes, printer_database.d) outlive an update |

#### Pi install-directory cascade

`detect_pi_install_dir()` in `scripts/lib/installer/platform.sh`, first match wins:

| # | Condition | Result |
|---|-----------|--------|
| 1 | `INSTALL_DIR` set by the user | that path, after `validate_install_dir` |
| 2 | An install already on disk (`<dir>/bin/helix-screen` exists) | that path |
| 3 | `~/klipper` or `~/moonraker` exists | `$KLIPPER_HOME/helixscreen` |
| 4 | `~/printer_data` exists | `$KLIPPER_HOME/helixscreen` |
| 5 | `moonraker.service` is active | `$KLIPPER_HOME/helixscreen` |
| 6 | Non-root service user whose home they own | `$KLIPPER_HOME/helixscreen` |
| 7 | Otherwise (root installs) | `/opt/helixscreen` |

Rule 2 exists because the cascade's answer is not stable across releases: a box that
matched one branch on first install can match a different one later, and moving the
install would orphan the old tree and the config inside it. An install on disk always
wins over a fresh decision.

Rule 6 exists because of how an update applies. `install.sh` prefers renaming the
install root (`mv <root> <root>.old; mv <new> <root>`), and rename mutates the
**parent's** directory entries — so it is the parent that has to be writable by the
service user. `/opt` is root-owned and the service runs unprivileged, which leaves only
the in-place fallback: delete the root's contents, then move the new ones in. That path
works and is kept, but it deletes before it moves, so an interruption leaves a partial
tree (#970). Escalation does not rescue it either: `helixscreen.service` sets
`NoNewPrivileges=true`, so `sudo` fails from the app and from the `install.sh` it forks.

Rule 6's shape is the standalone display: a Pi driving a panel with Klipper and
Moonraker on another host, so rules 3-5 all miss. Before rule 6 existed those boxes
landed on `/opt/helixscreen` and were the only layout depending on the in-place path.
See `UPDATE_SYSTEM.md` for how `self_update_supported()` reads the resulting tree.

### FlashForge Adventurer 5M -- Forge-X Firmware (`ad5m`, `forge_x`)

The `--standalone` layout on this firmware (the payload contract above is what a bare
install uses):

| Setting | Value |
|---------|-------|
| **Detection** | `armv7l` + kernel contains `ad5m` or `5.4.61` |
| **Firmware** | Forge-X detected by `/opt/config/mod/.root` directory |
| **Install dir** | `/opt/helixscreen` (`--standalone` only) |
| **Init script** | `/etc/init.d/S90helixscreen` (host-side, `--standalone` only) |
| **Previous UI** | `/opt/config/mod/.root/S80guppyscreen` |

**ForgeX-specific patches (all reversible on uninstall):**

- **Display mode**: switches `variables.cfg` display to `HEADLESS` -- the slot Forge-X asks custom screens to occupy, because any other slot risks failed OTA updates and repeated Moonraker recovery prompts. The takeover probes `STOCK`/`FEATHER`/`GUPPY` (and `HEADLESS` as an arrival state for upgrades) and records the pre-install mode to `mod_data/helixscreen_prev_display`. Under `HEADLESS` the mod's start.sh starts neither tslib nor GuppyScreen; the installer still de-execs `S80guppyscreen`, the `guppyscreen` launcher (reachable via zdisplay.sh in any mode) and `S35tslib` as belt-and-braces. Backlight is not a reason to pick a mode: the mod's screen.sh path is mode-independent.
- **Stock UI disable**: Comments out `ffstartup-arm` in /opt/auto_run.sh
- **screen.sh backlight patch**: Blocks non-100 backlight changes when HelixScreen active (allows S99root init cycle)
- **screen.sh drawing patch**: Guards whichever of `draw_splash`, `draw_loading`, `boot_message`, `splash_start` the installed Forge-X version actually has, each label verified individually, so the mod's splash cannot draw over us
- **logged wrapper**: Wraps `/opt/config/mod/.bin/exec/logged` to strip `--send-to-screen` flag (prevents direct framebuffer writes)

### FlashForge Adventurer -- Forge-X Payload Mode (`ad5m`/`ad5x`, `forge_x`)

What a bare install actually does on a verified Forge-X host (either board - the probe keys
on the mod's shape, `mips` selects the AD5X hook, everything else the AD5M's):

| Setting | Value |
|---------|-------|
| **Detection** | mod tree + Buildroot chroot probed by `host_profile.sh` (.shell/platform.sh marker) |
| **Install dir** | `<mod-tree-parent>/mod_data/helixscreen` - `/opt/config/mod_data/helixscreen` (AD5M), `/usr/data/config/mod_data/helixscreen` (AD5X). A **sibling** of the mod's git tree: the OTA's `git clean -fd` and a Feather reset cannot reach it, and the `/opt/config` bind makes it chroot-visible on both boards |
| **Config** | `mod_data/helixscreen/config` - interior to the payload root, preserved by every in-place update |
| **Init script** | inside the mod chroot: `<chroot>/etc/init.d/S90helixscreen` (AD5M) / `S80helixscreen` (AD5X), started by the mod's `<chroot>/.root/start.sh` |
| **Cache / logs** | AD5M: `/data/.helixscreen/{cache,logs}`. AD5X: `/opt/config/mod_data/helixscreen-state/cache` and `/opt/config/mod_data/log/helix.log` - never inside the payload root, which every update replaces |
| **Payload root record** | `mod_data/helixscreen_payload_root` (the uninstaller's `flag > record > default` resolver) |
| **Uninstall** | `install.sh --uninstall` auto-arms; the standalone `uninstall.sh` needs `--mod-payload` (optionally `--payload-root`) |

Runtime caches never live inside the payload root: an update replaces everything in it
except `config/` and `platform/`, so a cache there would be wiped on every refresh.

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

K2 is fully detected by `detect_platform()` (which echoes `k2`), alongside `cc1`, `ad5m`, `ad5x`, `k1`, `snapmaker-u1`, `m1`, `pi`, `pi32`, and `x86`.

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
4. Installs by default when KIAUH is detected; `--skip-kiauh-registration` opts out
5. On updates, silently updates the extension files

### Updating the KIAUH Extension

When modifying the extension:

1. Edit files in `scripts/kiauh/helixscreen/`
2. The extension files are included in release archives and auto-updated during `--update`
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
       config/settings.json
       config/helixscreen.env
       config/.disabled_services
   ```

2. **release_info.json** written to `$INSTALL_DIR/` -- Moonraker `type:web` needs this to detect the installed version

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

The uninstaller (`uninstall.sh`, bundled from `lib/installer/uninstall.sh` and its dependencies) reverses the installation:

1. **Stop service** -- systemd or SysV, plus kill remaining processes (watchdog first to prevent crash dialog)
2. **Remove service** -- Delete systemd unit or init script
3. **Re-enable disabled services** -- Reads `config/.disabled_services` state file and re-enables each recorded entry
4. **Remove installation** -- Checks every known install path (`HELIX_INSTALL_DIRS` in `scripts/lib/installer/common.sh`: `/root/printer_software/helixscreen`, `/opt/helixscreen`, `/mnt/UDISK/helixscreen`, `/usr/data/helixscreen`, `/srv/helixscreen`, `/user-resource/helixscreen`, `/userdata/helixscreen`)
5. **Restore previous UI** -- Platform-specific:
   - Klipper Mod: Re-enable Xorg and KlipperScreen
   - K1: Re-enable GuppyScreen
   - ForgeX: Full cleanup via `uninstall_forgex()` (restore display mode, unpatch screen.sh, remove logged wrapper, re-enable GuppyScreen/tslib)
6. **Remove caches** -- `cache/` and `logs/` under every declared state root (`HELIX_STATE_DIRS` in `scripts/lib/installer/common.sh`, mirroring `kStateRoots` in `include/helix_install_roots.h`), plus the legacy in-payload locations, temp files, PID files
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

1. Fetch stable/manifest.json for latest version and per-platform download URLs
2. Download the platform-specific archive from R2

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

The installer has on the order of **2000 test cases** across **100+ bats files**, making it one of the most thoroughly tested shell installer systems for 3D printer firmware.

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
| `test_download_validation.bats` | Archive validation, HTTPS capability |
| `test_r2_installer.bats` | R2 CDN manifest parsing, fallback to GitHub |
| `test_extract_release.bats` | Extraction, atomic swap, rollback |
| `test_release_packaging_make.bats` / `test_release_packaging_artifacts.bats` | Release archive structure |
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
| `test_helix_launcher_e2e.bats` / `_env` / `_nice` / `_respawn` | Launcher script, env file sourcing, watchdog |
| `test_generate_manifest.bats` | Release manifest generation |
| `test_no_echo_ansi.bats` | No raw ANSI in echo (BusyBox compat) |
| `test_code_lint.bats` | Shell code quality checks |
| `test_symbol_ci.bats` / `test_symbol_makefile.bats` | Debug symbol extraction for crash reporting |
| `test_telemetry_pull_args.bats` / `test_telemetry_pull_download.bats` | Telemetry data pull scripts |
| `test_resolve_backtrace.bats` | Backtrace symbol resolution |

_The table above is a representative subset; the suite has well over 100 bats files in `tests/shell/`._

### Test Helpers

`tests/shell/helpers.bash` provides shared utilities:

- **`mock_command`** -- Create a mock executable that outputs specific text
- **`mock_command_fail`** -- Create a mock that exits non-zero
- **`mock_command_script`** -- Create a mock with custom shell logic
- **`setup_mock_pi`** -- Create temp directory structure mimicking a Pi system
- **`create_fake_elf`** / **`create_fake_arm32_elf`** / **`create_fake_aarch64_elf`** -- Generate minimal ELF headers for architecture validation tests
- **`SUDO=""`** -- Exported no-op for tests that call `$SUDO`
- **`HELIX_INSTALL_VERBOSE=1`** -- Exported so `log_info` and command output reach the screen, where `run`/`assert_output` can see them
- **`HELIX_TTY_DEVICE`** -- Set to a nonexistent path so the `Continue?` prompt never reads a real terminal
- Logging stubs (`log_info`, `log_warn`, etc.) suppressed during tests
- End-to-end golden transcripts of whole installer runs live in `tests/shell/fixtures/install_transcripts/`

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

If the platform needs runtime hooks (pre-start/post-start behavior), create config/platform/hooks-myplatform.sh in the release package and add the mapping in `install_platform_hooks()` in the bundled installer.

### Step 4: Firmware-Specific Module (Optional)

For platforms with complex setup (like ForgeX), create a dedicated module lib/installer/myplatform.sh:
- Add source guard
- Implement install-time and uninstall-time functions
- Source it in `install-dev.sh` and add to the `bundle-installer.sh` module list

### Step 5: Tests

Create tests/shell/test_myplatform.bats covering:
- Platform detection (positive and negative cases)
- Install path configuration
- Any firmware-specific patching
- Uninstall/restore behavior

### Step 6: Check the Bundles

```bash
make installer    # build/installer/install.sh + uninstall.sh
```

### Step 7: Release Asset

Add the platform to the CI/CD build matrix so release archives are generated. Update the `write_release_info()` case statement in `moonraker.sh` with the asset name.

---

## Troubleshooting

### "HTTPS Download Not Available"

**Cause**: BusyBox wget on AD5M/K1 doesn't support HTTPS.

**Fix**: Download the archive on another computer and use `--local`:
```bash
scp -O helixscreen-ad5m.zip root@printer-ip:/data/
ssh root@printer-ip "sh /data/install.sh --local /data/helixscreen-ad5m.zip"
```

### "Architecture mismatch"

**Cause**: Wrong release archive for the platform (e.g., Pi binary on AD5M).

**Fix**: Ensure you download the correct platform variant. The installer validates ELF headers before proceeding.

### "Insufficient disk space"

**Cause**: The target filesystem needs at least 50MB free, plus temp space for extraction (~3x archive size).

**Fix**: Free space, or override the temp directory: `TMP_DIR=/path/with/space sh install.sh`

### "Failed to extract archive: no space left on device"

**Cause**: Temp directory ran out of space during extraction.

**Fix**: The installer tries multiple temp locations, picking the first with 100MB+ free.
The order is: a platform-declared staging root (`TMP_DIR_PREFERRED`, set by
`set_install_paths` -- the K2 declares `/mnt/UDISK/helixscreen-install`, since both `/opt`
and `/usr/data` are on its ~240MB root overlay), then a sibling of `INSTALL_DIR`, then
`$HOME`, then `/user-resource/`, `/data/`, `/mnt/data/`, `/usr/data/`, `/var/tmp/`, `/tmp/`.
Override with the `TMP_DIR=` env var.

Whatever wins is `rm -rf`'d when the installer exits, so every candidate -- including a
platform-declared one -- goes through the same name guard: the final path component must
contain `helixscreen-install`. The scratch dir is removed on **any** exit (EXIT/INT/TERM),
not just the success path; the older `ERR`-only trap was a bash extension that silently did
nothing under the ash/dash shells the embedded platforms run, which leaked the full download.

That trap only ever cleans the **current** run's scratch dir. Directories leaked by
*previous* installs are reclaimed separately: `cleanup_stale_cache_dirs()`
(`scripts/lib/installer/release.sh`) removes the paths a platform declares in
`STALE_CACHE_DIRS` — on the K2, the old `/usr/data/helixscreen/cache` thumbnail/gcode
location (the app now caches on `/mnt/UDISK`); on the AD5M, `/data/helixscreen/cache`,
whatever `migrate_state_root()` leaves there after carrying the state root to its
dot-prefixed `/data/.helixscreen` (a plainly named directory at `/data` top level shows in
Moonraker's print-file picker, because the vendor symlinks the whole partition in as the
gcodes root); plus installer scratch dirs like
`/usr/data/helixscreen-install` and `/opt/.helixscreen-install` left behind by pre-EXIT-trap
installers (one unit held a 60MB archive for two months). It runs after the service starts,
never touches the scratch dir this run is staging into, and applies the same shape of name
guard: a declared path is only removed when its final component is exactly `cache` or names
an installer scratch dir — never a top-level directory (the K2 `/mnt/UDISK` wipe incident).

### ForgeX: Screen flickers or goes blank after install

**Cause**: ForgeX display mode not set correctly, or screen.sh patches didn't apply.

**Fix**: Check display mode: `grep display /opt/config/mod_data/variables.cfg` -- should be `GUPPY`. Verify patches: `grep helixscreen_active /opt/config/mod/.shell/screen.sh`.

### Moonraker update_manager not working

**Cause**: Missing release_info.json, wrong section type, or service not in `moonraker.asvc`.

**Fix**:
1. Check release_info.json exists in install dir
2. Verify section is `type: web` (the updater migrates away from `git_repo`/`zip`)
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
