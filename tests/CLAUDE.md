# tests/CLAUDE.md — Writing HelixScreen Tests

## Layout

| Path | Contents |
|------|----------|
| `tests/unit/` | Catch2 unit tests. **Auto-globbed** (`Makefile#TEST_UNIT_DIR`) — drop a `.cpp` in and it builds |
| `tests/unit/application/` | App-lifecycle tests (separate glob, own include path) |
| `tests/mocks/` | Mock implementations |
| `tests/shell/` | `bats` tests — installer, lint gates, packaging. Product unless listed in `tooling-triggers.tsv` (§ "Which shell tests run") |
| `tests/ui/` | Out-of-process pytest driving the real binary via `helix-screen ctl` |
| `tests/test_helpers/` | `*TestAccess` friend classes for reaching private members |

```bash
make t F='[tag]'               # build, then run ONE tag or case (the inner loop)
make test                      # build tests only (does NOT run them)
make unit-sweep                # build AND run the C++ unit suite, sharded (~50s)
make full-test-run             # unit-sweep + bats shell suite (~2m) - the completion gate
make land-gate                 # C++ in CI's 8-shard layout + bats + tests/ui + freshness - before landing
./build/bin/helix-tests "[tag]"  # one tag with NO dependency scan, 0.05-0.6s
```

`make -j` builds **only** the app binary. Run `make test` before `./build/bin/helix-tests`
or you are testing a stale binary. The binary says so: run from a checkout where a file it
is compiled from differs from its build, it prints a one-line `WARNING: helix-tests is
stale` to stderr before the run (build-time stamp from `scripts/tree_stamp.sh`, over the
files `scripts/test_link_sources.py` lists; `HELIX_TESTS_NO_STALE_CHECK=1` skips it, and
runs make starts skip it because make just rebuilt the binary). A run from a tree with no `.git` (an rsynced
mirror, an unpacked artifact) checks nothing. ui_xml/ and assets/ are read at run time and never stale it.

---

## Fixtures — inherit, don't hand-roll

```
HelixTestFixture          drains UpdateQueue, resets language, clears ModalStack (ctor + dtor)
└── LVGLTestFixture       + LVGL init, headless display, UpdateQueue init/shutdown
    └── LVGLUITestFixture + XML component registration
XMLTestFixture            per-instance PrinterState / MoonrakerClient / MoonrakerAPI
```

**Always derive from one of these.** A hand-rolled fixture that calls `lv_init_safe()` and
`lv_display_create()` looks sufficient and is not — it silently skips the UpdateQueue
lifecycle, and anything that defers work through `ui_queue_update()` **never runs**. There is
no error; the code under test simply does nothing and the test fails on a value that looks
like a logic bug.

That is the trap: `NavigationManager::push_overlay()` queues its entire body through
`UpdateQueue`. With an uninitialised queue, `push_overlay()` + `drain()` is a no-op pair, so a
width/lifecycle assertion fails as if the production logic were wrong.

```cpp
class MyFixture : public LVGLTestFixture {   // ← not a bare class
  public:
    MyFixture()  { /* your setup */ }
    ~MyFixture() override { helix::ui::UpdateQueue::instance().drain(); }
};
```

### Deferred work needs an explicit drain

Anything routed through `ui_queue_update()` / `tok.defer()` runs on the next queue tick, not
at call time. Drain before asserting:

```cpp
nav.push_overlay(widget);
helix::ui::UpdateQueue::instance().drain();
```

`process_lvgl(ms)` (on `LVGLTestFixture`) additionally pumps timers and animations.

### `process_lvgl()` moves *virtual* time — it is not a wall-clock wait

The test binary builds its display with a bare `lv_display_create()`. No driver, so nothing
ever calls `lv_tick_set_cb()` and `lv_tick_get()` returns exactly what `lv_tick_inc()` has
been fed. `process_lvgl()` is the thing feeding it, and it never sleeps: the only real time it
takes is the CPU its timers use.

