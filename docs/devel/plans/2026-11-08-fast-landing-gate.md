# A landing gate in minutes (#2004)

`make land-gate` held a median of 5 to 6 minutes from 10-22 to 11-06. On 11-07 its median was
19 minutes and its longest run 53. The C++ suite is not what grew. The gate's local lanes on
thelio wait for build slots that other sessions hold, and on `release/1.1` they also rebuild
everything and run the whole bats suite. This plan measures where the time goes and orders
the cuts by saving and by risk to what the gate catches. The target from #2004 is unchanged:
a full gate in under 20 minutes, and under 10 on an idle test host, with no step dropped.

No builds or gate runs were made for this plan. Every number below comes from a log that
already existed.

## Sources

| Source | What it gives |
|---|---|
| `~/.local/state/helixscreen/land-gate-history.tsv` on thelio | 140 gates: end time, seconds, verdict, where the C++ ran. No SHA, no per-step rows |
| `build/land-gate/results/<sha>/` in `.worktrees/beta18` and `.worktrees/beta17-recut` | Per-step rows for 7 gates (11-06 and 11-07). Main's batch trees were torn down with their rows |
| `/tmp/test-host-sweep-<tree>-<sha>.log`, `/tmp/test-host-exec-...log` on thelio | Every test-host C++ run since 11-06: compile count, shard run time, failing test |
| `.worktrees/beta18/build/land-gate/*.log` | The 22:14 gate on 11-07, every step's full log |
| `make dev-timing` | Does not see the gate: gate runs are backgrounded, and the ledger drops them |
| `jobpool status`, `uptime`, at 23:08 on 11-07 | thelio: 32 threads, load 31 to 43, pool target 30 with 0 free. zeus: 72 threads, load 4 |

## The gate today

Four lanes start together. `unit` runs on zeus, the other three on thelio.

