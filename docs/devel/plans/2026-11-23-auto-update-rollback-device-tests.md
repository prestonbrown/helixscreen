# Automatic-update rollback: device test plan (#1304)

`kRollbackDeviceProven` (`include/system/update_rollback.h`) keeps Install overnight hidden on every platform until this plan passes. It must pass on at least one systemd host (Pi) and one SysV printer (K1 or AD5M) before the constant changes. A platform class that has not passed stays out of the rule in `rollback_support()`. The mechanism is described in UPDATE_SYSTEM.md § "Automatic Updates and Rollback". Delete this file in the change that sets the constant.

## What each run proves

| Run | New version | Expected |
|---|---|---|
| R1 launch failure | `bin/helix-screen` replaced by `#!/bin/sh` + `exit 1` | helix-watchdog retries with its backoff (3, 6, 12, 24, 48 s; `decide_restart_action`, `kRestartLoopMaxFailures` = 5) and gives up with exit 42 after about 95 s, well inside the window. The child launcher exits 42 and the guard restores. Reason `exit_42`. If the reason reads `timeout`, the watchdog kept retrying: record that as a finding |
| R2 hang | `bin/helix-screen` replaced by `#!/bin/sh` + `sleep 100000` | Nothing reports healthy; restored after 600 s. Reason `timeout` |
| R3 happy path | The unmodified 99.0.0 build | Reports healthy at 150 s. The kept tree, and only that tree, is deleted (on systemd only after the restarted start is healthy too) |
| R4 power cut | R2's build; pull power about 60 s after the restart | After boot the guard resumes at start 2 and restores at the timeout |
| R5 crash loop, real UI | A real 99.0.0 build with one scratch line after `helix::crash_loop_detected_and_record();` in `src/application/application.cpp`: `if (!get_runtime_config()->crash_loop_safe_mode) std::abort();` | It aborts on each normal start; the watchdog's crash dialog restarts it after 30 s. The fourth start, inside the detector's 120 s window, comes up in crash-loop safe mode, and its UI writes `.request`. Reason `crash_loop`, well before the 600 s timeout. If the starts are too far apart for the detector, the run ends as `timeout`: record the start times from the app log |
| R6 too many starts | R2's build | After the restart, `echo 3 > $D/update_rollback.starts` and reboot (or power-cycle three times inside the window). The next launcher restores before starting 99.0.0. Reason `too_many_starts`, and the app log shows no 99.0.0 start after the reboot |
| R7 broken new launcher | R3's build with `bin/helix-launcher.sh` replaced by `#!/bin/sh` + `exit 2` | The guard runs the new tree's launcher as its child, so this fails at once. Reason `exit_2` |
| R8 stop during the window | R3's build | About 60 s after the restart: SysV `/etc/init.d/S99helixscreen stop` (use the platform's init script name), systemd `systemctl stop helixscreen`. Expect launcher log `stopped during the rollback window` or `Stopped during the rollback window`, no `Rolling back`, and `.armed`, `.old` and the new tree still present. Then start the service again: `start 2`, healthy, `.old` removed. Repeat once with `reboot` instead of stop |
| R9 shipping build, manual update | The landing build (constant false), installed normally; serve 99.0.0 | Settings > Updates shows no Install overnight. Install Update installs 99.0.0 exactly as before this branch: afterwards no `<install>.old`, no `update_rollback.*` files, no `rollback guard` line in the launcher log. On SysV the watchdog respawns in place (`Child exited normally, restarting` after the update-restart marker); on systemd the service restarts once |

## Setup, once per device

