# Device Matrix Sweep (`rigsweep`) — Design

Date: 2026-08-30
Status: approved shape, pre-implementation

## Goal

Every platform in the matrix — real hardware, real Moonraker — gets a fresh
build deployed and smoke-tested on a schedule, and the result is one evergreen
GitHub issue (status table) plus an issue per failed rig. The maintainer's
mental load goes from "did my change break the K2?" (manual, anxious) to
"glance at one issue" (automated, red when it matters).

## Constraints settled with Preston

| Decision | Choice |
|----------|--------|
| Orchestrator language | Bash (matches existing tooling; BATS-testable) |
| Runner | cron on **zeus** (house server; `make deploy-*` cross-builds already run there) |
| Smoke mode | Real Moonraker on the rig, **skip-if-printing** |
| Cadence | Nightly rotating canary (one rig/night) + weekly full sweep (all rigs) |
| Build source | **Green-CI commit only** — latest `build.yml` success on `main` |
| Reporting | Evergreen status issue + one issue per rig failure, auto-close-on-pass |
| Device interaction | **Full takeover**: stop installed service, smoke new build, restore previous install |

Defaults chosen without objection, overridable later: canary rigs rotate
through the roster; OFFLINE becomes a failure issue after 3 consecutive nights;
failure issues auto-close when the rig passes again.

## Architecture

Three new files, one reused script pattern:

```
scripts/rigs.conf           # rig manifest (one line per rig)
scripts/rigsweep.sh         # orchestrator: select commit -> build -> sweep -> report
scripts/smoke-device.sh     # on-rig half of the smoke (ssh-piped, BusyBox-safe)
```

Plus a cron entry on zeus (`rigsweep-run@.timer` equivalent): nightly 03:00
canary, Sunday 04:00 full.

### `rigs.conf` manifest

One line per rig:

```
<name> <ssh-target> <platform> <deploy-target> <tier>
# e.g.
ad5m root@192.168.1.67 ad5m deploy-ad5m full
```

- `ssh-target` — as passed to `make <deploy-target> <PLATFORM>_HOST=...`
- `platform` — the `make PLATFORM_TARGET=` value (`pi32`, `ad5m`, `ad5x`,
  `cc1`, `k1-dynamic`, `k2`, `snapmaker-u1`, ...)
- `tier` — `canary` | `full` (all rigs are in the full sweep; canary rigs are
  the nightly rotation pool)

Initial roster: Pi, AD5M, AD5X, SonicPad, K1C, K2, U1, CC1, CB1/Voron (per
device inventory in project memory). Rigs added/retired by editing the file —
no code change.

### `rigsweep.sh` — orchestrator

Subcommands:

```
rigsweep.sh sweep [--canary|--full|--rig NAME] [--dry-run]
rigsweep.sh status    # dump last sweep's results from the evergreen issue
```

Pipeline:

1. **Lock** — `flock` on `/work/rigsweep/.lock`; a long full sweep must not
   overlap the next canary. Second invocation exits immediately with a note.
2. **Commit select** — `gh run list --branch main --workflow build.yml
   --status success --limit 1` → SHA. If none (CI red), abort with a warning
   issue-edit; do not sweep a known-broken commit.
3. **Build** — detached checkout of SHA in a `git worktree` under
   `/work/rigsweep/worktrees/`, then `make PLATFORM_TARGET=<p>` for each
   distinct platform in the sweep, bounded `-j` (zeus is shared; check load).
   Cache build dirs between sweeps keyed by platform to reuse ccache.
