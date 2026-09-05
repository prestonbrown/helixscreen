# Update System (Developer Guide)

How the HelixScreen update system works internally: architecture, update channels, CDN distribution, download/install flow, Moonraker integration, and troubleshooting.

---

## Architecture Overview

The update system has three main components:

```
UpdateChecker (singleton, async update checker)
  |  Checks for new versions, manages download/install lifecycle.
  |  All network I/O runs on background threads; results dispatched to LVGL thread.
  |
  +-> R2 CDN (primary source)
  |     Fetches manifest.json from releases.helixscreen.org/{channel}/
  |     Platform-specific archives (.zip preferred, legacy .tar.gz fallback) + SHA-256 checksums.
  |
  +-> GitHub API (fallback)
  |     /repos/prestonbrown/helixscreen/releases/latest (stable)
  |     /repos/prestonbrown/helixscreen/releases (beta — full array)
  |
  +-> Moonraker update_manager (parallel path)
        type: zip in moonraker.conf — enables updates from Mainsail/Fluidd web UI.
        Configured automatically by install.sh.
```

### Thread Model

```
LVGL thread (main)                    Worker thread (background)
  |                                     |
  check_for_updates(callback)           |
    |                                   |
    +-- cache channel config --+        |
    +-- spawn worker thread ---+------->|
    |                                   |
    |                          fetch_r2_manifest() or fetch_*_release()
    |                          parse JSON, compare versions
    |                          |
    |                          report_result() --- ui_queue_update() --->|
    |                                                                    |
    |<---- callback(status, ReleaseInfo) on LVGL thread -----------------+
```

Key safety rules:
- **Config** is read on the main thread and cached before spawning the worker (Config is NOT thread-safe).
- **LVGL subjects** are only updated via `ui_queue_update()` from background threads.
- **The LVGL thread never joins the download worker.** `start_download()` runs inside a button's
  event callback, and the worker sits in libhv's synchronous `requests::downloadFile()`, whose
  `req->timeout` is **3600 seconds** with no abort hook (its progress callback returns `void`).
  Joining there froze the touchscreen until the transfer ended. `shutdown()` and the destructor
  wait a bounded time and then **detach** for the same reason — see `reap_download_thread()`.
- **Downloads are blocked** while a print is in progress (`PrintJobState::PRINTING` or `PAUSED`).
- **Rate limited** to one check per `MIN_CHECK_INTERVAL` (10 minutes). A manual tap inside that
  window returns the cached result and logs only at `debug`, so it looks like nothing happened.

---

## When the Updater Hides Itself

**Looking and applying are gated separately**, by two predicates in `app_globals.h`. Which
one fired is the first thing to establish on any "my updates are disabled" report:

| Predicate | Definition | Gates |
|-----------|------------|-------|
| `update_checks_suppressed()` | `updates_externally_managed()` alone | `check_for_updates()`, `start_auto_check()`, and `show_update_settings` — false hides the whole update section |
| `update_install_suppressed()` | `updates_externally_managed() \|\| !self_update_supported()` | `start_download()` and the "Install Update" row |

The weaker check gate is deliberate. Checking is a manifest fetch that touches no files, so an
install tree we cannot write is no reason to refuse to look, and knowing a newer version exists
is the only thing that makes a suppressed install recoverable — the user can still be told to
re-run the installer. The two shared one predicate through v0.99.96–v0.99.113, and that made a
false negative in `self_update_supported()` a permanent lockout: the rows vanished wholesale, so
nothing could tell the user an update existed, and the fix could only ship inside the update
they were being kept from.

The two underlying reasons, and the notice each raises:

| Reason | Predicate | UI notice |
|--------|-----------|-----------|
| Firmware owns updates | `updates_externally_managed()` — the `HELIX_DISABLE_AUTO_UPDATES` env flag, else the platform default | "Managed by your firmware" |
| Self-update can't physically apply | `!self_update_supported()` | "Updates aren't available on this installation" |

### Who decides `updates_externally_managed()`

`HELIX_DISABLE_AUTO_UPDATES` decides it **in either direction** when set to a value that
parses — truthy suppresses, falsy force-enables. Unset (or empty) defers to
`helix::platform_defaults_to_external_updates()` in `include/platform_info.h`, which is the
single place any platform is named.

Today that default is true only for the **Snapmaker U1**. PAXX Extended Firmware ships
HelixScreen as a selectable component, downloading a pinned, sha256-verified tarball into
`/oem/apps/helixscreen` via `extended-pkg`; self-updating there rewrites a package the
firmware believes it owns. Suppression previously depended entirely on the firmware hook
exporting the flag, which it does not — their lmd hook (<paxx-firmware>/etc/hooks/lmd.d/30-helixscreen.sh) exports `HELIX_DATA_DIR`,
`HELIX_SUPERVISED`, `HELIX_DRM_DEVICE`, `HELIX_CACHE_DIR`, `HELIX_CONFIG_DIR` and
`HELIX_REMOTE_SCREEN_FB0`, and nothing else. Every U1 install therefore checked for updates
and raised the update modal.