1. Build the branch for the platform with `kRollbackDeviceProven` set to `true` in a scratch commit that never lands. Install that build normally (`install.sh --local`). This is the "previous" version, and it is rollback-aware.
2. Make sure `bin/helix-watchdog` is present in the install. Without it the launcher exports no `HELIX_ROLLBACK_DIR`, and Settings > Updates must show no Install overnight. That absence is the expected result for the platform; record it and stop.
3. On the build host, run `HELIX_TEST_PRINTER=<host> HELIX_TEST_USERNAME=<user> scripts/serve-local-update.sh --platform <p> --configure-remote` once. It selects the dev channel, installs a root-owned `update_urls.json` that trusts the local signing key, and restarts. On a non-Pi platform, write the two files by hand as the script's header describes.
4. Settings > Updates > Automatic Updates: Install overnight must be listed. Choose it.
5. Settings > Language & Time > Timezone: pick a zone where local time is about 02:30. The scheduler installs only between 03:00 and 04:00, and only 30 minutes after the last touch, so leave the screen untouched from then on. With no print running and no power-loss resume pending, the install starts at the first 5-minute tick after 03:00.

## Making each build

- R3: `scripts/serve-local-update.sh --platform <p>` builds, signs and serves 99.0.0.
- R5: build with the scratch line, then serve it as for R3.
- R1, R2 and R7: build once as for R3 and stop the server. Unzip `dist/helixscreen-<p>.zip`, replace `helixscreen/bin/helix-screen` (R7: `helixscreen/bin/helix-launcher.sh`; or the flat `bin/...` in a zip) with the two-line script from the table, `chmod +x` it, and zip it back. Then run `scripts/serve-local-update.sh --platform <p> --no-build`, which re-signs the manifest over the changed archive.

## Watching a run (all runs)

Keep these running on the device before the install starts:

```sh
D=<install parent>/.helixscreen/rollback-helixscreen   # HELIX_ROLLBACK_DIR: /home/pi/.helixscreen/rollback-helixscreen on a Pi
while :; do date +%T; ls -la "$D" | grep update_rollback; ls -d <install>* ; sleep 5; done
# The restored app deletes .result within seconds of starting, so catch it as it appears:
while :; do [ -f "$D/update_rollback.result" ] && { date +%T; cat "$D/update_rollback.result"; break; }; sleep 0.2; done
```

Launcher log: `journalctl -u helixscreen -f` on systemd. On SysV, follow wherever the init script sends the launcher's stderr. App log: the HelixScreen log file at debug level (`--configure-remote` sets `HELIX_LOG_LEVEL=debug`).

## Pass criteria

**Every run**
- App log: `Automatic install: keeping the previous version`, then `Rollback armed: v<old> -> v99.0.0, previous at <install>.old`.
- `update_rollback.armed` appears; `<install>.old` exists and holds the previous `bin/helix-screen`.
- Launcher log: `Update installed; relaunching under the rollback guard`, then `Starting v99.0.0 under the rollback guard (start 1, window 600s)`, then the NEW launcher's own start lines (`Selected binary`, `Starting main application`) from a second launcher PID. On SysV this shows the watchdog handing back. If it is missing there, the old watchdog respawned in place and the platform fails.
- During the run no `helixscreen-update.service` restart appears in the journal (systemd), including right after a rollback.

**R1, R2, R4, R5, R6 and R7 (rollback)**
- Launcher log: `Rolling back v99.0.0 to v<old> (reason: <reason>)` and `Rolled back to v<old>`, with the reason from the table.
- `.result` holds `from_version=<old>`, `to_version=99.0.0` and that reason.
- About → Version shows the old version, and the screen works: touch, printer status, a jog.
- The notification "Update Rolled Back" reads "v99.0.0 did not start properly, so v<old> was put back.", and it stays in notification history. Restart the service within 120 s of the rollback: the notice appears again. Restart after 120 s: it does not.
- `<install>.failed`, `<install>.old` and `update_rollback.armed`/`.starts`/`.request` are gone. Settings, `helixscreen.env`, printer config and user themes are as they were before the install.
- `/update/dismissed_version` in settings.json is `99.0.0`, and no new install starts at the next tick, even with the window still open.
- R4 only: after boot the launcher log shows `start 2`.