4. **Per-rig sweep** (sequential, order = manifest order):
   - **Pre-flight** (over ssh): query Moonraker
     `printer/objects/query?print_stats` on the rig.
     - `printing`/`paused` → **SKIP** (recorded, never an issue)
     - Moonraker unreachable or ssh fails → **OFFLINE** (row only; becomes a
       failure issue after 3 consecutive nights)
   - **Backup**: tar the installed app dir on the rig, excluding `config/`
     and logs (same exclusion list as `DEPLOY_*_EXCLUDES` in `mk/cross.mk`).
     Stored under `/work/rigsweep/backups/<rig>/` on zeus.
   - **Deploy**: stop the rig's helix-screen service, then
     `make <deploy-target> <PLATFORM>_HOST=<host>` from the checked-out tree.
   - **Smoke**: run `scripts/smoke-device.sh` (ssh-piped to the rig):
     boot app in foreground with `--remote-socket /tmp/rigsweep.sock` pointed
     at the rig's real Moonraker, wait for socket, `ctl navigate` three
     panels, `ctl screenshot`, `ctl shutdown`, exit-code + crash-signature
     checks. Log tail + screenshot returned to zeus under
     `/work/rigsweep/artifacts/<date>-<rig>/`.
   - **Restore**: untar backup, verify restored binary sha256, restart
     service. **Restore failure = immediate critical issue** (rig left on a
     dev build must never be silent).
5. **Report** — edit the evergreen issue's status table (date, SHA, per-rig
   PASS/SKIP/OFFLINE/FAIL); on FAIL open `[rigsweep] <rig> FAIL <date>` with
   log tail + artifact path, labels `rigsweep` + platform label. Close
   prior open failure issues for rigs that passed this sweep. `gh` calls are
   stubbed in tests via PATH shim.

### `smoke-device.sh` — on-rig half

The `smoke-headless.sh` sequence adapted for a real rig:

- **No `--test`** — must talk to the real Moonraker.
- BusyBox-safe (`#!/bin/sh`, no bashisms; explicit `>file 2>&1 &`, never
  `&>` — BusyBox ash parses `&>` as a redirect while dash treats it as
  background, per measured shell matrix).
- Same checks: socket appears within timeout (poll, no fixed sleep),
  `navigate settings|filament|print_select`, screenshot non-empty,
  `ctl shutdown` within 30s, exit-code table (0 / 139 / 134 / other),
  crash-signature grep, fail loudly with last-60-lines.

## Data flow

```
cron (zeus) -> rigsweep.sh
  -> gh API (green SHA) -> git worktree -> make (N platforms)
  -> for rig in roster:
       ssh: moonraker print_stats -> SKIP/OFFLINE/proceed
       ssh: service stop; make deploy; smoke-device.sh
       ssh: restore backup; service start
  -> artifacts on zeus; gh issue edits/creates
```

## Error handling matrix

| Event | Sweep result | GitHub effect |
|-------|-------------|---------------|
| Rig printing/paused | SKIP row | none |
| Rig offline (1-2 nights) | OFFLINE row | none |
| Rig offline (≥3 nights) | OFFLINE row | failure issue |
| Smoke fails (any check) | FAIL row | failure issue + artifact path |
| Restore fails | FAIL row | **critical** failure issue immediately |
| No green CI commit | abort sweep | note on evergreen issue |
| Lock held | abort sweep | none (expected overlap) |
| Rig passes after failing | PASS row | close its open failure issue |

## Testing

BATS suite (`tests/shell/test_rigsweep.bats`) covering:

- `rigs.conf` parsing: valid lines, comments/blank lines, malformed line
  hard-fails with line number
- Pre-flight classification from a fake `print_stats.state` JSON:
  printing → SKIP, paused → SKIP, idle → proceed, missing → OFFLINE
- `smoke-device.sh` against a local `helix-screen --test` booted headless
  (mock, socket available — same technique `smoke-headless.sh` tests use):
  clean pass, exit-139 detection, crash-signature grep
- Reporting: issue-body generation, auto-close candidate computation — `gh`
  stubbed via PATH shim, dry-run mode emits actions without touching a rig
- Restore-failure path forces the critical issue regardless of smoke result

Manual verification (not in CI — rigs are LAN-only): `rigsweep.sh sweep
--rig cb1 --dry-run` first, then one real single-rig sweep on the least-used
rig, then enable cron.

## Out of scope (YAGNI)

- Web dashboard / notifications beyond GitHub issues
- Parallel per-rig sweeps (zeus load; sequential is deterministic)
- Rolling back a rig's *installed* version if the sweep restores correctly
  (restore is the rollback)
- Running against printers Preston uses for real prints — the roster is the
  test-rig list; real-print interference remains manual-permission territory