The falsy arm is the dev-box escape hatch: `HELIX_DISABLE_AUTO_UPDATES=0` turns self-update
back on where the platform defaults it off, from the CLI or a deploy script, without a
rebuild. An **empty** value reads as unset rather than falsy, because `helixscreen.env` can
export one and treating that as an explicit "no" would silently re-enable self-update on a
firmware-managed box.

ESP32 stubs both predicates constant-true (`helixapp_platform_stubs.cpp`): OTA is the only
update route there, so the settings UI hides its update affordances entirely.

`self_update_supported()` must recognise **every** route `install.sh` can take to apply an
update, because suppression is a one-way door: the fix for a false negative can only ship
inside an update the user is being prevented from installing. It is true when any of:

1. the **parent** of the install root is writable by the service user → `install.sh` takes the
   atomic swap (`mv <root> <root>.old; mv <new> <root>`); rename mutates the parent's entries,
   which is why the parent is what matters, **or**
2. the **install root itself** is writable → `install.sh` takes the in-place path: delete the
   root's contents (bar `config/`) and move the new ones in, entirely inside the root. It
   selects this by itself whenever the parent is not writable, **or**
3. root is reachable — `geteuid() == 0`, or `sudo -n true` succeeds.

Term 2 is what covers the standalone-display layout: no local Klipper or Moonraker, so
`detect_pi_install_dir()` falls through every ecosystem check to the `/opt/helixscreen`
fallback. `/opt` is root-owned, so term 1 is false — but the root itself is chowned to the
service user by the unit's `ExecStartPre`, so the install updates fine.

**Term 3 cannot rescue term 2's absence, despite what it looks like.** `config/helixscreen.service`
sets `NoNewPrivileges=true`, which strips setuid on `execve`, so `sudo` fails from the app and
from the `install.sh` it forks regardless of what sudoers says. Escalation only ever answers
for installs that are already root (`geteuid() == 0` — every root-run embedded platform) or
that run outside the shipped unit. `install.sh` knows this and has `_has_no_new_privs()`
branches throughout; the in-place update path is one of them.

The sudo probe runs at most once per process, only when terms 1 and 2 have both failed, and is
bounded at 2s so a network-backed sudoers lookup can't stall startup.

Debug bundles carry all of this under `update`: `install_parent_writable` (term 1),
`install_root_writable` (term 2), `self_update_supported` (the OR of all three),
`externally_managed`, and `suppressed`. Read the first three together:

| parent | root | supported | Meaning |
|--------|------|-----------|---------|
| true   | —    | true      | ordinary home-directory / ecosystem install, atomic swap |
| false  | true | true      | `/opt` standalone-display install, in-place update |
| false  | false| true      | running as root, or outside the shipped unit — sudo is carrying it |
| false  | false| false     | genuinely stuck; the user must re-run the installer |

The `[UpdateChecker] ... suppressed` log line names which branch fired, but the bundle fields
are what separate the four shapes above.

---

## Update Channels

Three channels are available. The channel is stored in config at `/update/channel` as an integer.

| Channel | Enum | Config Value | Source | Description |
|---------|------|-------------|--------|-------------|
| **Stable** | `UpdateChannel::Stable` | `0` | R2 stable/manifest.json, fallback: GitHub `/releases/latest` | Production releases only. Default for all users. |
| **Beta** | `UpdateChannel::Beta` | `1` | R2 beta/manifest.json, fallback: GitHub `/releases` array (first prerelease) | Includes pre-release tags (`v1.0.0-beta.1`, `v1.0.0-rc.1`). Falls back to latest stable if no prereleases exist. |
| **Dev** | `UpdateChannel::Dev` | `2` | R2 dev/manifest.json, or explicit `/update/dev_url` | Cutting-edge builds. Supports custom manifest URLs for local development servers. |

### Channel Selection in UI

The About Settings overlay (`about_settings_overlay.xml`) carries **two** Update
Channel rows with complementary conditions, because a dropdown's `options` is a
static string and the entries on offer differ per install:

| Row | Options | Visible when |
|-----|---------|--------------|
| `row_update_channel` | `"Stable\nBeta"` | `show_beta_features eq 0 and show_update_settings eq 1` |
| `row_update_channel_dev` | `"Stable\nBeta\nDev"` | `show_beta_features eq 1 and show_update_settings eq 1` |

Index 0 is Stable and index 1 is Beta in both lists, so `/update/channel` and the
single `on_about_update_channel_changed` callback behind both rows mean the same
thing whichever row is on screen. Index 2 is reachable only from the beta row.

Dev stays behind the beta gate because it fetches from an arbitrary
`/update/dev_url` rather than a published channel, and because `main` publishes to
both the beta and dev channels — the two deliver identical builds, so a third
entry would offer a stable-line user a duplicate of its neighbour.