**R3 (happy path)**
- About shows 99.0.0. At about 150 s the app log says `Running for 150s; reporting healthy`. Within 5 s the launcher log says `v99.0.0 is healthy; removing the kept v<old> at <install>.old`.
- On systemd only: the service restarts once more right after that, for the unit refresh. It must come back on 99.0.0 with no rollback.
- Gone: only `<install>.old` and the `update_rollback.*` files. Still present: the install, its `config/` (diff it against a copy taken before the run), `<install parent>/.helixscreen/` and its other files, `printer_data`, and any other `<install>.old.<n>` the user had.
- Run `ls -la <install parent>` before and after: no other entry appears or disappears.
- Start a print from the slicer while 99.0.0 downloads: the app log says `Automatic install of 99.0.0 skipped: a job holds the machine`, nothing is swapped, and the install happens the next night.
- Start a print just before the healthy report on systemd: the unit-refresh restart waits until it ends (the `.busy` file appears within a second of the print starting).
- Leave R3's 99.0.0 running into the next night with a 99.0.1 served: it installs under a fresh guard (`Starting v99.0.1 under the rollback guard`), not a rollback.

## Platform classes

| Class | Hosts | Extra steps |
|---|---|---|
| systemd | Pi 64, Pi 32, x86 kiosk | `systemctl status helixscreen` stays active through the whole run: one launcher PID for R1 to R3, then the unit-refresh restart in R3. Also check that `helixscreen-update.path` caused no extra restart (its log says `Skipping: self-update restart handled by watchdog`) |
| SysV with watchdog | K1, AD5M on Klipper Mod, CC1 | `ps` shows one launcher whose PID is unchanged across the guarded launch and the exec of the restored launcher. After a rollback no second launcher or orphaned `helix-screen` remains (`ps \| grep helix`) |
| Snapmaker U1 | U1 | First check whether `bin/helix-watchdog` ships. Without it, pass means Install overnight is absent. With it, run R1 to R3 as SysV |
| Excluded | K2, AD5M/AD5X on Forge-X or ZMOD | With the scratch build, Install overnight must be absent. On a K2, also force an install with `HELIX_SELF_UPDATE=1 HELIX_UPDATE_KEEP_PREVIOUS=1 sh install.sh --local <zip> --update`. It must refuse with "cannot keep the previous version" and leave the install byte-identical (`find <install> -type f \| xargs md5sum` before and after) |

## Recording a pass

Per platform: the build SHA, the platform key, R1 to R4 pass or fail with the launcher log excerpt for each, and the before/after listings from R3. Post them on #1304. After a class passes, enable it: move `kRollbackDeviceProven` into the platform rule in `rollback_support()`, or set it once every class has passed.

## Results: Pi 3B, systemd class (2026-11-24)

Raspberry Pi 3 Model B, MainsailOS (Debian 13), systemd, install `/home/pi/helixscreen`, starting from 1.1.0-rc.4 (a294f77). Moonraker pointed at the Pi's own instance; Klipper there ran on a host-process MCU with `kinematics: none` (no motion possible). Telemetry and crash uploads off for the runs. Test builds: the branch with `kRollbackDeviceProven = true` and `kAutoUpdateIdleSeconds = 60` as uncommitted edits, with the ctl server compiled in; the update served as 99.0.0 by `serve-local-update.sh`; the window reached through the app's timezone. Times are UTC.

