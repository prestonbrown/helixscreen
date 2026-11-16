# Careful testing and instrumentation after the 1.2 refactors

Batches 18 through 31 moved a lot of code with tests and reviews but little hardware or mutation
evidence. This plan collects the evidence before any further refactor. No new refactor batch starts
until phases 1 to 3 report.

## Principles

- One thing at a time on one device. Claim `device:<name>`, ask before anything that moves a machine.
- A number only counts with its baseline: `v1.1.0-beta.16` or the commit before batch 18, same device.
- A green run proves little until the check has been seen to fail.
- Findings become issues or small fix branches; they do not widen a phase.

## Phase 0: land what is in flight

Batch 31, with #1900 after D1 and D2. `helixscreen-ab`'s perf farm and host probe follow-ups.

## Phase 1: nightly green (first run tonight)

Read tonight's run on the batch 31 tip. Sort every red into new or old. Expected risks: the abort-default
thread check in flows the discovery run missed, and the open TSan stream-stop case. Run the Test Quality
Gates order-dependence job, which `nightly-27` never saw run. Done means one fully green nightly.

## Phase 2: whole-branch mutation

`scripts/test-host-run.sh mutate --base 81f8623e9d --max-minutes 0`, or split by subsystem if a single
run cannot finish overnight, on an idle machine. Base is the commit before batch 25. Start with the
files where a wrong answer hits hardware:

1. `src/printer/ams_backend*.cpp` and the capability interfaces, print routing first
2. `src/printer/notice.cpp`, `publish_ui_request` handlers: `OpenHardwareHealth` and the held prompt have no direct test
3. `src/network/ipv4_*`: the store wiring runs real nmcli and has no test, so `required=false` would survive
4. Released overlays' lifecycle

First, fix why so many hunks read `uncompilable` and why the baseline eats the cap (#1984, #2004).
Survivors become tests, or a written reason it is equivalent. Report kill rate per subsystem.

## Phase 3: device measurement

| Question | Devices | Tool | Baseline |
|---|---|---|---|
| Overlay reopen cost now the tree is rebuilt | AD5M, CC1, K1C | timing log around show(), `ctl` loop of the 19 flipped overlays | beta.16 |
| RSS after 20 open and close cycles, leak or not | AD5M, K1C | `/proc/<pid>/status` | same run |
| CPU idle and printing | full farm | `scripts/perf-farm-check.sh`, `config/perf_budgets.json` | budgets |
| RSS and CPU over a long real print | K1C, AD5X | print health log line, once a minute | beta.16 |
| Firmware links and boots with the notice router | K-Touch, when connected | `idf.py build`, serial | current |
| Splash handoff | U1 DRM, Pi fbdev | boot, screenshot | current |

Real prints only with the plate confirmed clear, and each machine-moving step confirmed.

## Phase 4: instrumentation worth keeping

Decide from what phases 1 to 3 needed and lacked. Built (#2042): overlay open duration
and build count, frame render time, slow frames and peak RSS, read by `ctl perf`, the debug
bundle's `ui_perf` and the print health line (`docs/devel/PERFORMANCE.md`). Candidates still
open:

- A telemetry field for overlay reopen time, opt-in like the rest, only if devices show a real range
- A firmware link in the ESP32 gate, or a nightly on it, since syntax checks missed it once already
- `scripts/perf-farm-check.sh` on a schedule from zeus, with results kept for trend

Each must show a real question it answers, per the rule that performance code needs a measured benefit.

## Open items carried in

- #2017, #2018, #2019: #1900's revert gaps. D3 to D5 from the batch 31 review.
- Mutation survivors: `quality-checks.sh` failed-gates summary has no test seam.
- Test lines grew about 2,700 across batches 27 and 31 under grants: review after phase 2 for what to cut.
- Sensor names stay English under Settings > Sensors unless Preston rules otherwise.
- Android PCH lost json and spdlog: the build is slower and unbuilt. `jobpool hold` exit status on thelio.
- Flake to check: `test_remap_restore_confirmation.cpp:585`. `[printer_manager]` after `[probe]` order failure.

## Where it stands (2026-11-06)

Phases 1 to 3 have reported. Phase 1: the nightly is green apart from the order-dependence fixes that landed in batch 39. Phase 2: 232 survivors, 106 still open (#2044). Phase 3: the K1C, AD5M and AD5X are measured. No leaks; print status CPU is up on two boards with the cause unknown (#2041); Spoolman reopens slower (#2036); the print preview's layers per frame can stick at 1 (#2035). Phase 4 is #2042. Hardware checks still owed: #2043. Tooling follow-ups: #2046. Test-line review: #2045. Remaining refactors: #2047.
