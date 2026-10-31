# tests/CLAUDE.md — Writing HelixScreen Tests

## Layout

| Path | Contents |
|------|----------|
| `tests/unit/` | Catch2 unit tests. **Auto-globbed** (`Makefile#TEST_UNIT_DIR`) — drop a `.cpp` in and it builds |
| `tests/unit/application/` | App-lifecycle tests (separate glob, own include path) |
| `tests/mocks/` | Mock implementations |
| `tests/shell/` | `bats` tests — installer, lint gates, packaging |
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
| Do my changed lines have detection? | `make mutate-diff MUTATE_ARGS="--base <origin/main it landed on>"`, **once per batch, right after it lands** | per hunk: one rebuild + that file's tests |
| Did I break something elsewhere in C++? | `make unit-sweep` | ~50s idle, minutes loaded |
| Will CI's unit job pass? | `make unit-sweep NPROCS=8`: CI runs 8 shards, split four apiece across two 4-core runners, so tests share a process with different neighbours than the local 96 | ~100s |
| Am I finished - did I break anything, anywhere? | `make full-test-run` | ~2m, **once, at the end** |
| **May this land on main?** - will CI go green on it | `make land-gate`: THE pre-landing gate (below) | prints its measured ETA at start (17-36m on 2026-10-21), **once per landing** |
| What does any of this actually cost? | `make dev-timing` | 5s, measured medians |

The rule the table encodes: **a full run cannot tell you your feature works.** It can only
tell you something else broke, and that question is not interesting until you are done. A
96-shard run mid-feature costs a minute of attention to answer a question nobody has yet.

`make t` takes a tag, an exact case name, or any Catch2 expression, so one variable covers
every way of naming what you want. With no `F` it refuses rather than running everything.
`make test-run` runs nothing: it prints this choice and exits non-zero, because the
shortest target name is the one people type, so it must not start a full run.

`make land-gate` is the gate a branch passes before it reaches main, and it is the one a
batch landing runs on the combined result. It runs the C++ suite once, in CI's 8-shard
layout rather than the local sweep's 96 (a use-after-free can crash only next to the
neighbours CI's layout gives it, and CI never runs the 96), the bats suite, CI's exact
`tests/ui` pytest slice, the moonraker plugin tests and `tests/python` (`make test-ui-ci`, `make test-plugin`, `make test-python`), and the
generated-artefact checks
(`check-theme-tokens`, `check-xml-schema`, `lint-xml`, `check-lvgl-event-codes`), and the
`quality-checks.sh` sweep pre-push would otherwise repeat on the same commit. The C++
run goes to the test host when `full-test-run`'s would (`TEST_HOST`); everything else runs
here in parallel, and it ends with a PASS/FAIL/TIMEOUT table with durations, naming the
commit it judged (`+dirty` when uncommitted changes were in the tree). It holds
`build:<tree>` while it runs and refuses a missing bats rather than skipping it
(`scripts/land-gate.sh`, logs in `build/land-gate/`). A gate red on one flaky or environment
step is finished with `make land-gate STAGES=<step>[,<step>]` on the same clean HEAD: it
reruns only those steps (app-build, bats, ui and quality as one group, since three of them
test the binary app-build links), merges them into that commit's table, and stamps the
commit for pre-push once every step has passed. `full-test-run` alone misses
`tests/ui` and CI's shard layout, which is how a branch goes green here and red on CI.

Rows four through seven are the evidence ladder below. "Proving a test can fail" is where
each one's cost and blind spot is set out; read it there rather than picking from this
table.

### The wrapper costs more than the test

Measured on the build host at load average 37, with five other sessions compiling:

| | |
|---|---|
| `make test`, nothing to rebuild | 11.2s |
| `make t F='[lane]'`, nothing changed | 5.3 to 6.7s over four runs |
| `./build/bin/helix-tests '[lane]'`, 223 cases | **0.58s**, four runs, no variance |
| `./build/bin/helix-tests '<one case>'` | **0.05s**, four runs, no variance |

`make`'s dependency scan over this tree is the entire cost, and it is the part that moves
with load: the same warm `make t` measures about 4s on an idle box and 18s on a busy one.
The tests do not move at all. Read these as ratios rather than absolutes.

So `make t` buys exactly one thing, the guarantee that the binary matches your source, and
that guarantee is what the seconds are for.

**When you have edited code it is not optional.** `make -j` builds only the app binary, so
running `./build/bin/helix-tests` after an edit silently tests the previous build and
reports the previous run's numbers. That is a green that means nothing.

**When you have not edited code, skip it.** Re-running a failure, trying a different
filter, checking a neighbouring tag, reading what an assertion actually said: the binary
already matches your source, and those are 0.05s questions.