So a loop that counts nominal milliseconds toward a timeout is not waiting for anything:

```cpp
int waited = 0;
while (!done && waited < 120000) { process_lvgl(100); waited += 100; }   // ← milliseconds
```

That burns a two-minute budget at once and never yields to the thread it is
watching. The symptom is baffling rather than obvious: the test runs on past the assertion,
the fixture destructor tears down the screen, and teardown effects appear to happen mid-test.

Two correct answers, in order of preference:

1. **Join the worker, then drain.** Deterministic, no timeout, no flake under CI load.
   `ActivePrintMediaAsyncFixture::drain()` in `test_active_print_media_manager.cpp` is the
   model — `ThumbnailProcessor::wait_for_completion()` then drain, repeated, since a drained
   callback can commit more pool work.
2. **`wait_until(pred, timeout_ms)`** on `LVGLTestFixture` when there is no joinable handle.
   Real `steady_clock` deadline, real sleeps, and it advances the tick each pass so timers and
   animations still come due.

The frozen-clock trap has a mirror image, which is why `wait_until` lives on the fixture: a
wait that sleeps on the real clock *without* `lv_tick_inc()` leaves LVGL's clock stopped.
`lv_async_call` one-shots (period 0) still fire, but no timer with a real period ever comes
due, however long you wait.

---

## LVGL traps in tests

**`lv_obj_get_width()` reads the COMPUTED coord, not what you just set.**
`lv_obj_set_width()` writes a style; the computed value only refreshes on a layout pass. A
freshly created widget reports LVGL's default (160) no matter what you set. Force it:

```cpp
lv_obj_set_width(w, 900);
lv_obj_update_layout(w);          // ← without this, get_width() still says 160
CHECK(lv_obj_get_width(w) == 900);
```

Same applies to heights, content sizes, and anything measured off a flex/grid parent.

**Seed `NavigationManager` the way the app does.** `panel_stack_[0]` always holds the active
root panel in production, and overlay code reads `panel_stack_.back()` to find what is
beneath it. If you only ever `push_overlay()`, the *second* push still looks like the first:

```cpp
std::array<lv_obj_t*, kUiPanelCount> panels{};
for (auto& p : panels) p = lv_obj_create(lv_screen_active());
NavigationManager::instance().set_panels(panels.data());   // seeds panel_stack_[0]
NavigationManager::instance().set_active(PanelId::Advanced);
```

**Overlays must be registered before push.** `register_overlay_instance(widget, lifecycle)` —
or `(widget, nullptr)` for an intentional lifecycle-less overlay. Tests run with
`HELIX_STRICT_OVERLAY_CHECK`, so an unregistered push **aborts** rather than warning.

---

## What to run when

Key the choice on the question you actually have, not on a phase of work:

| The question you actually have | Command | Cost |
|---|---|---|
| Does the thing I just wrote work, and I edited code | `make t F='exact case name'` | 5 to 18s |
| Same, and nothing has changed since the last build | `./build/bin/helix-tests 'exact case name'` | **0.05s** |
| Did I break this area | `make t F='[tag]'`, or the binary directly | 5 to 18s, or 0.6s |
| **Can this test fail at all?** | break the line it covers, rerun that one case, restore | one `make t` cycle |
| Do any assertions only look like assertions? | `make check-tautology`, `make test-vacuous` | 3s, seconds |
| Does any test even reach this code? | `make cov-diff` | minutes |
| Do my changed lines have detection? | `make mutate-diff MUTATE_ARGS="--base <origin/main before the series>"`, **once, when the series of work is done** | per hunk: one rebuild + that file's tests |
| Did I break something elsewhere in C++? | `make unit-sweep` | ~50s idle, minutes loaded |
| Will CI's unit job pass? | `make unit-sweep NPROCS=8`: CI runs 8 shards, split four apiece across two 4-core runners, so tests share a process with different neighbours than the local 96 | ~100s |
| Am I finished - did I break anything, anywhere? | `make full-test-run` | ~2m, **once, at the end** |
| **May this land on main?** - will CI go green on it | `make land-gate`: THE pre-landing gate (below) | prints its measured ETA at start, **once per landing** |
| What does any of this actually cost? | `make dev-timing` | 5s, measured medians |

