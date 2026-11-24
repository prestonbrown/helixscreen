# Automatic-update rollback: device test plan (#1304)

`kRollbackDeviceProven` (`include/system/update_rollback.h`) keeps Install overnight hidden on every platform until this plan passes. It must pass on at least one systemd host (Pi) and one SysV printer (K1 or AD5M) before the constant changes. A platform class that has not passed stays out of the rule in `rollback_support()`. The mechanism is described in UPDATE_SYSTEM.md § "Automatic Updates and Rollback". Delete this file in the change that sets the constant.

## What each run proves

| Run | New version | Expected |
|---|---|---|
| R1 launch failure | `bin/helix-screen` replaced by `#!/bin/sh` + `exit 1` | The watchdog gives up (exit 42); the launcher restores the previous version at once. Reason `exit_42` |
| R2 hang | `bin/helix-screen` replaced by `#!/bin/sh` + `sleep 100000` | Nothing reports healthy; the launcher restores the previous version after 600 s. Reason `timeout` |
| R3 happy path | The unmodified 99.0.0 build | It reports healthy at 150 s; the previous tree is deleted, and only that tree |
| R4 power cut | R2's build; pull power about 60 s after the restart | After boot the guard resumes with a start count of 2 and restores at the timeout |

## Setup, once per device

1. Build the branch for the platform with `kRollbackDeviceProven` set to `true` in a scratch commit that never lands. Install that build normally (`install.sh --local`). This is the "previous" version, and it is rollback-aware.
2. Make sure `bin/helix-watchdog` is present in the install. Without it the launcher exports no `HELIX_ROLLBACK_DIR`, and Settings > Updates must show no Install overnight. That absence is the expected result for the platform; record it and stop.
3. On the build host, run `HELIX_TEST_PRINTER=<host> HELIX_TEST_USERNAME=<user> scripts/serve-local-update.sh --platform <p> --configure-remote` once. It selects the dev channel, installs a root-owned `update_urls.json` that trusts the local signing key, and restarts. On a non-Pi platform, write the two files by hand as the script's header describes.
4. Settings > Updates > Automatic Updates: Install overnight must be listed. Choose it.
5. Settings > Language & Time > Timezone: pick a zone where local time is about 02:30. The scheduler installs only between 03:00 and 04:00, and only 30 minutes after the last touch, so leave the screen untouched from then on. With no print running and no power-loss resume pending, the install starts at the first 5-minute tick after 03:00.

## Making each build

- R3: `scripts/serve-local-update.sh --platform <p>` builds, signs and serves 99.0.0.
- R1 and R2: build once as for R3 and stop the server. Unzip `dist/helixscreen-<p>.zip`, replace `helixscreen/bin/helix-screen` (or the flat `bin/helix-screen` in a zip) with the two-line script from the table, `chmod +x` it, and zip it back. Then run `scripts/serve-local-update.sh --platform <p> --no-build`, which re-signs the manifest over the changed archive.

## Watching a run (all runs)

Keep these running on the device before the install starts:

```sh
D=<install parent>/.helixscreen      # HELIX_ROLLBACK_DIR: /home/pi/.helixscreen on a Pi, /usr/data/.helixscreen on a K1
while :; do date +%T; ls -la "$D" | grep update_rollback; ls -d <install>* ; sleep 5; done
# The restored app deletes .result within seconds of starting, so catch it as it appears:
while :; do [ -f "$D/update_rollback.result" ] && { date +%T; cat "$D/update_rollback.result"; break; }; sleep 0.2; done
```

Launcher log: `journalctl -u helixscreen -f` on systemd. On SysV, follow wherever the init script sends the launcher's stderr. App log: the HelixScreen log file at debug level (`--configure-remote` sets `HELIX_LOG_LEVEL=debug`).

## Pass criteria

**Every run**
- App log: `Automatic install: keeping the previous version`, then `Rollback armed: v<old> -> v99.0.0, previous at <install>.old`.
- `update_rollback.armed` appears; `<install>.old` exists and holds the previous `bin/helix-screen`.
- Launcher log: `Update installed; relaunching under the rollback guard`, then `Starting v99.0.0 under the rollback guard (start 1, window 600s)`. On SysV this shows the watchdog handing back. If it is missing there, the old watchdog respawned in place and the platform fails.

**R1, R2 and R4 (rollback)**
- Launcher log: `Rolling back v99.0.0 to v<old> (reason: <reason>)` and `Rolled back to v<old>`, with the reason from the table.
- `.result` holds `from_version=<old>`, `to_version=99.0.0` and that reason.
- About → Version shows the old version, and the screen works: touch, printer status, a jog.
- The notification "Update Rolled Back" reads "v99.0.0 did not start properly, so v<old> was put back.", and it stays in notification history.
- `<install>.failed`, `<install>.old` and `update_rollback.armed`/`.starts`/`.request` are gone. Settings, `helixscreen.env`, printer config and user themes are as they were before the install.
- `/update/dismissed_version` in settings.json is `99.0.0`, and no new install starts at the next tick, even with the window still open.
- R4 only: after boot the launcher log shows `start 2`.

**R3 (happy path)**
- About shows 99.0.0. At about 150 s the app log says `Running for 150s; reporting healthy`. Within 5 s the launcher log says `v99.0.0 is healthy; removing the kept v<old> at <install>.old`.
- On systemd only: the service restarts once more right after that, for the unit refresh. It must come back on 99.0.0 with no rollback.
- Gone: only `<install>.old` and the `update_rollback.*` files. Still present: the install, its `config/` (diff it against a copy taken before the run), `<install parent>/.helixscreen/` and its other files, `printer_data`, and any other `<install>.old.<n>` the user had.
- Run `ls -la <install parent>` before and after: no other entry appears or disappears.

## Platform classes

| Class | Hosts | Extra steps |
|---|---|---|
| systemd | Pi 64, Pi 32, x86 kiosk | `systemctl status helixscreen` stays active through the whole run: one launcher PID for R1 to R3, then the unit-refresh restart in R3. Also check that `helixscreen-update.path` caused no extra restart (its log says `Skipping: self-update restart handled by watchdog`) |
| SysV with watchdog | K1, AD5M on Klipper Mod, CC1 | `ps` shows one launcher whose PID is unchanged across the guarded launch and the exec of the restored launcher. After a rollback no second launcher or orphaned `helix-screen` remains (`ps \| grep helix`) |
| Snapmaker U1 | U1 | First check whether `bin/helix-watchdog` ships. Without it, pass means Install overnight is absent. With it, run R1 to R3 as SysV |
| Excluded | K2, AD5M/AD5X on Forge-X or ZMOD | With the scratch build, Install overnight must be absent. On a K2, also force an install with `HELIX_SELF_UPDATE=1 HELIX_UPDATE_KEEP_PREVIOUS=1 sh install.sh --local <zip> --update`. It must refuse with "cannot keep the previous version" and leave the install byte-identical (`find <install> -type f \| xargs md5sum` before and after) |

## Recording a pass

Per platform: the build SHA, the platform key, R1 to R4 pass or fail with the launcher log excerpt for each, and the before/after listings from R3. Post them on #1304. After a class passes, enable it: move `kRollbackDeviceProven` into the platform rule in `rollback_support()`, or set it once every class has passed.