| Lane | Host | Steps, in order |
|---|---|---|
| unit | zeus (`helix-tsan` container) | sync, test-binary build, `ci-shards` (8 shards, CI's layout), `slow` (16 shards) |
| app | thelio | `app-build`, then `ui`, `plugin`, `no-mocks`, `quality` one after another |
| bats | thelio | waits for the `app-build` row, then `bats` |
| fresh | thelio | `python`, four artefact checks, `esp32` |

The wall time is the longest of `unit`, `app-build + max(ui+plugin+no-mocks+quality, bats)`
and `fresh`.

### Per step, seconds

From the recorded results. Release gates (`release/1.1` still runs an older gate without
`slow` and `no-mocks`).

| Gate (end, verdict) | app-build | ci-shards | bats | ui | plugin | python | esp32 | quality | ledger total |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| beta17 34e6b15 (11-06 13:48 PASS) | 10 | **614** | 114 | 105 | 25 | 67 | 138 | 3 | 614 |
| beta17 eeaa26e (11-06 16:15 FAIL) | 259 | **613** | 269 | 107 | 46 | 87 | 225 | 9 | 613 |
| beta17 17f1d3d (11-06 16:28 PASS) | 5 | **258** | 89 | 84 | 23 | 62 | 119 | 3 | 258 |
| beta18 2efbeed (11-07 12:19 FAIL) | 150 | **1132** | 113 | 96 | 28 | 92 | 195 | 3 | 1132 |
| beta18 ce28602 (11-07 13:55 PASS) | 7 | **586** | 568 | 86 | 32 | 65 | 187 | 6 | 586 |
| beta18 45af015 (11-07 17:31 PASS) | 8 | 255 | **542** | 95 | 44 | 65 | 244 | 16 | 551 |
| beta18 e9de266 (11-07 21:00 PASS) | **1998** | 671 | **881** | 90 | 34 | 62 | 211 | 238 | 2879 |
| beta18 30381fe (11-07 23:06 FAIL) | **2023** | 630 | **1164** | 92 | 36 | 71 | 229 | 312 | 3188 |

Bold marks the critical path. In the last two gates it is `app-build` then `bats`, and the
sum matches the ledger to the second (1998 + 881 = 2879).

Main's batch 46 gates kept no rows, but the ledger and the zeus logs bound them. The gate
that ended at 18:12 started at 17:38 (2059 s); its zeus sweep log was last written at 17:45
and its `slow` log at 17:47. The 19:48 gate (1925 s) started at 19:16 with nothing to
compile on zeus, and its unit lane was done by 19:20. **The unit lane took 4 to 9 minutes
and thelio's lanes took 32 to 34.**

### The unit lane on zeus

| Run | Compiles | Shard run (8 shards) | ci-shards step |
|---|--:|--:|--:|
| main batch 42 to 46 passes | 0 to 3098 | 132 to 139 s | 4 to 9 min |
| beta18, 5 runs | 769 to 2129 | 199 to 267 s | 255 to 1132 s |
| `slow` tier, main batch 46 | 0 | 56 s (16 shards) | about 1 min |

The suite itself is 2 to 4.5 minutes. The rest is the test-binary build, which runs at
zeus's 70-token pool with a 45 to 68 % ccache hit rate.

### What thelio's lanes are waiting on

- **Build slots.** At 23:08 thelio's pool had target 30, 0 free, 7 consumers holding tokens.
  The 23:06 gate's bats ran 5,445 cases in 1,162 s. The whole suite is about 1,680 CPU
  seconds (#2004), so it got about 1.4 cores. The same suite runs in about 75 s on helix-dev.
- **Full rebuilds.** Both late beta18 gates spent 2,000 s in `app-build` compiling all 1,814
  translation units, LVGL included. Between e9de266 and 30381fe nothing changed in
  `lv_conf.h`, `patches/`, `Makefile` or `mk/`, so LVGL should not have rebuilt. The log
  opens with `mv: cannot stat 'build/obj/.build-flags.new'`, two makes racing on the flags
  stamp's scratch file (`mk/rules.mk`, `FLAGS_STAMP_TEXT`). The race is harmless when both
  write the same text; whether something wrote different text, or ccache evicted the
  objects, is not known. thelio's ccache is at 150.0 of 150 GB with 74,451 cleanups.
- **The whole bats suite on release.** `release/1.1` has no `scripts/bats_select.py`, so every
  release gate runs all 305 files (5,445 cases). Main's selection runs 107 product files when
  no tooling path changed.
- **A serial app lane.** After `app-build`, `ui`, `plugin`, `no-mocks` and `quality` run one
  after another: 150 to 450 s, depending mostly on `quality`.
- **Work done twice.** `quality` runs `tests/python` (`scripts/qc/python_tests.sh`, 84 s in the
  23:06 gate) and the `python` step runs the same directory (62 to 92 s). The clang divergence
  check took 251 of `quality`'s 310 s on a 267-file merge.

## Candidate cuts

Savings are against the 11-07 gates. "Detection" is what the gate could stop catching.

| # | Cut | Saving | Risk to detection |
|---|---|---|---|
| 1 | Run the gate's local lanes where cores are free: zeus (72 threads, idle most of the day) or helix-dev, keeping thelio as a fallback. Failing that, the gate reserves a pool share with `scripts/helix-claim hold` for its lifetime | 20 to 45 min on a loaded day. The 23:06 gate's bats alone falls from 1,162 s to about 100 s at full width | None: same steps, same commit. The container differs from thelio (clang, Ubuntu), which is closer to CI, not further |
| 2 | Find and fix the 1,814-unit rebuild: per-make scratch name for `.build-flags.new`, log `ccache --print-stats` around `app-build`, size the cache to the trees in use | Up to 33 min when it fires; 0 when it does not | None |
| 3 | Port the bats selector (c03aa8b0e5) to `release/1.1` | 450 to 1,060 s on every release gate | Release tooling tests would run only when their triggers change. `make release-gate` should keep `BATS_ALL=1`, since the nightly runs main only |
| 4 | Start bats at t=0. Only the files that drive `build/bin/helix-screen` wait for `app-build` (five of the 107 product files: `test_helixctl_json`, `test_headless_display`, `test_helix_launcher_e2e`, `test_resolve_backtrace`, `test_resource_advisor`, plus `test_code_lint` and `test_doc_refs_gate` if their binary reads are real) | Up to the whole of `app-build` off the bats lane: 1,998 s on e9de266 | Low. A lint keeps the list honest: a bats file naming `build/bin/` must be on the wait list |
| 5 | Run `ui`, `plugin`, `no-mocks` and `quality` side by side after `app-build`; give `quality` a variable that skips `qc_python_tests` inside the gate, as `GITHUB_ACTIONS` already does | 150 to 200 s of serial tail; 65 to 90 CPU-s per gate | None. `python` still runs `tests/python` once. `quality` already shares the tree with a running make in local mode |
| 6 | Skip steps a diff cannot affect: `plugin` without a change under the moonraker plugin or its fixtures, `esp32` and `no-mocks` already range-select | 25 to 45 s, off the critical path | Medium for `ui` and `python`, which reach most of the tree; do not path-gate them |
| 7 | Split or rebalance the slowest CI shards | At most 60 s of a 132 to 267 s shard run, off the critical path | High. CI's 8-shard layout is the point of `ci-shards`: a use-after-free crashes only beside the neighbours CI gives it. Changing it here and not in `build.yml` loses that |
| 8 | Test-impact selection for the C++ shards, reusing `mutate_diff.py#auto_tests_filter` (#2051, #1984) | At most the shard run, 2 to 4.5 min, on a lane that already finishes 20+ minutes before thelio's | High, measured below. Selection also saves no compile: the test binary is built whole either way |
| 9 | sccache or a shared cache between thelio and zeus | Small. ccache is already on both hosts; the losses come from invalidation and eviction (cut 2), not from a missing cache | None |

### Replaying stem selection against today's red gates

`mutate_diff.py#auto_tests_filter`, run over each red gate's changed `src/` and `include/`
files (source text at the red commit, test sources from main today), plus every changed test
file. A file whose stem names too many tests returns no filter, which here means "run all".

| Red gate | Base | Changed code files | Files with no filter | Test files the filters name | Failing file named? |
|---|---|--:|--:|--:|---|
| ea36bbac3e | 6ed5d40002 | 73 | 30 | 161 | no (`test_print_status_widget_detailed_format`) |
| e5af2c8998 | 6ed5d40002 | 112 | 39 | 199 | no (`test_persisted_setting`) |
| 45e0c6567f | v1.1.0-beta.17 | 100 | 26 | 215 | no (`test_xml_bind_compose`) |
| 2efbeed9b5 | v1.1.0-beta.17 | 127 | 28 | 275 | yes, both |
| e5d00a9b31 | v1.1.0-beta.17 | 163 | 40 | 331 | no, neither |

Every batch has 26 to 40 files whose stem reaches too many tests (`app_globals.h`,
`ams_state.h`, `moonraker_client.h`), so the safe rule, any unfiltered file runs everything,
runs the whole suite on all five and saves nothing. The narrow union of filters misses four
of five. The persisted-setting mirror is the clearest case: it reads every settings table,
and no stem rule connects it to the setting that was added. Selection is worth building only as an early "first red" signal
inside the lane, never in place of the full CI-layout run.

## Recommended sequence

Each step is measured by the next gates' rows before the following one starts.

1. **Record what a gate did.** The ledger gains the judged SHA, the base, and each step's
   status and seconds; results survive `teardown-worktree.sh` by living beside the ledger.
   Without this no cut can be measured and no replay corpus grows. Small, tooling only.
2. **Cut 3, the bats selector on `release/1.1`**, ported with its `parallel` requirement and
   the Dockerfile change. Release gates lose 8 to 18 minutes at once.
3. **Cuts 4 and 5**, both inside `scripts/land-gate.sh`: bats split by whether a file needs the
   binary, and the app lane's tail in parallel. The critical path becomes
   `max(unit, app-build + quality, bats, fresh)`.
4. **Cut 2**, the rebuild. Instrument first (ccache stats and the flags stamp's text before and
   after each make the gate starts), then fix what the numbers name.
5. **Cut 1**, the lanes on zeus. Needs GNU `parallel` in `helix-tsan` (missing at 23:07 on
   11-07), a decision on `esp32` (docker inside the container, or leave it on thelio), and
   headless SDL for `ui`. `test-host-run.sh exec` already runs `make test-slow` there, so the
   path exists. Until then, the reservation variant: the gate takes a token share for its
   lifetime instead of competing for single slots.
6. Re-measure. Expected: release gates about 10 to 12 minutes (zeus's unit lane becomes the
   critical path), main gates 5 to 9. Cuts 6 to 9 are revisited only if a gate still misses
   20 minutes.

## Proving the gate still catches what it caught

Every change to the gate is replayed against the reds it has seen. The corpus, from the logs
above and from CI:

| SHA | Tree | Red step | What failed |
|---|---|---|---|
| ea36bbac3e | main-batch46 | ci-shards | `tests/unit/test_print_status_widget_detailed_format.cpp:198` |
| e5af2c8998 | main-batch46 | ci-shards | `tests/unit/test_persisted_setting.cpp:571` (the setting mirror, fixed in 08b7539ae1) |
| 45e0c6567f | beta18 | ci-shards | `tests/unit/test_xml_bind_compose.cpp:269` |
| 2efbeed9b5 | beta18 | ci-shards | `test_context_menu_row_width.cpp:207`, `test_printer_retarget.cpp:177` |
| e5d00a9b31 | beta18 | ci-shards | `test_prep_start_waiter.cpp:199`, `test_queued_job_start.cpp:516` |
| eeaa26e97e | beta17-recut | ci-shards | sweep log for it is gone; rerun to name it |
| 30381fe81f | beta18 | bats, quality | bats `not ok 575` (override_store_ assignment lint); quality: 2 `src/` files undecided for ESP32 |

CI reds after a gate had passed, which the new gate should catch and the old one did not:

| SHA | CI job | Step |
|---|---|---|
| 76b83226c5 | Build; esp32-build | Shell Tests (BATS); Size budget gate |
| 6ed5d40002, c28bd362f0 | Build | Test (Ubuntu 22.04, leg 1) |
| 571922dbb6 | Build; esp32-build | Shell Tests (BATS); Build firmware |
| c8d1c7dd62, 19f739092d | esp32-build | Build firmware |
| c12f253eab | Code Quality | Python script tests |

Procedure, run once per gate change, on zeus:

1. For each corpus SHA, a scratch worktree at that SHA with the new `scripts/land-gate.sh` and
   its helpers copied over it, and `LAND_GATE_BASE_REF` set to the base that gate used.
2. Run only the steps the change touches (`STAGES=`), so a bats change replays bats, and a
   selection change replays the step it selects for.
3. Pass: every red step is red again with the same test named. A green on a corpus SHA blocks
   the change.
4. For a selection cut, the cheap check first: run the selector over (base, SHA) and confirm
   the failing file is selected, as the replay above does. Only a selection that passes that
   goes to a real run.
5. Add each new red gate to the corpus as it happens, from the ledger's SHA and step columns
   (sequence step 1).

The corpus reaches no CI red in the `esp32` size budget or firmware link, which the gate does
not run. Those stay out of scope here.

## Status (2026-11-08)

| Item | State |
|---|---|
| Sequence 1, record what a gate did | Done. Each history row adds the judged SHA (`+dirty`), the base SHA and `<step>=<STATUS>:<seconds>` for every step. Results under `build/land-gate/results/` still go with the tree |
| Cut 2, instrumentation | Done. `app-build.why.log`: objects recompiled (oldest first), shared stamps whose mtime moved, the flags stamp's text when it changed, ccache counter delta. The fix waits on what the next gates record. One candidate to check against them: the fresh lane's makes parse at t=0 beside app-build, and two parses writing `.build-flags.new` at once can let one `cmp` read the other's half-written file and move a stamp holding truncated text |
| Cut 4, bats at t=0 | Done. `bats_select.py --app` splits the selection on `require_app` or `build/bin/`; the `without` half starts with the gate, the `needs` half waits for app-build's row. `apply-patches` now runs before every full gate's lanes, not only a test-host one |
| Cut 5, parallel app tail | Done. ui, plugin, no-mocks and a test-host gate's quality run side by side; quality skips `qc_python_tests` (`HELIX_QC_SKIP_PYTHON_TESTS=1`) |
| Cut 1, lanes on zeus | Open: question 1 |
| Cut 3, selector on `release/1.1` | Open: question 3 decides `release-gate`'s `BATS_ALL` |
| Selected C++ first red (cut 8) | Open: question 4 |
| Cuts 6, 7, 9 | Not started; revisit after re-measuring |

## Open questions

1. May the gate's local lanes move to zeus (cut 1)? It needs `parallel` added to the
   `helix-tsan` image, and the `esp32` step either needs docker inside the container or stays on
   thelio.
2. Why did beta18's `app-build` recompile all 1,814 units twice with no flag change? Cut 2
   instruments before it fixes.
3. Should `make release-gate` force `BATS_ALL=1` once the selector reaches `release/1.1`, given
   that the nightly covers main only?
4. Is a selected C++ "first red" signal wanted at all, given the replay result and that the
   unit lane is already off the critical path?