The rule the table encodes: **a full run cannot tell you your feature works.** It can only
tell you something else broke, and that question is not interesting until you are done. A
96-shard run mid-feature costs a minute of attention to answer a question nobody has yet.

`make t` takes a tag, an exact case name, or any Catch2 expression, so one variable covers
every way of naming what you want. With no `F` it refuses rather than running everything.
`make test-run` runs nothing: it prints this choice and exits non-zero, because the
shortest target name is the one people type, so it must not start a full run.

`make land-gate` is the gate a batch landing runs on the combined result. Over `full-test-run`
it adds CI's 8-shard C++ layout (a use-after-free can crash only next to CI's neighbours),
`tests/ui`, the plugin and python suites, the generated-artefact checks and the quality sweep,
which is how a branch goes green here and red on CI otherwise. It ends with a PASS/FAIL/TIMEOUT
table naming the commit it judged; `make land-gate STAGES=<step>[,<step>]` reruns failed steps on
the same clean HEAD. Stages, logs and stamps: `scripts/land-gate.sh`, `build/land-gate/`.

Rows four through seven are the evidence ladder below. "Proving a test can fail" is where
each one's cost and blind spot is set out; read it there rather than picking from this
table.

### The wrapper costs more than the test

`make t`'s cost is make's dependency scan (about 5-18s, moving with load); the binary runs one
case in 0.05s. Edited code: `make t`, because `make -j` builds only the app and the test binary
would silently report the previous build. Nothing edited (re-running a failure, another filter,
a neighbouring tag): run `./build/bin/helix-tests` directly.

### Reading a mutation verdict

`make mutate-diff` reports that a hunk died or survived; what that verdict means (existing
test stays green, every call site reddens, any test reaches the path, the test didn't stage
its own evidence) is in the `writing-tests` skill. `make cov-diff` answers "does any test
reach this path" in one run instead of one build per hunk.

---

## What counts as a real test

The rules (name the break, independent expectations, setup reaches the branch, doubles
inert or real, mutation reading) live in the personal `writing-tests` skill. What is
specific here:

- **Pure-function exemplar:** `include/overlay_class.h` + `tests/unit/test_overlay_width_class.cpp`
  for the rule without LVGL, `test_overlay_width_push.cpp` for the wiring.