| Run | Build | Result | Reason / outcome | Evidence |
|---|---|---|---|---|
| R3 happy path | 26fd156e9b | PASS | Healthy, units restart, healthy, kept tree removed | 09:03:36 install; 09:04:31 path unit fired and skipped (sentinel); 09:05:06 armed; 09:05:09 watchdog handed back, guarded start 1 (new launcher as child); 09:05:17 startup cleanup kept `.old`; 09:07:48 healthy; 09:07:49 service restart for units; 09:08:04 units phase start 1; 09:10:40 healthy; 09:10:44 `.old` removed. About 7 min |
| R1 launch failure | 2be4157ac7 | PASS | `exit_42` | 10:06:12 guarded start; watchdog backoff 3/6/12/24/48 s, then "Restart loop detected ... 6 times", exit 42; 10:07:46 rolled back, 94 s after the guarded start; notice shown, `dismissed_version=99.0.0` |
| R2 hang | 6d8743064a | PASS | `timeout` | 14:36:56 guarded start; 14:46:57 "did not come up healthy (timeout)", rolled back 14:46:58 (600 s); notice shown. A first attempt hung install.sh in its `--version` check (a test-build defect) and exposed F3, F4 and F5 |
| R7 broken new launcher | 6d8743064a | PASS | `exit_2` | 15:23:08 guarded start, rolled back 54 ms later; the restored version started normally |
| R5 crash loop, real UI | f9e215da66 | PASS | `crash_loop` | 15:38:40 guarded start; three SIGABRT crashes 30 s apart; the watchdog's crash-loop dialog started Safe Mode at 15:41:41; the app wrote the request at 15:41:45; rolled back 15:41:51; the restored version started without Safe Mode. A first R5 build aborted Safe Mode starts too and ended on `timeout`; with the watchdog present the app's own 3-in-120 s detector never trips (the watchdog's 2-minute dialog spaces the starts), so the watchdog's Safe Mode carries the signal |
| R6 too many starts | f9e215da66 | PASS | `too_many_starts` | 15:49:15 guarded start; starts set to 3, reboot; after boot the launcher counted 4 > 3 and restored at 15:51:20 without starting 99.0.0; notice shown |
| R8 stop during the window | f9e215da66 | PASS | Stop kept the trial; next start resumed and passed | 15:59:32 guarded start; 16:00:32 `systemctl stop`: "Stopped during the rollback window"; marker, guard, starts, stopping and `.old` kept; 16:00:50 start 2; 16:03:30 healthy and units restart; 16:06:25 healthy, `.old` removed |
| R4 reboot during a hang | f9e215da66 | PASS | `timeout` after the reboot | 16:14:19 guarded start 1; 16:15:20 reboot; guard token overwritten by the new launcher's pid, starts=2; 16:16:25 start 2; 16:26:26 timeout, rolled back 16:26:27; notice shown. Each guarded start gets its own 600 s window; `HELIX_ROLLBACK_MAX_STARTS` bounds the total (about 3 x 10 min plus boots) |
| R9 shipping build, manual update | f9e215da66 (constant false) | PASS | Unchanged manual update | With ctl compiled in: Settings > Updates showed only the two-entry Automatic Updates row ("Download and ask"; the Install overnight row absent). A stored Install overnight ran as Download and ask (the update notice appeared, nothing installed). Install from that notice: install.sh exit 0, no keep-previous, no `.old`, no `update_rollback.*` files, path unit skipped, the service restarted once. The plain shipping zip (no ctl) installed and started cleanly; its UI was not inspected (fb0 shows the console, the app draws on a DRM plane) |

Bugs the runs found, fixed on the branch: F1 helixscreen-update.path restarting the service under a running install.sh (12970acda0, 4895b675e8, 6e0972cc0d); F2 the startup cleanup deleting the kept copy (00313754b3); a guard identifying itself by marker text (2be4157ac7); F3 a timed-out install.sh leaving its children running and F5 the install log lost in the swap (4686e5c506); F4 an automatic install failing after its swap leaving the new tree live with no guard (6d8743064a, a390c28cc4); the guarded version and its watchdog running in the kept copy's directory, so a restore brought back the trial's Safe Mode marker (f9e215da66). Not device-tested: the fresh-sentinel timer (6f8e61db02), which removes one extra service restart after a rollback.

Not covered here: the SysV class (K1, AD5M on Klipper Mod, CC1), the U1, and the excluded-class checks. A manual install that fails after its swap still leaves the new tree live with no guard (pre-existing).