Neither row binds its selection to a subject; `lv_dropdown` has no such binding in
the XML engine. `AboutSettingsOverlay::sync_update_channel_rows()` seeds both from
`UpdateChecker::get_channel()` on activate and whenever the 7-tap toggles beta.

### Switching Channels (and moving backward)

Changing the dropdown calls `UpdateChecker::on_channel_changed()`, which drops the
cached result, re-snapshots the config for the debug bundle's off-thread reader,
**clears the rate-limit clock**, and starts a fresh check. The rate-limit reset is
load-bearing: the limiter predates user-switchable channels, so without it the
check returns the previous channel's verdict and the About row keeps advertising a
version the newly selected channel does not serve.

The check is a three-way comparison (`compare_channel_version()`), not the old
strict `latest > current`:

| Relation | Result |
|----------|--------|
| Channel is **ahead** | `UpdateAvailable`, `is_downgrade = false` — ordinary update |
| Channel is **behind** | `UpdateAvailable`, `is_downgrade = true` — offered, never auto-notified |
| **Same** or unparseable | `UpToDate` |

`Older` has to be actionable because channels are user-selectable. Someone who ran
the devel track and switched back to stable is *ahead* of the channel they now
want; under "offer only if newer" the check reports "Already up to date" forever
and there is no way back short of a manual reinstall.

A downgrade is deliberately quieter than an update:

- The auto-check **never** raises it unprompted (a transient bad manifest would
  otherwise push a "go back" prompt to the whole fleet at once).
- The About row reads *"Switch to vX"*, not *"vX available"*.
- Tapping install shows a confirmation naming both versions before anything
  downloads.

**Config compatibility.** An older build loading a config written by a newer one
leaves it entirely alone — `run_versioned_migrations()` returns early when
`config_version > CURRENT_CONFIG_VERSION` rather than stamping it down. Migration
gates are all `version < N` so none would fire anyway; the damage was the
unconditional stamp, which made the newer build re-run already-applied migrations
on its next boot. Unknown keys survive because `Config::save()` serializes the
whole in-memory document. Pinned by `tests/unit/test_config_migration_future.cpp`.

### Dev Channel Custom URL

The dev channel supports an explicit URL override via config:

```json
{
  "update": {
    "channel": 2,
    "dev_url": "http://192.168.1.100:8080/dev/"
  }
}
```

When `dev_url` is set, the checker fetches `{dev_url}/manifest.json` directly instead of using R2. The URL must use `http://` or `https://` scheme. This is useful for testing dev builds served from a local machine.

### How CI Determines Upload Channels

The channel is declared by the **`RELEASE_CHANNEL` file at the repo root**, on the
branch being tagged. `scripts/release-channel.sh` reads it and the release workflow
consumes its output; nothing is inferred from the tag string.

| `RELEASE_CHANNEL` | R2 channels | GitHub release | Docs deploy |
|-------------------|-------------|----------------|-------------|
| `stable` | `stable` | full release | yes |
| `beta` | `beta` + `dev` | prerelease | no |
| `dev` | `dev` | prerelease | no |

Each maintenance line carries its own value, so cutting a release is just tagging
the right branch (`release/1.0` holds `stable`, `main` holds `beta`).

**Why not derive it from the tag.** The old rule was "tag contains a hyphen ->
prerelease", which forced every devel build to carry a `-devN` suffix. But
`helix::version::Version` (`include/version.h`) parses major/minor/patch and
**discards the prerelease suffix**, so `v1.1.0-dev1` and `v1.1.0-dev2` compare
EQUAL — `is_update_available()` returns false and the devel channel goes silent
after the first install. Declaring the channel out-of-band lets the devel track use
plain monotonic versions (`1.1.0`, `1.1.1`, ...) that the updater actually orders.

**Stable does not publish to `dev`.** The `dev` channel follows the devel line
alone so its manifest only ever moves forward; a `1.0.x` hotfix publishing to `dev`
would strand everyone already on `1.1.x`. The workflow enforces this with a
pre-upload guard that refuses to move any channel manifest backward (override with
the `ALLOW_CHANNEL_DOWNGRADE` repository variable).

---

## R2 CDN Distribution

The primary update source is the R2 CDN at `releases.helixscreen.org`. This avoids GitHub API rate limits and provides faster downloads.

### Manifest Format

All channels use the same manifest schema, generated by `scripts/generate-manifest.sh`:

```json
{
  "version": "0.9.5",
  "tag": "v0.9.5",
  "notes": "Bug fixes and stability improvements",
  "published_at": "2026-02-07T10:00:00Z",
  "assets": {
    "pi": {
      "zip_url": "https://releases.helixscreen.org/stable/helixscreen-pi.zip",
      "zip_sha256": "def456...",
      "zip_size": 8123456,
      "url": "https://releases.helixscreen.org/stable/helixscreen-pi-v0.9.5.tar.gz",
      "sha256": "abc123...",
      "size": 7912345
    },
    "pi32": { ... },
    "ad5m": { ... },
    "k1": { ... }
  }
}
```