- **Stand-ins:** `mk/tests.mk` filters a few objects out of the test link (`TEST_APP_OBJS`)
  and `tests/ui_test_utils.cpp` stands in for them. `scripts/check_test_mirrors.py` flags a
  stand-in that branches as `stub-logic` against the `MIRROR_MAX` ratchet
  (prestonbrown/helixscreen#1220). To test the logic, drop the object from the filter list.

### A test earns its place

A branch should not grow the suite: `scripts/check_test_budget.py` measures the branch
from its fork point (to the index at commit, to HEAD at push and in `land-gate`) and warns
when

- **C++ test lines grow.** Pay for a new test by cutting or folding a weaker one. A branch
  that adds a genuinely new subsystem grants itself lines with a commit message line
  `Test-budget: +N <reason>`; the reason is what a reviewer reads.
- **`TestAccess` references grow.** Seed and observe through the public API instead.
- **`friend` declarations grow** in `src/` or `include/`.

The same branch-relative shape caps file size: `scripts/check_growth_ratchets.py` warns on a
C++ file under `tests/` (or `src/`, `include/`) that ends the branch over 1500 lines and
larger than it started, so a test file that size is split rather than grown.

Both are bookkeeping ratchets: a warning blocks no commit, push or landing (#2004), and
`scripts/ratchet_report.py` shows the weekly trend, so the totals are watched rather than
gated.

A case that buys nothing (`writing-tests` § "Before writing one") is the one to cut
(prestonbrown/helixscreen#1965).

### Proving a test can fail

Why a green suite is not evidence, and why only mutation sees the adjacent-invariant
failure: `writing-tests` § "Proving it can fail". The tools here, in increasing cost:

| Command | Cost | Answers |
|---------|------|---------|
| `make check-tautology` | instant | assertions that cannot fail, from the source |
| `make test-vacuous` | seconds | assertions that cannot fail, from a real run |
| `make cov-diff` | minutes | changed lines the suite never executes |
| `make test-order-dependence` | minutes | tests that pass only because of what ran before them |
| `make mutate-diff` | a build plus the scoped suite's slowest shard, per hunk | **changed lines no test detects** |

`make mutate-diff` reverts each changed hunk in turn, refreshes whatever the
suite reads, and runs it. A hunk that survives reversion is a change no test
detects, whatever the diff's test files claim. It is the only tool here that sees
the adjacent-invariant failure, so scope it (`MUTATE_ARGS="--limit 5"`, or
`--tests "[ams]"`) rather than skipping it. A filter as narrow as one tag is safe:
the run counts the cases it selects and uses no more shards than that, and a
filter matching no case stops before the baseline with the filter named.

**Read the first two lines: they say what the run is about.**

```
base d9f1c0a4e7b2  <- merge-base with origin/release/1.0 (nearest of 4 candidate upstream(s))
diff 3 hunk(s) across 2 file(s)
```

The base is the nearest fork point among the branch's upstream, the release branches and
main. A hunk count well above the size of your change means it picked the wrong one (a
branch off `release/1.0` measured against main inherits that branch's hunks), and the run
stops and says so rather than spending the builds
(`--max-hunks`, default 25; exit 4). Settle it with
`MUTATE_ARGS="--base origin/release/1.0"`, which is taken as given and never
second-guessed.

| Verdict | Means |
|---------|-------|
| `killed` / `killed (timeout)` | a test detects the change (a timeout counts only when it hangs twice) |
| `SURVIVED` | NO test detects it: a follow-up fix pins it |
| `uncompilable` / `unreversible` | never judged by a test; never a kill |
| `skipped (declaration-only)` | a revert that can only fail to compile; not built, unexamined |
| `UNEXAMINED` | the run stopped first (`--max-minutes`, default 45, or its parent exited) |
| `NOT COVERED` | nothing mutates that path (`tests/`, `lib/`, anything outside `src/`, `include/`, `ui_xml/`, `assets/config/`, scripts) |

The run answers `CLEAN` only when every hunk it built was mutated and every
mutant died. Declaration-only skips do not stop a run being `CLEAN`, but they are
unexamined and the verdict says so: `CLEAN (N hunk(s) not built: declaration-only,
unexamined)`. Anything short of that is `INCOMPLETE` (exit 3) and names what it
did not examine, because a clean answer about a file the tool never opened is
precisely what a commit body would go on to cite as proof.

`make cov-diff` is the cheap screen: a changed line the suite never runs cannot
be tested, and finding that costs one run instead of one build per hunk. The
converse does not hold, so a clean coverage report is not a substitute. It heads
its report with the same `base ... <- ...` line, resolved by the same module, so
the two tools cannot disagree about what the change under test is.

The full manual (base selection, workers and reflink copies, time caps and signals, proven-commit notes and their flags, per-file test selection, release runs): `docs/devel/TESTING.md` § "mutate-diff".

### A test must pass on its own

`make test-order-dependence` re-runs each source file's cases alone and compares against
the full-suite result: `dependent` (passes in the suite, fails alone) and `pollution` (the
reverse). Here it matters doubly, because any change to the case count reshuffles the
shards and moves a test's accidental prerequisite into another one.

`make test-vacuous` cannot see the semantic tautology (Catch2 expands `after <= before` to
`1024 <= 1024`, which differs from the source), and `-Werror=type-limits` catches only the
compiler-visible form (`>= 0` on a `size_t`). Assert the setup reached the branch first
(`writing-tests`).

### When to run it, and where the record goes

Run `make mutate-diff` once when a series of work is done, from the landed tree:
`MUTATE_ARGS="--base <origin/main before the series>"`, in the background. A series is every batch
serving one goal, so several landings can share one run. It never holds up a merge, and it is never
skipped at the end: a survivor becomes a follow-up fix (a test that pins the hunk), landed like any
other. Never run it per task, per commit, per agent or per landing while the work is still moving:
hunks a later commit rewrites get mutated twice for nothing, and the verdict on them is thrown away.

`make mutate-diff` needs reflink copies for its workers; on a host without them (a Mac, an ext4 home) it hands the run to the test host, which mutates the PUSHED HEAD and refuses uncommitted code or an unpushed HEAD. `MUTATE_LOCAL=1` runs it here, serially.

Record each mutation as a `mutation:` line in the body of the branch's last commit (or its
merge commit), as `writing-tests` § "Record it" describes.

### Tag conventions

`[core]`, `[navigation]`, `[ams]`, `[threading]`, `[compile][drift]`, `[slow]`, and a bare
issue number for bug-fix tests (`[1178]`) so the whole fix runs with
`./build/bin/helix-tests "[1178]"`.

`[slow]` runs sharded (`SLOW_SHARDS`, default 16) in `test-all` and `test-slow`, in a
fixed-seed shuffle (`SLOW_ORDER`, default `--order rand --rng-seed 1`), so a `[slow]` test
must pass whatever runs before it. A failing shard's diagnostics print the exact command,
order flags included. A `[slow]` test that only fails with sibling shards running gets
`[serial]` as well, which moves it to a single-process pass after the shards.

---

## Shell tests never touch the host

`tests/shell/setup_suite.bash` blocks the commands that address the host **by name** -
`killall`, `pkill`, `pidof`, `reboot`, `mount`, `addr2line` and friends. bats loads it for
every run of this directory, so a new test is sandboxed without opting in. A blocked call
is recorded with the test that made it and then fails, and the run fails at the end with
the list. `kill "$pid"` is deliberately not blocked: a pid is a handle the test already
holds, so it is scoped by construction, while a name reaches whatever else is running -
another bats file's app, or yours.

`jobpool` is on the list too, and the suite exports `JOBPOOL=0`, so a make a test runs
passes straight through the host's jobpool shim and no test draws from or reads the
machine's build pool. A test of pool handling names a fake through `HELIX_JOBPOOL` and
unsets `JOBPOOL` (`test_helix_claim_jobs.bats`).

**No test runs make in the tree under test.** A gate runs this suite while its own lanes
build there, and a second make in that tree deletes the test binary they are about to run.
`make -n` is no safer: it still runs recipe lines that name `$(MAKE)` and writes stamps under
`build/`. A guard in front of `make`/`gmake` blocks any make aimed at the tree; a make called
by absolute path, or under a callee's replaced PATH, gets past it, and
`lint_bats_real_tree_make` (`scripts/code_lint.sh`) catches those in source (exception:
`# REAL_TREE_MAKE_OK: <reason>`). A test that needs the real Makefile uses `scratch_tree`,
`scratch_make`, `make_db` or `run_mk_fragment` (helpers.bash). The suite unsets what an
enclosing make hands down, so `make test-shell` and bare `bats` behave the same.

To use one of those commands, mock it: `mock_command_script "killall" 'exit 0'` in
`setup()`. A mock is found first, so the sandbox never sees the call.

The sandbox is both a PATH shim and an exported bash function, since each is lost at a
different boundary; `test_sandbox_gate.bats` proves both.

`load helpers` additionally shadows `systemctl` with an inert exit-0 shim
(`tests/shell/helpers.bash`).
Installer code reaches systemctl through paths a test never names - the update-unit
stop/disable sweep is not gated on INIT_SYSTEM - and on headless CI the denied call hides
behind the installer's `|| true` while on a desktop it raises a polkit prompt per call. A
test that wants scripted systemctl behaviour still calls `mock_command*` (its later write
to the same PATH slot wins); `HELIX_TEST_REAL_SYSTEMCTL=1` disables the shadow for
debugging the helpers themselves.

---

## Test isolation

Fixture ctor **and** dtor call `reset_all()`. Cross-test leaks through `UpdateQueue` are
ratcheted by `scripts/check_update_queue_leaks.py` — if you add a test that queues callbacks
without draining, the nightly leak gate will name it.

A test that forks keeps every Catch2 macro on the parent side: fork, run the child block
to `_exit()`, and only then `REQUIRE(pid > 0)`. A macro evaluated in the child is
reported by the child, and Catch2's XML writer flushes each element into the report file
description the child shares, so a `--success` report (`make suite-report`) comes out
with the child's elements spliced into the parent's. `lint_fork_assertions` in
`scripts/code_lint.sh` enforces it (`qc_test_lint`).

XML subjects still register into LVGL's **global** scope (per-test scopes are blocked by LVGL
internals). Each test refreshes them with `init_subjects(true)`.

---

## Lint gates

Gates live in `scripts/check_*.py` and run from `scripts/quality-checks.sh`, each through its own
`scripts/qc/<gate>.sh` named in the driver's `QC_ALL`. A test that drives a gate sources
`scripts/qc/_lib.sh` and then that file; `qc_trigger <gate>` (helpers.bash) reads its trigger. Every gate gets a
**meta-test** in `tests/shell/test_*_gate.bats` pinning what it catches and what it stays quiet
about (`writing-tests` § "Lint gates and checkers").

A gate that must not match inside comments or string literals blanks them with
`scripts/source_strip.py` (`blank_cxx`, `blank_xml_comments`), never a private copy.

quality-checks.sh runs every gate over the real tree (pre-push, land-gate, CI), so a
meta-test pins behaviour on fixtures and the `scripts/qc/` wiring, never the repo verdict. Lints too small for a script of their own are functions in
`scripts/code_lint.sh`, run as `qc_code_lint`, `qc_test_lint` and `qc_ui_lint`, with
their fixture tests in `test_code_lint.bats`.

## Which shell tests run

`tests/shell/tooling-triggers.tsv` lists the files that test build tooling (gates,
quality-checks, land-gate, helix-claim, worktrees, patches, release and CI scripts, the
makefiles), each with the paths it exercises. `scripts/bats_select.py` picks what a run needs:
every file NOT listed (product: installer, uninstaller, launcher, platform hooks, plugin
install, device scripts), plus each listed file whose own path or trigger paths differ from
the fork point (`scripts/diff_base.py`), uncommitted and untracked files included. A change to
`helpers.bash`, `setup_suite.bash`, `fixtures/`, the table or the selector runs every file.

| Who | What runs |
|---|---|
| `make test-shell`, `full-test-run`, `land-gate` | the selection, from the fork point (`BATS_BASE=<ref>` overrides) |
| build.yml (push, PR) | the selection, from the PR's base or the commit before the push |
| release.yml | product files only (`BATS_BASE=HEAD`) |
| nightly.yml, `BATS_ALL=1` | every file, so a trigger the table is missing is caught within a day |

So a new test file runs on every change until it is listed. A tooling test adds its row,
naming the scripts, makefiles, workflows and data it reads (its own `.bats` is implied). The
selector fails the run on a listed file that does not exist and on a path that matches
nothing tracked, so a rename cannot silently retire a trigger. `python3 scripts/bats_select.py
--check` validates the table; tests: `tests/python/test_bats_select.py`.

The heavy in-process installer and launcher files call `skip_line_tracking` (helpers.bash)
in `setup()`, which turns off bats' per-command DEBUG trap: a third or more of their time. A
failure still fails, but bats names the helper as its location; rerun with
`HELIX_BATS_TRAP=1` to see the line.