### Four checks the ladder does not make for you

`make mutate-diff` reports that a hunk died or survived. Reading what that verdict means is
still yours:

1. **Does the test stage the thing it is proving gets produced?** A case that calls a
   `TestAccess` hook to record the expectation it then asserts stays green when
   production's own recording is deleted. It graded evidence it wrote itself.

2. **Under your mutation, does an EXISTING test stay green?** A new test going red only
   shows that it reaches the code. An existing test *not* going red is what shows the new
   one adds cover rather than duplicating cover that was already there.

3. **For a shared function, does the mutation redden EVERY call site?** One caller's test
   reddening while another's stays green means the second path is untested. That asymmetry
   is how an enum parameter no case varies, or a parser no case feeds, becomes visible.

4. **Before trusting "no existing test reddened", does any existing test reach the path?**
   A gate nothing can reach never reddens, so silence there is absence of a test, not
   evidence of one. This matters most for a hard stop whose whole justification is that
   existing tests would catch a regression. `make cov-diff` answers it for one run instead
   of one build per hunk.

---

## What counts as a real test

A test must FAIL if the feature is removed. After writing one, **mutate the implementation and
watch it go red** — a test that passes against broken code is worse than no test. `make
mutate-diff` does this across the whole diff; see "Proving a test can fail" below.

| ❌ | ✅ |
|----|----|
| `REQUIRE(true)`, `REQUIRE(ptr != nullptr)` and nothing else | assert the value the feature computes |
| Happy path only | edge cases, error paths, both sides of a branch |
| Asserting a constant you also hardcoded in the test | derive the expectation independently |

Prefer extracting the rule as a **pure function** and testing that without LVGL — see
`include/overlay_class.h` + `tests/unit/test_overlay_width_class.cpp`. Then test the wiring
separately (`test_overlay_width_push.cpp`). Pure-logic tests are fast, total, and survive
refactors of the widget layer.