Fields:
- `version` — Semver string without `v` prefix
- `tag` — Git tag with `v` prefix
- `notes` — Release notes (may be sparse; enriched with CHANGELOG.md at runtime)
- `published_at` — ISO 8601 timestamp
- `assets` — Object keyed by platform (`pi`, `pi32`, `ad5m`, `k1`, `k2`)
  - `zip_url` — **Preferred** download URL for the `.zip` archive (unversioned filename, shared with Moonraker `type: zip` updates). Emitted by default.
  - `zip_sha256` — SHA-256 hash of the zip archive
  - `zip_size` — Size in bytes of the zip archive
  - `url` — Legacy fallback download URL for the `.tar.gz` archive (may be omitted once the tar.gz is dropped)
  - `sha256` — SHA-256 hash of the tar.gz archive
  - `size` — Size in bytes of the tar.gz archive
  - The update checker prefers `zip_url` / `zip_sha256` when present and falls back to `url` / `sha256` otherwise. The `zip_*` fields are emitted **by default** — telemetry showed only ~0.6% of active devices remained on pre-v0.99.31 builds (which never read `zip_url` and keep using the legacy `url`); v0.99.31+ clients prefer the zip. Pass `generate-manifest.sh --no-include-zip` (documented in the script's `--help`) to restore the old suppression.
  - **Per-platform zip gate (helixscreen#993).** `zip_url` is withheld from the BusyBox/OpenWrt platforms listed in `ZIP_EXCLUDE_PLATFORMS` (`ad5m ad5x cc1 k1 k2 snapmaker-u1`); they keep a complete tar.gz asset. Pre-v0.99.102 in-app updaters verify a download with `unzip -tqq`, but BusyBox only grew `unzip -t` in 1.32 (K1 ships 1.31.1, AD5M 1.29.3) and the K2 has no unzip at all, so those clients reject a byte-perfect zip as "Corrupt download". **This is the only lever that reaches an already-deployed binary** — v0.99.102 fixes the verifier, but that fix ships inside the very update the broken verifier refuses to install, so a client-side fix cannot bootstrap itself. Shrink the list with `--zip-exclude` once telemetry shows a platform's population is on v0.99.102+; when it is empty, Phase 2 (dropping tar.gz entirely) becomes safe.

### URL Structure

```
https://releases.helixscreen.org/
  stable/
    manifest.json
    helixscreen-pi.zip
    helixscreen-pi-v0.9.5.tar.gz      # legacy bridge-release asset
    helixscreen-ad5m.zip
    helixscreen-ad5m-v0.9.5.tar.gz    # legacy bridge-release asset
    ...
  beta/
    manifest.json
    helixscreen-pi.zip
    helixscreen-pi-v1.0.0-beta.1.tar.gz   # legacy bridge-release asset
    ...
  dev/
    manifest.json
    ...
  symbols/
    v0.9.5/
      pi.sym
      ad5m.sym
      ...
```

### R2 Base URL Override

The R2 base URL can be overridden in config for testing:

```json
{
  "update": {
    "r2_url": "https://my-test-cdn.example.com"
  }
}
```

Default: `https://releases.helixscreen.org` (compiled as `DEFAULT_R2_BASE_URL`).

### GitHub API Fallback

If the R2 manifest fetch fails (network error, HTTP error, missing manifest), the checker falls back to the GitHub API:

- **Stable**: `GET /repos/prestonbrown/helixscreen/releases/latest` — parses single release object
- **Beta**: `GET /repos/prestonbrown/helixscreen/releases` — scans array for first non-draft prerelease, falls back to latest stable

GitHub responses are parsed for platform-specific assets. The update checker prefers an asset named `helixscreen-{platform}.zip` and falls back to the legacy versioned tar.gz (`helixscreen-{platform}-v{version}.tar.gz`) if no zip is present. Still no fallback to arbitrary assets — wrong-platform binaries could brick embedded devices.

### Changelog Enrichment

R2 manifests may have sparse release notes. After a successful R2 fetch, the checker fetches `CHANGELOG.md` from the GitHub repo's main branch and extracts the section for the detected version. This provides full Keep a Changelog formatted notes in the notification modal.

---

## Platform Detection

The platform key is determined at compile time via preprocessor defines:

| Define | Platform Key | Devices |
|--------|-------------|---------|
| `HELIX_PLATFORM_AD5M` | `ad5m` | Flashforge Adventurer 5M |
| `HELIX_PLATFORM_MIPS` | `k1` or `ad5x` | MIPS32 devices (Creality K1, FlashForge AD5X). Runtime detection via `/usr/prog` presence. |
| `HELIX_PLATFORM_K2` | `k2` | Creality K2 series. **Note:** K2 is supported in the UpdateChecker code but is not yet included in `generate-manifest.sh`. R2 manifests do not currently include K2 binaries. |
| `HELIX_PLATFORM_PI32` | `pi32` | Raspberry Pi (32-bit) |
| (default) | `pi` | Raspberry Pi (64-bit) |

Asset name format: `helixscreen-{platform}.zip` (preferred, e.g., `helixscreen-pi.zip`). The legacy `helixscreen-{platform}-{tag}.tar.gz` form is still published during the bridge release for backwards compatibility with older installed versions.

---

## Download and Install Flow

### State Machine

The download lifecycle is tracked by `DownloadStatus`:

```
Idle (0) ─> Confirming (1) ─> Downloading (2) ─> Verifying (3) ─> Installing (4) ─> Complete (5)
                |                   |                 |                |               |
                v                   v                 v                v               v
             (cancel)           (cancel)          Error (6)        Error (6)     Restarting (7)
                |                   |                 |                |               |
                v                   v                 v                v               v
             Idle (0)           Idle (0)       (Retry or Close)  (Retry or Close)   _exit(0)
```

**Cancel is deferred, and the status enum lies about it.** `cancel_download()` only sets a flag
the worker reads *after* `downloadFile()` returns, while `hide_update_download_modal()` resets
`download_status` to `Idle` immediately. So "Idle" can mean "a worker is still inside an hour-long
blocking call". `download_in_flight()` is the real answer, and `start_download()` gates re-entry on
it — a second Install while the old worker is alive is **refused with an Error**, never queued and
never joined.

**Every early return from `start_download()` must report a status.** `update_download_modal.xml`
binds every container *and* every button row to a NON-ZERO `download_status`, so a silent return
leaves an empty dialog with no buttons — dismissable only by an undiscoverable backdrop tap.

**Complete (5) → Restarting (7) → exit.** `report_download_status()` only *queues* its subject
write, so `::_exit(0)` straight after it meant the completion frame never painted and the last
thing on screen was "Installing... Do not power off your printer." The worker now holds each
terminal frame long enough for the LVGL thread to render it (2s, then 1s) and hands the exit
itself to that thread via `queue_update()`, with a 5s backstop in case no main loop is draining.

### Download Steps

1. **Safety check**: Refuses to download if a print is active or paused.
2. **Directory selection**: Scans candidate directories (`$TMPDIR`, `/tmp`, `/data`, `$HOME`, etc.) and picks the one with the **most free space** (minimum 50 MB required).
3. **HTTP download**: Uses `libhv requests::downloadFile()` with progress callback. UI updates throttled to every 2%.
4. **Size validation**: Rejects files < 1 MB or > 50 MB.
5. **Archive verification**: Validates archive integrity (`unzip -t` for zip, `gunzip -t` for legacy tar.gz) via `safe_exec()` (fork/exec, no shell).
6. **Architecture validation**: Extracts the binary from the archive and reads the ELF header to verify it matches the runtime architecture (ARM 32-bit vs AARCH64 64-bit). This prevents installing a Pi 64-bit build on a 32-bit Pi or vice versa.
7. **Install**: Runs `install.sh --local {archive} --update` via `safe_exec()`.

### Install Script Discovery

The installer is found by searching these paths in order:
1. Resolved from `/proc/self/exe` (strips `/bin/helix-screen` to find install root)
2. /opt/helixscreen/install.sh
3. /root/printer_software/helixscreen/install.sh
4. /usr/data/helixscreen/install.sh
5. /home/biqu/helixscreen/install.sh
6. /home/pi/helixscreen/install.sh
7. `scripts/install.sh` (development fallback)

### Safe Execution

All child processes are spawned via `safe_exec()` which uses `fork()/execvp()` directly, bypassing the shell entirely. This prevents command injection via crafted filenames or URLs. Stdout/stderr are redirected to `/dev/null`.

---

## Auto-Check Timer

After successful connection to a printer, `start_auto_check()` creates an LVGL timer:
- **Initial delay**: 15 seconds after startup
- **Periodic interval**: 24 hours (converted after the first check fires)

The auto-check callback:
1. Calls `check_for_updates()` with a notification callback
2. Skips notification if the version is **dismissed** (user previously chose "Ignore")
3. Skips notification if the printer is **printing or paused**
4. Populates release notes subject and shows `update_notify_modal`

### Dismissed Versions

Users can dismiss a specific version via the "Ignore" button. The dismissed version is stored in config at `/update/dismissed_version`. A version is considered dismissed if it is **less than or equal to** the dismissed version -- so only a *newer* version than what was dismissed triggers a notification.

---

## Moonraker Integration

HelixScreen registers itself with Moonraker's `update_manager` for updates via Mainsail/Fluidd web interfaces. This is a **parallel update path** -- users can update either from the HelixScreen touchscreen UI or from the web UI.

### Configuration Block

The installer appends this to `moonraker.conf`:

```ini
[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /opt/helixscreen
```

Key points:
- `type: web` -- Moonraker downloads ZIP assets from GitHub releases (workaround for mainsail-crew/mainsail#2444 where `type: zip` always shows UP-TO-DATE)
- `type: web` does **not** support `install_script`, `managed_services`, or `persistent_files` -- do not add these options (Moonraker will warn about unparsed config)
- User config files live in `printer_data/config/helixscreen/` (outside the managed path), so they survive Moonraker's `shutil.rmtree` without needing `persistent_files`
- release_info.json -- Written to the install directory so Moonraker can detect the installed version
- A systemd path unit (`helixscreen-update.path`) watches release_info.json and restarts the service after Moonraker extracts an update
- As a self-healing fallback, `helixscreen.service` runs `refresh-service-units.sh` on every start to re-template systemd units and install missing watcher units

#### Moonraker version requirement (helixscreen#993)

**This stanza requires Moonraker >= v0.10.0 (or a git checkout newer than 2025-01-19).** The installer probes for the capability and skips writing the stanza when it isn't there — see `moonraker_asset_name_support()` in `scripts/lib/installer/moonraker.sh`, which inspects the installed Moonraker source (found via `find_moonraker_update_manager_dir()`) rather than parsing a version string, since `v0.9.3-73-gfab6c5c1`-style descriptions can't be ordered across branches. It returns three states: `supported` (net_deploy.py containing `asset_name`), `unsupported` (zip_deploy.py/web_deploy.py, or a net_deploy.py without it), and `undetermined` (no source found — preserves the previous behavior and warns, rather than guessing). On `unsupported` the installer also *removes* an already-written stanza.

Asset selection lives in Moonraker's `NetDeploy._get_remote_version()` (moonraker/components/update_manager/net_deploy.py). It seeds `release_asset = assets[0]` and only overrides it when release_info.json's `asset_name` **exactly** matches an asset name; a miss logs `Asset '<name>' not found` at INFO and downloads `assets[0]` anyway. Support for `asset_name` arrived in commit `530f1c2016` (2025-01-19), which also renamed zip_deploy.py to net_deploy.py; the first tag containing it is **v0.10.0** (2026-01-21). Every earlier version reads `assets[0]` unconditionally and never looks at `asset_name`.

That matters because the GitHub API returns release assets **sorted by name**, and `_extract_release()` runs `shutil.rmtree(self.path)` *before* opening the archive. On an unsupported Moonraker the in-UI update button therefore wipes the install directory and then fails with `zipfile.BadZipFile: File is not a zip file`. Release symbol assets are named `symbols-<platform>.sym.zst` specifically so they sort after the `helixscreen-*` artifacts and never land in `assets[0]` (see the guard in `.github/workflows/release.yml`).

**Rollback is unsupported on every Moonraker version, including master.** `NetDeploy.rollback()` ignores `asset_name` entirely — it is still hardcoded to `result.get('assets', [{}])[0]`. The Mainsail/Fluidd rollback button will always fetch the alphabetically-first asset regardless of what release_info.json says, so it cannot be made to work from our side. Use HelixScreen's built-in updater or re-run the installer pinned to a version instead.

### Service Allowlist

The installer also adds `helixscreen` to `moonraker.asvc` (the service management allowlist in the printer_data directory).

### Migration

The installer detects and migrates old `type: git_repo` and `type: zip` configurations to `type: web`, cleaning up any leftover sparse clone directories. Existing `type: web` sections are cleaned of unsupported options (`persistent_files`, `managed_services`, `install_script`) that cause Moonraker warnings.

### Platform Scope

Moonraker integration is configured on **all platforms except AD5M** (which typically lacks a Mainsail/Fluidd web UI).

---

## User-Facing UI

### About Settings Overlay (`about_settings_overlay.xml`)

Located under Settings (tap the "About" action row), the About Settings overlay contains:
- **Current Version** row (7-tap to enable beta features)
- **Check for Updates** row (triggers manual check, description bound to `update_version_text` subject)
- **Install Update** row (visible only when `update_status == UpdateAvailable`)
- **Update Channel** dropdown — Stable/Beta on any install, Stable/Beta/Dev with beta features enabled

### Update Notification Modal (`update_notify_modal.xml`)

Shown automatically by auto-check when an update is found:
- Header with version info
- "View Changelog" toggle button that reveals a scrollable markdown area (`ui_markdown` bound to `update_release_notes`)
- "Ignore" button (dismisses this version) and "Install" button (opens download modal)

### Download Modal (`update_download_modal.xml`)

Multi-state modal driven by `download_status` subject:

| State | UI |
|-------|------|
| Confirming (1) | Version info + Cancel/Install buttons |
| Downloading (2) | Progress bar + percentage + Cancel button |
| Verifying (3) | Spinner + "Verifying..." text |
| Installing (4) | Spinner + "Installing..." + "Do not power off" warning |
| Complete (5) | Success icon + "Update installed!" (no buttons — the restart is automatic) |
| Error (6) | Error icon + error text + "Close" / "Retry" buttons |
| Restarting (7) | "Hang on, we'll be right back!" — the last frame before `_exit(0)` |

### LVGL Subjects

| Subject | Type | Purpose |
|---------|------|---------|
| `update_status` | int | `Status` enum (Idle/Checking/UpdateAvailable/UpToDate/Error) |
| `update_version_text` | string | Status text for the Check for Updates row |
| `update_new_version` | string | New version number (e.g., "0.9.6") |
| `update_current_version` | string | Current installed version. **Note:** This subject is owned by AboutSettingsOverlay, not UpdateChecker. |
| `update_channel` | int | Selected channel (managed by SettingsManager) |
| `download_status` | int | `DownloadStatus` enum |
| `download_progress` | int | 0-100 percentage |
| `download_text` | string | Status text shown in download modal |
| `update_release_notes` | string | Markdown release notes for notification modal |
| `update_changelog_visible` | int | Toggle for changelog visibility in notification |

---

## Configuration Reference

All update settings live under the `/update/` key in `settings.json`:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `/update/channel` | int | `0` | Update channel: 0=Stable, 1=Beta, 2=Dev |
| `/update/dev_url` | string | `""` | Custom manifest URL for dev channel |
| `/update/r2_url` | string | `""` | R2 base URL override (default: `https://releases.helixscreen.org`) |
| `/update/dismissed_version` | string | `""` | Version the user chose to ignore |

---

## Developer Guide

### Adding a New Platform

1. Add a `HELIX_PLATFORM_*` define and update `get_platform_key()` in `update_checker.cpp`
2. Add the platform to the `PLATFORMS` array in `scripts/generate-manifest.sh`
3. Add a build job in `.github/workflows/release.yml`
4. Add a `release-{platform}` target in the Makefile
5. Update `write_release_info()` in `scripts/lib/installer/moonraker.sh`

### Testing Update Checks

Run the program with `--test` mode and verbose logging:

```bash
./build/bin/helix-screen --test -vv
```

Navigate to Settings -> About -> Check for Updates. The log output shows the full check flow including R2 fetch, GitHub fallback, and version comparison.

### Testing with a Custom Dev Server

Serve a manifest locally:

```bash
# Build a local manifest
scripts/generate-manifest.sh \
  --version "99.0.0" \
  --tag "v99.0.0" \
  --notes "Test build" \
  --dir ./releases/ \
  --base-url "http://192.168.1.100:8080/dev" \
  --output ./manifest.json

# Serve it
python3 -m http.server 8080
```

Configure HelixScreen:

```json
{
  "update": {
    "channel": 2,
    "dev_url": "http://192.168.1.100:8080/"
  }
}
```

### End-to-End Self-Update Testing on Device

Use `scripts/serve-local-update.sh` to drive the complete download → install → restart
cycle without publishing a real release. See the script's `--help` for full setup.

**Quick setup (once per device):**

```bash
export HELIX_TEST_PRINTER=helixscreen.local   # device hostname or IP
export HELIX_TEST_USERNAME=pi                  # SSH username

# Configures dev channel, copies install.sh to /tmp/, enables debug logging
./scripts/serve-local-update.sh --configure-remote
```

**Two testing paths:**

| Path | Command | When to use |
|------|---------|-------------|
| Via update checker | Trigger from Settings → About → Check for Updates | Tests the full self-update flow |
| Direct local install | `ssh USER@PRINTER 'sh /tmp/install.sh --local /tmp/helixscreen-update.zip'` | Tests install.sh in isolation, bypasses update checker |

`--configure-remote` copies `install.sh` to `/tmp/` on the device automatically, enabling the direct path without an extra transfer step.

#### The Two-Install Rule for `update_checker.cpp` Changes

When you change `update_checker.cpp` (or any code that participates in the install flow),
**two consecutive installs are required** before you can observe the effect of your change:

| Install | Which binary runs the update? | What you see |
|---------|-------------------------------|-------------|
| First | The **old** binary — still in memory | Old update-checker behaviour. Your fix is on disk but not in the running process. |
| Second | The **new** binary — restarted after first install | Your change is now live. |

This is an inherent property of self-updating executables: the running process cannot
replace its own instructions mid-flight. Linux keeps the old inode in memory even after
`install.sh` swaps the file on disk. Only after the process exits and the watchdog restarts
it does the new binary take over.

**Practical workflow:**
```bash
# Serve the build (--no-build reuses the last archive for fast iteration)
./scripts/serve-local-update.sh --no-build

# On device: trigger update from Settings → About → Check for Updates
# → install completes, helix-screen restarts (1st install done, new binary running)

# Trigger update again from the UI  ← this is the one that exercises your change
```

`serve-local-update.sh` always serves version `99.0.0`, which is higher than any real
binary version, so the freshly restarted binary immediately sees a pending update and
lets you trigger the second install right away.

#### Monitoring Logs on Device

The service writes all output — launcher, watchdog, and app — to the systemd journal.
Use `-u helixscreen` (unit name) rather than `-t` (syslog identifier) to capture
everything, including install.sh lines forwarded through update_checker:

```bash
# Follow live, surviving service restarts
journalctl -u helixscreen -f

# Start from the last 50 lines (useful after the service has already been running)
journalctl -u helixscreen -f -n 50

# Dump the entire current boot session (offline analysis)
journalctl -u helixscreen -b
```

> **Note:** `journalctl -t helix -f` filters by syslog identifier, not unit name, and
> may show no output if the identifier doesn't match exactly. Stick with `-u helixscreen`.

`journalctl -f` continues across service restarts automatically — you don't need to
rerun it after the watchdog brings the new binary up.

### Unit Tests

- `tests/unit/test_update_checker.cpp` — Version comparison, JSON parsing, status enums, lifecycle, subjects, dismissed versions, auto-check timer
- `tests/unit/test_update_channel.cpp` — Beta channel array parsing, dev manifest parsing, platform asset selection, channel config mapping, R2 URL resolution

Run with:

```bash
./build/bin/helix-tests "[update_checker]"
./build/bin/helix-tests "[update_channel]"
```

### Release Workflow

1. Tag a commit: `git tag v0.9.6` (stable) or `git tag v1.0.0-beta.1` (prerelease)
2. Push the tag: `git push origin v0.9.6`
3. CI builds all platforms (Pi, Pi32, AD5M, K1, K2) via Docker cross-compilation
4. CI creates GitHub release with zips (and a legacy tar.gz per platform during the bridge release)
5. CI uploads to R2: zips, legacy tarballs, manifests, and symbol maps
6. Devices pick up the update on next auto-check (within 24 hours)

---

## Troubleshooting

### Update Check Fails

**Symptom**: "Error: Network request failed" or "Error: HTTP 403"

- Check network connectivity from the device
- R2 CDN may be down -- the checker automatically falls back to GitHub API
- GitHub API has rate limits (60 requests/hour unauthenticated). `MIN_CHECK_INTERVAL` caps a single
  running instance at 6 checks/hour, but the limiter is in-process only -- rapid restarts reset it
  and can exhaust the quota.
- Check logs: `./build/bin/helix-screen --test -vv` and look for `[UpdateChecker]` messages

### No Asset for Platform

**Symptom**: "No asset found for platform 'pi' in release X.Y.Z"

- The release may not have been built for this platform
- Check the manifest at `https://releases.helixscreen.org/stable/manifest.json`
- Verify `get_platform_key()` returns the expected value for the device

### Wrong Architecture

**Symptom**: "Error: Wrong architecture" during install

- The binary in the archive doesn't match the device's CPU architecture
- This is an ELF header check: ARM 32-bit (`armv7l`) vs AARCH64 (`aarch64`)
- Ensure the correct platform archive was downloaded (e.g., `pi32` for 32-bit Pi OS)

### Download Fails (No Space)

**Symptom**: "Error: No space for download"

- The checker requires at least 50 MB free in any writable directory
- Check disk space: `df -h`
- On embedded devices, `/tmp` may be a small tmpfs. The checker tries `/data`, `/mnt/data`, and other persistent storage paths.

### Install Script Not Found

**Symptom**: "Error: Installer not found"

- `install.sh` must be executable in the install directory
- Check: `ls -la /opt/helixscreen/install.sh` (or equivalent path)
- The checker searches multiple paths including resolving from the running binary's location

### Moonraker Update Not Showing

**Symptom**: HelixScreen doesn't appear in Moonraker's update manager

- Check that `[update_manager helixscreen]` exists in `moonraker.conf`
- Check that `helixscreen` is listed in `moonraker.asvc`
- Check that release_info.json exists in the install directory
- Restart Moonraker after config changes

### Debug Logging

Enable verbose logging for full update system trace:

```bash
./build/bin/helix-screen -vv  # DEBUG level shows all UpdateChecker messages
./build/bin/helix-screen -vvv # TRACE level for maximum detail
```

Or for deployed installations, set `HELIX_LOG_LEVEL=debug` in `~/helixscreen/config/helixscreen.env` and restart the service.

---

## Key Files

| File | Purpose |
|------|---------|
| `include/system/update_checker.h` | UpdateChecker class declaration |
| `src/system/update_checker.cpp` | Full implementation (check, download, install, auto-check) |
| `src/system/settings_manager.cpp` | Channel subject and persistence |
| `src/ui/ui_panel_settings.cpp` | Download modal callbacks, channel change handler |
| `src/ui/ui_settings_about.cpp` | AboutSettingsOverlay — version info, updates, branding, easter eggs |
| `ui_xml/update_notify_modal.xml` | Auto-check notification modal |
| `ui_xml/update_download_modal.xml` | Multi-state download/install modal |
| `ui_xml/about_settings_overlay.xml` | About Settings overlay with update controls, branding, contributors |
| `scripts/generate-manifest.sh` | Manifest generator for CI and dev |
| `scripts/install.sh` | Bundled installer (used for `--update` mode) |
| `scripts/lib/installer/moonraker.sh` | Moonraker update_manager configuration |
| `.github/workflows/release.yml` | CI: build, release, R2 upload |
| `tests/unit/test_update_checker.cpp` | UpdateChecker unit tests |
| `tests/unit/test_update_channel.cpp` | Channel/manifest parsing tests |