**A test double may be inert, or it may be the real object; it may not be a copy of the
logic.** `mk/tests.mk` filters a few objects out of the test link (`TEST_APP_OBJS`), and
`tests/ui_test_utils.cpp` stands in for them. A stand-in with an empty body or a single
return cannot lie. One that branches is a second implementation the tests exercise instead
of production's, and it drifts silently. `scripts/check_test_mirrors.py` flags it as
`stub-logic` against the `MIRROR_MAX` ratchet (prestonbrown/helixscreen#1220). To test the
logic, link the real object: drop it from the filter list.

### Proving a test can fail

"A test must FAIL if the feature is removed" is the rule, and a green suite is not
evidence of it. A test can have real assertions, execute the changed function, and
still pin an *adjacent invariant* instead of the changed line, so the change reverts
green with a test commit beside it. Assertion shape cannot find these; the
worst-looking files are often good tests. Five tools, in increasing cost, and only
the last is an oracle:

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

The base is the nearest fork point among the refs this branch could have been cut
from - its configured upstream, then the release branches, then main - because
`main` alone is wrong on anything growing out of a maintenance branch. A branch
cut from `release/1.0` shares with main only where those two parted, so measured
against main it inherits everything `release/1.0` has done since as its own
change: dozens of foreign hunks, a build apiece, and verdicts about other
people's code. Those mostly come back `uncompilable`, which is correctly not a
kill, so a mis-scoped run has no symptom of its own - it just looks slow and
stubborn.

A hunk count well above the size of your change is therefore the thing to
notice, and the run stops and says so rather than spending the builds
(`--max-hunks`, default 25; exit 4). Settle it with
`MUTATE_ARGS="--base origin/release/1.0"`, which is taken as given and never
second-guessed.

**Where a hunk's time goes, and how a run spends less of it.** A compiled hunk costs
one translation unit and a relink of `helix-tests` (seconds), then the scoped suite,
which is as slow as its slowest shard: a single heavy case decides it. So the filter
is cut into 4 Catch2 shards per process `--shards` allows at once (default 8),
started longest first by their baseline time as slots free up, and a detection in
any shard stops the rest. `--workers N` mutates N hunks at once, each in a reflink
copy of the tree beside it (`.<tree>.mutate-w<i>`, removed at the end) with its own
`build/` and binary, so no two mutants share a file and the tree you ran it in is
never mutated. It needs a standalone clone on btrfs, XFS or ZFS and says why when
it falls back to serial; `scripts/test-host-run.sh mutate` passes `--workers auto`.
Every make the run starts, and every shard or worker beside the first, draws from
the jobserver it inherits (the jobpool on the test host), so the workers never run
on top of the machine's budget. Verdicts are the same either way; only the order
they arrive in changes.

**A run is bounded, and any stop leaves the tree as it found it.**

- `--max-minutes N` (default and maximum 30; `--release` lifts the cap, and
  only then does `0` mean none) caps the whole run, baseline included. At the
  cap the in-flight hunk is stopped and restored, and it and
  every hunk after it are listed `UNEXAMINED`: the run is `INCOMPLETE`, never
  `CLEAN`.
- SIGINT, SIGTERM and SIGHUP all stop it the same way: every child (make and
  its compilers, `git apply`, the suites) runs in its own process group and is
  stopped and reaped first, then every mutated file is written back from the
  bytes captured at the start, then it exits `128+signo`. Ctrl-C and a plain
  `kill` are equally safe; only `kill -9` can strand a mutant. An inherited
  SIGHUP ignore (`nohup`) is kept.
- A run whose parent process exits stops before its next hunk instead of
  mutating a tree nobody is watching. `--allow-orphan` for a deliberate detach.

**Landing a fix mutates the fix, not the branch again.** A run that leaves
nothing unexamined (every mutatable hunk killed or declaration-only) records HEAD
as proven in `refs/notes/mutate-proven`, but only when all of these hold: its
base is the fork point or itself a proven commit, HEAD did not move during the
run, the tracked tree matches HEAD, and nothing untracked sits under `tests/`,
`src/` or `include/`. Commit before the run if you want it recorded. The next
run with no `--base` starts from the newest proven commit on the branch, and its
first line says so (`<- last commit proven by mutate-diff on this branch`). The
note carries the run's `NOT COVERED` paths and the next run reports them again,
so a delta run is as INCOMPLETE as the branch it continues. A delta that removes
or edits a line under `tests/` can undo the kills the proof rests on, so it falls
back to the fork point and prints why.

| Flag | Scope |
|------|-------|
| (none) | since the last proven commit, else the fork point |
| `--since <sha>` | since a commit you name; must be an ancestor of HEAD, and records nothing unless it is proven |
| `--whole-branch` | everything since the fork point, ignoring the record |
| `--base <ref>` | exactly `<ref>`, taken as given; records only if it is the fork point or proven |
| `--no-record` | run as usual, record nothing |

The record lives in the repo that ran the gate, so a run on the test host
(`scripts/test-host-run.sh mutate`) records there; pass `--since` explicitly to
carry a proof between machines.

The verdicts are not interchangeable:

| Verdict | Means |
|---------|-------|
| `killed` | a test detects the change — the outcome you want |
| `killed (timeout)` | the mutant hung the suite at a limit derived from the baseline (`max(60s, 5x baseline)` of the same Catch2 shard, or `--timeout`) AND hung again when re-run at twice that limit. One stall alone is a machine under load, not a detection. The whole process group dies with each timeout |
| `SURVIVED` | the mutant ran and the suite stayed green: NO test detects it |
| `uncompilable` / `unreversible` | a mutant was attempted but no test ever judged it. Never a kill: a compiler error proves the code is load-bearing for the build, not that anything tests its behaviour |
| `skipped (declaration-only)` | every changed line of a C/C++ hunk is one of: a forward declaration (`class X;`, `enum class E : int;`), an access specifier or `friend class`, a `static_assert`, `#pragma once`, a class head with no `alignas` or base that is not changed between `class` and `struct`, or a member function or destructor declaration that is not `virtual` or `explicit` (changes to its signature, `const`, `noexcept` or `= delete` included, as long as `static`/`constexpr`/`inline` match the other side). Its revert can only fail to compile, so it is not built and never counts as a kill. Everything else is mutated: every `#include`, every data member, namespace-scope prototypes, aliases and externs, friend functions and operators, explicit specialisations and instantiations, initialisers, default arguments, inline bodies, enumerators, `#define`. `--no-skip-declarations` mutates them all |
| `UNEXAMINED` | the run stopped (`--max-minutes`, or its parent exited) before reaching this hunk |
| `NOT COVERED` | nothing here can mutate that file, so the gate did not look at it |

The run answers `CLEAN` only when every hunk it built was mutated and every
mutant died. Declaration-only skips do not stop a run being `CLEAN`, but they are
unexamined and the verdict says so: `CLEAN (N hunk(s) not built: declaration-only,
unexamined)`. Anything short of that is `INCOMPLETE` (exit 3) and names what it
did not examine, because a clean answer about a file the tool never opened is
precisely what a commit body would go on to cite as proof.

What it mutates, and what pays for what:

| Path | Refresh per mutant | Suite |
|------|--------------------|-------|
| `src/`, `include/` | `make test-build` | `helix-tests` |
| `ui_xml/*.xml`, `assets/config/*.json` | none — the binary reads these off the tree at run time | `helix-tests` |
| `scripts/*.py`, `scripts/*.sh` | none | `bats tests/shell` + `pytest tests/python` |

Everything else is `NOT COVERED`, with the reason printed. Two are worth knowing:
`tests/` itself, because a test is proven by mutating the code it pins rather
than by reverting itself; and anything under `lib/`, because a superproject diff
carries only a submodule's pointer and never its content. Clear one by mutating
it by hand and naming the result in the commit body, or accept it with
`--allow-incomplete`.

`make cov-diff` is the cheap screen: a changed line the suite never runs cannot
be tested, and finding that costs one run instead of one build per hunk. The
converse does not hold, so a clean coverage report is not a substitute. It heads
its report with the same `base ... <- ...` line, resolved by the same module, so
the two tools cannot disagree about what the change under test is.

### A test must pass on its own

`make test-order-dependence` re-runs each source file's cases alone and
compares against the full-suite result. A case that passes in the suite and
fails alone is reading state some other file's test established.

This is the one class none of the other tools can see. Such a test asserts real
computed values, its lines are covered, and reverting the production hunk would
report it killed -- it simply is not testing what it claims. It stays invisible
until an unrelated change perturbs ordering, which is the worst moment to find
it: any change to the case count reshuffles the shards, a test's accidental
prerequisite lands in another shard, and a green suite goes red with nothing wrong
in the changed code.

The gate also reports the opposite sign as `pollution` (fails in the suite, passes
alone): same root cause, different fix. The polluter needs cleanup, the dependent
needs its own setup.

### Assert that the setup reached the branch, first

An assertion about a result is worthless if the code never took the path that
produces it. Assert the precondition **before** asserting the outcome:

```cpp
const size_t emergency_budget = dense.get_cache_budget() / 2;
const size_t before = dense.get_cache_memory_usage();
REQUIRE(before > emergency_budget);      // <-- the setup actually overshoots
dense.respond_to_memory_pressure();
CHECK(dense.get_cache_memory_usage() < before);
```

Without the middle line this test passes with `respond_to_memory_pressure()`
emptied out. The fixture cached a few hundred bytes against a 1MB floor, so the
evict loop's exit condition was already true on entry and nothing could ever
change. Worse, the obvious repair is *also* vacuous: `CHECK(after <= before)`
cannot fail either, for exactly the same reason.

That case is the one to keep in mind, because it defeats three of the four
tools above. `make test-vacuous` sees a real assertion (Catch2 expands it to
`1024 <= 1024`, which differs from the source text). `make cov-diff` is green,
because the changed line does execute. `-Werror=type-limits` catches only the
compiler-visible form (`>= 0` on a `size_t`); the semantic tautology hides better.
Only reverting the hunk and watching for red finds it.

### When to run it, and where the record goes

Run `make mutate-diff` once per batch, right after the batch lands, from the landed tree:
`MUTATE_ARGS="--base <the origin/main it landed on>"`, in the background. It never holds
up a merge, and it is never skipped: the landing session starts it as soon as the push is
verified, and a survivor becomes a follow-up fix (a test that pins the hunk), landed like any
other. When several branches land together, it runs once over the merged result, never once
per branch. Never run it per task, per commit or per agent while work is in progress: hunks a
later commit rewrites get mutated twice for nothing. A whole-branch run against the full
suite is a release check (`--release`), not a landing check. While building, a new test's
proof is one hand break of the line it pins (above). Root `CLAUDE.md` § Verification
Cadence has the whole schedule.

**Each changed file runs against its own tests.** A run prints `tests per file:` first. A
file gets the filter of the first `--tests-for GLOB=FILTER` it matches, else the test files
that name its stem as a whole word (Catch2's `[#test_<file>]` tags, under `-#`; a stem more
than 12 files name counts as none), else `--tests`. A script under `scripts/` runs the
`.bats` and pytest files that name it (its file name, or a Python script's import stem):
with only one of the two, that one judges it; with neither, both whole suites do, which is
slow. A bats or pytest case red at baseline (inside the test container some fail on
the environment alone: root, no jobpool) is set aside by name, printed, and every other
case judges; a red Catch2 baseline still stops the run. Files sharing what judges
them run as one group, each group a child run with its own baseline, inside the one
30-minute budget; the worst group's exit is the run's. `--no-auto-tests` puts every file on
`--tests`, `--shell-tests` and `--python-tests`.
Generated output (`src/generated/`) is not mutated: its source is, and the regeneration
gates pin the two together.

**`make mutate-diff` runs where workers can run** (`scripts/mutate-diff.sh`). Parallel
workers need reflink copies; a host without them (a Mac, an ext4 home) hands the run to the
test host, which mutates the PUSHED HEAD, so it refuses with uncommitted code or an
unpushed HEAD instead of judging another tree. `MUTATE_LOCAL=1` runs it here, serially. On
the test host the run first makes its checkout's venv from `requirements.txt`, since script
hunks are judged by pytest as well as bats and the image has no pytest.

Red-green is invisible after the fact. A test that was mutated and verified is
indistinguishable from one nobody checked, which is why the discipline decays.
One line per mutation in the body of the branch's last commit (or its merge commit) fixes
that:

```
mutation: flipped >= to > in resolve_slot(); test_ams_topology went red
```

Or, for a whole hunk: `mutation: reverted the guard; test_print_start went red`.
It costs a sentence and it is the only record that the cycle happened.

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
build there, and a second make in that tree deletes the test binary they are about to run
(`prune-orphan-test-objs`). `make -n` is no safer: it still runs every recipe line that
names `$(MAKE)`, and parsing the Makefile writes stamps under `build/`. The sandbox puts a
guard in front of `make` and `gmake` (shim plus exported function, deferring to a test's
own make mock) that blocks and records any make whose directory is the tree or inside it,
reading `-C` alone or in a cluster (`-sC`), `--directory` and its prefixes (`--dir=`). A
make that never meets either layer passes no guard: one called by absolute path
(`/usr/bin/make`), and one a non-bash callee runs under a replaced PATH (`env
PATH=/usr/bin:/bin make -C ...`, or `env PATH=... sh -c 'make ...'`), since neither
the shim nor the exported function reaches it. Those shapes are left to review, and
`test_sandbox_gate.bats` lints the source for a make pointed at a repo-root
variable or `$BATS_TEST_DIRNAME/../..` (exception: `# REAL_TREE_MAKE_OK: <reason>` inside
the @test). A test that needs the real Makefile runs it in `scratch_tree` (helpers.bash):
every top-level entry symlinked, with a `build/` of its own, safe for parsing, print-style
targets and recipes that write only under `build/` or to paths the test overrides.
`scratch_make` runs make there, `make_db [vars]` prints the database parsed there, and
`run_mk_fragment` runs one target's fragment of an `mk/` file as a scratch Makefile.
`path_without_jobpool` drops the host's jobpool from PATH and keeps the guard. The suite also unsets what an enclosing make hands down (`MAKEFLAGS`,
`MAKELEVEL`, `BATS_REQUIRED`, the Makefile's exports), so a test behaves the same under
`make test-shell` as under bare `bats`.

On a host whose bare `make` is older than the Makefile's GNU make 4.2 floor (Apple's
/usr/bin/make is 3.81; Homebrew's 4.x installs as `gmake`), that guard hands make calls to
`gmake`. A Linux make, and the jobpool shim, parse as 4.2+ and are used as found
(`test_sandbox_gate.bats`).

To use one of those commands, mock it: `mock_command_script "killall" 'exit 0'` in
`setup()`. A mock is found first, so the sandbox never sees the call.

The sandbox installs itself as both a PATH shim and an exported bash function, because
neither survives every boundary: PATH is lost when a callee **replaces** PATH instead of
prepending (`env PATH="$bin" ...`, or a script that hardens PATH the way
`install.sh` does), and a bash function is lost at a non-bash callee - which
installer scripts are, since they are `#!/bin/sh`. `test_sandbox_gate.bats` proves each
boundary and fails any test that lands in the gap between the two.

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
with the child's elements spliced into the parent's. `tests/shell/test_fork_assertion_gate.bats`
enforces it.

XML subjects still register into LVGL's **global** scope (per-test scopes are blocked by LVGL
internals). Each test refreshes them with `init_subjects(true)`.

---

## Lint gates

Gates live in `scripts/check_*.py` and run from `scripts/quality-checks.sh`, each through its own
`scripts/qc/<gate>.sh` named in the driver's `QC_ALL`. A test that drives a gate sources
`scripts/qc/_lib.sh` and then that file; `qc_trigger <gate>` (helpers.bash) reads its trigger. Every gate gets a
**meta-test** in `tests/shell/test_*_gate.bats` pinning both halves of its contract: the shape
it must catch, and the idioms it must stay quiet about. A gate that fires on legitimate code
gets switched off, so the silent cases matter as much as the loud ones.

Verify a new gate by running it against known-good and known-bad fixtures before wiring it in.
